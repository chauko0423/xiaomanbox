import * as Y from "yjs";
import * as dagre from "dagre";

/**
 * 思维导图元件：在现有 Excalidraw + Yjs 之上叠加「父子关系 + 自动布局」。
 *
 * v2（导图元件版）：
 *   - 画板上可以同时放**多棵**导图（yMind 里 parentId 为空的就是一棵树的根），
 *     每棵树独立做 dagre 布局，锚定在各自根节点当前位置，互不干扰
 *   - 连线是 **90° 正交折线**（组织架构图式）：水平 → 垂直 → 水平，在水平中点转弯，
 *     线条笔直规整；roughness=0 无手绘抖动。
 *     起终点仍带 binding，拖动节点时 Excalidraw 会保持连接，下次结构操作时重新取直
 *   - 节点文字是**独立的 bound text 元素**（id = 节点id + "_label"，containerId 指回节点），
 *     双击节点即可原生编辑文字，编辑会走 handleChange 正常同步
 *
 * 设计要点（沿 v1）：
 *   - 节点本体是普通 Excalidraw 矩形，走 board 的 elements 同步链路
 *   - 关系数据在 ydoc.getMap("mindmap")：nodeId -> {parentId, collapsed, text}
 *   - 所有结构变更都在一个 Yjs transaction 内完成（含 relayout）
 *   - 布局由发起操作的客户端算好写回 Yjs，其他客户端直接应用
 */

const NODE_W = 180;
const NODE_H = 56;
const TEXT_FONT = 16;
const RANKSEP = 90; // 横向：层与层之间的水平距离
const NODESEP = 24; // 同层节点之间的垂直间距
const MIN_LEAD = 14; // 90° 折线生效的最小水平引线段长度
/** 折线竖直段相对父节点右边缘的偏移量（≈ RANKSEP 的一半）。
 *  固定偏移量让**同一个父节点的所有子节点共用一条竖线**，是组织架构图的经典画法，
 *  比「每条连线各取中点」整齐得多。 */
const ARM = Math.round(RANKSEP / 2);

function genId(): string {
  return "n_" + Math.random().toString(36).slice(2, 10);
}

function nowMs() {
  return Date.now();
}

function randSeed() {
  return Math.floor(Math.random() * 2 ** 31);
}

/** 粗略估中文/英文混排的文本宽度（与 shapePalette 同一套算法） */
function estimateTextWidth(text: string, fontSize: number): number {
  let units = 0;
  for (const ch of text) units += /[\u4e00-\u9fa5\u3000-\u303f\uff00-\uffef]/.test(ch) ? 1 : 0.55;
  return Math.ceil(units * fontSize);
}

function nodeWidthFor(text: string): number {
  return Math.min(320, Math.max(120, estimateTextWidth(text, TEXT_FONT) + 28));
}

/**
 * 解析导图连线 id `arrow_<父>_<子>`。
 * 不能用贪婪正则：节点 id 是 "n_xxx" 自带下划线，必须从下标 2 起找第一个 "_"。
 */
export function parseArrowId(arrowId: string): [string, string] | null {
  if (!arrowId.startsWith("arrow_")) return null;
  const rest = arrowId.slice("arrow_".length);
  const sep = rest.indexOf("_", 2);
  if (sep < 0) return null;
  return [rest.slice(0, sep), rest.slice(sep + 1)];
}

export class MindMap {
  private doc: Y.Doc;
  private api: any;
  private yMind: Y.Map<any>;
  private yElements: Y.Map<any>;
  private yOrder: Y.Array<string>;

  constructor(doc: Y.Doc, api: any) {
    this.doc = doc;
    this.api = api;
    this.yMind = doc.getMap("mindmap");
    this.yElements = doc.getMap("elements");
    this.yOrder = doc.getArray("order");
  }

  // ---------------- 查询 ----------------

  /** 所有根节点（画板上每棵导图一个根） */
  roots(): string[] {
    const out: string[] = [];
    this.yMind.forEach((v: any, id: string) => {
      if (!v.get("parentId")) out.push(id);
    });
    return out;
  }

  getRootId(): string | null {
    return this.roots()[0] ?? null;
  }

  private childrenOf(id: string): string[] {
    const out: string[] = [];
    this.yMind.forEach((v: any, cid: string) => {
      if (v.get("parentId") === id) out.push(cid);
    });
    return out;
  }

