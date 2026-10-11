<template>
  <div class="bp-panel">
    <!-- 工具栏：当前蓝图 / 自动排序 / 节点坐标 / 搜索 / 设置 / 格式规范（对照 NodeUE 蓝图查看器） -->
    <header class="bp-bar">
      <span class="brand">🧩 {{ t('蓝图画布') }}</span>
      <span class="dot" :class="ualOnline ? 'on' : 'off'" :title="ualOnline ? t('插件已连接') : t('插件未连接（需 UE 编辑器 + UnrealAgentLink）')"></span>

      <input v-model="bpPath" class="inp bp-path" placeholder="/Game/Blueprints/BP_XXX" @keyup.enter="loadGraph()" />
      <button class="btn" :disabled="loading" :title="t('读取这张蓝图（复制蓝图本体到画布）')" @click="loadGraph()">{{ t('当前蓝图') }}</button>

      <select v-model="graphName" class="inp sel" :disabled="!graphOptions.length" :title="t('选择图（事件图/函数/宏）')" @change="loadGraph()">
        <option v-if="!graphOptions.length" value="">{{ graphName || 'EventGraph' }}</option>
        <option v-for="g in graphOptions" :key="g" :value="g">{{ g }}</option>
      </select>

      <button class="btn" :disabled="!nodes.length" :title="t('最长路径分层排版（可在设置里开写回引擎）')" @click="autoArrange()">{{ t('自动排序') }}</button>
      <label class="chk"><input v-model="showCoords" type="checkbox" />{{ t('节点坐标') }}</label>

      <input v-model="search" class="inp bp-search" :placeholder="t('搜索…')" @keyup.enter="onSearchEnter" />
      <span v-if="search" class="hit-count">{{ hits.length }} {{ t('匹配') }}</span>

      <span class="grow"></span>

      <button class="btn" :disabled="!bpPath" :title="t('编译这张蓝图')" @click="compileBp()">{{ t('编译') }}</button>
      <button class="btn" :disabled="!bpPath" @click="doExportT3D()">{{ t('T3D 导出') }}</button>
      <button class="btn" :disabled="!bpPath" @click="showImport = true">{{ t('T3D 导入') }}</button>
      <button class="btn warn" :disabled="!selectedEdge" :title="t('断开选中的连线')" @click="disconnectSelected()">{{ t('断连线') }}</button>
      <button class="btn danger" :disabled="!selectedNode" :title="t('删除选中的节点（需确认）')" @click="showDelete = true">{{ t('删除') }}</button>
      <button class="btn" @click="showSpec = true">{{ t('格式规范(AI提示词)') }}</button>
      <button class="btn" @click="showSettings = !showSettings">{{ t('设置') }}</button>
    </header>

    <p v-if="err" class="err">{{ err }}</p>
    <p v-if="notice" class="notice">{{ notice }}</p>

    <!-- 画布：拖空白平移 · 滚轮缩放 · 拖节点挪位 · 点选节点/连线 -->
    <div ref="viewport" class="bp-viewport" @wheel="onWheel" @mousedown="onBgDown">
      <div class="bp-world" :style="{ transform: `translate(${view.x}px, ${view.y}px) scale(${view.k})` }">
        <svg class="bp-edges" :class="{ 'no-data': !cfg.dataLinks }">
          <g v-for="e in visibleEdges" :key="e.key">
            <path class="hit-area" :d="e.d" @click.stop="selectedEdge = e.key" />
            <path
              class="wire" :d="e.d"
              :class="{ exec: e.exec, sel: selectedEdge === e.key }"
              :style="{ stroke: e.exec ? undefined : e.color }"
            />
          </g>
        </svg>

        <div
          v-for="n in nodes" :key="n.id"
          class="bp-node" :class="{ comment: n.isComment, sel: selectedNode === n.id, hit: isHit(n.id), ghost: n.ghost }"
          :style="{ left: n.x + 'px', top: n.y + 'px', width: n.w + 'px', height: n.h + 'px' }"
          :title="n.cls + (n.member ? ' · ' + n.member : '')"
          @mousedown.stop="onNodeDown(n, $event)"
        >
          <template v-if="!n.isComment">
            <div class="n-head" :class="headKind(n)">{{ n.title }}</div>
            <div class="n-body">
              <div class="col in">
                <div v-for="(p, i) in n.inPins" :key="'i' + i" class="pin">
                  <span class="pl" :style="{ background: p.color }"></span>
                  <span v-if="cfg.pinNames" class="pn">{{ p.label }}</span>
                </div>
              </div>
              <div class="col out">
                <div v-for="(p, i) in n.outPins" :key="'o' + i" class="pin">
                  <span v-if="cfg.pinNames" class="pn">{{ p.label }}</span>
                  <span class="pl" :style="{ background: p.color }"></span>
                </div>
              </div>
            </div>
            <span v-if="showCoords" class="coord">{{ n.x }}, {{ n.y }}</span>
          </template>
          <span v-else class="c-text">{{ n.title }}</span>
        </div>
      </div>

      <div v-if="!nodes.length" class="bp-empty">
        <p><b>{{ t('把蓝图本体复制进画布') }}</b></p>
        <p>{{ t('① 填蓝图路径（如') }} <code>/Game/Blueprints/BP_Example</code>{{ t('）→ 点「当前蓝图」') }}</p>
        <p>{{ t('② 需要 UE 编辑器在线且装了 UnrealAgentLink 插件（工作台内置 UAL 桥直连，零外部依赖）') }}</p>
        <p>{{ t('③ 加载后：拖动排版 · 搜索定位 · 删节点 · 断连线 · T3D 导入导出 · 编译') }}</p>
      </div>
    </div>

    <footer class="bp-foot">
      {{ t('拖空白平移 · 滚轮缩放 · 拖节点挪位 · 点选节点/连线后用右上「删除 / 断连线」·') }} {{ nodes.length }} {{ t('节点') }} / {{ visibleEdges.length }} {{ t('线') }}
      <template v-if="graphName"> · {{ graphName }}</template>
    </footer>

    <!-- 设置抽屉 -->
    <div v-if="showSettings" class="drawer">
      <div class="d-head"><b>{{ t('设置') }}</b><button class="btn" @click="showSettings = false">{{ t('收起') }}</button></div>
      <label class="chk"><input v-model="cfg.pinNames" type="checkbox" />{{ t('显示引脚名称') }}</label>
      <label class="chk"><input v-model="cfg.grid" type="checkbox" />{{ t('显示网格') }}</label>
      <label class="chk"><input v-model="cfg.dataLinks" type="checkbox" />{{ t('显示数据连线（关=只看执行流）') }}</label>
      <label class="chk"><input v-model="cfg.curve" type="checkbox" />{{ t('连线走曲线（关=直线）') }}</label>
      <label class="chk"><input v-model="cfg.writeBack" type="checkbox" />{{ t('自动排序后写回引擎坐标') }}</label>
      <button class="btn" @click="resetView()">{{ t('重置视图') }}</button>
    </div>

    <!-- 格式规范（AI 提示词） -->
    <div v-if="showSpec" class="modal-mask" @click.self="showSpec = false">
      <div class="modal wide">
        <div class="d-head"><b>{{ t('格式规范（AI 提示词）') }}</b><button class="btn" @click="showSpec = false">{{ t('关闭') }}</button></div>
        <p class="m-hint">{{ t('复制给 AI，让它按这份规范产出能粘回 UE 的 T3D（配合「T3D 导入」）。') }}</p>
        <textarea class="spec" readonly :value="SPEC_PROMPT"></textarea>
        <button class="btn primary" @click="copySpec()">{{ t('复制规范全文') }}</button>
      </div>
    </div>

    <!-- 删除确认（destructive 双保险的前端那一半） -->
    <div v-if="showDelete" class="modal-mask" @click.self="showDelete = false">
      <div class="modal">
        <div class="d-head"><b>{{ t('删除节点') }}</b></div>
        <p class="m-hint danger-text">{{ t('将把蓝图里的这个节点真删掉（引擎侧，可 Ctrl+Z）：') }}<br /><b>{{ deleteTargetTitle }}</b></p>
        <div class="m-row">
          <button class="btn" @click="showDelete = false">{{ t('取消') }}</button>
          <button class="btn danger" @click="doDelete()">{{ t('确认删除') }}</button>
        </div>
      </div>
    </div>

    <!-- T3D 导入 -->
    <div v-if="showImport" class="modal-mask" @click.self="showImport = false">
      <div class="modal wide">
        <div class="d-head"><b>{{ t('T3D 导入') }} → {{ graphName || 'EventGraph' }}</b><button class="btn" @click="showImport = false">{{ t('关闭') }}</button></div>
        <p class="m-hint">{{ t('粘贴 T3D 文本（') }}<code>blueprint_export_t3d</code> {{ t('导出的原文或 AI 按规范改写的），点导入会真粘进蓝图并刷新画布。') }}</p>
        <textarea v-model="t3dText" class="spec" placeholder="Begin Object Class=/Script/BlueprintGraph.K2Node_CallFunction Name=&quot;K2Node_CallFunction_0&quot; …"></textarea>
        <label class="chk"><input v-model="requireEmpty" type="checkbox" />{{ t('仅当图为空时粘入（require_empty，防覆盖）') }}</label>
        <div class="m-row">
          <button class="btn" @click="showImport = false">{{ t('取消') }}</button>
          <button class="btn primary" :disabled="!t3dText.trim()" @click="doImportT3D()">{{ t('粘入蓝图并刷新') }}</button>
        </div>
      </div>
    </div>

    <!-- T3D 导出 -->
    <div v-if="showExport" class="modal-mask" @click.self="showExport = false">
      <div class="modal wide">
        <div class="d-head"><b>{{ t('T3D 导出') }} · {{ exportLen }} {{ t('字符') }}{{ exportTrunc ? t('（已截断）') : '' }}</b><button class="btn" @click="showExport = false">{{ t('关闭') }}</button></div>
        <textarea class="spec" readonly :value="t3dOut"></textarea>
        <button class="btn primary" @click="copyExport()">{{ t('复制全文') }}</button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue';
