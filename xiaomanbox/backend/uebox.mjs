/**
 * seam.ue5 —— UEBox MCP 客户端（后端独立进程内运行）
 *
 * 协议：MCP 旧版 2025-06-18（有 session + SSE，UEBox 实测就是这一版，不能用新 stateless SDK）。
 * 握手：POST initialize（Bearer token）→ 拿 mcp-session-id 响应头 →
 *       POST notifications/initialized（202）→ tools/list。
 * 响应格式：text/event-stream，取 `data:` 行 JSON.parse。
 *
 * 工具分级（risk: safe/mutating/destructive）：
 *   MCP 的 tools/list **没有** annotations（实测），分级来自 UEBox CLI
 *   `uebox tools list --allow-write --json`（risk 字段 + writeToolCount）。
 *   CLI 不可用时降级到 MCP tools/list 只报数量，**不编数字**（规矩 4）。
 *
 * 鉴权（立项 §9.3）：token 只从本机 UEBox 配置读，只留在后端进程内，
 * 任何契约响应都不得回传 token。
 */

import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import http from 'node:http';
import { spawn } from 'node:child_process';

export const MCP_PROTOCOL_VERSION = '2025-06-18';
const TOOLS_TTL_MS = 300_000;

/** UEBox MCP 配置（port + token），路径可用环境变量 UEBOX_CONFIG 覆盖 */
function configPath() {
  if (process.env.UEBOX_CONFIG) return process.env.UEBOX_CONFIG;
  const appData = process.env.APPDATA || path.join(os.homedir(), 'AppData', 'Roaming');
  return path.join(appData, 'unreal-box', 'mcp-server.json');
}

function readConfig() {
  try {
    const cfg = JSON.parse(fs.readFileSync(configPath(), 'utf8'));
    if (!cfg || !cfg.port || !cfg.token) return null;
    return {
      port: cfg.port,
      token: cfg.token,
      endpoint: `http://127.0.0.1:${cfg.port}/mcp`,
    };
  } catch {
    return null;
  }
}

/** UEBox CLI（uebox.cmd），路径可用环境变量 UEBOX_CLI 覆盖 */
function cliPath() {
  if (process.env.UEBOX_CLI) return process.env.UEBOX_CLI;
  return 'uebox.cmd';
}

/** 从 SSE 文本里取第一个 data: 行并 JSON.parse */
function parseSseData(text) {
  for (const line of String(text).split('\n')) {
    if (line.startsWith('data:')) {
      try {
        return JSON.parse(line.slice(5).trim());
      } catch {
        /* 继续找下一行 */
      }
    }
  }
  return null;
}

function mcpPost(endpoint, token, body, sessionId, timeoutMs = 15000) {
  return new Promise((resolve, reject) => {
    const u = new URL(endpoint);
    const data = JSON.stringify(body);
    const req = http.request(
      {
        hostname: u.hostname,
        port: u.port,
        path: u.pathname,
        method: 'POST',
        headers: {
          Authorization: `Bearer ${token}`,
          'Content-Type': 'application/json',
          Accept: 'application/json, text/event-stream',
          ...(sessionId ? { 'Mcp-Session-Id': sessionId } : {}),
          'Content-Length': Buffer.byteLength(data),
        },
        timeout: timeoutMs,
      },
      (res) => {
        let buf = '';
        res.on('data', (c) => (buf += c));
        res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, text: buf }));
      }
    );
    req.on('timeout', () => req.destroy(new Error('UEBox 请求超时')));
    req.on('error', reject);
    req.write(data);
    req.end();
  });
}

// ── MCP 会话 ────────────────────────────────────────────────
let session = null; // { id, serverInfo, protocolVersion }

async function ensureSession(force = false) {
  if (session && !force) return session;
  const cfg = readConfig();
  if (!cfg) throw new Error('UEBox 未配置（找不到 mcp-server.json 或缺 port/token）');

  const init = await mcpPost(cfg.endpoint, cfg.token, {
    jsonrpc: '2.0',
    id: 1,
    method: 'initialize',
    params: {
      protocolVersion: MCP_PROTOCOL_VERSION,
      capabilities: {},
      clientInfo: { name: 'xiaoman-box', version: '0.3.0' },
    },
  });
  if (init.status !== 200) {
    throw new Error(`initialize 失败 HTTP ${init.status}${init.status === 401 ? '（token 不对？）' : ''}`);
  }
  const sid = init.headers['mcp-session-id'];
  if (!sid) throw new Error('initialize 响应缺 mcp-session-id');
  const parsed = parseSseData(init.text);
  session = {
    id: sid,
    serverInfo: parsed?.result?.serverInfo ?? null,
    protocolVersion: parsed?.result?.protocolVersion ?? MCP_PROTOCOL_VERSION,
  };

  // initialized 通知（期望 202，无 body）
  await mcpPost(cfg.endpoint, cfg.token, { jsonrpc: '2.0', method: 'notifications/initialized' }, sid);
  return session;
}

