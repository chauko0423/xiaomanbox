<script setup lang="ts">
import { onMounted, ref } from 'vue';
import {
  annotateAsset,
  blueprintLibraryApply,
  blueprintLibrarySave,
  createFolders,
  createNote,
  deleteAssets,
  deleteFolders,
  deleteNote,
  getVaultOverview,
  moveAssets,
  projectList,
  projectOpen,
  restoreAssets,
  restoreFolders,
  searchAssets,
  searchNotes,
  searchSnippets,
  updateNote,
} from '../api/vault';
import type {
  VaultAsset,
  VaultNote,
  VaultOverviewResponse,
  VaultSnippet,
} from '../api/contract';
import { ContractError } from '../api/contract';
import { t } from '../i18n';

/**
 * 资料库面板（2026-10-07，UEBox 拆解第三批的 UI 落点）。
 * 四个分页：素材库 / 笔记 / 蓝图片段库 / 工程。
 * 后端 vault.mjs 是本地 JSON 实现（backend/data/vault/vault.json），
 * 资产库是「索引」不是 UE 工程里的真资产——素材靠手动登记或小满经 vault.* 工具写入。
 */
type Tab = 'assets' | 'notes' | 'snippets' | 'projects';

const tab = ref<Tab>('assets');
const overview = ref<VaultOverviewResponse | null>(null);
const loading = ref(true);
const err = ref<string | null>(null);
const notice = ref<string | null>(null);

function errOf(e: unknown): string {
  return e instanceof ContractError ? `${e.kind} · ${e.message}` : String((e as Error)?.message || e);
}

async function refreshOverview() {
  try {
    overview.value = await getVaultOverview();
    err.value = null;
  } catch (e) {
    err.value = errOf(e);
  } finally {
    loading.value = false;
  }
}

// ── 素材库 ────────────────────────────────────────────────
const assets = ref<VaultAsset[]>([]);
const assetTotal = ref(0);
const assetQ = ref('');
const assetFolder = ref('');
const assetsLoading = ref(false);

async function loadAssets() {
  assetsLoading.value = true;
  try {
    const r = await searchAssets({
      query: assetQ.value.trim() || undefined,
      folder: assetFolder.value || undefined,
      limit: 200,
    });
    assets.value = r.assets;
    assetTotal.value = r.total;
    err.value = null;
  } catch (e) {
    err.value = errOf(e);
  } finally {
    assetsLoading.value = false;
  }
}

const newFolder = ref('');
async function addFolder() {
  const name = newFolder.value.trim();
  if (!name) return;
  try {
    const r = await createFolders([name]);
    notice.value = r.ok ? `已建文件夹「${name}」` : (r.error || '失败');
    newFolder.value = '';
    await Promise.all([refreshOverview(), loadAssets()]);
  } catch (e) {
    err.value = errOf(e);
  }
}

async function removeFolder(name: string) {
  if (!window.confirm(`删除文件夹「${name}」？素材会回到未分类，可恢复。`)) return;
  try {
    await deleteFolders([name]);
    notice.value = `已删除文件夹「${name}」（可恢复）`;
    await Promise.all([refreshOverview(), loadAssets()]);
  } catch (e) {
    err.value = errOf(e);
  }
}

async function restoreFolder(name: string) {
  try {
    await restoreFolders([name]);
    notice.value = `已恢复文件夹「${name}」`;
    await Promise.all([refreshOverview(), loadAssets()]);
  } catch (e) {
    err.value = errOf(e);
  }
}

async function removeAsset(a: VaultAsset) {
  if (!window.confirm(`软删除素材「${a.name}」？可恢复。`)) return;
  try {
    await deleteAssets([a.id]);
    notice.value = `已软删除「${a.name}」`;
    await Promise.all([refreshOverview(), loadAssets()]);
  } catch (e) {
    err.value = errOf(e);
  }
}

// ── 笔记 ────────────────────────────────────────────────
const notes = ref<VaultNote[]>([]);
const noteTotal = ref(0);
const noteQ = ref('');
const notesLoading = ref(false);
const editingNote = ref<VaultNote | null>(null);
const noteForm = ref({ title: '', content: '', tags: '' });

async function loadNotes() {
  notesLoading.value = true;
  try {
    const r = await searchNotes({ query: noteQ.value.trim() || undefined, limit: 200 });
    notes.value = r.notes;
    noteTotal.value = r.total;
    err.value = null;
  } catch (e) {
    err.value = errOf(e);
  } finally {
    notesLoading.value = false;
  }
}

