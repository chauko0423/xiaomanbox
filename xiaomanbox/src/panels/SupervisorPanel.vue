<template>
  <div class="sv">
    <p class="lead">
      {{ t('外部监工——一个独立模型负责') }}<span class="hl">{{ t('派单') }}</span>{{ t('和') }}<span class="hl">{{ t('验收') }}</span>{{ t('，小满（本地）负责') }}<span class="hl">{{ t('执行') }}</span>{{ t('。闭环：派单 → 执行 → 验收 → 迭代。') }}
    </p>

    <!-- 监工模型选择 -->
    <div class="row">
      <label class="lab">{{ t('监工模型') }}</label>
      <select v-model="supervisorId" class="inp" @change="chooseSupervisor()">
        <option v-for="p in providers" :key="p.id" :value="p.id">
          {{ p.name }}<template v-if="p.id === activeId">{{ t('（执行者）') }}</template>
        </option>
      </select>
      <span class="dim">{{ t('执行者（小满）=') }} {{ activeName }}</span>
    </div>

    <!-- 目标输入 -->
    <div class="row">
      <label class="lab">{{ t('总目标') }}</label>
      <textarea v-model="goal" class="goal" rows="3" :placeholder="t('例：给 DIAOYU 写一个「鱼咬钩后卡牌消除」的蓝图功能，编译通过并回读核对')" />
    </div>

    <!-- 参数 -->
    <div class="row">
      <label class="lab">{{ t('验收策略') }}</label>
      <select v-model="acceptMode" class="inp">
        <option v-for="m in acceptModes" :key="m.id" :value="m.id">{{ m.name }}（{{ m.desc }}）</option>
      </select>
      <label class="lab">{{ t('最多迭代') }}</label>
      <select v-model.number="maxRounds" class="inp">
        <option v-for="n in [1, 2, 3, 4, 5]" :key="n" :value="n">{{ n }} {{ t('轮') }}</option>
      </select>
      <span class="grow"></span>
      <button class="btn primary" :disabled="!goal.trim() || starting" @click="start()">
        {{ starting ? t('监工启动中…') : t('启动监工') }}
      </button>
    </div>

    <p v-if="err" class="err">{{ err }}</p>
    <p v-if="notice" class="notice">{{ notice }}</p>

    <!-- 过程 -->
    <div v-if="job" class="job">
      <div class="job-head">
        <span class="tag" :class="job.state">{{ job.state === 'running' ? t('运行中') : job.state === 'done' ? t('完成') : t('出错') }}</span>
        <span class="dim">{{ t('监工') }} {{ job.supervisor }} {{ t('→ 执行') }} {{ job.executor }} · {{ job.acceptMode }}</span>
      </div>

      <!-- 派单 -->
      <div v-if="job.plan" class="card">
        <div class="card-t">{{ t('📋 监工派单') }}</div>
        <p class="task">{{ job.plan.task }}</p>
        <div v-if="job.plan.criteria?.length" class="crit">
          <div class="card-t s">{{ t('验收标准') }}</div>
          <ul><li v-for="(c, i) in job.plan.criteria" :key="i">{{ c }}</li></ul>
        </div>
      </div>

      <!-- 每轮 -->
      <div v-for="r in job.rounds" :key="r.round" class="card round">
        <div class="card-t">🔁 {{ t('第') }} {{ r.round }} {{ t('轮') }}</div>
        <details>
          <summary>{{ t('执行产出（点开看全文）') }}</summary>
          <pre class="res">{{ r.result || t('（无产出）') }}</pre>
        </details>
        <div class="verdict" :class="r.verdict === 'PASS' ? 'pass' : 'fail'">
          <b>{{ r.verdict === 'PASS' ? t('✅ 验收通过') : r.verdict === 'FAIL' ? t('❌ 验收不通过') : '…' }}</b>
          <span v-if="r.reason" class="reason">{{ r.reason }}</span>
        </div>
      </div>

      <!-- 最终结论 -->
      <div v-if="job.verdict" class="card final" :class="job.verdict.verdict === 'PASS' ? 'pass' : 'fail'">
        <b>{{ job.verdict.verdict === 'PASS' ? t('✅ 最终：验收通过') : t('❌ 最终：未通过（迭代到上限）') }}</b>
        <span class="dim">{{ t('共') }} {{ job.verdict.rounds }} {{ t('轮') }}</span>
      </div>

      <p v-if="job.error" class="err">{{ t('出错：') }}{{ job.error }}</p>
    </div>

    <div v-else class="empty">
      {{ t('还没有监工任务。选好监工模型、填总目标、点「启动监工」。') }}<br />
      {{ t('提示：监工模型可换成云端大模型（DeepSeek/GPT/千问…），在「模型配置」面板添加后这里就能选。') }}
    </div>

    <!-- 外部监工接入（GPT/WorkBuddy 等外部 AI 当监工） -->
    <div class="card ext">
      <div class="card-t">{{ t('🔌 外部监工接入（GPT / WorkBuddy 等当监工）') }}</div>
      <p class="ext-hint">
        {{ t('让小满盒子留一个端口给外部 AI：外部 AI 派单 → 小满执行 → 外部 AI 验收。') }}
        {{ t('点「复制接入 Skill」，粘贴给 GPT / WorkBuddy，它们照着就能当监工。') }}
      </p>
      <div class="ext-row">
        <span class="dim">{{ t('端口') }} <code>http://127.0.0.1:8871/api/v1</code> {{ t('· 派单') }} <code>POST /work/assign</code> {{ t('· 拉结果') }} <code>GET /work/status/{id}</code></span>
        <span class="grow"></span>
        <button class="btn" @click="copySkill()">{{ t('📋 复制接入 Skill') }}</button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from 'vue';
