/**
 * XiaoMan Box —— 前后端契约 v1
 *
 * 这是「前后端彻底分离」的落点（立项 §9.2 铁律 #1）：
 * 前端（Vue3+TS）与后端（独立进程 backend/server.mjs）之间，
 * 只靠这里定义的、带版本号的 HTTP 契约通信。
 *
 * 契约演化规则：
 *   - 破坏性变更 → 升大版本，走新路径 /api/v2/...
 *   - 新增字段（向后兼容）→ 保持 v1，前端按可选字段处理
 */

export const CONTRACT_VERSION = 'v1' as const;

/** 后端端口（与 backend/server.mjs 的 WORKBENCH_BACKEND_PORT 默认值一致） */
export const BACKEND_PORT = 8871;

/**
 * 契约根地址。默认指向本机后端进程；
 * 可通过 Vite 环境变量 VITE_API_BASE 覆盖（例如指向局域网另一台机器）。
 */
export const API_BASE: string =
  (import.meta.env.VITE_API_BASE as string | undefined) ??
  `http://127.0.0.1:${BACKEND_PORT}/api/v1`;

export interface HealthResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  app: string;
  version: string;
  ts: string;
}

export interface StatusResponse {
  contract: typeof CONTRACT_VERSION;
  app: string;
  backendPid: number;
  hostname: string;
  platform: string;
  arch: string;
  nodeVersion: string;
  uptimeSec: number;
  memory: { rss: number; heapUsed: number };
}

/* ── W-07 状态持久化 ─────────────────────────────────────── */

export interface StateGetResponse {
  contract: typeof CONTRACT_VERSION;
  key: string;
  data: unknown;
}

export interface StatePutResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  key: string;
  bytes: number;
}

/* ── seam.ue5：UEBox MCP ─────────────────────────────────── */

export interface UeboxStatusResponse {
  contract: typeof CONTRACT_VERSION;
  configured: boolean;
  endpoint: string | null;
  connected: boolean;
  serverInfo?: { name: string; version: string } | null;
  protocolVersion?: string;
  toolCount?: number;
  safeCount?: number;
  mutatingCount?: number;
  destructiveCount?: number;
  writeCount?: number;
  namespaceCount?: number;
  /** 分级数据来源：cli（权威）| mcp（降级，无分级） */
  riskSource?: 'cli' | 'mcp';
  error?: string;
}

export interface UeboxToolInfo {
  name: string;
  brief?: string;
  /** safe | mutating | destructive（仅 riskSource=cli 时有值） */
  risk?: 'safe' | 'mutating' | 'destructive';
  namespace?: string;
}

export interface UeboxToolsResponse {
  contract: typeof CONTRACT_VERSION;
  configured: boolean;
  endpoint: string | null;
  count: number;
  tools: UeboxToolInfo[];
  error?: string;
}

/* ── seam.ual：内置 UnrealAgentLink 服务端（UEBox 拆解融合）────── */

export interface UalConnectionInfo {
  connId: string;
  connected: boolean;
  project: { name: string | null; path: string | null; engine: string | null } | null;
  engine: string | null;
  connectedAt: number;
  lastHeartbeat: number;
  lastEvt: string | null;
}

export interface UalStatusResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  listening: boolean;
  port: number | null;
  connectionCount: number;
  toolCount: number;
  connections: UalConnectionInfo[];
}

/* ── seam.nodelib：内置蓝图节点参考库（NodeUE 拆解内置）────────── */

export interface NodeLibPin {
  name: string;
  type: string;
  dir: 'in' | 'out' | '';
  default?: string;
}

export interface NodeLibHit {
  name: string;
  member_name: string;
  kind: 'function' | 'macro';
  parent: string | null;
  pure: boolean | null;
  score: number;
  pins: NodeLibPin[];
}

export interface NodeLibStatsResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  nodeCount: number;
  functions: number;
  macros: number;
  uniqueNames: number;
  source: string;
}

export interface NodeLibSearchResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  query: string;
  total: number;
  returned: number;
  hits: NodeLibHit[];
}

export interface NodeLibPinFull extends NodeLibPin {
  subobj: string | null;
  hidden: boolean;
}

export interface NodeLibNode {
  name: string;
  member_name: string;
  kind: 'function' | 'macro';
  parent: string | null;
  pure: boolean | null;
  pins: NodeLibPinFull[];
}

