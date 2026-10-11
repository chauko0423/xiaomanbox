/**
 * UE5 工程选取 + 打开 —— 「选工程项目 → 小满打开所选工程」
 *
 * 相比旧的「一键建 Xiaoman 工程 + 建蓝图」，改为更贴合日常工作的两步：
 *   ① 扫描本地目录，列出所有 .uproject 工程（零依赖，纯 node fs）
 *   ② 点选一个工程 → 直接拉起 UE 编辑器打开它（走 vault.project_manage.open_project）
 *   ③ 等编辑器握手（UnrealAgentLink 插件连上工作台 UAL WS 服务端）
 *
 * 状态机：idle → running → done / failed；单任务互斥（409）。
 * 配置：~/.xiaomanbox/ue5boot.json 可覆盖 scanDirs（扫描根目录数组）与当前选中工程。
 */

import fs from 'node:fs';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
import os from 'node:os';
import { spawn, execFile } from 'node:child_process';
import * as vault from './vault.mjs';

const DEFAULTS = {
  // 扫描工程根目录（可多个，扫描每个目录下一层的 *.uproject）
  scanDirs: [],
  connectTimeoutMs: 360_000, // 等编辑器握手总上限（首次启动编辑器+加载工程较慢）
};

function configPath() {
  return path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'ue5boot.json');
}

function loadConfig() {
  const cfg = { ...DEFAULTS, scanDirs: [...DEFAULTS.scanDirs] };
  try {
    const raw = JSON.parse(fs.readFileSync(configPath(), 'utf8'));
    if (Array.isArray(raw?.scanDirs)) cfg.scanDirs = raw.scanDirs.filter((d) => typeof d === 'string' && d.trim());
    if (typeof raw?.connectTimeoutMs === 'number') cfg.connectTimeoutMs = raw.connectTimeoutMs;
  } catch {
    /* 无配置 = 默认值 */
  }
  return cfg;
}

function saveConfig(cfg) {
  const dir = path.dirname(configPath());
  try {
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(configPath(), JSON.stringify({ scanDirs: cfg.scanDirs, connectTimeoutMs: cfg.connectTimeoutMs }, null, 2), 'utf8');
    return true;
  } catch {
    return false;
  }
}

let job = null;
let selectedProjectPath = null; // 当前选中的工程（本次进程内记忆）

function shortErr(e) {
  return String(e?.message || e).slice(0, 300);
}

/**
 * 确保工程里装了 UnrealAgentLink 插件（没有则装 + 在 .uproject 启用）。
 * 复刻 UEBox「open_project 前自动装插件」的行为 —— 插件不装，编辑器起也不会连 17860。
 * 插件源查找顺序：UAL_PLUGIN_SRC 环境变量 → 工作台发行版（带编译 DLL）→ fork 源码（兜底）。
 */
function ensurePlugin(uproject) {
  const projDir = path.dirname(uproject);
  const dest = path.join(projDir, 'Plugins', 'UnrealAgentLink');
  const destMarker = path.join(dest, 'UnrealAgentLink.uplugin');

  // 已装 → 只确保 .uproject 里启用
  if (fs.existsSync(destMarker)) {
    const enabled = enablePluginInUproject(uproject);
    return { already: true, installed: false, uprojectTouched: enabled };
  }

  // 找插件源（发行版优先：带 Binaries 编译产物，编辑器直接加载不用编译）
  const backendDir = path.dirname(fileURLToPath(import.meta.url));
  const workbenchRoot = path.resolve(backendDir, '..');
  const candidates = [
    process.env.UAL_PLUGIN_SRC,
    path.join(workbenchRoot, 'plugin', 'UnrealAgentLink-dist'),
    path.join(workbenchRoot, 'plugin', 'UnrealAgentLink'),
  ].filter(Boolean);
  const src = candidates.find((c) => fs.existsSync(path.join(c, 'UnrealAgentLink.uplugin')));
  if (!src) {
    return { installed: false, already: false, error: `找不到插件源（查过：${candidates.join('、')}）` };
  }

  try {
    fs.cpSync(src, dest, { recursive: true });
    const uprojectTouched = enablePluginInUproject(uproject);
    return { installed: true, already: false, source: src, uprojectTouched };
  } catch (e) {
    return { installed: false, already: false, error: `插件拷贝失败：${shortErr(e)}` };
  }
}

