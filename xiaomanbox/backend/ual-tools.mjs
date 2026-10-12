/**
 * ual-tools —— 内置 UE 工具定义（2026-10-07，UEBox 拆解融合）
 *
 * 把 UEBox 的引擎工具变成工作台内置工具：每个工具 = 工具名（ue_xxx 下划线，
 * 满足厂商 ^[a-zA-Z0-9_-]+$ 约束）+ 插件命令（xxx.yyy 点分）+ 描述 + JSON Schema + 风险分级。
 *
 * 工具名/风险/命名空间取自 UEBox registry.ts；method 取自各工具实现与插件
 * RegisterCommands（plugin/UnrealAgentLink/.../UAL_*Commands.cpp）。
 * 执行统一走 ual.mjs 的 ualCall(method, params)，不再经 MCP。
 */

import * as ual from './ual.mjs';
import * as vault from './vault.mjs';

const S = (props, required = []) => ({ type: 'object', properties: props, required });

// ── 复用片段 ─────────────────────────────────────────────────
const BLUEPRINT_PATH = { blueprint_path: { type: 'string', description: '蓝图路径，如 /Game/Blueprints/BP_Door' } };
const ASSET_PATH = { asset_path: { type: 'string', description: '资产路径，如 /Game/Materials/M_Base' } };

/**
 * 工具表。字段：
 *   name      —— 给模型看的工具名（下划线）
 *   method    —— 插件 RPC 命令（点分）
 *   risk      —— safe / mutating / destructive（destructive 不进模型工具清单）
 *   desc      —— 描述（喂给模型的简短说明）
 *   params    —— JSON Schema（OpenAI function parameters）
 */
