import * as Y from "yjs";
import { MindMap } from "./mindmap";
import { trimUndoHistory, undoFileIds } from "./memory";
import { uploadAsset, fetchAsset } from "./assets";

/**
 * Excalidraw 场景 <-> Yjs 文档的双向绑定层
 *
 * 数据布局：
 *   yElements: Y.Map<elementId, Y.Map<prop, value>>   元素内容
 *   yOrder:    Y.Array<elementId>                       z 序（重排只动这里）
 *
 * 两个关键设计：
 *   1. 内容 与 顺序 分离 —— 移动层级不会重写元素属性，冲突面更小
 *   2. lastSnapshot 快路径 —— 远端写入的数据会同步进快照，
 *      因此 updateScene 触发的回声 onChange 会被判定为「无变化」而跳过，
 *      避免 本地写入 -> 远端回声 -> 再写入 的死循环
 */

type AnyMap = Y.Map<any>;

/**
 * 本地写入标记（Yjs transaction origin）。
 *
 * 撤销栈只追踪带这个 origin 的事务：远端同步过来的变化 origin 是 provider，
 * 不在这个集合里，所以「我按 Ctrl+Z」只会回滚我自己写进去的东西，
 * 别人画的内容一个字节都不会动（A 撤销不影响 B 的创作）。
 */
export const LOCAL_ORIGIN = Symbol("team-board-local");

/** 本地撤销栈深度（用户要求：只记最近 5 步） */
export const UNDO_MAX_STEPS = 5;

/**
 * 画笔预览（awareness brush）广播节流间隔。
 * 绘制时每个 pointermove 都会触发 brush 更新，而 brush.points 是「整条笔迹」，
 * 不节流的话数据量随点数呈 O(n²) 增长，弱服务器会被广播直接拖垮。
 */
const BRUSH_THROTTLE_MS = 60;

function snapshotOf(el: any) {
  return { version: el.version, versionNonce: el.versionNonce, updated: el.updated };
}

function sameValue(a: any, b: any): boolean {
  if (a === b) return true;
  if (a == null || b == null) return false;
  if (typeof a !== typeof b) return false;
  if (typeof a === "object") return JSON.stringify(a) === JSON.stringify(b);
  return false;
}

/**
 * freedraw 的 points 编码成紧凑字符串。
 *
 * 【为什么必须这么做】实测房间 1.ydoc：只有 3 笔 freedraw / 134 个点，却产生了
 * 119,077 个 CRDT item、2.1MB 文档。原因是 Y.Map.set("points",[...]) 对数组是
 * **整体替换** —— 每次写入都会新建 Y.Array，旧数组里所有 item 全部变成废弃墓碑
 * 永久留在文档里。Excalidraw 绘制时每个 pointermove 都触发一次 onChange，
 * 于是废弃 item 数量以平方级堆积，服务器的 applyUpdate / encodeStateAsUpdate /
 * 广播 / 落盘全都要带着这些垃圾走，文档越大越慢。
 *
 * 改成「一个紧凑字符串 = 1 个 CRDT item」之后，重写只废弃 1 个 item 而不是 N 个，
 * 文档体积与服务器开销降一到两个数量级。
 *
 * 格式：扁平整数数组 [x0,y0,x1,y1,...]，坐标取整（手绘笔迹 1px 精度足够）。
 */
function encodePoints(points: any): string | null {
  if (!Array.isArray(points) || points.length === 0) return null;
  const flat: number[] = new Array(points.length * 2);
  for (let i = 0; i < points.length; i += 1) {
    const p = points[i];
    if (Array.isArray(p)) {
      flat[i * 2] = Math.round(p[0] || 0);
      flat[i * 2 + 1] = Math.round(p[1] || 0);
    } else if (p && typeof p === "object") {
      flat[i * 2] = Math.round(p.x || 0);
      flat[i * 2 + 1] = Math.round(p.y || 0);
    } else {
      return null;
    }
  }
  return JSON.stringify(flat);
}

/**
 * 解码 points。任何异常都返回 null（调用方回退用原始数据，绝不丢笔迹）。
 * 兼容点里带 pressure 第三位的情况 —— 这里只取前两位 x/y。
 */
function decodePoints(s: any): [number, number][] | null {
  if (typeof s !== "string" || !s) return null;
  try {
    const flat = JSON.parse(s);
    if (!Array.isArray(flat) || flat.length < 2) return null;
    const out: [number, number][] = [];
    for (let i = 0; i + 1 < flat.length; i += 2) {
      out.push([flat[i], flat[i + 1]]);
    }
    return out;
  } catch {
    return null;
  }
}

/** 把 number[]（如 freedraw 的 pressures）编码成紧凑字符串，保留 2 位小数 */
function encodeNumbers(nums: any): string | null {
  if (!Array.isArray(nums) || nums.length === 0) return null;
  const out = new Array(nums.length);
  for (let i = 0; i < nums.length; i += 1) {
    const n = nums[i];
    if (typeof n !== "number" || !Number.isFinite(n)) return null;
    out[i] = Math.round(n * 100) / 100;
  }
  return JSON.stringify(out);
}

/**
 * 写 Yjs 前的元素转换：把「每个数字一个 CRDT item」的数组压成「一个字符串 = 一个 item」。
 *
 * - points:    [[x,y],...] -> pointsEnc: "x,y,x,y,..."
 * - pressures: [n,...]     -> pressuresEnc: "[n,n,...]"
 *   且 simulatePressure=true 时 pressures 全是常数 0.5，直接不写（读回时按需重建），
 *   否则一笔 200 点的笔迹光 pressures 就是 200 个 item。
 *
 * 返回新对象，绝不改动调用方传入的元素（Excalidraw 持有同一引用）。
 */
function prepareForWrite(el: Record<string, any>): Record<string, any> {
  let out = el;
  if (Array.isArray(el.points)) {
    const enc = encodePoints(el.points);
    if (enc) {
      if (out === el) out = { ...el };
      delete (out as any).points;
      (out as any).pointsEnc = enc;
    }
  }
  if (Array.isArray(el.pressures)) {
    if (out === el) out = { ...el };
    delete (out as any).pressures;
    if (el.simulatePressure !== true) {
      const enc = encodeNumbers(el.pressures);
      if (enc) (out as any).pressuresEnc = enc;
    }
  }
  return out;
}

/**
 * 读 Yjs 后的元素还原（prepareForWrite 的逆运算）。
 * 兼容旧数据：没有 pointsEnc 时保留原始的 points（旧文档里是 Y.Array，
 * toJSON() 出来就是 [[x,y],...]，Excalidraw 直接可用）。
 */
function restoreAfterRead(json: any): any {
  if (!json) return json;
  if (typeof json.pointsEnc === "string") {
    const pts = decodePoints(json.pointsEnc);
    if (pts) json.points = pts;
    delete json.pointsEnc;
  }
  if (typeof json.pressuresEnc === "string") {
    try {
      const arr = JSON.parse(json.pressuresEnc);
      if (Array.isArray(arr)) json.pressures = arr;
    } catch {
      /* 解析失败就留空，Excalidraw 会按默认压力渲染 */
    }
    delete json.pressuresEnc;
  }
  // simulatePressure 模式下的 pressures 没存（全是 0.5），这里补回来
  if (!json.pressures && json.simulatePressure === true && Array.isArray(json.points)) {
    json.pressures = json.points.map(() => 0.5);
  }
  return json;
}

function writeElement(m: AnyMap, el: Record<string, any>) {
  const src = prepareForWrite(el);
  for (const key of Object.keys(src)) {
    const value = (src as any)[key];
    if (value === undefined) continue;
    if (!sameValue(m.get(key), value)) m.set(key, value);
  }
  for (const key of Array.from(m.keys())) {
    if (!(key in src) || (src as any)[key] === undefined) m.delete(key);
  }
}

