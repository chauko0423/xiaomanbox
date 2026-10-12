<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { getContextUsage, getAgentJob, startAgentTask, stopAgentTask, transcribeVoice, type ContextUsageResponse } from '../api/agent';
import type { AgentJobResponse } from '../api/contract';
import { ContractError } from '../api/contract';
import { currentScene, currentSessionId, newChatPending, rebindSceneSession, settleNewChat, switchFunction, switchScene, presetCategories, presetList, presetsLoaded, bumpSessions, setAgentChatMode } from '../scenes/store';
import { loadState, patchState } from '../api/state';
import logoMark from '../assets/logo-mark.png';
import { getUalStatus } from '../api/ual';
import { listSkills, type SkillMeta } from '../api/skills';
import { listWorkspaces, addWorkspace, setActiveWorkspace, type WorkspaceInfo } from '../api/workspace';
import { getGlobalMemory } from '../api/memory';

import { archiveSession, createSession, getSessionMessages, listSessions, type SessionMessage, type SessionSummary } from '../api/session';
import {
  listProviders,
  activateProvider,
  getAgentConfig,
  putAgentConfig,
  type ProviderInfo,
} from '../api/providers';
import { listMcpServers, type McpServerInfo } from '../api/mcp';
import AppIcon from '../components/AppIcon.vue';
import { listPresented, openPresentedFile, fetchFileContent, type PresentedFile } from '../api/presented';
import { t } from '../i18n';

/**
 * 小满 Agent —— Hero 启动台（2026-10-07，参考 WorkBuddy「WorkBuddy, 我帮你」）。
 * 这一版不再「绕过」参考图里的槽位，而是把它们做成真实能力：
 *   - 斜杠指令 `/`：工作台自有工具 + skill 清单（真实数据，点选填入结构化任务）
 *   - @ 上下文：会话域 / 全局记忆 / 在线成员 / 内置引擎工具数（真实数据，注入任务文本）
 *   - 输入框工具条（2026-10-07 下午，参考 WorkBuddy 三截图）：
 *       「+」功能菜单（添加文件 / 模式 / 技能 / 连接器）
 *       「允许完全访问」权限气泡（写 agent.json，真实改写 system prompt 行为约束）
 *       模型选择器（providers 真实列表 + 激活切换 + 思考模式/强度）
 *   - 麦克风：诚实标注「待语音引擎」——唯一缺硬件能力的槽位，不放可点的假按钮
 */
const task = ref('');
const job = ref<AgentJobResponse | null>(null);
const err = ref<string | null>(null);
const notice = ref<string | null>(null); // 轻提示（复制成功等），随下次操作清除
const starting = ref(false);
const stopping = ref(false); // 停止按钮点击中（2026-10-10 中断任务）
/** 停止当前任务：立即掐断在途模型请求，任务标「已停止」；结束后刷新会话与产出物 */
async function stopTask() {
  if (stopping.value) return;
  stopping.value = true;
  try {
    await stopAgentTask();
    // 等 job 轮询自然看到 aborted；立即刷一次让按钮立刻切回发送
    await refresh();
    if (job.value?.state !== 'running') {
      pendingTask.value = null;
      await loadChatHistory();
      void refreshPresented();
      scrollChat();
    }
  } catch (e) {
    err.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  } finally {
    stopping.value = false;
  }
}
let pollTimer: number | undefined;

// ── Hero 数据（真实数据，拿不到就降级）───────────────────────
const ualConnected = ref<boolean | null>(null);
const ualToolCount = ref(0);
const sceneName = currentScene;

// ── 斜杠指令（真实数据源：工作台工具 + skill 清单 + 会话指令）──
// 工作台自有工具（与 backend/agent.mjs 的 WORKBENCH_TOOLS 对齐，这里是给用户看的可点指令）
interface Command {
  key: string;      // 唯一 key
  label: string;    // 短标签
  hint: string;     // 一句话说明
  fill: string;     // 点选后填入输入框的任务文本（action 存在时可不填）
  kind: 'tool' | 'skill' | 'session';
  action?: () => void; // 会话指令：点击直接执行（不填文本）
}
const WORKBENCH_COMMANDS: Command[] = [
  { key: 'launch', label: '拉起编辑器', hint: '启动 UE5 编辑器并等连接就绪', fill: '启动 UE5 编辑器，打开 Xiaoman 工程并等就绪', kind: 'tool' },
  { key: 'compile', label: '编译验证', hint: '增量编译并解读结果', fill: '跑一次编译验证，并解读结果', kind: 'tool' },
  { key: 'bootstrap', label: '一键建工程', hint: '从零拉起：建工程+蓝图+编译', fill: '一键从零拉起 UE5 工程（建工程+建蓝图+编译）', kind: 'tool' },
  { key: 'board', label: '启动画板', hint: '拉起团队协作白板', fill: '启动团队画板，给我可打开的地址', kind: 'tool' },
  { key: 'chat', label: '发频道消息', hint: '往团队频道发一条消息', fill: '在团队频道里发一条消息：', kind: 'tool' },
];
// 会话级指令（DSH 图4；只放有真实实现的——compact/goal/feedback 等待后端，不造假）
const SESSION_COMMANDS: Command[] = [
  { key: 'export', label: 'export', hint: '把当前会话导出为 Markdown 文件', fill: '', kind: 'session', action: () => exportSession() },
  { key: 'plan', label: 'plan', hint: '计划模式：下一条任务先出计划再动手', fill: '请先给出分步计划，经我确认后再执行：', kind: 'session' },
  { key: 'permission', label: 'permission', hint: '切换权限预设（允许完全访问）', fill: '', kind: 'session', action: () => togglePop('perm') },
  { key: 'model', label: 'model', hint: '选择本会话使用的模型', fill: '', kind: 'session', action: () => togglePop('model') },
];
const skillCommands = ref<Command[]>([]);
const commands = computed<Command[]>(() => [...WORKBENCH_COMMANDS, ...SESSION_COMMANDS, ...skillCommands.value]);
/** 本次任务选择加载的 skill（2026-10-09 用户：选择性加载，默认空 = 不注入任何 skill） */
const selectedSkills = ref<string[]>([]);
function toggleSkill(name: string) {
  const i = selectedSkills.value.indexOf(name);
  if (i >= 0) selectedSkills.value.splice(i, 1);
  else selectedSkills.value.push(name);
}
/** 工具组（2026-10-10 用户）：UE 工具 166 个 schema 62KB，默认不带（prefill 11s+/502 主因），勾选才带 */
const limitToolText = ref('');
const noTools = ref(false);
function selectedToolNames(): string[] | undefined {
  return noTools.value ? [] : limitToolText.value.trim() ? limitToolText.value.split(/[\s,，]+/).filter(Boolean) : undefined;
}
const loadUeTools = ref<boolean | null>(null); // null=按预设自动；true/false=用户显式选择
const autoUe = ref(true); // 当前会话按预设是否自动带 UE 工具（展示用）
function effectiveUe(): boolean {
  return loadUeTools.value === null ? autoUe.value : loadUeTools.value;
}
/** Command.key 是 `skill:<name>`，提取 name */
function skillNameOf(c: { key: string }): string {
  return c.key.startsWith('skill:') ? c.key.slice(6) : c.key;
}