export const UAL_TOOLS = [
  // ── Blueprint ──────────────────────────────────────────────
  {
    name: 'blueprint_describe', method: 'blueprint.describe', risk: 'safe',
    desc: '读蓝图结构：变量、函数、事件、组件、父类。写蓝图前先调它摸清现状。',
    params: S(BLUEPRINT_PATH, ['blueprint_path']),
  },
  {
    name: 'blueprint_get_graph', method: 'blueprint.get_graph', risk: 'safe',
    desc: '读蓝图某张图（EventGraph/函数）的节点与连线，拿到 node_id 供回读核查。',
    params: S({ ...BLUEPRINT_PATH, graph_name: { type: 'string', description: '图名，如 EventGraph 或函数名，省略默认主事件图' } }, ['blueprint_path']),
  },
  {
    // 蓝图画布（2026-10-08）：选图下拉、写图前确认目标图都靠它
    name: 'blueprint_list_graphs', method: 'blueprint.list_graphs', risk: 'safe',
    desc: '列出蓝图的全部图（事件图/函数/宏）与名字清单。写图/读图前先列图，别猜图名。',
    params: S({ ...BLUEPRINT_PATH }, ['blueprint_path']),
  },
  {
    name: 'blueprint_search_nodes', method: 'blueprint.search_nodes', risk: 'safe',
    desc: '按关键字查蓝图可用节点的精确 member_name 和引脚名。写图前必调，别凭记忆。关键：成员函数（AddMovementInput、SetActorLocation、SetStaticMesh、GetActorLocation 等）只在类的父类链上，搜它们必须先传 blueprint_path（指向这个蓝图），否则只搜通用函数库、会漏掉成员函数。顺序：先 blueprint_create 建好蓝图 → 再传 blueprint_path 搜成员函数。',
    params: S({
      query: { type: 'string', description: '关键字，如 rotate / add movement input' },
      blueprint_path: { type: 'string', description: '蓝图路径（搜成员函数时必传）。传了才搜该蓝图的父类链（成员函数都在这），不传只搜通用函数库' },
    }, ['query']),
  },
  {
    name: 'blueprint_create', method: 'blueprint.create', risk: 'mutating',
    desc: '新建蓝图资产（Actor/组件等），可指定父类和路径。',
    params: S({
      blueprint_path: { type: 'string', description: '新蓝图路径，如 /Game/Blueprints/BP_MyActor' },
      parent_class: { type: 'string', description: '父类，如 Actor / ActorComponent，省略默认 Actor' },
      name: { type: 'string', description: '资产名，省略从路径取' },
    }, ['blueprint_path']),
  },
  {
    name: 'blueprint_apply_graph', method: 'blueprint.create_graph', risk: 'mutating',
    desc: '写蓝图节点图（唯一写图入口）。两种用法：①整图写入——nodes 建节点+connections 连线一次性发全（常用，节点在本次 nodes 里用 id 标签互相引用）；②增量补线——nodes 传 []，connections 里引用图里已有节点（必须用 GUID，见 nodes 说明）。pin_defaults 设常量。member_name 用 blueprint_search_nodes 查到的精确名。',
    params: S({
      blueprint_path: { type: 'string', description: '蓝图路径' },
      nodes: {
        type: 'array', description: '要建的节点。必填；传 [] 表示不建节点、只补已有节点之间的连线（补线模式）。注意：id 标签只在本次调用内有效，上一次调用用过的标签不会被记住——补线时引用已有节点必须用它们的 GUID（blueprint_get_graph 回的 node_id，或上次 apply_graph 成功回执 created_nodes[].node_id）',
        items: {
          type: 'object',
          properties: {
            id: { type: 'string', description: '节点标识，仅本次调用内用于连线/诊断引用，随便起不重复' },
            class: { type: 'string', description: '节点类：Event/Function/VariableGet/VariableSet/CustomEvent/Cast/SpawnActor 等' },
            member_name: { type: 'string', description: '函数/事件/变量的精确名（blueprint_search_nodes 查）' },
            pin_defaults: { type: 'object', description: '引脚常量默认值（键=引脚名，值=字符串）', additionalProperties: { type: 'string' } },
            position: { type: 'object', properties: { x: { type: 'number' }, y: { type: 'number' } } },
            target_class: { type: 'string', description: 'Cast/SpawnActor 的目标类' },
            struct_type: { type: 'string', description: 'MakeStruct/BreakStruct 的结构体' },
          },
          required: ['id', 'class'],
        },
      },
      connections: {
        type: 'array', description: '连线',
        items: {
          type: 'object',
          properties: {
            from: { type: 'string', description: '源「节点id.引脚名」，如 begin.then。节点id 两种来源：本次 nodes 里定义的标签，或图里已有节点的 GUID（补线模式只能用 GUID）' },
            to: { type: 'string', description: '目标「节点id.引脚名」，如 print.execute。节点id 来源同 from' },
          },
          required: ['from', 'to'],
        },
      },
      clear_existing: { type: 'boolean', description: '先清空现有节点再写（重写整片逻辑用，函数入口保留）' },
      compile: { type: 'boolean', description: '写完是否编译，默认 true' },
    }, ['blueprint_path', 'nodes']),
  },
  {
    name: 'blueprint_delete_node', method: 'blueprint.delete_node', risk: 'destructive',
    desc: '删蓝图里一个节点。接错/多建时用。',
    params: S({ ...BLUEPRINT_PATH, node_id: { type: 'string', description: '要删的节点 id' } }, ['blueprint_path', 'node_id']),
  },
  {
    name: 'blueprint_disconnect_pins', method: 'blueprint.disconnect_pins', risk: 'mutating',
    desc: '断开蓝图里一根连线。',
    params: S({
      ...BLUEPRINT_PATH,
      from: { type: 'string', description: '源「节点id.引脚名」' },
      to: { type: 'string', description: '目标「节点id.引脚名」' },
    }, ['blueprint_path']),
  },
  {
    name: 'blueprint_comment', method: 'blueprint.set_comment', risk: 'mutating',
    desc: '给蓝图节点加/改注释框（分组说明）。',
    params: S({
      ...BLUEPRINT_PATH,
      node_ids: { type: 'array', items: { type: 'string' }, description: '框住的节点 id 列表' },
      text: { type: 'string', description: '注释文字' },
    }, ['blueprint_path']),
  },
  {
    name: 'blueprint_add_component', method: 'blueprint.add_component', risk: 'mutating',
    desc: '给蓝图加一个组件（如 StaticMeshComponent / BoxCollision）。',
    params: S({
      ...BLUEPRINT_PATH,
      component_class: { type: 'string', description: '组件类名，如 StaticMeshComponent' },
      component_name: { type: 'string', description: '组件名，省略用类名' },
    }, ['blueprint_path', 'component_class']),
  },
  {
    name: 'blueprint_set_property', method: 'blueprint.set_property', risk: 'mutating',
    desc: '设置蓝图/组件上的属性（如 StaticMesh 组件的 StaticMesh 资产、碰撞配置）。',
    params: S({
      ...BLUEPRINT_PATH,
      target: { type: 'string', description: '目标：蓝图或组件名' },
      property_name: { type: 'string', description: '属性名' },
      value: { type: 'string', description: '值（字符串形式）' },
    }, ['blueprint_path', 'property_name', 'value']),
  },
  {
    name: 'blueprint_create_function', method: 'blueprint.create_function', risk: 'mutating',
    desc: '在蓝图里新建一个函数（含输入/输出参数）。',
    params: S({
      ...BLUEPRINT_PATH,
      function_name: { type: 'string', description: '函数名' },
      inputs: { type: 'array', items: { type: 'object', properties: { name: { type: 'string' }, type: { type: 'string' } } }, description: '输入参数' },
      outputs: { type: 'array', items: { type: 'object', properties: { name: { type: 'string' }, type: { type: 'string' } } }, description: '输出参数' },
    }, ['blueprint_path', 'function_name']),
  },
  {
    name: 'blueprint_add_variable', method: 'blueprint.add_variable', risk: 'mutating',
    desc: '给蓝图加一个变量。',
    params: S({
      ...BLUEPRINT_PATH,
      variable_name: { type: 'string', description: '变量名' },
      variable_type: {
        type: 'string',
        description:
          '类型。标量：float/bool/int/int64/double/string/name/text/byte；结构体/枚举：直接写名（Vector/Rotator/Transform/自定义名）；' +
          '对象引用（如 StaticMesh/Actor）：variable_type 写 "object"，类名放 object_class 字段。不要写 "object:XXX" 冒号格式。',
      },
      object_class: {
        type: 'string',
        description: '仅对象引用类型用：引用的类名，如 StaticMesh / Actor / Character（短名即可，自动补 U 前缀）',
      },
      is_array: { type: 'boolean', description: '是否数组变量' },
      default_value: { type: 'string', description: '默认值，省略用类型默认' },
    }, ['blueprint_path', 'variable_name', 'variable_type']),
  },
  {
    name: 'blueprint_set_variable_meta', method: 'blueprint.set_variable_meta', risk: 'mutating',
    desc: '设置变量元数据（实例可编辑/分类/工具提示），让变量暴露给实例在场景里调。',
    params: S({
      ...BLUEPRINT_PATH,
      variable_name: { type: 'string', description: '变量名' },
      instance_editable: { type: 'boolean', description: '是否实例可编辑' },
      category: { type: 'string', description: '分类' },
    }, ['blueprint_path', 'variable_name']),
  },
  {
    name: 'blueprint_remove_variable', method: 'blueprint.remove_variable', risk: 'destructive',
    desc: '删蓝图变量（会孤儿化引用它的节点）。',
    params: S({ ...BLUEPRINT_PATH, variable_name: { type: 'string', description: '变量名' } }, ['blueprint_path', 'variable_name']),
  },
  {
    name: 'blueprint_event_dispatcher', method: 'blueprint.event_dispatcher', risk: 'mutating',
    desc: '创建/管理事件分发器（蓝图间通信）。',
    params: S({
      ...BLUEPRINT_PATH,
      dispatcher_name: { type: 'string', description: '分发器名' },
      action: { type: 'string', description: 'create/bind/call，省略 create' },
    }, ['blueprint_path', 'dispatcher_name']),
  },
  {
    name: 'blueprint_component_event', method: 'blueprint.component_event', risk: 'mutating',
    desc: '给组件加触发事件（如 BeginOverlap 重叠触发）。所有触发式交互的入口。',
    params: S({
      ...BLUEPRINT_PATH,
      component_name: { type: 'string', description: '组件名' },
      event_name: { type: 'string', description: '事件名，如 OnComponentBeginOverlap' },
    }, ['blueprint_path', 'component_name', 'event_name']),
  },
  {
    name: 'blueprint_function_signature', method: 'blueprint.function_signature', risk: 'destructive',
    desc: '改函数签名（增删参数，可删函数）。',
    params: S({
      ...BLUEPRINT_PATH,
      function_name: { type: 'string', description: '函数名' },
      inputs: { type: 'array', items: { type: 'object' }, description: '新输入参数' },
      outputs: { type: 'array', items: { type: 'object' }, description: '新输出参数' },
    }, ['blueprint_path', 'function_name']),
  },
  {
    name: 'blueprint_set_parent_class', method: 'blueprint.set_parent_class', risk: 'destructive',
    desc: '改蓝图父类（可能打断一批节点）。',
    params: S({ ...BLUEPRINT_PATH, parent_class: { type: 'string', description: '新父类' } }, ['blueprint_path', 'parent_class']),
  },
  {
    name: 'blueprint_compile', method: 'blueprint.compile', risk: 'mutating',
    desc: '编译单个蓝图，返回错误列表。',
    params: S(BLUEPRINT_PATH, ['blueprint_path']),
  },
  {
    name: 'blueprint_tidy_graph', method: 'blueprint.set_node_positions', risk: 'mutating',
    desc: '自动排版蓝图节点（只挪位置不动逻辑）。',
    params: S(BLUEPRINT_PATH, ['blueprint_path']),
  },
  {
    name: 'blueprint_compile_all', method: 'blueprint.compile_all', risk: 'mutating',
    desc: '编译全部已改脏的蓝图。',
    params: S({}),
  },
  {
    // NodeUE 完整功能（W-55）：蓝图 ↔ T3D 文本双向转换。
    // 走引擎自己的节点序列化（编辑器 Ctrl+C/V 同一对 FEdGraphUtilities 函数），
    // 内嵌式零外部依赖——不碰 NodeUE.exe，直接经 UAL 桥驱动插件。
    name: 'blueprint_export_t3d', method: 'blueprint.export_t3d', risk: 'safe',
    desc: '把蓝图（或指定节点）导出成 T3D 文本（= 编辑器里 Ctrl+C 出来的那种复制粘贴格式）。模型能读、能改。不传 node_ids 导出整张图，传了只导出点到的节点（指向选区外的连线会变悬空端口，这是预期）。超 20 万字符会截断并置 truncated（截断过的文本粘不回去）。',
    params: S({
      blueprint_path: { type: 'string', description: '蓝图路径，如 /Game/Blueprints/BP_Door' },
      graph_name: { type: 'string', description: '图名，如 EventGraph 或函数名，省略默认主事件图' },
      node_ids: { type: 'array', description: '要导出的节点 id 列表（blueprint_get_graph 回的 node_id）。省略=导整张图', items: { type: 'string' } },
    }, ['blueprint_path']),
  },
  {
    name: 'blueprint_import_t3d', method: 'blueprint.import_t3d', risk: 'mutating',
    desc: '把 T3D 文本粘进蓝图（= 编辑器里 Ctrl+V）。text 是 blueprint_export_t3d 导出的原文（或模型改过的）。粘入的节点自动换新 GUID、错位事件节点会替换掉引擎自带的灰色占位事件。require_empty=true 时图非空则一字不写（409）。',
    params: S({
      blueprint_path: { type: 'string', description: '蓝图路径，如 /Game/Blueprints/BP_Door' },
      graph_name: { type: 'string', description: '图名，如 EventGraph 或函数名，省略默认主事件图' },
      text: { type: 'string', description: '要粘入的 T3D 文本（导出原文或改过的）' },
      require_empty: { type: 'boolean', description: '要求目标图为空，非空则拒绝写入（防覆盖已有逻辑），默认 false' },
      offset_x: { type: 'number', description: '粘入节点整体 X 偏移（图坐标）' },
      offset_y: { type: 'number', description: '粘入节点整体 Y 偏移（图坐标）' },
    }, ['blueprint_path', 'text']),
  },

  // ── Actor ──────────────────────────────────────────────────
  {
    name: 'ue_spawn_actor', method: 'actor.spawn', risk: 'mutating',
    desc: '在场景创建 Actor（单体/批量）。asset_id 支持别名（cube/sphere/cylinder/cone/plane/point_light/spot_light/directional_light/rect_light/camera）、资源路径或类名；可带 transform 或顶层 location/rotation/scale（位置单位厘米，缩放是倍数）。',
    params: S({
      instances: {
        type: 'array', description: '待创建实例列表（最多 50）',
        items: {
          type: 'object',
          properties: {
            asset_id: { type: 'string', description: '别名/资源路径/类名' },
            name: { type: 'string', description: '强制命名' },
            mesh: { type: 'string', description: '覆盖静态网格路径' },
            transform: {
              type: 'object',
              properties: {
                location: { type: 'object', properties: { x: { type: 'number' }, y: { type: 'number' }, z: { type: 'number' } } },
                rotation: { type: 'object', properties: { pitch: { type: 'number' }, yaw: { type: 'number' }, roll: { type: 'number' } } },
                scale: { type: 'object', properties: { x: { type: 'number' }, y: { type: 'number' }, z: { type: 'number' } } },
              },
            },
          },
        },
      },
      asset_id: { type: 'string', description: '单体：别名/路径/类名' },
      name: { type: 'string' },
      location: { type: 'object', properties: { x: { type: 'number' }, y: { type: 'number' }, z: { type: 'number' } } },
      rotation: { type: 'object', properties: { pitch: { type: 'number' }, yaw: { type: 'number' }, roll: { type: 'number' } } },
      scale: { type: 'object', properties: { x: { type: 'number' }, y: { type: 'number' }, z: { type: 'number' } } },
    }),
  },
  {
    name: 'ue_destroy_actor', method: 'actor.destroy', risk: 'destructive',
    desc: '销毁场景里的 Actor（不可逆）。',
    params: S({ name: { type: 'string', description: 'Actor 名或路径' }, actor_path: { type: 'string' } }),
  },
  {
    name: 'ue_get_actor', method: 'actor.get_info', risk: 'safe',
    desc: '读 Actor 的基本信息、变换和包围盒。可按名或路径查，return_bounds=true 时返回包围盒。',
    params: S({
      name: { type: 'string', description: 'Actor 名；省略列出场景，默认最多 50 个' },
      path: { type: 'string', description: 'Actor 对象路径，与 name 二选一' },
      return_bounds: { type: 'boolean', description: '是否返回包围盒 bounds_min/bounds_max/pivot_offset' },
    }),
  },
  {
    name: 'ue_set_property', method: 'actor.set_property', risk: 'mutating',
    desc: '改 Actor 或组件的属性（如灯光强度、材质参数）。',
    params: S({
      name: { type: 'string', description: 'Actor 名' },
      component: { type: 'string', description: '组件名，省略改 Actor 本身' },
      property_name: { type: 'string', description: '属性名' },
      value: { description: '属性值；数值和布尔值使用对应 JSON 类型', anyOf: [{ type: 'string' }, { type: 'number' }, { type: 'boolean' }, { type: 'object' }, { type: 'array' }] },
    }, ['name', 'property_name', 'value']),
  },
  {
    name: 'ue_set_transform', method: 'actor.set_transform', risk: 'mutating',
    desc: '改 Actor 的位置/旋转/缩放。可 snap_to_floor 落地；灯光角度使用 rotation。',
    params: S({
      name: { type: 'string', description: 'Actor 名' },
      location: { type: 'object', properties: { x: { type: 'number' }, y: { type: 'number' }, z: { type: 'number' } } },
      rotation: { type: 'object', properties: { pitch: { type: 'number' }, yaw: { type: 'number' }, roll: { type: 'number' } } },
      scale: { type: 'object', properties: { x: { type: 'number' }, y: { type: 'number' }, z: { type: 'number' } } },
      snap_to_floor: { type: 'boolean', description: '落地对齐' },
    }, ['name']),
  },

  // ── Material ───────────────────────────────────────────────
  {
    name: 'material_create', method: 'material.create', risk: 'mutating',
    desc: '新建材质资产。',
    params: S({
      material_path: { type: 'string', description: '材质路径，如 /Game/Materials/M_Base' },
      parent: { type: 'string', description: '父材质，省略默认 M_Master/Default' },
    }, ['material_path']),
  },
  {
    name: 'material_create_instance', method: 'material.create_instance', risk: 'mutating',
    desc: '创建材质实例（基于母材质调参）。',
    params: S({
      material_path: { type: 'string', description: '新实例路径' },
      parent: { type: 'string', description: '母材质路径' },
    }, ['material_path', 'parent']),
  },
  {
    name: 'material_apply', method: 'material.apply', risk: 'mutating',
    desc: '把材质应用给 Actor 的组件（整图/属性应用）。',
    params: S({
      actor_name: { type: 'string', description: 'Actor 名' },
      component: { type: 'string', description: '组件名' },
      material_path: { type: 'string', description: '材质路径' },
    }, ['material_path']),
  },
  {
    name: 'material_describe', method: 'material.describe', risk: 'safe',
    desc: '读材质结构（节点/参数/父材质）。',
    params: S(ASSET_PATH, ['asset_path']),
  },
  {
    name: 'material_set_property', method: 'material.set_property', risk: 'mutating',
    desc: '改材质属性（如 shading model、blend mode）。',
    params: S({ ...ASSET_PATH, property_name: { type: 'string' }, value: { type: 'string' } }, ['asset_path', 'property_name', 'value']),
  },
  {
    name: 'material_set_param', method: 'material.set_param', risk: 'mutating',
    desc: '改材质实例参数（颜色/标量/贴图）。',
    params: S({
      asset_path: { type: 'string', description: '材质实例路径' },
      param_name: { type: 'string', description: '参数名' },
      value: { type: 'string', description: '值' },
    }, ['asset_path', 'param_name', 'value']),
  },
  {
    name: 'material_get_graph', method: 'material.get_graph', risk: 'safe',
    desc: '读材质节点图。',
    params: S(ASSET_PATH, ['asset_path']),
  },
  {
    name: 'material_search_nodes', method: 'material.search_nodes', risk: 'safe',
    desc: '查材质节点类型与引脚。',
    params: S({ query: { type: 'string' } }, ['query']),
  },
  {
    name: 'material_compile', method: 'material.compile', risk: 'mutating',
    desc: '编译材质，返回错误。',
    params: S(ASSET_PATH, ['asset_path']),
  },

  // ── Editor / Project ───────────────────────────────────────
  {
    name: 'ue_screenshot', method: 'editor.screenshot', risk: 'safe',
    desc: '对编辑器视口截图，返回图片路径（自动曝光时先锁曝光再调）。',
    params: S({ path: { type: 'string', description: '保存路径，省略默认 Saved/Screenshots' } }),
  },
  {
    name: 'ue_focus_viewport', method: 'viewport.focus', risk: 'mutating',
    desc: '把视口镜头对准某 Actor（等同于选中按 F）。',
    params: S({ name: { type: 'string', description: 'Actor 名' } }, ['name']),
  },
  {
    name: 'ue_get_project_info', method: 'system.get_project_info', risk: 'safe',
    desc: '读工程信息：路径、引擎版本、有没有 C++、插件能力声明。',
    params: S({}),
  },
  {
    name: 'ue_get_selection', method: 'editor.get_focus_context', risk: 'safe',
    desc: '读用户当前选中/打开的资产或 Actor。',
    params: S({}),
  },
  {
    name: 'ue_list_unsaved', method: 'editor.get_dirty', risk: 'safe',
    desc: '列出未保存的改动。',
    params: S({}),
  },
  {
    name: 'ue_save', method: 'editor.save', risk: 'mutating',
    desc: '保存（默认只存 agent 自己改脏的包）。',
    params: S({ scope: { type: 'string', description: 'touched/current/all，省略 touched' } }),
  },
  {
    name: 'ue_undo', method: 'editor.undo', risk: 'destructive',
    desc: '撤销 agent 自己做过的步骤（不影响用户手动操作）。',
    params: S({ steps: { type: 'number', description: '撤几步，省略撤全部' } }),
  },
  {
    name: 'ue_collect_garbage', method: 'editor.collect_garbage', risk: 'mutating',
    desc: '触发引擎垃圾回收。',
    params: S({}),
  },

  // ── Level ──────────────────────────────────────────────────
  {
    name: 'ue_get_current_level', method: 'level.get_current', risk: 'safe',
    desc: '读当前关卡路径/名字。',
    params: S({}),
  },
  {
    name: 'ue_save_level', method: 'level.save', risk: 'mutating',
    desc: '保存当前关卡。当前是临时关卡（从未保存过，报 no file yet）时，必须传 path 指定落盘路径（如 /Game/Maps/RacingTrack）。',
    params: S({ path: { type: 'string', description: '保存路径（如 /Game/Maps/RacingTrack）。临时关卡首次保存必传' } }),
  },
  {
    name: 'ue_open_level', method: 'level.open', risk: 'destructive',
    desc: '打开关卡（丢弃未保存改动，不可撤销）。',
    params: S({ level_path: { type: 'string', description: '关卡路径，如 /Game/Maps/Main' } }, ['level_path']),
  },
  {
    name: 'ue_new_level', method: 'level.new', risk: 'destructive',
    desc: '新建关卡。',
    params: S({ level_path: { type: 'string', description: '新关卡路径' }, template: { type: 'string' } }, ['level_path']),
  },
  {
    name: 'ue_get_levels', method: 'level.list', risk: 'safe',
    desc: '列出工程所有关卡。',
    params: S({}),
  },
  {
    name: 'ue_set_level_streaming', method: 'level.set_streaming', risk: 'destructive',
    desc: '改关卡的流送方式。',
    params: S({ level_path: { type: 'string' }, streaming_method: { type: 'string' } }, ['level_path']),
  },
  {
    name: 'ue_find_heavy_assets', method: 'level.query_assets', risk: 'safe',
    desc: '查关卡里资源占用（重资产/面数）。',
    params: S({ level_path: { type: 'string', description: '关卡路径，省略当前' } }),
  },
  {
    name: 'level_organize_actors', method: 'level.organize_actors', risk: 'mutating',
    desc: '把关卡 Actor 按类型归进大纲文件夹。',
    params: S({}),
  },

  // ── System ─────────────────────────────────────────────────
  {
    name: 'ue_get_performance_stats', method: 'system.get_performance_stats', risk: 'safe',
    desc: '读当前帧性能快照（FPS/线程耗时）。',
    params: S({}),
  },
  {
    name: 'ue_editor_health', method: 'system.get_editor_health', risk: 'safe',
    desc: '读编辑器健康状况：启动耗时、卡顿率、内存。',
    params: S({}),
  },
  {
    name: 'ue_capture_perf_trace', method: 'system.capture_perf_trace', risk: 'safe',
    desc: '录一段性能采样（CSV Profiler），返回 csv 路径和统计。',
    params: S({ duration_seconds: { type: 'number', description: '采样时长（1-120，默认 10）' }, gpu_stats: { type: 'boolean' } }),
  },
  {
    name: 'ue_run_console_command', method: 'system.run_console_command', risk: 'destructive',
    desc: '执行引擎控制台命令（如 stat unit / r.ScreenPercentage）。',
    params: S({ command: { type: 'string', description: '控制台命令' } }, ['command']),
  },
  {
    name: 'ue_run_python_script', method: 'cmd.run_python', risk: 'destructive',
    desc: '在编辑器里执行 Python 脚本。',
    params: S({ script: { type: 'string', description: 'Python 脚本内容' } }, ['script']),
  },
  {
    name: 'ue_manage_plugin', method: 'system.manage_plugin', risk: 'destructive',
    desc: '启用/禁用/查询插件。',
    params: S({ plugin_name: { type: 'string' }, action: { type: 'string', description: 'enable/disable/query' } }, ['plugin_name']),
  },

  // ── Content Browser ────────────────────────────────────────
  {
    name: 'ue_content_search', method: 'content.search', risk: 'safe',
    desc: '搜索工程资产（按名/类型/路径）。',
    params: S({ query: { type: 'string', description: '关键字' }, type: { type: 'string', description: '资产类型过滤' } }),
  },
  {
    name: 'ue_content_describe', method: 'content.describe', risk: 'safe',
    desc: '读单个资产详情（类型/大小/引用）。',
    params: S(ASSET_PATH, ['asset_path']),
  },
  {
    name: 'ue_message_log', method: 'messagelog.get', risk: 'safe',
    desc: '读引擎消息日志（编译/加载错误）。',
    params: S({}),
  },
  {
    name: 'ue_content_import', method: 'content.import', risk: 'mutating',
    desc: '导入资产到工程。',
    params: S({ source_path: { type: 'string', description: '源文件路径' }, destination_path: { type: 'string', description: '目标 Content 路径' } }, ['source_path']),
  },
  {
    name: 'ue_content_delete', method: 'content.delete', risk: 'destructive',
    desc: '删除资产（不可逆）。',
    params: S(ASSET_PATH, ['asset_path']),
  },

  // ── C++ ────────────────────────────────────────────────────
  {
    name: 'cpp_compile', method: 'cpp.compile', risk: 'mutating',
    desc: '编译工程 C++ 代码。',
    params: S({}),
  },
  {
    name: 'cpp_list_modules', method: 'cpp.list_modules', risk: 'safe',
    desc: '列出工程 C++ 模块。',
    params: S({}),
  },
  {
    name: 'cpp_probe', method: 'cpp.probe', risk: 'safe',
    desc: '探测编译能力/环境。',
    params: S({}),
  },

  // ── PIE / Input ────────────────────────────────────────────
  {
    name: 'ue_playtest', method: 'pie.run', risk: 'mutating',
    desc: '启动 PIE 试玩（运行游戏逻辑）。',
    params: S({}),
  },
  {
    name: 'ue_input_map', method: 'input.map', risk: 'safe',
    desc: '查询输入映射（模拟玩家操作前先调）。',
    params: S({}),
  },
  {
    name: 'ue_inject_input', method: 'input.inject_key', risk: 'mutating',
    desc: '在运行中的游戏里模拟按键/操作。',
    params: S({ key: { type: 'string', description: '按键，如 W / SpaceBar' }, action: { type: 'string', description: '动作名' } }),
  },

  // ── Widget (UMG) ───────────────────────────────────────────
  {
    name: 'widget_get_hierarchy', method: 'widget.get_hierarchy', risk: 'safe',
    desc: '读控件蓝图层级。',
    params: S(BLUEPRINT_PATH, ['blueprint_path']),
  },
  {
    name: 'widget_create', method: 'widget.create', risk: 'mutating',
    desc: '创建控件蓝图（UMG）。',
    params: S({ blueprint_path: { type: 'string' }, parent: { type: 'string', description: '父控件，如 UserWidget' } }, ['blueprint_path']),
  },
  {
    name: 'widget_add_child', method: 'widget.add_child', risk: 'mutating',
    desc: '给控件加子控件（如 Button/TextBlock）。',
    params: S({
      ...BLUEPRINT_PATH,
      parent_widget: { type: 'string', description: '父控件名' },
      widget_type: { type: 'string', description: '子控件类型，如 Button / TextBlock' },
      widget_name: { type: 'string', description: '子控件名' },
    }, ['blueprint_path', 'widget_type']),
  },
  {
    name: 'widget_set_property', method: 'widget.set_property', risk: 'mutating',
    desc: '改控件属性（文本/颜色/尺寸）。',
    params: S({ ...BLUEPRINT_PATH, widget_name: { type: 'string' }, property_name: { type: 'string' }, value: { type: 'string' } }, ['blueprint_path', 'widget_name', 'property_name', 'value']),
  },

  // ══════════════════════════════════════════════════════════
  // 以下为 UEBox 拆解第二批补齐（2026-10-07）：
  // PCG / Sequencer / Landscape / Animation / Mesh / Inspect / 内容批量
  // ══════════════════════════════════════════════════════════

  // ── PCG（程序化内容生成）─────────────────────────────────
  {
    name: 'pcg_status', method: 'pcg.status', risk: 'safe',
    desc: '查工程能否用 PCG 及插件版本。任何 pcg_* 工具报 501 时先调它判断是引擎太老还是插件没启用。',
    params: S({}),
  },
  {
    name: 'pcg_get_graph', method: 'pcg.get_graph', risk: 'safe',
    desc: '读一张 PCG 图的完整结构（所有节点、类型、位置、引脚、连线）。改图前先调它拿 node_id（引擎生成的，猜不出来）。',
    params: S({ graph_path: { type: 'string', description: 'PCG 图资产路径，如 /Game/PCG/PCG_Forest' } }, ['graph_path']),
  },
  {
    name: 'pcg_list_node_types', method: 'pcg.list_node_types', risk: 'safe',
    desc: '列出工程里可用的 PCG 节点类型。加节点前先用它确认类型名存在，别凭记忆写。',
    params: S({ search: { type: 'string', description: '按类型名模糊筛选，如 Sampler / Spawner / Filter' }, limit: { type: 'number', description: '最多返回几条，默认 120' } }),
  },
  {
    name: 'pcg_get_node_schema', method: 'pcg.get_node_schema', risk: 'safe',
    desc: '查一个 PCG 节点类型的引脚、可改属性和默认值。改属性前先调它。',
    params: S({ node_type: { type: 'string', description: '节点类型名，如 PCGSurfaceSamplerSettings，也认省略写法 SurfaceSampler' } }, ['node_type']),
  },
  {
    name: 'pcg_create_graph', method: 'pcg.create_graph', risk: 'mutating',
    desc: '新建一张 PCG 图资产。',
    params: S({ name: { type: 'string', description: '图名字，如 PCG_Forest' }, path: { type: 'string', description: '存放目录，默认 /Game/PCG' } }, ['name']),
  },
  {
    name: 'pcg_add_node', method: 'pcg.add_node', risk: 'mutating',
    desc: '给 PCG 图加一个节点。批量搭图优先用 pcg_apply_graph（一次写整图）。',
    params: S({
      graph_path: { type: 'string' }, node_type: { type: 'string', description: '节点类型名，如 PCGSurfaceSamplerSettings' },
      x: { type: 'number', description: '图内 X 坐标' }, y: { type: 'number', description: '图内 Y 坐标' },
      title: { type: 'string' }, properties: { type: 'object', description: '要设置的属性，键名来自 pcg_get_node_schema' },
    }, ['graph_path', 'node_type']),
  },
  {
    name: 'pcg_update_node', method: 'pcg.update_node', risk: 'mutating',
    desc: '改 PCG 节点的属性 / 标题 / 注释。',
    params: S({
      graph_path: { type: 'string' }, node_id: { type: 'string', description: '节点 id，来自 pcg_get_graph' },
      title: { type: 'string' }, comment: { type: 'string' }, properties: { type: 'object', description: '要改的属性' },
    }, ['graph_path', 'node_id']),
  },
  {
    name: 'pcg_remove_node', method: 'pcg.remove_node', risk: 'mutating',
    desc: '删除 PCG 图里的一个节点。',
    params: S({ graph_path: { type: 'string' }, node_id: { type: 'string', description: '要删的节点 id，来自 pcg_get_graph' } }, ['graph_path', 'node_id']),
  },
  {
    name: 'pcg_connect_pins', method: 'pcg.connect_pins', risk: 'mutating',
    desc: '连接 PCG 图里两个节点的引脚。',
    params: S({
      graph_path: { type: 'string' },
      from_node: { type: 'string', description: '上游节点 id，或 Input 表示图输入端点' },
      from_pin: { type: 'string', description: '上游输出引脚名，不填自动选唯一那个' },
      to_node: { type: 'string', description: '下游节点 id，或 Output 表示图输出端点' },
      to_pin: { type: 'string', description: '下游输入引脚名，不填自动选唯一那个' },
    }, ['graph_path', 'from_node', 'to_node']),
  },
  {
    name: 'pcg_disconnect_pins', method: 'pcg.disconnect_pins', risk: 'mutating',
    desc: '断开 PCG 图里两个节点之间的连线。',
    params: S({
      graph_path: { type: 'string' },
      from_node: { type: 'string' }, from_pin: { type: 'string' },
      to_node: { type: 'string' }, to_pin: { type: 'string' },
    }, ['graph_path', 'from_node', 'to_node']),
  },
  {
    name: 'pcg_apply_graph', method: 'pcg.apply_graph', risk: 'mutating',
    desc: '搭 PCG 图的首选工具：一次调用写完节点、连线和属性。图不存在会自动建；默认 replace 重建，往已有图补东西用 mode=merge；可重复执行（按 id 别名复用）。',
    params: S({
      graph_path: { type: 'string' },
      mode: { type: 'string', enum: ['replace', 'merge'], description: 'replace 先清空重建（默认），merge 只新增/更新声明节点' },
      nodes: { type: 'array', description: '要创建或更新的节点', items: { type: 'object', properties: { id: { type: 'string', description: '别名，连线时用它引用' }, type: { type: 'string', description: '节点类型名' }, x: { type: 'number' }, y: { type: 'number' }, properties: { type: 'object', description: '属性，键名支持 A.B[0].C 路径' } }, required: ['id', 'type'] } },
      edges: { type: 'array', description: '要建立的连线', items: { type: 'object', properties: { from: { type: 'string', description: '上游别名或 Input' }, from_pin: { type: 'string' }, to: { type: 'string', description: '下游别名或 Output' }, to_pin: { type: 'string' } }, required: ['from', 'to'] } },
    }, ['graph_path']),
  },
  {
    name: 'pcg_diagnose', method: 'pcg.diagnose', risk: 'safe',
    desc: '查一张 PCG 图为什么生成不出东西（pcg_execute 返回 0 实例时调它）：Output 没连、采样器没表面、孤立节点、生成器没网格、World Partition 未加载等。',
    params: S({ graph_path: { type: 'string' } }, ['graph_path']),
  },
  {
    name: 'pcg_scene_report', method: 'pcg.scene_report', risk: 'safe',
    desc: '一次问清当前关卡 PCG 能采到什么：地形、PCG 体积、World Partition、未保存关卡等。',
    params: S({}),
  },
  {
    name: 'pcg_describe_node', method: 'pcg.describe_node', risk: 'safe',
    desc: '完整读一个 PCG 节点的属性（含嵌套结构体、数组、子对象）。改已有图前先调它，读回来的键就是 update/apply 认的路径写法。',
    params: S({ graph_path: { type: 'string' }, node_id: { type: 'string', description: '节点 id，来自 pcg_get_graph' }, limit: { type: 'number', description: '最多返回多少条属性，默认 200' } }, ['graph_path', 'node_id']),
  },
  {
    name: 'pcg_graph_parameters', method: 'pcg.graph_parameters', risk: 'mutating',
    desc: '读/建/改/删 PCG 图的用户参数（细节面板上直接调的那几个值）。只给 graph_path 是纯读。',
    params: S({
      graph_path: { type: 'string' },
      parameters: { type: 'array', description: '要新建或改值的参数，不给就是纯读', items: { type: 'object', properties: { name: { type: 'string' }, type: { type: 'string', description: 'bool/int32/float/double/string/object 等' }, value: { type: 'string', description: '参数值' } }, required: ['name'] } },
      remove: { type: 'array', items: { type: 'string' }, description: '要删掉的参数名' },
    }, ['graph_path']),
  },
  {
    name: 'pcg_spawn_volume', method: 'pcg.spawn_volume', risk: 'mutating',
    desc: '在当前关卡放一个 PCG 体积并挂图。图只是资产，不放体积不会生成任何东西。放完用 pcg_execute 跑。',
    params: S({
      graph_path: { type: 'string', description: '要挂的 PCG 图，不填放空体积' },
      location: { type: 'object', description: '体积中心世界坐标', properties: { x: { type: 'number' }, y: { type: 'number' }, z: { type: 'number' } } },
      size: { type: 'object', description: '体积尺寸（厘米），z 要够高穿过地形起伏', properties: { x: { type: 'number' }, y: { type: 'number' }, z: { type: 'number' } } },
      label: { type: 'string', description: '体积在大纲里的名字' },
    }),
  },
  {
    name: 'pcg_execute', method: 'pcg.execute', risk: 'mutating',
    desc: '让关卡里一个 PCG 体积重新跑一遍生成，回报实际生成了多少实例（instance_count）。这是验证图对不对的唯一办法。',
    params: S({ actor_label: { type: 'string', description: 'PCG 体积在大纲里的名字，来自 pcg_spawn_volume' }, timeout_seconds: { type: 'number', description: '最多等多少秒确认完成，默认 30' }, verbose: { type: 'boolean', description: '转储每个引脚收发数据量，查产出 0 时开' } }, ['actor_label']),
  },
  {
    name: 'pcg_tidy_graph', method: 'pcg.set_node_positions', risk: 'mutating',
    desc: '重排 PCG 图节点位置（底层排版命令）。传 positions 数组 [{node_id,x,y}]，先 pcg_get_graph 拿 node_id 和当前坐标。只挪位置不改逻辑。',
    params: S({
      graph_path: { type: 'string' },
      positions: { type: 'array', description: '每个节点的目标位置', items: { type: 'object', properties: { node_id: { type: 'string' }, x: { type: 'number' }, y: { type: 'number' } }, required: ['node_id', 'x', 'y'] } },
    }, ['graph_path', 'positions']),
  },

  // ── Sequencer（镜头与成片）────────────────────────────────
  {
    name: 'sequence_describe', method: 'sequence.describe', risk: 'safe',
    desc: '读一条 Level Sequence 的结构（绑定/轨道/段/关键帧）+ 出片体检（有没有相机切轨、绑定失效）。改序列前先调它。',
    params: S({
      sequence_path: { type: 'string', description: 'Level Sequence 资产路径，如 /Game/Cinematics/Shot_01.Shot_01' },
      detail: { type: 'string', enum: ['names', 'outline', 'tracks', 'keys'], description: 'names=只列绑定名；outline=加轨道数（默认）；tracks=加段与时间；keys=加关键帧（须配 bindings）' },
      bindings: { type: 'array', items: { type: 'string' }, description: '只看这几个绑定（按名字），detail=keys 时必填' },
    }, ['sequence_path']),
  },
  {
    name: 'sequence_audit', method: 'sequence.audit', risk: 'safe',
    desc: '出片前体检：一次检查 N 条 Level Sequence，只报问题不报结构，给 PASS/FAIL。',
    params: S({
      sequence_paths: { type: 'array', items: { type: 'string' }, description: '要检查的序列路径列表' },
      recursive: { type: 'boolean', description: '是否递归检查子序列，默认 true' },
      only_blockers: { type: 'boolean', description: '只报会导致渲染失败的问题' },
    }, ['sequence_paths']),
  },
  {
    name: 'sequence_diff', method: 'sequence.diff', risk: 'safe',
    desc: '对比两条 Level Sequence 的绑定差异（基准 vs 对比），用于核对受保护资产改没改、风格迁移覆盖全没全。',
    params: S({
      base_path: { type: 'string', description: '基准序列路径' },
      compare_path: { type: 'string', description: '对比序列路径' },
      binding_map: { type: 'array', description: '绑定名映射', items: { type: 'object', properties: { base: { type: 'string' }, compare: { type: 'string' } }, required: ['base', 'compare'] } },
      bindings: { type: 'array', items: { type: 'string' }, description: '只看这几个绑定' },
    }, ['base_path', 'compare_path']),
  },
  {
    name: 'sequence_camera_keys', method: 'sequence.camera_keys', risk: 'mutating',
    desc: '写相机关键帧（任意运镜都走这里）。keys 按 frame 升序至少两个。',
    params: S({
      sequence_path: { type: 'string', description: '目标序列，不存在就新建' },
      camera_label: { type: 'string', description: '相机名' },
      keys: { type: 'array', description: '关键帧列表，按 frame 升序至少两个', items: { type: 'object', properties: { frame: { type: 'number' }, location: { type: 'object', properties: { x: { type: 'number' }, y: { type: 'number' }, z: { type: 'number' } } }, rotation: { type: 'object', properties: { roll: { type: 'number' }, pitch: { type: 'number' }, yaw: { type: 'number' } } }, fov: { type: 'number' } }, required: ['frame', 'location', 'rotation'] } },
      fps: { type: 'number', description: '序列帧率，新建序列时生效，默认 30' },
      playback_end_frame: { type: 'number' },
      interpolation: { type: 'string', enum: ['linear', 'cubic', 'constant'], description: 'linear 匀速直线 / cubic 缓入缓出 / constant 跳变' },
      focal_length_mm: { type: 'number', description: '相机焦距毫米，只在新建相机时设一次' },
      replace_existing_keys: { type: 'boolean' },
      camera_cuts: { type: 'boolean', description: '顺便建好相机切轨并盖满播放范围' },
      rebuild_camera_cuts: { type: 'boolean' },
    }, ['sequence_path', 'camera_label', 'keys']),
  },
  {
    name: 'sequence_camera_cuts', method: 'sequence.camera_cuts', risk: 'mutating',
    desc: '给已有序列补相机切轨（Camera Cuts）。',
    params: S({
      sequence_path: { type: 'string', description: '要处理的序列路径' },
      camera_label: { type: 'string', description: '相机绑定名' },
      rebuild: { type: 'boolean', description: '是否重建切轨' },
    }, ['sequence_path']),
  },

  // ── Landscape（地形）──────────────────────────────────────
  {
    name: 'landscape_create', method: 'landscape.create', risk: 'mutating',
    desc: '新建地形（平地给 size_x_m，或按高度图 heightmap_path 建）。',
    params: S({
      size_x_m: { type: 'number', description: 'X 方向边长（米），平地必填' },
      size_y_m: { type: 'number', description: 'Y 方向边长（米），省略 = 和 X 一样' },
      heightmap_path: { type: 'string', description: '高度图绝对路径（16 位灰度 PNG/.r16/.raw），给了按图建' },
      location: { type: 'object', description: '地形中心位置', properties: { x: { type: 'number' }, y: { type: 'number' }, z: { type: 'number' } } },
      quad_size_m: { type: 'number', description: '每格边长（米），默认 1' },
      height_range_m: { type: 'number', description: '高度图总落差（米），默认 512' },
      material: { type: 'string', description: '地形材质路径' },
      label: { type: 'string', description: '大纲里的名字' },
      wp_grid_size: { type: 'number', description: 'World Partition 每块流送代理包几个地形块，默认 2' },
    }),
  },
  {
    name: 'landscape_list', method: 'landscape.list', risk: 'safe',
    desc: '列出当前关卡里的地形。',
    params: S({}),
  },
  {
    name: 'landscape_setup_rvt', method: 'landscape.setup_rvt', risk: 'mutating',
    desc: '给地形配 RVT（运行时虚拟纹理）：color（颜色）或 height（高度），至少一个。',
    params: S({
      landscape: { type: 'string', description: '地形的大纲名字，关卡只有一块可省略' },
      color: { type: 'string', description: '颜色 RVT，true=按默认路径新建，或资产路径' },
      height: { type: 'string', description: '高度 RVT，true=按默认路径新建，或资产路径' },
      asset_folder: { type: 'string', description: '新建 RVT 资产放哪个目录，默认 /Game/Landscape/RVT' },
    }),
  },

  // ── Animation（动画）──────────────────────────────────────
  {
    name: 'anim_measure', method: 'anim.measure', risk: 'safe',
    desc: '测量动画姿势：帧率、帧数、缺失轨道、关节角度、骨世界坐标。describe=true 只读帧率/帧数/角色表。',
    params: S({
      path: { type: 'string', description: '动画资产路径，如 /Game/Anim/Talk_01' },
      describe: { type: 'boolean', description: '只读帧率/帧数/缺失轨道和角色表，默认 false' },
      metrics: { type: 'array', items: { type: 'string' }, description: '要量的指标' },
      frames: { type: 'array', items: { type: 'number' }, description: '要采样的帧，不传采全部' },
      bones: { type: 'array', items: { type: 'string' }, description: '要回世界坐标序列的骨名或角色名' },
      angle_bones: { type: 'array', items: { type: 'string' }, description: '两条骨向量的起终点，共四个骨名或角色名' },
      bone_map: { type: 'object', description: '骨骼名映射' },
    }, ['path']),
  },
  {
    name: 'anim_preview', method: 'anim.preview', risk: 'safe',
    desc: '预览动画在某一帧的姿势。',
    params: S({
      mesh: { type: 'string', description: '骨骼网格资产路径' },
      animation: { type: 'string', description: '动画资产路径' },
      time: { type: 'number', description: '时间（秒）' },
      camera: { type: 'string', enum: ['front', 'side', 'three_quarter'], description: '视角，默认 three_quarter' },
      bone_map: { type: 'object', description: '骨骼名映射' },
    }, ['mesh', 'animation', 'time']),
  },
  {
    name: 'anim_retarget', method: 'anim.retarget', risk: 'mutating',
    desc: '在骨架之间重定向动画（身体动画/测量姿势）。已有调好的 IK Retargeter 就直接用。',
    params: S({
      source_mesh: { type: 'string', description: '源骨架网格资产路径' },
      target_mesh: { type: 'string', description: '目标骨架网格资产路径' },
      retargeter: { type: 'string', description: '已有 IK Retargeter 资产路径，可选' },
      animations: { type: 'array', description: '要重定向的动画', items: { type: 'object', properties: { animation: { type: 'string' }, output_path: { type: 'string' } }, required: ['animation', 'output_path'] } },
    }, ['source_mesh', 'target_mesh', 'animations']),
  },
  {
    name: 'anim_write_pose', method: 'anim.write_pose', risk: 'mutating',
    desc: '把源动画某一帧的姿势写成一段固定姿势动画。',
    params: S({
      path: { type: 'string', description: '输出动画资产路径' },
      source: { type: 'string', description: '源动画资产路径' },
      source_time: { type: 'number', description: '取源动画哪一帧（秒）' },
      start_frame: { type: 'number' }, end_frame: { type: 'number' },
      transition_frames: { type: 'number', description: '过渡帧数，默认 0' },
    }, ['path', 'source', 'source_time', 'start_frame', 'end_frame']),
  },

  // ── Mesh（几何体）─────────────────────────────────────────
  {
    name: 'mesh_describe', method: 'mesh.describe', risk: 'safe',
    desc: '读静态网格资产详情（LOD、碰撞、材质槽、顶点数等）。',
    params: S({ path: { type: 'string', description: '网格资产路径，如 /Game/Meshes/SM_Rock' } }, ['path']),
  },

  // ── Inspect（细粒度只读）──────────────────────────────────
  {
    name: 'ue_inspect_components', method: 'actor.inspect_components', risk: 'safe',
    desc: '细粒度读 Actor 或蓝图的组件属性值（组件级回读）。blueprint_describe 给组件树不给值，这个给值。',
    params: S({
      targets: { type: 'object', description: '场景里的 Actor（PIE 在跑读游戏里那份），和 blueprint_path 二选一', properties: { selection: { type: 'boolean', description: 'true=当前选中的 Actor' }, names: { type: 'array', items: { type: 'string' } }, paths: { type: 'array', items: { type: 'string' } } } },
      blueprint_path: { type: 'string', description: '蓝图资产路径：读类默认值（CDO）和组件模板，和 targets 二选一' },
      components: { type: 'array', items: { type: 'string' }, description: '只看这些组件（组件名或类名）' },
      properties: { type: 'array', items: { type: 'string' }, description: '要读的组件属性名' },
      object_properties: { type: 'array', items: { type: 'string' }, description: 'Actor 本身/蓝图 CDO 上的属性，点路径' },
      include_render_state: { type: 'boolean', description: '是否包含渲染状态' },
    }),
  },
  {
    name: 'material_graph_slice', method: 'material.get_graph', risk: 'safe',
    desc: '只读材质图里的一段：从主节点引脚（如 OpacityMask）或某节点往上游走 N 层，或直接点名节点。要看全图用 material_get_graph。',
    params: S({
      path: { type: 'string', description: '材质资产路径（母材质，不是实例）' },
      from: { type: 'array', items: { type: 'string' }, description: '从这里往上游走：主节点引脚名或 node_id/guid' },
      depth: { type: 'number', description: '往上游走几层，默认 3' },
      node_ids: { type: 'array', items: { type: 'string' }, description: '或直接点名要看的节点' },
    }, ['path']),
  },

  // ── Content（内容批量整理）────────────────────────────────
  {
    name: 'ue_content_dependencies', method: 'content.dependencies', risk: 'safe',
    desc: '查一个资产或整个目录的依赖闭包 / 被引用闭包。只读注册表不加载资产，大工程也秒级。',
    params: S({
      path: { type: 'string', description: '资产或目录路径' },
      direction: { type: 'string', enum: ['dependencies', 'referencers', 'both', 'unreferenced'], description: 'dependencies=它用了谁；referencers=谁用它；both=双向；unreferenced=无引用者' },
      recursive: { type: 'boolean', description: '沿依赖链走到底，默认 true' },
      max_depth: { type: 'number', description: '最多走几层，默认 8' },
      hard_only: { type: 'boolean', description: '只算硬引用，默认 false' },
      include_engine: { type: 'boolean', description: '把 /Engine 算进去，默认 false' },
      max_nodes: { type: 'number', description: '闭包最多收集多少个包，默认 2000' },
      limit: { type: 'number', description: '最多列多少条，默认 200' },
      include_edges: { type: 'boolean', description: '是否返回逐条引用边' },
    }, ['path']),
  },
  {
    name: 'ue_content_move', method: 'local.content_move', risk: 'mutating',
    desc: '批量移动/重命名资产或整目录。第一次先 dry_run=true 看计划。成功后自动记账，可用 ue_content_rollback 回滚。',
    params: S({
      moves: { type: 'array', description: '逐资产移动/改名列表', items: { type: 'object', properties: { source: { type: 'string', description: '当前路径' }, destination: { type: 'string', description: '目标路径（/ 结尾=保留原名搬进去）' } }, required: ['source', 'destination'] } },
      folder_moves: { type: 'array', description: '整目录搬迁', items: { type: 'object', properties: { source_folder: { type: 'string' }, destination_folder: { type: 'string' } }, required: ['source_folder', 'destination_folder'] } },
      dry_run: { type: 'boolean', description: '只做计划不动手，第一次先用它' },
      on_conflict: { type: 'string', enum: ['fail', 'skip', 'auto_rename'], description: '目标已存在时，默认 fail' },
      on_blocked: { type: 'string', enum: ['fail', 'proceed'], description: '签出预检有文件动不了时，默认 fail' },
      on_cdo_refs: { type: 'string', enum: ['fail', 'proceed'] },
      on_external_refs: { type: 'string', enum: ['fail', 'proceed'] },
      save: { type: 'boolean', description: '搬完落盘，默认 true' },
      batch_size: { type: 'number', description: '每次交给引擎多少条，默认 100' },
    }),
  },
  {
    name: 'ue_content_migrate', method: 'content.migrate', risk: 'mutating',
    desc: '把资产（连同依赖闭包）迁移到另一个虚幻工程，等价于编辑器右键 Migrate。先 dry_run=true 看会拷多少。',
    params: S({
      paths: { type: 'array', items: { type: 'string' }, description: '要迁的资产或目录列表' },
      destination: { type: 'string', description: '目标工程 .uproject 绝对路径或工程目录/Content 目录' },
      include_dependencies: { type: 'boolean', description: '连依赖闭包一起迁，默认 true' },
      on_conflict: { type: 'string', enum: ['skip', 'overwrite'], description: '默认 skip' },
      dry_run: { type: 'boolean', description: '只列出会拷哪些，不动手' },
      save_first: { type: 'boolean', description: '源资产有未保存改动先保存，默认 true' },
    }, ['paths', 'destination']),
  },
  {
    name: 'ue_content_naming_audit', method: 'content.naming_audit', risk: 'safe',
    desc: '按命名规范审计资产并出改名建议（含前缀规则）。',
    params: S({
      path: { type: 'string', description: '要体检的目录，默认 /Game' },
      recursive: { type: 'boolean', description: '是否含子目录，默认 true' },
      class_filter: { type: 'string', description: '只查某一类，如 StaticMesh / Texture2D' },
      rules: { type: 'object', description: '自定义前缀规则 { "Texture2D": "T_" }' },
      ignore_classes: { type: 'array', items: { type: 'string' }, description: '跳过这些类' },
      ignore_paths: { type: 'array', items: { type: 'string' }, description: '跳过这些目录' },
      pascal_case: { type: 'boolean', description: '建议名主体首字母大写，默认 false' },
      include_compliant: { type: 'boolean', description: '也列合规资产，默认 false' },
      limit: { type: 'number', description: '最多列多少条，默认 200' },
    }),
  },

  // ══════════════════════════════════════════════════════════
  // 第三批：散落补齐（material 扩展 / content / editor / system / widget / cpp）
  // ══════════════════════════════════════════════════════════

  // ── Material 扩展 ─────────────────────────────────────────
  {
    name: 'material_apply_graph', method: 'material.apply_graph', risk: 'mutating',
    desc: '写材质节点图（唯一写图入口，一次写完节点+连线+自动排版+编译）。nodes 建节点（id 是局部别名，连线用它指代），connections 连线（"节点id.引脚名"，主节点写 "Material.BaseColor"）。node_type 如 Constant3Vector/TextureSample/Lerp/ScalarParameter。',
    params: S({
      path: { type: 'string', description: '材质资产路径，如 /Game/Materials/M_Wood' },
      nodes: { type: 'array', description: '要新建的节点', items: { type: 'object', properties: { id: { type: 'string', description: '局部 id，只在本次调用有效，连线用它指代' }, node_type: { type: 'string', description: '节点类型，如 Constant3Vector / TextureSample / Lerp' }, node_name: { type: 'string', description: '参数节点的参数名（ScalarParameter 等必填）' }, value: { type: 'string', description: '常量/参数节点初始值' }, texture_path: { type: 'string' }, function_path: { type: 'string' }, collection_path: { type: 'string' }, group_name: { type: 'string' }, properties: { type: 'object', description: '节点细节面板设置' } }, required: ['id', 'node_type'] } },
      connections: { type: 'array', description: '连线', items: { type: 'object', properties: { from: { type: 'string', description: '源，如 "tex.RGB"（节点id.引脚名）' }, to: { type: 'string', description: '目标，主节点写 "Material.BaseColor"' } }, required: ['from', 'to'] } },
      tidy: { type: 'boolean', description: '完事自动排版，默认 true' },
      compile: { type: 'boolean', description: '完事自动编译，默认 true' },
    }, ['path']),
  },
  {
    name: 'material_duplicate', method: 'material.duplicate', risk: 'mutating',
    desc: '复制一个材质 / 材质实例。',
    params: S({
      path: { type: 'string', description: '要复制的材质路径' },
      new_name: { type: 'string', description: '新资产名，默认 <原名>_Copy' },
      destination_path: { type: 'string', description: '保存路径，默认与原材质同目录' },
    }, ['path']),
  },
  {
    name: 'material_set_node_value', method: 'material.set_node_value', risk: 'mutating',
    desc: '改材质图里某个已有节点的值（Constant/颜色/向量/贴图采样/纹理坐标/ComponentMask 等）。纯运算节点没值可设。',
    params: S({
      path: { type: 'string', description: '材质路径' },
      node_id: { type: 'string', description: '节点 ID，用 material_get_graph 读' },
      value: { type: 'string', description: '要设的值：数值/贴图路径/{r,g,b,a}/[r,g,b]/{u_tiling,v_tiling}' },
      properties: { type: 'object', description: '节点设置，如 { SubSurfaceType: "MSS_SimpleVolume" }' },
    }, ['path', 'node_id']),
  },
  {
    name: 'material_delete_node', method: 'material.delete_node', risk: 'destructive',
    desc: '从材质图删除一个节点（连到它的线一并断开）。删完用 material_compile 确认。',
    params: S({ path: { type: 'string' }, node_id: { type: 'string', description: '要删的节点 ID' } }, ['path', 'node_id']),
  },
  {
    name: 'material_tidy_graph', method: 'material.set_node_positions', risk: 'mutating',
    desc: '重排材质图节点位置（底层排版命令）。传 positions 数组 [{node_id,x,y}]，先 material_get_graph 拿 node_id 和坐标。只挪位置不改逻辑。',
    params: S({
      material_path: { type: 'string', description: '材质资产路径' },
      positions: { type: 'array', description: '每个节点的目标位置', items: { type: 'object', properties: { node_id: { type: 'string' }, x: { type: 'number' }, y: { type: 'number' } }, required: ['node_id', 'x', 'y'] } },
    }, ['material_path', 'positions']),
  },
  {
    name: 'material_disconnect_pins', method: 'material.disconnect_pins', risk: 'mutating',
    desc: '断开材质图某个节点的输入连线。断完要 material_compile 才生效。',
    params: S({
      path: { type: 'string' },
      target_node: { type: 'string', description: '要断开输入的那个节点，主节点填 Material，其余填 node_id' },
      target_pin: { type: 'string', description: '输入引脚名，如 BaseColor / Roughness / A / B，省略断第一个输入' },
    }, ['path', 'target_node']),
  },
  {
    name: 'material_delete_unused_nodes', method: 'material.delete_unused_nodes', risk: 'destructive',
    desc: '找出并删除对最终输出没有贡献的材质节点。默认 dry_run=true 只报不删。',
    params: S({
      path: { type: 'string' },
      dry_run: { type: 'boolean', description: 'true（默认）只列出；确认名单后传 false 才真的删' },
    }, ['path']),
  },
  {
    name: 'material_parameter_collection', method: 'material.parameter_collection', risk: 'mutating',
    desc: '材质参数集合（MPC）：建/查/改。「一个开关控制全场景材质」就用它（昼夜、季节、队伍配色、受击闪白）。',
    params: S({
      action: { type: 'string', enum: ['create', 'list', 'set'], description: 'create 新建 / list 只读 / set 改参数' },
      collection_path: { type: 'string', description: 'MPC 资产路径，list/set 时必填' },
      collection_name: { type: 'string', description: '新建时的资产名，习惯 MPC_ 开头' },
      destination_path: { type: 'string', description: '新建保存路径，默认 /Game/Materials' },
      scalars: { type: 'array', description: '标量参数', items: { type: 'object', properties: { name: { type: 'string' }, value: { type: 'number' } }, required: ['name', 'value'] } },
      vectors: { type: 'array', description: '向量参数', items: { type: 'object', properties: { name: { type: 'string' }, r: { type: 'number' }, g: { type: 'number' }, b: { type: 'number' }, a: { type: 'number' } }, required: ['name'] } },
    }, ['action']),
  },
  {
    name: 'material_create_function', method: 'material.create_function', risk: 'mutating',
    desc: '创建材质函数（MaterialFunction）。注意：图表命令只认 UMaterial，建好后要手工或用别的方式填内容。',
    params: S({
      function_name: { type: 'string', description: '函数名，习惯 MF_ 开头' },
      destination_path: { type: 'string', description: '保存路径，默认 /Game/Materials' },
      description: { type: 'string' },
    }, ['function_name']),
  },
  {
    name: 'material_get_referencers', method: 'material.get_referencers', risk: 'safe',
    desc: '查一个资产被哪些资产引用（材质/贴图/函数/参数集合都能查）。改母材质或删资产前先问一句。',
    params: S({ path: { type: 'string', description: '要查的资产路径' }, limit: { type: 'number', description: '最多回几条，默认 50' } }, ['path']),
  },

  // ── Content 补充 ──────────────────────────────────────────
  {
    name: 'ue_content_audit_optimization', method: 'content.audit_optimization', risk: 'safe',
    desc: '资产优化审计：Nanite 使用 / Lumen 材质 / 纹理大小，出建议。',
    params: S({ check_type: { type: 'string', enum: ['NaniteUsage', 'LumenMaterials', 'TextureSize', 'All'], description: '检查类型，默认 All' } }),
  },
  {
    name: 'ue_project_asset_ranking', method: 'content.asset_ranking', risk: 'safe',
    desc: '全工程资产排行：哪些最占地方、三角面最多、贴图最大。',
    params: S({
      path: { type: 'string', description: '限定目录，默认 /Game' },
      class_filter: { type: 'string', description: '只看某一类，如 StaticMesh / Texture2D' },
      sort_by: { type: 'string', enum: ['DiskSize', 'Triangles', 'Vertices', 'TextureMemory'], description: '排序依据，默认 DiskSize' },
      limit: { type: 'number', description: '返回多少条，默认 30' },
    }),
  },
  {
    name: 'ue_asset_size_map', method: 'content.size_map', risk: 'safe',
    desc: '查一个资产连同依赖一共多大（依赖体积图）。',
    params: S({ path: { type: 'string', description: '资产路径' } }, ['path']),
  },
  {
    name: 'ue_fixup_redirectors', method: 'content.fixup_redirectors', risk: 'destructive',
    desc: '清理重定向器（资产改名/搬迁后残留的指向）。',
    params: S({ path: { type: 'string', description: '搜索根路径，默认 /Game' } }),
  },

  // ── Editor 补充 ───────────────────────────────────────────
  {
    name: 'ue_get_config', method: 'project.get_config', risk: 'safe',
    desc: '读引擎/项目配置文件里的一项。',
    params: S({
      config_name: { type: 'string', enum: ['Engine', 'Game', 'Editor', 'EditorPerProjectUserSettings'], description: '配置文件名称' },
      section: { type: 'string', description: '配置节，如 /Script/Engine.RendererSettings' },
      key: { type: 'string', description: '键名，如 r.DynamicGlobalIlluminationMethod' },
    }, ['config_name', 'section', 'key']),
  },
  {
    name: 'ue_set_config', method: 'project.set_config', risk: 'mutating',
    desc: '写引擎/项目配置文件里的一项。',
    params: S({
      config_name: { type: 'string', enum: ['Engine', 'Game', 'Editor', 'EditorPerProjectUserSettings'] },
      section: { type: 'string', description: '配置节' },
      key: { type: 'string', description: '键名' },
      value: { type: 'string', description: '值，统一传字符串（"True"/"False"/"1" 等）' },
    }, ['config_name', 'section', 'key', 'value']),
  },
  {
    name: 'ue_undo_history', method: 'editor.undo_history', risk: 'safe',
    desc: '读编辑器撤销历史（都改了什么）。',
    params: S({ limit: { type: 'number', description: '只要最近几步，默认 20' } }),
  },
  {
    name: 'ue_autoplay', method: 'pie.run', risk: 'mutating',
    desc: '跑一段 PIE 并自动走到目标附近 / 等 PrintString 出现，回报运行结果。验证「跑起来不炸」用它。',
    params: S({
      reach_actor: { type: 'string', description: '走到这个 Actor 附近（大纲里的名字）' },
      reach_location: { type: 'object', description: '或走到这个坐标附近', properties: { x: { type: 'number' }, y: { type: 'number' }, z: { type: 'number' } } },
      reach_radius: { type: 'number', description: '多近算到了，默认 150' },
      reach_text: { type: 'string', description: '等到出现这段 PrintString 才算达成' },
      max_duration: { type: 'number', description: '最多跑多久（秒），默认 30 上限 60' },
      step_frames: { type: 'number', description: '每一步按住多少帧，默认 12' },
      screenshot: { type: 'boolean', description: '结束时截玩家视角，默认 true' },
    }),
  },
  {
    name: 'ue_restart_editor', method: 'editor.restart', risk: 'destructive',
    desc: '重启编辑器（改模块依赖等场景）。',
    params: S({}),
  },

  // ── System 补充 ───────────────────────────────────────────
  {
    name: 'ue_get_crash_logs', method: 'system.get_latest_crash_info', risk: 'safe',
    desc: '读最近的编辑器崩溃日志。',
    params: S({ limit: { type: 'number', description: '最近几条崩溃记录，默认 1' } }),
  },
  {
    name: 'ue_insights_trace', method: 'system.capture_insights_trace', risk: 'safe',
    desc: '性能采集：录 Unreal Insights trace 并分析（capture/analyze/compare）。',
    params: S({
      action: { type: 'string', enum: ['capture', 'analyze', 'compare'], description: 'capture 录制 / analyze 分析 / compare 比较两份' },
      duration_seconds: { type: 'number', description: 'capture 录制时长（秒），默认 10' },
      channels: { type: 'string', description: 'capture 通道，默认 cpu,gpu,frame,bookmark' },
      utrace_path: { type: 'string', description: 'analyze/compare 的 .utrace 路径' },
      baseline_utrace_path: { type: 'string', description: 'compare 的基线 .utrace' },
      start_seconds: { type: 'number', description: '只看从这一秒开始' },
      end_seconds: { type: 'number', description: '只看到这一秒为止' },
    }, ['action']),
  },
  {
    name: 'ue_session_health', method: 'local.session_health', risk: 'safe',
    desc: '判断「编辑器连没连上」：工作台在监听吗 / 有几个编辑器连上 / 没连上时下一步是什么。引擎命令报未连接时先调它。',
    params: S({}),
  },
  {
    name: 'ue_zen_server', method: 'local.zen_server', risk: 'safe',
    desc: '查 ZenServer（编辑器启动带起的本地 HTTP 缓存服务）的缓存命中率与占盘情况。',
    params: S({ port: { type: 'number', description: 'ZenServer 端口，默认 8558' } }),
  },
  {
    name: 'ue_content_rollback', method: 'local.rollback', risk: 'mutating',
    desc: '回滚一次批量搬迁（ue_content_move 成功后自动记账，这里反向搬回）。传 ledger_id（move 返回的 _ledger_id），省略则列出可用账本。',
    params: S({ ledger_id: { type: 'string', description: '账本 id（ue_content_move 返回的 _ledger_id）' }, on_conflict: { type: 'string', enum: ['fail', 'skip', 'auto_rename'] } }),
  },
  {
    name: 'ue_project_path_refs', method: 'local.path_refs', risk: 'safe',
    desc: '扫描工程文本文件（.uproject/.ini/.cpp/.py/.csv 等）里对 /Game 路径的引用，用于迁移前检查哪些外部文件引用了要搬的资产。',
    params: S({ uproject: { type: 'string', description: '工程 .uproject 路径' }, roots: { type: 'array', items: { type: 'string' }, description: '要扫描的根目录，默认取 uproject 所在目录' } }),
  },

  // ── Widget 补充 ───────────────────────────────────────────
  {
    name: 'widget_preview', method: 'widget.preview', risk: 'safe',
    desc: '渲染一张 Widget 蓝图的预览图。',
    params: S({ path: { type: 'string', description: 'Widget Blueprint 路径' }, width: { type: 'number', description: '预览宽度，默认 1920' }, height: { type: 'number', description: '预览高度，默认 1080' } }, ['path']),
  },
  {
    name: 'widget_set_slot', method: 'widget.set_canvas_slot', risk: 'mutating',
    desc: '调整控件在 CanvasPanel 里的槽位（位置/尺寸/锚点/层级）。VerticalBox 等其它容器需相应槽位命令。',
    params: S({
      path: { type: 'string', description: 'Widget Blueprint 路径' },
      widget_name: { type: 'string', description: '要调整的控件名（widget_get_hierarchy 可见）' },
      position: { type: 'object', description: '位置', properties: { x: { type: 'number' }, y: { type: 'number' } } },
      size: { type: 'object', description: '尺寸', properties: { width: { type: 'number' }, height: { type: 'number' } } },
      anchors: { type: 'object', description: '锚点', properties: { x: { type: 'number' }, y: { type: 'number' } } },
      z_order: { type: 'number', description: '层级，大的盖在上面' },
    }, ['path', 'widget_name']),
  },
  {
    name: 'widget_make_variable', method: 'widget.make_variable', risk: 'mutating',
    desc: '把控件暴露为变量供蓝图驱动。',
    params: S({ path: { type: 'string', description: 'Widget Blueprint 路径' }, widget_name: { type: 'string', description: '控件名称' }, variable_name: { type: 'string', description: '变量名，默认用控件名' } }, ['path', 'widget_name']),
  },

  // ── C++ 补充 ──────────────────────────────────────────────
  {
    name: 'cpp_add_class', method: 'cpp.add_class', risk: 'destructive',
    desc: '新建一个 C++ 类（纯蓝图工程加不了，第一个类须手动建）。',
    params: S({
      name: { type: 'string', description: '新类名，不要带 U/A/F 前缀，如 TurretActor' },
      parent_class: { type: 'string', description: '父类，如 Actor / ActorComponent / UserWidget' },
      module: { type: 'string', description: '放进哪个模块，留空用主模块' },
      location: { type: 'string', enum: ['default', 'public', 'private'], description: '模块哪一层，默认 default' },
    }, ['name']),
  },

  // ══════════════════════════════════════════════════════════
  // 第四批：本地资料库（asset 资产库 / note 笔记 / library 蓝图片段库 / project 项目库）
  // 底层是本地 JSON（backend/data/vault/），非插件 RPC、非 SQLite
  // ══════════════════════════════════════════════════════════

  // ── 资产库 ────────────────────────────────────────────────
  {
    name: 'search_assets', method: 'vault.search_assets', risk: 'safe',
    desc: '搜本地素材库（按名字/类型/标签/文件夹）。素材库是工作台本地 JSON 索引，不是 UE 工程里的资产。',
    params: S({ query: { type: 'string' }, type: { type: 'string', description: '素材类型，如 StaticMesh/Texture2D' }, tag: { type: 'string' }, folder: { type: 'string' }, limit: { type: 'number' } }),
  },
  {
    name: 'library_overview', method: 'vault.library_overview', risk: 'safe',
    desc: '素材库概览：总数、类型分布、文件夹、标签、笔记数、片段数。',
    params: S({}),
  },
  {
    name: 'switch_vault', method: 'vault.switch_vault', risk: 'mutating',
    desc: '切换保管库（工作台为单库模式）。',
    params: S({}),
  },
  {
    name: 'annotate_asset', method: 'vault.annotate_asset', risk: 'mutating',
    desc: '给素材加/改备注。',
    params: S({ id: { type: 'string', description: '素材 id（search_assets 返回）' }, note: { type: 'string' } }, ['id']),
  },
  {
    name: 'list_tags', method: 'vault.list_tags', risk: 'safe',
    desc: '列出全部标签及计数。',
    params: S({}),
  },
  {
    name: 'manage_tags', method: 'vault.manage_tags', risk: 'mutating',
    desc: '给素材加/删标签。',
    params: S({ asset_id: { type: 'string' }, add: { type: 'array', items: { type: 'string' } }, remove: { type: 'array', items: { type: 'string' } } }, ['asset_id']),
  },
  {
    name: 'delete_tags', method: 'vault.delete_tags', risk: 'destructive',
    desc: '删除标签（从所有素材移除）。',
    params: S({ tags: { type: 'array', items: { type: 'string' } } }, ['tags']),
  },
  {
    name: 'delete_assets', method: 'vault.delete_assets', risk: 'destructive',
    desc: '软删除素材（可恢复）。',
    params: S({ ids: { type: 'array', items: { type: 'string' } } }, ['ids']),
  },
  {
    name: 'restore_assets', method: 'vault.restore_assets', risk: 'mutating',
    desc: '恢复软删除的素材。',
    params: S({ ids: { type: 'array', items: { type: 'string' } } }, ['ids']),
  },
  {
    name: 'move_assets', method: 'vault.move_assets', risk: 'mutating',
    desc: '把素材移到文件夹（moves: [{id, folder}]）。',
    params: S({ moves: { type: 'array', items: { type: 'object', properties: { id: { type: 'string' }, folder: { type: 'string' } }, required: ['id'] } } }, ['moves']),
  },
  {
    name: 'create_folders', method: 'vault.create_folders', risk: 'mutating',
    desc: '新建素材库文件夹。',
    params: S({ names: { type: 'array', items: { type: 'string' } } }, ['names']),
  },
  {
    name: 'rename_folder', method: 'vault.rename_folder', risk: 'mutating',
    desc: '重命名文件夹（同步更新素材归属）。',
    params: S({ old: { type: 'string' }, new: { type: 'string' } }, ['old', 'new']),
  },
  {
    name: 'delete_folders', method: 'vault.delete_folders', risk: 'destructive',
    desc: '软删除文件夹（素材仍在，folder 置空）。',
    params: S({ names: { type: 'array', items: { type: 'string' } } }, ['names']),
  },
  {
    name: 'restore_folders', method: 'vault.restore_folders', risk: 'mutating',
    desc: '恢复软删除的文件夹。',
    params: S({ names: { type: 'array', items: { type: 'string' } } }, ['names']),
  },

  // ── 笔记 ──────────────────────────────────────────────────
  {
    name: 'search_notes', method: 'vault.search_notes', risk: 'safe',
    desc: '搜笔记（按标题/内容/标签）。',
    params: S({ query: { type: 'string' }, tag: { type: 'string' }, limit: { type: 'number' } }),
  },
  {
    name: 'get_note', method: 'vault.get_note', risk: 'safe',
    desc: '读一条笔记。',
    params: S({ id: { type: 'string' } }, ['id']),
  },
  {
    name: 'create_note', method: 'vault.create_note', risk: 'mutating',
    desc: '新建笔记。',
    params: S({ title: { type: 'string' }, content: { type: 'string' }, tags: { type: 'array', items: { type: 'string' } } }, ['title']),
  },
  {
    name: 'update_note', method: 'vault.update_note', risk: 'mutating',
    desc: '改笔记（标题/内容/标签）。',
    params: S({ id: { type: 'string' }, title: { type: 'string' }, content: { type: 'string' }, tags: { type: 'array', items: { type: 'string' } } }, ['id']),
  },
  {
    name: 'delete_note', method: 'vault.delete_note', risk: 'destructive',
    desc: '删除笔记。',
    params: S({ id: { type: 'string' } }, ['id']),
  },

  // ── 蓝图片段库 ────────────────────────────────────────────
  {
    name: 'library_search', method: 'vault.library_search', risk: 'safe',
    desc: '搜蓝图片段库（按片段名/标签）。',
    params: S({ query: { type: 'string' }, tag: { type: 'string' }, limit: { type: 'number' } }),
  },
  {
    name: 'blueprint_library_save', method: 'vault.blueprint_library_save', risk: 'mutating',
    desc: '读一张蓝图图存成片段（读 blueprint.get_graph 存本地）。复用蓝图片段时用。',
    params: S({ blueprint_path: { type: 'string', description: '要存片段的蓝图路径' }, name: { type: 'string', description: '片段名' }, tags: { type: 'array', items: { type: 'string' } } }, ['blueprint_path']),
  },
  {
    name: 'blueprint_library_apply', method: 'vault.blueprint_library_apply', risk: 'mutating',
    desc: '把库里片段写进一张空图（blueprint.create_graph）。目标图必须已存在且为空。',
    params: S({ snippet_id: { type: 'string', description: '片段 id（library_search 返回）' }, target_path: { type: 'string', description: '目标空图蓝图路径' } }, ['snippet_id', 'target_path']),
  },

  // ── 项目库 ────────────────────────────────────────────────
  {
    name: 'project_list', method: 'vault.project_list', risk: 'safe',
    desc: '列本机 UE 工程（扫描 .uproject）或列引擎模板。',
    params: S({ action: { type: 'string', enum: ['list_projects', 'list_templates'], description: '默认 list_projects' }, dirs: { type: 'array', items: { type: 'string' }, description: '扫描的工程根目录' } }),
  },
  {
    name: 'project_manage', method: 'vault.project_manage', risk: 'destructive',
    desc: '打开工程（open_project 直接拉起编辑器）；建工程请用 workbench_ue5_bootstrap。',
    params: S({ action: { type: 'string', enum: ['open_project', 'create_project'], description: '默认 open_project' }, projectPath: { type: 'string', description: '工程 .uproject 路径（open_project 必填）' } }),
  },
  {
    name: 'project_organize', method: 'vault.project_organize', risk: 'mutating',
    desc: '工程分组管理（list/add_group/assign/register）。',
    params: S({ action: { type: 'string', enum: ['list', 'add_group', 'assign', 'register'] }, name: { type: 'string', description: 'add_group 的分组名' }, group: { type: 'string', description: 'assign 的目标分组' }, projects: { type: 'array', items: { type: 'string' } } }, ['action']),
  },
];