  /** 从某根 DFS 可见节点，遇到 collapsed 节点不展开其子（折叠） */
  private visibleFrom(rootId: string): string[] {
    const out: string[] = [];
    const visit = (id: string) => {
      out.push(id);
      const m = this.yMind.get(id);
      if (m && m.get("collapsed")) return;
      this.childrenOf(id).forEach(visit);
    };
    visit(rootId);
    return out;
  }

  // ---------------- 手动位置 ----------------

  /** 本轮手势里是否手动拖动过节点（settle 用完即清） */
  private manualDirty = false;

  /**
   * 标记节点被用户手动拖动过（由同步层 boardCollab 检测到位置变化时调用）。
   * 标记后 relayout 不再重排该节点，它的子树跟着它刚性平移。
   * yMind 没有观察者，这里不会触发 pushRemoteToScene 回声。
   */
  markManual(id: string) {
    if (!id.startsWith("n_") || id.endsWith("_label")) return;
    const m = this.yMind.get(id);
    if (!m || m.get("manual")) return;
    m.set("manual", true);
    this.manualDirty = true;
  }

  /**
   * 手势（拖拽）结束后调用：这轮里手动拖过节点的话，跑一次 relayout 把
   * 折线取直、文字重新居中——manual 位置全部保留，只有连线和文字会动。
   */
  settle() {
    if (!this.manualDirty) return;
    this.manualDirty = false;
    this.relayout();
  }

  // ---------------- 元素构造 ----------------

  private ensureInOrder(id: string) {
    if (!this.yOrder.toArray().includes(id)) this.yOrder.push([id]);
  }

  /** 确保节点矩形 + 绑定文字都存在；已存在的不动（文字编辑不会被覆盖） */
  private ensureNodeElements(id: string, text: string) {
    const labelId = id + "_label";
    let yEl = this.yElements.get(id);
    if (!yEl) {
      yEl = new Y.Map();
      this.yElements.set(id, yEl);
      const data = this.makeRect(id, text);
      for (const k of Object.keys(data)) {
        if (data[k] !== undefined) yEl.set(k, data[k]);
      }
      this.ensureInOrder(id);
    }
    // v1 遗留：旧数据用内嵌 label 属性（Excalidraw 不渲染），清掉
    if (yEl.has("label")) yEl.delete("label");

    // 绑定文字元素
    if (!this.yElements.get(labelId)) {
      const ly = new Y.Map();
      this.yElements.set(labelId, ly);
      const data = this.makeText(id, text);
      for (const k of Object.keys(data)) {
        if (data[k] !== undefined) ly.set(k, data[k]);
      }
      this.ensureInOrder(labelId);
    }

    // 容器的 boundElements 必须包含文字（拖动/编辑时 Excalidraw 靠它找绑定文字）
    const cur = yEl.get("boundElements");
    const arr = Array.isArray(cur) ? cur.slice() : [];
    if (!arr.some((b: any) => b && b.id === labelId)) {
      arr.push({ id: labelId, type: "text" });
      yEl.set("boundElements", arr);
    }
  }

  private nodeBox(id: string): { x: number; y: number; w: number; h: number } {
    const yEl = this.yElements.get(id);
    return {
      x: (yEl && yEl.get("x")) || 0,
      y: (yEl && yEl.get("y")) || 0,
      w: (yEl && yEl.get("width")) || NODE_W,
      h: (yEl && yEl.get("height")) || NODE_H,
    };
  }

  private makeRect(id: string, text: string): Record<string, any> {
    const t = nowMs();
    const seed = randSeed();
    const w = nodeWidthFor(text);
    return {
      id,
      type: "rectangle",
      x: 0,
      y: 0,
      width: w,
      height: NODE_H,
      angle: 0,
      strokeColor: "#1971C2",
      backgroundColor: "#a5d8ff",
      fillStyle: "solid",
      strokeWidth: 2,
      strokeStyle: "solid",
      roughness: 1,
      opacity: 100,
      groupIds: [],
      frameId: null,
      roundness: { type: 3 },
      seed,
      version: 1,
      versionNonce: seed,
      isDeleted: false,
      boundElements: [{ id: id + "_label", type: "text" }],
      updated: t,
      link: null,
      locked: false,
    };
  }

