/**
 * 小满 Agent —— 让本地模型（NInfer xiaoman @ 8390）全程用 XiaoMan Box 工具干活。
 *
 * 机制：OpenAI 兼容 function-calling 循环 ——
 *   发任务 + 工具清单给 8390 → 模型返回 tool_calls → 后端执行（经 ual 直连插件）→
 *   结果回填 role=tool → 再问模型 → … 直到模型给出最终回答。
 * （NInfer function calling 三关已实测全过，见会话 2026-10-07_01。）
 *
 * 配置：~/.xiaomanbox/agent.json 可覆盖 modelUrl / model / maxIterations。
 */

import http from 'node:http';
import { estimateTokens as estTokens, requestBudget, enforceRequestBudget } from './agent-budget.mjs';
import https from 'node:https';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import * as ual from './ual.mjs';
import { ualModelTools, findUalTool, executeUalTool } from './ual-tools.mjs';
import * as nodelib from './nodelib.mjs';
import * as compiler from './compiler.mjs';
import { listProjects, selectProject, openProject, closeProject, jobInfo as ue5JobInfo } from './ue5boot.mjs';
import * as session from './session.mjs';
import * as skills from './skills.mjs';
import * as events from './events.mjs';
import * as mcp from './mcp.mjs';



import * as providers from './providers.mjs';
import * as basic from './basic-tools.mjs';
import * as keepalive from './keepalive.mjs';
import { presetPrompt, isChatPreset, isUe5Preset } from './presets.mjs';
import { getActiveWorkspace } from './workspace.mjs';

const DEFAULTS = {
  modelUrl: 'http://127.0.0.1:8390/v1/chat/completions',
  model: 'xiaoman',
  maxIterations: 25, // <= 0 = 无上限（模型给最终答复才停；每轮仍受 modelTimeoutMs 约束）
  toolTimeoutMs: 180_000,
  modelTimeoutMs: 120_000,
  temperature: 0.3,
  thinkingBudget: 800, // >0 透传 thinking_budget；0 用 reasoning_effort=none 关闭思考；-1 = 无上限（不传参，引擎自己决定思考量，如 4090 服务端默认 max）
  reasoningEffort: 'low', // 思考档（2026-10-08 修「思考泄漏」）：NInfer 引擎默认开思考，长答案会写进 reasoning_content 而 content 空。
  //   显式传 reasoning_effort 控制：'none'=不思考只出正文（最稳）、'low'=轻思考正文为主（默认，平衡）、
  //   'medium'/'high'=更深思考（长创作任务更容易泄漏）。引擎不认识该字段时忽略，无害。
  allowFullAccess: false, // 权限模式：false=标准受限（破坏性操作先征得同意），true=直接执行
  retainSessions: true, // 会话保留：true=历史进上下文并持久化（可查询）；false=一句一句独立（不留存）
  contextWindow: 131072, // 模型上下文窗口（token，估算请求预算）
  maxInputTokens: 0, // 单次请求输入 token 上限（0 = 使用上下文窗口减输出预留）
  maxOutputTokens: 0, // 单次响应输出 token 上限（0 = 自动预留输出；>0 时透传引擎 max_tokens）
};

function loadConfig() {
  const cfg = { ...DEFAULTS };
  try {
    const raw = JSON.parse(fs.readFileSync(path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'agent.json'), 'utf8'));
    for (const k of Object.keys(DEFAULTS)) {
      if (typeof raw?.[k] === typeof DEFAULTS[k]) cfg[k] = raw[k];
    }
  } catch {
    /* 默认 */
  }
  return cfg;
}

/** 运行参数读取（插件页「Agent 循环」用；modelUrl/model 仅在无激活提供方时才生效） */
export function getConfig() {
  return loadConfig();
}

/** 运行参数白名单写入（agent.json）。只允许改 DEFAULTS 里已有的键，类型不符即拒。 */
export function setConfigPatch(patch) {
  if (!patch || typeof patch !== 'object' || Array.isArray(patch)) {
    return { ok: false, code: 400, error: 'body 必须是对象' };
  }
  const cur = loadConfig();
  for (const k of Object.keys(patch)) {
    if (!(k in DEFAULTS)) return { ok: false, code: 400, error: `未知配置项：${k}` };
    if (patch[k] === null || typeof patch[k] !== typeof DEFAULTS[k]) {
      return { ok: false, code: 400, error: `${k} 类型应为 ${typeof DEFAULTS[k]}` };
    }
    if (typeof patch[k] === 'number' && !Number.isFinite(patch[k])) return { ok: false, code: 400, error: `${k} 必须是有限数值` };
    if (['contextWindow', 'maxInputTokens', 'maxOutputTokens'].includes(k) && (!Number.isSafeInteger(patch[k]) || patch[k] < (k === 'contextWindow' ? 1024 : 0))) return { ok: false, code: 400, error: `${k} 超出 token 参数范围` };
    if (k === 'thinkingBudget' && (!Number.isSafeInteger(patch[k]) || patch[k] < -1)) return { ok: false, code: 400, error: 'thinkingBudget 应为 -1、0 或正整数' };
    cur[k] = patch[k];
  }
  try {
    fs.mkdirSync(path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox')), { recursive: true });
    fs.writeFileSync(path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'agent.json'), JSON.stringify(cur, null, 2), 'utf8');
  } catch (e) {
    return { ok: false, code: 500, error: String(e.message || e) };
  }
  return { ok: true, config: cur };
}

/**
 * 系统提示词底座（2026-10-08 预设改造）：所有预设共用。
 * 领域专精内容（蓝图要领 / 运维要领 / 策划模式等）在 presets.mjs 的
 * preset.prompt 里，由 buildPromptParts 按会话绑定的预设（pinTarget）追加。
 */
const SYSTEM_PROMPT = `你是「小满」，XiaoMan Box（AI 工作台）的 AI 助手，能用工具帮用户完成任务。

规则：
1. 中文命名：用户要求中文命名时，资产名、变量名、组件名、函数名、Timeline 名都用中文。
2. 一步一步来，每步根据工具返回结果决定下一步。
3. 完成一个完整的小任务后，用中文简洁总结你做了什么、结果如何。

只调用本次请求提供的工具；用户自行配置的 MCP 服务以实际工具列表为准。局域网画板通过本地网页分享，不内置团队画板 MCP 服务。`;

// ── 工作台自有工具（后端自己的能力，不走 UEBox）────────────────
// 这些是 XiaoMan Box 后端自己的动作：拉起编辑器、编译验证、一键编排。
// 挂给「小满」后，它连启动 UE 都能自己搞定，不必依赖外部预先开好编辑器。

