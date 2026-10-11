<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from 'vue';
import { listProjects, selectProject, openProject, getUe5Bootstrap, addScanDir, removeScanDir, closeProject, switchProject } from '../api/ue5';
import type { Ue5BootResponse, Ue5Project } from '../api/contract';
import { ContractError } from '../api/contract';
import { t } from '../i18n';

/** UE5 工程面板：管理扫描目录 → 点选工程 → 打开/切换/关闭工程（全程经内置 UnrealAgentLink） */

const projects = ref<Ue5Project[]>([]);
const scanDirs = ref<string[]>([]);
const selectedPath = ref<string | null>(null);
const boot = ref<Ue5BootResponse | null>(null);
const err = ref<string | null>(null);
const opening = ref(false);
const switching = ref<string | null>(null);
const closing = ref(false);
const showDirEditor = ref(false);
const newDir = ref('');
const addingDir = ref(false);
let pollTimer: number | undefined;

async function refreshProjects() {
  try {
    const r = await listProjects();
    projects.value = r.projects || [];
    scanDirs.value = r.scanDirs || [];
    selectedPath.value = r.selectedPath ?? boot.value?.selectedPath ?? null;
    err.value = null;
  } catch (e) {
    err.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  }
}

async function refreshJob() {
  try {
    boot.value = await getUe5Bootstrap();
    err.value = null;
  } catch (e) {
    err.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  }
}

function schedulePoll() {
  window.clearTimeout(pollTimer);
  if (boot.value?.state === 'running') {
    pollTimer = window.setTimeout(async () => {
      await refreshJob();
      schedulePoll();
    }, 2500);
  }
}

async function pick(p: Ue5Project) {
  try {
    await selectProject(p.path);
    selectedPath.value = p.path;
    err.value = null;
  } catch (e) {
    err.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  }
}

async function open() {
  opening.value = true;
  try {
    await openProject(selectedPath.value ?? undefined);
    await refreshJob();
    schedulePoll();
  } catch (e) {
    err.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  } finally {
    opening.value = false;
  }
}

async function switchTo(p: Ue5Project) {
  switching.value = p.path;
  try {
    const r = await switchProject(p.path);
    if (r.ok) {
      selectedPath.value = p.path;
      if (r.saveNote) err.value = `${r.saveNote}（已切换）`;
      await refreshJob();
      schedulePoll();
    } else {
      err.value = r.error ?? '切换失败';
    }
  } catch (e) {
    err.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  } finally {
    switching.value = null;
  }
}

async function close() {
  closing.value = true;
  try {
    const r = await closeProject();
    if (r.ok) {
      if (r.saveNote) err.value = r.saveNote;
      await refreshJob();
    } else {
      err.value = r.error ?? '关闭失败';
    }
  } catch (e) {
    err.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  } finally {
    closing.value = false;
  }
}

async function addDir() {
  const d = newDir.value.trim();
  if (!d) return;
  addingDir.value = true;
  try {
    const r = await addScanDir(d);
    if (r.ok) {
      scanDirs.value = r.scanDirs ?? [...scanDirs.value, d];
      newDir.value = '';
      await refreshProjects();
    } else {
      err.value = r.error ?? '添加失败';
    }
  } catch (e) {
    err.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  } finally {
    addingDir.value = false;
  }
}

async function browseDir() {
  const bridge = window.xiaomanBox;
  if (!bridge?.selectDirectory) {
    err.value = '当前运行环境不支持系统文件夹选择（需在 Electron 应用内使用）';
    return;
  }
  try {
    const r = await bridge.selectDirectory({ title: '选择工程所在目录' });
    if (r.canceled || !r.dir) return;
    newDir.value = r.dir;
  } catch (e) {
    err.value = e instanceof Error ? e.message : String(e);
  }
}

async function removeDir(dir: string) {
  try {
    const r = await removeScanDir(dir);
    if (r.ok) {
      scanDirs.value = r.scanDirs ?? scanDirs.value.filter((x) => x !== dir);
      await refreshProjects();
    } else {
      err.value = r.error ?? '删除失败';
    }
  } catch (e) {
    err.value = e instanceof ContractError ? `${e.kind} · ${e.message}` : String(e);
  }
}