/**
 * 用 canvas 把大图压成 JPEG dataURL：最长边限制 + 质量递降 + 必要时再缩尺寸，
 * 直到结果不超过 targetChars。PNG 透明区会被铺上白底（JPEG 不支持透明）。
 * 任何一步失败或压完反而更大都返回 null（调用方退回原图）。
 */
async function compressDataURL(
  source: string,
  maxEdge: number,
  targetChars: number
): Promise<{ dataURL: string; mimeType: string } | null> {
  if (typeof document === "undefined") return null;
  const img = await new Promise<HTMLImageElement | null>((resolve) => {
    const im = new Image();
    im.onload = () => resolve(im);
    im.onerror = () => resolve(null);
    im.src = source;
  });
  if (!img) return null;
  let w = img.naturalWidth || img.width;
  let h = img.naturalHeight || img.height;
  if (!w || !h) return null;

  const canvas = document.createElement("canvas");
  const ctx = canvas.getContext("2d");
  if (!ctx) return null;

  const redraw = (cw: number, ch: number) => {
    canvas.width = cw;
    canvas.height = ch;
    ctx.fillStyle = "#ffffff";
    ctx.fillRect(0, 0, cw, ch);
    ctx.drawImage(img, 0, 0, cw, ch);
  };

  const scale = Math.min(1, maxEdge / Math.max(w, h));
  w = Math.max(1, Math.round(w * scale));
  h = Math.max(1, Math.round(h * scale));
  redraw(w, h);

  let quality = 0.85;
  let out = canvas.toDataURL("image/jpeg", quality);
  while (out.length > targetChars && quality > 0.4) {
    quality -= 0.15;
    out = canvas.toDataURL("image/jpeg", quality);
  }
  // 质量降到底还超：逐步缩尺寸（保底 480px，再小就看不清了）
  while (out.length > targetChars && Math.max(w, h) > 480) {
    w = Math.max(1, Math.round(w * 0.8));
    h = Math.max(1, Math.round(h * 0.8));
    redraw(w, h);
    out = canvas.toDataURL("image/jpeg", Math.max(quality, 0.5));
  }
  // 压缩结果必须真的更小才有意义
  return out.length < source.length ? { dataURL: out, mimeType: "image/jpeg" } : null;
}

// 只在元素被显式 isDeleted=true 时才同步从 yOrder 移除，避免多人协作下
//   「对方的草稿/未提交笔触被我本地不可见而误删顺序位置」造成的元素丢失
function reconcileOrder(yOrder: Y.Array<string>, target: string[]) {
  const cur = yOrder.toArray();
  const curSet = new Set(cur);

  // 1. 补齐缺失项（一次性 push，不要逐个 push —— 逐个 push 每次都要重建索引）
  //    pushed 去重：实测线上房间 order 里同一个 id 被重复插入了 5236 次
  //    （target 里带重复 / 并发回声都可能导致），不去重的话 yOrder 会无限膨胀，
  //    每次 toArray() 都是几千项，客户端和服务器一起变慢。
  const missing: string[] = [];
  const pushed = new Set<string>();
  for (const id of target) {
    if (curSet.has(id) || pushed.has(id)) continue;
    pushed.add(id);
    missing.push(id);
  }
  if (missing.length) yOrder.push(missing);

  // 2. 目标顺序 = target + (cur 里不在 target 的项，原样留在尾部)
  //    「cur 里有但这一帧 target 里没有」= 对方正在编辑/未提交的草稿，绝不能删
  const targetSet = new Set(target);
  const tail: string[] = [];
  for (const id of cur) if (!targetSet.has(id)) tail.push(id);
  const want = target.concat(tail);
  const actual = missing.length ? cur.concat(missing) : cur;

  // 3. 顺序已经一致 → 一次 Yjs 操作都不做。
  //    这是常态（拖动/改属性不改 z 序），旧实现在这里也会跑 O(n²) 的 toArray()
  let same = actual.length === want.length;
  if (same) {
    for (let i = 0; i < want.length; i += 1) {
      if (actual[i] !== want[i]) {
        same = false;
        break;
      }
    }
  }
  if (same) return;

  // 4. 真需要重排（用户改了层级）：最小移动，把 want[i] 从当前位置挪到 i
  const arr = actual.slice();
  const pos = new Map<string, number>();
  for (let i = 0; i < arr.length; i += 1) pos.set(arr[i], i);
  for (let i = 0; i < want.length; i += 1) {
    const w = want[i];
    const j = pos.get(w);
    if (j === undefined || j === i) continue;
    yOrder.delete(j, 1);
    yOrder.insert(i, [w]);
    arr.splice(j, 1);
    arr.splice(i, 0, w);
    const lo = Math.min(i, j);
    const hi = Math.max(i, j);
    for (let k = lo; k <= hi; k += 1) pos.set(arr[k], k);
  }
}

export type BoardCollabOptions = {
  doc: Y.Doc;
  api: any;
  awareness?: any;
  readOnly?: boolean;
};

export class BoardCollab {
  private doc: Y.Doc;
  private api: any;
  private awareness: any;
  private readOnly: boolean;

  private yElements: Y.Map<AnyMap>;
  private yOrder: Y.Array<string>;
  /** 已解析元素的本地镜像 Map<id, element>：避免每次远端变更都全量 toJSON+JSON.parse(pointsEnc)。
   *  远端只有"被改的那个元素"会重新解析进镜像，刷新场景时只克隆引用，
   *  单次远端变更代价从 O(全场景×点数) 降到 O(变更元素)。 */
  private elementMirror = new Map<string, any>();
  /** 镜像对应的 z 序缓存（yOrder 的本地副本），刷新场景时按它排序输出。 */
  private orderCache: string[] = [];
  /** rAF/定时器批处理开关：把一帧内的多次远端/本地变更合并成一次 updateScene。 */
  private flushScheduled = false;
  private flushHandle: any = null;
  private destroyed = false;
  private maintenanceTimer: ReturnType<typeof setInterval> | null = null;
  private lastSelectionKey = "";
  private loadingFiles = new Set<string>();
  private failedFiles = new Map<string, number>();
  private fileLoadQueue: Promise<any> = Promise.resolve();
  /** 图片二进制共享表：fileId -> { id, mimeType, dataURL, created }。
   *  Excalidraw 的图片元素只存 fileId 引用，二进制在组件内部 files 表里
   *  （不进场景元素）——不单独同步的话，其他客户端只有引用没有本体，
   *  图片渲染成占位灰块。 */
  private yFiles: Y.Map<AnyMap>;