const byName = new Map(UAL_TOOLS.map((t) => [t.name, t]));

/** 查工具定义（name → {method, risk}） */
export function findUalTool(name) {
  return byName.get(name) || null;
}

/**
 * 参数适配层（2026-10-08）：修正「工具 schema 字段名」与「插件 C++ 实现认的字段名」
 * 之间的脱节。这类脱节是静默失效——模型按 schema 传参，插件悄悄忽略，导致路径被
 * 换成默认值、后续推理全建立在假路径上（故障链见会话流水）。
 *
 * 已知脱节清单（schema → 插件实现）：
 *   - blueprint.create：schema 要 blueprint_path，C++ 只读 path/folder/name。
 *     blueprint_path 被静默丢弃 → 走默认 /Game/UnrealAgent/Blueprints。
 *   - blueprint.add_variable：schema 用 variable_name/variable_type，C++ 读 name/type；
 *     对象引用类型 C++ 要求 type:"object" + object_class:"类名" 两个字段，
 *     而模型凭直觉写 "object:StaticMesh" 冒号格式（4 种变体全被拒，见会话流水）。
 */
function adaptArgs(method, args) {
  if (!args || typeof args !== 'object') return args;
  const a = { ...args };

  // 当前插件使用统一 targets；修改工具缺少目标时不能扩展为全场景操作。
  if (['actor.get_info', 'actor.set_transform', 'actor.set_property'].includes(method) && !a.targets) {
    if (typeof a.name === 'string' && a.name.trim()) a.targets = { names: [a.name] };
    else if (typeof a.path === 'string' && a.path.trim()) a.targets = { paths: [a.path] };
    else if (method === 'actor.get_info') a.targets = { filter: {} };
  }
  if (method === 'actor.set_transform' && !a.operation) {
    const set = {};
    for (const key of ['location', 'rotation', 'scale']) if (a[key] !== undefined) set[key] = a[key];
    a.operation = {};
    if (Object.keys(set).length) a.operation.set = set;
    if (a.snap_to_floor !== undefined) a.operation.snap_to_floor = a.snap_to_floor;
  }
  if (method === 'actor.set_property' && !a.properties && typeof a.property_name === 'string') {
    const key = a.component ? `${a.component}.${a.property_name}` : a.property_name;
    a.properties = { [key]: a.value };
  }

  if (method === 'blueprint.create') {
    // 模型按 schema 传 blueprint_path，C++ 只认 path/folder。
    // 若没传 path/folder，把 blueprint_path 拆成 path（目录）+ name（资产名）喂给 C++。
    if (typeof a.blueprint_path === 'string' && a.blueprint_path.trim()) {
      if (typeof a.path !== 'string' || !a.path.trim()) {
        a.path = a.blueprint_path;
      }
      // name 缺失时从 blueprint_path 尾段推断（C++ 也接受，但显式给更稳）
      if (typeof a.name !== 'string' || !a.name.trim()) {
        const tail = a.blueprint_path.replace(/\/+$/, '').split('/').pop() || '';
        if (tail) a.name = tail;
      }
    }
  }

  if (method === 'blueprint.add_variable') {
    // ① 字段名对齐：schema 用 variable_name/variable_type，C++ 只读 name/type
    if (typeof a.variable_name === 'string' && a.variable_name.trim() && !(typeof a.name === 'string' && a.name.trim())) {
      a.name = a.variable_name;
    }
    if (typeof a.variable_type === 'string' && a.variable_type.trim() && !(typeof a.type === 'string' && a.type.trim())) {
      a.type = a.variable_type;
    }
    // ② 冒号格式拆解：模型直觉写 "object:StaticMesh" / "object:/Script/Engine.StaticMesh"，
    //    C++ 只认独立的 type:"object" + object_class:"StaticMesh" 两个字段。
    //    拆开后 ResolveClassFromIdentifier 短名/全路径都认（自动补 U 前缀）。
    const m = /^(object|class|soft_object|soft_class)\s*:\s*(.+)$/.exec(String(a.type || '').trim());
    if (m) {
      a.type = m[1];
      if (!(typeof a.object_class === 'string' && a.object_class.trim())) {
        a.object_class = m[2].trim();
      }
    }
  }

  if (method === 'blueprint.add_component') {
    // 字段名对齐：schema 用 blueprint_path/component_class，C++ 读 blueprint_name/component_type/component_name（三个都必填）
    if (typeof a.blueprint_path === 'string' && a.blueprint_path.trim() && !(typeof a.blueprint_name === 'string' && a.blueprint_name.trim())) {
      a.blueprint_name = a.blueprint_path;
    }
    if (typeof a.component_class === 'string' && a.component_class.trim() && !(typeof a.component_type === 'string' && a.component_type.trim())) {
      a.component_type = a.component_class;
    }
    // component_name 必填：没传时从 component_type 推断（C++ 不接受省略）
    if (!(typeof a.component_name === 'string' && a.component_name.trim())) {
      if (typeof a.component_type === 'string' && a.component_type.trim()) a.component_name = a.component_type;
      else if (typeof a.component_class === 'string' && a.component_class.trim()) a.component_name = a.component_class;
    }
  }

  if (method === 'material.create') {
    // 字段名对齐 + 拆分：schema 用 material_path（完整路径如 /Game/Materials/M_Base），
    // C++ 认 material_name（必填，材质名）+ destination_path（目录，默认 /Game/Materials）。
    if (typeof a.material_path === 'string' && a.material_path.trim()) {
      const p = a.material_path.replace(/\/+$/, '');
      if (!(typeof a.material_name === 'string' && a.material_name.trim())) {
        a.material_name = p.split('/').pop() || p;
      }
      if (!(typeof a.destination_path === 'string' && a.destination_path.trim())) {
        const dir = p.split('/').slice(0, -1).join('/');
        if (dir) a.destination_path = dir;
      }
    }
  }

  if (method === 'material.compile') {
    // schema 用 asset_path，C++ 读 path
    if (typeof a.asset_path === 'string' && a.asset_path.trim() && !(typeof a.path === 'string' && a.path.trim())) {
      a.path = a.asset_path;
    }
  }

  if (method === 'material.describe' || method === 'material.get_graph') {
    // 字段名对齐：schema 用 asset_path，C++ 只读 path
    if (typeof a.asset_path === 'string' && a.asset_path.trim() && !(typeof a.path === 'string' && a.path.trim())) {
      a.path = a.asset_path;
    }
  }

  if (method === 'material.set_property') {
    // 字段名对齐 + 结构转换：schema 用 asset_path + property_name + value，
    // C++ 读 path + properties 对象（如 { blend_mode: "Translucent" }）。
    if (typeof a.asset_path === 'string' && a.asset_path.trim() && !(typeof a.path === 'string' && a.path.trim())) {
      a.path = a.asset_path;
    }
    // 把 property_name/value 折进 properties 对象；若已传 properties 则合并
    if (!a.properties || typeof a.properties !== 'object') a.properties = {};
    if (typeof a.property_name === 'string' && a.property_name.trim()) {
      a.properties[a.property_name.trim()] = a.value;
    }
  }

  if (method === 'material.set_param') {
    // schema 用 asset_path + param_name + value，C++ 读 path + params 对象
    if (typeof a.asset_path === 'string' && a.asset_path.trim() && !(typeof a.path === 'string' && a.path.trim())) {
      a.path = a.asset_path;
    }
    if (!a.params && typeof a.param_name === 'string' && a.param_name.trim()) {
      a.params = { [a.param_name]: a.value };
    }
  }

  if (method === 'material.set_node_value') {
    // 字段名对齐：schema 用 path，C++ 读 material_path
    if (typeof a.path === 'string' && a.path.trim() && !(typeof a.material_path === 'string' && a.material_path.trim())) {
      a.material_path = a.path;
    }
    // value 字符串 → 数字数组/对象。C++ 的 ParseMaterialValue 只认 number/array/object
    // 才设 bHasColor（给 Constant3Vector 设颜色），String 只设 bHasString ——
    // 而 schema 里 value 是 type:string，模型只能传字符串，于是「设成红色」永远失败。
    // 这里把模型写的字符串智能还原成 C++ 认的结构。
    if (typeof a.value === 'string') {
      const s = a.value.trim();
      // 单数字："0.25" → number（C++ Number 分支设标量+灰度颜色）
      if (/^-?\d*\.?\d+$/.test(s)) {
        a.value = parseFloat(s);
      } else {
        // 逗号分隔数字（含方括号包裹）："0.25,0.25,0.25" / "[0.25,0.25,0.25]" → 数组
        const inner = s.replace(/^\[|\]$/g, '');
        if (/^-?\d*\.?\d+(?:\s*,\s*-?\d*\.?\d+){1,3}$/.test(inner)) {
          a.value = inner.split(',').map((x) => parseFloat(x.trim()));
        } else {
          // JSON 对象字符串："{'r':0.25,...}" 或 "{\"r\":0.25,...}" → 对象
          try {
            const obj = JSON.parse(s);
            if (obj && typeof obj === 'object' && !Array.isArray(obj)) a.value = obj;
          } catch {
            /* 保持原字符串（可能是贴图路径等合法字符串值） */
          }
        }
      }
    }
  }

  if (method === 'level.new' || method === 'level.open') {
    // schema 用 level_path，C++ 读 path
    if (typeof a.level_path === 'string' && a.level_path.trim() && !(typeof a.path === 'string' && a.path.trim())) {
      a.path = a.level_path;
    }
  }

  if (method === 'widget.create') {
    // schema 用 blueprint_path（完整路径）+ parent，C++ 只读 name + folder + root_type
    if (typeof a.blueprint_path === 'string' && a.blueprint_path.trim()) {
      const p = a.blueprint_path.replace(/\/+$/, '');
      if (!(typeof a.name === 'string' && a.name.trim())) {
        a.name = p.split('/').pop() || p;
      }
      if (!(typeof a.folder === 'string' && a.folder.trim())) {
        const dir = p.split('/').slice(0, -1).join('/');
        if (dir) a.folder = dir;
      }
    }
  }

  if (method === 'widget.add_child') {
    // schema: blueprint_path/parent_widget/widget_type/widget_name → C++: path/parent_name/control_type/name
    if (typeof a.blueprint_path === 'string' && a.blueprint_path.trim() && !(typeof a.path === 'string' && a.path.trim())) {
      a.path = a.blueprint_path;
    }
    if (typeof a.parent_widget === 'string' && a.parent_widget.trim() && !(typeof a.parent_name === 'string' && a.parent_name.trim())) {
      a.parent_name = a.parent_widget;
    }
    if (typeof a.widget_type === 'string' && a.widget_type.trim() && !(typeof a.control_type === 'string' && a.control_type.trim())) {
      a.control_type = a.widget_type;
    }
    if (typeof a.widget_name === 'string' && a.widget_name.trim() && !(typeof a.name === 'string' && a.name.trim())) {
      a.name = a.widget_name;
    }
  }

  if (method === 'widget.set_property') {
    // schema: blueprint_path → C++: path
    if (typeof a.blueprint_path === 'string' && a.blueprint_path.trim() && !(typeof a.path === 'string' && a.path.trim())) {
      a.path = a.blueprint_path;
    }
  }

  return a;
}

