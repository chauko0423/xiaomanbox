/**
 * ual —— UnrealAgentLink 内置服务端（2026-10-07，UEBox 拆解融合）
 *
 * 把 UEBox 的工具变成工作台的内置工具，摆脱 MCP 与 UEBox 桌面应用的依赖。
 *
 * ## 拆解结论（读 UEBox 开源仓库 ueboxai/uebox 得出）
 *
 * UEBox 的「一百多个引擎工具」本质上不是 MCP，而是：
 *   - 引擎侧插件 `plugin/UnrealAgentLink`（MIT 许可）是一个 **WebSocket 客户端**，
 *     主动连接 `ws://127.0.0.1:17860`（命令行 `-UALServer=` 可覆盖），
 *     UEBox 桌面应用只是那个被连的 WS 服务端。
 *   - 工具调用 = 一段 JSON-RPC over WebSocket：
 *       请求  {ver:"1.0", type:"req",  method:"actor.spawn", id:"<str>", params:{...}}
 *       响应  {ver:"1.0", type:"res",  id:"<str>", code:200, result:{...}}
 *       事件  {ver:"1.0", type:"evt",  method:"project.info", payload:{...}}
 *     id 是字符串；错误时 code≠200 且 result={message,...}（见
 *     UAL_CommandUtils.cpp 的 SendResponse / SendError）。
 *
 * 所以「摆脱 MCP」的正解是：我们后端自己当这个 WS 服务端，插件直连我们，
 * 工具定义内置（见 ual-tools.mjs），执行走本模块的 ualCall() —— 全程不经 MCP、
 * 不经 UEBox 桌面应用。
 *
 * ## 连接方向与多工程
 *
 * 插件断线后每 5 秒自动重连（UAL_NetworkManager.cpp）。一个编辑器一条连接，
 * 多工程时会有多条连接；当前小满工作台按单工程处理，默认用第一条活跃连接。
 */

import { UEBridge, PROTOCOL_VERSION, DEFAULT_PORT as BRIDGE_DEFAULT_PORT } from './uebridge.mjs';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';

const DEFAULT_PORT = process.env.UAL_PORT ? Number(process.env.UAL_PORT) : BRIDGE_DEFAULT_PORT;
const DEFAULT_TIMEOUT_MS = 30_000;

/**
 * 心跳新鲜度阈值（毫秒）：插件每 10s 发一次 system.heartbeat（UAL_NetworkManager TickHeartbeat），
 * 连续 3 个周期没有任何事件（心跳/project.info 等）→ 视为连接中断（编辑器忙死/半开 TCP/插件停摆）。
 * 之所以在应用层判而不是等 TCP 断开事件：半开连接（编辑器卡死、对端无通知消失）不会触发 disconnected，
 * 死连接会一直挂在连接表里，调用只会干等超时且模型看不到真实原因。
 */
export const HEARTBEAT_STALE_MS = 30_000;

/** 连接是否已中断（心跳/事件停滞超过阈值）。纯函数便于负控测试。 */
export function isConnectionStale(conn, atMs = now()) {
  return atMs - conn.lastHeartbeat > HEARTBEAT_STALE_MS;
}

/** 连接元数据：connId -> { project, engine, connectedAt, lastHeartbeat, lastEvt } */
const connections = new Map();

function now() {
  return Date.now();
}

let bridge = null;
let bound = false; // 服务器是否已真正绑定端口
let retryTimer = null;
let lastError = null;
let targetPort = null;

function stopRetry() {
  if (retryTimer) {
    clearInterval(retryTimer);
    retryTimer = null;
  }
}