/** 在 .uproject 的 Plugins 数组里启用 UnrealAgentLink（已启用则不动）。返回是否修改了文件。 */
function enablePluginInUproject(uproject) {
  let j;
  try {
    j = JSON.parse(fs.readFileSync(uproject, 'utf8'));
  } catch (e) {
    return false; // 解析失败不动文件（插件目录在，引擎也可能扫描到）
  }
  if (!Array.isArray(j.Plugins)) j.Plugins = [];
  if (j.Plugins.some((p) => p?.Name === 'UnrealAgentLink')) return false;
  j.Plugins.push({ Name: 'UnrealAgentLink', Enabled: true });
  try {
    fs.writeFileSync(uproject, JSON.stringify(j, null, '\t') + '\n', 'utf8');
    return true;
  } catch {
    return false;
  }
}

/**
 * 扫描工程目录，返回所有 .uproject 工程。
 * 纯本地 fs 扫描，零依赖、离线可用，不依赖编辑器或插件在线。
 *
 * 兼容两种常见工程目录结构（用户实测踩坑）：
 *   ① 目录/工程名/工程名.uproject   —— 如 D:\UE5_7_4\Projects\Xiaoman\Xiaoman.uproject
 *   ② 目录/工程名.uproject           —— 如 D:\UGit\DIAOYU\DIAOYU.uproject（.uproject 直接在根下）
 */
export function listProjects() {
  const cfg = loadConfig();
  const dirs = cfg.scanDirs.filter(Boolean);
  const projects = [];
  const seen = new Set();
  const push = (up, dir) => {
    if (seen.has(up)) return;
    seen.add(up);
    projects.push({ name: path.basename(up).replace(/\.uproject$/, ''), path: up, dir });
  };
  for (const dir of dirs) {
    try {
      const entries = fs.readdirSync(dir, { withFileTypes: true });
      for (const e of entries) {
        // 结构②：.uproject 直接躺在扫描目录根下
        if (e.isFile() && e.name.endsWith('.uproject')) {
          push(path.join(dir, e.name), dir);
          continue;
        }
        // 结构①：进入下一层子目录找 .uproject
        if (!e.isDirectory()) continue;
        const inner = path.join(dir, e.name);
        try {
          for (const f of fs.readdirSync(inner)) {
            if (f.endsWith('.uproject')) push(path.join(inner, f), inner);
          }
        } catch {
          /* 无权限 */
        }
      }
    } catch {
      /* 目录不存在 */
    }
  }
  projects.sort((a, b) => a.name.localeCompare(b.name, 'zh-CN'));
  return { contract: 'v1', ok: true, scanDirs: dirs, projects, selectedPath: selectedProjectPath };
}

/** 添加一个扫描目录（持久化到 ~/.xiaomanbox/ue5boot.json）。 */
export function addScanDir(dir) {
  if (!dir || typeof dir !== 'string' || !dir.trim()) {
    return { contract: 'v1', ok: false, code: 400, error: '缺少目录路径' };
  }
  const clean = dir.trim();
  if (!fs.existsSync(clean) || !fs.statSync(clean).isDirectory()) {
    return { contract: 'v1', ok: false, code: 400, error: `目录不存在或不是文件夹：${clean}` };
  }
  const cfg = loadConfig();
  const norm = path.resolve(clean);
  if (cfg.scanDirs.some((d) => path.resolve(d) === norm)) {
    return { contract: 'v1', ok: false, code: 409, error: '该目录已在扫描列表里' };
  }
  cfg.scanDirs.push(norm);
  const saved = saveConfig(cfg);
  return { contract: 'v1', ok: saved, code: saved ? 0 : 500, error: saved ? undefined : '配置写入失败', scanDirs: cfg.scanDirs };
}