/** MCP tools/list（后备数据源：只有 name/description，无 risk） */
async function fetchToolsViaMcp() {
  const cfg = readConfig();
  if (!cfg) throw new Error('UEBox 未配置');
  const s = await ensureSession();
  const r = await mcpPost(cfg.endpoint, cfg.token, { jsonrpc: '2.0', id: 2, method: 'tools/list', params: {} }, s.id);
  if (r.status !== 200) throw new Error(`tools/list 失败 HTTP ${r.status}`);
  const parsed = parseSseData(r.text);
  const tools = parsed?.result?.tools;
  if (!Array.isArray(tools)) throw new Error('tools/list 响应里没有 tools 数组');
  return tools.map((t) => ({
    name: t.name,
    brief: typeof t.description === 'string' ? t.description.slice(0, 160) : undefined,
    risk: undefined,
    namespace: undefined,
  }));
}

/**
 * CLI tools list --allow-write --json（权威数据源：带 risk/namespace/brief）。
 * spawn .cmd 必须 shell 方式（Node ≥18.20 CVE-2024-27980）。
 */
function fetchToolsViaCli(timeoutMs = 30000) {
  return new Promise((resolve, reject) => {
    const cmd = `"${cliPath()}" tools list --allow-write --json`;
    const child = spawn(cmd, { shell: true, windowsHide: true });
    let out = '';
    let err = '';
    const timer = setTimeout(() => child.kill(), timeoutMs);
    child.stdout.on('data', (c) => (out += c));
    child.stderr.on('data', (c) => (err += c));
    child.on('exit', (code) => {
      clearTimeout(timer);
      if (code !== 0) return reject(new Error(`uebox CLI 退出码 ${code}${err ? '：' + err.slice(0, 120) : ''}`));
      try {
        const parsed = JSON.parse(out);
        const tools = parsed?.data?.tools;
        if (!Array.isArray(tools)) return reject(new Error('CLI JSON 里没有 data.tools'));
        resolve({
          tools: tools.map((t) => ({
            name: t.name,
            brief: typeof t.brief === 'string' ? t.brief.slice(0, 160) : undefined,
            risk: t.risk,
            namespace: t.namespace,
          })),
          writeToolCount: parsed.data.writeToolCount,
          returnedCount: parsed.data.returnedCount,
        });
      } catch (e) {
        reject(new Error(`CLI JSON 解析失败: ${e.message}`));
      }
    });
    child.on('error', (e) => {
      clearTimeout(timer);
      reject(e);
    });
  });
}

// ── 缓存 ────────────────────────────────────────────────────
let toolsCache = null; // { tools, source: 'cli'|'mcp', writeToolCount, ts }

async function loadTools(force = false) {
  if (!force && toolsCache && Date.now() - toolsCache.ts < TOOLS_TTL_MS) return toolsCache;
  try {
    const viaCli = await fetchToolsViaCli();
    toolsCache = { ...viaCli, source: 'cli', ts: Date.now() };
  } catch {
    // CLI 不可用 → 降级 MCP tools/list（只报数量，不编分级）
    const tools = await fetchToolsViaMcp();
    toolsCache = { tools, source: 'mcp', writeToolCount: undefined, ts: Date.now() };
  }
  return toolsCache;
}

/**
 * 调 UEBox 工具（MCP tools/call）。
 * 长操作（建工程/开编辑器）可能数分钟 → 超时由调用方给。
 * 返回 { ok, data, text, error }：data = structuredContent 或解析后的 content[0].text。
 */
