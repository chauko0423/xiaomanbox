import { get, post } from './client';
import type { Ue5BootResponse, Ue5BootStartResponse, Ue5ProjectsResponse } from './contract';

/** UE5 工程选取 + 打开客户端：扫描本地工程列表 → 选定 → 打开 */

export function listProjects(): Promise<Ue5ProjectsResponse> {
  return get<Ue5ProjectsResponse>('/ue5/projects');
}

export function selectProject(projectPath: string): Promise<{ contract: string; ok: boolean; projectPath?: string; name?: string; code?: number; error?: string }> {
  return post('/ue5/select', { projectPath });
}

export function openProject(projectPath?: string): Promise<Ue5BootStartResponse> {
  return post<Ue5BootStartResponse>('/ue5/open', { projectPath });
}

export function addScanDir(dir: string): Promise<{ contract: string; ok: boolean; scanDirs?: string[]; code?: number; error?: string }> {
  return post('/ue5/scan-dir', { dir });
}

export function removeScanDir(dir: string): Promise<{ contract: string; ok: boolean; scanDirs?: string[]; code?: number; error?: string }> {
  return post('/ue5/scan-dir/remove', { dir });
}

export function closeProject(): Promise<{ contract: string; ok: boolean; killed?: boolean; forced?: boolean; saved?: boolean; saveNote?: string; note?: string; code?: number; error?: string }> {
  return post('/ue5/close');
}

export function switchProject(projectPath: string): Promise<{ contract: string; ok: boolean; killed?: boolean; forced?: boolean; saved?: boolean; saveNote?: string; jobId?: string; projectPath?: string; code?: number; error?: string }> {
  return post('/ue5/switch', { projectPath });
}

export function getUe5Bootstrap(): Promise<Ue5BootResponse> {
  return get<Ue5BootResponse>('/ue5/bootstrap');
}
