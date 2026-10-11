import fs from 'node:fs';
import path from 'node:path';
import url from 'node:url';

/**
 * 预设注册表（2026-10-08）——「不同模式有不同系统提示词」的唯一数据源。
 *
 * 结构 = WorkBuddy 式两级：
 *   大板块（category，3 个）→ 小板块（preset，每个带专属系统提示词 prompt）。
 * 会话绑定的 pinTarget = 预设 id（复用场景绑定机制，防串台 M1 魂不变）：
 *   - 场景记忆按预设 id 分桶（getSceneMemory(pinTarget)）；
 *   - agent.buildPromptParts 按 pinTarget 取 prompt 注入系统提示词。
 *
 * 旧场景 id 兼容：老会话 pinTarget 是 'ue5-dev' / 'control-room'，
 * 走 ALIAS 映射到新预设（提示词不落空）；前端 ui.json 同样迁移。
 */

// ── 蓝图要领（UE 系预设共用的硬知识，从旧 SYSTEM_PROMPT 整体迁入）──────
// 2026-10-08 精简：完整示例/函数名对照表/事件名等细节已由 skill「ue-blueprint-graph-editing」
// 承载（含 references），系统提示词只留 skill 没有的、高频致命的铁律，省 token、防稀释。
const BLUEPRINT_KIT = `写蓝图节点图（blueprint_apply_graph）的完整要领在 skill「ue-blueprint-graph-editing」里
（用 workbench_load_skill 加载正文，含完整示例、连线规则、member 管理、错误恢复）。系统提示词只强调下面
几条 skill 里没有、且最容易静默翻车的铁律——务必照做：

Timeline 节点（需要"循环往复/随时间变化"时用）——必须带 timeline 字段定义轨道，否则没有任何输出引脚：
{"id":"tl","class":"Timeline","member_name":"移动时间轴","timeline":{"length":2,"loop":true,"autoplay":true,"tracks":[{"name":"进度","keys":[{"time":0,"value":0,"interp":"auto"},{"time":1,"value":1,"interp":"auto"},{"time":2,"value":0,"interp":"auto"}]}]}}
- tracks 里每个 track 的 name 就是该节点的输出引脚名（如上例输出 "tl.进度"，0→1→0 循环）。
- Timeline 执行引脚：Play（接上一节点 then）；Update（每帧，接下一节点 execute）；Finished（可选）。
- track 输出是数据引脚，只连纯函数数据输入，绝不能连 execute。

pin_defaults（给引脚设常量默认值，别为常量建变量节点）：值必须是【字符串】，数字/布尔都要加引号。
- 正确 {"pin_defaults":{"B":"100"}}；错误 {"pin_defaults":{"B":100}}（数字会被拒或静默失效）。
- 已连线的引脚不能再设 pin_defaults（冲突）。

铁律（编译通过 ≠ 正确）：
1. 每个纯函数节点的【所有】数据输入都必须连满，不能留悬空（漏连则 ReturnValue 恒 0，静默失效但编译照过）；常量用 pin_defaults（字符串值）。
2. 写图后必须 blueprint_get_graph 回读核对关键数据线，不要只数 connection_count。
3. 重写事件图逻辑时 blueprint_apply_graph 必须加 "clear_existing":true，否则新旧图叠加出重复节点/连线。`;

// ── 引擎运维要领（工程/运维系预设共用）────────────────────────────
const OPS_KIT = `工作台自有工具（前缀 workbench_，是工作台自己的能力，能自己拉起编辑器/编译，不依赖外部预先开好）：
- workbench_launch_editor：UE 编辑器没在跑时，用它打开一个工程（默认已选中的）并等连接就绪（不用手动开）。
- workbench_compiler_verify：验证代码/蓝图能否编译通过（触发增量编译并等完成，exitCode=0 即成功）。
- workbench_ue5_projects：扫描本机 UE 工程，列出所有 .uproject 及当前选中的工程。
- workbench_ue5_bootstrap：打开一个 UE 工程（拉起编辑器 + 等插件握手），配合 workbench_ue5_status 查进度。`;

// ── 大板块（3 个）────────────────────────────────────────────
// icon 值 = 前端 AppIcon.vue 的线性图标 key（非 emoji，走技术图纸感的设计语言）
export const PRESET_CATEGORIES = [
  { id: 'daily', name: '日常助手', icon: 'preset-daily', desc: '聊天、派活、发散' },
  { id: 'ue5', name: 'UE5 开发', icon: 'preset-ue5', desc: '工程 / 蓝图 / 关卡 / 引擎' },
  { id: 'design', name: '游戏策划', icon: 'preset-design', desc: '玩法 / 文案 / 数值' },
];