/** 删除一个扫描目录（持久化）。 */
export function removeScanDir(dir) {
  if (!dir || typeof dir !== 'string') {
    return { contract: 'v1', ok: false, code: 400, error: '缺少目录路径' };
  }
  const cfg = loadConfig();
  const norm = path.resolve(dir.trim());
  const before = cfg.scanDirs.length;
  cfg.scanDirs = cfg.scanDirs.filter((d) => path.resolve(d) !== norm);
  if (cfg.scanDirs.length === before) {
    return { contract: 'v1', ok: false, code: 404, error: '该目录不在扫描列表里' };
  }
  const saved = saveConfig(cfg);
  return { contract: 'v1', ok: saved, code: saved ? 0 : 500, error: saved ? undefined : '配置写入失败', scanDirs: cfg.scanDirs };
}

/** 选定一个工程（进程内记忆）。返回选中的工程信息。 */
export function selectProject(projectPath) {
  if (!projectPath || typeof projectPath !== 'string') {
    return { contract: 'v1', ok: false, code: 400, error: '缺少 projectPath' };
  }
  if (!fs.existsSync(projectPath) || !projectPath.endsWith('.uproject')) {
    return { contract: 'v1', ok: false, code: 400, error: `工程文件不存在或不是 .uproject：${projectPath}` };
  }
  selectedProjectPath = projectPath;
  return { contract: 'v1', ok: true, projectPath, name: path.basename(projectPath).replace(/\.uproject$/, '') };
}

/**
 * 「流程进行中」409 的载荷构造（纯函数，导出供负控测试）。
 *
 * 为什么 409 要带这么多信息：模型收到裸的「已有打开流程在进行中」时无从判断
 * 该等还是该重试，只会盲目再发（2026-10-08 蓝图任务里连吃三个 409 空转）。
 * 带上已等时长 / 上限 / 当前步骤后，模型能读懂「这是正常等待期」，
 * 转去 workbench_ue5_status 轮询而不是反复 launch。
 *
 * @returns {{expired:true, limitSec:number} | {expired:false, payload:object}}
 *   expired=true 表示 running 已超窗（握手窗口+缓冲），调用方应把 job 置 failed 并放行新流程。
 */
export function runningGuardPayload(jobLike, nowMs, limitSec, hint) {
  const startedMs = new Date(jobLike.startedAt).getTime();
  const elapsedSec = Math.max(0, Math.round((nowMs - startedMs) / 1000));
  if (elapsedSec > limitSec) {
    return { expired: true, limitSec };
  }
  const steps = jobLike.steps || [];
  const cur = steps.find((s) => s.state === 'running') || steps[steps.length - 1];
  return {
    expired: false,
    payload: {
      ok: false,
      code: 409,
      error:
        `已有打开流程在进行中（已进行 ${elapsedSec}s / 上限 ${limitSec}s，当前步骤：${cur ? `${cur.label}［${cur.state}］` : '…'}）。${hint}`,
      jobId: jobLike.id,
      jobState: 'running',
      elapsedSec,
      limitSec,
      currentStep: cur ? { key: cur.key, label: cur.label, state: cur.state, detail: cur.detail || '' } : null,
      startedAt: jobLike.startedAt,
      projectPath: jobLike.projectPath,
    },
  };
}

/** 守门用上限：握手窗口（connectTimeoutMs）+ 60s 缓冲。 */
function guardLimitSec() {
  return Math.round((loadConfig().connectTimeoutMs + 60_000) / 1000);
}

/**
 * 「流程进行中」守门：running 未超窗 → 返回 409 富信息载荷（调用方直接 return）；
 * 已超窗 → 把 job 置 failed（自动解锁）并返回 null 放行新流程。
 * @param {string} hint 针对调用方动作的提示语（launch/switch/close 各不同）
 */
function runningGuard(hint) {
  if (!job || job.state !== 'running') return null;
  const r = runningGuardPayload(job, Date.now(), guardLimitSec(), hint);
  if (r.expired) {
    job.state = 'failed';
    job.error = `打开流程超过 ${r.limitSec}s 未完成，已自动解锁（检查编辑器是否弹了版本升级/缺模块对话框，或 HTTP_PROXY 劫持了插件连接）`;
    return null;
  }
  return r.payload;
}

