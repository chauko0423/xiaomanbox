/**
 * M2b 生态 —— 通用 MCP 客户端（A 类资产：dsh/社区 MCP 服务器直接可用）
 *
 * 支持两种 transport：
 *   - stdio：spawn 子进程（如 npx -y @xxx/mcp-server），换行分隔 JSON-RPC over stdin/stdout
 *   - http：POST + SSE（如 UEBox），Bearer 可选
 *
 * 协议：MCP（initialize → notifications/initialized → tools/list → tools/call）。
 * 注册表：~/.xiaomanbox/mcp-servers.json
 *   {
 *     "servers": {
 *       "git": { "transport":"stdio", "command":"npx", "args":["-y","@modelcontextprotocol/server-git"] },
 *       "uebox": { "transport":"http", "url":"http://127.0.0.1:17861/mcp", "token":"..." }
 *     }
 *   }
 *
 * 安全（照抄 dsh 的环境变量清洗）：stdio 子进程不继承 *_KEY/*_PASSWORD/*_SECRET/*_TOKEN/DSH_*。
 */

import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import http from 'node:http';
import { spawn } from 'node:child_process';

const REGISTRY = path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'mcp-servers.json');
const MCP_VERSION = '2025-06-18';

function loadRegistry() {
  try {
    return JSON.parse(fs.readFileSync(REGISTRY, 'utf8'));
  } catch {
    return { servers: {} };
  }
}
function saveRegistry(reg) {
  fs.mkdirSync(path.dirname(REGISTRY), { recursive: true });
  fs.writeFileSync(REGISTRY, JSON.stringify(reg, null, 2), 'utf8');
}

export function listServers() {
  const reg = loadRegistry();
  return Object.entries(reg.servers || {}).map(([name, c]) => ({
    name,
    transport: c.transport,
    ...(c.transport === 'stdio' ? { command: c.command, args: c.args || [] } : { url: c.url }),
  }));
}

export function addServer(name, cfg) {
  if (!name || !/^[a-z0-9-]{1,64}$/.test(name)) return { ok: false, code: 400, error: '服务器名只允许小写/数字/连字符' };
  if (!cfg || (cfg.transport !== 'stdio' && cfg.transport !== 'http')) {
    return { ok: false, code: 400, error: 'transport 必须是 stdio 或 http' };
  }
  if (cfg.transport === 'stdio' && !cfg.command) return { ok: false, code: 400, error: 'stdio 需要 command' };
  if (cfg.transport === 'http' && !cfg.url) return { ok: false, code: 400, error: 'http 需要 url' };
  const reg = loadRegistry();
  reg.servers = reg.servers || {};
  reg.servers[name] = cfg;
  saveRegistry(reg);
  return { ok: true, name };
}

/** 删除一个 MCP 服务器（从注册表移除） */
export function removeServer(name) {
  if (!name || !/^[a-z0-9-]{1,64}$/.test(name)) return { ok: false, code: 400, error: '服务器名只允许小写/数字/连字符' };
  const reg = loadRegistry();
  if (!reg.servers || !reg.servers[name]) return { ok: false, code: 404, error: `服务器「${name}」不存在` };
  delete reg.servers[name];
  saveRegistry(reg);
  return { ok: true, name, removed: true };
}

// ── 环境变量清洗（照抄 dsh）────────────────────────────────
function cleanEnv(env) {
  const out = {};
  for (const [k, v] of Object.entries(env)) {
    if (/_KEY$|_PASSWORD$|_SECRET$|_TOKEN$/.test(k) || k.startsWith('DSH_')) continue;
    out[k] = v;
  }
  return out;
}