  /** 节点的绑定文字：独立 text 元素，containerId 指回矩形 */
  private makeText(nodeId: string, text: string): Record<string, any> {
    const t = nowMs();
    const seed = randSeed();
    const rectW = nodeWidthFor(text);
    const lines = text.split("\n").length;
    return {
      id: nodeId + "_label",
      type: "text",
      x: 8,
      y: (NODE_H - lines * TEXT_FONT * 1.25) / 2,
      width: rectW - 16,
      height: Math.round(lines * TEXT_FONT * 1.25),
      angle: 0,
      strokeColor: "#1e1e1e",
      backgroundColor: "transparent",
      fillStyle: "solid",
      strokeWidth: 2,
      strokeStyle: "solid",
      roughness: 1,
      opacity: 100,
      groupIds: [],
      frameId: null,
      roundness: null,
      seed,
      version: 1,
      versionNonce: seed,
      isDeleted: false,
      boundElements: null,
      updated: t,
      link: null,
      locked: false,
      text,
      fontSize: TEXT_FONT,
      fontFamily: 1,
      textAlign: "center",
      verticalAlign: "middle",
      containerId: nodeId,
      lineHeight: 1.25,
      // ⚠️ updateScene 不走 restore，text 元素缺 baseline 会让 fillText 拿到
      // undefined → 文字整个画不出来。官方 measureBaseline 是 DOM 实测，
      // 这里用其稳定比例（原生样本：20px → 18，≈ fontSize*0.9），双击编辑后
      // Excalidraw 会用 redrawTextBoundingBox 重算精确值
      baseline: Math.round(TEXT_FONT * 0.9),
      originalText: text,
    };
  }

  private makeArrowSkeleton(aid: string, parentId: string, childId: string): Record<string, any> {
    const t = nowMs();
    const seed = randSeed();
    return {
      id: aid,
      type: "arrow",
      x: 0,
      y: 0,
      width: 10,
      height: 0,
      angle: 0,
      strokeColor: "#868e96",
      backgroundColor: "transparent",
      fillStyle: "solid",
      strokeWidth: 2,
      strokeStyle: "solid",
      roughness: 0, // 折线要规整——关掉手绘抖动
      opacity: 100,
      groupIds: [],
      frameId: null,
      // ⚠️ 必须是 null，不能是 { type: 2 } / { type: 0 }：
      //    Excalidraw 渲染线性元素的分支是
      //      T = element.roundness ? [rc.curve(points)] : [rc.linearPath(points)]
      //    只要 roundness 是对象（哪怕 type:0）就会走 curve()，把 90° 折角画成圆滑曲线。
      //    null → linearPath() → 每一段都是笔直的线段。
      roundness: null,
      seed,
      version: 1,
      versionNonce: seed,
      isDeleted: false,
      boundElements: null,
      updated: t,
      link: null,
      locked: false,
      points: [
        [0, 0],
        [10, 0],
      ],
      lastCommittedPoint: null,
      startBinding: { elementId: parentId, gap: 4, focus: 0, fixedPoint: null },
      endBinding: { elementId: childId, gap: 4, focus: 0, fixedPoint: null },
      startArrowhead: null,
      endArrowhead: null,
    };
  }

  private bindArrow(parentId: string, childId: string, arrowId: string) {
    for (const nid of [parentId, childId]) {
      const yEl = this.yElements.get(nid);
      if (!yEl) continue;
      const cur = yEl.get("boundElements");
      const arr = Array.isArray(cur) ? cur.slice() : [];
      if (!arr.some((b: any) => b && b.id === arrowId)) {
        arr.push({ id: arrowId, type: "arrow" });
        yEl.set("boundElements", arr);
      }
    }
  }

  private removeOrdered(id: string) {
    this.yElements.delete(id);
    const idx = this.yOrder.toArray().indexOf(id);
    if (idx >= 0) this.yOrder.delete(idx, 1);
  }

  // ---------------- 90° 正交折线连线 ----------------