function ensureBridge() {
  if (bridge) return bridge;
  bridge = new UEBridge({ port: targetPort ?? DEFAULT_PORT, host: '127.0.0.1' });

  // 连接建立
  bridge.on('connected', (connId) => {
    // 新连接建立时，清理已 stale 的旧死连接（崩溃残留/半开 TCP：TCP 没断不会触发 disconnected，
    // 心跳却早已停摆。不清理的话这些僵尸连接会一直占用连接表，误导 pickConnection 和握手判据）。
    for (const [cid, c] of connections) {
      if (isConnectionStale(c)) {
        console.log(`[ual] 清理死连接（心跳停滞）: ${cid}`);
        connections.delete(cid);
      }
    }
    connections.set(connId, {
      project: null,
      engine: null,
      connectedAt: now(),
      lastHeartbeat: now(),
      lastEvt: null,
    });
    console.log(`[ual] 插件已连接: ${connId}`);
  });

  // 断开
  bridge.on('disconnected', (connId) => {
    connections.delete(connId);
  });

  // 插件事件（project.info / system.heartbeat / project.closed）
  bridge.on('event', (method, payload, connId) => {
    const conn = connections.get(connId);
    if (!conn) return;
    conn.lastEvt = method;
    conn.lastHeartbeat = now();
    if (method === 'project.info') {
      conn.project = {
        name: payload?.project_name ?? payload?.projectName ?? null,
        path: payload?.project_path ?? payload?.projectPath ?? null,
        engine: payload?.engine_version ?? null,
      };
    } else if (method === 'project.closed') {
      // 工程关闭：断开这条连接
      bridge._handleDisconnect(connId);
    }
  });

  // 协议层活性（PONG）：插件应用层心跳（system.heartbeat）可能因重连后 ticker 停摆而不发
  // （插件 bug，见 UAL_NetworkManager.cpp TickHeartbeat），但协议层 PONG 是 WebSockets 库自动
  // 响应、仍在回 → 也更新 lastHeartbeat，避免把「TCP 活着、只是应用层心跳停了」误判成中断。
  bridge.on('activity', (connId) => {
    const conn = connections.get(connId);
    if (conn) conn.lastHeartbeat = now();
  });

  return bridge;
}

function bindServer(port) {
  targetPort = port;
  const b = ensureBridge();
  b.start()
    .then(() => {
      bound = true;
      lastError = null;
      stopRetry();
      console.log(`[ual] WS 服务端已接管端口 ${port}（零依赖自研），等待插件连接`);
    })
    .catch((e) => {
      // 端口被占（例如 UEBox 桌面应用还开着占着 17860）——不崩，进重试循环等它释放
      bound = false;
      bridge = null;
      lastError = e.message;
      console.error(`[ual] 端口 ${port} 绑定失败: ${e.message} —— 每 15s 自动重试接管（关掉占用方后无需重启）`);
      startRetry();
    });
}

function startRetry() {
  if (retryTimer) return;
  retryTimer = setInterval(() => {
    if (bound) {
      stopRetry();
      return;
    }
    bindServer(targetPort);
  }, 15_000);
}

export function ualStart(port = DEFAULT_PORT) {
  if (bound) return { ok: true, already: true, port: targetPort ?? port };
  stopRetry();
  bindServer(port);
  return { ok: true, port };
}

/** 挑一条心跳新鲜（未中断）的连接（默认第一条；可选 connId 指定）。半死连接不选，避免往死 socket 里写后干等超时。 */
function pickConnection(connId) {
  if (connId) return connections.has(connId) ? connId : null;
  for (const [cid, c] of connections) {
    if (!isConnectionStale(c)) return cid;
  }
  return null;
}

/** 连接表里是否还有连接（不论健康）——用于区分「完全没连」和「连了但已中断」两种报错。 */
function hasAnyConnection() {
  return connections.size > 0;
}

/**
 * 调一次插件命令（JSON-RPC over WS，零依赖底层）。
 * 返回 Promise<result 对象>；code≠200 / 未连接 / 超时 → reject（message 已可读）。
 */
