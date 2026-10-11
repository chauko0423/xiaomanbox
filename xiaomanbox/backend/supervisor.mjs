/**
 * 外部监工（2026-10-08 用户）—— 多模型主从编排。
 *
 * 闭环：监工模型（providers.supervisorProvider，云端/本地/任意 OpenAI 兼容 API）
 *   派单 → 小满（providers.activeProvider，本地）执行 → 结果回监工 → 监工验收 → 迭代。
 *
 * 通用性：监工模型 = providers 里任意一个（复用 providers.mjs，DeepSeek/GPT/千问/本地都行），
 *   执行者 = active（本地小满）。两者独立，互不干扰。
 * 验收策略 = 预设（ACCEPT_MODES）：relaxed 主观判 / strict 判据+程序化核查。
 */
import http from 'node:http';
import * as providers from './providers.mjs';
import * as agent from './agent.mjs';

/** 验收策略预设（用户按情况自选） */
export const ACCEPT_MODES = [
  { id: 'relaxed', name: '宽松验收', desc: '监工对照验收标准主观判 PASS/FAIL（先跑通闭环）' },
  { id: 'strict', name: '严格验收', desc: '监工生成可程序化核查的判据，逐条对照（符合负控铁律）' },
];

/** 严格验收：附加给执行者（小满）的提示——产出必须附可程序化核查的证据 */
const STRICT_CRITERIA_HINT =
  '（严格验收：请在最终产出里附上可程序化核查的证据，如编译 exitCode、回读核对结果、实测数字与来源；没有证据的断言一律视为未通过。）';

/** 当前监工任务（单例：同一时刻只跑一个监工） */
let job = null;

/** 轻量 OpenAI 兼容单轮调用（监工派单/验收用，不需要 tools/工具循环） */
function chatOnce(provider, messages, opts = {}) {
  return new Promise((resolve, reject) => {
    const body = {
      model: provider.model,
      messages,
      stream: false,
      temperature: opts.temperature ?? 0.2,
    };
    if (typeof opts.maxTokens === 'number' && opts.maxTokens > 0) body.max_tokens = opts.maxTokens;
    const payload = JSON.stringify(body);
    let u;
    try {
      u = new URL(provider.baseUrl);
    } catch {
      reject(new Error('监工模型 baseUrl 非法：' + provider.baseUrl));
      return;
    }
    const headers = { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(payload) };
    if (provider.apiKey) headers.Authorization = `Bearer ${provider.apiKey}`;
    const req = http.request(
      {
        hostname: u.hostname,
        port: u.port,
        path: u.pathname + u.search,
        method: 'POST',
        headers,
        timeout: opts.timeoutMs ?? 120000,
      },
      (res) => {
        let d = '';
        res.on('data', (c) => (d += c));
        res.on('end', () => {
          try {
            resolve(JSON.parse(d));
          } catch {
            reject(new Error('监工模型响应非 JSON：' + d.slice(0, 200)));
          }
        });
      }
    );
    req.on('timeout', () => req.destroy(new Error('监工模型请求超时（120s）')));
    req.on('error', (e) => reject(e));
    req.write(payload);
    req.end();
  });
}

/** 从模型输出里抽 JSON（容忍 ```json 围栏、前后废话） */
function extractJson(text) {
  const s = String(text || '').trim();
  const fence = s.match(/```(?:json)?\s*([\s\S]*?)```/i);
  const raw = fence ? fence[1] : s;
  const start = raw.indexOf('{');
  const end = raw.lastIndexOf('}');
  if (start >= 0 && end > start) return raw.slice(start, end + 1);
  return raw;
}

/** 轮询 agent job 直到结束（done → answer；其余 → throw error） */
function waitAgentDone(timeoutMs = 30 * 60 * 1000) {
  return new Promise((resolve, reject) => {
    const t0 = Date.now();
    const timer = setInterval(() => {
      const info = agent.agentJobInfo();
      if (info && info.state !== 'running' && info.state !== 'idle') {
        clearInterval(timer);
        if (info.state === 'done') resolve(String(info.answer ?? ''));
        else reject(new Error(info.error || `执行未完成（state=${info.state}）`));
        return;
      }
      if (Date.now() - t0 > timeoutMs) {
        clearInterval(timer);
        reject(new Error('小满执行超时（30 分钟）'));
      }
    }, 1500);
  });
}

/** 当前监工任务信息（含每轮过程），无则 null */
export function supervisorJobInfo() {
  if (!job) return null;
  return {
    id: job.id,
    goal: job.goal,
    acceptMode: job.acceptMode,
    maxRounds: job.maxRounds,
    supervisor: job.supervisor,
    executor: job.executor,
    state: job.state,
    round: job.round,
    plan: job.plan,
    verdict: job.verdict,
    error: job.error,
    rounds: job.rounds.map((r) => ({ ...r })),
  };
}