export async function ueboxCall(name, args = {}, timeoutMs = 180_000) {
  const cfg = readConfig();
  if (!cfg) return { ok: false, error: 'UEBox 未配置' };
  try {
    const s = await ensureSession();
    const r = await mcpPost(
      cfg.endpoint,
      cfg.token,
      { jsonrpc: '2.0', id: Date.now(), method: 'tools/call', params: { name, arguments: args } },
      s.id,
      timeoutMs
    );
    if (r.status !== 200) return { ok: false, error: `tools/call HTTP ${r.status}` };
    const parsed = parseSseData(r.text);
    if (parsed?.error) return { ok: false, error: parsed.error.message || JSON.stringify(parsed.error) };
    const result = parsed?.result;
    if (!result) return { ok: false, error: 'tools/call 响应无 result' };
    if (result.isError) {
      const t = result.content?.map((c) => c.text || '').join('\n') || '未知工具错误';
      return { ok: false, error: t.slice(0, 500) };
    }
    let data = result.structuredContent ?? null;
    if (!data && Array.isArray(result.content)) {
      const text = result.content.map((c) => c.text || '').join('\n');
      try {
        data = JSON.parse(text);
      } catch {
        data = null;
        return { ok: true, data: null, text };
      }
    }
    return { ok: true, data, text: undefined };
  } catch (e) {
    return { ok: false, error: String(e.message || e) };
  }
}

/**
 * 白名单校验版 ueboxCall（工作台通用工具入口用）。
 * 只放行 safe / mutating；destructive 与未知工具一律拒（deny-by-default）。
 */
export async function ueboxCallChecked(name, args = {}, timeoutMs = 240_000) {
  let tc;
  try {
    tc = await loadTools();
  } catch {
    return { ok: false, code: 500, error: '无法获取工具分级清单（CLI 与 MCP 都不通）' };
  }
  const t = tc.tools.find((x) => x.name === name);
  if (!t) return { ok: false, code: 404, error: `工具不在清单：${name}` };
  if (t.risk === 'destructive') return { ok: false, code: 403, error: `destructive 工具被拒（${name}）` };
  return ueboxCall(name, args, timeoutMs);
}

function riskBreakdown(tools) {
  let safe = 0;
  let mutating = 0;
  let destructive = 0;
  const namespaces = new Set();
  for (const t of tools) {
    if (t.risk === 'safe') safe++;
    else if (t.risk === 'mutating') mutating++;
    else if (t.risk === 'destructive') destructive++;
    if (t.namespace) namespaces.add(t.namespace);
  }
  return { safe, mutating, destructive, namespaceCount: namespaces.size };
}

/** 契约端点用：UEBox 连接状态（不含 token） */
export async function ueboxStatus() {
  const cfg = readConfig();
  const base = { contract: 'v1', configured: Boolean(cfg), endpoint: cfg ? cfg.endpoint : null };
  if (!cfg) return { ...base, connected: false, error: '未找到 UEBox 配置' };
  try {
    const s = await ensureSession();
    const tc = await loadTools();
    const bd = riskBreakdown(tc.tools);
    return {
      ...base,
      connected: true,
      serverInfo: s.serverInfo,
      protocolVersion: s.protocolVersion,
      toolCount: tc.tools.length,
      safeCount: bd.safe,
      mutatingCount: bd.mutating,
      destructiveCount: bd.destructive,
      writeCount: tc.writeToolCount ?? (bd.mutating + bd.destructive || undefined),
      namespaceCount: bd.namespaceCount || undefined,
      riskSource: tc.source,
    };
  } catch (e) {
    session = null;
    return { ...base, connected: false, error: String(e.message || e) };
  }
}

/**
 * 只读缓存版工具清单（供全局搜索等「绝不能阻塞」的场景用）。
 * 与 ueboxTools 的区别：绝不触发 CLI / MCP 网络调用——
 * 命中 5 分钟缓存就返回，否则立即返回空数组 + stale:true。
 * 这样 UEBox 离线时全局搜索不会卡 30 秒（外部 0 依赖）。
 */
export function ueboxToolsCached() {
  const cfg = readConfig();
  const base = { contract: 'v1', configured: Boolean(cfg), endpoint: cfg ? cfg.endpoint : null };
  if (!cfg) return { ...base, count: 0, tools: [], stale: true, error: '未找到 UEBox 配置' };
  if (!toolsCache || Date.now() - toolsCache.ts >= TOOLS_TTL_MS) {
    // 无缓存或已过期：不阻塞，直接返回空 + 标记 stale
    return { ...base, count: 0, tools: [], stale: true, error: '工具清单缓存过期（UEBox 未刷新）' };
  }
  return {
    ...base,
    count: toolsCache.tools.length,
    riskSource: toolsCache.source,
    tools: toolsCache.tools,
    stale: false,
  };
}

/** 契约端点用：工具清单（缓存 5 分钟；force=1 强刷） */
export async function ueboxTools(force = false) {
  const cfg = readConfig();
  const base = { contract: 'v1', configured: Boolean(cfg), endpoint: cfg ? cfg.endpoint : null };
  if (!cfg) return { ...base, count: 0, tools: [], error: '未找到 UEBox 配置' };
  try {
    const tc = await loadTools(force);
    return {
      ...base,
      count: tc.tools.length,
      riskSource: tc.source,
      tools: tc.tools,
    };
  } catch (e) {
    return { ...base, count: 0, tools: [], error: String(e.message || e) };
  }
}

