/**
 * seam.compiler —— UE 原生编译验证（后端独立进程内运行）
 *
 * 机制：调 UE5 UnrealBuildTool（Build.bat）真编译 DIAOYU 工程，
 *       exit code 0 = 通过；错误行用 `error (C|LNK|MSB)\d+` 提取。
 *       （判据与 finetune/_compile_verify.py 一致：这是唯一能真正回答"代码对不对"的闸门）
 *
 * 安全（立项 §9.3）：
 *  - 编译命令的参数（Build.bat 路径 / target / project）**只从本机配置文件读**
 *    （~/.xiaomanbox/compiler.json，可用状态端点维护），HTTP 请求体不接受任意命令路径。
 *  - 单任务互斥：已有编译在跑时再触发 → 409（Build.bat 有 mutex，并发只会互相坑）。
 *  - 后端退出时杀掉编译子进程，不留孤儿（配合 server.mjs 的 stdin 孤儿守护）。
 */

import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';

const DEFAULTS = {
  buildBat: '',
  project: '',
  cwd: '',
  target: 'DIAOYUEditor',
  platform: 'Win64',
  config: 'Development',
  timeoutMs: 3_600_000,
};

const ERR_RE = /\berror\s+(C|LNK|MSB)\d+/i;

/**
 * Windows 下杀进程树（cmd.exe → Build.bat → UBT → MSVC）。
 * child.kill() 只杀 cmd.exe，UBT 会被留下继续编译 → 必须用 taskkill /T。
 */
function killTree(pid) {
  if (!pid) return;
  if (process.platform === 'win32') {
    try {
      spawn('taskkill', ['/pid', String(pid), '/T', '/F'], { windowsHide: true });
    } catch {
      /* 忽略 */
    }
  } else {
    try {
      process.kill(pid);
    } catch {
      /* 忽略 */
    }
  }
}

function configFile() {
  return path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'compiler.json');
}

function loadConfig() {
  const cfg = { ...DEFAULTS };
  try {
    const raw = JSON.parse(fs.readFileSync(configFile(), 'utf8'));
    for (const k of Object.keys(DEFAULTS)) {
      if (typeof raw?.[k] === typeof DEFAULTS[k]) cfg[k] = raw[k];
    }
  } catch {
    /* 无配置文件 = 用默认值 */
  }
  return cfg;
}

let job = null; // { id, state, cfg, startedAt, child, exitCode, durationSec, errors, tail, error }

function tailOf(s, n = 4000) {
  return s.length > n ? s.slice(-n) : s;
}

/** 启动一次编译验证。返回 { ok, jobId } 或 { ok:false, code:409, error } */
export function startVerify() {
  if (job && job.state === 'running') {
    return { ok: false, code: 409, error: '编译已在进行中', jobId: job.id, startedAt: job.startedAt };
  }
  const cfg = loadConfig();
  // ★ Node ≥18.20 禁止直接 spawn .bat（CVE-2024-27980，EINVAL）→ 必须 shell 方式。
  //   本机路径无空格，但引号照加，路径带空格也稳。
  const cmdline =
    `"${cfg.buildBat}" ${cfg.target} ${cfg.platform} ${cfg.config} "${cfg.project}" ` +
    '-WaitMutex -NoHotReload -NoUBA';
  let child;
  try {
    child = spawn(cmdline, { shell: true, cwd: cfg.cwd, windowsHide: true });
  } catch (e) {
    return { ok: false, code: 500, error: `编译进程启动失败: ${e.message}` };
  }
  if (!child.pid) {
    return { ok: false, code: 500, error: '编译进程启动失败（spawn 无 pid，检查 Build.bat 路径）' };
  }

  job = {
    id: `C-${Date.now()}`,
    state: 'running',
    cfg: { target: cfg.target, platform: cfg.platform, config: cfg.config, project: cfg.project },
    startedAt: new Date().toISOString(),
    child,
    exitCode: null,
    durationSec: null,
    errors: [],
    tail: '',
    error: null,
  };

  let out = '';
  const pump = (c) => {
    out += c;
    if (out.length > 400_000) out = out.slice(-200_000);
  };
  child.stdout?.on('data', pump);
  child.stderr?.on('data', pump);

  child.on('exit', (code) => {
    if (!job || job.child !== child) return;
    job.state = 'done';
    job.exitCode = code;
    job.durationSec = Number(((Date.now() - job.startedMs) / 1000).toFixed(1));
    job.errors = out.split('\n').filter((l) => ERR_RE.test(l)).slice(0, 8);
    job.tail = tailOf(out);
  });
  child.on('error', (e) => {
    if (!job || job.child !== child) return;
    job.state = 'done';
    job.exitCode = -1;
    job.error = String(e.message || e);
    job.tail = tailOf(out);
  });

  job.startedMs = Date.now();
  job.timer = setTimeout(() => {
    if (job && job.state === 'running') {
      job.error = `编译超时（>${Math.round(cfg.timeoutMs / 60000)} 分钟），已终止`;
      killTree(child.pid);
    }
  }, cfg.timeoutMs);
  job.timer.unref?.();

  return { ok: true, jobId: job.id };
}

/** 契约端点用：当前/最近一次编译任务状态 */
export function jobInfo() {
  if (!job) return { contract: 'v1', state: 'idle' };
  return {
    contract: 'v1',
    state: job.state,
    jobId: job.id,
    startedAt: job.startedAt,
    exitCode: job.exitCode,
    durationSec: job.durationSec,
    errors: job.errors,
    error: job.error,
    cfg: job.cfg,
  };
}

/** 后端退出：杀掉还在跑的编译子进程树，不留孤儿 */
export function stopAll() {
  if (job && job.state === 'running' && job.child) {
    killTree(job.child.pid);
    job.state = 'done';
    job.exitCode = -1;
    job.error = '后端退出，编译被终止';
  }
}