  private lastSnapshot = new Map<string, ReturnType<typeof snapshotOf>>();
  private localTransaction = false;
  private applyingRemote = false;
  private unobserve: (() => void) | null = null;
  private mindmap: MindMap | null = null;
  /** 本地正在画笔（freedraw）期间，需要跳过 in-progress 元素的中间帧写入 Yjs。
   *  采用"按 id 追踪"策略，比单一 flag 更稳——mouseup finalize 后会清空追踪。 */
  private inProgressFreedrawIds = new Set<string>();
  /** pushRemoteToScene 刚推过来的元素 id 集合——下一帧 handleChange 看到这些 id 必须跳过写 Yjs，
   *  否则 Excalidraw updateScene 内部 mutate（version/versionNonce 变化）触发的 onChange
   *  会把远端元素原封不动写回 Yjs，造成 A→B→A→B 反馈循环。 */
  private remoteJustSyncedIds: Set<string> | null = null;
  /** 上一次远端 observe 触发时的 origin clientID（用于区分远端写 vs 自己写）。 */
  private lastRemoteOrigin: number | null = null;
  /** 本帧刚刚 finalize 了一个本地 freedraw，下一次 pushRemoteToScene 必须把 finalize 元素推回。 */
  private localDrawingJustFinished = false;
  /** 指针是否正按下（freedraw 绘制中）。setDrawing(true/false) 维护。
   *  只有按下期间到达的 freedraw onChange 才当作"in-progress"跳过写入；
   *  松手之后到达的帧（如 Excalidraw mouseup finalize 晚于 onPointerUp 回调）
   *  走正常写入路径，否则最终笔迹永远进不了 Yjs（松手即消失）。 */
  private drawingActive = false;
  /** in-progress freedraw 最后一次 onChange 的元素快照（兜底用：
   *  松手时场景里已找不到该 id 的话，用它写入 Yjs，避免笔迹丢失）。 */
  private lastInProgressFreedrawEl: any = null;

  /** 本地撤销栈（Y.UndoManager，只追踪 LOCAL_ORIGIN 事务，最多 5 步） */
  private undoManager: Y.UndoManager | null = null;
  /** setBrush 节流：待发送的 brush 与定时器 */
  private pendingBrush: any = null;
  private brushTimer: ReturnType<typeof setTimeout> | null = null;
  private lastBrushAt = 0;

  /** 已注入 Excalidraw 的图片 id（避免重复 addFiles 触发缓存失效/重渲染风暴） */
  private appliedFileIds = new Set<string>();
  /** 正在异步压缩的图片 id（防止后续 onChange 帧重复发起压缩） */
  private pendingCompressIds = new Set<string>();
  /** dataURL 超过此字符数（≈1.1MB 图片）触发自动压缩后再同步——
   *  本地保留原图，远端收压缩版，任意大小的照片/截图都能协作。 */
  private static COMPRESS_THRESHOLD_CHARS = 1.5 * 1024 * 1024;
  /** 压缩后目标字符数（略低于阈值，保证压完达标） */
  private static COMPRESS_TARGET_CHARS = 1.4 * 1024 * 1024;
  /** 压缩时保留的最长边像素 */
  private static COMPRESS_MAX_EDGE = 2200;
  /** 不可压缩格式（GIF 动图 / SVG 矢量）的 dataURL 兜底上限（字符数）。
   *  压缩会丢动画帧/矢量信息，这些格式跳过压缩直接同步原图，超此值放弃。 */
  private static MAX_FILE_CHARS = 8 * 1024 * 1024;

  /** canvas 压缩可安全处理的位图格式（GIF/SVG 之外） */
  private static isCompressible(mimeType: string): boolean {
    return (
      mimeType === "image/png" ||
      mimeType === "image/jpeg" ||
      mimeType === "image/webp" ||
      mimeType === "image/bmp"
    );
  }

  constructor({ doc, api, awareness, readOnly = false }: BoardCollabOptions) {
    this.doc = doc;
    this.api = api;
    this.awareness = awareness;
    this.readOnly = readOnly;
    this.yElements = doc.getMap("elements") as Y.Map<AnyMap>;
    this.yOrder = doc.getArray("order") as Y.Array<string>;
    this.yFiles = doc.getMap("files") as Y.Map<AnyMap>;

    // 本地撤销栈：只认 LOCAL_ORIGIN 写入的事务。
    // 远端同步过来的变化 origin 是 Hocuspocus provider，不在这个集合里，
    // 因此「我按 Ctrl+Z」只会回滚我自己写的东西，别人画的内容不会被我撤销掉。
    // 只追踪 elements + order：图片二进制不进栈，避免大 blob 被反复回滚。
    this.undoManager = new Y.UndoManager([this.yElements, this.yOrder], {
      trackedOrigins: new Set([LOCAL_ORIGIN]),
      // 300ms 内的连续写入合并成一步（拖动/连续绘制不会被拆成几十步撤销）
      captureTimeout: 300,
    });
    // 只保留最近 5 步：超出就丢掉最老的一步
    this.undoManager.on("stack-item-added", () => {
      const stack = this.undoManager?.undoStack;
      if (stack && stack.length > UNDO_MAX_STEPS && this.undoManager) trimUndoHistory(this.doc, this.undoManager, UNDO_MAX_STEPS);
    });
  }

  /**
   * 自愈：清掉 yOrder 里的重复项（保留每个 id 的首次出现位置）。
   *
   * 历史房间被旧版 reconcileOrder 的重复插入 bug 污染过 —— 实测 1.ydoc 里
   * order 有 6112 项但只有 3 个元素，单个 id 重复 5236 次。这些垃圾是**永久性**的：
   * 每次 yOrder.toArray() 都要遍历几千项，客户端绘制每一帧都在做无用功。
   *
   * 幂等：多个客户端同时执行，删的都是同一批"第 2 次及以后出现的"项，结果一致。
   * 无重复时一次 Yjs 写操作都不产生。
   */
  private compactOrder() {
    const arr = this.yOrder.toArray();
    const seen = new Set<string>();
    const drop: number[] = [];
    for (let i = 0; i < arr.length; i += 1) {
      if (seen.has(arr[i])) drop.push(i);
      else seen.add(arr[i]);
    }
    if (!drop.length) return;
    // 从后往前删，索引不会偏移
    for (let k = drop.length - 1; k >= 0; k -= 1) this.yOrder.delete(drop[k], 1);
    if (typeof window !== "undefined") {
      console.log(`[order] 清理重复项 ${drop.length} 个（${arr.length} -> ${arr.length - drop.length}）`);
    }
  }

  start() {
    // 先自愈历史脏数据，再注册观察者（避免自己触发一轮 pushRemoteToScene）
    this.compactOrder();
    // 全量重建镜像（迟到加入者：把当前 Yjs 场景一次性灌进本地镜像）
    this.rebuildMirror();

    // 元素内容变更：只把"被改的那个元素"增量解析进镜像，再统一帧刷新。
    // observeDeep 的 event.path[0] 即该元素在 yElements 里的 key（id）。
    const onElementsRemote = (events: any[], transaction: Y.Transaction) => {
      if (this.localTransaction) return;
      this.lastRemoteOrigin =
        typeof transaction.origin === "number" ? transaction.origin : null;
      const changed = new Set<string>();
      for (const ev of events) {
        const id = ev.path && ev.path[0];
        if (typeof id === "string") changed.add(id);
        else if (ev.keysChanged) for (const key of ev.keysChanged) changed.add(key);
      }
      for (const id of changed) this.updateMirrorForId(id);
      this.scheduleFlush();
    };
    // 顺序变更：重建 orderCache（低频），再帧刷新
    const onOrderRemote = (_event: any, transaction: Y.Transaction) => {
      if (this.localTransaction) return;
      this.orderCache = this.yOrder.toArray();
      this.scheduleFlush();
    };
    // 图片二进制：远端写入 yFiles 时注入本地 Excalidraw
    const onFilesRemote = () => {
      if (this.localTransaction) return;
      this.applyFilesToScene();
      this.refreshImageCache();
      this.scheduleFlush();
    };
    this.yElements.observeDeep(onElementsRemote);
    this.yOrder.observe(onOrderRemote as any);
    this.yFiles.observe(onFilesRemote as any);
    this.unobserve = () => {
      this.yElements.unobserveDeep(onElementsRemote);
      this.yOrder.unobserve(onOrderRemote as any);
      this.yFiles.unobserve(onFilesRemote as any);
    };
    // 迟到加入者：先把场景推回来（元素先落地），再把历史图片二进制注入，
    // 最后刷新 imageCache，这样不会闪占位灰块。
    this.applyFilesToScene(true);
    this.refreshImageCache();
    this.pushRemoteToScene();
    this.mindmap = new MindMap(this.doc, this.api);
    this.maintenanceTimer = setInterval(() => this.maintainMemory(), 2000);
  }

