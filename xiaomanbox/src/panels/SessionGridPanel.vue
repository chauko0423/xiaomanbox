<script setup lang="ts">
// 会话卡片网格（W-73）占位 —— 工作中 / 待你决定 / 已完成 三列。
// M0 壳阶段用静态数据占位，真实会话状态来自事件订阅（W-74，M3 眼）。
interface SessionCard {
  id: string;
  pinTarget: string;
  title: string;
  status: 'working' | 'pending' | 'done';
}

const columns: { key: SessionCard['status']; label: string }[] = [
  { key: 'working', label: '工作中' },
  { key: 'pending', label: '待你决定' },
  { key: 'done', label: '已完成' },
];

const cards: SessionCard[] = [
  { id: 'S-018', pinTarget: 'DIAOYU/FishAI', title: '鱼类 AI 行为树重构', status: 'working' },
  { id: 'S-017', pinTarget: 'DIAOYU/UI', title: '卡牌消除结算面板', status: 'working' },
  { id: 'S-016', pinTarget: 'NInfer/gateway', title: '档位网关参数校验', status: 'pending' },
  { id: 'S-015', pinTarget: 'DIAOYU/Anim', title: '上钩体感曲线调参', status: 'pending' },
  { id: 'S-014', pinTarget: 'NInfer/quant', title: 'IQ4_XS 精度回归', status: 'done' },
  { id: 'S-013', pinTarget: 'DIAOYU/Build', title: 'AIModule 依赖接入', status: 'done' },
];
</script>

<template>
  <div class="session-grid">
    <div v-for="col in columns" :key="col.key" class="sess-col">
      <div class="sess-col-head">{{ col.label }}</div>
      <div v-for="c in cards.filter((x) => x.status === col.key)" :key="c.id" class="sess-card">
        <div class="sess-card-top">
          <span class="sess-id">{{ c.id }}</span>
          <span class="sess-pin">{{ c.pinTarget }}</span>
        </div>
        <div class="sess-title">{{ c.title }}</div>
      </div>
      <div v-if="!cards.some((x) => x.status === col.key)" class="sess-empty">—</div>
    </div>
  </div>
</template>