import { getUalStatus, listBlueprintGraphs, loadBlueprintGraph, ualCall } from '../api/ual';
import { exportT3D, importT3D } from '../api/nodes';
import type { UalGraphNode } from '../api/contract';
import { ContractError } from '../api/contract';
import { copyText as copyToClipboard } from '../utils/clipboard';
import { t } from '../i18n';

/**
 * 蓝图画布（2026-10-08，用户：像 NodeUE 蓝图查看器一样在面板里操作蓝图）。
 *
 * 数据源 = 内置 UAL 桥的 blueprint.get_graph（节点/引脚/连线/坐标一次拿全），
 * 渲染零外部依赖（HTML 节点卡 + SVG 贝塞尔），跟团队画板同一个路数。
 * 画布操作映射到真实引擎命令：
 *   删节点 → blueprint.delete_node（destructive，前端确认 + allowDestructive 双保险）
 *   断连线 → blueprint.disconnect_pins
 *   自动排序 → 画布分层排版，可选 blueprint.set_node_positions 写回
 *   T3D 导入/导出 → blueprint.import_t3d / export_t3d（NodeUE 完整功能）
 */

// ── 几何常量（world 单位 ≈ UE 坐标，1:1 映射）──
const NODE_W = 236;
const HEADER_H = 30;
const ROW_H = 22;
const PAD_V = 10;

