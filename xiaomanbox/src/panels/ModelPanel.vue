<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import {
  listProviders,
  addProvider,
  updateProvider,
  activateProvider,
  removeProvider,
  listPresets,
  type ProviderInfo,
  type ModelPreset,
} from '../api/providers';
import { getAgentConfig, putAgentConfig, type AgentConfig } from '../api/providers';
import { t } from '../i18n';

/**
 * 模型配置（2026-10-07，DSH 图2「模型」页）：
 *   提供方卡片列表 + 添加/编辑内联表单 + 激活切换。
 * 真实能力：providers CRUD 走后端 ~/.workbench/providers.json，
 * 激活后小满下次派活即用该提供方（agent.mjs run() 每次读 activeProvider）。
 * 安全：apiKey 只写不读 —— 列表只回 hasKey，编辑留空 = 不修改。
 */
const providers = ref<ProviderInfo[]>([]);
const activeId = ref<string | null>(null);
const loading = ref(true);
const err = ref<string | null>(null);
const notice = ref<string | null>(null);

// 表单状态：null = 关闭；'new' = 新增；ProviderInfo.id = 编辑该条
const editing = ref<string | 'new' | null>(null);
const form = ref({ name: '', baseUrl: '', model: '', apiKey: '' });
const formErr = ref<string | null>(null);
const saving = ref(false);

// ── 快速接入主流模型（2026-10-11 用户：只填 API Key，URL/模型名由模板带出）──
const presets = ref<ModelPreset[]>([]);
const presetsOpen = ref(false); // 默认折叠
const activePresetId = ref<string | null>(null); // 当前表单正使用哪个模板（控制模型下拉提示）

async function loadPresets() {
  try {
    const r = await listPresets();
    presets.value = r.presets || [];
  } catch {
    /* 模板拉取失败不阻断主功能 */
  }
}

/** 当前模板的可选模型（新建表单模型名下拉提示） */
const activePresetModels = computed(
  () => presets.value.find((p) => p.id === activePresetId.value)?.models ?? []
);

/** 用模板填充新增表单：name/baseUrl/model 自动带出，用户只填 API Key */
function usePreset(p: ModelPreset) {
  editing.value = 'new';
  form.value = { name: p.name, baseUrl: p.baseUrl, model: p.model, apiKey: '' };
  formErr.value = null;
  activePresetId.value = p.id;
  presetsOpen.value = false; // 选完收起，聚焦表单
}

async function refresh() {
  loading.value = true;
  try {
    const r = await listProviders();
    providers.value = r.providers;
    activeId.value = r.active;
    err.value = null;
  } catch (e) {
    err.value = String((e as Error).message || e);
  } finally {
    loading.value = false;
  }
}

function openNew() {
  editing.value = 'new';
  form.value = { name: '', baseUrl: '', model: '', apiKey: '' };
  formErr.value = null;
  activePresetId.value = null;
}

function openEdit(p: ProviderInfo) {
  editing.value = p.id;
  // apiKey 不回显（后端也不给），留空 = 不修改
  form.value = { name: p.name, baseUrl: p.baseUrl, model: p.model, apiKey: '' };
  formErr.value = null;
}

function closeForm() {
  editing.value = null;
  formErr.value = null;
}

async function save() {
  if (saving.value) return;
  saving.value = true;
  formErr.value = null;
  try {
    if (editing.value === 'new') {
      const r = await addProvider({ ...form.value });
      if (!r.ok) throw new Error(r.error || `HTTP 错误 ${r.code}`);
      notice.value = `已添加「${form.value.name}」`;
    } else if (editing.value) {
      const patch: Record<string, string> = {
        name: form.value.name,
        baseUrl: form.value.baseUrl,
        model: form.value.model,
      };
      if (form.value.apiKey) patch.apiKey = form.value.apiKey; // 留空 = 不改
      const r = await updateProvider(editing.value, patch);
      if (!r.ok) throw new Error(r.error || `HTTP 错误 ${r.code}`);
      notice.value = '已保存';
    }
    closeForm();
    await refresh();
  } catch (e) {
    formErr.value = String((e as Error).message || e);
  } finally {
    saving.value = false;
  }
}

