import { get } from './client';

/**
 * 记忆读取 —— @ 上下文的真实数据源。
 * 全局记忆（跨场景长期）后端已经拼进 agent 系统提示，
 * 这里把「可挂载上下文」显式暴露给用户，让它能看见/选择要带哪些背景。
 *
 * 注意：后端只暴露 GET /memory/global（场景记忆仅通过 agent 内部注入，
 * 无独立 GET 路由），所以这里只封装全局记忆，不造不存在的接口。
 */

export interface MemoryResponse {
  contract: string;
  memory: Record<string, unknown> | null;
}

export function getGlobalMemory(): Promise<MemoryResponse> {
  return get<MemoryResponse>('/memory/global');
}
