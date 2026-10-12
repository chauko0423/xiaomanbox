/**
 * 通用基础工具（2026-10-08 用户：统计 dsh 工具后补齐 XiaoMan Box 缺的通用 agent 能力）。
 *
 * 背景：dsh（DeepSeek Harness）有 bash/pwsh/fs/fs-search/web/todo 等通用工具；
 * 小满之前只有 UE5 垂直工具（ual 166 个）+ 资料库，缺「执行命令/读写文件/搜文件/联网/待办」。
 * 真机已暴露：小满答「没有终端/命令执行工具」。这里补齐最核心的 6 个。
 *
 * 安全边界：bash 限超时 + 输出截断；文件读写限大小；路径必须绝对（防相对路径歧义）。
 */
import { exec } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import http from 'node:http';
import https from 'node:https';

const OUT_MAX = 8000; // 单次输出截断上限（字符）

/** 执行 shell 命令（Windows：cmd；超时默认 30s 上限 120s；输出截断防爆 context） */
export function runBash({ command, timeoutMs = 30000, cwd } = {}) {
  if (!command || typeof command !== 'string') return { ok: false, error: '缺 command' };
  const timeout = Math.min(Number(timeoutMs) || 30000, 120000);
  return new Promise((resolve) => {
    exec(
      command,
      { timeout, cwd: cwd || undefined, windowsHide: true, maxBuffer: 2 * 1024 * 1024 },
      (err, stdout, stderr) => {
        const out = `${stdout || ''}${stderr ? `\n[stderr]\n${stderr}` : ''}`.trim().slice(0, OUT_MAX);
        resolve({
          ok: true,
          exitCode: err ? (typeof err.code === 'number' ? err.code : 1) : 0,
          output: out,
          killed: !!(err && err.killed),
        });
      }
    );
  });
}

/** 读文件（文本；限 512KB；UTF-8） */
export function readFileTool({ path: p, offset = 0, limit } = {}) {
  if (!p || typeof p !== 'string') return { ok: false, error: '缺 path' };
  try {
    const stat = fs.statSync(p);
    if (stat.size > 512 * 1024) return { ok: false, error: `文件太大（${(stat.size / 1024).toFixed(0)}KB > 512KB），请用 offset/limit 分段读` };
    let text = fs.readFileSync(p, 'utf8');
    if (limit && typeof limit === 'number') {
      const lines = text.split('\n');
      text = lines.slice(offset || 0, (offset || 0) + limit).join('\n');
    }
    return { ok: true, path: p, size: stat.size, content: text.slice(0, OUT_MAX * 2), truncated: text.length > OUT_MAX * 2 };
  } catch (e) {
    return { ok: false, error: String(e.message || e) };
  }
}

/** 写文件（UTF-8；父目录自动建；限 1MB） */
/** 相对路径 → 活跃工作区为基解析（2026-10-10 用户「产出物放进工作区」）。
 *  绝对路径原样返回；无工作区时相对路径按原行为（process.cwd）解析。 */
function resolveToWorkspace(p) {
  if (path.isAbsolute(p)) return p;
  try {
    const ws = JSON.parse(fs.readFileSync(path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'workspaces.json'), 'utf8'))?.active;
    if (ws && typeof ws === 'string') return path.join(ws, p);
  } catch { /* 无工作区清单 → 原行为 */ }
  return p;
}

export function writeFileTool({ path: p, content } = {}) {
  if (!p || typeof p !== 'string') return { ok: false, error: '缺 path' };
  if (typeof content !== 'string') return { ok: false, error: '缺 content' };
  if (Buffer.byteLength(content, 'utf8') > 1024 * 1024) return { ok: false, error: '内容超过 1MB，拒绝写入' };
  const full = resolveToWorkspace(p);
  try {
    fs.mkdirSync(path.dirname(full), { recursive: true });
    fs.writeFileSync(full, content, 'utf8');
    const res = { ok: true, path: full, bytes: Buffer.byteLength(content, 'utf8') };
    // 工作区外提醒（2026-10-10 用户）：文件落在了用户选的工作区外 → 结果里注入纠正线索，
    //   模型看到会自我纠错（重写进工作区或最后 present 登记）。present 兜底也会自动复制。
    try {
      const raw = JSON.parse(fs.readFileSync(path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'workspaces.json'), 'utf8'))?.active;
      if (raw && typeof raw === 'string') {
        const wsRoot = path.resolve(raw).toLowerCase();
        const abs = path.resolve(full).toLowerCase();
        if (!abs.startsWith(wsRoot + path.sep.toLowerCase()) && abs !== wsRoot) {
          res.hint = `注意：文件写在了用户选择的工作区之外。若是给用户的产出物，请把文件写一份到工作区（${raw}）下，或完成后调用 workbench_present_file 登记（会自动复制进工作区）。`;
        }
      }
    } catch { /* 无工作区配置 → 不提醒 */ }
    return res;
  } catch (e) {
    return { ok: false, error: String(e.message || e) };
  }
}

