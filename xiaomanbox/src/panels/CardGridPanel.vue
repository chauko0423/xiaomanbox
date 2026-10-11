<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from 'vue';
import { getEvents, getSystemMemory } from '../api/events';
import type { SystemMemoryResponse } from '../api/events';
import { t } from '../i18n';

/**
 * 控制室「眼」卡片网格（M3）：状态来自事件订阅镜像，不调模型。
 * 断开引擎后，事件日志（本地后端）仍在 → 卡片仍显示最后状态（W-75 负控）。
 */

interface Card {
  jobId: string;
  task: string;
  state: 'working' | 'done' | 'failed';
  answer?: string;
  error?: string;
  startedAt?: string;
}

const cards = ref<Card[]>([]);
const mem = ref<SystemMemoryResponse | null>(null);
const err = ref<string | null>(null);
let cursor = 0;
let pollTimer: number | undefined;
let memTimer: number | undefined;

const cardMap = new Map<string, Card>();

async function pollEvents() {
  try {
    const r = await getEvents(cursor);
    cursor = r.latest;
    for (const e of r.events) {
      if (e.type === 'agent.started' && e.jobId) {
        cardMap.set(e.jobId, { jobId: e.jobId, task: e.task || '', state: 'working', startedAt: e.ts });
      } else if (e.type === 'agent.done' && e.jobId) {
        const c = cardMap.get(e.jobId);
        if (c) {
          c.state = 'done';
          c.answer = e.answer;
        }
      } else if (e.type === 'agent.failed' && e.jobId) {
        const c = cardMap.get(e.jobId);
        if (c) {
          c.state = 'failed';
          c.error = e.error;
        }
      }
    }
    cards.value = Array.from(cardMap.values()).reverse(); // 最新的在前
    err.value = null;
  } catch (e) {
    // 后端离线：保持已有卡片（证明不依赖后端实时查询）
    err.value = String(e instanceof Error ? e.message : e);
  }
}

async function pollMemory() {
  try {
    mem.value = await getSystemMemory();
  } catch {
    /* 后端离线：保持上次值 */
  }
}

onMounted(() => {
  pollEvents();
  pollMemory();
  pollTimer = window.setInterval(pollEvents, 2000);
  memTimer = window.setInterval(pollMemory, 5000);
});
onBeforeUnmount(() => {
  window.clearInterval(pollTimer);
  window.clearInterval(memTimer);
});
</script>

<template>
  <div class="control-room">
    <!-- 内存余量（W-76：低于 8 GiB 红色告警，BSOD 同源风险） -->
    <div class="mem" :class="{ low: mem?.lowBelow8GiB }">
      <span class="label">{{ t('系统可用内存') }}</span>
      <span class="val">{{ mem ? `${mem.freeGiB} / ${mem.totalGiB} GiB` : '—' }}</span>
      <span v-if="mem?.lowBelow8GiB" class="warn">{{ t('⚠️ 低于 8 GiB（BSOD 风险）') }}</span>
      <span v-else class="ok">{{ t('正常') }}</span>
    </div>

    <!-- 卡片网格（W-73：工作中 / 待你决定 / 已完成） -->
    <div v-if="err" class="err">{{ err }}</div>
    <div v-if="cards.length" class="grid">
      <div v-for="c in cards" :key="c.jobId" class="card" :class="c.state">
        <div class="card-head">
          <span class="dot" :class="c.state" />
          <span class="job">{{ c.jobId }}</span>
          <span class="tag">{{ c.state === 'working' ? t('工作中') : c.state === 'done' ? t('已完成') : t('失败') }}</span>
        </div>
        <div class="task">{{ c.task }}</div>
        <div v-if="c.state === 'done' && c.answer" class="answer">✓ {{ c.answer }}</div>
        <div v-else-if="c.state === 'failed' && c.error" class="answer err">{{ c.error }}</div>
      </div>
    </div>
    <p v-else class="dim">{{ t('暂无会话卡片（事件订阅镜像，断开引擎后仍在）') }}</p>
  </div>
</template>

<style scoped>
.mem { display: flex; align-items: center; gap: 10px; font-size: 0.923rem; padding: 8px; border: 1px solid var(--border); margin-bottom: 10px; }
.mem.low { border-color: var(--danger, #e5534b); background: rgba(229, 83, 75, 0.08); }
.mem .label { color: var(--dim); }
.mem .val { font-family: var(--font-tech); }
.mem .warn { color: var(--danger, #e5534b); font-weight: bold; }
.mem .ok { color: var(--ink); }
.err { color: var(--danger, #e5534b); font-size: 0.846rem; }
.dim { color: var(--dim); font-size: 0.923rem; }
.grid { display: flex; flex-direction: column; gap: 8px; }
.card { border: 1px solid var(--border); padding: 8px 10px; font-size: 0.923rem; }
.card.working { border-left: 3px solid var(--caution); }
.card.done { border-left: 3px solid var(--ink); }
.card.failed { border-left: 3px solid var(--danger, #e5534b); }
.card-head { display: flex; align-items: center; gap: 8px; }
.dot { width: 8px; height: 8px; border-radius: 50%; flex: none; }
.dot.working { background: var(--caution); animation: pulse 1s infinite; }
.dot.done { background: var(--ink); }
.dot.failed { background: var(--danger, #e5534b); }
.job { font-family: var(--font-tech); color: var(--dim); font-size: 0.846rem; }
.tag { color: var(--ink); }
.task { margin-top: 4px; color: var(--ink); }
.answer { margin-top: 4px; color: var(--dim); }
@keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.3; } }
</style>