function openNewNote() {
  editingNote.value = null;
  noteForm.value = { title: '', content: '', tags: '' };
}

function openEditNote(n: VaultNote) {
  editingNote.value = n;
  noteForm.value = { title: n.title, content: n.content, tags: (n.tags || []).join(', ') };
}

function tagsOf(s: string): string[] {
  return s.split(/[,，]/).map((t) => t.trim()).filter(Boolean);
}

async function saveNote() {
  const tags = tagsOf(noteForm.value.tags);
  const title = noteForm.value.title.trim();
  if (!title) {
    err.value = '标题不能为空';
    return;
  }
  try {
    if (editingNote.value) {
      const r = await updateNote(editingNote.value.id, { title, content: noteForm.value.content, tags });
      notice.value = r.ok ? '已保存笔记' : (r.error || '失败');
    } else {
      const r = await createNote(title, noteForm.value.content, tags);
      notice.value = r.ok ? '已新建笔记' : (r.error || '失败');
    }
    editingNote.value = null;
    await Promise.all([refreshOverview(), loadNotes()]);
  } catch (e) {
    err.value = errOf(e);
  }
}

async function removeNote(n: VaultNote) {
  if (!window.confirm(`删除笔记「${n.title}」？不可恢复。`)) return;
  try {
    await deleteNote(n.id);
    notice.value = '已删除笔记';
    await Promise.all([refreshOverview(), loadNotes()]);
  } catch (e) {
    err.value = errOf(e);
  }
}

// ── 蓝图片段库 ────────────────────────────────────────────
const snippets = ref<VaultSnippet[]>([]);
const snippetTotal = ref(0);
const snippetQ = ref('');
const snippetsLoading = ref(false);
const applyTarget = ref('');

async function loadSnippets() {
  snippetsLoading.value = true;
  try {
    const r = await searchSnippets({ query: snippetQ.value.trim() || undefined, limit: 200 });
    snippets.value = r.snippets;
    snippetTotal.value = r.total;
    err.value = null;
  } catch (e) {
    err.value = errOf(e);
  } finally {
    snippetsLoading.value = false;
  }
}

async function applySnippet(s: VaultSnippet) {
  const target = applyTarget.value.trim();
  if (!target) {
    err.value = '先填目标空图路径（blueprint_path），再点应用';
    return;
  }
  try {
    const r = await blueprintLibraryApply(s.id, target);
    notice.value = r.ok ? `片段「${s.name}」已写入 ${target}` : (r.error || '失败');
  } catch (e) {
    err.value = errOf(e);
  }
}

// ── 工程 ────────────────────────────────────────────────
const projects = ref<{ name: string; path: string }[]>([]);
const templates = ref<{ name: string; path: string }[]>([]);
const projectsLoading = ref(false);

async function loadProjects() {
  projectsLoading.value = true;
  try {
    const r = await projectList('list_projects');
    projects.value = r.projects || [];
    err.value = null;
    const t = await projectList('list_templates');
    templates.value = t.templates || [];
  } catch (e) {
    err.value = errOf(e);
  } finally {
    projectsLoading.value = false;
  }
}

async function openProject(p: { name: string; path: string }) {
  try {
    const r = await projectOpen(p.path);
    notice.value = r.ok ? `正在打开「${p.name}」（编辑器拉起中）` : (r.error || '失败');
  } catch (e) {
    err.value = errOf(e);
  }
}

function fmtTime(iso: string): string {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return iso;
  return d.toLocaleString('zh-CN', { hour12: false });
}

onMounted(async () => {
  await refreshOverview();
  await Promise.all([loadAssets(), loadNotes(), loadSnippets(), loadProjects()]);
});

function switchTab(t: Tab) {
  tab.value = t;
  notice.value = null;
}
</script>

