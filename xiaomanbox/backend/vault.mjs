/**
 * vault —— 本地资料库（2026-10-07，UEBox 拆解第三批：asset/note/library）
 *
 * UEBox 的「资产库 / 笔记 / 蓝图库 / 项目库」是一套桌面应用的本地 SQLite 功能，
 * 不受 MCP 限制。这里用**本地 JSON 文件**做等价轻实现，融合工作台已有的
 * ualCall（蓝图片段存取走插件读写图），不照搬 SQLite、不重复造轮子。
 *
 * 存储：backend/data/vault/vault.json（单文件，进程内缓存 + 落盘）
 *   { assets:[], notes:[], snippets:[], folders:[], deleted_folders:[], groups:{ungrouped:[],grouped:[]} }
 */

import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { ualCall } from './ual.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const VAULT_DIR = path.join(__dirname, 'data', 'vault');
const VAULT_FILE = path.join(VAULT_DIR, 'vault.json');

const EMPTY = {
  assets: [],        // { id, name, path, type, tags:[], note, folder, deleted, deletedAt }
  notes: [],         // { id, title, content, tags:[], createdAt, updatedAt }
  snippets: [],      // { id, name, tags:[], graph:{nodes,connections}, blueprint_path, savedAt }
  folders: [],       // 素材库虚拟文件夹（分类目录）
  deleted_folders: [], // 软删除的文件夹（恢复用）
  groups: { ungrouped: [], grouped: [] }, // 工程分组：{ name, projects:[] }
};

let cache = null;
let seq = 0;

function load() {
  if (cache) return cache;
  try {
    cache = JSON.parse(fs.readFileSync(VAULT_FILE, 'utf8'));
  } catch {
    cache = JSON.parse(JSON.stringify(EMPTY));
  }
  // 兼容缺字段
  for (const k of Object.keys(EMPTY)) {
    if (cache[k] === undefined) cache[k] = JSON.parse(JSON.stringify(EMPTY[k]));
  }
  return cache;
}

function save() {
  fs.mkdirSync(VAULT_DIR, { recursive: true });
  fs.writeFileSync(VAULT_FILE, JSON.stringify(cache, null, 2), 'utf8');
}

function nid(prefix) {
  seq += 1;
  return `${prefix}_${Date.now().toString(36)}_${seq}`;
}

function nowIso() {
  return new Date().toISOString();
}

function listAsset(a) {
  return { id: a.id, name: a.name, path: a.path, type: a.type, tags: a.tags, note: a.note, folder: a.folder };
}

// ────────────────────────────────────────────────────────────
// 资产库（asset）
// ────────────────────────────────────────────────────────────

/** search_assets：按名字/类型/标签/文件夹搜素材 */
export function vaultSearchAssets(p = {}) {
  const d = load();
  const q = String(p?.query || '').toLowerCase();
  const limit = Math.min(Math.max(Number(p?.limit) || 50, 1), 500);
  let hits = d.assets.filter((a) => !a.deleted);
  if (q) {
    hits = hits.filter((a) =>
      a.name.toLowerCase().includes(q) ||
      (a.tags || []).some((t) => t.toLowerCase().includes(q)) ||
      (a.type || '').toLowerCase().includes(q) ||
      (a.folder || '').toLowerCase().includes(q)
    );
  }
  if (p?.type) hits = hits.filter((a) => a.type === p.type);
  if (p?.tag) hits = hits.filter((a) => (a.tags || []).includes(p.tag));
  if (p?.folder) hits = hits.filter((a) => a.folder === p.folder);
  return { ok: true, total: hits.length, returned: Math.min(hits.length, limit), assets: hits.slice(0, limit).map(listAsset) };
}

/** library_overview：素材库概览（总数/类型分布/文件夹/标签） */
export function vaultLibraryOverview() {
  const d = load();
  const assets = d.assets.filter((a) => !a.deleted);
  const byType = {};
  const byFolder = {};
  const tags = new Set();
  for (const a of assets) {
    byType[a.type || 'unknown'] = (byType[a.type || 'unknown'] || 0) + 1;
    byFolder[a.folder || '(未分类)'] = (byFolder[a.folder || '(未分类)'] || 0) + 1;
    (a.tags || []).forEach((t) => tags.add(t));
  }
  return {
    ok: true,
    total: assets.length,
    by_type: byType,
    by_folder: byFolder,
    folders: d.folders,
    tags: [...tags],
    notes: d.notes.length,
    snippets: d.snippets.length,
  };
}

