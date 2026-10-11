import { API_BASE, ContractError, assertContract } from './contract';
import { get } from './client';
import type { StateGetResponse, StatePutResponse } from './contract';

/**
 * W-07 状态持久化客户端 —— 状态存后端文件系统 `~/.workbench/<key>.json`，
 * 不碰 localStorage（铁律 #3）。所有失败抛 ContractError，由调用方降级。
 *
 * 注意 GET 与 PUT 方法不同，所以 PUT 单独实现。
 */

export async function loadState<T>(key: string): Promise<T | null> {
  try {
    const r = await get<StateGetResponse>(`/state/${key}`);
    return (r.data ?? null) as T | null;
  } catch (e) {
    // 404 = 还没存过（正常），返回 null；其余错误继续抛，由调用方降级
    const kind = (e as { kind?: string }).kind;
    if (kind === 'http') return null;
    throw e;
  }
}

export async function putState(key: string, data: unknown): Promise<StatePutResponse> {
  let res: Response;
  try {
    res = await fetch(`${API_BASE}/state/${key}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json', Accept: 'application/json' },
      body: JSON.stringify(data),
    });
  } catch {
    throw new ContractError(`无法连接后端（PUT /state/${key}）`, 'network');
  }
  if (!res.ok) throw new ContractError(`后端返回 ${res.status}（PUT /state/${key}）`, 'http');
  let body: unknown;
  try {
    body = await res.json();
  } catch {
    throw new ContractError('后端响应不是合法 JSON', 'contract');
  }
  assertContract(body);
  return body as StatePutResponse;
}

/**
 * 读-改-写合并：后端 PUT /state/:key 是「整体覆盖」，不是字段合并。
 * 若直接 putState('ui', { navCollapsed }) 会把 ui.json 里已有的
 * currentSceneId / activePanelId 一起冲掉（后写覆盖前写）。
 * 所以所有「只改 ui 里一个字段」的写操作都走这里：先读旧值，合并后整写。
 */
export async function patchState<T extends Record<string, unknown>>(key: string, patch: Partial<T>): Promise<void> {
  let base: T;
  try {
    base = (await loadState<T>(key)) ?? ({} as T);
  } catch {
    base = {} as T;
  }
  await putState(key, { ...base, ...patch });
}