/** 导出纯适配函数供负控探针测试（不碰 RPC，纯逻辑可判） */
export function adaptToolArgs(method, args) {
  return adaptArgs(method, args);
}

/**
 * 统一执行内置 UE 工具（按工具名分发到正确的执行路径）。
 * 与 agent.mjs 里旧 executeUalTool 等价，抽到这里供 agent / ue5boot 共用。
 * 返回 { ok, data?, error? }，与 ueboxCall 旧返回格式对齐。
 */
export async function executeUalTool(tool, args, timeoutMs) {
  try {
    args = adaptArgs(tool.method, args);
    // material_apply_graph 是多步编排，不走单 RPC
    if (tool.method === 'material.apply_graph') {
      const data = await ual.ualApplyMaterialGraph(args || {}, timeoutMs);
      return data && data.ok === false
        ? { ok: false, error: data.error || '写图失败', data }
        : { ok: true, data };
    }
    // local.* = 本地逻辑引擎工具（进程检测/HTTP/账本回滚/文件扫描）
    if (tool.method.startsWith('local.')) {
      const data = await ual.ualLocalCall(tool.method, args || {}, timeoutMs);
      return data && data.ok === false
        ? { ok: false, error: data.error || '执行失败', data }
        : { ok: true, data };
    }
    // vault.* = 本地资料库工具（资产库/笔记/蓝图片段库/项目库）
    if (tool.method.startsWith('vault.')) {
      const data = await vault.vaultLocalCall(tool.method, args || {}, timeoutMs);
      return data && data.ok === false
        ? { ok: false, error: data.error || '执行失败', data }
        : { ok: true, data };
    }
    // 其余 = 插件 RPC（直连 UnrealAgentLink，零依赖 UEBox）
    const data = await ual.ualCall(tool.method, args || {}, timeoutMs);
    return data && (data.ok === false || data.success === false)
      ? { ok: false, error: data.error || data.message || '命令执行失败', data }
      : { ok: true, data };
  } catch (e) {
    return { ok: false, error: String(e.message || e) };
  }
}

/** 工具清单（契约端点用）：name + method + risk + desc */
export function ualToolList() {
  return UAL_TOOLS.map(({ name, method, risk, desc }) => ({ name, method, risk, brief: desc.slice(0, 160) }));
}

/**
 * 给本地模型「小满」的工具清单（OpenAI function-calling 格式）。
 * 默认只放行 safe / mutating —— destructive（删资产/跑 Python/控制台命令）不进模型。
 * 当 allowFullAccess=true（用户开了「允许完全访问」）时，连 destructive 一起放行：
 * 此时模型需要 cpp_add_class / ue_run_python_script（写 C++ 源码）等破坏性工具才能干活。
 */
export function ualModelTools(allowFullAccess = false) {
  return UAL_TOOLS.filter((t) => allowFullAccess || t.risk === 'safe' || t.risk === 'mutating').map((t) => ({
    type: 'function',
    function: {
      name: t.name,
      description: t.desc,
      parameters: t.params,
    },
  }));
}