/** switch_vault：切换保管库（工作台轻实现：单库，列出当前库名） */
export function vaultSwitchVault(p = {}) {
  return { ok: true, current_vault: 'default', note: '工作台本地资料库为单库模式（default），无需切换' };
}

/** annotate_asset：给素材加/改备注 */
export function vaultAnnotateAsset(p = {}) {
  const d = load();
  const a = d.assets.find((x) => x.id === p?.id && !x.deleted);
  if (!a) return { ok: false, error: `素材 ${p?.id} 不存在` };
  a.note = String(p?.note ?? '');
  save();
  return { ok: true, asset: listAsset(a) };
}

/** list_tags：列出全部标签及计数 */
export function vaultListTags() {
  const d = load();
  const count = {};
  for (const a of d.assets) {
    if (a.deleted) continue;
    for (const t of a.tags || []) count[t] = (count[t] || 0) + 1;
  }
  return { ok: true, tags: Object.entries(count).map(([tag, n]) => ({ tag, count: n })).sort((a, b) => b.count - a.count) };
}

/** manage_tags：给素材加/删标签 */
export function vaultManageTags(p = {}) {
  const d = load();
  const a = d.assets.find((x) => x.id === p?.asset_id && !x.deleted);
  if (!a) return { ok: false, error: `素材 ${p?.asset_id} 不存在` };
  a.tags = a.tags || [];
  for (const t of p?.add || []) if (!a.tags.includes(t)) a.tags.push(t);
  a.tags = a.tags.filter((t) => !(p?.remove || []).includes(t));
  save();
  return { ok: true, asset: listAsset(a) };
}

/** delete_tags：删除标签（从所有素材移除） */
export function vaultDeleteTags(p = {}) {
  const d = load();
  const targets = p?.tags || [];
  let removed = 0;
  for (const a of d.assets) {
    const before = (a.tags || []).length;
    a.tags = (a.tags || []).filter((t) => !targets.includes(t));
    removed += before - a.tags.length;
  }
  save();
  return { ok: true, removed };
}

/** delete_assets：软删除素材（可恢复） */
export function vaultDeleteAssets(p = {}) {
  const d = load();
  const ids = p?.ids || [];
  let n = 0;
  for (const id of ids) {
    const a = d.assets.find((x) => x.id === id && !x.deleted);
    if (a) { a.deleted = true; a.deletedAt = nowIso(); n++; }
  }
  save();
  return { ok: true, deleted: n };
}

/** restore_assets：恢复软删除的素材 */
export function vaultRestoreAssets(p = {}) {
  const d = load();
  const ids = p?.ids || [];
  let n = 0;
  for (const id of ids) {
    const a = d.assets.find((x) => x.id === id && x.deleted);
    if (a) { a.deleted = false; delete a.deletedAt; n++; }
  }
  save();
  return { ok: true, restored: n };
}

/** move_assets：把素材移到文件夹（moves: [{id, folder}]） */
export function vaultMoveAssets(p = {}) {
  const d = load();
  const moves = p?.moves || [];
  const results = [];
  for (const m of moves) {
    const a = d.assets.find((x) => x.id === m.id && !x.deleted);
    if (!a) { results.push({ id: m.id, ok: false, error: '素材不存在' }); continue; }
    a.folder = m.folder || '';
    if (a.folder && !d.folders.includes(a.folder)) d.folders.push(a.folder);
    results.push({ id: m.id, ok: true, folder: a.folder });
  }
  save();
  return { ok: true, moved: results.filter((r) => r.ok).length, results };
}

/** create_folders：新建文件夹 */
export function vaultCreateFolders(p = {}) {
  const d = load();
  const created = [];
  for (const name of p?.names || []) {
    if (!d.folders.includes(name)) { d.folders.push(name); created.push(name); }
  }
  save();
  return { ok: true, created };
}

/** rename_folder：重命名文件夹（同步更新素材的 folder 字段） */
export function vaultRenameFolder(p = {}) {
  const d = load();
  const { old: oldName, new: newName } = p || {};
  if (!d.folders.includes(oldName)) return { ok: false, error: `文件夹 ${oldName} 不存在` };
  d.folders = d.folders.map((f) => (f === oldName ? newName : f));
  for (const a of d.assets) if (a.folder === oldName) a.folder = newName;
  save();
  return { ok: true, renamed: `${oldName} → ${newName}` };
}

