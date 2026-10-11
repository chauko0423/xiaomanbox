import { API_BASE, CONTRACT_VERSION, ContractError, assertContract } from './contract';

/**
 * 契约 v1 客户端：前端访问后端的唯一入口。
 * 任何失败（后端未启动 / 非 2xx / 契约不符 / 响应非 JSON）都会抛 ContractError，
 * 由调用方捕获后「明确报错并降级」，而不是白屏。
 */
export async function get<T>(path: string): Promise<T> {
  let res: Response;
  try {
    res = await fetch(`${API_BASE}${path}`, {
      method: 'GET',
      headers: { Accept: 'application/json' },
    });
  } catch {
    throw new ContractError(`无法连接后端（${API_BASE}${path}），后端可能未启动`, 'network');
  }

  if (!res.ok) {
    throw new ContractError(`后端返回 ${res.status}（${API_BASE}${path}）`, 'http');
  }

  let body: unknown;
  try {
    body = await res.json();
  } catch {
    throw new ContractError('后端响应不是合法 JSON', 'contract');
  }

  assertContract(body);
  return body as T;
}

export async function post<T>(path: string, body?: unknown): Promise<T> {
  let res: Response;
  try {
    res = await fetch(`${API_BASE}${path}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', Accept: 'application/json' },
      body: body === undefined ? '{}' : JSON.stringify(body),
    });
  } catch {
    throw new ContractError(`无法连接后端（${API_BASE}${path}）`, 'network');
  }
  if (!res.ok) {
    throw new ContractError(`后端返回 ${res.status}（${API_BASE}${path}）`, 'http');
  }
  let parsed: unknown;
  try {
    parsed = await res.json();
  } catch {
    throw new ContractError('后端响应不是合法 JSON', 'contract');
  }
  assertContract(parsed);
  return parsed as T;
}

export async function put<T>(path: string, body?: unknown): Promise<T> {
  let res: Response;
  try {
    res = await fetch(`${API_BASE}${path}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json', Accept: 'application/json' },
      body: body === undefined ? '{}' : JSON.stringify(body),
    });
  } catch {
    throw new ContractError(`无法连接后端（${API_BASE}${path}）`, 'network');
  }
  if (!res.ok) {
    throw new ContractError(`后端返回 ${res.status}（${API_BASE}${path}）`, 'http');
  }
  let parsed: unknown;
  try {
    parsed = await res.json();
  } catch {
    throw new ContractError('后端响应不是合法 JSON', 'contract');
  }
  assertContract(parsed);
  return parsed as T;
}

export async function del<T>(path: string): Promise<T> {
  let res: Response;
  try {
    res = await fetch(`${API_BASE}${path}`, { method: 'DELETE', headers: { Accept: 'application/json' } });
  } catch {
    throw new ContractError(`无法连接后端（${API_BASE}${path}）`, 'network');
  }
  if (!res.ok) {
    throw new ContractError(`后端返回 ${res.status}（${API_BASE}${path}）`, 'http');
  }
  let parsed: unknown;
  try {
    parsed = await res.json();
  } catch {
    throw new ContractError('后端响应不是合法 JSON', 'contract');
  }
  assertContract(parsed);
  return parsed as T;
}

/** 二进制 POST（语音转写用）：body 是原始字节，响应仍是契约 JSON */
export async function postRaw<T>(path: string, body: Blob | ArrayBuffer, contentType: string): Promise<T> {
  let res: Response;
  try {
    res = await fetch(`${API_BASE}${path}`, {
      method: 'POST',
      headers: { 'Content-Type': contentType, Accept: 'application/json' },
      body,
    });
  } catch {
    throw new ContractError(`无法连接后端（${API_BASE}${path}），后端可能未启动`, 'network');
  }
  if (!res.ok) {
    throw new ContractError(`后端返回 ${res.status}（${API_BASE}${path}）`, 'http');
  }
  let parsed: unknown;
  try {
    parsed = await res.json();
  } catch {
    throw new ContractError('后端响应不是合法 JSON', 'contract');
  }
  assertContract(parsed);
  return parsed as T;
}

export function getHealth() {
  return get<import('./contract').HealthResponse>('/health');
}

export function getStatus() {
  return get<import('./contract').StatusResponse>('/status');
}

export { API_BASE, CONTRACT_VERSION };