async function activate(p: ProviderInfo) {
  err.value = null;
  notice.value = null;
  try {
    const r = await activateProvider(p.id);
    if (!r.ok) throw new Error(r.error || `HTTP 错误 ${r.code}`);
    notice.value = `小满下次派活将使用「${p.name}」`;
    await refresh();
  } catch (e) {
    err.value = String((e as Error).message || e);
  }
}

async function remove(p: ProviderInfo) {
  err.value = null;
  notice.value = null;
  try {
    const r = await removeProvider(p.id);
    if (!r.ok) throw new Error(r.error || `HTTP 错误 ${r.code}`);
    notice.value = `已删除「${p.name}」`;
    await refresh();
  } catch (e) {
    err.value = String((e as Error).message || e);
  }
}

// ── 运行参数（2026-10-09 从插件页迁入）────────────────────
const cfg = ref<AgentConfig | null>(null);
const cfgErr = ref<string | null>(null);
const cfgNotice = ref<string | null>(null);
const cfgSaving = ref(false);
const openCards = ref<Record<string, boolean>>({ 'agent-loop': false, 'timeouts': false, 'context': false });

interface NumField {
  key: 'maxIterations' | 'temperature' | 'thinkingBudget' | 'toolTimeoutMs' | 'modelTimeoutMs' | 'contextWindow' | 'maxInputTokens' | 'maxOutputTokens';
  label: string;
  hint: string;
  step: number;
  min: number;
  max: number;
  unlimitedValue?: number;
  fallbackValue?: number;
}
const LOOP_FIELDS: NumField[] = [
  { key: 'maxIterations', label: '最大循环轮数', hint: '小满一次任务里最多「想→调工具」多少轮；无上限 = 模型给最终答复才停', step: 1, min: 1, max: 100, unlimitedValue: 0, fallbackValue: 25 },
  { key: 'temperature', label: '温度', hint: '越低越严谨，越高越发散（0~1）', step: 0.1, min: 0, max: 1 },
  { key: 'thinkingBudget', label: '思考预算', hint: '限制思考 token；无上限 = 不传参，引擎自己决定（4090 服务端默认满血思考）；0 = 禁止思考', step: 100, min: 0, max: 8000, unlimitedValue: -1, fallbackValue: 800 },
];
const TIMEOUT_FIELDS: NumField[] = [
  { key: 'modelTimeoutMs', label: '模型超时', hint: '等模型响应多久（毫秒）', step: 10000, min: 10000, max: 600000 },
  { key: 'toolTimeoutMs', label: '工具超时', hint: '等工具执行多久（毫秒，如编译/UE 操作）', step: 10000, min: 10000, max: 600000 },
];
const CONTEXT_FIELDS: NumField[] = [
  { key: 'contextWindow', label: '上下文窗口', hint: '应与引擎一致；每轮按估算检查输入与输出预留', step: 1024, min: 1024, max: 1048576 },
  { key: 'maxInputTokens', label: '输入 token 上限', hint: '估算输入上限（0 = 使用窗口减输出预留）', step: 1024, min: 0, max: 1048576 },
  { key: 'maxOutputTokens', label: '输出 token 上限', hint: '输出上限（0 = 自动预留 256–1024；透传 max_tokens）', step: 1024, min: 0, max: 1048576 },
];

async function loadCfg() {
  try {
    const r = await getAgentConfig();
    if (!r.ok || !r.config) throw new Error(r.error || '读取失败');
    cfg.value = r.config;
    cfgErr.value = null;
  } catch (e) {
    cfgErr.value = String((e as Error).message || e);
  }
}