  /** 重新建立 observe（断线重连后 Yjs 增量同步可能漏掉一些变更，
   * 这里主动触发一次远端推回，强制把服务端当前场景写回 Excalidraw）。 */
  restart() {
    // 重连后重建镜像（增量观察可能漏掉一些远端变更），再统一推回场景
    this.rebuildMirror();
    this.pushRemoteToScene();
    this.applyFilesToScene(true);
    this.refreshImageCache();
  }

  destroy() {
    this.destroyed = true;
    if (this.maintenanceTimer) clearInterval(this.maintenanceTimer);
    if (this.flushHandle != null) {
      if (typeof cancelAnimationFrame !== "undefined") cancelAnimationFrame(this.flushHandle);
      else clearTimeout(this.flushHandle);
    }
    this.undoManager?.clear();
    if (this.brushTimer) {
      clearTimeout(this.brushTimer);
      this.brushTimer = null;
    }
    this.pendingBrush = null;
    this.undoManager?.destroy();
    this.undoManager = null;
    this.unobserve?.();
    this.unobserve = null;
    this.mindmap = null;
    this.lastSnapshot.clear();
    this.appliedFileIds.clear();
    this.pendingCompressIds.clear();
    this.loadingFiles.clear();
    this.failedFiles.clear();
    this.lastInProgressFreedrawEl = null;
    this.remoteJustSyncedIds = null;
    this.flushScheduled = false;
    this.elementMirror.clear();
    this.orderCache = [];
  }

  /** 清空整块画布（所有客户端同步生效） */
  clear() {
    this.doc.transact(() => {
      for (const id of Array.from(this.yElements.keys())) this.yElements.delete(id);
      this.yOrder.delete(0, this.yOrder.length);
      // mindmap 关系表也必须清：否则关系还在、元素没了，
      // 下一次 relayout 会把「已清空」的旧节点全部重建出来
      this.doc.getMap("mindmap").clear();
      // 图片二进制一起清（元素已删，留着只会占同步带宽）
      this.yFiles.clear();
      this.lastSnapshot.clear();
    }, LOCAL_ORIGIN);
    // 本地事务不触发 observer，手动让镜像与 Yjs 一致，再合并刷新
    this.rebuildMirror();
    this.scheduleFlush();
  }

  /** 把 yFiles 里还没注入的图片二进制 addFiles 进本地 Excalidraw。
   *  幂等：本地 Excalidraw.files 里已有同 id 则跳过。 */
  getFileMetadata(): Record<string, any> { return this.yFiles.toJSON(); }

  private imageNearViewport(el: any): boolean {
    const app = this.api?.getAppState?.();
    if (!app?.width || !app?.height) return true;
    const z = app.zoom?.value || 1;
    const margin = 320 / z;
    const left = -app.scrollX - margin, top = -app.scrollY - margin;
    const right = left + app.width / z + 2 * margin, bottom = top + app.height / z + 2 * margin;
    const angle = el.angle || 0;
    const w = Math.abs((el.width || 0) * Math.cos(angle)) + Math.abs((el.height || 0) * Math.sin(angle));
    const h = Math.abs((el.width || 0) * Math.sin(angle)) + Math.abs((el.height || 0) * Math.cos(angle));
    const cx = el.x + el.width / 2, cy = el.y + el.height / 2;
    return cx + w / 2 >= left && cx - w / 2 <= right && cy + h / 2 >= top && cy - h / 2 <= bottom;
  }

  private applyFilesToScene(force = false) {
    if (this.destroyed || !this.api?.addFiles) return;
    const localFiles = this.api.getFiles?.() || {};
    const needed = new Set<string>();
    for (const el of this.elementMirror.values()) if (el.type === "image" && el.fileId && !el.isDeleted && this.imageNearViewport(el)) needed.add(el.fileId);
    const batch: any[] = [];
    for (const fid of needed) {
      if (localFiles[fid] || this.loadingFiles.has(fid)) continue;
      const yf = this.yFiles.get(fid);
      if (!yf) continue;
      const meta = { id: fid, mimeType: yf.get("mimeType") || "image/png", created: yf.get("created") || Date.now() };
      const dataURL = yf.get("dataURL");
      if (dataURL) batch.push({ ...meta, dataURL });
      else if (yf.get("assetId") && Date.now() - (this.failedFiles.get(fid) || 0) > 10000) {
        const assetId = yf.get("assetId");
        this.loadingFiles.add(fid);
        this.fileLoadQueue = this.fileLoadQueue.then(async () => {
          if (this.destroyed) return;
          try {
            const url = await fetchAsset(assetId);
            if (!this.destroyed && this.yFiles.has(fid) && Array.from(this.elementMirror.values()).some(el => el.fileId === fid && this.imageNearViewport(el))) { this.api.addFiles([{ ...meta, dataURL: url }]); this.failedFiles.delete(fid); }
          } catch (err) { this.failedFiles.set(fid, Date.now()); console.warn("[files]", err); }
          finally { this.loadingFiles.delete(fid); }
        });
      }
    }
    if (batch.length) this.api.addFiles(batch);
  }

  private maintainMemory() {
    if (this.destroyed || this.drawingActive) return;
    const state = this.api?.getAppState?.();
    if (state?.editingTextElement || state?.editingLinearElement || state?.draggingElement || state?.resizingElement) return;
    // Only Yjs owns undo; release the second full-scene history at idle boundaries.
    this.api?.history?.clear?.();
    const scene = this.api?.getSceneElementsIncludingDeleted?.() || [];
    if (scene.some((el: any) => el.isDeleted)) {
      this.applyingRemote = true;
      try { this.api.updateScene({ elements: scene.filter((el: any) => !el.isDeleted), commitToHistory: false }); }
      finally { this.applyingRemote = false; }
    }
    const needed = undoFileIds(this.doc, this.undoManager);
    for (const el of this.elementMirror.values()) if (el.fileId && !el.isDeleted && this.imageNearViewport(el)) needed.add(el.fileId);
    for (const fid of this.pendingCompressIds) needed.add(fid);
    for (const el of this.elementMirror.values()) if (el.fileId && !this.yFiles.has(el.fileId)) needed.add(el.fileId);
    const stale = Object.keys(this.api?.getFiles?.() || {}).filter(fid => !needed.has(fid));
    if (stale.length) this.api?.pruneFiles?.(stale);
    this.applyFilesToScene();
  }

  /**
   * 触发 Excalidraw 重新扫描场景里的 image 元素并加载 imageCache。
   * 文件注入后若 image 元素尚未就位，二进制只进了 files 表、没进 imageCache，
   * 远端用户会永远看到灰占位块；等场景元素到齐后调用本方法即可渲染。
   * 传空数组会强制走 addFiles → addNewImagesToImageCache() 分支，不会重复写入文件。
   */
  private refreshImageCache() {
    const api = this.api;
    if (!api || typeof api.addFiles !== "function") return;
    try {
      api.addFiles([]);
    } catch (err) {
      console.warn("[files] refreshImageCache 失败", err);
    }
  }

