/**
 * XiaoMan Box —— Electron 主进程
 *
 * 职责：
 *  1. spawn 后端独立进程（backend/server.mjs，用 Electron 自带 Node 跑，自包含可移植）
 *  2. 创建窗口，加载前端（dev: Vite URL / prod: dist/index.html）
 *  3. 关窗时杀后端 → 保证「无残留进程」（验收 #3）
 *
 * 关键坑（常青知识 2026-10-07）：
 *  - 后端用 process.execPath + ELECTRON_RUN_AS_NODE=1 跑，这个变量只进子进程 env，
 *    不能残留到主进程（否则 require('electron') 返回空）。
 *  - agent 沙箱起 GUI 会 GPU 进程崩溃 → 起窗验证让用户双击 start.bat；
 *    --smoke 模式不建窗口，可无 GUI 自检 spawn/kill 生命周期。
 */

const { app, BrowserWindow, ipcMain, dialog, session, shell, screen } = require('electron');
const { spawn } = require('node:child_process');
const path = require('node:path');
const http = require('node:http');

// Windows GUI launchers can close stdout/stderr while the backend is still logging.
// A broken diagnostic pipe must never become an uncaught UI exception.
for (const stream of [process.stdout, process.stderr]) {
  stream.on('error', (error) => {
    if (error.code !== 'EPIPE') blog('diagnostic stream error: ' + error.message);
  });
}
function diagnosticWrite(stream, data) {
  if (stream.destroyed || !stream.writable) return;
  try { stream.write(data); } catch (error) {
    if (error.code !== 'EPIPE') blog('diagnostic write error: ' + error.message);
  }
}
app.disableHardwareAcceleration();
const BACKEND_PORT = Number(process.env.WORKBENCH_BACKEND_PORT ?? 8871);
const BACKEND_URL = `http://127.0.0.1:${BACKEND_PORT}`;
const isDev = Boolean(process.env.VITE_DEV_SERVER_URL);
const isSmoke = process.argv.includes('--smoke');

let backend = null;

// 后端生命周期文件日志（2026-10-10）：GUI 模式 console 会被丢弃，spawn 故障必须落文件可查
const os = require('node:os');
const fs = require('node:fs');
const BACKEND_LOG = path.join(os.tmpdir(), 'xiaoman-box-backend.log');
function blog(line) {
  try {
    fs.appendFileSync(BACKEND_LOG, `[${new Date().toISOString()}] ${line}\n`, 'utf8');
  } catch { /* 日志失败不影响主流程 */ }
}

function startBackend() {
  // 打包后后端在 resources/backend/（extraResources，asar 外）——
  // 因为 ELECTRON_RUN_AS_NODE 纯 node 模式读不了 asar，后端必须放 asar 外。
  const serverPath = app.isPackaged
    ? path.join(process.resourcesPath, 'backend', 'server.mjs')
    : path.join(__dirname, '..', 'backend', 'server.mjs');
  blog(`spawn: exe=${process.execPath} server=${serverPath} exists=${fs.existsSync(serverPath)}`);
  backend = spawn(process.execPath, [serverPath], {
    // 用 Electron 自带 Node 跑后端：不绑死外部 node 路径，装哪都能跑。
    env: { ...process.env, ELECTRON_RUN_AS_NODE: '1', WORKBENCH_BACKEND_PORT: String(BACKEND_PORT) },
    stdio: ['pipe', 'pipe', 'pipe'],
    windowsHide: true,
  });
  backend.stdout.on('data', (d) => { diagnosticWrite(process.stdout,d); blog(`out: ${String(d).trim().slice(0, 200)}`); });
  backend.stderr.on('data', (d) => { diagnosticWrite(process.stderr,d); blog(`err: ${String(d).trim().slice(0, 500)}`); });
  backend.on('error', (e) => blog(`spawn error: ${e.message}`));
  backend.on('exit', (code, sig) => {
    blog(`exit code=${code} sig=${sig}`);
    console.log(`[main] backend exit code=${code} sig=${sig}`);
    backend = null;
  });
  return backend;
}

function stopBackend() {
  if (backend && !backend.killed) {
    try {
      backend.kill('SIGTERM');
    } catch {
      /* 已退出则忽略 */
    }
  }
}

function waitForBackend(timeoutMs = 8000) {
  return new Promise((resolve, reject) => {
    const deadline = Date.now() + timeoutMs;
    const tick = () => {
      const req = http.get(`${BACKEND_URL}/api/v1/health`, (res) => {
        let data = '';
        res.on('data', (c) => (data += c));
        res.on('end', () => {
          try {
            const j = JSON.parse(data);
            if (j.contract === 'v1' && j.ok) resolve(j);
            else reject(new Error('health 响应非法: ' + data));
          } catch {
            reject(new Error('health 响应非法: ' + data));
          }
        });
      });
      req.on('error', () => {
        if (Date.now() > deadline) reject(new Error('后端超时未就绪'));
        else setTimeout(tick, 200);
      });
      req.setTimeout(1000, () => req.destroy());
    };
    tick();
  });
}