<template>
  <div class="vault">
    <p class="lead">
      {{ t('本地资料库 —— 资产库是本地 JSON 索引（不是 UE 工程真资产），笔记 / 蓝图片段 / 工程分组都存') }}
      <span class="mono">backend/data/vault/vault.json</span>{{ t('。小满也能经') }} <span class="mono">vault.*</span> {{ t('工具读写这里。') }}
    </p>

    <div v-if="loading" class="empty">{{ t('加载中…') }}</div>
    <template v-else>
      <!-- 概览条 -->
      <div v-if="overview" class="statbar">
        <span class="stat"><b class="mono">{{ overview.total }}</b> {{ t('素材') }}</span>
        <span class="stat"><b class="mono">{{ overview.notes }}</b> {{ t('笔记') }}</span>
        <span class="stat"><b class="mono">{{ overview.snippets }}</b> {{ t('片段') }}</span>
        <span class="stat"><b class="mono">{{ overview.folders.length }}</b> {{ t('文件夹') }}</span>
      </div>

      <!-- 分页 -->
      <div class="tabs">
        <button v-for="tk in (['assets','notes','snippets','projects'] as Tab[])" :key="tk"
          class="tab" :class="{ on: tab === tk }" @click="switchTab(tk)">
          {{ { assets: t('素材库'), notes: t('笔记'), snippets: t('蓝图片段'), projects: t('工程') }[tk] }}
        </button>
      </div>

      <div v-if="err" class="err block">{{ err }}</div>
      <div v-else-if="notice" class="notice block">{{ notice }}</div>

      <!-- 素材库 -->
      <div v-if="tab === 'assets'" class="tab-body">
        <div class="searchbox">
          <input v-model="assetQ" type="text" :placeholder="t('搜素材：名字 / 类型 / 标签…')" @keydown.enter="loadAssets" />
          <select v-model="assetFolder" @change="loadAssets">
            <option value="">{{ t('全部文件夹') }}</option>
            <option v-for="f in overview?.folders || []" :key="f" :value="f">{{ f }}</option>
          </select>
          <button class="btn-inline" :disabled="assetsLoading" @click="loadAssets">{{ t('搜索') }}</button>
        </div>

        <div class="folders">
          <span class="sec-label">{{ t('文件夹') }}</span>
          <div class="folder-chips">
            <span v-for="f in overview?.folders || []" :key="f" class="folder-chip">
              {{ f }}
              <button class="chip-x" :title="t('删除文件夹')" @click="removeFolder(f)">×</button>
            </span>
            <span v-if="!(overview?.folders || []).length" class="dim">{{ t('暂无文件夹') }}</span>
          </div>
          <div class="folder-add">
            <input v-model="newFolder" type="text" :placeholder="t('新文件夹名')" @keydown.enter="addFolder" />
            <button class="btn-inline" @click="addFolder">{{ t('新建') }}</button>
          </div>
        </div>

        <div class="sec-label">
          {{ t('素材') }} <span class="mono">{{ assetTotal }}</span> {{ t('条') }}
          <span v-if="assetTotal > assets.length" class="dim">{{ t('（显示前') }} {{ assets.length }} {{ t('）') }}</span>
        </div>
        <div v-if="!assets.length" class="empty">{{ t('素材库还是空的 —— 素材靠手动登记或小满写入，暂无内容。') }}</div>
        <ul v-else class="list">
          <li v-for="a in assets" :key="a.id" class="row">
            <div class="row-main">
              <span class="row-name">{{ a.name }}</span>
              <span class="mono dim row-path">{{ a.path }}</span>
              <span class="row-tags">
              <span class="mini-tag dim">{{ a.type || t('未分类') }}</span>
              <span v-for="tg in a.tags" :key="tg" class="mini-tag">{{ tg }}</span>
              </span>
            </div>
            <div class="row-actions">
              <button class="btn-inline" @click="removeAsset(a)">{{ t('删除') }}</button>
            </div>
          </li>
        </ul>
      </div>

      <!-- 笔记 -->
      <div v-if="tab === 'notes'" class="tab-body">
        <div class="searchbox">
          <input v-model="noteQ" type="text" :placeholder="t('搜笔记：标题 / 内容 / 标签…')" @keydown.enter="loadNotes" />
          <button class="btn-inline" :disabled="notesLoading" @click="loadNotes">{{ t('搜索') }}</button>
          <button class="btn-inline primary" @click="openNewNote">{{ t('+ 新笔记') }}</button>
        </div>

        <form v-if="editingNote !== null || noteForm.title || noteForm.content || noteForm.tags"
          class="note-form" @submit.prevent="saveNote">
          <div class="note-form-head">{{ editingNote ? t('编辑笔记') : t('新笔记') }}</div>
          <input v-model="noteForm.title" type="text" :placeholder="t('标题')" />
          <textarea v-model="noteForm.content" rows="6" :placeholder="t('内容（Markdown 亦可）')"></textarea>
          <input v-model="noteForm.tags" type="text" :placeholder="t('标签，逗号分隔')" />
          <div class="form-actions">
            <button type="submit" class="btn-inline primary">{{ t('保存') }}</button>
            <button type="button" class="btn-inline" @click="editingNote = null; noteForm = { title: '', content: '', tags: '' }">{{ t('取消') }}</button>
          </div>
        </form>

        <div class="sec-label">{{ t('笔记') }} <span class="mono">{{ noteTotal }}</span> {{ t('条') }}</div>
        <div v-if="!notes.length" class="empty">{{ t('暂无笔记 —— 点「+ 新笔记」写一条。') }}</div>
        <ul v-else class="list">
          <li v-for="n in notes" :key="n.id" class="row note">
            <div class="row-main">
              <span class="row-name">{{ n.title }}</span>
              <span class="dim note-preview">{{ n.content.slice(0, 120) }}{{ n.content.length > 120 ? '…' : '' }}</span>
              <span class="row-tags">
                <span v-for="tg in n.tags" :key="tg" class="mini-tag">{{ tg }}</span>
                <span class="mono dim row-time">{{ fmtTime(n.updatedAt) }}</span>
              </span>
            </div>
            <div class="row-actions">
              <button class="btn-inline" @click="openEditNote(n)">{{ t('编辑') }}</button>
              <button class="btn-inline danger" @click="removeNote(n)">{{ t('删除') }}</button>
            </div>
          </li>
        </ul>
      </div>

      <!-- 蓝图片段库 -->
      <div v-if="tab === 'snippets'" class="tab-body">
        <div class="searchbox">
          <input v-model="snippetQ" type="text" :placeholder="t('搜片段：名字 / 标签…')" @keydown.enter="loadSnippets" />
          <button class="btn-inline" :disabled="snippetsLoading" @click="loadSnippets">{{ t('搜索') }}</button>
        </div>

        <div class="apply-bar">
          <input v-model="applyTarget" type="text" :placeholder="t('目标空图路径（blueprint_path），点片段「应用」写入')" />
        </div>

        <div class="sec-label">{{ t('片段') }} <span class="mono">{{ snippetTotal }}</span> {{ t('条') }}</div>
        <div v-if="!snippets.length" class="empty">{{ t('暂无蓝图片段 —— 小满可经') }} <span class="mono">blueprint_library_save</span> {{ t('存一张图为片段。') }}</div>
        <ul v-else class="list">
          <li v-for="s in snippets" :key="s.id" class="row">
            <div class="row-main">
              <span class="row-name">{{ s.name }}</span>
              <span class="mono dim row-path">{{ s.blueprint_path }}</span>
              <span class="row-tags">
                <span v-for="tg in s.tags" :key="tg" class="mini-tag">{{ tg }}</span>
                <span class="mono dim row-time">{{ fmtTime(s.savedAt) }}</span>
              </span>
            </div>
            <div class="row-actions">
              <button class="btn-inline" @click="applySnippet(s)">{{ t('应用到目标图') }}</button>
            </div>
          </li>
        </ul>
      </div>

      <!-- 工程 -->
      <div v-if="tab === 'projects'" class="tab-body">
        <div class="searchbox">
          <button class="btn-inline" :disabled="projectsLoading" @click="loadProjects">{{ t('重新扫描') }}</button>
        </div>

        <div class="sec-label">{{ t('本机 UE 工程') }} <span class="mono">{{ projects.length }}</span> {{ t('个') }}</div>
        <div v-if="!projects.length" class="empty">{{ t('没扫到 .uproject —— 确认') }} <span class="mono">UE5_PROJECT_DIR</span> {{ t('指向工程目录。') }}</div>
        <ul v-else class="list">
          <li v-for="p in projects" :key="p.path" class="row">
            <div class="row-main">
              <span class="row-name">{{ p.name }}</span>
              <span class="mono dim row-path">{{ p.path }}</span>
            </div>
            <div class="row-actions">
              <button class="btn-inline" @click="openProject(p)">{{ t('打开') }}</button>
            </div>
          </li>
        </ul>

        <div v-if="templates.length" class="sec-label" style="margin-top: 16px">{{ t('引擎模板') }} <span class="mono">{{ templates.length }}</span> {{ t('个') }}</div>
        <ul v-if="templates.length" class="list">
          <li v-for="tpl in templates" :key="tpl.path" class="row">
            <div class="row-main">
              <span class="row-name">{{ tpl.name }}</span>
              <span class="mono dim row-path">{{ tpl.path }}</span>
            </div>
          </li>
        </ul>
      </div>
    </template>
  </div>
