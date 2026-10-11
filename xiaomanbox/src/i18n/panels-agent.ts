import { addTranslations } from './index';

/**
 * AgentPanel 面板（2026-10-09 全应用双语）。
 * key = 中文原文，值 = 英文。人名「用户」「小满」保留中文不翻。
 */
addTranslations({
  // Hero 品牌行
  '小得盈满，未至大满': 'Gently full, never overflowing',
  预览版: 'Preview',
  '选择工作区目录': 'Select workspace directory',
  '添加工作区…': 'Add workspace…',
  模式: 'Mode',

  // 对话消息流
  '这个会话还没有消息，在下面输入第一条吧': 'No messages in this session yet. Type the first one below.',
  '这个会话还没有消息': 'This session has no messages yet',

  // 输入框 / 指令 / 上下文
  '今天帮你做些什么？输入 / 唤起指令、@ 挂载上下文': 'What can I do for you today? Type / for commands, @ to attach context',
  '指令 · 交给小满的活': 'Commands · tasks for 小满',
  无匹配指令: 'No matching commands',
  '挂载上下文 · 让任务带上背景': 'Attach context · give the task background',
  无可用上下文: 'No context available',
  移除附件: 'Remove attachment',

  // 工具条 pill / 标题
  '添加文件、模式、技能、连接器': 'Add files, modes, skills, connectors',
  '切换模型 / 思考模式': 'Switch model / thinking mode',
  权限模式: 'Permission mode',
  允许完全访问: 'Allow full access',
  '会话：历史列表 · 新建 · 消息查询 · 保留开关': 'Sessions: history · new · message lookup · retention toggle',
  会话: 'Session',
  '上下文用量：分类 token 统计': 'Context usage: token stats by category',
  用量: 'Usage',

  // 引擎状态（工具条 + 连接器）
  后端离线: 'Backend offline',
  内置: 'Built-in',
  '个工具，引擎已连接': 'tools, engine connected',
  '引擎未连接（需打开 UE 工程并装 UnrealAgentLink 插件）': 'Engine offline (open a UE project and install the UnrealAgentLink plugin)',
  引擎离线: 'Engine offline',
  引擎: 'Engine',
  工具: 'tools',
  引擎未连接: 'Engine offline',
  已连接: 'Connected',
  已内置: 'Built-in',
  个工具: 'tools',
  等待引擎连接: 'waiting for engine connection',

  // 发送 / 语音
  '小满干活中…': '小满 is working…',
  '交给小满（Enter）': 'Send to 小满 (Enter)',

  // 「+」功能菜单
  添加文件: 'Add files',
  '图片/音视频/文档/skill，真内容进任务（可拖拽进输入框）': 'Images, audio/video, docs, and skills; real content goes into the task (drag into the box)',
  '温度与思考预算': 'temperature & thinking budget',
  技能: 'Skills',
  已装: 'Installed',
  '个 skill': 'skills',
  连接器: 'Connectors',
  内置引擎工具: 'Built-in engine tools',
  '个 MCP 服务器': 'MCP servers',
  '暂无已装 skill': 'No skills installed yet',
  '内置引擎工具（UnrealAgentLink）': 'Built-in engine tools (UnrealAgentLink)',
  已注册: 'Registered',
  '暂无 MCP 服务器': 'No MCP servers yet',
  管理连接器: 'Manage connectors',
  '去「插件」页添加 MCP 服务器': 'Add MCP servers from the Plugins page',

  // 权限气泡
  '当前权限为允许完全访问，小满将直接执行工具调用，请注意数据安全，建议执行可信的任务。':
    'Current permission is full access: 小满 will run tool calls directly. Mind data safety and prefer trusted tasks.',
  '当前权限为标准受限：执行删除文件、覆盖资产等破坏性操作前，小满会先说明并征得你的同意。':
    'Current permission is restricted: before destructive actions like deleting files or overwriting assets, 小满 will explain and ask for your consent.',

  // 会话抽屉
  保留会话: 'Retain sessions',
  '开：历史进上下文并持久化可查 · 关：一句一句独立不留存': 'On: history enters context and persists · Off: each message is independent, not stored',
  新建会话: 'New session',
  '新任务开新话题，消息只进这个会话': 'A new task opens a new topic; messages only go into this session',
  当前: 'Current',
  条: 'messages',
  '切换到此会话（下一条消息进它）': 'Switch to this session (next message goes into it)',
  切: 'Switch',
  '归档（收起，不删除）': 'Archive (collapse, not delete)',
  还没有会话: 'No sessions yet',
  已归档: 'Archived',
  恢复: 'Restore',

  // 上下文用量
  上下文用量: 'Context usage',
  已使用: 'Used',
  保留开启: 'Retention on',
  条历史在上下文中: 'history messages in context',
  '保留已关 · 每条任务独立（不带历史）': 'Retention off · each task independent (no history)',
  '后端离线，拿不到用量': 'Backend offline, no usage data',

  // 模型菜单
  思考模式: 'Thinking mode',
  '开启后模型先推理再动手（thinkingBudget）': 'When on, the model reasons before acting (thinkingBudget)',
  思考强度: 'Thinking level',
  配置自定义模型: 'Configure custom models',
  '去「模型配置」页添加提供方': 'Add providers from the Model Config page',

  // 底部提示
  'Enter 发送 · Shift+Enter 换行 · / 唤起指令 · @ 挂载上下文 · 可拖文件进输入框': 'Enter to send · Shift+Enter for newline · / for commands · @ to attach context · drag files into the box',

  // 执行轨迹
  执行轨迹: 'Execution trace',
  运行中: 'Running',
  已完成: 'Completed',
  失败: 'Failed',
  系统提示词: 'System prompt',
  上下文注入: 'Context injection',
  记忆: 'Memory',
  键: 'keys',
  历史: 'History',
  个: '',
  场景域: 'Scene domain',
  全局记忆: 'Global memory',
  场景记忆: 'Scene memory',
  会话历史: 'Session history',
  '防串台：只带本会话': 'only this session (no cross-session mixing)',
  'skill 清单': 'skill list',
  无: 'None',
  '调工具：': 'Tool calls:',
  答: 'Answer',
  已思考: 'Reasoning',
  程序化核查: 'Programmatic check',
  悬空输入: 'dangling input',

  // 会话统计
  轮: 'rounds',
  步: 'steps',
  缓存命中: 'cache hit',
  'Token 用量': 'Token usage',
  输入: 'Input',
  缓存读取: 'Cache read',
  输出: 'Output',
  模型用时: 'Model time',
  秒: 's',

  // 答复 / 产出
  '小满的答复': 'Reply from 小满',
  '失败：': 'Failed: ',
  产出文件: 'Produced files',
  打开: 'Open',

  // 工具条四件套（2026-10-09 用户）
  '对话内搜索（Ctrl+F）': 'Search in conversation (Ctrl+F)',
  '分享：复制全文 / 下载 Markdown': 'Share: copy all / download Markdown',
  '历史提问：本会话问过的每一句': 'Past prompts: every question in this session',
  '任务概览：执行摘要': 'Task overview: execution summary',
  '在对话里搜…': 'Search in conversation…',
  '输入关键词，Enter 跳第一条': 'Type a keyword; Enter jumps to the first hit',
  没有匹配的消息: 'No matching messages',
  复制全文到剪贴板: 'Copy full text to clipboard',
  'Markdown 格式，粘给谁都能看': 'Markdown format, paste anywhere',
  '下载 .md 文件': 'Download .md file',
  存成会话导出文件: 'Save as a session export file',
  '还没有会话（先发一条消息）': 'No session yet (send a message first)',
  这个会话还没有提问: 'No prompts in this session yet',
  任务概览: 'Task overview',
  会话全文已复制到剪贴板: 'Session copied to clipboard',
  '当前没有绑定的会话，无法分享': 'No bound session; nothing to share',
  循环轮数: 'Loop rounds',
  工具调用: 'Tool calls',
  '输入 token': 'Input tokens',
  '输出 token': 'Output tokens',

  // 右上角工具条 + 右侧产出物栏（2026-10-09 用户）
  '打开右侧栏 Ctrl+Shift+B': 'Open right sidebar Ctrl+Shift+B',
  产出物: 'Outputs',
  '这个会话还没有产出物 —— 小满做完东西会出现在这里': 'No outputs in this session yet — they will appear here when 小满 produces something',
  模型: 'Model',

  // 工作过程（WorkBuddy 式行内暴露）
  深度思考: 'Deep thinking',
  调用: 'Call',
  运行命令: 'Run command',
  读取: 'Read',
  写入: 'Write',
  编辑: 'Edit',
  搜索文件: 'Search files',
  搜索网页: 'Search web',
  抓取网页: 'Fetch page',
  更新待办: 'Update todos',
  向用户提问: 'Ask 用户',
  '运行 PowerShell': 'Run PowerShell',
  查看技能: 'List skills',
  加载技能: 'Load skill',
  新对话: 'New chat',

  // skill 选择性加载（2026-10-09 用户）
  '勾选 = 本次任务加载该 skill（不勾不注入，省 token 提速）': 'Check = load this skill for this task (unchecked means not injected, saving tokens and speed)',
  '移除 skill': 'Remove skill',
  已选: 'Selected',
  '个：': ':',

  // 工具组按需加载（2026-10-10 用户）
  '本次加载 UE 工具': 'Load UE tools this run',
  '已加载（UE5 会话默认加载）': 'Loaded (default for UE5 sessions)',
  '未加载（166 个工具 · 62KB）': 'Not loaded (166 tools · 62KB)',
  'UE 工具 166 个（schema 62KB），非 UE 任务不带可大幅提速': 'UE tools: 166 (62KB schema). Excluding them for non-UE tasks greatly speeds things up',
});