export interface NodeLibDescribeResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  name: string;
  matches?: number;
  nodes?: NodeLibNode[];
  error?: string;
  candidates?: string[];
}

/* ── 蓝图画布（2026-10-08）：读图渲染 + 画布内操作（blueprint.get_graph 等）── */

export interface UalGraphPinLink {
  pin_name: string;
  dir?: string;
  node_id?: string;
  node_class?: string;
  node_title?: string;
}

export interface UalGraphPin {
  name: string;
  dir: 'Input' | 'Output' | string;
  category: string;
  sub_category?: string;
  is_array?: boolean;
  is_reference?: boolean;
  is_const?: boolean;
  sub_category_object?: string;
  friendly_name?: string;
  default_value?: string;
  default_object?: string;
  default_text?: string;
  linked_to?: UalGraphPinLink[];
  linked_count?: number;
  is_connected?: boolean;
}

export interface UalGraphNode {
  node_id: string;
  class: string;
  title: string;
  pos_x: number;
  pos_y: number;
  is_ghost_node?: boolean;
  write_as?: string;
  member_name?: string;
  struct_type?: string;
  comment_text?: string;
  node_width?: number;
  node_height?: number;
  font_size?: number;
  nodes_under_comment?: string[];
  pins: UalGraphPin[];
}

export interface UalGraphData {
  ok: boolean;
  blueprint_path: string;
  graph_name: string;
  nodes: UalGraphNode[];
}

export interface UalGraphListData {
  ok: boolean;
  blueprint_path: string;
  graph_count?: number;
  graph_names?: string[];
  event_graphs?: string[];
}

/** /ual/call 通用回执：executeUalTool 的 { ok, data?, error? } 外面再包一层 contract */
export interface UalCallEnvelope {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  data?: Record<string, unknown>;
  error?: string;
}

/* ── NodeUE 完整功能：蓝图 ↔ T3D 文本（W-55，走 UAL 桥零外部依赖）── */

export interface UalExportT3DResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  data?: {
    ok: boolean;
    blueprint_path: string;
    graph_name: string;
    node_count: number;
    exported_node_ids: string[];
    text: string;
    text_length: number;
    truncated: boolean;
    note?: string;
    not_found?: string[];
  };
  error?: string;
}

export interface UalImportT3DResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  data?: {
    ok: boolean;
    blueprint_path: string;
    graph_name: string;
    imported_count: number;
    nodes: { id: string; class: string; title: string; x: number; y: number }[];
    structural: boolean;
    undoable: boolean;
    removed_ghost_events?: string[];
    removed_ghost_events_note?: string;
    self_context_unresolved?: string[];
  };
  error?: string;
}

/* ── seam.vault：本地资料库（资产库 / 笔记 / 蓝图片段库 / 项目库）── */

export interface VaultAsset {
  id: string;
  name: string;
  path: string;
  type: string;
  tags: string[];
  note: string;
  folder: string;
}

export interface VaultNote {
  id: string;
  title: string;
  content: string;
  tags: string[];
  createdAt: string;
  updatedAt: string;
}

export interface VaultSnippet {
  id: string;
  name: string;
  tags: string[];
  blueprint_path: string;
  savedAt: string;
}

export interface VaultOverviewResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  total: number;
  by_type: Record<string, number>;
  by_folder: Record<string, number>;
  folders: string[];
  tags: string[];
  notes: number;
  snippets: number;
}

export interface VaultSearchAssetsResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  total: number;
  returned: number;
  assets: VaultAsset[];
}

export interface VaultSearchNotesResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  total: number;
  returned: number;
  notes: VaultNote[];
}

export interface VaultSearchSnippetsResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  total: number;
  returned: number;
  snippets: VaultSnippet[];
}

export interface VaultProjectListResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  action: string;
  projects?: { name: string; path: string }[];
  templates?: { name: string; path: string }[];
}

export interface VaultCallResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  error?: string;
  [key: string]: unknown;
}

/* ── seam.compiler：编译验证 ─────────────────────────────── */