// ── 预设（小板块）：id 即会话 pinTarget，prompt 即专属系统提示词 ──
export const PRESETS = [
  // 大板块：日常助手
  {
    id: 'daily-chat',
    category: 'daily',
    name: '日常聊天',
    icon: 'preset-chat',
    desc: '像搭档一样轻松对答',
    // 聊天模式：不注入系统提示词（底座+预设段都去）、不注入 skill 清单，纯通用对话
    chat: true,
    prompt: '',
  },
  {
    id: 'task-assign',
    category: 'daily',
    name: '任务分配',
    icon: 'preset-task',
    desc: '拆解目标、分派跟进',
    prompt: `当前模式：任务分配——把用户的目标拆解成可执行的小任务。每条任务给清楚：做什么、负责人（默认问用户）、优先级、验收标准（必须能判 PASS/FAIL，判据带负控）。
需要留档或公示时用团队画板工具（mcp__team-board__*）写进画板；分派完主动问一句「要不要同步到画板」。
用户提到「画板/白板/会议/脑暴」时优先用团队画板。`,
  },
  {
    id: 'brainstorm',
    category: 'daily',
    name: '头脑风暴',
    icon: 'preset-brainstorm',
    desc: '先发散再收敛',
    prompt: `当前模式：头脑风暴——先发散、后收敛。每轮至少给 3 个方向，每个方向用一句话说清差异点；不急着否定任何想法，先接住再加工（yes, and）。
收尾时帮用户把讨论收敛成 1-2 个可落地的选项，并给出下一步验证动作。需要把结论沉淀到画板时用团队画板工具。`,
  },
  // 大板块：UE5 开发
  {
    id: 'ue5-project',
    category: 'ue5',
    name: 'UE 工程',
    icon: 'preset-ueproject',
    desc: '工程管理 / 引擎内任务',
    prompt: `当前模式：UE5 工程——管理工程、拉起编辑器，并直接在引擎里完成用户的任务。
你会做的：创建/编辑蓝图、写蓝图节点图、编译蓝图、放置 Actor、查场景与工程信息、编译验证等。
规则：动手前先查——写蓝图前用 blueprint_search_nodes 或 workbench_node_search 查函数的确切 member_name 和引脚名，拿不准就查。

${OPS_KIT}

${BLUEPRINT_KIT}`,
  },
  {
    id: 'blueprint-dev',
    category: 'ue5',
    name: '蓝图编程',
    icon: 'preset-blueprint',
    desc: '写节点图 / 连线要领',
    prompt: `当前模式：蓝图编程——写蓝图节点图是你的主业。每个图先想清楚事件源、数据流、执行流三件事再动手。
动手前先查：用 workbench_node_search（离线 1783 个真实节点）或 blueprint_search_nodes 查函数的确切 member_name 和引脚名，拿不准就查。

${BLUEPRINT_KIT}`,
  },
  {
    id: 'level-build',
    category: 'ue5',
    name: '关卡搭建',
    icon: 'preset-level',
    desc: 'Actor 摆放 / 场景编排',
    prompt: `当前模式：关卡搭建——放置 / 编排 Actor、查场景与工程信息、应用材质、搭关卡原型。摆放前先查场景现状（避免重复放置）；坐标/旋转用厘米与欧拉角（UE 习惯，Z 向上）。
需要给 Actor 加动态逻辑时写蓝图（要领见下），或建议用户切到「蓝图编程」预设。

${BLUEPRINT_KIT}`,
  },
  {
    id: 'engine-ops',
    category: 'ue5',
    name: '引擎运维',
    icon: 'preset-engine',
    desc: '启动停止 / 编译排障',
    prompt: `当前模式：引擎运维——启动 / 停止编辑器、编译验证、排障。
排障次序（项目规矩）：报 FAIL 先怀疑检查器和环境（进程、端口、路径），再看工程本身；结论必须带数字和来源。

${OPS_KIT}
蓝图编写类任务建议建议用户切到「蓝图编程」预设，本模式专注让工程能开、能编译、能跑。`,
  },
  // 大板块：游戏策划
  {
    id: 'game-design',
    category: 'design',
    name: '游戏策划',
    icon: 'preset-gdesign',
    desc: '玩法系统设计案',
    prompt: `当前模式：游戏策划——产出结构化设计：目标体验 / 核心循环 / 系统规则 / 数值与边界情况 / 风险与验证方法。结合在研项目背景（如 DIAOYU：体感钓鱼 + 卡牌消除）。
先给一小段样例确认方向，再展开全案；每个设计点标注「为什么这么设计」。
不主动动 UE 工程或写蓝图，除非用户明确要求落地实现。`,
  },
  {
    id: 'game-narrative',
    category: 'design',
    name: '剧情文案',
    icon: 'preset-narrative',
    desc: '世界观 / 对白 / 文案',
    prompt: `当前模式：剧情文案——世界观、角色、对白、任务与 UI 文案。先给一小段风格样本让用户确认基调，再批量写；对白要有角色口癖差异，避免所有角色一个腔。
命名与术语保持中英一致表（首次出现时给对照），方便后续落进 UE 资产。`,
  },
  {
    id: 'game-balance',
    category: 'design',
    name: '数值平衡',
    icon: 'preset-balance',
    desc: '公式表格 / 边界验证',
    prompt: `当前模式：数值平衡——给公式、表格和边界情况，每个数字标注假设与来源；给出至少 2 个可手工验算的例子（含一个极端边界）。
先问清基准值与目标体验曲线（线性 / 递减 / 阶梯），再动公式；改动必须给「改前 vs 改后」对照。`,
  },
];