/** delete_folders：软删除文件夹（素材仍在，folder 置空） */
export function vaultDeleteFolders(p = {}) {
  const d = load();
  const names = p?.names || [];
  const removed = [];
  for (const name of names) {
    if (d.folders.includes(name)) {
      d.folders = d.folders.filter((f) => f !== name);
      if (!d.deleted_folders.includes(name)) d.deleted_folders.push(name);
      for (const a of d.assets) if (a.folder === name) a.folder = '';
      removed.push(name);
    }
  }
  save();
  return { ok: true, removed };
}

/** restore_folders：恢复软删除的文件夹 */
export function vaultRestoreFolders(p = {}) {
  const d = load();
  const names = p?.names || [];
  const restored = [];
  for (const name of names) {
    if (d.deleted_folders.includes(name)) {
      d.deleted_folders = d.deleted_folders.filter((f) => f !== name);
      if (!d.folders.includes(name)) d.folders.push(name);
      restored.push(name);
    }
  }
  save();
  return { ok: true, restored };
}

// ────────────────────────────────────────────────────────────
// 笔记（note）
// ────────────────────────────────────────────────────────────

export function vaultSearchNotes(p = {}) {
  const d = load();
  const q = String(p?.query || '').toLowerCase();
  const limit = Math.min(Math.max(Number(p?.limit) || 50, 1), 500);
  let hits = d.notes;
  if (q) hits = hits.filter((n) => n.title.toLowerCase().includes(q) || n.content.toLowerCase().includes(q) || (n.tags || []).some((t) => t.toLowerCase().includes(q)));
  if (p?.tag) hits = hits.filter((n) => (n.tags || []).includes(p.tag));
  return { ok: true, total: hits.length, returned: Math.min(hits.length, limit), notes: hits.slice(0, limit) };
}

export function vaultGetNote(p = {}) {
  const d = load();
  const n = d.notes.find((x) => x.id === p?.id);
  if (!n) return { ok: false, error: `笔记 ${p?.id} 不存在` };
  return { ok: true, note: n };
}

export function vaultCreateNote(p = {}) {
  const d = load();
  const note = {
    id: nid('n'), title: String(p?.title || '未命名'), content: String(p?.content || ''),
    tags: p?.tags || [], createdAt: nowIso(), updatedAt: nowIso(),
  };
  d.notes.push(note);
  save();
  return { ok: true, note };
}

export function vaultUpdateNote(p = {}) {
  const d = load();
  const n = d.notes.find((x) => x.id === p?.id);
  if (!n) return { ok: false, error: `笔记 ${p?.id} 不存在` };
  if (p?.title !== undefined) n.title = String(p.title);
  if (p?.content !== undefined) n.content = String(p.content);
  if (p?.tags !== undefined) n.tags = p.tags;
  n.updatedAt = nowIso();
  save();
  return { ok: true, note: n };
}

export function vaultDeleteNote(p = {}) {
  const d = load();
  const before = d.notes.length;
  d.notes = d.notes.filter((x) => x.id !== p?.id);
  save();
  return { ok: true, deleted: before - d.notes.length };
}

// ────────────────────────────────────────────────────────────
// 蓝图片段库（library）
// ────────────────────────────────────────────────────────────

export function vaultLibrarySearch(p = {}) {
  const d = load();
  const q = String(p?.query || '').toLowerCase();
  const limit = Math.min(Math.max(Number(p?.limit) || 50, 1), 500);
  let hits = d.snippets;
  if (q) hits = hits.filter((s) => s.name.toLowerCase().includes(q) || (s.tags || []).some((t) => t.toLowerCase().includes(q)));
  if (p?.tag) hits = hits.filter((s) => (s.tags || []).includes(p.tag));
  return { ok: true, total: hits.length, returned: Math.min(hits.length, limit), snippets: hits.slice(0, limit).map((s) => ({ id: s.id, name: s.name, tags: s.tags, blueprint_path: s.blueprint_path, savedAt: s.savedAt })) };
}

