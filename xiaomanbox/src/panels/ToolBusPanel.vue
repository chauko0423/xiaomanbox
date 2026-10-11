<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from 'vue';
import { getUalStatus } from '../api/ual';
import { getNodeStats } from '../api/nodes';
import { getCompilerJob, startCompileVerify } from '../api/compiler';
import type { CompilerJobResponse } from '../api/contract';
import { ContractError } from '../api/contract';
import { t } from '../i18n';

/**
 * 工具总线面板 —— 两条 seam：
 *  - seam.ual     内置 UnrealAgentLink 服务端：引擎连接 + 内置工具数（UEBox 拆解融合，摆脱 MCP）
 *  - seam.compiler UE 真编译验证（触发 + 轮询，M2 手）
 * 引擎 / 后端任一不可用时降级提示，不白屏（§9.3）。
 */

const ual = ref<{
  ok: boolean;
  connected?: boolean;
  toolCount?: number;
  connectionCount?: number;
  error?: string;
} | null>(null);

const job = ref<CompilerJobResponse | null>(null);
const jobErr = ref<string | null>(null);
const starting = ref(false);
let pollTimer: number | undefined;

const nodelib = ref<{ ok: boolean; nodeCount?: number; error?: string } | null>(null);

async function loadNodeLib() {
  try {
    const s = await getNodeStats();
    nodelib.value = { ok: true, nodeCount: s.nodeCount };
  } catch (e) {
    nodelib.value = { ok: false, error: e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e) };
  }
}

async function loadUal() {
  try {
    const s = await getUalStatus();
    ual.value = {
      ok: true,
      connected: s.connectionCount > 0,
      toolCount: s.toolCount,
      connectionCount: s.connectionCount,
    };
  } catch (e) {
    ual.value = { ok: false, error: e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e) };
  }
}

async function refreshJob() {
  try {
    job.value = await getCompilerJob();
    jobErr.value = null;
  } catch (e) {
    jobErr.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  }
}

function schedulePoll() {
  window.clearTimeout(pollTimer);
  if (job.value?.state === 'running') {
    pollTimer = window.setTimeout(async () => {
      await refreshJob();
      schedulePoll();
    }, 2000);
  }
}

async function trigger() {
  starting.value = true;
  try {
    await startCompileVerify();
    await refreshJob();
    schedulePoll();
  } catch (e) {
    jobErr.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  } finally {
    starting.value = false;
  }
}

function jobLabel(): string {
  const j = job.value;
  if (!j || j.state === 'idle') return t('未跑过');
  if (j.state === 'running') return t('编译中…');
  if (j.exitCode === 0) return `${t('✅ 通过')}（${j.durationSec ?? '?'}s）`;
  return `${t('❌ 失败')} exit=${j.exitCode ?? '?'}${j.durationSec != null ? `（${j.durationSec}s）` : ''}`;
}

onMounted(async () => {
  loadUal();
  loadNodeLib();
  await refreshJob();
  schedulePoll();
});

onBeforeUnmount(() => window.clearTimeout(pollTimer));
</script>

<template>
  <ul class="kv-list">
    <li class="kv-row">
      <span class="k mono">seam.ual</span>
      <span v-if="ual?.ok && ual.connected" class="v">
        {{ t('内置引擎工具已就绪（') }}{{ ual.connectionCount }}{{ t(' 个引擎连接）') }}
      </span>
      <span v-else-if="ual?.ok" class="v dim">{{ t('引擎未连接（需打开 UE 工程并装 UnrealAgentLink 插件）') }}</span>
      <span v-else class="v dim">{{ t('状态获取失败：') }}{{ ual?.error }}</span>
      <span v-if="ual?.ok" class="tag">{{ ual.toolCount }}{{ t(' 个内置工具 · 直连插件（摆脱 MCP）') }}</span>
      <span v-else class="tag dim">{{ t('未接') }}</span>
    </li>

    <li class="kv-row">
      <span class="k mono">seam.nodelib</span>
      <span v-if="nodelib?.ok" class="v">{{ t('内置节点参考库（NodeUE 拆解 · 离线）') }}</span>
      <span v-else class="v dim">{{ t('节点库状态获取失败：') }}{{ nodelib?.error }}</span>
      <span v-if="nodelib?.ok" class="tag">{{ nodelib.nodeCount }}{{ t(' 个节点签名') }}</span>
      <span v-else class="tag dim">{{ t('未接') }}</span>
    </li>

    <li class="kv-row">
      <span class="k mono">seam.compiler</span>
      <span class="v">{{ jobLabel() }}</span>
      <button class="btn-inline" :disabled="starting || job?.state === 'running'" @click="trigger">
        {{ job?.state === 'running' ? t('编译中…') : t('触发编译验证') }}
      </button>
    </li>
  </ul>

  <div v-if="job?.errors?.length" class="compile-errs">
    <div v-for="(e, i) in job.errors.slice(0, 4)" :key="i" class="mono err">{{ e }}</div>
  </div>

  <p class="foot-note">
    {{ t('目标') }} {{ job?.cfg?.target ?? 'DIAOYUEditor' }} · {{ job?.cfg?.platform ?? 'Win64' }} / {{ job?.cfg?.config ?? 'Development' }} · {{ t('错误全量看后端日志') }}
    <template v-if="jobErr"> · {{ t('任务状态获取失败：') }}{{ jobErr }}</template>
  </p>
</template>

<style scoped>
.compile-errs {
  margin-top: 8px;
  display: flex;
  flex-direction: column;
  gap: 2px;
  font-size: 0.846rem;
}
</style>