/** glob 文件搜索：在 base 目录下递归找匹配 pattern 的文件（支持 ** 和 *）。 */
export function globTool({ pattern, path: base = os.homedir() } = {}) {
  if (!pattern || typeof pattern !== 'string') return { ok: false, error: '缺 pattern' };
  const baseDir = path.resolve(base);
  const results = [];
  const maxHits = 200;
  try {
    walk(baseDir, pattern, baseDir, results, maxHits, 8);
  } catch (e) {
    return { ok: false, error: String(e.message || e) };
  }
  return { ok: true, pattern, base: baseDir, count: results.length, hits: results.slice(0, maxHits) };
}

function walk(dir, pattern, baseDir, out, maxHits, depth) {
  if (out.length >= maxHits || depth <= 0) return;
  let entries;
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true });
  } catch {
    return;
  }
  for (const e of entries) {
    if (out.length >= maxHits) return;
    const full = path.join(dir, e.name);
    if (e.isDirectory()) {
      if (e.name === 'node_modules' || e.name === '.git') continue;
      walk(full, pattern, baseDir, out, maxHits, depth - 1);
    } else if (e.isFile() && matchGlob(pattern, path.relative(baseDir, full))) {
      out.push(full);
    }
  }
}

/** 极简通配符匹配：** 跨目录，* 任意非分隔符。 */
function matchGlob(pattern, filePath) {
  const norm = filePath.replace(/\\/g, '/');
  const pat = pattern.replace(/\\/g, '/');
  const re = new RegExp(
    '^' + pat.split('**').map((seg) => seg.split('*').map((s) => s.replace(/[.+?^${}()|[\]\\]/g, '\\$&')).join('[^/]*')).join('.*') + '$'
  );
  return re.test(norm);
}

/** 抓取 URL 内容（HTTP/HTTPS，返回文本，限 64KB，超时 15s） */
export function webFetchTool({ url } = {}) {
  if (!url || typeof url !== 'string' || !/^https?:\/\//.test(url)) return { ok: false, error: 'url 必须是 http(s) 地址' };
  return new Promise((resolve) => {
    const mod = url.startsWith('https') ? https : http;
    const req = mod.get(url, { timeout: 15000, headers: { 'User-Agent': 'XiaoManBox/0.3' } }, (res) => {
      let buf = '';
      res.on('data', (c) => {
        buf += c;
        if (buf.length > 64 * 1024) req.destroy();
      });
      res.on('end', () => resolve({ ok: true, url, status: res.statusCode, text: buf.slice(0, OUT_MAX * 2) }));
      res.on('error', (e) => resolve({ ok: false, error: String(e.message || e) }));
    });
    req.on('timeout', () => { req.destroy(); resolve({ ok: false, error: '抓取超时（15s）' }); });
    req.on('error', (e) => resolve({ ok: false, error: String(e.message || e) }));
  });
}

/** 待办清单（内存 + 文件持久化 ~/.xiaomanbox/todo.json） */
let todos = [];
let todosLoaded = false;
const TODO_FILE = path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'todo.json');

function loadTodos() {
  if (todosLoaded) return;
  todosLoaded = true;
  try {
    const raw = JSON.parse(fs.readFileSync(TODO_FILE, 'utf8'));
    if (Array.isArray(raw)) todos = raw;
  } catch {
    todos = [];
  }
}

function saveTodos() {
  try {
    fs.mkdirSync(path.dirname(TODO_FILE), { recursive: true });
    fs.writeFileSync(TODO_FILE, JSON.stringify(todos, null, 2), 'utf8');
  } catch {
    /* 持久化失败不阻断 */
  }
}

/** items: [{text, done}] 整体替换待办清单 */
export function todoWriteTool({ items } = {}) {
  loadTodos();
  if (items === undefined || items === null) {
    return { ok: true, todos };
  }
  if (!Array.isArray(items)) return { ok: false, error: 'items 必须是数组' };
  todos = items.map((t) => ({
    text: String((t && t.text) || ''),
    done: !!(t && t.done),
  })).filter((t) => t.text);
  saveTodos();
  return { ok: true, todos };
}

