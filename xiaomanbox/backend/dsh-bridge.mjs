/**
 * dsh 桥接（2026-10-09）—— 路线 B「外部接入」的底座。
 *
 * 思路：dsh 是独立进程（headless/ACP 模式），xiaomanbox 通过 stdio 桥接它，
 *   跑 dsh 的 Cordis 插件原代码。这样 xiaomanbox 保持零依赖（不装 dsh 就不受影响），
 *   用户显式选「外部接入」才用这条路。
 *
 * 本模块当前职责：
 *   1. detectDsh() —— 探测本机 dsh 运行时是否可调用（三处：PATH / npm 全局 / npx 缓存），
 *      零副作用，不实际安装任何东西。
 *   2. bridgeDsh() —— ACP 桥接入口（预留，返回当前能力边界，后续接 JSON-RPC stdio）。
 *
 * 零依赖：只用 node 内置 fs/path/os/child_process。
 */
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { execFileSync, spawn } from 'node:child_process';

/** 在 npx 缓存目录下找 @deepseek-ai/dsh 的 bin 入口 */
function findDshInNpx() {
  const localAppData = process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local');
  const npxRoot = path.join(localAppData, 'npm-cache', '_npx');
  if (!fs.existsSync(npxRoot)) return null;
  try {
    for (const sub of fs.readdirSync(npxRoot)) {
      const bin = path.join(npxRoot, sub, 'node_modules', '@deepseek-ai', 'dsh', 'lib', 'bin.js');
      if (fs.existsSync(bin)) return bin;
    }
  } catch { /* 忽略 */ }
  return null;
}

/**
 * 解析「真 node」可执行文件：跑 dsh 需要较新的 node（v22+，dsh 用现代 ESM 语法）。
 * 工作台后端在 Electron 链条里 process.execPath 是 electron.exe（其自带 node 是 v20，
 * 跑 dsh 会静默退出）——所以 Electron 场景下必须另找系统 node。
 */
function resolveNodeExe() {
  const exe = process.execPath || '';
  // 非 Electron（直接 node 跑 server.mjs 的场景）→ 直接用
  if (!/electron/i.test(path.basename(exe))) return { exe, source: 'execPath' };
  // Electron 场景：先查 PATH 里的 node
  try {
    const whereCmd = process.platform === 'win32' ? 'where' : 'which';
    const out = execFileSync(whereCmd, ['node'], { encoding: 'utf8', timeout: 5000 });
    const first = out.split(/\r?\n/).map((s) => s.trim()).find((s) => s && !/electron/i.test(s));
    if (first && fs.existsSync(first)) return { exe: first, source: 'PATH' };
  } catch { /* 忽略 */ }
  // workbuddy 管理的 node（versions 下取最新版本目录）
  const wbVersions = path.join(os.homedir(), '.workbuddy', 'binaries', 'node', 'versions');
  if (fs.existsSync(wbVersions)) {
    try {
      const versions = fs.readdirSync(wbVersions).sort().reverse();
      for (const v of versions) {
        const exe = path.join(wbVersions, v, 'node.exe');
        if (fs.existsSync(exe)) return { exe, source: 'workbuddy' };
      }
    } catch { /* 忽略 */ }
  }
  // 常见安装位置兜底
  for (const c of ['C:/Program Files/nodejs/node.exe']) {
    if (fs.existsSync(c)) return { exe: c, source: 'fallback' };
  }
  return { exe: null, source: null };
}

/** 在 PATH 里找 dsh（Windows 下可能是 dsh.cmd / dsh.ps1） */
function findDshInPath() {
  const exts = process.platform === 'win32' ? ['', '.cmd', '.ps1', '.exe'] : [''];
  const dirs = (process.env.PATH || '').split(path.delimiter).filter(Boolean);
  for (const dir of dirs) {
    for (const ext of exts) {
      const p = path.join(dir, `dsh${ext}`);
      try {
        if (fs.existsSync(p)) return p;
      } catch { /* 忽略 */ }
    }
  }
  return null;
}

