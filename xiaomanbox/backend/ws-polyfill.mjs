/**
 * WebSocket polyfill —— 必须在所有 import 之前执行（server.mjs 第一行导入）。
 *
 * @hocuspocus/provider 依赖全局 WebSocket：Node 22+ 内置有；
 * 打包版后端跑在 Electron 33 内置的 Node 20 上没有 →
 * 成员发现接口 500「WebSocket is not defined」。用 backend 自带的 ws 包补齐。
 */
if (typeof globalThis.WebSocket === 'undefined') {
  const { WebSocket } = await import('ws');
  globalThis.WebSocket = WebSocket;
}
