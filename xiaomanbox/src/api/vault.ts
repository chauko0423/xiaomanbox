import { get, post } from './client';
import type {
  VaultCallResponse,
  VaultOverviewResponse,
  VaultProjectListResponse,
  VaultSearchAssetsResponse,
  VaultSearchNotesResponse,
  VaultSearchSnippetsResponse,
} from './contract';

/**
 * seam.vault —— 本地资料库（2026-10-07，UEBox 拆解第三批：asset/note/library）。
 * 后端 vault.mjs 提供本地 JSON 资料库（资产库 / 笔记 / 蓝图片段库 / 项目库），
 * 这里只做一层契约化封装：GET 概览 + POST 动作分发（method 形如 vault.<动作>）。
 */

export function getVaultOverview(): Promise<VaultOverviewResponse> {
  return get<VaultOverviewResponse>('/vault/overview');
}

/** 动作分发：method 形如 vault.search_assets / vault.create_note / vault.project_list ... */
function call<T = VaultCallResponse>(method: string, args: Record<string, unknown> = {}): Promise<T> {
  return post<T>(`/vault/call`, { method, args });
}

// ── 资产库 ────────────────────────────────────────────────
export function searchAssets(args: {
  query?: string;
  type?: string;
  tag?: string;
  folder?: string;
  limit?: number;
}): Promise<VaultSearchAssetsResponse> {
  return call<VaultSearchAssetsResponse>('vault.search_assets', args);
}

export function createFolders(names: string[]): Promise<VaultCallResponse> {
  return call('vault.create_folders', { names });
}

export function deleteFolders(names: string[]): Promise<VaultCallResponse> {
  return call('vault.delete_folders', { names });
}

export function restoreFolders(names: string[]): Promise<VaultCallResponse> {
  return call('vault.restore_folders', { names });
}

export function moveAssets(moves: { id: string; folder: string }[]): Promise<VaultCallResponse> {
  return call('vault.move_assets', { moves });
}

export function annotateAsset(id: string, note: string): Promise<VaultCallResponse> {
  return call('vault.annotate_asset', { id, note });
}

export function deleteAssets(ids: string[]): Promise<VaultCallResponse> {
  return call('vault.delete_assets', { ids });
}

export function restoreAssets(ids: string[]): Promise<VaultCallResponse> {
  return call('vault.restore_assets', { ids });
}

// ── 笔记 ────────────────────────────────────────────────
export function searchNotes(args: {
  query?: string;
  tag?: string;
  limit?: number;
}): Promise<VaultSearchNotesResponse> {
  return call<VaultSearchNotesResponse>('vault.search_notes', args);
}

export function createNote(title: string, content: string, tags: string[] = []): Promise<VaultCallResponse> {
  return call('vault.create_note', { title, content, tags });
}

export function updateNote(id: string, patch: { title?: string; content?: string; tags?: string[] }): Promise<VaultCallResponse> {
  return call('vault.update_note', { id, ...patch });
}

export function deleteNote(id: string): Promise<VaultCallResponse> {
  return call('vault.delete_note', { id });
}

// ── 蓝图片段库 ────────────────────────────────────────────
export function searchSnippets(args: {
  query?: string;
  tag?: string;
  limit?: number;
}): Promise<VaultSearchSnippetsResponse> {
  return call<VaultSearchSnippetsResponse>('vault.library_search', args);
}

export function blueprintLibrarySave(blueprint_path: string, name?: string, tags: string[] = []): Promise<VaultCallResponse> {
  return call('vault.blueprint_library_save', { blueprint_path, name, tags });
}

export function blueprintLibraryApply(snippet_id: string, target_path: string): Promise<VaultCallResponse> {
  return call('vault.blueprint_library_apply', { snippet_id, target_path });
}

// ── 项目库 ────────────────────────────────────────────────
export function projectList(action: 'list_projects' | 'list_templates' = 'list_projects', dirs?: string[]): Promise<VaultProjectListResponse> {
  return call<VaultProjectListResponse>('vault.project_list', { action, dirs });
}

export function projectOrganize(
  action: 'list' | 'add_group' | 'assign' | 'register',
  extra: Record<string, unknown> = {},
): Promise<VaultCallResponse> {
  return call('vault.project_organize', { action, ...extra });
}

export function projectOpen(projectPath: string): Promise<VaultCallResponse> {
  return call('vault.project_manage', { action: 'open_project', projectPath });
}
