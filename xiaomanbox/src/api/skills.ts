import { get, del } from './client';

/**
 * M2b 生态 —— skill 清单客户端。
 * 斜杠指令面板用 listSkills 拉真实 skill 清单（T1 元数据 name+description），
 * 点选后填入结构化任务描述；正文（T2）由后端 agent 命中时再 workbench_load_skill 加载。
 */

export interface SkillMeta {
  name: string;
  description: string;
  /** 中文一句话摘要（内置 skill 有；第三方 skill 为空，前端回退英文首句） */
  titleZh?: string;
  issues?: string[];
}

export interface SkillListResponse {
  contract: string;
  ok: boolean;
  skills: SkillMeta[];
  errors?: { skill: string; error: string }[];
  dir?: string;
  exists?: boolean;
}

export interface SkillBody {
  name: string;
  description: string;
  body: string;
}

export interface SkillLoadResponse {
  contract: string;
  ok: boolean;
  skill?: SkillBody;
  code?: number;
  error?: string;
}

export function listSkills(): Promise<SkillListResponse> {
  return get<SkillListResponse>('/skills');
}

export function loadSkill(name: string): Promise<SkillLoadResponse> {
  return get<SkillLoadResponse>(`/skills/${name}`);
}

/** 删除一个 skill（删整个 skill 目录，不可恢复） */
export function deleteSkill(name: string): Promise<{ ok: boolean; name?: string; code?: number; error?: string }> {
  return del(`/skills/${encodeURIComponent(name)}`);
}