/** 产出文件呈现（2026-10-08：小满做出网页/报告等，登记到「产出文件」，前端点开拉起） */
let presented = null;
const PRESENTED_FILE = path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'presented.json');

function loadPresented() {
  // 每次从文件读（文件很小，成本可忽略）：避免进程内缓存导致外部写入（如别的进程/工具登记）读不到。
  try {
    const raw = JSON.parse(fs.readFileSync(PRESENTED_FILE, 'utf8'));
    presented = Array.isArray(raw) ? raw : [];
  } catch {
    presented = [];
  }
}

export function presentFileTool({ path: p, label, sessionId } = {}) {
  if (!p || typeof p !== 'string') return { ok: false, error: '缺 path' };
  let full = p;
  if (!fs.existsSync(full)) full = resolveToWorkspace(p); // 相对路径以工作区解析
  if (!fs.existsSync(full)) return { ok: false, error: `文件不存在：${p}` };
  // 兜底归位（2026-10-10 用户「产出物都放在工作区」）：文件不在活跃工作区内 →
  //   复制一份进工作区根（重名加时间戳），登记工作区里的那份——保证用户在选的工作区能看到产出。
  let copied = false;
  let originalPath = null;
  try {
    const raw = JSON.parse(fs.readFileSync(path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'workspaces.json'), 'utf8'))?.active;
    if (raw && typeof raw === 'string') {
      const wsRoot = path.resolve(raw);
      const abs = path.resolve(full);
      if (!abs.toLowerCase().startsWith(wsRoot.toLowerCase() + path.sep) && abs.toLowerCase() !== wsRoot.toLowerCase()) {
        const base = path.basename(abs);
        let dest = path.join(wsRoot, base);
        if (fs.existsSync(dest)) {
          const ext = path.extname(base);
          dest = path.join(wsRoot, `${path.basename(base, ext)}_${Date.now()}${ext}`);
        }
        fs.copyFileSync(abs, dest);
        originalPath = abs;
        full = dest;
        copied = true;
      }
    }
  } catch { /* 无工作区/复制失败 → 按原路径登记 */ }
  loadPresented();
  presented = presented.filter((x) => x.path !== full);
  presented.unshift({ path: full, label: label || path.basename(full), at: Date.now(), sessionId: sessionId || null });
  try {
    fs.mkdirSync(path.dirname(PRESENTED_FILE), { recursive: true });
    fs.writeFileSync(PRESENTED_FILE, JSON.stringify(presented.slice(0, 50), null, 2), 'utf8');
  } catch {
    /* 持久化失败不阻断 */
  }
  return { ok: true, path: full, label: label || path.basename(full), ...(copied ? { copied: true, originalPath } : {}) };
}

/** 已呈现的产出文件列表（最新在前；传 sessionId 只返回该会话登记的，防串台） */
export function listPresented(sessionId) {
  loadPresented();
  const list = sessionId ? presented.filter((x) => x.sessionId === sessionId) : presented;
  return { ok: true, files: list.slice(0, 50) };
}

// ── edit_file（对应 dsh str-replace-editor，2026-10-09 补可映射工具）─────────
// Claude Code 风格：view（带行号查看）/ create（新建）/ str_replace（精确替换，唯一匹配）/ insert（指定行后插入）。
export function editFileTool({ path: p, command, old_str, new_str, insert_line, file_text } = {}) {
  if (!p || typeof p !== 'string') return { ok: false, error: '缺 path' };
  const cmd = String(command || 'view');

  if (cmd === 'view') {
    try {
      const text = fs.readFileSync(p, 'utf8');
      const lines = text.split('\n');
      const numbered = lines.map((l, i) => `${String(i + 1).padStart(4)}\t${l}`).join('\n');
      return { ok: true, path: p, lines: lines.length, content: numbered.slice(0, OUT_MAX) };
    } catch (e) {
      return { ok: false, error: `读取失败：${e.message || e}` };
    }
  }

  if (cmd === 'create') {
    if (fs.existsSync(p)) return { ok: false, error: `文件已存在：${p}（改用 str_replace 修改）` };
    if (file_text === undefined) return { ok: false, error: '缺 file_text' };
    try {
      fs.mkdirSync(path.dirname(p), { recursive: true });
      fs.writeFileSync(p, String(file_text), 'utf8');
      return { ok: true, path: p, created: true };
    } catch (e) {
      return { ok: false, error: `写入失败：${e.message || e}` };
    }
  }

  if (cmd === 'str_replace') {
    if (old_str === undefined || new_str === undefined) return { ok: false, error: '缺 old_str / new_str' };
    let text;
    try { text = fs.readFileSync(p, 'utf8'); } catch (e) { return { ok: false, error: `读取失败：${e.message || e}` }; }
    const count = text.split(old_str).length - 1;
    if (count === 0) return { ok: false, error: 'old_str 未找到（内容需完全匹配，含缩进/换行）' };
    if (count > 1) return { ok: false, error: `old_str 出现 ${count} 次，需唯一匹配（扩大 old_str 使其唯一）` };
    try {
      fs.writeFileSync(p, text.replace(old_str, new_str), 'utf8');
      return { ok: true, path: p, replaced: 1 };
    } catch (e) {
      return { ok: false, error: `写入失败：${e.message || e}` };
    }
  }

  if (cmd === 'insert') {
    if (insert_line === undefined || new_str === undefined) return { ok: false, error: '缺 insert_line / new_str' };
    let lines;
    try { lines = fs.readFileSync(p, 'utf8').split('\n'); } catch (e) { return { ok: false, error: `读取失败：${e.message || e}` }; }
    const idx = Number(insert_line);
    if (!Number.isInteger(idx) || idx < 0 || idx > lines.length) return { ok: false, error: `insert_line 超出范围（0~${lines.length}）` };
    lines.splice(idx, 0, String(new_str));
    try {
      fs.writeFileSync(p, lines.join('\n'), 'utf8');
      return { ok: true, path: p, inserted: 1, atLine: idx };
    } catch (e) {
      return { ok: false, error: `写入失败：${e.message || e}` };
    }
  }

  return { ok: false, error: `未知 command：${cmd}（支持 view / create / str_replace / insert）` };
}