/** blueprint_library_save：读一张蓝图图存成片段（融合 ualCall 读图） */
export async function vaultBlueprintSave(p = {}, timeoutMs) {
  const bp = p?.blueprint_path;
  if (!bp) return { ok: false, error: '缺少 blueprint_path' };
  let graph;
  try {
    graph = await ualCall('blueprint.get_graph', { blueprint_path: bp }, timeoutMs);
  } catch (e) {
    return { ok: false, error: `读图失败：${e.message}` };
  }
  const d = load();
  const snippet = {
    id: nid('s'), name: String(p?.name || bp.split('/').pop() || '片段'),
    tags: p?.tags || [], blueprint_path: bp,
    graph: { nodes: graph?.nodes || [], connections: graph?.connections || [] },
    savedAt: nowIso(),
  };
  d.snippets.push(snippet);
  save();
  return { ok: true, snippet: { id: snippet.id, name: snippet.name, blueprint_path: bp, nodes: snippet.graph.nodes.length, connections: snippet.graph.connections.length } };
}

/** blueprint_library_apply：把库里的片段写进一张空图（融合 ualCall 写图） */
export async function vaultBlueprintApply(p = {}, timeoutMs) {
  const d = load();
  const s = d.snippets.find((x) => x.id === p?.snippet_id);
  if (!s) return { ok: false, error: `片段 ${p?.snippet_id} 不存在` };
  const target = p?.target_path;
  if (!target) return { ok: false, error: '缺少 target_path（目标空图路径）' };
  try {
    const r = await ualCall('blueprint.create_graph', {
      blueprint_path: target,
      nodes: s.graph.nodes || [],
      connections: s.graph.connections || [],
    }, timeoutMs);
    return { ok: true, applied: true, target, snippet: s.name, result: r };
  } catch (e) {
    return { ok: false, error: `写图失败：${e.message}` };
  }
}

// ────────────────────────────────────────────────────────────
// 项目库（project）
// ────────────────────────────────────────────────────────────

/** project_list：扫描本机 UE 工程（.uproject）或列引擎模板 */
export function vaultProjectList(p = {}) {
  const action = p?.action || 'list_projects';
  if (action === 'list_templates') {
    // 读引擎 Template 目录（.uproject 里的模板），跨版本目录结构不一，尽力而为
    const roots = [
      process.env.UE5_TEMPLATE_DIR,
    ].filter(Boolean);
    const found = [];
    for (const r of roots) {
      try {
        for (const e of fs.readdirSync(r, { withFileTypes: true })) {
          if (e.isDirectory()) {
            const up = path.join(r, e.name, `${e.name}.uproject`);
            if (fs.existsSync(up)) found.push({ name: e.name, path: up });
          }
        }
      } catch { /* 目录不存在 */ }
    }
    return { ok: true, action, templates: found };
  }
  // list_projects：扫描常见工程目录下的 .uproject
  const dirs = (p?.dirs && p.dirs.length) ? p.dirs : [process.env.UE5_PROJECT_DIR, process.cwd()].filter(Boolean);
  const projects = [];
  const seen = new Set();
  for (const dir of dirs) {
    if (!dir) continue;
    try {
      for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
        if (e.isDirectory()) {
          const inner = path.join(dir, e.name);
          try {
            for (const f of fs.readdirSync(inner)) {
              if (f.endsWith('.uproject')) {
                const up = path.join(inner, f);
                if (!seen.has(up)) { seen.add(up); projects.push({ name: f.replace(/\.uproject$/, ''), path: up }); }
              }
            }
          } catch { /* 无权限 */ }
        }
      }
    } catch { /* 目录不存在 */ }
  }
  return { ok: true, action, projects };
}

/** project_organize：工程分组管理 */
export function vaultProjectOrganize(p = {}) {
  const d = load();
  const action = p?.action || 'list';
  if (action === 'list') return { ok: true, groups: d.groups };
  if (action === 'add_group') {
    const name = p?.name;
    if (!name) return { ok: false, error: '缺少 name' };
    if (d.groups.grouped.find((g) => g.name === name)) return { ok: false, error: `分组 ${name} 已存在` };
    d.groups.grouped.push({ name, projects: [] });
    save();
    return { ok: true, groups: d.groups };
  }
  if (action === 'assign') {
    const g = d.groups.grouped.find((x) => x.name === p?.group);
    if (!g) return { ok: false, error: `分组 ${p?.group} 不存在` };
    for (const proj of p?.projects || []) {
      d.groups.ungrouped = d.groups.ungrouped.filter((x) => x !== proj);
      if (!g.projects.includes(proj)) g.projects.push(proj);
    }
    save();
    return { ok: true, groups: d.groups };
  }
  if (action === 'register') {
    for (const proj of p?.projects || []) {
      const inAny = d.groups.grouped.some((g) => g.projects.includes(proj));
      if (!inAny && !d.groups.ungrouped.includes(proj)) d.groups.ungrouped.push(proj);
    }
    save();
    return { ok: true, groups: d.groups };
  }
  return { ok: false, error: `未知 action：${action}` };
}