  /**
   * 大图压缩后写入 yFiles（异步，不阻塞 handleChange 的 transact）。
   * 压缩失败或压完反而更大时退回原图；原图超兜底上限才放弃同步。
   * 本地 Excalidraw 继续用原图渲染（不降质），远端收到的 dataURL 是压缩版。
   */
  private async compressAndShareFile(
    fid: string,
    file: { dataURL: string; mimeType?: string; created?: number }
  ) {
    try {
      let dataURL = file.dataURL;
      let mimeType = String(file.mimeType || "image/png");
      let compressed = false;
      try {
        const result = file.dataURL.length > BoardCollab.COMPRESS_THRESHOLD_CHARS ? await compressDataURL(
          file.dataURL,
          BoardCollab.COMPRESS_MAX_EDGE,
          BoardCollab.COMPRESS_TARGET_CHARS
        ) : null;
        if (result) {
          dataURL = result.dataURL;
          mimeType = result.mimeType;
          compressed = true;
        }
      } catch (err) {
        console.warn("[files] 压缩异常，尝试按原图同步", fid.slice(0, 8), err);
      }
      if (dataURL.length > BoardCollab.MAX_FILE_CHARS) {
        console.warn(
          "[files] 图片压缩后仍过大，跳过协作同步（其他端将看不到它）：",
          fid.slice(0, 8),
          Math.round(dataURL.length / 1024) + "KB"
        );
        return;
      }
      const assetId = await uploadAsset(dataURL, mimeType);
      if (this.destroyed || !Array.from(this.yElements.values()).some(el => el.get("fileId") === fid && !el.get("isDeleted"))) return;
      // 【关键顺序】必须在写 Yjs **之前**把 fid 记为「已应用」：
      // 这次 transact 会触发本地 yFiles 观察者 → applyFilesToScene()，
      // 若此时 fid 还没进 appliedFileIds，本地会把**压缩版** addFiles 回自己的
      // Excalidraw，把用户刚传的高清原图替换掉（远端没影响，但本地画质被偷偷降了）。
      this.appliedFileIds.add(fid);
      this.localTransaction = true;
      try {
        this.doc.transact(() => {
          if (this.yFiles.has(fid)) return; // 期间已被其他路径写入（如另一个客户端）
          const yf = new Y.Map();
          yf.set("id", fid);
          yf.set("mimeType", mimeType);
          yf.set("assetId", assetId);
          yf.set("created", file.created || Date.now());
          this.yFiles.set(fid, yf);
        });
      } finally {
        this.localTransaction = false;
      }
      if (typeof window !== "undefined") {
        console.log(
          "[files] 本地图片写入 Yjs:",
          fid.slice(0, 8),
          compressed
            ? `已压缩 ${Math.round(file.dataURL.length / 1024)}KB -> ${Math.round(dataURL.length / 1024)}KB`
            : Math.round(dataURL.length / 1024) + "KB"
        );
      }
    } finally {
      this.pendingCompressIds.delete(fid);
    }
  }

  getMindmap(): MindMap | null {
    return this.mindmap;
  }

  // ---------------- 本地撤销 / 重做 ----------------
  //
  // 设计要点（对应用户需求：A 画苹果、B 放图片，B 撤销不能动 A 的内容）：
  //  1. 撤销栈只追踪 origin === LOCAL_ORIGIN 的事务，也就是"我自己写进去的"。
  //     远端同步过来的变化 origin 是 provider，根本不进栈，所以撤销永远碰不到别人的东西。
  //  2. 栈数据只在浏览器内存里，不写 Yjs、不上服务器 —— 服务器只看到一条
  //     "这条元素被删/改" 的增量，看不到任何历史记录。
  //  3. 最多 5 步，超出丢最老的。

  /** 还能撤销几步（0 = 撤不动了） */
  undoDepth(): number {
    return this.undoManager?.undoStack.length ?? 0;
  }

  /** 还能重做几步 */
  redoDepth(): number {
    return this.undoManager?.redoStack.length ?? 0;
  }

  /**
   * 撤销一步本端操作。
   *
   * Y.UndoManager 内部会用「UndoSanager 自身」作为 origin 发起一个新事务，
   * 这个 origin 不在 trackedOrigins 里，所以撤销本身不会再进撤销栈（会进重做栈）。
   * 产生的数据变化会正常广播出去 —— 别人看到"我的东西没了"，但别人的东西一个字节都不动。
   */
  undo(): boolean {
    if (this.readOnly) return false;
    const um = this.undoManager;
    if (!um || !um.undoStack.length) return false;
    try {
      um.undo();
    } catch (err) {
      console.warn("[undo] 撤销失败", err);
      return false;
    }
    // UndoManager 已通过 observer 增量更新镜像；这里兜底重建一次保证一致，并合并刷新。
    this.rebuildMirror();
    this.scheduleFlush();
    return true;
  }

  /** 重做一步（撤销的反向操作，同样只作用于本端记录过的操作） */
  redo(): boolean {
    if (this.readOnly) return false;
    const um = this.undoManager;
    if (!um || !um.redoStack.length) return false;
    try {
      um.redo();
    } catch (err) {
      console.warn("[redo] 重做失败", err);
      return false;
    }
    this.rebuildMirror();
    this.scheduleFlush();
    return true;
  }

  /** 清空本地撤销/重做栈（导入大量内容后调用，避免一次 Ctrl+Z 就把整批导入撤掉心智负担过重时可用） */
  clearHistory() {
    this.undoManager?.clear();
  }

  // ---------------- 批量导入 ----------------

  /**
   * 一次性把一批元素写进 Yjs（导入画板 / 导入 Excel 专用）。
   *
   * 与常规 handleChange 的区别：
   *  - 只跑 **一个** Yjs 事务（而不是每帧一个），服务器只收到一条 update；
   *  - 完全跳过 reconcileOrder 的逐项比对，新元素一次性 push 进 yOrder；
   *  - 不经过 api.updateScene → 不会触发 Excalidraw 逐帧 diff，UI 不会卡；
   *  - 写完后用 pushRemoteToScene() 一次性把 Yjs 结果推回场景（这一帧 applyingRemote=true，
   *    引发的 onChange 被忽略，不会二次写回）。
   *
   * 整批导入在撤销栈里算 **一步**（captureTimeout 内的连续写入会合并），Ctrl+Z 整批撤销。
   */
  bulkWrite(elements: readonly any[]) {
    if (this.readOnly || !elements.length) return;
    const ids: string[] = [];
    const seen = new Set<string>();

    this.localTransaction = true;
    try {
      this.doc.transact(() => {
        for (const el of elements) {
          if (!el || !el.id || seen.has(el.id)) continue;
          seen.add(el.id);
          if (el.isDeleted === true) {
            if (this.yElements.has(el.id)) this.yElements.delete(el.id);
            this.lastSnapshot.delete(el.id);
            continue;
          }
          let m = this.yElements.get(el.id);
          if (!m) {
            m = new Y.Map();
            this.yElements.set(el.id, m);
          }
          writeElement(m, el);
          this.lastSnapshot.set(el.id, snapshotOf(el));
          ids.push(el.id);
        }

        // 顺序：只补缺失项，一次 push 搞定（不做全量重排）
        if (ids.length) {
          const curSet = new Set(this.yOrder.toArray());
          const missing = ids.filter((id) => !curSet.has(id));
          if (missing.length) this.yOrder.push(missing);
        }
      }, LOCAL_ORIGIN);
    } finally {
      this.localTransaction = false;
    }

    // 一次性把结果推回 Excalidraw（期间 applyingRemote=true，回声 onChange 被忽略）
    this.rebuildMirror();
    this.scheduleFlush();
    if (typeof window !== "undefined") {
      console.log("[bulkWrite] 批量写入", ids.length, "个元素（单事务）");
    }
  }