</template>

<style scoped>
.vault { display: flex; flex-direction: column; gap: 14px; max-width: 860px; }
.lead { margin: 0; font-size: 0.962rem; color: var(--dim); }

.statbar { display: flex; gap: 8px; flex-wrap: wrap; }
.stat {
  font-size: 0.923rem;
  color: var(--dim);
  border: 1px solid var(--border);
  background: var(--panel-bg-2);
  border-radius: var(--radius-sm);
  padding: 4px 10px;
}
.stat b { color: var(--ink); font-weight: 600; margin-right: 4px; }

.tabs { display: flex; gap: 4px; border-bottom: 1px solid var(--border); }
.tab {
  background: none;
  border: none;
  border-bottom: 2px solid transparent;
  color: var(--dim);
  font-size: 1rem;
  padding: 8px 12px;
  cursor: pointer;
  transition: color 0.12s ease, border-color 0.12s ease;
}
.tab:hover { color: var(--ink); }
.tab.on { color: var(--ink); border-bottom-color: var(--caution); font-weight: 600; }

.tab-body { display: flex; flex-direction: column; gap: 12px; }

.searchbox { display: flex; gap: 8px; }
.searchbox input { flex: 1; }
.searchbox select { background: var(--panel-bg-2); border: 1px solid var(--border-bright); color: var(--ink); border-radius: var(--radius-ctl); padding: 6px 10px; font-size: 0.962rem; }