// ────────────────────────────────────────────────────────────
// project_manage（open_project 融合 launchEditor 逻辑）+ 分发
// ────────────────────────────────────────────────────────────

/** project_manage：open_project 直接拉起编辑器打开指定工程；create_project 引导走 bootstrap */
export function vaultProjectManage(p = {}) {
  const action = p?.action || 'open_project';
  if (action === 'open_project') {
    const uproject = p?.projectPath || p?.path;
    if (!uproject) return { ok: false, error: '缺少 projectPath' };
    const exe = process.env.UE5_EDITOR_EXE;
    if (!exe || !fs.existsSync(exe)) return {ok:false,error:'请设置 UE5_EDITOR_EXE 为本机 UnrealEditor.exe 的完整路径'};
    const env = { ...process.env };
    for (const k of ['HTTP_PROXY', 'HTTPS_PROXY', 'http_proxy', 'https_proxy', 'ALL_PROXY', 'all_proxy']) delete env[k];
    try {
      const child = spawn(exe, [uproject], { env, detached: true, stdio: 'ignore', windowsHide: true });
      child.on('error', () => {});
      child.unref();
      return { ok: true, action, uproject, pid: child.pid };
    } catch (e) {
      return { ok: false, error: String(e.message || e) };
    }
  }
  if (action === 'create_project') {
    // 完整建工程流程已在 workbench_ue5_bootstrap 里（建工程→装插件→开编辑器）
    return { ok: false, error: '建工程请用 workbench_ue5_bootstrap（一键从零建工程+开编辑器+建示例蓝图），它会处理模板选择、插件安装和启动' };
  }
  return { ok: false, error: `未知 action：${action}` };
}

/** 资料库工具分发：method 形如 vault.search_assets / vault.blueprint_library_save / ... */
export async function vaultLocalCall(name, params = {}, timeoutMs = 30_000) {
  switch (name) {
    // 资产库
    case 'vault.search_assets': return vaultSearchAssets(params);
    case 'vault.library_overview': return vaultLibraryOverview();
    case 'vault.switch_vault': return vaultSwitchVault(params);
    case 'vault.annotate_asset': return vaultAnnotateAsset(params);
    case 'vault.list_tags': return vaultListTags();
    case 'vault.manage_tags': return vaultManageTags(params);
    case 'vault.delete_tags': return vaultDeleteTags(params);
    case 'vault.delete_assets': return vaultDeleteAssets(params);
    case 'vault.restore_assets': return vaultRestoreAssets(params);
    case 'vault.move_assets': return vaultMoveAssets(params);
    case 'vault.create_folders': return vaultCreateFolders(params);
    case 'vault.rename_folder': return vaultRenameFolder(params);
    case 'vault.delete_folders': return vaultDeleteFolders(params);
    case 'vault.restore_folders': return vaultRestoreFolders(params);
    // 笔记
    case 'vault.search_notes': return vaultSearchNotes(params);
    case 'vault.get_note': return vaultGetNote(params);
    case 'vault.create_note': return vaultCreateNote(params);
    case 'vault.update_note': return vaultUpdateNote(params);
    case 'vault.delete_note': return vaultDeleteNote(params);
    // 蓝图片段库
    case 'vault.library_search': return vaultLibrarySearch(params);
    case 'vault.blueprint_library_save': return vaultBlueprintSave(params, timeoutMs);
    case 'vault.blueprint_library_apply': return vaultBlueprintApply(params, timeoutMs);
    // 项目库
    case 'vault.project_list': return vaultProjectList(params);
    case 'vault.project_manage': return vaultProjectManage(params);
    case 'vault.project_organize': return vaultProjectOrganize(params);
    default: return { ok: false, error: `未知资料库工具：${name}` };
  }
}