  /** 本地场景变化 -> 写入 Yjs */
  handleChange(elements: readonly any[]) {
    if (this.readOnly || this.applyingRemote) return;

    this.localTransaction = true;
    try {
      this.doc.transact(() => {
        const seen = new Set<string>();

        for (const el of elements) {
          if (!el || !el.id) continue;
          seen.add(el.id);

          // 【反馈循环防护】远端刚推过来的元素（updateScene 触发了 Excalidraw 内部
          // mutate，version/versionNonce 变化导致新一轮 onChange），这里跳过写回 Yjs，
          // 避免 A→B→A→B 死循环。这个集合由 pushRemoteToScene 在下一帧清空。
          if (this.remoteJustSyncedIds?.has(el.id)) {
            this.lastSnapshot.set(el.id, snapshotOf(el));
            // 远端刚推过来的元素也同步进本地镜像，保证后续 flush 不会用旧镜像覆盖它
            this.elementMirror.set(el.id, { ...el });
            continue;
          }

          // 被显式删除的元素：把它从 yElements 删掉（按 isDeleted 语义才删，避免误删草稿）
          if (el.isDeleted === true) {
            if (this.yElements.has(el.id)) this.yElements.delete(el.id);
            this.lastSnapshot.delete(el.id);
            this.elementMirror.delete(el.id);
            continue;
          }

          // 【并发画笔修复】只有"指针按下期间"（drawingActive）到达的 freedraw 才算
          // in-progress、跳过 Yjs 写入。且只追踪「还没进 Yjs 的新元素」——
          // 场景里已有的远端/旧笔迹（yElements 里有）绝不能进追踪集合，
          // 否则别人画过的线会被当成"我正在画的"：finalize 反复重写、
          // 远端更新（拖动/删除）还会被 merge 分支用陈旧的本地引用挡掉。
          if (el.type === "freedraw" && this.drawingActive) {
            const tracked = this.inProgressFreedrawIds.has(el.id);
            if (tracked || !this.yElements.has(el.id)) {
              if (!tracked) {
                if (typeof window !== "undefined") console.log("[press track]", el.id.slice(0,6), "ver=", el.version);
                this.inProgressFreedrawIds.add(el.id);
              }
              this.lastInProgressFreedrawEl = el;
              continue;
            }
            // 已在 Yjs 的旧笔迹 → 走正常路径（无变化时快照比较会跳过）
          }

          const prev = this.lastSnapshot.get(el.id);
          if (
            prev &&
            prev.version === el.version &&
            prev.versionNonce === el.versionNonce &&
            prev.updated === el.updated
          ) {
            continue;
          }

          let m = this.yElements.get(el.id);
          if (!m) {
            m = new Y.Map();
            this.yElements.set(el.id, m);
          }
          // 导图节点位置变了 = 被手动拖动：打 manual 标记，
          // 之后 relayout 会尊重这个位置（不再 dagre 弹回）。
          // 放在快照判断之后，远端回声/自己 relayout 的写回不会误标
          if (el.type === "rectangle") {
            const eid = String(el.id);
            if (eid.startsWith("n_") && !eid.endsWith("_label")) {
              const sx = m.get("x");
              const sy = m.get("y");
              if (
                typeof sx === "number" &&
                typeof sy === "number" &&
                (sx !== el.x || sy !== el.y)
              ) {
                this.mindmap?.markManual(eid);
              }
            }
          }
          writeElement(m, el);
          this.lastSnapshot.set(el.id, snapshotOf(el));
          // 本地写入同步进镜像，避免后续远端 flush 用旧镜像把本端刚做的修改覆盖回去
          this.elementMirror.set(el.id, { ...el });
        }

        // 【Esc 取消检测】in-progress 元素直接从场景数组消失 = 笔迹被取消
        //（Excalidraw 对 Escape 取消的 freedraw 不打 isDeleted 标记，直接移除）。
        // 必须立刻摘掉追踪，否则 finalize 兜底逻辑会把已取消的笔迹"复活"写进 Yjs。
        // 二次确认以活场景为准：onChange 偶尔可能只给子集，避免误判取消。
        if (this.inProgressFreedrawIds.size > 0) {
          for (const id of Array.from(this.inProgressFreedrawIds)) {
            if (seen.has(id)) continue;
            const live = this.api?.getSceneElementsIncludingDeleted?.()?.find?.((e: any) => e.id === id);
            if (live) continue; // 活场景里还在——只是这一帧没带上，不算取消
            this.inProgressFreedrawIds.delete(id);
            if (this.lastInProgressFreedrawEl?.id === id) {
              this.lastInProgressFreedrawEl = null;
            }
          }
        }

        for (const id of Array.from(this.yElements.keys())) {
          if (!seen.has(id)) {
            // ⚠️ 多人协作时不要根据「本地 elements 不含某 id」来删除 Yjs 元素：
            //   本地 onChange 的 elements 数组只反映当前场景快照，而其他人可能
            //   正在编辑/拖动/绘制同一画布，他们的元素这一帧可能没出现在我们的快照里
            //   （Excalidraw 在某些时机回调只含 subset）。简单「删除未出现的」会
            //   把对方未完成的笔触/形状擦掉，造成「抢画笔/断点」。
            //   真正删除仅在元素被显式标记 isDeleted=true 时进行
            //   （isDeleted 是显式语义，与「这一帧不可见」是两回事）。
            const existing = this.yElements.get(id);
            const isDeleted = existing?.get?.("isDeleted") === true;
            if (isDeleted) {
              this.yElements.delete(id);
              this.lastSnapshot.delete(id);
              this.elementMirror.delete(id);
            }
          }
        }

        // 【图片二进制同步】本地新出现的图片元素（fileId 还不在 yFiles）→ 把
        // Excalidraw 内部 files 里的二进制写进 yFiles。小图与元素写入在同一
        // transact 内（远端原子收到「元素 + 二进制」）；大图异步压缩后单独写入。
        // ⚠️ 判定条件只看 **fileId 是否已经回填**：
        //    Excalidraw 0.17.6 里图片元素的 status 建出来是 "pending"，插入成功后
        //    也**只 mutate fileId，不会改成 "saved"**（`isInitializedImageElement`
        //    本身就只判 `!!element.fileId`，压根不看 status）。
        //    旧代码写的 `el.status !== "saved"` 永远为真 → 二进制一次都没同步过 →
        //    远端只有引用没有本体，图片渲染成占位灰块（「图片在远端加载不出来」的根因）。
        //    占位期（fileId 为 null）本来就会被 `!fid` 挡掉，不需要 status 帮忙。
        const newFileIds = new Set<string>();
        for (const el of elements) {
          if (!el || el.type !== "image" || el.isDeleted) continue;
          const fid = el.fileId;
          if (!fid || this.yFiles.has(fid) || this.pendingCompressIds.has(fid)) {
            continue;
          }
          newFileIds.add(fid);
        }
        for (const fid of newFileIds) {
          const f = this.api?.getFiles?.()?.[fid];
          if (!f || !f.dataURL) continue;
          const mt = String(f.mimeType || "image/png");
          const oversize = f.dataURL.length > BoardCollab.MAX_FILE_CHARS;
          if (oversize && !BoardCollab.isCompressible(mt)) {
            // GIF/SVG 等不可压缩格式且超兜底上限：无法既保内容又同步
            console.warn(
              "[files] 图片过大且格式不可压缩，跳过协作同步（其他端将看不到它）：",
              fid.slice(0, 8),
              mt,
              Math.round(f.dataURL.length / 1024) + "KB"
            );
            continue;
          }
          if (
            f.dataURL.length > BoardCollab.COMPRESS_THRESHOLD_CHARS &&
            BoardCollab.isCompressible(mt)
          ) {
            // 大图：异步压缩（canvas 缩边 + JPEG 质量递降）后写入，
            // 不阻塞本帧 transact；压缩期间 pendingCompressIds 防重复触发
            this.pendingCompressIds.add(fid);
            void this.compressAndShareFile(fid, f).catch(err => console.warn("[files]", err));
            continue;
          }
          this.pendingCompressIds.add(fid);
          void this.compressAndShareFile(fid, f).catch(err => {
            console.warn("[files] 图片上传失败", err);
            this.api?.setToast?.({ message: "图片尚未同步，请检查本地服务连接" });
          });
        }

        // 【性能/B】纯属性编辑（移动/改色/改文字）不改 z 序，跳过本次 O(n) 的 reconcileOrder；
        // 仅在结构变化（增/删元素、或 onChange 子集回调长度不符）时才重排，并同步 orderCache。
        const incIds = elements.filter((e) => e && e.id && !e.isDeleted).map((e) => e.id);
        const deletedIds = new Set(elements.filter(e => e?.isDeleted).map(e => e.id));
        if (deletedIds.size) {
          const ordered = this.yOrder.toArray();
          for (let i = ordered.length - 1; i >= 0; i--) if (deletedIds.has(ordered[i])) this.yOrder.delete(i, 1);
        }
        const orderSet = new Set(this.orderCache);
        let structuralChange = incIds.length !== this.orderCache.length;
        if (!structuralChange) {
          for (let i = 0; i < incIds.length; i++) {
            if (!orderSet.has(incIds[i]) || this.orderCache[i] !== incIds[i]) {
              structuralChange = true;
              break;
            }
          }
        }
        if (structuralChange) {
          reconcileOrder(this.yOrder, incIds);
          this.orderCache = this.yOrder.toArray();
        }
      }, LOCAL_ORIGIN);
    } finally {
      this.localTransaction = false;
    }
  }