function stateMark(s: string): string {
  switch (s) {
    case 'ok': return '✅';
    case 'fail': return '❌';
    case 'running': return '◐';
    case 'skip': return '⚠';
    default: return '·';
  }
}

onMounted(async () => {
  await Promise.all([refreshProjects(), refreshJob()]);
  schedulePoll();
});

onBeforeUnmount(() => window.clearTimeout(pollTimer));
</script>

<template>
  <div class="boot">
    <div class="boot-head">
      <button class="btn-inline boot-btn" :disabled="opening || boot?.state === 'running' || !selectedPath" @click="open">
        {{ boot?.state === 'running' ? t('正在打开…') : t('让编辑器打开所选工程') }}
      </button>
      <button class="btn-inline boot-btn danger" :disabled="closing || boot?.state === 'running'" @click="close">
        {{ closing ? t('保存并关闭中…') : t('保存并关闭工程') }}
      </button>
      <span v-if="boot?.state === 'done'" class="ok-line">{{ t('✅ 已打开 ·') }} {{ boot?.projectPath }}</span>
      <span v-else-if="boot?.state === 'failed'" class="err">{{ t('失败：') }}{{ boot?.error }}</span>
      <span v-else-if="err" class="err">{{ err }}</span>
    </div>

    <div class="proj-section">
      <div class="proj-title">
        <span>{{ t('本机工程') }}</span>
        <span class="title-actions">
          <button class="btn-ghost" @click="refreshProjects">{{ t('刷新') }}</button>
          <button class="btn-ghost" @click="showDirEditor = !showDirEditor">{{ showDirEditor ? t('收起路径') : t('路径设置') }}</button>
        </span>
      </div>

      <!-- 路径设置 -->
      <div v-if="showDirEditor" class="dir-editor">
        <div class="dir-list">
          <div v-for="d in scanDirs" :key="d" class="dir-row">
            <span class="dir-path">{{ d }}</span>
            <button class="btn-ghost btn-remove" :disabled="scanDirs.length <= 1" @click="removeDir(d)">{{ t('移除') }}</button>
          </div>
          <p v-if="!scanDirs.length" class="foot-note">{{ t('还没有扫描目录，添加一个工程所在目录即可。') }}</p>
        </div>
        <div class="dir-add">
          <input
            v-model="newDir"
            class="dir-input"
            type="text"
            :placeholder="t('粘贴工程所在目录，如 D:\\UE5_7_4\\Projects')"
            @keyup.enter="addDir"
          />
          <button class="btn-inline" :disabled="addingDir || !newDir.trim()" @click="addDir">{{ t('添加') }}</button>
          <button class="btn-inline" @click="browseDir">{{ t('浏览…') }}</button>
        </div>
        <p class="hint">{{ t('扫描目录下一层的所有 .uproject 工程会被列出。配置保存在 ~/.workbench/ue5boot.json。') }}</p>
      </div>

      <p v-if="!projects.length" class="foot-note">
        {{ scanDirs.length ? t('在') + ' ' + scanDirs.join('、') + ' ' + t('下没有扫描到 .uproject 工程。') : t('未配置扫描目录。') }}
        {{ t('点右上角「路径设置」添加工程目录。') }}
      </p>
      <ul v-else class="proj-list">
        <li
          v-for="p in projects"
          :key="p.path"
          class="proj"
          :class="{ selected: p.path === selectedPath }"
          @click="pick(p)"
        >
          <span class="proj-name">{{ p.name }}</span>
          <span class="proj-path">{{ p.path }}</span>
          <span v-if="p.path === selectedPath" class="proj-mark">✓ {{ t('已选') }}</span>
          <button
            class="btn-inline proj-switch"
            :disabled="switching === p.path || boot?.state === 'running'"
            @click.stop="switchTo(p)"
          >
            {{ switching === p.path ? t('保存并切换中…') : t('保存并切换到此工程') }}
          </button>
        </li>
      </ul>
    </div>

    <ol v-if="boot?.steps?.length" class="steps">
      <li v-for="s in boot.steps" :key="s.key" class="step" :class="s.state">
        <span class="mark">{{ stateMark(s.state) }}</span>
        <span class="label">{{ s.label }}</span>
        <span v-if="s.detail" class="detail">{{ s.detail }}</span>
        <span v-if="s.durationMs" class="ms">{{ (s.durationMs / 1000).toFixed(1) }}s</span>
      </li>
    </ol>

    <p v-else class="foot-note">
      {{ t('点选一个工程，再点「让编辑器打开所选工程」：小满会拉起 UE 编辑器打开它，并等 UnrealAgentLink 插件连上工作台。') }}
      {{ t('全程经内置插件，编辑器会自动出现在桌面。') }}
    </p>
  </div>
