<script setup lang="ts">
import { onMounted, ref } from 'vue';
import { currentScene } from '../scenes/store';
import { getSceneSession, getSessionMessages, updateSession } from '../api/session';
import type { Session, SessionMessage } from '../api/session';
import { t } from '../i18n';

/** 会话面板（M1 魂）：显示当前场景绑定的会话三字段（sessionId/sessionTitle/pinTarget），可改标题 */

const session = ref<Session | null>(null);
const messages = ref<SessionMessage[]>([]);
const titleInput = ref('');
const err = ref<string | null>(null);

async function refresh() {
  try {
    const { session: s } = await getSceneSession(currentScene.value.id);
    session.value = s;
    titleInput.value = s.sessionTitle;
    const m = await getSessionMessages(s.sessionId);
    messages.value = m.messages;
    err.value = null;
  } catch (e) {
    err.value = String(e instanceof Error ? e.message : e);
  }
}

async function saveTitle() {
  if (!session.value || !titleInput.value.trim()) return;
  try {
    const { session: s } = await updateSession(session.value.sessionId, { sessionTitle: titleInput.value.trim() });
    session.value = s;
    err.value = null;
  } catch (e) {
    err.value = String(e instanceof Error ? e.message : e);
  }
}

onMounted(refresh);
</script>

<template>
  <div class="session">
    <div v-if="session" class="grid">
      <div class="row"><span class="label">{{ t('会话ID') }}</span><span class="mono">{{ session.sessionId.slice(0, 8) }}…</span></div>
      <div class="row"><span class="label">{{ t('钉住场景') }}</span><span class="mono">{{ session.pinTarget }}</span></div>
      <div class="row">
        <span class="label">{{ t('标题') }}</span>
        <input v-model="titleInput" class="title" @keyup.enter="saveTitle" />
        <button class="btn-inline" @click="saveTitle">{{ t('改') }}</button>
      </div>
      <div class="row">
        <span class="label">{{ t('消息') }}</span><span>{{ messages.length }} {{ t('条') }}</span>
        <button class="btn-inline" @click="refresh">{{ t('刷新') }}</button>
      </div>
    </div>
    <div v-else class="dim">{{ t('后端离线，会话不可用') }}</div>

    <div v-if="err" class="err">{{ err }}</div>

    <ol v-if="messages.length" class="msgs">
      <li v-for="(m, i) in messages" :key="i" :class="m.role">
        <span class="role">{{ m.role === 'user' ? t('你') : '小满' }}</span>
        <span class="content">{{ m.content }}</span>
      </li>
    </ol>
  </div>
</template>

<style scoped>
.grid { display: flex; flex-direction: column; gap: 6px; }
.row { display: flex; align-items: center; gap: 8px; font-size: 0.923rem; }
.label { color: var(--dim); flex: none; width: 56px; }
.mono { font-family: var(--font-tech); }
.title { flex: 1; background: var(--panel-bg-2); color: var(--ink); border: 1px solid var(--border); padding: 4px 6px; font-size: 0.923rem; }
.err { color: var(--danger, #e5534b); font-size: 0.846rem; margin-top: 6px; }
.dim { color: var(--dim); font-size: 0.923rem; }
.msgs { list-style: none; margin: 10px 0 0; padding: 0; display: flex; flex-direction: column; gap: 6px; max-height: 200px; overflow-y: auto; }
.msgs li { display: flex; gap: 8px; font-size: 0.923rem; padding: 6px; border: 1px solid var(--border); }
.msgs .role { color: var(--ink); flex: none; }
.msgs .content { word-break: break-all; }
</style>
