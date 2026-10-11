import { get, post } from './client';
import type { NodeLibDescribeResponse, NodeLibSearchResponse, NodeLibStatsResponse, UalExportT3DResponse, UalImportT3DResponse } from './contract';

/**
 * seam.nodelib —— 内置蓝图节点参考库（2026-10-07，NodeUE 拆解内置）。
 * 离线 1783 个真实节点签名：小满写蓝图时查精确 member_name / 引脚名，不用等引擎。
 *
 * NodeUE 完整功能（2026-10-08）：蓝图 ↔ T3D 文本双向转换（W-55），
 * 走内置 UAL 桥（zero-dep）驱动 UnrealAgentLink 插件的 blueprint.export_t3d / import_t3d，
 * 不依赖 NodeUE.exe 外部程序。
 */

export function getNodeStats(): Promise<NodeLibStatsResponse> {
  return get<NodeLibStatsResponse>('/nodes/stats');
}

export function searchNodes(query: string, limit = 10): Promise<NodeLibSearchResponse> {
  const q = encodeURIComponent(query);
  return get<NodeLibSearchResponse>(`/nodes/search?q=${q}&limit=${limit}`);
}

export function describeNode(name: string): Promise<NodeLibDescribeResponse> {
  return get<NodeLibDescribeResponse>(`/nodes/describe?name=${encodeURIComponent(name)}`);
}

/** 导出蓝图（或节点）为 T3D 文本 */
export function exportT3D(blueprint_path: string, opts: { graph_name?: string; node_ids?: string[] } = {}): Promise<UalExportT3DResponse> {
  return post<UalExportT3DResponse>('/ual/call', {
    name: 'blueprint_export_t3d',
    args: { blueprint_path, ...opts },
  });
}

/** 把 T3D 文本粘回蓝图 */
export function importT3D(
  blueprint_path: string,
  text: string,
  opts: { graph_name?: string; require_empty?: boolean; offset_x?: number; offset_y?: number } = {},
): Promise<UalImportT3DResponse> {
  return post<UalImportT3DResponse>('/ual/call', {
    name: 'blueprint_import_t3d',
    args: { blueprint_path, text, ...opts },
  });
}