import { listProviders, setSupervisorProvider } from '../api/providers';
import { getSupervisorSkill, getSupervisorStatus, runSupervisor } from '../api/supervisor';
import type { AcceptMode, SupervisorJob } from '../api/supervisor';
import { ContractError } from '../api/contract';
import { copyText } from '../utils/clipboard';
import { t } from '../i18n';

/** 外部监工面板：选监工模型 → 填目标 → 启动 → 看派单/执行/验收全过程。 */

const providers = ref<{ id: string; name: string }[]>([]);
const activeId = ref<string | null>(null);
const activeName = ref('…');
const supervisorId = ref<string | null>(null);
const goal = ref('');
const acceptMode = ref('relaxed');
const maxRounds = ref(3);
const acceptModes = ref<AcceptMode[]>([]);
const job = ref<SupervisorJob | null>(null);
const err = ref<string | null>(null);
const notice = ref<string | null>(null);
const starting = ref(false);
let pollTimer: number | undefined;

function fmtErr(e: unknown): string {
  if (e instanceof ContractError) return `${e.kind} · ${e.message}`;
  return String((e as Error)?.message || e);
}

async function refreshProviders(): Promise<void> {
  try {
    const r = await listProviders();
    providers.value = r.providers.map((p) => ({ id: p.id, name: p.name }));
    activeId.value = r.active;
    activeName.value = r.providers.find((p) => p.id === r.active)?.name ?? '?';
    if (!supervisorId.value) supervisorId.value = r.supervisor ?? r.active;
  } catch (e) {
    err.value = fmtErr(e);
  }
}

async function chooseSupervisor(): Promise<void> {
  if (!supervisorId.value) return;
  try {
    await setSupervisorProvider(supervisorId.value);
    notice.value = '监工模型已切换';
    setTimeout(() => (notice.value = null), 3000);
  } catch (e) {
    err.value = fmtErr(e);
  }
}

/** 复制「外部监工接入 Skill」全文（用户粘贴给 GPT/WorkBuddy 当监工） */
async function copySkill(): Promise<void> {
  try {
    const r = await getSupervisorSkill();
    await copyText(r.skill);
    notice.value = '接入 Skill 已复制，粘贴给 GPT/WorkBuddy 即可';
    setTimeout(() => (notice.value = null), 4000);
  } catch (e) {
    err.value = fmtErr(e);
  }
}

async function start(): Promise<void> {
  if (!goal.value.trim() || starting.value) return;
  starting.value = true;
  err.value = null;
  try {
    const r = await runSupervisor(goal.value, { acceptMode: acceptMode.value, maxRounds: maxRounds.value });
    if (!r.ok) throw new Error(r.error || '启动失败');
    notice.value = `监工已启动（${r.supervisor} 监督 → ${r.executor} 执行）`;
    schedulePoll();
  } catch (e) {
    err.value = fmtErr(e);
  } finally {
    starting.value = false;
  }
}

