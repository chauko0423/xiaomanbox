/**
 * XiaoMan Box 后端 —— 独立进程，契约 v1
 *
 * 职责：向前端（Vue3+TS）提供带版本号的 HTTP 契约 /api/v1/*。
 * 定位：独立进程（立项 §9.2 铁律 #1 前后端分离 + #2 插件/工具解耦），
 *       由 Electron 主进程 spawn 管理生命周期（见 electron/main.cjs）。
 *
 * v1 契约（增量演进，见 backend/contract.md）：
 *   GET  /api/v1/health            健康检查
 *   GET  /api/v1/status            后端进程状态
 *   GET  /api/v1/state/:key        W-07 读状态（~/.xiaomanbox/<key>.json）
 *   PUT  /api/v1/state/:key        W-07 写状态
 *   GET  /api/v1/uebox/status      seam.ue5 UEBox MCP 连接状态
 *   GET  /api/v1/uebox/tools       seam.ue5 工具清单（缓存 5 分钟）
 *   POST /api/v1/compiler/verify   seam.compiler 触发编译验证（单任务互斥）
 *   GET  /api/v1/compiler/job      seam.compiler 当前/最近任务
 */

import './ws-polyfill.mjs'; // 必须第一个 import：补齐 Electron 内置 Node 缺失的全局 WebSocket
import http from 'node:http';
import os from 'node:os';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { ueboxStatus, ueboxTools, ueboxCallChecked, ueboxToolSchema } from './uebox.mjs';
import * as ual from './ual.mjs';
import { ualToolList, findUalTool, executeUalTool } from './ual-tools.mjs';
import * as nodelib from './nodelib.mjs';
import * as compiler from './compiler.mjs';
import * as ue5boot from './ue5boot.mjs';
import * as agent from './agent.mjs';
import * as keepalive from './keepalive.mjs';
import * as session from './session.mjs';
import * as skills from './skills.mjs';
import * as mcp from './mcp.mjs';
import * as events from './events.mjs';
import * as teamboard from './teamboard.mjs';



import * as search from './search.mjs';

import * as providers from './providers.mjs';
import * as supervisor from './supervisor.mjs';
import * as work from './work.mjs';
import * as pluginsCommunity from './plugins-community.mjs';
import * as dshBridge from './dsh-bridge.mjs';
import * as vault from './vault.mjs';
import * as engine from './engine.mjs';
import * as enginectl from './enginectl.mjs';
import * as presets from './presets.mjs';
import * as workspace from './workspace.mjs';
import * as asr from './asr.mjs';
import * as basic from './basic-tools.mjs';

const PORT = Number(process.env.WORKBENCH_BACKEND_PORT ?? 8871);
const HOST = '127.0.0.1'; // AI/file/UE APIs are local-only in the public edition.
const CONTRACT = 'v1';
const APP = 'XiaoMan Box';
const VERSION = '0.3.0';

/** W-07 状态目录（铁律 #3：状态存文件系统，不存浏览器） */
const STATE_DIR = path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'));
/** 键名白名单：只允许安全文件名字符，从根上堵死路径穿越 */
const KEY_RE = /^[A-Za-z0-9_-]{1,64}$/;

// ── API key（分级鉴权，2026-10-08：放开外网前的安全地基）──────
// 本机回环（127.0.0.1）免 key（信任边界内）；非回环（局域网/外网）强制 X-API-Key。
// key 存 ~/.xiaomanbox/api-key（首次自动生成 64 hex），env XIAOMAN_API_KEY 可显式覆盖。
const API_KEY_FILE = path.join(STATE_DIR, 'api-key');

function loadApiKey() {
  if (process.env.XIAOMAN_API_KEY) return process.env.XIAOMAN_API_KEY;
  try {
    const k = fs.readFileSync(API_KEY_FILE, 'utf8').trim();
    if (k) return k;
  } catch {
    /* 无 key 文件 */
  }
  const k = crypto.randomBytes(32).toString('hex');
  try {
    fs.mkdirSync(STATE_DIR, { recursive: true });
    fs.writeFileSync(API_KEY_FILE, k, 'utf8');
  } catch {
    /* 写失败就用内存里的（本次进程有效） */
  }
  return k;
}

function resetApiKey() {
  const k = crypto.randomBytes(32).toString('hex');
  try {
    fs.mkdirSync(STATE_DIR, { recursive: true });
    fs.writeFileSync(API_KEY_FILE, k, 'utf8');
  } catch {
    /* 写失败 */
  }
  return k;
}

function isLoopback(remote) {
  return remote === '127.0.0.1' || remote === '::1' || remote === '::ffff:127.0.0.1' || remote === '::ffff:7f00:1';
}

const startedAt = Date.now();

const CORS_HEADERS = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET, PUT, POST, OPTIONS',
  'Access-Control-Allow-Headers': 'Content-Type, X-API-Key',
};

function send(res, status, body) {
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    ...CORS_HEADERS,
  });
  res.end(JSON.stringify(body));
}

function notFound(res, pathname) {
  send(res, 404, {
    error: 'not_found',
    path: pathname,
    contract: CONTRACT,
    hint: '可用端点见 GET /api/v1/',
  });
}

function readBody(req, limit = 1_000_000) {
  return new Promise((resolve, reject) => {
    let size = 0;
    const chunks = [];
    req.on('data', (c) => {
      size += c.length;
      if (size > limit) {
        reject(new Error('body too large'));
        req.destroy();
        return;
      }
      chunks.push(c);
    });
    req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
    req.on('error', reject);
  });
}

