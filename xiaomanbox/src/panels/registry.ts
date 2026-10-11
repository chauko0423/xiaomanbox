import type { PanelRegistration } from '../scenes/types';
import AgentPanel from './AgentPanel.vue';
import Ue5BootPanel from './Ue5BootPanel.vue';
import SessionPanel from './SessionPanel.vue';
import EnginePanel from './EnginePanel.vue';
import ToolBusPanel from './ToolBusPanel.vue';
import SysStatusPanel from './SysStatusPanel.vue';
import EntropyPanel from './EntropyPanel.vue';
import ResidentPanel from './ResidentPanel.vue';
import CardGridPanel from './CardGridPanel.vue';
import SearchPanel from './SearchPanel.vue';
import SupervisorPanel from './SupervisorPanel.vue';
import ExplainPanel from './ExplainPanel.vue';
import TeamBoardPanel from './TeamBoardPanel.vue';



import ModelPanel from './ModelPanel.vue';
import PluginsPanel from './PluginsPanel.vue';

import BlueprintCanvasPanel from './BlueprintCanvasPanel.vue';
import VaultPanel from './VaultPanel.vue';

/**
 * 面板注册表 —— 唯一事实来源。
 * 新增一个面板 = 在这里 register 一行 + 在 config.ts 的 NAV_GROUPS 里加进某个分组
 * （加载时负控会核对两边一致，漏了会在控制台直接报「不可达」）。
 *
 * 2026-10-07 单项专精改造：一页只渲染一个面板（全宽），
 * 网格占位（span）字段移除；侧栏图标按面板 id 从 AppIcon 取。
 */
const registry = new Map<string, PanelRegistration>();

function register(p: PanelRegistration): void {
  if (registry.has(p.id)) {
    console.warn(`[panel] 重复注册面板 id=${p.id}，后注册覆盖`);
  }
  registry.set(p.id, p);
}

// ── 主按钮（侧栏顶部通栏 CTA）────────────────────────────────
register({
  id: 'agent',
  title: '小满 Agent',
  desc: '本地模型自主规划、调工具、核对结果',
  component: AgentPanel,
});

// ── 工作 ─────────────────────────────────────────────────────
register({
  id: 'session',
  title: '会话绑定（魂）',
  desc: '场景↔会话绑定，防串台',
  component: SessionPanel,
});
register({
  id: 'search',
  title: '全局搜索',
  desc: '跨会话 / 工具 / skill 检索',
  component: SearchPanel,
});
register({
  id: 'supervisor',
  title: '外部监工',
  desc: '独立模型派单 + 验收，小满执行（多模型主从编排）',
  component: SupervisorPanel,
});

// ── UE5 开发 ─────────────────────────────────────────────────
register({
  id: 'ue5-boot',
  title: 'UE5 自动拉起',
  desc: '一键建工程 / 拉编辑器 / 建蓝图 / 编译',
  component: Ue5BootPanel,
});
register({
  id: 'engine',
  title: '本地模型服务',
  desc: '引擎连接、启停、开放 API 与档位切换',
  component: EnginePanel,
});
register({
  id: 'toolbus',
  title: '工具总线',
  desc: '已挂载工具与命名空间',
  component: ToolBusPanel,
  placeholder: true,
});

register({
  id: 'blueprint',
  title: '蓝图画布',
  desc: '加载 UE 蓝图到画布：浏览 / 搜索 / 排版 / 删改 / T3D 导入导出',
  component: BlueprintCanvasPanel,
});
register({
  id: 'vault',
  title: '资料库',
  desc: '素材库 / 笔记 / 蓝图片段 / 工程分组（本地 JSON）',
  component: VaultPanel,
});

// ── 团队协作 ─────────────────────────────────────────────────


register({
  id: 'teamboard',
  title: '本地画板',
  desc: '一键拉起协同底座与画板前端',
  component: TeamBoardPanel,
});


// ── 控制室 ───────────────────────────────────────────────────
register({
  id: 'card-grid',
  title: '会话卡片网格（眼）',
  desc: '事件订阅镜像 + 内存告警',
  component: CardGridPanel,
});
register({
  id: 'explain',
  title: '思考可解释',
  desc: '回答「为什么思考这么久」',
  component: ExplainPanel,
});
register({
  id: 'sys-status',
  title: '系统状态',
  desc: '本机资源水位',
  component: SysStatusPanel,
});
register({
  id: 'entropy',
  title: '熵面板',
  desc: '首 token 熵与破例阈值',
  component: EntropyPanel,
  placeholder: true,
});
register({
  id: 'resident',
  title: '驻留角色',
  desc: '多角色常驻显存、切面板即切脑子（真实运行时驻留数据）',
  component: ResidentPanel,
});

// ── 设置 ─────────────────────────────────────────────────────
register({
  id: 'model',
  title: '模型配置',
  desc: '模型提供方管理：添加 / 编辑 / 切换小满的大脑',
  component: ModelPanel,
});
register({
  id: 'plugins',
  title: '插件',
  desc: '能力工具 / Skill 包 / MCP 连接器，一个入口装卸启停',
  component: PluginsPanel,
});

export function getPanel(id: string): PanelRegistration | undefined {
  return registry.get(id);
}

export function listPanels(): PanelRegistration[] {
  return Array.from(registry.values());
}

