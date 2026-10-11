<script setup lang="ts">
import AppIcon from './AppIcon.vue';
import { t } from '../i18n';

/**
 * 面板骨架：1px 发丝线 + 大圆角 + 无填充头。
 * bare = 不渲染头 —— 专精页页头已带标题与说明，再带头就重复了。
 * icon = 面板标题左侧的线性图标（图标指引，简约风格）。
 */
withDefaults(
  defineProps<{
    title?: string;
    placeholder?: boolean;
    bare?: boolean;
    icon?: string;
  }>(),
  { title: '', placeholder: false, bare: false, icon: '' }
);
</script>

<template>
  <section class="panel">
    <header v-if="!bare" class="panel-head">
      <span class="panel-title">
        <span v-if="icon" class="panel-ico"><AppIcon :name="icon" :size="14" /></span>
        {{ title }}
      </span>
      <span v-if="placeholder" class="tag">{{ t('占位 · 未接 AI') }}</span>
    </header>
    <div class="panel-body">
      <slot />
    </div>
  </section>
</template>
