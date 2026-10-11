/**
 * M1 魂 —— 会话绑定三字段 + 防串台 + 记忆三层
 *
 * 会话 = { sessionId, sessionTitle, pinTarget, messages, createdAt, updatedAt }
 *  - sessionId：会话唯一键（UUID）
 *  - sessionTitle：标题（可改）
 *  - pinTarget：钉住的目标（预设 id，如 'ue5-project' / 'daily-chat'，见 presets.mjs；
 *    旧场景 id 'ue5-dev'/'control-room' 由 presets 的 ALIAS 兼容）
 *  - messages：会话消息历史（防串台核心：按 sessionId 严格隔离）
 *
 * 防串台语义（W-65 负控）：messages 挂在 session 对象上，key 是 sessionId。
 *   同 sessionId（即使 pinTarget 不同）→ 同一份 messages = 串台（绑定生效的证明）
 *   不同 sessionId → 各自独立 = 不串台
 * 即「sessionId 是上下文的唯一键」，两个场景绑同一 sessionId 必然共享上下文。
 *
 * 记忆三层（W-66）：
 *   - 短期 = session.messages（会话内）
 *   - 场景 = memory/scenes/<sceneId>.json（按场景持久）
 *   - 全局 = memory/global.json
 *
 * 持久化：~/.xiaomanbox/sessions/<sessionId>.json（每会话一文件）
 */

import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';

const DIR = path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'));
const SESSIONS_DIR = path.join(DIR, 'sessions');
const MEMORY_DIR = path.join(DIR, 'memory');
const GLOBAL_MEMORY = path.join(MEMORY_DIR, 'global.json');
const SCENES_MEMORY_DIR = path.join(MEMORY_DIR, 'scenes');
const BINDINGS_FILE = path.join(DIR, 'scene-bindings.json');

/** 会话历史上限：防止无限增长 */
const MAX_MESSAGES = 200;

const sessions = new Map(); // sessionId -> session
let loaded = false;

const now = () => new Date().toISOString();
const uuid = () => crypto.randomUUID();

function sessionFile(id) {
  return path.join(SESSIONS_DIR, `${id}.json`);
}
function sceneMemoryFile(sceneId) {
  return path.join(SCENES_MEMORY_DIR, `${sceneId}.json`);
}
function ensureDirs() {
  fs.mkdirSync(SESSIONS_DIR, { recursive: true });
  fs.mkdirSync(SCENES_MEMORY_DIR, { recursive: true });
}

function loadAll() {
  if (loaded) return;
  loaded = true;
  try {
    ensureDirs();
    for (const f of fs.readdirSync(SESSIONS_DIR)) {
      if (!f.endsWith('.json')) continue;
      try {
        const s = JSON.parse(fs.readFileSync(path.join(SESSIONS_DIR, f), 'utf8'));
        if (s?.sessionId) sessions.set(s.sessionId, s);
      } catch {
        /* 单文件损坏不阻断其余 */
      }
    }
  } catch {
    /* 目录不存在等，忽略 */
  }
}

function save(s) {
  try {
    ensureDirs();
    fs.writeFileSync(sessionFile(s.sessionId), JSON.stringify(s, null, 2), 'utf8');
  } catch {
    /* 落盘失败不影响内存态 */
  }
}

/** 列表（不含 messages 全文，只给元信息）。置顶优先，其余按最近更新倒序 */
export function listSessions() {
  loadAll();
  return [...sessions.values()]
    .map((s) => ({
      sessionId: s.sessionId,
      sessionTitle: s.sessionTitle,
      pinTarget: s.pinTarget,
      createdAt: s.createdAt,
      updatedAt: s.updatedAt,
      messageCount: (s.messages || []).length,
      archived: !!s.archived,
      pinned: !!s.pinned,
    }))
    .sort((a, b) => {
      if (a.pinned !== b.pinned) return a.pinned ? -1 : 1;
      return String(b.updatedAt || '').localeCompare(String(a.updatedAt || ''));
    });
}

export function getSession(id) {
  loadAll();
  return sessions.get(id) || null;
}

export function createSession({ pinTarget = 'ue5-project', sessionTitle = '' } = {}) {
  loadAll();
  const s = {
    sessionId: uuid(),
    sessionTitle: sessionTitle || `会话 ${new Date().toLocaleString('zh-CN')}`,
    pinTarget,
    messages: [],
    createdAt: now(),
    updatedAt: now(),
    archived: false,
    titleAuto: true, // 标题可被 autoTitle 自动更新；手动改名后置 false（用户意志优先）
  };
  sessions.set(s.sessionId, s);
  save(s);
  return s;
}