export function ualCall(method, params = {}, timeoutMs = DEFAULT_TIMEOUT_MS, connId = null) {
  return new Promise((resolve, reject) => {
    const targetId = pickConnection(connId);
    if (!targetId) {
      if (hasAnyConnection()) {
        // 有连接但心跳全部停滞：连接已中断（编辑器长操作卡死 / 半开 TCP / 插件停摆）
        let staleSec = 0;
        for (const c of connections.values()) {
          staleSec = Math.max(staleSec, Math.round((now() - c.lastHeartbeat) / 1000));
        }
        reject(
          new Error(
            `引擎连接已中断：编辑器与工作台的连接已 ${staleSec}s 没有任何心跳（正常每 10s 一次）。` +
              `常见原因：编辑器正忙于长时间操作（编译蓝图/保存地图/批量放置大量 Actor，此时等它完成即可）或连接已断开。` +
              `建议等 1-2 分钟重试本工具；若持续报错，到虚幻编辑器里确认 UnrealAgentLink 插件仍在运行（或重启编辑器）`
          )
        );
        return;
      }
      reject(
        new Error(
          '引擎未连接：没有任何虚幻编辑器连到工作台（需先打开工程并装上 UnrealAgentLink 插件）。若刚发起打开流程，编辑器可能仍在启动加载中——用 workbench_ue5_status 查进度，等连接就绪后再重试本工具'
        )
      );
      return;
    }
    bridge
      .callRequest(method, params, { timeout: timeoutMs, connId: targetId })
      .then((result) => {
        // bridge 已把 __rpc.code 注入；code≥400 时 result 里已有 ok=false/error
        if (result && result.__rpc && typeof result.__rpc.code === 'number' && result.__rpc.code >= 400) {
          const detail = result;
          const err = new Error(detail.error || detail.message || `插件返回错误码 ${result.__rpc.code}`);
          err.code = result.__rpc.code;
          err.details = detail.details;
          reject(err);
        } else {
          resolve(result ?? {});
        }
      })
      .catch(reject);
  });
}

/** 契约端点：连接状态（不含任何敏感信息） */
export function ualStatus() {
  const list = [];
  for (const [connId, c] of connections) {
    list.push({
      connId,
      connected: true,
      healthy: !isConnectionStale(c), // 心跳停滞超过 30s → false（连接已中断）
      heartbeatAgeSec: Math.round((now() - c.lastHeartbeat) / 1000),
      project: c.project,
      engine: c.engine,
      connectedAt: c.connectedAt,
      lastHeartbeat: c.lastHeartbeat,
      lastEvt: c.lastEvt,
    });
  }
  return {
    contract: 'v1',
    ok: true,
    listening: Boolean(bridge) && bound,
    port: bound ? (targetPort ?? null) : null,
    /** 端口被占、正在自动重试接管（大概率 UEBox 桌面应用还开着） */
    retrying: !bound && Boolean(retryTimer),
    targetPort,
    lastError,
    connectionCount: list.length,
    connections: list,
  };
}

/** 测试钩子：清空连接与未决请求 */
export function ualReset() {
  for (const connId of [...connections.keys()]) {
    bridge?._handleDisconnect(connId);
  }
  connections.clear();
}

/** 测试钩子：关闭 WS 服务端 */
export function ualStop() {
  stopRetry();
  lastError = null;
  if (bridge) {
    bridge.stop().catch(() => {});
    bridge = null;
  }
  bound = false;
  connections.clear();
}

// ────────────────────────────────────────────────────────────
// material_apply_graph 多步编排（复刻 UEBox applyGraph.ts 的 execute）
// 引擎侧没有「一次写整张材质图」的 RPC，只有 material.add_node / connect_pins
// 单步命令，所以这里按顺序编排：addNodes → connect → tidy → compile。
// 失败即停并如实报出已建成了什么（不回滚，与 UEBox 语义一致）。
// ────────────────────────────────────────────────────────────