async function saveCfg() {
  if (!cfg.value || cfgSaving.value) return;
  cfgSaving.value = true;
  cfgNotice.value = null;
  cfgErr.value = null;
  try {
    const patch = {
      maxIterations: Number(cfg.value.maxIterations),
      temperature: Number(cfg.value.temperature),
      thinkingBudget: Number(cfg.value.thinkingBudget),
      toolTimeoutMs: Number(cfg.value.toolTimeoutMs),
      modelTimeoutMs: Number(cfg.value.modelTimeoutMs),
      contextWindow: Number(cfg.value.contextWindow),
      maxInputTokens: Number(cfg.value.maxInputTokens),
      maxOutputTokens: Number(cfg.value.maxOutputTokens),
    };
    const r = await putAgentConfig(patch);
    if (!r.ok) throw new Error(r.error || `HTTP 错误 ${r.code}`);
    cfgNotice.value = '已保存 —— 小满下次派活生效';
    if (r.config) cfg.value = r.config;
  } catch (e) {
    cfgErr.value = String((e as Error).message || e);
  } finally {
    cfgSaving.value = false;
  }
}

function toggleUnlimited(f: NumField) {
  if (!cfg.value || f.unlimitedValue === undefined) return;
  cfg.value[f.key] = cfg.value[f.key] === f.unlimitedValue ? (f.fallbackValue ?? f.min) : f.unlimitedValue;
}

onMounted(() => {
  refresh();
  loadCfg();
  loadPresets();
});
</script>

