import { get } from './client';
import type { UeboxStatusResponse, UeboxToolsResponse } from './contract';

/** seam.ue5 客户端：UEBox MCP 连接状态与工具清单（数据来自后端 MCP 客户端） */

export function getUeboxStatus(): Promise<UeboxStatusResponse> {
  return get<UeboxStatusResponse>('/uebox/status');
}

export function getUeboxTools(force = false): Promise<UeboxToolsResponse> {
  return get<UeboxToolsResponse>(`/uebox/tools${force ? '?force=1' : ''}`);
}