// ── 会话标题：从任务文本提取关键词自动命名（2026-10-08）─────
// 零依赖启发式（不调模型）：剥掉 @上下文标签 / 斜杠指令 / 引号括号，
// 取第一个信息量足够的从句，掐掉开头的客套填充词，超长截断加省略号。
const TITLE_MAX = 18; // 标题上限（含省略号）
const TITLE_LEAD_FILLER = /^(?:请|麻烦|帮我|帮忙|我想|我要|我需要|我们需要|让我们|咱们|小满)[，,]?\s*/;

export function deriveSessionTitle(text) {
  if (!text || typeof text !== 'string') return '';
  const cleaned = text
    .replace(/\[@[^:\]]*:[^\]]*\]/g, ' ') // [@标签: 摘要] 上下文标签整体剔除
    .replace(/\/[^\s/]+/g, ' ')           // /指令 词剔除（保留路径里的 /：前面须有空白才算指令，这里宽松处理够用）
    .replace(/[\r\n]+/g, '，')             // 换行视作从句边界
    .trim();
  if (!cleaned) return '';
  // 取从句：从第一个开始拼到信息量足够（≥8 字）为止，避免「帮我」这种半个词标题
  const clauses = cleaned.split(/[，,。.!！？?；;：:、]+/).map((x) => x.trim()).filter(Boolean);
  if (!clauses.length) return ''; // 纯符号输入（「。！？」）→ 无可命名内容
  let pick = '';
  for (const c of clauses) {
    pick = pick ? `${pick}，${c}` : c;
    if (pick.length >= 8) break;
  }
  if (!pick) pick = clauses[0];
  // 掐掉开头的客套填充词（可叠加：「麻烦帮我」）
  let prev = null;
  let title = pick;
  while (title !== prev) {
    prev = title;
    title = title.replace(TITLE_LEAD_FILLER, '').trim();
  }
  if (!title) title = pick;
  if (title.length > TITLE_MAX) {
    // 中文任务的核心宾语通常在句尾（「…建一个会循环旋转的小方块蓝图」）：
    // 尾部截断保住关键词；英文（无词边界）保持头部截断防切出半个词。
    const hasCJK = /[\u4e00-\u9fff]/.test(title);
    if (hasCJK) {
      let s = title.length - TITLE_MAX + 1;
      // 截断点落在英文/数字词中间（如 DIAOYU → YU）→ 向左扩到词边界（最多让 5 字）
      if (s > 0 && /[A-Za-z0-9]/.test(title[s]) && /[A-Za-z0-9]/.test(title[s - 1])) {
        while (s > 0 && /[A-Za-z0-9]/.test(title[s - 1]) && title.length - s < TITLE_MAX + 7) s -= 1;
      }
      title = `…${title.slice(s)}`;
    } else {
      title = `${title.slice(0, TITLE_MAX - 1)}…`;
    }
  }
  return title;
}

/** 任务开始时自动命名会话（实时更新：每次新任务都跟随最新话题）。手动改过的标题不覆盖。 */
export function autoTitle(sessionId, taskText) {
  loadAll();
  const s = sessions.get(sessionId);
  if (!s) return null;
  if (s.titleAuto === false) return s.sessionTitle;
  const t = deriveSessionTitle(taskText);
  if (!t || t === s.sessionTitle) return s.sessionTitle;
  s.sessionTitle = t;
  s.titleAuto = true;
  s.updatedAt = now();
  save(s);
  return t;
}

/** 获取（或创建）某场景的会话 —— 切场景 = 切会话（先查绑定表，无则新建并绑定）。
 * 2026-10-08 用户：绑定的会话已归档时视同无绑定（解绑 + 新建）——
 * 归档 = 从活跃列表收起，不再被场景复用；否则欢迎页选预设会「跳进已归档会话」。 */
export function getOrCreateSessionForScene(sceneId) {
  loadAll();
  const bound = getSceneBinding(sceneId);
  const boundSess = bound ? sessions.get(bound) : null;
  if (boundSess && !boundSess.archived) return boundSess;
  // 归档/悬空绑定：清掉再新建（不留垃圾，行为与 deleteSession 的绑定清理一致）
  if (bound) {
    const b = loadBindings();
    if (b[sceneId] === bound) {
      delete b[sceneId];
      saveBindings(b);
    }
  }
  const s = createSession({ pinTarget: sceneId, sessionTitle: `${sceneId} 会话` });
  bindScene(sceneId, s.sessionId);
  return s;
}