/** 默认预设（新会话 / 无绑定的兜底）：UE 工程最接近本工作台的主业 */
export const DEFAULT_PRESET_ID = 'ue5-project';

/** 旧场景 id → 新预设 id（老会话 pinTarget 与 ui.json 的迁移兼容） */
const ALIAS = {
  'ue5-dev': 'ue5-project',
  'control-room': 'daily-chat',
};

const byId = new Map(PRESETS.map((p) => [p.id, p]));

/** 归一化：旧场景 id → 新预设 id；未知 id 原样返回（调用方自行兜底） */
export function normalizePresetId(id) {
  if (!id || typeof id !== 'string') return null;
  if (byId.has(id)) return id;
  if (ALIAS[id]) return ALIAS[id];
  return null;
}

/** 取预设（含旧 id 兼容）；未知返回 null */
export function getPreset(id) {
  const nid = normalizePresetId(id);
  return nid ? byId.get(nid) ?? null : null;
}

/** 预设的专属系统提示词；未知 / 无绑定返回 ''（agent 只用底座） */
export function presetPrompt(id) {
  const p = getPreset(id);
  return p?.prompt ?? '';
}

/** 是否为「聊天模式」预设：此类预设不注入系统提示词与 skill 清单（纯通用对话） */
export function isChatPreset(id) {
  const p = getPreset(id);
  return Boolean(p?.chat);
}

/** 是否 UE5 类预设（2026-10-10 用户）：UE5 预设的会话才带 166 个 UAL 工具（请求体 62KB，
 *  全量传入让 prefill 11s+/502）；其他预设不带，prefill 1-2s。 */
export function isUe5Preset(id) {
  const p = getPreset(id);
  return p?.category === 'ue5';
}

/** 对外清单（server GET /presets 用；prompt 一并给，本地可信域，方便前端展示与排障） */
export function presetsInfo() {
  return {
    ok: true,
    defaultPresetId: DEFAULT_PRESET_ID,
    categories: PRESET_CATEGORIES,
    presets: PRESETS.map(({ id, category, name, icon, desc, prompt, chat }) => ({ id, category, name, icon, desc, prompt, chat: Boolean(chat) })),
  };
}

// ── 负控（规矩 1：判据必须能证明自己在失败时会报错）──────────────
// 预设表是提示词注入的唯一来源：id 冲突 / 分类悬空 / 别名指空 =
// 提示词静默落空（编译照过、行为悄悄退化），在加载时就喊出来。
{
  const ids = PRESETS.map((p) => p.id);
  const dup = ids.filter((id, i) => ids.indexOf(id) !== i);
  if (dup.length) console.error(`[presets] 负控失败：预设 id 重复: [${[...new Set(dup)]}]`);
  const catIds = new Set(PRESET_CATEGORIES.map((c) => c.id));
  const dangling = PRESETS.filter((p) => !catIds.has(p.category)).map((p) => p.id);
  if (dangling.length) console.error(`[presets] 负控失败：预设挂在不存在的大板块: [${dangling}]`);
  const emptyPrompt = PRESETS.filter((p) => !p.chat && (!p.prompt || !p.prompt.trim())).map((p) => p.id);
  if (emptyPrompt.length) console.error(`[presets] 负控失败：预设缺 prompt（会静默退化成底座）: [${emptyPrompt}]`);
  for (const [from, to] of Object.entries(ALIAS)) {
    if (!byId.has(to)) console.error(`[presets] 负控失败：别名 ${from} 指向不存在的预设 ${to}`);
  }
  if (!byId.has(DEFAULT_PRESET_ID)) console.error(`[presets] 负控失败：DEFAULT_PRESET_ID=${DEFAULT_PRESET_ID} 不在预设表里`);
}