  /**
   * 计算并写入「父 → 子」的 90° 正交折线路径。
   *
   * 起点 = 父节点右边缘中点，终点 = 子节点左边缘中点（dx/dy 为两点水平/垂直差）：
   *  1. 完全同高（|dy| < 1）：一条水平直线，不必绕。
   *  2. 其余：组织架构图式「水平 → 垂直 → 水平」，竖直段固定在
   *     父右边缘 + ARM（ARM = RANKSEP/2），**同一父节点的所有子节点共用这条竖线**，
   *     整棵树看起来是一条主干加分叉，而不是每条线各转各的。
   *     水平空间不足（dx < 2×MIN_LEAD，节点被手动拖近/拖到左侧）时退化为
   *     sx + MIN_LEAD，仍然保证四段全水平/垂直 —— 绝不退化成斜线。
   *
   * 另：连线必须 `roundness: null`。Excalidraw 渲染线性元素时
   * `element.roundness ? rc.curve(points) : rc.linearPath(points)`，
   * 只要 roundness 是对象就会把 90° 折角画成圆滑曲线。
   *
   * 两端保留 binding：拖动节点时 Excalidraw 保持连接，端点跟随；
   * 松手后 mindmap.settle() 重跑 relayout 把线掰回正交。
   */
  private writeElbowArrow(parentId: string, childId: string) {
    const p = this.nodeBox(parentId);
    const c = this.nodeBox(childId);
    const sx = Math.round(p.x + p.w); // 起点：父节点右边缘中点
    const sy = Math.round(p.y + p.h / 2);
    const ex = Math.round(c.x); // 终点：子节点左边缘中点
    const ey = Math.round(c.y + c.h / 2);
    const dx = ex - sx;
    const dy = ey - sy;

    let pts: [number, number][];
    if (Math.abs(dy) < 1) {
      // 与父节点完全同高：直接一条水平线（本来就在正交体系内，不必绕）
      pts = [
        [sx, sy],
        [ex, ey],
      ];
    } else {
      // 组织架构图式「水平 → 垂直 → 水平」。
      // 竖线 x = 父右边缘 + ARM（固定），同层兄弟因此共用一条竖线，整棵树更整齐；
      // 水平空间不足时（节点被手动拖近 / 拖到父节点左侧）退化为取中点，
      // 保证四个点始终是「水平段 + 垂直段 + 水平段」，不会退化成斜线。
      const busX =
        dx >= MIN_LEAD * 2 ? Math.round(sx + Math.min(ARM, dx / 2)) : Math.round(sx + MIN_LEAD);
      pts = [
        [sx, sy],
        [busX, sy],
        [busX, ey],
        [ex, ey],
      ];
    }

    const minX = Math.min(...pts.map((q) => q[0]));
    const minY = Math.min(...pts.map((q) => q[1]));
    const maxX = Math.max(...pts.map((q) => q[0]));
    const maxY = Math.max(...pts.map((q) => q[1]));

    const aid = `arrow_${parentId}_${childId}`;
    let ay = this.yElements.get(aid);
    if (!ay) {
      ay = new Y.Map();
      this.yElements.set(aid, ay);
      const data = this.makeArrowSkeleton(aid, parentId, childId);
      for (const k of Object.keys(data)) if (data[k] !== undefined) ay.set(k, data[k]);
      this.ensureInOrder(aid);
    }
    ay.set("x", Math.round(minX));
    ay.set("y", Math.round(minY));
    ay.set("points", pts.map(([x, y]) => [Math.round(x - minX), Math.round(y - minY)]));
    ay.set("width", Math.max(1, Math.round(maxX - minX)));
    ay.set("height", Math.max(1, Math.round(maxY - minY)));
    ay.set("roughness", 0); // 旧数据是 1（手绘抖动），顺手升级成规整直线
    // 旧数据 / 用户手动画过的连线可能是 { type: 2 }（圆滑折角），每次 relayout 都掰直
    ay.set("roundness", null);
    ay.set("strokeStyle", "solid");
    // binding 每次都重写（不只是创建时）：旧数据缺 focus 字段的话，拖动节点时
    // Excalidraw 的 updateBoundPoint → determineFocusPoint 会拿到 undefined，
    // 算出 NaN 端点 —— 连线既不跟随节点，还会污染拖拽手势（粘鼠标）。
    // focus = 0 是合法值：绑定点取「相邻点 → 元素中心」连线与轮廓的交点，
    // 对导图正好是面对面的边缘中点，节点被拖到哪端点就贴到哪
    ay.set("startBinding", { elementId: parentId, gap: 4, focus: 0, fixedPoint: null });
    ay.set("endBinding", { elementId: childId, gap: 4, focus: 0, fixedPoint: null });
    this.bindArrow(parentId, childId, aid);
  }

  // ---------------- 布局 ----------------