async function readJsonBody(req) {
  const raw = await readBody(req).catch(() => '');
  if (!raw.trim()) return {};
  try {
    return JSON.parse(raw);
  } catch {
    return {};
  }
}

/** 二进制 body（ASR 用）：上限保护，默认 15MB ≈ 4 分钟 16k/16bit 单声道录音 */
function readRawBody(req, limit = 15 * 1024 * 1024) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    req.on('data', (c) => {
      size += c.length;
      if (size > limit) {
        reject(new Error('body_too_large'));
        req.destroy();
        return;
      }
      chunks.push(c);
    });
    req.on('end', () => resolve(Buffer.concat(chunks)));
    req.on('error', reject);
  });
}

// ── W-07 状态读写 ───────────────────────────────────────────
function statePath(key) {
  return path.join(STATE_DIR, `${key}.json`);
}

function handleStateGet(res, key) {
  if (!KEY_RE.test(key)) {
    return send(res, 400, { error: 'invalid_key', contract: CONTRACT, hint: '键名只允许 [A-Za-z0-9_-]{1,64}' });
  }
  let raw;
  try {
    raw = fs.readFileSync(statePath(key), 'utf8');
  } catch {
    return send(res, 404, { error: 'not_found', key, contract: CONTRACT, hint: '还没有这份状态（正常，首次使用）' });
  }
  let data;
  try {
    data = JSON.parse(raw);
  } catch {
    return send(res, 500, { error: 'state_corrupt', key, contract: CONTRACT });
  }
  return send(res, 200, { contract: CONTRACT, key, data });
}

async function handleStatePut(req, res, key) {
  if (!KEY_RE.test(key)) {
    return send(res, 400, { error: 'invalid_key', contract: CONTRACT, hint: '键名只允许 [A-Za-z0-9_-]{1,64}' });
  }
  const body = await readBody(req);
  let data;
  try {
    data = JSON.parse(body);
  } catch {
    return send(res, 400, { error: 'invalid_json', contract: CONTRACT, hint: '请求体必须是合法 JSON' });
  }
  fs.mkdirSync(STATE_DIR, { recursive: true });
  const file = statePath(key);
  const tmp = `${file}.tmp`;
  fs.writeFileSync(tmp, JSON.stringify(data, null, 2), 'utf8');
  fs.renameSync(tmp, file); // 原子落盘：先写临时文件再改名
  return send(res, 200, { contract: CONTRACT, ok: true, key, bytes: Buffer.byteLength(JSON.stringify(data, null, 2)) });
}

// ── 路由 ────────────────────────────────────────────────────
const STATE_RE = /^\/api\/v1\/state\/([A-Za-z0-9_-]{1,64})$/;

