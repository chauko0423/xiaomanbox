/**
 * 模型提供方管理（2026-10-07，DSH 图2「模型」页的底座）。
 *
 * 存储：~/.xiaomanbox/providers.json
 *   { active: "<id>", providers: [{ id, name, baseUrl, model, apiKey, builtin }] }
 *
 * 设计要点：
 *   - 种子内置 NInfer Local 小满（builtin，不可删，保证本地引擎永远可用）。
 *   - 路径在函数内现算，不在模块顶层 —— account.mjs 的教训：ESM 顶层按加载时的
 *     os.homedir() 算死路径，测试想隔离 HOME 就晚了。这里每次 load/save 现算。
 *   - apiKey 只存本地文件，list 不回显（只回 hasKey），编辑留空 = 不修改。
 *   - agent.mjs 的 run() 每次任务开始时调 activeProvider()，切换即生效（无需重启）。
 */

import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';

const SEED = {
  active: 'ninfer-local',
  supervisor: 'ninfer-local',
  providers: [
    {
      id: 'ninfer-local',
      name: 'NInfer Local 小满',
      baseUrl: 'http://127.0.0.1:8390/v1/chat/completions',
      model: 'xiaoman',
      apiKey: '',
      builtin: true,
    },
  ],
};

function fileOf() {
  return path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'providers.json');
}

function load() {
  try {
    const raw = JSON.parse(fs.readFileSync(fileOf(), 'utf8'));
    if (!raw || !Array.isArray(raw.providers)) return structuredClone(SEED);
    return raw;
  } catch {
    return structuredClone(SEED);
  }
}

function save(d) {
  fs.mkdirSync(path.dirname(fileOf()), { recursive: true });
  fs.writeFileSync(fileOf(), JSON.stringify(d, null, 2), 'utf8');
}

const URL_RE = /^https?:\/\/.+/;

/**
 * 主流模型预配置模板（2026-10-11 用户：用户只想填 API Key，不想找 URL/模型名）。
 * baseUrl 一律写完整的 /chat/completions 地址 —— 避开 normalizeModelUrl 对
 *   非 /v1 结尾（如智谱 /v4）误补 /v1/chat/completions 的坑。
 * models 数组给可选模型，model 是默认值；用户在表单里仍可改。
 */
const PRESETS = [];

/** 预设模板列表（前端「快速接入」折叠区用） */
export function listPresets() {
  return { ok: true, presets: PRESETS };
}

/** 列表（脱敏：不回显 apiKey，只回 hasKey） */
export function listProviders() {
  const d = load();
  return {
    ok: true,
    active: d.active,
    supervisor: d.supervisor || d.active,
    providers: d.providers.map((p) => ({
      id: p.id,
      name: p.name,
      baseUrl: p.baseUrl,
      model: p.model,
      builtin: !!p.builtin,
      hasKey: !!p.apiKey,
    })),
  };
}

export function addProvider({ name, baseUrl, model, apiKey } = {}) {
  if (!name || !String(name).trim()) return { ok: false, code: 400, error: '名称不能为空' };
  if (!URL_RE.test(String(baseUrl || ''))) return { ok: false, code: 400, error: 'Base URL 必须是 http(s) 地址' };
  if (!model || !String(model).trim()) return { ok: false, code: 400, error: '模型名不能为空' };
  const d = load();
  const id = `prov-${Date.now().toString(36)}`;
  d.providers.push({
    id,
    name: String(name).trim(),
    baseUrl: String(baseUrl).trim(),
    model: String(model).trim(),
    apiKey: String(apiKey || ''),
  });
  save(d);
  return { ok: true, id };
}

export function updateProvider(id, patch = {}) {
  const d = load();
  const p = d.providers.find((x) => x.id === id);
  if (!p) return { ok: false, code: 404, error: '提供方不存在' };
  // 显式校验，拒绝而非静默忽略 —— 静默忽略会让「打错的 URL 没保存」看起来像「保存成功」。
  if (patch.name !== undefined) {
    if (typeof patch.name !== 'string' || !patch.name.trim()) return { ok: false, code: 400, error: '名称不能为空' };
    p.name = patch.name.trim();
  }
  if (patch.baseUrl !== undefined) {
    if (typeof patch.baseUrl !== 'string' || !URL_RE.test(patch.baseUrl)) {
      return { ok: false, code: 400, error: 'Base URL 必须是 http(s) 地址' };
    }
    p.baseUrl = patch.baseUrl.trim();
  }
  if (patch.model !== undefined) {
    if (typeof patch.model !== 'string' || !patch.model.trim()) return { ok: false, code: 400, error: '模型名不能为空' };
    p.model = patch.model.trim();
  }
  // apiKey 语义：字段出现且为 string 才改；空串 = 清除；前端编辑时不回显，留空不发此字段
  if (typeof patch.apiKey === 'string') p.apiKey = patch.apiKey;
  save(d);
  return { ok: true };
}

export function removeProvider(id) {
  const d = load();
  const p = d.providers.find((x) => x.id === id);
  if (!p) return { ok: false, code: 404, error: '提供方不存在' };
  if (p.builtin) return { ok: false, code: 409, error: '内置提供方不可删除' };
  d.providers = d.providers.filter((x) => x.id !== id);
  if (d.active === id) d.active = d.providers[0]?.id ?? null;
  save(d);
  return { ok: true };
}

export function activateProvider(id) {
  const d = load();
  if (!d.providers.some((x) => x.id === id)) return { ok: false, code: 404, error: '提供方不存在' };
  d.active = id;
  save(d);
  return { ok: true };
}

/** agent.mjs 用：当前激活的提供方（含 apiKey），没有则 null */
export function activeProvider() {
  const d = load();
  return d.providers.find((x) => x.id === d.active) ?? null;
}

/**
 * 监工角色用的提供方（2026-10-08 用户：外部监工模型）。
 * supervisor 独立于 active —— 监工（派单/验收）可以用云端大模型，而执行的小满用 active（本地）。
 * 未显式设置 supervisor 时，回落到 active（同一模型自监自验，跑通框架）。
 */
export function supervisorProvider() {
  const d = load();
  const id = d.supervisor || d.active;
  return d.providers.find((x) => x.id === id) ?? d.providers[0] ?? null;
}

/** 设置监工角色绑定的提供方（不改变 active=小满执行者） */
export function setSupervisorProvider(id) {
  const d = load();
  if (!d.providers.some((x) => x.id === id)) return { ok: false, code: 404, error: '提供方不存在' };
  d.supervisor = id;
  save(d);
  return { ok: true, supervisor: id };
}