/** 打开选中/指定工程：拉起编辑器 + 等握手。异步编排，立即返回 jobId。 */
export async function openProject(projectPath) {
  const guard = runningGuard(
    '这是正常等待期，不要重复发起打开：编辑器首次启动+加载工程可能需要几分钟。用 workbench_ue5_status 查实时进度，等 state=done 且引擎已连接再继续后续操作'
  );
  if (guard) return guard;
  const target = projectPath || selectedProjectPath;
  if (!target) {
    return { ok: false, code: 400, error: '尚未选定工程，请先选择一个工程' };
  }
  if (!fs.existsSync(target)) {
    return { ok: false, code: 404, error: `工程文件不存在：${target}` };
  }

  // 编辑器进程已在跑 → 不要重复拉起（否则会出现两个编辑器进程抢同一工程、半死连接残留）。
  // 历史教训：模型在编辑器半死时调 bootstrap 又拉起一个新编辑器，造成进程混乱。
  if (await editorRunning()) {
    const { ualStatus, isConnectionStale } = await import('./ual.mjs');
    const st = ualStatus();
    const healthy = (st.connections || []).some((c) => !isConnectionStale({ lastHeartbeat: c.lastHeartbeat }));
    if (healthy) {
      return {
        ok: true,
        alreadyRunning: true,
        connected: true,
        projectPath: target,
        note: '编辑器已在运行且连接健康，无需重新打开，可直接进行蓝图/资产操作',
      };
    }
    return {
      ok: false,
      code: 409,
      error:
        '编辑器进程已在运行，但与工作台的连接已中断（心跳停滞）。不要重复打开（会产生两个编辑器进程抢工程）。请先用 workbench_ue5_close 关闭旧编辑器，再重新打开',
    };
  }

  selectedProjectPath = target;

  const cfg = loadConfig();
  job = {
    id: `UE5-${Date.now()}`,
    state: 'running',
    startedAt: new Date().toISOString(),
    projectPath: target,
    projectName: path.basename(target).replace(/\.uproject$/, ''),
    steps: [],
    error: null,
  };
  runOpen(cfg, target).catch((e) => {
    job.state = 'failed';
    job.error = shortErr(e);
  });
  return { ok: true, jobId: job.id, projectPath: target };
}

/** 检测 UnrealEditor 进程是否在跑。 */
function editorRunning() {
  return new Promise((resolve) => {
    execFile('tasklist', ['/FI', 'IMAGENAME eq UnrealEditor.exe', '/FO', 'CSV', '/NH'], { windowsHide: true }, (err, stdout) => {
      if (err) return resolve(false);
      resolve(/UnrealEditor\.exe/i.test(stdout || ''));
    });
  });
}

/** 检测崩溃报告客户端（CrashReportClientEditor.exe）是否在跑 —— 编辑器崩溃后它会弹出收集信息。 */
function crashReporterRunning() {
  return new Promise((resolve) => {
    execFile('tasklist', ['/FI', 'IMAGENAME eq CrashReportClientEditor.exe', '/FO', 'CSV', '/NH'], { windowsHide: true }, (err, stdout) => {
      if (err) return resolve(false);
      resolve(/CrashReportClientEditor\.exe/i.test(stdout || ''));
    });
  });
}

/**
 * 编辑器健康三态检测（进程级，与插件心跳互补）。
 *  - running：编辑器进程在跑（正常）
 *  - crashed：编辑器进程消失 且 崩溃报告客户端在跑 → 编辑器刚崩溃
 *  - closed：两者都不在 → 编辑器已关闭/未启动
 * 崩溃是「进程消失 + CrashReportClient 出现」的组合信号，比只看进程更准（半死/假死场景下进程可能还在）。
 */
export async function editorHealth() {
  const [running, crashedReporter] = await Promise.all([editorRunning(), crashReporterRunning()]);
  if (running) return { state: 'running', editorRunning: true, crashReporter: crashedReporter };
  if (crashedReporter) return { state: 'crashed', editorRunning: false, crashReporter: true };
  return { state: 'closed', editorRunning: false, crashReporter: false };
}

/**
 * 若编辑器已连接（插件在线），先保存全部未保存改动（editor.save scope=all）。
 * 返回 { saved, note }。没连接/保存失败不阻断关闭（返回 saved:false + 原因）。
 */
