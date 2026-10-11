import { API_BASE, assertContract } from './contract';
import type { ContractError } from './contract';

export interface TeamBoardStatus {
  contract: string;
  mode?: 'public' | 'local';
  publicHost?: string;
  running: boolean;
  wsUp: boolean;
  webUp: boolean;
  wsPort: number;
  webPort: number;
  room: string;
  url: string;
  startedAt: number | null;
}

export interface TeamBoardStartResponse {
  contract: string;
  ok: boolean;
  alreadyRunning?: boolean;
  url?: string;
  wsPort?: number;
  webPort?: number;
  room?: string;
  wsUp?: boolean;
  webUp?: boolean;
  startedAt?: number | null;
  code?: number;
  error?: string;
}

/** 团队画板：查状态 */
export async function getTeamBoardStatus(): Promise<TeamBoardStatus> {
  const res = await fetch(`${API_BASE}/teamboard/status`, { headers: { Accept: 'application/json' } });
  if (!res.ok) throw new Error(`teamboard/status HTTP ${res.status}`);
  const parsed = await res.json();
  assertContract(parsed);
  return parsed as TeamBoardStatus;
}

/** 团队画板：一键启动 */
export async function startTeamBoard(): Promise<TeamBoardStartResponse> {
  const res = await fetch(`${API_BASE}/teamboard/start`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', Accept: 'application/json' },
    body: '{}',
  });
  if (!res.ok) throw new Error(`teamboard/start HTTP ${res.status}`);
  const parsed = await res.json();
  assertContract(parsed);
  return parsed as TeamBoardStartResponse;
}

/** 团队画板：停止 */
export async function stopTeamBoard(): Promise<{ contract: string; ok: boolean }> {
  const res = await fetch(`${API_BASE}/teamboard/stop`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', Accept: 'application/json' },
    body: '{}',
  });
  if (!res.ok) throw new Error(`teamboard/stop HTTP ${res.status}`);
  const parsed = await res.json();
  assertContract(parsed);
  return parsed as { contract: string; ok: boolean };
}

export type { ContractError };

/** 现铸全新 SSO URL（面板打开时取一次；令牌不过期、始终对应当前账号） */
export interface TeamBoardUrlResponse {
  contract: string;
  mode: 'public' | 'local';
  url: string;
  room: string;
}

export async function fetchTeamBoardUrl(): Promise<TeamBoardUrlResponse> {
  const res = await fetch(`${API_BASE}/teamboard/url`, { headers: { Accept: 'application/json' } });
  if (!res.ok) throw new Error(`teamboard/url HTTP ${res.status}`);
  const parsed = await res.json();
  assertContract(parsed);
  return parsed as TeamBoardUrlResponse;
}
