<script setup lang="ts">
import { ref } from 'vue';
import { searchAll } from '../api/search';
import type { SearchResponse } from '../api/search';
import { t } from '../i18n';

/** W-78 全局搜索：一处搜全仓（会话 / 工具 / skill） */

const q = ref('');
const result = ref<SearchResponse | null>(null);
const err = ref<string | null>(null);
const searching = ref(false);

async function doSearch() {
  if (!q.value.trim() || searching.value) return;
  searching.value = true;
  err.value = null;
  try {
    result.value = await searchAll(q.value.trim());
  } catch (e) {
    err.value = String(e instanceof Error ? e.message : e);
  } finally {
    searching.value = false;
  }
}
</script>

<template>
  <div class="search">
    <div class="input-row">
      <input v-model="q" :placeholder="t('全局搜索：会话 / 工具 / skill')" @keyup.enter="doSearch" />
      <button class="btn-inline" :disabled="searching" @click="doSearch">{{ t('搜索') }}</button>
    </div>

    <div v-if="err" class="err">{{ err }}</div>

    <div v-if="result" class="result">
      <div class="summary">
        {{ t('命中：会话') }} {{ result.counts.sessions }} · {{ t('工具') }} {{ result.counts.tools }} · skill {{ result.counts.skills }}
      </div>
      <div v-if="result.ueboxSkipped" class="uebox-note">
        {{ t('UEBox 工具未纳入本次搜索（引擎/UEBox 离线，搜索本地即时返回）。启动后重搜即可覆盖 UEBox 工具。') }}
      </div>

      <section v-if="result.sessions.length">
        <h4>{{ t('会话') }}</h4>
        <div v-for="s in result.sessions" :key="s.sessionId" class="item">
          <span class="title">{{ s.title }} <span class="dim">{{ s.sessionId?.slice(0, 8) }}</span></span>
          <div v-for="(h, i) in s.hits" :key="i" class="hit">— {{ h }}</div>
        </div>
      </section>

      <section v-if="result.tools.length">
        <h4>{{ t('工具') }}</h4>
        <div v-for="tool in result.tools" :key="(tool.source ?? '') + (tool.name ?? '')" class="item">
          <span class="title">{{ tool.name ?? '' }} <span class="dim">{{ tool.source ?? '' }} {{ tool.risk ? '· ' + tool.risk : '' }}</span></span>
          <div v-if="tool.brief" class="hit">{{ tool.brief }}</div>
        </div>
      </section>

      <section v-if="result.skills.length">
        <h4>Skill</h4>
        <div v-for="s in result.skills" :key="s.name" class="item">
          <span class="title">{{ s.name }}</span>
          <div v-if="s.description" class="hit">{{ s.description }}</div>
        </div>
      </section>
    </div>
  </div>
</template>

<style scoped>
.input-row { display: flex; gap: 8px; }
input { flex: 1; background: var(--panel-bg-2); color: var(--ink); border: 1px solid var(--border); padding: 6px 8px; font-size: 0.923rem; font-family: var(--font-tech); }
.err { color: var(--danger, #e5534b); font-size: 0.846rem; margin-top: 6px; }
.result { margin-top: 10px; font-size: 0.923rem; }
.summary { color: var(--ink); margin-bottom: 6px; }
.uebox-note { color: var(--caution, #d29922); font-size: 0.846rem; margin: 6px 0; }
section h4 { margin: 10px 0 4px; color: var(--dim); font-size: 0.846rem; text-transform: uppercase; }
.item { border: 1px solid var(--border); padding: 6px 8px; margin-bottom: 6px; }
.title { color: var(--ink); }
.dim { color: var(--dim); font-size: 0.769rem; }
.hit { color: var(--dim); margin-top: 3px; word-break: break-all; }
</style>