// ── skill（对应 dsh-tool-skill，2026-10-09 补可映射工具）─────────
// 列出小满可加载的技能（从 agent 的 skill 机制读取，这里只做「查看」；实际加载由 agent 循环负责）
export function skillListTool() {
  // 技能清单来源：~/.workbuddy/skills（用户级）+ 项目 .workbuddy/skills（项目级）
  const dirs = [
    path.join(os.homedir(), '.workbuddy', 'skills'),
    path.join(process.cwd(), '.workbuddy', 'skills'),
  ];
  const skills = [];
  for (const dir of dirs) {
    if (!fs.existsSync(dir)) continue;
    try {
      for (const name of fs.readdirSync(dir)) {
        const sk = path.join(dir, name, 'SKILL.md');
        if (fs.existsSync(sk)) skills.push({ name, path: sk });
      }
    } catch { /* 忽略 */ }
  }
  return { ok: true, count: skills.length, skills };
}

// ── 工具启用状态（2026-10-09 插件「下载→装载→使用」闭环，路线 A）─────────
// 工作台自研工具清单：每个工具对应一个 dsh 插件（能力对齐），可单独启用/停用。
// 「下载」一个插件 = 启用对应工具；停用后 agent 不再把它传给模型。
const ENABLED_FILE = path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'enabled-tools.json');

export const WORKBENCH_TOOL_META = [
  { name: 'bash', dsh: 'dsh-tool-bash', label: '执行命令' },
  { name: 'read_file', dsh: 'dsh-tool-fs', label: '读文件' },
  { name: 'write_file', dsh: 'dsh-tool-fs', label: '写文件' },
  { name: 'glob', dsh: 'dsh-tool-fs-search', label: '搜文件' },
  { name: 'web_fetch', dsh: 'dsh-tool-web', label: '联网抓取' },
  { name: 'web_search', dsh: 'dsh-tool-web', label: '联网搜索' },
  { name: 'todo_write', dsh: 'dsh-tool-todo', label: '待办清单' },
  { name: 'workbench_present_file', dsh: 'dsh-tool-present', label: '产出文件' },
  { name: 'edit_file', dsh: 'dsh-tool-str-replace-editor', label: '精确编辑文件' },
  { name: 'skill_list', dsh: 'dsh-tool-skill', label: '技能清单' },
  { name: 'ask_user', dsh: 'dsh-tool-ask-user', label: '向用户提问' },
  { name: 'bash_persistent', dsh: 'dsh-tool-bash-persistent', label: '持久终端会话' },
  { name: 'pwsh', dsh: 'dsh-tool-pwsh', label: 'PowerShell' },
  { name: 'jobs', dsh: 'dsh-tool-jobs', label: '后台任务控制' },
  { name: 'subagent', dsh: 'dsh-tool-subagent', label: '子代理委派' },
  { name: 'workflow', dsh: 'dsh-tool-workflow', label: '编排脚本' },
  { name: 'goal', dsh: 'dsh-tool-goal', label: '会话目标' },
  { name: 'schedule', dsh: 'dsh-schedule', label: '定时提醒' },
  { name: 'webhook', dsh: 'dsh-webhook', label: '触发器' },
  { name: 'hooks', dsh: 'dsh-hooks-claude-code', label: 'Hook 桥接' },
];

