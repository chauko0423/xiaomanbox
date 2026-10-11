/**
 * M2b 生态 —— Skill 库（B 类资产，兼容 SKILL.md 开放标准）
 *
 * 三级加载（对齐 Agent Skills 开放标准，Anthropic 2025-12-18）：
 *   T1 · 元数据 = name + description（YAML frontmatter）→ 始终注入（≈50-100 tok/个）
 *   T2 · 正文   = SKILL.md body → 命中才加载（<5,000 tok，建议 <500 行）
 *   T3 · 资源   = scripts/references/assets → 按需（script 永不进上下文，只回传 stdout）
 *
 * 入库 lint（质量门禁，研究 238 真实 skill 后结论）：
 *   - name：小写/数字/连字符，≤64 字符，必须与父文件夹名完全一致
 *   - description：≤1,024 字符，写清「做什么」+「何时用」
 *   - frontmatter 禁用尖括号 < >（会被注入 system prompt）
 *
 * 来源：~/.xiaomanbox/skills/<name>/SKILL.md
 */

import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';

const SKILLS_DIR = path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'skills');

/** 内置技能目录：随安装包走（resources/builtin-skills），首次运行播种到用户目录 */
const BUILTIN_SKILLS_DIR = path.join(
  path.dirname(fileURLToPath(import.meta.url)),
  '..', 'builtin-skills'
);

let builtinSeeded = false;

/**
 * 内置技能播种：把安装包自带的 UE 技能拷进 ~/.xiaomanbox/skills/。
 * 只拷用户目录里**还没有**的技能 —— 用户改过/删过的不覆盖。
 * 首次 listSkills 时触发一次；新装机器由此获得全部内置技能。
 */
function ensureBuiltinSkills() {
  if (builtinSeeded) return;
  builtinSeeded = true;
  let entries = [];
  try {
    entries = fs.readdirSync(BUILTIN_SKILLS_DIR, { withFileTypes: true });
  } catch {
    return; // dev 环境目录不存在或为空，静默跳过
  }
  fs.mkdirSync(SKILLS_DIR, { recursive: true });
  for (const e of entries) {
    if (!e.isDirectory() || !NAME_RE.test(e.name)) continue;
    const dst = path.join(SKILLS_DIR, e.name);
    if (fs.existsSync(dst)) continue; // 用户已有：不覆盖
    try {
      fs.cpSync(path.join(BUILTIN_SKILLS_DIR, e.name), dst, { recursive: true });
    } catch {
      /* 单个技能播种失败不影响其余 */
    }
  }
}

/** name 校验：小写/数字/连字符，≤64 */
const NAME_RE = /^[a-z0-9-]{1,64}$/;