/** 在 npm 全局目录找 dsh */
function findDshInNpmGlobal() {
  const globalDir = process.env.APPDATA ? path.join(process.env.APPDATA, 'npm') : path.join(os.homedir(), 'AppData', 'Roaming', 'npm');
  for (const f of ['dsh', 'dsh.cmd', 'dsh.ps1']) {
    const p = path.join(globalDir, f);
    try {
      if (fs.existsSync(p)) return p;
    } catch { /* 忽略 */ }
  }
  return null;
}

/** 探测 dsh 运行时：返回可调用性 + 来源 + 入口路径 + 版本（能拿到的话） */
export function detectDsh() {
  const npxBin = findDshInNpx();
  const pathBin = findDshInPath();
  const globalBin = findDshInNpmGlobal();

  let bin = null;
  let source = null;
  if (pathBin) { bin = pathBin; source = 'PATH'; }
  else if (globalBin) { bin = globalBin; source = 'npm-global'; }
  else if (npxBin) { bin = npxBin; source = 'npx-cache'; }

  if (!bin) {
    return {
      available: false,
      source: null,
      bin: null,
      version: null,
      reason: '未检测到 dsh 运行时（PATH / npm 全局 / npx 缓存均无）。选「外部接入」需先安装 dsh：npm i -g @deepseek-ai/dsh',
    };
  }

  let version = null;
  let versionError = null;
  try {
    // npx-cache 的 bin.js 用「真 node」执行；PATH/global 的 wrapper 直接执行
    const node = resolveNodeExe();
    const cmd = source === 'npx-cache' ? node.exe : bin;
    const args = source === 'npx-cache' ? [bin, '--version'] : ['--version'];
    if (!cmd) throw new Error('未找到 node');
    version = execFileSync(cmd, args, { timeout: 15000, encoding: 'utf8' }).trim();
  } catch (e) {
    versionError = String(e.message || e).split('\n')[0];
  }

  return {
    available: true,
    source,
    bin,
    version,
    versionError,
    reason: null,
  };
}

/** ACP 桥接入口（预留）：后续起 dsh 子进程走 JSON-RPC stdio，把 dsh 插件能力暴露给小满 */
export function bridgeDsh() {
  const det = detectDsh();
  return {
    ok: true,
    route: 'B',
    runtime: det,
    // 当前能力边界：检测已通，ACP 桥接待实现（后续接 dsh-acp 的 JSON-RPC stdio）
    bridgeStatus: det.available ? 'runtime-ready' : 'runtime-missing',
    note: det.available
      ? `dsh 运行时可用（${det.source}${det.version ? ' · ' + det.version : ''}），可接入其 Cordis 插件原代码。`
      : det.reason,
  };
}

/**
 * 路线 B 深度桥接：起 dsh 子进程（headless one-shot）真跑任务。
 * dsh 在独立进程里加载它的 Cordis 插件（dsh-tool-bash 等），跑完把最终答案写到 stdout、
 * 推理过程写到 stderr。xiaomanbox 捕获 stdout 作为结果——这就是「外部接入」的真跑。
 *
 * 前置：引擎（ninfer-local 模型，默认 127.0.0.1:8390）需在线；dsh 运行时需可调用。
 */