async function poll(): Promise<void> {
  try {
    const r = await getSupervisorStatus();
    if (r.acceptModes?.length) acceptModes.value = r.acceptModes;
    job.value = r.job;
    if (job.value?.state === 'running') schedulePoll();
    else if (pollTimer) window.clearInterval(pollTimer);
  } catch (e) {
    err.value = fmtErr(e);
  }
}

function schedulePoll(): void {
  if (pollTimer) window.clearInterval(pollTimer);
  pollTimer = window.setInterval(() => void poll(), 2000);
}

onMounted(async () => {
  await refreshProviders();
  await poll();
});

onBeforeUnmount(() => {
  if (pollTimer) window.clearInterval(pollTimer);
});
</script>

<style scoped>
.sv { display: flex; flex-direction: column; gap: 12px; }
.lead { margin: 0; opacity: 0.85; }
.hl { color: var(--caution, #fff500); }
.row { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.lab { font-size: 0.846rem; opacity: 0.8; white-space: nowrap; }
.inp { background: rgba(255, 255, 255, 0.07); border: 1px solid rgba(255, 255, 255, 0.14); border-radius: 6px; color: inherit; padding: 5px 8px; font-size: 0.846rem; }
.goal { flex: 1; min-width: 320px; background: rgba(255, 255, 255, 0.07); border: 1px solid rgba(255, 255, 255, 0.14); border-radius: 6px; color: inherit; padding: 8px; font-size: 0.923rem; resize: vertical; }
.grow { flex: 1; }
.dim { font-size: 0.769rem; opacity: 0.6; }
.btn { background: rgba(255, 255, 255, 0.09); border: 1px solid rgba(255, 255, 255, 0.16); border-radius: 6px; color: inherit; padding: 6px 14px; cursor: pointer; font-size: 0.846rem; }
.btn:disabled { opacity: 0.45; cursor: default; }
.btn.primary { border-color: var(--caution, #fff500); color: var(--caution, #fff500); }
.err { color: #e5534b; font-size: 0.846rem; margin: 0; }
.notice { color: var(--caution, #fff500); font-size: 0.846rem; margin: 0; }

.job { display: flex; flex-direction: column; gap: 10px; }
.job-head { display: flex; align-items: center; gap: 10px; }
.tag { font-size: 0.769rem; padding: 2px 8px; border-radius: 4px; }
.tag.running { background: rgba(255, 245, 0, 0.15); color: #fff500; }
.tag.done { background: rgba(76, 195, 138, 0.15); color: #4cc38a; }
.tag.error { background: rgba(229, 83, 75, 0.15); color: #e5534b; }

.card { background: rgba(255, 255, 255, 0.04); border: 1px solid rgba(255, 255, 255, 0.12); border-radius: 8px; padding: 10px 12px; }
.card-t { font-weight: 600; font-size: 0.923rem; margin-bottom: 4px; }
.card-t.s { font-size: 0.846rem; opacity: 0.8; margin-top: 6px; }
.task { margin: 0; font-size: 0.923rem; }
.crit ul { margin: 4px 0 0; padding-left: 18px; font-size: 0.846rem; opacity: 0.9; }
.round summary { cursor: pointer; font-size: 0.846rem; opacity: 0.8; }
.res { background: rgba(0, 0, 0, 0.3); border-radius: 6px; padding: 8px; font-size: 0.769rem; max-height: 200px; overflow: auto; white-space: pre-wrap; word-break: break-all; }
.verdict { margin-top: 8px; font-size: 0.846rem; }
.verdict.pass { color: #4cc38a; }
.verdict.fail { color: #ff9a94; }
.verdict .reason { display: block; margin-top: 2px; opacity: 0.85; }
.final { display: flex; align-items: center; gap: 10px; }
.final.pass { border-color: #4cc38a; }
.final.fail { border-color: #e5534b; }
.empty { opacity: 0.6; font-size: 0.923rem; line-height: 1.7; }
.ext { border-color: rgba(255, 245, 0, 0.25); }
.ext-hint { margin: 0 0 8px; font-size: 0.846rem; opacity: 0.8; line-height: 1.6; }
.ext-row { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.ext code { background: rgba(255, 255, 255, 0.09); padding: 1px 5px; border-radius: 4px; font-size: 0.769rem; }
</style>
