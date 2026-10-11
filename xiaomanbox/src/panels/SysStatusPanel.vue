<script setup lang="ts">
import { onMounted, ref } from 'vue';
import { getHealth, getStatus } from '../api/client';
import { API_BASE, CONTRACT_VERSION, ContractError } from '../api/contract';
import type { HealthResponse, StatusResponse } from '../api/contract';
import { t } from '../i18n';

// 系统状态面板 —— 复用 M−1 地基的契约 v1，是壳里唯一的「真数据」面板。
const loading = ref(true);
const error = ref<string | null>(null);
const health = ref<HealthResponse | null>(null);
const status = ref<StatusResponse | null>(null);

async function load() {
  loading.value = true;
  error.value = null;
  try {
    const [h, s] = await Promise.all([getHealth(), getStatus()]);
    health.value = h;
    status.value = s;
  } catch (e) {
    error.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  } finally {
    loading.value = false;
  }
}

function fmtBytes(n: number): string {
  return `${(n / 1048576).toFixed(1)} MiB`;
}

onMounted(load);
</script>

<template>
  <div v-if="loading" class="mono dim">{{ t('通过契约') }} {{ CONTRACT_VERSION }} {{ t('拉取后端…') }}</div>
  <div v-else-if="error" class="mono err">
    {{ error }}
    <button class="btn-inline" @click="load">{{ t('重试') }}</button>
  </div>
  <dl v-else class="kv">
    <dt>{{ t('后端 PID') }}</dt><dd>{{ status?.backendPid }}</dd>
    <dt>{{ t('主机') }}</dt><dd>{{ status?.hostname }}</dd>
    <dt>{{ t('平台') }}</dt><dd>{{ status?.platform }} / {{ status?.arch }}</dd>
    <dt>Node</dt><dd>{{ status?.nodeVersion }}</dd>
    <dt>{{ t('后端 RSS') }}</dt><dd>{{ status ? fmtBytes(status.memory.rss) : '—' }}</dd>
    <dt>{{ t('契约') }}</dt><dd>{{ health?.contract }} · {{ API_BASE }}</dd>
  </dl>
</template>