interface PinView { name: string; label: string; color: string; exec: boolean; connected: boolean }
interface CanvasNode {
  id: string; cls: string; title: string; member: string;
  x: number; y: number; w: number; h: number;
  ghost: boolean; isComment: boolean;
  inPins: PinView[]; outPins: PinView[];
}
interface EdgeView { key: string; fromNode: string; fromPin: string; toNode: string; toPin: string; exec: boolean; color: string }
interface EdgePath extends EdgeView { d: string }

const PIN_COLORS: Record<string, string> = {
  exec: '#e6e6e6', boolean: '#ff7a7a', byte: '#7ce0a0', integer: '#6fe06f', int64: '#8fe6b0',
  real: '#54e0c6', string: '#ff9ed2', name: '#d9a8ff', text: '#e8c07a', struct: '#7fa8ff',
  object: '#5ec8ff', class: '#8f7aff', enum: '#a0e07f', interface: '#d0d060', delegate: '#ffd166',
  wildcard: '#f0c060',
};
function pinColor(cat: string): string {
  return PIN_COLORS[String(cat || '').toLowerCase()] || '#9aa4b2';
}

/** 节点头配色分类（事件红 / 变量青 / 流程灰 / 函数蓝） */
function headKind(n: CanvasNode): string {
  if (n.isComment) return 'k-comment';
  if (/Event|CustomEvent/.test(n.cls)) return 'k-event';
  if (/Variable/.test(n.cls)) return 'k-var';
  if (/IfThenElse|ExecutionSequence|Select|Switch/.test(n.cls)) return 'k-flow';
  return 'k-func';
}

function pinLabel(raw: { name: string; friendly_name?: string; category: string }): string {
  if (raw.friendly_name) return raw.friendly_name;
  if (raw.name) return raw.name;
  return raw.category === 'exec' ? 'exec' : 'value';
}

// ── 状态 ──
const bpPath = ref(localStorage.getItem('bp-canvas.path') || '');
const graphName = ref('');
const graphOptions = ref<string[]>([]);
const loading = ref(false);
const err = ref<string | null>(null);
const notice = ref<string | null>(null);
const ualOnline = ref<boolean | null>(null);

const nodes = ref<CanvasNode[]>([]);
const edges = ref<EdgeView[]>([]);
const selectedNode = ref<string | null>(null);
const selectedEdge = ref<string | null>(null);
const search = ref('');
const showCoords = ref(false);
const viewport = ref<HTMLDivElement | null>(null);
const view = reactive({ x: 60, y: 40, k: 1 });

const CFG_KEY = 'bp-canvas.settings';
const cfg = reactive<{ pinNames: boolean; grid: boolean; dataLinks: boolean; curve: boolean; writeBack: boolean }>(
  (() => {
    try {
      const saved = JSON.parse(localStorage.getItem(CFG_KEY) || '{}');
      return { pinNames: true, grid: true, dataLinks: true, curve: true, writeBack: false, ...saved };
    } catch {
      return { pinNames: true, grid: true, dataLinks: true, curve: true, writeBack: false };
    }
  })(),
);
watch(cfg, () => localStorage.setItem(CFG_KEY, JSON.stringify(cfg)), { deep: true });

