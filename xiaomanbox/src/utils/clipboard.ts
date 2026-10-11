/** 复制文本到剪贴板（Electron 安全版，2026-10-08）。
 *
 *  navigator.clipboard 在 Electron 渲染进程可能被权限拦
 *  （"Failed to execute 'writeText' on 'Clipboard': Write permission denied"），
 *  失败时降级到「临时 textarea + execCommand('copy')」——
 *  有用户点击手势（按钮点击）时 execCommand 在 Electron 里可通。
 */
export async function copyText(text: string): Promise<void> {
  try {
    await navigator.clipboard.writeText(text);
  } catch {
    const ta = document.createElement('textarea');
    ta.value = text;
    ta.style.position = 'fixed';
    ta.style.opacity = '0';
    document.body.appendChild(ta);
    ta.focus();
    ta.select();
    const ok = document.execCommand('copy');
    document.body.removeChild(ta);
    if (!ok) throw new Error('复制失败：剪贴板不可用');
  }
}
