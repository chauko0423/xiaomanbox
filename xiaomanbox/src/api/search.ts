import { get } from './client';

/** W-78 全局搜索 */

export interface SearchHit {
  sessionId?: string;
  title?: string;
  pinTarget?: string;
  hitCount?: number;
  hits?: string[];
  source?: string;
  name?: string;
  transport?: string;
  brief?: string;
  risk?: string;
  description?: string;
}

export interface SearchResponse {
  query: string;
  sessions: SearchHit[];
  tools: SearchHit[];
  skills: SearchHit[];
  counts: { sessions: number; tools: number; skills: number };
  /** UEBox 工具清单缓存未命中（离线/过期/未配置），这部分工具未纳入本次搜索 */
  ueboxSkipped?: boolean;
}

export function searchAll(q: string): Promise<SearchResponse> {
  return get<SearchResponse>(`/search?q=${encodeURIComponent(q)}`);
}