<template>
  <div class="model-cfg">
    <p class="lead">{{ t('填入各提供方的地址与密钥即可使用其模型；点「设为使用中」切换小满的大脑。本地引擎（NInfer）无需密钥。') }}</p>

    <!-- ⚡ 快速接入主流模型（默认折叠；2026-10-11 用户：只填 API Key 即可） -->
    <section class="acc" :class="{ open: presetsOpen }">
      <button class="acc-head" @click="presetsOpen = !presetsOpen">
        <span class="acc-titles">
          <span class="acc-title">{{ t('⚡ 快速接入主流模型') }}</span>
          <span class="acc-hint">{{ t('选一家服务商，只需填 API Key，地址和模型名自动带出。') }}</span>
        </span>
        <span class="chev">⌄</span>
      </button>
      <div v-if="presetsOpen" class="acc-body">
        <div v-for="p in presets" :key="p.id" class="preset-item">
          <div class="preset-txt">
            <span class="preset-name">{{ p.name }}</span>
            <span class="preset-hint">{{ p.hint }}</span>
          </div>
          <button class="btn-inline primary" @click="usePreset(p)">{{ t('使用') }}</button>
        </div>
        <p class="preset-note">{{ t('没找到你的服务商？用下方「+ 添加提供方」手动填 Base URL / 模型名 / API Key 即可（任何 OpenAI 兼容接口都支持）。') }}</p>
      </div>
    </section>

    <div v-if="err" class="err block">{{ err }}</div>
    <div v-else-if="notice" class="notice block">{{ notice }}</div>

    <div v-if="loading" class="empty">{{ t('加载中…') }}</div>
    <div v-else class="cards">
      <div v-for="p in providers" :key="p.id" class="prov" :class="{ active: p.id === activeId }">
        <div class="prov-row">
          <span class="dot" :class="{ on: p.id === activeId }"></span>
          <span class="prov-name">{{ p.name }}</span>
          <span v-if="p.builtin" class="mini-tag">{{ t('内置') }}</span>
          <span v-else class="mini-tag dim">{{ t('自定义') }}</span>
          <span v-if="p.hasKey" class="mini-tag key">{{ t('已设密钥') }}</span>
          <span class="prov-model">{{ p.model }}</span>
          <span class="prov-actions">
            <button v-if="p.id !== activeId" class="btn-inline" @click="activate(p)">{{ t('设为使用中') }}</button>
            <span v-else class="using">{{ t('使用中') }}</span>
            <button class="btn-inline" @click="openEdit(p)">{{ t('编辑') }}</button>
            <button v-if="!p.builtin" class="btn-inline danger" @click="remove(p)">{{ t('删除') }}</button>
          </span>
        </div>
        <div class="prov-url">{{ p.baseUrl }}</div>

        <!-- 编辑内联表单 -->
        <form v-if="editing === p.id" class="form" @submit.prevent="save">
          <label>{{ t('名称') }}<input v-model="form.name" type="text" :placeholder="t('如 DeepSeek')" /></label>
          <label>Base URL<input v-model="form.baseUrl" type="text" placeholder="https://api.example.com/v1/chat/completions" /></label>
          <label>{{ t('模型名') }}<input v-model="form.model" type="text" :placeholder="t('如 deepseek-chat')" /></label>
          <label>API Key<input v-model="form.apiKey" type="password" :placeholder="p.hasKey ? t('已设置 —— 留空表示不修改') : t('本地引擎留空即可')" autocomplete="off" /></label>
          <div v-if="formErr" class="form-err">{{ formErr }}</div>
          <div class="form-actions">
            <button type="submit" class="btn-inline primary" :disabled="saving">{{ saving ? t('保存中…') : t('保存') }}</button>
            <button type="button" class="btn-inline" @click="closeForm">{{ t('取消') }}</button>
          </div>
        </form>
      </div>

      <!-- 添加：虚线大按钮 → 展开表单 -->
      <button v-if="editing !== 'new'" class="add-btn" @click="openNew">{{ t('+ 添加提供方') }}</button>
      <div v-else class="prov new">
        <div class="prov-row"><span class="prov-name">{{ t('新提供方') }}</span></div>
        <form class="form" @submit.prevent="save">
          <label>{{ t('名称') }}<input v-model="form.name" type="text" :placeholder="t('如 DeepSeek')" /></label>
          <label>Base URL<input v-model="form.baseUrl" type="text" placeholder="https://api.example.com/v1/chat/completions" /></label>
          <label>{{ t('模型名') }}<input v-model="form.model" type="text" list="preset-model-options" :placeholder="t('如 deepseek-chat')" /></label>
          <datalist id="preset-model-options">
            <option v-for="m in activePresetModels" :key="m" :value="m" />
          </datalist>
          <label>API Key<input v-model="form.apiKey" type="password" :placeholder="t('本地引擎留空即可')" autocomplete="off" /></label>
          <div v-if="formErr" class="form-err">{{ formErr }}</div>
          <div class="form-actions">
            <button type="submit" class="btn-inline primary" :disabled="saving">{{ saving ? t('保存中…') : t('添加') }}</button>
            <button type="button" class="btn-inline" @click="closeForm">{{ t('取消') }}</button>
          </div>
        </form>
      </div>
    </div>

    <!-- ══ 运行参数（2026-10-09 从插件页迁入）════ -->
    <h3 class="sec-h">{{ t('运行参数') }}</h3>
    <div v-if="cfgErr" class="err block">{{ cfgErr }}</div>
    <div v-else-if="cfgNotice" class="notice block">{{ cfgNotice }}</div>

    <template v-if="cfg">
      <section class="acc" :class="{ open: openCards['agent-loop'] }">
        <button class="acc-head" @click="openCards['agent-loop'] = !openCards['agent-loop']">
          <span class="acc-titles">
            <span class="acc-title">{{ t('Agent 循环') }}</span>
            <span class="acc-hint">{{ t('Agent 如何派发工具调用。') }}</span>
          </span>
          <span class="chev">⌄</span>
        </button>
        <div v-if="openCards['agent-loop']" class="acc-body">
          <div v-for="f in LOOP_FIELDS" :key="f.key" class="field">
            <div class="field-txt">
              <span class="field-label">{{ t(f.label) }}</span>
              <span class="field-hint">{{ t(f.hint) }}</span>
            </div>
            <div class="field-ctl">
              <label v-if="f.unlimitedValue !== undefined" class="unl">
                <input type="checkbox" :checked="cfg[f.key] === f.unlimitedValue" @change="toggleUnlimited(f)" />
                <span>{{ t('无上限') }}</span>
              </label>
              <input v-if="cfg[f.key] !== f.unlimitedValue" v-model.number="cfg[f.key]" type="number" :step="f.step" :min="f.min" :max="f.max" />
              <span v-else class="unl-tag">{{ t('无上限') }}</span>
            </div>
          </div>
        </div>
      </section>

      <section class="acc" :class="{ open: openCards['timeouts'] }">
        <button class="acc-head" @click="openCards['timeouts'] = !openCards['timeouts']">
          <span class="acc-titles">
            <span class="acc-title">{{ t('超时') }}</span>
            <span class="acc-hint">{{ t('模型与工具的等待上限。') }}</span>
          </span>
          <span class="chev">⌄</span>
        </button>
        <div v-if="openCards['timeouts']" class="acc-body">
          <div v-for="f in TIMEOUT_FIELDS" :key="f.key" class="field">
            <div class="field-txt">
              <span class="field-label">{{ t(f.label) }}</span>
              <span class="field-hint">{{ t(f.hint) }}</span>
            </div>
            <input v-model.number="cfg[f.key]" type="number" :step="f.step" :min="f.min" :max="f.max" />
          </div>
        </div>
      </section>

      <section class="acc" :class="{ open: openCards['context'] }">
        <button class="acc-head" @click="openCards['context'] = !openCards['context']">
          <span class="acc-titles">
            <span class="acc-title">{{ t('上下文与 Token') }}</span>
            <span class="acc-hint">{{ t('上下文窗口大小，以及输入 / 输出的 token 上限。') }}</span>
          </span>
          <span class="chev">⌄</span>
        </button>
        <div v-if="openCards['context']" class="acc-body">
          <div v-for="f in CONTEXT_FIELDS" :key="f.key" class="field">
            <div class="field-txt">
              <span class="field-label">{{ t(f.label) }}</span>
              <span class="field-hint">{{ t(f.hint) }}</span>
            </div>
            <input v-model.number="cfg[f.key]" type="number" :step="f.step" :min="f.min" :max="f.max" />
          </div>
        </div>
      </section>

      <button class="save-cfg" :disabled="cfgSaving" @click="saveCfg">{{ cfgSaving ? t('保存中…') : t('保存运行配置') }}</button>
    </template>
  </div>
