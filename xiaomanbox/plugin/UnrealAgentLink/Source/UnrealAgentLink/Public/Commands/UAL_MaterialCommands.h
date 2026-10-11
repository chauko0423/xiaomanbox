#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UMaterial;
class UMaterialExpression;

/**
 * 材质命令处理器
 * 实现 PBR 材质自动化流程：从贴图创建材质、应用材质到 Actor
 * 
 * 包含 4 个原子工具：
 * - material.create   : 从贴图创建 Material Instance
 * - material.apply    : 将材质应用到场景中的 Actor
 * - material.describe : 查询材质的参数和贴图槽信息
 * - material.set_param: 设置材质参数（颜色、标量、向量等）
 * 
 * 核心功能：
 * 1. 智能识别贴图类型（Albedo/Normal/Roughness 等）
 * 2. 自动分组同一资产的多张贴图
 * 3. 创建 PBR Material Instance 并连接贴图
 * 4. 应用材质到 StaticMeshComponent 的材质槽
 */
class FUAL_MaterialCommands
{
public:
	/**
	 * 注册所有材质相关命令到 CommandMap
	 * @param CommandMap 命令映射表
	 */
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	// ========================================================================
	// Public Handlers (由 Dispatcher 调用)
	// ========================================================================
	
	/**
	 * material.create - 从贴图创建 PBR 材质
	 * 
	 * 使用场景：
	 * 1. 用户导入了多张 PBR 贴图（如 T_Wood_D.png, T_Wood_N.png）
	 * 2. Agent 调用此命令自动创建材质并连接贴图
	 * 
	 * 请求参数：
	 * - texture_paths: 贴图资产路径列表（必填）
	 *   例: ["/Game/Textures/T_Wood_D", "/Game/Textures/T_Wood_N"]
	 * - material_name: 输出材质名称（可选，自动生成）
	 * - destination_path: 输出材质路径（可选，默认与贴图同目录）
	 * - parent_material: 父材质路径（可选，默认使用 M_MasterPBR）
	 * 
	 * 响应数据：
	 * - material_path: 创建的材质路径
	 * - material_name: 材质名称
	 * - texture_bindings: 贴图槽绑定信息
	 *   例: { "BaseColor": "/Game/Textures/T_Wood_D", "Normal": "/Game/Textures/T_Wood_N" }
	 * 
	 * @param Payload 请求参数
	 * @param RequestId 请求 ID
	 */
	static void Handle_CreateMaterial(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.apply - 将材质应用到 Actor
	 * 
	 * 使用场景：
	 * 1. 用户创建了材质，想应用到场景中的某个物体
	 * 2. Agent 调用此命令将材质设置到指定 Actor 的材质槽
	 * 
	 * 请求参数：
	 * - targets: Actor 选择器（统一格式：names/paths/filter）
	 * - material_path: 材质资产路径（单槽写法必填，与 slots 二选一）
	 * - slot_index: 材质槽索引（可选，默认 0）
	 * - slot_name: 材质槽名称（可选，与 slot_index 二选一）
	 * - slots: 多槽写法 [{slot_index | slot_name, material_path}]，一次改同一组件的多个槽，
	 *   没点名的槽不动；某个 Actor 上有一条对不上就整个跳过，一个槽都不改
	 * 
	 * 响应数据：
	 * - applied_count: 成功应用的 Actor 数量
	 * - actors: 受影响的 Actor 信息列表，每个带 slots: [{slot_index, slot_name, previous, material}]
	 * 
	 * @param Payload 请求参数
	 * @param RequestId 请求 ID
	 */
	static void Handle_ApplyMaterial(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.describe - 获取材质详细信息
	 * 
	 * 使用场景：
	 * 1. Agent 需要了解材质有哪些参数可以调整
	 * 2. 检查材质当前使用了哪些贴图
	 * 
	 * 请求参数：
	 * - path: 材质资产路径（必填）
	 * - include_parent_params: 是否包含父材质参数（可选，默认 true）
	 * 
	 * 响应数据：
	 * - name: 材质名称
	 * - path: 材质路径
	 * - class: 材质类型（Material/MaterialInstance）
	 * - parent_material: 父材质路径（如果是 Instance）
	 * - scalar_params: 标量参数列表
	 * - vector_params: 向量参数列表 
	 * - texture_params: 贴图参数列表
	 * 
	 * @param Payload 请求参数
	 * @param RequestId 请求 ID
	 */
	static void Handle_DescribeMaterial(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.set_param - 设置材质参数
	 * 
	 * 使用场景：
	 * 1. 调整材质的颜色、金属度、粗糙度等参数
	 * 2. 更换材质使用的贴图
	 * 
	 * 请求参数：
	 * - path: 材质资产路径（必填，必须是 MaterialInstanceConstant）
	 * - params: 参数键值对
	 *   - 标量: { "Roughness": 0.5, "Metallic": 1.0 }
	 *   - 向量/颜色: { "BaseColor": { "r": 1, "g": 0, "b": 0 } }
	 *   - 贴图: { "NormalMap": "/Game/Textures/T_Normal" }
	 * 
	 * 响应数据：
	 * - updated_params: 成功更新的参数列表
	 * - errors: 失败的参数及原因
	 * 
	 * @param Payload 请求参数
	 * @param RequestId 请求 ID
	 */
	static void Handle_SetMaterialParam(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// ========================================================================
	// Phase 1: 材质图表编辑命令
	// ========================================================================
	
	/**
	 * material.get_graph - 获取材质图表结构
	 * 
	 * 使用场景：
	 * 1. 查看材质中所有表达式节点
	 * 2. 获取节点的引脚和连接关系
	 * 
	 * 请求参数：
	 * - path: 材质资产路径（必填）
	 * - include_values: 是否包含节点当前值（可选，默认 true）
	 * 
	 * 响应数据：
	 * - nodes: 节点列表（node_id, class, pins, position 等）
	 * - connections: 连接列表（from -> to）
	 * - material_pins: 材质主节点可用引脚
	 */
	static void Handle_GetMaterialGraph(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.search_nodes - 材质节点说明书
	 *
	 * 对应蓝图那边的 `blueprint.search_nodes`：给个关键字，回来的是节点类型的
	 * 确切名字和每个引脚的名字/类型。写图之前查一批，别靠「先建一个再读回来」
	 * 试引脚名 —— 那些名字没有规律（Sine 的输入叫 Input、Lerp 的叫 A/B/Alpha、
	 * TextureSample 的 UV 输入叫 Coordinates 而不是 UVs），猜不出来。
	 *
	 * 请求参数：
	 * - query: 节点类型名关键字（可选，留空 = 列出全部）
	 * - limit: 最多回几条（可选，默认 12，上限 100）
	 *
	 * 响应数据：
	 * - nodes: 每条带 node_type / class / inputs / outputs / requires / has_value
	 * - match_count / total_types / truncated
	 *
	 * 读的是类默认对象，不碰任何资产，也不需要先打开材质。
	 */
	static void Handle_SearchMaterialNodes(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * material.add_node - 添加材质表达式节点
	 *
	 * 使用场景：
	 * 1. 添加 TextureSample、Constant、Math 等材质节点
	 * 2. 创建参数节点（ScalarParameter、VectorParameter）
	 * 
	 * 请求参数：
	 * - material_path: 材质资产路径（必填）
	 * - node_type: 节点类型（必填）
	 * - node_name: 参数名称（参数节点时使用）
	 * - position: 节点位置（可选）
	 * - initial_value: 初始值（可选）
	 * - texture_path: 贴图路径（TextureSample 时使用）
	 * 
	 * 响应数据：
	 * - node_id: 节点唯一标识
	 * - pins: 引脚列表
	 */
	static void Handle_AddMaterialNode(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.connect_pins - 连接材质节点引脚
	 * 
	 * 使用场景：
	 * 1. 连接节点输出到材质主节点
	 * 2. 连接节点之间的引脚
	 * 
	 * 请求参数：
	 * - material_path: 材质资产路径（必填）
	 * - source_node: 源节点 ID（必填）
	 * - source_pin: 源引脚名称（必填）
	 * - target_node: 目标节点 ID（必填）
	 * - target_pin: 目标引脚名称（必填）
	 * 
	 * 响应数据：
	 * - connection: 连接信息
	 */
	static void Handle_ConnectMaterialPins(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.compile - 编译材质
	 * 
	 * 使用场景：
	 * 1. 验证材质图表的正确性
	 * 2. 生成 Shader 并获取编译错误
	 * 
	 * 请求参数：
	 * - path: 材质资产路径（必填）
	 * - force_recompile: 是否强制重新编译（可选）
	 * 
	 * 响应数据：
	 * - compiled: 是否编译成功
	 * - errors: 错误列表
	 * - warnings: 警告列表
	 */
	static void Handle_CompileMaterial(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.set_node_value - 设置材质节点值
	 * 
	 * 使用场景：
	 * 1. 修改 Constant 节点的数值
	 * 2. 设置参数节点的默认值
	 * 3. 更换 TextureSample 节点的贴图
	 * 
	 * 请求参数：
	 * - material_path: 材质资产路径（必填）
	 * - node_id: 节点 ID（必填）
	 * - value: 要设置的值（必填）。数字、布尔、贴图路径字符串、
	 *          `{r,g,b,a?}`、`{x,y,z?,w?}`、`{u_tiling,v_tiling}`、2~4 个数的数组
	 *
	 * 响应数据：
	 * - node_id / node_class
	 * - old_value: 修改前的值
	 * - new_value: 修改后的值（回读出来的，不是把入参原样回显）
	 *
	 * 设不上时回 400 并说明原因 —— 不要再像以前那样取消事务却回 200。
	 */
	static void Handle_SetMaterialNodeValue(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.delete_node - 删除材质节点
	 * 
	 * 使用场景：
	 * 1. 删除不需要的材质节点
	 * 2. 清理材质图表
	 * 
	 * 请求参数：
	 * - material_path: 材质资产路径（必填）
	 * - node_id: 要删除的节点 ID（必填）
	 *
	 * 指向这个节点的连线一律先断掉再删 —— 留着就是指向不存在节点的悬空连线，
	 * 材质会从此编不过。原来有个 disconnect_first 参数，实现里从来没读过它。
	 *
	 * 响应数据：
	 * - node_id: 被删除的节点 ID
	 * - disconnected_count: 实际断开的连接数量
	 */
	static void Handle_DeleteMaterialNode(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * material.set_node_positions - 批量挪节点（排版落地）
	 *
	 * 蓝图有 blueprint.set_node_positions，材质一直没有 —— 也就是说节点一旦
	 * 建出来就再也挪不动了。而 add_node 不传 position 时坐标是 (0,0)，
	 * 于是整张图叠成一摞，根本没法看。
	 *
	 * 请求参数：
	 * - material_path: 材质资产路径（必填）
	 * - positions: [{ node_id, x, y }, ...]（必填）
	 *
	 * 响应数据：
	 * - moved: 挪动的节点数
	 * - not_found: 没找到的 node_id 列表（不静默跳过 —— 多半是拿着过期的图在算坐标）
	 */
	static void Handle_SetMaterialNodePositions(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// ========================================================================
	// Phase 2: 材质管理命令（智能容错）
	// ========================================================================
	
	/**
	 * material.duplicate - 复制材质
	 * 
	 * 智能容错特性：
	 * - 路径自动补全（省略 /Game/ 时自动添加）
	 * - 模糊路径匹配（返回相似资产建议）
	 * - 名称冲突自动处理
	 * 
	 * 请求参数：
	 * - source_path: 源材质路径（支持简写）
	 * - new_name: 新材质名称（可选）
	 * - destination_path: 目标路径（可选）
	 * 
	 * 响应数据：
	 * - new_path: 新材质的完整路径
	 * - suggestions: 如失败，返回修复建议
	 * - similar_assets: 相似资产列表（路径错误时）
	 */
	static void Handle_DuplicateMaterial(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.set_property - 设置材质属性
	 * 
	 * 智能容错特性：
	 * - 属性值多格式支持（枚举名/中文/数字索引）
	 * - 大小写不敏感
	 * - 失败时返回有效值列表
	 * 
	 * 请求参数：
	 * - path: 材质资产路径
	 * - properties: 属性键值对
	 *   - blend_mode: Opaque/Masked/Translucent/Additive（或 不透明/半透明）
	 *   - shading_model: DefaultLit/Unlit/Subsurface（或 默认/无光照）
	 *   - two_sided: 是否双面
	 * 
	 * 响应数据：
	 * - updated_properties: 成功更新的属性
	 * - failed_properties: 失败的属性及修复建议
	 * - current_state: 当前材质状态
	 */
	static void Handle_SetMaterialProperty(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.create_instance - 创建材质实例
	 * 
	 * 智能容错特性：
	 * - 父材质路径自动补全
	 * - 实例名称自动生成
	 * - 返回可用参数列表
	 * 
	 * 请求参数：
	 * - parent_path: 父材质路径
	 * - instance_name: 实例名称（可选）
	 * - destination_path: 保存路径（可选）
	 * - initial_params: 初始参数值（可选）
	 * 
	 * 响应数据：
	 * - instance_path: 新实例路径
	 * - available_params: 可用参数列表
	 * - suggestions: 如失败，返回修复建议
	 */
	static void Handle_CreateMaterialInstance(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * 给图里每个表达式编号，就是 `material.get_graph` / `add_node` 回的那个 node_id。
	 *
	 * 公开出来是因为「用户在材质图里选中了哪个节点」（editor.get_focus_context）
	 * 也要报同一套 id —— 报一套调用方喂不回来的编号等于没报。
	 * 编号规则只此一份，别在别处重写。
	 */
	static void BuildExpressionIds(UMaterial* Material, TMap<UMaterialExpression*, FString>& OutIds);

	/**
	 * 只要**一个**表达式的 node_id，别为它把整张表建一遍。
	 *
	 * 和 `BuildExpressionIds` 同一套规则（跳空槽、按数组序、`类名_序号`），
	 * 区别只是不建 TMap、只拼一次字符串。`add_node` 每建一个节点都要报 id，
	 * 走建表那条路的话，一次 200 节点的 apply_graph 就是两万次 Printf。
	 *
	 * 找不到（表达式不在这张图里）时返回空串，由调用方决定怎么兜。
	 */
	static FString ExpressionIdFor(UMaterial* Material, UMaterialExpression* Target);

private:
	// ========================================================================
	// 智能容错辅助函数
	// ========================================================================
	
	/**
	 * 标准化资产路径（智能补全）
	 * - 自动添加 /Game/ 前缀（如果缺失）
	 * - 移除多余的斜杠
	 * - 处理文件扩展名
	 */
	static FString NormalizePath(const FString& InputPath, const FString& DefaultPrefix = TEXT("/Game/Materials"));
	
	/**
	 * 查找相似资产（用于路径错误时的建议）
	 * @return 相似资产路径列表（最多5个）
	 */
	static TArray<FString> FindSimilarAssets(const FString& PartialPath, const FString& AssetClass = TEXT("MaterialInterface"));
	
	/**
	 * 解析 BlendMode（支持多种格式）
	 * @param Value 输入值（字符串或数字）
	 * @param OutMode 输出的 BlendMode
	 * @return 是否解析成功
	 */
	static bool ParseBlendMode(const FString& Value, EBlendMode& OutMode);
	
	/**
	 * 解析 ShadingModel（支持多种格式）
	 * @param Value 输入值
	 * @param OutModel 输出的 ShadingModel  
	 * @return 是否解析成功
	 */
	static bool ParseShadingModel(const FString& Value, EMaterialShadingModel& OutModel);
	
	/**
	 * 获取 BlendMode 的有效值列表（用于错误提示）
	 */
	static TArray<FString> GetValidBlendModes();
	
	/**
	 * 获取 ShadingModel 的有效值列表
	 */
	static TArray<FString> GetValidShadingModels();
	
	/**
	 * 把一个 JSON 值写进材质节点。
	 *
	 * add_node 的 initial_value 和 set_node_value 的 value 共用这一条路径 ——
	 * 它们以前各写各的解析，于是各错各的：前者只认 `{"value": ...}`（而调用方
	 * 传的是裸值，所以从来没生效过），后者只认标量和贴图（所有颜色节点都设不了）。
	 *
	 * 接受的形状与调用方的 schema 逐一对应：数字、布尔、字符串（贴图路径）、
	 * `{r,g,b,a?}`、`{x,y,z?,w?}`、`{u_tiling,v_tiling,...}`、2~4 个数的数组。
	 *
	 * @param Expression 目标节点
	 * @param RawValue   原始 JSON 值
	 * @param OutError   失败原因，**只在返回 false 时有意义**
	 * @return 是否真的改到了东西。false 时调用方必须报错，不要回 200
	 */
	static bool ApplyValueToNode(UMaterialExpression* Expression, const TSharedPtr<FJsonValue>& RawValue, FString& OutError);

	/**
	 * 按 node_id 找表达式节点。
	 *
	 * 两种 ID 都认：`get_graph` / `add_node` 返回的 `ClassName_Index`，
	 * 以及 UE 对象的实际名字。这段查找原来在 connect_pins（两处）、
	 * set_node_value、delete_node 里各抄了一遍，共四份。
	 */
	static UMaterialExpression* FindExpressionById(UMaterial* Material, const FString& NodeId);

	/**
	 * 读出节点的当前值，读不出来返回无效指针。
	 *
	 * set_node_value 用它填 old_value / new_value，get_graph 用它填 nodes[].value。
	 */
	static TSharedPtr<FJsonValue> ReadNodeValue(UMaterialExpression* Expression);

	/**
	 * 源引脚名 → 输出序号。
	 *
	 * `TextureSample` 的输出依次是 RGB(0) R(1) G(2) B(3) A(4) RGBA(5)。
	 * 以前接材质主节点时这里写死 0，接节点时只认纯数字 —— 于是
	 * 「把贴图的 Alpha 接到 Opacity」实际接的是 RGB，返回体里还照抄
	 * `"from": "TextureSample_0.A"`，看起来完全正确。
	 *
	 * @param OutAvailable 认不出来时填上该节点实际有哪些输出名，用于报错
	 * @return 是否解析成功。名字留空时取 0 并返回 true（多数节点只有一个输出）
	 */
	static bool ResolveOutputIndex(UMaterialExpression* Expression, const FString& PinName,
		int32& OutIndex, TArray<FString>& OutAvailable);
	
	/**
	 * material.disconnect_pins —— 断开一条连线。
	 *
	 * 此前**只能连不能断**。改图于是只有两条路：把节点整个删掉重建，
	 * 或者留着错误的连线不管。前者会连带丢掉这个节点上其他正确的连线，
	 * 后者直接编译失败 —— 两条都不对。
	 *
	 * 断的是**输入端**，因为一个输入只能接一根线，指定输入就唯一确定了这根线；
	 * 输出端可以扇出多根，光说「断开某个输出」是有歧义的。
	 *
	 * 请求: { "material_path": "...", "target_node": "Multiply_1"|"Material", "target_pin": "A" }
	 * 响应: { ok, disconnected, target_node, target_pin, was_connected_to }
	 */
	static void Handle_DisconnectMaterialPins(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * material.delete_unused_nodes —— 删掉对最终输出没有贡献的节点。
	 *
	 * 边试边搭出来的图里总会剩下一堆断开的节点。它们不影响渲染结果，
	 * 但会让后续每次 get_graph 都多读一遍，模型也会被这些无主节点带偏
	 * （「这个 Multiply 是干嘛的？」）。
	 *
	 * 从材质主节点的各个输入反向走一遍可达性，没被走到的就是无用的。
	 * 默认 dry_run=true 只报不删 —— 删除不可逆，先让调用方看一眼名单。
	 *
	 * 请求: { "material_path": "...", "dry_run": true }
	 * 响应: { ok, dry_run, unused_count, unused:[...], deleted_count }
	 */
	static void Handle_DeleteUnusedMaterialNodes(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * material.parameter_collection —— 材质参数集合（MPC）。
	 *
	 * 「一个开关控制全场景材质」的标准做法：昼夜、季节、队伍配色、
	 * 受击闪白。参数放在一个 MPC 资产里，任意数量的材质引用它，
	 * 运行时改一处全场景跟着变。
	 *
	 * 此前没有 MPC，这类需求只能退化成「逐个材质实例去改参数」——
	 * 材质一多就不可行，而且运行时改不了。
	 *
	 * 建完要在材质里用 `material.add_node` 加一个 `CollectionParameter`
	 * 节点，带上 `collection_path` 和 `node_name`（参数名）才接得上。
	 *
	 * 请求: { "action": "create"|"list"|"set", "collection_path": "/Game/MPC_Weather",
	 *         "collection_name": "MPC_Weather", "destination_path": "/Game",
	 *         "scalars": [{ "name": "Wetness", "value": 0.0 }],
	 *         "vectors": [{ "name": "SkyTint", "value": {"r":1,"g":1,"b":1,"a":1} }] }
	 * 响应: { ok, collection_path, scalars:[...], vectors:[...] }
	 */
	static void Handle_MaterialParameterCollection(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * material.create_function —— 建材质函数（MaterialFunction）。
	 *
	 * 一组节点用在三个材质里，就该抽成材质函数：改一次三个都跟着变。
	 * 此前没有，只能在每个材质里把同一串节点重搭一遍 —— 搭得越多越不敢改。
	 *
	 * 建完在材质里用 `material.add_node` 的 `MaterialFunctionCall`
	 * 加上 `function_path` 引用它。
	 *
	 * 请求: { "function_name": "MF_Wetness", "destination_path": "/Game/Materials",
	 *         "description": "..." }
	 * 响应: { ok, function_path, function_name }
	 */
	static void Handle_CreateMaterialFunction(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * material.get_referencers —— 反查谁在用这个资产。
	 *
	 * 「这个材质被哪些资产用了」「这张贴图还有人用吗」——
	 * 改之前问一句、删之前问一句，此前只能靠人去编辑器里点「引用查看器」。
	 *
	 * 请求: { "asset_path": "/Game/Materials/M_Wood", "limit": 50 }
	 * 响应: { ok, asset_path, referencer_count, referencers:[{path, class}], truncated }
	 */
	static void Handle_GetMaterialReferencers(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
};

