import { computed, reactive } from 'vue';
import { NAV_CTA } from './config';
import type { PanelRegistration } from './types';
import type { PresetCategory, PresetInfo } from '../api/contract';
import { getPanel } from '../panels/registry';
import { loadState, patchState } from '../api/state';
import { fetchPresets } from '../api/presets';
import { bindScene } from '../api/session';

/**
 * 壳状态 store。
 *
 * W-07 状态持久化（铁律 #3：状态存文件系统，不存浏览器）：
 *  - 启动时从后端 `~/.workbench/ui.json` 恢复（initSceneState）；
 *  - 切功能即写回（fire-and-forget，失败静默降级）；
 *  - 绝不写 localStorage。
 *
 * 2026-10-08 预设改造：
 *  - 「场景」升级为 WorkBuddy 式两级预设（大板块 → 小板块，见 backend/presets.mjs）；
 *  - 会话绑定域（M1 魂）不变：pinTarget = 预设 id，切预设 = 切会话域；
 *  - 预设清单从后端拉取（唯一真源，含各模式专属系统提示词），本文件不再维护静态表。
 */

/** 旧场景 id → 新预设 id（ui.json 里可能存着旧值，启动时迁移） */
const OLD_SCENE_ALIAS: Record<string, string> = {
  'ue5-dev': 'ue5-project',
  'control-room': 'daily-chat',
};

/** 后端离线时的兜底预设（须与 backend/presets.mjs 的 DEFAULT_PRESET_ID 一致） */
const FALLBACK_PRESET: PresetInfo = {
  id: 'ue5-project',
  category: 'ue5',
  name: 'UE 工程',
  icon: '🎮',
  desc: '工程管理 / 引擎内任务',
};

const state = reactive({
  currentSceneId: FALLBACK_PRESET.id,
  currentSessionId: null as string | null,
  activePanelId: NAV_CTA,
  /** 新对话意向：true = 用户点「唤起 Agent」要一个全新空对话栏，尚未建会话 */
  newChatPending: false,
  /** 会话列表版本号：任何「会话清单可能变化」的动作自增，侧栏 watch 它刷新工具栏 */
  sessionRev: 0,
  /** Agent 对话模式（2026-10-10 用户）：true = 对话流进行中，App.vue 据此隐藏大页头——
   *  会话内容要从面包屑白线下面直接顶满（大标题+副标题不再占垂直空间） */
  agentChatMode: false,
});

/** 预设清单（后端唯一真源；拉取失败保持空 → 选择器隐藏、currentScene 走兜底） */
const presetState = reactive({
  categories: [] as PresetCategory[],
  presets: [] as PresetInfo[],
  defaultPresetId: FALLBACK_PRESET.id,
  loaded: false,
});

/** 会话清单可能变化（新建/换绑/归档等）时自增，驱动侧栏「最近会话」自动刷新 */
export function bumpSessions(): void {
  state.sessionRev += 1;
}

/** 拉取预设清单（启动时一次）。失败静默：选择器隐藏，currentScene 走兜底。 */
async function loadPresets(): Promise<void> {
  try {
    const r = await fetchPresets();
    presetState.categories = r.categories ?? [];
    presetState.presets = r.presets ?? [];
    presetState.defaultPresetId = r.defaultPresetId || FALLBACK_PRESET.id;
  } catch {
    /* 后端离线：保持空表 */
  } finally {
    presetState.loaded = true;
  }
}

/** （内部）校验/迁移一个已保存的场景 id：合法预设 → 原样；旧场景 id → 迁移；其余 → 默认 */
function resolvePresetId(saved: string | undefined): string {
  if (saved) {
    if (presetState.presets.some((p) => p.id === saved)) return saved;
    if (OLD_SCENE_ALIAS[saved] && presetState.presets.some((p) => p.id === OLD_SCENE_ALIAS[saved])) {
      return OLD_SCENE_ALIAS[saved];
    }
  }
  const def = presetState.defaultPresetId;
  return presetState.presets.some((p) => p.id === def) ? def : FALLBACK_PRESET.id;
}

/** 切换专精页：主区只渲染这一个功能，并持久化到 ui.json */
export function switchFunction(id: string): void {
  if (!getPanel(id)) return; // 注册表里没有的 id 不接（防空指向白屏）
  state.activePanelId = id;
  patchState('ui', { currentSceneId: state.currentSceneId, activePanelId: id }).catch(() => {});
}

