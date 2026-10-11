<script setup lang="ts">
import { computed } from 'vue';

/**
 * 线性几何图标（终末地：技术图纸感 → 方角端点 + 尖角连接，不用 emoji）。
 * 以面板 id 为键：侧栏直接 <AppIcon :name="panelId" />，
 * 加一个面板 = 在 ICONS 里加一组路径，映射不上时退化为空心方块（能看出缺）。
 */
const props = withDefaults(defineProps<{ name: string; size?: number }>(), { size: 15 });

const ICONS: Record<string, string[]> = {
  // 小满 Agent：四芒星（指向「主动出击」的那个星标）
  agent: ['M12 3 L14 10 L21 12 L14 14 L12 21 L10 14 L3 12 L10 10 Z'],
  // 会话绑定：链环
  session: [
    'M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71',
    'M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71',
  ],
  // 全局搜索：放大镜
  search: ['M17 11a6 6 0 1 1-12 0a6 6 0 0 1 12 0', 'M15.5 15.5L21 21'],
  // UE5 自动拉起：电源符
  'ue5-boot': ['M12 3v8', 'M17.66 6.34a8 8 0 1 1-11.32 0'],
  // 引擎档位：芯片
  engine: [
    'M5 5h14v14H5z',
    'M9.5 9.5h5v5h-5z',
    'M9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 15h3M19 9h3M19 15h3',
  ],
  // 工具总线：四挂载位
  toolbus: [
    'M4 4h6.5v6.5H4z',
    'M13.5 4H20v6.5h-6.5z',
    'M4 13.5h6.5V20H4z',
    'M13.5 13.5H20V20h-6.5z',
  ],
  // 团队交流：气泡
  chat: ['M4 5h16v11H10l-6 4z'],
  // 成员发现：双人
  members: [
    'M11.5 8a3 3 0 1 1-6 0a3 3 0 0 1 6 0',
    'M3 19.5c0-3 2.5-5.5 5.5-5.5s5.5 2.5 5.5 5.5',
    'M15.5 5.5a3 3 0 0 1 0 5.6',
    'M16.5 14.4c2.6.6 4.5 2.6 4.5 5.1',
  ],
  // 团队画板：画框 + 分栏
  teamboard: ['M4 4h16v14H4z', 'M9 4v14'],
  // 蓝图画布：两个节点卡 + 一条连线
  blueprint: [
    'M4 4h8v6H4z',
    'M13 14h7v6h-7z',
    'M12 7h4.5',
    'M16.5 7v10',
  ],
  // 外部监工：一只眼睛（监督）
  supervisor: [
    'M3 12c2.5-4 6-6 9-6s6.5 2 9 6c-2.5 4-6 6-9 6s-6.5-2-9-6z',
    'M12 15a3 3 0 1 0 0-6a3 3 0 0 0 0 6',
  ],
  // 账户：单人
  account: ['M15 8a3 3 0 1 1-6 0a3 3 0 0 1 6 0', 'M5 20c0-3.5 3-6 7-6s7 2.5 7 6'],
  // 会话卡片网格：两行卡槽
  'card-grid': ['M4 5h16v5H4z', 'M4 13h16v5H4z'],
  // 思考可解释：翻开的书
  explain: [
    'M4 19.5A2.5 2.5 0 0 1 6.5 17H20',
    'M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z',
  ],
  // 系统状态：水位折线
  'sys-status': ['M3 12h4l3-8 4 16 3-8h4'],
  // 熵面板：双环（分布交叠）
  entropy: ['M14 12a5 5 0 1 1-10 0a5 5 0 0 1 10 0', 'M20 12a5 5 0 1 1-10 0a5 5 0 0 1 10 0'],
  // 驻留角色：六边舱位 + 人位
  resident: ['M12 2.5L20 7v10l-8 4.5L4 17V7z', 'M12 14.5a2.5 2.5 0 1 1 0-5a2.5 2.5 0 0 1 0 5'],
  // 模型配置：双滑轨 + 方块滑块（调档位）
  model: ['M4 8h16', 'M4 16h16', 'M9 5.5v5', 'M15 13.5v5'],
  // 插件：拼图块（四边凸榫的外接方）
  plugins: ['M4 9h5V4h6v5h5v6h-5v5H9v-5H4z', 'M9 9h6v6H9z'],
  // Skill 社区：宝石（能力包）
  skills: ['M12 3l7 6-7 12L5 9z', 'M5 9h14', 'M12 3l-3.5 6L12 21l3.5-12L12 3z'],
  // 节点参考库：三个节点 + 连线（蓝图图）
  nodes: [
    'M6 6h5v5H6z',
    'M13 13h5v5h-5z',
    'M6 16h5v2H6z',
    'M13 6h2v7h-2z',
    'M8.5 8.5v3h2',
    'M8.5 8.5h3',
  ],
  // 资料库：抽屉柜（两列 + 把手）
  vault: [
    'M4 4h16v16H4z',
    'M4 12h16',
    'M12 12v8',
    'M8 15h1.5M14.5 15H16',
  ],

  // ── 预设（2026-10-08）：大板块 + 小板块的线性图标，键 = presets.mjs 里的 icon 值 ──
  // 大板块
  'preset-daily': ['M7 9a3 3 0 1 1 6 0a3 3 0 0 1-6 0', 'M3 19.5c0-3 2.5-5.5 5.5-5.5h1', 'M15.5 9.2a3 3 0 0 1 0 5.6', 'M17 15.5c1.6.6 2.5 2 2.5 3.5'], // 日常：双人对话
  'preset-ue5': [
    'M12 3l7 4.5v9L12 21l-7-4.5v-9z',
    'M12 3v9',
    'M5 7.5l7 4.5l7-4.5',
    'M12 21v-9',
  ], // UE5：等轴立方
  'preset-design': ['M12 3a3 3 0 1 0 0 6a3 3 0 0 0 0-6', 'M12 6v3', 'M12 9l7 12H5z'], // 策划：靶 + 箭头
  // 日常助手 小板块
  'preset-chat': ['M4 5h16v11H10l-6 4z'], // 日常聊天：气泡
  'preset-task': ['M6 4h12v3H6z', 'M6 10h12v3H6z', 'M6 16h7v3H6z', 'M4 4v16'], // 任务分配：清单 + 左轨
  'preset-brainstorm': [
    'M12 3.5a3.5 3.5 0 1 0 0 7a3.5 3.5 0 0 0 0-7',
    'M5.5 14.5a3.5 3.5 0 1 0 0 7a3.5 3.5 0 0 0 0-7',
    'M18.5 14.5a3.5 3.5 0 1 0 0 7a3.5 3.5 0 0 0 0-7',
    'M8 13l3-4',
    'M16 13l-3-4',
    'M14 16.5l-2.5 2.5',
  ], // 头脑风暴：三气泡 + 连线
  // UE5 开发 小板块
  'preset-ueproject': ['M4 4h16v16H4z', 'M4 9h16', 'M9 9v11'], // UE 工程：工程窗口分栏
  'preset-blueprint': [
    'M6 5h5v5H6z',
    'M13 14h5v5h-5z',
    'M6 16h2v2H6z',
    'M14 5h2v6h-2z',
    'M8.5 7.5v3h2',
    'M8.5 7.5h2',
  ], // 蓝图编程：节点 + 连线
  'preset-level': [
    'M3 20h18',
    'M5 20l4-8l3 5l2.5-3l4.5 6',
    'M14 6l1-3l1 3l3 1l-3 1l-1 3l-1-3l-3-1z',
  ], // 关卡搭建：山峦 + 星标
  'preset-engine': ['M5 5h14v14H5z', 'M9.5 9.5h5v5h-5z', 'M9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 15h3M19 9h3M19 15h3'], // 引擎运维：芯片
  // 游戏策划 小板块
  'preset-gdesign': ['M4 5h16v14H4z', 'M8 5v14', 'M16 5v14', 'M4 12h16'], // 游戏策划：棋盘网格
  'preset-narrative': [
    'M4 19.5A2.5 2.5 0 0 1 6.5 17H20',
    'M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z',
    'M9 7h6M9 11h6',
  ], // 剧情文案：书 + 文行
  'preset-balance': ['M12 4v16', 'M8 20h8', 'M7 8l5-3l5 3l-5 3z', 'M4 8h16'], // 数值平衡：天平
};

const paths = computed(() => ICONS[props.name] ?? ['M5 5h14v14H5z']);
</script>

<template>
  <svg
    :width="size"
    :height="size"
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    stroke-width="1.6"
    stroke-linecap="square"
    stroke-linejoin="miter"
    aria-hidden="true"
  >
    <path v-for="(d, i) in paths" :key="i" :d="d" />
  </svg>
</template>
