import type { NavGroupDef } from './types';
import { listPanels } from '../panels/registry';

/**
 * 场景配置 —— 2026-10-08 预设改造后，本文件只管侧栏导航。
 * 「场景/预设」升级为 WorkBuddy 式两级结构，数据源 = backend/presets.mjs
 * （经 GET /presets 下发，scenes/store.ts 拉取），静态表已删除。
 */

/**
 * 侧栏顶部主按钮（终末地主色实心按钮，同 UNREAL BOX 的「新对话」）：
 * 指向最常用的功能 —— 唤起小满 Agent。
 */
export const NAV_CTA = 'agent';

/**
 * 侧栏分组导航（参考 WorkBuddy / 偏好设置 / UNREAL BOX 三张截图）：
 *   组间细分割线 · 条目 = 线性图标 + 名称 · 当前项左轨警戒黄。
 * 一页 = 一个专精功能：点哪个，主区就只渲染哪个。
 */
export const NAV_GROUPS: NavGroupDef[] = [
  { id: 'work', name: '工作', items: ['session', 'search', 'supervisor'] },
  { id: 'ue5', name: 'UE5 开发', items: ['ue5-boot', 'engine', 'toolbus', 'blueprint', 'vault'] },
  { id: 'board', name: '本地画板', items: ['teamboard'] },
  { id: 'control', name: '控制室', items: ['card-grid', 'explain', 'sys-status', 'entropy', 'resident'] },
  { id: 'settings', name: '设置', items: ['model', 'plugins'] },
];

/** 面板 → 所属分组（agent 是主按钮功能，不在分组里） */
export function groupOf(panelId: string): NavGroupDef | undefined {
  return NAV_GROUPS.find((g) => g.items.includes(panelId));
}

/** 侧栏全部可达功能（主按钮 + 分组条目），按显示顺序 */
export const NAV_ALL: string[] = [NAV_CTA, ...NAV_GROUPS.flatMap((g) => g.items)];

// ── 负控（规矩 1：判据必须能证明自己在失败时会报错）──────────────
// 侧栏是唯一的导航入口：NAV_ALL 与注册表不一致 = 功能不可达 / 空指向。
// 这是最阴的 bug（编译照过、功能静默消失），在加载时就喊出来。
{
  const registryIds = listPanels().map((p) => p.id);
  const missing = registryIds.filter((id) => !NAV_ALL.includes(id));
  const ghost = [...new Set(NAV_ALL.filter((id) => !registryIds.includes(id)))];
  if (missing.length || ghost.length) {
    console.error(
      `[nav] 负控失败：侧栏与面板注册表不一致 | ` +
        `不可达（注册表有、侧栏没有）: [${missing}] | ` +
        `空指向（侧栏有、注册表没有）: [${ghost}]`
    );
  }
  const dup = [...new Set(NAV_ALL.filter((id, i) => NAV_ALL.indexOf(id) !== i))];
  if (dup.length) {
    console.error(`[nav] 负控失败：侧栏条目重复: [${dup}]（重复条目会互相覆盖高亮）`);
  }
}
