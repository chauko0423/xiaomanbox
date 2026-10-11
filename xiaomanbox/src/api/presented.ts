import { get } from './client';

/** 产出文件（小满 present 的文件，点开用系统默认程序拉起） */

export interface PresentedFile {
  path: string;
  label: string;
  at: number;
}

export interface PresentedResponse {
  contract: string;
  ok: boolean;
  files: PresentedFile[];
}

export function listPresented(sessionId?: string): Promise<PresentedResponse> {
  return get<PresentedResponse>(sessionId ? `/presented?sessionId=${encodeURIComponent(sessionId)}` : '/presented');
}

/** 用系统默认程序打开文件（Electron 桥；浏览器环境降级 window.open） */
export async function openPresentedFile(path: string): Promise<boolean> {
  const bridge = (window as unknown as { xiaomanBox?: { openPath?: (p: string) => Promise<{ ok: boolean }> } }).xiaomanBox;
  if (bridge?.openPath) {
    const r = await bridge.openPath(path);
    return !!r?.ok;
  }
  // 非 Electron（浏览器预览）降级：新标签打开
  window.open(`file:///${path.replace(/\\/g, '/')}`, '_blank');
  return true;
}

/** 产出物文件内容（2026-10-10 用户「侧边栏内预览产出物」） */
export interface FileContentResponse {
  contract: string;
  ok: boolean;
  name: string;
  kind: 'text' | 'html' | 'image' | 'binary';
  text?: string;
  dataUrl?: string;
  size?: number;
  error?: string;
}

export function fetchFileContent(path: string): Promise<FileContentResponse> {
  return get<FileContentResponse>(`/file?path=${encodeURIComponent(path)}`);
}
