import { get, post, postRaw } from './client';
import type { AgentJobResponse, AgentStartResponse } from './contract';

/** 小满 Agent：把任务交给本地模型，让它全程用工作台工具完成（可绑定会话，防串台） */

/** 附件：Electron 有真实路径（后端读盘）；纯浏览器退回文件内容（text/base64） */
export interface AgentAttachment {
  name: string;
  path?: string | null;
  size?: number;
  mime?: string;
  text?: string | null;
  base64?: string | null;
}

export function startAgentTask(task: string, sessionId?: string, attachments?: AgentAttachment[], skills?: string[], toolGroups?: { ue?: boolean }, toolNames?: string[]): Promise<AgentStartResponse> {
  return post<AgentStartResponse>('/agent/run', {
    task,
    sessionId,
    attachments: attachments?.length ? attachments : undefined,
    skills: skills ?? [],
    toolNames,
    toolGroups: toolGroups && typeof toolGroups.ue === 'boolean' ? toolGroups : undefined,
  });
}

/** 停止当前运行中的任务（2026-10-10 用户「中断任务」：立即掐断在途模型请求） */
export function stopAgentTask(): Promise<{ ok: boolean; jobId?: string; error?: string }> {
  return post<{ ok: boolean; jobId?: string; error?: string }>('/agent/stop');
}

export function getAgentJob(): Promise<AgentJobResponse> {
  return get<AgentJobResponse>('/agent/job');
}

/** 上下文用量（对标 UEBox「上下文用量」面板）：分类 token 统计 */
export interface ContextUsagePart {
  key: string;
  label: string;
  tokens: number;
  ratio: number;
}
export interface ContextUsageResponse {
  contract: string;
  ok: boolean;
  sessionId: string | null;
  contextWindow: number;
  used: number;
  ratio: number;
  retainSessions: boolean;
  historyMessages: number;
  parts: ContextUsagePart[];
}
export function getContextUsage(sessionId?: string | null, skills: string[] = [], toolGroups?: {ue?: boolean}, toolNames?: string[]): Promise<ContextUsageResponse> {
  const q = `?selection=${encodeURIComponent(JSON.stringify({skills, toolGroups, toolNames}))}` + (sessionId ? `&sessionId=${encodeURIComponent(sessionId)}` : '');
  return get<ContextUsageResponse>(`/agent/context-usage${q}`);
}

/** 语音转写（2026-10-08 用户：麦克风真功能）——本地 SenseVoice ASR，body = WAV 二进制 */
export interface AsrResponse {
  contract: string;
  ok: boolean;
  text: string;
  ms: number;
}
export function transcribeVoice(wav: Blob): Promise<AsrResponse> {
  return postRaw<AsrResponse>('/agent/asr', wav, 'audio/wav');
}