const server = http.createServer(async (req, res) => {
  if (req.method === 'OPTIONS') {
    res.writeHead(204, CORS_HEADERS);
    res.end();
    return;
  }

  const url = new URL(req.url, `http://${req.headers.host ?? HOST}`);
  const p = url.pathname;

  // 分级鉴权：本机回环免 key；非回环（局域网/外网）强制 X-API-Key，缺/错一律 401。
  if (!isLoopback(String(req.socket?.remoteAddress || ''))) {
    const key = loadApiKey();
    if (!key || String(req.headers['x-api-key'] || '') !== key) {
      return send(res, 401, { contract: CONTRACT, ok: false, error: '非本机访问需要有效的 X-API-Key' });
    }
  }

  try {
    if (req.method === 'GET') {
      if (p === '/api/v1/') {
        return send(res, 200, {
          contract: CONTRACT,
          app: APP,
          version: VERSION,
          endpoints: [
            'GET /api/v1/health',
            'GET /api/v1/status',
            'GET|PUT /api/v1/state/:key',
            'GET /api/v1/uebox/status',
            'GET /api/v1/uebox/tools',
            'POST /api/v1/compiler/verify',
            'GET /api/v1/compiler/job',
          ],
        });
      }
      if (p === '/api/v1/health') {
        return send(res, 200, {
          contract: CONTRACT,
          ok: true,
          app: APP,
          version: VERSION,
          ts: new Date().toISOString(),
        });
      }
      if (p === '/api/v1/status') {
        const mem = process.memoryUsage();
        return send(res, 200, {
          contract: CONTRACT,
          app: APP,
          backendPid: process.pid,
          hostname: os.hostname(),
          platform: process.platform,
          arch: process.arch,
          nodeVersion: process.version,
          uptimeSec: Math.round((Date.now() - startedAt) / 1000),
          memory: { rss: mem.rss, heapUsed: mem.heapUsed },
        });
      }

      const m = STATE_RE.exec(p);
      if (m) return handleStateGet(res, m[1]);

      // ── 预设清单（2026-10-08）：大板块 + 小板块 + 专属系统提示词 ──
      if (p === '/api/v1/presets') {
        return send(res, 200, { contract: CONTRACT, ...presets.presetsInfo() });
      }

      // ── 工作区（2026-10-08 用户）：欢迎页「工作区 ▾」选择器 ──
      if (p === '/api/v1/workspace') {
        return send(res, 200, { contract: CONTRACT, ...workspace.listWorkspaces() });
      }

      if (p === '/api/v1/uebox/status') {
        return send(res, 200, await ueboxStatus());
      }
      if (p === '/api/v1/uebox/tools') {
        return send(res, 200, await ueboxTools(url.searchParams.get('force') === '1'));
      }
      if (p === '/api/v1/ual/status') {
        const st = ual.ualStatus();
        return send(res, 200, { ...st, toolCount: ualToolList().length });
      }
      if (p === '/api/v1/ual/tools') {
        return send(res, 200, { contract: 'v1', count: ualToolList().length, tools: ualToolList() });
      }
      // ── W-11 引擎状态：只读消费 8390 真实快照（档位/上下文/profiles）──
      if (p === '/api/v1/engine/status') {
        return send(res, 200, { contract: CONTRACT, ...(await engine.engineStatus()) });
      }
      // ── 引擎控制：模型启动/停止 + 开放 API 状态（2026-10-07）──
      if (p === '/api/v1/engine/control') {
        return send(res, 200, await enginectl.engineControlStatus());
      }
      if (p === '/api/v1/nodes/stats') {
        return send(res, 200, nodelib.nodeLibStats());
      }
      if (p === '/api/v1/nodes/search') {
        return send(res, 200, nodelib.nodeSearch(url.searchParams.get('q') || '', url.searchParams.get('limit')));
      }
      if (p === '/api/v1/nodes/describe') {
        return send(res, 200, nodelib.nodeDescribe(url.searchParams.get('name') || ''));
      }
      const schemaMatch = /^\/api\/v1\/uebox\/schema\/([A-Za-z0-9_.]+)$/.exec(p);
      if (schemaMatch) {
        return send(res, 200, await ueboxToolSchema(decodeURIComponent(schemaMatch[1])));
      }
      if (p === '/api/v1/compiler/job') {
        return send(res, 200, compiler.jobInfo());
      }
      if (p === '/api/v1/ue5/bootstrap') {
        return send(res, 200, ue5boot.jobInfo());
      }
      if (p === '/api/v1/ue5/projects') {
        return send(res, 200, ue5boot.listProjects());
      }
      // ── M1 魂：会话与记忆 ─────────────────────────────
      if (p === '/api/v1/sessions') {
        // running 标记：该会话是否有 agent 任务正在跑（侧栏画转圈，2026-10-08）
        const rid = agent.runningSessionId();
        return send(res, 200, {
          contract: CONTRACT,
          sessions: session.listSessions().map((s) => ({ ...s, running: !!rid && s.sessionId === rid })),
        });
      }
      if (p === '/api/v1/memory/global') {
        return send(res, 200, { contract: CONTRACT, memory: session.getGlobalMemory() });
      }
      let sm = /^\/api\/v1\/session\/scene\/([A-Za-z0-9_-]+)$/.exec(p);
      if (sm) {
        const s = session.getOrCreateSessionForScene(sm[1]);
        return send(res, 200, { contract: CONTRACT, session: s });
      }
      sm = /^\/api\/v1\/session\/([A-Za-z0-9_-]+)\/messages$/.exec(p);
      if (sm) {
        const s = session.getSession(sm[1]);
        if (!s) return send(res, 404, { contract: CONTRACT, error: 'session_not_found' });
        return send(res, 200, { contract: CONTRACT, sessionId: sm[1], messages: s.messages });
      }
      sm = /^\/api\/v1\/session\/([A-Za-z0-9_-]+)$/.exec(p);
      if (sm) {
        const s = session.getSession(sm[1]);
        if (!s) return send(res, 404, { contract: CONTRACT, error: 'session_not_found' });
        return send(res, 200, { contract: CONTRACT, session: s });
      }
      sm = /^\/api\/v1\/memory\/scene\/([A-Za-z0-9_-]+)$/.exec(p);
      if (sm) {
        return send(res, 200, { contract: CONTRACT, sceneId: sm[1], memory: session.getSceneMemory(sm[1]) });
      }

      // ── M2b 生态：skill 库 ─────────────────────────────
      if (p === '/api/v1/skills') {
        return send(res, 200, { contract: CONTRACT, ...skills.listSkills() });
      }
      sm = /^\/api\/v1\/skills\/([A-Za-z0-9-]+)$/.exec(p);
      if (sm) {
        const r = skills.loadSkill(sm[1]);
        return send(res, r.ok ? 200 : r.code ?? 500, { contract: CONTRACT, ...r });
      }

      // ── M2b 生态：通用 MCP 接入（A 类）──────────────────
      if (p === '/api/v1/mcp/servers') {
        return send(res, 200, { contract: CONTRACT, servers: mcp.listServers() });
      }

      // ── M3 眼：事件订阅镜像 + 系统内存 ─────────────────
      if (p === '/api/v1/events') {
        const since = Number(url.searchParams.get('since') || 0);
        return send(res, 200, { contract: CONTRACT, latest: events.latestId(), events: events.listSince(since) });
      }
      if (p === '/api/v1/search') {
        return send(res, 200, { contract: CONTRACT, ...(await search.search(url.searchParams.get('q') || '')) });
      }
      // ── 团队画板：一键拉起协作底座 + 前端 ─────────────────
      if (p === '/api/v1/teamboard/status') {
        return send(res, 200, await teamboard.teamboardStatus());
      }
      // 现铸全新 SSO URL（面板打开时取一次；修复令牌过期/换账号不刷新）
      if (p === '/api/v1/teamboard/url') {
        return send(res, 200, teamboard.teamboardUrl());
      }
      // 打开指定群组（= 团队交流频道）的团队画板 URL
      
      // ── M-13 成员/设备发现（局域网在线表 + 底座自动发现）─────
      
      
      // ── 团队交流：频道 + 消息（Yjs/Hocuspocus）────────────
      
      // 群组详情：owner / admins / members（成员管理弹层用）
      const cmGet = /^\/api\/v1\/chat\/channels\/([A-Za-z0-9_-]+)\/members$/.exec(p);
      
      
      
      // ── 好友（手机号搜索添加；关系存画板服务端，跨设备可用）──
      
      
      
      
      // ── 账户体系（M-17 注册 + M-18 持久化）──────────────
      
      if (p === '/api/v1/system-memory') {
        const total = os.totalmem();
        const free = os.freemem();
        return send(res, 200, {
          contract: CONTRACT,
          totalBytes: total,
          freeBytes: free,
          usedBytes: total - free,
          freeGiB: Number((free / 1073741824).toFixed(2)),
          totalGiB: Number((total / 1073741824).toFixed(2)),
          lowBelow8GiB: free < 8 * 1073741824,
        });
      }

      if (p === '/api/v1/agent/job') {
        return send(res, 200, agent.agentJobInfo());
      }

      // 桌宠状态订阅（2026-10-10）：busy/lastError 驱动桌宠表情
      if (p === '/api/v1/pet/status') {
        const st = agent.agentJobInfo();
        return send(res, 200, {
          contract: CONTRACT,
          busy: st.state === 'running',
          lastError: st.state === 'failed',
          state: st.state,
        });
      }

      // 上下文用量（对标 UEBox「上下文用量」面板）：分类 token 统计
      if (p === '/api/v1/agent/context-usage') {
        const u = new URL(req.url, 'http://localhost');
        return send(res, 200, await agent.contextUsage(u.searchParams.get('sessionId') || undefined));
      }

      // API key（分级鉴权）：本机回环可读，返回 key 供用户复制给外部 AI（GPT/WorkBuddy 等）
      if (p === '/api/v1/auth/key') {
        return send(res, 200, { contract: CONTRACT, ok: true, apiKey: loadApiKey(), note: '跨机/外网调用时在请求头带 X-API-Key: <此值>' });
      }
      // 产出文件列表（小满 present 的文件；?sessionId= 只返回该会话登记的，防串台）
      if (p === '/api/v1/presented') {
        return send(res, 200, { contract: CONTRACT, ...basic.listPresented(url.searchParams.get('sessionId') || undefined) });
      }
      // 产出物文件内容（2026-10-10 用户「侧边栏内预览产出物」）：?path= 完整路径。
      //   文本类返回 text（前端渲染 md/html），图片返回 dataUrl，其他返回 kind:binary。
      if (p === '/api/v1/file') {
        const fp = url.searchParams.get('path') || '';
        try {
          if (!fp) return send(res, 400, { contract: CONTRACT, ok: false, error: '缺 path' });
          const st = fs.statSync(fp);
          if (!st.isFile()) return send(res, 400, { contract: CONTRACT, ok: false, error: '不是文件' });
          if (st.size > 5 * 1024 * 1024) return send(res, 400, { contract: CONTRACT, ok: false, error: '文件超过 5MB，请用系统程序打开' });
          const ext = path.extname(fp).toLowerCase();
          const IMG = new Set(['.png', '.jpg', '.jpeg', '.gif', '.webp', '.bmp', '.ico']);
          const TEXT = new Set(['.md', '.markdown', '.txt', '.html', '.htm', '.svg', '.json', '.css', '.js', '.ts', '.vue', '.py', '.csv', '.log', '.xml', '.yaml', '.yml']);
          const name = path.basename(fp);
          if (IMG.has(ext)) {
            const mime = ext === '.jpg' ? 'image/jpeg' : `image/${ext.slice(1)}`;
            return send(res, 200, { contract: CONTRACT, ok: true, name, kind: 'image', dataUrl: `data:${mime};base64,${fs.readFileSync(fp).toString('base64')}` });
          }
          if (TEXT.has(ext)) {
            return send(res, 200, { contract: CONTRACT, ok: true, name, kind: ext === '.html' || ext === '.htm' ? 'html' : ext === '.svg' ? 'image' : 'text', text: fs.readFileSync(fp, 'utf8') });
          }
          return send(res, 200, { contract: CONTRACT, ok: true, name, kind: 'binary', size: st.size });
        } catch (e) {
          return send(res, 404, { contract: CONTRACT, ok: false, error: String(e.message || e) });
        }
      }
      // 插件社区（dsh 生态内嵌能力目录；支持 ?category=&adapt=&q= 过滤）
      if (p === '/api/v1/plugins/community') {
        return send(res, 200, {
          contract: CONTRACT,
          ...pluginsCommunity.listCommunity({
            category: url.searchParams.get('category') || undefined,
            adapt: url.searchParams.get('adapt') || undefined,
            q: url.searchParams.get('q') || undefined,
          }),
        });
      }
      // 工具映射表（dsh 工具 → 小满等价工具）
      if (p === '/api/v1/plugins/mapping') {
        return send(res, 200, { contract: CONTRACT, ...pluginsCommunity.listToolMapping() });
      }
      // 工作台工具清单（含启用状态）——插件社区「下载/已装载」
      if (p === '/api/v1/plugins/tools') {
        return send(res, 200, { contract: CONTRACT, ...basic.listWorkbenchTools() });
      }
      // dsh 运行时检测（路线 B 外部接入前置）
      if (p === '/api/v1/plugins/dsh') {
        return send(res, 200, { contract: CONTRACT, ...dshBridge.bridgeDsh() });
      }
      // dsh 插件清单（路线 B：headless profile 已下载/装载的插件）
      if (p === '/api/v1/plugins/dsh/plugins') {
        return send(res, 200, { contract: CONTRACT, ...dshBridge.listDshPlugins() });
      }
      // 待答问题（ask_user 工具登记的问题，前端展示/回答）
      if (p === '/api/v1/plugins/ask') {
        return send(res, 200, { contract: CONTRACT, ...basic.listPendingQuestions() });
      }
      // ── 模型提供方 + Agent 运行参数（DSH「模型/插件」页底座）──
      if (p === '/api/v1/providers') {
        return send(res, 200, { contract: CONTRACT, ...providers.listProviders() });
      }
      // 主流模型预配置模板（前端「快速接入」折叠区；2026-10-11 用户）
      if (p === '/api/v1/providers/presets') {
        return send(res, 200, { contract: CONTRACT, ...providers.listPresets() });
      }
      if (p === '/api/v1/supervisor/status') {
        const info = supervisor.supervisorJobInfo();
        return send(res, 200, { contract: CONTRACT, ok: true, acceptModes: supervisor.ACCEPT_MODES, job: info });
      }
      // 外部监工接入规范（Skill）—— 用户复制给 GPT/WorkBuddy 当监工
      if (p === '/api/v1/supervisor/skill') {
        return send(res, 200, { contract: CONTRACT, ok: true, skill: work.SUPERVISOR_SKILL });
      }
      // 外部监工拉执行结果
      {
        const wm = /^\/api\/v1\/work\/status\/([A-Za-z0-9-]+)$/.exec(p);
        if (wm) {
          const wr = work.workStatus(wm[1]);
          return send(res, wr.ok ? 200 : wr.code ?? 500, { contract: CONTRACT, ...wr });
        }
      }
      if (p === '/api/v1/agent/config') {
        return send(res, 200, { contract: CONTRACT, ok: true, config: agent.getConfig() });
      }
      // ── 本地资料库：概览（只读，供资料库面板首屏）──────
      if (p === '/api/v1/vault/overview') {
        return send(res, 200, { contract: CONTRACT, ...vault.vaultLibraryOverview() });
      }
    }

    if (req.method === 'PUT') {
      const m = STATE_RE.exec(p);
      if (m) return await handleStatePut(req, res, m[1]);

      // ── M1 魂：会话改绑定 / 记忆写入 ─────────────────
      let sm = /^\/api\/v1\/session\/([A-Za-z0-9_-]+)$/.exec(p);
      if (sm) {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const s = session.updateSession(sm[1], body);
        if (!s) return send(res, 404, { contract: CONTRACT, error: 'session_not_found' });
        return send(res, 200, { contract: CONTRACT, session: s });
      }
      sm = /^\/api\/v1\/session\/scene\/([A-Za-z0-9_-]+)\/bind$/.exec(p);
      if (sm) {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        if (typeof body.sessionId !== 'string' || !body.sessionId) {
          return send(res, 400, { contract: CONTRACT, error: 'missing_sessionId' });
        }
        return send(res, 200, { contract: CONTRACT, ...session.bindScene(sm[1], body.sessionId) });
      }
      // 会话归档 / 取消归档（不删数据，收进/移出「已归档」分组）
      sm = /^\/api\/v1\/session\/([A-Za-z0-9_-]+)\/archive$/.exec(p);
      if (sm) {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const s = session.archiveSession(sm[1], body.archived !== false);
        if (!s) return send(res, 404, { contract: CONTRACT, error: 'session_not_found' });
        return send(res, 200, { contract: CONTRACT, session: s });
      }
      if (p === '/api/v1/memory/global') {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        return send(res, 200, { contract: CONTRACT, ...session.setGlobalMemory(body) });
      }
      sm = /^\/api\/v1\/memory\/scene\/([A-Za-z0-9_-]+)$/.exec(p);
      if (sm) {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        return send(res, 200, { contract: CONTRACT, ...session.setSceneMemory(sm[1], body) });
      }

      // ── 模型提供方：更新 / 激活 / 删除（动作式，沿用本文件 PUT 风格）──
      let pm = /^\/api\/v1\/providers\/([A-Za-z0-9-]+)\/activate$/.exec(p);
      if (pm) {
        return send(res, 200, { contract: CONTRACT, ...providers.activateProvider(pm[1]) });
      }
      pm = /^\/api\/v1\/providers\/([A-Za-z0-9-]+)\/remove$/.exec(p);
      if (pm) {
        return send(res, 200, { contract: CONTRACT, ...providers.removeProvider(pm[1]) });
      }
      pm = /^\/api\/v1\/providers\/([A-Za-z0-9-]+)$/.exec(p);
      if (pm) {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        return send(res, 200, { contract: CONTRACT, ...providers.updateProvider(pm[1], body) });
      }
      if (p === '/api/v1/agent/config') {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        return send(res, 200, { contract: CONTRACT, ...agent.setConfigPatch(body) });
      }
    }

    if (req.method === 'DELETE') {
      // ── 会话删除（不可恢复；与归档区别开）──────────────────
      let dm = /^\/api\/v1\/session\/([A-Za-z0-9_-]+)$/.exec(p);
      if (dm) {
        if (agent.runningSessionId() === dm[1]) {
          return send(res, 409, { contract: CONTRACT, error: 'session_running', hint: '任务进行中的会话不能删除，请等任务结束或先停止任务' });
        }
        const r = session.deleteSession(dm[1]);
        if (!r.ok) return send(res, 404, { contract: CONTRACT, error: 'session_not_found' });
        return send(res, 200, { contract: CONTRACT, ...r });
      }
      // ── skill 删除（删整个 skill 目录，不可恢复）──────────────
      dm = /^\/api\/v1\/skills\/([A-Za-z0-9-]+)$/.exec(p);
      if (dm) {
        const r = skills.deleteSkill(dm[1]);
        return send(res, r.ok ? 200 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      // ── 解散群组（删除团队交流频道，群主可操作）──────────────
      dm = /^\/api\/v1\/chat\/channels\/([A-Za-z0-9_-]+)$/.exec(p);
      
      // ── MCP 服务器删除（从注册表移除）────────────────────────
      dm = /^\/api\/v1\/mcp\/servers\/([A-Za-z0-9-]+)$/.exec(p);
      if (dm) {
        const r = mcp.removeServer(dm[1]);
        return send(res, r.ok ? 200 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
    }

    if (req.method === 'POST') {
      // ── 执行单个内置 UE 工具（前端面板用，如 NodeUE T3D 导出/导入）。deny destructive。──
      if (p === '/api/v1/ual/call') {
        // T3D 文本可能较大（插件上限 20 万字符），用 4MB 上限的读取，别用默认 1MB。
        const raw = await readBody(req, 4_000_000).catch(() => '');
        let body = {};
        try { body = raw.trim() ? JSON.parse(raw) : {}; } catch { body = {}; }
        const tool = findUalTool(body.name || '');
        if (!tool) return send(res, 404, { contract: CONTRACT, ok: false, error: `工具不存在：${body.name}` });
        if (tool.risk === 'destructive' && body.allowDestructive !== true) {
          // 画布等前端入口想用 destructive 工具（如删节点）：必须显式带 allowDestructive，
          // 且前端要在用户确认弹窗里才置真 —— 后端闸门 + 前端确认，双保险。
          return send(res, 403, { contract: CONTRACT, ok: false, error: `destructive 工具须经用户确认（allowDestructive）才可调用：${body.name}` });
        }
        const r = await executeUalTool(tool, body.args || {}, body.timeoutMs);
        return send(res, r.ok ? 200 : 502, { contract: CONTRACT, ...r });
      }
      // ── 工作区：添加 / 激活 / 移除（2026-10-08 用户）──
      if (p === '/api/v1/workspace/add') {
        const body = await readJsonBody(req);
        const r = workspace.addWorkspace(body?.path);
        return send(res, r.ok ? 200 : r.code ?? 400, { contract: CONTRACT, ...r });
      }
      if (p === '/api/v1/workspace/active') {
        const body = await readJsonBody(req);
        const r = workspace.setActiveWorkspace(body?.path);
        return send(res, r.ok ? 200 : r.code ?? 400, { contract: CONTRACT, ...r });
      }
      if (p === '/api/v1/workspace/remove') {
        const body = await readJsonBody(req);
        const r = workspace.removeWorkspace(body?.path);
        return send(res, r.ok ? 200 : r.code ?? 400, { contract: CONTRACT, ...r });
      }
      // 重置 API key（本机回环可调；重置后旧 key 立即失效）
      if (p === '/api/v1/auth/key/reset') {
        await readBody(req).catch(() => '');
        return send(res, 200, { contract: CONTRACT, ok: true, apiKey: resetApiKey() });
      }
      // ── 模型提供方：新增 ────────────────────────────────
      if (p === '/api/v1/providers') {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const r = providers.addProvider(body);
        return send(res, r.ok ? 201 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      // 设置监工角色绑定的提供方（云端/本地任意模型，与 active 执行者独立）
      if (p === '/api/v1/providers/supervisor') {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const r = providers.setSupervisorProvider(body.id);
        return send(res, r.ok ? 200 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      // 启动外部监工闭环：监工派单 → 小满执行 → 监工验收 → 迭代
      if (p === '/api/v1/supervisor/run') {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const r = supervisor.startSupervise(body.goal, body);
        return send(res, r.ok ? 202 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      // 外部监工接入端口：派单（GPT/WorkBuddy 等外部 AI 经此把任务交给小满执行）
      if (p === '/api/v1/work/assign') {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const r = work.assign(body);
        return send(res, r.ok ? 202 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      // 路线 B 外部接入：起 dsh 子进程（headless）真跑任务
      if (p === '/api/v1/plugins/dsh/run') {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const r = await dshBridge.runDsh(body.task, { timeoutMs: Number(body.timeoutMs) || undefined });
        return send(res, r.ok ? 200 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      // 路线 B 真下载/卸载原生 dsh 插件（转发 dsh plugin → pnpm）
      if (p === '/api/v1/plugins/dsh/install') {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const r = dshBridge.installDshPlugin(body.pkg, { timeoutMs: Number(body.timeoutMs) || undefined });
        return send(res, r.ok ? 200 : r.code ?? 502, { contract: CONTRACT, ...r });
      }
      if (p === '/api/v1/plugins/dsh/remove') {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const r = dshBridge.removeDshPlugin(body.pkg);
        return send(res, r.ok ? 200 : r.code ?? 502, { contract: CONTRACT, ...r });
      }
      // 回答待答问题（ask_user）
      if (p === '/api/v1/plugins/ask/answer') {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const r = basic.answerQuestion(body.id, body.answer);
        return send(res, r.ok ? 200 : r.code ?? 404, { contract: CONTRACT, ...r });
      }
      // 工具启用/停用（插件「下载→装载」）
      if (p === '/api/v1/plugins/tools/toggle') {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const r = basic.setToolEnabled(body.name, !!body.enabled);
        return send(res, r.ok ? 200 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      if (p === '/api/v1/compiler/verify') {
        await readBody(req).catch(() => ''); // 允许空体
        const r = compiler.startVerify();
        return send(res, r.ok ? 202 : r.code, r.ok ? { contract: CONTRACT, ...r } : { contract: CONTRACT, ...r });
      }
      if (p === '/api/v1/ue5/bootstrap') {
        await readBody(req).catch(() => ''); // 兼容旧端点：不再建工程，改为打开选中工程
        const r = await ue5boot.openProject();
        return send(res, r.ok ? 202 : r.code, { contract: CONTRACT, ...r });
      }
      if (p === '/api/v1/ue5/select') {
        const body = await readJsonBody(req);
        const r = ue5boot.selectProject(body?.projectPath);
        return send(res, r.ok ? 200 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      if (p === '/api/v1/ue5/open') {
        const body = await readJsonBody(req);
        const r = await ue5boot.openProject(body?.projectPath);
        return send(res, r.ok ? 202 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      if (p === '/api/v1/ue5/scan-dir') {
        const body = await readJsonBody(req);
        const r = ue5boot.addScanDir(body?.dir);
        return send(res, r.ok ? 200 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      if (p === '/api/v1/ue5/scan-dir/remove') {
        const body = await readJsonBody(req);
        const r = ue5boot.removeScanDir(body?.dir);
        return send(res, r.ok ? 200 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      if (p === '/api/v1/ue5/close') {
        await readBody(req).catch(() => '');
        const r = await ue5boot.closeProject();
        return send(res, r.ok ? 200 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      if (p === '/api/v1/ue5/switch') {
        const body = await readJsonBody(req);
        const r = await ue5boot.switchProject(body?.projectPath);
        return send(res, r.ok ? 202 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      if (p === '/api/v1/uebox/call') {
        let body = {};
        const raw = await readBody(req).catch(() => '');
        if (raw.trim()) {
          try {
            body = JSON.parse(raw);
          } catch {
            return send(res, 400, { contract: CONTRACT, error: 'invalid_json', hint: '请求体必须是合法 JSON' });
          }
        }
        const name = body.name;
        if (typeof name !== 'string' || !name) {
          return send(res, 400, { contract: CONTRACT, error: 'missing_name', hint: '需要 { name, args }' });
        }
        const args = body.args && typeof body.args === 'object' ? body.args : {};
        const timeoutMs = Math.min(Math.max(Number(body.timeoutMs) || 240000, 5000), 300000);
        const r = await ueboxCallChecked(name, args, timeoutMs);
        return send(res, r.ok ? 200 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      if (p === '/api/v1/session') {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const s = session.createSession({ pinTarget: body.pinTarget, sessionTitle: body.sessionTitle });
        return send(res, 201, { contract: CONTRACT, session: s });
      }
      if (p === '/api/v1/skills/install') {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const r = skills.installSkill(body.name, body.content);
        return send(res, r.ok ? 201 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      if (p === '/api/v1/mcp/servers') {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const r = mcp.addServer(body.name, body.config);
        return send(res, r.ok ? 201 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      let mp = /^\/api\/v1\/mcp\/servers\/([A-Za-z0-9-]+)\/tools\/list$/.exec(p);
      if (mp) {
        return send(res, 200, { contract: CONTRACT, ...(await mcp.listTools(mp[1])) });
      }
      mp = /^\/api\/v1\/mcp\/servers\/([A-Za-z0-9-]+)\/tools\/call$/.exec(p);
      if (mp) {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const r = await mcp.callTool(mp[1], body.tool, body.args || {}, Number(body.timeoutMs) || 120_000);
        return send(res, r.ok ? 200 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      if (p === '/api/v1/teamboard/start') {
        await readBody(req).catch(() => '');
        const r = await teamboard.startTeamBoard();
        return send(res, r.ok ? 200 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      if (p === '/api/v1/teamboard/stop') {
        await readBody(req).catch(() => '');
        return send(res, 200, { contract: CONTRACT, ...teamboard.stopTeamBoard() });
      }
      // ── 语音转写（2026-10-08 用户：麦克风真功能）── body = WAV 二进制（16k/16bit/单声道）
      if (p === '/api/v1/agent/asr') {
        const raw = await readRawBody(req, 15 * 1024 * 1024).catch(() => null);
        if (!raw || !raw.length) {
          return send(res, 400, { contract: CONTRACT, error: 'empty_body', hint: 'body 必须是 WAV 二进制' });
        }
        const r = await asr.transcribeWav(raw);
        return send(res, r.ok ? 200 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      // ── 引擎控制动作：model-start/stop、openapi-start/stop/key-rotate ──
      // 统一 200 + ok 标记：控制动作的失败原因（显存不足/脚本缺失/端口占用）要原样到前端，
      // 不能被 400 + 笼统 ContractError 吃掉（2026-10-08 用户反馈「报错看不懂」）。
      if (p === '/api/v1/engine/control') {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const action = body.action;
        if (typeof action !== 'string' || !action) {
          return send(res, 400, { contract: CONTRACT, ok: false, error: 'missing_action', hint: '需要 { action: "model-start|model-stop|openapi-start|..." }' });
        }
        const r = await enginectl.engineControlAction(action);
        return send(res, 200, { contract: CONTRACT, ...r });
      }
      // ── 引擎换挡：POST { profile } → 网关 /admin/switch（重载权重 10~40s）──
      if (p === '/api/v1/engine/gear') {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const r = await enginectl.engineSwitchGear(body?.profile);
        return send(res, 200, { contract: CONTRACT, ...r });
      }
      // ── 团队交流：建频道 / 发消息 ─────────────────────────
      
      // ── 群成员管理（微信建群式：邀请/移出/副管理）────────────
      let cm = /^\/api\/v1\/chat\/channels\/([A-Za-z0-9_-]+)\/members$/.exec(p);
      
      cm = /^\/api\/v1\/chat\/channels\/([A-Za-z0-9_-]+)\/invite$/.exec(p);
      
      cm = /^\/api\/v1\/chat\/channels\/([A-Za-z0-9_-]+)\/kick$/.exec(p);
      
      cm = /^\/api\/v1\/chat\/channels\/([A-Za-z0-9_-]+)\/admin$/.exec(p);
      
      
      // ── M-06 发文档 ─────────────────────────────
      
      // ── M-07 发 skill 包 ─────────────────────────
      
      // ── M-07/M-08 对端导入 skill 包（坏包必须拒绝）──
      
      // ── M-09 发文件 / 下载文件 ──────────────────────
      
      
      // ── 账户体系：注册 / 登录 ───────────────────────────
      // ── 中央账户（2026-10-11）：注册/登录都走公网账号服务器，账号跨设备/重装不丢 ──
      
      
      
      
      
      
      if (p === '/api/v1/agent/run') {
        let body = {};
        const raw = await readBody(req).catch(() => '');
        try {
          body = raw.trim() ? JSON.parse(raw) : {};
        } catch {
          return send(res, 400, { contract: CONTRACT, error: 'invalid_json' });
        }
        const r = agent.startAgentTask(body.task, body.sessionId, body.attachments, body.skills, body.toolGroups);
        return send(res, r.ok ? 202 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
      if (p === '/api/v1/agent/stop') {
        return send(res, 200, { contract: CONTRACT, ...agent.stopAgent() });
      }
      // ── 桌宠（2026-10-10）：轻量对话 + 工作台状态订阅 ─────
      if (p === '/api/v1/pet/chat') {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const msg = String(body.message || '').trim();
        if (!msg) return send(res, 400, { contract: CONTRACT, ok: false, error: 'message 不能为空' });
        const history = Array.isArray(body.history) ? body.history : [];
        try {
          const r = await agent.petChat(msg, history);
          return send(res, 200, { contract: CONTRACT, ...r });
        } catch (e) {
          return send(res, 200, { contract: CONTRACT, ok: false, error: String(e.message || e).slice(0, 200) });
        }
      }
      // ── 本地资料库：动作调用（method 分发到 vault.mjs）────
      if (p === '/api/v1/vault/call') {
        const body = JSON.parse((await readBody(req).catch(() => '{}')) || '{}');
        const method = body.method;
        if (typeof method !== 'string' || !method.startsWith('vault.')) {
          return send(res, 400, { contract: CONTRACT, error: 'missing_method', hint: '需要 { method: "vault.<动作>", args }' });
        }
        const args = body.args && typeof body.args === 'object' && !Array.isArray(body.args) ? body.args : {};
        const timeoutMs = Math.min(Math.max(Number(body.timeoutMs) || 30000, 5000), 300000);
        const r = await vault.vaultLocalCall(method, args, timeoutMs);
        return send(res, r.ok ? 200 : r.code ?? 500, { contract: CONTRACT, ...r });
      }
    }

    return notFound(res, p);
  } catch (e) {
    return send(res, 500, { error: 'internal', contract: CONTRACT, message: String(e.message || e) });
  }
});

server.listen(PORT, HOST, () => {
  // 启动 UnrealAgentLink WS 服务端：UE 编辑器插件直连这里（摆脱 MCP/UEBox 桌面应用）。
  // 端口被占（如还装着 UEBox 占着 17860）不阻断后端主流程，ual/status 里会如实报。
  ual.ualStart();
  // 用 ASCII 前缀 + 结构化字段，方便主进程 grep / 解析
  // M-13：宣告自己在场（UDP beacon 应答 + 共享在线表心跳）。
  // 失败不影响后端主流程（没连上底座时只是没人看得到我）。
  
  keepalive.startKeepalive(); // 模型保活/预热（2026-10-09 用户：降低发任务到 GPU 开算的冷启动延迟）
  console.log(
    `[backend] ready app=${APP} contract=${CONTRACT} url=http://${HOST}:${PORT}/api/v1/ pid=${process.pid}`
  );
});

let shuttingDown = false;
function shutdown(code = 0) {
  if (shuttingDown) return;
  shuttingDown = true;
  console.log(`[backend] shutdown pid=${process.pid}`);
  enginectl.stopManagedEngine();
  compiler.stopAll(); // 杀掉还在跑的编译子进程，不留孤儿
  ue5boot.stopAll(); // 标记编排中断
  agent.stopAll(); // 标记 agent 中断
  keepalive.stopKeepalive(); // 停保活定时器
  ual.ualStop(); // 关 UAL WS 服务端 + 清连接
  teamboard.stopTeamBoard(); // 停团队画板（Hocuspocus + vite preview），关工作台不留孤儿
   // 摘掉自己的在线条目 + 停心跳，别人立刻看不到我
  server.close(() => process.exit(code));
  // 兜底：3 秒后仍没关掉就强制退出，保证不留残留
  setTimeout(() => process.exit(code), 3000).unref();
}

process.on('SIGTERM', () => shutdown(0));
process.on('SIGINT', () => shutdown(0));

// 孤儿守护：父进程（Electron）退出 → 子进程 stdin 管道关闭 → 后端自行退出。
// 这是「关窗无残留进程」（验收 #3）的一道兜底，即使主进程没来得及发 SIGTERM 也生效。
process.stdin.resume();
process.stdin.on('end', () => shutdown(0));
process.stdin.on('error', () => {});