  /** Yjs 变化 -> 写回 Excalidraw 场景（不污染本地撤销栈） */
  private pushRemoteToScene() {
    let elements = this.readElements();
    // 【并发画笔掐断修复】本地正在画（inProgressFreedrawIds 非空）时，远端 freedraw
    // 通过 awareness.brush 单独渲染（不依赖 Excalidraw），避免 updateScene 把本地
    // in-progress 元素用陈旧快照覆盖导致 points 不再增长。我们要做的是：
    //   - 远端 finalized freedraw（不在追踪集合里）→ 正常推回
    //   - 本地 in-progress freedraw（id 在追踪集合里）→ 从当前 Excalidraw scene 拿
    //     最新引用塞回去，否则 Excalidraw 内部 mutate 的对象引用会被替换掉
    if (this.drawingActive && this.inProgressFreedrawIds.size > 0 && !this.localDrawingJustFinished) {
      const localScene = this.api?.getSceneElementsIncludingDeleted?.() || [];
      const localById = new Map(localScene.map((e: any) => [e.id, e]));
      // 1) 移除 yjs 里所有 in-progress freedraw（避免用陈旧快照覆盖本地最新引用）
      elements = elements.filter((e) => !(e.type === "freedraw" && this.inProgressFreedrawIds.has(e.id)));
      // 2) 把本地 in-progress 元素按 yOrder 中的位置插回（保留 z 序）
      const localInProgress: any[] = Array.from(this.inProgressFreedrawIds)
        .map((id) => localById.get(id))
        .filter(Boolean);
      if (localInProgress.length) {
        const yOrderArr = this.yOrder.toArray();
        const merged: any[] = [];
        const remoteById = new Map(elements.map((e: any) => [e.id, e]));
        const mergedIds = new Set<string>();
        const inserted = new Set<string>();
        for (const id of yOrderArr) {
          if (this.inProgressFreedrawIds.has(id)) {
            const live = localById.get(id);
            if (live) { merged.push(live); inserted.add(id); }
          } else {
            const e = remoteById.get(id);
            if (e) { merged.push(e); mergedIds.add(id); }
          }
        }
        // 兜底：yOrder 里没有但本地还有（极少见，例：新建但 yOrder 还没更新）的元素追加
        for (const lp of localInProgress) {
          if (!inserted.has(lp.id)) merged.push(lp);
        }
        // 远端其他 freedraw 也要保留（不是 in-progress 的远端 finalized）
        const remoteOtherFreedraw = elements.filter(
          (e: any) => e.type === "freedraw" && !this.inProgressFreedrawIds.has(e.id)
        );
        for (const ro of remoteOtherFreedraw) {
          if (!mergedIds.has(ro.id)) { merged.push(ro); mergedIds.add(ro.id); }
        }
        elements = merged;
      }
    }
    this.localDrawingJustFinished = false;
    this.applyingRemote = true;
    try {
      // commitToHistory: false —— 远端改动不该进本地撤销栈
      this.api.updateScene({ elements, commitToHistory: false });
    } finally {
      this.applyingRemote = false;
      // 新场景元素就位后，再触发一次 imageCache 刷新。
      // 这处理了「文件先到、元素后到」的竞态：文件已进 Excalidraw.files，
      // 但 addFiles 时还没 image 元素，不会加载；现在元素到了，刷新即可渲染。
      this.applyFilesToScene();
      this.lastSnapshot.clear();
      for (const el of elements) this.lastSnapshot.set(el.id, snapshotOf(el));
      // 标记"远端刚推过来的元素"——这些元素之后被 Excalidraw 重新调 onChange 时
      // （updateScene 会触发 Excalidraw 内部 mutateElement 改 version/versionNonce，
      // 触发新一轮 onChange），handleChange 必须跳过写 Yjs，否则与远端互写形成反馈循环。
      this.remoteJustSyncedIds = new Set(elements.map((e) => e.id));
      queueMicrotask(() => { this.remoteJustSyncedIds = null; });
    }
  }

  /** 全量重建本地镜像（初始同步 / 重连 / 清空 / 撤销 / 导入后用）。O(n) 但只在低频事件触发。 */
  private rebuildMirror() {
    this.elementMirror.clear();
    this.orderCache = this.yOrder.toArray();
    const seen = new Set<string>();
    const fill = (id: string) => {
      const ym = this.yElements.get(id);
      if (!ym) return;
      const json = restoreAfterRead(ym.toJSON());
      if (json && json.isDeleted !== true) this.elementMirror.set(id, json);
      seen.add(id);
    };
    for (const id of this.orderCache) fill(id);
    for (const id of Array.from(this.yElements.keys())) {
      if (seen.has(id)) continue;
      fill(id);
    }
  }

  /** 增量更新单个元素的镜像：只对该元素做 toJSON+restoreAfterRead（O(该元素)），其余不受影响。 */
  private updateMirrorForId(id: string) {
    const ym = this.yElements.get(id);
    if (!ym) {
      this.elementMirror.delete(id);
      return;
    }
    const json = restoreAfterRead(ym.toJSON());
    if (!json || json.isDeleted === true) {
      this.elementMirror.delete(id);
      return;
    }
    this.elementMirror.set(id, json);
  }

  /** 把一帧内的多次远端/本地变更合并成一次 updateScene（rAF 优先，降级到 setTimeout）。 */
  private scheduleFlush() {
    if (this.destroyed || this.flushScheduled) return;
    this.flushScheduled = true;
    const run = () => {
      this.flushScheduled = false;
      if (!this.destroyed) this.pushRemoteToScene();
    };
    if (typeof requestAnimationFrame !== "undefined") {
      this.flushHandle = requestAnimationFrame(run);
    } else {
      this.flushHandle = setTimeout(run, 16);
    }
  }

