/**
 * 外部监工接入端口（2026-10-08 用户）—— 让 GPT / WorkBuddy 等外部 AI 当监工。
 *
 * 与内部 supervisor.mjs（小满盒子自己选个模型当监工自动跑闭环）互补：
 * 这里是「外部 AI 驱动」模式——外部监工通过 HTTP 端口自己控制节奏：
 *   派单（assign）→ 小满执行 → 拉结果（status）→ 验收 → 不通过则带 feedback 重派。
 * 端口本身只负责「执行」，验收判断是外部监工的事（规范见 SUPERVISOR_SKILL）。
 *
 * 复用现有 agent 单例（同一时刻只跑一个执行任务，Agent 面板与此端口共享）。
 */
import * as agent from './agent.mjs';
import * as session from './session.mjs';
import * as presets from './presets.mjs';

/** 派单：外部监工把任务交给小满执行，返回 taskId（= agent jobId）。
 *  一个任务一个会话：新任务自动建会话（任务+产出落在会话里，工作台可见）；
 *  返工时外部监工把上一轮的 sessionId 传回来，复用同一会话（验收历史都在里面）。
 *  pinTarget：按任务类型选预设（UE 工程/蓝图/关卡/运维/策划/文案/数值…），决定小满用哪套系统提示词。 */
export function assign({ task, criteria, feedback, sessionId, pinTarget } = {}) {
  const t = String(task || '').trim();
  if (!t) return { ok: false, code: 400, error: 'task 不能为空' };

  // 预设：按任务类型选，非法/缺省回落默认（ue5-project）
  const target = presets.normalizePresetId(pinTarget) || presets.DEFAULT_PRESET_ID;

  // 会话：传了 sessionId = 返工（复用）；没传 = 新任务（新建会话，标题由 autoTitle 按任务文本起）
  let sid = typeof sessionId === 'string' && sessionId.trim() ? sessionId.trim() : null;
  const isNewSession = !sid;
  if (isNewSession) {
    const s = session.createSession({ pinTarget: target, sessionTitle: '' });
    sid = s.sessionId;
  }

  const fullTask = feedback ? `${t}\n\n（上一轮监工反馈，务必修正）：${String(feedback)}` : t;
  const r = agent.startAgentTask(fullTask, sid);
  if (!r || r.ok === false) {
    // 启动失败（最常见：小满正忙）。新建的空会话删掉，避免留下孤儿会话；返工的会话保留。
    if (isNewSession) session.deleteSession(sid);
    return { ok: false, code: r?.code ?? 409, error: r?.error || '小满拒绝执行' };
  }
  return {
    ok: true,
    taskId: r.jobId,
    sessionId: sid,
    isNewSession,
    pinTarget: target,
    task: t,
    criteria: Array.isArray(criteria) ? criteria.map((c) => String(c)) : [],
  };
}

/** 拉结果：按 taskId 查执行状态（done → result=产出；running → 轮询）。 */
export function workStatus(taskId) {
  if (!taskId) return { ok: false, code: 400, error: '缺 taskId' };
  const info = agent.agentJobInfo();
  if (!info || info.jobId !== taskId) {
    return { ok: false, code: 404, error: '任务不存在或已被新任务替换（小满同时只跑一个任务）' };
  }
  const state = info.state === 'done' ? 'done' : info.state === 'running' ? 'running' : 'error';
  return {
    ok: true,
    state,
    sessionId: info.sessionId ?? null,
    result: state === 'done' ? (info.answer ?? '') : null,
    error: state === 'error' ? (info.error || '执行失败') : null,
  };
}

/**
 * 外部监工接入规范（Skill）—— 用户复制给 GPT / WorkBuddy 等外部 AI。
 * 内容是「协议 + 分工 + 验收铁律 + 严格验收」，外部 AI 照着就能当监工。
 */
const PRESET_LIST = presets.PRESETS.map((p) => `  - ${p.id}：${p.name}（${p.desc}）`).join('\n');

export const SUPERVISOR_SKILL = `# 小满盒子 · 外部监工接入规范（Skill）

## 你的角色：监工
你是「外部监工」。小满（小满盒子里的本地 AI Agent）是你的执行者。你只做两件事：
1. **派单**：把目标拆成小满能执行的任务，附上验收标准
2. **验收**：对照验收标准，判断小满的产出是 PASS 还是 FAIL

## 分工（谁做什么）
- **你（监工）**：拆任务、定验收标准、判结果、给修正建议。不碰执行细节。
- **小满（执行者）**：执行任务、用工具干活、返回产出。不碰验收判断。

## 按任务类型选预设（pinTarget）
小满有不同「预设」，决定它干某类活时用哪套专业知识。派单时按任务类型选一个填进 pinTarget（不传默认 ue5-project）：
${PRESET_LIST}

## 鉴权（跨机 / 外网接入时）
小满盒子后端默认只允许本机（127.0.0.1）访问。跨机 / 外网接入时，每个请求头都要带：
  X-API-Key: <小满盒子主人给你的 API Key>
（Key 从小满盒子的「引擎」面板获取，或直接问主人要。本机调用无需带。）

## 接入端口
小满盒子后端 HTTP 地址（默认）：\`http://127.0.0.1:8871/api/v1\`

### 1. 派单
POST /work/assign
请求体：
{"task":"一句话任务描述（具体、可执行）","criteria":["验收标准1","验收标准2",...],"pinTarget":"按任务类型选的预设 id"}
响应：
{"ok":true,"taskId":"AG-xxx","sessionId":"会话 id","pinTarget":"..."}

### 2. 拉结果（执行是异步的，轮询直到 state 不是 running）
GET /work/status/{taskId}
响应：
{"ok":true,"state":"running"|"done"|"error","result":"执行产出（done 时才有）","error":"失败原因（error 时才有）"}
轮询间隔建议 1~2 秒。

### 3. 验收（你做）
对照 criteria 逐条判断：
- 全过 → PASS，任务结束
- 有没过 → FAIL，**重新派单并带上 sessionId**（返工落回同一个会话）：
  POST /work/assign
  {"task":"...","criteria":[...],"feedback":"具体哪里没过 + 可执行的修正建议","sessionId":"上一轮响应里的 sessionId","pinTarget":"保持和首次派单一致"}
  ⚠️ sessionId 必须传回——它是「一个任务一个会话」的关键：同一任务的返工都落在一个会话里，用户才能在小满盒子看到完整验收历史。不传 = 每轮新开一个会话，历史就散了。

## 验收铁律（负控）
每个验收标准必须是**可证伪**的——能明确说出什么情况算 PASS、什么算 FAIL。
- 坏标准："蓝图写对了"（无法判）
- 好标准："编译通过（exitCode=0）" 或 "蓝图有 N 个节点、M 条连线" 或 "游戏里按 W 角色前移"

## 严格验收（强烈建议）
如果小满的产出涉及「能程序化验证」的东西（编译、回读、探针），
**要求小满在产出里附上验证证据**（如编译 exitCode、回读的节点数），
你据实核对，不要凭感觉 PASS。没有证据的一律算 FAIL。`;

/** 输出一段「严格验收」提示词，附加在派单 criteria 尾部（strict 模式用） */
export const STRICT_CRITERIA_HINT =
  '（严格验收：请在最终产出里附上可程序化核查的证据，如编译 exitCode、回读核对结果、实测数字与来源；没有证据的断言一律视为未通过。）';