</template>

<style scoped>
.model-cfg { display: flex; flex-direction: column; gap: 14px; max-width: 760px; }
.lead { margin: 0; font-size: 0.962rem; color: var(--dim); }

.cards { display: flex; flex-direction: column; gap: 10px; }
.prov {
  background: var(--panel-bg-2);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 14px 16px;
  transition: border-color 0.12s ease;
}
.prov:hover { border-color: var(--border-bright); }
.prov.active { border-color: var(--ok); }
.prov.new { border-style: dashed; }

.prov-row { display: flex; align-items: center; gap: 10px; min-width: 0; }
.prov-name { font-size: 1.077rem; font-weight: 600; color: var(--ink); flex: none; }
.prov-model { font-family: var(--font-num); font-size: 0.846rem; color: var(--dim-2); flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.prov-actions { display: inline-flex; gap: 8px; flex: none; }
.using { font-size: 0.846rem; color: var(--ok); }
.prov-url {
  margin: 6px 0 0 17px;
  font-family: var(--font-num);
  font-size: 0.808rem;
  color: var(--ink-3);
  word-break: break-all;
}

.mini-tag {
  font-size: 0.769rem;
  color: var(--dim);
  border: 1px solid var(--border-bright);
  border-radius: var(--radius-sm);
  padding: 0 6px;
  flex: none;
}
.mini-tag.dim { color: var(--ink-3); }
.mini-tag.key { color: var(--caution); border-color: var(--caution-mid); }

.btn-inline.danger { color: var(--err); border-color: var(--err); }
.btn-inline.danger:hover { color: var(--err); border-color: var(--err); opacity: 0.8; }
.btn-inline.primary {
  background: var(--caution);
  border-color: var(--caution);
  color: #000;
  font-weight: 600;
}
.btn-inline.primary:hover { background: var(--caution-deep); border-color: var(--caution-deep); color: #000; }
.btn-inline:disabled { opacity: 0.5; cursor: not-allowed; }

.add-btn {
  width: 100%;
  background: transparent;
  border: 1px dashed var(--border-bright);
  border-radius: var(--radius);
  color: var(--dim);
  font-size: 1rem;
  padding: 14px 0;
  cursor: pointer;
  transition: color 0.12s ease, border-color 0.12s ease;
}
.add-btn:hover { color: var(--ink); border-color: var(--caution); }

/* 内联表单 */
.form { margin-top: 12px; display: flex; flex-direction: column; gap: 10px; border-top: 1px solid var(--border); padding-top: 12px; }
.form label { display: flex; flex-direction: column; gap: 4px; font-size: 0.846rem; color: var(--dim); }
.form input { width: 100%; }
.form-err { font-size: 0.885rem; color: var(--err); }
.form-actions { display: flex; gap: 8px; }

.notice { color: var(--ok); }
.block { font-size: 0.923rem; padding: 8px 12px; border: 1px solid var(--border); border-radius: var(--radius-sm); }
.err { color: var(--err); border-color: var(--err); }

/* ── 运行参数手风琴 ── */
.sec-h { margin: 18px 0 0; font-size: 1.077rem; font-weight: 600; color: var(--ink); }
.acc {
  background: var(--panel-bg-2);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  overflow: hidden;
}
.acc-head {
  width: 100%;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
  background: transparent;
  border: none;
  padding: 12px 16px;
  cursor: pointer;
  text-align: left;
}
.acc-titles { display: flex; flex-direction: column; gap: 3px; }
.acc-title { font-size: 0.962rem; font-weight: 600; color: var(--ink); }
.acc-hint { font-size: 0.808rem; color: var(--dim); }
.chev { color: var(--dim-2); font-size: 1.154rem; }
.acc-body { border-top: 1px solid var(--border); padding: 10px 16px; display: flex; flex-direction: column; gap: 10px; }
.field { display: flex; align-items: center; justify-content: space-between; gap: 12px; }
.field-txt { display: flex; flex-direction: column; gap: 2px; min-width: 0; }
.field-label { font-size: 0.885rem; color: var(--ink); }
.field-hint { font-size: 0.769rem; color: var(--dim-2); line-height: 1.4; }
.field-ctl { display: flex; align-items: center; gap: 8px; flex: none; }
.field input[type='number'] { width: 110px; }
.unl { display: inline-flex; align-items: center; gap: 4px; font-size: 0.769rem; color: var(--dim); }
.unl-tag { font-size: 0.769rem; color: var(--caution); }
.save-cfg {
  margin-top: 4px;
  padding: 8px 16px;
  font-size: 0.923rem;
  font-weight: 600;
  background: var(--caution);
  border: 1px solid var(--caution);
  border-radius: var(--radius-sm);
  color: #000;
  cursor: pointer;
  align-self: flex-start;
}
.save-cfg:hover { background: var(--caution-deep); border-color: var(--caution-deep); }
.save-cfg:disabled { opacity: 0.5; cursor: not-allowed; }

/* ⚡ 快速接入主流模型（2026-10-11 用户） */
.preset-item { display: flex; align-items: center; justify-content: space-between; gap: 10px; padding: 7px 2px; border-bottom: 1px dashed var(--border); }
.preset-item:last-of-type { border-bottom: none; }
.preset-txt { display: flex; flex-direction: column; gap: 2px; min-width: 0; }
.preset-name { font-weight: 600; font-size: 0.923rem; }
.preset-hint { color: var(--dim); font-size: 0.8rem; }
.preset-note { color: var(--dim); font-size: 0.8rem; margin-top: 8px; }
</style>