/** "tex.RGB" / "Material.BaseColor" → { node, pin } */
function splitMaterialEndpoint(s) {
  const i = String(s).lastIndexOf('.');
  if (i <= 0) return { node: String(s), pin: '' };
  return { node: String(s).slice(0, i), pin: String(s).slice(i + 1) };
}

/** 简化分层排版：数据源在左、主节点（无出边）在最右，同层纵向铺开。 */
function layoutMaterialSimple(nodes, edges) {
  const adj = new Map(); // to -> [from]
  for (const e of edges) {
    if (!adj.has(e.to_node)) adj.set(e.to_node, []);
    adj.get(e.to_node).push(e.from_node);
  }
  const hasOut = new Set(edges.map((e) => e.from_node));
  const level = new Map();
  const q = [];
  for (const n of nodes) {
    if (!hasOut.has(n.node_id)) {
      level.set(n.node_id, 0);
      q.push(n.node_id);
    }
  }
  if (q.length === 0 && nodes.length) {
    level.set(nodes[0].node_id, 0);
    q.push(nodes[0].node_id);
  }
  while (q.length) {
    const cur = q.shift();
    const curL = level.get(cur) ?? 0;
    for (const p of adj.get(cur) || []) {
      const nl = curL + 1;
      if (!level.has(p) || nl > level.get(p)) {
        level.set(p, nl);
        q.push(p);
      }
    }
  }
  const maxL = Math.max(0, ...level.values());
  const byLevel = new Map();
  for (const n of nodes) {
    const l = level.has(n.node_id) ? level.get(n.node_id) : maxL + 1;
    if (!byLevel.has(l)) byLevel.set(l, []);
    byLevel.get(l).push(n.node_id);
  }
  const positions = [];
  const GAP_X = 320;
  const GAP_Y = 140;
  for (const [l, ids] of [...byLevel.entries()].sort((a, b) => a[0] - b[0])) {
    ids.forEach((id, i) => {
      positions.push({ node_id: id, x: (maxL - l) * GAP_X, y: i * GAP_Y });
    });
  }
  return positions;
}

/**
 * 一次写完整张材质图（多步编排）。params：
 *   path, nodes[{id,node_type,node_name?,value?,texture_path?,function_path?,
 *        collection_path?,group_name?,properties?}],
 *   connections[{from,to}], tidy?, compile?
 * 返回 { ok, created, connected, compiled, node_ids, warnings, error? }。
 */