async function saveAllIfConnected() {
  try {
    const { ualStatus, ualCall, isConnectionStale } = await import('./ual.mjs');
    const st = ualStatus();
    // 只看「心跳健康的连接」：半死连接（connected:true 但心跳停）不尝试保存，避免打到死 socket 上干等超时。
    const healthy = (st.connections || []).filter((c) => !isConnectionStale({ lastHeartbeat: c.lastHeartbeat }));
    if (!healthy.length) {
      const stale = (st.connections || []).some((c) => c.connected);
      return stale
        ? { saved: false, note: '编辑器连接已中断（心跳停滞），跳过自动保存' }
        : { saved: false, note: '编辑器未连接，跳过自动保存' };
    }
    await ualCall('editor.save', { scope: 'all' }, 30_000);
    return { saved: true, note: '已保存全部未保存改动' };
  } catch (e) {
    return { saved: false, note: `自动保存未完成：${shortErr(e)}` };
  }
}

/** 关闭 UnrealEditor：先优雅 taskkill（等 8s），仍在跑则 /F 强杀。返回 { ok, killed, forced, error }。 */
async function killEditor(graceMs = 8000) {
  const running = await editorRunning();
  if (!running) return { ok: true, killed: false, forced: false, error: null, note: '编辑器本来就没在跑' };

  const exec = (args) =>
    new Promise((resolve) => {
      execFile('taskkill', args, { windowsHide: true }, (err, stdout, stderr) => resolve({ err, stdout, stderr }));
    });

  // 优雅关闭（不带 /F，给 UE 保存/提示的机会）
  await exec(['/IM', 'UnrealEditor.exe']);
  const deadline = Date.now() + graceMs;
  while (Date.now() < deadline) {
    if (!(await editorRunning())) return { ok: true, killed: true, forced: false, error: null };
    await new Promise((r) => setTimeout(r, 500));
  }

  // 兜底强杀
  await exec(['/F', '/IM', 'UnrealEditor.exe']);
  const stillRunning = await editorRunning();
  if (stillRunning) {
    return { ok: false, killed: false, forced: true, error: '强制关闭后进程仍在，可能需要手动关闭' };
  }
  return { ok: true, killed: true, forced: true, error: null };
}

/** 一键关闭工程：先自动保存（若已连接）→ 杀掉 UnrealEditor 进程。 */
export async function closeProject() {
  const guard = runningGuard('等打开/切换流程结束再关闭（用 workbench_ue5_status 查进度）');
  if (guard) return guard;
  const save = await saveAllIfConnected();
  const r = await killEditor();
  if (r.ok) {
    job = null;
    return {
      contract: 'v1',
      ok: true,
      killed: r.killed,
      forced: r.forced,
      saved: save.saved,
      saveNote: save.note,
      note: r.killed ? (r.forced ? '编辑器已强制关闭' : '编辑器已关闭') : '编辑器本来就没在跑',
    };
  }
  return { contract: 'v1', ok: false, code: 500, error: r.error, saveNote: save.note };
}

/** 一键切换工程：关闭当前编辑器 → 打开目标工程。UE 无热切换，本质是关旧开新。 */
export async function switchProject(projectPath) {
  const guard = runningGuard('等当前流程结束再切换（用 workbench_ue5_status 查进度）');
  if (guard) return guard;
  const target = projectPath || selectedProjectPath;
  if (!target) {
    return { ok: false, code: 400, error: '尚未选定工程，请先选择一个工程' };
  }
  if (!fs.existsSync(target)) {
    return { ok: false, code: 404, error: `工程文件不存在：${target}` };
  }

  // 先自动保存（若已连接），再关旧编辑器
  const save = await saveAllIfConnected();
  const kill = await killEditor();

  // 再打开目标工程（复用 openProject 的编排：装插件→拉起→握手）
  selectedProjectPath = target;
  const opened = openProject(target);
  if (!opened.ok) {
    return { ok: false, code: opened.code, error: `切换打开失败：${opened.error}`, killed: kill.killed, saveNote: save.note };
  }
  return { ok: true, killed: kill.killed, forced: kill.forced, saved: save.saved, saveNote: save.note, jobId: opened.jobId, projectPath: target };
}