/** 测试钩子：清空会话与缓存 */
export function ueboxReset() {
  session = null;
  toolsCache = null;
  schemaCache = null;
}

// ── 工具完整 inputSchema（MCP tools/list 才有，CLI list 不含）──
let schemaCache = null; // { map: Map<name, tool>, ts }

async function loadMcpSchemas(force = false) {
  if (!force && schemaCache && Date.now() - schemaCache.ts < TOOLS_TTL_MS) return schemaCache;
  const cfg = readConfig();
  if (!cfg) throw new Error('UEBox 未配置');
  const s = await ensureSession();
  const r = await mcpPost(
    cfg.endpoint,
    cfg.token,
    { jsonrpc: '2.0', id: Date.now(), method: 'tools/list', params: {} },
    s.id,
    30000
  );
  if (r.status !== 200) throw new Error(`tools/list HTTP ${r.status}`);
  const parsed = parseSseData(r.text);
  const tools = parsed?.result?.tools;
  if (!Array.isArray(tools)) throw new Error('tools/list 响应无 tools');
  schemaCache = { map: new Map(tools.map((t) => [t.name, t])), ts: Date.now() };
  return schemaCache;
}

/** 契约端点用：查单个工具的完整 inputSchema（用于"参数不猜"） */
export async function ueboxToolSchema(name) {
  try {
    const { map } = await loadMcpSchemas();
    const t = map.get(name);
    if (!t) return { contract: 'v1', ok: false, code: 404, error: `工具不存在：${name}` };
    return {
      contract: 'v1',
      ok: true,
      name: t.name,
      description: typeof t.description === 'string' ? t.description.slice(0, 300) : undefined,
      inputSchema: t.inputSchema ?? null,
    };
  } catch (e) {
    return { contract: 'v1', ok: false, code: 500, error: String(e.message || e) };
  }
}

/**
 * 给本地模型「小满」用的工具清单（OpenAI function-calling 格式）。
 * 只放行 safe / mutating 且属于 UE 常用范围的工具，描述截断到 200 字 ——
 * 32K 上下文放不下 163 个工具的完整描述，必须精选。
 */
const MODEL_TOOL_ALLOW = [
  /^blueprint_/,
  'ue_spawn_actor',
  'ue_get_actor',
  'ue_set_transform',
  'ue_save_level',
  'ue_get_current_level',
  'ue_get_levels',
  'ue_get_project_info',
  'ue_session_health',
  'ue_content_search',
  'ue_input_map',
  'project_list',
  'project_manage',
];

/**
 * 压缩 JSON Schema 给模型：NInfer 的工具解析器不吃 `$schema` / `default` / `examples`，
 * 且完整 schema 里的 description 会撑爆 32K 上下文 —— 一律剥掉/截断。
 */
function compactSchema(node, depth = 0) {
  if (depth > 6) return undefined;
  if (Array.isArray(node)) return node.map((n) => compactSchema(n, depth + 1));
  if (node && typeof node === 'object') {
    const out = {};
    for (const [k, v] of Object.entries(node)) {
      if (k === '$schema' || k === 'default' || k === 'examples' || k === 'title') continue;
      if (k === 'description' && typeof v === 'string') {
        out[k] = v.slice(0, 80);
        continue;
      }
      out[k] = compactSchema(v, depth + 1);
    }
    return out;
  }
  return node;
}

export async function ueboxModelTools() {
  const tc = await loadTools(); // CLI：risk / brief
  const { map } = await loadMcpSchemas(); // MCP：inputSchema / 完整描述
  const out = [];
  for (const t of tc.tools) {
    if (t.risk !== 'safe' && t.risk !== 'mutating') continue; // destructive / unknown 一律不进模型
    const allow = MODEL_TOOL_ALLOW.some((a) => (a instanceof RegExp ? a.test(t.name) : a === t.name));
    if (!allow) continue;
    const schema = map.get(t.name)?.inputSchema;
    const desc = (map.get(t.name)?.description || t.brief || '').slice(0, 160);
    out.push({
      type: 'function',
      function: {
        name: t.name,
        description: desc,
        parameters:
          schema && typeof schema === 'object'
            ? (compactSchema(schema) ?? { type: 'object', properties: {} })
            : { type: 'object', properties: {} },
      },
    });
  }
  return out;
}