const showSettings = ref(false);
const showSpec = ref(false);
const showDelete = ref(false);
const showImport = ref(false);
const showExport = ref(false);
const t3dText = ref('');
const requireEmpty = ref(false);
const t3dOut = ref('');
const exportLen = ref(0);
const exportTrunc = ref(false);

// ── 几何 ──
function nodeHeight(n: CanvasNode): number {
  return HEADER_H + Math.max(n.inPins.length, n.outPins.length) * ROW_H + PAD_V * 2;
}

const byId = computed(() => new Map(nodes.value.map((n) => [n.id, n])));

/** 引脚锚点（world 坐标）：in 在左边缘、out 在右边缘，按各自列序号排 */
function pinPoint(nid: string, pinName: string, side: 'in' | 'out'): { x: number; y: number } | null {
  const n = byId.value.get(nid);
  if (!n) return null;
  const list = side === 'in' ? n.inPins : n.outPins;
  const i = list.findIndex((p) => p.name === pinName);
  if (i < 0) return null;
  return { x: side === 'in' ? n.x : n.x + n.w, y: n.y + HEADER_H + PAD_V + i * ROW_H + ROW_H / 2 };
}

const visibleEdges = computed<EdgePath[]>(() => {
  const out: EdgePath[] = [];
  for (const e of edges.value) {
    if (e.exec && !cfg.dataLinks) {
      /* exec 线永远显示 */
    } else if (!e.exec && !cfg.dataLinks) continue;
    const a = pinPoint(e.fromNode, e.fromPin, 'out');
    const b = pinPoint(e.toNode, e.toPin, 'in');
    if (!a || !b) continue;
    const dx = Math.min(Math.max(Math.abs(b.x - a.x) * 0.5, 48), 170);
    const d = cfg.curve
      ? `M ${a.x} ${a.y} C ${a.x + dx} ${a.y}, ${b.x - dx} ${b.y}, ${b.x} ${b.y}`
      : `M ${a.x} ${a.y} L ${b.x} ${b.y}`;
    out.push({ ...e, d });
  }
  return out;
});

// ── 读图 → 画布 ──
function buildCanvas(raw: UalGraphNode[]): void {
  const cn: CanvasNode[] = [];
  const ce: EdgeView[] = [];
  for (const r of raw) {
    const isComment = r.class === 'EdGraphNode_Comment' || !!r.comment_text;
    const inPins: PinView[] = [];
    const outPins: PinView[] = [];
    for (const p of r.pins || []) {
      const pv: PinView = {
        name: p.name || (p.category === 'exec' ? 'exec' : 'value'),
        label: pinLabel(p),
        color: pinColor(p.category),
        exec: p.category === 'exec',
        connected: !!p.is_connected,
      };
      if (p.dir === 'Input') inPins.push(pv);
      else outPins.push(pv);
      // 连线只从 Input 侧收一次（linked_to 双向都记，防重复）
      if (p.dir === 'Input') {
        for (const l of p.linked_to || []) {
          if (!l.node_id) continue;
          ce.push({
            key: `${l.node_id}::${l.pin_name}=>${r.node_id}::${pv.name}`,
            fromNode: l.node_id, fromPin: l.pin_name || 'exec',
            toNode: r.node_id, toPin: pv.name,
            exec: p.category === 'exec',
            color: pinColor(p.category),
          });
        }
      }
    }
    const n: CanvasNode = {
      id: r.node_id, cls: r.class, title: r.title || r.class,
      member: r.member_name || '',
      x: r.pos_x ?? 0, y: r.pos_y ?? 0, w: NODE_W, h: 60,
      ghost: !!r.is_ghost_node, isComment,
      inPins, outPins,
    };
    if (isComment) {
      n.w = r.node_width || 320;
      n.h = r.node_height || 160;
    } else {
      n.h = nodeHeight(n);
    }
    cn.push(n);
  }
  nodes.value = cn;
  edges.value = ce;
  selectedNode.value = null;
  selectedEdge.value = null;
}

async function loadGraph(): Promise<void> {
  const p = bpPath.value.trim();
  if (!p) {
    err.value = '先填蓝图路径，如 /Game/Blueprints/BP_Example';
    return;
  }
  loading.value = true;
  err.value = null;
  notice.value = null;
  try {
    const data = await loadBlueprintGraph(p, graphName.value || undefined);
    bpPath.value = data.blueprint_path || p;
    graphName.value = data.graph_name || '';
    buildCanvas(data.nodes || []);
    localStorage.setItem('bp-canvas.path', bpPath.value);
    notice.value = `已加载 ${graphName.value || '图'}：${nodes.value.length} 节点 / ${visibleEdges.value.length} 连线`;
    refreshGraphList();
  } catch (e) {
    err.value = fmtErr(e);
  } finally {
    loading.value = false;
  }
}

