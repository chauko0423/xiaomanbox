import { get, post } from './client';

/**
 * 外部监工（2026-10-08 用户）—— 多模型主从编排。
 * 监工模型（providers 里任意一个）派单 + 验收，小满（active）执行。
 */

export interface AcceptMode {
  id: string;
  name: string;
  desc: string;
}

export interface SupervisorRound {
  round: number;
  task: string;
  state: string;
  result: string | null;
  verdict: string | null;
  reason: string;
}

export interface SupervisorJob {
  id: string;
  goal: string;
  acceptMode: string;
  maxRounds: number;
  supervisor: string;
  executor: string;
  state: 'running' | 'done' | 'error';
  round: number;
  plan: { task: string; criteria: string[] } | null;
  verdict: { verdict: string; reason: string; rounds: number } | null;
  error: string | null;
  rounds: SupervisorRound[];
}

export interface SupervisorStatusResponse {
  contract: string;
  ok: boolean;
  acceptModes: AcceptMode[];
  job: SupervisorJob | null;
}

export interface SupervisorRunResponse {
  contract: string;
  ok: boolean;
  jobId?: string;
  supervisor?: string;
  executor?: string;
  acceptMode?: string;
  code?: number;
  error?: string;
}

export function getSupervisorStatus(): Promise<SupervisorStatusResponse> {
  return get<SupervisorStatusResponse>('/supervisor/status');
}

export function runSupervisor(
  goal: string,
  opts: { acceptMode?: string; maxRounds?: number } = {},
): Promise<SupervisorRunResponse> {
  return post<SupervisorRunResponse>('/supervisor/run', { goal, ...opts });
}

/* ── 外部监工接入（GPT / WorkBuddy 等外部 AI 当监工）────────── */

export interface SupervisorSkillResponse {
  contract: string;
  ok: boolean;
  skill: string;
}

/** 拉取「外部监工接入规范」全文（用户复制给 GPT/WorkBuddy） */
export function getSupervisorSkill(): Promise<SupervisorSkillResponse> {
  return get<SupervisorSkillResponse>('/supervisor/skill');
}
