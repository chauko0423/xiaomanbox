import { get } from './client';

/** M3 眼 —— 事件订阅镜像 + 系统内存 */

export interface WorkbenchEvent {
  id: number;
  ts: string;
  type: string;
  sessionId?: string;
  sceneId?: string;
  jobId?: string;
  task?: string;
  answer?: string;
  error?: string;
}

export interface EventsResponse {
  latest: number;
  events: WorkbenchEvent[];
}

export interface SystemMemoryResponse {
  freeBytes: number;
  usedBytes: number;
  totalBytes: number;
  freeGiB: number;
  totalGiB: number;
  lowBelow8GiB: boolean;
}

/** 增量订阅：返回 id > since 的事件（卡片状态来自事件日志，不调模型） */
export function getEvents(since = 0): Promise<EventsResponse> {
  return get<EventsResponse>(`/events?since=${since}`);
}

export function getSystemMemory(): Promise<SystemMemoryResponse> {
  return get<SystemMemoryResponse>('/system-memory');
}
