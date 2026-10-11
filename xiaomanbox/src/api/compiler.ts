import { get, post } from './client';
import type { CompilerJobResponse, CompilerStartResponse } from './contract';

/** seam.compiler 客户端：触发 UE 真编译 + 轮询任务状态 */

export function startCompileVerify(): Promise<CompilerStartResponse> {
  return post<CompilerStartResponse>('/compiler/verify');
}

export function getCompilerJob(): Promise<CompilerJobResponse> {
  return get<CompilerJobResponse>('/compiler/job');
}
