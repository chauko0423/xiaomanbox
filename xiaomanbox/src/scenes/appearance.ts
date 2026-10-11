import { computed, reactive } from 'vue';
import { loadState, patchState } from '../api/state';

/**
 * 外观系统（2026-10-08，用户：自定义配色 / 字体 / 字号）。
 *
 * - 唯一真源：ui.json 的 appearance 字段（W-07 铁律：状态存文件系统，不存浏览器）；
 * - 应用方式：改 documentElement 上的 CSS 变量（inline 优先级高于 :root 规则），
 *   强调色四件套（--caution / -rgb / -deep / -mid）+ --font-ui + --font-size；
 * - 语义状态色（--err/--ok/--warn）不参与换肤——错误就该是红的（style.css 铁律）。
 */

export interface AccentPreset {
  id: string;
  name: string;
  /** 主信号色 */
  caution: string;
  /** rgba 半透明底用 */
  rgb: string;
  /** 悬停加深一档 */
  deep: string;
  /** 暗色「亮带」压暗 */
  mid: string;
}

/** 强调色预设（6 个，全部按「暗底信号色」调校：够亮但不过曝） */
export const ACCENTS: AccentPreset[] = [
  { id: 'gold', name: '沙金', caution: '#e6d16a', rgb: '230, 209, 106', deep: '#cfb852', mid: '#8a7a3c' },
  { id: 'teal', name: '青碧', caution: '#5ad0c0', rgb: '90, 208, 192', deep: '#46b3a5', mid: '#2e7a70' },
  { id: 'blue', name: '天青', caution: '#6aa8e6', rgb: '106, 168, 230', deep: '#5288c4', mid: '#37597e' },
  { id: 'purple', name: '紫晶', caution: '#b48ae6', rgb: '180, 138, 230', deep: '#9670c4', mid: '#634a82' },
  { id: 'ember', name: '橘焰', caution: '#e68a5a', rgb: '230, 138, 90', deep: '#c4703f', mid: '#7e4a2a' },
  { id: 'jade', name: '翠玉', caution: '#7dd06a', rgb: '125, 208, 106', deep: '#63b350', mid: '#427636' },
];

export interface FontPreset {
  id: string;
  name: string;
  /** 完整 fallback 栈（首个为候选主字体，未装则逐级回落） */
  stack: string;
}

/** 字体预设（本地已装哪个走哪个，栈尾兜底 system-ui） */
export const FONTS: FontPreset[] = [
  {
    id: 'default',
    name: '系统默认',
    stack: "'HarmonyOS Sans SC', 'MiSans', 'Noto Sans SC', 'Source Han Sans SC', 'PingFang SC', 'Microsoft YaHei', system-ui, sans-serif",
  },
  {
    id: 'yahei',
    name: '微软雅黑',
    stack: "'Microsoft YaHei', 'PingFang SC', 'Noto Sans SC', system-ui, sans-serif",
  },
  {
    id: 'sourcehan',
    name: '思源黑体',
    stack: "'Source Han Sans SC', 'Noto Sans SC', 'MiSans', 'PingFang SC', system-ui, sans-serif",
  },
  {
    id: 'mono',
    name: '等宽（工程感）',
    stack: "'Cascadia Mono', 'JetBrains Mono', Consolas, 'Microsoft YaHei', monospace",
  },
];

export const FONT_MIN = 11;
export const FONT_MAX = 16;
export const FONT_DEFAULT = 13;

export interface Appearance {
  accent: string;
  font: string;
  fontSize: number;
}

const DEFAULTS: Appearance = { accent: 'gold', font: 'default', fontSize: FONT_DEFAULT };

const state = reactive({
  accent: DEFAULTS.accent,
  font: DEFAULTS.font,
  fontSize: DEFAULTS.fontSize,
  loaded: false,
});

/** 当前强调色预设（未加载/未知 id 回落沙金） */
export const currentAccent = computed<AccentPreset>(
  () => ACCENTS.find((a) => a.id === state.accent) ?? ACCENTS[0]
);

/** 把当前外观写进 CSS 变量（同步、立即可见） */
function applyAppearance(): void {
  const a = currentAccent.value;
  const root = document.documentElement.style;
  root.setProperty('--caution', a.caution);
  root.setProperty('--caution-rgb', a.rgb);
  root.setProperty('--caution-deep', a.deep);
  root.setProperty('--caution-mid', a.mid);
  const f = FONTS.find((x) => x.id === state.font) ?? FONTS[0];
  root.setProperty('--font-ui', f.stack);
  const size = Math.min(FONT_MAX, Math.max(FONT_MIN, state.fontSize));
  root.setProperty('--font-size', `${size}px`);
}

/** 启动时恢复：读 ui.json 的 appearance 字段并应用（失败保持默认） */
export async function loadAppearance(): Promise<void> {
  try {
    const saved = await loadState<{ appearance?: Partial<Appearance> }>('ui');
    const ap = saved?.appearance;
    if (ap) {
      if (ACCENTS.some((a) => a.id === ap.accent)) state.accent = ap.accent as string;
      if (FONTS.some((f) => f.id === ap.font)) state.font = ap.font as string;
      if (typeof ap.fontSize === 'number') state.fontSize = ap.fontSize;
    }
  } catch {
    /* 后端离线：保持默认 */
  } finally {
    applyAppearance();
    state.loaded = true;
  }
}

/** 通用保存：应用 + 持久化到 ui.json（patch 合并写，不冲掉其它字段） */
function persist(): void {
  applyAppearance();
  patchState('ui', {
    appearance: { accent: state.accent, font: state.font, fontSize: state.fontSize },
  }).catch(() => {});
}

export function setAccent(id: string): void {
  if (!ACCENTS.some((a) => a.id === id)) return;
  state.accent = id;
  persist();
}

export function setFont(id: string): void {
  if (!FONTS.some((f) => f.id === id)) return;
  state.font = id;
  persist();
}

export function setFontSize(px: number): void {
  state.fontSize = Math.min(FONT_MAX, Math.max(FONT_MIN, px));
  persist();
}

/** 恢复默认外观 */
export function resetAppearance(): void {
  state.accent = DEFAULTS.accent;
  state.font = DEFAULTS.font;
  state.fontSize = DEFAULTS.fontSize;
  persist();
}

/** 只读快照（设置弹窗用） */
export const appearanceState = computed(() => ({
  accent: state.accent,
  font: state.font,
  fontSize: state.fontSize,
}));
