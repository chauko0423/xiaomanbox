import { get, post } from './client';

/**
 * 引擎状态（W-11）—— 只读消费 8390 真实快照。
 * 驻留层（residency）现在接引擎真实 /admin/host-state（M2 手，2026-10-07）：
 * available 为 true 时携带真实驻留槽/KV/命中数据；false 时前端诚实标注「待接入」。
 */

export interface EngineProfile {
  id: string;
  label: string;
  ctx: number | null;
  ctxCap: number | null;
  ctxFloor: number | null;
  vision: boolean;
  artifact: string | null;
  note: string;
}

/** 驻留层真实快照（对应引擎 /admin/host-state） */
export interface Residency {
  available: boolean;
  slots?: { used: number; capacity: number } | null;
  kv?: { used_bytes: number; capacity_bytes: number } | null;
  dropped?: number | null;
  hits?: {
    reused_prompt_tokens: number;
    shared_stable_prefix: number;
    historical_fork_hits: number;
  } | null;
  frontier_tokens?: number | null;
  ts_ms?: number | null;
}

export interface EngineStatus {
  online: boolean;
  active: string | null;
  ctx: number | null;
  since: string | null;
  backUp: boolean;
  pid: number | null;
  profiles: EngineProfile[];
  residency: Residency;
}

export function getEngineStatus(): Promise<EngineStatus> {
  return get<EngineStatus>('/engine/status');
}

/* ── 引擎控制（2026-10-07）：启动/停止模型 + 开放/关闭局域网 API ───────── */

export interface EngineControlStatus {
  contract: string;
  ok: boolean;
  code?: string;
  error?: string;
  engineDir?: string;
  engine?: {
    running: boolean;
    port: number;
    activeProfile: string | null;
    ctx?: number | null;
    since?: string | null;
    /** 在线但默认档还在后台加载（权重载入中） */
    loading?: boolean;
  };
  openapi?: {
    running: boolean;
    port: number;
    key: string | null;
    keySet: boolean;
    /** 本机局域网接入地址（后端算好直接下发） */
    urls?: string[];
  };
}

export type EngineControlAction =
  | 'model-start'
  | 'model-stop'
  | 'openapi-start'
  | 'openapi-stop'
  | 'openapi-key-rotate';

export interface EngineControlActionResponse {
  contract: string;
  ok: boolean;
  code?: number | string;
  error?: string;
  note?: string;
  detail?: string;
  key?: string | null;
  urls?: string[];
}

export function getEngineControl(): Promise<EngineControlStatus> {
  return get<EngineControlStatus>('/engine/control');
}

export function postEngineControl(action: EngineControlAction): Promise<EngineControlActionResponse> {
  return post<EngineControlActionResponse>('/engine/control', { action });
}

/* ── 换挡（2026-10-08）：档位清单可点，走网关 /admin/switch（重载权重 10~40s）── */

export interface EngineGearResponse {
  contract: string;
  ok: boolean;
  profile?: string;
  ctx?: number | null;
  note?: string;
  error?: string;
}

export function switchEngineGear(profile: string): Promise<EngineGearResponse> {
  return post<EngineGearResponse>('/engine/gear', { profile });
}
