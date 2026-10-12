import { get, post, put } from './client';

/**
 * 模型提供方 + Agent 运行参数客户端（DSH「模型 / 插件」页）。
 * 安全约定：apiKey 只写不读 —— 列表只回 hasKey，编辑留空 = 不修改。
 */

export interface ProviderInfo {
  id: string;
  name: string;
  baseUrl: string;
  model: string;
  builtin: boolean;
  hasKey: boolean;
}

export interface ProviderListResponse {
  contract: string;
  ok: boolean;
  active: string | null;
  /** 监工角色绑定的提供方 id（未显式设置时 = active） */
  supervisor?: string | null;
  providers: ProviderInfo[];
}

export interface ProviderMutationResponse {
  contract: string;
  ok: boolean;
  id?: string;
  code?: number;
  error?: string;
}

export interface ProviderDraft {
  name: string;
  baseUrl: string;
  model: string;
  apiKey?: string;
}

export function listProviders(): Promise<ProviderListResponse> {
  return get<ProviderListResponse>('/providers');
}

export function addProvider(draft: ProviderDraft): Promise<ProviderMutationResponse> {
  return post<ProviderMutationResponse>('/providers', draft);
}

export function updateProvider(id: string, patch: Partial<ProviderDraft>): Promise<ProviderMutationResponse> {
  return put<ProviderMutationResponse>(`/providers/${id}`, patch);
}

export function activateProvider(id: string): Promise<ProviderMutationResponse> {
  return put<ProviderMutationResponse>(`/providers/${id}/activate`, {});
}

export function removeProvider(id: string): Promise<ProviderMutationResponse> {
  return put<ProviderMutationResponse>(`/providers/${id}/remove`, {});
}

/** 设置监工角色绑定的提供方（云端/本地任意模型） */
export function setSupervisorProvider(id: string): Promise<ProviderMutationResponse> {
  return post<ProviderMutationResponse>('/providers/supervisor', { id });
}

/* ── Agent 运行参数（插件页「Agent 循环」手风琴）────────────── */

export interface AgentConfig {
  modelUrl: string;
  model: string;
  maxIterations: number;
  toolTimeoutMs: number;
  modelTimeoutMs: number;
  temperature: number;
  thinkingBudget: number;
  /** 权限模式：false=标准受限（破坏性操作先征得同意），true=允许完全访问 */
  allowFullAccess: boolean;
  /** 会话保留：true=历史进上下文并持久化可查；false=一句一句独立不留存 */
  retainSessions: boolean;
  /** 模型上下文窗口（token，用量面板分母） */
  contextWindow: number;
  /** 估算输入上限（0 = 使用窗口减输出预留） */
  maxInputTokens: number;
  /** 输出上限（0 = 自动预留；透传引擎 max_tokens） */
  maxOutputTokens: number;
}

export interface AgentConfigResponse {
  contract: string;
  ok: boolean;
  config?: AgentConfig;
  code?: number;
  error?: string;
}

export function getAgentConfig(): Promise<AgentConfigResponse> {
  return get<AgentConfigResponse>('/agent/config');
}

export function putAgentConfig(patch: Partial<AgentConfig>): Promise<AgentConfigResponse> {
  return put<AgentConfigResponse>('/agent/config', patch);
}

/** 主流模型预配置模板（「快速接入」折叠区） */
export interface ModelPreset {
  id: string;
  name: string;
  baseUrl: string;
  model: string;
  models?: string[];
  hint?: string;
}

export function listPresets(): Promise<{ contract: string; ok: boolean; presets: ModelPreset[] }> {
  return get<{ contract: string; ok: boolean; presets: ModelPreset[] }>('/providers/presets');
}