/** 应用启动时调用：拉预设清单 + 恢复上次功能与预设域 + 绑定该域会话 */
export async function initSceneState(): Promise<void> {
  await loadPresets();
  try {
    const saved = await loadState<{ currentSceneId?: string; activePanelId?: string }>('ui');
    state.currentSceneId = resolvePresetId(saved?.currentSceneId);
    if (saved?.activePanelId && getPanel(saved.activePanelId)) {
      state.activePanelId = saved.activePanelId;
    }
  } catch {
    /* 后端离线：保持默认预设 */
  }
  // 2026-10-08 用户：启动默认「全新欢迎页」——不自动续接上次会话，也不在后端拉起/新建绑定会话
  // （绑定表保持原样：旧会话仍在侧栏，点它随时回去；currentSessionId=null = 空对话栏，
  // 首条消息发出时才建新会话 settleNewChat 重绑。运行中的任务由 AgentPanel 按 job 状态自行恢复现场。）
  state.newChatPending = true;
  state.currentSessionId = null;
}

/** 当前会话绑定域（M1 魂底层概念）。预设未加载/后端离线时返回兜底，保证 .id/.name 永不缺。 */
export const currentScene = computed<PresetInfo>(() => {
  return presetState.presets.find((p) => p.id === state.currentSceneId) ?? FALLBACK_PRESET;
});

export const currentSessionId = computed(() => state.currentSessionId);

/** 会话清单版本号（只读）：SideNav watch 它刷新「最近会话」工具栏 */
export const sessionRev = computed(() => state.sessionRev);
export const agentChatMode = computed(() => state.agentChatMode);
export function setAgentChatMode(v: boolean) {
  state.agentChatMode = v;
}

/** 预设大板块（选择器第一行数据源） */
export const presetCategories = computed(() => presetState.categories);

/** 预设小板块全集（选择器第二行按 category 过滤） */
export const presetList = computed(() => presetState.presets);

/** 预设清单是否已加载（false = 后端没拉到，选择器可隐藏） */
export const presetsLoaded = computed(() => presetState.loaded);

/** 切换预设（选择器点选）：切会话域 + 回到「全新对话栏」（2026-10-08 用户）。
 *  不再跳进该预设绑定的旧会话（旧会话可能已归档，且欢迎页选预设的预期是「换模式、留在欢迎页」）。
 *  绑定表不动：旧会话仍在侧栏可点；发首条消息时 settleNewChat 重绑新会话。 */
export function switchScene(presetId: string): void {
  if (presetId === state.currentSceneId) return;
  if (!presetState.presets.some((p) => p.id === presetId)) return; // 清单里没有的 id 不接（防空指向）
  state.currentSceneId = presetId;
  state.newChatPending = true; // 切预设 = 该模式下的全新对话栏
  state.currentSessionId = null;
  patchState('ui', { currentSceneId: presetId, activePanelId: state.activePanelId }).catch(() => {});
}

/**
 * 把当前场景换绑到指定会话（会话抽屉「切换/新建」用）。
 * 后端 bindScene 落绑定表，这里同步刷新 currentSessionId —— 下一条消息即进该会话。
 */
export async function rebindSceneSession(sceneId: string, sessionId: string): Promise<void> {
  try {
    await bindScene(sceneId, sessionId);
    state.currentSessionId = sessionId;
    bumpSessions();
  } catch {
    /* 后端离线：保持原绑定 */
  }
}

/**
 * 侧栏会话切换（2026-10-07）：把当前场景换绑到指定会话，并跳到 Agent 面板续接。
 * 由 SideNav 直接调用 —— 侧栏点会话 = 切会话 + 回对话现场。
 */
export async function switchToSession(sessionId: string): Promise<void> {
  if (sessionId === state.currentSessionId) {
    // 已在当前会话：只回 Agent 面板，不重绑
    state.newChatPending = false;
    switchFunction(NAV_CTA);
    return;
  }
  await rebindSceneSession(state.currentSceneId, sessionId);
  state.newChatPending = false;
  switchFunction(NAV_CTA);
}

/**
 * 唤起 Agent = 新对话栏（2026-10-07）。
 * 不创建会话、不绑场景：currentSessionId 置 null 表示「空对话栏、待发送」。
 * 只有发送第一条消息开始任务时，才由 AgentPanel 创建会话并正式并入侧栏工具栏。
 */
export function openNewChat(): void {
  state.newChatPending = true;
  state.currentSessionId = null;
  switchFunction(NAV_CTA);
}

/** 是否处于「新对话栏（尚未建会话）」态 */
export const newChatPending = computed(() => state.newChatPending);

/** 新对话落地：发送首条消息时由 AgentPanel 调用，把场景绑定到新会话并清除意向态 */
export async function settleNewChat(sceneId: string, sessionId: string): Promise<void> {
  await rebindSceneSession(sceneId, sessionId);
  state.newChatPending = false;
  bumpSessions(); // rebindSceneSession 已 bump，这里再兜一次确保新建会话侧栏必刷
}

/** 当前专精页（注册表兜底：agent 一定在，防 ui.json 写坏白屏） */
export const activePanel = computed<PanelRegistration>(() => {
  return getPanel(state.activePanelId) ?? getPanel(NAV_CTA)!;
});
