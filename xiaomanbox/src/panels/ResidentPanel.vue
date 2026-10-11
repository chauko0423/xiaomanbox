<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { getEngineStatus, type EngineStatus } from '../api/engine';
import { listProviders, type ProviderInfo } from '../api/providers';
import { t } from '../i18n';

/**
 * 驻留角色（W-14）—— 概念说明页 + 真实驻留数据（2026-10-07）。
 *
 * 第①步讲清概念；第②步接通引擎真实 /admin/host-state（M2 手，网关新增），
 * 这里亮出真实驻留槽占用 / host KV / 淘汰计数。数据来自引擎遥测
 * （serve_telemetry.jsonl 的 context_cache.occupancy），是真实的运行时快照。
 */

const st = ref<EngineStatus | null>(null);

onMounted(async () => {
  try {
    st.value = await getEngineStatus();
  } catch {
    st.value = null;
  }
  refreshProviders();
});

// ── 当前提供方（判断是否走外部 API）────────────────────────
const providerList = ref<ProviderInfo[]>([]);
const activeProviderId = ref<string | null>(null);
const activeProvider = computed(() => providerList.value.find((p) => p.id === activeProviderId.value));
/** 当前激活的是外部 API（非本机 NInfer 引擎）→ 本机驻留层数据无法监控 */
const usingExternalApi = computed(() => {
  const p = activeProvider.value;
  return !!p && p.builtin !== true;
});
const externalProviderName = computed(() => activeProvider.value?.name ?? '外部 API');

async function refreshProviders() {
  try {
    const r = await listProviders();
    providerList.value = r.providers || [];
    activeProviderId.value = r.active;
  } catch {
    /* 忽略 */
  }
}

// 驻留层真实快照（引擎 host-state 就绪时非空）
const res = computed(() => st.value?.residency ?? null);
const residencyReady = computed(() => res.value?.available === true);

// 真实槽位占用（用于示意图点亮）
const usedSlots = computed(() => res.value?.slots?.used ?? 0);
const slotCapacity = computed(() => res.value?.slots?.capacity ?? 128);

// 槽位占用百分比（进度条）
const pct = computed(() => {
  const cap = slotCapacity.value || 1;
  return Math.min(100, Math.round((usedSlots.value / cap) * 100));
});

// 真实 host KV 占用（GiB）
const kvUsedGiB = computed(() => {
  const bytes = res.value?.kv?.used_bytes;
  return bytes != null ? (bytes / 1073741824).toFixed(2) : null;
});
const kvCapGiB = computed(() => {
  const bytes = res.value?.kv?.capacity_bytes;
  return bytes != null ? (bytes / 1073741824).toFixed(0) : null;
});

// 命中统计（reused_prompt_tokens / shared_stable_prefix / historical_fork_hits 合计）
const hitTotal = computed(() => {
  const h = res.value?.hits;
  if (!h) return null;
  return (h.reused_prompt_tokens || 0) + (h.shared_stable_prefix || 0) + (h.historical_fork_hits || 0);
});

// 驻留层的设计目标（来自立项架构，非运行数据）
const design = {
  slots: 128,
  roles: 43,
  recall: '0.22 ~ 0.27 s',
  hitRate: '99.6%',
};
</script>

