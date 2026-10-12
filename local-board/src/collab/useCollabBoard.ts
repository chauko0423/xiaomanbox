import { useEffect, useRef, useState } from "react";
import * as Y from "yjs";
import { HocuspocusProvider } from "@hocuspocus/provider";
import { BoardCollab } from "./boardCollab";
import { CommentsStore } from "./comments";
import { getIdentity } from "./identity";
import { destroyOwnedProvider } from "./providerLifecycle.mjs";


// HTTP 与协作连接使用同一宿主和端口。
const WS_URL = `${location.protocol === "https:" ? "wss:" : "ws:"}//${location.host}`;

export type Peer = {
  clientId: number;
  name: string;
  color: string;
};

export type BoardStatus = "connecting" | "connected" | "disconnected" | "denied";

/**
 * @param key 房间分享 key（editKey / viewKey）。
 *   有 key 时 token 拼成 `<role>_<key>`，服务端 onAuthenticate 校验；
 *   服务端始终要求已登记房间的有效密钥。
 */
export function useCollabBoard(
  roomId: string,
  role: "edit" | "view",
  api: any,
  key?: string | null
) {
  const collaborators = useRef<Map<string, any>>(new Map());
  const peersKey = useRef("");
  const [peers, setPeers] = useState<Peer[]>([]);
  const [status, setStatus] = useState<BoardStatus>("connecting");
  const [denyReason, setDenyReason] = useState("");
  const [commentsStore, setCommentsStore] = useState<CommentsStore | null>(null);
  const collabRef = useRef<BoardCollab | null>(null);
  const lastPointerAt = useRef(0);

  useEffect(() => {
    if (!api || !roomId) return;

    // 基础版用本机生成的匿名显示名。
    const identity = getIdentity(null);
    const doc = new Y.Doc();
    const readOnly = role === "view";

    const provider = new HocuspocusProvider({
      url: WS_URL,
      name: roomId,
      document: doc,
      token: key ? `${role}_${key}` : role,
      onStatus: ({ status }: { status: string }) => {
        if (status === "connected") setStatus("connected");
        else if (status === "connecting") setStatus("connecting");
        else setStatus("disconnected");
      },
      // 服务端 onAuthenticate 抛错（key 不对 / 房间已换 key）时走到这里，
      // 不处理的话界面会一直卡在「连接中」，用户不知道发生了什么。
      onAuthenticationFailed: ({ reason }: { reason?: string }) => {
        console.warn("[collab] 鉴权失败:", reason);
        setDenyReason(reason || "没有访问该画板的权限");
        setStatus("denied");
      },
    });

    const awareness = provider.awareness;
    if (awareness) {
      awareness.setLocalStateField("user", {
        name: identity.name,
        color: identity.color,
      });
      // 先广播一个空 cursor，让其他客户端的 awareness state 含 cursor 字段（否则要等我们下次动鼠标才广播）
      awareness.setLocalStateField("cursor", null);
    }

    // 只有完成首次同步后才开始绑定，否则本地空场景会把远端数据清空
    let started = false;
    let comments: CommentsStore | null = null;
    const startCollab = () => {
      if (started) return;
      started = true;
      const collab = new BoardCollab({ doc, api, awareness, readOnly });
      collab.start();
      collabRef.current = collab;
      comments = new CommentsStore(doc);
      setCommentsStore(comments);
    };
    // 每次 synced 都启动（或重启）绑定层：保证短暂断线后远端数据能再次推回 Excalidraw
    // (boardCollab.start 内部会清掉旧的 unobserve 再注册新的 observe)
    provider.on("synced", () => {
      if (started && collabRef.current) {
        // 重连后再触发一次远端推回，弥补断线期间本地 observe 未触发的场景变化
        try { collabRef.current.restart(); } catch {}
        return;
      }
      startCollab();
      // 修复「打开画板一片空白」：initialData 的 scrollToContent 只在画布还空时生效一次，
      // 而远端元素是同步推入后才就位，视图会停在 (0,0) 原点附近（内容实际在远处，如脑图
      // x≈1500~2000 区域）。这里在首次推回后延迟一帧自动滚动定位到内容。
      setTimeout(() => {
        try { api?.scrollToContent?.(); } catch { /* 某些版本无此方法则忽略 */ }
      }, 350);
    });
    const fallbackTimer = setTimeout(startCollab, 3000);

    const onAwareness = () => {
      if (!awareness) return;
      const nextMap = new Map<string, any>();
      const nextPeers: Peer[] = [];

      awareness.getStates().forEach((state: any, clientId: number) => {
        if (clientId === awareness.clientID) return;
        if (!state.user) return;

        nextPeers.push({
          clientId,
          name: state.user.name,
          color: state.user.color,
        });

        if (state.cursor) {
          nextMap.set(String(clientId), {
            username: state.user.name,
            color: state.user.color,
            pointer: state.cursor,
            selectedElementIds: state.selection || {},
          });
        }
      });

      collaborators.current = nextMap;
      const nextKey = JSON.stringify(nextPeers);
      if (nextKey !== peersKey.current) { peersKey.current = nextKey; setPeers(nextPeers); }
    };

    awareness?.on("change", onAwareness);

    return () => {
      clearTimeout(fallbackTimer);
      awareness?.off("change", onAwareness);
      collabRef.current?.destroy();
      collabRef.current = null;
      comments?.destroy();
      collaborators.current = new Map();
      peersKey.current = "";
      destroyOwnedProvider(provider);
      doc.destroy();
    };
  }, [api, roomId, role, key]);

  // 拦截 Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y。
  //
  // 为什么必须拦：Excalidraw 自带的历史栈按「场景快照」记录，不区分是谁改的。
  // 多人协作时 B 按 Ctrl+Z，Excalidraw 会把整个场景回滚到上一帧 ——
  // 也就是连 A 刚画的苹果一起撤掉。这里改成走 BoardCollab 的本地撤销栈
  // （只追踪 origin=LOCAL_ORIGIN 的事务），B 撤销只影响 B 自己写进去的东西。
  useEffect(() => {
    if (role === "view") return;
    const onKey = (e: KeyboardEvent) => {
      if (!(e.ctrlKey || e.metaKey)) return;
      const k = (e.key || "").toLowerCase();
      if (k !== "z" && k !== "y") return;
      // 正在输入文字（评论框 / Excalidraw 文本编辑）时交给原生撤销
      const t = e.target as HTMLElement | null;
      if (t && (t.tagName === "INPUT" || t.tagName === "TEXTAREA" || (t as any).isContentEditable)) {
        return;
      }
      const collab = collabRef.current;
      if (!collab) return;
      e.preventDefault();
      e.stopPropagation();
      const isRedo = k === "y" || (k === "z" && e.shiftKey);
      const ok = isRedo ? collab.redo() : collab.undo();
      if (!ok && typeof window !== "undefined") {
        console.log(isRedo ? "[redo] 没有可重做的操作" : "[undo] 没有可撤销的操作（最多 5 步，且只撤本端）");
      }
    };
    // capture 阶段 + stopPropagation：抢在 Excalidraw（document 上）的处理器之前吃掉事件
    window.addEventListener("keydown", onKey, true);
    return () => window.removeEventListener("keydown", onKey, true);
  }, [role]);

  return {
    collaborators,
    peers,
    status,
    denyReason,
    commentsStore,
    /** 撤销一步本端操作（最多 5 步，不影响别人的内容） */
    undo: () => collabRef.current?.undo() ?? false,
    /** 重做一步 */
    redo: () => collabRef.current?.redo() ?? false,
    /** 还能撤几步（顶栏可以用来禁用按钮） */
    undoDepth: () => collabRef.current?.undoDepth() ?? 0,
    redoDepth: () => collabRef.current?.redoDepth() ?? 0,
    fileMetadata: () => collabRef.current?.getFileMetadata() || {},
    /** 批量写入（导入专用，单事务） */
    bulkWrite: (els: any[]) => collabRef.current?.bulkWrite(els),
    handleChange: (elements: readonly any[], appState: any) => {
      collabRef.current?.handleChange(elements);
      const selected = appState?.selectedElementIds || {};
      collabRef.current?.setSelection(selected);
    },
    handlePointer: (pointer: { x: number; y: number } | null, button?: string) => {
      // 光标位置高频变化，节流到 20fps；抬起时强制同步最后一次位置。
      // 0.17.6 的 onPointerUpdate payload 是 {pointer, button, pointersMap}：
      // button 在松手那一刻是 "up"（旧代码读的 action 字段并不存在）
      const now = Date.now();
      if (button !== "up" && now && now - lastPointerAt.current < 50) return;
      lastPointerAt.current = now;
      collabRef.current?.setPointer(pointer);
      // 每次松手：这轮手势里手动拖过导图节点的话，把折线取直、文字居中
      //（relayout 保留手动位置，只动连线和文字）
      if (button === "up") collabRef.current?.getMindmap()?.settle();
    },
    setBrush: (brush: { x: number; y: number; points: [number, number][] } | null) =>
      collabRef.current?.setBrush(brush),
    setDrawing: (drawing: boolean, brush?: { x: number; y: number; points: [number, number][] } | null) =>
      collabRef.current?.setDrawing(drawing, brush),
    /**
     * onPointerUp 时调用：按协作层追踪的 in-progress 元素 id 从场景拿最终元素并写入 Yjs。
     * 不能在 App 里 find() 猜元素 —— 场景里有旧笔迹时会拿错，新笔迹进不了 Yjs（松手即消失）。
     */
    finalizeInProgressFreedraw: () => collabRef.current?.finalizeInProgressFreedraw() ?? false,
    /** 兼容保留：显式把单个 freedraw 元素写入 Yjs */
    finalizeFreedraw: (el: any) => collabRef.current?.finalizeFreedraw(el),
    clearBoard: () => {
      collabRef.current?.clear();
    },
    mindmap: {
      enter: (x?: number, y?: number) => collabRef.current?.getMindmap()?.enter(x, y) ?? null,
      insertAt: (x: number, y: number, text?: string) =>
        collabRef.current?.getMindmap()?.insertRoot(x, y, text),
      getRootId: (): string | null => collabRef.current?.getMindmap()?.getRootId() ?? null,
      addChild: (id?: string) => collabRef.current?.getMindmap()?.addChild(id),
      addSibling: (id?: string) => collabRef.current?.getMindmap()?.addSibling(id),
      toggleCollapse: (id?: string) => collabRef.current?.getMindmap()?.toggleCollapse(id),
      deleteNode: (id?: string) => collabRef.current?.getMindmap()?.deleteNode(id),
    },
  };
}