const WORKBENCH_TOOLS = [
  {
    type: 'function',
    function: {
      name: 'workbench_launch_editor',
      description: '启动 UE5 编辑器并打开一个工程（默认打开已选中的工程，也可用 projectPath 指定 .uproject 路径），等待编辑器连接就绪。编辑器没在跑时用它。',
      parameters: { type: 'object', properties: { projectPath: { type: 'string', description: '要打开的 .uproject 完整路径（可选，缺省用已选中的工程）' } }, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'workbench_compiler_verify',
      description: '触发用户配置的 UE 工程增量编译验证，等待编译完成，返回 exitCode/errors。用于验证代码改动能否编译通过。',
      parameters: { type: 'object', properties: {}, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'workbench_compiler_status',
      description: '查询最近一次编译验证的状态（running/done 及 exitCode、错误行）。',
      parameters: { type: 'object', properties: {}, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'workbench_ue5_projects',
      description: '扫描本机 UE 工程目录，列出所有 .uproject 工程（含名字与路径），并返回当前已选中的工程。',
      parameters: { type: 'object', properties: {}, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'workbench_ue5_bootstrap',
      description: '打开一个 UE 工程（默认已选中的，也可用 projectPath 指定 .uproject 路径）：拉起编辑器 + 等插件握手。返回 jobId，用 workbench_ue5_status 查进度。',
      parameters: { type: 'object', properties: { projectPath: { type: 'string', description: '要打开的 .uproject 完整路径（可选，缺省用已选中的工程）' } }, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'workbench_ue5_status',
      description: '查询 UE 工程打开（拉起编辑器+握手）的进度与引擎连接状态。返回 state（idle/running/done/failed）、已等时长、当前步骤、引擎是否已连接。launch 报「已有打开流程在进行中」时用它轮询，等 state=done 且 engineConnected=true 再继续 UE 操作；编辑器首次启动可能需要几分钟。',
      parameters: { type: 'object', properties: {}, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'workbench_ue5_close',
      description: '关闭当前 UE 编辑器（先自动保存已连接工程，再结束进程）。当编辑器连接中断（心跳停滞）需要重启、或要切换工程时，先调用它关掉旧编辑器，再 workbench_ue5_bootstrap 重新打开。',
      parameters: { type: 'object', properties: {}, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'workbench_load_skill',
      description: '加载某个 skill 的完整正文（SKILL.md body）。只有系统提示里列出的 skill 名可选；需要某个 skill 的详细步骤/知识时才调用。',
      parameters: { type: 'object', properties: { name: { type: 'string', description: 'skill 名（见系统提示里的 skill 清单）' } }, required: ['name'] },
    },
  },
  
  
  
  
  
  
  
  
  
  // ── 通用基础工具（2026-10-08 补齐 dsh 有、小满缺的能力）──────
  {
    type: 'function',
    function: {
      name: 'bash',
      description: '执行一条 shell 命令并返回输出（Windows cmd）。用于跑 node/python/git/查看目录等。默认 30 秒超时，输出过长自动截断。',
      parameters: { type: 'object', properties: { command: { type: 'string', description: '要执行的命令' }, timeoutMs: { type: 'number', description: '超时毫秒（默认 30000，上限 120000）' }, cwd: { type: 'string', description: '工作目录（可选）' } }, required: ['command'] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'read_file',
      description: '读取文本文件内容（UTF-8，限 512KB）。读代码/配置/日志用它。大文件用 offset/limit 分段。',
      parameters: { type: 'object', properties: { path: { type: 'string', description: '文件完整路径' }, offset: { type: 'number', description: '起始行号（默认 0）' }, limit: { type: 'number', description: '读多少行' } }, required: ['path'] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'write_file',
      description: '把文本内容写入文件（UTF-8，父目录自动创建，限 1MB）。写代码/网页/文档/脚本用它。',
      parameters: { type: 'object', properties: { path: { type: 'string', description: '文件完整路径' }, content: { type: 'string', description: '要写入的完整文本内容' } }, required: ['path', 'content'] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'glob',
      description: '按通配符在目录下递归搜文件（支持 ** 和 *，自动跳过 node_modules/.git）。找文件用它。',
      parameters: { type: 'object', properties: { pattern: { type: 'string', description: '通配符，如 **/*.uproject 或 src/**/*.vue' }, path: { type: 'string', description: '搜索根目录（默认用户主目录）' } }, required: ['pattern'] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'web_fetch',
      description: '抓取一个 http(s) URL 的文本内容（限 64KB，15 秒超时）。查网页/接口/文档时用它。',
      parameters: { type: 'object', properties: { url: { type: 'string', description: '完整 http(s) URL' } }, required: ['url'] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'todo_write',
      description: '维护待办清单（整体替换）。拆解多步任务时用它记录进度。不传 items 则返回当前清单。',
      parameters: { type: 'object', properties: { items: { type: 'array', items: { type: 'object', properties: { text: { type: 'string' }, done: { type: 'boolean' } }, required: ['text'] }, description: '待办项数组' } }, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'workbench_present_file',
      description: '把做好的产出文件（网页/报告/图片/文档等）登记到「产出文件」，用户能在工作台里看到并点开（点开用系统默认程序拉起）。做完网页/文档/图片后必须调用它。',
      parameters: { type: 'object', properties: { path: { type: 'string', description: '产出文件的完整路径' }, label: { type: 'string', description: '显示名称（可选，默认文件名）' } }, required: ['path'] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'edit_file',
      description: '精确编辑文件（对应 dsh 的 str_replace_editor）。command：view=带行号查看 / create=新建（不存在时）/ str_replace=精确替换（old_str 必须唯一匹配）/ insert=在指定行后插入。改代码/配置时用它，比整文件重写安全。',
      parameters: { type: 'object', properties: { path: { type: 'string', description: '文件完整路径' }, command: { type: 'string', enum: ['view', 'create', 'str_replace', 'insert'], description: '操作类型' }, old_str: { type: 'string', description: 'str_replace：要替换的原文本（必须唯一）' }, new_str: { type: 'string', description: 'str_replace/insert：替换为/插入的文本' }, insert_line: { type: 'number', description: 'insert：插入到第几行之后（0=文件开头）' }, file_text: { type: 'string', description: 'create：新文件完整内容' } }, required: ['path', 'command'] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'skill_list',
      description: '列出小满可加载的技能（skill）清单。skill 是特定任务的专家指令集，加载后小满按该技能的工作流干活。',
      parameters: { type: 'object', properties: {}, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'ask_user',
      description: '向用户（用户）提问。小满是「派任务→跑完→返回」的单轮模型，无法在任务中途暂停等你回答——问题会登记到「待答问题」，你在最终产出里把这个问题连同你的选择一起说明，让用户回答后我再继续。需要澄清需求、二选一、确认方向时用它。',
      parameters: { type: 'object', properties: { question: { type: 'string', description: '要向用户提出的问题' }, options: { type: 'array', items: { type: 'string' }, description: '可选的候选项（可选，最多 10 个）' } }, required: ['question'] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'bash_persistent',
      description: '持久终端会话：跨轮保持工作目录（Windows cmd）。传 command 执行命令，传 session_id 区分多个会话（默认 default），传 cd <目录> 切换工作目录。不传 command 则返回会话当前状态。用于需要在同一目录下连续执行多条命令的场景。',
      parameters: { type: 'object', properties: { command: { type: 'string', description: '要执行的命令（或 cd <目录> 切换目录；不传则查状态）' }, session_id: { type: 'string', description: '会话 id（默认 default）' }, cwd: { type: 'string', description: '初始工作目录（仅新建会话时生效）' } }, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'jobs',
      description: '后台任务控制：action=list/status 查当前运行中的任务，action=kill 停止当前任务。小满是单例，同时只跑一个任务。',
      parameters: { type: 'object', properties: { action: { type: 'string', enum: ['list', 'status', 'kill'], description: 'list/status=查询，kill=停止' } }, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'subagent',
      description: '委派子任务给子代理。诚实说明：小满当前是单例架构（同时只跑一个任务），暂不支持 spawn 并行子代理——如需多模型主从协作，请用「外部监工」（Supervisor 面板）。',
      parameters: { type: 'object', properties: { task: { type: 'string', description: '要委派的子任务描述' } }, required: ['task'] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'workflow',
      description: '跑 JS 编排脚本。诚实说明：小满当前没有 JS 编排引擎，暂不支持——可用 todo_write 拆解多步任务 + 顺序执行达到类似效果。',
      parameters: { type: 'object', properties: { script: { type: 'string', description: '编排脚本内容' } }, required: ['script'] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'web_search',
      description: '联网搜索（DuckDuckGo，免 key），返回标题+摘要+链接。需要查最新信息、找资料、了解某个话题时用它。注意可能需要联网，失败时改用 web_fetch 抓具体页面。',
      parameters: { type: 'object', properties: { query: { type: 'string', description: '搜索关键词' }, max: { type: 'number', description: '最多返回条数（默认 8，上限 20）' } }, required: ['query'] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'pwsh',
      description: '执行一条 PowerShell 命令并返回输出。需要 PowerShell 特有功能（如 Windows 管理、注册表）时用它；普通命令优先用 bash。',
      parameters: { type: 'object', properties: { command: { type: 'string', description: 'PowerShell 命令' }, timeoutMs: { type: 'number', description: '超时毫秒（默认 30000）' } }, required: ['command'] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'goal',
      description: '记录/查询/清除「当前会话目标」。action=set 设置目标（后续轮次对齐），get 查询，clear 清除。让多轮任务始终围绕同一目标时用它。',
      parameters: { type: 'object', properties: { action: { type: 'string', enum: ['set', 'get', 'clear'], description: 'set/get/clear' }, goal: { type: 'string', description: '目标文本（action=set 时必填）' } }, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'schedule',
      description: '定时提醒：add 登记一个到点提醒（at=ISO 时间 + text），list 列出，remove 删除。注意：到点触发需工作台运行时由调度器 tick，当前登记后需外部调度。',
      parameters: { type: 'object', properties: { action: { type: 'string', enum: ['add', 'list', 'remove'], description: 'add/list/remove' }, at: { type: 'string', description: '触发时间（ISO 8601，action=add 时必填）' }, text: { type: 'string', description: '提醒内容（action=add 时必填）' }, id: { type: 'string', description: '要删的提醒 id（action=remove 时必填）' } }, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'webhook',
      description: 'HTTP 触发器：add 登记一个 webhook 规则（收到请求触发任务），list 列出，remove 删除。诚实说明：真实「收到请求即触发」需常驻监听，当前为登记态。',
      parameters: { type: 'object', properties: { action: { type: 'string', enum: ['add', 'list', 'remove'], description: 'add/list/remove' }, name: { type: 'string', description: '规则名（add/remove 时必填）' }, task: { type: 'string', description: '触发后执行的任务（add 时必填）' }, url: { type: 'string', description: '监听地址（可选）' } }, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'hooks',
      description: 'Claude Code / Codex hook 桥接。诚实说明：这些是外部工具的内部机制，小满无法桥接——需在任务前后挂钩子请用「外部监工」或 schedule。',
      parameters: { type: 'object', properties: {}, required: [] },
    },
  },
];

/**
 * 把已注册的 MCP 服务器工具（A 类生态，如 team-board 团队画板）转成 OpenAI function。
 * 工具名 mcp__<server>__<tool>，避免与 UEBox/workbench 工具冲突。
 */
async function mcpModelTools() {
  const list = await mcp.modelTools();
  return list.map((m) => ({
    type: 'function',
    function: {
      name: `mcp__${m.server}__${m.tool}`,
      description: `[MCP:${m.server}] ${m.description || ''}`,
      parameters: m.inputSchema || { type: 'object', properties: {} },
    },
  }));
}

/** 执行 MCP 工具（mcp__<server>__<tool>），自动注入该服务器的 toolToken（写鉴权） */
async function executeMcpTool(name, args = {}) {
  const m = name.match(/^mcp__(.+?)__(.+)$/);
  if (!m) return { ok: false, error: `非法 MCP 工具名：${name}` };
  const server = m[1];
  const tool = m[2];
  const cfg = mcp.getServerCfg(server);
  if (!cfg) return { ok: false, error: `MCP 服务器不存在：${server}` };
  if (cfg.toolToken && args && typeof args === 'object' && !Array.isArray(args)) {
    args = { ...args, token: cfg.toolToken };
  }
  return mcp.callTool(server, tool, args);
}

/** 记忆截断：防止过长记忆撞上下文（JSON 序列化后截断） */
function clipMem(o, n = 1500) {
  const s = JSON.stringify(o, null, 2);
  return s.length > n ? s.slice(0, n) + '…' : s;
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

/** 工作台自有工具执行器：直接调后端模块，返回 {ok,...} */
async function executeWorkbenchTool(name, args = {}, sessionId = null) {
  switch (name) {
    case 'workbench_launch_editor': {
      // 打开一个工程（可指定 projectPath，缺省用已选中的工程）
      const target = typeof args?.projectPath === 'string' ? args.projectPath : null;
      const r = await openProject(target);
      if (!r.ok) return r;
      // 编辑器已在运行且连接健康 → 直接返回，不再重复拉起
      if (r.alreadyRunning) return r;
      // 等编辑器装上 UnrealAgentLink 插件后连上工作台的 UAL WS 服务端（首次启动+加载工程较慢，最多 120s）
      const t0 = Date.now();
      while (Date.now() - t0 < 120_000) {
        await sleep(5000);
        const st = ual.ualStatus();
        if (st.connectionCount > 0) {
          const c = st.connections[0];
          return {
            ok: true,
            jobId: r.jobId,
            projectPath: r.projectPath,
            connected: true,
            project: c?.project?.path || '',
          };
        }
      }
      return { ok: true, jobId: r.jobId, projectPath: r.projectPath, connected: false, hint: '编辑器已拉起但插件尚未连上，稍后再查 ual 状态' };
    }
    case 'workbench_compiler_verify': {
      const r = compiler.startVerify();
      if (!r.ok) return r; // 409 已在编译
      // 等待编译完成（增量 ~20-30s，最多 180s）
      const t0 = Date.now();
      while (Date.now() - t0 < 180_000) {
        await sleep(3000);
        const info = compiler.jobInfo();
        if (info.state === 'done' || info.state === 'failed') {
          return {
            ok: true,
            state: info.state,
            exitCode: info.exitCode,
            errors: info.errors,
            durationSec: info.durationSec,
            error: info.error,
          };
        }
      }
      return { ok: true, state: 'running', hint: '编译仍在进行，稍后用 workbench_compiler_status 查' };
    }
    case 'workbench_compiler_status': {
      const info = compiler.jobInfo();
      return { ok: true, ...info };
    }
    case 'workbench_ue5_bootstrap': {
      // 兼容旧工具名：语义改为「打开选中/指定工程」
      const target = typeof args?.projectPath === 'string' ? args.projectPath : null;
      return await openProject(target); // 异步编排，返回 {ok, jobId, projectPath}
    }
    case 'workbench_ue5_close': {
      return await closeProject(); // 关闭当前编辑器（先保存再杀进程）
    }
    case 'workbench_ue5_projects': {
      const r = listProjects();
      return { ok: true, projects: r.projects, scanDirs: r.scanDirs, selectedPath: r.selectedPath };
    }
    case 'workbench_ue5_status': {
      const info = ue5JobInfo();
      const st = ual.ualStatus();
      const elapsedSec = info.startedAt ? Math.max(0, Math.round((Date.now() - new Date(info.startedAt).getTime()) / 1000)) : null;
      const steps = info.steps || [];
      const cur = steps.find((s) => s.state === 'running') || steps[steps.length - 1] || null;
      return {
        ok: true,
        state: info.state, // idle / running / done / failed
        elapsedSec,
        currentStep: cur ? { key: cur.key, label: cur.label, state: cur.state, detail: cur.detail || '' } : null,
        engineConnected: st.connectionCount > 0,
        engineProject: st.connections[0]?.project?.path || null,
        error: info.error,
        bootstrap: info,
      };
    }
    case 'workbench_load_skill': {
      return skills.loadSkill(typeof args?.name === 'string' ? args.name : '');
    }
    
    
    
    
    
    
    
    
    
    case 'bash': {
      return basic.runBash(args);
    }
    case 'read_file': {
      return basic.readFileTool(args);
    }
    case 'write_file': {
      return basic.writeFileTool(args);
    }
    case 'glob': {
      return basic.globTool(args);
    }
    case 'web_fetch': {
      return await basic.webFetchTool(args);
    }
    case 'todo_write': {
      return basic.todoWriteTool(args);
    }
    case 'workbench_present_file': {
      return basic.presentFileTool({ ...args, sessionId });
    }
    case 'edit_file': {
      return basic.editFileTool(args);
    }
    case 'skill_list': {
      return basic.skillListTool();
    }
    case 'ask_user': {
      return basic.askUserTool(args);
    }
    case 'bash_persistent': {
      return await basic.bashPersistentTool(args);
    }
    case 'jobs': {
      return basic.jobsTool(args);
    }
    case 'subagent': {
      return basic.subagentTool();
    }
    case 'workflow': {
      return basic.workflowTool();
    }
    case 'web_search': {
      return await basic.webSearchTool(args);
    }
    case 'pwsh': {
      return await basic.pwshTool(args);
    }
    case 'goal': {
      return basic.goalTool(args);
    }
    case 'schedule': {
      return basic.scheduleTool(args);
    }
    case 'webhook': {
      return basic.webhookTool(args);
    }
    case 'hooks': {
      return basic.hooksTool();
    }
    default:
      return { ok: false, error: `未知工作台工具：${name}` };
  }
}

/** 数据输入引脚：Input 方向 + 非 exec + 非 delegate + 排除 self/target/世界上下文 */
function isDataPin(p) {
  return (
    p &&
    p.dir === 'Input' &&
    p.type !== 'exec' &&
    !String(p.type).startsWith('delegate') &&
    !['self', 'target', '__WorldContext'].includes(p.name)
  );
}

/**
 * 程序化核查（编译通过 ≠ 正确的兜底）：回读图，找出「参与计算链的纯函数」里
 * 悬空的数据输入——既没连线、模型也没设 pin_defaults，但它的输出已被使用。
 * 这种输入会静默取默认值（如乘法 B 悬空 → ReturnValue 恒 0），引擎不报错。
 * 返回悬空清单 [{node, pin}]。
 */
export function verifyGraph(graph, submitted) {
  const nodes = graph?.nodes || [];
  const connections = graph?.connections || [];
  const nodeById = new Map(nodes.map((n) => [n.id, n]));

  // 模型提交的 args 里设了 pin_defaults 的引脚 → 视为有来源，不报
  const hasDefault = new Set();
  for (const n of submitted?.nodes || []) {
    if (n.pin_defaults && typeof n.pin_defaults === 'object') {
      for (const k of Object.keys(n.pin_defaults)) hasDefault.add(`${n.id}.${k}`);
    }
  }
  // 已连线的输入引脚
  const connected = new Set(connections.map((c) => `${c.to_node}.${c.to_pin}`));
  // 有「数据输出」被连出去使用的节点 id
  const usedNode = new Set();
  for (const c of connections) {
    const from = nodeById.get(c.from_node);
    if (!from) {
      usedNode.add(c.from_node);
      continue;
    }
    const pin = (from.pins || []).find((p) => p.name === c.from_pin);
    if (pin && pin.dir === 'Output' && pin.type !== 'exec' && !String(pin.type).startsWith('delegate')) {
      usedNode.add(c.from_node);
    }
  }

  const issues = [];
  for (const n of nodes) {
    const pins = n.pins || [];
    const hasExecInput = pins.some((p) => p.dir === 'Input' && p.type === 'exec');
    const dataOutputs = pins.filter(
      (p) => p.dir === 'Output' && p.type !== 'exec' && !String(p.type).startsWith('delegate')
    );
    const dataInputs = pins.filter(isDataPin);
    // 纯函数特征：无 exec 输入、有数据输出、有数据输入
    if (hasExecInput || dataOutputs.length === 0 || dataInputs.length === 0) continue;
    // 只查「输出已被使用」的节点（在计算链里才需要输入）
    if (!usedNode.has(n.id)) continue;
    for (const inp of dataInputs) {
      const key = `${n.id}.${inp.name}`;
      if (!connected.has(key) && !hasDefault.has(key)) {
        issues.push({ node: n.title || n.member_name || n.class, pin: inp.name });
      }
    }
  }
  return issues;
}

let job = null;
let abortRequested = false; // stopAgent() 置位，run 循环下一轮检查点退出（2026-10-08）
let liveModelReq = null; // 当前在途的模型请求（2026-10-10 用户「立即中断」：stopAgent 直接 destroy，长思考也不用等本轮回合）

/** 提供方 baseUrl 归一化（2026-10-08 用户接入 4090 服务器踩坑）：
 *  modelUrl 必须是完整的 /chat/completions URL（内置种子的写法），
 *  但用户在 UI 里按惯例填 http://host:8400/v1 或裸 host —— 自动补全尾巴。
 *  已含 /chat/completions 的原样返回；/v1 结尾补 /chat/completions；
 *  以 / 结尾补 chat/completions；其余补 /v1/chat/completions。 */
function normalizeModelUrl(url) {
  const u = String(url || '').trim();
  if (!u || u.includes('/chat/completions')) return u;
  if (/\/v1$/.test(u)) return `${u}/chat/completions`;
  if (/\/$/.test(u)) return `${u}chat/completions`;
  return `${u}/v1/chat/completions`;
}

/** 把模型连接层错误翻译成人话（2026-10-07：裸 ECONNREFUSED 甩脸上不友好，用户截图反馈） */
function friendlyModelError(e, cfg) {
  const raw = String(e.message || e);
  let addr = '';
  try {
    const u = new URL(cfg.modelUrl);
    addr = `${u.hostname}:${u.port}`;
  } catch {
    addr = cfg.modelUrl;
  }
  const local = cfg.modelUrl.includes('127.0.0.1') || cfg.modelUrl.includes('localhost');
  if (e?.code === 'ECONNREFUSED' || /ECONNREFUSED/.test(raw)) {
    return local
      ? `模型引擎未启动（${addr} 拒连）——请先在引擎目录双击「一键启动.bat」，等网关就绪后再发消息。（原始错误：${raw}）`
      : `模型服务不可达（${addr} 拒连）——请检查提供方地址与服务状态。（原始错误：${raw}）`;
  }
  if (e?.code === 'ENOTFOUND' || /ENOTFOUND/.test(raw)) {
    return `模型地址无法解析（${addr}）——请检查提供方 URL 是否正确。（原始错误：${raw}）`;
  }
  // 2026-10-08 用户 4090 服务器接入：ETIMEDOUT = 整机不可达（关机/睡眠/断网/防火墙全拦），
  // 与 ECONNREFUSED（机器在但端口没开）区分开，给的指引不同
  if (e?.code === 'ETIMEDOUT' || /ETIMEDOUT|EHOSTUNREACH|ENETUNREACH/.test(raw)) {
    return `模型服务不可达（${addr} 连接超时）——目标机器可能关机/睡眠/断网，或防火墙拦截。请先到那台机器确认开机且服务在跑，再用 curl http://${addr}/v1/models 验证连通。（原始错误：${raw}）`;
  }
  return `模型请求失败：${raw}`;
}

function modelChatOnce(messages, tools, cfg) {
  const budget = enforceRequestBudget(messages, tools, cfg);
  const p = new Promise((resolve, reject) => {
    const body = {
      model: cfg.model,
      messages,
      tools,
      tool_choice: 'auto',
      // 2026-10-10 修「复杂任务 502」：改成流式（stream:true）。根因——非流式要求引擎一次性
      //   生成完整个响应（含满血思考）才返回，复杂任务思考超上游 30s 硬超时 → 502；而流式
      //   首 token 86ms 就回，边思考边输出，永不触发超时（这正是 WorkBuddy 能秒出的原因）。
      stream: true,
      stream_options: { include_usage: true }, // 让引擎在结尾 DONE 前回一个 usage chunk
      temperature: cfg.temperature,
    };
    // thinkingBudget = -1 → Default 档：不传 thinking_budget，让引擎自己决定思考量
    if (Number.isSafeInteger(cfg.thinkingBudget) && cfg.thinkingBudget > 0) {
      body.thinking_budget = cfg.thinkingBudget;
    }
    // 思考档（2026-10-08 修「思考泄漏」）：显式传引擎认识的 reasoning_effort，
    //   让思考内容稳定写进 content 而不是只进 reasoning_content（否则长答案 content 空）。
    if (cfg.thinkingBudget === 0) {
      body.reasoning_effort = 'none';
    } else if (typeof cfg.reasoningEffort === 'string' && cfg.reasoningEffort) {
      body.reasoning_effort = cfg.reasoningEffort;
    }
    body.max_tokens = budget.output;
    const payload = JSON.stringify(body);
    const u = new URL(cfg.modelUrl);
    const headers = { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(payload), Accept: 'text/event-stream' };
    if (cfg.apiKey) headers.Authorization = `Bearer ${cfg.apiKey}`;
    // 2026-10-11 修「云端 API 全部 301 失败」：原用 http.request 硬编码，接 https 提供方（智谱等）
    //   时拿明文 HTTP 连 443 → 服务器回 301 重定向页 → 「模型返回错误：<html>301...」。
    //   按 URL 协议选传输层：https:// 走 https.request（默认 443），http:// 保持原样。
    const transport = u.protocol === 'https:' ? https : http;
    const req = transport.request({
        hostname: u.hostname,
        port: u.port,
        path: u.pathname,
        method: 'POST',
        headers,
        // 流式：连接超时仍用 modelTimeoutMs，但读超时放宽到 5×modelTimeoutMs——因为流式
        //   会持续吐 token，只要在吐就说明活着，不该被单次读超时误杀。
        timeout: cfg.modelTimeoutMs * 5,
      },
      (res) => {
        // 非 200：直接读 body 解析错误（如 502/401），走统一的 reject 路径
        if (res.statusCode !== 200) {
          let d = '';
          res.on('data', (c) => (d += c));
          res.on('end', () => {
            let obj = null;
            try { obj = JSON.parse(d); } catch { /* 非 JSON 也走 reject */ }
            reject(new Error('模型返回错误：' + (obj && obj.error ? JSON.stringify(obj.error) : (d || `HTTP ${res.statusCode}`)).slice(0, 300)));
          });
          res.resume();
          return;
        }
        // 200：解析 SSE 流，重组出与「非流式」一致的 choices[0].message + usage
        let buf = '';
        let content = '';
        let reasoning = '';
        let finishReason = null;
        let usage = {};
        const toolCalls = new Map(); // index → { id, type, name, arguments }
        res.on('data', (chunk) => {
          buf += chunk.toString('utf8');
          let idx;
          while ((idx = buf.indexOf('\n')) >= 0) {
            const line = buf.slice(0, idx).replace(/\r$/, '');
            buf = buf.slice(idx + 1);
            if (!line.startsWith('data:')) continue;
            const data = line.slice(5).trim();
            if (!data || data === '[DONE]') continue;
            let obj;
            try { obj = JSON.parse(data); } catch { continue; }
            if (obj.usage) usage = obj.usage;
            const ch = obj.choices?.[0];
            if (!ch) continue;
            const delta = ch.delta || {};
            if (typeof delta.content === 'string') content += delta.content;
            if (typeof delta.reasoning_content === 'string') {
              reasoning += delta.reasoning_content;
              // 实时思考流：边收边累积到 job.liveThinking，前端轮询即可实时滚动显示
              if (job) job.liveThinking = (job.liveThinking || '') + delta.reasoning_content;
            }
            if (ch.finish_reason) finishReason = ch.finish_reason;
            if (Array.isArray(delta.tool_calls)) {
              for (const tc of delta.tool_calls) {
                const i = tc.index ?? 0;
                const cur = toolCalls.get(i) || { id: tc.id || '', type: tc.type || 'function', name: '', arguments: '', index: i };
                if (tc.id) cur.id = tc.id;
                if (tc.type) cur.type = tc.type;
                if (tc.function?.name) cur.name += tc.function.name;
                if (tc.function?.arguments) cur.arguments += tc.function.arguments;
                toolCalls.set(i, cur);
              }
            }
          }
        });
        res.on('end', () => {
          const tcList = [...toolCalls.values()].sort((a, b) => (a.index ?? 0) - (b.index ?? 0)).map((t) => ({
            id: t.id,
            type: t.type,
            function: { name: t.name, arguments: t.arguments },
          }));
          const message = { role: 'assistant', content: content || null };
          if (reasoning) message.reasoning_content = reasoning;
          if (tcList.length) message.tool_calls = tcList;
          resolve({
            choices: [{ message, finish_reason: finishReason, index: 0, logprobs: null }],
            usage,
          });
        });
        res.on('error', (e) => reject(new Error(friendlyModelError(e, cfg))));
      }
    );
    req.on('timeout', () =>
      req.destroy(new Error(`模型请求超时（已等待 ${Math.round((cfg.modelTimeoutMs * 5) / 1000)}s）——引擎可能在忙或卡住，可稍后重试或查看引擎状态`))
    );
    req.on('error', (e) => reject(new Error(friendlyModelError(e, cfg))));
    liveModelReq = req; // 登记：stopAgent 立即中断当前模型请求用（覆盖式，下个请求自动替换）
    req.write(payload);
    req.end();
  });
  return p;
}

/**
 * 临时性错误判定（2026-10-08）：长任务跑几十轮，一次 502/连接抖动就把整个任务
 * throw 掉太伤（实测：24 轮 107 步毁于一次 502）。只有「明确的临时性失败」值得重试：
 *   - 上游 5xx（Bad Gateway / proxy error / 服务重启中）
 *   - 连接层错误（ECONNRESET / ECONNREFUSED / ETIMEDOUT —— TCP 连接都没成或被断）
 * 不重试（重试也不会好或反而加压）：
 *   - 4xx（401 密钥错、400 参数错）
 *   - 读超时（请求已被引擎接住在算，重试 = 双倍负载；该调大 modelTimeoutMs）
 */
const RETRYABLE_RE = /proxy error|Bad Gateway|502|503|504|Service Unavailable|ECONNRESET|ECONNREFUSED|ENOTFOUND|ETIMEDOUT|socket hang up|EPIPE/i;

export function isRetryableModelError(e) {
  const msg = String((e && e.message) || e);
  if (e && e.retryable === false) return false;
  if (e && e.retryable === true) return true;
  return RETRYABLE_RE.test(msg);
}

/**
 * 工具结果 → 回传给模型的 tool message 内容。
 *
 * 失败时不能只回传一句 summary：插件很多失败响应（如 create_graph 的整批回滚）
 * 是 HTTP 200 + { ok:false, message:"Nothing was applied...", errors:[具体原因...] }，
 * 具体原因在 errors[] / details / data 里。只回 {error:summary} 的话，模型看不到
 * 「'forloop' is neither a node id nor a GUID」这种能直接指导改参数的行，只能瞎猜
 * —— 2026-10-08 蓝图补线任务里模型就这么被拖进死循环的。
 *
 * 成功：原样透传 data；失败：{ ...data, error: summary }（error 放最后，summary 兜底，
 * data 里的 error 字段若与 summary 同值会被覆盖成同值，无碍；不同则以 executeUalTool
 * 归一化的 summary 为准）。
 */
export function toolResultContent(res) {
  if (res && res.ok) {
    // 有 data 字段 → 透传 data（executeUalTool 的 { ok, data } 包装）
    // 无 data 字段 → workbench 工具平铺返回（如 nodeSearch 的 { ok, query, total, hits }），
    //   去掉 ok/contract 元字段后整体回传。否则模型只看到 {ok:true}，真实结果被吞。
    if (res.data != null) return JSON.stringify(res.data);
    const { ok: _ok, contract: _contract, ...rest } = res;
    return JSON.stringify(Object.keys(rest).length ? rest : { ok: true });
  }
  const summary = (res && res.error) || '命令执行失败';
  if (!res || res.data == null) {
    return JSON.stringify({ ok: false, error: summary });
  }
  const d = res.data && typeof res.data === 'object' ? res.data : { data: res.data };
  return JSON.stringify({ ...d, ok: false, error: summary });
}

/** 单次模型调用 → 归一化错误：body 是 error 结构（HTTP 200 但上游透传错误）也转成 reject */
async function modelChatChecked(messages, tools, cfg) {
  const r = await modelChatOnce(messages, tools, cfg);
  if (r && r.error && !r.choices) {
    const code = r.error.code;
    const e = new Error('模型返回错误：' + JSON.stringify(r.error).slice(0, 300));
    // code 是数字时 5xx 可重试；非数字（如字符串/缺失）看 message 特征
    e.retryable = typeof code === 'number' ? code >= 500 : undefined;
    throw e;
  }
  return r;
}

/** 带重试的模型调用：临时性错误指数退避，最多 3 次重试。
 *  2026-10-10 复盘：502 是「服务端网关秒拒」，退避太长（10s/20s/40s）在持续抖动时
 *  反而让任务卡 70s+ 才失败——用户看到「一直转圈」。改回 3s/6s/12s（共约 20s+请求时间）：
 *  短抖动能扛，持续抖动快速失败让用户重试，而不是干等。 */
async function modelChat(messages, tools, cfg) {
  const RETRIES = 3;
  let lastErr;
  for (let attempt = 0; attempt <= RETRIES; attempt++) {
    try {
      return await modelChatChecked(messages, tools, cfg);
    } catch (e) {
      lastErr = e;
      if (attempt === RETRIES || !isRetryableModelError(e)) throw e;
      const wait = 3000 * 2 ** attempt;
      console.log(`[agent] 模型请求临时失败（${String(e.message).slice(0, 120)}），${wait / 1000}s 后重试 ${attempt + 1}/${RETRIES}`);
      await new Promise((r) => setTimeout(r, wait));
    }
  }
  throw lastErr;
}

// ══ 附件层（2026-10-08 用户：添加文件要真内容，不是只塞文件名）════════
// 支持图片/视频/音频/文档/skill 等形式：
//   文档/代码 → 真读内容进上下文（fenced block）
//   skill     → SKILL.md 正文 + 附属文件清单（接 skill 库）
//   图片      → base64 dataURL → OpenAI 图文 multipart（档位不支持时优雅降级并如实说明）
//   音频 wav  → 走本地 ASR（sherpa-onnx）转写进上下文；其他音频如实标注
//   视频      → 模型看不了，注入路径+大小，让模型可用工具处理（诚实，不装能看）
const ATTACH_TEXT_EXT = new Set([
  'md', 'markdown', 'txt', 'log', 'json', 'jsonc', 'csv', 'tsv', 'yaml', 'yml', 'toml', 'ini', 'cfg', 'env',
  'js', 'mjs', 'cjs', 'ts', 'tsx', 'jsx', 'vue', 'py', 'pyi', 'cpp', 'cc', 'cxx', 'c', 'h', 'hpp', 'hh', 'inl',
  'cs', 'java', 'kt', 'go', 'rs', 'rb', 'php', 'sh', 'bat', 'ps1', 'sql', 'html', 'htm', 'css', 'scss', 'less',
  'xml', 'svg', 'gitignore', 'gitattributes', 'cmake', 'gradle', 'properties', 'proto', 'graphql', 'prisma',
]);
const ATTACH_IMAGE_EXT = new Set(['png', 'jpg', 'jpeg', 'webp', 'gif', 'bmp']);
const ATTACH_AUDIO_EXT = new Set(['wav', 'mp3', 'm4a', 'ogg', 'flac', 'aac']);
const ATTACH_VIDEO_EXT = new Set(['mp4', 'mkv', 'avi', 'mov', 'webm', 'wmv', 'flv']);
const ATTACH_TEXT_CAP = 256 * 1024;   // 文档注入上限（字节）
const ATTACH_IMAGE_CAP = 6 * 1024 * 1024; // 单图上限（base64 前）
const ATTACH_IMAGE_MAX = 4;           // 单条任务最多带 4 张图
const ATTACH_MAX_FILES = 10;

function extOf(name) {
  const m = /\.([A-Za-z0-9]+)$/.exec(String(name || ''));
  return m ? m[1].toLowerCase() : '';
}

function kindOf(name) {
  const ext = extOf(name);
  if (/^skill\.md$/i.test(String(name || '').split(/[\\/]/).pop() || '')) return 'skill';
  if (ATTACH_IMAGE_EXT.has(ext)) return 'image';
  if (ATTACH_AUDIO_EXT.has(ext)) return 'audio';
  if (ATTACH_VIDEO_EXT.has(ext)) return 'video';
  if (ATTACH_TEXT_EXT.has(ext)) return 'doc';
  return 'file';
}

function humanSize(n) {
  if (!Number.isFinite(n)) return '?';
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / 1024 / 1024).toFixed(2)} MB`;
}

/** 归一化前端传来的附件清单（Electron 给 path；纯浏览器给 text/base64 内容） */
function normalizeAttachments(atts) {
  if (!Array.isArray(atts)) return [];
  return atts.slice(0, ATTACH_MAX_FILES).map((a) => ({
    name: String(a?.name || '未命名').slice(0, 200),
    path: typeof a?.path === 'string' && a.path.trim() ? a.path.trim() : null,
    size: Number(a?.size) || 0,
    text: typeof a?.text === 'string' ? a.text : null,
    base64: typeof a?.base64 === 'string' ? a.base64 : null,
    mime: typeof a?.mime === 'string' ? a.mime : '',
  }));
}

/**
 * 解析附件 → { textBlocks, images, resolved }。
 * textBlocks: 注入 user 消息的文本块（文档内容/skill 正文/音频转写/视频说明）
 * images:     [{name, dataUrl}] 走图文 multipart
 * resolved:   每个附件的落点摘要（给 job.ctx / 前端「上下文注入」展示）
 */
export async function resolveAttachments(atts) {
  const list = normalizeAttachments(atts);
  const textBlocks = [];
  const images = [];
  const resolved = [];
  for (const a of list) {
    const kind = kindOf(a.name) === 'file' && ATTACH_IMAGE_EXT.has(extOf(a.name)) ? 'image' : kindOf(a.name);
    // ── skill：SKILL.md → 正文 + 同目录附属清单 ──
    if (/^skill\.md$/i.test(a.name.split(/[\\/]/).pop() || '') || kind === 'skill') {
      let md = a.text;
      let dir = null;
      if (!md && a.path) {
        try {
          md = fs.readFileSync(a.path, 'utf8');
          dir = path.dirname(a.path);
        } catch { /* 落到坏档提示 */ }
      }
      if (md) {
        const body = md.length > ATTACH_TEXT_CAP ? md.slice(0, ATTACH_TEXT_CAP) + '\n…(已截断)' : md;
        let siblings = '';
        if (dir) {
          try {
            const sib = fs.readdirSync(dir).filter((f) => f !== 'SKILL.md');
            if (sib.length) siblings = `\n（同目录附属文件：${sib.join('、')}）`;
          } catch { /* 忽略 */ }
        }
        textBlocks.push(`【附件·skill：${a.name}】\n${body}${siblings}`);
        resolved.push({ name: a.name, kind: 'skill', note: 'skill 正文已注入' });
      } else {
        textBlocks.push(`【附件·skill：${a.name}】（内容读取失败${a.path ? `：${a.path}` : ''}）`);
        resolved.push({ name: a.name, kind: 'skill', note: '读取失败' });
      }
      continue;
    }
    // ── 图片 → multipart（有 path 读盘；浏览器端直接给 base64）──
    if (kind === 'image') {
      let dataUrl = a.base64 && a.base64.startsWith('data:') ? a.base64 : null;
      let size = a.size;
      if (!dataUrl && a.path) {
        try {
          const st = fs.statSync(a.path);
          size = st.size;
          if (st.size <= ATTACH_IMAGE_CAP) {
            const buf = fs.readFileSync(a.path);
            const mime = a.mime || (extOf(a.name) === 'jpg' ? 'image/jpeg' : `image/${extOf(a.name) || 'png'}`);
            dataUrl = `data:${mime};base64,${buf.toString('base64')}`;
          }
        } catch { /* 落到失败提示 */ }
      } else if (!dataUrl && a.base64) {
        dataUrl = a.base64.startsWith('data:') ? a.base64 : `data:${a.mime || 'image/png'};base64,${a.base64}`;
      }
      if (dataUrl && images.length < ATTACH_IMAGE_MAX) {
        images.push({ name: a.name, dataUrl });
        resolved.push({ name: a.name, kind: 'image', note: `以图文形式发给模型（${humanSize(size)}）` });
      } else if (images.length >= ATTACH_IMAGE_MAX) {
        textBlocks.push(`【附件·图片：${a.name}】（超过单条 ${ATTACH_IMAGE_MAX} 张上限，未发送）`);
        resolved.push({ name: a.name, kind: 'image', note: '超上限未发送' });
      } else {
        textBlocks.push(`【附件·图片：${a.name}】（${humanSize(size)}，超出大小上限或读取失败，未发送${a.path ? `；路径：${a.path}` : ''}）`);
        resolved.push({ name: a.name, kind: 'image', note: '未发送（超限/读取失败）' });
      }
      continue;
    }
    // ── 音频：wav 走本地 ASR；其余如实标注 ──
    if (kind === 'audio') {
      const ext = extOf(a.name);
      let buf = null;
      if (a.path) {
        try { buf = fs.readFileSync(a.path); } catch { /* 忽略 */ }
      } else if (a.base64) {
        try { buf = Buffer.from(a.base64.replace(/^data:[^;]+;base64,/, ''), 'base64'); } catch { /* 忽略 */ }
      }
      if (buf && (ext === 'wav' || a.mime === 'audio/wav' || a.mime === 'audio/x-wav')) {
        try {
          const asr = await import('./asr.mjs');
          if (asr.modelReady()) {
            const tr = await asr.transcribeWav(buf);
            const txt = (tr && (tr.text || tr?.[0]?.text)) || '';
            if (txt) {
              textBlocks.push(`【附件·音频转写：${a.name}】\n${txt}`);
              resolved.push({ name: a.name, kind: 'audio', note: `已转写 ${txt.length} 字` });
              continue;
            }
            textBlocks.push(`【附件·音频：${a.name}】（ASR 转写为空）`);
            resolved.push({ name: a.name, kind: 'audio', note: '转写为空' });
            continue;
          }
          textBlocks.push(`【附件·音频：${a.name}】（本地 ASR 模型未就绪，无法转写）`);
          resolved.push({ name: a.name, kind: 'audio', note: 'ASR 未就绪' });
          continue;
        } catch (e) {
          textBlocks.push(`【附件·音频：${a.name}】（转写失败：${String(e.message || e).slice(0, 120)}）`);
          resolved.push({ name: a.name, kind: 'audio', note: '转写失败' });
          continue;
        }
      }
      textBlocks.push(`【附件·音频：${a.name}】（${humanSize(a.size)}${a.path ? `，路径：${a.path}` : ''}。模型不能直接听音频；${ext !== 'wav' ? '如需转写请先转成 wav 再添加' : '可用工具按路径处理该文件'}）`);
      resolved.push({ name: a.name, kind: 'audio', note: '未转写（格式/大小限制）' });
      continue;
    }
    // ── 视频：诚实标注（模型看不了），给路径让模型用工具处理 ──
    if (kind === 'video') {
      textBlocks.push(`【附件·视频：${a.name}】（${humanSize(a.size)}${a.path ? `，路径：${a.path}` : ''}。模型不能直接看视频；如需分析请用工具按路径处理，或让用户截关键帧发图）`);
      resolved.push({ name: a.name, kind: 'video', note: '路径已注入（模型不能直接看视频）' });
      continue;
    }
    // ── 文档/代码/未知文本 → 真读内容 ──
    let text = a.text;
    if (text == null && a.path) {
      try { text = fs.readFileSync(a.path, 'utf8'); } catch { text = null; }
    }
    if (text == null && a.base64) {
      try { text = Buffer.from(a.base64.replace(/^data:[^;]+;base64,/, ''), 'base64').toString('utf8'); } catch { text = null; }
    }
    if (text != null && text.trim()) {
      // 二进制防护：内容开头出现 NUL 字节视为二进制（.exe/.dll 等），如实标注而不是塞乱码
      if (text.slice(0, 1000).includes('\u0000')) {
        textBlocks.push(`【附件：${a.name}】（二进制文件，模型不能直接读${a.path ? `；路径：${a.path}` : ''}。如需处理请用工具按路径操作）`);
        resolved.push({ name: a.name, kind: 'file', note: '二进制文件，未注入内容' });
        continue;
      }
      const truncated = text.length > ATTACH_TEXT_CAP;
      const body = truncated ? text.slice(0, ATTACH_TEXT_CAP) + '\n…(已截断)' : text;
      const lang = extOf(a.name) || '';
      textBlocks.push(`【附件·文档：${a.name}${a.size ? `（${humanSize(a.size)}）` : ''}】\n\`\`\`${lang}\n${body}\n\`\`\``);
      resolved.push({ name: a.name, kind: 'doc', note: truncated ? '内容已注入（截断）' : `内容已注入（${humanSize(a.size)}）` });
    } else {
      textBlocks.push(`【附件：${a.name}】（内容读取失败${a.path ? `：${a.path}` : ''}）`);
      resolved.push({ name: a.name, kind: 'file', note: '读取失败' });
    }
  }
  return { textBlocks, images, resolved };
}

/** 图文请求被拒时的判定（用于优雅降级：剥掉图片重试 + 如实说明） */
const VISION_REJECT_RE = /image|vision|multimodal|multi-modal|content part|invalid content|visual/i;

export function startAgentTask(task, sessionId, attachments, skills, toolGroups, toolNames) {
  if (toolNames !== undefined && (!Array.isArray(toolNames) || toolNames.some(n => typeof n !== 'string'))) return { ok: false, code: 400, error: 'toolNames 必须是字符串数组' };
  if (job && job.state === 'running') {
    return { ok: false, code: 409, error: '小满正在干活', jobId: job.id };
  }
  if (typeof task !== 'string' || !task.trim()) {
    return { ok: false, code: 400, error: 'task 不能为空' };
  }
  abortRequested = false; // 清掉上次 stop 的残留标志
  job = {
    id: `AG-${Date.now()}`,
    state: 'running',
    startedAt: new Date().toISOString(),
    task,
    sessionId: sessionId || null,
    attachments: normalizeAttachments(attachments),
    steps: [],
    answer: null,
    error: null,
    toolCount: 0,
    provider: null,
    // 会话统计（DSH 图5/6）：轮数 / 模型总耗时 / token 用量 / TPS / 缓存命中
    stats: { rounds: 0, modelMs: 0, promptTokens: 0, completionTokens: 0, cachedTokens: 0 },
    // 上下文注入详情（DSH 图7）：这次任务到底带了什么背景
    ctx: null,
    // 系统提示词全文（图7「系统提示词」折叠块；拼完记忆/权限/skill 后的最终版）
    systemPrompt: null,
    // 实时思考流（2026-10-10 用户「暴露思考过程」）：流式推理时 reasoning_content 增量实时累积，
    //   前端轮询拿到后实时滚动显示；每轮结束归入 steps[].thinking，本字段清零等下一轮。
    liveThinking: '',
  };
  // M3 眼：任务开始事件（事件订阅镜像，不调模型）
  events.emit('agent.started', { jobId: job.id, task: task.slice(0, 80) }, { sessionId: sessionId || undefined });
  keepalive.setKeepaliveBusy(true); // 任务在跑，暂停保活（不抢推理槽）
  // 会话标题：从任务文本提取关键词自动命名（实时跟随最新话题；手动改过的标题不覆盖）
  if (sessionId) session.autoTitle(sessionId, task);
  run(task, sessionId, normalizeAttachments(attachments), Array.isArray(skills) ? skills : [], toolGroups || null, toolNames).catch((e) => {
    const msg = String(e.message || e);
    // 用户手动停止 → aborted（区别于真失败的 failed，UI 显示「已停止」而不是「失败」）
    job.state = msg.includes('手动停止') || abortRequested ? 'aborted' : 'failed';
    job.error = msg;
    keepalive.setKeepaliveBusy(false);
    // 用户任务消息保留进会话历史（2026-10-10 用户）：停止/失败后消息不丢，可「编辑重发」续接任务
    if (sessionId && loadConfig().retainSessions) {
      try {
        session.appendMessage(sessionId, { role: 'user', content: task });
      } catch { /* 历史写失败不影响停止 */ }
    }
    events.emit('agent.failed', { jobId: job.id, error: job.error }, { sessionId: sessionId || undefined });
  });
  return { ok: true, jobId: job.id };
}

/**
 * 手动停止当前运行中的任务（2026-10-08 用户）。
 * 置位 abortRequested，run 循环在下一轮检查点退出并把 job 标成 aborted。
 * 不是立刻打断（当前这轮模型请求/工具调用会先跑完），但能避免空转任务无限烧 token。
 */
export function stopAgent() {
  if (!job || job.state !== 'running') {
    return { ok: false, code: 409, error: '当前没有运行中的任务', jobId: job?.id ?? null };
  }
  abortRequested = true;
  // 立即中断（2026-10-10 用户）：直接掐断在途的模型请求（长思考也不等），reject 带
  // 「已被手动停止」标记 → run 抛出 → 任务标 aborted（而不是 failed）。
  if (liveModelReq) {
    try {
      liveModelReq.destroy(new Error('任务已被用户手动停止'));
    } catch { /* 已结束则忽略 */ }
    liveModelReq = null; // 已掐断，清登记（防误伤后续无关请求；下个请求会重新登记）
  }
  return { ok: true, jobId: job.id, note: '已停止任务' };
}

/**
 * 构造系统提示词（run 与 contextUsage 共用，防两处漂移）。
 * 分段结构：底座（SYSTEM_PROMPT）+ 预设专精段（按会话 pinTarget 查 presets.mjs，
 * 旧场景 id 'ue5-dev'/'control-room' 在 presets 里做兼容映射；未知/无绑定 → 仅底座）
 * + 记忆 / 技能清单 / 权限段。
 */
function buildPromptParts(sessionId, cfg, activeSkills = []) {
  const sess = sessionId ? session.getSession(sessionId) : null;
  const presetId = sess?.pinTarget ?? null;
  // 聊天模式（presets.mjs 里 chat:true 的预设）：去掉底座身份、预设专属段、记忆、skill 清单，
  // 仅保留一行「权限模式」安全约束（工具列表仍在，防止聊天时误调破坏性工具）。
  const isChat = isChatPreset(presetId);
  const presetPart = isChat ? '' : presetPrompt(presetId);
  let sysPrompt = isChat ? '' : SYSTEM_PROMPT + (presetPart ? '\n\n' + presetPart : '');
  const sceneMem = sess ? session.getSceneMemory(sess.pinTarget) : null;
  const globalMem = session.getGlobalMemory();
  const memParts = [];
  if (!isChat) {
    if (globalMem && Object.keys(globalMem).length) {
      memParts.push('【全局记忆（跨场景长期，可用作背景）】\n' + clipMem(globalMem));
    }
    if (sceneMem && Object.keys(sceneMem).length) {
      memParts.push(`【场景记忆（当前场景 ${sess.pinTarget}）】\n` + clipMem(sceneMem));
    }
    if (memParts.length) {
      sysPrompt += '\n\n' + memParts.join('\n\n');
    }
  }
  // skill 清单（2026-10-09 用户：选择性加载）——
  //   activeSkills = null → 内部兼容入口加载全部（公开任务入口默认 []）；
  //   activeSkills = [] → 不注入任何 skill（默认，用户没选就不带，省 token + 提速）；
  //   activeSkills = [names] → 只注入选中的 skill。
  const allSkillMetas = isChat ? [] : skills.listSkills().skills;
  const skillNames = isChat
    ? []
    : activeSkills === null
      ? allSkillMetas.map((s) => s.name)
      : allSkillMetas.filter((s) => activeSkills.includes(s.name)).map((s) => s.name);
  const skillSummaries = isChat
    ? ''
    : activeSkills === null
      ? skills.modelSkillSummaries()
      : skills.modelSkillSummaries(activeSkills);
  if (skillSummaries) {
    sysPrompt += '\n\n【本次加载的 skill（需要详细步骤时，用 workbench_load_skill 加载其正文）】\n' + skillSummaries;
  }
  // 权限模式（WorkBuddy「允许完全访问」的对应实现）：不是空开关——直接改写行为约束。
  // 聊天模式下也保留：工具列表仍在，这是防误操作的唯一安全底线。
  const permPart = cfg.allowFullAccess
    ? '\n\n【权限模式】允许完全访问：可直接执行全部工具调用，无需逐步确认；用户已知悉数据安全风险。'
    : '\n\n【权限模式】标准受限：执行删除文件、覆盖资产、改配置等破坏性操作前，先用一句话说明将要做什么并征得用户同意，得到确认后再动手。';
  sysPrompt += permPart;
  return { sess, sceneMem, globalMem, memParts, skillNames, skillSummaries, permPart, presetPart, presetId, isChat, sysPrompt };
}

/**
 * 上下文压缩：把旧对话历史总结成一段摘要（2026-10-10 用户）。
 * 调一次模型（不带工具、低温度、限长），把 old 消息串成一条 user 让模型概括。
 * 返回摘要字符串；失败抛错由调用方兜底（原样用完整历史）。
 */
async function summarizeHistory(old, cfg) {
  const lines = old.map((h) => `${h.role === 'user' ? '用户' : '小满'}：${String(h.content || '').slice(0, 2000)}`).join('\n\n');
  const prompt = `下面是之前对话的历史（按时间顺序）。请用简洁的中文概括成一段「前文摘要」，保留：①用户的目标/诉求；②已经完成的关键步骤与结论；③当前待办或未解决的问题。控制在 400 字以内，只输出摘要本身，不要任何解释。\n\n${lines}`;
  const r = await modelChatOnce(
    [{ role: 'user', content: prompt }],
    [],
    { ...cfg, temperature: 0.2, maxOutputTokens: Math.min(1024, Math.floor(cfg.contextWindow / 4)) }
  );
  const msg = r?.choices?.[0]?.message;
  const text = String(msg?.content || msg?.reasoning_content || '').trim();
  return text || null;
}

/**
 * 上下文用量（对标 UEBox「上下文用量」面板）：
 * 分类估算当前会话下一次请求将携带的 token 构成。
 */
async function selectTools(sessionId, cfg, toolGroups, toolNames) {
  const sess = sessionId ? session.getSession(sessionId) : null;
  const ueWanted = typeof toolGroups?.ue === 'boolean' ? toolGroups.ue : isUe5Preset(sess?.pinTarget);
  const ueTools = ueWanted ? ualModelTools(cfg.allowFullAccess) : [];
  const mcpTools = await mcpModelTools();
  const enabled = new Set(basic.loadEnabledTools());
  const workbenchTools = WORKBENCH_TOOLS.filter(t => t.function.name.startsWith('workbench_') || enabled.has(t.function.name));
  const selected = Array.isArray(toolNames) ? new Set(toolNames) : null;
  const filter = ts => selected ? ts.filter(t => selected.has(t.function.name)) : ts;
  return {ueTools: filter(ueTools), workbenchTools: filter(workbenchTools), mcpTools: filter(mcpTools)};
}
function workspacePrompt() {
  const activeWs = getActiveWorkspace();
  return activeWs ? `\n\n【当前工作区】${activeWs}\n所有产出文件默认写入此目录；相对路径以此目录为根。用户明确指定其他保存位置时按用户要求，并说明实际路径。` : '';
}
export async function contextUsage(sessionId, activeSkills = [], toolGroups = null, toolNames) {
  const cfg = loadConfig();
  const pp = buildPromptParts(sessionId, cfg, activeSkills);
  const {ueTools, workbenchTools, mcpTools} = await selectTools(sessionId, cfg, toolGroups, toolNames);
  const history = cfg.retainSessions && sessionId ? session.getMessages(sessionId) : [];
  const workspace = workspacePrompt();
  const parts = [
    {key: 'system', label: '系统提示词', tokens: estTokens(pp.sysPrompt) - estTokens(pp.presetPart) - estTokens(pp.skillSummaries || '') - estTokens(pp.memParts.join('\n\n')) + estTokens(workspace)},
    {key: 'preset', label: '预设专精', tokens: estTokens(pp.presetPart)},
    {key: 'tools', label: '已选工具', tokens: estTokens([...ueTools, ...workbenchTools])},
    {key: 'mcp', label: '连接器及 MCP', tokens: estTokens(mcpTools)},
    {key: 'skills', label: '已选技能', tokens: estTokens(pp.skillSummaries || '')},
    {key: 'memory', label: '记忆', tokens: estTokens(pp.memParts.join('\n\n'))},
    {key: 'messages', label: '对话消息', tokens: history.reduce((n,h) => n + estTokens(h.content) + 8, 0)},
  ];
  const used = parts.reduce((n,p) => n + p.tokens, 0);
  const budget = requestBudget([{role: 'system', content: pp.sysPrompt + workspace}, ...history], [...ueTools, ...workbenchTools, ...mcpTools], cfg);
  return {contract: 'v1', ok: true, estimated: true, sessionId: sessionId || null, contextWindow: budget.window, used,
    ratio: Math.min(used / budget.window, 1), retainSessions: Boolean(cfg.retainSessions), historyMessages: history.length,
    toolCount: ueTools.length + workbenchTools.length + mcpTools.length, skills: pp.skillNames, outputReserve: budget.output,
    note: '估算当前背景；待发任务、附件及下一轮工具结果未包含。模型实际 tokenizer 为准。',
    parts: parts.map(p => ({...p, ratio: p.tokens / budget.window}))};
}

async function run(task, sessionId, attachments = [], activeSkills = [], toolGroups = null, toolNames) {
  const cfg = loadConfig();
  // 模型提供方（2026-10-07）：激活的提供方优先于 agent.json 的 modelUrl/model。
  // 本地引擎无 apiKey；云端提供方带 key 时 modelChat 自动加 Authorization。
  const prov = providers.activeProvider();
  if (prov) {
    cfg.modelUrl = normalizeModelUrl(prov.baseUrl); // 2026-10-08：UI 里填 .../v1 或裸地址自动补 /chat/completions
    cfg.model = prov.model;
    if (prov.apiKey) cfg.apiKey = prov.apiKey;
    job.provider = prov.name;
  } else {
    job.provider = null;
  }
  // UE 工具：内置（ual-tools，直连 UnrealAgentLink 插件，摆脱 MCP）。
  // 工具定义是静态的——引擎没连时工具仍在手上，调用会如实返回「引擎未连接」
  // （对齐 UEBox 的诚实语义），而不是凭空编一个成功结果。
  // 2026-10-10 用户「发任务慢/502」优化：166 个 UAL 工具 schema ≈62KB，是请求体最大头——
  //   全量传入让 prefill 从 ~1s 拖到 11s+，再叠加历史/MCP 时超过服务端网关超时 → 502。
  //   工具组两级控制：①用户显式选择（toolGroups.ue，随任务传入）优先；②未选时按预设自动——
  //   UE5 类预设（category=ue5）才带 UAL 工具，其他预设不带，prefill 回到 1-2s。
  const {ueTools, workbenchTools, mcpTools} = await selectTools(sessionId, cfg, toolGroups, toolNames);
  const tools = [...ueTools, ...workbenchTools, ...mcpTools];
  const offeredNames = new Set(tools.map(t => t.function.name));
  job.toolCount = tools.length;

  // 防串台核心：只加载「这个 sessionId」的历史作为上下文前缀。
  // 会话历史只存 user/assistant 的最终 content（不含中间 tool 调用），拼回干净。
  // retainSessions=false（保留关闭）：不加载历史——每条任务独立，即「一句一句」模式。
  const history = cfg.retainSessions && sessionId ? session.getMessages(sessionId) : [];

  // 记忆注入 + skill 清单 + 权限段：构造逻辑收敛在 buildPromptParts（与用量面板共用，防两处漂移）
  const pp = buildPromptParts(sessionId, cfg, activeSkills);
  // 工作区注入（2026-10-10 用户「产出物放进工作区」）：模型不知道用户选的工作区路径，
  //   产出文件全写去别处（如 outputs）。注入路径 + 硬规则：产出文件一律写进工作区；
  //   相对路径以工作区为基。无工作区时保持原行为。
  const activeWs = getActiveWorkspace();
  const sysPrompt = pp.sysPrompt + workspacePrompt();
  job.workspace = activeWs;
  const sess = pp.sess;
  if (pp.memParts.length) {
    job.memory = {
      sceneId: pp.sess?.pinTarget ?? null,
      global: Boolean(pp.globalMem && Object.keys(pp.globalMem).length),
      scene: Boolean(pp.sceneMem && Object.keys(pp.sceneMem).length),
    };
  }

  // 上下文注入详情（DSH 图7「上下文注入」折叠块的数据源）
  const skillNames = pp.skillNames;

  // ── 附件解析（2026-10-08 用户：添加文件要真内容）──
  // 文档/skill/音频转写 → 文本块拼进任务；图片 → 图文 multipart（档位不支持时降级）。
  let attachTextBlocks = [];
  let attachImages = [];
  let attachResolved = [];
  if (attachments.length) {
    try {
      const ar = await resolveAttachments(attachments);
      attachTextBlocks = ar.textBlocks;
      attachImages = ar.images;
      attachResolved = ar.resolved;
    } catch (e) {
      job.steps.push({ type: 'attach', error: String(e.message || e).slice(0, 200) });
    }
  }
  const fullTask = attachTextBlocks.length ? task + '\n\n' + attachTextBlocks.join('\n\n') : task;
  // 图片 multipart：有图时 user content 变数组；第一轮模型响应后换回纯文本（避免每轮重发 base64）
  let hasImages = attachImages.length > 0;
  const buildUserContent = (withImages) => {
    if (withImages && attachImages.length) {
      return [
        { type: 'text', text: fullTask + '\n\n（附图见本条消息的 image 部分）' },
        ...attachImages.map((im) => ({ type: 'image_url', image_url: { url: im.dataUrl } })),
      ];
    }
    return fullTask;
  };

  // 系统提示词全文暴露（DSH 图7「系统提示词」折叠块）
  job.systemPrompt = sysPrompt;
  // 上下文注入详情（DSH 图7「上下文注入」折叠块）
  job.ctx = {
    sceneId: pp.sess?.pinTarget ?? null,
    memoryGlobalKeys: pp.globalMem ? Object.keys(pp.globalMem).length : 0,
    memorySceneKeys: pp.sceneMem ? Object.keys(pp.sceneMem).length : 0,
    historyMessages: history.length,
    skills: skillNames,
    attachments: attachResolved,
    attachmentImages: attachImages.length,
  };

  const messages = [
    { role: 'system', content: sysPrompt },
    ...history.map((h) => ({ role: h.role, content: h.content })),
    { role: 'user', content: buildUserContent(hasImages) },
  ];

  // ── 上下文自动压缩（2026-10-10 用户：历史接近窗口上限时，把旧对话摘要化，防打满截断）──
  // 只在「保留会话 + 历史足够长」时压缩；压缩 = 调一次模型把旧历史总结成摘要，替换旧逐条消息。
  const COMPACT_AT_MESSAGES = 12; // 历史超过 12 条才开始考虑压缩
  const win = cfg.contextWindow || 131072;
  const COMPACT_AT_TOKENS = Math.max(256, Math.floor(win * 0.2)); // 历史 token 超窗口 20% 就压（留足本轮+工具往返空间）
  const histTok = history.reduce((n, h) => n + estTokens(h.content) + 8, 0);
  if (history.length > COMPACT_AT_MESSAGES && histTok > COMPACT_AT_TOKENS) {
    // 保留最近 4 条原文，其余压成摘要
    const recent = history.slice(-4);
    const old = history.slice(0, -4);
    try {
      const sum = await summarizeHistory(old, cfg);
      if (sum) {
        messages.splice(1, history.length, { role: 'system', content: `【前文摘要】\n${sum}` }, ...recent.map((h) => ({ role: h.role, content: h.content })));
        job.ctx.compacted = { fromMessages: old.length, keptMessages: recent.length, summaryChars: sum.length, histTokens: histTok };
      }
    } catch {
      /* 压缩失败（模型离线等）不阻断：原样用完整历史，后续预算检查会如实阻止超限请求 */
    }
  }

  // maxIterations <= 0 = 无上限（2026-10-08 用户要求）：循环只靠模型「给最终答复」退出，
  // 每轮仍受 modelTimeoutMs 单次请求超时约束。慎用：模型停不下来时会一直跑。
  const maxIters = cfg.maxIterations > 0 ? cfg.maxIterations : Infinity;

  // 防呆：同工具 + 同参数连续失败计数（2026-10-08）。
  // 模型对同一失败调用原样重试（如 describe 用假路径连查两次）时，结果不会变，
  // 只会空转烧 token。检测到 ≥2 次后注入反馈，逼它从错误信息里提取修正线索。
  let failStreak = null; // { key, count }
  const failKey = (name, args) => `${name}::${JSON.stringify(args || {})}`;
  let toolStreak = null; // { tool, count }
  let finalNudgeUsed = false; // 最终回答为空时补问一次（防死循环）

  for (let i = 0; i < maxIters; i++) {
    // 手动停止检查点：stopAgent() 置位后，本轮回合结束即退出（不等模型再算一轮）
    if (abortRequested) {
      job.state = 'aborted';
      job.error = '任务已被手动停止';
      return;
    }
    const t0 = Date.now();
    // 图文请求：第一轮若被模型侧拒绝（档位/提供方不支持 vision），剥图降级重试 + 如实标注
    let r;
    try {
      r = await modelChat(messages, tools, cfg);
    } catch (e) {
      if (hasImages && VISION_REJECT_RE.test(String(e.message || e))) {
        hasImages = false;
        messages[messages.length - 1] = {
          role: 'user',
          content: fullTask + '\n\n（注：你随任务附发了图片，但当前模型/档位不支持图文输入，图片未能发送。如需看图请切到看图档或支持 vision 的提供方。）',
        };
        job.steps.push({ type: 'attach', note: '图片被模型侧拒绝（当前档位不支持图文），已降级为纯文本继续' });
        r = await modelChat(messages, tools, cfg);
      } else {
        throw e;
      }
    }
    // 首轮成功后把 multipart user content 换回纯文本：避免后续每轮重发 base64（省 token）
    if (hasImages && i === 0) {
      hasImages = false;
      messages[messages.length - 1] = {
        role: 'user',
        content: fullTask + `\n\n（注：本任务首轮已附发 ${attachImages.length} 张图片：${attachImages.map((im) => im.name).join('、')}）`,
      };
    }
    const choice = r.choices?.[0];
    const msg = choice?.message;
    // 会话统计（DSH 图5/6）：每轮累计 token / 耗时；usage 来自 OpenAI 兼容响应
    const usage = r.usage || {};
    const st = job.stats;
    st.rounds += 1;
    st.modelMs += Date.now() - t0;
    st.promptTokens += usage.prompt_tokens ?? 0;
    st.completionTokens += usage.completion_tokens ?? 0;
    st.cachedTokens += usage.prompt_tokens_details?.cached_tokens ?? 0;
    const stepUsage = {
      promptTokens: usage.prompt_tokens ?? 0,
      completionTokens: usage.completion_tokens ?? 0,
      cachedTokens: usage.prompt_tokens_details?.cached_tokens ?? 0,
    };
    if (!msg) {
      job.steps.push({ type: 'model', ms: Date.now() - t0, usage: stepUsage, error: JSON.stringify(r).slice(0, 300) });
      throw new Error('模型返回无 message：' + JSON.stringify(r).slice(0, 300));
    }

    if (choice.finish_reason === 'tool_calls' && Array.isArray(msg.tool_calls) && msg.tool_calls.length) {
      job.steps.push({
        type: 'model',
        ms: Date.now() - t0,
        tool_calls: msg.tool_calls.map((tc) => ({ name: tc.function.name, arguments: tc.function.arguments })),
        thinking: (msg.reasoning_content || '').slice(0, 4000), // 全文给「已思考」折叠块，不再截 300
        usage: stepUsage,
      });
      job.liveThinking = ''; // 本轮思考已归档进 steps[].thinking，清零等下一轮
      messages.push({ role: 'assistant', content: msg.content ?? null, tool_calls: msg.tool_calls });
      let lastApplyGraph = null;
      for (const tc of msg.tool_calls) {
        const name = tc.function.name;
        let args = {};
        try {
          args = JSON.parse(tc.function.arguments || '{}');
        } catch {
          args = { __raw: tc.function.arguments };
        }
        const s0 = Date.now();
        const ualTool = findUalTool(name);
        // 2026-10-10 修「write_file/bash 未内置」：分发只认 workbench_/mcp__/ual 三种前缀，
        //   漏掉了裸名通用工具（write_file/bash/read_file 等，它们在 WORKBENCH_TOOL_META 里）。
        //   导致模型明明调了 write_file，却走「工具未内置」兜底。改为：名字在 workbench 工具
        //   清单（workbench_ 前缀 或 裸名通用工具）里 → 都路由到 executeWorkbenchTool。
        const isWorkbenchTool = name.startsWith('workbench_') || basic.WORKBENCH_TOOL_META.some((t) => t.name === name);
        const res = !offeredNames.has(name)
          ? { ok: false, error: `tool_not_offered：本轮未启用工具 ${name}，未执行` }
          : isWorkbenchTool
          ? await executeWorkbenchTool(name, args, sessionId)
          : name.startsWith('mcp__')
            ? await executeMcpTool(name, args)
            : ualTool
              ? await executeUalTool(ualTool, args, cfg.toolTimeoutMs)
              : { ok: false, error: `工具未内置：${name}（不在工作台 UE 工具清单里）` };
        const content = toolResultContent(res);
        messages.push({ role: 'tool', tool_call_id: tc.id, content });
        job.steps.push({
          type: 'tool',
          name,
          args,
          ok: res.ok,
          ms: Date.now() - s0,
          result: content.slice(0, 400),
        });

        // 防呆追踪（两级）：
        //   failStreak —— 同工具+同参数连续失败（≥2 注入：原样重试不会变）
        //   toolStreak —— 同工具连续失败不论参数（≥5 注入：换着花样的「变体轰炸」，
        //                 如 add_variable 换 4 种 type 写法连环被拒，也是空转烧 token）
        const fk = failKey(name, args);
        if (res.ok) {
          failStreak = null;
          toolStreak = null;
        } else {
          failStreak = failStreak && failStreak.key === fk ? { key: fk, count: failStreak.count + 1 } : { key: fk, count: 1 };
          toolStreak = toolStreak && toolStreak.tool === name ? { tool: name, count: toolStreak.count + 1 } : { tool: name, count: 1 };
        }

        if (name === 'blueprint_apply_graph' && res.ok) lastApplyGraph = args;
      }

      // 防呆注入（先判严格档）：同工具+同参数 ≥2 次 → 原样重试不会变结果。
      if (failStreak && failStreak.count >= 2) {
        const [fname] = failStreak.key.split('::');
        const fb =
          `⚠️ 连续 ${failStreak.count} 次用相同参数调用「${fname}」都失败了——原样重试不会改变结果。` +
          `请停下重试，先读最近几条工具报错里的修正线索（比如真实路径、正确字段名或取值范围），` +
          `据此换参数或换一种工具/路径再试；若信息不足，先用只读工具（如 ${fname.startsWith('blueprint') ? 'blueprint_search_nodes' : 'blueprint_describe'}）把现状查清楚。`;
        messages.push({ role: 'user', content: fb });
        job.steps.push({ type: 'guard', tool: fname, count: failStreak.count });
        failStreak = null; // 注入一次即复位，避免每轮重复刷屏
      } else if (toolStreak && toolStreak.count >= 5) {
        // 变体轰炸档：同工具连败 ≥5 次（参数在变但一直被拒）→ 换个角度提醒。
        const fb =
          `⚠️ 「${toolStreak.tool}」已经连续失败 ${toolStreak.count} 次了。你在反复尝试同一件事的不同写法，` +
          `但报错一直没变——说明问题可能不在参数拼写上，而在别处（工具的能力边界、字段格式、前置条件）。` +
          `请仔细逐字读一次完整报错，考虑：① 用 workbench_load_skill 加载相关 skill 看正确用法；` +
          `② 换一种完全不同的实现路径；③ 或直接告诉用户这里被卡住、需要人确认。别再换写法硬试了。`;
        messages.push({ role: 'user', content: fb });
        job.steps.push({ type: 'guard', tool: toolStreak.tool, count: toolStreak.count, kind: 'tool-streak' });
        toolStreak = null;
      }

      // 程序化核查兜底：apply_graph 成功后回读图，查纯函数输入是否连满。
      // 发现悬空输入 → 额外塞一条反馈给模型，让它修正（比靠模型自觉更可靠）。
      if (lastApplyGraph?.blueprint_path) {
        try {
          const g = await ual.ualCall('blueprint.get_graph', { blueprint_path: lastApplyGraph.blueprint_path }, 60_000);
          if (g) {
            const issues = verifyGraph(g, lastApplyGraph);
            if (issues.length) {
              const fb =
                '⚠️ 程序化核查：你刚写的蓝图有悬空输入（编译通过但逻辑可能失效，结果会静默取默认值）：\n' +
                issues.map((i) => `- 节点「${i.node}」的输入引脚「${i.pin}」没连线、也没设 pin_defaults，但它的输出已被使用。`).join('\n') +
                '\n请用 blueprint_apply_graph（加 clear_existing:true 重写完整图）修正：确保每个纯函数的每个输入引脚都有来源——要么连线，要么 pin_defaults 设字符串默认值。修正后再回读确认。';
              messages.push({ role: 'user', content: fb });
              job.steps.push({ type: 'verify', issues: issues.map((i) => `${i.node}.${i.pin}`) });
            }
          }
        } catch {
          /* 核查失败不阻断 agent */
        }
      }
      continue;
    }

    // 最终回答（2026-10-08 修「done 但 result 空」：真机 WorkBuddy 监工实测，搜索类长任务
    //   最终轮 content 可能为空——内容全在 reasoning_content 里、或上下文打满被截断。
    //   静默返回空串会让外部监工拿到 result="" 无法验收。三层兜底：补问一次 → 思考摘录 → 说明文字。）
    let contentText = String(msg.content ?? '').trim();
    if (!contentText && !finalNudgeUsed) {
      finalNudgeUsed = true;
      // ★ 措辞关键（2026-10-08 真机踩坑）：绝不能写「不要再调用工具」——
      //   那会让模型明明还没执行 write_file，就放弃工具、口头撒谎「已完成」。
      //   这里中性引导：该调工具就调工具，真完成了才输出正文。
      messages.push({ role: 'user', content: `（系统提示）你上一轮没有输出正文，也没有调用工具（finish_reason=${choice.finish_reason ?? '?'}）。请明确下一步：如果还需要执行操作（写文件、跑命令、查数据等），就调用相应的工具；如果任务已经全部完成，就把最终答案——尤其是代码、HTML、文档这类长内容——完整地写在正文 content 里（不要只写在思考过程中），否则我收不到你的答案。` });
      continue;
    }
    if (!contentText) {
      // 思考泄漏（2026-10-08 真机定位）：NInfer 引擎默认「开思考」，长答案会写进
      // reasoning_content 而 content 恒空。答案其实完整在 reasoning 里——直接当作最终答案，
      // 不再只标「思考摘录」让 result 变相为空。
      const rc = String(msg.reasoning_content ?? '').trim();
      contentText = rc
        ? rc.slice(0, 16000)
        : `（模型本轮未输出内容：finish_reason=${choice.finish_reason ?? '?'}。可能上下文已打满，建议减小任务规模或调大上下文窗口。）`;
    }
    job.steps.push({ type: 'model', ms: Date.now() - t0, answer: contentText, thinking: (msg.reasoning_content || '').slice(0, 4000), usage: stepUsage });
    job.liveThinking = ''; // 最终答复的思考已归档，清零
    job.answer = contentText;
    job.state = 'done';
    keepalive.setKeepaliveBusy(false); // 任务完成，恢复保活
    // M3 眼：任务完成事件
    events.emit('agent.done', { jobId: job.id, answer: contentText.slice(0, 80) }, { sessionId: sessionId || undefined });
    // 写回会话历史（防串台：只写这个 sessionId 的会话；保留关闭时不写——不留存）
    if (sessionId && cfg.retainSessions) {
      try {
        session.appendMessage(sessionId, { role: 'user', content: task });
        session.appendMessage(sessionId, { role: 'assistant', content: contentText });
      } catch {
        /* 写回失败不阻断 */
      }
    }
    return;
  }
  // 无上限时此行不可达（循环只能靠模型给最终答复 return 退出）
  throw new Error(`超过最大迭代次数 ${maxIters === Infinity ? '∞（异常退出）' : maxIters}`);
}

export function agentJobInfo() {
  if (!job) return { contract: 'v1', state: 'idle' };
  // TPS（图5「输出速度」）：输出 token / 模型总耗时秒
  const st = job.stats;
  const tps = st.modelMs > 0 ? Math.round((st.completionTokens / (st.modelMs / 1000)) * 10) / 10 : 0;
  return {
    contract: 'v1',
    state: job.state,
    jobId: job.id,
    startedAt: job.startedAt,
    task: job.task,
    sessionId: job.sessionId,
    provider: job.provider,
    memory: job.memory,
    ctx: job.ctx,
    systemPrompt: job.systemPrompt,
    toolCount: job.toolCount,
    stats: { ...st, tps },
    steps: job.steps,
    answer: job.answer,
    error: job.error,
    liveThinking: job.liveThinking || '',
  };
}

export function stopAll() {
  if (job && job.state === 'running') {
    job.state = 'failed';
    job.error = '后端退出，任务中断';
  }
}

/** 当前正在跑任务的会话 id（无运行任务 → null）。侧栏会话列表给运行中项画转圈用。 */
export function runningSessionId() {
  return job && job.state === 'running' ? job.sessionId : null;
}

// ══ 桌宠轻量对话（2026-10-10 用户「给 xiaomanbox 做个桌宠」）════════
// 桌宠闲聊不走 function-calling 循环（太重）：单次模型调用，无工具、短人设、
// 复用 agent 的 modelChat（含重试与错误归一）与 providers 激活配置。
// 注意：modelChat 引用模块级 liveModelReq/job（仅写 job.liveThinking 时判 job 存在），
// 桌宠与 agent 并发调用不会互相打架（各自 Promise 独立），但 liveModelReq 会被覆盖——
// 桌宠聊天中用户 stopAgent 只会打断 agent 自己的请求，可接受。

const PET_SYSTEM = [
  '你是「小满」，桌面上的Q版桌宠小助手，陪伴主人工作。',
  '说话要自然简短、一句到两句（30字以内），轻松俏皮，说人话不啰嗦。',
  '不解释自己是不是AI，不用 Markdown，就当桌面上的小伙伴聊天。',
].join('\n');

/** 桌宠单轮聊天：{ reply } 或 { ok:false, error }（不进会话历史，一次一个请求） */
export async function petChat(message, history = []) {
  const cfg = loadConfig();
  const prov = providers.activeProvider();
  if (prov) {
    cfg.modelUrl = normalizeModelUrl(prov.baseUrl);
    cfg.model = prov.model;
    cfg.apiKey = prov.apiKey || '';
  }
  const msgs = [{ role: 'system', content: PET_SYSTEM }];
  // 最多带最近 5 轮历史（桌宠闲聊不需要长记忆）
  for (const m of history.slice(-10)) {
    if (m && (m.role === 'user' || m.role === 'assistant') && typeof m.content === 'string' && m.content) {
      msgs.push({ role: m.role, content: m.content.slice(0, 500) });
    }
  }
  msgs.push({ role: 'user', content: String(message || '').slice(0, 1000) });
  const r = await modelChat(msgs, undefined, cfg);
  const reply = r?.choices?.[0]?.message?.content || '';
  if (!reply) throw new Error('模型返回空回复');
  return { ok: true, reply: reply.trim().slice(0, 800) };
}
