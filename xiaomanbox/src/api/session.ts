import { get, post, put, del } from './client';

/**
 * M1 魂 —— 会话绑定客户端。
 * 会话三字段：sessionId / sessionTitle / pinTarget；场景 ↔ 会话绑定可查可改。
 */

export interface SessionMessage {
  role: string;
  content: string;
}

export interface Session {
  sessionId: string;
  sessionTitle: string;
  pinTarget: string;
  messages?: SessionMessage[];
  createdAt?: string;
  updatedAt?: string;
  messageCount?: number;
}

export interface SessionSummary {
  sessionId: string;
  sessionTitle: string;
  pinTarget: string;
  createdAt?: string;
  updatedAt?: string;
  messageCount?: number;
  archived?: boolean;
  /** 置顶（侧栏会话排序置前） */
  pinned?: boolean;
  /** 该会话是否有 agent 任务正在跑（侧栏画转圈） */
  running?: boolean;
}

/** 获取（或创建）某场景绑定的会话 —— 切场景 = 切会话 */
export function getSceneSession(sceneId: string): Promise<{ session: Session }> {
  return get<{ session: Session }>(`/session/scene/${sceneId}`);
}

export function listSessions(): Promise<{ sessions: SessionSummary[] }> {
  return get<{ sessions: SessionSummary[] }>('/sessions');
}

/** 新建会话（新任务自动创建会话） */
export function createSession(body: { pinTarget?: string; sessionTitle?: string } = {}): Promise<{ session: Session }> {
  return post<{ session: Session }>('/session', body);
}

export function getSessionMessages(sessionId: string): Promise<{ sessionId: string; messages: SessionMessage[] }> {
  return get<{ sessionId: string; messages: SessionMessage[] }>(`/session/${sessionId}/messages`);
}

/** 改绑定：标题 / pinTarget / 置顶 */
export function updateSession(sessionId: string, body: { sessionTitle?: string; pinTarget?: string; pinned?: boolean }): Promise<{ session: Session }> {
  return put<{ session: Session }>(`/session/${sessionId}`, body);
}

/** 把场景绑到指定会话（防串台负控用） */
export function bindScene(sceneId: string, sessionId: string): Promise<{ ok: boolean; sceneId: string; sessionId: string }> {
  return put<{ ok: boolean; sceneId: string; sessionId: string }>(`/session/scene/${sceneId}/bind`, { sessionId });
}

/** 归档 / 取消归档会话（不删数据） */
export function archiveSession(sessionId: string, archived: boolean): Promise<{ session: Session }> {
  return put<{ session: Session }>(`/session/${sessionId}/archive`, { archived });
}

/** 删除会话（连同消息记录与场景绑定，不可恢复；归档才是可恢复的收纳）。运行中的会话后端会拒绝（409） */
export function deleteSession(sessionId: string): Promise<{ ok: boolean; deleted?: string }> {
  return del<{ ok: boolean; deleted?: string }>(`/session/${sessionId}`);
}