export async function ualApplyMaterialGraph(params = {}, timeoutMs = DEFAULT_TIMEOUT_MS) {
  const { path, nodes = [], connections = [], tidy = true, compile = true } = params;
  const node_ids = {};
  const warnings = [];
  let created = 0;
  let connected = 0;
  let compiled = false;

  if (!path) return { ok: false, error: '缺少 path（材质资产路径）' };

  // 1. addNodes
  for (const node of nodes) {
    try {
      const r = await ualCall('material.add_node', {
        material_path: path,
        node_type: node.node_type,
        ...(node.node_name !== undefined ? { node_name: node.node_name } : {}),
        ...(node.value !== undefined ? { initial_value: node.value } : {}),
        ...(node.texture_path !== undefined ? { texture_path: node.texture_path } : {}),
        ...(node.function_path !== undefined ? { function_path: node.function_path } : {}),
        ...(node.collection_path !== undefined ? { collection_path: node.collection_path } : {}),
        ...(node.group_name !== undefined ? { group_name: node.group_name } : {}),
        ...(node.properties !== undefined ? { properties: node.properties } : {}),
      }, timeoutMs);
      if (!r?.node_id) {
        return { ok: false, error: `节点 ${node.id}（${node.node_type}）没有返回 node_id`, created, node_ids, warnings };
      }
      node_ids[node.id] = r.node_id;
      created++;
      if (r.texture_applied === false) warnings.push(`${node.id}：贴图没设上，节点是空的`);
      if (r.initial_value_applied === false) warnings.push(`${node.id}：初始值没设上，现在是默认值`);
    } catch (e) {
      return { ok: false, error: `建节点 ${node.id}（${node.node_type}）失败：${e.message}`, created, node_ids, warnings };
    }
  }

  // 2. connect
  for (const conn of connections) {
    const from = splitMaterialEndpoint(conn.from);
    const to = splitMaterialEndpoint(conn.to);
    const sourceNode = node_ids[from.node] ?? from.node;
    const targetNode = to.node === 'Material' ? 'Material' : (node_ids[to.node] ?? to.node);
    try {
      await ualCall('material.connect_pins', {
        material_path: path,
        source_node: sourceNode,
        source_pin: from.pin,
        target_node: targetNode,
        target_pin: to.pin,
      }, timeoutMs);
      connected++;
    } catch (e) {
      return { ok: false, error: `连线 ${conn.from} → ${conn.to} 失败：${e.message}`, created, connected, node_ids, warnings };
    }
  }

  // 3. tidy（简化排版）
  if (tidy && (nodes.length || connections.length)) {
    try {
      const g = await ualCall('material.get_graph', { path, include_values: false }, timeoutMs);
      const positions = layoutMaterialSimple(g?.nodes || [], g?.connections || []);
      if (positions.length) {
        await ualCall('material.set_node_positions', { material_path: path, positions }, timeoutMs);
      }
    } catch {
      warnings.push('自动排版失败（不阻断）：节点位置未调整，可用 material_tidy_graph 手动排');
    }
  }

  // 4. compile
  if (compile) {
    try {
      // 注意：material.compile 读的是 `path`（不是 material_path，与 add_node/connect_pins 不一致）
      await ualCall('material.compile', { path }, Math.max(timeoutMs, 120_000));
      compiled = true;
    } catch (e) {
      return { ok: false, error: `编译失败：${e.message}`, created, connected, compiled, node_ids, warnings };
    }
  }

  return { ok: true, created, connected, compiled, node_ids, warnings };
}

// ────────────────────────────────────────────────────────────
// 本地逻辑引擎工具（不走插件 RPC，逻辑在工作台后端）
// UEBox 里这几个是「桌面应用本地能力」：进程检测 / HTTP 调 ZenServer /
// 搬迁账本回滚 / 工程路径引用扫描。这里做等价实现，让工具清单补满。
// ────────────────────────────────────────────────────────────