  /**
   * 森林布局：每棵树独立跑 dagre，然后整体平移让根节点落回它当前的位置。
   * 这样插入/编辑一棵导图不会挪动其他导图。
   */
  relayout() {
    this.doc.transact(() => {
      // 0) 清掉孤儿条目（父节点已不存在且自己不是根）
      this.yMind.forEach((v: any, id: string) => {
        const parent = v.get("parentId");
        if (parent && !this.yMind.has(parent)) this.yMind.delete(id);
      });

      const rootList = this.roots();
      const allVisible = new Set<string>();
      for (const r of rootList) this.visibleFrom(r).forEach((id) => allVisible.add(id));

      // 1) 确保可见节点的矩形 + 文字都存在（折叠展开丢节点的兜底也在这里）
      allVisible.forEach((id) => {
        const m = this.yMind.get(id);
        this.ensureNodeElements(id, (m && m.get("text")) || "节点");
      });

      // 2) 每棵树独立布局。
      //    锚定规则：手动拖动过的节点（yMind.manual）位置不重排，它的整棵
      //    子树跟着它刚性平移；其余节点锚定根节点当前位置（与旧行为一致）。
      //    这样用户拖完节点再点加号加子节点，摆好的位置不会被 dagre 弹回去。
      for (const r of rootList) {
        const visible = this.visibleFrom(r);
        const g = new dagre.graphlib.Graph();
        g.setGraph({ rankdir: "LR", nodesep: NODESEP, ranksep: RANKSEP, marginx: 0, marginy: 0 });
        g.setDefaultEdgeLabel(() => ({}));
        const box: Record<string, { w: number; h: number }> = {};
        visible.forEach((id) => {
          const b = this.nodeBox(id);
          box[id] = { w: b.w, h: b.h };
          g.setNode(id, { width: b.w, height: b.h });
        });
        visible.forEach((id) => {
          const m = this.yMind.get(id);
          const parent = m && m.get("parentId");
          if (parent && visible.includes(parent)) g.setEdge(parent, id);
        });
        dagre.layout(g);

        // dagre 位置（中心点坐标）与当前位置快照——必须先算偏移再写回
        const dagrePos: Record<string, { x: number; y: number }> = {};
        const curPos: Record<string, { x: number; y: number }> = {};
        visible.forEach((id) => {
          const n = g.node(id);
          dagrePos[id] = { x: n.x, y: n.y };
          const b = this.nodeBox(id);
          curPos[id] = { x: b.x + b.w / 2, y: b.y + b.h / 2 };
        });

        const parentOf = (id: string): string | null => {
          const m = this.yMind.get(id);
          const p = m && m.get("parentId");
          return p && visible.includes(p) ? p : null;
        };

        // 节点偏移 = 最近的 manual 祖先（含自己）的「当前位置 - dagre 位置」；
        // 链上没有 manual 就退回根节点锚定偏移
        const offsetOf = (id: string): { dx: number; dy: number } => {
          let cur: string | null = id;
          for (let guard = 0; cur && guard < 128; guard += 1) {
            const m = this.yMind.get(cur);
            if (m && m.get("manual")) {
              return {
                dx: curPos[cur].x - dagrePos[cur].x,
                dy: curPos[cur].y - dagrePos[cur].y,
              };
            }
            cur = parentOf(cur);
          }
          return {
            dx: curPos[r].x - dagrePos[r].x,
            dy: curPos[r].y - dagrePos[r].y,
          };
        };

        visible.forEach((id) => {
          const off = offsetOf(id);
          const yEl = this.yElements.get(id);
          if (!yEl) return;
          yEl.set("x", Math.round(dagrePos[id].x - box[id].w / 2 + off.dx));
          yEl.set("y", Math.round(dagrePos[id].y - box[id].h / 2 + off.dy));
        });
      }

      // 2.5) 文字跟随节点居中。bound text 的 x/y 是**场景绝对坐标**，
      //      Excalidraw 只在编辑/缩放时重排它，restore 不会自动居中，
      //      所以布局后必须自己把文字摆进节点
      allVisible.forEach((id) => {
        const b = this.nodeBox(id);
        const ly = this.yElements.get(id + "_label");
        if (!ly) return;
        const tw = ly.get("width") || b.w - 16;
        const th = ly.get("height") || 20;
        ly.set("x", Math.round(b.x + (b.w - tw) / 2));
        ly.set("y", Math.round(b.y + (b.h - th) / 2));
      });

      // 3) 可见连线：写入折角路径并绑定
      allVisible.forEach((id) => {
        const m = this.yMind.get(id);
        const parent = m && m.get("parentId");
        if (parent && allVisible.has(parent)) this.writeElbowArrow(parent, id);
      });

      // 4) 清理不可见元素：折叠掉的子树/悬空连线/无主文字
      const toRemove: string[] = [];
      this.yElements.forEach((_v: any, id: string) => {
        if (id.startsWith("arrow_")) {
          const pair = parseArrowId(id);
          const ok = pair && allVisible.has(pair[0]) && allVisible.has(pair[1]);
          if (!ok) toRemove.push(id);
        } else if (id.startsWith("n_")) {
          // 注意 label id "n_xxx_label" 也以 n_ 开头，先剥掉后缀再判断
          const nid = id.endsWith("_label") ? id.slice(0, -"_label".length) : id;
          if (!allVisible.has(nid)) toRemove.push(id);
        }
      });
      for (const id of toRemove) this.removeOrdered(id);
    });
  }

