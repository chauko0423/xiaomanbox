<script setup lang="ts">
import { onMounted, ref } from 'vue';
import { getAgentJob } from '../api/agent';
import type { AgentJobResponse } from '../api/contract';
import { t } from '../i18n';

/**
 * W-77 熵/budget 可解释面板：回答「这一轮为什么思考这么久」。
 * 数据来自 agent job 的 steps（每轮模型思考时长 + reasoning 摘要 + 工具调用）。
 * 引擎 logprobs 熵尚未暴露（logprobs_not_supported），故本面板先解释「思考时长 + 决策链」。
 */

const job = ref<AgentJobResponse | null>(null);
const err = ref<string | null>(null);

function fmtMs(ms?: number): string {
  if (ms == null) return '—';
  return ms >= 1000 ? `${(ms / 1000).toFixed(1)}s` : `${ms}ms`;
}

function totalThinkMs(): number {
  return (job.value?.steps ?? []).filter((s) => s.type === 'model').reduce((a, s) => a + (s.ms ?? 0), 0);
}

async function load() {
  try {
    job.value = await getAgentJob();
    err.value = null;
  } catch (e) {
    err.value = String(e instanceof Error ? e.message : e);
  }
}

onMounted(load);
</script>

<template>
  <div class="explain">
    <div class="head">
      <span class="label">{{ t('最近任务') }}</span>
      <button class="btn-inline" @click="load">{{ t('刷新') }}</button>
    </div>
    <div v-if="err" class="err">{{ err }}</div>

    <div v-if="job && job.state !== 'idle'" class="summary">
      <div>{{ t('任务：') }}{{ job.task }}</div>
      <div>
        {{ t('模型思考轮次') }} <strong>{{ (job.steps ?? []).filter((s) => s.type === 'model').length }}</strong> {{ t('· 累计思考') }} <strong>{{ fmtMs(totalThinkMs()) }}</strong> {{ t('· 工具调用') }} <strong>{{ (job.steps ?? []).filter((s) => s.type === 'tool').length }}</strong>
      </div>
    </div>
    <p v-else class="dim">{{ t('还没有跑过任务') }}</p>

    <ol v-if="job?.steps?.length" class="timeline">
      <li v-for="(s, i) in job.steps" :key="i" :class="s.type">
        <template v-if="s.type === 'model'">
          <span class="mark">🤖</span>
          <span class="tc">{{ s.tool_calls?.length ? t('调工具 ') + s.tool_calls.map((c) => c.name).join('、') : t('回答') }}</span>
          <span class="ms">⏱ {{ fmtMs(s.ms) }}</span>
          <div v-if="s.thinking" class="think" :title="s.thinking">{{ t('思考：') }}{{ s.thinking }}</div>
        </template>
        <template v-else-if="s.type === 'tool'">
          <span class="mark">{{ s.ok ? '🔧' : '❌' }}</span>
          <span class="tn">{{ s.name }}</span>
          <span class="ms">⏱ {{ fmtMs(s.ms) }}</span>
        </template>
        <template v-else>
          <span class="mark">🔍</span>
          <span class="tn">{{ t('核查') }} {{ s.issues?.length ? '：' + s.issues.join('；') : '' }}</span>
        </template>
      </li>
    </ol>

    <p class="foot">{{ t('思考时长 = 模型每轮的推理耗时；引擎 logprobs 熵未暴露，暂以「思考时长 + 决策链」解释。') }}</p>
  </div>
</template>

<style scoped>
.head { display: flex; justify-content: space-between; align-items: center; margin-bottom: 6px; }
.label { color: var(--dim); font-size: 0.923rem; }
.err { color: var(--danger, #e5534b); font-size: 0.846rem; }
.dim { color: var(--dim); font-size: 0.923rem; }
.summary { font-size: 0.923rem; border: 1px solid var(--border); padding: 8px; margin-bottom: 8px; }
.summary strong { color: var(--ink); }
.timeline { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 4px; }
.timeline li { display: flex; flex-wrap: wrap; align-items: baseline; gap: 8px; font-size: 0.923rem; padding: 3px 4px; }
.timeline li.tool { background: rgba(77, 216, 192, 0.05); }
.mark { flex: none; }
.tc, .tn { color: var(--ink); }
.ms { color: var(--caution); font-size: 0.769rem; }
.think { color: var(--dim); font-size: 0.769rem; width: 100%; word-break: break-all; }
.foot { color: var(--dim); font-size: 0.769rem; margin-top: 8px; }
</style>
