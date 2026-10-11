/**
 * XiaoMan Box —— preload
 *
 * contextIsolation 已开、nodeIntegration 已关（安全基线）。
 * 目前只暴露契约版本给渲染进程；后续 IPC（引擎控制 / 状态订阅）在这里收口。
 */

const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('xiaomanBox', {
  contractVersion: 'v1',
  /** 弹出系统「选择文件夹」对话框，返回 { canceled, dir }。 */
  selectDirectory: (opts) => ipcRenderer.invoke('select-directory', opts),
  /** 用系统默认程序打开文件/链接（产出文件「点开拉起」），返回 { ok, error }。 */
  openPath: (p) => ipcRenderer.invoke('open-path', p),
  // ── 桌宠（2026-10-10）──
  /** 显示桌宠窗口 */
  petShow: () => ipcRenderer.invoke('pet:show'),
  /** 隐藏桌宠窗口 */
  petHide: () => ipcRenderer.invoke('pet:hide'),
  /** 显示/隐藏切换 */
  petToggle: () => ipcRenderer.invoke('pet:toggle'),
  /** 查询桌宠可见性 */
  petIsVisible: () => ipcRenderer.invoke('pet:visible'),
  /** 拖拽移动桌宠窗口（渲染端上报屏幕坐标偏移） */
  petMoveBy: (dx, dy) => ipcRenderer.invoke('pet:move-by', { dx, dy }),
  /** 桌宠置顶开关 */
  petSetTopmost: (top) => ipcRenderer.invoke('pet:set-topmost', top),
  /** 光标进入/离开可交互区（翻转整窗穿透） */
  petSetInteractive: (on) => ipcRenderer.send('pet:set-interactive', on),
  // ── 桌宠移动后端（替代 Petra Rust mover，2026-10-10 用户移植）──
  /** 全局光标位置 + 窗口位置（物理 px，含相对窗口中心偏移 rx/ry） */
  petCursorPos: () => ipcRenderer.invoke('pet:cursor-pos'),
  /** 离 (x,y) 最近的显示器工作区（物理 px） */
  petWorkAreaAt: (x, y) => ipcRenderer.invoke('pet:work-area-at', { x, y }),
  /** 平滑移动到目标点（speed px/s） */
  petSetTarget: (x, y, speed) => ipcRenderer.invoke('pet:set-target', { x, y, speed }),
  petSetTargetSpeed: (x, y, speed) => ipcRenderer.invoke('pet:set-target-speed', { x, y, speed }),
  /** 停止移动 */
  petClearTarget: () => ipcRenderer.invoke('pet:clear-target'),
  /** 逗猫棒开关 */
  petSetTracking: (on) => ipcRenderer.invoke('pet:set-tracking', { on }),
  /** 调试标记 */
  petDebugMark: (msg) => ipcRenderer.invoke('pet:debug-mark', { msg }),
  /** 移动窗口到绝对位置（待机贴边用，绕过 mover 平滑） */
  petSetPosition: (x, y) => ipcRenderer.invoke('pet:set-position', { x, y }),
  // ── 桌宠角色（PSD 自由导入 / 切换）──
  /** 列出可用角色：{ builtIn: [{file,name}], user: [{file,name}] } */
  petListModels: () => ipcRenderer.invoke('pet:list-models'),
  /** 读取角色 PSD 字节（用户目录优先于内置） */
  petReadModel: (name) => ipcRenderer.invoke('pet:read-model', { name }),
  /** 弹系统对话框导入 PSD 到 userData/pet-models，返回 { ok, file, name } */
  petImportPsd: () => ipcRenderer.invoke('pet:import-model'),
});