export function runDsh(task, { timeoutMs = 300000 } = {}) {
  const det = detectDsh();
  if (!det.available) {
    return Promise.resolve({ ok: false, code: 502, error: det.reason, runtime: det });
  }
  const t = String(task || '').trim();
  if (!t) return Promise.resolve({ ok: false, code: 400, error: 'task 不能为空' });

  return new Promise((resolve) => {
    const startedAt = Date.now();
    let stdout = '';
    let stderr = '';
    let done = false;
    let child;
    // 跑 dsh 必须用「真 node」（v22+）：Electron 链条里 execPath 是 electron.exe（自带 node v20 跑 dsh 静默退出）
    const node = resolveNodeExe();
    if (!node.exe) {
      return resolve({ ok: false, code: 500, error: '未找到可用的 node（v22+）来运行 dsh。请安装 Node.js 22+ 或确认其在 PATH 中。' });
    }
    const env = { ...process.env };
    delete env.ELECTRON_RUN_AS_NODE; // 真 node 不需要；残留会干扰
    try {
      child = spawn(node.exe, [det.bin, '--profile', 'headless', t], {
        env,
        stdio: ['ignore', 'pipe', 'pipe'],
      });
    } catch (e) {
      return resolve({ ok: false, code: 500, error: String(e.message || e) });
    }

    const timer = setTimeout(() => {
      if (done) return;
      done = true;
      try { child.kill(); } catch { /* 忽略 */ }
      resolve({ ok: false, code: 504, error: `dsh 执行超时（${Math.round(timeoutMs / 1000)}s）`, partial: stdout.trim().slice(0, 2000), durationMs: Date.now() - startedAt });
    }, timeoutMs);

    child.stdout.on('data', (c) => { stdout += c; });
    child.stderr.on('data', (c) => { stderr += c; });
    child.on('error', (e) => {
      if (done) return;
      done = true; clearTimeout(timer);
      resolve({ ok: false, code: 500, error: String(e.message || e) });
    });
    child.on('close', (code) => {
      if (done) return;
      done = true; clearTimeout(timer);
      const result = stdout.trim();
      const combined = `${stdout}\n${stderr}`;
      // dsh 传输/模型错误（引擎离线等）判定为失败
      const transportFail = /dsh:\s*(TRANSPORT|LLM|TOOL)[^\n]*/i.test(combined);
      if (result && !transportFail) {
        resolve({ ok: true, result, reasoning: stderr.trim().slice(0, 4000), exitCode: code, durationMs: Date.now() - startedAt });
      } else {
        // TRANSPORT = dsh 连不上模型（引擎 8390 离线）→ 给用户一眼能懂的提示
        let friendly = (stderr.trim() || result || `dsh 退出码 ${code}`).slice(0, 500);
        if (/TRANSPORT/i.test(combined)) {
          friendly = '本地模型引擎未启动（dsh 连不上 127.0.0.1:8390）。请先双击「一键启动.bat」拉起引擎，再点「用 dsh 跑」。';
        } else if (!result && !stderr.trim() && code === 0) {
          // 静默退出：典型原因是 node 运行时不兼容（Electron 自带 node v20 跑 dsh 就是这样）
          friendly = 'dsh 进程静默退出（无任何输出）。通常是 node 运行时不兼容——dsh 需要 node v22+。请确认系统已安装新版 Node.js。';
        }
        resolve({
          ok: false,
          code: 502,
          error: friendly,
          exitCode: code,
          durationMs: Date.now() - startedAt,
        });
      }
    });
  });
}

// ── 路线 B「真下载插件」（2026-10-09）─────────
// 机制（实测 @deepseek-ai/dsh@0.2.0-rc.2）：`dsh plugin --profile <name> <args>` 把 args 原样
//   转发给 pnpm，在 $DSH_HOME/profiles/<name> 目录里装包；装完 reconcile：
//   · 声明了 dsh.bundle（package.json 里 "dsh":{"bundle":{"patch":"..."}}）的包 → 追加进
//     dsh.profile.bundles 层，dsh 启动时会作为独立插件层加载；
//   · 未声明 dsh.bundle 的普通包 → 只进 dependencies，不作为插件层加载（会打 warning）。
// 前置：pnpm 需在 PATH（可用 corepack enable pnpm 提供）；dsh 运行时需可调用。

/** 定位 profile 目录（dsh 用 $DSH_HOME，缺省 ~/.dsh） */
function profileDir(profile = 'headless') {
  const home = process.env.DSH_HOME || path.join(os.homedir(), '.dsh');
  return path.join(home, 'profiles', profile);
}

/** 读一个 profile 的 package.json（不存在返回 null） */
function readProfilePkg(profile = 'headless') {
  try {
    return JSON.parse(fs.readFileSync(path.join(profileDir(profile), 'package.json'), 'utf8'));
  } catch {
    return null;
  }
}