<template>
  <div class="resident">
    <!-- 外部 API 警示：本机驻留层数据无法监控 -->
    <div v-if="usingExternalApi" class="ext-warn" role="alert">
      <span class="ext-warn-icon">⚠</span>
      <div class="ext-warn-body">
        <strong class="ext-warn-title">{{ t('当前使用外部 API（') }}{{ externalProviderName }}{{ t('），驻留角色数据无法监控') }}</strong>
        <p class="ext-warn-desc">
          {{ t('下面的驻留槽 / host KV / 前缀命中只反映') }}<strong>{{ t('本机 5080 引擎') }}</strong>{{ t('的运行时快照。') }}
          {{ t('现在任务实际跑在外部提供方上，这里的数据') }}<strong>{{ t('不代表当前真实状态') }}</strong>{{ t('。') }}
          {{ t('切回本机引擎（「模型配置」→ 激活') }} <code>{{ t('NInfer Local 小满') }}</code>{{ t('）后数据才对应得上。') }}
        </p>
      </div>
    </div>

    <!-- 顶部：引擎真实状态 + 诚实标注 -->
    <div class="r-head">
      <div class="r-status">
        <span class="dot" :class="{ on: st?.online === true }" />
        <span class="r-status-txt">
          {{ st?.online ? `${t('引擎在线 · 当前档位')} ${st.active ?? '—'} · ${st.ctx != null ? st.ctx.toLocaleString('en-US') : '—'} ${t('上下文')}` : t('引擎离线') }}
        </span>
      </div>
      <span class="r-tag" :class="{ ready: residencyReady }">
        {{ residencyReady ? t('驻留数据已接入') : t('能力说明 · 驻留数据待接入') }}
      </span>
    </div>

    <!-- 概念：一句话 -->
    <p class="r-lead">
      {{ t('驻留角色 = 把多个「脑子」同时常驻在显存里，') }}<strong>{{ t('切面板就是切脑子') }}</strong>{{ t('，不用每次重新喂一遍背景。') }}
    </p>

    <!-- 对比：普通会话 vs 驻留角色 -->
    <div class="r-cols">
      <div class="r-card">
        <h3 class="r-card-title">{{ t('普通会话') }}</h3>
        <p class="r-card-desc">{{ t('每次换话题 / 换角色，都要把背景重新喂一遍（重付 prefill）') }}</p>
        <ul class="r-points">
          <li>{{ t('上下文靠「重新读历史」重建') }}</li>
          <li>{{ t('切得越频繁，越慢、越费算力') }}</li>
          <li>{{ t('同一时刻通常只挂一个脑子') }}</li>
        </ul>
      </div>
      <div class="r-card highlight">
        <h3 class="r-card-title">{{ t('驻留角色') }}</h3>
        <p class="r-card-desc">{{ t('每个角色占一个槽，常驻显存，切面板 = 切脑子，秒回') }}</p>
        <ul class="r-points">
          <li>{{ t('回访从内存拉 checkpoint，不重付 prefill') }}</li>
          <li>{{ t('回访') }} <span class="num">{{ design.recall }}</span>{{ t('，命中') }} <span class="num">{{ design.hitRate }}</span></li>
          <li>{{ t('可同时挂多个角色，随切随用') }}</li>
        </ul>
      </div>
    </div>

    <!-- 槽位占用：真实运行时数据（host-state 就绪时） -->
    <div class="r-slots">
      <div class="r-slots-head">
        <span class="r-slots-title">
          {{ t('驻留槽') }} <span class="num">{{ usedSlots }}</span> / {{ slotCapacity }}
          <template v-if="residencyReady"> {{ t('已占用') }}</template>
        </span>
        <span class="r-slots-note">
          {{ residencyReady ? t('真实运行时快照') : t('尚未接入运行时数据') }}
        </span>
      </div>

      <!-- 真实占用：进度条 + 指标 -->
      <template v-if="residencyReady">
        <div class="r-meter">
          <div class="r-meter-fill" :style="{ width: pct + '%' }" />
        </div>
        <div class="r-metrics">
          <div class="r-metric">
            <span class="r-metric-k">host KV</span>
            <span class="r-metric-v">{{ kvUsedGiB ?? '—' }}<span class="r-metric-u"> / {{ kvCapGiB ?? '—' }} GiB</span></span>
          </div>
          <div class="r-metric">
            <span class="r-metric-k">{{ t('淘汰 checkpoint') }}</span>
            <span class="r-metric-v">{{ res?.dropped ?? 0 }}</span>
          </div>
          <div class="r-metric">
            <span class="r-metric-k">{{ t('前缀命中') }}</span>
            <span class="r-metric-v">{{ hitTotal ?? 0 }}</span>
          </div>
          <div class="r-metric">
            <span class="r-metric-k">{{ t('前沿 token') }}</span>
            <span class="r-metric-v">{{ res?.frontier_tokens != null ? res.frontier_tokens.toLocaleString('en-US') : '—' }}</span>
          </div>
        </div>
        <p class="foot-note">
          {{ t('每个驻留会话的 KV ≈ tokens × 17.1 KiB（立项实测）。槽位是「角色还活不活在显存里」的占位；切换面板命中同一驻留槽时，回访从内存拉 checkpoint，不重付 prefill。') }}
        </p>
      </template>

      <!-- 未接入：保留设计目标说明 -->
      <template v-else>
        <div class="r-slot-grid">
          <div v-for="i in 12" :key="i" class="r-slot">
            <span class="r-slot-idx">{{ i }}</span>
          </div>
          <span class="r-more">… {{ t('共') }} {{ design.slots }} {{ t('槽') }}</span>
        </div>
        <p class="foot-note">
          {{ t('槽位是「角色还活不活在显存里」的占位，128 槽 ≈ 43 个角色（含 KV）。引擎侧') }}
          <code>host-state</code> {{ t('运行时接口待接入，接入后这里会显示真实驻留角色与占用。') }}
        </p>
      </template>
    </div>

    <!-- 使用说明（第③步）：怎么用、怎么看，全对上真实接口 -->
    <div class="r-guide">
      <h3 class="r-guide-title">{{ t('怎么用') }}</h3>

      <!-- 三步走 -->
      <ol class="r-steps">
        <li class="r-step">
          <span class="r-step-n">1</span>
          <div class="r-step-body">
            <strong class="r-step-k">{{ t('切换档位（profile）') }}</strong>
            <p class="r-step-d">
              {{ t('引擎一次常驻一个档位的脑子。去「模型配置」或档位入口把档位切到要用的角色，引擎就会把它的背景常驻进显存 —— ') }}<em>{{ t('驻留是引擎自动做的，没有「开/关」按钮') }}</em>{{ t('。') }}
            </p>
          </div>
        </li>
        <li class="r-step">
          <span class="r-step-n">2</span>
          <div class="r-step-body">
            <strong class="r-step-k">{{ t('切面板回访') }}</strong>
            <p class="r-step-d">
              {{ t('从别的面板切回同一个角色时，引擎从内存里拉出它的 checkpoint，') }}<strong>{{ t('不重新喂背景') }}</strong>{{ t('。') }}
              {{ t('上面「前缀命中」计数会 +1，回访速度就是立项测的 0.22~0.27s。') }}
            </p>
          </div>
        </li>
        <li class="r-step">
          <span class="r-step-n">3</span>
          <div class="r-step-body">
            <strong class="r-step-k">{{ t('看命中（前缀命中）') }}</strong>
            <p class="r-step-d">
              {{ t('「前缀命中」是所有复用类命中的合计。引擎') }}<strong>{{ t('空闲时它一直是 0') }}</strong>{{ t('——不是坏了，是还没有回访流量。你有真实「切面板回访」后，这个数才会涨。') }}
            </p>
          </div>
        </li>
      </ol>

      <!-- 面板数字怎么看 -->
      <h4 class="r-guide-sub">{{ t('面板上的数字怎么看') }}</h4>
      <table class="r-table">
        <thead>
          <tr><th>{{ t('指标') }}</th><th>{{ t('含义') }}</th><th>{{ t('健康信号') }}</th></tr>
        </thead>
        <tbody>
          <tr>
            <td class="r-td-k">{{ t('驻留槽 23 / 128') }}</td>
            <td>{{ t('显存里「活着的角色」占了多少个槽') }}</td>
            <td>{{ t('占用升 = 常驻角色变多；满 128 会触发换入换出') }}</td>
          </tr>
          <tr>
            <td class="r-td-k">host KV</td>
            <td>{{ t('驻留角色的背景缓存占用（≈ 每个会话 tokens × 17.1 KiB）') }}</td>
            <td>{{ t('接近容量上限（13 GiB）说明该清理不用的角色') }}</td>
          </tr>
          <tr>
            <td class="r-td-k">{{ t('淘汰 checkpoint') }}</td>
            <td>{{ t('显存不够时被换出、下次要重付 prefill 的次数') }}</td>
            <td><strong>{{ t('理想是 0') }}</strong>{{ t('；涨了说明驻留不够放、被挤出去了') }}</td>
          </tr>
          <tr>
            <td class="r-td-k">{{ t('前缀命中') }}</td>
            <td>{{ t('回访时省掉 prefill 的复用命中次数') }}</td>
            <td><strong>{{ t('0 不一定是问题') }}</strong>{{ t('（空闲无回访）；越高越省') }}</td>
          </tr>
          <tr>
            <td class="r-td-k">{{ t('前沿 token') }}</td>
            <td>{{ t('最近一次驻留时缓存的上下文长度') }}</td>
            <td>{{ t('反映当前驻留角色的背景体量') }}</td>
          </tr>
        </tbody>
      </table>

      <p class="r-guide-note">
        {{ t('引擎侧目前只读开放 status / profiles / switch / host-state。运行时调参（slot 数、KV 容量、精度，MOD-01~MOD-10）的') }}
        <code>/admin/tune</code> {{ t('尚未开放——需要调优时找用户，属引擎侧 M2 手后续工作。') }}
      </p>
    </div>
  </div>
