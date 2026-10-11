import { get, post } from './client';

/** 插件社区（dsh 生态内嵌能力目录，零外部依赖） */

export type AdaptState = 'adapted' | 'mappable' | 'n-a';

export interface DshPlugin {
  name: string;
  version: string;
  description: string;
  /** 中文描述（2026-10-09 汉化补充，中英双语） */
  descriptionZh: string;
  category: string;
  adapt: AdaptState;
  mapsTo: string[];
  note: string;
  /** 路线 A：0 依赖自研工具映射 */
  routeA: { adapt: AdaptState; mapsTo: string[]; note: string };
  /** 路线 B：外部接入 dsh 原生插件 */
  routeB: { available: boolean; note: string };
}

export interface CommunityCategory {
  id: string;
  name: string;
  count: number;
}

export interface DshRuntime {
  available: boolean;
  source: string | null;
  bin: string | null;
  version: string | null;
  reason: string | null;
}

export interface CommunityResponse {
  contract: string;
  ok: boolean;
  source: string;
  snapshotAt: string;
  total: number;
  categories: CommunityCategory[];
  adaptCounts: Record<AdaptState, number>;
  runtime: DshRuntime;
  count: number;
  plugins: DshPlugin[];
}

export function listCommunity(
  params: { category?: string; adapt?: string; q?: string } = {},
): Promise<CommunityResponse> {
  const qs = new URLSearchParams();
  if (params.category) qs.set('category', params.category);
  if (params.adapt) qs.set('adapt', params.adapt);
  if (params.q) qs.set('q', params.q);
  const s = qs.toString();
  return get<CommunityResponse>(`/plugins/community${s ? `?${s}` : ''}`);
}

/** 路线 B 外部接入：起 dsh 子进程（headless）真跑任务 */
export interface DshRunResponse {
  contract: string;
  ok: boolean;
  code?: number;
  result?: string;
  reasoning?: string;
  error?: string;
  exitCode?: number;
  durationMs?: number;
}

export function runDsh(task: string, timeoutMs?: number): Promise<DshRunResponse> {
  return post<DshRunResponse>('/plugins/dsh/run', { task, timeoutMs });
}

/** 工作台自研工具（含启用状态 + 对应 dsh 插件）——插件「下载/已装载」 */
export interface WorkbenchTool {
  name: string;
  dsh: string;
  label: string;
  enabled: boolean;
}

export interface WorkbenchToolsResponse {
  contract: string;
  ok: boolean;
  count: number;
  tools: WorkbenchTool[];
}

export function listWorkbenchTools(): Promise<WorkbenchToolsResponse> {
  return get<WorkbenchToolsResponse>('/plugins/tools');
}

export function toggleTool(name: string, enabled: boolean): Promise<{ contract: string; ok: boolean; error?: string }> {
  return post('/plugins/tools/toggle', { name, enabled });
}

/** 路线 B：headless profile 已下载/装载的原生 dsh 插件 */
export interface DshInstalledPlugin {
  name: string;
  kind: 'bundle' | 'dependency';
  loaded: boolean;
  note: string;
}

export interface DshPluginsResponse {
  contract: string;
  ok: boolean;
  route: string;
  profile: string;
  profilePath: string;
  runtime: DshRuntime;
  bundles: DshInstalledPlugin[];
  dependencies: DshInstalledPlugin[];
}

export function listDshPlugins(): Promise<DshPluginsResponse> {
  return get<DshPluginsResponse>('/plugins/dsh/plugins');
}

/** 下载/装载一个原生 dsh 插件（转发 dsh plugin → pnpm） */
export interface DshInstallResponse {
  contract: string;
  ok: boolean;
  code?: number;
  pkg: string;
  profile: string;
  loaded: boolean;
  kind: 'bundle' | 'dependency';
  note: string;
  bundles: string[];
  dependencies: string[];
  exitCode: number | null;
  detail: string;
  error?: string;
}

export function installDshPlugin(pkg: string, timeoutMs?: number): Promise<DshInstallResponse> {
  return post<DshInstallResponse>('/plugins/dsh/install', { pkg, timeoutMs });
}

export function removeDshPlugin(pkg: string): Promise<DshInstallResponse> {
  return post<DshInstallResponse>('/plugins/dsh/remove', { pkg });
}
