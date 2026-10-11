<script setup lang="ts">
import { t } from '../i18n';

// 熵面板（W-16）占位 —— first_token_entropy + b2 破例阈值 1.5。
// 真实数据来自引擎 logits 熵（b1 已落地），M3 眼接。
const rows = [
  { label: '常识题', val: '0.018 ~ 0.060', bar: 3 },
  { label: '陷阱题（洗车）', val: '2.839', bar: 92 },
  { label: 'b2 破例阈值', val: '1.5（标定）', bar: 50 },
];

const thresholdPct = 50; // 1.5 / 3 归一化
</script>

<template>
  <div class="entropy">
    <div v-for="r in rows" :key="r.label" class="ent-row">
      <div class="ent-head">
        <span class="mono">{{ t(r.label) }}</span>
        <span class="mono val">{{ t(r.val) }}</span>
      </div>
      <div class="bar">
        <div class="bar-fill" :style="{ width: r.bar + '%' }" />
        <div class="bar-thresh" :style="{ left: thresholdPct + '%' }" />
      </div>
    </div>
    <p class="foot-note">{{ t('熵 > 1.5 → b2 破例（Off 档自动升档思考）') }}</p>
  </div>
</template>
