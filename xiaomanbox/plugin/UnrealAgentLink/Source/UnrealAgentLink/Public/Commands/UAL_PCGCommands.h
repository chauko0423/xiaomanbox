#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UObject;
class UClass;

/**
 * PCG（程序化内容生成）命令处理器。
 *
 * ## 为什么整个文件不 include 任何 PCG 头文件
 *
 * PCG 是**可选插件**：5.0/5.1 引擎里根本没有，5.2 起有但默认关闭。而我们发的是
 * 预编译二进制，装进别人的工程。一旦 Build.cs 链接 PCG，用户工程没开 PCG 时整个
 * UnrealAgentLink 就加载不起来 —— 为一个可选功能把所有老用户搞挂。
 *
 * 所以全程走反射（见 UAL_ReflectCall.h）。代价是拿不到编译期类型检查，换来的是：
 *   - Build.cs / .uplugin 一行不用改
 *   - 不强制用户工程启用 PCG
 *   - PCG 不在时命令干净地回一句「未启用」，主模块毫发无伤
 *
 * ## 版本窗口
 *
 * PCG 自己的成熟度是分版本的：5.2/5.3 实验性、5.4–5.6 Beta、5.7 起正式版。
 * 我们不按引擎版本硬卡 —— `pcg.status` 如实报出插件版本，让上层决定。
 * 但 5.2/5.3 上 PCG 的 API 还在变，行为不保证。
 *
 * ## 节点怎么寻址
 *
 * 用 UPCGNode 的对象名（FName）当 node_id，它在一张图内唯一且稳定。
 * 图的输入/输出节点不在 Nodes 数组里，另外用别名 "Input" / "Output" 访问。
 */