/**
 * 探测 pnpm 可执行文件所在目录（dsh plugin 内部 `spawnSync("pnpm")` 依赖 PATH 里有它）。
 * 生产环境后端可能由 Electron 或独立 node 启动，PATH 未必含 pnpm 目录——这里主动探测，
 * 把目录 prepend 进 env.PATH，确保 dsh 能调到 pnpm。
 * 探测顺序：workbuddy 管理的 node 目录 → PATH 里的 node 同目录 → 常见位置。
 */
function findPnpmDir() {
  const candidates = [];
  // workbuddy 管理的 node versions 目录（pnpm 通常随 node 一起）
  const wbVersions = path.join(os.homedir(), '.workbuddy', 'binaries', 'node', 'versions');
  if (fs.existsSync(wbVersions)) {
    try {
      for (const v of fs.readdirSync(wbVersions).sort().reverse()) {
        candidates.push(path.join(wbVersions, v));
      }
    } catch { /* 忽略 */ }
  }
  // PATH 里每个目录（找 pnpm / pnpm.cmd / pnpm.exe）
  for (const dir of (process.env.PATH || '').split(path.delimiter)) {
    if (dir) candidates.push(dir);
  }
  // 常见位置
  candidates.push(path.join(process.env.APPDATA || '', 'npm'), 'C:/Program Files/nodejs');
  const exts = ['', '.cmd', '.exe', '.ps1'];
  for (const dir of candidates) {
    try {
      for (const ext of exts) {
        if (fs.existsSync(path.join(dir, `pnpm${ext}`))) return dir;
      }
    } catch { /* 忽略 */ }
  }
  return null;
}

/**
 * 列出 headless profile 已安装的插件：
 *   · bundles = dsh.profile.bundles（会被 dsh 启动加载的插件层）
 *   · dependencies = package.json.dependencies（下载的普通组件，不会被独立加载）
 */
export function listDshPlugins() {
  const det = detectDsh();
  const pkg = readProfilePkg('headless');
  const bundles = (pkg?.dsh?.profile?.bundles || []).filter((b) => b !== '@deepseek-ai/dsh-base' && b !== '@deepseek-ai/dsh-headless');
  const deps = Object.keys(pkg?.dependencies || {});
  // dependencies 里已在 bundles 的归到 bundles，其余是「普通组件」
  const plainDeps = deps.filter((d) => !bundles.includes(d));
  return {
    ok: true,
    route: 'B',
    runtime: det,
    profile: 'headless',
    profilePath: profileDir('headless'),
    bundles: bundles.map((name) => ({ name, kind: 'bundle', loaded: true, note: '声明 dsh.bundle，dsh 启动时会作为独立插件层加载' })),
    dependencies: plainDeps.map((name) => ({ name, kind: 'dependency', loaded: false, note: '未声明 dsh.bundle，已下载为普通依赖，不会作为插件层加载' })),
  };
}

/**
 * 下载/装载一个原生 dsh 插件：转 `dsh plugin --profile headless add <pkg>`（pnpm）。
 * 返回装完后的 bundles + dependencies，并标注这个包是否真被加载（声明 dsh.bundle）。
 */