// ── stdio transport ─────────────────────────────────────────
/** 换行分隔 JSON-RPC 客户端（MCP stdio） */
class StdioClient {
  constructor(name, cfg) {
    this.name = name;
    this.cfg = cfg;
    this.proc = null;
    this.buf = '';
    this.nextId = 1;
    this.pending = new Map(); // id -> {resolve, reject}
    this._spawn();
  }
  _spawn() {
    const env = cleanEnv(process.env);
    this.proc = spawn(this.cfg.command, this.cfg.args || [], {
      env,
      windowsHide: true,
      stdio: ['pipe', 'pipe', 'pipe'],
    });
    this.proc.stdout.on('data', (c) => this._onData(c));
    this.proc.stderr.on('data', () => {}); // stderr 忽略（调试信息）
    this.proc.on('error', (e) => this._failAll(e));
    this.proc.on('exit', (code) => {
      if (code !== 0 && code !== null) this._failAll(new Error(`stdio 子进程退出码 ${code}`));
    });
  }
  _onData(c) {
    this.buf += c.toString('utf8');
    let idx;
    while ((idx = this.buf.indexOf('\n')) >= 0) {
      const line = this.buf.slice(0, idx).trim();
      this.buf = this.buf.slice(idx + 1);
      if (!line) continue;
      try {
        const msg = JSON.parse(line);
        if (msg.id != null && this.pending.has(msg.id)) {
          const p = this.pending.get(msg.id);
          this.pending.delete(msg.id);
          msg.error ? p.reject(new Error(msg.error.message || JSON.stringify(msg.error))) : p.resolve(msg.result);
        }
      } catch {
        /* 忽略非 JSON 行 */
      }
    }
  }
  _failAll(e) {
    for (const p of this.pending.values()) p.reject(e);
    this.pending.clear();
  }
  request(method, params = {}, timeoutMs = 30000) {
    return new Promise((resolve, reject) => {
      const id = this.nextId++;
      const body = { jsonrpc: '2.0', id, method, params };
      this.pending.set(id, { resolve, reject });
      try {
        this.proc.stdin.write(JSON.stringify(body) + '\n');
      } catch (e) {
        this.pending.delete(id);
        reject(e);
      }
      setTimeout(() => {
        if (this.pending.has(id)) {
          this.pending.delete(id);
          reject(new Error(`${method} 超时`));
        }
      }, timeoutMs);
    });
  }
  notify(method, params = {}) {
    try {
      this.proc.stdin.write(JSON.stringify({ jsonrpc: '2.0', method, params }) + '\n');
    } catch {
      /* 忽略 */
    }
  }
  close() {
    try {
      this.proc.kill();
    } catch {
      /* 忽略 */
    }
  }
}