class FUAL_PCGCommands
{
public:
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	/** pcg.status — PCG 插件是否可用、版本多少。所有别的命令失败前先看这个 */
	static void Handle_Status(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/** pcg.create_graph — 新建一个 PCGGraph 资产（自带 Input/Output 节点） */
	static void Handle_CreateGraph(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/** pcg.get_graph — 读整张图：节点、位置、引脚、连线 */
	static void Handle_GetGraph(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/** pcg.list_node_types — 列出可用的节点类型（UPCGSettings 的子类） */
	static void Handle_ListNodeTypes(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/** pcg.get_node_schema — 某个节点类型的引脚和可设置属性 */
	static void Handle_GetNodeSchema(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/** pcg.add_node — 往图里加一个节点 */
	static void Handle_AddNode(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/** pcg.update_node — 改节点的属性 / 标题 / 注释 */
	static void Handle_UpdateNode(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/** pcg.remove_node — 删节点 */
	static void Handle_RemoveNode(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/** pcg.connect_pins — 连线 */
	static void Handle_ConnectPins(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/** pcg.disconnect_pins — 断线 */
	static void Handle_DisconnectPins(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/** pcg.set_node_positions — 批量挪节点位置（排版用） */
	static void Handle_SetNodePositions(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/** pcg.spawn_volume — 在关卡里放一个 PCGVolume 并挂上图 */
	static void Handle_SpawnVolume(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/** pcg.execute — 让某个 PCGVolume 跑一次生成，回传生成了多少东西 */
	static void Handle_Execute(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/** pcg.apply_graph — 一次调用写完整张图：节点 + 连线 + 嵌套属性，逐项回读校验 */
	static void Handle_ApplyGraph(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/** pcg.diagnose — 找出一张图为什么生成不出东西 */
	static void Handle_Diagnose(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * pcg.describe_node — 把一个节点的设置**完整**读出来，含嵌套结构和数组。
	 *
	 * 返回的键就是 update_node / apply_graph 认的路径写法，所以
	 * 「读回来 → 改两个字段 → 整图写回去」是安全的。
	 * 读不全就不敢重写 —— 实测里正是因为读不出生成器上那 12 个树种的网格路径，
	 * 只能绕路 merge，多花了好几轮。
	 */
	static void Handle_DescribeNode(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * pcg.graph_parameters — 读 / 建 / 改 / 删图的用户参数（细节面板上那几个可调项）。
	 *
	 * 一个入口把四件事都办了：不带 `parameters` / `remove` 就是纯读，带了就是改，
	 * 无论如何都全量回读整份清单。分成四条命令的话，调用方光「现在有哪些参数」
	 * 就得先问一轮。
	 *
	 * 存在的理由：用户说「这三个值我要能在细节面板里调」，UE 的官方答案就是图的
	 * User Parameters，而在这条命令之前**整套工具没有任何入口** —— Python 侧也没有
	 * （`AddUserParameters` 没有 UFUNCTION，参数包没有文本导入）。真机上只能改道去
	 * 建一个参数演员，功能成立但调参 UI 落在了错的地方。
	 *
	 * 5.5+ 限定，原因见实现处的长注释。
	 */
	static void Handle_GraphParameters(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * pcg.scene_report — 一次问清「我现在在哪、这里 PCG 能采到什么」。
	 *
	 * 当前关卡是哪张、是不是 World Partition、地形是主地形还是流送代理、
	 * 加载了几块、关卡里有哪些 PCG 体积、体积盖住了什么。
	 *
	 * 存在的理由：实测里最费时间的不是 PCG 本身，是**环境不确定性** ——
	 * 有人在不知情的情况下在一张临时的 World Partition 大世界里做实验，
	 * 得出「干净关卡也生成不出来」的错误结论，白绕很久。
	 * 没有任何一个工具能一句话回答「你现在到底在哪」，那是工具的失职。
	 */
	static void Handle_SceneReport(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

private:
	// ------------------------------------------------------------------
	// 内部助手
	// ------------------------------------------------------------------

	/** PCG 装了且启用了吗。没有的话 OutReason 里是给模型看的完整说明 */
	static bool EnsurePCGAvailable(FString& OutReason);

	/** 统一的「PCG 不可用」错误响应 */
	static bool RequirePCG(const FString& RequestId);

	static FString NormalizePath(const FString& InputPath, const FString& DefaultPrefix = TEXT("/Game/PCG"));

	/** 加载一个 PCGGraph 资产。失败时 OutError 已经是完整错误句 */
	static UObject* LoadGraph(const FString& GraphPath, FString& OutError);

	/**
	 * 这张图已经有了吗 —— 磁盘上有文件**或者**内存里已经有这个对象。
	 *
	 * 「或者」那半是重点：`FPackageName::DoesPackageExist` 只看磁盘，
	 * 会把本轮刚建、还没存盘的图判成不存在，于是新建分支把它整个盖掉。
	 * 实现处有完整的事故说明。
	 */
	static bool GraphExists(const FString& PackageName);

	/** 新建一张空的 PCGGraph 资产（自带 Input/Output 节点）。create_graph 和 apply_graph 共用 */
	static UObject* CreateGraphAsset(const FString& PackageName, FString& OutError);

	/** 按 node_id 找节点，认 "Input"/"Output" 别名 */
	static UObject* FindNodeById(UObject* Graph, const FString& NodeId);

	/** 把一个节点序列化成 JSON（类型、位置、引脚等） */
	static TSharedPtr<FJsonObject> BuildNodeJson(UObject* Node);

	/** 解析节点类型名 → UPCGSettings 子类。认全名和去掉 Settings 后缀的短名 */
	static UClass* ResolveSettingsClass(const FString& TypeName, FString& OutError);

	/** 取节点身上真正的 UPCGSettings 对象（节点存的可能是 SettingsInterface） */
	static UObject* GetNodeSettings(UObject* Node);

	/** 把 JSON 里的 properties 映射写到设置对象上，返回成功的和失败的 */
	static void ApplyProperties(
		UObject* Settings,
		const TSharedPtr<FJsonObject>& Properties,
		TArray<TSharedPtr<FJsonValue>>& OutUpdated,
		TArray<TSharedPtr<FJsonValue>>& OutFailed);

	/** 读节点的引脚标签（只列在编辑器里看得见的那些 —— 隐藏引脚不是给人连的） */
	static void CollectPinLabels(UObject* Node, bool bOutput, TArray<FString>& Out);

	/** 同上，直接给出 JSON 数组 */
	static void CollectPinLabelsJson(UObject* Node, bool bOutput, TArray<TSharedPtr<FJsonValue>>& Out);

	/**
	 * 定下一次连线要用哪个引脚。
	 *
	 * 不填引脚名时**不猜默认值**，而是看这个方向上有没有且仅有一个可见引脚 ——
	 * 有就用它，有多个就报错并列出候选。
	 *
	 * 为什么不能猜：图的 Input 节点输出引脚叫 "In"、Output 节点输入引脚叫 "Out"，
	 * 跟直觉正好相反。任何写死的默认值在这两个端点上都是错的。
	 */
	static bool ResolvePinLabel(
		UObject* Node,
		const FString& NodeId,
		const FString& GivenLabel,
		bool bWantOutput,
		FString& OutLabel,
		FString& OutError);

	/**
	 * 图里真的存在这条边吗。
	 *
	 * **连线之后必须靠它回读校验** —— UPCGGraph::AddEdge 把内部
	 * AddLabeledEdge 的返回值丢掉了，无论成败都返回 To 节点，
	 * 拿返回值判断成败等于没判断。
	 */
	static bool EdgeExists(UObject* FromNode, const FString& FromLabel, UObject* ToNode, const FString& ToLabel);

	/**
	 * 连一条边并回读确认。connect_pins 和 apply_graph 共用这一份。
	 *
	 * InOutFromPin / InOutToPin 传空表示「按节点上唯一的可见引脚自动定」，
	 * 返回时会被改写成实际用的引脚名。
	 */
	static bool ConnectEdge(
		UObject* Graph,
		UObject* FromNode,
		const FString& FromNodeId,
		UObject* ToNode,
		const FString& ToNodeId,
		FString& InOutFromPin,
		FString& InOutToPin,
		FString& OutError);

	/** 收集一张图里的全部连线 */
	static void CollectEdges(UObject* Graph, TArray<TSharedPtr<FJsonValue>>& Out);

	/** 图里的全部节点，含不在 Nodes 数组里的 Input / Output 节点 */
	static void CollectAllNodes(UObject* Graph, TArray<UObject*>& Out);
};