</template>

<style scoped>
.resident {
  display: flex;
  flex-direction: column;
  gap: 16px;
  font-family: var(--font-ui);
}

/* ── 外部 API 警示条 ─────────────────────────────────── */
.ext-warn {
  display: flex;
  gap: 10px;
  align-items: flex-start;
  padding: 12px 14px;
  border: 1px solid var(--warn);
  border-radius: var(--radius);
  background: rgba(var(--warn-rgb), 0.06);
}
.ext-warn-icon { flex: none; font-size: 1.077rem; line-height: 1.4; color: var(--warn); }
.ext-warn-body { display: flex; flex-direction: column; gap: 4px; min-width: 0; }
.ext-warn-title { font-size: 0.962rem; font-weight: 600; color: var(--warn); }
.ext-warn-desc { margin: 0; font-size: 0.885rem; line-height: 1.6; color: var(--dim); }
.ext-warn-desc strong { color: var(--ink); font-weight: 500; }
.ext-warn-desc code {
  font-family: var(--font-num);
  font-size: 0.808rem;
  color: var(--caution);
  background: var(--panel-bg-2);
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  padding: 0 4px;
}

/* 顶栏：引擎状态 + 诚实标注 */
.r-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}
.r-status {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 0.923rem;
  color: var(--dim);
}
.r-status-txt { color: var(--dim); }
.r-tag {
  flex: none;
  font-size: 0.808rem;
  color: var(--dim-2);
  border: 1px solid var(--border-bright);
  border-radius: var(--radius-sm);
  padding: 2px 8px;
}
.r-tag.ready {
  color: var(--caution);
  border-color: rgba(var(--caution-rgb), 0.4);
  background: rgba(var(--caution-rgb), 0.06);
}