/** 读取已启用的工具名列表（缺省 = 全启用） */
export function loadEnabledTools() {
  try {
    const raw = JSON.parse(fs.readFileSync(ENABLED_FILE, 'utf8'));
    if (raw && Array.isArray(raw.enabled)) {
      const list = raw.enabled.filter((n) => typeof n === 'string');
      return list; // 显式空数组表示全停用；只有缺少配置才默认全启用。
    }
  } catch { /* 忽略 */ }
  return WORKBENCH_TOOL_META.map((t) => t.name);
}

/** 启用/停用一个工具；返回最新启用列表 */
export function setToolEnabled(name, enabled) {
  if (!WORKBENCH_TOOL_META.some((t) => t.name === name)) {
    return { ok: false, code: 404, error: `未知工具：${name}` };
  }
  let list = loadEnabledTools();
  if (enabled) {
    if (!list.includes(name)) list.push(name);
  } else {
    list = list.filter((n) => n !== name);
  }
  try {
    fs.mkdirSync(path.dirname(ENABLED_FILE), { recursive: true });
    fs.writeFileSync(ENABLED_FILE, JSON.stringify({ enabled: list }, null, 2), 'utf8');
  } catch { /* 忽略 */ }
  return { ok: true, name, enabled: !!enabled, enabledTools: list };
}

/** 工作台工具清单（含启用状态）——插件社区「下载/已装载」用 */
export function listWorkbenchTools() {
  const enabled = loadEnabledTools();
  return {
    ok: true,
    count: WORKBENCH_TOOL_META.length,
    tools: WORKBENCH_TOOL_META.map((t) => ({ ...t, enabled: enabled.includes(t.name) })),
  };
}

// ── ask_user（对应 dsh-tool-ask-user，2026-10-09 补可映射工具）─────────
// 小满是「派任务→跑完→返回」的单例模型，无交互式挂起。故 ask_user 落地为：
//   登记一条待答问题（落盘 ~/.xiaomanbox/pending-questions.json），用户在「团队交流/Agent 面板」
//   能看到并回答；小满下一次派活时把「已登记的问题」注入上下文提示用户回答。
// 诚实边界：不假装「当场等到答案」，返回的是「已登记，待用户在界面回答」。
const ASK_FILE = path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'pending-questions.json');

export function askUserTool({ question, options } = {}) {
  if (!question || typeof question !== 'string' || !question.trim()) return { ok: false, error: '缺 question' };
  let list = [];
  try { list = JSON.parse(fs.readFileSync(ASK_FILE, 'utf8')); if (!Array.isArray(list)) list = []; } catch { list = []; }
  const item = {
    id: `Q-${Date.now()}`,
    question: question.trim(),
    options: Array.isArray(options) ? options.map((o) => String(o)).slice(0, 10) : [],
    at: new Date().toISOString(),
    answered: null,
  };
  list.unshift(item);
  try {
    fs.mkdirSync(path.dirname(ASK_FILE), { recursive: true });
    fs.writeFileSync(ASK_FILE, JSON.stringify(list.slice(0, 20), null, 2), 'utf8');
  } catch { /* 持久化失败不阻断 */ }
  return {
    ok: true,
    id: item.id,
    question: item.question,
    note: '已向用户提问并登记。小满当前是「派任务→跑完→返回」的单轮模型，无法在任务中途暂停等你回答——请在任务产出里把这个问题连同你的选择一起回复，或结束本轮后在输入框直接回答，我再继续。',
  };
}

/** 列出待答问题（前端「团队交流」面板可展示） */
export function listPendingQuestions() {
  let list = [];
  try { list = JSON.parse(fs.readFileSync(ASK_FILE, 'utf8')); if (!Array.isArray(list)) list = []; } catch { list = []; }
  return { ok: true, count: list.length, questions: list.filter((q) => q.answered === null) };
}

/** 回答一个待答问题（前端或 agent 回答后标记） */
export function answerQuestion(id, answer) {
  let list = [];
  try { list = JSON.parse(fs.readFileSync(ASK_FILE, 'utf8')); if (!Array.isArray(list)) list = []; } catch { list = []; }
  const q = list.find((x) => x.id === id);
  if (!q) return { ok: false, error: `问题不存在：${id}` };
  q.answered = String(answer ?? '');
  q.answeredAt = new Date().toISOString();
  try { fs.writeFileSync(ASK_FILE, JSON.stringify(list.slice(0, 20), null, 2), 'utf8'); } catch { /* 忽略 */ }
  return { ok: true, id, answered: q.answered };
}