async function refreshGraphList(): Promise<void> {
  try {
    const r = await listBlueprintGraphs(bpPath.value);
    graphOptions.value = (r.graph_names || []).slice();
  } catch {
    graphOptions.value = []; /* 列图失败不阻断（旧插件没有 list_graphs 也能用） */
  }
}

// ── 视图变换 ──
function onWheel(e: WheelEvent): void {
  e.preventDefault();
  const rect = viewport.value?.getBoundingClientRect();
  if (!rect) return;
  const f = e.deltaY < 0 ? 1.12 : 1 / 1.12;
  const k = Math.min(Math.max(view.k * f, 0.2), 2.5);
  const rf = k / view.k;
  const mx = e.clientX - rect.left;
  const my = e.clientY - rect.top;
  view.x = mx - (mx - view.x) * rf;
  view.y = my - (my - view.y) * rf;
  view.k = k;
}

let pan: { sx: number; sy: number; vx: number; vy: number } | null = null;
function onBgDown(e: MouseEvent): void {
  if (e.button !== 0) return;
  selectedNode.value = null;
  selectedEdge.value = null;
  pan = { sx: e.clientX, sy: e.clientY, vx: view.x, vy: view.y };
  window.addEventListener('mousemove', onPanMove);
  window.addEventListener('mouseup', onPanUp);
}
function onPanMove(e: MouseEvent): void {
  if (!pan) return;
  view.x = pan.vx + (e.clientX - pan.sx);
  view.y = pan.vy + (e.clientY - pan.sy);
}
function onPanUp(): void {
  pan = null;
  window.removeEventListener('mousemove', onPanMove);
  window.removeEventListener('mouseup', onPanUp);
}

let drag: { id: string; sx: number; sy: number; ox: number; oy: number } | null = null;
function onNodeDown(n: CanvasNode, e: MouseEvent): void {
  if (e.button !== 0) return;
  selectedNode.value = n.id;
  selectedEdge.value = null;
  drag = { id: n.id, sx: e.clientX, sy: e.clientY, ox: n.x, oy: n.y };
  window.addEventListener('mousemove', onNodeMove);
  window.addEventListener('mouseup', onNodeUp);
}
function onNodeMove(e: MouseEvent): void {
  if (!drag) return;
  const n = byId.value.get(drag.id);
  if (!n) return;
  n.x = Math.round(drag.ox + (e.clientX - drag.sx) / view.k);
  n.y = Math.round(drag.oy + (e.clientY - drag.sy) / view.k);
}
function onNodeUp(): void {
  drag = null;
  window.removeEventListener('mousemove', onNodeMove);
  window.removeEventListener('mouseup', onNodeUp);
}

function resetView(): void {
  view.x = 60;
  view.y = 40;
  view.k = 1;
}

// ── 自动排序：最长路径分层（执行线 + 数据线同方向 out→in），可选写回引擎 ──
async function autoArrange(): Promise<void> {
  const real = nodes.value.filter((n) => !n.isComment);
  if (!real.length) return;
  const layer = new Map<string, number>(real.map((n) => [n.id, 0]));
  // 迭代松弛（O(V·E)，蓝图规模足够；有环自动收敛到稳定值）
  for (let i = 0; i < real.length; i++) {
    let changed = false;
    for (const e of edges.value) {
      const ls = layer.get(e.fromNode);
      const lt = layer.get(e.toNode);
      if (ls === undefined || lt === undefined) continue;
      if (ls + 1 > lt) {
        layer.set(e.toNode, ls + 1);
        changed = true;
      }
    }
    if (!changed) break;
  }
  const byLayer = new Map<number, CanvasNode[]>();
  for (const n of real) {
    const l = layer.get(n.id) || 0;
    if (!byLayer.has(l)) byLayer.set(l, []);
    byLayer.get(l)!.push(n);
  }
  for (const [l, col] of [...byLayer.entries()].sort((a, b) => a[0] - b[0])) {
    col.sort((a, b) => a.y - b.y);
    let y = 80;
    for (const n of col) {
      n.x = 80 + l * 300;
      n.y = y;
      y += n.h + 48;
    }
  }
  // 注释框不动（它框的是相对位置，跟着逻辑走才有意义）
  if (cfg.writeBack) {
    try {
      const positions = real.map((n) => ({ node_id: n.id, x: n.x, y: n.y }));
      const r = await ualCall('blueprint_tidy_graph', {
        blueprint_path: bpPath.value,
        graph_name: graphName.value || undefined,
        positions,
      });
      if (!r.ok) throw new Error(r.error || '写回失败');
      const moved = (r.data as { moved?: number } | undefined)?.moved ?? positions.length;
      notice.value = `已排版并写回引擎（moved=${moved}）。注释框未动。`;
    } catch (e) {
      err.value = fmtErr(e);
      return;
    }
  } else {
    notice.value = '已排版（仅画布内；设置里开「自动排序后写回引擎坐标」可同步 UE）';
  }
}