/* 一句话概念 */
.r-lead {
  margin: 0;
  font-size: 1rem;
  line-height: 1.7;
  color: var(--ink);
}
.r-lead strong { color: var(--caution); font-weight: 500; }

/* 对比两卡 */
.r-cols {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 12px;
}
.r-card {
  background: var(--panel-bg);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 14px;
}
.r-card.highlight {
  background: rgba(var(--caution-rgb), 0.05);
  border-color: rgba(var(--caution-rgb), 0.35);
}
.r-card-title {
  margin: 0 0 6px;
  font-size: 1rem;
  font-weight: 500;
  color: var(--ink);
}
.r-card.highlight .r-card-title { color: var(--caution); }
.r-card-desc {
  margin: 0 0 8px;
  font-size: 0.885rem;
  line-height: 1.6;
  color: var(--dim);
}
.r-points {
  list-style: none;
  margin: 0;
  padding: 0;
  display: flex;
  flex-direction: column;
  gap: 5px;
}
.r-points li {
  position: relative;
  padding-left: 14px;
  font-size: 0.885rem;
  line-height: 1.5;
  color: var(--dim);
}
.r-points li::before {
  content: '';
  position: absolute;
  left: 2px;
  top: 7px;
  width: 5px;
  height: 5px;
  border-radius: var(--radius-round);
  background: var(--ink-3);
}
.r-card.highlight .r-points li::before { background: var(--caution); }
.r-points .num {
  font-family: var(--font-num);
  font-variant-numeric: tabular-nums;
  color: var(--ink);
}

