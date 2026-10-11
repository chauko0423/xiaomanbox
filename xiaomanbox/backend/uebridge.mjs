/**
 * uebridge —— 工作台内置的 UE 桥接 WebSocket 服务端（零外部依赖）
 *
 * 目标：彻底取代 UEBox 那个 Electron 桌面程序，让 XiaoMan Box 直接驱动
 * 装在 UE 里的 UnrealAgentLink 插件。
 *
 * ## 协议（与 UEBox 开源实现保持一致，见 uebox-src/src/main/services/websocket/）
 *
 *   - 插件是 WebSocket **客户端**，默认连 ws://127.0.0.1:17860（可 -UALServer 覆盖）。
 *   - 消息信封（JSON-RPC 风格）：
 *       { ver:'1.0', type:'req'|'res'|'evt', id, method, params/payload, time }
 *   - 服务端 RPC：callRequest(method, params) → 发 {type:'req',id,method,params}
 *     → 插件回 {type:'res',id,result} → 按 id 匹配 Promise resolve。
 *   - 心跳：插件每 ~15s 发 {type:'evt',method:'system.heartbeat'}；服务端
 *     45s 没收到任何消息判死。服务端也主动发协议层 ping 帧。
 *
 * ## 零依赖约束（用户部署包哲学）
 *
 *   不用 `ws` 库，用 Node 内置 `http` 实现 RFC6455 握手 + 帧编解码。
 *   只依赖：node:http / node:crypto / node:events。任何显卡、任何环境都能跑。
 *
 * ## 设计纪律（学习自 UEBox，连同它们踩过的坑一起复刻）
 *
 *   1. 断线善后幂等：close/error/心跳超时三条路汇到同一个 handleDisconnect。
 *   2. 僵尸连接按「答不答话」判死，不只看 socket 在不在（编辑器崩溃时
 *      socket 还挂着、心跳还回，但请求全超时）。
 *   3. 断线时立即 reject 该连接所有在途请求，别让它们各自等满超时。
 *   4. 目标连接不定时，多个连接并存时拒绝猜测（改错工程代价 > 报错）。
 */

import http from 'node:http';
import crypto from 'node:crypto';
import { EventEmitter } from 'node:events';

export const PROTOCOL_VERSION = '1.0';
export const DEFAULT_PORT = 17860;
const HEARTBEAT_INTERVAL_MS = 15_000; // 服务端主动 ping 间隔
const HEARTBEAT_TIMEOUT_MS = 45_000; // 超过这么久没收到任何消息判死
const ZOMBIE_TIMEOUT_STREAK = 3; // 连续这么多次请求超时当僵尸连接
const MAX_FRAME_BYTES = 32 * 1024 * 1024; // 单帧硬上限

// ── RFC6455 帧编解码（零依赖自研）──────────────────────────

const OPCODE = { CONTINUATION: 0x0, TEXT: 0x1, BINARY: 0x2, CLOSE: 0x8, PING: 0x9, PONG: 0xa };

/** 解析一个客户端帧。返回 { opcode, payload:Buffer, fin } 或 null（数据不足） */
function decodeFrame(buf) {
  if (buf.length < 2) return null;
  const fin = (buf[0] & 0x80) !== 0;
  const opcode = buf[0] & 0x0f;
  const masked = (buf[1] & 0x80) !== 0;
  let len = buf[1] & 0x7f;
  let offset = 2;

  if (len === 126) {
    if (buf.length < offset + 2) return null;
    len = buf.readUInt16BE(offset);
    offset += 2;
  } else if (len === 127) {
    if (buf.length < offset + 8) return null;
    const big = buf.readBigUInt64BE(offset);
    if (big > BigInt(MAX_FRAME_BYTES)) throw new Error('帧过大');
    len = Number(big);
    offset += 8;
  }

  let maskKey = null;
  if (masked) {
    if (buf.length < offset + 4) return null;
    maskKey = buf.slice(offset, offset + 4);
    offset += 4;
  }

  if (buf.length < offset + len) return null;
  let payload = buf.slice(offset, offset + len);
  if (maskKey) {
    const out = Buffer.allocUnsafe(len);
    for (let i = 0; i < len; i++) out[i] = payload[i] ^ maskKey[i & 3];
    payload = out;
  }

  return { fin, opcode, payload, consumed: offset + len };
}