export function installDshPlugin(pkg, { timeoutMs = 120000 } = {}) {
  const det = detectDsh();
  if (!det.available) return { ok: false, code: 502, error: det.reason, runtime: det };
  const name = String(pkg || '').trim();
  if (!name) return { ok: false, code: 400, error: 'pkg 不能为空' };
  if (!/^[@\w][\w./-]*$/.test(name)) return { ok: false, code: 400, error: `非法包名：${name}` };

  const node = resolveNodeExe();
  if (!node.exe) return { ok: false, code: 500, error: '未找到可用的 node（v22+）来运行 dsh' };

  const before = readProfilePkg('headless');
  const beforeBundles = before?.dsh?.profile?.bundles || [];

  let stdout = '';
  let stderr = '';
  let exitCode = null;
  try {
    const env = { ...process.env };
    delete env.ELECTRON_RUN_AS_NODE;
    // dsh plugin 内部 spawnSync("pnpm") 依赖 PATH 里有 pnpm——主动探测并把目录 prepend 进去
    const pnpmDir = findPnpmDir();
    if (pnpmDir) env.PATH = `${pnpmDir}${path.delimiter}${env.PATH || ''}`;
    const args = det.source === 'npx-cache' ? [det.bin, 'plugin', '--profile', 'headless', 'add', name] : ['plugin', '--profile', 'headless', 'add', name];
    const cmd = det.source === 'npx-cache' ? node.exe : det.bin;
    const r = execFileSync(cmd, args, { env, timeout: timeoutMs, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
    stdout = r || '';
    exitCode = 0;
  } catch (e) {
    exitCode = typeof e.status === 'number' ? e.status : 1;
    stderr = String(e.stderr || e.message || e);
  }

  const after = readProfilePkg('headless');
  const afterBundles = after?.dsh?.profile?.bundles || [];
  const isBundle = afterBundles.includes(name);
  const newlyLoaded = afterBundles.some((b) => !beforeBundles.includes(b));

  return {
    ok: exitCode === 0,
    code: exitCode === 0 ? 200 : 502,
    pkg: name,
    profile: 'headless',
    loaded: isBundle && newlyLoaded,
    kind: isBundle ? 'bundle' : 'dependency',
    note: isBundle
      ? (newlyLoaded ? `已下载并装载为独立插件层（dsh.bundle），dsh 启动时会加载` : `已下载（已是 bundles 层成员，未新增层）`)
      : `已下载为普通依赖，未声明 dsh.bundle，不会作为独立插件层加载（详见 stderr warning）`,
    bundles: afterBundles.filter((b) => b !== '@deepseek-ai/dsh-base' && b !== '@deepseek-ai/dsh-headless'),
    dependencies: Object.keys(after?.dependencies || {}).filter((d) => !afterBundles.includes(d)),
    exitCode,
    detail: (stdout || stderr).trim().slice(-2000),
  };
}

/** 卸载一个原生 dsh 插件：转 `dsh plugin --profile headless remove <pkg>` */
export function removeDshPlugin(pkg) {
  const det = detectDsh();
  if (!det.available) return { ok: false, code: 502, error: det.reason, runtime: det };
  const name = String(pkg || '').trim();
  if (!name) return { ok: false, code: 400, error: 'pkg 不能为空' };
  if (!/^[@\w][\w./-]*$/.test(name)) return { ok: false, code: 400, error: `非法包名：${name}` };

  const node = resolveNodeExe();
  if (!node.exe) return { ok: false, code: 500, error: '未找到可用的 node（v22+）来运行 dsh' };

  let stdout = '';
  let stderr = '';
  let exitCode = null;
  try {
    const env = { ...process.env };
    delete env.ELECTRON_RUN_AS_NODE;
    // dsh plugin 内部 spawnSync("pnpm") 依赖 PATH 里有 pnpm——主动探测并把目录 prepend 进去
    const pnpmDir = findPnpmDir();
    if (pnpmDir) env.PATH = `${pnpmDir}${path.delimiter}${env.PATH || ''}`;
    const args = det.source === 'npx-cache' ? [det.bin, 'plugin', '--profile', 'headless', 'remove', name] : ['plugin', '--profile', 'headless', 'remove', name];
    const cmd = det.source === 'npx-cache' ? node.exe : det.bin;
    const r = execFileSync(cmd, args, { env, timeout: 120000, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
    stdout = r || '';
    exitCode = 0;
  } catch (e) {
    exitCode = typeof e.status === 'number' ? e.status : 1;
    stderr = String(e.stderr || e.message || e);
  }

  const after = readProfilePkg('headless');
  const afterBundles = after?.dsh?.profile?.bundles || [];
  return {
    ok: exitCode === 0,
    code: exitCode === 0 ? 200 : 502,
    pkg: name,
    removed: exitCode === 0,
    bundles: afterBundles.filter((b) => b !== '@deepseek-ai/dsh-base' && b !== '@deepseek-ai/dsh-headless'),
    dependencies: Object.keys(after?.dependencies || {}).filter((d) => !afterBundles.includes(d)),
    exitCode,
    detail: (stdout || stderr).trim().slice(-2000),
  };
}