export interface CompilerJobResponse {
  contract: typeof CONTRACT_VERSION;
  state: 'idle' | 'running' | 'done';
  jobId?: string;
  startedAt?: string;
  exitCode?: number | null;
  durationSec?: number | null;
  errors?: string[];
  error?: string | null;
  cfg?: { target: string; platform: string; config: string; project: string };
}

export interface CompilerStartResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  jobId?: string;
  code?: number;
  error?: string;
}

/* ── UE5 自动拉起编排 ────────────────────────────────────── */

export interface Ue5BootStep {
  key: string;
  label: string;
  state: 'pending' | 'running' | 'ok' | 'fail' | 'skip';
  detail: string;
  durationMs: number | null;
}

export interface Ue5BootResponse {
  contract: typeof CONTRACT_VERSION;
  state: 'idle' | 'running' | 'done' | 'failed' | 'aborted';
  jobId?: string;
  startedAt?: string;
  steps?: Ue5BootStep[];
  error?: string | null;
  projectPath?: string | null;
  projectName?: string;
  selectedPath?: string | null;
}

export interface Ue5BootStartResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  jobId?: string;
  code?: number;
  error?: string;
}

export interface Ue5Project {
  name: string;
  path: string;
  dir: string;
}

export interface Ue5ProjectsResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  scanDirs: string[];
  projects: Ue5Project[];
  selectedPath: string | null;
}

/* ── 小满 Agent（本地模型 function-calling 循环）──────────── */

export interface AgentStep {
  type: 'model' | 'tool' | 'verify';
  ms?: number;
  // model 步
  thinking?: string;
  tool_calls?: { name: string; arguments: string }[];
  answer?: string | null;
  error?: string;
  usage?: { promptTokens: number; completionTokens: number; cachedTokens: number };
  // tool 步
  name?: string;
  args?: unknown;
  ok?: boolean;
  result?: string;
  // verify 步（核查层）
  issues?: string[];
}

export interface AgentJobStats {
  rounds: number;
  modelMs: number;
  promptTokens: number;
  completionTokens: number;
  cachedTokens: number;
  tps?: number;
}

export interface AgentJobCtx {
  sceneId: string | null;
  memoryGlobalKeys: number;
  memorySceneKeys: number;
  historyMessages: number;
  skills: string[];
}

export interface AgentJobResponse {
  contract: typeof CONTRACT_VERSION;
  state: 'idle' | 'running' | 'done' | 'failed' | 'aborted';
  jobId?: string;
  startedAt?: string;
  task?: string;
  sessionId?: string | null;
  toolCount?: number;
  provider?: string | null;
  ctx?: AgentJobCtx | null;
  systemPrompt?: string | null;
  stats?: AgentJobStats;
  steps?: AgentStep[];
  answer?: string | null;
  error?: string | null;
  /** 实时思考流（2026-10-10 暴露思考过程）：流式推理时 reasoning_content 增量，前端实时滚动显示 */
  liveThinking?: string;
}

export interface AgentStartResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  jobId?: string;
  code?: number;
  error?: string;
}

/** 预设大板块（WorkBuddy 式两级：大板块 → 小板块） */
export interface PresetCategory {
  id: string;
  name: string;
  icon: string;
  desc?: string;
}

/** 预设（小板块）：id 即会话 pinTarget，prompt 即该模式专属系统提示词 */
export interface PresetInfo {
  id: string;
  category: string;
  name: string;
  icon: string;
  desc?: string;
  prompt?: string;
}

export interface PresetsResponse {
  contract: typeof CONTRACT_VERSION;
  ok: boolean;
  defaultPresetId: string;
  categories: PresetCategory[];
  presets: PresetInfo[];
}

/** 契约错误：区分「网络不通 / HTTP 非 2xx / 契约版本不符」三类，供前端降级提示。 */
export class ContractError extends Error {
  readonly kind: 'network' | 'http' | 'contract';
  constructor(message: string, kind: 'network' | 'http' | 'contract') {
    super(message);
    this.name = 'ContractError';
    this.kind = kind;
  }
}

export function assertContract(v: unknown): asserts v is { contract: typeof CONTRACT_VERSION } {
  const c = (v as { contract?: unknown } | null | undefined)?.contract;
  if (c !== CONTRACT_VERSION) {
    throw new ContractError(`契约版本不匹配：期望 ${CONTRACT_VERSION}，实得 ${String(c)}`, 'contract');
  }
}
