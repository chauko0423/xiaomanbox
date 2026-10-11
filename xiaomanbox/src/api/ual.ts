import { get, post } from './client';
import type { UalCallEnvelope, UalGraphData, UalGraphListData, UalStatusResponse } from './contract';

/**
 * seam.ual —— 内置 UnrealAgentLink 服务端（2026-10-07，UEBox 拆解融合）。
 * UE 编辑器插件直连工作台，工具已内置，摆脱 MCP 与 UEBox 桌面应用。
 */

export function getUalStatus(): Promise<UalStatusResponse> {
  return get<UalStatusResponse>('/ual/status');
}

/** 经 /ual/call 执行单个内置工具。destructive 工具必须显式 allowDestructive（画布确认弹窗后才置真）。 */
export function ualCall(
  name: string,
  args: Record<string, unknown>,
  opts: { allowDestructive?: boolean; timeoutMs?: number } = {},
): Promise<UalCallEnvelope> {
  return post<UalCallEnvelope>('/ual/call', { name, args, ...opts });
}

/** 读蓝图某张图（蓝图画布的主数据源：节点 + 引脚 + 连线 + 坐标）。 */
export async function loadBlueprintGraph(blueprintPath: string, graphName?: string): Promise<UalGraphData> {
  const r = await ualCall('blueprint_get_graph', {
    blueprint_path: blueprintPath,
    ...(graphName ? { graph_name: graphName } : {}),
  });
  if (!r.ok || !r.data) throw new Error(r.error || '读图失败');
  return r.data as unknown as UalGraphData;
}

/** 列出蓝图的全部图（事件图/函数/宏），画布选图下拉用。 */
export async function listBlueprintGraphs(blueprintPath: string): Promise<UalGraphListData> {
  const r = await ualCall('blueprint_list_graphs', { blueprint_path: blueprintPath });
  if (!r.ok || !r.data) throw new Error(r.error || '列图失败');
  return r.data as unknown as UalGraphListData;
}