/** 解析 SKILL.md 的 YAML frontmatter（name/description）+ body。返回 {ok, skill} 或 {ok:false, error} */
function parseSkillMd(raw, folderName) {
  // frontmatter 必须在文件开头，--- 起止
  const m = /^---\s*\n([\s\S]*?)\n---\s*\n?([\s\S]*)$/.exec(raw);
  if (!m) {
    return { ok: false, error: '缺少 YAML frontmatter（文件开头要 --- ... ---）' };
  }
  const fm = m[1];
  const body = m[2] || '';

  // 提取 name / description（简单 YAML 键值，容忍引号）
  const nameMatch = /^name:\s*["']?([^"'\n]+)["']?\s*$/m.exec(fm);
  const descMatch = /^description:\s*["']?([\s\S]*?)["']?\s*$/m.exec(fm);
  if (!nameMatch || !nameMatch[1]?.trim()) {
    return { ok: false, error: 'frontmatter 缺 name' };
  }
  if (!descMatch || !descMatch[1]?.trim()) {
    return { ok: false, error: 'frontmatter 缺 description' };
  }

  const name = nameMatch[1].trim();
  const description = descMatch[1].trim();

  // ── lint ──
  const issues = [];
  if (!NAME_RE.test(name)) issues.push('name 只允许小写/数字/连字符，≤64 字符');
  if (name !== folderName) issues.push(`name「${name}」与文件夹名「${folderName}」不一致`);
  if (description.length > 1024) issues.push(`description 超 1024 字符（当前 ${description.length}）`);
  if (/[<>]/.test(fm)) issues.push('frontmatter 禁用尖括号 < >');
  if (body.length > 50000) issues.push('正文过长（>50,000 字符），应拆到 references/');

  const skill = { name, description, body, issues };
  return { ok: true, skill };
}

/** 扫描并解析所有 skill，返回 { ok, skills, errors } */
export function listSkills() {
  ensureBuiltinSkills(); // 首次调用时播种内置技能（新装机器由此获得 UE 技能）
  const skills = [];
  const errors = [];
  let entries = [];
  try {
    entries = fs.readdirSync(SKILLS_DIR, { withFileTypes: true });
  } catch {
    return { ok: true, skills, errors, dir: SKILLS_DIR, exists: false };
  }
  for (const e of entries) {
    if (!e.isDirectory()) continue;
    const mdPath = path.join(SKILLS_DIR, e.name, 'SKILL.md');
    try {
      const raw = fs.readFileSync(mdPath, 'utf8');
      const r = parseSkillMd(raw, e.name);
      if (r.ok) {
        // T1 元数据：只暴露 name + description（正文 T2 命中才加载）+ 中文摘要（展示用）
        skills.push({ name: r.skill.name, description: r.skill.description, titleZh: SKILL_ZH[r.skill.name] || '', issues: r.skill.issues });
      } else {
        errors.push({ skill: e.name, error: r.error });
      }
    } catch {
      errors.push({ skill: e.name, error: 'SKILL.md 不存在或不可读' });
    }
  }
  return { ok: true, skills, errors, dir: SKILLS_DIR, exists: true };
}

/** 加载单个 skill 的 T2 正文（命中才调用） */
export function loadSkill(name) {
  if (!NAME_RE.test(name)) return { ok: false, code: 400, error: '非法 skill 名' };
  const mdPath = path.join(SKILLS_DIR, name, 'SKILL.md');
  try {
    const raw = fs.readFileSync(mdPath, 'utf8');
    const r = parseSkillMd(raw, name);
    if (!r.ok) return { ok: false, code: 500, error: r.error };
    return { ok: true, skill: { name: r.skill.name, description: r.skill.description, body: r.skill.body } };
  } catch {
    return { ok: false, code: 404, error: `skill「${name}」不存在` };
  }
}

/** 入库：安装一个 skill（写 SKILL.md 到 ~/.xiaomanbox/skills/<name>/）。先 lint。 */
export function installSkill(name, content) {
  if (!name || !NAME_RE.test(name)) {
    return { ok: false, code: 400, error: 'skill 名只允许小写/数字/连字符，≤64 字符' };
  }
  const r = parseSkillMd(content, name);
  if (!r.ok) return { ok: false, code: 400, error: r.error };
  if (r.skill.issues.length) {
    return { ok: false, code: 422, error: 'lint 未通过：' + r.skill.issues.join('；'), issues: r.skill.issues };
  }
  const dir = path.join(SKILLS_DIR, name);
  try {
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(path.join(dir, 'SKILL.md'), content, 'utf8');
  } catch (e) {
    return { ok: false, code: 500, error: String(e.message || e) };
  }
  return { ok: true, name, path: path.join(dir, 'SKILL.md') };
}

/** T1 元数据清单（给模型注入用）。
 *  2026-10-09 用户「发任务到 GPU 开算慢 12s」优化：原来注入完整 description（20 个 skill ≈3951 token），
 *  让 4090 的 prefill 从 2.8s 拖到 12s。改用「name + 中文一句话摘要」（SKILL_ZH），降到 ~200 token；
 *  正文仍 T2 命中时用 workbench_load_skill 按需加载（不损失能力）。
 *  @param names 可选白名单：只注入这些 skill；不传 = 全部。 */
export function modelSkillSummaries(names) {
  const { skills } = listSkills();
  const list = names ? skills.filter((s) => names.includes(s.name)) : skills;
  return list
    .map((s) => `${s.name}: ${SKILL_ZH[s.name] || s.description.split(/[.!?]\s/)[0]}`)
    .join('\n');
}

// ── 内置 UE skill 的中文摘要（2026-10-09：用户看不懂英文 description，给中文一句话说明）──
// key = skill 名，值 = 中文一句话摘要。第三方自定义 skill 不在此表，前端回退显示英文首句。
const SKILL_ZH = {
  'ue-actor-placement': '在关卡里摆放、查询、移动、改参数或批量删除 Actor',
  'ue-ai-render-from-blockout': '把灰盒白模用 AI 生成概念图/效果图（拍屏喂给生图）',
  'ue-animation-retargeting': '把动画在不同骨骼间重定向，量姿态、预览结果',
  'ue-blueprint-graph-editing': '在已有蓝图里写逻辑：加节点、连线、设引脚默认值、修编译错',
  'ue-blueprint-move': '实现 Actor 往复移动（Timeline + 乘法 + SetActorLocation）',
  'ue-content-import-organize': '导入外部文件、整理/批量改名资源、查依赖、迁到别的工程',
  'ue-cpp-workflow': '写/加/编译 UE 的 C++ 代码，并热重载进正在跑的编辑器',
  'ue-editor-inspection': '看视口、读工程设置与 .uproject、查用到的插件/模块',
  'ue-geometry-editing': '用 Geometry Script 直接改网格：布尔、切面、减面、补洞、生成碰撞',
  'ue-landscape': '创建地形（平地或高度图）并配 RVT，诊断 RVT 不显示的问题',
  'ue-level-organize': '把关卡里的 Actor 归档到 World Outliner 文件夹',
  'ue-material-authoring': '做母材质、连线编译、建实例、诊断材质渲染/编译问题',
  'ue-material-substrate': '做 Substrate 材质（车漆/玻璃/清漆/分层等，UE 5.4+）',
  'ue-pcg-scattering': '建并调试 PCG 图，在地形上程序化散布树/石/草',
  'ue-project-audit': '审计工程/关卡的大小、性能与资源问题，出带修复建议的排名报告',
  'ue-sequencer': '写关键帧做运镜（环绕/推拉/跟拍等），诊断序列渲染黑屏等问题',
  'ue-skill-creator': '把验证过的经验沉淀成可复用的 skill/脚本，或修错的 skill',
  'ue-system-diagnostics': '在编辑器里跑控制台命令和 Python、查性能/崩溃/缓存/插件',
  'ue-widget-umg-layout': '建 UMG 控件蓝图（HUD/菜单/游戏 UI），排版、设值、预览',
};

/** 删除一个 skill（删整个 ~/.xiaomanbox/skills/<name>/ 目录） */
export function deleteSkill(name) {
  if (!name || !NAME_RE.test(name)) {
    return { ok: false, code: 400, error: 'skill 名只允许小写/数字/连字符，≤64 字符' };
  }
  const dir = path.join(SKILLS_DIR, name);
  try {
    if (!fs.existsSync(dir)) return { ok: false, code: 404, error: `skill「${name}」不存在` };
    fs.rmSync(dir, { recursive: true, force: true });
  } catch (e) {
    return { ok: false, code: 500, error: String(e.message || e) };
  }
  return { ok: true, name, removed: true };
}

export { parseSkillMd as _parseSkillMd, SKILL_ZH as _skillZh };