async function runOpen(cfg, uproject) {
  const steps = job.steps;
  const add = (key, label) => {
    const s = { key, label, state: 'pending', detail: '', durationMs: null };
    steps.push(s);
    return s;
  };

  // ⓪ 确保 UnrealAgentLink 插件已装（没装的话编辑器起来了也不会连 17860）
  const s0 = add('plugin', '检查/安装 UnrealAgentLink 插件');
  try {
    const pr = ensurePlugin(uproject);
    if (pr.error) throw new Error(pr.error);
    if (pr.installed) {
      s0.state = 'ok';
      s0.detail = `已装进工程（源：${pr.source}）。若编辑器此前已开着该工程，需重启编辑器才生效`;
    } else {
      s0.state = 'skip';
      s0.detail = '插件已就绪';
    }
  } catch (e) {
    s0.state = 'fail';
    s0.detail = shortErr(e);
    job.state = 'failed';
    job.error = `插件安装失败：${shortErr(e)}`;
    return;
  }

  // ① 拉起编辑器打开工程（走 vault.project_manage.open_project，纯本地）
  const s1 = add('open', '拉起 UE 编辑器（打开工程）');
  try {
    const r = vault.vaultProjectManage({ action: 'open_project', projectPath: uproject });
    if (!r.ok) throw new Error(r.error || 'open_project 失败');
    s1.state = 'ok';
    s1.detail = uproject;
  } catch (e) {
    job.state = 'failed';
    job.error = shortErr(e);
    s1.state = 'fail';
    s1.detail = shortErr(e);
    return;
  }

  // ② 等握手（编辑器进程在跑 + UnrealAgentLink 插件连上工作台 UAL WS 服务端）
  const s2 = add('connect', '等待编辑器连接（首次启动较慢）');
  {
    const t0 = Date.now();
    s2.state = 'running';
    let connected = null;
    const { ualStatus, isConnectionStale } = await import('./ual.mjs');
    while (Date.now() - t0 < cfg.connectTimeoutMs) {
      const ust = ualStatus();
      // 判据收紧：必须有「心跳健康 + 已收到 project.info（带工程路径）」的连接才算真正握手成功。
      // 旧判据只要 connectionCount>0 就判成功，导致「插件 WS 刚建立、还没发任何事件」也被误判为 connected，
      // 之后模型对着一条死连接狂发命令（历史教训：崩溃残留的死连接 / 半开 TCP 都栽在这）。
      const live = (ust.connections || []).find(
        (c) => c.healthy !== false && c.project?.path
      );
      if (live) {
        const p = live.project.path;
        if (p && job.projectName && !p.toLowerCase().includes(job.projectName.toLowerCase())) {
          // 连上的是别的工程 → 继续等
          await new Promise((res) => setTimeout(res, 5000));
          continue;
        }
        connected = p;
        break;
      }
      await new Promise((res) => setTimeout(res, 5000));
    }
    s2.durationMs = Date.now() - t0;
    if (!connected) {
      s2.state = 'fail';
      s2.detail = `超时（>${Math.round(cfg.connectTimeoutMs / 1000)}s）。若 UE 起了但连不上：检查 HTTP_PROXY 等代理变量（会劫持 UE 连 localhost 的 WebSocket）`;
      job.state = 'failed';
      job.error = '编辑器握手超时';
      return;
    }
    s2.state = 'ok';
    s2.detail = connected;
  }

  job.state = 'done';
}

/** 契约端点用：编排状态 */
export function jobInfo() {
  if (!job) return { contract: 'v1', state: 'idle', selectedPath: selectedProjectPath };
  return {
    contract: 'v1',
    state: job.state,
    jobId: job.id,
    startedAt: job.startedAt,
    steps: job.steps,
    error: job.error,
    projectPath: job.projectPath,
    projectName: job.projectName,
    selectedPath: selectedProjectPath,
  };
}

/** 后端退出时编排自然终止 */
export function stopAll() {
  if (job && job.state === 'running') {
    job.state = 'failed';
    job.error = '后端退出，流程中断';
  }
}