.sec-label { font-size: 0.885rem; color: var(--dim); letter-spacing: 0.3px; margin-top: 4px; }

.folders { display: flex; flex-direction: column; gap: 8px; }
.folder-chips { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; }
.folder-chip {
  display: inline-flex; align-items: center; gap: 6px;
  font-size: 0.923rem; color: var(--ink);
  border: 1px solid var(--border-bright);
  border-radius: var(--radius-sm);
  padding: 3px 6px 3px 10px;
  background: var(--panel-bg-2);
}
.chip-x { background: none; border: none; color: var(--dim-2); cursor: pointer; font-size: 1.077rem; line-height: 1; padding: 0 2px; }
.chip-x:hover { color: var(--err); }
.folder-add { display: flex; gap: 8px; }
.folder-add input { width: 200px; }

.list { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 6px; }
.row {
  display: flex; align-items: center; justify-content: space-between; gap: 12px;
  border: 1px solid var(--border);
  background: var(--panel-bg-2);
  border-radius: var(--radius);
  padding: 10px 14px;
}
.row-main { display: flex; flex-direction: column; gap: 3px; min-width: 0; }
.row-name { font-weight: 600; color: var(--ink); }
.row-path { font-size: 0.808rem; word-break: break-all; }
.row-tags { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; }
.row-time { font-size: 0.769rem; }
.row-actions { display: inline-flex; gap: 8px; flex: none; }
.note-preview { font-size: 0.885rem; word-break: break-all; }

.mini-tag {
  font-size: 0.769rem; color: var(--dim);
  border: 1px solid var(--border-bright);
  border-radius: var(--radius-sm);
  padding: 0 6px;
}
.mini-tag.dim { color: var(--ink-3); }

.note-form { display: flex; flex-direction: column; gap: 8px; border: 1px solid var(--border-bright); border-radius: var(--radius); padding: 12px; background: var(--panel-bg-2); }
.note-form-head { font-size: 0.923rem; font-weight: 600; color: var(--ink); }
.note-form textarea { resize: vertical; }
.form-actions { display: flex; gap: 8px; }

.apply-bar input { width: 100%; }

.btn-inline.primary { background: var(--caution); border-color: var(--caution); color: #000; font-weight: 600; }
.btn-inline.primary:hover { background: var(--caution-deep); border-color: var(--caution-deep); color: #000; }
.btn-inline.danger { color: var(--err); border-color: var(--err); }
.btn-inline.danger:hover { color: var(--err); border-color: var(--err); opacity: 0.8; }
.btn-inline:disabled { opacity: 0.5; cursor: not-allowed; }

.notice { color: var(--ok); }
.block { font-size: 0.923rem; padding: 8px 12px; border: 1px solid var(--border); border-radius: var(--radius-sm); }
.err { color: var(--err); border-color: var(--err); }
.empty { color: var(--dim-2); font-size: 0.923rem; padding: 32px; text-align: center; }
</style>