/* 槽位示意 */
.r-slots {
  background: var(--panel-bg);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 14px;
}
.r-slots-head {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 12px;
}
.r-slots-title {
  font-size: 0.962rem;
  font-weight: 500;
  color: var(--ink);
}
.r-slots-note { font-size: 0.808rem; color: var(--ink-3); }

/* 真实占用进度条 */
.r-meter {
  height: 8px;
  border-radius: var(--radius-round);
  background: var(--panel-bg-2);
  overflow: hidden;
  margin-bottom: 12px;
}
.r-meter-fill {
  height: 100%;
  background: var(--caution);
  border-radius: var(--radius-round);
  transition: width 0.3s ease;
}

/* 指标网格 */
.r-metrics {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 10px;
  margin-bottom: 10px;
}
.r-metric {
  display: flex;
  flex-direction: column;
  gap: 3px;
  padding: 8px 10px;
  background: var(--panel-bg-2);
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
}
.r-metric-k {
  font-size: 0.769rem;
  color: var(--ink-3);
}
.r-metric-v {
  font-family: var(--font-num);
  font-variant-numeric: tabular-nums;
  font-size: 1.154rem;
  color: var(--ink);
}
.r-metric-u {
  font-size: 0.808rem;
  color: var(--dim-2);
}

/* 未接入时的假网格（保留设计目标示意） */
.r-slot-grid {
  display: flex;
  align-items: center;
  gap: 6px;
  flex-wrap: wrap;
}
.r-slot {
  width: 34px;
  height: 34px;
  display: flex;
  align-items: center;
  justify-content: center;
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  background: var(--panel-bg-2);
}
.r-slot-idx {
  font-family: var(--font-num);
  font-size: 0.808rem;
  color: var(--ink-3);
}
.r-more {
  font-size: 0.846rem;
  color: var(--dim-2);
  padding: 0 4px;
}
code {
  font-family: var(--font-num);
  font-size: 0.808rem;
  color: var(--dim-2);
  background: var(--panel-bg-2);
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  padding: 0 4px;
}

/* ── 使用说明（第③步） ─────────────────────────────────── */
.r-guide {
  background: var(--panel-bg);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 16px;
}
.r-guide-title {
  margin: 0 0 12px;
  font-size: 1rem;
  font-weight: 500;
  color: var(--ink);
}
.r-guide-sub {
  margin: 16px 0 8px;
  font-size: 0.923rem;
  font-weight: 500;
  color: var(--ink);
}

/* 三步走 */
.r-steps {
  list-style: none;
  margin: 0;
  padding: 0;
  display: flex;
  flex-direction: column;
  gap: 12px;
}
.r-step {
  display: flex;
  gap: 10px;
}
.r-step-n {
  flex: none;
  width: 20px;
  height: 20px;
  border-radius: var(--radius-round);
  background: rgba(var(--caution-rgb), 0.12);
  color: var(--caution);
  font-family: var(--font-num);
  font-size: 0.846rem;
  display: flex;
  align-items: center;
  justify-content: center;
  margin-top: 1px;
}
.r-step-body { display: flex; flex-direction: column; gap: 3px; }
.r-step-k {
  font-size: 0.962rem;
  font-weight: 500;
  color: var(--ink);
}
.r-step-d {
  margin: 0;
  font-size: 0.885rem;
  line-height: 1.6;
  color: var(--dim);
}
.r-step-d em { font-style: normal; color: var(--dim-2); }
.r-step-d strong { color: var(--ink); font-weight: 500; }

/* 数字怎么看表格 */
.r-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 0.846rem;
}
.r-table th,
.r-table td {
  text-align: left;
  padding: 7px 8px;
  border-bottom: 1px solid var(--border);
  vertical-align: top;
  line-height: 1.5;
}
.r-table th {
  font-size: 0.769rem;
  color: var(--ink-3);
  font-weight: 500;
}
.r-table tbody tr:last-child td { border-bottom: none; }
.r-td-k {
  font-family: var(--font-num);
  font-variant-numeric: tabular-nums;
  color: var(--ink);
  white-space: nowrap;
}
.r-table td { color: var(--dim); }
.r-table td strong { color: var(--ink); font-weight: 500; }

.r-guide-note {
  margin: 12px 0 0;
  font-size: 0.808rem;
  line-height: 1.6;
  color: var(--ink-3);
}
</style>
