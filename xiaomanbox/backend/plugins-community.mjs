/**
 * 插件社区（2026-10-09）—— dsh 插件生态的内嵌能力目录。
 *
 * ★ 零外部依赖铁律：运行时只读内嵌的 data/dsh-plugins.json（一次性固化快照），
 *   不联网拉 npm、不 import 任何非 Node 内置模块。dsh 的插件（Cordis 代码插件）
 *   无法直接在小满里运行，故以「能力目录 + 等价工具映射」的形式接入：
 *   - adapted  = 小满已有等价工具（直接能用）
 *   - mappable = 能力明确、待补对应工具
 *   - n-a      = 依赖 dsh/Cordis 特有机制，不适用
 */
import fs from 'node:fs';
import { detectDsh } from './dsh-bridge.mjs';

// 种子数据：固化快照。缺失时降级为空目录（2026-10-10：打包安装后 data 种子缺失不再崩整个后端）
let DATA = { plugins: [], source: 'embedded', snapshotAt: null, total: 0, categories: [], adaptCounts: {} };
try {
  DATA = JSON.parse(
    fs.readFileSync(new URL('./data/dsh-plugins.json', import.meta.url), 'utf8'),
  );
} catch (e) {
  console.warn('[plugins-community] data/dsh-plugins.json 缺失，插件社区降级为空目录：', String(e.message || e).slice(0, 120));
}

/** 插件社区清单：支持 category / adapt / q（名称或描述关键字）过滤 */
export function listCommunity({ category, adapt, q } = {}) {
  let plugins = DATA.plugins;
  if (category) plugins = plugins.filter((p) => p.category === category);
  if (adapt) plugins = plugins.filter((p) => p.adapt === adapt);
  if (q && String(q).trim()) {
    const kw = String(q).trim().toLowerCase();
    plugins = plugins.filter(
      (p) =>
        p.name.toLowerCase().includes(kw) ||
        p.description.toLowerCase().includes(kw) ||
        (p.descriptionZh || '').toLowerCase().includes(kw) ||
        (p.mapsTo || []).some((m) => m.toLowerCase().includes(kw)),
    );
  }
  const runtime = detectDsh();
  return {
    ok: true,
    source: DATA.source,
    snapshotAt: DATA.snapshotAt,
    total: DATA.total,
    categories: DATA.categories,
    adaptCounts: DATA.adaptCounts,
    runtime, // 路线 B 全局状态（dsh 运行时检测）
    count: plugins.length,
    plugins: plugins.map((p) => ({
      ...p,
      // 双路状态（2026-10-09 双路并行：0依赖自研 + 外部接入原生 dsh）
      routeA: { adapt: p.adapt, mapsTo: p.mapsTo, note: p.note },
      routeB: runtime.available
        ? { available: true, note: 'dsh 原生插件，可走外部接入' }
        : { available: false, note: '需先安装 dsh 运行时' },
    })),
  };
}

/** 可落到小满身上的工具映射表（已适配 + 可映射）——给「能力映射」视图用 */
export function listToolMapping() {
  const rows = DATA.plugins
    .filter((p) => p.adapt === 'adapted' || p.adapt === 'mappable')
    .map((p) => ({
      name: p.name,
      adapt: p.adapt,
      mapsTo: p.mapsTo,
      note: p.note,
      description: p.description,
    }));
  return {
    ok: true,
    count: rows.length,
    rows,
  };
}
