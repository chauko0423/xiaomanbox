/**
 * 工作区 API（2026-10-08 用户）：欢迎页「工作区 ▾」选择器的数据源。
 * 后端端点：GET /workspace、POST /workspace/add|active|remove（backend/workspace.mjs）。
 */
import { get, post } from './client';

export interface WorkspaceInfo {
  path: string;
  name: string;
  addedAt: number;
}

export interface WorkspaceListResponse {
  active: string | null;
  list: WorkspaceInfo[];
}

/** 工作区清单 + 当前活跃工作区 */
export async function listWorkspaces(): Promise<WorkspaceListResponse> {
  const r = await get<{ active: string | null; list: WorkspaceInfo[] }>('/workspace');
  return { active: r.active ?? null, list: r.list ?? [] };
}

/** 添加工作区（幂等；Electron selectDirectory 选出的绝对路径） */
export async function addWorkspace(path: string): Promise<WorkspaceListResponse> {
  return post<WorkspaceListResponse>('/workspace/add', { path });
}

/** 切换活跃工作区 */
export async function setActiveWorkspace(path: string): Promise<WorkspaceListResponse> {
  return post<WorkspaceListResponse>('/workspace/active', { path });
}

/** 移除工作区 */
export async function removeWorkspace(path: string): Promise<WorkspaceListResponse> {
  return post<WorkspaceListResponse>('/workspace/remove', { path });
}