// ── bash_persistent（对应 dsh-tool-bash-persistent，2026-10-09 补可映射工具）─────────
// 持久 bash 会话：跨轮保持 cwd（Windows cmd），进程内会话表（重启后清空，如实说明）。
const PTY_SESSIONS = new Map(); // id -> { cwd, createdAt, lastCmd }

export function bashPersistentTool({ command, session_id, cwd } = {}) {
  const sid = session_id && typeof session_id === 'string' && session_id.trim() ? session_id.trim() : 'default';
  let s = PTY_SESSIONS.get(sid);
  if (!s) {
    s = { cwd: cwd || process.cwd(), createdAt: Date.now(), lastCmd: null };
    PTY_SESSIONS.set(sid, s);
  }
  if (!command || typeof command !== 'string') {
    return { ok: true, session_id: sid, cwd: s.cwd, note: '未传 command，返回会话当前状态（可用 command 参数继续执行，跨轮保持工作目录）' };
  }
  // 支持 cd 切换目录（解析为 chdir，其余交给 cmd）
  const cdMatch = /^(?:cd|chdir)\s+(.+)$/i.exec(command.trim());
  if (cdMatch) {
    const target = cdMatch[1].replace(/^["']|["']$/g, '').trim();
    const resolved = path.resolve(s.cwd, target);
    try {
      if (!fs.existsSync(resolved) || !fs.statSync(resolved).isDirectory()) return { ok: false, error: `目录不存在：${resolved}` };
      s.cwd = resolved;
      s.lastCmd = command;
      return { ok: true, session_id: sid, cwd: s.cwd, note: `已切换到 ${s.cwd}` };
    } catch (e) {
      return { ok: false, error: String(e.message || e) };
    }
  }
  s.lastCmd = command;
  return new Promise((resolve) => {
    exec(
      command,
      { timeout: Math.min(Number(30000), 120000), cwd: s.cwd, windowsHide: true, maxBuffer: 2 * 1024 * 1024 },
      (err, stdout, stderr) => {
        const out = `${stdout || ''}${stderr ? `\n[stderr]\n${stderr}` : ''}`.trim().slice(0, OUT_MAX);
        resolve({
          ok: true,
          session_id: sid,
          cwd: s.cwd,
          exitCode: err ? (typeof err.code === 'number' ? err.code : 1) : 0,
          output: out,
        });
      }
    );
  });
}

/** 列出持久 bash 会话（诊断用） */
export function listBashPersistentSessions() {
  return { ok: true, sessions: [...PTY_SESSIONS.entries()].map(([id, s]) => ({ id, cwd: s.cwd, lastCmd: s.lastCmd })) };
}

// ── jobs（对应 dsh-tool-jobs，2026-10-09 补可映射工具）─────────
// 后台任务控制：映射到小满的单例 agent 任务（agentJobInfo / stopAgent）。
// 诚实边界：小满同时只跑一个任务，job_list 通常只有一个或没有。
import * as _agent from './agent.mjs';

export function jobsTool({ action = 'list' } = {}) {
  const a = String(action || 'list').toLowerCase();
  if (a === 'list' || a === 'status') {
    const info = _agent.agentJobInfo();
    if (!info || info.state === 'idle') return { ok: true, action, jobs: [], note: '当前没有运行中的后台任务（小满单例，同时只跑一个）' };
    return {
      ok: true,
      action,
      jobs: [{ jobId: info.jobId, state: info.state, task: (info.task || '').slice(0, 200), startedAt: info.startedAt, sessionId: info.sessionId || null }],
    };
  }
  if (a === 'kill' || a === 'stop') {
    const r = _agent.stopAgent();
    return r.ok ? { ok: true, action, ...r } : { ok: false, error: r.error };
  }
  return { ok: false, error: `未知 action：${action}（支持 list / status / kill）` };
}

// ── subagent / workflow（诚实占位，2026-10-09）─────────
// 小满当前是单例 agent 架构（同一时刻只跑一个任务），且没有 JS 编排引擎。
// 硬做成「并行子代理」或「跑 JS 编排脚本」会是空壳（违反诚实边界），
// 故这里如实返回能力边界，不假装实现。
export function subagentTool() {
  return {
    ok: false,
    error: '小满当前是单例架构（同一时刻只跑一个任务），暂不支持 spawn 并行子代理。可改用「外部监工」多模型主从编排（Supervisor 面板）来委派子任务。',
  };
}

export function workflowTool() {
  return {
    ok: false,
    error: '小满当前没有 JS 编排引擎，暂不支持跑 workflow 编排脚本。可用 todo_write 拆解多步任务 + 顺序执行来达到类似效果。',
  };
}

// ── web_search（对应 dsh-tool-web 的 web_search，2026-10-09 补）─────────
// 联网搜索：调 DuckDuckGo HTML 端点（免 key），返回标题+摘要+链接，喂给模型。
// 诚实边界：DuckDuckGo HTML 无官方 API 保证，可能被限流/改版；失败时如实报错并建议用 web_fetch。
export async function webSearchTool({ query, max = 8 } = {}) {
  if (!query || typeof query !== 'string' || !query.trim()) return { ok: false, error: '缺 query' };
  const n = Math.min(Number(max) || 8, 20);
  const url = `https://html.duckduckgo.com/html/?q=${encodeURIComponent(query.trim())}`;
  return new Promise((resolve) => {
    const req = https.get(url, { timeout: 20000, headers: { 'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) XiaoManBox/0.3' } }, (res) => {
      let buf = '';
      res.on('data', (c) => { buf += c; if (buf.length > 512 * 1024) req.destroy(); });
      res.on('end', () => {
        // 解析 DuckDuckGo HTML 结果：<a class="result__a">标题</a> + <a class="result__snippet">摘要</a>
        const results = [];
        const linkRe = /<a[^>]*class="result__a"[^>]*href="([^"]+)"[^>]*>([\s\S]*?)<\/a>/g;
        const snipRe = /<a[^>]*class="result__snippet"[^>]*>([\s\S]*?)<\/a>/g;
        const links = [...buf.matchAll(linkRe)];
        const snips = [...buf.matchAll(snipRe)];
        for (let i = 0; i < Math.min(links.length, n); i++) {
          const rawUrl = links[i][1] || '';
          const title = (links[i][2] || '').replace(/<[^>]+>/g, '').trim();
          const snippet = (snips[i]?.[1] || '').replace(/<[^>]+>/g, '').trim();
          // 去掉 DuckDuckGo 的跳转包装，还原真实 URL
          const real = decodeURIComponent((rawUrl.match(/uddg=([^&]+)/)?.[1] || rawUrl));
          if (title) results.push({ title, url: real, snippet });
        }
        resolve({ ok: true, query: query.trim(), count: results.length, results });
      });
      res.on('error', (e) => resolve({ ok: false, error: `搜索失败：${e.message || e}` }));
    });
    req.on('timeout', () => { req.destroy(); resolve({ ok: false, error: '搜索超时（20s），可改用 web_fetch 抓具体页面' }); });
    req.on('error', (e) => resolve({ ok: false, error: `搜索失败：${e.message || e}（可能需要联网）` }));
  });
}

