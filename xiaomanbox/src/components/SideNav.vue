<script setup lang="ts">
import { ref, computed, onMounted, onBeforeUnmount } from 'vue';
import { getPanel } from '../panels/registry';
import type { NavGroupDef } from '../scenes/types';
import { listSessions } from '../api/session';
import type { SessionSummary } from '../api/session';
import { currentSessionId, switchToSession, openNewChat } from '../scenes/store';
import SettingsModal from './SettingsModal.vue';
import { t } from '../i18n';
defineProps<{ groups: NavGroupDef[]; activeId: string; collapsed?: boolean }>();
const emit = defineEmits<{ (e:'select',id:string):void; (e:'cta'):void; (e:'toggle-collapse'):void }>();
const sessions=ref<SessionSummary[]>([]), settings=ref(false);
const visible=computed(()=>sessions.value.filter(s=>!s.archived).slice(0,10));
let timer:ReturnType<typeof setInterval>;
async function refresh(){try{sessions.value=(await listSessions()).sessions;}catch{}}
function selectSession(id:string){switchToSession(id);emit('cta');}
function create(){openNewChat();emit('cta');void refresh();}
onMounted(()=>{void refresh();timer=setInterval(refresh,6000);});
onBeforeUnmount(()=>clearInterval(timer));
</script>
<template>
  <aside class="basic-nav" :class="{collapsed}">
    <div class="brand">XiaoManBox <small>Basic</small><button @click="emit('toggle-collapse')">{{collapsed?'›':'‹'}}</button></div>
    <button class="new" @click="create">{{collapsed?'+':t('新对话')}}</button>
    <template v-if="!collapsed">
      <div class="sessions"><button v-for="s in visible" :key="s.sessionId" :class="{active:s.sessionId===currentSessionId}" @click="selectSession(s.sessionId)">{{s.sessionTitle||s.sessionId}}</button></div>
      <section v-for="g in groups" :key="g.id"><h4>{{t(g.name)}}</h4><button v-for="id in g.items" :key="id" :class="{active:activeId===id}" @click="emit('select',id)">{{t(getPanel(id)?.title||id)}}</button></section>
      <button @click="settings=true">{{t('设置')}}</button><small class="local">本地基础版 · 无需账号</small>
    </template>
    <SettingsModal v-if="settings" @close="settings=false" />
  </aside>
</template>
<style scoped>
.basic-nav{width:230px;flex-shrink:0;background:var(--panel);color:var(--ink);padding:18px 12px;overflow:auto;border-right:1px solid var(--line)}.basic-nav.collapsed{width:74px}.brand{display:flex;align-items:center;gap:5px;margin-bottom:18px}.brand small{font-size:10px}.brand button{margin-left:auto;width:auto}button{display:block;width:100%;text-align:left;background:transparent;color:inherit;border:0;border-radius:6px;padding:9px;cursor:pointer}button:hover,.active{background:var(--hover)}.new{background:var(--hover);margin-bottom:12px}h4{font-size:11px;opacity:.55;margin:16px 9px 4px}.sessions{max-height:230px;overflow:auto}.sessions button{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.local{display:block;opacity:.5;margin-top:12px;font-size:10px}
</style>
