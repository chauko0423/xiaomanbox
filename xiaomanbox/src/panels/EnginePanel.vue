<script setup lang="ts">
import {onMounted,ref} from 'vue';
import {getEngineControl,postEngineControl,type EngineControlStatus} from '../api/engine';
const status=ref<EngineControlStatus|null>(null),message=ref(''),busy=ref(false);
async function refresh(){try{status.value=await getEngineControl();}catch(e){message.value=String(e);}}
async function control(action:'model-start'|'model-stop'){busy.value=true;try{const r=await postEngineControl(action);message.value=r.note||r.error||(r.ok?'操作完成':'操作失败');await refresh();}finally{busy.value=false;}}
onMounted(refresh);
</script>
<template><section class="engine"><h2>本地模型服务</h2><p>状态：{{ status?.engine?.running?'在线':status?.engine?.loading?'加载中':'未连接' }}</p><p>请先按部署说明配置量化模型；也可以在“模型配置”中使用自己的兼容 API。</p><button :disabled="busy" @click="control('model-start')">启动受管模型</button><button :disabled="busy" @click="control('model-stop')">停止本窗口启动的模型</button><button @click="refresh">刷新</button><p>{{ message }}</p><small>AI 服务监听本机 127.0.0.1。量化文件、推理引擎与桌面程序分开下载。</small></section></template>
<style scoped>.engine{padding:24px;max-width:900px}.engine p{margin:18px 0}.engine button{margin-right:12px;padding:8px 14px}</style>