// ── HTTP transport ──────────────────────────────────────────
function httpPost(url, token, body, sessionId, timeoutMs = 30000) {
  return new Promise((resolve, reject) => {
    const u = new URL(url);
    const data = JSON.stringify(body);
    const req = http.request(
      {
        hostname: u.hostname,
        port: u.port,
        path: u.pathname,
        method: 'POST',
        headers: {
          ...(token ? { Authorization: `Bearer ${token}` } : {}),
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
    req.on('timeout', () => req.destroy(new Error('HTTP MCP 请求超时')));
    req.on('error', reject);
    req.write(data);
    req.end();
  });
}

function parseSseOrJson(text) {
  // SSE：取第一个 data: 行；否则整段 JSON
  for (const line of String(text).split('\n')) {
    if (line.startsWith('data:')) {
      try {
        return JSON.parse(line.slice(5).trim());
      } catch {
        /* 继续 */
      }
    }
  }
  try {
    return JSON.parse(text);
  } catch {
    return null;
  }
}

// ── 连接缓存 ─────────────────────────────────────────────────
const conns = new Map(); // name -> { transport, client?, httpSession? }

function getConn(name, cfg) {
  if (!conns.has(name)) {
    conns.set(name, cfg.transport === 'stdio' ? { transport: 'stdio', client: new StdioClient(name, cfg) } : { transport: 'http' });
  }
  return conns.get(name);
}

async function initialize(name, cfg, conn) {
  const initParams = {
    protocolVersion: MCP_VERSION,
    capabilities: {},
    clientInfo: { name: 'xiaoman-box', version: '0.3.0' },
  };
  if (conn.transport === 'stdio') {
    const result = await conn.client.request('initialize', initParams, 30000);
    conn.client.notify('notifications/initialized');
    return { serverInfo: result?.serverInfo, protocolVersion: result?.protocolVersion };
  }
  // HTTP
  const r = await httpPost(cfg.url, cfg.token, { jsonrpc: '2.0', id: 1, method: 'initialize', params: initParams });
  if (r.status !== 200) throw new Error(`initialize HTTP ${r.status}`);
  const sid = r.headers['mcp-session-id'];
  conn.httpSession = sid;
  if (sid) await httpPost(cfg.url, cfg.token, { jsonrpc: '2.0', method: 'notifications/initialized' }, sid);
  const parsed = parseSseOrJson(r.text);
  return { serverInfo: parsed?.result?.serverInfo, protocolVersion: parsed?.result?.protocolVersion };
}

function getCfg(name) {
  const reg = loadRegistry();
  const cfg = reg.servers?.[name];
  return cfg || null;
}

/** 列出某 MCP 服务器的工具 */
export async function listTools(name) {
  const cfg = getCfg(name);
  if (!cfg) return { ok: false, code: 404, error: `MCP 服务器不存在：${name}` };
  const conn = getConn(name, cfg);
  try {
    await initialize(name, cfg, conn);
    let tools;
    if (conn.transport === 'stdio') {
      const result = await conn.client.request('tools/list', {}, 30000);
      tools = result?.tools;
    } else {
      const r = await httpPost(cfg.url, cfg.token, { jsonrpc: '2.0', id: 2, method: 'tools/list', params: {} }, conn.httpSession);
      if (r.status !== 200) throw new Error(`tools/list HTTP ${r.status}`);
      tools = parseSseOrJson(r.text)?.result?.tools;
    }
    if (!Array.isArray(tools)) throw new Error('tools/list 响应无 tools');
    return { ok: true, count: tools.length, tools: tools.map((t) => ({ name: t.name, description: typeof t.description === 'string' ? t.description.slice(0, 160) : undefined })) };
  } catch (e) {
    conns.delete(name);
    return { ok: false, error: String(e.message || e) };
  }
}

/** 暴露某服务器配置（agent 读 toolToken 自动注入写鉴权 token） */
export function getServerCfg(name) {
  return getCfg(name);
}

/**
 * 给 agent 用的完整工具清单（含 inputSchema）。
 * 返回 [{ server, tool, description, inputSchema }]，agent 转成 OpenAI function
 * （工具名 mcp__<server>__<tool>，避免与 UEBox/workbench 工具冲突）。
 * 某服务器不可用时跳过（不阻断整体）。
 */
export async function modelTools() {
  const reg = loadRegistry();
  const out = [];
  for (const [name, cfg] of Object.entries(reg.servers || {})) {
    const conn = getConn(name, cfg);
    try {
      await initialize(name, cfg, conn);
      let tools;
      if (conn.transport === 'stdio') {
        tools = (await conn.client.request('tools/list', {}, 30000))?.tools;
      } else {
        const r = await httpPost(cfg.url, cfg.token, { jsonrpc: '2.0', id: Date.now(), method: 'tools/list', params: {} }, conn.httpSession);
        if (r.status !== 200) continue;
        tools = parseSseOrJson(r.text)?.result?.tools;
      }
      if (!Array.isArray(tools)) continue;
      for (const t of tools) {
        out.push({
          server: name,
          tool: t.name,
          description: typeof t.description === 'string' ? t.description.slice(0, 200) : '',
          inputSchema: t.inputSchema || { type: 'object', properties: {} },
        });
      }
    } catch {
      /* 该服务器不可用，跳过 */
    }
  }
  return out;
}

/** 调用某 MCP 服务器的工具 */
export async function callTool(name, toolName, args = {}, timeoutMs = 120_000) {
  const cfg = getCfg(name);
  if (!cfg) return { ok: false, code: 404, error: `MCP 服务器不存在：${name}` };
  const conn = getConn(name, cfg);
  try {
    await initialize(name, cfg, conn);
    let result;
    if (conn.transport === 'stdio') {
      result = await conn.client.request('tools/call', { name: toolName, arguments: args }, timeoutMs);
    } else {
      const r = await httpPost(cfg.url, cfg.token, { jsonrpc: '2.0', id: Date.now(), method: 'tools/call', params: { name: toolName, arguments: args } }, conn.httpSession, timeoutMs);
      if (r.status !== 200) throw new Error(`tools/call HTTP ${r.status}`);
      result = parseSseOrJson(r.text)?.result;
    }
    if (result?.isError) {
      const t = result.content?.map((c) => c.text || '').join('\n') || '工具错误';
      return { ok: false, error: t.slice(0, 500) };
    }
    let data = result?.structuredContent ?? null;
    if (!data && Array.isArray(result?.content)) {
      const text = result.content.map((c) => c.text || '').join('\n');
      try {
        data = JSON.parse(text);
      } catch {
        return { ok: true, data: null, text };
      }
    }
    return { ok: true, data };
  } catch (e) {
    conns.delete(name);
    return { ok: false, error: String(e.message || e) };
  }
}