/** 服务端帧不 mask。封装一个文本帧 */
function encodeFrame(opcode, payload) {
  const data = Buffer.isBuffer(payload) ? payload : Buffer.from(String(payload), 'utf8');
  let header;
  if (data.length < 126) {
    header = Buffer.from([0x80 | opcode, data.length]);
  } else if (data.length < 65536) {
    header = Buffer.alloc(4);
    header[0] = 0x80 | opcode;
    header[1] = 126;
    header.writeUInt16BE(data.length, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x80 | opcode;
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(data.length), 2);
  }
  return Buffer.concat([header, data]);
}

/** RFC6455 握手：算 Sec-WebSocket-Accept */
function computeAccept(key) {
  return crypto
    .createHash('sha1')
    .update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
    .digest('base64');
}

/** 生成消息 id（无 uuid 依赖，用 crypto.randomUUID） */
const newId = () => crypto.randomUUID();

// ── 连接与请求状态 ─────────────────────────────────────────

class UEConnection {
  constructor(id, socket) {
    this.id = id;
    this.socket = socket;
    this.lastSeen = Date.now();
    this.connectedAt = Date.now();
    this.closed = false;
    this.fragments = []; // 分片缓冲（opcode 0 连续帧）
    this.buf = Buffer.alloc(0);
  }
  get isAlive() {
    return !this.closed;
  }
  send(payload) {
    if (this.closed) return false;
    try {
      this.socket.write(payload);
      return true;
    } catch {
      this.closed = true;
      return false;
    }
  }
  close() {
    if (this.closed) return;
    this.closed = true;
    try {
      this.socket.write(encodeFrame(OPCODE.CLOSE, Buffer.from([0x03, 0xe8])));
      this.socket.end();
    } catch {
      /* 忽略 */
    }
  }
}

/**
 * UE 桥接服务。
 * 事件：'status' (state)、'connected' (connId)、'disconnected' (connId)、
 *       'event' (method, payload, connId)
 */
export class UEBridge extends EventEmitter {
  constructor({ port = DEFAULT_PORT, host = '127.0.0.1' } = {}) {
    super();
    this.port = port;
    this.host = host;
    this.server = null;
    this.connections = new Map(); // id -> UEConnection
    this.pending = new Map(); // messageId -> { resolve, reject, timer, method, connId }
    this.timeoutStreaks = new Map(); // connId -> 连续超时次数
    this.heartbeatTimer = null;
    this.state = 'idle'; // idle | listening | error
    this.startedAt = undefined;
    this.startError = undefined;
  }

  /** 启动服务 */
  start() {
    return new Promise((resolve, reject) => {
      if (this.state === 'listening') return resolve();
      this.server = http.createServer((req, res) => {
        // 只服务 upgrade 请求；普通 HTTP 一律 404
        res.writeHead(404);
        res.end();
      });

      this.server.on('upgrade', (req, socket, head) => {
        this._handleUpgrade(req, socket, head);
      });

      this.server.on('error', (err) => {
        this.state = 'error';
        this.startError = describeListenError(err, this.port);
        this.emit('status', this.getStatus());
        reject(err);
      });

      this.server.listen(this.port, this.host, () => {
        this.state = 'listening';
        this.startedAt = Date.now();
        this.startError = undefined;
        this._startHeartbeat();
        this.emit('status', this.getStatus());
        resolve();
      });
    });
  }