  private readElements(): any[] {
    const out: any[] = [];
    const seen = new Set<string>();
    // 按 orderCache 顺序输出（z 序），每个元素浅克隆，避免 Excalidraw updateScene 的
    // 内部 mutate 反过来污染我们的镜像。
    for (const id of this.orderCache) {
      const el = this.elementMirror.get(id);
      if (!el) continue;
      seen.add(id);
      out.push({ ...el });
    }
    // 兜底：镜像里有但还没进 order（并发写入）的元素追加到尾部
    for (const [id, el] of this.elementMirror) {
      if (seen.has(id)) continue;
      out.push({ ...el });
    }
    return out;
  }

  setPointer(pointer: { x: number; y: number } | null) {
    this.awareness?.setLocalStateField("cursor", pointer);
  }

  /**
   * 广播"我正在画的临时笔触"：仅用于多人实时可视化（不写 Yjs，不持久化）。
   * - brush：当前正在按下的笔（freedraw），points 是相对起点 [{x,y},...]
   * - 其他客户端拿到后画一条半透明覆盖线，颜色用 awareness.user.color
   * - 同一时间本地只会有一笔，松手时把 brush 设为 null
   */
  setBrush(brush: { x: number; y: number; points: [number, number][] } | null) {
    const aw = this.awareness;
    if (!aw) return;

    // 松手：立即清掉（这一帧必须马上发，否则别人屏幕上会留一条鬼影笔迹）
    if (!brush) {
      if (this.brushTimer) {
        clearTimeout(this.brushTimer);
        this.brushTimer = null;
      }
      this.pendingBrush = null;
      aw.setLocalStateField("brush", null);
      return;
    }

    this.pendingBrush = brush;

    // 前沿节流：第一次立即发，之后 BRUSH_THROTTLE_MS 内只发一次
    const now = Date.now();
    const wait = BRUSH_THROTTLE_MS - (now - this.lastBrushAt);
    if (wait <= 0) {
      this.flushBrush();
      return;
    }
    if (!this.brushTimer) {
      this.brushTimer = setTimeout(() => {
        this.brushTimer = null;
        this.flushBrush();
      }, wait);
    }
  }

  private flushBrush() {
    this.lastBrushAt = Date.now();
    const brush = this.pendingBrush;
    if (!brush) return;
    const pts = brush.points || [];
    const count = Math.min(512, pts.length);
    const flat: number[] = [];
    for (let i = 0; i < count; i++) {
      const p = pts[Math.round(i * (pts.length - 1) / Math.max(1, count - 1))];
      flat.push(Math.round(p?.[0] || 0), Math.round(p?.[1] || 0));
    }
    this.awareness?.setLocalStateField("brush", { x: Math.round(brush.x), y: Math.round(brush.y), p: flat });
  }

  /** 标记本地正在画（freedraw）。在 Excalidraw 的 onPointerDown 时设 true，
   *  onPointerUp 时设 false。handleChange 会据 drawingActive 判断哪些 freedraw
   *  帧是"in-progress 中间帧"（跳过写入，mouseup 统一 finalize）。 */
  setDrawing(drawing: boolean, brush: { x: number; y: number; points: [number, number][] } | null = null) {
    this.drawingActive = drawing;
    // 无论按下还是松手，都重置追踪状态：
    // - 按下：清掉上一笔可能残留的追踪（新的一笔从零开始）
    // - 松手：finalizeInProgressFreedraw 已经处理过追踪集合，这里兜底再清一次
    this.inProgressFreedrawIds.clear();
    this.lastInProgressFreedrawEl = null;
    this.awareness?.setLocalStateField("drawing", drawing);
    // 松手必须把 brush 清掉，否则别人屏幕上会留着一条永远不消失的鬼影笔迹
    if (drawing) this.setBrush(brush);
    else this.setBrush(null);
  }

  /**
   * onPointerUp 时调用：把本地这一笔 freedraw 的最终元素写入 Yjs。
   *
   * 【关键】依据 inProgressFreedrawIds 里追踪的 id 从当前场景拿「活的最终元素」，
   * 而不是让调用方 find() 猜 —— 场景里存在任何旧笔迹（对方画的、自己之前画的）
   * 时 find() 会拿错元素，导致新笔迹永远进不了 Yjs，松手即消失。
   */
  finalizeInProgressFreedraw(): boolean {
    const ids = Array.from(this.inProgressFreedrawIds);
    const scene: any[] = this.api?.getSceneElementsIncludingDeleted?.() || [];
    const byId = new Map(scene.map((e: any) => [e.id, e]));
    const els: any[] = [];
    for (const id of ids) {
      const el = byId.get(id);
      if (el && el.type === "freedraw" && !el.isDeleted) els.push(el);
    }
    // 兜底：场景里已找不到（异常路径），用最后一次 onChange 的元素快照写入
    if (!els.length && this.lastInProgressFreedrawEl &&
        this.lastInProgressFreedrawEl.type === "freedraw" &&
        !this.lastInProgressFreedrawEl.isDeleted) {
      els.push(this.lastInProgressFreedrawEl);
    }
    // 清理追踪（无论是否写入成功）
    this.inProgressFreedrawIds.clear();
    this.lastInProgressFreedrawEl = null;
    if (!els.length) return false;

    if (typeof window !== "undefined") console.log(
      "[finalizeInProgress] n=", els.length,
      els.map((e) => `${e.id.slice(0, 6)}:npts=${(e.points || []).length},ver=${e.version}`).join(" ")
    );
    this.localTransaction = true;
    try {
      this.doc.transact(() => {
        for (const el of els) {
          let m = this.yElements.get(el.id);
          if (!m) {
            m = new Y.Map();
            this.yElements.set(el.id, m);
          }
          writeElement(m, el);
          this.lastSnapshot.set(el.id, snapshotOf(el));
          if (!this.yOrder.toArray().includes(el.id)) {
            this.yOrder.push([el.id]);
          }
        }
      }, LOCAL_ORIGIN);
    } finally {
      this.localTransaction = false;
    }
    // finalize 后推回 Excalidraw（从 Yjs 重建场景，让本端也确认笔迹落盘）
    this.localDrawingJustFinished = true;
    this.orderCache = this.yOrder.toArray();
    for (const el of els) this.updateMirrorForId(el.id);
    this.scheduleFlush();
    return true;
  }

  /**
   * 兼容保留：把单个 freedraw 元素显式写入 Yjs。
   * 新代码请用 finalizeInProgressFreedraw()（按追踪 id 定位，不会拿错元素）。
   */
  finalizeFreedraw(el: any) {
    if (!el || el.type !== "freedraw") return;
    if (typeof window !== "undefined") console.log("[finalizeFreedraw] write", el.id.slice(0,6), "npts=", (el.points||[]).length, "ver=", el.version);
    this.inProgressFreedrawIds.delete(el.id);
    this.localTransaction = true;
    try {
      this.doc.transact(() => {
        let m = this.yElements.get(el.id);
        if (!m) {
          m = new Y.Map();
          this.yElements.set(el.id, m);
        }
        writeElement(m, el);
        this.lastSnapshot.set(el.id, snapshotOf(el));
        // 确保 finalize 元素也在 yOrder 中
        if (!this.yOrder.toArray().includes(el.id)) {
          this.yOrder.push([el.id]);
        }
      }, LOCAL_ORIGIN);
    } finally {
      this.localTransaction = false;
    }
    this.localDrawingJustFinished = true;
    this.orderCache = this.yOrder.toArray();
    this.updateMirrorForId(el.id);
    this.scheduleFlush();
  }

  setSelection(elementIds: Record<string, boolean>) {
    const key = Object.keys(elementIds).filter(id => elementIds[id]).sort().join("|");
    if (key === this.lastSelectionKey) return;
    this.lastSelectionKey = key;
    this.awareness?.setLocalStateField("selection", elementIds);
  }
}