/** ue_session_health：判断「编辑器连没连上」五种情况（本地，不依赖引擎 RPC）。异步（需查崩溃报告进程）。 */
export async function ualSessionHealth() {
  const st = ualStatus();
  const connected = st.connectionCount > 0;
  const anyHealthy = st.connections.some((c) => c.healthy !== false);
  const staleSec = st.connections.reduce(
    (m, c) => Math.max(m, c.heartbeatAgeSec ?? 0),
    0
  );
  const connections = st.connections.map((c) => ({
    project: c.project?.name || c.project?.path || null,
    engine: c.engine || null,
    connectedAt: c.connectedAt,
    lastHeartbeat: c.lastHeartbeat,
    healthy: c.healthy,
    heartbeatAgeSec: c.heartbeatAgeSec,
  }));

  // 编辑器进程级健康（崩溃检测）：进程消失 + CrashReportClient 出现 = 崩溃
  let editor = null;
  try {
    const { editorHealth } = await import('./ue5boot.mjs');
    editor = await editorHealth();
  } catch {
    /* 检测失败不阻断，只缺编辑器崩溃提示 */
  }

  let verdict;
  if (editor && editor.state === 'crashed') {
    // 最高优先级：编辑器崩溃了（进程没了 + 崩溃报告客户端在跑）
    verdict =
      '⚠️ 编辑器已崩溃：UnrealEditor 进程消失，且检测到崩溃报告客户端（CrashReportClientEditor）正在运行。' +
      '这意味着刚才的一次操作（很可能是蓝图写入了非法引用，如无效的 StaticMesh 对象）让引擎崩溃了。' +
      '不要重试同样的操作——先处理崩溃报告弹窗（关掉它），然后用 workbench_launch_editor 重新拉起编辑器，再检查并修正导致崩溃的资产。';
  } else if (!st.listening && st.retrying) {
    verdict =
      `端口 ${st.targetPort} 被占（大概率 UEBox 桌面应用还开着），工作台每 15s 自动重试接管 —— ` +
      '关掉占用方即可，无需重启工作台。插件断线后也会每 5s 自行重连，接管后自动连上。';
  } else if (!st.listening) {
    verdict = '未监听：工作台 WS 服务端没起来（重启后端或看 ual/status 的 lastError）。';
  } else if (connected && anyHealthy) {
    verdict = `已连接：${st.connectionCount} 个编辑器连上，插件工作正常。`;
  } else if (connected) {
    // 连接还在连接表里，但心跳全部停滞 → 连接已中断（半开 TCP / 编辑器忙死 / 插件停摆）
    verdict =
      `连接已中断：编辑器连接还在，但已 ${staleSec}s 没有任何心跳（正常每 10s 一次）。` +
      '常见原因：编辑器正忙于长时间操作（编译/保存/批量放置大量 Actor——等它完成即可，不要重复发命令），或连接已断开（到编辑器里确认 UnrealAgentLink 插件状态，必要时重启编辑器）。';
  } else {
    verdict = '未连接：工作台在监听，但没有编辑器连上 —— 编辑器没在跑 / 在跑但插件没装或没连上。用 workbench_launch_editor 拉起编辑器并等连接。';
  }
  return {
    ok: true,
    listening: st.listening,
    port: st.port,
    retrying: st.retrying,
    targetPort: st.targetPort,
    connected,
    healthy: connected && anyHealthy,
    connection_count: st.connectionCount,
    editor: editor ? editor.state : null,
    connections,
    verdict,
  };
}

/** ue_zen_server：查 ZenServer（本地 HTTP 服务，编辑器启动时带起）缓存与命中率 */
export function ualZenServer(port = 8558, timeoutMs = 4000) {
  return new Promise((resolve) => {
    const req = http.get(`http://127.0.0.1:${port}/stats/z$`, { timeout: timeoutMs }, (res) => {
      let b = '';
      res.on('data', (c) => { b += c; });
      res.on('end', () => {
        try {
          resolve({ ok: true, port, ...JSON.parse(b) });
        } catch {
          resolve({ ok: true, port, raw: String(b).slice(0, 600) });
        }
      });
    });
    req.on('error', () => resolve({ ok: false, error: `ZenServer 没在 127.0.0.1:${port} 响应 —— 编辑器没用 Zen 本地缓存，或端口不是 ${port}` }));
    req.on('timeout', () => { req.destroy(); resolve({ ok: false, error: 'ZenServer 响应超时' }); });
  });
}

// 搬迁账本（内存）：ue_content_move 成功后记账，rollback 读账本反向搬
const moveLedgers = new Map(); // ledgerId -> { moves:[{source,destination}], folder_moves, createdAt }
let nextLedgerId = 1;

/** ue_content_move 的记账包装：调 content.batch_move 并记录账本（回滚用） */
export async function ualContentMove(params = {}, timeoutMs = DEFAULT_TIMEOUT_MS) {
  const res = await ualCall('content.batch_move', params, timeoutMs);
  const moves = params?.moves || [];
  const folder_moves = params?.folder_moves || [];
  if (res && res.ok !== false && (moves.length || folder_moves.length)) {
    const ledgerId = `ledger_${nextLedgerId++}`;
    moveLedgers.set(ledgerId, { moves, folder_moves, createdAt: now() });
    res._ledger_id = ledgerId;
    res._ledger_hint = '本次搬迁已记账，可用 ue_content_rollback 回滚';
  }
  return res;
}