// ── 场景绑定表（防串台核心：sceneId → sessionId）────────────
function loadBindings() {
  try {
    return JSON.parse(fs.readFileSync(BINDINGS_FILE, 'utf8'));
  } catch {
    return {};
  }
}
function saveBindings(b) {
  try {
    ensureDirs();
    fs.writeFileSync(BINDINGS_FILE, JSON.stringify(b, null, 2), 'utf8');
  } catch {
    /* 忽略 */
  }
}

/** 场景当前绑定的会话 id */
export function getSceneBinding(sceneId) {
  return loadBindings()[sceneId] || null;
}

/** 把场景绑定到某会话（可查可改，W-64）。两个场景可绑同一 sessionId → 串台。 */
export function bindScene(sceneId, sessionId) {
  const b = loadBindings();
  b[sceneId] = sessionId;
  saveBindings(b);
  return { ok: true, sceneId, sessionId };
}

/** 改绑定：pinTarget / sessionTitle / pinned（可查可改，W-64）。手动改标题 = 用户意志，自动命名不再覆盖 */
export function updateSession(id, { pinTarget, sessionTitle, pinned } = {}) {
  loadAll();
  const s = sessions.get(id);
  if (!s) return null;
  if (pinTarget !== undefined) s.pinTarget = pinTarget;
  if (sessionTitle !== undefined && sessionTitle !== '') {
    s.sessionTitle = sessionTitle;
    s.titleAuto = false; // 手动命名优先：此后 autoTitle 不再覆盖
  }
  if (typeof pinned === 'boolean') s.pinned = pinned;
  s.updatedAt = now();
  save(s);
  return s;
}

/** 归档 / 取消归档（不删数据，只是从活跃列表收起；可查可恢复，W-64）。
 * 2026-10-08 用户：归档时同时清掉绑定表里指向它的场景绑定 ——
 * 归档 = 退出活跃域，场景下次取会话时走新建（防「跳进归档会话」）。 */
export function archiveSession(id, archived = true) {
  loadAll();
  const s = sessions.get(id);
  if (!s) return null;
  s.archived = !!archived;
  s.updatedAt = now();
  save(s);
  if (s.archived) {
    const b = loadBindings();
    let touched = false;
    for (const k of Object.keys(b)) {
      if (b[k] === id) {
        delete b[k];
        touched = true;
      }
    }
    if (touched) saveBindings(b);
  }
  return s;
}

/**
 * 删除会话（与归档的本质区别：不可恢复）。
 * 连同持久化文件一并删除，并清理绑定表里指向它的绑定（防悬空脏数据；
 * getOrCreateSessionForScene 对悬空绑定本就有兜底新建，这里只是不留垃圾）。
 */
export function deleteSession(id) {
  loadAll();
  if (!sessions.has(id)) return { ok: false, code: 404 };
  sessions.delete(id);
  try {
    fs.rmSync(sessionFile(id), { force: true });
  } catch {
    /* 文件删不掉但内存已删，不阻断 */
  }
  const b = loadBindings();
  let touched = false;
  for (const k of Object.keys(b)) {
    if (b[k] === id) {
      delete b[k];
      touched = true;
    }
  }
  if (touched) saveBindings(b);
  return { ok: true, deleted: id };
}

export function getMessages(id) {
  const s = sessions.get(id);
  return s ? s.messages : [];
}

export function appendMessage(id, msg) {
  loadAll();
  const s = sessions.get(id);
  if (!s) return false;
  s.messages.push(msg);
  if (s.messages.length > MAX_MESSAGES) s.messages = s.messages.slice(-MAX_MESSAGES);
  s.updatedAt = now();
  save(s);
  return true;
}

// ── 记忆三层：场景 / 全局 ────────────────────────────────────
export function getSceneMemory(sceneId) {
  try {
    return JSON.parse(fs.readFileSync(sceneMemoryFile(sceneId), 'utf8'));
  } catch {
    return null;
  }
}

export function setSceneMemory(sceneId, data) {
  ensureDirs();
  fs.writeFileSync(sceneMemoryFile(sceneId), JSON.stringify(data, null, 2), 'utf8');
  return { ok: true, sceneId };
}

export function getGlobalMemory() {
  try {
    return JSON.parse(fs.readFileSync(GLOBAL_MEMORY, 'utf8'));
  } catch {
    return null;
  }
}

export function setGlobalMemory(data) {
  ensureDirs();
  fs.writeFileSync(GLOBAL_MEMORY, JSON.stringify(data, null, 2), 'utf8');
  return { ok: true };
}