// ── 搜索 ──
const hits = computed<CanvasNode[]>(() => {
  const q = search.value.trim().toLowerCase();
  if (!q) return [];
  return nodes.value.filter((n) => `${n.title} ${n.cls} ${n.member}`.toLowerCase().includes(q));
});
const hitIds = computed(() => new Set(hits.value.map((n) => n.id)));
function isHit(id: string): boolean {
  return hitIds.value.has(id);
}
function centerOn(n: CanvasNode): void {
  const rect = viewport.value?.getBoundingClientRect();
  if (!rect) return;
  view.x = rect.width / 2 - (n.x + n.w / 2) * view.k;
  view.y = rect.height / 2 - (n.y + n.h / 2) * view.k;
}
function onSearchEnter(): void {
  if (hits.value.length) centerOn(hits.value[0]);
}

// ── 画布操作 → 引擎命令 ──
const deleteTargetTitle = computed(() => {
  const n = selectedNode.value ? byId.value.get(selectedNode.value) : null;
  return n ? `${n.title}（${n.cls}）` : '';
});

async function doDelete(): Promise<void> {
  const id = selectedNode.value;
  showDelete.value = false;
  if (!id) return;
  try {
    const r = await ualCall(
      'blueprint_delete_node',
      { blueprint_path: bpPath.value, graph_name: graphName.value || undefined, node_id: id },
      { allowDestructive: true }, // 用户已在确认弹窗点过「确认删除」
    );
    if (!r.ok) throw new Error(r.error || '删除失败');
    notice.value = '节点已删除';
    await loadGraph();
  } catch (e) {
    err.value = fmtErr(e);
  }
}

async function disconnectSelected(): Promise<void> {
  const key = selectedEdge.value;
  if (!key) return;
  const e = edges.value.find((x) => x.key === key);
  if (!e) return;
  try {
    const r = await ualCall('blueprint_disconnect_pins', {
      blueprint_path: bpPath.value,
      graph_name: graphName.value || undefined,
      from: `${e.fromNode}.${e.fromPin}`,
      to: `${e.toNode}.${e.toPin}`,
    });
    if (!r.ok) throw new Error(r.error || '断开失败');
    selectedEdge.value = null;
    notice.value = '连线已断开';
    await loadGraph();
  } catch (err2) {
    err.value = fmtErr(err2);
  }
}

async function compileBp(): Promise<void> {
  try {
    const r = await ualCall('blueprint_compile', { blueprint_path: bpPath.value });
    if (!r.ok) throw new Error(r.error || '编译请求失败');
    const d = r.data as { errors?: unknown; message?: string } | undefined;
    const errs = Array.isArray(d?.errors) ? (d?.errors as string[]) : [];
    notice.value = errs.length ? `编译完成：${errs.length} 个错误 —— ${errs.slice(0, 2).join(' | ')}` : '编译通过 ✓';
  } catch (e) {
    err.value = fmtErr(e);
  }
}

async function doImportT3D(): Promise<void> {
  const text = t3dText.value;
  if (!text.trim()) return;
  try {
    const r = await importT3D(bpPath.value, text, {
      graph_name: graphName.value || undefined,
      require_empty: requireEmpty.value,
    });
    if (!r.ok) throw new Error(r.error || '导入失败');
    const d = r.data;
    notice.value = `已粘入 ${d?.imported_count ?? '?'} 个节点${d?.removed_ghost_events?.length ? `，替换灰占位事件：${d.removed_ghost_events.join(', ')}` : ''}`;
    showImport.value = false;
    t3dText.value = '';
    await loadGraph();
  } catch (e) {
    err.value = fmtErr(e);
  }
}

async function doExportT3D(): Promise<void> {
  try {
    const r = await exportT3D(bpPath.value, { graph_name: graphName.value || undefined });
    if (!r.ok || !r.data) throw new Error(r.error || '导出失败');
    t3dOut.value = r.data.text;
    exportLen.value = r.data.text_length;
    exportTrunc.value = !!r.data.truncated;
    showExport.value = true;
  } catch (e) {
    err.value = fmtErr(e);
  }
}

