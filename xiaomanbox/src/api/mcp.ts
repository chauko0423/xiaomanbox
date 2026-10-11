import { get, post, del } from './client';

/**
 * MCP 服务器管理客户端（插件页「MCP 服务器」tab）。
 * registry 存 ~/.workbench/mcp-registry.json；addServer 后端带校验。
 */

export interface McpServerInfo {
  name: string;
  transport: 'stdio' | 'http';
  command?: string;
  args?: string[];
  url?: string;
}

export interface McpListResponse {
  contract: string;
  servers: McpServerInfo[];
}

export interface McpAddResponse {
  contract: string;
  ok: boolean;
  code?: number;
  error?: string;
}

export function listMcpServers(): Promise<McpListResponse> {
  return get<McpListResponse>('/mcp/servers');
}

export function addMcpServer(
  name: string,
  config: { transport: 'stdio' | 'http'; command?: string; url?: string; args?: string[] }
): Promise<McpAddResponse> {
  return post<McpAddResponse>('/mcp/servers', { name, config });
}

/** 删除一个 MCP 服务器（从注册表移除） */
export function removeMcpServer(name: string): Promise<McpAddResponse> {
  return del<McpAddResponse>(`/mcp/servers/${encodeURIComponent(name)}`);
}
