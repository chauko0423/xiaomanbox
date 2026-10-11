<script setup lang="ts">
import { ref,onMounted } from 'vue';
import { getTeamBoardStatus,startTeamBoard,stopTeamBoard } from '../api/teamboard';
const url=ref(''),error=ref(''),running=ref(false);
async function refresh(){try{const s=await getTeamBoardStatus();running.value=s.running;url.value=s.url;}catch(e){error.value=String(e);}}
async function start(){error.value='';try{const r=await startTeamBoard();if(!r.ok)throw new Error(r.error);await refresh();}catch(e){error.value=String(e);}}
async function stop(){await stopTeamBoard();running.value=false;url.value='';}
onMounted(refresh);
</script>
<template><div class="board"><header><strong>本地画板</strong><button @click="start">启动 / 打开</button><button v-if="running" @click="stop">停止</button><span>无需账号 · 内容保存在本机</span></header><p v-if="error">{{error}}</p><iframe v-if="running&&url" :src="url" title="本地基础画板" /><div v-else class="empty">点击启动，在画板中创建内容。需要协作时选择局域网分享。</div></div></template>
<style scoped>.board{height:100%;display:flex;flex-direction:column}header{display:flex;align-items:center;gap:12px;padding:12px;color:var(--ink)}header span{opacity:.5;font-size:12px}button{padding:7px;border:1px solid var(--line);background:var(--hover);color:inherit;border-radius:5px}iframe{flex:1;border:0;width:100%;min-height:500px}.empty{padding:40px;opacity:.6}</style>