/** 启动监工闭环（异步，立即返回 jobId；进度经 supervisorJobInfo 查） */
export function startSupervise(goal, opts = {}) {
  if (job && job.state === 'running') {
    return { ok: false, code: 409, error: '已有监工任务在跑，等它结束' };
  }
  const goalStr = String(goal || '').trim();
  if (!goalStr) return { ok: false, code: 400, error: '目标不能为空' };

  const sv = providers.supervisorProvider();
  const exec = providers.activeProvider();
  const acceptMode = ACCEPT_MODES.some((m) => m.id === opts.acceptMode) ? opts.acceptMode : 'relaxed';
  const maxRounds = Math.min(Math.max(Number(opts.maxRounds) || 3, 1), 8);

  job = {
    id: `SV-${Date.now()}`,
    goal: goalStr,
    acceptMode,
    maxRounds,
    supervisor: sv?.name ?? '?',
    executor: exec?.name ?? '?',
    state: 'running',
    round: 0,
    plan: null,
    verdict: null,
    error: null,
    rounds: [],
    startedAt: Date.now(),
  };

  runSupervise(job).catch((e) => {
    job.state = 'error';
    job.error = String((e && e.message) || e);
  });

  return { ok: true, jobId: job.id, supervisor: job.supervisor, executor: job.executor, acceptMode };
}

/** 监工编排主流程：派单 → 执行 → 验收 → 迭代 */
async function runSupervise(j) {
  const sv = providers.supervisorProvider();
  if (!sv) {
    j.error = '没有可用的监工模型（providers 为空）';
    j.state = 'error';
    return;
  }

  // ── 阶段 1：监工派单 ──
  const strict = j.acceptMode === 'strict';
  let plan;
  try {
    const r = await chatOnce(
      sv,
      [
        {
          role: 'system',
          content: `你是「监工」——负责给执行模型（小满）派活并验收的主控模型。输出必须是严格 JSON，不要任何解释文字。${strict ? '（严格验收模式）验收标准必须可证伪（能明确判 PASS/FAIL），并在任务里要求执行者附上可程序化核查的证据。' : ''}`,
        },
        {
          role: 'user',
          content: `把下面这个目标拆成一张任务单，输出 JSON：
{"task":"给执行者的一句话任务描述（具体、可执行）","criteria":["验收标准1","验收标准2",...]}

目标：${j.goal}`,
        },
      ],
      { temperature: 0.2, maxTokens: 2000 }
    );
    const content = r?.choices?.[0]?.message?.content ?? '';
    plan = JSON.parse(extractJson(content));
    if (!plan || typeof plan.task !== 'string' || !plan.task.trim()) throw new Error('监工未给出 task');
    plan.task = plan.task.trim();
    plan.criteria = Array.isArray(plan.criteria) ? plan.criteria.map((c) => String(c)).filter(Boolean) : [];
    j.plan = plan;
  } catch (e) {
    j.error = '监工派单失败：' + String((e && e.message) || e).slice(0, 300);
    j.state = 'error';
    return;
  }

  // ── 阶段 2~4：执行 + 验收 + 迭代 ──
  let feedback = '';
  for (let round = 1; round <= j.maxRounds; round++) {
    j.round = round;
    const rec = { round, task: plan.task, state: 'running', result: null, verdict: null, reason: '' };
    j.rounds.push(rec);

    // 阶段 2：小满执行（复用现有 agent loop，active provider = 本地小满）
    const strictHint = strict ? `\n\n${STRICT_CRITERIA_HINT}` : '';
    const taskLine = `${plan.task}${strictHint}${feedback ? `\n\n（上一轮监工反馈，务必修正）：${feedback}` : ''}`;
    const startRes = agent.startAgentTask(taskLine, null);
    if (startRes && startRes.ok === false) {
      rec.state = 'exec_error';
      rec.result = startRes.error || '小满拒绝执行';
      break;
    }
    let answer;
    try {
      answer = await waitAgentDone();
    } catch (e) {
      rec.state = 'exec_error';
      rec.result = String((e && e.message) || e);
      break;
    }
    rec.result = answer;

    // 阶段 3：监工验收
    let verdict;
    try {
      const r2 = await chatOnce(
        sv,
        [
          { role: 'system', content: `你是监工，负责验收执行者的产出。输出严格 JSON。${strict ? '（严格验收）逐条核对执行者是否附上可程序化核查的证据（编译 exitCode / 回读结果 / 实测数字与来源），没有证据的断言一律判 FAIL。' : ''}` },
          {
            role: 'user',
            content: `验收标准：
${(plan.criteria.length ? plan.criteria : ['（未列标准，按目标本身判断）']).map((c, i) => `${i + 1}. ${c}`).join('\n')}

执行者（小满）的产出：
${answer}

请逐条对照，输出 JSON：
{"verdict":"PASS" 或 "FAIL","reason":"结论理由（PASS 简短；FAIL 给出具体可执行的修正建议）"}`,
          },
        ],
        { temperature: 0.2, maxTokens: 2000 }
      );
      const c2 = r2?.choices?.[0]?.message?.content ?? '';
      verdict = JSON.parse(extractJson(c2));
    } catch (e) {
      verdict = { verdict: 'FAIL', reason: '验收解析失败：' + String((e && e.message) || e).slice(0, 200) };
    }
    rec.verdict = verdict.verdict;
    rec.reason = verdict.reason || '';

    if (String(verdict.verdict).toUpperCase() === 'PASS') {
      rec.state = 'done';
      j.verdict = { verdict: 'PASS', reason: verdict.reason || '', rounds: round };
      j.state = 'done';
      return;
    }
    // FAIL → 反馈迭代
    rec.state = 'need_redo';
    feedback = verdict.reason || '未说明原因';
    j.verdict = { verdict: 'FAIL', reason: feedback, rounds: round };
  }
  // 迭代到上限，按最后一轮验收为准
  j.state = 'done';
}