// ── pwsh（对应 dsh-tool-pwsh，2026-10-09 补）─────────
// PowerShell 执行：复用 bash 的执行器，换 powershell.exe。
export function pwshTool({ command, timeoutMs = 30000 } = {}) {
  if (!command || typeof command !== 'string') return { ok: false, error: '缺 command' };
  const timeout = Math.min(Number(timeoutMs) || 30000, 120000);
  return new Promise((resolve) => {
    exec(
      `powershell -NoProfile -NonInteractive -Command ${JSON.stringify(command)}`,
      { timeout, windowsHide: true, maxBuffer: 2 * 1024 * 1024 },
      (err, stdout, stderr) => {
        const out = `${stdout || ''}${stderr ? `\n[stderr]\n${stderr}` : ''}`.trim().slice(0, OUT_MAX);
        resolve({ ok: true, exitCode: err ? (typeof err.code === 'number' ? err.code : 1) : 0, output: out });
      }
    );
  });
}

// ── goal（对应 dsh-tool-goal，2026-10-09 补）─────────
// 会话目标：落盘一个「当前会话目标」，注入系统提示词让模型对齐目标。
const GOAL_FILE = path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'session-goal.json');

export function goalTool({ goal, action = 'set' } = {}) {
  const a = String(action || 'set').toLowerCase();
  if (a === 'clear') {
    try { fs.rmSync(GOAL_FILE, { force: true }); } catch { /* 忽略 */ }
    return { ok: true, action, goal: null, note: '已清除会话目标' };
  }
  if (a === 'get' || a === 'list') {
    try { const g = JSON.parse(fs.readFileSync(GOAL_FILE, 'utf8')); return { ok: true, action, ...g }; } catch { return { ok: true, action, goal: null }; }
  }
  if (a === 'set') {
    if (!goal || typeof goal !== 'string' || !goal.trim()) return { ok: false, error: '缺 goal（或 action=get/clear）' };
    const rec = { goal: goal.trim(), at: new Date().toISOString() };
    try { fs.mkdirSync(path.dirname(GOAL_FILE), { recursive: true }); fs.writeFileSync(GOAL_FILE, JSON.stringify(rec, null, 2), 'utf8'); } catch { /* 忽略 */ }
    return { ok: true, action, ...rec, note: '已记录会话目标，后续轮次会注入系统提示词对齐' };
  }
  return { ok: false, error: `未知 action：${action}（支持 set / get / clear）` };
}

