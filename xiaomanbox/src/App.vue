<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { activePanel, initSceneState, openNewChat, switchFunction, agentChatMode } from './scenes/store';
import { NAV_GROUPS, groupOf } from './scenes/config';
import SideNav from './components/SideNav.vue';
import PanelFrame from './components/PanelFrame.vue';
import AppIcon from './components/AppIcon.vue';
import { loadState, patchState } from './api/state';
import { loadAppearance } from './scenes/appearance';
import { loadLang, t } from './i18n';
import './i18n/nav';

/**
 * 壳布局（2026-10-07 简约化改造，参考 ChatGPT / DSH / WorkBuddy）：
 *   左侧栏 = 详细导航（主按钮 + 分组条目 + 动态区 + 常驻状态），可折叠
 *   主区   = 一页只渲染一个功能（全宽专精），页头给标题与一句话说明
 * 去工程化：HUD 取景框 / 路径标注 / 序号读数全部删除，顶栏只留一条干净面包屑。
 */
const active = activePanel;

const activeGroup = computed(() => groupOf(active.value.id));

// 侧栏折叠（持久化到 ui.json，W-07：状态存文件系统；patchState 合并写，不冲掉场景/功能字段）
const collapsed = ref(false);
function toggleCollapse() {
  collapsed.value = !collapsed.value;
  patchState('ui', { navCollapsed: collapsed.value }).catch(() => {});
}

// W-07：启动时从 ~/.workbench/ui.json 恢复上次功能、场景域与折叠态；外观（配色/字体/字号）一并恢复
onMounted(async () => {
  await initSceneState();
  void loadAppearance(); // 失败静默走默认，不阻断
  void loadLang(); // 语言（中/英），失败走默认中文
  try {
    const saved = await loadState<{ navCollapsed?: boolean }>('ui');
    collapsed.value = !!saved?.navCollapsed;
  } catch {
    /* 后端离线：保持展开 */
  }
});
</script>

<template>
  <div class="shell">
    <SideNav
      :groups="NAV_GROUPS"
      :active-id="active.id"
      :collapsed="collapsed"
      @select="switchFunction"
      @cta="openNewChat"
      @toggle-collapse="toggleCollapse"
    />

    <main class="main">
      <header class="main-head">
        <div class="crumb">
          <span>{{ t(activeGroup?.name ?? '主控') }}</span>
          <span class="sep">/</span>
          <span class="here">{{ t(active.title) }}</span>
        </div>
      </header>

      <!-- agent-launch（2026-10-08 用户）：Agent 页专用布局——
           ① .content 转 flex 列并让 .page 撑满，hero 的 margin:auto 才有高度可分（真·垂直居中）；
           ② .page 解除 max-width 限宽，页头「小满 Agent」贴左上角 -->
      <div class="content" :class="{ 'agent-launch': active.id === 'agent', 'chat-launch': active.id === 'chat' }">
        <!-- :key 换功能即重挂载，入场动画每次都走 -->
        <div :key="active.id" class="page">
          <!-- 2026-10-10 用户：对话模式下标题副标题悬浮（不占垂直流），会话内容贴白线 -->
          <div class="page-head" :class="{ 'float': active.id === 'agent' && agentChatMode }">
            <h1 class="page-title">
              <span class="title-ico"><AppIcon :name="active.id" :size="22" /></span>
              <span>{{ t(active.title) }}</span>
            </h1>
            <p v-if="active.desc" class="page-desc">{{ t(active.desc) }}</p>
            <span v-if="active.placeholder" class="tag">占位 · 未接 AI</span>
          </div>
          <!-- hero-shell（2026-10-08 用户，参考千问欢迎页）：Agent 页脱掉面板壳，
               与主区同色大背景融为一体（欢迎页 + 对话页都是同色，无卡片分层感） -->
          <PanelFrame bare class="page-panel" :class="{ 'hero-shell': active.id === 'agent' }">
            <component :is="active.component" />
          </PanelFrame>
        </div>
      </div>
    </main>
  </div>
</template>