/** ue_content_rollback：读账本，反向调 content.batch_move 搬回原路径 */
export async function ualContentRollback(params = {}, timeoutMs = DEFAULT_TIMEOUT_MS) {
  const ledgerId = params?.ledger_id;
  const ledger = ledgerId ? moveLedgers.get(ledgerId) : undefined;
  if (!ledger) {
    const ids = [...moveLedgers.keys()];
    return {
      ok: false,
      error: ledgerId
        ? `没有 id=${ledgerId} 的账本（账本在内存里，进程重启会丢）`
        : `没有搬迁账本。先用 ue_content_move 搬迁（会自动记账），再回滚。` + (ids.length ? `现有账本：${ids.join(', ')}` : ''),
    };
  }
  // 反向 moves：source/destination 互换
  const reverseMoves = ledger.moves.map((m) => ({ source: m.destination, destination: m.source }));
  const reverseFolders = (ledger.folder_moves || []).map((f) => ({ source_folder: f.destination_folder, destination_folder: f.source_folder }));
  const res = await ualCall('content.batch_move', {
    moves: reverseMoves,
    folder_moves: reverseFolders,
    ...(params.on_conflict ? { on_conflict: params.on_conflict } : {}),
  }, timeoutMs);
  return { ok: res && res.ok !== false, ...res, rolled_back_from: ledgerId };
}

/** ue_project_path_refs：扫描工程文本文件里对 /Game 路径的引用（迁移前检查） */
export function ualProjectPathRefs(params = {}) {
  const roots = Array.isArray(params?.roots) && params.roots.length ? params.roots : null;
  const uproject = params?.uproject;
  const dirs = roots || (uproject ? [path.dirname(uproject)] : [process.cwd()]);
  const extRe = /\.(uproject|ini|csv|py|cpp|h|cs|json|txt)$/i;
  const refRe = /\/Game\/[A-Za-z0-9_\/\.\-]+/g;
  const hits = [];
  const seen = new Set();
  for (const dir of dirs) {
    if (!fs.existsSync(dir)) continue;
    try {
      const files = walkTextFiles(dir, extRe, 2000);
      for (const f of files) {
        const txt = fs.readFileSync(f, 'utf8');
        for (const m of txt.match(refRe) || []) {
          const key = `${f}::${m}`;
          if (!seen.has(key)) { seen.add(key); hits.push({ file: f, reference: m }); }
        }
      }
    } catch {
      /* 跳过不可读目录 */
    }
  }
  return { ok: true, scanned_roots: dirs, reference_count: hits.length, references: hits.slice(0, 200) };
}

function walkTextFiles(dir, re, maxFiles, depth = 0, out = []) {
  if (depth > 5 || out.length >= maxFiles) return out;
  let entries;
  try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { return out; }
  for (const e of entries) {
    if (out.length >= maxFiles) break;
    const full = path.join(dir, e.name);
    if (e.isDirectory()) {
      if (['node_modules', '.git', 'Intermediate', 'Saved', 'DerivedDataCache', 'Binaries', '.idea', '.vscode'].includes(e.name)) continue;
      walkTextFiles(full, re, maxFiles, depth + 1, out);
    } else if (e.isFile() && re.test(e.name)) {
      out.push(full);
    }
  }
  return out;
}

/** 本地逻辑工具分发：method 形如 local.session_health / local.zen_server / ... */
export async function ualLocalCall(name, params = {}, timeoutMs = DEFAULT_TIMEOUT_MS) {
  switch (name) {
    case 'local.session_health': return ualSessionHealth();
    case 'local.zen_server': return ualZenServer(params?.port ?? 8558);
    case 'local.content_move': return ualContentMove(params, timeoutMs);
    case 'local.rollback': return ualContentRollback(params, timeoutMs);
    case 'local.path_refs': return ualProjectPathRefs(params);
    default: return { ok: false, error: `未知本地工具：${name}` };
  }
}
