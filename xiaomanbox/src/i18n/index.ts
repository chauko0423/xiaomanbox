import { computed, reactive } from 'vue';
import { loadState, patchState } from '../api/state';

/**
 * 轻量 i18n（2026-10-09，用户：全应用双语）。
 *
 * 设计（务实、可增量）：
 *  - 默认中文。界面文案以「中文」为原文（key），英文是映射值。
 *  - t(text) 查 zh→en 词典：命中返回英文，未命中返回原文（中文兜底，不出现空白/漏翻）。
 *  - 语言状态存 ui.json 的 lang 字段（W-07 铁律：状态存文件系统，不存浏览器）。
 *  - 切换即时生效：state.lang 是响应式，模板里用 t() 的地方会自动重渲染。
 *
 * 词典分两块：
 *  - NAV_DICT：导航层（分组名 + 面板 title/desc + 面包屑），改动小、直接覆盖。
 *  - 各面板内部文案：逐步把「中文原文 → 英文」补进 EN 词典，未补到的兜底显示中文。
 */
export type Lang = 'zh' | 'en';

const state = reactive<{ lang: Lang; loaded: boolean }>({ lang: 'zh', loaded: false });

/** 中文原文 → 英文（key 为中文，命中即翻，未命中兜底中文） */
const EN: Record<string, string> = {};

/** 注册一批翻译（中文原文为 key）。多处复用同一文案时只存一份。 */
export function addTranslations(entries: Record<string, string>): void {
  for (const [zh, en] of Object.entries(entries)) {
    if (!(zh in EN)) EN[zh] = en;
  }
}

/** 翻译：英文模式下查词典，命中返回英文，未命中返回原文（中文兜底） */
export function t(text: string): string {
  if (state.lang === 'zh') return text;
  return EN[text] ?? text;
}

/** 当前语言 */
export const currentLang = computed<Lang>(() => state.lang);

/** 是否英文 */
export const isEnglish = computed<boolean>(() => state.lang === 'en');

/** 设置语言（即时生效 + 持久化到 ui.json） */
export function setLang(lang: Lang): void {
  if (lang !== 'zh' && lang !== 'en') return;
  state.lang = lang;
  document.documentElement.lang = lang === 'zh' ? 'zh-CN' : 'en';
  patchState('ui', { lang }).catch(() => {});
}

/** 启动时恢复语言（失败保持默认中文） */
export async function loadLang(): Promise<void> {
  try {
    const saved = await loadState<{ lang?: Lang }>('ui');
    if (saved?.lang === 'zh' || saved?.lang === 'en') {
      state.lang = saved.lang;
    }
  } catch {
    /* 后端离线：保持默认 */
  } finally {
    document.documentElement.lang = state.lang === 'zh' ? 'zh-CN' : 'en';
    state.loaded = true;
  }
}
