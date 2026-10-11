/**
 * 工作区（2026-10-08 用户）：用户在电脑内选取的工作目录清单 + 活跃工作区。
 *
 * - 工作区 = 一个本机目录（如 E:\ninfer-work、DIAOYU 工程目录），是「我在哪干活」的锚点；
 * - 清单持久化在 ~/.xiaomanbox/workspaces.json（与 ui.json/agent.json 同域，零依赖）；
 * - 由欢迎页「工作区 ▾」选择器消费；添加走 Electron selectDirectory（前端已有桥）。
 *
 * 数据形状：
 *   { active: "<path>" | null, list: [{ path, name, addedAt }] }
 *   - path 为唯一键（同目录重复添加 = 幂等，只刷新位置不新增）；
 *   - active 指向 list 中的 path；移除的工作区若是 active → active 落到 list[0] 或 null。
 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const FILE = path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'workspaces.json');

function loadAll() {
  try {
    const raw = JSON.parse(fs.readFileSync(FILE, 'utf8'));
    return {
      active: typeof raw?.active === 'string' ? raw.active : null,
      list: Array.isArray(raw?.list)
        ? raw.list
            .filter((w) => w && typeof w.path === 'string' && w.path)
            .map((w) => ({ path: w.path, name: typeof w.name === 'string' && w.name ? w.name : path.basename(w.path) || w.path, addedAt: Number(w.addedAt) || 0 }))
        : [],
    };
  } catch {
    return { active: null, list: [] };
  }
}

function saveAll(data) {
  const dir = path.dirname(FILE);
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(FILE, JSON.stringify(data, null, 2), 'utf8');
}

/** 目录名：取 basename（盘根目录如 E:\ → 盘符名） */
function dirName(p) {
  const base = path.basename(p);
  return base || p;
}

/** 清单 + 活跃工作区 */
export function listWorkspaces() {
  return { ok: true, ...loadAll() };
}

/** 当前活跃工作区路径（无则 null）。agent 注入系统提示 + 产出物归位用（2026-10-10 用户）。 */
export function getActiveWorkspace() {
  return loadAll().active;
}

/** 添加工作区（幂等：同 path 只刷新 name/addedAt）。path 必须是绝对路径。 */
export function addWorkspace(dirPath) {
  const p = String(dirPath || '').trim();
  if (!p) return { ok: false, code: 400, error: 'missing_path' };
  if (!path.isAbsolute(p)) return { ok: false, code: 400, error: 'path_not_absolute' };
  const data = loadAll();
  const existing = data.list.find((w) => w.path.toLowerCase() === p.toLowerCase());
  if (existing) {
    existing.name = dirName(p);
    existing.addedAt = Date.now();
  } else {
    data.list.push({ path: p, name: dirName(p), addedAt: Date.now() });
  }
  if (!data.active) data.active = p; // 第一个加入的默认成为活跃工作区
  saveAll(data);
  return { ok: true, ...data };
}

/** 切换活跃工作区（必须在清单里） */
export function setActiveWorkspace(dirPath) {
  const p = String(dirPath || '').trim();
  const data = loadAll();
  if (!data.list.some((w) => w.path.toLowerCase() === p.toLowerCase())) {
    return { ok: false, code: 404, error: 'workspace_not_found' };
  }
  data.active = data.list.find((w) => w.path.toLowerCase() === p.toLowerCase())?.path ?? p;
  saveAll(data);
  return { ok: true, ...data };
}

/** 移除工作区；若移除的是 active → 落到剩余第一个或 null */
export function removeWorkspace(dirPath) {
  const p = String(dirPath || '').trim();
  const data = loadAll();
  const before = data.list.length;
  data.list = data.list.filter((w) => w.path.toLowerCase() !== p.toLowerCase());
  if (data.list.length === before) return { ok: false, code: 404, error: 'workspace_not_found' };
  if (data.active && data.active.toLowerCase() === p.toLowerCase()) {
    data.active = data.list[0]?.path ?? null;
  }
  saveAll(data);
  return { ok: true, ...data };
}