// ── schedule（对应 dsh-schedule，2026-10-09 补）─────────
// 定时提醒：落盘定时任务表。真正到点触发靠「自动化」机制（后端 tick 或外部调度），
// 这里提供「登记/列表/删除」；诚实说明触发需另接调度器。
const SCHEDULE_FILE = path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'schedule.json');

export function scheduleTool({ action = 'list', at, text, id } = {}) {
  const a = String(action || 'list').toLowerCase();
  let list = [];
  try { list = JSON.parse(fs.readFileSync(SCHEDULE_FILE, 'utf8')); if (!Array.isArray(list)) list = []; } catch { list = []; }
  if (a === 'add') {
    if (!at || !text) return { ok: false, error: '缺 at（ISO 时间）或 text（提醒内容）' };
    const rec = { id: `S-${Date.now()}`, at: String(at), text: String(text).trim(), createdAt: new Date().toISOString() };
    list.push(rec);
    fs.writeFileSync(SCHEDULE_FILE, JSON.stringify(list, null, 2), 'utf8');
    return { ok: true, action, ...rec, note: '已登记定时提醒。到点触发需工作台运行时由调度器 tick（当前为登记态）' };
  }
  if (a === 'remove' || a === 'del') {
    if (!id) return { ok: false, error: '缺 id' };
    const before = list.length;
    list = list.filter((r) => r.id !== id);
    fs.writeFileSync(SCHEDULE_FILE, JSON.stringify(list, null, 2), 'utf8');
    return { ok: true, action, removed: before - list.length };
  }
  return { ok: true, action, count: list.length, items: list };
}

// ── webhook（对应 dsh-webhook，2026-10-09 补）─────────
// HTTP 触发器：登记一个 webhook 规则（URL 收到请求 → 触发一个任务）。
// 诚实边界：真正「收到请求就触发」需要常驻 HTTP 监听 + 任务调度，这里提供登记/列表，
// 触发执行需接工作台的调度层。
const WEBHOOK_FILE = path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'webhooks.json');

export function webhookTool({ action = 'list', name, task, url } = {}) {
  const a = String(action || 'list').toLowerCase();
  let list = [];
  try { list = JSON.parse(fs.readFileSync(WEBHOOK_FILE, 'utf8')); if (!Array.isArray(list)) list = []; } catch { list = []; }
  if (a === 'add') {
    if (!name || !task) return { ok: false, error: '缺 name 或 task' };
    const rec = { id: `W-${Date.now()}`, name: String(name).trim(), task: String(task).trim(), url: url ? String(url) : '', createdAt: new Date().toISOString() };
    list.push(rec);
    fs.writeFileSync(WEBHOOK_FILE, JSON.stringify(list, null, 2), 'utf8');
    return { ok: true, action, ...rec, note: '已登记 webhook 规则。真实「收到请求即触发」需常驻监听 + 调度，当前为登记态' };
  }
  if (a === 'remove' || a === 'del') {
    if (!name) return { ok: false, error: '缺 name' };
    const before = list.length;
    list = list.filter((r) => r.name !== name);
    fs.writeFileSync(WEBHOOK_FILE, JSON.stringify(list, null, 2), 'utf8');
    return { ok: true, action, removed: before - list.length };
  }
  return { ok: true, action, count: list.length, items: list };
}

// ── hooks（对应 dsh-hooks-claude-code / codex，2026-10-09 补）─────────
// 诚实占位：Claude Code/Codex 的 hook 是那些工具自己的机制，小满无法「桥接」它们的
// 内部 hook 运行时。这里如实返回边界，不假装实现。
export function hooksTool() {
  return {
    ok: false,
    error: 'Claude Code / Codex 的 hooks 是它们各自的内部机制，小满无法桥接。若要在小满任务前后挂钩子，请用「外部监工」（Supervisor）或 schedule 定时触发替代。',
  };
}
