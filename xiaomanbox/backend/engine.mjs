/**
 * 引擎代理 —— 只读消费 8390 网关的真实状态（W-11）。
 *
 * 立项 §9.2 铁律：工作台**只读消费**引擎能力（档位 / 驻留 / 熵），**不改引擎**。
 * 引擎侧 admin 接口（实测）：
 *   GET /admin/status     → { active, ctx, since, back_up, pid, profiles[] }
 *   GET /admin/profiles   → { <id>: { label, artifact, ctx, vision, note, ... } }
 *   GET /admin/switch     → { ok, profile, ctx }
 *   GET /admin/host-state → 驻留层真实快照（M2 手，2026-10-07 新增）：
 *       { available, slots:{used,capacity}, kv:{used_bytes,capacity_bytes},
 *         dropped, hits:{...}, frontier_tokens, ts_ms }
 */

const ENGINE_BASE = process.env.NINFOR_ENGINE_URL ?? 'http://127.0.0.1:8390';

async function fetchEngine(pathname) {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), 2500);
  try {
    const res = await fetch(`${ENGINE_BASE}${pathname}`, { signal: ctrl.signal });
    if (!res.ok) return null;
    return await res.json();
  } catch {
    return null;
  } finally {
    clearTimeout(timer);
  }
}

/** 合并 /admin/status + /admin/profiles + /admin/host-state 成一个前端快照 */
export async function engineStatus() {
  const [status, profiles, hostState] = await Promise.all([
    fetchEngine('/admin/status'),
    fetchEngine('/admin/profiles'),
    fetchEngine('/admin/host-state'),
  ]);

  if (!status) {
    return {
      online: false,
      active: null,
      ctx: null,
      profiles: [],
      residency: { available: false },
    };
  }

  // profiles 可能是对象（{id: {...}}）或数组，归一成数组方便前端渲染
  const profileList = Object.entries(profiles ?? {}).map(([id, p]) => ({
    id,
    label: p?.label ?? id,
    ctx: p?.ctx ?? null,
    ctxCap: p?.ctx_cap ?? null,
    ctxFloor: p?.ctx_floor ?? null,
    vision: !!p?.vision,
    artifact: p?.artifact ?? null,
    note: p?.note ?? '',
  }));

  // 驻留层：host-state 就绪则透传真实快照，否则诚实标注 available:false
  const residency =
    hostState && hostState.available
      ? hostState
      : { available: false, slots: null, kv: null, dropped: null, hits: null };

  return {
    online: true,
    active: status.active ?? null,
    ctx: status.ctx ?? null,
    since: status.since ?? null,
    backUp: !!status.back_up,
    pid: status.pid ?? null,
    profiles: profileList,
    residency,
  };
}