  /** 停止服务 */
  async stop() {
    if (this.heartbeatTimer) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = null;
    }
    for (const conn of [...this.connections.values()]) {
      this._handleDisconnect(conn.id);
    }
    this._cleanupPending();
    if (this.server) {
      await new Promise((resolve) => this.server.close(() => resolve()));
      this.server = null;
    }
    this.state = 'idle';
    this.startedAt = undefined;
    this.emit('status', this.getStatus());
  }

  getStatus() {
    return {
      state: this.state,
      port: this.port,
      connectionCount: this.connections.size,
      startedAt: this.startedAt,
      ...(this.startError ? { startError: this.startError } : {}),
    };
  }

  // ── RPC ────────────────────────────────────────────────

  /**
   * 发请求并等回包。
   * @param method 方法名（如 system.get_project_info / actor.spawn）
   * @param params 参数对象
   * @param timeout 超时 ms，默认 120s（引擎操作可能数分钟）
   * @param connId 目标连接，省略则要求恰好一个存活连接
   */
  callRequest(method, params = {}, { timeout = 120_000, connId } = {}) {
    return new Promise((resolve, reject) => {
      const targetId = connId ?? this._pickDefaultConn();
      if (!targetId) {
        const err = new Error(
          this.connections.size === 0
            ? '没有连接上的 UE 编辑器（插件未连上）。'
            : `有 ${this.connections.size} 个 UE 工程在线但未指定目标，拒绝猜测。`
        );
        err.code = 'E_CLIENT_NOT_FOUND';
        return reject(err);
      }
      const conn = this.connections.get(targetId);
      if (!conn || !conn.isAlive) {
        const err = new Error('这条 UE 连接已断开（编辑器关了或重启过）。');
        err.code = 'E_CLIENT_NOT_FOUND';
        err.agentHint = '用 system.get_project_info 确认状态，再决定等重连还是让用户开工程。';
        return reject(err);
      }

      const id = newId();
      const envelope = { ver: PROTOCOL_VERSION, type: 'req', id, method, params, time: Date.now() };

      let timer;
      const promise = new Promise((res, rej) => {
        timer = setTimeout(() => {
          this.pending.delete(id);
          this._noteTimeout(targetId, method);
          const e = new Error(`请求超时: ${method}`);
          e.code = 'E_TIMEOUT';
          rej(e);
        }, timeout);
        this.pending.set(id, { resolve: res, reject: rej, timer, method, connId: targetId });
      });

      const ok = conn.send(encodeFrame(OPCODE.TEXT, JSON.stringify(envelope)));
      if (!ok) {
        clearTimeout(timer);
        this.pending.delete(id);
        const e = new Error('发送失败：连接已关闭');
        e.code = 'E_CONNECTION_CLOSED';
        return reject(e);
      }
      resolve(promise);
    });
  }

  /** 发单向事件（不等待回复） */
  emitEvent(method, payload = {}, connId) {
    const envelope = { ver: PROTOCOL_VERSION, type: 'evt', method, payload, time: Date.now() };
    const text = JSON.stringify(envelope);
    if (connId) {
      const conn = this.connections.get(connId);
      if (conn) conn.send(encodeFrame(OPCODE.TEXT, text));
    } else {
      for (const c of this.connections.values()) c.send(encodeFrame(OPCODE.TEXT, text));
    }
  }

  // ── 内部 ────────────────────────────────────────────────

  _pickDefaultConn() {
    const alive = [...this.connections.values()].filter((c) => c.isAlive);
    if (alive.length === 1) return alive[0].id;
    return undefined; // 0 个或多个都拒绝猜测
  }

  _noteTimeout(connId, method) {
    const streak = (this.timeoutStreaks.get(connId) ?? 0) + 1;
    this.timeoutStreaks.set(connId, streak);
    const busy = [...this.pending.values()].some((p) => p.connId === connId);
    if (streak >= ZOMBIE_TIMEOUT_STREAK && (!busy || streak >= ZOMBIE_TIMEOUT_STREAK * 2)) {
      console.warn(`[uebridge] 连接连续 ${streak} 次请求超时（最后 ${method}），按僵尸连接断开: ${connId}`);
      this.timeoutStreaks.delete(connId);
      this._handleDisconnect(connId);
    }
  }

  _startHeartbeat() {
    this.heartbeatTimer = setInterval(() => {
      const now = Date.now();
      for (const conn of [...this.connections.values()]) {
        if (now - conn.lastSeen > HEARTBEAT_TIMEOUT_MS) {
          console.warn(`[uebridge] 连接心跳超时: ${conn.id}`);
          this._handleDisconnect(conn.id);
        } else if (conn.isAlive) {
          // 服务端主动 ping（协议层），客户端自动回 pong
          try {
            conn.socket.write(encodeFrame(OPCODE.PING, Buffer.alloc(0)));
          } catch {
            this._handleDisconnect(conn.id);
          }
        }
      }
    }, HEARTBEAT_INTERVAL_MS);
  }

  _handleUpgrade(req, socket, head) {
    const key = req.headers['sec-websocket-key'];
    if (req.headers.upgrade?.toLowerCase() !== 'websocket' || !key) {
      socket.write('HTTP/1.1 400 Bad Request\r\n\r\n');
      socket.destroy();
      return;
    }
    const accept = computeAccept(key);
    socket.write(
      'HTTP/1.1 101 Switching Protocols\r\n' +
        'Upgrade: websocket\r\n' +
        'Connection: Upgrade\r\n' +
        `Sec-WebSocket-Accept: ${accept}\r\n\r\n`
    );

    const conn = new UEConnection(newId(), socket);
    this.connections.set(conn.id, conn);
    this.timeoutStreaks.delete(conn.id);
    this.emit('connected', conn.id);

    // 欢迎消息（让插件拿到自己的 connectionId）
    conn.send(
      encodeFrame(
        OPCODE.TEXT,
        JSON.stringify({
          ver: PROTOCOL_VERSION,
          type: 'evt',
          method: 'system.connected',
          payload: { connectionId: conn.id },
          time: Date.now(),
        })
      )
    );

    if (head && head.length) conn.buf = Buffer.concat([conn.buf, head]);

    socket.on('data', (chunk) => {
      conn.buf = Buffer.concat([conn.buf, chunk]);
      conn.lastSeen = Date.now();
      this._drainFrames(conn);
    });
    socket.on('error', () => this._handleDisconnect(conn.id));
    socket.on('close', () => this._handleDisconnect(conn.id));
    socket.on('end', () => this._handleDisconnect(conn.id));
  }

  _drainFrames(conn) {
    while (true) {
      let frame;
      try {
        frame = decodeFrame(conn.buf);
      } catch (e) {
        console.warn('[uebridge] 帧解析失败:', e.message);
        this._handleDisconnect(conn.id);
        return;
      }
      if (!frame) return; // 数据不足，等下一次
      conn.buf = conn.buf.slice(frame.consumed);

      switch (frame.opcode) {
        case OPCODE.TEXT:
        case OPCODE.BINARY:
          this._handleText(conn, frame.payload.toString('utf8'));
          break;
        case OPCODE.PING:
          conn.send(encodeFrame(OPCODE.PONG, frame.payload));
          break;
        case OPCODE.PONG:
          conn.lastSeen = Date.now();
          // 协议层活性信号：PONG 是客户端对服务端 PING 的自动响应（WebSockets 库处理，
          // 不依赖插件应用层 ticker）。插件应用层心跳（system.heartbeat）可能因重连后
          // ticker 停摆而不发，但 PONG 仍在回 → 让 ual 层也感知「连接活着」，避免误判中断。
          this.emit('activity', conn.id);
          break;
        case OPCODE.CLOSE:
          this._handleDisconnect(conn.id);
          return;
        default:
          // 忽略 continuation 等未实现帧（插件不发送）
          break;
      }
    }
  }

  _handleText(conn, text) {
    let env;
    try {
      env = JSON.parse(text);
    } catch {
      console.warn('[uebridge] 收到非 JSON 消息，忽略');
      return;
    }

    if (env.type === 'res') {
      // 响应 → 匹配 pending
      if (env.id && this.pending.has(env.id)) {
        const p = this.pending.get(env.id);
        clearTimeout(p.timer);
        this.pending.delete(env.id);
        this.timeoutStreaks.delete(conn.id);
        // 兼容 result / data / payload；统一注入 __rpc.code
        const raw = env.result ?? env.data ?? env.payload;
        const code = typeof env.code === 'number' ? env.code : undefined;
        let normalized = raw;
        if (raw && typeof raw === 'object') {
          const obj = { ...raw };
          const rpcMeta = { ...(obj.__rpc || {}) };
          if (code !== undefined) rpcMeta.code = code;
          obj.__rpc = rpcMeta;
          if (code !== undefined && code >= 400) {
            if (obj.ok === undefined) obj.ok = false;
            if (obj.success === undefined) obj.success = false;
            if (obj.error === undefined && typeof obj.message === 'string') obj.error = obj.message;
            if (obj.code === undefined) obj.code = code;
          }
          normalized = obj;
        }
        p.resolve(normalized);
      }
    } else if (env.type === 'evt') {
      // 心跳事件也照常 emit：ual.mjs 靠这个事件更新 lastHeartbeat（应用层活性判定）。
      // 之前这里 `if (system.heartbeat) return` 把心跳吞掉了，导致 ual.mjs 的 lastHeartbeat
      // 永远停摆 → 死链检测（isConnectionStale）误杀所有正常连接。协议层 lastSeen 已由
      // socket.on('data') 统一更新，这里再 emit 一次不影响协议层、只补上应用层。
      this.emit('event', env.method, env.payload ?? {}, conn.id);
    } else if (env.type === 'req') {
      // 插件主动发请求（少见，暂不支持业务处理，仅记录）
      console.warn('[uebridge] 收到插件主动请求（未处理）:', env.method);
    }
  }

  _handleDisconnect(connId) {
    const conn = this.connections.get(connId);
    if (!conn) return; // 幂等
    this.connections.delete(connId);
    this.timeoutStreaks.delete(connId);
    conn.close();

    // 拒绝该连接所有在途请求
    for (const [id, p] of [...this.pending.entries()]) {
      if (p.connId === connId) {
        clearTimeout(p.timer);
        this.pending.delete(id);
        const e = new Error(
          `引擎在执行 ${p.method} 的过程中断开连接（编辑器崩溃/被关/插件停止）。` +
            '命令可能只做了一半，先确认引擎状态再决定，不要用同样参数直接重试。'
        );
        e.code = 'E_CONNECTION_CLOSED';
        p.reject(e);
      }
    }
    this.emit('disconnected', connId);
  }

  _cleanupPending() {
    for (const [id, p] of this.pending.entries()) {
      clearTimeout(p.timer);
      this.pending.delete(id);
      const e = new Error('服务已关闭');
      e.code = 'E_CONNECTION_CLOSED';
      p.reject(e);
    }
  }
}

/** 把 listen 错误翻译成人话 */
function describeListenError(err, port) {
  if (err.code === 'EADDRINUSE') {
    return `端口 ${port} 已被占用（可能还开着 UEBox，或别的程序占用了）。关掉占用程序后重启工作台即可。`;
  }
  if (err.code === 'EACCES') {
    return `没有权限监听端口 ${port}。`;
  }
  return `UE 桥接服务启动失败：${err.message}`;
}

// ── 单例 ─────────────────────────────────────────────────

let bridge = null;
export function getUEBridge(opts) {
  if (!bridge) bridge = new UEBridge(opts);
  return bridge;
}

export function resetUEBridge() {
  bridge = null;
}
