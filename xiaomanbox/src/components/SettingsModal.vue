<script setup lang="ts">
import { currentLang, setLang, t } from '../i18n';
import { ACCENTS,FONTS,setAccent,setFont,setFontSize,resetAppearance,appearanceState } from '../scenes/appearance';
defineProps<{startTab?:string}>();
const emit=defineEmits<{(e:'close'):void;(e:'updated'):void}>();
const app=appearanceState;
</script>
<template><Teleport to="body"><div class="mask" @click="emit('close')"><div class="settings" @click.stop>
<h2>{{t('设置')}} <button @click="emit('close')">×</button></h2>
<label>{{t('语言')}}<select :value="currentLang" @change="setLang(($event.target as HTMLSelectElement).value as any)"><option value="zh-CN">简体中文</option><option value="en-US">English</option></select></label>
<label>{{t('字号')}}<input type="number" min="11" max="22" :value="app.fontSize" @change="setFontSize(Number(($event.target as HTMLInputElement).value))"></label>
<label>{{t('强调色')}}<select :value="app.accent" @change="setAccent(($event.target as HTMLSelectElement).value)"><option v-for="a in ACCENTS" :key="a.id" :value="a.id">{{a.name}}</option></select></label>
<label>{{t('字体')}}<select :value="app.font" @change="setFont(($event.target as HTMLSelectElement).value)"><option v-for="f in FONTS" :key="f.id" :value="f.id">{{f.name}}</option></select></label>
<button @click="resetAppearance">{{t('恢复默认')}}</button><p>模型配置与工具在侧栏设置中。此版本无需注册或登录。</p>
</div></div></Teleport></template>
<style scoped>.mask{position:fixed;inset:0;background:#0008;z-index:2000;display:grid;place-items:center}.settings{width:440px;max-width:90vw;padding:24px;background:var(--panel);color:var(--ink);border-radius:12px}h2{display:flex;justify-content:space-between}label{display:flex;justify-content:space-between;align-items:center;margin:18px 0}select,input,button{background:var(--hover);color:inherit;border:1px solid var(--line);padding:7px;border-radius:5px}p{opacity:.6;font-size:12px}</style>