// 导出会话（/export）：拉会话消息拼 Markdown 下载——真实能力，不经过后端新接口
async function exportSession() {
  menuOpen.value = null;
  if (!currentSessionId.value) {
    err.value = '当前没有绑定的会话，无法导出';
    return;
  }
  try {
    const r = await getSessionMessages(currentSessionId.value);
    const md = buildSessionMd(r.messages || [], currentSessionId.value);
    const blob = new Blob([md], { type: 'text/markdown;charset=utf-8' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `xiaoman-session-${currentSessionId.value.slice(0, 8)}.md`;
    a.click();
    URL.revokeObjectURL(a.href);
  } catch (e) {
    err.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  }
}

// ── @ 上下文（真实数据源：会话域 / 记忆 / 成员 / 工具）─────────
interface Ctx {
  key: string;
  label: string;
  kind: 'session' | 'memory' | 'member' | 'uebox';
  summary: string;  // 一行摘要，点选后以 [标签:摘要] 注入
}
const memoryKeys = ref<string[]>([]);


const ctxs = computed<Ctx[]>(() => {
  const list: Ctx[] = [];
  // 会话域（总是有）
  list.push({
    key: 'session',
    label: '当前会话',
    kind: 'session',
    summary: `会话域 ${sceneName.value?.name ?? '—'} · ${shortId(currentSessionId.value)}`,
  });
  // 全局记忆（有键才出现）
  for (const k of memoryKeys.value) {
    list.push({ key: `mem:${k}`, label: `记忆 · ${k}`, kind: 'memory', summary: k });
  }
  // 在线成员

  // 内置引擎工具（UAL 服务端就绪就有，连上引擎更直观）
  if (ualConnected.value === true) {
    list.push({ key: 'ual', label: '引擎工具', kind: 'uebox', summary: `内置 ${ualToolCount.value} 个工具，引擎已连接` });
  } else if (ualConnected.value === false) {
    list.push({ key: 'ual', label: '引擎工具', kind: 'uebox', summary: `内置 ${ualToolCount.value} 个工具，等待引擎连接` });
  }
  return list;
});

// ── 菜单开合状态 ───────────────────────────────────────────
const menuOpen = ref<null | 'slash' | 'at'>(null);
const cmdQuery = ref('');
const ctxQuery = ref('');
const inputBox = ref<HTMLTextAreaElement | null>(null);

// ── 输入框工具条（WorkBuddy 三截图：+ 菜单 / 权限 / 模型）────
// pop：弹出互斥（+ 菜单 / 权限气泡 / 模型菜单 / 会话 / 用量 / 搜索 / 分享 / 历史提问 / 任务概览）
const pop = ref<null | 'plus' | 'perm' | 'model' | 'sessions' | 'usage' | 'search' | 'share' | 'history' | 'overview'>(null);
const plusView = ref<'root' | 'mode' | 'skill' | 'conn'>('root');
const modelView = ref<'root' | 'think'>('root');
const fileInput = ref<HTMLInputElement | null>(null);

// 模型提供方（真实：providers.json）
const providerList = ref<ProviderInfo[]>([]);
const activeProviderId = ref<string | null>(null);
const activeProviderName = computed(() => {
  const p = providerList.value.find((x) => x.id === activeProviderId.value);
  return p ? p.name : '内置模型';
});

// Agent 运行参数（真实：agent.json）
const agentCfg = ref<{ temperature: number; thinkingBudget: number; allowFullAccess: boolean; retainSessions: boolean } | null>(null);
const mcpServers = ref<McpServerInfo[]>([]);

function togglePop(which: 'plus' | 'perm' | 'model' | 'sessions' | 'usage' | 'search' | 'share' | 'history' | 'overview') {
  if (pop.value === which) {
    pop.value = null;
    return;
  }
  pop.value = which;
  plusView.value = 'root';
  modelView.value = 'root';
  if (which === 'sessions') void loadSessions();
  if (which === 'usage') void loadUsage();
}

// ── 工具条四件套（2026-10-09 用户：右上角 + 右侧产出物栏）──

// 右侧产出物栏（md/文件/网页，随会话；Ctrl+Shift+B 切换，持久化 ui.json）
const outbarOpen = ref(false);
// 产出物栏宽度（2026-10-10 用户「可拖拽分界线」）：右侧预览栏不设最大限制（自由拖，仅保底最小 200），持久化 ui.json
const outbarWidth = ref(306);
const OUTBAR_MIN = 200;
async function loadOutbarPref() {
  try {
    const saved = await loadState<{ outbarOpen?: boolean; outbarWidth?: number }>('ui');
    outbarOpen.value = !!saved?.outbarOpen;
    if (typeof saved?.outbarWidth === 'number') outbarWidth.value = Math.max(OUTBAR_MIN, saved.outbarWidth);
  } catch { /* 后端离线走默认关 */ }
}
function toggleOutbar() {
  outbarOpen.value = !outbarOpen.value;
  patchState('ui', { outbarOpen: outbarOpen.value }).catch(() => {});
}
function onOutbarResizeStart(e: MouseEvent) {
  e.preventDefault();
  const startX = e.clientX;
  const startW = outbarWidth.value;
  const move = (ev: MouseEvent) => {
    outbarWidth.value = Math.max(OUTBAR_MIN, startW - (ev.clientX - startX));
  };
  const up = () => {
    window.removeEventListener('mousemove', move);
    window.removeEventListener('mouseup', up);
    patchState('ui', { outbarWidth: outbarWidth.value }).catch(() => {});
  };
  window.addEventListener('mousemove', move);
  window.addEventListener('mouseup', up);
}

// ── 产出物应用内预览（2026-10-10 用户「像 WorkBuddy 直接在侧边栏打开文件」）──
const selectedFilePath = ref<string | null>(null);
const filePreview = ref<{ kind: string; text?: string; dataUrl?: string; name: string } | null>(null);
const previewLoading = ref(false);
const previewError = ref<string | null>(null);
async function selectFile(f: PresentedFile) {
  selectedFilePath.value = f.path;
  previewLoading.value = true;
  previewError.value = null;
  filePreview.value = null;
  try {
    const r = await fetchFileContent(f.path);
    if (!r.ok) previewError.value = r.error || '读取失败';
    else filePreview.value = { kind: r.kind, text: r.text, dataUrl: r.dataUrl, name: r.name };
  } catch (e) {
    previewError.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  } finally {
    previewLoading.value = false;
  }
}
/** 轻量 md → html（标题/列表/粗斜体/行内代码/代码块/引用/链接/分隔线，覆盖学习规划类文档够用） */
function mdToHtml(md: string): string {
  const esc = (s: string) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  const blocks: string[] = [];
  let src = md.replace(/```([\s\S]*?)```/g, (_m, code) => {
    blocks.push(`<pre class="pv-code">${esc(code.trim())}</pre>`);
    return `\u0000B${blocks.length - 1}\u0000`;
  });
  src = esc(src);
  src = src.replace(/^######\s+(.+)$/gm, '<h6>$1</h6>')
    .replace(/^#####\s+(.+)$/gm, '<h5>$1</h5>')
    .replace(/^####\s+(.+)$/gm, '<h4>$1</h4>')
    .replace(/^###\s+(.+)$/gm, '<h3>$1</h3>')
    .replace(/^##\s+(.+)$/gm, '<h2>$1</h2>')
    .replace(/^#\s+(.+)$/gm, '<h1>$1</h1>')
    .replace(/^&gt;\s?(.+)$/gm, '<blockquote>$1</blockquote>')
    .replace(/^---+\s*$/gm, '<hr/>')
    .replace(/^\d+\.\s+(.+)$/gm, '<ol-li>$1</ol-li>')
    .replace(/^[-*]\s+(.+)$/gm, '<ul-li>$1</ul-li>')
    .replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>')
    .replace(/\*([^*]+)\*/g, '<em>$1</em>')
    .replace(/`([^`]+)`/g, '<code>$1</code>')
    .replace(/\[([^\]]+)\]\(([^)]+)\)/g, '<a href="$2" target="_blank">$1</a>');
  // 相邻列表行合并成列表
  src = src.replace(/(<ul-li>[\s\S]*?<\/ul-li>)(?!\s*<ul-li>)/g, (m) => `<ul>${m.replace(/<\/?ul-li>/g, (t) => (t === '<ul-li>' ? '<li>' : '</li>'))}</ul>`);
  src = src.replace(/(<ol-li>[\s\S]*?<\/ol-li>)(?!\s*<ol-li>)/g, (m) => `<ol>${m.replace(/<\/?ol-li>/g, (t) => (t === '<ol-li>' ? '<li>' : '</li>'))}</ol>`);
  // 段落：连续非空非标签行包 <p>
  src = src.split(/\n{2,}/).map((seg) => {
    const s = seg.trim();
    if (!s) return '';
    if (/^<(h\d|ul|ol|blockquote|pre|hr)/.test(s) || /^\u0000B\d+\u0000$/.test(s)) return s;
    return `<p>${s.replace(/\n/g, '<br/>')}</p>`;
  }).join('\n');
  return src.replace(/\u0000B(\d+)\u0000/g, (_m, i) => blocks[Number(i)] ?? '');
}

/** 产出物文件类型（图标 + 类型名） */
function fileKind(p: string): { ico: string; kind: string } {
  const ext = (p.split(/[\\/]/).pop() || '').split('.').pop()?.toLowerCase() || '';
  if (ext === 'md' || ext === 'markdown') return { ico: 'MD', kind: 'Markdown' };
  if (ext === 'html' || ext === 'htm') return { ico: '◆', kind: '网页' };
  if (['png', 'jpg', 'jpeg', 'gif', 'webp', 'svg'].includes(ext)) return { ico: '▣', kind: '图片' };
  return { ico: '▤', kind: '文件' };
}

// 1) 对话内搜索：按关键词过滤当前会话消息，点击跳到那条
const searchQ = ref('');
const searchHitIdx = ref<number | null>(null);
const searchHits = computed(() => {
  const q = searchQ.value.trim().toLowerCase();
  if (!q) return [];
  return chatHistory.value
    .map((m, i) => ({ i, role: m.role, content: m.content }))
    .filter((m) => m.content.toLowerCase().includes(q))
    .slice(0, 50);
});
function jumpToMsg(i: number) {
  searchHitIdx.value = i;
  document.getElementById(`chat-msg-${i}`)?.scrollIntoView({ behavior: 'smooth', block: 'center' });
}

// 2) 分享：会话导出为 Markdown（复制到剪贴板 / 下载 .md）
function buildSessionMd(messages: { role: string; content: string }[], sessionId: string): string {
  const lines = [`# 会话导出 · ${sessionId}`, ''];
  for (const m of messages) {
    lines.push(m.role === 'user' ? '## 用户' : '## 小满', '', m.content, '');
  }
  return lines.join('\n');
}
async function copySessionMd() {
  err.value = null;
  if (!currentSessionId.value) {
    err.value = t('当前没有绑定的会话，无法分享');
    return;
  }
  try {
    const r = await getSessionMessages(currentSessionId.value);
    const md = buildSessionMd(r.messages || [], currentSessionId.value);
    await navigator.clipboard.writeText(md);
    notice.value = t('会话全文已复制到剪贴板');
    pop.value = null;
  } catch (e) {
    err.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  }
}
async function downloadSessionMd() {
  err.value = null;
  if (!currentSessionId.value) {
    err.value = t('当前没有绑定的会话，无法分享');
    return;
  }
  try {
    const r = await getSessionMessages(currentSessionId.value);
    const md = buildSessionMd(r.messages || [], currentSessionId.value);
    const blob = new Blob([md], { type: 'text/markdown;charset=utf-8' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `xiaoman-session-${currentSessionId.value.slice(0, 8)}.md`;
    a.click();
    URL.revokeObjectURL(a.href);
    pop.value = null;
  } catch (e) {
    err.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  }
}

// 3) 历史提问：本会话里用户问过的每一句，点击回填输入框
const userQuestions = computed(() =>
  chatHistory.value
    .map((m, i) => ({ i, role: m.role, content: m.content }))
    .filter((m) => m.role === 'user')
    .slice(-50)
);
function reuseQuestion(content: string) {
  task.value = content;
  pop.value = null;
  autoGrow();
}

// 4) 任务概览：当前/最近一次任务的执行摘要（状态/轮数/token/步骤）
const jobOverview = computed(() => {
  if (!job.value || job.value.state === 'idle') return null;
  const steps = job.value.steps || [];
  const toolSteps = steps.filter((s) => s.type === 'tool');
  return {
    state: job.value.state,
    task: job.value.task,
    rounds: job.value.stats?.rounds ?? steps.filter((s) => s.type === 'model').length,
    promptTokens: job.value.stats?.promptTokens ?? 0,
    completionTokens: job.value.stats?.completionTokens ?? 0,
    toolCalls: toolSteps.length,
    toolOk: toolSteps.filter((s) => s.ok).length,
    toolsUsed: [...new Set(toolSteps.map((s) => s.name).filter(Boolean))] as string[],
    error: job.value.error,
    recent: steps.slice(-8).reverse(),
  };
});

// ── 工作过程（2026-10-09 用户：WorkBuddy 式，行内暴露读了/编辑了/搜了什么、思考了什么）──
const wpOpen = ref(true);

/** 本轮思考全文（合并所有轮次的 thinking，去掉重复的实时思考）——展示在正文上方 */
const thinkingFull = computed(() => {
  const parts: string[] = [];
  for (const s of job.value?.steps || []) {
    if (s.type === 'model' && s.thinking) parts.push(s.thinking);
  }
  return parts.join('\n\n');
});

/** job 是否属于当前会话（2026-10-10 修会话串台）：job 是后端全局单例（最近一次任务的），
 *  切到别的会话后，旧任务的思考流/结果/运行状态绝不能渲染进当前会话视图。
 *  job.sessionId 为 null（未绑定会话的任务）只在当前也无会话时匹配。 */
const jobInSession = computed(() => {
  const j = job.value;
  if (!j) return false;
  return (j.sessionId ?? null) === (currentSessionId.value ?? null);
});

/** 是否处于「本轮已完成、正文已写回」状态——此时才把思考段落插到正文上方 */
const answerReady = computed(() => jobInSession.value && job.value?.state === 'done' && !!thinkingFull.value);

/** 历史消息：任务完成后，去掉最后一条 assistant（本轮正文），正文放到思考段落之后 */
const historyBeforeAnswer = computed<SessionMessage[]>(() => {
  const msgs = chatHistory.value;
  if (!answerReady.value) return msgs; // 未完成或无思考 → 原样显示完整历史
  let lastAssistant = -1;
  for (let i = msgs.length - 1; i >= 0; i--) {
    if (msgs[i].role === 'assistant') { lastAssistant = i; break; }
  }
  return lastAssistant >= 0 ? msgs.slice(0, lastAssistant) : msgs;
});
const lastAssistantMsg = computed<SessionMessage | null>(() => {
  const msgs = chatHistory.value;
  if (!answerReady.value) return null; // 未完成时不单独抽正文
  for (let i = msgs.length - 1; i >= 0; i--) {
    if (msgs[i].role === 'assistant') return msgs[i];
  }
  return null;
});

/** 工具名 → 动作词（照 WorkBuddy：读取/编辑/搜索网页…） */
const TOOL_ACTIONS: Record<string, string> = {
  bash: '运行命令',
  bash_persistent: '运行命令',
  read_file: '读取',
  write_file: '写入',
  edit_file: '编辑',
  glob: '搜索文件',
  web_search: '搜索网页',
  web_fetch: '抓取网页',
  todo_write: '更新待办',
  ask_user: '向用户提问',
  pwsh: '运行 PowerShell',
  skill_list: '查看技能',
  workbench_load_skill: '加载技能',
  workbench_present_file: '产出文件',
};

function toolTarget(name: string, args: unknown): string {
  const a = (args || {}) as Record<string, unknown>;
  const pick = [a.path, a.query, a.url, a.pattern, a.skill, a.name].find(
    (v) => typeof v === 'string' && v.trim()
  ) as string | undefined;
  if (pick) return pick;
  if (typeof a.command === 'string' && a.command.trim()) return a.command.slice(0, 60);
  return '';
}

interface WpRow {
  kind: 'think' | 'tool' | 'verify';
  title: string;
  target?: string;
  detail?: string;
  ms?: number;
  ok?: boolean;
}

const workRows = computed<WpRow[]>(() => {
  const rows: WpRow[] = [];
  for (const s of job.value?.steps || []) {
    if (s.type === 'model') {
      // 2026-10-10：思考已上移到正文上方独立展示（think-sec），这里不再重复
      continue;
    }
    if (s.type === 'tool') {
      const action = TOOL_ACTIONS[s.name || ''] ? t(TOOL_ACTIONS[s.name || '']) : `${t('调用')} ${s.name}`;
      rows.push({ kind: 'tool', title: action, target: toolTarget(s.name || '', s.args), detail: s.result, ms: s.ms, ok: s.ok });
      continue;
    }
    if (s.type === 'verify') {
      rows.push({ kind: 'verify', title: t('程序化核查'), detail: s.issues?.length ? s.issues.join('；') : undefined, ms: s.ms, ok: !s.issues?.length });
    }
  }
  return rows;
});

const wpDurText = computed(() => {
  if (!job.value || job.value.state === 'idle') return '';
  const secs = (job.value.stats?.modelMs || 0) / 1000;
  return secs > 0 ? `${secs.toFixed(0)}s` : '';
});

const wpExpanded = ref<Set<number>>(new Set());
function toggleWpRow(i: number) {
  const next = new Set(wpExpanded.value);
  if (next.has(i)) next.delete(i);
  else next.add(i);
  wpExpanded.value = next;
}


// ── 会话保留（2026-10-07 晚，参考 WorkBuddy/DSH 会话机制）──────────
// 开关开：历史进上下文并持久化，可查询可切换；开关关：一句一句独立（不留存）。
// 新任务 = 「新建会话」（抽屉内），无会话时发送会由后端自动创建并绑定场景。

const sessionList = ref<SessionSummary[]>([]);
const viewingSessionId = ref<string | null>(null);
const viewingMessages = ref<SessionMessage[]>([]);
const usage = ref<ContextUsageResponse | null>(null);

/** 会话抽屉分组：活跃 / 已归档（归档不删数据，可查可恢复） */
const activeSessions = computed(() => sessionList.value.filter((s) => !s.archived));
const archivedSessions = computed(() => sessionList.value.filter((s) => s.archived));

async function toggleArchive(s: SessionSummary) {
  try {
    await archiveSession(s.sessionId, !s.archived);
    await loadSessions();
    bumpSessions(); // 同步刷新侧栏「最近会话」工具栏（归档的会话应即时消失）
  } catch {
    /* 后端离线 */
  }
}

async function loadSessions() {
  try {
    const r = await listSessions();
    sessionList.value = r.sessions || [];
  } catch {
    sessionList.value = [];
  }
}

/** 会话标题（左上角，同 WorkBuddy）：当前会话的名字；没有会话时显示「新对话」 */
const chatTitle = computed(() => {
  const s = sessionList.value.find((x) => x.sessionId === currentSessionId.value);
  return s?.sessionTitle || pendingTask.value || t('新对话');
});

/** 新任务 → 新会话：创建并绑定当前场景，后续消息都进这个会话；回到启动台 */
async function newSession() {
  try {
    // 标题不传死值：发首条任务时由后端自动提取关键词命名（autoTitle）
    const r = await createSession({ pinTarget: currentScene.value?.id });
    await rebindSceneSession(currentScene.value?.id ?? '', r.session.sessionId);
    viewingSessionId.value = r.session.sessionId;
    viewingMessages.value = [];
    chatMode.value = false; // 新会话无消息，回 Hero 启动台
    chatHistory.value = [];
    pendingTask.value = null;
    await loadSessions();
  } catch {
    /* 后端离线 */
  }
}

/** 切换到某个历史会话（下一条消息即进它），并进入对话模式续接 */
async function switchToSession(id: string) {
  await rebindSceneSession(currentScene.value?.id ?? '', id);
  chatMode.value = true;
  pendingTask.value = null;
  await loadChatHistory();
  scrollChat();
}

/** 查询某个会话内发送过的消息（只读） */
async function viewSession(id: string) {
  viewingSessionId.value = id;
  try {
    const r = await getSessionMessages(id);
    viewingMessages.value = r.messages || [];
  } catch {
    viewingMessages.value = [];
  }
}

watch([selectedSkills, loadUeTools, limitToolText, noTools], () => { if (pop.value === 'usage') void loadUsage(); }, {deep: true});
async function loadUsage() {
  try {
    usage.value = await getContextUsage(currentSessionId.value, selectedSkills.value, {ue: effectiveUe()}, selectedToolNames());
  } catch {
    usage.value = null;
  }
}

// ── 对话模式（2026-10-07 晚，WorkBuddy 式：发送后切消息流 + 底部输入栏续接）──
// chatFlow = 会话历史气泡 + 本轮任务气泡 + 执行轨迹；box 复用为底部输入栏（弹层全共享）。

const chatMode = ref(false);
// 对话模式同步到全局 store：App.vue 据此隐藏大页头（会话内容顶到面包屑白线下）
watch(chatMode, (v) => setAgentChatMode(v), { immediate: true });
const chatHistory = ref<SessionMessage[]>([]);
const pendingTask = ref<string | null>(null); // 本轮任务（完成写回历史前先本地显示）
const chatMsgs = ref<HTMLElement | null>(null);

async function loadChatHistory() {
  if (!currentSessionId.value) {
    chatHistory.value = [];
    return;
  }
  try {
    const r = await getSessionMessages(currentSessionId.value);
    chatHistory.value = r.messages || [];
  } catch {
    chatHistory.value = [];
  }
}

function scrollChat(smooth = true) {
  void nextTick(() => chatMsgs.value?.scrollTo({ top: chatMsgs.value.scrollHeight, behavior: smooth ? 'smooth' : 'auto' }));
}

/** 用户滚轮向上 → 标记主动上滚（停止自动跟随）；滚回底部附近（距底 <40px）→ 恢复跟随 */
function onChatWheel(e: WheelEvent) {
  if (e.deltaY < 0) userScrolledUp.value = true;
  else {
    const el = chatMsgs.value;
    if (el && el.scrollHeight - el.scrollTop - el.clientHeight < 40) userScrolledUp.value = false;
  }
}

function fmtTok(n: number): string {
  return n >= 1000 ? `${(n / 1000).toFixed(1)}K` : String(n);
}

const USAGE_COLORS: Record<string, string> = {
  system: '#a78bfa',
  tools: '#34d399',
  messages: '#fbbf24',
  mcp: '#60a5fa',
  skills: '#f472b6',
  memory: '#22d3ee',
};

/** 分类条形宽度按最大类归一（对标 UEBox 截图） */
const usageMaxTok = computed(() => Math.max(1, ...(usage.value?.parts.map((p) => p.tokens) ?? [1])));

// 思考强度档位（DSH 图3 七档的本地化，真实映射 thinkingBudget；-1 = Default 不强制）
const THINK_LEVELS = [
  { name: 'Default', hint: '跟随引擎默认 · 不强制思考预算', b: -1 },
  { name: '关', hint: '不推理，直接答 · 最快', b: 0 },
  { name: '低', hint: '思考 200 tokens · 简单问答', b: 200 },
  { name: '中', hint: '思考 800 tokens · 日常任务', b: 800 },
  { name: '高', hint: '思考 1600 tokens · 多步工具链', b: 1600 },
  { name: '超高', hint: '思考 3200 tokens · 复杂写图/长链路', b: 3200 },
  { name: 'Max', hint: '思考 6400 tokens · 全力推理，最慢', b: 6400 },
] as const;
function thinkLevelOf(budget: number): string {
  if (budget < 0) return 'Default';
  if (budget <= 0) return '关';
  if (budget <= 400) return '低';
  if (budget <= 1200) return '中';
  if (budget <= 2400) return '高';
  if (budget <= 4800) return '超高';
  return 'Max';
}
const thinkLevel = computed(() => thinkLevelOf(agentCfg.value?.thinkingBudget ?? 800));
const currentMode = computed(() => {
  const t = agentCfg.value?.temperature;
  const b = agentCfg.value?.thinkingBudget;
  if (t === 0.2 && b === 2000) return '严谨模式';
  if (t === 0.7 && b === 1600) return '创意模式';
  return '平衡模式';
});

async function patchCfg(patch: { temperature?: number; thinkingBudget?: number; allowFullAccess?: boolean; retainSessions?: boolean }) {
  try {
    const r = await putAgentConfig(patch);
    if (r.config) {
      agentCfg.value = {
        temperature: r.config.temperature,
        thinkingBudget: r.config.thinkingBudget,
        allowFullAccess: r.config.allowFullAccess,
        retainSessions: r.config.retainSessions,
      };
    }
  } catch {
    /* 后端离线：开关回落（不假装成功） */
  }
}

async function pickProvider(id: string) {
  try {
    await activateProvider(id);
    const r = await listProviders();
    providerList.value = r.providers || [];
    activeProviderId.value = r.active;
  } catch {
    /* 后端离线 */
  }
}

// ── 附件（2026-10-08 用户：添加文件要真内容，不是只塞文件名）──
interface Attachment {
  name: string;
  path: string | null;
  size: number;
  mime: string;
  text?: string | null;
  base64?: string | null;
  kind: 'image' | 'video' | 'audio' | 'doc' | 'skill' | 'file';
}
const attachments = ref<Attachment[]>([]);
const IMAGE_EXT = /\.(png|jpe?g|webp|gif|bmp)$/i;
const AUDIO_EXT = /\.(wav|mp3|m4a|ogg|flac|aac)$/i;
const VIDEO_EXT = /\.(mp4|mkv|avi|mov|webm|wmv|flv)$/i;
const TEXT_EXT =
  /\.(md|markdown|txt|log|json|jsonc|csv|tsv|ya?ml|toml|ini|cfg|env|js|mjs|cjs|ts|tsx|jsx|vue|py|pyi|cpp|cc|cxx|c|h|hpp|hh|inl|cs|java|kt|go|rs|rb|php|sh|bat|ps1|sql|html?|css|scss|less|xml|svg|cmake|gradle|properties|proto|graphql|prisma)$/i;

function kindOfName(name: string): Attachment['kind'] {
  if (/^skill\.md$/i.test(name.split(/[\\/]/).pop() || '')) return 'skill';
  if (IMAGE_EXT.test(name)) return 'image';
  if (AUDIO_EXT.test(name)) return 'audio';
  if (VIDEO_EXT.test(name)) return 'video';
  if (TEXT_EXT.test(name)) return 'doc';
  return 'file';
}
const kindIcon: Record<Attachment['kind'], string> = { image: '🖼', video: '🎬', audio: '🎵', doc: '📄', skill: '🧩', file: '📎' };

function fmtSize(n: number): string {
  if (!n) return '';
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / 1024 / 1024).toFixed(1)} MB`;
}

/** 纯浏览器（无 path）时前端读内容兜底；Electron 只给 path，后端读盘 */
function readContent(f: File): Promise<Partial<Attachment>> {
  const kind = kindOfName(f.name);
  if (kind === 'image') {
    return new Promise((res) => {
      const r = new FileReader();
      r.onload = () => res({ base64: String(r.result || ''), mime: f.type });
      r.onerror = () => res({});
      r.readAsDataURL(f);
    });
  }
  if (kind !== 'audio' && kind !== 'video') {
    return new Promise((res) => {
      const r = new FileReader();
      r.onload = () => res({ text: String(r.result || '') });
      r.onerror = () => res({});
      r.readAsText(f);
    });
  }
  return Promise.resolve({});
}

async function addFiles(files: FileList | File[]) {
  for (const f of Array.from(files)) {
    if (attachments.value.length >= 10) {
      err.value = '附件最多 10 个，先删掉一些再加';
      break;
    }
    const anyF = f as unknown as { path?: string };
    const att: Attachment = {
      name: f.name,
      path: anyF.path || null, // Electron 才有绝对路径
      size: f.size,
      mime: f.type,
      kind: kindOfName(f.name),
    };
    if (!att.path) Object.assign(att, await readContent(f)); // 浏览器兜底：直接带内容
    attachments.value.push(att);
  }
}

function onFiles(e: Event) {
  const el = e.target as HTMLInputElement;
  if (!el.files?.length) return;
  void addFiles(el.files);
  el.value = '';
  pop.value = null;
  inputBox.value?.focus();
}

function removeAttachment(i: number) {
  attachments.value.splice(i, 1);
}

function onDrop(e: DragEvent) {
  if (!e.dataTransfer?.files?.length) return;
  void addFiles(e.dataTransfer.files);
}

/** 回车发送（2026-10-08 用户）：带 IME 保护——中文输入法组词中的回车是选词，不发送 */
function onEnterKey(e: KeyboardEvent) {
  if (e.isComposing || e.keyCode === 229) return; // 输入法组词中：交给输入法
  if (e.shiftKey) return; // Shift+Enter：换行（走浏览器默认）
  e.preventDefault();
  void send();
}

function goManage() {
  pop.value = null;
  switchFunction('plugins');
}

function goConfigureModel() {
  pop.value = null;
  switchFunction('model');
}

// ── 预设选择器（2026-10-08 用户：极简下拉，替代两行按钮矩阵）──
// 点下拉项 = 切预设（= 切会话域 + 专属提示词，注入在后端 buildPromptParts）。
function presetsOfCat(catId: string) {
  return presetList.value.filter((p) => p.category === catId);
}

function pickPreset(id: string): void {
  switchScene(id); // store 新语义：切预设 = 回到该模式下的全新对话栏（不跳旧会话）
}

// ── 工作区（2026-10-08 用户）：本机工作目录清单 + 活跃工作区 ──
const wsList = ref<WorkspaceInfo[]>([]);
const wsActive = ref<string | null>(null);
const wsOpen = ref(false);
const presetOpen = ref(false);

const wsName = computed(() => {
  if (!wsActive.value) return '选择工作区';
  const hit = wsList.value.find((w) => w.path === wsActive.value);
  return hit?.name ?? wsActive.value.split(/[\\/]/).filter(Boolean).pop() ?? wsActive.value;
});

async function loadWorkspaces() {
  try {
    const r = await listWorkspaces();
    wsList.value = r.list;
    wsActive.value = r.active;
  } catch {
    /* 后端离线：下拉显示「选择工作区」，添加时再报错 */
  }
}

async function pickWorkspace(p: string) {
  try {
    const r = await setActiveWorkspace(p);
    wsList.value = r.list;
    wsActive.value = r.active;
  } catch {
    /* 保持原状 */
  }
  wsOpen.value = false;
}

/** 添加工作区：走 Electron 原生目录选择框（浏览器环境无桥，诚实降级） */
async function addWorkspaceViaDialog() {
  const bridge = window.xiaomanBox;
  if (!bridge?.selectDirectory) {
    wsOpen.value = false;
    err.value = '添加工作区需要在 XiaoMan Box 桌面端里使用（浏览器里没有目录选择能力）';
    return;
  }
  try {
    const r = await bridge.selectDirectory({ title: '选择工作区目录' });
    if (!r.canceled && r.dir) {
      const res = await addWorkspace(r.dir);
      wsList.value = res.list;
      wsActive.value = res.active;
    }
  } catch (e) {
    err.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  }
  wsOpen.value = false;
}

// ── 执行轨迹展开态（DSH 图7）───────────────────────────────
const openSys = ref(false);   // 系统提示词折叠
const openCtx = ref(false);   // 上下文注入折叠
const openThink = ref<Set<number>>(new Set()); // 已思考折叠（按 step 序号）

function toggleThink(i: number) {
  const s = new Set(openThink.value);
  if (s.has(i)) s.delete(i);
  else s.add(i);
  openThink.value = s;
}

// 会话统计（DSH 图5/6）：底部统计条 + Token 明细
const jobStats = computed(() => {
  if (!job.value?.stats) return null;
  const s = job.value.stats;
  const total = s.promptTokens + s.completionTokens;
  return {
    rounds: s.rounds,
    steps: job.value.steps?.length ?? 0,
    tps: s.tps ?? 0,
    totalTok: total,
    totalK: total >= 1000 ? (total / 1000).toFixed(1) + 'K' : String(total),
    hitRate: s.promptTokens > 0 ? ((s.cachedTokens / s.promptTokens) * 100).toFixed(2) + '%' : '—',
    promptTokens: s.promptTokens,
    completionTokens: s.completionTokens,
    cachedTokens: s.cachedTokens,
    modelMs: s.modelMs,
  };
});

const filteredCommands = computed(() => {
  const q = cmdQuery.value.trim().toLowerCase();
  if (!q) return commands.value;
  return commands.value.filter((c) => c.label.toLowerCase().includes(q) || c.hint.toLowerCase().includes(q));
});
const filteredCtxs = computed(() => {
  const q = ctxQuery.value.trim().toLowerCase();
  if (!q) return ctxs.value;
  return ctxs.value.filter((c) => c.label.toLowerCase().includes(q) || c.summary.toLowerCase().includes(q));
});

function shortId(id: string | null): string {
  return id ? (id.length > 8 ? id.slice(0, 8) : id) : '未绑定';
}

// ── 输入区 @ 与 / 的即时触发 ───────────────────────────────
function onInput() {
  autoGrow();
  const v = task.value;
  // 检测最近一次输入的触发符。触发符必须是「行首或前面是空白」，
  // 避免把邮箱里的 @ 或路径里的 / 误判成指令/上下文。
  const atIdx = v.lastIndexOf('@');
  const slashIdx = v.lastIndexOf('/');
  const last = Math.max(atIdx, slashIdx);
  if (last < 0) {
    menuOpen.value = null;
    return;
  }
  const c = v[last];
  const before = v[last - 1];
  const after = v.slice(last + 1);
  const isTrigger = (before === undefined || /\s/.test(before)) && !after.includes(' ');
  if (c === '@' && isTrigger) {
    menuOpen.value = 'at';
    ctxQuery.value = after;
  } else if (c === '/' && isTrigger) {
    menuOpen.value = 'slash';
    cmdQuery.value = after;
  } else {
    menuOpen.value = null;
  }
}

function pickCommand(c: Command) {
  menuOpen.value = null;
  cmdQuery.value = '';
  // 会话指令：直接执行动作（导出/开权限面板/开模型菜单）
  if (c.action) {
    c.action();
    return;
  }
  // 把触发符到光标处替换成指令的 fill 文本
  const v = task.value;
  const idx = v.lastIndexOf('/');
  const prefix = idx >= 0 ? v.slice(0, idx).trimEnd() : v.trimEnd();
  task.value = prefix ? prefix + ' ' + c.fill : c.fill;
  inputBox.value?.focus();
}

function pickCtx(c: Ctx) {
  menuOpen.value = null;
  ctxQuery.value = '';
  const v = task.value;
  const idx = v.lastIndexOf('@');
  const tag = `[@${c.label}: ${c.summary}]`;
  const prefix = idx >= 0 ? v.slice(0, idx).trimEnd() : v.trimEnd();
  task.value = prefix ? prefix + ' ' + tag + ' ' : tag + ' ';
  inputBox.value?.focus();
}

function closeMenus() {
  menuOpen.value = null;
}

// ── 发送 ──────────────────────────────────────────────────
/** 用户滚轮向上 → 标记主动上滚（停止自动跟随底部，防止思考流把用户拉回去） */
const userScrolledUp = ref(false);

// ── 消息编辑重发（2026-10-10 用户，对标 WorkBuddy「编辑」）：停止/失败后最后一条
//    用户消息可编辑，发送后作为同一会话的续接任务重新开始，已有产出物不受影响 ──
const editingIdx = ref(-1);
const editText = ref('');
/** 是否可编辑第 i 条：它是会话最后一条且是用户消息，且任务不在跑（正是停止/失败后的场景） */
function canEditMsg(i: number): boolean {
  const msgs = chatHistory.value;
  return job.value?.state !== 'running' && i === msgs.length - 1 && msgs[i]?.role === 'user';
}
function startEdit(i: number) {
  editingIdx.value = i;
  editText.value = chatHistory.value[i]?.content ?? '';
  nextTick(() => scrollChat(false));
}
function cancelEdit() {
  editingIdx.value = -1;
  editText.value = '';
}
async function sendEdited() {
  const t = editText.value.trim();
  if (!t || job.value?.state === 'running' || starting.value) return;
  editingIdx.value = -1;
  pendingTask.value = t;
  userScrolledUp.value = false;
  try {
    await startAgentTask(t, currentSessionId.value ?? undefined);
    await refresh();
    schedulePoll();
    scrollChat();
  } catch (e) {
    err.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  }
}

async function refresh() {
  try {
    const prevLen = job.value?.liveThinking?.length ?? 0;
    job.value = await getAgentJob();
    err.value = null;
    // 实时思考流：running 且思考在增长时跟随底部——但用户主动上滚后停止拉扯（滚回底部附近自动恢复跟随）
    const newLen = job.value?.liveThinking?.length ?? 0;
    if (job.value?.state === 'running' && newLen > prevLen && !userScrolledUp.value) {
      void nextTick(() => scrollChat(false));
    }
  } catch (e) {
    err.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  }
}

function schedulePoll() {
  window.clearTimeout(pollTimer);
  if (job.value?.state === 'running') {
    pollTimer = window.setTimeout(async () => {
      const before = job.value?.state;
      await refresh();
      // 本轮结束：后端已把 user/assistant 写进会话历史，收进来并清掉本地「本轮」气泡
      if (before === 'running' && job.value?.state !== 'running') {
        pendingTask.value = null;
        await loadChatHistory();
        void refreshPresented(); // 任务完成即刷新产出物栏（此前只在切会话时刷新，产出物栏一直空）
        scrollChat();
      }
      schedulePoll();
    }, 500); // 2026-10-10 暴露思考过程：running 时 500ms 快轮询，实时思考流更流畅
  }
}

async function send() {
  const hasAttach = attachments.value.length > 0;
  if ((!task.value.trim() && !hasAttach) || starting.value || job.value?.state === 'running') return;
  const t = task.value.trim() || '（见附件）';
  starting.value = true;
  try {
    // 新对话栏（唤起 Agent 后还没建会话）：首条消息落地时才创建会话并绑场景 → 正式并入侧栏工具栏
    if (!currentSessionId.value) {
      // 标题不传死值：后端从任务文本自动提取关键词命名（autoTitle，实时更新）
      const r = await createSession({ pinTarget: currentScene.value?.id });
      await settleNewChat(currentScene.value?.id ?? '', r.session.sessionId);
    }
    await startAgentTask(t, currentSessionId.value ?? undefined, hasAttach ? attachments.value : undefined, selectedSkills.value, loadUeTools.value === null ? undefined : { ue: loadUeTools.value }, selectedToolNames());
    bumpSessions(); // 标题已在 startAgentTask 内自动更新 → 侧栏立即刷新（不等轮询）
    pendingTask.value = t;
    task.value = '';
    attachments.value = []; // 附件已随任务发出，清空待下一轮
    chatMode.value = true; // 进入对话模式：消息流 + 底部输入栏
    userScrolledUp.value = false; // 新任务开始，恢复自动跟随底部
    await refresh();
    schedulePoll();
    scrollChat();
  } catch (e) {
    err.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  } finally {
    starting.value = false;
  }
}

const presentedFiles = ref<PresentedFile[]>([]);
async function refreshPresented() {
  try {
    // 防串台：只显示当前会话登记的产出文件（新对话栏无会话 → 不显示）
    const r = await listPresented(currentSessionId.value ?? undefined);
    presentedFiles.value = r.files || [];
  } catch {
    /* 静默 */
  }
}
async function openFile(p: string) {
  try {
    await openPresentedFile(p);
  } catch {
    /* 静默 */
  }
}

onMounted(async () => {
  void loadWorkspaces(); // 工作区清单（失败静默：下拉显示「选择工作区」）
  void refreshPresented(); // 产出文件（小满 present 的文件）
  void loadOutbarPref(); // 右侧产出物栏开/关（ui.json 持久化）
  try {
    const st = await getUalStatus();
    ualConnected.value = st.connectionCount > 0;
    ualToolCount.value = st.toolCount ?? 0;
  } catch {
    ualConnected.value = null;
  }
  // 斜杠指令：skill 清单（真实）
  try {
    const r = await listSkills();
    skillCommands.value = (r.skills || []).map((s: SkillMeta) => ({
      key: `skill:${s.name}`,
      label: `${t('技能')} ${s.name}`,
      hint: s.titleZh || s.description || '加载该 skill 的详细步骤',
      fill: `用「${s.name}」这个技能来：`,
      kind: 'skill' as const,
    }));
  } catch {
    /* 后端离线或 skills 目录不存在 */
  }
  // @ 上下文：全局记忆 + 在线成员（真实）
  try {
    const m = await getGlobalMemory();
    memoryKeys.value = Object.keys(m.memory ?? {});
  } catch {
    /* 忽略 */
  }

  // 工具条：providers + agent config + MCP 服务器（真实）
  try {
    const r = await listProviders();
    providerList.value = r.providers || [];
    activeProviderId.value = r.active;
  } catch {
    /* 后端离线 */
  }
  try {
    const r = await getAgentConfig();
    if (r.config) {
      agentCfg.value = {
        temperature: r.config.temperature,
        thinkingBudget: r.config.thinkingBudget,
        allowFullAccess: r.config.allowFullAccess,
        retainSessions: r.config.retainSessions ?? true,
      };
    }
  } catch {
    /* 后端离线 */
  }
  try {
    const r = await listMcpServers();
    mcpServers.value = r.servers || [];
  } catch {
    /* 后端离线 */
  }
  await refresh();
  // 新对话栏（唤起 Agent 后未发送）：保持空 Hero 启动台，不续接上一个任务的轨迹
  if (newChatPending.value) {
    chatMode.value = false;
    chatHistory.value = [];
    pendingTask.value = null;
  }
  // 刷新后如果上一个任务还有轨迹（或正在跑），直接回到对话模式续接
  else if (job.value?.steps?.length || job.value?.state === 'running') {
    chatMode.value = true;
    if (job.value?.state === 'running') pendingTask.value = job.value.task ?? null;
    await loadChatHistory();
    scrollChat();
  }
  schedulePoll();
  // Ctrl+F：对话内搜索（同 WorkBuddy；输入框聚焦时不抢，仅当不在输入态时打开）
  window.addEventListener('keydown', onPanelKey);
});
function onPanelKey(e: KeyboardEvent) {
  if ((e.ctrlKey || e.metaKey) && (e.key === 'f' || e.key === 'F')) {
    const el = document.activeElement;
    const typing = el instanceof HTMLInputElement || el instanceof HTMLTextAreaElement;
    if (!typing && chatMode.value) {
      e.preventDefault();
      togglePop('search');
    }
  }
  // Ctrl+Shift+B：开/关右侧产出物栏（同 WorkBuddy）
  if ((e.ctrlKey || e.metaKey) && e.shiftKey && (e.key === 'b' || e.key === 'B')) {
    e.preventDefault();
    toggleOutbar();
  }
}
onBeforeUnmount(() => {
  window.clearTimeout(pollTimer);
  window.removeEventListener('mousedown', onGlobalDown);
  window.removeEventListener('keydown', onPanelKey);
  abortRecording(); // 录音中切页/卸载：停流防麦克风挂住
  setAgentChatMode(false); // 切走面板恢复大页头（对话模式标志不残留）
});

// ── 语音输入（2026-10-08 用户：麦克风真功能，本地 SenseVoice ASR 纯离线）──
// 链路：getUserMedia(16k 单声道) → ScriptProcessor 攒 PCM → 停止编码 WAV → POST /agent/asr → 文本填输入框

/** 输入框自动长高（2026-10-08 用户）：手动拖 resize 手柄会叠字，改为内容驱动——
    height:auto 回零重算 scrollHeight，夹在 64~240 之间；语音填入/预设回填也走 watch(task) 触发 */
function autoGrow() {
  const el = inputBox.value;
  if (!el) return;
  el.style.height = 'auto';
  el.style.height = `${Math.min(Math.max(el.scrollHeight, 64), 240)}px`;
}
type MicState = 'idle' | 'rec' | 'busy';
const micState = ref<MicState>('idle');
const micSecs = ref(0);
let micTimer: number | null = null;
let audioCtx: AudioContext | null = null;
let mediaStream: MediaStream | null = null;
let micNode: ScriptProcessorNode | null = null;
let micSource: MediaStreamAudioSourceNode | null = null;
let pcmChunks: Float32Array[] = [];

const micTitle = computed(() =>
  micState.value === 'rec'
    ? `录音中 ${micSecs.value}s · 点击停止并转写`
    : micState.value === 'busy'
      ? '本地转写中…'
      : '语音输入（本地离线识别）'
);

/** Float32 PCM → 16bit 单声道 WAV Blob（前端编码，后端零额外依赖） */
function encodeWav(samples: Float32Array, sampleRate: number): Blob {
  const buf = new ArrayBuffer(44 + samples.length * 2);
  const v = new DataView(buf);
  const ws = (o: number, s: string) => {
    for (let i = 0; i < s.length; i++) v.setUint8(o + i, s.charCodeAt(i));
  };
  ws(0, 'RIFF');
  v.setUint32(4, 36 + samples.length * 2, true);
  ws(8, 'WAVE');
  ws(12, 'fmt ');
  v.setUint32(16, 16, true);
  v.setUint16(20, 1, true); // PCM
  v.setUint16(22, 1, true); // 单声道
  v.setUint32(24, sampleRate, true);
  v.setUint32(28, sampleRate * 2, true);
  v.setUint16(32, 2, true);
  v.setUint16(34, 16, true);
  ws(36, 'data');
  v.setUint32(40, samples.length * 2, true);
  let o = 44;
  for (let i = 0; i < samples.length; i++, o += 2) {
    const s = Math.max(-1, Math.min(1, samples[i]));
    v.setInt16(o, s < 0 ? s * 0x8000 : s * 0x7fff, true);
  }
  return new Blob([buf], { type: 'audio/wav' });
}

async function toggleMic() {
  if (micState.value === 'busy') return;
  if (micState.value === 'rec') {
    stopRecording();
    return;
  }
  try {
    mediaStream = await navigator.mediaDevices.getUserMedia({
      audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true },
    });
  } catch {
    err.value = '麦克风不可用：请在系统/应用设置里允许 XiaoMan Box 使用麦克风';
    return;
  }
  try {
    audioCtx = new AudioContext({ sampleRate: 16000 });
    micSource = audioCtx.createMediaStreamSource(mediaStream);
    micNode = audioCtx.createScriptProcessor(4096, 1, 1);
    pcmChunks = [];
    micNode.onaudioprocess = (e) => {
      pcmChunks.push(new Float32Array(e.inputBuffer.getChannelData(0)));
    };
    micSource.connect(micNode);
    // ScriptProcessor 必须连到 destination 才会被驱动；经零增益防外放回声
    const silent = audioCtx.createGain();
    silent.gain.value = 0;
    micNode.connect(silent);
    silent.connect(audioCtx.destination);
  } catch {
    abortRecording();
    err.value = '音频采集初始化失败';
    return;
  }
  micState.value = 'rec';
  micSecs.value = 0;
  micTimer = window.setInterval(() => {
    micSecs.value++;
  }, 1000);
}

function teardownAudio() {
  if (micTimer) {
    window.clearInterval(micTimer);
    micTimer = null;
  }
  try {
    micNode?.disconnect();
    micSource?.disconnect();
  } catch {
    /* 已断开 */
  }
  mediaStream?.getTracks().forEach((t) => t.stop());
  const ctx = audioCtx;
  ctx?.close().catch(() => {});
  micNode = null;
  micSource = null;
  mediaStream = null;
  audioCtx = null;
}

function abortRecording() {
  if (micState.value === 'idle') return;
  teardownAudio();
  pcmChunks = [];
  micState.value = 'idle';
}

function stopRecording() {
  if (micState.value !== 'rec') return;
  teardownAudio();
  const chunks = pcmChunks;
  const sampleRate = 16000;
  pcmChunks = [];
  micState.value = 'busy';
  const total = chunks.reduce((a, c) => a + c.length, 0);
  if (!total) {
    micState.value = 'idle';
    err.value = '没有录到声音，再试一次？';
    return;
  }
  const merged = new Float32Array(total);
  let o = 0;
  for (const c of chunks) {
    merged.set(c, o);
    o += c.length;
  }
  if (merged.length < sampleRate * 0.3) {
    micState.value = 'idle';
    err.value = '录音太短（不足 0.3 秒），再试一次？';
    return;
  }
  transcribeVoice(encodeWav(merged, sampleRate))
    .then((j) => {
      const text = String(j.text || '').trim();
      if (text) {
        task.value = task.value.trimEnd() ? `${task.value.trimEnd()} ${text}` : text;
        nextTick(() => inputBox.value?.focus());
      } else {
        err.value = '没听清，靠近一点再试一次？';
      }
    })
    .catch((e) => {
      err.value = `语音转写失败：${e instanceof ContractError ? e.message : String(e)}`;
    })
    .finally(() => {
      micState.value = 'idle';
    });
}

// 侧栏切换会话 / 唤起新对话（SideNav 调 store.switchToSession / openNewChat）都会改 currentSessionId。
//   - 切到某会话：进入对话模式并加载历史（回对话现场续接）。
//   - 置 null（唤起新对话）：清空历史、回 Hero 启动台 —— 空对话栏，等首条消息才建会话。
// task 变化（手输 onInput / 语音填入 / 指令回填 / 清空）都重算输入框高度
watch(task, () => {
  autoGrow();
});

watch(currentSessionId, async (id, prev) => {
  if (id === prev) return;
  if (!id) {
    // 新对话栏：还没建会话，清空现场回启动台
    chatMode.value = false;
    chatHistory.value = [];
    pendingTask.value = null;
    presentedFiles.value = []; // 产出文件跟着会话走：无会话不显示
    return;
  }
  chatMode.value = true;
  pendingTask.value = null;
  userScrolledUp.value = false; // 切会话重置跟随状态
  await loadChatHistory();
  await loadSessions(); // 左上角会话标题 + 工具组展示态的数据源
  void refreshPresented(); // 切会话 → 刷新该会话自己的产出文件
  // 工具组展示态：按当前会话的预设分类算「是否自动带 UE 工具」（用户显式选择优先，见 loadUeTools）
  const s = sessionList.value.find((x) => x.sessionId === id);
  const p = s ? presetList.value.find((x) => x.id === s.pinTarget) : null;
  autoUe.value = p?.category === 'ue5';
  scrollChat();
});

// 点击 box 外关闭工具条弹出（slash/at 仍由输入逻辑与 Esc 管理）
function onGlobalDown(e: MouseEvent) {
  const t = e.target as HTMLElement | null;
  // 工作区 / 预设 下拉：点控件行外即收起
  if (!t?.closest('.ctl')) {
    wsOpen.value = false;
    presetOpen.value = false;
  }
  if (!pop.value) return;
  if (t && !t.closest('.box')) pop.value = null;
}
window.addEventListener('mousedown', onGlobalDown);
</script>

<template>
  <div class="agent" :class="{ chatting: chatMode, 'with-outbar': chatMode && outbarOpen }" :style="chatMode && outbarOpen ? { paddingRight: outbarWidth + 8 + 'px' } : {}">
    <!-- ── 右上角工具条（2026-10-09 用户：四件套 + 右侧栏开关，同 WorkBuddy）── -->
    <div v-if="chatMode" class="corner-bar" :class="{ pushed: outbarOpen }">
      <button class="corner-btn" :class="{ on: pop === 'search' }" :title="t('对话内搜索（Ctrl+F）')" @click="togglePop('search')">
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><circle cx="11" cy="11" r="7" /><path d="M20 20l-4-4" /></svg>
      </button>
      <button class="corner-btn" :class="{ on: pop === 'share' }" :title="t('分享：复制全文 / 下载 Markdown')" @click="togglePop('share')">
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M13 5l7 7-7 7" /><path d="M20 12H8a5 5 0 0 0-5 5v2" /></svg>
      </button>
      <button class="corner-btn" :class="{ on: pop === 'history' }" :title="t('历史提问：本会话问过的每一句')" @click="togglePop('history')">
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><circle cx="12" cy="12" r="9" /><path d="M12 7v5l3 3" /></svg>
      </button>
      <button class="corner-btn" :class="{ on: pop === 'overview' }" :title="t('任务概览：执行摘要')" @click="togglePop('overview')">
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="5" y="3" width="14" height="18" rx="2" /><path d="M9 8h6M9 12h6M9 16h4" /></svg>
      </button>
      <span class="corner-sep"></span>
      <button class="corner-btn" :class="{ on: outbarOpen }" :title="t('打开右侧栏 Ctrl+Shift+B')" @click="toggleOutbar">
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="3" y="4" width="18" height="16" rx="2" /><path d="M15 4v16" /></svg>
      </button>

      <!-- 四件套弹出（锚在右上角工具条下方） -->
      <div v-if="pop === 'search'" class="pop corner" @mousedown.prevent>
        <input v-model="searchQ" class="search-inp" type="text" :placeholder="t('在对话里搜…')" @keyup.enter="searchHits.length && jumpToMsg(searchHits[0].i)" />
        <div class="search-meta">{{ searchQ.trim() ? `${t('命中')} ${searchHits.length} ${t('条')}` : t('输入关键词，Enter 跳第一条') }}</div>
        <div class="search-list">
          <button v-for="h in searchHits" :key="h.i" class="search-hit" @click="jumpToMsg(h.i)">
            <span class="sh-role">{{ h.role === 'user' ? t('用户') : t('小满') }}</span>
            <span class="sh-txt">{{ h.content.slice(0, 80) }}</span>
          </button>
          <div v-if="searchQ.trim() && !searchHits.length" class="pop-empty">{{ t('没有匹配的消息') }}</div>
        </div>
      </div>
      <div v-if="pop === 'share'" class="pop corner" @mousedown.prevent>
        <button class="pop-item" @click="copySessionMd">
          <span class="pi-main"><span class="pi-label">{{ t('复制全文到剪贴板') }}</span><span class="pi-hint">{{ t('Markdown 格式，粘给谁都能看') }}</span></span>
        </button>
        <button class="pop-item" @click="downloadSessionMd">
          <span class="pi-main"><span class="pi-label">{{ t('下载 .md 文件') }}</span><span class="pi-hint">{{ t('存成会话导出文件') }}</span></span>
        </button>
        <div v-if="!currentSessionId" class="pop-empty">{{ t('还没有会话（先发一条消息）') }}</div>
      </div>
      <div v-if="pop === 'history'" class="pop corner" @mousedown.prevent>
        <button v-for="q in userQuestions" :key="q.i" class="pop-item" :title="q.content" @click="reuseQuestion(q.content)">
          <span class="pi-main"><span class="pi-label q-txt">{{ q.content.slice(0, 60) }}{{ q.content.length > 60 ? '…' : '' }}</span></span>
        </button>
        <div v-if="!userQuestions.length" class="pop-empty">{{ t('这个会话还没有提问') }}</div>
      </div>
      <div v-if="pop === 'overview'" class="pop corner overview" @mousedown.prevent>
        <template v-if="jobOverview">
          <div class="ov-head">
            <span class="usage-title">{{ t('任务概览') }}</span>
            <span class="ov-state" :class="jobOverview.state">{{ t(jobOverview.state === 'running' ? '运行中' : jobOverview.state === 'done' ? '已完成' : '失败') }}</span>
          </div>
          <div v-if="jobOverview.task" class="ov-task">{{ jobOverview.task.slice(0, 120) }}</div>
          <div class="ov-rows">
            <span class="ov-row"><i>{{ t('循环轮数') }}</i><b>{{ jobOverview.rounds }}</b></span>
            <span class="ov-row"><i>{{ t('工具调用') }}</i><b>{{ jobOverview.toolOk }}/{{ jobOverview.toolCalls }}</b></span>
            <span class="ov-row"><i>{{ t('输入 token') }}</i><b>{{ jobOverview.promptTokens }}</b></span>
            <span class="ov-row"><i>{{ t('输出 token') }}</i><b>{{ jobOverview.completionTokens }}</b></span>
          </div>
          <div v-if="jobOverview.toolsUsed.length" class="ov-tools">
            <span v-for="n in jobOverview.toolsUsed" :key="n" class="comm-map-tag">{{ n }}</span>
          </div>
          <div v-if="jobOverview.error" class="ov-err">{{ jobOverview.error.slice(0, 200) }}</div>
          <div class="ov-recent">
            <div v-for="(s, idx) in jobOverview.recent" :key="idx" class="ov-step">
              <span class="os-type" :class="s.type">{{ s.type === 'model' ? t('模型') : s.type === 'tool' ? t('工具') : t('核查') }}</span>
              <span class="os-txt">{{ s.type === 'tool' ? s.name : (s.answer || s.thinking || '').slice(0, 60) }}</span>
              <span v-if="s.ms" class="os-ms">{{ (s.ms / 1000).toFixed(1) }}s</span>
            </div>
          </div>
        </template>
        <div v-else class="pop-empty">{{ t('还没有跑过任务') }}</div>
      </div>
    </div>

    <!-- ── 右侧产出物栏（md / 文件 / 网页，随会话；Ctrl+Shift+B 开关）── -->
    <!-- 可拖拽分界线（2026-10-10 用户）：调产出物栏宽度，不设最大限制（自由拖） -->
    <div
      v-if="chatMode && outbarOpen"
      class="outbar-resizer"
      :style="{ right: outbarWidth + 'px' }"
      :title="t('拖动调节宽度')"
      @mousedown="onOutbarResizeStart"
    ></div>
    <aside v-if="chatMode && outbarOpen" class="outbar" :style="{ width: outbarWidth + 'px' }">
      <div class="outbar-head">
        <span class="outbar-title">{{ t('产出物') }}</span>
        <span class="outbar-n">{{ presentedFiles.length }}</span>
      </div>
      <div class="outbar-list">
        <button v-for="f in presentedFiles" :key="f.path" class="outbar-item" :class="{ on: selectedFilePath === f.path }" :title="f.path" @click="selectFile(f)">
          <span class="oi-ico" :class="fileKind(f.path).kind">{{ fileKind(f.path).ico }}</span>
          <span class="oi-txt">
            <span class="oi-name">{{ f.label }}</span>
            <span class="oi-kind">{{ fileKind(f.path).kind }}</span>
          </span>
        </button>
        <div v-if="!presentedFiles.length" class="outbar-empty">{{ t('这个会话还没有产出物 —— 小满做完东西会出现在这里') }}</div>
      </div>
      <!-- 应用内预览区（像 WorkBuddy：侧边栏直接打开文件内容） -->
      <div class="outbar-preview">
        <div v-if="previewLoading" class="pv-empty">{{ t('读取中…') }}</div>
        <template v-else-if="filePreview">
          <div class="pv-head">
            <span class="pv-name" :title="selectedFilePath ?? ''">{{ filePreview.name }}</span>
            <button class="pv-open" :title="t('用系统程序打开')" @click="selectedFilePath && openFile(selectedFilePath)">⧉</button>
          </div>
          <div class="pv-body">
            <iframe v-if="filePreview.kind === 'html'" class="pv-frame" :srcdoc="filePreview.text" sandbox="allow-same-origin"></iframe>
            <img v-else-if="filePreview.kind === 'image' && filePreview.dataUrl" class="pv-img" :src="filePreview.dataUrl" :alt="filePreview.name" />
            <!-- eslint-disable-next-line vue/no-v-html -->
            <div v-else-if="filePreview.kind === 'text'" class="pv-md" v-html="mdToHtml(filePreview.text || '')"></div>
            <div v-else class="pv-empty">
              {{ t('此类型不支持内嵌预览') }}
              <button class="pv-open-btn" @click="selectedFilePath && openFile(selectedFilePath)">{{ t('用系统程序打开') }}</button>
            </div>
          </div>
        </template>
        <div v-else-if="previewError" class="pv-empty">{{ previewError }}</div>
        <div v-else class="pv-empty">{{ t('点击上面的产出物，在这里直接预览') }}</div>
      </div>
    </aside>
    <!-- ── 对话模式：消息流（会话历史 + 本轮任务），视觉上沉到轨迹之后、输入栏之上 ── -->
    <div v-if="chatMode" class="chat-flow">
      <!-- 左上角会话标题（同 WorkBuddy：顶部左侧题目，功能区在右上） -->
      <div class="chat-title" :title="chatTitle">{{ chatTitle }}</div>
      <div ref="chatMsgs" class="chat-msgs" @wheel="onChatWheel">
        <div
          v-for="(m, i) in historyBeforeAnswer"
          :id="`chat-msg-${i}`"
          :key="i"
          class="chat-msg"
          :class="[m.role, { hit: searchHitIdx === i }]"
        >
          <span class="chat-role">{{ m.role === 'user' ? '用户' : '小满' }}</span>
          <!-- 编辑态（2026-10-10 用户「编辑重发」）：停止/失败后可改最后一条用户消息再续接 -->
          <div v-if="editingIdx === i" class="msg-edit">
            <textarea v-model="editText" class="me-input" rows="4" @keydown.enter.exact.prevent="sendEdited"></textarea>
            <div class="me-hint">ⓘ {{ t('编辑后将从此处重新开始对话，已有产出物不会被删除') }}</div>
            <div class="me-btns">
              <button class="me-cancel" @click="cancelEdit">{{ t('取消') }}</button>
              <button class="me-send" :disabled="!editText.trim() || job?.state === 'running'" @click="sendEdited">{{ t('发送') }}</button>
            </div>
          </div>
          <template v-else>
            <div class="chat-bubble">{{ m.content }}</div>
            <button v-if="canEditMsg(i)" class="msg-edit-btn" :title="t('编辑')" @click="startEdit(i)">
              <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="square"><path d="M17 3l4 4L8 20l-5 1 1-5z" /></svg>
            </button>
          </template>
        </div>
        <div v-if="pendingTask" class="chat-msg user">
          <span class="chat-role">用户</span>
          <div class="chat-bubble">{{ pendingTask }}</div>
        </div>
        <div v-if="!historyBeforeAnswer.length && !pendingTask" class="chat-empty">{{ t('这个会话还没有消息，在下面输入第一条吧') }}</div>
        <!-- 实时思考流（2026-10-10 暴露思考过程）：running 时边想边滚动显示，随消息一起滚动 -->
        <div v-if="jobInSession && job?.state === 'running' && job.liveThinking" class="wp-live">
          <span class="wp-live-ico">✦</span>
          <pre class="wp-live-txt">{{ job.liveThinking }}</pre>
        </div>
        <!-- 深度思考段落（2026-10-10）：任务完成后，思考全文展示在正文上方（真实思考的位置） -->
        <div v-if="answerReady" class="think-sec">
          <div class="think-sec-head">💭 {{ t('深度思考') }}</div>
          <pre class="think-sec-body">{{ thinkingFull }}</pre>
        </div>
        <!-- 本轮正文（小满最终答复，放在思考段落之后） -->
        <div v-if="lastAssistantMsg" class="chat-msg assistant">
          <span class="chat-role">小满</span>
          <div class="chat-bubble">{{ lastAssistantMsg.content }}</div>
        </div>
      </div>

      <!-- 工作过程（WorkBuddy 式）：只显示属于当前会话的任务（job 全局单例，防串台） -->
      <div v-if="jobInSession && (job?.steps?.length || job?.state === 'running')" class="workproc">
        <button class="wp-head" @click="wpOpen = !wpOpen">
          <span class="wp-state" :class="job?.state">
            <span v-if="job?.state === 'running'" class="wp-spin"></span>
            {{ job?.state === 'running' ? t('运行中') : job?.state === 'aborted' ? t('已停止') : t('已完成') }}
          </span>
          <span v-if="wpDurText" class="wp-dur">{{ wpDurText }}</span>
          <span class="chev" :class="{ up: wpOpen }">⌄</span>
        </button>
        <div v-if="wpOpen" class="wp-steps">
          <template v-for="(r, i) in workRows" :key="i">
            <!-- 深度思考：默认收起，点开展开全文 -->
            <button v-if="r.kind === 'think'" class="wp-row think" @click="toggleWpRow(i)">
              <span class="wp-ico">✦</span>
              <span class="wp-title">{{ r.title }}</span>
              <span class="wp-ms" v-if="r.ms">{{ (r.ms / 1000).toFixed(1) }}s</span>
            </button>
            <!-- 工具行：动作 + 对象 -->
            <button v-else-if="r.kind === 'tool'" class="wp-row tool" :class="{ bad: r.ok === false }" @click="r.detail && toggleWpRow(i)">
              <span class="wp-ico">{{ r.title.includes('编辑') || r.title.includes('写入') ? '✎' : '⊕' }}</span>
              <span class="wp-title">{{ r.title }}</span>
              <span v-if="r.target" class="wp-target">{{ r.target }}</span>
              <span class="wp-ms" v-if="r.ms">{{ (r.ms / 1000).toFixed(1) }}s</span>
            </button>
            <!-- 核查行 -->
            <button v-else class="wp-row verify" :class="{ bad: r.ok === false }" @click="r.detail && toggleWpRow(i)">
              <span class="wp-ico">◎</span>
              <span class="wp-title">{{ r.title }}</span>
              <span class="wp-ms" v-if="r.ms">{{ (r.ms / 1000).toFixed(1) }}s</span>
            </button>
            <!-- 展开的详情（思考全文 / 工具结果） -->
            <pre v-if="wpExpanded.has(i) && r.detail" class="wp-detail">{{ r.detail.slice(0, 2000) }}</pre>
          </template>
        </div>
      </div>
    </div>
    <!-- ── Hero 启动台（2026-10-08 用户：极简改版，参考千问欢迎页）───────── -->
    <div class="hero">
      <!-- 品牌行：M 徽记 + slogan + 预览版 -->
      <div v-if="!chatMode" class="hero-brand">
        <img :src="logoMark" alt="XiaoMan Box" class="hero-logo" draggable="false" />
        <span class="hero-name">{{ t('小得盈满，未至大满') }}</span>
        <span class="tag-prev" :title="t('预览版')">{{ t('预览版') }}</span>
      </div>

      <!-- 控件行（极简两个下拉）：工作区（在本机选工作目录） + 模式（预设 = 会话域 + 专属提示词） -->
      <div v-if="!chatMode" class="ctl-row">
        <div class="ctl">
          <button class="ctl-btn" :title="wsActive || t('选择工作区目录')" @click="wsOpen = !wsOpen; presetOpen = false">
            <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">
              <path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" />
            </svg>
            <span class="ctl-label">{{ wsName }}</span>
            <svg class="chev" :class="{ up: wsOpen }" width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M6 9l6 6 6-6" /></svg>
          </button>
          <div v-if="wsOpen" class="ctl-menu" @mousedown.prevent>
            <button v-for="w in wsList" :key="w.path" class="ctl-item" :title="w.path" @click="pickWorkspace(w.path)">
              <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">
                <path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" />
              </svg>
              <span class="ci-main">{{ w.name }}</span>
              <svg v-if="w.path === wsActive" class="ci-check" width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 13l4 4L19 7" /></svg>
            </button>
            <div v-if="wsList.length" class="ctl-sep" />
            <button class="ctl-item add" @click="addWorkspaceViaDialog">
              <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M12 5v14M5 12h14" /></svg>
              <span class="ci-main">{{ t('添加工作区…') }}</span>
            </button>
          </div>
        </div>

        <div v-if="presetsLoaded && presetCategories.length" class="ctl">
          <button class="ctl-btn" :title="t(sceneName?.desc || sceneName?.name || '')" @click="presetOpen = !presetOpen; wsOpen = false">
            <AppIcon :name="sceneName?.icon ?? 'preset-daily'" :size="12" />
            <span class="ctl-label">{{ t(sceneName?.name ?? '') || t('模式') }}</span>
            <svg class="chev" :class="{ up: presetOpen }" width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M6 9l6 6 6-6" /></svg>
          </button>
          <div v-if="presetOpen" class="ctl-menu" @mousedown.prevent>
            <template v-for="c in presetCategories" :key="c.id">
              <div class="ctl-group">{{ t(c.name) }}</div>
              <button
                v-for="p in presetsOfCat(c.id)"
                :key="p.id"
                class="ctl-item"
                :title="t(p.desc || p.name)"
                @click="pickPreset(p.id); presetOpen = false"
              >
                <AppIcon :name="p.icon" :size="12" />
                <span class="ci-main">{{ t(p.name) }}</span>
                <svg v-if="p.id === sceneName?.id" class="ci-check" width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 13l4 4L19 7" /></svg>
              </button>
            </template>
          </div>
        </div>
      </div>

      <div class="box" :class="{ 'menu-pinned': menuOpen }">
        <!-- 附件 chip 行（真内容：文档/skill 读内容，图片走图文，音频转写，视频给路径） -->
        <div v-if="attachments.length" class="attach-row">
          <span v-for="(a, i) in attachments" :key="a.name + i" class="chip" :title="a.path || a.name">
            <span class="chip-ico">{{ kindIcon[a.kind] }}</span>
            <span class="chip-name">{{ a.name }}</span>
            <span v-if="a.size" class="chip-size">{{ fmtSize(a.size) }}</span>
            <button class="chip-x" :title="t('移除附件')" @click="removeAttachment(i)">✕</button>
          </span>
        </div>
        <!-- 已选 skill chip（2026-10-09 用户：选择性加载，发送时随任务注入） -->
        <div v-if="selectedSkills.length" class="attach-row">
          <span v-for="s in selectedSkills" :key="s" class="chip skill-chip">
            <span class="chip-ico">§</span>
            <span class="chip-name">{{ s }}</span>
            <button class="chip-x" :title="t('移除 skill')" @click="toggleSkill(s)">✕</button>
          </span>
        </div>
        <textarea
          ref="inputBox"
          v-model="task"
          rows="1"
          :placeholder="t('今天帮你做些什么？输入 / 唤起指令、@ 挂载上下文')"
          @input="onInput"
          @keydown.enter="onEnterKey"
          @keydown.esc="closeMenus"
          @blur="closeMenus"
          @drop.prevent="onDrop"
          @dragover.prevent
        />

        <!-- 斜杠指令菜单（真实数据：工具 + 会话指令 + skill） -->
        <div v-if="menuOpen === 'slash'" class="menu" @mousedown.prevent>
          <div class="menu-title">{{ t('指令 · 交给小满的活') }}</div>
          <button v-for="c in filteredCommands" :key="c.key" class="menu-item" @click="pickCommand(c)">
            <span class="mi-kind" :class="c.kind">{{ c.kind === 'tool' ? '▸' : c.kind === 'session' ? '⌘' : '§' }}</span>
            <span class="mi-main">
              <span class="mi-label">{{ c.label }}</span>
              <span class="mi-hint">{{ c.hint }}</span>
            </span>
          </button>
          <div v-if="!filteredCommands.length" class="menu-empty">{{ t('无匹配指令') }}</div>
        </div>

        <!-- @ 上下文菜单（真实数据） -->
        <div v-if="menuOpen === 'at'" class="menu" @mousedown.prevent>
          <div class="menu-title">{{ t('挂载上下文 · 让任务带上背景') }}</div>
          <button v-for="c in filteredCtxs" :key="c.key" class="menu-item" @click="pickCtx(c)">
            <span class="mi-kind" :class="c.kind">@</span>
            <span class="mi-main">
              <span class="mi-label">{{ c.label }}</span>
              <span class="mi-hint">{{ c.summary }}</span>
            </span>
          </button>
          <div v-if="!filteredCtxs.length" class="menu-empty">{{ t('无可用上下文') }}</div>
        </div>

        <div class="box-bar">
          <div class="bar-left">
            <!-- 「+」功能菜单：添加文件 / 模式 / 技能 / 连接器（真实数据） -->
            <button class="pill" :class="{ on: pop === 'plus' }" :title="t('添加文件、模式、技能、连接器')" @click="togglePop('plus')">
              <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round">
                <path d="M12 5v14M5 12h14" />
              </svg>
            </button>
            <!-- 模型选择器：当前激活提供方（真实） -->
            <button class="pill" :class="{ on: pop === 'model' }" :title="t('切换模型 / 思考模式')" @click="togglePop('model')">
              <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">
                <path d="M12 2l8 5v10l-8 5-8-5V7z" /><path d="M12 12l8-5M12 12v10M12 12L4 7" />
              </svg>
              {{ activeProviderName }}
            </button>
            <!-- 权限：允许完全访问（真实写 agent.json 并改写 system prompt） -->
            <button
              class="pill access"
              :class="{ on: pop === 'perm', danger: agentCfg?.allowFullAccess }"
              :title="t('权限模式')"
              @click="togglePop('perm')"
            >
              <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8">
                <path d="M12 3l9 16H3z" /><path d="M12 10v4M12 16.5v.5" />
              </svg>
              {{ t('允许完全访问') }}
            </button>
            <!-- 会话保留（列表 / 新建 / 查询 / 开关） -->
            <button class="pill" :class="{ on: pop === 'sessions' }" :title="t('会话：历史列表 · 新建 · 消息查询 · 保留开关')" @click="togglePop('sessions')">
              <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">
                <path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z" />
              </svg>
              {{ t('会话') }}
            </button>
            <!-- 上下文用量（对标 UEBox 面板） -->
            <button class="pill" :class="{ on: pop === 'usage' }" :title="t('上下文用量：分类 token 统计')" @click="togglePop('usage')">
              <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">
                <path d="M3 3v18h18" /><path d="M7 16v-5M12 16V8M17 16v-3" />
              </svg>
              {{ usage ? (usage.ratio * 100).toFixed(0) + '%' : t('用量') }}
            </button>
          </div>
          <!-- 会话栏中置（用户要求）：当前会话域在工具条正中 -->
          <div class="bar-center">
            <span class="box-note">{{ t('会话') }} · {{ sceneName?.name ?? '—' }}</span>
          </div>
          <div class="bar-right">
            <span
              class="box-note"
              :class="{ off: ualConnected === false || ualConnected === null }"
              :title="ualConnected === null ? t('后端离线') : ualConnected ? `${t('内置')} ${ualToolCount} ${t('个工具，引擎已连接')}` : t('引擎未连接（需打开 UE 工程并装 UnrealAgentLink 插件）')"
            >
              {{ ualConnected === null ? t('引擎离线') : ualConnected ? `${t('引擎')} · ${ualToolCount} ${t('工具')}` : t('引擎未连接') }}
            </span>
            <!-- 麦克风：真功能（2026-10-08 用户）——本地 SenseVoice ASR 纯离线，点击开录/停止转写 -->
            <button
              class="mic"
              :class="{ rec: micState === 'rec', busy: micState === 'busy' }"
              :title="micTitle"
              :disabled="micState === 'busy'"
              @click="toggleMic"
            >
              <span v-if="micState === 'rec'" class="mic-dot"></span>
              <svg v-else width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor"
                stroke-width="1.5" stroke-linecap="square" stroke-linejoin="miter">
                <rect x="9" y="3" width="6" height="11" />
                <path d="M5 11a7 7 0 0 0 14 0" /><path d="M12 18v3" />
              </svg>
            </button>
            <!-- 2026-10-10 用户「中断任务」：运行中发送按钮变停止按钮（同 WorkBuddy），点击立即掐断 -->
            <button
              v-if="job?.state === 'running'"
              class="send stop"
              :disabled="stopping"
              :title="t('停止任务')"
              @click="stopTask"
            >
              <svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor" stroke="none">
                <rect x="6" y="6" width="12" height="12" rx="2" />
              </svg>
            </button>
            <button
              v-else
              class="send"
              :disabled="starting || (!task.trim() && !attachments.length)"
              :title="t('交给小满（Enter）')"
              @click="send"
            >
              <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor"
                stroke-width="2" stroke-linecap="square" stroke-linejoin="miter">
                <path d="M12 20V5" /><path d="M5.5 11.5L12 5l6.5 6.5" />
              </svg>
            </button>
          </div>
        </div>

        <!-- 「+」功能菜单（二级导航式，真实数据） -->
        <div v-if="pop === 'plus'" class="pop" @mousedown.prevent>
          <template v-if="plusView === 'root'">
            <button class="pop-item" @click="fileInput?.click()">
              <span class="pi-ico">📎</span>
              <span class="pi-main"><span class="pi-label">{{ t('添加文件') }}</span><span class="pi-hint">{{ t('图片/音视频/文档/skill，真内容进任务（可拖拽进输入框）') }}</span></span>
              <span class="pi-arrow">›</span>
            </button>
            <button class="pop-item" @click="plusView = 'mode'">
              <span class="pi-ico">◆</span>
              <span class="pi-main"><span class="pi-label">{{ t('模式') }}</span><span class="pi-hint">{{ t(currentMode) }} · {{ t('温度与思考预算') }}</span></span>
              <span class="pi-arrow">›</span>
            </button>
            <button class="pop-item" @click="plusView = 'skill'">
              <span class="pi-ico">§</span>
              <span class="pi-main"><span class="pi-label">{{ t('技能') }}</span><span class="pi-hint">{{ t('已装') }} {{ skillCommands.length }} {{ t('个 skill') }}</span></span>
              <span class="pi-arrow">›</span>
            </button>
            <button class="pop-item" @click="plusView = 'conn'">
              <span class="pi-ico">⇄</span>
              <span class="pi-main"><span class="pi-label">{{ t('连接器') }}</span><span class="pi-hint">{{ t('内置引擎工具') }} + {{ mcpServers.length }} {{ t('个 MCP 服务器') }}</span></span>
              <span class="pi-arrow">›</span>
            </button>
          </template>
          <template v-else-if="plusView === 'mode'">
            <div class="pop-title"><button class="back" @click="plusView = 'root'">‹</button>{{ t('模式') }}</div>
            <button
              v-for="m in [
                { name: '严谨模式', hint: '温度 0.2 · 思考 2000 · 适合改代码/写图', t: 0.2, b: 2000 },
                { name: '平衡模式', hint: '温度 0.3 · 思考 800 · 日常任务', t: 0.3, b: 800 },
                { name: '创意模式', hint: '温度 0.7 · 思考 1600 · 头脑风暴/命名', t: 0.7, b: 1600 },
              ]"
              :key="m.name"
              class="pop-item"
              @click="patchCfg({ temperature: m.t, thinkingBudget: m.b }); pop = null"
            >
              <span class="pi-main">
                <span class="pi-label">{{ t(m.name) }} <span v-if="currentMode === m.name" class="pi-check">✓</span></span>
                <span class="pi-hint">{{ t(m.hint) }}</span>
              </span>
            </button>
          </template>
          <template v-else-if="plusView === 'skill'">
            <div class="pop-title"><button class="back" @click="plusView = 'root'">‹</button>{{ t('技能') }}</div>
            <div class="skill-load-hint">{{ t('勾选 = 本次任务加载该 skill（不勾不注入，省 token 提速）') }}</div>
            <div
              v-for="c in skillCommands"
              :key="c.key"
              class="pop-item skill-load"
              @click="toggleSkill(skillNameOf(c))"
            >
              <span class="pi-check" :class="{ on: selectedSkills.includes(skillNameOf(c)) }">✓</span>
              <span class="pi-ico">§</span>
              <span class="pi-main"><span class="pi-label">{{ c.label }}</span><span class="pi-hint">{{ c.hint }}</span></span>
            </div>
            <div v-if="!skillCommands.length" class="pop-empty">{{ t('暂无已装 skill') }}</div>
            <div v-if="selectedSkills.length" class="skill-load-foot">
              {{ t('已选') }} {{ selectedSkills.length }} {{ t('个：') }} {{ selectedSkills.join('、') }}
            </div>
          </template>
          <template v-else>
            <div class="pop-title"><button class="back" @click="plusView = 'root'">‹</button>{{ t('连接器') }}</div>
            <div class="pop-item static">
              <span class="pi-ico" :class="ualConnected ? 'ok' : ''">●</span>
              <span class="pi-main">
                <span class="pi-label">{{ t('内置引擎工具（UnrealAgentLink）') }}</span>
                <span class="pi-hint">{{ ualConnected ? `${t('已连接')} · ${ualToolCount} ${t('个工具')}` : `${t('已内置')} ${ualToolCount} ${t('个工具')} · ${t('等待引擎连接')}` }}</span>
              </span>
            </div>
            <!-- UE 工具组按需加载（2026-10-10 用户）：166 个 schema 62KB，非 UE 任务不带（prefill 11s+/502 主因） -->
            <div
              class="pop-item skill-load"
              :title="t('UE 工具 数量较多；短上下文请限定所需工具')"
              @click="loadUeTools = effectiveUe() ? false : true"
            >
              <span class="pi-check" :class="{ on: effectiveUe() }">✓</span>
              <span class="pi-ico">▦</span>
              <span class="pi-main">
                <span class="pi-label">{{ t('本次加载 UE 工具') }}</span>
                <span class="pi-hint">{{ effectiveUe() ? t('已加载（UE5 会话默认加载）') : t('未加载') }}</span>
              </span>
            </div>
            <div class="pop-item static">
              <label><input v-model="noTools" type="checkbox" /> {{ t('纯对话：本次不加载工具') }}</label>
            </div>
            <div class="pop-item static" style="display:block">
              <label>{{ t('限定工具名（可选，逗号分隔）') }}<input @mousedown.stop v-model="limitToolText" :disabled="noTools" placeholder="read_file, write_file" style="width:100%;margin-top:6px" /></label>
              <div class="pi-hint">{{ t('留空加载已启用工具；短上下文可只选本次需要的工具。') }}</div>
            </div>
            <div v-for="s in mcpServers" :key="s.name" class="pop-item static">
              <span class="pi-ico">⇄</span>
              <span class="pi-main"><span class="pi-label">{{ s.name }}</span><span class="pi-hint">{{ s.transport === 'http' ? s.url : s.command }}</span></span>
              <span class="pi-badge">{{ t('已注册') }}</span>
            </div>
            <div v-if="!mcpServers.length" class="pop-empty">{{ t('暂无 MCP 服务器') }}</div>
            <button class="pop-item manage" @click="goManage">
              <span class="pi-ico">↗</span>
              <span class="pi-main"><span class="pi-label">{{ t('管理连接器') }}</span><span class="pi-hint">{{ t('去「插件」页添加 MCP 服务器') }}</span></span>
            </button>
          </template>
        </div>

        <!-- 权限气泡（真实开关：写 agent.json 并改写 system prompt 行为约束） -->
        <div v-if="pop === 'perm'" class="pop perm" @mousedown.prevent>
          <p class="perm-text">
            {{
              agentCfg?.allowFullAccess
                ? t('当前权限为允许完全访问，小满将直接执行工具调用，请注意数据安全，建议执行可信的任务。')
                : t('当前权限为标准受限：执行删除文件、覆盖资产等破坏性操作前，小满会先说明并征得你的同意。')
            }}
          </p>
          <div class="perm-row">
            <span class="perm-label">{{ t('允许完全访问') }}</span>
            <button
              class="switch"
              :class="{ on: agentCfg?.allowFullAccess }"
              role="switch"
              :aria-checked="agentCfg?.allowFullAccess ?? false"
              @click="patchCfg({ allowFullAccess: !agentCfg?.allowFullAccess })"
            >
              <i class="knob"></i>
            </button>
          </div>
        </div>

        <!-- 会话抽屉（保留开关 / 新建会话 / 历史列表 / 消息查询） -->
        <div v-if="pop === 'sessions'" class="pop sessions" @mousedown.prevent>
          <div class="pop-item static">
            <span class="pi-main">
              <span class="pi-label">{{ t('保留会话') }}</span>
              <span class="pi-hint">{{ t('开：历史进上下文并持久化可查 · 关：一句一句独立不留存') }}</span>
            </span>
            <button
              class="switch"
              :class="{ on: agentCfg?.retainSessions }"
              role="switch"
              :aria-checked="agentCfg?.retainSessions ?? true"
              @click="patchCfg({ retainSessions: !agentCfg?.retainSessions })"
            >
              <i class="knob"></i>
            </button>
          </div>
          <button class="pop-item" @click="newSession">
            <span class="pi-ico">＋</span>
            <span class="pi-main"><span class="pi-label">{{ t('新建会话') }}</span><span class="pi-hint">{{ t('新任务开新话题，消息只进这个会话') }}</span></span>
          </button>
          <div class="pop-sep"></div>
          <button
            v-for="s in activeSessions"
            :key="s.sessionId"
            class="pop-item"
            :class="{ cur: s.sessionId === viewingSessionId }"
            @click="viewSession(s.sessionId)"
          >
            <span class="pi-main">
              <span class="pi-label">{{ s.sessionTitle }} <span v-if="s.sessionId === currentSessionId" class="pi-check">{{ t('当前') }}</span></span>
              <span class="pi-hint">{{ s.messageCount }} {{ t('条') }} · {{ (s.updatedAt || '').slice(5, 16).replace('T', ' ') }}</span>
            </span>
            <span
              v-if="s.sessionId !== currentSessionId"
              class="pi-arrow sw"
              :title="t('切换到此会话（下一条消息进它）')"
              @click.stop="switchToSession(s.sessionId)"
            >{{ t('切') }}</span>
            <span class="pi-arrow ar" :title="t('归档（收起，不删除）')" @click.stop="toggleArchive(s)">▤</span>
          </button>
          <div v-if="!sessionList.length" class="pop-empty">{{ t('还没有会话') }}</div>
          <template v-if="archivedSessions.length">
            <div class="pop-sep"></div>
            <div class="pop-sub">{{ t('已归档') }} · {{ archivedSessions.length }}</div>
            <button
              v-for="s in archivedSessions"
              :key="s.sessionId"
              class="pop-item muted"
              @click="viewSession(s.sessionId)"
            >
              <span class="pi-main">
                <span class="pi-label">{{ s.sessionTitle }}</span>
                <span class="pi-hint">{{ s.messageCount }} {{ t('条') }} · {{ (s.updatedAt || '').slice(5, 16).replace('T', ' ') }}</span>
              </span>
              <span class="pi-arrow ar" :title="t('恢复')" @click.stop="toggleArchive(s)">↺</span>
            </button>
          </template>
          <template v-if="viewingSessionId">
            <div class="pop-sep"></div>
            <div class="msg-view">
              <div v-for="(m, i) in viewingMessages" :key="i" class="msg-row" :class="m.role">
                <span class="msg-role">{{ m.role === 'user' ? '用户' : '小满' }}</span>
                <span class="msg-text">{{ m.content }}</span>
              </div>
              <div v-if="!viewingMessages.length" class="pop-empty">{{ t('这个会话还没有消息') }}</div>
            </div>
          </template>
        </div>

        <!-- 上下文用量（分类条形，对标 UEBox 面板） -->
        <div v-if="pop === 'usage'" class="pop usage" @mousedown.prevent>
          <template v-if="usage">
            <div class="usage-head">
              <span class="usage-title">{{ t('上下文用量') }}</span>
              <span class="usage-pct">{{ (usage.ratio * 100).toFixed(1) }}%</span>
            </div>
            <div class="usage-bar"><i class="usage-fill" :style="{ width: Math.min(usage.ratio * 100, 100) + '%' }"></i></div>
            <div class="usage-total">{{ t('背景估算') }} {{ fmtTok(usage.used) }} / {{ fmtTok(usage.contextWindow) }}</div>
            <div v-for="p in usage.parts" :key="p.key" class="usage-row">
              <span class="u-dot" :style="{ background: USAGE_COLORS[p.key] || '#888' }"></span>
              <span class="u-label">{{ p.label }}</span>
              <span class="u-bar"><i :style="{ width: (p.tokens / usageMaxTok) * 100 + '%', background: USAGE_COLORS[p.key] || '#888' }"></i></span>
              <span class="u-val">{{ (p.ratio * 100).toFixed(1) }}%</span>
            </div>
            <div class="usage-foot">{{ t('背景为估算，未含待发任务与附件；模型实际 token 为准。') }}</div>
            <div class="usage-foot">
              {{ usage.retainSessions ? `${t('保留开启')} · ${usage.historyMessages} ${t('条历史在上下文中')}` : t('保留已关 · 每条任务独立（不带历史）') }}
            </div>
          </template>
          <div v-else class="pop-empty">{{ t('后端离线，拿不到用量') }}</div>
        </div>

        <!-- 模型菜单（真实 providers + 思考模式/强度） -->
        <div v-if="pop === 'model'" class="pop" @mousedown.prevent>
          <template v-if="modelView === 'root'">
            <div class="pop-item static">
              <span class="pi-main">
                <span class="pi-label">{{ t('思考模式') }}</span>
                <span class="pi-hint">{{ t('开启后模型先推理再动手（thinkingBudget）') }}</span>
              </span>
              <button
                class="switch"
                :class="{ on: thinkLevel !== '关' }"
                role="switch"
                :aria-checked="thinkLevel !== '关'"
                @click="patchCfg({ thinkingBudget: thinkLevel === '关' ? 800 : 0 })"
              >
                <i class="knob"></i>
              </button>
            </div>
            <button class="pop-item" @click="modelView = 'think'">
              <span class="pi-main">
                <span class="pi-label">{{ t('思考强度') }}</span>
                <span class="pi-hint">{{ thinkLevel }}</span>
              </span>
              <span class="pi-arrow">›</span>
            </button>
            <div class="pop-sep"></div>
            <button
              v-for="p in providerList"
              :key="p.id"
              class="pop-item"
              @click="p.id === activeProviderId ? (pop = null) : pickProvider(p.id)"
            >
              <span class="pi-main">
                <span class="pi-label">{{ p.name }} <span v-if="p.id === activeProviderId" class="pi-check">✓</span></span>
                <span class="pi-hint">{{ p.model }} · {{ p.baseUrl }}</span>
              </span>
            </button>
            <div class="pop-sep"></div>
            <button class="pop-item manage" @click="goConfigureModel">
              <span class="pi-ico">✎</span>
              <span class="pi-main"><span class="pi-label">{{ t('配置自定义模型') }}</span><span class="pi-hint">{{ t('去「模型配置」页添加提供方') }}</span></span>
            </button>
          </template>
          <template v-else>
            <div class="pop-title"><button class="back" @click="modelView = 'root'">‹</button>{{ t('思考强度') }}</div>
            <button
              v-for="lv in THINK_LEVELS"
              :key="lv.name"
              class="pop-item"
              @click="patchCfg({ thinkingBudget: lv.b }); pop = null"
            >
              <span class="pi-main">
                <span class="pi-label">{{ t(lv.name) }} <span v-if="thinkLevel === lv.name" class="pi-check">✓</span></span>
                <span class="pi-hint">{{ t(lv.hint) }}</span>
              </span>
            </button>
          </template>
        </div>

        <!-- 添加文件：隐藏 input（Electron 下 File.path 是绝对路径） -->
        <input ref="fileInput" type="file" multiple style="display: none" @change="onFiles" />
      </div>

      <p class="hint">{{ t('Enter 发送 · Shift+Enter 换行 · / 唤起指令 · @ 挂载上下文 · 可拖文件进输入框') }}</p>
    </div>

    <div v-if="err" class="err block">{{ err }}</div>
    <div v-else-if="notice" class="ok-line block">{{ notice }}</div>

    <!-- ── 执行轨迹（有活才出现，空闲时不挂空标题；2026-10-08 用户：加 chatMode 守卫——
         上次任务的轨迹/失败信息不得出现在全新欢迎页，进对话模式才渲染）────────────────── -->
    <div
      v-if="chatMode && jobInSession && (job?.steps?.length || job?.state === 'running' || job?.state === 'done' || job?.state === 'failed')"
      class="log"
    >
      <div class="log-head">
        <span class="log-title">{{ t('执行轨迹') }}</span>
        <span v-if="job?.provider" class="log-provider">{{ job.provider }}</span>
        <span v-if="job?.state === 'running'" class="run"><i class="run-dot"></i>{{ t('运行中') }}</span>
        <span v-else-if="job?.state === 'done'" class="run done">{{ t('已完成') }}</span>
        <span v-else-if="job?.state === 'failed'" class="run failed">{{ t('失败') }}</span>
      </div>

      <!-- 系统提示词（DSH 图7）：折叠查看本次任务实际下发的完整提示词 -->
      <div v-if="job?.systemPrompt" class="fold-block">
        <button class="fold-head" @click="openSys = !openSys">
          <span class="fold-caret" :class="{ open: openSys }">▾</span>{{ t('系统提示词') }}
        </button>
        <pre v-if="openSys" class="fold-body">{{ job.systemPrompt }}</pre>
      </div>

      <!-- 上下文注入（DSH 图7）：这次任务带了什么背景 -->
      <div v-if="job?.ctx" class="fold-block">
        <button class="fold-head" @click="openCtx = !openCtx">
          <span class="fold-caret" :class="{ open: openCtx }">▾</span>{{ t('上下文注入') }}
          <span class="fold-meta">{{ t('记忆') }} {{ (job.ctx.memoryGlobalKeys || 0) + (job.ctx.memorySceneKeys || 0) }} {{ t('键') }} · {{ t('历史') }} {{ job.ctx.historyMessages }} {{ t('条') }} · skill {{ job.ctx.skills?.length ?? 0 }} {{ t('个') }}</span>
        </button>
        <div v-if="openCtx" class="fold-body ctx-body">
          <div class="ctx-row"><span class="k">{{ t('场景域') }}</span><span>{{ job.ctx.sceneId ?? '—' }}</span></div>
          <div class="ctx-row"><span class="k">{{ t('全局记忆') }}</span><span>{{ job.ctx.memoryGlobalKeys }} {{ t('键') }}</span></div>
          <div class="ctx-row"><span class="k">{{ t('场景记忆') }}</span><span>{{ job.ctx.memorySceneKeys }} {{ t('键') }}</span></div>
          <div class="ctx-row"><span class="k">{{ t('会话历史') }}</span><span>{{ job.ctx.historyMessages }} {{ t('条') }}（{{ t('防串台：只带本会话') }}）</span></div>
          <div class="ctx-row"><span class="k">{{ t('skill 清单') }}</span><span>{{ job.ctx.skills?.length ? job.ctx.skills.join('、') : t('无') }}</span></div>
        </div>
      </div>

      <ol v-if="job?.steps?.length" class="steps">
        <li
          v-for="(s, i) in job.steps"
          :key="i"
          class="step"
          :class="{ tool: s.type === 'tool', fail: s.type === 'tool' && !s.ok }"
        >
          <template v-if="s.type === 'model'">
            <div class="step-line">
              <span class="mark model">◆</span>
              <span v-if="s.tool_calls?.length" class="tc">{{ t('调工具：') }}{{ s.tool_calls.map((c) => c.name).join('、') }}</span>
              <span v-else class="ans">{{ t('答') }}</span>
              <span v-if="s.usage" class="step-meta">{{ s.usage.completionTokens }} tok · {{ s.ms }}ms</span>
            </div>
            <!-- 已思考（DSH 图7）：可折叠的推理全文 -->
            <button v-if="s.thinking" class="think-head" @click="toggleThink(i)">
              <span class="fold-caret" :class="{ open: openThink.has(i) }">▾</span>{{ t('已思考') }}
            </button>
            <pre v-if="s.thinking && openThink.has(i)" class="fold-body think-body">{{ s.thinking }}</pre>
            <div v-if="s.tool_calls?.length" class="tc-args">
              <span v-for="c in s.tool_calls" :key="c.name" class="tc-line">{{ c.name }} {{ c.arguments.slice(0, 120) }}{{ c.arguments.length > 120 ? '…' : '' }}</span>
            </div>
            <p v-if="!s.tool_calls && s.answer" class="ans-body">{{ s.answer }}</p>
          </template>
          <template v-else-if="s.type === 'tool'">
            <div class="step-line">
              <span class="mark" :class="s.ok ? 'ok' : 'bad'">{{ s.ok ? '▪' : '✕' }}</span>
              <span class="tn">{{ s.name }}</span>
              <span class="step-meta">{{ s.ms }}ms</span>
            </div>
            <div class="tc-args">
              <span class="tc-line">{{ JSON.stringify(s.args).slice(0, 140) }}{{ JSON.stringify(s.args).length > 140 ? '…' : '' }}</span>
              <span v-if="s.result" class="tc-line res">{{ s.result.slice(0, 160) }}{{ s.result.length > 160 ? '…' : '' }}</span>
            </div>
          </template>
          <template v-else>
            <div class="step-line">
              <span class="mark bad">⚠</span>
              <span class="tn">{{ t('程序化核查') }}</span>
            </div>
            <div class="tc-args">
              <span v-for="(iss, j) in s.issues" :key="j" class="tc-line">{{ iss }} {{ t('悬空输入') }}</span>
            </div>
          </template>
        </li>
      </ol>

      <!-- 会话统计（DSH 图5/6）：轮数/步数/TPS + Token 用量（hover 明细） -->
      <div v-if="jobStats && jobStats.totalTok > 0" class="stat-bar">
        <span class="stat">{{ jobStats.rounds }} {{ t('轮') }} · {{ jobStats.steps }} {{ t('步') }} · {{ jobStats.tps }} tok/s</span>
        <span class="stat tok">
          {{ jobStats.totalK }} tok · {{ t('缓存命中') }} {{ jobStats.hitRate }}
          <span class="tok-pop">
            <span class="tp-title">{{ t('Token 用量') }}</span>
            <span class="tp-row"><i>{{ t('输入') }}</i><b>{{ jobStats.promptTokens }} tok</b></span>
            <span class="tp-row"><i>{{ t('缓存读取') }}</i><b>{{ jobStats.cachedTokens }} tok</b></span>
            <span class="tp-row"><i>{{ t('输出') }}</i><b>{{ jobStats.completionTokens }} tok</b></span>
            <span class="tp-row"><i>{{ t('模型用时') }}</i><b>{{ (jobStats.modelMs / 1000).toFixed(1) }} {{ t('秒') }}</b></span>
          </span>
        </span>
      </div>

      <div v-if="job?.state === 'done' && job.answer" class="answer">
        <span class="answer-label">{{ t('小满的答复') }}</span>
        <p>{{ job.answer }}</p>
      </div>
      <div v-else-if="job?.state === 'failed'" class="err block">{{ t('失败：') }}{{ job.error }}</div>
    </div>
  </div>
</template>

<style scoped>
.agent { display: flex; flex-direction: column; flex: 1; min-height: 100%; position: relative; }

/* 欢迎态的报错（如「无法连接后端」）沉底小字化（2026-10-08 用户）：
   absolute 脱离 flex 流，不再挤掉 hero 的垂直居中；对话模式 err 走 order:3 原布局不受影响 */
.agent:not(.chatting) > .err {
  position: absolute;
  bottom: 10px;
  left: 50%;
  transform: translateX(-50%);
  width: fit-content;
  max-width: 90%;
  margin: 0;
}

/* 启动台在工作区垂直居中（用户 2026-10-07 晚）；对话模式保持顶对齐流式。
   用 margin-block:auto 而不是 justify-content:center —— 内容超高时不会裁掉顶部。
   （2026-10-08：flex:1 生效依赖 App.vue 的 .content.agent-launch 显式高度链） */
.agent:not(.chatting) .hero { margin: auto; }

/* ── 对话模式（用户 2026-10-07 晚）：消息流 → 轨迹 → 底部输入栏 ──
   复用 CSS order 重排，不移动 DOM：气泡流(0) → 执行轨迹(1) → hero 输入栏(2) → 错误(3)
   （2026-10-08：chat-flow flex:1 吃掉剩余高度，输入栏沉底——同 .agent flex:1 依赖外层高度链） */
.agent.chatting .chat-flow { order: 0; flex: 1 1 0px; height: 0; min-height: 0; } /* 2026-10-10：flex-basis 0 + height 0 强制高度分配，对话区高度确定性不依赖外层链 */
/* 2026-10-09 用户：工作过程已内嵌对话流（workproc），旧「执行轨迹」区块隐藏避免重复
   （系统提示词/上下文注入诊断 → 任务概览弹窗；步骤+思考 → workproc） */
.agent.chatting .log { display: none; }
.agent.chatting .hero { order: 2; }
.agent.chatting .err { order: 3; }

/* 消息流（会话历史 + 本轮任务） */
.chat-flow { width: 100%; max-width: 1100px; margin: 0 auto 10px; display: flex; flex-direction: column; } /* 2026-10-09 用户：宽度撤回 1100 居中（不顶满）；flex 列让 chat-msgs 吃满剩余高度与输入框衔接 */
/* 左上角会话标题（同 WorkBuddy：顶部左侧题目，与右上角功能区同行） */
.chat-title {
  font-size: 1rem;
  font-weight: 600;
  color: var(--ink);
  padding: 0 0 6px; /* 2026-10-10：压缩顶部留白，会话内容高度上提 */
  max-width: 55%;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.chat-msgs {
  flex: 1 1 0px;        /* 2026-10-10：flex-basis 0 + height 0 强制高度分配——纯 flex:1 依赖 min-height 链，链上任何一环失效就滚不动 */
  height: 0;
  overflow-y: auto;
  display: flex;
  flex-direction: column;
  gap: 10px;
  padding: 4px 2px;
  overscroll-behavior: contain;
}
/* 2026-10-10 滚动修复：flex 列容器里子项默认 flex-shrink:1 会被压缩以适应容器，
   导致 scrollHeight 永远 ≈ clientHeight、overflow 不触发、滚轮滚不动。
   子项全部 flex-shrink:0，让内容自然溢出触发滚动。 */
.chat-msgs > * { flex-shrink: 0; }
.chat-msg { display: flex; gap: 9px; align-items: flex-start; }
.chat-msg.user { flex-direction: row-reverse; }
/* 编辑重发（2026-10-10 用户）：hover 用户消息出铅笔按钮；编辑态 textarea+提示+按钮 */
.msg-edit-btn {
  flex: none;
  align-self: center;
  background: transparent;
  border: none;
  color: var(--dim-2);
  cursor: pointer;
  padding: 4px 6px;
  border-radius: var(--radius-sm);
  opacity: 0;
  transition: opacity 0.12s ease;
}
.chat-msg:hover .msg-edit-btn { opacity: 1; }
.msg-edit-btn:hover { background: var(--hover); color: var(--ink); }
.msg-edit {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: 8px;
  background: var(--panel-bg-2, var(--hover));
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 10px 12px;
}
.me-input {
  width: 100%;
  resize: vertical;
  min-height: 64px;
  background: transparent;
  border: none;
  outline: none;
  color: var(--ink);
  font-size: 0.923rem;
  line-height: 1.6;
  font-family: inherit;
}
.me-hint { font-size: 0.769rem; color: var(--dim-2); }
.me-btns { display: flex; justify-content: flex-end; gap: 8px; }
.me-btns button {
  border-radius: var(--radius-sm);
  padding: 5px 14px;
  font-size: 0.846rem;
  cursor: pointer;
  border: 1px solid var(--border);
}
.me-cancel { background: transparent; color: var(--dim); }
.me-cancel:hover { background: var(--hover); color: var(--ink); }
.me-send { background: var(--caution); border-color: var(--caution); color: #000; }
.me-send:hover:not(:disabled) { background: var(--caution-deep); border-color: var(--caution-deep); }
.me-send:disabled { opacity: 0.4; cursor: not-allowed; }
.chat-role { flex: none; font-size: 0.846rem; font-weight: 700; color: var(--dim); margin-top: 5px; }
.chat-msg.assistant .chat-role { color: var(--ink); }
.chat-bubble {
  max-width: 80%;
  padding: 8px 13px;
  border-radius: 12px;
  font-size: 0.962rem;
  line-height: 1.7;
  white-space: pre-wrap;
  word-break: break-word;
  background: var(--hover);
  color: var(--ink);
  border: 1px solid var(--border);
}
.chat-msg.user .chat-bubble {
  background: rgba(250, 204, 21, 0.1);
  border-color: rgba(250, 204, 21, 0.28);
}
/* 2026-10-09 用户：模型输出不要对话框（黄框/边框）——纯文本全宽，像 WorkBuddy */
.chat-msg.assistant .chat-bubble {
  background: transparent;
  border: none;
  border-radius: 0;
  padding: 2px 0;
  max-width: 100%;
  flex: 1;
  min-width: 0;
}
.chat-empty { text-align: center; font-size: 0.923rem; color: var(--dim-2); padding: 16px 0; }

/* 会话栏中置 */
.bar-center { flex: 1; text-align: center; min-width: 0; }
/* 2026-10-08 用户：inline-block + nowrap 在窄容器里 min-content 不收缩 → 溢出压到右侧 note 上（实测重叠 26px）。
   改 block + max-width:100% 强制裁剪在容器内，ellipsis 才真正生效 */
.bar-center .box-note { display: block; max-width: 100%; margin: 0 auto; }

/* ── Hero ─────────────────────────────────────────────────── */
.hero {
  max-width: 1100px; /* 2026-10-09 用户：宽度撤回 1100 居中（不顶满） */
  margin: 0 auto;
  width: 100%;
  padding: 30px 0 6px;
  display: flex;
  flex-direction: column;
  align-items: center;
}
/* 品牌行：M 徽记 + slogan + 预览版（替代原 2rem 大标题，收敛、克制） */
.hero-brand {
  display: flex;
  align-items: center;
  gap: 10px;
  margin: 0 0 22px;
}
.hero-logo { width: 32px; height: 32px; display: block; user-select: none; }
.hero-name {
  font-size: 1.538rem;
  font-weight: 600;
  letter-spacing: 1.5px;
  color: var(--ink);
}
/* 预览版徽标：千问式灰底实心块（2026-10-08 同色大背景改版） */
.tag-prev {
  flex: none;
  font-size: 0.692rem;
  line-height: 1;
  color: var(--dim);
  background: rgba(255, 255, 255, 0.08);
  border: none;
  border-radius: 4px;
  padding: 3px 6px;
  letter-spacing: 0.5px;
}

/* 控件行（2026-10-08 用户 极简改版）：工作区 + 模式 两个下拉，替代原按钮矩阵与快捷片。
   ghost 化（千问式「干河工作区 ∨」）：无边框融入同色大背景，hover 才显形 */
.ctl-row {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 12px;
}
.ctl { position: relative; }
.ctl-btn {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  background: transparent;
  border: none;
  border-radius: var(--radius-ctl);
  color: var(--dim);
  font-family: var(--font-ui);
  font-size: 0.846rem;
  padding: 4px 10px;
  cursor: pointer;
  transition: color 0.12s ease, background 0.12s ease;
}
.ctl-btn:hover { color: var(--ink); background: var(--hover); }
.ctl-label { max-width: 160px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.chev { opacity: 0.7; transition: transform 0.12s ease; }
.chev.up { transform: rotate(180deg); }
.ctl-menu {
  position: absolute;
  top: calc(100% + 6px);
  left: 0;
  min-width: 180px;
  max-height: 320px;
  overflow-y: auto;
  background: var(--panel-bg-2);
  border: 1px solid var(--border-bright);
  border-radius: var(--radius);
  box-shadow: 0 8px 24px rgba(0, 0, 0, 0.5);
  z-index: 40;
  padding: 5px 0;
}
.ctl-item {
  display: flex;
  align-items: center;
  gap: 8px;
  width: 100%;
  background: transparent;
  border: none;
  color: var(--dim);
  font-family: var(--font-ui);
  font-size: 0.846rem;
  padding: 6px 12px;
  cursor: pointer;
  text-align: left;
  transition: color 0.12s ease, background 0.12s ease;
}
.ctl-item:hover { color: var(--ink); background: var(--hover); }
.ctl-item .ci-main { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.ctl-item .ci-check { color: var(--caution); flex: none; }
.ctl-item.add { color: var(--dim); }
.ctl-group {
  font-size: 0.692rem;
  color: var(--ink-3);
  letter-spacing: 0.5px;
  padding: 6px 12px 3px;
}
.ctl-sep { height: 1px; background: var(--border); margin: 5px 0; }

/* 输入框：大盒子，工具条内嵌底部。同色大背景上的浮起面（2026-10-08 二轮 用户）：
   底色调浅到 rgba .10 ≈ #2c2c2e（用户参考色块），内边距放宽——不再「短紧」 */
.box {
  width: 100%;
  position: relative;
  background: rgba(255, 255, 255, 0.1);
  border: 1px solid rgba(255, 255, 255, 0.09);
  border-radius: var(--radius);
  padding: 14px 16px 10px;
  transition: border-color 0.12s ease, background 0.12s ease;
}
.box:focus-within {
  border-color: var(--caution);
  background: rgba(255, 255, 255, 0.125);
}
/* 附件 chip 行（2026-10-08 用户：真内容附件） */
.attach-row { display: flex; flex-wrap: wrap; gap: 6px; margin-bottom: 8px; }
.chip {
  display: inline-flex; align-items: center; gap: 6px;
  font-size: 0.846rem; padding: 3px 8px;
  border: 1px solid rgba(255, 255, 255, 0.14);
  border-radius: var(--radius-ctl);
  color: var(--ink); background: rgba(255, 255, 255, 0.06);
}
.chip-name { max-width: 260px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.chip-size { color: var(--ink-3); font-size: 0.769rem; white-space: nowrap; }
.chip-x { background: none; border: none; color: var(--ink-3); cursor: pointer; padding: 0 2px; font-size: 0.846rem; line-height: 1; }
.chip-x:hover { color: #e5534b; }
/* skill 已选 chip：沙金描边区分于附件 */
.chip.skill-chip { border-color: var(--caution-mid); background: rgba(250, 204, 21, 0.08); }
.chip.skill-chip .chip-ico { color: var(--caution); }
textarea {
  width: 100%;
  background: transparent;
  border: none;
  padding: 0;
  color: var(--ink);
  font-family: var(--font-ui);
  font-size: 1.038rem;
  line-height: 1.6;
  /* 2026-10-08 用户：resize 手柄叠字（根治=去掉手柄），改自动长高（autoGrow）：
     64px 两行起步，最多 240px 后内部滚动 */
  min-height: 64px;
  max-height: 240px;
  height: auto;
  resize: none;
  overflow-y: auto;
  outline: none;
}

/* ── 指令 / 上下文 下拉菜单（真实数据）────────────────────── */
.menu {
  position: absolute;
  left: 0;
  right: 0;
  bottom: calc(100% - 2px);
  max-height: 300px;
  overflow-y: auto;
  background: var(--panel-bg-2);
  border: 1px solid var(--border-bright);
  border-radius: var(--radius);
  box-shadow: 0 8px 24px rgba(0, 0, 0, 0.5);
  z-index: 40;
  padding: 6px 0;
}
.menu-title {
  font-family: var(--font-ui);
  font-size: 0.846rem;
  font-weight: 600;
  letter-spacing: 0.3px;
  color: var(--dim);
  padding: 6px 12px 8px;
  border-bottom: 1px solid var(--border);
}
.menu-item {
  width: 100%;
  display: flex;
  align-items: center;
  gap: 10px;
  background: transparent;
  border: none;
  border-left: 2px solid transparent;
  padding: 7px 12px;
  cursor: pointer;
  text-align: left;
  transition: background 0.1s ease;
}
.menu-item:hover, .menu-item:focus {
  background: var(--panel-bg);
  border-left-color: var(--caution);
}
.mi-kind { flex: none; width: 16px; font-family: var(--font-tech); color: var(--ink-3); text-align: center; }
.mi-kind.tool { color: var(--ink-3); }
.mi-kind.skill { color: var(--caution); }
.mi-kind.memory { color: var(--caution); }
.mi-kind.member { color: var(--ink); }
.mi-kind.uebox { color: var(--ink); }
.mi-kind.session { color: var(--ink); }
.mi-main { display: flex; flex-direction: column; gap: 1px; min-width: 0; }
.mi-label { font-size: 0.962rem; color: var(--ink); }
.mi-hint { font-size: 0.808rem; color: var(--dim-2); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.menu-empty { padding: 10px 12px; font-size: 0.923rem; color: var(--dim); }

.box-bar { display: flex; align-items: center; justify-content: space-between; gap: 14px; margin-top: 8px; }
.bar-left { display: flex; align-items: center; gap: 8px; min-width: 0; }
.bar-center { flex: 1; text-align: center; min-width: 0; }
.bar-right { display: flex; align-items: center; gap: 10px; flex: none; }

/* 底部状态小字：普通灰字，无路径标注 */
.box-note {
  font-size: 0.846rem;
  color: var(--dim-2);
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.box-note.off { color: var(--ink-3); }

/* ── 工具条 pill（+ / 模型 / 权限）─────────────────────────── */
.pill {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  max-width: 220px;
  background: transparent;
  border: 1px solid var(--border-bright);
  border-radius: 999px;
  color: var(--dim);
  font-size: 0.885rem;
  padding: 4px 11px;
  cursor: pointer;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  transition: color 0.12s ease, border-color 0.12s ease, background 0.12s ease;
}
/* 四件套图标 pill（搜索/分享/历史/概览）：纯图标小圆钮 + 分隔线 */
.pill.ico { padding: 4px 7px; }
.bar-sep { width: 1px; height: 14px; background: var(--border); margin: 0 2px; flex: none; }

/* 对话内搜索 */
.search-inp { width: 100%; }
.search-meta { font-size: 0.769rem; color: var(--dim-2); }
.search-list { display: flex; flex-direction: column; gap: 2px; max-height: 220px; overflow-y: auto; }
.search-hit { display: flex; gap: 8px; align-items: baseline; background: transparent; border: none; color: var(--dim); font-size: 0.808rem; padding: 5px 6px; cursor: pointer; text-align: left; border-radius: var(--radius-sm); }
.search-hit:hover { background: var(--panel-bg-2); color: var(--ink); }
.sh-role { flex: none; color: var(--ink-3); font-size: 0.769rem; }
.sh-txt { min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.chat-msg.hit .chat-bubble { outline: 1px solid var(--caution); border-radius: var(--radius-sm); }

/* 历史提问 */
.q-txt { max-width: 300px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

/* 任务概览 */
.ov-head { display: flex; align-items: center; justify-content: space-between; gap: 10px; }
.ov-state { font-size: 0.769rem; }
.ov-state.running { color: var(--caution); }
.ov-state.done { color: var(--ok); }
.ov-state.failed { color: var(--err); }
.ov-task { font-size: 0.808rem; color: var(--dim); line-height: 1.5; }
.ov-rows { display: grid; grid-template-columns: 1fr 1fr; gap: 4px 14px; }
.ov-row { display: flex; justify-content: space-between; gap: 10px; font-size: 0.808rem; }
.ov-row i { font-style: normal; color: var(--dim-2); }
.ov-row b { font-family: var(--font-num); color: var(--ink); font-weight: 500; }
.ov-tools { display: flex; flex-wrap: wrap; gap: 4px; }
.ov-err { font-size: 0.808rem; color: var(--err); }
.ov-recent { display: flex; flex-direction: column; gap: 3px; border-top: 1px solid var(--border); padding-top: 6px; }
.ov-step { display: flex; align-items: baseline; gap: 8px; font-size: 0.769rem; }
.os-type { flex: none; color: var(--ink-3); }
.os-type.tool { color: var(--caution); }
.os-txt { min-width: 0; flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: var(--dim); }
.os-ms { flex: none; color: var(--dim-2); font-family: var(--font-num); }
.pill:hover, .pill.on { color: var(--ink); border-color: var(--dim); background: var(--hover); }
.pill.access.danger { color: var(--warn); border-color: rgba(255, 183, 0, 0.45); }

/* ── 工具条弹出面板（+ 菜单 / 权限 / 模型）────────────────── */
.pop {
  position: absolute;
  left: 0;
  bottom: calc(100% - 2px);
  width: 320px;
  max-height: 340px;
  overflow-y: auto;
  background: var(--panel-bg-2);
  border: 1px solid var(--border-bright);
  border-radius: var(--radius);
  box-shadow: 0 8px 24px rgba(0, 0, 0, 0.55);
  z-index: 45;
  padding: 6px;
}
.pop.perm { width: 280px; padding: 14px; }
.pop-item {
  width: 100%;
  display: flex;
  align-items: center;
  gap: 10px;
  background: transparent;
  border: none;
  border-radius: var(--radius-ctl);
  padding: 8px 10px;
  cursor: pointer;
  text-align: left;
  transition: background 0.1s ease;
}
.pop-item:hover:not(.static) { background: var(--hover); }
.pop-item.static { cursor: default; }
.pi-ico { flex: none; width: 18px; text-align: center; color: var(--dim); font-size: 0.923rem; }
.pi-ico.ok { color: var(--ok); }
.pi-main { display: flex; flex-direction: column; gap: 1px; min-width: 0; flex: 1; }
.pi-label { font-size: 0.962rem; color: var(--ink); }
.pi-hint { font-size: 0.808rem; color: var(--dim-2); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.pi-arrow { flex: none; color: var(--ink-3); font-size: 1.077rem; }
.pi-check { color: var(--caution); }
.pi-badge {
  flex: none;
  font-size: 0.769rem;
  color: var(--dim);
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  padding: 0 6px;
}
.pop-title {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 0.923rem;
  font-weight: 600;
  color: var(--ink);
  padding: 6px 10px 8px;
  border-bottom: 1px solid var(--border);
  margin-bottom: 4px;
}
.back {
  flex: none;
  width: 20px;
  height: 20px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  background: transparent;
  border: none;
  border-radius: var(--radius-sm);
  color: var(--dim);
  font-size: 1.077rem;
  cursor: pointer;
}
.back:hover { color: var(--ink); background: var(--hover); }
.pop-empty { padding: 10px; font-size: 0.923rem; color: var(--dim-2); text-align: center; }

/* skill 选择性加载（2026-10-09 用户） */
.skill-load-hint { font-size: 0.769rem; color: var(--dim-2); padding: 2px 8px 6px; line-height: 1.5; }
.pop-item.skill-load { cursor: pointer; }
.skill-load .pi-check {
  flex: none;
  width: 16px;
  height: 16px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  font-size: 0.769rem;
  border: 1px solid var(--border-bright);
  border-radius: 4px;
  color: transparent;
}
.skill-load .pi-check.on { background: var(--caution); border-color: var(--caution); color: #000; font-weight: 700; }
.skill-load-foot {
  margin-top: 6px;
  padding: 6px 8px;
  border-top: 1px solid var(--border);
  font-size: 0.769rem;
  color: var(--caution);
  word-break: break-all;
}
.pop-sep { height: 1px; background: var(--border); margin: 5px 6px; }
.pop-item.manage { border-top: none; color: var(--dim); }
.pop-item.manage .pi-label { color: var(--dim); }

/* ── 会话抽屉（保留开关 / 列表 / 消息查询）────────────────── */
.pop.sessions { width: 340px; max-height: 420px; overflow-y: auto; }
.pop-item.cur { background: var(--hover); }
.pi-arrow.sw,
.pi-arrow.ar {
  flex: none;
  font-size: 0.846rem;
  color: var(--dim);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 2px 7px;
  margin-left: 2px;
}
.pi-arrow.sw:hover,
.pi-arrow.ar:hover { color: var(--ink); border-color: var(--dim); }
.pop-sub { padding: 6px 10px 2px; font-size: 0.808rem; color: var(--dim-2); text-transform: none; }
.pop-item.muted { opacity: 0.55; }
.pop-item.muted .pi-label { text-decoration: none; }
.pi-check { font-size: 0.769rem; color: var(--ok, #34d399); margin-left: 4px; }
.msg-view { padding: 4px 10px 10px; max-height: 200px; overflow-y: auto; }
.msg-row { display: flex; gap: 8px; padding: 5px 0; border-bottom: 1px dashed var(--border); align-items: flex-start; }
.msg-row:last-child { border-bottom: none; }
.msg-role { flex: none; font-size: 0.846rem; font-weight: 700; color: var(--dim); width: 30px; }
.msg-row.assistant .msg-role { color: var(--ink); }
.msg-text { font-size: 0.923rem; line-height: 1.6; color: var(--dim); word-break: break-all; display: -webkit-box; -webkit-line-clamp: 4; -webkit-box-orient: vertical; overflow: hidden; }

/* ── 上下文用量（对标 UEBox 面板）─────────────────────────── */
.pop.usage { width: 300px; padding: 12px 14px; }
.usage-head { display: flex; align-items: baseline; justify-content: space-between; margin-bottom: 8px; }
.usage-title { font-size: 1rem; font-weight: 700; color: var(--ink); }
.usage-pct { font-size: 1.308rem; font-weight: 800; color: var(--ink); }
.usage-bar { height: 8px; border-radius: 4px; background: var(--border); overflow: hidden; display: flex; }
.usage-fill { height: 100%; border-radius: 4px; background: linear-gradient(90deg, #a78bfa, #fbbf24, #f472b6); transition: width 0.3s; }
.usage-total { font-size: 0.846rem; color: var(--dim); margin: 6px 0 10px; }
.usage-row { display: flex; align-items: center; gap: 7px; padding: 3px 0; }
.u-dot { flex: none; width: 7px; height: 7px; border-radius: 50%; }
.u-label { flex: none; width: 84px; font-size: 0.885rem; color: var(--dim); }
.u-bar { flex: 1; height: 5px; border-radius: 3px; background: var(--border); overflow: hidden; }
.u-bar i { display: block; height: 100%; border-radius: 3px; transition: width 0.3s; }
.u-val { flex: none; width: 44px; text-align: right; font-size: 0.846rem; color: var(--dim); font-variant-numeric: tabular-nums; }
.usage-foot { margin-top: 10px; font-size: 0.846rem; color: var(--dim-2); border-top: 1px solid var(--border); padding-top: 8px; }

/* ── 权限气泡 ─────────────────────────────────────────────── */
.perm-text { margin: 0 0 12px; font-size: 0.923rem; line-height: 1.7; color: var(--dim); }
.perm-row { display: flex; align-items: center; justify-content: space-between; gap: 10px; }
.perm-label { font-size: 0.962rem; font-weight: 600; color: var(--ink); }

/* 开关（权限 / 思考模式）：DsH 式圆角滑块 */
.switch {
  flex: none;
  width: 36px;
  height: 20px;
  display: inline-flex;
  align-items: center;
  background: var(--panel-bg);
  border: 1px solid var(--border-bright);
  border-radius: 999px;
  padding: 2px;
  cursor: pointer;
  transition: background 0.15s ease, border-color 0.15s ease;
}
.switch .knob {
  width: 14px;
  height: 14px;
  border-radius: 50%;
  background: var(--dim);
  transition: transform 0.15s ease, background 0.15s ease;
}
.switch.on { background: rgba(var(--caution-rgb), 0.25); border-color: var(--caution); }
.switch.on .knob { transform: translateX(16px); background: var(--caution); }

/* 麦克风：真功能（本地 SenseVoice ASR）——idle 淡实线描边（2026-10-08 用户：虚线被误认成 resize 手柄），
   rec 红点呼吸，busy 半透明 */
.mic {
  flex: none;
  width: 26px;
  height: 26px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  color: var(--ink-3);
  background: transparent;
  border: 1px solid var(--border);
  border-radius: var(--radius-ctl);
  cursor: pointer;
  transition: color 0.12s ease, border-color 0.12s ease, background 0.12s ease;
}
.mic:hover { color: var(--ink); border-color: var(--border-bright); background: var(--hover); }
.mic.rec { color: var(--err); border-color: var(--err); }
.mic.rec .mic-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: var(--err);
  animation: mic-pulse 1.1s ease-in-out infinite;
}
.mic.busy { color: var(--dim); opacity: 0.55; cursor: wait; }
@keyframes mic-pulse {
  0%, 100% { opacity: 1; transform: scale(1); }
  50% { opacity: 0.35; transform: scale(0.72); }
}

/* 发送键：全页唯一实心强调色；无字/运行中退化为描边 */
.send {
  flex: none;
  width: 34px;
  height: 34px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  background: var(--caution);
  border: 1px solid var(--caution);
  border-radius: var(--radius-ctl);
  color: #000;
  cursor: pointer;
  transition: background 0.12s ease, border-color 0.12s ease;
}
.send:hover:not(:disabled) { background: var(--caution-deep); border-color: var(--caution-deep); }
.send:disabled { background: transparent; color: var(--ink-3); border-color: var(--border-bright); cursor: not-allowed; }
/* 停止按钮（2026-10-10 用户「中断任务」）：运行中替代发送按钮，红色醒目 */
.send.stop { background: var(--err, #e5484d); border-color: var(--err, #e5484d); color: #fff; }
.send.stop:hover:not(:disabled) { background: var(--err-deep, #c53740); border-color: var(--err-deep, #c53740); }

.hint { margin: 10px 0 0; font-size: 0.808rem; color: var(--ink-3); letter-spacing: 0.5px; }

/* ── 执行轨迹：折叠块（系统提示词 / 上下文注入 / 已思考）──── */
.fold-block { margin-bottom: 8px; border: 1px solid var(--border); border-radius: var(--radius-ctl); background: var(--panel-bg); }
.fold-head {
  width: 100%;
  display: flex;
  align-items: center;
  gap: 7px;
  background: transparent;
  border: none;
  color: var(--dim);
  font-size: 0.923rem;
  font-weight: 600;
  padding: 8px 12px;
  cursor: pointer;
  text-align: left;
}
.fold-head:hover { color: var(--ink); }
.fold-caret { font-size: 0.692rem; color: var(--ink-3); transition: transform 0.12s ease; display: inline-block; }
.fold-caret.open { transform: rotate(-90deg); }
.fold-meta { margin-left: auto; font-size: 0.808rem; font-weight: 400; color: var(--ink-3); }
.fold-body {
  margin: 0;
  padding: 10px 12px;
  border-top: 1px solid var(--border);
  font-family: var(--font-num);
  font-size: 0.846rem;
  line-height: 1.7;
  color: var(--dim);
  white-space: pre-wrap;
  word-break: break-all;
  max-height: 260px;
  overflow-y: auto;
}
.ctx-body { font-family: var(--font-ui); }
.ctx-row { display: flex; gap: 12px; padding: 3px 0; font-size: 0.923rem; }
.ctx-row .k { flex: none; width: 76px; color: var(--ink-3); }
.ctx-row span:last-child { color: var(--dim); min-width: 0; word-break: break-all; }

.think-head {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  background: transparent;
  border: none;
  color: var(--dim-2);
  font-size: 0.846rem;
  padding: 2px 6px 2px 22px;
  cursor: pointer;
}
.think-head:hover { color: var(--dim); }
.think-body { margin: 2px 0 6px 22px; padding: 8px 10px; border-left: 2px solid var(--border); border-top: none; max-height: 200px; color: var(--dim-2); }

.step { display: flex; flex-direction: column; gap: 2px; font-size: 0.923rem; padding: 6px 8px; }
.step.tool { background: var(--panel-bg-2); }
.step.fail { background: rgba(255, 107, 97, 0.08); }
.step-line { display: flex; align-items: baseline; gap: 8px; }
.step-meta { margin-left: auto; flex: none; font-size: 0.808rem; color: var(--ink-3); font-family: var(--font-num); }
.tc-args { display: flex; flex-direction: column; gap: 1px; padding-left: 22px; }
.tc-line { font-family: var(--font-num); font-size: 0.808rem; color: var(--dim-2); word-break: break-all; }
.tc-line.res { color: var(--dim); }
.ans-body { margin: 2px 0 2px 22px; font-size: 1rem; color: var(--ink); line-height: 1.7; }

/* ── 会话统计条（DSH 图5/6）───────────────────────────────── */
.stat-bar { display: flex; align-items: center; gap: 14px; margin-top: 10px; font-size: 0.846rem; color: var(--dim-2); font-family: var(--font-num); }
.stat.tok { position: relative; cursor: help; }
/* Token 用量悬浮气泡（图6） */
.tok-pop {
  display: none;
  position: absolute;
  bottom: calc(100% + 8px);
  left: 50%;
  transform: translateX(-50%);
  width: 220px;
  background: var(--panel-bg-2);
  border: 1px solid var(--border-bright);
  border-radius: var(--radius);
  box-shadow: 0 8px 24px rgba(0, 0, 0, 0.55);
  padding: 12px 14px;
  z-index: 30;
  font-family: var(--font-ui);
}
.stat.tok:hover .tok-pop { display: flex; flex-direction: column; gap: 6px; }
.tp-title { font-size: 0.923rem; font-weight: 600; color: var(--ink); margin-bottom: 2px; }
.tp-row { display: flex; justify-content: space-between; font-size: 0.885rem; }
.tp-row i { font-style: normal; color: var(--dim-2); }
.tp-row b { color: var(--ink); font-family: var(--font-num); font-weight: 600; }

/* ── 执行轨迹 ─────────────────────────────────────────────── */
.log { max-width: 1100px; width: 100%; margin: 26px auto 0; }
.log-head { display: flex; align-items: baseline; gap: 10px; padding-bottom: 8px; border-bottom: 1px solid var(--border); margin-bottom: 10px; }
.log-title { font-size: 1rem; font-weight: 600; color: var(--ink); }
.run { margin-left: auto; font-size: 0.846rem; color: var(--caution); display: inline-flex; align-items: center; gap: 6px; }
.run.done { color: var(--ok); }
.run.failed { color: var(--err); }
/* 运行点：形状本身是圆的 → 留圆（直角化铁律②） */
.run-dot {
  width: 7px;
  height: 7px;
  border-radius: var(--radius-round);
  background: var(--caution);
  animation: pulse 1.2s ease-in-out infinite;
}
@keyframes pulse {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.3; }
}
@media (prefers-reduced-motion: reduce) {
  .run-dot { animation: none; }
}

.steps { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 4px; }
.step { display: flex; align-items: baseline; gap: 8px; font-size: 0.923rem; padding: 4px 6px; }
.step.tool { background: var(--panel-bg-2); }
.step.fail { background: rgba(255, 107, 97, 0.08); }
.mark { flex: none; }
.mark.model { color: var(--ink); }
.mark.ok { color: var(--ink); }
.mark.bad { color: var(--err); }
.tn { color: var(--ink); flex: none; }
.args { color: var(--dim); font-size: 0.846rem; word-break: break-all; }
.res { color: var(--dim); font-size: 0.769rem; word-break: break-all; }
.think { color: var(--caution); font-size: 0.769rem; cursor: help; }
.tc { color: var(--ink); }
.ans { color: var(--ink); }

.answer { margin-top: 12px; padding: 12px 14px; border: 1px solid var(--border); border-radius: var(--radius); background: var(--panel-bg); }
.answer-label { font-size: 0.923rem; font-weight: 600; color: var(--dim); }
.answer p { margin: 8px 0 0; font-size: 1rem; color: var(--ink); }

.block { margin: 14px auto 0; max-width: 1100px; }

/* ── 右上角工具条（2026-10-09 用户：四件套 + 右侧栏开关）────── */
/* 位置：固定在「主控/小满」面包屑白线下、页头行右侧（同 WorkBuddy：左标题右功能）。
   fixed 而非 absolute——.agent 顶部隔着 page-head（标题+说明 ~90px），absolute 会沉到标题下面 */
.corner-bar {
  position: fixed;
  top: 56px;
  right: 32px;
  display: flex;
  align-items: center;
  gap: 2px;
  z-index: 50;
}
.corner-bar.pushed { right: 332px; } /* 右侧栏开着时给产出物栏让位 */
.corner-btn {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 28px;
  height: 28px;
  background: transparent;
  border: none;
  border-radius: var(--radius-sm);
  color: var(--dim);
  cursor: pointer;
  transition: color 0.12s ease, background 0.12s ease;
}
.corner-btn:hover { color: var(--ink); background: var(--hover); }
.corner-btn.on { color: var(--caution); background: rgba(250, 204, 21, 0.08); }
.corner-sep { width: 1px; height: 14px; background: var(--border); margin: 0 4px; flex: none; }
.pop.corner {
  left: auto;
  right: 0;
  bottom: auto;
  top: calc(100% + 6px);
  width: 340px;
}

/* ── 右侧产出物栏（md/文件/网页，随会话）─────────────────── */
.outbar {
  position: absolute;
  top: 0;
  right: 0;
  bottom: 0;
  width: 306px; /* 实际宽度由内联 style 控制（可拖拽 280~640） */
  background: var(--panel-bg);
  border-left: 1px solid var(--border);
  display: flex;
  flex-direction: column;
  z-index: 40;
}
/* 可拖拽分界线（2026-10-10 用户）：会话区与产出物栏之间，拖动调宽度（right 由内联 style 同步） */
.outbar-resizer {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 8px;
  cursor: col-resize;
  z-index: 41;
}
.outbar-resizer:hover { background: color-mix(in srgb, var(--accent, #4c8bf5) 30%, transparent); }
/* 应用内预览区（2026-10-10 用户「像 WorkBuddy 侧边栏直接打开文件」） */
.outbar-preview {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
  border-top: 1px solid var(--border);
}
.pv-head {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 6px 10px;
  border-bottom: 1px solid var(--border);
}
.pv-name {
  flex: 1;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-size: 0.846rem;
  font-weight: 600;
  color: var(--ink);
}
.pv-open {
  flex: none;
  background: transparent;
  border: none;
  color: var(--dim);
  cursor: pointer;
  font-size: 0.923rem;
  padding: 2px 6px;
  border-radius: var(--radius-sm);
}
.pv-open:hover { background: var(--hover); color: var(--ink); }
.pv-body { flex: 1; min-height: 0; overflow-y: auto; overscroll-behavior: contain; }
.pv-frame { width: 100%; height: 100%; border: none; background: #fff; }
.pv-img { max-width: 100%; display: block; margin: 12px auto; }
.pv-md { padding: 12px 14px; font-size: 0.846rem; line-height: 1.7; color: var(--ink); word-break: break-word; }
.pv-md h1, .pv-md h2, .pv-md h3, .pv-md h4 { margin: 14px 0 8px; color: var(--ink); }
.pv-md h1 { font-size: 1.23rem; } .pv-md h2 { font-size: 1.08rem; } .pv-md h3 { font-size: 1rem; }
.pv-md p { margin: 6px 0; }
.pv-md ul, .pv-md ol { margin: 6px 0; padding-left: 22px; }
.pv-md li { margin: 3px 0; }
.pv-md blockquote { margin: 8px 0; padding: 4px 10px; border-left: 3px solid var(--accent, #4c8bf5); color: var(--dim); background: var(--hover); }
.pv-md code { font-family: var(--font-num); font-size: 0.846rem; background: var(--hover); padding: 1px 5px; border-radius: 4px; }
.pv-md pre.pv-code { margin: 8px 0; padding: 10px; background: var(--hover); border-radius: var(--radius-sm); overflow-x: auto; }
.pv-md pre.pv-code code { background: transparent; padding: 0; }
.pv-md hr { border: none; border-top: 1px solid var(--border); margin: 12px 0; }
.pv-md a { color: var(--accent, #4c8bf5); }
.pv-empty { padding: 28px 14px; text-align: center; color: var(--dim-2); font-size: 0.846rem; display: flex; flex-direction: column; align-items: center; gap: 10px; }
.pv-open-btn { background: var(--hover); border: 1px solid var(--border); color: var(--ink); border-radius: var(--radius-sm); padding: 5px 12px; cursor: pointer; font-size: 0.846rem; }
.pv-open-btn:hover { border-color: var(--accent, #4c8bf5); }
.agent.with-outbar { padding-right: 306px; } /* 兜底；实际由内联 style 控制 */
.outbar-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 12px 14px 10px;
  border-bottom: 1px solid var(--border);
}
.outbar-title { font-size: 0.923rem; font-weight: 600; color: var(--ink); }
.outbar-n { font-family: var(--font-num); font-size: 0.769rem; color: var(--dim-2); }
.outbar-list { flex: none; max-height: 38%; overflow-y: auto; padding: 8px; display: flex; flex-direction: column; gap: 2px; } /* 2026-10-10：列表收缩，预览区吃剩余高度 */
.outbar-item {
  display: flex;
  align-items: center;
  gap: 9px;
  background: transparent;
  border: none;
  border-radius: var(--radius-sm);
  padding: 7px 8px;
  cursor: pointer;
  text-align: left;
  color: var(--dim);
  transition: background 0.12s ease, color 0.12s ease;
}
.outbar-item:hover { background: var(--hover); color: var(--ink); }
.oi-ico {
  flex: none;
  width: 22px;
  height: 22px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  font-size: 0.65rem;
  font-weight: 700;
  font-family: var(--font-num);
  border-radius: 4px;
  background: var(--hover);
  color: var(--caution);
  border: 1px solid var(--border);
}
.oi-ico.网页 { color: #7ab8f5; }
.oi-ico.图片 { color: #8fd694; }
.oi-txt { display: flex; flex-direction: column; gap: 1px; min-width: 0; }
.oi-name { font-size: 0.846rem; color: var(--ink); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.oi-kind { font-size: 0.692rem; color: var(--dim-2); }
.outbar-empty { font-size: 0.808rem; color: var(--dim-2); padding: 14px 10px; line-height: 1.6; }

/* ── 工作过程（WorkBuddy 式：行内暴露工具调用与思考）───────── */
.workproc { margin-top: 8px; flex: none; min-height: 0; max-height: 30vh; overflow-y: auto; } /* 2026-10-10：限高 30vh 可滚，防止工具轨迹撑满、把 chat-msgs 挤成缝导致对话滚不动 */
.wp-head {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  background: transparent;
  border: none;
  padding: 4px 0;
  cursor: pointer;
  color: var(--dim);
}
.wp-state { display: inline-flex; align-items: center; gap: 6px; font-size: 0.846rem; color: var(--dim); }
.wp-state.running { color: var(--caution); }
.wp-state.done { color: var(--dim); }
.wp-spin {
  width: 9px;
  height: 9px;
  border: 1.5px solid var(--caution);
  border-top-color: transparent;
  border-radius: 50%;
  animation: wp-spin 0.9s linear infinite;
}
@keyframes wp-spin { to { transform: rotate(360deg); } }
.wp-dur { font-size: 0.769rem; color: var(--dim-2); font-family: var(--font-num); }
.wp-steps { display: flex; flex-direction: column; gap: 1px; padding: 4px 0 6px; }
/* 实时思考流（2026-10-10 暴露思考过程）：running 时边想边滚动的思考全文 */
.wp-live {
  display: flex;
  gap: 8px;
  align-items: flex-start;
  padding: 8px 10px;
  margin: 2px 0 4px;
  border: 1px solid var(--border);
  border-left: 3px solid var(--accent, #4c8bf5);
  border-radius: var(--radius-sm);
  background: var(--hover);
}
.wp-live-ico { flex: none; color: var(--accent, #4c8bf5); font-size: 0.846rem; line-height: 1.5; }
.wp-live-txt {
  flex: 1;
  min-width: 0;
  min-height: 0; /* flex 子项默认 min-height:auto 会阻止 max-height 生效，必须显式归零 */
  margin: 0;
  white-space: pre-wrap;
  word-break: break-word;
  font-size: 0.846rem;
  line-height: 1.6;
  color: var(--dim);
  max-height: 260px;
  overflow-y: auto;
  overscroll-behavior: contain; /* 滚到边界时不把滚动抛给外层，避免整页/消息区跟着滚 */
}
/* 深度思考段落（2026-10-10）：任务完成后展示在正文上方的思考全文 */
.think-sec {
  margin: 6px 0 10px;
  padding: 10px 12px;
  border: 1px solid var(--border);
  border-left: 3px solid var(--accent, #4c8bf5);
  border-radius: var(--radius-sm);
  background: var(--hover);
}
.think-sec-head {
  font-size: 0.846rem;
  font-weight: 600;
  color: var(--accent, #4c8bf5);
  margin-bottom: 6px;
}
.think-sec-body {
  margin: 0;
  white-space: pre-wrap;
  word-break: break-word;
  font-size: 0.846rem;
  line-height: 1.6;
  color: var(--dim);
  max-height: 320px;
  overflow-y: auto;
  overscroll-behavior: contain;
}
.wp-row {
  display: flex;
  align-items: baseline;
  gap: 8px;
  background: transparent;
  border: none;
  padding: 3px 6px;
  cursor: pointer;
  text-align: left;
  border-radius: var(--radius-sm);
  color: var(--dim);
  font-size: 0.846rem;
  max-width: 100%;
}
.wp-row:hover { background: var(--hover); color: var(--ink); }
.wp-row.bad .wp-title { color: var(--err); }
.wp-ico { flex: none; color: var(--ink-3); font-size: 0.769rem; }
.wp-title { flex: none; }
.wp-target {
  min-width: 0;
  flex: 1;
  font-family: var(--font-num);
  font-size: 0.769rem;
  color: var(--ink-3);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.wp-ms { flex: none; font-family: var(--font-num); font-size: 0.692rem; color: var(--dim-2); }
.wp-detail {
  margin: 0 0 4px 22px;
  padding: 8px 10px;
  font-family: var(--font-num);
  font-size: 0.769rem;
  line-height: 1.6;
  color: var(--dim);
  background: var(--panel-bg-2);
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  white-space: pre-wrap;
  word-break: break-word;
  max-height: 220px;
  overflow-y: auto;
}
</style>