</template>

<style scoped>
.boot-head { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; }
.boot-btn { padding: 5px 14px; }
.ok-line { color: var(--ink); font-size: 0.923rem; }
.proj-section { margin-top: 14px; }
.proj-title { display: flex; align-items: center; justify-content: space-between; margin-bottom: 6px; font-size: 0.923rem; color: var(--dim); }
.title-actions { display: flex; gap: 4px; }
.btn-ghost { border: none; background: none; color: var(--ink-3); cursor: pointer; font-size: 0.846rem; padding: 2px 6px; }
.btn-ghost:hover { color: var(--ink); }
.btn-ghost:disabled { color: var(--ink-3); opacity: 0.4; cursor: not-allowed; }
.dir-editor { border: 1px solid var(--line, rgba(0,0,0,0.08)); border-radius: 6px; padding: 8px 10px; margin-bottom: 8px; }
.dir-list { display: flex; flex-direction: column; gap: 4px; margin-bottom: 8px; }
.dir-row { display: flex; align-items: center; justify-content: space-between; gap: 8px; font-size: 0.846rem; }
.dir-path { color: var(--ink); word-break: break-all; }
.btn-remove { flex: none; color: #e5534b; }
.dir-add { display: flex; gap: 6px; }
.dir-input { flex: 1; padding: 5px 8px; font-size: 0.923rem; border: 1px solid var(--line, rgba(0,0,0,0.15)); border-radius: 4px; background: var(--bg, #fff); color: var(--ink); }
.dir-input:focus { outline: none; border-color: var(--accent, #4c8bf5); }
.hint { color: var(--ink-3); font-size: 0.769rem; margin: 6px 0 0; }
.proj-list { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 4px; }
.proj { display: flex; align-items: center; gap: 10px; padding: 7px 10px; border: 1px solid var(--line, rgba(0,0,0,0.08)); border-radius: 6px; cursor: pointer; font-size: 0.923rem; }
.proj:hover { background: rgba(0,0,0,0.03); }
.proj.selected { border-color: var(--accent, #4c8bf5); background: rgba(76, 139, 245, 0.08); }
.proj-name { flex: none; font-weight: 600; color: var(--ink); }
.proj-path { flex: 1; color: var(--dim); font-size: 0.846rem; word-break: break-all; }
.proj-mark { flex: none; color: var(--accent, #4c8bf5); font-size: 0.846rem; }
.proj-switch { flex: none; padding: 3px 8px; font-size: 0.846rem; }
.boot-btn.danger { color: #e5534b; border-color: rgba(229, 83, 75, 0.5); }
.boot-btn.danger:hover { background: rgba(229, 83, 75, 0.08); }
.boot-btn.danger:disabled { color: var(--ink-3); border-color: var(--line, rgba(0,0,0,0.15)); opacity: 0.4; }
.steps { list-style: none; margin: 12px 0 0; padding: 0; display: flex; flex-direction: column; gap: 4px; }
.step { display: flex; align-items: baseline; gap: 10px; font-size: 0.923rem; padding: 3px 4px; }
.step.fail { background: rgba(229, 83, 75, 0.08); }
.mark { width: 16px; text-align: center; flex: none; }
.step.ok .label { color: var(--ink); }
.step.pending .label, .step.skip .label { color: var(--dim); }
.label { flex: none; }
.detail { color: var(--dim); font-size: 0.846rem; word-break: break-all; }
.ms { color: var(--ink-3); font-size: 0.769rem; flex: none; }
.foot-note { color: var(--dim); font-size: 0.923rem; margin: 12px 0 0; }
</style>
