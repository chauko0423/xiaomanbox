import type { Component } from 'vue';

/**
 * 面板注册项 —— 「面板注册」的落点（立项 §6 M0 壳）。
 * 2026-10-07 单项专精改造：一个页面只渲染一个面板（全宽专精），
 * 12 列网格取消，网格占位 span 与装饰图标字段一并移除
 * （侧栏图标按面板 id 从 AppIcon 取线性几何图标）。
 */
export interface PanelRegistration {
  id: string;
  title: string;
  component: Component;
  /** 一句话说明：显示在专精页页头，条目要能自己说清是干什么的 */
  desc?: string;
  /** 占位面板（未接真实数据源）——页头会打「占位 · 未接 AI」标 */
  placeholder?: boolean;
}

/**
 * 侧栏导航分组 —— 详细侧栏（参考 WorkBuddy / 偏好设置 / UNREAL BOX）：
 * 组间细分割线，条目 = 线性图标 + 名称，当前项左轨警戒黄。
 * （场景/预设两级结构见 api/contract.ts 的 PresetCategory / PresetInfo，数据源在后端。）
 */
export interface NavGroupDef {
  id: string;
  name: string;
  /** 组内面板 id（registry 键），顺序即显示顺序 */
  items: string[];
}