// ── 格式规范（AI 提示词）──
const SPEC_PROMPT = `你是 UE5 蓝图工程师。请生成可被 blueprint_import_t3d 粘回 UE 编辑器的蓝图节点 T3D 文本（与编辑器 Ctrl+C 复制出的格式同构）。

【硬性规则】
1. 每个节点一段：Begin Object Class=<节点类完整路径> Name="<文本内唯一名>" ... End Object。导入时 GUID 自动重排，Name 只需文本内唯一。
2. 常用节点类：
   - 函数调用  /Script/BlueprintGraph.K2Node_CallFunction，字段 FunctionReference=(MemberName="函数名",bSelfContext=True)
     （成员函数带 bSelfContext=True；静态/库函数改用 MemberParent=/Script/包.类）
   - 变量读取 K2Node_VariableGet / 变量写入 K2Node_VariableSet：VariableReference=(MemberName="变量名",bSelfContext=True)
   - 分支 K2Node_IfThenElse · 序列 K2Node_ExecutionSequence · 动态 Cast K2Node_DynamicCast（TargetType=/Script/…）
   - 自定义事件 K2Node_CustomEvent（CustomFunctionName="…"）· 事件 BeginPlay 等用 K2Node_Event（EventReference=…）
3. 引脚写法（需要连线/默认值时才必须写）：
   CustomProperties Pin (PinId=00000000000000000000000000000000,PinName="execute",PinType.PinCategory="exec",LinkedTo=(对端Name 对端PinId))
   PinCategory 常见：exec / boolean / byte / integer / int64 / real / string / name / text / struct / object / interface / delegate / wildcard。
4. 省略 Pins 时引擎按节点类自动补默认引脚 —— 不连线就别写 Pin，越少越不容易错。
5. 粘入事件图时，错位的引擎灰占位事件（BeginPlay/Tick 等）会被自动替换；文本总量上限 20 万字符。
6. 只输出纯 T3D 文本（不要 Markdown 围栏以外的解释文字）。
【强烈建议】先对目标蓝图调用 blueprint_export_t3d 导出一份真实样本，照抄其字段与结构改写 —— 拼写以导出原文为准，别凭记忆。`;

function copySpec(): void {
  void copyToClipboard(SPEC_PROMPT).then(() => {
    notice.value = '格式规范已复制';
  }).catch(() => {
    err.value = '复制失败（浏览器剪贴板不可用）';
  });
}

function copyExport(): void {
  void copyToClipboard(t3dOut.value).then(() => {
    notice.value = 'T3D 已复制';
  }).catch(() => {
    err.value = '复制失败，请在文本框里手动全选复制';
  });
}

// ── 其它 ──
function fmtErr(e: unknown): string {
  if (e instanceof ContractError) return `${e.kind} · ${e.message}`;
  const m = (e as Error)?.message || String(e);
  return m;
}

function onKey(e: KeyboardEvent): void {
  const tag = (e.target as HTMLElement)?.tagName;
  if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
  if ((e.key === 'Delete' || e.key === 'Backspace') && selectedNode.value) {
    e.preventDefault();
    showDelete.value = true;
  }
  if (e.key === 'Escape') {
    selectedNode.value = null;
    selectedEdge.value = null;
    showSettings.value = false;
  }
}

onMounted(async () => {
  window.addEventListener('keydown', onKey);
  try {
    const s = await getUalStatus();
    ualOnline.value = s.listening && s.connectionCount > 0;
  } catch {
    ualOnline.value = false;
  }
});

onBeforeUnmount(() => {
  window.removeEventListener('keydown', onKey);
  window.removeEventListener('mousemove', onPanMove);
  window.removeEventListener('mouseup', onPanUp);
  window.removeEventListener('mousemove', onNodeMove);
  window.removeEventListener('mouseup', onNodeUp);
});
</script>

<style scoped>
.bp-panel { display: flex; flex-direction: column; height: 100%; gap: 8px; }

