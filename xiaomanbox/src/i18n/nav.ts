import { addTranslations } from './index';

/**
 * 导航层翻译（2026-10-09）：侧栏分组名 + 面包屑 + 面板 title/desc。
 * key = 中文原文，值 = 英文。未覆盖的文案由 t() 兜底返回中文。
 */
addTranslations({
  // ── 侧栏分组名 ──
  工作: 'Work',
  'UE5 开发': 'UE5 Development',
  团队协作: 'Collaboration',
  控制室: 'Control Room',
  设置: 'Settings',

  // ── 面包屑兜底 ──
  主控: 'Console',

  // ── 面板 title ──
  '小满 Agent': 'XiaoMan Agent',
  '会话绑定（魂）': 'Session Binding',
  全局搜索: 'Global Search',
  外部监工: 'External Supervisor',
  'UE5 自动拉起': 'UE5 Launcher',
  引擎档位: 'Engine Profiles',
  工具总线: 'Tool Bus',
  节点参考库: 'Node Reference',
  蓝图画布: 'Blueprint Canvas',
  资料库: 'Vault',
  团队交流: 'Team Chat',
  成员发现: 'Member Discovery',
  团队画板: 'Team Board',
  账户: 'Account',
  '会话卡片网格（眼）': 'Session Cards',
  思考可解释: 'Thinking Explainability',
  系统状态: 'System Status',
  熵面板: 'Entropy',
  驻留角色: 'Resident Roles',
  模型配置: 'Model Config',
  插件: 'Plugins',
  'Skill 社区': 'Skill Community',

  // ── 面板 desc ──
  '本地模型自主规划、调工具、核对结果': 'Local model plans, uses tools, and verifies results autonomously',
  '场景↔会话绑定，防串台': 'Scene-to-session binding, prevents cross-talk',
  '跨会话 / 工具 / skill 检索': 'Search across sessions, tools, and skills',
  '独立模型派单 + 验收，小满执行（多模型主从编排）': 'Independent model dispatches and verifies; XiaoMan executes (multi-model master-worker)',
  '一键建工程 / 拉编辑器 / 建蓝图 / 编译': 'One-click project setup, editor launch, blueprints, and compilation',
  '引擎连接、启停、开放 API 与档位切换': 'Engine connection, start/stop, open API, and profile switching',
  '已挂载工具与命名空间': 'Mounted tools and namespaces',
  '离线蓝图节点签名查询（NodeUE 内置）': 'Offline blueprint node signature lookup (built into NodeUE)',
  '加载 UE 蓝图到画布：浏览 / 搜索 / 排版 / 删改 / T3D 导入导出': 'Load UE blueprints onto canvas: browse, search, layout, edit, and T3D import/export',
  '素材库 / 笔记 / 蓝图片段 / 工程分组（本地 JSON）': 'Assets, notes, blueprint snippets, and project groups (local JSON)',
  '频道与私聊，走同一套协同底座': 'Channels and DMs over the same collaboration backbone',
  '局域网在线成员与设备，无需手配 IP': 'LAN members and devices, no manual IP setup',
  '一键拉起协同底座与画板前端': 'One-click launch of the collaboration backend and board frontend',
  '注册 / 登录，密码只存哈希': 'Register and sign in; passwords stored as hashes only',
  '事件订阅镜像 + 内存告警': 'Event-subscription mirror plus memory alerts',
  '回答「为什么思考这么久」': 'Explains why reasoning takes so long',
  '本机资源水位': 'Local resource utilization',
  '首 token 熵与破例阈值': 'First-token entropy and override threshold',
  '多角色常驻显存、切面板即切脑子（真实运行时驻留数据）': 'Multiple roles resident in VRAM; switch panels to switch minds (live runtime data)',
  '模型提供方管理：添加 / 编辑 / 切换小满的大脑': 'Provider management: add, edit, and switch XiaoMan\'s brain',
  'Agent 运行参数与 MCP 外部能力': 'Agent runtime parameters and external MCP capabilities',
  '小满的能力包：查看 / 安装，装完即进技能清单': 'XiaoMan capability packs: browse and install, then available as skills',

  // ── 侧栏 / 会话操作 ──
  取消置顶: 'Unpin',
  置顶: 'Pin',
  重命名: 'Rename',
  归档: 'Archive',
  删除会话: 'Delete session',
  收起: 'Collapse',
  展开: 'Expand',
  个会话: 'sessions',
  未注册账户: 'No account',
  后端离线: 'Backend offline',
  引擎已连: 'Engine connected',
  工具: 'tools',
  引擎未连接: 'Engine offline',
  任务进行中: 'Task in progress',
  会话操作: 'Session actions',
  '占位 · 未接 AI': 'Placeholder · not wired',
});
