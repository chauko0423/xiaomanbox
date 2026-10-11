#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;

/**
 * 蓝图命令处理器
 * 包含: blueprint.describe, blueprint.create, blueprint.add_component, blueprint.set_property, blueprint.compile
 * 
 * 对应文档: 蓝图开发接口文档.md
 */
class FUAL_BlueprintCommands
{
public:
	/**
	 * 注册所有蓝图相关命令到 CommandMap
	 * @param CommandMap 命令映射表
	 */
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	// ============================================================================
	// 命令处理函数
	// ============================================================================
	
	// blueprint.describe - 获取蓝图完整结构信息（组件、变量等）
	static void Handle_DescribeBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// blueprint.create - 创建蓝图
	static void Handle_CreateBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// blueprint.add_component - 为已存在的蓝图添加组件
	static void Handle_AddComponentToBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// blueprint.set_property - 设置蓝图属性（支持 CDO 和 SCS 组件）
	static void Handle_SetBlueprintProperty(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// blueprint.add_variable - 添加蓝图成员变量
	static void Handle_AddVariableToBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// blueprint.get_graph - 获取蓝图图表（节点、引脚等）
	static void Handle_GetBlueprintGraph(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	static void Handle_ListBlueprintGraphs(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// blueprint.create_function - 创建蓝图函数图表（可选定义输入输出参数）
	static void Handle_CreateFunctionGraph(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// blueprint.compile - 编译蓝图并可选保存
	static void Handle_CompileBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// blueprint.delete_node - 删除图表中的节点
	static void Handle_DeleteNode(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * blueprint.disconnect_pins - 断开某个引脚上的连线。
	 *
	 * 请求: { blueprint_path, graph_name?, node_id, pin,
	 *         other_node_id?, other_pin? }   // 两个 other_* 都给就只断那一根
	 * 响应: { ok, broken, remaining }
	 *
	 * 本来就没接线不算失败 —— 「确保这里是断的」应该能直接调。
	 */
	static void Handle_DisconnectBlueprintPins(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * blueprint.create_graph - 声明式蓝图图表创建（原子操作）
	 * 
	 * 一次性创建完整的蓝图图表，包括所有节点和连线。
	 * 这是一个原子操作：要么全部成功，要么全部失败。
	 * 
	 * 参数格式：
	 * {
	 *   "blueprint_path": "/Game/Blueprints/BP_Test",
	 *   "graph_name": "EventGraph",  // 可选，默认 EventGraph
	 *   "clear_existing": false,      // 可选，是否清除现有节点
	 *   "nodes": [
	 *     { "id": "node1", "type": "Event", "name": "BeginPlay" },
	 *     { "id": "node2", "type": "Function", "name": "KismetSystemLibrary.PrintString" }
	 *   ],
	 *   "connections": [
	 *     ["node1.Then", "node2.execute"]
	 *   ],
	 *   "pin_values": {  // 可选，设置 Pin 默认值
	 *     "node2.InString": "Hello World"
	 *   },
	 *   "auto_layout": true,  // 可选，自动布局
	 *   "compile": true       // 可选，完成后编译
	 * }
	 */
	static void Handle_CreateGraphDeclarative(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * blueprint.search_nodes —— 只读地搜可用函数，连引脚签名一起回。
	 *
	 * 让「查一批 → 写一整张」成为可能：在它之前引脚信息只能靠先把节点建出来
	 * 才拿得到，调用方因此被迫逐节点往返。
	 *
	 * 请求: { "query": "print", "blueprint_path": "...", "limit": 12 }
	 * 响应: { ok, query, match_count, total_candidates, functions:[
	 *          { name, member_name, class, is_pure, summary, params:[{name,type,dir}] } ] }
	 */
	static void Handle_SearchBlueprintNodes(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * blueprint.compile_all —— 批量编译蓝图。
	 *
	 * 此前只能一个一个编。改完一批蓝图（或改了某个被很多蓝图继承的父类）
	 * 之后想确认整个工程还编得过，只能逐个调，几十次往返。
	 *
	 * 请求: { "scope": "touched"|"all", "path": "/Game", "limit": 200 }
	 *   - touched（默认）：只编本插件改脏的那些蓝图
	 *   - all：扫 path 下的全部蓝图（默认 /Game）
	 * 响应: { ok, compiled_count, error_count, warning_count,
	 *         failures:[{path, errors, warnings, messages:[...]}], truncated }
	 */
	static void Handle_CompileAllBlueprints(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * blueprint.set_node_positions —— 批量挪节点。
	 *
	 * 排版算在 app 侧（ELK 分层布局），这里只负责把算好的坐标写进图里。
	 * 插件不做布局：节点渲染宽度取决于引脚数、函数名长度、编辑器语言，
	 * 这些 app 侧同样不知道，但至少 ELK 的分层结果比「按行铺开」强得多。
	 *
	 * 在它之前**没有任何办法挪动已有节点** —— 图一旦排乱了就只能人工去拖，
	 * 或者整张删掉重建。
	 *
	 * 请求: { "blueprint_path": "...", "graph_name": "EventGraph",
	 *         "positions": [{ "node_id": "GUID", "x": 0, "y": 0 }] }
	 * 响应: { ok, moved, not_found:[...] }
	 */
	static void Handle_SetNodePositions(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * blueprint.set_comment —— 建一个注释框，或者改一个已有的（文字、大小、
	 * 颜色、框住谁）。
	 *
	 * 注释框是蓝图里唯一的分组手段。在它之前工具集**读不出它有多大、框住了谁，
	 * 也写不了它**：排版只能绕着走，于是图整理完，用户手写的说明还贴在原处，
	 * 指着一片空白。
	 *
	 * 请求: { "blueprint_path": "...", "graph_name": "EventGraph",
	 *         "node_id": "GUID"（省略＝新建）, "text": "...",
	 *         "bounds": { x, y, width, height },
	 *         "enclose_nodes": ["GUID", ...],
	 *         "color": { r, g, b, a }, "font_size": 18 }
	 * 响应: { ok, node_id, created, text, x, y, width, height, enclosed_nodes }
	 */
	static void Handle_SetComment(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * blueprint.set_variable_meta —— 改变量的元数据。
	 *
	 * 在它之前**变量建出来就定死了**：只能加，不能调整它在细节面板里的表现。
	 * 而「暴露到细节面板让策划改」恰恰是蓝图变量最常见的用途 —— 做不到这个，
	 * 建出来的变量只能在图里用，等于少了一半价值。
	 *
	 * 只改传进来的字段，没传的原样不动 —— 调用方想只改分类时，
	 * 不该被迫把其余几项一起重报一遍（重报就意味着可能报错）。
	 *
	 * 请求: { "blueprint_path": "...", "name": "Speed",
	 *         "instance_editable": true, "blueprint_read_only": false,
	 *         "category": "移动", "tooltip": "每秒移动距离", "expose_on_spawn": true }
	 * 响应: { ok, blueprint_path, name, applied:[...], variable:{...} }
	 */
	static void Handle_SetVariableMeta(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * blueprint.remove_variable —— 删变量。
	 *
	 * 配合 add_variable 才闭环：此前加错一个变量就只能留着，或者让用户手动去删。
	 * 删除不可逆，且会让引用它的节点变成 orphan，所以响应里带上受影响的图。
	 *
	 * 请求: { "blueprint_path": "...", "name": "Speed" }
	 * 响应: { ok, blueprint_path, name, removed }
	 */
	static void Handle_RemoveVariable(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * blueprint.event_dispatcher —— 建 / 列事件分发器。
	 *
	 * 事件分发器是蓝图里做解耦的标准手段（UI 通知逻辑、子系统通知关卡）。
	 * 此前一个都建不出来，凡是需要「A 发事件 B 收」的需求全都做不了 ——
	 * 只能退化成 B 每帧去轮询 A，那是错的写法。
	 *
	 * 请求: { "blueprint_path": "...", "action": "add"|"list", "name": "OnDoorOpened",
	 *         "params": [{ "name": "Opener", "type": "object", "class": "Actor" }] }
	 * 响应(add):  { ok, name, created }
	 * 响应(list): { ok, dispatchers: [{ name, params:[{name,type}] }] }
	 */
	static void Handle_EventDispatcher(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * blueprint.component_event —— 列 / 绑组件事件。
	 *
	 * 「走进触发区就开门」「点一下就捡起来」这类需求的入口全在这里：
	 * OnComponentBeginOverlap、OnClicked、OnComponentHit 都是组件上的
	 * BlueprintAssignable 委托。
	 *
	 * 此前**一个都绑不了** —— 组件加得上，但没办法在它的事件上挂逻辑。
	 * 于是所有触发式交互都做不了，只能退化成 Tick 里每帧算距离。
	 *
	 * 请求: { "blueprint_path": "...", "action": "list"|"add",
	 *         "component_name": "TriggerBox", "event_name": "OnComponentBeginOverlap",
	 *         "graph_name": "EventGraph" }
	 * 响应(list): { ok, component_name, component_class,
	 *               events:[{ name, params:[{name,type}] }] }
	 * 响应(add):  { ok, node_id, event_name, reused, pins:[...] }
	 */
	static void Handle_ComponentEvent(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * blueprint.function_signature —— 改函数的参数表 / 删函数。
	 *
	 * `create_function` 建完就定死：加不了参数、删不掉。于是函数一旦建错，
	 * 只能连着蓝图一起重来，或者留一个用不上的空函数在那儿。
	 *
	 * 请求: { "blueprint_path": "...", "graph_name": "OpenDoor",
	 *         "action": "add_param"|"remove_param"|"remove_function",
	 *         "param": { "name": "Speed", "type": "float", "direction": "in"|"out" } }
	 * 响应: { ok, graph_name, action, inputs:[...], outputs:[...] }
	 */
	static void Handle_FunctionSignature(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * blueprint.set_parent_class —— 改蓝图父类。
	 *
	 * 「这个 BP 应该继承 Character 而不是 Actor」在建完之后没法改，
	 * 只能重建一个再把图抄过去 —— 而图抄过去这件事本身就容易出错。
	 *
	 * 改父类会让引用旧父类成员的节点失效，所以响应里带上编译结果。
	 *
	 * 请求: { "blueprint_path": "...", "parent_class": "Character" }
	 * 响应: { ok, old_parent, new_parent, compile_errors, compile_warnings, messages:[...] }
	 */
	static void Handle_SetParentClass(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * blueprint.export_t3d —— 把节点导成 T3D 文本（= 编辑器里 Ctrl+C 出来的那个）。
	 *
	 * ## 这一对命令是干什么的
	 *
	 * 现有的 `get_graph` / `create_graph` 走的是**结构化 JSON**：读侧描述节点，
	 * 写侧照着描述重新**建**一个默认形态的节点。代价是「用户改过形状的节点写不回去」——
	 * 手动加过输出引脚的 Sequence、折叠成 Composite 的一整块逻辑，
	 * 写侧建出来的都是默认形态，多出来的东西静默丢失。
	 *
	 * T3D 这条路不重建节点，它让**引擎自己**反序列化，
	 * 走的就是 Ctrl+C / Ctrl+V 那条路（`FEdGraphUtilities`，
	 * 编辑器的 `FBlueprintEditor::PasteNodesHere` 调的同一对函数）。
	 * 所以编辑器里复制粘贴保得住的东西，这条路都保得住。
	 *
	 * **这一对是路线验证用的，还没有在任何产品路径上接线。**
	 *
	 * 请求: { "blueprint_path": "...", "graph_name": "EventGraph",
	 *         "node_ids": ["GUID", ...] }   // 不给就导整张图
	 * 响应: { ok, blueprint_path, graph_name, node_count, exported_node_ids:[...],
	 *         text, text_length, truncated, not_found:[...] }
	 */
	static void Handle_ExportNodesT3D(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * blueprint.import_t3d —— 把 T3D 文本粘进一张图。
	 *
	 * 镜像编辑器的粘贴流程（`BlueprintEditor.cpp` 的 `PasteNodesHere`）：
	 * 先问 `CanImportNodesFromText`，再 `ImportNodesFromText`，
	 * 然后重新发 GUID、按需挪位置、按节点类型决定结构性还是普通脏标记。
	 *
	 * ## 两处**故意**和编辑器不一样
	 *
	 * ① 编辑器在这一步会弹 `SFixupSelfContextDialog` 让用户手工修
	 *   「引用了自身上下文但找不到函数」的节点。自动化路径上不能弹模态框，
	 *   所以改成**检测出来如实报告**（`self_context_unresolved`），不拦截、不代替用户决定。
	 * ② 编辑器还会跑一个 `FUpdatePastedNodes` 做跨蓝图的变量/自引用重定向。
	 *   那个类声明在 `Editor/Kismet/Private/BlueprintEditor.cpp` 里，**插件调不到**。
	 *   后果：跨蓝图粘贴时引用变量的节点会是断的。
	 *   这是这条路的**已知边界**，不是 bug —— 调用方该在发之前做依赖扫描。
	 *
	 * `require_empty` 复用和 `create_graph` **同一个**判据函数，
	 * 且同样在进事务之前检查：不空就一个字都不写。
	 *
	 * 请求: { "blueprint_path": "...", "graph_name": "EventGraph", "text": "Begin Object ...",
	 *         "require_empty": false, "offset_x": 0, "offset_y": 0, "compile": false }
	 * 响应: { ok, blueprint_path, graph_name, imported_count, undoable,
	 *         nodes:[{ id, class, title, x, y, is_ghost_node }],
	 *         self_context_unresolved:[...], structural,
	 *         compiled, compile_error_count, compile_warning_count, diagnostics:[...] }
	 */
	static void Handle_ImportNodesT3D(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// ============================================================================
	// 辅助函数
	// ============================================================================
	
	/**
	 * 构建蓝图结构 JSON 对象
	 * 包含：基本信息、所有组件（SCS + 继承）、变量列表、编译状态
	 * 被 describe、create、add_component 复用
	 * 
	 * @param Blueprint 蓝图对象
	 * @param bIncludeVariables 是否包含变量列表（默认 true）
	 * @param bIncludeComponentDetails 是否包含组件详细属性（默认 false，仅返回基本信息）
	 * @return JSON 对象
	 */
	static TSharedPtr<FJsonObject> BuildBlueprintStructureJson(
		UBlueprint* Blueprint, 
		bool bIncludeVariables = true, 
		bool bIncludeComponentDetails = false
	);

private:
	/**
	 * 收集蓝图的所有组件信息（包括 SCS 添加的和继承的）
	 * @param Blueprint 蓝图对象
	 * @return 组件信息数组
	 */
	static TArray<TSharedPtr<FJsonValue>> CollectComponentsInfo(UBlueprint* Blueprint);
	
	/**
	 * 收集蓝图的变量列表
	 * @param Blueprint 蓝图对象
	 * @return 变量信息数组
	 */
	static TArray<TSharedPtr<FJsonValue>> CollectVariablesInfo(UBlueprint* Blueprint);
};