  // ---------------- 操作 ----------------

  /** 在指定场景坐标插入一棵新导图（中心点对齐到 x,y），返回根节点 id */
  insertRoot(x: number, y: number, text = "中心主题"): string {
    const id = genId();
    this.doc.transact(() => {
      // 防重叠：目标位置已被别的根占着就往下挪（每次 96px），直到空出来
      const clash = (px: number, py: number) =>
        this.roots().some((rid) => {
          const b = this.nodeBox(rid);
          return (
            Math.abs(px - (b.x + b.w / 2)) < (b.w + nodeWidthFor(text)) / 2 - 20 &&
            Math.abs(py - (b.y + b.h / 2)) < (b.h + NODE_H) / 2 + 20
          );
        });
      let cy = y;
      let guard = 0;
      while (clash(x, cy) && guard < 40) {
        cy += 96;
        guard += 1;
      }

      const m = new Y.Map();
      m.set("parentId", null);
      m.set("collapsed", false);
      m.set("text", text);
      this.yMind.set(id, m);
      this.ensureNodeElements(id, text);
      const w = nodeWidthFor(text);
      const yEl = this.yElements.get(id);
      yEl.set("x", Math.round(x - w / 2));
      yEl.set("y", Math.round(cy - NODE_H / 2));
      this.relayout();
    });
    return id;
  }

  /** 兼容旧入口：有根就返回第一个根，没有则在指定位置（默认原点）创建 */
  enter(x?: number, y?: number): string {
    const first = this.roots()[0];
    if (first) return first;
    return this.insertRoot(x ?? 0, y ?? 0);
  }

  addChild(id?: string): string {
    const parentId = id || this.roots()[0];
    if (!parentId) return this.insertRoot(0, 0);
    const newId = genId();
    this.doc.transact(() => {
      const m = new Y.Map();
      m.set("parentId", parentId);
      m.set("collapsed", false);
      m.set("text", "新节点");
      this.yMind.set(newId, m);
      this.ensureNodeElements(newId, "新节点");
      this.relayout();
    });
    return newId;
  }

  addSibling(id?: string): string {
    if (!id) return this.addChild();
    const m = this.yMind.get(id);
    const parent = m && m.get("parentId");
    if (!parent) return this.addChild(id);
    return this.addChild(parent);
  }

  toggleCollapse(id?: string) {
    if (!id) return;
    const m = this.yMind.get(id);
    if (!m) return;
    this.doc.transact(() => {
      m.set("collapsed", !m.get("collapsed"));
      this.relayout();
    });
  }

  deleteNode(id?: string) {
    if (!id) return;
    const sub = new Set<string>();
    const collect = (nid: string) => {
      sub.add(nid);
      this.childrenOf(nid).forEach(collect);
    };
    collect(id);
    this.doc.transact(() => {
      for (const nid of sub) {
        this.yMind.delete(nid);
        // 节点矩形 + 它的绑定文字一起删
        this.removeOrdered(nid);
        this.removeOrdered(nid + "_label");
      }
      // 删除所有涉及被删节点的连线（按解析出的父子 id 判断，不能 includes 猜）
      const victims: string[] = [];
      this.yElements.forEach((_v: any, aid: string) => {
        const pair = parseArrowId(aid);
        if (pair && (sub.has(pair[0]) || sub.has(pair[1]))) victims.push(aid);
      });
      for (const aid of victims) this.removeOrdered(aid);
      this.relayout();
    });
  }
}