/* 工具栏（对照 NodeUE 蓝图查看器顶部） */
.bp-bar { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
.brand { font-weight: 600; }
.dot { width: 9px; height: 9px; border-radius: 50%; display: inline-block; }
.dot.on { background: #4cc38a; box-shadow: 0 0 6px #4cc38a; }
.dot.off { background: #e5534b; }
.inp { background: rgba(255, 255, 255, 0.07); border: 1px solid rgba(255, 255, 255, 0.14); border-radius: 6px; color: inherit; padding: 4px 8px; font-size: 0.846rem; }
.bp-path { width: 280px; }
.bp-search { width: 130px; }
.sel { min-width: 110px; }
.btn { background: rgba(255, 255, 255, 0.09); border: 1px solid rgba(255, 255, 255, 0.16); border-radius: 6px; color: inherit; padding: 4px 10px; cursor: pointer; font-size: 0.846rem; white-space: nowrap; }
.btn:hover:not(:disabled) { background: rgba(255, 255, 255, 0.16); }
.btn:disabled { opacity: 0.45; cursor: default; }
.btn.primary { border-color: var(--caution, #fff500); color: var(--caution, #fff500); }
.btn.warn { border-color: #e8b339; color: #e8b339; }
.btn.danger { border-color: #e5534b; color: #e5534b; }
.chk { display: inline-flex; align-items: center; gap: 4px; font-size: 0.846rem; white-space: nowrap; cursor: pointer; }
.grow { flex: 1; }
.hit-count { font-size: 0.769rem; opacity: 0.75; }

.err { color: #e5534b; font-size: 0.846rem; margin: 0; }
.notice { color: var(--caution, #fff500); font-size: 0.846rem; margin: 0; }

/* 画布本体：UE 风格暗色 + 网格（不随主题变量，保持看图体验） */
.bp-viewport { position: relative; flex: 1; min-height: 420px; overflow: hidden; border-radius: 8px; border: 1px solid rgba(255, 255, 255, 0.1); background: #15181e; cursor: grab; }
.bp-viewport:active { cursor: grabbing; }
.bp-world { position: absolute; left: 0; top: 0; transform-origin: 0 0; will-change: transform; }
.bp-world::before { content: ''; position: absolute; left: -5000px; top: -5000px; width: 10000px; height: 10000px; background-image: linear-gradient(rgba(255, 255, 255, 0.045) 1px, transparent 1px), linear-gradient(90deg, rgba(255, 255, 255, 0.045) 1px, transparent 1px); background-size: 24px 24px; pointer-events: none; }
.bp-edges { position: absolute; left: 0; top: 0; width: 1px; height: 1px; overflow: visible; pointer-events: none; }
.bp-edges .wire { fill: none; stroke: #cfcfcf; stroke-width: 2; opacity: 0.85; }
.bp-edges .wire.exec { stroke: #e6e6e6; }
.bp-edges .wire.sel { stroke: #fff500; stroke-width: 3.5; opacity: 1; }
.bp-edges .hit-area { fill: none; stroke: transparent; stroke-width: 12; pointer-events: stroke; cursor: pointer; }

/* 节点卡 */
.bp-node { position: absolute; background: #262b33; border: 1px solid rgba(255, 255, 255, 0.22); border-radius: 7px; overflow: hidden; cursor: move; box-shadow: 0 2px 10px rgba(0, 0, 0, 0.45); }
.bp-node.sel { border-color: #fff500; box-shadow: 0 0 0 2px rgba(255, 245, 0, 0.35), 0 2px 10px rgba(0, 0, 0, 0.45); }
.bp-node.hit { outline: 2px dashed #e8b339; outline-offset: 2px; }
.bp-node.ghost { opacity: 0.62; }
.n-head { height: 30px; line-height: 30px; padding: 0 10px; font-size: 0.846rem; font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.k-event { background: #5c1f24; }
.k-func { background: #24435c; }
.k-var { background: #1f4a44; }
.k-flow { background: #3a3f47; }
.k-comment { background: #4a4425; }
.n-body { display: flex; justify-content: space-between; padding: 10px 8px; gap: 12px; }
.col { display: flex; flex-direction: column; gap: 2px; min-width: 0; }
.col.out { align-items: flex-end; }
.pin { display: flex; align-items: center; gap: 5px; height: 22px; font-size: 0.769rem; opacity: 0.92; }
.pl { width: 10px; height: 10px; border-radius: 3px; flex: none; border: 1px solid rgba(0, 0, 0, 0.5); }
.pn { max-width: 88px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.coord { position: absolute; right: 4px; bottom: 2px; font-size: 0.69rem; opacity: 0.55; pointer-events: none; }
.c-text { font-size: 0.846rem; opacity: 0.9; padding: 6px 8px; display: block; }
.bp-node.comment { background: rgba(250, 220, 100, 0.07); border-color: rgba(250, 220, 100, 0.35); cursor: move; }

.bp-empty { position: absolute; inset: 0; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 6px; color: rgba(255, 255, 255, 0.65); font-size: 0.923rem; pointer-events: none; text-align: center; }
.bp-empty code { background: rgba(255, 255, 255, 0.09); padding: 1px 6px; border-radius: 4px; }
.bp-foot { font-size: 0.769rem; opacity: 0.6; }

/* 设置抽屉 / 弹窗 */
.drawer { position: fixed; right: 18px; top: 84px; z-index: 30; width: 280px; display: flex; flex-direction: column; gap: 10px; background: var(--panel, #1c2027); border: 1px solid rgba(255, 255, 255, 0.16); border-radius: 10px; padding: 14px; box-shadow: 0 8px 28px rgba(0, 0, 0, 0.5); }
.d-head { display: flex; justify-content: space-between; align-items: center; }
.modal-mask { position: fixed; inset: 0; z-index: 40; background: rgba(0, 0, 0, 0.55); display: flex; align-items: center; justify-content: center; }
.modal { background: var(--panel, #1c2027); border: 1px solid rgba(255, 255, 255, 0.16); border-radius: 10px; padding: 14px; width: 380px; max-width: 92vw; display: flex; flex-direction: column; gap: 10px; max-height: 84vh; }
.modal.wide { width: 720px; }
.m-hint { font-size: 0.846rem; opacity: 0.8; margin: 0; }
.danger-text { color: #ff9a94; }
.m-row { display: flex; justify-content: flex-end; gap: 8px; }
.spec { width: 100%; height: 300px; resize: vertical; background: rgba(0, 0, 0, 0.35); border: 1px solid rgba(255, 255, 255, 0.14); border-radius: 6px; color: inherit; font-family: Consolas, monospace; font-size: 0.769rem; padding: 8px; }
</style>
