/**
 * W-78 全局搜索 —— 一处搜全仓（会话 / 工具 / skill）
 * 会话 = session.messages；工具 = MCP 服务器列表 + UEBox 工具清单；skill = SKILL.md 元数据。
 */

import * as session from './session.mjs';
import * as skills from './skills.mjs';
import * as mcp from './mcp.mjs';
import { ueboxToolsCached } from './uebox.mjs';

export async function search(q) {
  if (!q || !q.trim()) return { ok: false, code: 400, error: 'q 不能为空' };
  const term = q.trim().toLowerCase();
  const out = { ok: true, query: q, sessions: [], tools: [], skills: [] };

  // 1. 会话 messages
  for (const s of session.listSessions()) {
    const msgs = session.getMessages(s.sessionId);
    const hits = msgs.filter((m) => (m.content || '').toLowerCase().includes(term));
    if (hits.length) {
      out.sessions.push({
        sessionId: s.sessionId,
        title: s.sessionTitle,
        pinTarget: s.pinTarget,
        hitCount: hits.length,
        hits: hits.slice(0, 5).map((h) => h.content.slice(0, 120)),
      });
    }
  }

  // 2. skill（name + description）
  const { skills: skillList } = skills.listSkills();
  out.skills = skillList
    .filter((s) => s.name.toLowerCase().includes(term) || s.description.toLowerCase().includes(term))
    .map((s) => ({ name: s.name, description: s.description }));

  // 3. 工具：MCP 服务器 + UEBox 工具
  out.tools.push(
    ...mcp
      .listServers()
      .filter((s) => s.name.toLowerCase().includes(term))
      .map((s) => ({ source: 'mcp-server', name: s.name, transport: s.transport }))
  );
  const ub = ueboxToolsCached();
  if (Array.isArray(ub?.tools)) {
    out.tools.push(
      ...ub.tools
        .filter((t) => t.name.toLowerCase().includes(term) || (t.brief || '').toLowerCase().includes(term))
        .slice(0, 20)
        .map((t) => ({ source: 'uebox', name: t.name, brief: t.brief, risk: t.risk }))
    );
  }
  // UEBox 工具缓存未命中（离线/过期/未配置）时，明确告知前端，而不是静默漏掉
  out.ueboxSkipped = ub?.stale === true;

  out.counts = { sessions: out.sessions.length, tools: out.tools.length, skills: out.skills.length };
  return out;
}