function fetchStatus() {
  return new Promise((resolve, reject) => {
    http
      .get(`${BACKEND_URL}/api/v1/status`, (res) => {
        let d = '';
        res.on('data', (c) => (d += c));
        res.on('end', () => {
          try {
            resolve(JSON.parse(d));
          } catch (e) {
            reject(e);
          }
        });
      })
      .on('error', reject);
  });
}

/** 无窗口自检：spawn 后端 → 契约 v1 校验 → kill → 断言无残留。 */
async function runSmoke() {
  const b = startBackend();
  try {
    const health = await waitForBackend();
    const status = await fetchStatus();

    const exitPromise = new Promise((r) => b.once('exit', (code, sig) => r({ code, sig })));
    const exitPromiseWithTimeout = Promise.race([
      exitPromise,
      new Promise((r) => setTimeout(() => r('TIMEOUT'), 5000)),
    ]);

    console.log('[smoke] health=' + JSON.stringify(health));
    console.log('[smoke] status=' + JSON.stringify(status));

    const a1 = status.contract === 'v1';
    const a2 = status.backendPid === b.pid;
    console.log('[smoke] 断言 contract=v1 : ' + a1);
    console.log('[smoke] 断言 backendPid 与子进程一致 : ' + a2);

    b.kill('SIGTERM');
    const exitInfo = await exitPromiseWithTimeout;
    const a3 = exitInfo !== 'TIMEOUT';
    console.log('[smoke] 断言 后端已退出(无残留) : ' + a3 + ' ' + JSON.stringify(exitInfo));

    const pass = a1 && a2 && a3;
    console.log('[smoke] RESULT=' + (pass ? 'PASS' : 'FAIL'));
    app.exit(pass ? 0 : 1);
  } catch (e) {
    console.error('[smoke] RESULT=FAIL ' + e.message);
    try {
      b.kill('SIGTERM');
    } catch {}
    app.exit(1);
  }
}

function createWindow() {
  const win = new BrowserWindow({
    width: 1080,
    height: 720,
    minWidth: 720,
    minHeight: 520,
    title: 'XiaoManBox Basic',
    backgroundColor: '#14151a',
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
      preload: path.join(__dirname, 'preload.cjs'),
    },
  });
  if (isDev) {
    win.loadURL(process.env.VITE_DEV_SERVER_URL);
  } else {
    win.loadFile(path.join(__dirname, '..', 'dist', 'index.html'));
  }
  return win;
}

app.whenReady().then(() => {
  if (isSmoke) {
    runSmoke();
    return;
  }

  // ── IPC：原生「选择文件夹」对话框（渲染进程用于路径设置里的「浏览」）──
  ipcMain.handle('select-directory', async (event, opts) => {
    const win = BrowserWindow.fromWebContents(event.sender);
    const options = {
      title: (opts && opts.title) || '选择工程所在目录',
      defaultPath: (opts && opts.defaultPath) || undefined,
      properties: ['openDirectory'],
    };
    const result = win
      ? await dialog.showOpenDialog(win, options)
      : await dialog.showOpenDialog(options);
    if (result.canceled || !result.filePaths.length) return { canceled: true, dir: null };
    return { canceled: false, dir: result.filePaths[0] };
  });

  // ── IPC：用系统默认程序打开文件/链接（产出文件「点开拉起」）──
  ipcMain.handle('open-path', async (event, p) => {
    try {
      const err = await shell.openPath(String(p || ''));
      return { ok: !err, error: err || null };
    } catch (e) {
      return { ok: false, error: String(e.message || e) };
    }
  });

  // ── 权限（2026-10-08 用户：语音输入真功能）──
  // Chromium 默认对 getUserMedia 弹权限请求；这里显式放行 media（麦克风），
  // 其余（通知/地理位置等）保持拒绝——最小授权。
  session.defaultSession.setPermissionRequestHandler((_wc, permission, callback) => {
    callback(permission === 'media');
  });

  startBackend();
  createWindow();

  setImmediate(() => {
    const usage = process.memoryUsage();
    console.log(
      '[main] 主进程内存 rss=' +
        (usage.rss / 1048576).toFixed(1) +
        ' MiB heapUsed=' +
        (usage.heapUsed / 1048576).toFixed(1) +
        ' MiB'
    );
  });

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on('before-quit', () => {
  console.log('[main] before-quit: 停止后端');
  stopBackend();
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});

// 双保险：进程退出前确保后端被清理，不留残留
process.on('exit', () => stopBackend());
