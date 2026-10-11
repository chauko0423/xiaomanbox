#include "UAL_PCGCommands.h"

#include "UAL_CommandUtils.h"
#include "UAL_PropertyPath.h"
#include "UAL_ReflectCall.h"
#include "UAL_TouchedPackages.h"

#include "ActorFactories/ActorFactory.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Builders/CubeBuilder.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Volume.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "UAL_ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

/**
 * 图的用户参数要用到 `FInstancedPropertyBag`。它 **5.5 起在 CoreUObject 里**
 * （我们本来就依赖），5.2–5.4 上还在 `StructUtils` 插件里、而那个插件默认关闭 ——
 * 为它加依赖等于强制用户工程启用一个实验性插件。所以按版本收口，
 * 老版本上那条命令如实回 501 并给替代方案。详见 Handle_GraphParameters 的长注释。
 */
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
#define UAL_PCG_GRAPH_PARAMS_SUPPORTED 1
#include "StructUtils/PropertyBag.h"
#else
#define UAL_PCG_GRAPH_PARAMS_SUPPORTED 0
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUALPCG, Log, All);

namespace
{
	const TCHAR* const PCG_GRAPH_CLASS = TEXT("/Script/PCG.PCGGraph");
	const TCHAR* const PCG_SETTINGS_CLASS = TEXT("/Script/PCG.PCGSettings");
	const TCHAR* const PCG_VOLUME_CLASS = TEXT("/Script/PCG.PCGVolume");

	/** 图的输入/输出节点不在 Nodes 数组里，给它们一对好记的别名 */
	const TCHAR* const ALIAS_INPUT = TEXT("Input");
	const TCHAR* const ALIAS_OUTPUT = TEXT("Output");

	/** list_node_types 默认返回上限。PCG 有几百个节点类型，全倒给模型是纯浪费上下文 */
	constexpr int32 DEFAULT_TYPE_LIMIT = 120;

	/** get_node_schema 里每个节点最多列多少个属性 */
	constexpr int32 MAX_SCHEMA_PROPERTIES = 60;

	/** execute 默认等多久（秒） */
	constexpr double DEFAULT_EXECUTE_TIMEOUT = 30.0;

	FString NodeIdOf(const UObject* Node)
	{
		return Node ? Node->GetFName().ToString() : FString();
	}
}

// ======================================================================
// 可用性
// ======================================================================

bool FUAL_PCGCommands::EnsurePCGAvailable(FString& OutReason)
{
	if (UALReflect::FindScriptClass(PCG_GRAPH_CLASS) != nullptr)
	{
		OutReason.Reset();
		return true;
	}

	// 分清「引擎里没有」和「引擎里有但工程没开」—— 这两种情况用户要做的事完全不同
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PCG"));
	if (!Plugin.IsValid())
	{
		OutReason = FString::Printf(
			TEXT("This engine (%s) does not ship the PCG plugin. PCG was introduced in UE 5.2; ")
			TEXT("it is Beta in 5.4-5.6 and production-ready from 5.7."),
			*FEngineVersion::Current().ToString(EVersionComponent::Minor));
		return false;
	}

	OutReason = FString::Printf(
		TEXT("The PCG plugin (version %s) is installed but not enabled in this project. ")
		TEXT("Enable it in Edit > Plugins > Procedural Generation, then restart the editor."),
		*Plugin->GetDescriptor().VersionName);
	return false;
}

bool FUAL_PCGCommands::RequirePCG(const FString& RequestId)
{
	FString Reason;
	if (EnsurePCGAvailable(Reason))
	{
		return true;
	}
	// 501 而不是 400/404：这不是调用方参数写错，是这台机器上就没这个能力
	UAL_CommandUtils::SendError(RequestId, 501, Reason);
	return false;
}

// ======================================================================
// 路径 / 查找助手
// ======================================================================

FString FUAL_PCGCommands::NormalizePath(const FString& InputPath, const FString& DefaultPrefix)
{
	FString Result = InputPath.TrimStartAndEnd();

	if (Result.EndsWith(TEXT(".uasset")))
	{
		Result = Result.LeftChop(7);
	}

	Result.ReplaceInline(TEXT("\\"), TEXT("/"));
	while (Result.Contains(TEXT("//")))
	{
		Result.ReplaceInline(TEXT("//"), TEXT("/"));
	}

	if (!Result.StartsWith(TEXT("/")))
	{
		Result = DefaultPrefix / Result;
	}

	return Result;
}

UObject* FUAL_PCGCommands::LoadGraph(const FString& GraphPath, FString& OutError)
{
	UClass* GraphClass = UALReflect::FindScriptClass(PCG_GRAPH_CLASS);
	if (!GraphClass)
	{
		OutError = TEXT("PCG plugin is not available");
		return nullptr;
	}

	UObject* Loaded = LoadObject<UObject>(nullptr, *GraphPath);
	if (!Loaded)
	{
		OutError = FString::Printf(
			TEXT("PCG graph not found: %s. Create one with pcg.create_graph first."), *GraphPath);
		return nullptr;
	}

	if (!Loaded->IsA(GraphClass))
	{
		OutError = FString::Printf(
			TEXT("%s is a %s, not a PCGGraph."), *GraphPath, *Loaded->GetClass()->GetName());
		return nullptr;
	}

	OutError.Reset();
	return Loaded;
}

UObject* FUAL_PCGCommands::FindNodeById(UObject* Graph, const FString& NodeId)
{
	if (!Graph || NodeId.IsEmpty())
	{
		return nullptr;
	}

	if (NodeId.Equals(ALIAS_INPUT, ESearchCase::IgnoreCase))
	{
		return UALReflect::GetObjectProp(Graph, TEXT("InputNode"));
	}
	if (NodeId.Equals(ALIAS_OUTPUT, ESearchCase::IgnoreCase))
	{
		return UALReflect::GetObjectProp(Graph, TEXT("OutputNode"));
	}

	TArray<UObject*> Nodes;
	UALReflect::GetObjectArrayProp(Graph, TEXT("Nodes"), Nodes);
	for (UObject* Node : Nodes)
	{
		if (Node && NodeIdOf(Node).Equals(NodeId, ESearchCase::IgnoreCase))
		{
			return Node;
		}
	}

	// 输入/输出节点也允许用真名寻址，不然 get_graph 报出来的 id 有两个用不了
	for (const TCHAR* PropName : { TEXT("InputNode"), TEXT("OutputNode") })
	{
		UObject* Node = UALReflect::GetObjectProp(Graph, PropName);
		if (Node && NodeIdOf(Node).Equals(NodeId, ESearchCase::IgnoreCase))
		{
			return Node;
		}
	}

	return nullptr;
}

UObject* FUAL_PCGCommands::GetNodeSettings(UObject* Node)
{
	if (!Node)
	{
		return nullptr;
	}

	UObject* Interface = UALReflect::GetObjectProp(Node, TEXT("SettingsInterface"));
	if (!Interface)
	{
		return nullptr;
	}

	// UPCGSettings 自己就继承 UPCGSettingsInterface，所以普通节点上这个指针就是设置对象。
	// 只有「实例化设置」的节点（UPCGSettingsInstance）才需要再往里剥一层。
	UClass* SettingsClass = UALReflect::FindScriptClass(PCG_SETTINGS_CLASS);
	if (SettingsClass && Interface->IsA(SettingsClass))
	{
		return Interface;
	}

	if (UObject* Inner = UALReflect::GetObjectProp(Interface, TEXT("Settings")))
	{
		return Inner;
	}
	return Interface;
}

void FUAL_PCGCommands::CollectPinLabels(UObject* Node, bool bOutput, TArray<FString>& Out)
{
	Out.Reset();

	TArray<UObject*> Pins;
	UALReflect::GetObjectArrayProp(Node, bOutput ? TEXT("OutputPins") : TEXT("InputPins"), Pins);
	for (UObject* Pin : Pins)
	{
		if (!Pin)
		{
			continue;
		}

		// 隐藏引脚要滤掉。图的 Input 节点两侧各有一个叫 "In" 的引脚，
		// 只有输出侧那个是可见、可连的；不滤的话「这个方向只有一个引脚」的
		// 判断会失效，而那正是我们用来省掉引脚名的依据
		bool bInvisible = false;
		if (UALReflect::GetBoolInStructProp(Pin, TEXT("Properties"), TEXT("bInvisiblePin"), bInvisible) && bInvisible)
		{
			continue;
		}

		FName Label;
		if (UALReflect::GetNameInStructProp(Pin, TEXT("Properties"), TEXT("Label"), Label))
		{
			Out.Add(Label.ToString());
		}
	}
}

void FUAL_PCGCommands::CollectPinLabelsJson(UObject* Node, bool bOutput, TArray<TSharedPtr<FJsonValue>>& Out)
{
	Out.Reset();

	TArray<FString> Labels;
	CollectPinLabels(Node, bOutput, Labels);
	for (const FString& Label : Labels)
	{
		Out.Add(MakeShared<FJsonValueString>(Label));
	}
}

bool FUAL_PCGCommands::ResolvePinLabel(
	UObject* Node,
	const FString& NodeId,
	const FString& GivenLabel,
	bool bWantOutput,
	FString& OutLabel,
	FString& OutError)
{
	TArray<FString> Labels;
	CollectPinLabels(Node, bWantOutput, Labels);

	const TCHAR* const Direction = bWantOutput ? TEXT("output") : TEXT("input");

	if (!GivenLabel.IsEmpty())
	{
		for (const FString& Label : Labels)
		{
			if (Label.Equals(GivenLabel, ESearchCase::IgnoreCase))
			{
				OutLabel = Label;
				OutError.Reset();
				return true;
			}
		}
		OutError = FString::Printf(
			TEXT("Node '%s' has no %s pin named '%s'. Available: %s"),
			*NodeId, Direction, *GivenLabel,
			Labels.Num() ? *FString::Join(Labels, TEXT(", ")) : TEXT("(none)"));
		return false;
	}

	if (Labels.Num() == 1)
	{
		OutLabel = Labels[0];
		OutError.Reset();
		return true;
	}

	if (Labels.Num() == 0)
	{
		OutError = FString::Printf(TEXT("Node '%s' has no %s pins at all."), *NodeId, Direction);
		return false;
	}

	// 多个候选时不替调用方挑。挑错了 PCG 不会报错，只会安静地不生成任何东西
	OutError = FString::Printf(
		TEXT("Node '%s' has %d %s pins, so the pin name is required. Available: %s"),
		*NodeId, Labels.Num(), Direction, *FString::Join(Labels, TEXT(", ")));
	return false;
}

bool FUAL_PCGCommands::EdgeExists(UObject* FromNode, const FString& FromLabel, UObject* ToNode, const FString& ToLabel)
{
	if (!FromNode || !ToNode)
	{
		return false;
	}

	TArray<UObject*> OutPins;
	UALReflect::GetObjectArrayProp(FromNode, TEXT("OutputPins"), OutPins);

	for (UObject* Pin : OutPins)
	{
		if (!Pin)
		{
			continue;
		}
		FName Label;
		if (!UALReflect::GetNameInStructProp(Pin, TEXT("Properties"), TEXT("Label"), Label) ||
			!Label.ToString().Equals(FromLabel, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TArray<UObject*> Edges;
		UALReflect::GetObjectArrayProp(Pin, TEXT("Edges"), Edges);
		for (UObject* Edge : Edges)
		{
			if (!Edge)
			{
				continue;
			}
			// FPCGEdge 的命名反直觉：InputPin 是上游端，OutputPin 是下游端
			UObject* DownstreamPin = UALReflect::GetObjectProp(Edge, TEXT("OutputPin"));
			if (!DownstreamPin)
			{
				continue;
			}

			FName DownstreamLabel;
			UALReflect::GetNameInStructProp(DownstreamPin, TEXT("Properties"), TEXT("Label"), DownstreamLabel);
			if (!DownstreamLabel.ToString().Equals(ToLabel, ESearchCase::IgnoreCase))
			{
				continue;
			}

			// 引脚的宿主节点就是它的 Outer
			if (DownstreamPin->GetOuter() == ToNode)
			{
				return true;
			}
		}
	}

	return false;
}

void FUAL_PCGCommands::CollectAllNodes(UObject* Graph, TArray<UObject*>& Out)
{
	Out.Reset();
	UALReflect::GetObjectArrayProp(Graph, TEXT("Nodes"), Out);

	// Input / Output 节点不在 Nodes 数组里，但它们是连线的必经端点。
	// 上一版漏了这一点，导致读回来的图缺边、模型看不出到底断在哪
	for (const TCHAR* PropName : { TEXT("InputNode"), TEXT("OutputNode") })
	{
		if (UObject* Node = UALReflect::GetObjectProp(Graph, PropName))
		{
			Out.AddUnique(Node);
		}
	}

	Out.RemoveAll([](const UObject* Node) { return Node == nullptr; });
}

void FUAL_PCGCommands::CollectEdges(UObject* Graph, TArray<TSharedPtr<FJsonValue>>& Out)
{
	Out.Reset();

	TArray<UObject*> Nodes;
	CollectAllNodes(Graph, Nodes);

	// 引脚 → 所属节点 id / 标签。先建索引，省得为每条边全图搜一遍。
	// **两个方向的引脚都要进索引** —— 上一版只索引了输出引脚，
	// 于是边的下游端查不到节点名，报出来的连线是残的
	TMap<const UObject*, FString> PinToNodeId;
	TMap<const UObject*, FString> PinToLabel;

	for (UObject* Node : Nodes)
	{
		const FString NodeId = NodeIdOf(Node);
		for (const TCHAR* PinsProp : { TEXT("InputPins"), TEXT("OutputPins") })
		{
			TArray<UObject*> Pins;
			UALReflect::GetObjectArrayProp(Node, PinsProp, Pins);
			for (UObject* Pin : Pins)
			{
				if (!Pin)
				{
					continue;
				}
				PinToNodeId.Add(Pin, NodeId);
				FName Label;
				if (UALReflect::GetNameInStructProp(Pin, TEXT("Properties"), TEXT("Label"), Label))
				{
					PinToLabel.Add(Pin, Label.ToString());
				}
			}
		}
	}

	// 边对象两端的引脚各持有它一次，所以两个方向都扫、按边对象去重。
	// 只扫一个方向的话，某一端引脚没被索引到的边就整条丢了
	TSet<const UObject*> SeenEdges;

	for (UObject* Node : Nodes)
	{
		for (const TCHAR* PinsProp : { TEXT("InputPins"), TEXT("OutputPins") })
		{
			TArray<UObject*> Pins;
			UALReflect::GetObjectArrayProp(Node, PinsProp, Pins);
			for (UObject* Pin : Pins)
			{
				if (!Pin)
				{
					continue;
				}
				TArray<UObject*> Edges;
				UALReflect::GetObjectArrayProp(Pin, TEXT("Edges"), Edges);
				for (UObject* Edge : Edges)
				{
					if (!Edge || SeenEdges.Contains(Edge))
					{
						continue;
					}
					SeenEdges.Add(Edge);

					UObject* UpstreamPin = UALReflect::GetObjectProp(Edge, TEXT("InputPin"));
					UObject* DownstreamPin = UALReflect::GetObjectProp(Edge, TEXT("OutputPin"));
					if (!UpstreamPin || !DownstreamPin)
					{
						continue;
					}

					TSharedPtr<FJsonObject> EdgeJson = MakeShared<FJsonObject>();
					EdgeJson->SetStringField(TEXT("from_node"), PinToNodeId.FindRef(UpstreamPin));
					EdgeJson->SetStringField(TEXT("from_pin"), PinToLabel.FindRef(UpstreamPin));
					EdgeJson->SetStringField(TEXT("to_node"), PinToNodeId.FindRef(DownstreamPin));
					EdgeJson->SetStringField(TEXT("to_pin"), PinToLabel.FindRef(DownstreamPin));
					Out.Add(MakeShared<FJsonValueObject>(EdgeJson));
				}
			}
		}
	}
}

TSharedPtr<FJsonObject> FUAL_PCGCommands::BuildNodeJson(UObject* Node)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	if (!Node)
	{
		return Json;
	}

	Json->SetStringField(TEXT("node_id"), NodeIdOf(Node));

	UObject* Settings = GetNodeSettings(Node);
	Json->SetStringField(TEXT("type"), Settings ? Settings->GetClass()->GetName() : TEXT("Unknown"));

	FName Title;
	if (UALReflect::GetNameProp(Node, TEXT("NodeTitle"), Title) && !Title.IsNone())
	{
		Json->SetStringField(TEXT("title"), Title.ToString());
	}

	int32 X = 0, Y = 0;
	UALReflect::GetIntProp(Node, TEXT("PositionX"), X);
	UALReflect::GetIntProp(Node, TEXT("PositionY"), Y);
	Json->SetNumberField(TEXT("x"), X);
	Json->SetNumberField(TEXT("y"), Y);

	FString Comment;
	if (UALReflect::GetStringProp(Node, TEXT("NodeComment"), Comment) && !Comment.IsEmpty())
	{
		Json->SetStringField(TEXT("comment"), Comment);
	}

	TArray<TSharedPtr<FJsonValue>> InputPins;
	TArray<TSharedPtr<FJsonValue>> OutputPins;
	CollectPinLabelsJson(Node, /*bOutput=*/false, InputPins);
	CollectPinLabelsJson(Node, /*bOutput=*/true, OutputPins);
	Json->SetArrayField(TEXT("input_pins"), InputPins);
	Json->SetArrayField(TEXT("output_pins"), OutputPins);

	return Json;
}

UClass* FUAL_PCGCommands::ResolveSettingsClass(const FString& TypeName, FString& OutError)
{
	UClass* Base = UALReflect::FindScriptClass(PCG_SETTINGS_CLASS);
	if (!Base)
	{
		OutError = TEXT("PCG plugin is not available");
		return nullptr;
	}

	TArray<UClass*> Candidates;
	UALReflect::FindDerivedClasses(Base, Candidates);

	// 三段匹配：全名 → 补 Settings 后缀 → 大小写不敏感。
	// 模型十有八九会写 "SurfaceSampler" 而不是 "PCGSurfaceSamplerSettings"
	const FString WithSettings = TypeName.EndsWith(TEXT("Settings")) ? TypeName : TypeName + TEXT("Settings");
	const FString WithPrefix = TypeName.StartsWith(TEXT("PCG")) ? WithSettings : FString(TEXT("PCG")) + WithSettings;

	for (UClass* Cls : Candidates)
	{
		const FString Name = Cls->GetName();
		if (Name.Equals(TypeName, ESearchCase::IgnoreCase) ||
			Name.Equals(WithSettings, ESearchCase::IgnoreCase) ||
			Name.Equals(WithPrefix, ESearchCase::IgnoreCase))
		{
			OutError.Reset();
			return Cls;
		}
	}

	// 找不到就给几个近似的 —— 光说「不存在」等于让模型再猜一次
	TArray<FString> Names;
	Names.Reserve(Candidates.Num());
	for (UClass* Cls : Candidates)
	{
		Names.Add(Cls->GetName());
	}
	TArray<FString> Suggestions;
	UAL_CommandUtils::SuggestProperties(WithPrefix, Names, Suggestions, 5);

	OutError = FString::Printf(TEXT("Unknown PCG node type '%s'."), *TypeName);
	if (Suggestions.Num() > 0)
	{
		OutError += FString::Printf(TEXT(" Did you mean: %s? "), *FString::Join(Suggestions, TEXT(", ")));
	}
	OutError += TEXT(" Call pcg.list_node_types to see what is available.");
	return nullptr;
}

void FUAL_PCGCommands::ApplyProperties(
	UObject* Settings,
	const TSharedPtr<FJsonObject>& Properties,
	TArray<TSharedPtr<FJsonValue>>& OutUpdated,
	TArray<TSharedPtr<FJsonValue>>& OutFailed)
{
	if (!Properties.IsValid())
	{
		return;
	}

	// 没有设置对象也要逐条记失败。上一版在这里直接 return，调用方拿到的是
	// 两份空清单 —— add_node 就此回 200，给的属性一个没写、也没人说
	if (!Settings)
	{
		for (const auto& Pair : Properties->Values)
		{
			TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
			Failure->SetStringField(TEXT("name"), UAL_JsonKey(Pair.Key));
			Failure->SetStringField(TEXT("error"), TEXT("the node has no settings object to write to"));
			OutFailed.Add(MakeShared<FJsonValueObject>(Failure));
		}
		return;
	}

	for (const auto& Pair : Properties->Values)
	{
		const FString Key = UAL_JsonKey(Pair.Key);

		// 走路径写入 —— 键名可以是 `A.B[0].C`。PCG 里真正要配的东西大多埋在
		// 好几层下面（静态网格生成器的树种列表在第 4 层），只能设顶层标量的话
		// 这套工具做不成任何一件真事，只能逼调用方回去写 Python 逆向。
		FString SetError;
		if (UALPropertyPath::SetByPath(Settings, Key, Pair.Value, SetError))
		{
			// 写完立刻读回。ImportText 对个别类型会「不报错也没写进去」，
			// 只信返回值的话又是一次静默假成功
			FString ReadError;
			const TSharedPtr<FJsonValue> ReadBack = UALPropertyPath::GetByPath(Settings, Key, ReadError);

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Key);
			if (ReadBack.IsValid())
			{
				Entry->SetField(TEXT("value"), ReadBack);
			}
			OutUpdated.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
		Failure->SetStringField(TEXT("name"), Key);
		Failure->SetStringField(TEXT("error"), SetError);
		OutFailed.Add(MakeShared<FJsonValueObject>(Failure));
	}

#if WITH_EDITOR
	/**
	 * 写完必须通知一次，否则改了等于没改。
	 *
	 * PCG 会缓存每个节点的执行结果，缓存只在 `UPCGSettings::PostEditChangeProperty`
	 * 里失效。我们是绕过细节面板直接往属性内存里写的，不补这一下的话：
	 * 属性确实变了、读回来也是新值，但 pcg.execute 拿的还是旧缓存 —— 表现为
	 * 「改了密度数量纹丝不动」，而整图重建又是好的（那条路建的是新节点对象，
	 * 缓存键跟着变了），于是看起来像是属性被忽略了。
	 *
	 * 传 nullptr 属性：PCG 在拿不到属性名时按**最深**的变更类型处理，
	 * 也就是全量失效。外部盲写本来就无法判断变更粒度，宁可多重算一次 ——
	 * 少失效一次的代价是悄悄返回过期结果。
	 */
	if (OutUpdated.Num() > 0)
	{
		FPropertyChangedEvent ChangedEvent(nullptr, EPropertyChangeType::ValueSet);
		Settings->PostEditChangeProperty(ChangedEvent);
	}
#endif
}

// ======================================================================
// 命令
// ======================================================================

void FUAL_PCGCommands::Handle_Status(const TSharedPtr<FJsonObject>& /*Payload*/, const FString RequestId)
{
	FString Reason;
	const bool bAvailable = EnsurePCGAvailable(Reason);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("available"), bAvailable);
	Data->SetStringField(TEXT("engine_version"),
		FEngineVersion::Current().ToString(EVersionComponent::Minor));

	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PCG")))
	{
		Data->SetStringField(TEXT("plugin_version"), Plugin->GetDescriptor().VersionName);
		Data->SetBoolField(TEXT("plugin_enabled"), Plugin->IsEnabled());
		Data->SetBoolField(TEXT("plugin_beta"), Plugin->GetDescriptor().bIsBetaVersion);
	}

	if (!bAvailable)
	{
		Data->SetStringField(TEXT("reason"), Reason);
	}
	else
	{
		// 有多少个节点类型可用，顺手报出来 —— 这是「PCG 真的活着」最直接的证据
		TArray<UClass*> Types;
		UALReflect::FindDerivedClasses(UALReflect::FindScriptClass(PCG_SETTINGS_CLASS), Types);
		Data->SetNumberField(TEXT("node_type_count"), Types.Num());
	}

	// status 的职责是如实汇报，PCG 没启用也是一个成功的回答，不该走错误分支
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

/**
 * 这张图是不是**已经有了** —— 磁盘上有文件，或者内存里已经有这个对象。
 *
 * ## 为什么不能只问 FPackageName::DoesPackageExist
 *
 * 那个函数的文档原话是「Checks if the package exists **on disk**」。而本轮里刚
 * 新建、还没 ue_save 过的图**没有磁盘文件** —— 它只活在内存里。只问它的话，
 * 一张刚搭好的图会被判成「不存在」。
 *
 * 后果是静默毁数据：`apply_graph` 于是走进新建分支，在同一个包里 `NewObject`
 * 出一张空的 UPCGGraph，把上一次建的节点、连线和用户参数**整个盖掉**，
 * 然后接着按声明往这张空图上写。
 *
 * 实测反馈里的两条其实是同一个 bug 的两张脸：
 *   - 「merge 模式把我未声明的节点清掉了」—— 清掉它们的不是 merge，
 *     merge 那条路一个节点都不删；
 *   - 「整图重建把已建的用户参数清成 0 个」—— 同理，参数跟着旧的图对象一起没了。
 * 报告里说「分不清是 merge 还是 replace 清的」，因为两者都不是。
 *
 * 内存那半用带完整对象路径的 FindObject（`/Game/X/Y.Y`）。5.0–5.8 都支持
 * Outer 传 nullptr + 全路径，这也是 ANY_PACKAGE 弃用后的官方写法。
 */
bool FUAL_PCGCommands::GraphExists(const FString& PackageName)
{
	if (FPackageName::DoesPackageExist(PackageName))
	{
		return true;
	}
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
	return FindObject<UObject>(nullptr, *ObjectPath) != nullptr;
}

UObject* FUAL_PCGCommands::CreateGraphAsset(const FString& PackageName, FString& OutError)
{
	UClass* GraphClass = UALReflect::FindScriptClass(PCG_GRAPH_CLASS);
	if (!GraphClass)
	{
		OutError = TEXT("PCG plugin is not available");
		return nullptr;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Could not create package %s"), *PackageName);
		return nullptr;
	}

	const FString AssetName = FPackageName::GetShortName(PackageName);

	// 最后一道闸。调用方应该先用 GraphExists 判断，但**新建这一步是不可逆的**：
	// 同名 NewObject 会把旧对象改名扔掉，连同它身上的节点和用户参数。
	// 判据漏一次就永久丢一次数据，所以在真正动手的地方再挡一遍。
	if (UObject* Occupied = StaticFindObject(nullptr, Package, *AssetName))
	{
		OutError = FString::Printf(
			TEXT("%s already exists in memory as a %s (it may simply not be saved to disk yet). ")
			TEXT("Refusing to create over it - that would discard its nodes and user parameters."),
			*PackageName, *Occupied->GetClass()->GetName());
		return nullptr;
	}

	// UPCGGraph 的构造函数会自己建好 Input / Output 节点，所以裸 NewObject 就够，
	// 不需要走 factory（factory 在 PCGEditor 模块里，反射调它反而更绕）
	UObject* Graph = NewObject<UObject>(
		Package, GraphClass, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!Graph)
	{
		OutError = TEXT("NewObject failed for PCGGraph");
		return nullptr;
	}

	FAssetRegistryModule::AssetCreated(Graph);
	Package->MarkPackageDirty();

	OutError.Reset();
	return Graph;
}

void FUAL_PCGCommands::Handle_CreateGraph(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!RequirePCG(RequestId))
	{
		return;
	}

	FString Name;
	if (!Payload->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: name"));
		return;
	}

	FString FolderPath = TEXT("/Game/PCG");
	Payload->TryGetStringField(TEXT("path"), FolderPath);
	FolderPath = NormalizePath(FolderPath);

	const FString PackageName = FolderPath / Name;
	// GraphExists 而不是 DoesPackageExist：后者只看磁盘，会把本轮刚建、还没存盘的图
	// 判成「不存在」，然后 409 变成静默覆盖。见 GraphExists 的注释
	if (GraphExists(PackageName))
	{
		UAL_CommandUtils::SendError(RequestId, 409,
			FString::Printf(TEXT("An asset already exists at %s. Pick another name or edit the existing graph."),
				*PackageName));
		return;
	}

	FString CreateError;
	UObject* Graph = CreateGraphAsset(PackageName, CreateError);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 500, CreateError);
		return;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("graph_path"), PackageName);
	Data->SetStringField(TEXT("name"), Name);
	if (UObject* InputNode = UALReflect::GetObjectProp(Graph, TEXT("InputNode")))
	{
		Data->SetStringField(TEXT("input_node_id"), NodeIdOf(InputNode));
	}
	if (UObject* OutputNode = UALReflect::GetObjectProp(Graph, TEXT("OutputNode")))
	{
		Data->SetStringField(TEXT("output_node_id"), NodeIdOf(OutputNode));
	}

	UE_LOG(LogUALPCG, Log, TEXT("Created PCG graph %s"), *PackageName);
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_PCGCommands::Handle_GetGraph(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!RequirePCG(RequestId))
	{
		return;
	}

	FString GraphPath;
	if (!Payload->TryGetStringField(TEXT("graph_path"), GraphPath) || GraphPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: graph_path"));
		return;
	}
	GraphPath = NormalizePath(GraphPath);

	FString LoadError;
	UObject* Graph = LoadGraph(GraphPath, LoadError);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, LoadError);
		return;
	}

	TArray<UObject*> AllNodes;
	CollectAllNodes(Graph, AllNodes);

	TArray<TSharedPtr<FJsonValue>> NodesJson;
	for (UObject* Node : AllNodes)
	{
		NodesJson.Add(MakeShared<FJsonValueObject>(BuildNodeJson(Node)));
	}

	TArray<TSharedPtr<FJsonValue>> EdgesJson;
	CollectEdges(Graph, EdgesJson);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("graph_path"), GraphPath);
	Data->SetArrayField(TEXT("nodes"), NodesJson);
	Data->SetArrayField(TEXT("edges"), EdgesJson);
	Data->SetNumberField(TEXT("node_count"), NodesJson.Num());
	Data->SetNumberField(TEXT("edge_count"), EdgesJson.Num());

	// 把两个特殊端点的真实 id 单独报出来。它们不在 Nodes 数组里、名字是
	// DefaultInputNode / DefaultOutputNode，光看 nodes 列表分不出哪个是哪个
	if (UObject* InputNode = UALReflect::GetObjectProp(Graph, TEXT("InputNode")))
	{
		Data->SetStringField(TEXT("input_node_id"), NodeIdOf(InputNode));
	}
	if (UObject* OutputNode = UALReflect::GetObjectProp(Graph, TEXT("OutputNode")))
	{
		Data->SetStringField(TEXT("output_node_id"), NodeIdOf(OutputNode));
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_PCGCommands::Handle_ListNodeTypes(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!RequirePCG(RequestId))
	{
		return;
	}

	FString Search;
	Payload->TryGetStringField(TEXT("search"), Search);
	Search = Search.TrimStartAndEnd();

	int32 Limit = DEFAULT_TYPE_LIMIT;
	Payload->TryGetNumberField(TEXT("limit"), Limit);
	Limit = FMath::Clamp(Limit, 1, 1000);

	TArray<UClass*> Candidates;
	UALReflect::FindDerivedClasses(UALReflect::FindScriptClass(PCG_SETTINGS_CLASS), Candidates);

	TArray<UClass*> Matched;
	Matched.Reserve(Candidates.Num());
	for (UClass* Cls : Candidates)
	{
		if (Search.IsEmpty() || Cls->GetName().Contains(Search, ESearchCase::IgnoreCase))
		{
			Matched.Add(Cls);
		}
	}

	Matched.Sort([](const UClass& A, const UClass& B) { return A.GetName() < B.GetName(); });

	TArray<TSharedPtr<FJsonValue>> TypesJson;
	const int32 Shown = FMath::Min(Matched.Num(), Limit);
	for (int32 Index = 0; Index < Shown; ++Index)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("type"), Matched[Index]->GetName());
		Entry->SetStringField(TEXT("display_name"), Matched[Index]->GetDisplayNameText().ToString());
		TypesJson.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("types"), TypesJson);
	Data->SetNumberField(TEXT("shown"), Shown);
	Data->SetNumberField(TEXT("total_matched"), Matched.Num());
	if (Matched.Num() > Shown)
	{
		// 截断了就说清楚。默默截断会让模型以为「就这些」，然后断言某个能力不存在
		Data->SetStringField(TEXT("truncated_hint"),
			FString::Printf(TEXT("%d of %d shown. Narrow it down with the 'search' parameter."),
				Shown, Matched.Num()));
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_PCGCommands::Handle_GetNodeSchema(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!RequirePCG(RequestId))
	{
		return;
	}

	FString TypeName;
	if (!Payload->TryGetStringField(TEXT("node_type"), TypeName) || TypeName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: node_type"));
		return;
	}

	FString ResolveError;
	UClass* SettingsClass = ResolveSettingsClass(TypeName, ResolveError);
	if (!SettingsClass)
	{
		UAL_CommandUtils::SendError(RequestId, 404, ResolveError);
		return;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("type"), SettingsClass->GetName());
	Data->SetStringField(TEXT("display_name"), SettingsClass->GetDisplayNameText().ToString());

	// 引脚不是设置类上的属性，是节点被加进图之后才生成的。所以在一张临时图上
	// 真加一个节点来问引脚 —— 比猜可靠，而且临时图不进资产注册表、用完就被 GC。
	UClass* GraphClass = UALReflect::FindScriptClass(PCG_GRAPH_CLASS);
	UObject* ScratchGraph = NewObject<UObject>(GetTransientPackage(), GraphClass);
	if (ScratchGraph)
	{
		UALReflect::FCall Add(ScratchGraph, TEXT("AddNodeOfType"));
		Add.Cls(TEXT("InSettingsClass"), SettingsClass);
		if (Add.Invoke())
		{
			if (UObject* Node = Add.OutObject(TEXT("ReturnValue")))
			{
				TArray<TSharedPtr<FJsonValue>> InputPins;
				TArray<TSharedPtr<FJsonValue>> OutputPins;
				CollectPinLabelsJson(Node, /*bOutput=*/false, InputPins);
				CollectPinLabelsJson(Node, /*bOutput=*/true, OutputPins);
				Data->SetArrayField(TEXT("input_pins"), InputPins);
				Data->SetArrayField(TEXT("output_pins"), OutputPins);
			}
		}
	}

	// 可设置的属性从 CDO 上读，连默认值一起给 —— 模型要知道「不改会是什么样」
	TArray<TSharedPtr<FJsonValue>> PropsJson;
	const UObject* CDO = SettingsClass->GetDefaultObject();
	int32 Count = 0;
	for (TFieldIterator<FProperty> It(SettingsClass); It && Count < MAX_SCHEMA_PROPERTIES; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit) || Prop->HasAnyPropertyFlags(CPF_EditConst))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Prop->GetName());
		Entry->SetStringField(TEXT("type"), Prop->GetCPPType());
		if (CDO)
		{
			if (TSharedPtr<FJsonValue> Default = UAL_CommandUtils::PropertyToJsonValueCompat(
				Prop, Prop->ContainerPtrToValuePtr<void>(CDO)))
			{
				Entry->SetField(TEXT("default"), Default);
			}
		}
		PropsJson.Add(MakeShared<FJsonValueObject>(Entry));
		++Count;
	}
	Data->SetArrayField(TEXT("properties"), PropsJson);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_PCGCommands::Handle_AddNode(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!RequirePCG(RequestId))
	{
		return;
	}

	FString GraphPath;
	if (!Payload->TryGetStringField(TEXT("graph_path"), GraphPath) || GraphPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: graph_path"));
		return;
	}
	GraphPath = NormalizePath(GraphPath);

	FString TypeName;
	if (!Payload->TryGetStringField(TEXT("node_type"), TypeName) || TypeName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: node_type"));
		return;
	}

	FString LoadError;
	UObject* Graph = LoadGraph(GraphPath, LoadError);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, LoadError);
		return;
	}

	FString ResolveError;
	UClass* SettingsClass = ResolveSettingsClass(TypeName, ResolveError);
	if (!SettingsClass)
	{
		UAL_CommandUtils::SendError(RequestId, 404, ResolveError);
		return;
	}

	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALPCG", "AddPCGNode", "Add PCG Node"));
	Graph->Modify();

	UALReflect::FCall Add(Graph, TEXT("AddNodeOfType"));
	if (!Add.IsValid())
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 500,
			FString::Printf(TEXT("PCG API mismatch: %s"), *Add.GetError()));
		return;
	}
	Add.Cls(TEXT("InSettingsClass"), SettingsClass);
	Add.Invoke();

	UObject* Node = Add.OutObject(TEXT("ReturnValue"));
	if (!Node)
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 500,
			FString::Printf(TEXT("PCG refused to create a node of type %s"), *SettingsClass->GetName()));
		return;
	}

	int32 X = 0, Y = 0;
	if (Payload->TryGetNumberField(TEXT("x"), X))
	{
		UALReflect::SetIntProp(Node, TEXT("PositionX"), X);
	}
	if (Payload->TryGetNumberField(TEXT("y"), Y))
	{
		UALReflect::SetIntProp(Node, TEXT("PositionY"), Y);
	}

	FString Title;
	if (Payload->TryGetStringField(TEXT("title"), Title) && !Title.IsEmpty())
	{
		UALReflect::SetNameProp(Node, TEXT("NodeTitle"), FName(*Title));
	}

	TArray<TSharedPtr<FJsonValue>> Updated;
	TArray<TSharedPtr<FJsonValue>> Failed;
	TSharedPtr<FJsonObject> Properties;
	if (UAL_CommandUtils::TryGetObjectFieldFlexible(Payload, TEXT("properties"), Properties))
	{
		// DefaultNodeSettings 这个输出参数拿不到时，从节点上再取一次 ——
		// 两边都拿不到才算真没有，ApplyProperties 会逐条记成失败
		UObject* Settings = Add.OutObject(TEXT("DefaultNodeSettings"));
		if (!Settings)
		{
			Settings = GetNodeSettings(Node);
		}
		ApplyProperties(Settings, Properties, Updated, Failed);
	}

	if (Updated.Num() == 0 && Failed.Num() > 0)
	{
		/**
		 * 给的属性一个都没设上 —— 和 update_node 一样按失败处理，不回 200。
		 *
		 * 但节点已经加进图了，而 `Transaction.Cancel()` 只丢撤销记录、不回退改动
		 * （见 UAL_SequencerCommands 里那段长注释）。所以真把它删掉，再回读确认；
		 * 删不掉就在错误里说清它还在、叫什么，别让调用方以为图没动过。
		 */
		const FString CreatedId = NodeIdOf(Node);
		UALReflect::FCall Remove(Graph, TEXT("RemoveNode"));
		if (Remove.IsValid())
		{
			Remove.Obj(TEXT("InNode"), Node);
			Remove.Invoke();
		}
		TArray<UObject*> After;
		CollectAllNodes(Graph, After);
		const bool bStillThere = After.Contains(Node);

		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("failed_properties"), Failed);
		Details->SetNumberField(TEXT("failed_count"), Failed.Num());
		if (bStillThere)
		{
			Graph->MarkPackageDirty();
			Details->SetStringField(TEXT("node_id"), CreatedId);
			UAL_CommandUtils::SendError(RequestId, 400,
				FString::Printf(
					TEXT("None of the given properties could be set, and the node %s that was already added could not be ")
					TEXT("removed again - it is still in %s with default settings. Fix it with pcg.update_node or remove it ")
					TEXT("with pcg.remove_node. Call pcg.get_node_schema for the valid property names."),
					*CreatedId, *GraphPath),
				Details);
			return;
		}

		// 加了又删，图回到了原样，这时丢掉撤销记录才是对的
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 400,
			FString::Printf(
				TEXT("None of the given properties could be set, so the node was not kept (the graph is unchanged). ")
				TEXT("Call pcg.get_node_schema for the valid names of %s."),
				*SettingsClass->GetName()),
			Details);
		return;
	}

	Graph->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = BuildNodeJson(Node);
	Data->SetStringField(TEXT("graph_path"), GraphPath);
	Data->SetArrayField(TEXT("updated_properties"), Updated);
	Data->SetArrayField(TEXT("failed_properties"), Failed);
	if (Failed.Num() > 0)
	{
		Data->SetNumberField(TEXT("failed_count"), Failed.Num());
	}

	UE_LOG(LogUALPCG, Log, TEXT("Added %s to %s as %s"),
		*SettingsClass->GetName(), *GraphPath, *NodeIdOf(Node));
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_PCGCommands::Handle_UpdateNode(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!RequirePCG(RequestId))
	{
		return;
	}

	FString GraphPath, NodeId;
	if (!Payload->TryGetStringField(TEXT("graph_path"), GraphPath) || GraphPath.IsEmpty() ||
		!Payload->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required fields: graph_path, node_id"));
		return;
	}
	GraphPath = NormalizePath(GraphPath);

	FString LoadError;
	UObject* Graph = LoadGraph(GraphPath, LoadError);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, LoadError);
		return;
	}

	UObject* Node = FindNodeById(Graph, NodeId);
	if (!Node)
	{
		UAL_CommandUtils::SendError(RequestId, 404,
			FString::Printf(TEXT("No node '%s' in %s. Call pcg.get_graph for the current node ids."),
				*NodeId, *GraphPath));
		return;
	}

	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALPCG", "UpdatePCGNode", "Update PCG Node"));
	Graph->Modify();
	Node->Modify();

	// 标题 / 注释放到属性校验**之后**再写。上一版先写了它们，属性全失败时回 400
	// 「一个都没设上」，而标题其实已经改了 —— Cancel 只丢撤销记录、不回退，
	// 回执和图对不上。先过属性这关，失败出口就真的什么都没动过
	TArray<TSharedPtr<FJsonValue>> Updated;
	TArray<TSharedPtr<FJsonValue>> Failed;
	TSharedPtr<FJsonObject> Properties;
	if (UAL_CommandUtils::TryGetObjectFieldFlexible(Payload, TEXT("properties"), Properties))
	{
		UObject* Settings = GetNodeSettings(Node);
		if (!Settings)
		{
			Transaction.Cancel();
			UAL_CommandUtils::SendError(RequestId, 500,
				FString::Printf(TEXT("Node '%s' has no settings object to write to"), *NodeId));
			return;
		}
		Settings->Modify();
		ApplyProperties(Settings, Properties, Updated, Failed);
	}

	if (Updated.Num() == 0 && Failed.Num() > 0)
	{
		// 一个都没改成 —— 别声称成功，撤销掉，把失败清单原样交回去
		Transaction.Cancel();
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("failed_properties"), Failed);
		Details->SetNumberField(TEXT("failed_count"), Failed.Num());
		UAL_CommandUtils::SendError(RequestId, 400,
			FString::Printf(TEXT("None of the given properties could be set on '%s', so nothing was changed ")
				TEXT("(title / comment were not applied either). Call pcg.get_node_schema for the valid names."),
				*NodeId),
			Details);
		return;
	}

	// 标题 / 注释写失败也进 failed 清单。返回里的 title / comment 由 BuildNodeJson 回读，
	// 所以即便这里没报错，调用方看到的也是引擎里的真值
	const auto FailField = [&Failed](const TCHAR* Name, const TCHAR* Why)
	{
		TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
		Failure->SetStringField(TEXT("name"), Name);
		Failure->SetStringField(TEXT("error"), Why);
		Failed.Add(MakeShared<FJsonValueObject>(Failure));
	};

	FString Title;
	if (Payload->TryGetStringField(TEXT("title"), Title) &&
		!UALReflect::SetNameProp(Node, TEXT("NodeTitle"), FName(*Title)))
	{
		FailField(TEXT("title"), TEXT("this node has no writable NodeTitle"));
	}

	FString Comment;
	if (Payload->TryGetStringField(TEXT("comment"), Comment) &&
		!UALReflect::SetStringProp(Node, TEXT("NodeComment"), Comment))
	{
		FailField(TEXT("comment"), TEXT("this node has no writable NodeComment"));
	}

	Graph->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = BuildNodeJson(Node);
	Data->SetStringField(TEXT("graph_path"), GraphPath);
	Data->SetArrayField(TEXT("updated_properties"), Updated);
	Data->SetArrayField(TEXT("failed_properties"), Failed);
	if (Failed.Num() > 0)
	{
		Data->SetNumberField(TEXT("failed_count"), Failed.Num());
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_PCGCommands::Handle_RemoveNode(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!RequirePCG(RequestId))
	{
		return;
	}

	FString GraphPath, NodeId;
	if (!Payload->TryGetStringField(TEXT("graph_path"), GraphPath) || GraphPath.IsEmpty() ||
		!Payload->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required fields: graph_path, node_id"));
		return;
	}
	GraphPath = NormalizePath(GraphPath);

	FString LoadError;
	UObject* Graph = LoadGraph(GraphPath, LoadError);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, LoadError);
		return;
	}

	if (NodeId.Equals(ALIAS_INPUT, ESearchCase::IgnoreCase) ||
		NodeId.Equals(ALIAS_OUTPUT, ESearchCase::IgnoreCase))
	{
		UAL_CommandUtils::SendError(RequestId, 400,
			TEXT("The graph Input and Output nodes cannot be removed."));
		return;
	}

	UObject* Node = FindNodeById(Graph, NodeId);
	if (!Node)
	{
		UAL_CommandUtils::SendError(RequestId, 404,
			FString::Printf(TEXT("No node '%s' in %s"), *NodeId, *GraphPath));
		return;
	}

	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALPCG", "RemovePCGNode", "Remove PCG Node"));
	Graph->Modify();

	UALReflect::FCall Remove(Graph, TEXT("RemoveNode"));
	if (!Remove.IsValid())
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 500,
			FString::Printf(TEXT("PCG API mismatch: %s"), *Remove.GetError()));
		return;
	}
	const FString RemovedId = NodeIdOf(Node);
	Remove.Obj(TEXT("InNode"), Node);
	Remove.Invoke();

	// **回读确认它真的不在了。** Invoke 只说明函数调到了，不说明节点被摘掉 ——
	// apply_graph 的 replace 清图就实测过「调了 RemoveNode、节点还在」。
	// 按指针比，不走 FindNodeById：那条路还会认 Input / Output 别名，这里不需要
	TArray<UObject*> After;
	CollectAllNodes(Graph, After);
	if (After.Contains(Node))
	{
		// 不 Cancel：RemoveNode 可能已经断掉了一部分连线，Cancel 不回退改动，
		// 只会让这些改动连撤销都撤不回来
		Graph->MarkPackageDirty();
		UAL_CommandUtils::SendError(RequestId, 500,
			FString::Printf(
				TEXT("RemoveNode ran but '%s' is still in %s. Its links may have been partly cut - ")
				TEXT("call pcg.get_graph to see the current state."),
				*RemovedId, *GraphPath));
		return;
	}

	Graph->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("graph_path"), GraphPath);
	Data->SetStringField(TEXT("removed_node_id"), RemovedId);
	Data->SetNumberField(TEXT("node_count_after"), After.Num());

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

namespace
{
	/** connect / disconnect 只差调用的函数名和形参名，参数解析完全一样 */
	struct FPinLinkRequest
	{
		FString GraphPath;
		FString FromNode;
		FString FromPin;
		FString ToNode;
		FString ToPin;
	};

	bool ParsePinLinkRequest(const TSharedPtr<FJsonObject>& Payload, FPinLinkRequest& Out, FString& OutError)
	{
		const TCHAR* const Required[] = { TEXT("graph_path"), TEXT("from_node"), TEXT("to_node") };
		for (const TCHAR* Field : Required)
		{
			FString Value;
			if (!Payload->TryGetStringField(Field, Value) || Value.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Missing required field: %s"), Field);
				return false;
			}
		}

		Payload->TryGetStringField(TEXT("graph_path"), Out.GraphPath);
		Payload->TryGetStringField(TEXT("from_node"), Out.FromNode);
		Payload->TryGetStringField(TEXT("to_node"), Out.ToNode);

		// 引脚名留空，交给 ResolvePinLabel 按节点实际的可见引脚去定。
		//
		// 这里**曾经**写死默认值 Out → In，那是个会静默毁掉整张图的错：
		// 图的 Input 节点输出引脚叫 "In"，Output 节点输入引脚叫 "Out"，
		// 跟直觉正好相反，于是最常见的两条边（Input→X、X→Output）都连不上。
		// 而 UPCGGraph::AddEdge 连不上也照样返回 To 节点，两个问题叠一起，
		// 结果就是「每一步都报成功、最后生成 0 个实例」。
		Payload->TryGetStringField(TEXT("from_pin"), Out.FromPin);
		Payload->TryGetStringField(TEXT("to_pin"), Out.ToPin);
		return true;
	}
}

void FUAL_PCGCommands::Handle_ConnectPins(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!RequirePCG(RequestId))
	{
		return;
	}

	FPinLinkRequest Request;
	FString ParseError;
	if (!ParsePinLinkRequest(Payload, Request, ParseError))
	{
		UAL_CommandUtils::SendError(RequestId, 400, ParseError);
		return;
	}
	Request.GraphPath = NormalizePath(Request.GraphPath);

	FString LoadError;
	UObject* Graph = LoadGraph(Request.GraphPath, LoadError);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, LoadError);
		return;
	}

	UObject* FromNode = FindNodeById(Graph, Request.FromNode);
	UObject* ToNode = FindNodeById(Graph, Request.ToNode);
	if (!FromNode || !ToNode)
	{
		UAL_CommandUtils::SendError(RequestId, 404,
			FString::Printf(TEXT("Node not found: %s. Call pcg.get_graph for the current node ids."),
				!FromNode ? *Request.FromNode : *Request.ToNode));
		return;
	}

	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALPCG", "ConnectPCGPins", "Connect PCG Pins"));
	Graph->Modify();

	FString ConnectError;
	if (!ConnectEdge(Graph, FromNode, Request.FromNode, ToNode, Request.ToNode,
			Request.FromPin, Request.ToPin, ConnectError))
	{
		Transaction.Cancel();

		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> FromPins, ToPins;
		CollectPinLabelsJson(FromNode, /*bOutput=*/true, FromPins);
		CollectPinLabelsJson(ToNode, /*bOutput=*/false, ToPins);
		Details->SetArrayField(TEXT("from_node_output_pins"), FromPins);
		Details->SetArrayField(TEXT("to_node_input_pins"), ToPins);

		UAL_CommandUtils::SendError(RequestId, 400, ConnectError, Details);
		return;
	}

	Graph->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("graph_path"), Request.GraphPath);
	Data->SetStringField(TEXT("from_node"), Request.FromNode);
	Data->SetStringField(TEXT("from_pin"), Request.FromPin);
	Data->SetStringField(TEXT("to_node"), Request.ToNode);
	Data->SetStringField(TEXT("to_pin"), Request.ToPin);
	// 回读确认过才敢说成功，这里把实际用到的引脚名报出来 ——
	// 调用方多半没填，让它知道系统替它选了哪个
	Data->SetBoolField(TEXT("verified"), true);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

bool FUAL_PCGCommands::ConnectEdge(
	UObject* Graph,
	UObject* FromNode,
	const FString& FromNodeId,
	UObject* ToNode,
	const FString& ToNodeId,
	FString& InOutFromPin,
	FString& InOutToPin,
	FString& OutError)
{
	FString FromLabel;
	if (!ResolvePinLabel(FromNode, FromNodeId, InOutFromPin, /*bWantOutput=*/true, FromLabel, OutError))
	{
		return false;
	}

	FString ToLabel;
	if (!ResolvePinLabel(ToNode, ToNodeId, InOutToPin, /*bWantOutput=*/false, ToLabel, OutError))
	{
		return false;
	}

	InOutFromPin = FromLabel;
	InOutToPin = ToLabel;

	if (EdgeExists(FromNode, FromLabel, ToNode, ToLabel))
	{
		// 已经连着不算失败 ——「确保这里是通的」是个合法诉求，
		// 而且重跑一次 apply_graph 不该因为图已经对了就报错
		OutError.Reset();
		return true;
	}

	UALReflect::FCall Connect(Graph, TEXT("AddEdge"));
	if (!Connect.IsValid())
	{
		OutError = FString::Printf(TEXT("PCG API mismatch: %s"), *Connect.GetError());
		return false;
	}
	Connect.Obj(TEXT("From"), FromNode)
		.Name(TEXT("FromPinLabel"), FName(*FromLabel))
		.Obj(TEXT("To"), ToNode)
		.Name(TEXT("ToPinLabel"), FName(*ToLabel));
	Connect.Invoke();

	// **回读校验，不看返回值。**
	// UPCGGraph::AddEdge 的实现是：调 AddLabeledEdge、把它的 bool 丢掉、
	// 无条件 `return To;`。拿返回值判断成败等于没判断 —— 这正是之前
	// 「每一步都成功、最后 0 个实例」的根因
	if (!EdgeExists(FromNode, FromLabel, ToNode, ToLabel))
	{
		OutError = FString::Printf(
			TEXT("PCG refused the edge %s.%s -> %s.%s (verified by reading the graph back). ")
			TEXT("The pin names exist, so the two pins' data types are incompatible."),
			*FromNodeId, *FromLabel, *ToNodeId, *ToLabel);
		return false;
	}

	OutError.Reset();
	return true;
}

void FUAL_PCGCommands::Handle_DisconnectPins(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!RequirePCG(RequestId))
	{
		return;
	}

	FPinLinkRequest Request;
	FString ParseError;
	if (!ParsePinLinkRequest(Payload, Request, ParseError))
	{
		UAL_CommandUtils::SendError(RequestId, 400, ParseError);
		return;
	}
	Request.GraphPath = NormalizePath(Request.GraphPath);

	FString LoadError;
	UObject* Graph = LoadGraph(Request.GraphPath, LoadError);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, LoadError);
		return;
	}

	UObject* FromNode = FindNodeById(Graph, Request.FromNode);
	UObject* ToNode = FindNodeById(Graph, Request.ToNode);
	if (!FromNode || !ToNode)
	{
		UAL_CommandUtils::SendError(RequestId, 404,
			FString::Printf(TEXT("Node not found: %s"), !FromNode ? *Request.FromNode : *Request.ToNode));
		return;
	}

	FString FromLabel, ToLabel, ResolveError;
	if (!ResolvePinLabel(FromNode, Request.FromNode, Request.FromPin, /*bWantOutput=*/true, FromLabel, ResolveError) ||
		!ResolvePinLabel(ToNode, Request.ToNode, Request.ToPin, /*bWantOutput=*/false, ToLabel, ResolveError))
	{
		UAL_CommandUtils::SendError(RequestId, 400, ResolveError);
		return;
	}

	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALPCG", "DisconnectPCGPins", "Disconnect PCG Pins"));
	Graph->Modify();

	// 注意形参名：RemoveEdge 用的是 FromLabel / ToLabel，跟 AddEdge 的
	// FromPinLabel / ToPinLabel 不一样。反射按名字找，写错会静默传空名
	UALReflect::FCall Disconnect(Graph, TEXT("RemoveEdge"));
	if (!Disconnect.IsValid())
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 500,
			FString::Printf(TEXT("PCG API mismatch: %s"), *Disconnect.GetError()));
		return;
	}
	Disconnect.Obj(TEXT("From"), FromNode)
		.Name(TEXT("FromLabel"), FName(*FromLabel))
		.Obj(TEXT("To"), ToNode)
		.Name(TEXT("ToLabel"), FName(*ToLabel));
	Disconnect.Invoke();

	// RemoveEdge 的返回值语义可信（AddEdge 不可信），但仍然回读一次 ——
	// 两条路径用同一套判据，读者不用记「哪个能信哪个不能信」
	if (EdgeExists(FromNode, FromLabel, ToNode, ToLabel))
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 500,
			FString::Printf(TEXT("The edge %s.%s -> %s.%s is still present after RemoveEdge."),
				*Request.FromNode, *FromLabel, *Request.ToNode, *ToLabel));
		return;
	}

	Graph->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("graph_path"), Request.GraphPath);
	Data->SetStringField(TEXT("from_pin"), FromLabel);
	Data->SetStringField(TEXT("to_pin"), ToLabel);
	Data->SetBoolField(TEXT("removed"), Disconnect.OutBool(TEXT("ReturnValue"), true));

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_PCGCommands::Handle_SetNodePositions(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!RequirePCG(RequestId))
	{
		return;
	}

	FString GraphPath;
	if (!Payload->TryGetStringField(TEXT("graph_path"), GraphPath) || GraphPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: graph_path"));
		return;
	}
	GraphPath = NormalizePath(GraphPath);

	const TArray<TSharedPtr<FJsonValue>>* Positions = nullptr;
	if (!Payload->TryGetArrayField(TEXT("positions"), Positions) || !Positions || Positions->Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing or empty required field: positions"));
		return;
	}

	FString LoadError;
	UObject* Graph = LoadGraph(GraphPath, LoadError);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, LoadError);
		return;
	}

	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALPCG", "LayoutPCGGraph", "Layout PCG Graph"));
	Graph->Modify();

	int32 Moved = 0;
	TArray<TSharedPtr<FJsonValue>> NotFound;
	// 格式不对的条目和写不进去的节点也要单列。上一版这两种都是静默 continue /
	// 不看 SetIntProp 返回值就 ++Moved，于是「重排 5/5」里可能有几个根本没动
	TArray<TSharedPtr<FJsonValue>> Skipped;
	TArray<TSharedPtr<FJsonValue>> Failed;
	bool bWroteAnything = false;

	const auto Record = [](TArray<TSharedPtr<FJsonValue>>& List, const FString& Item, const FString& Why)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("item"), Item);
		Entry->SetStringField(TEXT("error"), Why);
		List.Add(MakeShared<FJsonValueObject>(Entry));
	};

	for (int32 Index = 0; Index < Positions->Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& Value = (*Positions)[Index];
		const FString Where = FString::Printf(TEXT("positions[%d]"), Index);

		const TSharedPtr<FJsonObject>* Entry = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Entry) || !Entry || !(*Entry).IsValid())
		{
			Record(Skipped, Where, TEXT("entry is not an object"));
			continue;
		}

		FString NodeId;
		if (!(*Entry)->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
		{
			Record(Skipped, Where, TEXT("entry has no 'node_id'"));
			continue;
		}

		UObject* Node = FindNodeById(Graph, NodeId);
		if (!Node)
		{
			// 挪不到的单列出来。调用方多半拿着一份过期的图在算坐标，
			// 「成功挪了 3 个」会把真正的问题盖掉
			NotFound.Add(MakeShared<FJsonValueString>(NodeId));
			continue;
		}

		int32 X = 0, Y = 0;
		UALReflect::GetIntProp(Node, TEXT("PositionX"), X);
		UALReflect::GetIntProp(Node, TEXT("PositionY"), Y);
		(*Entry)->TryGetNumberField(TEXT("x"), X);
		(*Entry)->TryGetNumberField(TEXT("y"), Y);

		Node->Modify();
		bWroteAnything = true;
		const bool bSetX = UALReflect::SetIntProp(Node, TEXT("PositionX"), X);
		const bool bSetY = UALReflect::SetIntProp(Node, TEXT("PositionY"), Y);

		// 只数回读对得上的。返回 true 也回读一次，和属性写入同一个道理
		int32 ReadX = 0, ReadY = 0;
		const bool bReadBack = UALReflect::GetIntProp(Node, TEXT("PositionX"), ReadX) &&
			UALReflect::GetIntProp(Node, TEXT("PositionY"), ReadY);
		if (!bSetX || !bSetY || !bReadBack || ReadX != X || ReadY != Y)
		{
			Record(Failed, NodeId, FString::Printf(
				TEXT("position did not stick: asked (%d, %d), node reads (%d, %d)"), X, Y, ReadX, ReadY));
			continue;
		}
		++Moved;
	}

	const int32 FailedCount = NotFound.Num() + Failed.Num() + Skipped.Num();

	if (Moved == 0)
	{
		// 一个写都没发生时撤销记录是空的，丢掉无妨；写过（哪怕回读对不上）就留着，
		// Cancel 不回退改动，只会让它连撤销都撤不了
		if (!bWroteAnything)
		{
			Transaction.Cancel();
		}
		else
		{
			Graph->MarkPackageDirty();
		}
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("not_found"), NotFound);
		Details->SetArrayField(TEXT("failed"), Failed);
		Details->SetArrayField(TEXT("skipped"), Skipped);
		Details->SetNumberField(TEXT("failed_count"), FailedCount);
		UAL_CommandUtils::SendError(RequestId, NotFound.Num() == FailedCount ? 404 : 400,
			NotFound.Num() == FailedCount
				? TEXT("None of the given node_ids exist in this graph. Call pcg.get_graph for the current ids.")
				: TEXT("No node was moved. See not_found / failed / skipped for why."),
			Details);
		return;
	}

	Graph->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("graph_path"), GraphPath);
	Data->SetNumberField(TEXT("moved"), Moved);
	Data->SetArrayField(TEXT("not_found"), NotFound);
	if (FailedCount > 0)
	{
		// failed_count 是所有没挪成的：not_found + failed + skipped。skipped_count 单拎出来，
		// TS 那边的第一句要把「格式不对被跳过」和「没挪成」分开说
		Data->SetArrayField(TEXT("failed"), Failed);
		Data->SetArrayField(TEXT("skipped"), Skipped);
		Data->SetNumberField(TEXT("failed_count"), FailedCount);
		Data->SetNumberField(TEXT("skipped_count"), Skipped.Num());
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_PCGCommands::Handle_SpawnVolume(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!RequirePCG(RequestId))
	{
		return;
	}

	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("No editor world is open."));
		return;
	}

	UClass* VolumeClass = UALReflect::FindScriptClass(PCG_VOLUME_CLASS);
	if (!VolumeClass)
	{
		UAL_CommandUtils::SendError(RequestId, 501, TEXT("PCGVolume class not found - is the PCG plugin enabled?"));
		return;
	}

	const FVector Location = UAL_CommandUtils::ReadVector(Payload, TEXT("location"), FVector::ZeroVector);
	const FVector Size = UAL_CommandUtils::ReadVector(Payload, TEXT("size"), FVector(2000.0, 2000.0, 2000.0));

	FString Label;
	Payload->TryGetStringField(TEXT("label"), Label);

	// 图先加载再放体积。反过来的话，图路径写错就得「先放下去再销毁」，
	// 而在已回滚的事务上再 DestroyActor 是自找麻烦
	FString GraphPath;
	UObject* Graph = nullptr;
	if (Payload->TryGetStringField(TEXT("graph_path"), GraphPath) && !GraphPath.IsEmpty())
	{
		GraphPath = NormalizePath(GraphPath);
		FString LoadError;
		Graph = LoadGraph(GraphPath, LoadError);
		if (!Graph)
		{
			UAL_CommandUtils::SendError(RequestId, 404, LoadError);
			return;
		}
	}

	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALPCG", "SpawnPCGVolume", "Spawn PCG Volume"));

	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags = RF_Transactional;
	AActor* Actor = World->SpawnActor<AActor>(VolumeClass, FTransform(Location), SpawnParams);
	if (!Actor)
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("SpawnActor failed for PCGVolume"));
		return;
	}

	// 裸 SpawnActor 出来的 Volume 没有画刷几何体 —— 在编辑器里是个看不见也框不住
	// 任何东西的空壳。用引擎自己给 Volume 工厂用的那个 helper 把方盒子建出来。
	// APCGVolume 继承 AVolume（Engine 模块），这一段不需要 PCG 的类型
	//
	// UActorFactory::CreateBrushForVolumeActor 是 **5.1** 才加的公开静态函数。
	// 逐版本查过引擎头文件确认，不要凭印象改这个数字。
	// 5.0 上走不到这里 —— 那个版本根本没有 PCG 插件，前面的 RequirePCG 就挡住了；
	// 这个分支存在只是为了让代码在 5.0 上**编得过**
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	if (AVolume* Volume = Cast<AVolume>(Actor))
	{
		UCubeBuilder* Builder = NewObject<UCubeBuilder>();
		Builder->X = Size.X;
		Builder->Y = Size.Y;
		Builder->Z = Size.Z;
		UActorFactory::CreateBrushForVolumeActor(Volume, Builder);
	}
#endif

	if (!Label.IsEmpty())
	{
		Actor->SetActorLabel(Label);
	}

	/**
	 * 挂图失败时把刚生成的体积拆掉再报错。
	 *
	 * 上一版在这里 `Transaction.Cancel()` 然后回 500 —— 但 Cancel 只丢撤销记录、
	 * 不回退改动，体积还留在关卡里，而且连 Ctrl+Z 都撤不掉。调用方看到 500 会再调一次，
	 * 关卡里就多出一个没挂图的野体积。
	 * 拆掉之后回读确认；拆不掉就在错误里给出它的路径，让调用方知道它还在。
	 */
	const auto AbortAndCleanUp = [&](const FString& Why)
	{
		const FString ActorPath = Actor->GetPathName();
		const FString ActorLabelNow = Actor->GetActorLabel();
		bool bDestroyed = false;
#if WITH_EDITOR
		bDestroyed = World->EditorDestroyActor(Actor, true);
#else
		bDestroyed = Actor->Destroy();
#endif
		// 返回值之外再看一眼对象状态。IsValid 在 5.0 起对「待销毁」一律给 false
		bDestroyed = bDestroyed || !IsValid(Actor);

		if (bDestroyed)
		{
			// 生成又销毁，关卡回到原样，这时丢掉撤销记录才是对的
			Transaction.Cancel();
			UAL_CommandUtils::SendError(RequestId, 500,
				Why + TEXT(" The volume that had been spawned was removed again; the level is unchanged."));
			return;
		}

		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("actor_path"), ActorPath);
		Details->SetStringField(TEXT("actor_label"), ActorLabelNow);
		UAL_CommandUtils::SendError(RequestId, 500,
			FString::Printf(
				TEXT("%s The volume '%s' was already spawned and could not be removed - it is still in the level ")
				TEXT("without a graph. Delete it or assign a graph to it by hand."),
				*Why, *ActorLabelNow),
			Details);
	};

	bool bGraphAssigned = false;
	bool bGraphReadBackUnavailable = false;
	if (Graph)
	{
		UObject* Component = UALReflect::GetObjectProp(Actor, TEXT("PCGComponent"));
		if (!Component)
		{
			AbortAndCleanUp(TEXT("Spawned PCGVolume has no PCGComponent."));
			return;
		}

		UALReflect::FCall SetGraph(Component, TEXT("SetGraph"));
		if (!SetGraph.IsValid())
		{
			AbortAndCleanUp(FString::Printf(TEXT("PCG API mismatch: %s."), *SetGraph.GetError()));
			return;
		}
		SetGraph.Obj(TEXT("InGraph"), Graph);
		SetGraph.Invoke();

		// **回读组件上真正挂着的图。** Invoke 只说明调到了。
		// 走 GraphInstance → Graph 这条 UPROPERTY 链而不是 GetGraph()，理由见
		// Handle_SceneReport：GetGraph 在 5.5 上不是 UFUNCTION，反射找不到
		//
		// 这条属性链不存在（某个版本改了名）时是「读不回来」，不是「没挂上」——
		// 为这个拆掉体积就是把一次成功报成失败。如实标 unverified 交给调用方
		if (!Component->GetClass()->FindPropertyByName(TEXT("GraphInstance")))
		{
			bGraphReadBackUnavailable = true;
		}
		UObject* Instance = UALReflect::GetObjectProp(Component, TEXT("GraphInstance"));
		UObject* AssignedGraph = Instance ? UALReflect::GetObjectProp(Instance, TEXT("Graph")) : nullptr;
		if (!bGraphReadBackUnavailable && AssignedGraph != Graph)
		{
			AbortAndCleanUp(FString::Printf(
				TEXT("SetGraph ran but the component's graph reads back as %s instead of %s."),
				AssignedGraph ? *AssignedGraph->GetPathName() : TEXT("(none)"),
				*Graph->GetPathName()));
			return;
		}
		bGraphAssigned = true;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	Data->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Data->SetObjectField(TEXT("location"), UAL_CommandUtils::MakeVectorJson(Location));
	Data->SetObjectField(TEXT("size"), UAL_CommandUtils::MakeVectorJson(Size));

	// 回读真实包围盒。actor 的位置不等于采样盒的中心（画刷有自己的枢轴），
	// 只报请求值的话，体积其实没盖住地形时看起来一切正常，
	// 而表现是「采到几个点」而不是报错 —— 实测里为此排查过一整轮
	const FBox Bounds = Actor->GetComponentsBoundingBox();
	TSharedPtr<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
	BoundsJson->SetObjectField(TEXT("center"), UAL_CommandUtils::MakeVectorJson(Bounds.GetCenter()));
	BoundsJson->SetObjectField(TEXT("extent"), UAL_CommandUtils::MakeVectorJson(Bounds.GetExtent()));
	BoundsJson->SetObjectField(TEXT("min"), UAL_CommandUtils::MakeVectorJson(Bounds.Min));
	BoundsJson->SetObjectField(TEXT("max"), UAL_CommandUtils::MakeVectorJson(Bounds.Max));
	Data->SetObjectField(TEXT("actual_bounds"), BoundsJson);
	Data->SetBoolField(TEXT("graph_assigned"), bGraphAssigned);
	if (bGraphAssigned)
	{
		Data->SetStringField(TEXT("graph_path"), GraphPath);
	}
	if (bGraphReadBackUnavailable)
	{
		Data->SetBoolField(TEXT("graph_assigned_unverified"), true);
	}

	UE_LOG(LogUALPCG, Log, TEXT("Spawned PCG volume %s"), *Actor->GetActorLabel());
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

namespace
{
	/**
	 * 生成期间把引擎自己的 PCG 日志抓下来。
	 *
	 * 「跑完了但产出是 0」这类问题，图结构、连线、属性全都能在我们这边验证，
	 * 唯独**执行链路里发生了什么**看不见 —— 只能让人去翻 LogPCG，而那要求
	 * 他知道该翻什么。引擎自己每次执行都在往日志里写原因，直接抓回来就是了。
	 *
	 * 只收 PCG 相关分类，且只收 Warning 及以上加少量 Display —— 全量抓会把
	 * 上下文淹掉，而淹掉的结果等于没抓。
	 */
	class FPCGLogCapture : public FOutputDevice
	{
	public:
		FPCGLogCapture()
		{
			if (GLog)
			{
				GLog->AddOutputDevice(this);
			}
		}

		virtual ~FPCGLogCapture() override
		{
			if (GLog)
			{
				GLog->RemoveOutputDevice(this);
			}
		}

		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Lines.Num() >= MaxLines || !Message)
			{
				return;
			}

			const FString CategoryName = Category.ToString();
			if (!CategoryName.StartsWith(TEXT("LogPCG")))
			{
				return;
			}
			if (Verbosity > ELogVerbosity::Display)
			{
				return;
			}

			const TCHAR* Level =
				Verbosity <= ELogVerbosity::Error ? TEXT("error") :
				Verbosity == ELogVerbosity::Warning ? TEXT("warning") : TEXT("info");

			Lines.Add(FString::Printf(TEXT("[%s] %s: %s"), Level, *CategoryName, Message));
		}

		/** 日志线程也会调 Serialize，声明成线程安全免得引擎替我们加锁转发 */
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		TArray<TSharedPtr<FJsonValue>> ToJson() const
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const FString& Line : Lines)
			{
				Out.Add(MakeShared<FJsonValueString>(Line));
			}
			return Out;
		}

		bool HasAny() const { return Lines.Num() > 0; }

	private:
		/** 够看清一次执行发生了什么，又不至于把响应撑爆 */
		static constexpr int32 MaxLines = 80;
		TArray<FString> Lines;
	};

	/**
	 * 分区生成的产物不在体积上，而在 PCGPartitionActor 上。
	 *
	 * 组件勾了 Is Partitioned 之后，PCG 会按网格给每个单元建一个
	 * APCGPartitionActor，生成出来的 ISM 挂在那些 actor 上 —— 只数体积自己的话
	 * 永远是 0，看起来和「什么都没生成」一模一样。
	 *
	 * 这里数的是关卡里**所有**分区 actor 的总量（分不出是哪个体积的：
	 * 那个映射表是 private TMap，反射读它性价比太低），所以单独报一个数字，
	 * 并在文案里说清它的口径。
	 */
	int32 CountPartitionedInstances(UWorld* World)
	{
		UClass* PartitionClass = UALReflect::FindScriptClass(TEXT("/Script/PCG.PCGPartitionActor"));
		if (!World || !PartitionClass)
		{
			return 0;
		}

		int32 Total = 0;
		for (TActorIterator<AActor> It(World, PartitionClass); It; ++It)
		{
			TArray<UInstancedStaticMeshComponent*> Components;
			It->GetComponents(Components);
			for (UInstancedStaticMeshComponent* Component : Components)
			{
				if (Component)
				{
					Total += Component->GetInstanceCount();
				}
			}
		}
		return Total;
	}

	/** 统计一个 Actor 上的 ISM 组件和实例总数 —— 「图到底生成出东西了没有」的直接证据 */
	void CountGeneratedInstances(AActor* Actor, int32& OutComponents, int32& OutInstances,
		TArray<TSharedPtr<FJsonValue>>& OutBreakdown)
	{
		OutComponents = 0;
		OutInstances = 0;
		OutBreakdown.Reset();

		if (!Actor)
		{
			return;
		}

		TArray<UInstancedStaticMeshComponent*> Components;
		Actor->GetComponents(Components);
		for (UInstancedStaticMeshComponent* Component : Components)
		{
			if (!Component)
			{
				continue;
			}
			const int32 Instances = Component->GetInstanceCount();
			++OutComponents;
			OutInstances += Instances;

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("component"), Component->GetName());
			Entry->SetStringField(TEXT("mesh"),
				Component->GetStaticMesh() ? Component->GetStaticMesh()->GetName() : TEXT("(none)"));
			Entry->SetNumberField(TEXT("instances"), Instances);
			OutBreakdown.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}
}

void FUAL_PCGCommands::Handle_Execute(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!RequirePCG(RequestId))
	{
		return;
	}

	FString ActorLabel;
	if (!Payload->TryGetStringField(TEXT("actor_label"), ActorLabel) || ActorLabel.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: actor_label"));
		return;
	}

	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("No editor world is open."));
		return;
	}

	AActor* Actor = UAL_CommandUtils::FindActorByLabel(World, ActorLabel);
	if (!Actor)
	{
		UAL_CommandUtils::SendError(RequestId, 404,
			FString::Printf(TEXT("No actor labelled '%s' in the current level."), *ActorLabel));
		return;
	}

	UObject* Component = UALReflect::GetObjectProp(Actor, TEXT("PCGComponent"));
	if (!Component)
	{
		UAL_CommandUtils::SendError(RequestId, 400,
			FString::Printf(TEXT("Actor '%s' is not a PCG volume (no PCGComponent)."), *ActorLabel));
		return;
	}

	double TimeoutSeconds = DEFAULT_EXECUTE_TIMEOUT;
	Payload->TryGetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
	TimeoutSeconds = FMath::Clamp(TimeoutSeconds, 1.0, 300.0);

	// 两个入口先探一遍再动手（verbose 那段会改节点设置）。
	// 清理入口缺了就不跑：不先清掉上一轮，轮询时读到的 bGenerated 可能是上一轮的 true，
	// 于是会把「上次的残留」报成「这次生成完成」—— 宁可直说跑不了
	{
		UALReflect::FCall ProbeCleanup(Component, TEXT("CleanupLocal"));
		UALReflect::FCall ProbeGenerate(Component, TEXT("GenerateLocal"));
		if (!ProbeCleanup.IsValid() || !ProbeGenerate.IsValid())
		{
			UAL_CommandUtils::SendError(RequestId, 500,
				FString::Printf(
					TEXT("PCG API mismatch: %s. Nothing was generated - without clearing the previous result first, ")
					TEXT("a finished run cannot be told apart from the last run's leftovers."),
					!ProbeCleanup.IsValid() ? *ProbeCleanup.GetError() : *ProbeGenerate.GetError()));
			return;
		}
	}

	// 日志抓取要在下发生成**之前**挂上，跨整个异步过程活着
	TSharedPtr<FPCGLogCapture> LogCapture = MakeShared<FPCGLogCapture>();

	// 可选：把图里每个节点的 bDumpDataDescriptions 打开，让引擎把各引脚
	// 实际收发了多少数据打进日志。这是判断「点到底走到生成器没有」的唯一直接证据 ——
	// 默认不开，因为它很吵；查 0 实例的时候再开
	bool bVerbose = false;
	Payload->TryGetBoolField(TEXT("verbose"), bVerbose);
	if (bVerbose)
	{
		if (UObject* Graph = UALReflect::GetObjectProp(Component, TEXT("GraphInstance")))
		{
			UObject* Underlying = UALReflect::GetObjectProp(Graph, TEXT("Graph"));
			TArray<UObject*> Nodes;
			CollectAllNodes(Underlying ? Underlying : Graph, Nodes);
			for (UObject* Node : Nodes)
			{
				if (UObject* Settings = GetNodeSettings(Node))
				{
					FString Ignored;
					UALPropertyPath::SetByPath(
						Settings, TEXT("bDumpDataDescriptions"), MakeShared<FJsonValueBoolean>(true), Ignored);
				}
			}
		}
	}

	/**
	 * 先 Cleanup 再 Generate，而且**等 bGenerated 真的落回 false 才下发 Generate**。
	 *
	 * 不清一遍的话 bGenerated 可能上一轮就是 true，我们就没法分辨「这次跑完了」
	 * 和「上次的残留」。上一版清了就立刻 Generate：CleanupLocal 是排进 PCG 调度器的
	 * 异步任务，调用返回时 bGenerated 往往还是上一轮的 true，第一次轮询就会
	 * 把残留当成这次的结果报回去。
	 *
	 * 所以分两段：清理落地（bGenerated == false）之前不下发生成；下发之后
	 * 再读到的 true 才一定是这一轮的。清理是同步完成的话第一段直接跳过。
	 */
	{
		UALReflect::FCall Cleanup(Component, TEXT("CleanupLocal"));
		Cleanup.Bool(TEXT("bRemoveComponents"), true);
		Cleanup.Invoke();
	}

	const auto IssueGenerate = [](UObject* Target) -> bool
	{
		UALReflect::FCall Generate(Target, TEXT("GenerateLocal"));
		if (!Generate.IsValid())
		{
			return false;
		}
		Generate.Bool(TEXT("bForce"), true);
		return Generate.Invoke();
	};

	// 共享给 ticker：Generate 下发了没有
	TSharedRef<bool> bGenerateIssued = MakeShared<bool>(false);
	{
		bool bStillGenerated = false;
		UALReflect::GetBoolProp(Component, TEXT("bGenerated"), bStillGenerated);
		if (!bStillGenerated)
		{
			*bGenerateIssued = IssueGenerate(Component);
		}
	}

	// PCG 生成是异步的，不能在这里同步等 —— 阻塞 GameThread 只会让它永远跑不完。
	// 挂一个 ticker 轮询 bGenerated，跑完或超时再回响应。
	const double StartTime = FPlatformTime::Seconds();
	TWeakObjectPtr<AActor> WeakActor(Actor);
	TWeakObjectPtr<UObject> WeakComponent(Component);

	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda(
			[RequestId, WeakActor, WeakComponent, StartTime, TimeoutSeconds, ActorLabel, LogCapture,
				bGenerateIssued, IssueGenerate](float) -> bool
			{
				AActor* LiveActor = WeakActor.Get();
				UObject* LiveComponent = WeakComponent.Get();
				if (!LiveActor || !LiveComponent)
				{
					UAL_CommandUtils::SendError(RequestId, 410,
						TEXT("The PCG volume was destroyed while generating."));
					return false;
				}

				bool bGenerated = false;
				UALReflect::GetBoolProp(LiveComponent, TEXT("bGenerated"), bGenerated);

				const double Elapsed = FPlatformTime::Seconds() - StartTime;

				// ---- 第一段：等上一轮的产物清掉 ----
				if (!*bGenerateIssued)
				{
					if (bGenerated)
					{
						if (Elapsed < TimeoutSeconds)
						{
							return true; // 清理还没落地，继续等
						}
						UAL_CommandUtils::SendError(RequestId, 504,
							FString::Printf(
								TEXT("The previous generation result on '%s' was not cleared within %.0fs, so a new run was ")
								TEXT("not started (its completion could not be told apart from the old result). ")
								TEXT("The old output may be partly removed. Try pcg.execute again with a longer timeout_seconds."),
								*ActorLabel, TimeoutSeconds));
						return false;
					}
					if (!IssueGenerate(LiveComponent))
					{
						UAL_CommandUtils::SendError(RequestId, 500,
							TEXT("PCG API mismatch: GenerateLocal disappeared after cleanup. The previous result was cleared ")
							TEXT("and nothing new was generated."));
						return false;
					}
					*bGenerateIssued = true;
					return true; // 下一拍再读 bGenerated —— 这一拍读到的是下发前的值
				}

				// ---- 第二段：等这一轮生成完 ----
				if (!bGenerated && Elapsed < TimeoutSeconds)
				{
					return true; // 继续轮询
				}

				int32 ComponentCount = 0, InstanceCount = 0;
				TArray<TSharedPtr<FJsonValue>> Breakdown;
				CountGeneratedInstances(LiveActor, ComponentCount, InstanceCount, Breakdown);

				// 组件勾了 Is Partitioned 的话，产物挂在 PCGPartitionActor 上而不是体积上。
				// 只数体积会永远报 0 —— 和「什么都没生成」看起来一模一样
				bool bPartitioned = false;
				UALReflect::GetBoolProp(LiveComponent, TEXT("bIsComponentPartitioned"), bPartitioned);
				const int32 PartitionedInstances =
					bPartitioned ? CountPartitionedInstances(LiveActor->GetWorld()) : 0;

				TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("actor_label"), ActorLabel);
				Data->SetBoolField(TEXT("generated"), bGenerated);
				Data->SetNumberField(TEXT("elapsed_seconds"), FMath::RoundToFloat(Elapsed * 100.0) / 100.0);
				Data->SetNumberField(TEXT("component_count"), ComponentCount);
				Data->SetNumberField(TEXT("instance_count"), InstanceCount);
				Data->SetArrayField(TEXT("components"), Breakdown);
				Data->SetBoolField(TEXT("partitioned"), bPartitioned);
				if (bPartitioned)
				{
					// 口径要说清：这是关卡里**所有**分区 actor 的总量，分不出是哪个体积的
					// （那个映射表是 private TMap，反射读它性价比太低）
					Data->SetNumberField(TEXT("partitioned_instance_count"), PartitionedInstances);
				}

				// 引擎自己在执行期间说了什么，原样带回去。产出 0 的时候
				// 这几行往往就是答案，比让人事后去翻 LogPCG 快一个数量级
				if (LogCapture.IsValid() && LogCapture->HasAny())
				{
					Data->SetArrayField(TEXT("engine_log"), LogCapture->ToJson());
				}

				if (!bGenerated)
				{
					// 超时不等于失败：分区 Actor 上 bGenerated 永远是 false。
					// 如实说「没等到确认」，同时把真正的产出数量给出去让调用方自己判断
					Data->SetStringField(TEXT("note"), FString::Printf(
						TEXT("Generation did not report completion within %.0fs. ")
						TEXT("For partitioned actors this flag is never set, so check instance_count instead."),
						TimeoutSeconds));
				}
				else if (InstanceCount == 0)
				{
					// 「跑完了但产出是 0」是最常见的失败形态，而且从外面完全看不出原因。
					// 一句泛泛的「检查一下连线和网格」等于把排查工作原样退回去，
					// 所以直接指向能给出分级根因的那个命令
					Data->SetStringField(TEXT("note"),
						TEXT("The graph ran but produced no instances. Call pcg.diagnose on this graph - "
							 "it checks the usual root causes in order (Output not wired, Input not wired, "
							 "orphan nodes, spawner without a mesh) and tells you which one applies. "
							 "If the graph itself is clean, the volume probably does not overlap any "
							 "landscape or mesh for the sampler to hit."));
				}

				// 异步写落在命令作用域之外，包不会被自动登记，得手动补
				if (UPackage* Package = LiveActor->GetPackage())
				{
					FUAL_TouchedPackages::Touch(Package);
				}

				UAL_CommandUtils::SendResponse(RequestId, 200, Data);
				return false;
			}),
		0.25f);
}

// ======================================================================
// 整图声明式写入
// ======================================================================

/**
 * 一次调用写完整张图。
 *
 * ## 为什么必须有这个
 *
 * 逐节点 add / connect 的形态下，搭一张十来个节点的图要几十次往返，每次都可能
 * 在某个环节静默失败，而失败要到最后 execute 出 0 个实例才暴露 —— 那时候已经
 * 无从判断是哪一步断的。材质有 material_apply_graph、蓝图有 blueprint_apply_graph，
 * PCG 缺这一块是最大的空缺。
 *
 * ## 语义
 *
 * - 按 key 声明节点：key 是调用方自己起的别名，不是引擎的 node_id。
 *   已存在同别名节点就复用（按 title 匹配），否则新建 —— 所以**重复执行同一份
 *   声明不会长出重复节点**，这条对"改一改再跑一遍"至关重要
 * - 连线用别名，也认 "Input" / "Output"
 * - 属性走路径写入，支持 `A.B[0].C`
 * - 每一项都回读校验，并逐条报出原因
 *
 * ## 失败时图是什么样
 *
 * **事务回滚不了。** `Transaction.Cancel()` 只把撤销记录丢掉，从不回放 ——
 * 改过的东西原样留着，而且连 Ctrl+Z 都没了（见 UAL_SequencerCommands 里那段长注释）。
 * 上一版在清图、建节点、连线之后 Cancel，然后回「整批回滚，什么都没变」，
 * 而图其实已经被清空了一半。所以现在分两段：
 *
 * 1. **只读预检**：节点缺 id / type、类型名不存在、连线缺端点、端点找不到 ——
 *    这些不动图就能判掉的，全在动手前判完。有一项不对就原样不动地报回去，
 *    这时说「什么都没变」才是真话（连图都还没建）
 * 2. **动手之后**才暴露的失败（属性写不进、引脚连不上、清图清不干净）不 Cancel：
 *    整批留在撤销栈里、一步可撤，并把图**现在**的样子（graph_nodes_after /
 *    graph_edges_after）和已经做了什么一起报回去
 */
void FUAL_PCGCommands::Handle_ApplyGraph(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!RequirePCG(RequestId))
	{
		return;
	}

	FString GraphPath;
	if (!Payload->TryGetStringField(TEXT("graph_path"), GraphPath) || GraphPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: graph_path"));
		return;
	}
	GraphPath = NormalizePath(GraphPath);

	// 空声明先挡掉。上一版这一步排在自动建图之后，空调用也会留下一张新图
	const TArray<TSharedPtr<FJsonValue>>* NodeSpecs = nullptr;
	Payload->TryGetArrayField(TEXT("nodes"), NodeSpecs);
	const TArray<TSharedPtr<FJsonValue>>* EdgeSpecs = nullptr;
	Payload->TryGetArrayField(TEXT("edges"), EdgeSpecs);

	if ((!NodeSpecs || NodeSpecs->Num() == 0) && (!EdgeSpecs || EdgeSpecs->Num() == 0))
	{
		UAL_CommandUtils::SendError(RequestId, 400,
			TEXT("Nothing to apply: provide 'nodes', 'edges', or both."));
		return;
	}

	/**
	 * merge（默认）：只按别名新建/更新声明里提到的节点，其余原样保留。
	 * replace：先清空图里除 Input / Output 之外的一切，再按声明重建。
	 *
	 * 为什么要有 replace：改拓扑时 merge 会把上一版的节点和边留在图里，
	 * 声明看着是对的、图却是脏的，而且这个差别从返回值上看不出来。
	 * 那种「残留」只能靠人一个个 remove_node，是上一轮实测里真的绕进去过的坑。
	 */
	// **默认 replace。** 工具叫 apply_graph、参数叫 nodes/edges，读起来就是
	// 「这张图长这样」，而不是「往图里加这些」。默认 merge 违反了这个直觉：
	// 实测里改完拓扑，上一版的节点还留在图里，声明看着对、图却是脏的、产出是 0，
	// 而这个差别从返回值上看不出来。
	// 想增量往图里补东西的走 mode="merge"，那是明确表达的意图。
	FString Mode = TEXT("replace");
	Payload->TryGetStringField(TEXT("mode"), Mode);
	const bool bReplace = !Mode.Equals(TEXT("merge"), ESearchCase::IgnoreCase);

	// 图不存在就顺手建一张。「声明这张图长什么样」里本来就包含「这张图存在」，
	// 硬要求先调一次 create_graph 只是多一次往返和一个能踩的坑。
	// 路径打错的后果只是多一个空图资产，看得见也删得掉 —— 比直接失败轻。
	//
	// GraphExists 而不是 DoesPackageExist。后者只看磁盘，于是「本轮刚建、还没存盘」
	// 的图会走进新建分支，把自己的节点、连线和用户参数整个盖掉 ——
	// 真机上表现为「merge 却清了我的节点」「用户参数变成 0 个」。见 GraphExists 的注释
	//
	// 这里只**加载**已有的图；真要新建推迟到预检通过之后，预检失败不留下空图
	UObject* Graph = nullptr;
	if (GraphExists(GraphPath))
	{
		FString LoadError;
		Graph = LoadGraph(GraphPath, LoadError);
		if (!Graph)
		{
			UAL_CommandUtils::SendError(RequestId, 404, LoadError);
			return;
		}
	}

	TArray<TSharedPtr<FJsonValue>> Failures;

	const auto Fail = [&Failures](const FString& What, const FString& Why)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("item"), What);
		Entry->SetStringField(TEXT("error"), Why);
		Failures.Add(MakeShared<FJsonValueObject>(Entry));
	};

	// ══ 第一段：只读预检 ══════════════════════════════════════════════════
	//
	// 这一段不碰图。能在这里判掉的失败，都不该等到清图、建节点之后才发现

	struct FNodePlan
	{
		FString Alias;
		UClass* SettingsClass = nullptr;
		TSharedPtr<FJsonObject> Spec;
	};
	struct FEdgePlan
	{
		FString FromAlias;
		FString ToAlias;
		FString FromPin;
		FString ToPin;
	};

	TArray<FNodePlan> NodePlans;
	// FString 作键的 TSet 默认大小写不敏感，和下面 ByAlias 的查找口径一致
	TSet<FString> DeclaredAliases;

	for (const TSharedPtr<FJsonValue>& Value : NodeSpecs ? *NodeSpecs : TArray<TSharedPtr<FJsonValue>>())
	{
		const TSharedPtr<FJsonObject>* Spec = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Spec) || !Spec)
		{
			Fail(TEXT("(node)"), TEXT("node entry is not an object"));
			continue;
		}

		FString Alias;
		if (!(*Spec)->TryGetStringField(TEXT("id"), Alias) || Alias.IsEmpty())
		{
			Fail(TEXT("(node)"), TEXT("every node needs an 'id' to reference it from 'edges'"));
			continue;
		}

		FString TypeName;
		if (!(*Spec)->TryGetStringField(TEXT("type"), TypeName) || TypeName.IsEmpty())
		{
			Fail(Alias, TEXT("missing 'type'"));
			continue;
		}

		FString ResolveError;
		UClass* SettingsClass = ResolveSettingsClass(TypeName, ResolveError);
		if (!SettingsClass)
		{
			Fail(Alias, ResolveError);
			continue;
		}

		FNodePlan Plan;
		Plan.Alias = Alias;
		Plan.SettingsClass = SettingsClass;
		Plan.Spec = *Spec;
		NodePlans.Add(Plan);
		DeclaredAliases.Add(Alias);
	}

	UObject* ExistingInput = Graph ? UALReflect::GetObjectProp(Graph, TEXT("InputNode")) : nullptr;
	UObject* ExistingOutput = Graph ? UALReflect::GetObjectProp(Graph, TEXT("OutputNode")) : nullptr;

	// 连线端点在动手之后能不能找到。和下面写入阶段的查找顺序一致：
	// 先别名（声明的节点 + Input / Output），再退回按真实 node_id 找
	const auto EndpointProblem = [&](const FString& Alias) -> FString
	{
		if (DeclaredAliases.Contains(Alias) ||
			Alias.Equals(ALIAS_INPUT, ESearchCase::IgnoreCase) ||
			Alias.Equals(ALIAS_OUTPUT, ESearchCase::IgnoreCase))
		{
			return FString();
		}
		UObject* Found = Graph ? FindNodeById(Graph, Alias) : nullptr;
		if (!Found)
		{
			return FString::Printf(TEXT("unknown node '%s' - not declared in 'nodes' and not an existing node id"), *Alias);
		}
		// replace 会先清掉 Input / Output 以外的一切，指着旧节点的边到时候就悬空了
		if (bReplace && Found != ExistingInput && Found != ExistingOutput)
		{
			return FString::Printf(
				TEXT("'%s' is an existing node, but mode=replace removes it before edges are made. ")
				TEXT("Declare it in 'nodes' or use mode=merge."),
				*Alias);
		}
		return FString();
	};

	TArray<FEdgePlan> EdgePlans;
	for (const TSharedPtr<FJsonValue>& Value : EdgeSpecs ? *EdgeSpecs : TArray<TSharedPtr<FJsonValue>>())
	{
		const TSharedPtr<FJsonObject>* Spec = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Spec) || !Spec)
		{
			Fail(TEXT("(edge)"), TEXT("edge entry is not an object"));
			continue;
		}

		FEdgePlan Plan;
		(*Spec)->TryGetStringField(TEXT("from"), Plan.FromAlias);
		(*Spec)->TryGetStringField(TEXT("to"), Plan.ToAlias);
		(*Spec)->TryGetStringField(TEXT("from_pin"), Plan.FromPin);
		(*Spec)->TryGetStringField(TEXT("to_pin"), Plan.ToPin);

		const FString EdgeLabel = FString::Printf(TEXT("%s -> %s"), *Plan.FromAlias, *Plan.ToAlias);

		if (Plan.FromAlias.IsEmpty() || Plan.ToAlias.IsEmpty())
		{
			Fail(EdgeLabel, TEXT("edges need both 'from' and 'to'"));
			continue;
		}

		FString Problem = EndpointProblem(Plan.FromAlias);
		if (Problem.IsEmpty())
		{
			Problem = EndpointProblem(Plan.ToAlias);
		}
		if (!Problem.IsEmpty())
		{
			Fail(EdgeLabel, Problem);
			continue;
		}

		EdgePlans.Add(Plan);
	}

	if (Failures.Num() > 0)
	{
		// 这时候确实什么都没动：图没建、没清、没写
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("failures"), Failures);
		Details->SetNumberField(TEXT("failed_count"), Failures.Num());
		UAL_CommandUtils::SendError(RequestId, 400,
			FString::Printf(
				TEXT("%d item(s) are invalid. Nothing was changed - these were caught before touching the graph."),
				Failures.Num()),
			Details);
		return;
	}

	// ══ 第二段：动手 ══════════════════════════════════════════════════════

	bool bCreated = false;
	if (!Graph)
	{
		FString CreateError;
		Graph = CreateGraphAsset(GraphPath, CreateError);
		if (!Graph)
		{
			UAL_CommandUtils::SendError(RequestId, 500, CreateError);
			return;
		}
		bCreated = true;
	}

	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALPCG", "ApplyPCGGraph", "Apply PCG Graph"));
	Graph->Modify();

	UObject* InputNode = UALReflect::GetObjectProp(Graph, TEXT("InputNode"));
	UObject* OutputNode = UALReflect::GetObjectProp(Graph, TEXT("OutputNode"));

	/** 写完（或写到一半）之后图里实际有什么。成功和失败两条出口都要带 */
	const auto AttachGraphAfter = [Graph](const TSharedPtr<FJsonObject>& Target)
	{
		TArray<TSharedPtr<FJsonValue>> AllEdges;
		CollectEdges(Graph, AllEdges);
		Target->SetArrayField(TEXT("graph_edges_after"), AllEdges);

		TArray<UObject*> NodesAfter;
		CollectAllNodes(Graph, NodesAfter);
		TArray<TSharedPtr<FJsonValue>> NodesAfterJson;
		for (UObject* Node : NodesAfter)
		{
			NodesAfterJson.Add(MakeShared<FJsonValueObject>(BuildNodeJson(Node)));
		}
		Target->SetArrayField(TEXT("graph_nodes_after"), NodesAfterJson);
	};

	int32 ClearedNodes = 0;
	TArray<TSharedPtr<FJsonValue>> ClearFailures;
	if (bReplace)
	{
		// 一次性批量删，而不是逐个调 RemoveNode：RemoveNodes 内部会先把所有
		// 关联边断干净再摘节点，逐个删时前一个节点残留的边会挡住后一个
		TArray<UObject*> ToRemove;
		TArray<UObject*> Existing;
		CollectAllNodes(Graph, Existing);
		for (UObject* Node : Existing)
		{
			// Input / Output 是图的固有端点，删不掉也不该删
			if (Node && Node != InputNode && Node != OutputNode)
			{
				ToRemove.Add(Node);
			}
		}

		for (UObject* Node : ToRemove)
		{
			UALReflect::FCall Remove(Graph, TEXT("RemoveNode"));
			if (!Remove.IsValid())
			{
				break;
			}
			Remove.Obj(TEXT("InNode"), Node);
			Remove.Invoke();
		}

		// **回读校验。** 上一版这里只是数了调用次数就报 cleared_nodes，
		// 实测发现节点其实还在 —— 又是一次「调了就当成功」。
		TArray<UObject*> AfterClear;
		CollectAllNodes(Graph, AfterClear);
		for (UObject* Node : AfterClear)
		{
			if (Node && Node != InputNode && Node != OutputNode)
			{
				ClearFailures.Add(MakeShared<FJsonValueString>(NodeIdOf(Node)));
			}
		}
		ClearedNodes = ToRemove.Num() - ClearFailures.Num();

		if (ClearFailures.Num() > 0)
		{
			// 不 Cancel：已经删掉的那几个回不来，Cancel 只会让它们连撤销都撤不回。
			// 声明里的节点和连线一个都没写 —— 半清的图上再往下写只会更难收拾
			Graph->MarkPackageDirty();
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("still_present"), ClearFailures);
			Details->SetNumberField(TEXT("cleared_nodes"), ClearedNodes);
			Details->SetNumberField(TEXT("failed_count"), ClearFailures.Num());
			Details->SetBoolField(TEXT("created_graph"), bCreated);
			AttachGraphAfter(Details);
			UAL_CommandUtils::SendError(RequestId, 500,
				FString::Printf(
					TEXT("mode=replace removed %d old node(s) but could not clear %d more, so none of the declared ")
					TEXT("nodes or edges were written. The %d removed node(s) are gone (one undo step brings them back); ")
					TEXT("graph_nodes_after shows what is left. Remove the rest with pcg.remove_node and try again, ")
					TEXT("or use mode=merge."),
					ClearedNodes, ClearFailures.Num(), ClearedNodes),
				Details);
			return;
		}
	}

	// 别名 → 真实节点。预置两个端点，让连线里能直接写 Input / Output
	TMap<FString, UObject*> ByAlias;
	if (InputNode)
	{
		ByAlias.Add(TEXT("Input"), InputNode);
	}
	if (OutputNode)
	{
		ByAlias.Add(TEXT("Output"), OutputNode);
	}

	TArray<TSharedPtr<FJsonValue>> NodeResults;
	TArray<TSharedPtr<FJsonValue>> EdgeResults;
	int32 CreatedNodes = 0;

	// ---- 节点 ----
	for (const FNodePlan& Plan : NodePlans)
	{
		const FString& Alias = Plan.Alias;
		UClass* SettingsClass = Plan.SettingsClass;

		// 复用同别名的既有节点。不这么做的话，同一份声明跑两遍会长出两套节点，
		// 而"改一改再跑一遍"正是使用这个工具最自然的方式
		UObject* Node = nullptr;
		UObject* Settings = nullptr;
		{
			TArray<UObject*> Existing;
			CollectAllNodes(Graph, Existing);
			for (UObject* Candidate : Existing)
			{
				FName Title;
				UALReflect::GetNameProp(Candidate, TEXT("NodeTitle"), Title);
				if (Title.ToString().Equals(Alias, ESearchCase::CaseSensitive))
				{
					UObject* CandidateSettings = GetNodeSettings(Candidate);
					if (CandidateSettings && CandidateSettings->IsA(SettingsClass))
					{
						Node = Candidate;
						Settings = CandidateSettings;
					}
					break;
				}
			}
		}

		bool bNewNode = false;
		if (!Node)
		{
			UALReflect::FCall Add(Graph, TEXT("AddNodeOfType"));
			if (!Add.IsValid())
			{
				Fail(Alias, FString::Printf(TEXT("PCG API mismatch: %s"), *Add.GetError()));
				continue;
			}
			Add.Cls(TEXT("InSettingsClass"), SettingsClass);
			Add.Invoke();

			Node = Add.OutObject(TEXT("ReturnValue"));
			if (!Node)
			{
				Fail(Alias, FString::Printf(TEXT("PCG refused to create a %s node"), *SettingsClass->GetName()));
				continue;
			}
			Settings = Add.OutObject(TEXT("DefaultNodeSettings"));
			if (!Settings)
			{
				Settings = GetNodeSettings(Node);
			}
			// 别名写进 NodeTitle，下次 apply 才认得出这是同一个节点
			UALReflect::SetNameProp(Node, TEXT("NodeTitle"), FName(*Alias));
			bNewNode = true;
			++CreatedNodes;
		}

		int32 X = 0, Y = 0;
		if (Plan.Spec->TryGetNumberField(TEXT("x"), X))
		{
			UALReflect::SetIntProp(Node, TEXT("PositionX"), X);
		}
		if (Plan.Spec->TryGetNumberField(TEXT("y"), Y))
		{
			UALReflect::SetIntProp(Node, TEXT("PositionY"), Y);
		}

		TArray<TSharedPtr<FJsonValue>> Updated, FailedProps;
		TSharedPtr<FJsonObject> Properties;
		if (UAL_CommandUtils::TryGetObjectFieldFlexible(Plan.Spec, TEXT("properties"), Properties))
		{
			ApplyProperties(Settings, Properties, Updated, FailedProps);
		}

		for (const TSharedPtr<FJsonValue>& FailedProp : FailedProps)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (FailedProp.IsValid() && FailedProp->TryGetObject(Obj) && Obj)
			{
				Fail(FString::Printf(TEXT("%s.%s"), *Alias, *(*Obj)->GetStringField(TEXT("name"))),
					(*Obj)->GetStringField(TEXT("error")));
			}
		}

		ByAlias.Add(Alias, Node);

		// 位置、标题、引脚都由 BuildNodeJson 从节点上回读
		TSharedPtr<FJsonObject> Result = BuildNodeJson(Node);
		Result->SetStringField(TEXT("alias"), Alias);
		Result->SetBoolField(TEXT("created"), bNewNode);
		Result->SetArrayField(TEXT("applied_properties"), Updated);
		NodeResults.Add(MakeShared<FJsonValueObject>(Result));
	}

	// ---- 连线 ----
	for (const FEdgePlan& Plan : EdgePlans)
	{
		const FString EdgeLabel = FString::Printf(TEXT("%s -> %s"), *Plan.FromAlias, *Plan.ToAlias);

		// 别名找不到就退回按真实 node_id 找，两种写法都认
		UObject* FromNode = ByAlias.FindRef(Plan.FromAlias);
		if (!FromNode)
		{
			FromNode = FindNodeById(Graph, Plan.FromAlias);
		}
		UObject* ToNode = ByAlias.FindRef(Plan.ToAlias);
		if (!ToNode)
		{
			ToNode = FindNodeById(Graph, Plan.ToAlias);
		}

		// 预检过了还找不到，只可能是上面那个节点没建出来 —— 它的失败已经记过了
		if (!FromNode || !ToNode)
		{
			Fail(EdgeLabel, FString::Printf(TEXT("node '%s' was not created, so this edge was skipped"),
				!FromNode ? *Plan.FromAlias : *Plan.ToAlias));
			continue;
		}

		FString FromPin = Plan.FromPin;
		FString ToPin = Plan.ToPin;
		FString ConnectError;
		if (!ConnectEdge(Graph, FromNode, Plan.FromAlias, ToNode, Plan.ToAlias, FromPin, ToPin, ConnectError))
		{
			Fail(EdgeLabel, ConnectError);
			continue;
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("from"), Plan.FromAlias);
		Result->SetStringField(TEXT("from_pin"), FromPin);
		Result->SetStringField(TEXT("to"), Plan.ToAlias);
		Result->SetStringField(TEXT("to_pin"), ToPin);
		EdgeResults.Add(MakeShared<FJsonValueObject>(Result));
	}

	Graph->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("graph_path"), GraphPath);
	// 自动建的图要说出来。不说的话，路径打错会表现成「写成功了但编辑器里找不到」
	Data->SetBoolField(TEXT("created_graph"), bCreated);
	Data->SetStringField(TEXT("mode"), bReplace ? TEXT("replace") : TEXT("merge"));
	// merge 模式下图里可能还留着这次声明没提到的旧节点。把总数报出来，
	// 调用方一对比就知道有没有残留 —— 上一轮就是因为看不出来才绕了几圈
	Data->SetNumberField(TEXT("cleared_nodes"), ClearedNodes);
	Data->SetArrayField(TEXT("nodes"), NodeResults);
	Data->SetArrayField(TEXT("edges"), EdgeResults);

	// 回读整张图一起返回，省掉调用方紧接着再来一次 get_graph。
	// **节点也要回读** —— merge 模式下这次没提到的旧节点还在图里，
	// 只报本次写入的那几个会让调用方以为图就长这样
	AttachGraphAfter(Data);

	if (Failures.Num() > 0)
	{
		// 动手之后才失败的：不 Cancel（它不回退，只会让已做的改动撤不回来），
		// 如实说图已经变了、变成了什么样。整批是一个撤销步
		Data->SetArrayField(TEXT("failures"), Failures);
		Data->SetNumberField(TEXT("failed_count"), Failures.Num());
		UAL_CommandUtils::SendError(RequestId, 400,
			FString::Printf(
				TEXT("%d item(s) failed after the graph had already been changed, and the changes were NOT rolled back: ")
				TEXT("%s%s%d node(s) written (%d newly created), %d edge(s) connected. graph_nodes_after / graph_edges_after ")
				TEXT("show the graph as it is now; the whole apply is one undo step. Fix the failed items and run ")
				TEXT("pcg.apply_graph again - nodes are matched by id, so nothing gets duplicated."),
				Failures.Num(),
				bCreated ? TEXT("created the graph, ") : TEXT(""),
				*(bReplace ? FString::Printf(TEXT("cleared %d old node(s), "), ClearedNodes) : FString()),
				NodeResults.Num(), CreatedNodes, EdgeResults.Num()),
			Data);
		return;
	}

	UE_LOG(LogUALPCG, Log, TEXT("Applied %d nodes / %d edges to %s"),
		NodeResults.Num(), EdgeResults.Num(), *GraphPath);
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ======================================================================
// 诊断
// ======================================================================

/**
 * 「为什么什么都没生成」的分级排查。
 *
 * 之前 execute 只会说一句「跑完了但产出是 0，检查一下连线和网格」，
 * 那等于把排查工作原样退回给调用方。这里把常见根因逐条判掉，
 * 每条给出可执行的下一步。
 */
void FUAL_PCGCommands::Handle_Diagnose(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!RequirePCG(RequestId))
	{
		return;
	}

	FString GraphPath;
	if (!Payload->TryGetStringField(TEXT("graph_path"), GraphPath) || GraphPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: graph_path"));
		return;
	}
	GraphPath = NormalizePath(GraphPath);

	FString LoadError;
	UObject* Graph = LoadGraph(GraphPath, LoadError);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, LoadError);
		return;
	}

	TArray<TSharedPtr<FJsonValue>> Problems;
	const auto Report = [&Problems](const TCHAR* Severity, const FString& What, const FString& Fix)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("severity"), Severity);
		Entry->SetStringField(TEXT("problem"), What);
		Entry->SetStringField(TEXT("fix"), Fix);
		Problems.Add(MakeShared<FJsonValueObject>(Entry));
	};

	TArray<UObject*> Nodes;
	CollectAllNodes(Graph, Nodes);

	UObject* OutputNode = UALReflect::GetObjectProp(Graph, TEXT("OutputNode"));
	UObject* InputNode = UALReflect::GetObjectProp(Graph, TEXT("InputNode"));

	// 1. Output 有没有被连上 —— 图不接 Output 就什么都不会产出，
	//    这是最常见也最难自己发现的一条
	bool bOutputConnected = false;
	if (OutputNode)
	{
		TArray<UObject*> InPins;
		UALReflect::GetObjectArrayProp(OutputNode, TEXT("InputPins"), InPins);
		for (UObject* Pin : InPins)
		{
			TArray<UObject*> Edges;
			UALReflect::GetObjectArrayProp(Pin, TEXT("Edges"), Edges);
			if (Edges.Num() > 0)
			{
				bOutputConnected = true;
				break;
			}
		}
	}
	if (!bOutputConnected)
	{
		TArray<FString> Labels;
		CollectPinLabels(OutputNode, /*bOutput=*/false, Labels);
		Report(TEXT("blocker"),
			TEXT("Nothing is connected to the graph's Output node, so the graph produces nothing."),
			FString::Printf(
				TEXT("Connect your last node to the Output node. Its input pin is named '%s' (not 'In')."),
				Labels.Num() ? *Labels[0] : TEXT("Out")));
	}

	// 2. Input 有没有被连上
	bool bInputConnected = false;
	if (InputNode)
	{
		TArray<UObject*> OutPins;
		UALReflect::GetObjectArrayProp(InputNode, TEXT("OutputPins"), OutPins);
		for (UObject* Pin : OutPins)
		{
			TArray<UObject*> Edges;
			UALReflect::GetObjectArrayProp(Pin, TEXT("Edges"), Edges);
			if (Edges.Num() > 0)
			{
				bInputConnected = true;
				break;
			}
		}
	}
	if (!bInputConnected)
	{
		TArray<FString> Labels;
		CollectPinLabels(InputNode, /*bOutput=*/true, Labels);
		Report(TEXT("blocker"),
			TEXT("The graph's Input node feeds nothing, so samplers receive no surface to sample."),
			FString::Printf(
				TEXT("Connect the Input node to your first node. Its output pin is named '%s' (not 'Out')."),
				Labels.Num() ? *Labels[0] : TEXT("In")));
	}

	// 3. 孤立节点：既没有入边也没有出边，跑起来等于不存在
	TArray<TSharedPtr<FJsonValue>> Orphans;
	for (UObject* Node : Nodes)
	{
		if (Node == InputNode || Node == OutputNode)
		{
			continue;
		}
		int32 EdgeCount = 0;
		for (const TCHAR* PinsProp : { TEXT("InputPins"), TEXT("OutputPins") })
		{
			TArray<UObject*> Pins;
			UALReflect::GetObjectArrayProp(Node, PinsProp, Pins);
			for (UObject* Pin : Pins)
			{
				TArray<UObject*> Edges;
				UALReflect::GetObjectArrayProp(Pin, TEXT("Edges"), Edges);
				EdgeCount += Edges.Num();
			}
		}
		if (EdgeCount == 0)
		{
			Orphans.Add(MakeShared<FJsonValueString>(NodeIdOf(Node)));
		}
	}
	if (Orphans.Num() > 0)
	{
		TArray<FString> Names;
		for (const TSharedPtr<FJsonValue>& Orphan : Orphans)
		{
			Names.Add(Orphan->AsString());
		}
		Report(TEXT("warning"),
			FString::Printf(TEXT("%d node(s) have no connections at all: %s"),
				Orphans.Num(), *FString::Join(Names, TEXT(", "))),
			TEXT("Either wire them into the graph or remove them with pcg.remove_node."));
	}

	// 4. 生成器节点有没有指定网格 —— 没网格的生成器会跑完并产出 0 个实例，
	//    从外面看和「采样器没采到点」一模一样
	for (UObject* Node : Nodes)
	{
		UObject* Settings = GetNodeSettings(Node);
		if (!Settings || !Settings->GetClass()->GetName().Contains(TEXT("StaticMeshSpawner")))
		{
			continue;
		}

		FString ReadError;
		const TSharedPtr<FJsonValue> First =
			UALPropertyPath::GetByPath(Settings,
				TEXT("MeshSelectorParameters.MeshEntries[0].Descriptor.StaticMesh"), ReadError);

		const FString MeshPath = First.IsValid() ? First->AsString() : FString();
		if (MeshPath.IsEmpty() || MeshPath == TEXT("None"))
		{
			Report(TEXT("blocker"),
				FString::Printf(TEXT("Spawner node '%s' has no static mesh assigned, so it spawns nothing."),
					*NodeIdOf(Node)),
				TEXT("Set it with pcg.update_node, e.g. properties = ")
				TEXT("{\"MeshSelectorParameters.MeshEntries[0].Descriptor.StaticMesh\": \"/Game/Trees/SM_Oak\", ")
				TEXT("\"MeshSelectorParameters.MeshEntries[0].Weight\": 1}"));
		}
	}

	// 5. 表面采样器的 Surface 引脚接了东西吗。
	//
	//    这条是实测里花了最多轮才摸出来的：把图的 Input 直接接进采样器**拿不到地形**，
	//    引擎只会说「没有找到进行生成的表面」，而那句话看起来像是场景的问题，
	//    于是人会去反复折腾关卡、体积、流送 —— 真正缺的是一个 PCGGetLandscape 节点。
	for (UObject* Node : Nodes)
	{
		UObject* Settings = GetNodeSettings(Node);
		if (!Settings || !Settings->GetClass()->GetName().Contains(TEXT("SurfaceSampler")))
		{
			continue;
		}

		TArray<UObject*> InPins;
		UALReflect::GetObjectArrayProp(Node, TEXT("InputPins"), InPins);

		bool bSurfaceFed = false;
		for (UObject* Pin : InPins)
		{
			FName Label;
			if (!UALReflect::GetNameInStructProp(Pin, TEXT("Properties"), TEXT("Label"), Label) ||
				!Label.ToString().Equals(TEXT("Surface"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			TArray<UObject*> Edges;
			UALReflect::GetObjectArrayProp(Pin, TEXT("Edges"), Edges);
			bSurfaceFed = Edges.Num() > 0;
			break;
		}

		if (!bSurfaceFed)
		{
			Report(TEXT("blocker"),
				FString::Printf(
					TEXT("Surface sampler '%s' has nothing wired into its 'Surface' pin, so it has no "
						 "terrain to sample and will report 'no surface found' no matter what the level "
						 "contains."),
					*NodeIdOf(Node)),
				TEXT("Wiring the graph Input straight into the sampler does NOT feed it the landscape. "
					 "Add a PCGGetLandscape node and connect it to the sampler's 'Surface' pin: "
					 "PCGGetLandscape -> SurfaceSampler.Surface -> StaticMeshSpawner -> Output."));
		}
	}

	// 6. 关卡侧：World Partition 世界里 PCG 采不到没流送进来的 actor。
	//    这一条工具本来完全感知不到，只能靠人穷举各种接法 —— 实测里绕掉了一整轮。
	//    图结构没问题时它往往才是真正的原因，所以放在最后单独判
	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	const bool bPartitioned = World && World->IsPartitionedWorld();
	if (bPartitioned)
	{
		Report(TEXT("info"),
			TEXT("This level is a World Partition level. The landscape is split into ALandscapeStreamingProxy "
				 "actors, one per cell, and unloaded cells are not in the world at all - they are not empty, "
				 "they simply do not exist as actors. A PCG surface sampler therefore has nothing to hit and "
				 "correctly reports 'no surface found'."),
			TEXT("To cover one region: load that region in the World Partition editor, then run pcg.execute. "
				 "To cover the whole world: tick 'Is Partitioned' on the PCG component in the Details panel - "
				 "PCG then creates one PCGPartitionActor per grid cell so each cell generates on its own and "
				 "the world never has to be fully resident. Baking every cell is what the 'World Partition PCG "
				 "Builder' commandlet is for; for gameplay, use Runtime Generation with Hierarchical Generation."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("graph_path"), GraphPath);
	Data->SetArrayField(TEXT("problems"), Problems);
	Data->SetNumberField(TEXT("node_count"), Nodes.Num());
	Data->SetBoolField(TEXT("world_partition"), bPartitioned);
	if (Problems.Num() == 0)
	{
		Data->SetStringField(TEXT("summary"),
			TEXT("The graph structure looks fine: Input and Output are wired, no orphan nodes, "
				 "spawners have meshes. If generation still produces nothing, the cause is at the "
				 "level: check that the PCG volume actually overlaps a landscape or static mesh "
				 "for the sampler to hit, and that the level has been saved."));
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ======================================================================
// 完整读节点设置
// ======================================================================

void FUAL_PCGCommands::Handle_DescribeNode(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!RequirePCG(RequestId))
	{
		return;
	}

	FString GraphPath, NodeId;
	if (!Payload->TryGetStringField(TEXT("graph_path"), GraphPath) || GraphPath.IsEmpty() ||
		!Payload->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required fields: graph_path, node_id"));
		return;
	}
	GraphPath = NormalizePath(GraphPath);

	FString LoadError;
	UObject* Graph = LoadGraph(GraphPath, LoadError);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, LoadError);
		return;
	}

	UObject* Node = FindNodeById(Graph, NodeId);
	if (!Node)
	{
		UAL_CommandUtils::SendError(RequestId, 404,
			FString::Printf(TEXT("No node '%s' in %s. Call pcg.get_graph for the current node ids."),
				*NodeId, *GraphPath));
		return;
	}

	UObject* Settings = GetNodeSettings(Node);
	if (!Settings)
	{
		UAL_CommandUtils::SendError(RequestId, 500,
			FString::Printf(TEXT("Node '%s' has no settings object"), *NodeId));
		return;
	}

	int32 MaxEntries = 200;
	Payload->TryGetNumberField(TEXT("limit"), MaxEntries);
	MaxEntries = FMath::Clamp(MaxEntries, 1, 1000);

	TMap<FString, TSharedPtr<FJsonValue>> Flat;
	UALPropertyPath::FlattenProperties(Settings, Flat, /*MaxDepth=*/4, MaxEntries);

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	for (const auto& Pair : Flat)
	{
		Properties->SetField(Pair.Key, Pair.Value);
	}

	TSharedPtr<FJsonObject> Data = BuildNodeJson(Node);
	Data->SetStringField(TEXT("graph_path"), GraphPath);
	Data->SetObjectField(TEXT("properties"), Properties);
	Data->SetNumberField(TEXT("property_count"), Flat.Num());
	if (Flat.Num() >= MaxEntries)
	{
		// 截断了就说清楚。默默截断会让调用方以为「就这些」，
		// 然后拿一份残缺的配置去整图重写
		Data->SetStringField(TEXT("truncated_hint"),
			FString::Printf(TEXT("Hit the %d-property limit; raise 'limit' to see the rest."), MaxEntries));
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ======================================================================
// 图的用户参数（细节面板上那几个可调项）
// ======================================================================

/**
 * ## 为什么这一段是唯一不走反射的 PCG 代码
 *
 * 这个文件其他地方一律靠反射够 PCG（见头文件开头那段）。参数包够不着：
 *
 *   - `UPCGGraph::AddUserParameters` 是**普通 C++ 函数**，没有 UFUNCTION 标记，
 *     反射和 Python 都调不到；
 *   - `FInstancedPropertyBag` 没有 `ImportTextItem`，也没有任何 BlueprintCallable
 *     的创建入口，所以「用文本把参数导进去」这条路是死的（实测导完回读是空包）；
 *   - `PCGGraphParametersHelpers::SetFloatParameter` 只能改**已存在**的参数，
 *     参数不存在直接返回 PropertyNotFound。
 *
 * 但 `UserParameters` 本身是 `UPROPERTY(EditAnywhere)`（引擎 PCGGraph.h），
 * 所以**反射能拿到它的地址**，拿到之后就是一个普通的 `FInstancedPropertyBag`。
 * 而 `FInstancedPropertyBag` 从 **5.5 起在 CoreUObject 里**，我们本来就依赖
 * CoreUObject —— 这条路一行新依赖都不用加。
 *
 * ## 5.2–5.4 为什么不支持
 *
 * 那三版里 `FInstancedPropertyBag` 还在 `StructUtils` **插件**里，而那个插件
 * `EnabledByDefault: false`。为它加依赖就等于强制用户工程启用一个实验性插件 ——
 * 正是 Build.cs 里为 GameplayTagsEditor 明确否决过的做法。所以那三版如实报
 * 「这个引擎版本上做不了」，并给出替代方案，而不是假装支持。
 *
 * （版本开关和头文件在本文件顶部，`UAL_PCG_GRAPH_PARAMS_SUPPORTED`。）
 */
#if UAL_PCG_GRAPH_PARAMS_SUPPORTED
namespace
{
	/** 工具层认的类型名 → 参数包的类型。写法尽量宽松，模型爱写 int 也爱写 int32 */
	bool ParseBagType(const FString& Raw, EPropertyBagPropertyType& OutType)
	{
		const FString Name = Raw.ToLower();
		if (Name == TEXT("bool")) { OutType = EPropertyBagPropertyType::Bool; return true; }
		if (Name == TEXT("int") || Name == TEXT("int32")) { OutType = EPropertyBagPropertyType::Int32; return true; }
		if (Name == TEXT("int64")) { OutType = EPropertyBagPropertyType::Int64; return true; }
		if (Name == TEXT("float")) { OutType = EPropertyBagPropertyType::Float; return true; }
		if (Name == TEXT("double") || Name == TEXT("number")) { OutType = EPropertyBagPropertyType::Double; return true; }
		if (Name == TEXT("name")) { OutType = EPropertyBagPropertyType::Name; return true; }
		if (Name == TEXT("string")) { OutType = EPropertyBagPropertyType::String; return true; }
		if (Name == TEXT("text")) { OutType = EPropertyBagPropertyType::Text; return true; }
		if (Name == TEXT("object")) { OutType = EPropertyBagPropertyType::Object; return true; }
		if (Name == TEXT("softobject")) { OutType = EPropertyBagPropertyType::SoftObject; return true; }
		if (Name == TEXT("class")) { OutType = EPropertyBagPropertyType::Class; return true; }
		return false;
	}

	const TCHAR* BagTypeName(EPropertyBagPropertyType Type)
	{
		switch (Type)
		{
		case EPropertyBagPropertyType::Bool:        return TEXT("bool");
		case EPropertyBagPropertyType::Int32:       return TEXT("int32");
		case EPropertyBagPropertyType::Int64:       return TEXT("int64");
		case EPropertyBagPropertyType::Float:       return TEXT("float");
		case EPropertyBagPropertyType::Double:      return TEXT("double");
		case EPropertyBagPropertyType::Name:        return TEXT("name");
		case EPropertyBagPropertyType::String:      return TEXT("string");
		case EPropertyBagPropertyType::Text:        return TEXT("text");
		case EPropertyBagPropertyType::Object:      return TEXT("object");
		case EPropertyBagPropertyType::SoftObject:  return TEXT("softobject");
		case EPropertyBagPropertyType::Class:       return TEXT("class");
		default:                                    return TEXT("unsupported");
		}
	}

	/** 这些类型要一个「值的类型对象」，不给的话参数包建不出属性来 */
	const UObject* BagTypeObjectFor(EPropertyBagPropertyType Type)
	{
		switch (Type)
		{
		case EPropertyBagPropertyType::Object:
		case EPropertyBagPropertyType::SoftObject:
			return UObject::StaticClass();
		case EPropertyBagPropertyType::Class:
			return UObject::StaticClass();
		default:
			return nullptr;
		}
	}

	/** 参数当前的值 → JSON。读不出来就给 null，不编一个默认值糊弄过去 */
	TSharedPtr<FJsonValue> ReadBagValue(const FInstancedPropertyBag& Bag, const FPropertyBagPropertyDesc& Desc)
	{
		switch (Desc.ValueType)
		{
		case EPropertyBagPropertyType::Bool:
			if (auto R = Bag.GetValueBool(Desc.Name); R.HasValue()) return MakeShared<FJsonValueBoolean>(R.GetValue());
			break;
		case EPropertyBagPropertyType::Int32:
			if (auto R = Bag.GetValueInt32(Desc.Name); R.HasValue()) return MakeShared<FJsonValueNumber>(R.GetValue());
			break;
		case EPropertyBagPropertyType::Int64:
			if (auto R = Bag.GetValueInt64(Desc.Name); R.HasValue()) return MakeShared<FJsonValueNumber>(static_cast<double>(R.GetValue()));
			break;
		case EPropertyBagPropertyType::Float:
			if (auto R = Bag.GetValueFloat(Desc.Name); R.HasValue()) return MakeShared<FJsonValueNumber>(R.GetValue());
			break;
		case EPropertyBagPropertyType::Double:
			if (auto R = Bag.GetValueDouble(Desc.Name); R.HasValue()) return MakeShared<FJsonValueNumber>(R.GetValue());
			break;
		case EPropertyBagPropertyType::Name:
			if (auto R = Bag.GetValueName(Desc.Name); R.HasValue()) return MakeShared<FJsonValueString>(R.GetValue().ToString());
			break;
		case EPropertyBagPropertyType::String:
			if (auto R = Bag.GetValueString(Desc.Name); R.HasValue()) return MakeShared<FJsonValueString>(R.GetValue());
			break;
		case EPropertyBagPropertyType::Text:
			if (auto R = Bag.GetValueText(Desc.Name); R.HasValue()) return MakeShared<FJsonValueString>(R.GetValue().ToString());
			break;
		case EPropertyBagPropertyType::Object:
		case EPropertyBagPropertyType::Class:
			if (auto R = Bag.GetValueObject(Desc.Name); R.HasValue())
			{
				return MakeShared<FJsonValueString>(R.GetValue() ? R.GetValue()->GetPathName() : TEXT("None"));
			}
			break;
		case EPropertyBagPropertyType::SoftObject:
			if (auto R = Bag.GetValueSoftPath(Desc.Name); R.HasValue()) return MakeShared<FJsonValueString>(R.GetValue().ToString());
			break;
		default:
			break;
		}
		return MakeShared<FJsonValueNull>();
	}

	/** 把 JSON 值写进参数。写不进去时 OutError 说清是类型不对还是加载不到 */
	bool WriteBagValue(
		FInstancedPropertyBag& Bag,
		const FPropertyBagPropertyDesc& Desc,
		const TSharedPtr<FJsonValue>& Value,
		FString& OutError)
	{
		EPropertyBagResult Result = EPropertyBagResult::PropertyNotFound;

		switch (Desc.ValueType)
		{
		case EPropertyBagPropertyType::Bool:
			Result = Bag.SetValueBool(Desc.Name, Value->AsBool());
			break;
		case EPropertyBagPropertyType::Int32:
			Result = Bag.SetValueInt32(Desc.Name, static_cast<int32>(Value->AsNumber()));
			break;
		case EPropertyBagPropertyType::Int64:
			Result = Bag.SetValueInt64(Desc.Name, static_cast<int64>(Value->AsNumber()));
			break;
		case EPropertyBagPropertyType::Float:
			Result = Bag.SetValueFloat(Desc.Name, static_cast<float>(Value->AsNumber()));
			break;
		case EPropertyBagPropertyType::Double:
			Result = Bag.SetValueDouble(Desc.Name, Value->AsNumber());
			break;
		case EPropertyBagPropertyType::Name:
			Result = Bag.SetValueName(Desc.Name, FName(*Value->AsString()));
			break;
		case EPropertyBagPropertyType::String:
			Result = Bag.SetValueString(Desc.Name, Value->AsString());
			break;
		case EPropertyBagPropertyType::Text:
			Result = Bag.SetValueText(Desc.Name, FText::FromString(Value->AsString()));
			break;
		case EPropertyBagPropertyType::Object:
		case EPropertyBagPropertyType::Class:
		{
			// 资产路径要能加载得到才算写成功。加载不到就直说 ——
			// 悄悄写个 null 进去，用户在细节面板上看到的是空，谁也不知道为什么
			const FString Path = Value->AsString();
			UObject* Loaded = Path.IsEmpty() || Path == TEXT("None")
				? nullptr
				: FSoftObjectPath(Path).TryLoad();
			if (!Path.IsEmpty() && Path != TEXT("None") && !Loaded)
			{
				OutError = FString::Printf(TEXT("could not load '%s'"), *Path);
				return false;
			}
			Result = Bag.SetValueObject(Desc.Name, Loaded);
			break;
		}
		case EPropertyBagPropertyType::SoftObject:
			Result = Bag.SetValueSoftPath(Desc.Name, FSoftObjectPath(Value->AsString()));
			break;
		default:
			OutError = FString::Printf(TEXT("type '%s' cannot be written by this command"), BagTypeName(Desc.ValueType));
			return false;
		}

		if (Result != EPropertyBagResult::Success)
		{
			OutError = FString::Printf(
				TEXT("the bag refused the value (is it the right type for a '%s' parameter?)"),
				BagTypeName(Desc.ValueType));
			return false;
		}
		OutError.Reset();
		return true;
	}
}
#endif // UAL_PCG_GRAPH_PARAMS_SUPPORTED

void FUAL_PCGCommands::Handle_GraphParameters(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!RequirePCG(RequestId))
	{
		return;
	}

	FString GraphPath;
	if (!Payload->TryGetStringField(TEXT("graph_path"), GraphPath) || GraphPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: graph_path"));
		return;
	}
	GraphPath = NormalizePath(GraphPath);

	FString LoadError;
	UObject* Graph = LoadGraph(GraphPath, LoadError);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, LoadError);
		return;
	}

#if !UAL_PCG_GRAPH_PARAMS_SUPPORTED
	UAL_CommandUtils::SendError(
		RequestId,
		501,
		TEXT("Graph user parameters need FInstancedPropertyBag, which lives in the StructUtils plugin ")
		TEXT("(off by default) before UE 5.5 - linking it would force every project to enable an ")
		TEXT("experimental plugin, so this command is 5.5+ only. On this engine, expose the values on a ")
		TEXT("Blueprint actor as instance-editable variables and read them in the graph with ")
		TEXT("PCGGetActorPropertySettings."));
#else
	FStructProperty* BagProp = CastField<FStructProperty>(
		Graph->GetClass()->FindPropertyByName(TEXT("UserParameters")));
	if (!BagProp)
	{
		UAL_CommandUtils::SendError(
			RequestId,
			501,
			TEXT("This PCG version has no UserParameters property on UPCGGraph."));
		return;
	}
	FInstancedPropertyBag* Bag = BagProp->ContainerPtrToValuePtr<FInstancedPropertyBag>(Graph);

	TArray<TSharedPtr<FJsonValue>> AddedJson;
	TArray<TSharedPtr<FJsonValue>> UpdatedJson;
	TArray<TSharedPtr<FJsonValue>> RemovedJson;
	TArray<TSharedPtr<FJsonValue>> FailedJson;

	const auto Fail = [&FailedJson](const FString& Name, const FString& Error)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Name);
		Entry->SetStringField(TEXT("error"), Error);
		FailedJson.Add(MakeShared<FJsonValueObject>(Entry));
	};

	// 改之前先记一笔，否则改完不会进 undo，也不会被 ue_save 认作「你改脏的」
	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALPCG", "EditPCGGraphParameters", "Edit PCG Graph Parameters"));
	Graph->Modify();

	// ---- 删 ----
	const TArray<TSharedPtr<FJsonValue>>* RemoveArray = nullptr;
	if (Payload->TryGetArrayField(TEXT("remove"), RemoveArray) && RemoveArray)
	{
		TArray<FName> ToRemove;
		for (int32 Index = 0; Index < RemoveArray->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Item = (*RemoveArray)[Index];
			FString Name;
			if (!Item.IsValid() || !Item->TryGetString(Name) || Name.IsEmpty())
			{
				// 格式不对的也要记。上一版静默跳过，调用方以为删了
				Fail(FString::Printf(TEXT("remove[%d]"), Index), TEXT("entry is not a non-empty string"));
				continue;
			}
			if (!Bag->FindPropertyDescByName(FName(*Name)))
			{
				Fail(Name, TEXT("no such parameter on this graph"));
				continue;
			}
			ToRemove.Add(FName(*Name));
		}
		if (ToRemove.Num() > 0)
		{
			Bag->RemovePropertiesByName(ToRemove);

			// 删完逐个回读，确实不在了才算删掉。上一版在调用**之前**就把名字塞进
			// removed，删没删成都报「删掉了」
			for (const FName& Name : ToRemove)
			{
				if (Bag->FindPropertyDescByName(Name))
				{
					Fail(Name.ToString(), TEXT("still on the graph after removal"));
					continue;
				}
				RemovedJson.Add(MakeShared<FJsonValueString>(Name.ToString()));
			}
		}
	}

	// ---- 加 / 改 ----
	const TArray<TSharedPtr<FJsonValue>>* ParamArray = nullptr;
	if (Payload->TryGetArrayField(TEXT("parameters"), ParamArray) && ParamArray)
	{
		for (int32 Index = 0; Index < ParamArray->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Item = (*ParamArray)[Index];
			const TSharedPtr<FJsonObject>* Spec = nullptr;
			if (!Item.IsValid() || !Item->TryGetObject(Spec) || !Spec)
			{
				Fail(FString::Printf(TEXT("parameters[%d]"), Index), TEXT("entry is not an object"));
				continue;
			}

			FString Name;
			if (!(*Spec)->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
			{
				Fail(TEXT("(unnamed)"), TEXT("every entry needs a 'name'"));
				continue;
			}
			const FName ParamName(*Name);

			FString TypeText;
			(*Spec)->TryGetStringField(TEXT("type"), TypeText);

			const FPropertyBagPropertyDesc* Existing = Bag->FindPropertyDescByName(ParamName);

			// 「新建 1 个（X），改值 1 个（X）」读起来像动了两个参数，其实只有一个。
			// 带初值新建就是一件事，别在回执里数两遍（2026-09-18 反馈）
			bool bJustAdded = false;

			if (!Existing)
			{
				if (TypeText.IsEmpty())
				{
					Fail(Name, TEXT("this parameter does not exist yet, so 'type' is required to create it"));
					continue;
				}
				EPropertyBagPropertyType Type = EPropertyBagPropertyType::None;
				if (!ParseBagType(TypeText, Type))
				{
					Fail(Name, FString::Printf(
						TEXT("unknown type '%s'. Use one of: bool, int32, int64, float, double, name, string, text, object, softobject, class"),
						*TypeText));
					continue;
				}

				Bag->AddProperties({ FPropertyBagPropertyDesc(ParamName, Type, BagTypeObjectFor(Type)) });

				// 加完立刻回读确认它真的在包里了 —— AddProperties 没有返回值，
				// 不回读的话「名字非法被静默丢掉」会被报成成功
				if (!Bag->FindPropertyDescByName(ParamName))
				{
					Fail(Name, TEXT("the bag did not accept this parameter (is the name a valid identifier?)"));
					continue;
				}
				AddedJson.Add(MakeShared<FJsonValueString>(Name));
				bJustAdded = true;
			}
			else if (!TypeText.IsEmpty())
			{
				// 类型对不上要说出来。默默按旧类型写，用户在面板上看到的还是旧类型，
				// 而模型以为自己改成功了
				EPropertyBagPropertyType Wanted = EPropertyBagPropertyType::None;
				if (ParseBagType(TypeText, Wanted) && Wanted != Existing->ValueType)
				{
					Fail(Name, FString::Printf(
						TEXT("already exists as '%s'; remove it first if you really want it to be '%s'"),
						BagTypeName(Existing->ValueType), *TypeText));
					continue;
				}
			}

			const TSharedPtr<FJsonValue> Value = (*Spec)->TryGetField(TEXT("value"));
			if (Value.IsValid() && Value->Type != EJson::Null)
			{
				const FPropertyBagPropertyDesc* Desc = Bag->FindPropertyDescByName(ParamName);
				FString WriteError;
				if (!Desc || !WriteBagValue(*Bag, *Desc, Value, WriteError))
				{
					Fail(Name, WriteError.IsEmpty() ? TEXT("could not write the value") : WriteError);
					continue;
				}
				if (!bJustAdded)
				{
					UpdatedJson.Add(MakeShared<FJsonValueString>(Name));
				}
			}
		}
	}

	/**
	 * 改完必须发这一下，否则等于没改。
	 *
	 * `UPCGGraph::PostEditChangeProperty` 认出 UserParameters 之后会算出
	 * Added / Removed / Renamed 并调 `OnGraphParametersChanged` —— 那是把改动
	 * 传播到所有图实例和 PCG 组件的唯一入口。我们是绕过细节面板直接往包里写的，
	 * 不补这一下：资产里有参数，细节面板上不出现，组件也拿不到。
	 */
	if (AddedJson.Num() > 0 || UpdatedJson.Num() > 0 || RemovedJson.Num() > 0)
	{
		FPropertyChangedEvent ChangedEvent(BagProp, EPropertyChangeType::ValueSet);
		Graph->PostEditChangeProperty(ChangedEvent);
	}

	// ---- 回读整份清单 ----
	// 不管这次干了什么都全量回读：调用方拿到的永远是**现在真实的样子**，
	// 而不是「我以为我改成了什么」
	TArray<TSharedPtr<FJsonValue>> ParamsJson;
	if (const UPropertyBag* BagStruct = Bag->GetPropertyBagStruct())
	{
		for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Desc.Name.ToString());
			Entry->SetStringField(TEXT("type"), BagTypeName(Desc.ValueType));
			Entry->SetField(TEXT("value"), ReadBagValue(*Bag, Desc));
			ParamsJson.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("graph_path"), GraphPath);
	Data->SetArrayField(TEXT("parameters"), ParamsJson);
	Data->SetNumberField(TEXT("parameter_count"), ParamsJson.Num());
	Data->SetArrayField(TEXT("added"), AddedJson);
	Data->SetArrayField(TEXT("updated"), UpdatedJson);
	Data->SetArrayField(TEXT("removed"), RemovedJson);
	if (FailedJson.Num() > 0)
	{
		Data->SetArrayField(TEXT("failed"), FailedJson);
		Data->SetNumberField(TEXT("failed_count"), FailedJson.Num());
	}

	// 要求改了东西、却一件都没办成 —— 整体失败，不回 207。
	// 「部分完成：0 成功」读起来还是像办了点什么
	const bool bNothingDone = AddedJson.Num() == 0 && UpdatedJson.Num() == 0 && RemovedJson.Num() == 0;
	if (FailedJson.Num() > 0 && bNothingDone)
	{
		// 回读确认过一件没成，包是原样的，这时丢掉空的撤销记录才对
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 400,
			FString::Printf(TEXT("None of the %d requested parameter change(s) could be made; see failed. ")
				TEXT("parameters lists what the graph has now."),
				FailedJson.Num()),
			Data);
		return;
	}

	UAL_CommandUtils::SendResponse(RequestId, FailedJson.Num() > 0 ? 207 : 200, Data);
#endif // UAL_PCG_GRAPH_PARAMS_SUPPORTED
}

// ======================================================================
// 场景环境报告
// ======================================================================

void FUAL_PCGCommands::Handle_SceneReport(const TSharedPtr<FJsonObject>& /*Payload*/, const FString RequestId)
{
	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("No editor world is open."));
		return;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	// ---- 我在哪 ----
	Data->SetStringField(TEXT("level_package"), World->GetOutermost()->GetName());
	Data->SetStringField(TEXT("level_name"), World->GetName());
	const bool bPartitioned = World->IsPartitionedWorld();
	Data->SetBoolField(TEXT("world_partition"), bPartitioned);
	// 临时关卡（/Temp/Untitled_N）是最容易「不知不觉跑错地方」的一类：
	// 它没落盘、名字看着人畜无害，却可能是从模板来的整套大世界
	const bool bTemp = World->GetOutermost()->GetName().StartsWith(TEXT("/Temp/"));
	Data->SetBoolField(TEXT("unsaved_temp_level"), bTemp);

	// ---- 地形长什么样 ----
	// 主地形和流送代理要分开数：PCG 采不到没流送进来的代理，
	// 而「有 64 块代理但一块没加载」和「有一整块主地形」表现完全不同
	int32 MainLandscapes = 0;
	int32 StreamingProxies = 0;
	TArray<TSharedPtr<FJsonValue>> LandscapeNames;

	UClass* LandscapeClass = UALReflect::FindScriptClass(TEXT("/Script/Landscape.Landscape"));
	UClass* ProxyClass = UALReflect::FindScriptClass(TEXT("/Script/Landscape.LandscapeStreamingProxy"));

	if (LandscapeClass)
	{
		for (TActorIterator<AActor> It(World, LandscapeClass); It; ++It)
		{
			++MainLandscapes;
			if (LandscapeNames.Num() < 8)
			{
				LandscapeNames.Add(MakeShared<FJsonValueString>(It->GetActorLabel()));
			}
		}
	}
	if (ProxyClass)
	{
		for (TActorIterator<AActor> It(World, ProxyClass); It; ++It)
		{
			++StreamingProxies;
		}
	}

	TSharedPtr<FJsonObject> Landscape = MakeShared<FJsonObject>();
	Landscape->SetNumberField(TEXT("main_landscapes"), MainLandscapes);
	Landscape->SetNumberField(TEXT("streaming_proxies_loaded"), StreamingProxies);
	Landscape->SetArrayField(TEXT("names"), LandscapeNames);
	Data->SetObjectField(TEXT("landscape"), Landscape);

	// ---- 关卡里有哪些 PCG 体积 ----
	TArray<TSharedPtr<FJsonValue>> Volumes;
	if (UClass* VolumeClass = UALReflect::FindScriptClass(PCG_VOLUME_CLASS))
	{
		for (TActorIterator<AActor> It(World, VolumeClass); It; ++It)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("actor_label"), It->GetActorLabel());

			if (UObject* Component = UALReflect::GetObjectProp(*It, TEXT("PCGComponent")))
			{
				bool bComponentPartitioned = false;
				UALReflect::GetBoolProp(Component, TEXT("bIsComponentPartitioned"), bComponentPartitioned);
				Entry->SetBoolField(TEXT("partitioned"), bComponentPartitioned);

				// 读 UPROPERTY 链，不走 GetGraph()：那个方法在 5.5 上**不是 UFUNCTION**
				// （5.8 才加的标记），反射找不到，于是挂了图的体积会被报成「未挂图」——
				// 一个假的失败提示比没有提示更糟
				UObject* Instance = UALReflect::GetObjectProp(Component, TEXT("GraphInstance"));
				UObject* AssignedGraph = Instance ? UALReflect::GetObjectProp(Instance, TEXT("Graph")) : nullptr;
				Entry->SetStringField(TEXT("graph"),
					AssignedGraph ? AssignedGraph->GetPathName() : TEXT("(none assigned)"));
			}

			const FBox Bounds = It->GetComponentsBoundingBox();
			Entry->SetObjectField(TEXT("center"), UAL_CommandUtils::MakeVectorJson(Bounds.GetCenter()));
			Entry->SetObjectField(TEXT("extent"), UAL_CommandUtils::MakeVectorJson(Bounds.GetExtent()));
			Volumes.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}
	Data->SetArrayField(TEXT("pcg_volumes"), Volumes);

	// ---- 结论：这里 PCG 采得到东西吗 ----
	TArray<FString> Notes;
	if (bTemp)
	{
		Notes.Add(TEXT("This is an unsaved temporary level (/Temp/...). If you did not deliberately "
					   "create it, the editor is not on the level you think it is - that alone has cost "
					   "whole debugging sessions."));
	}
	if (bPartitioned)
	{
		Notes.Add(FString::Printf(
			TEXT("World Partition level with %d landscape streaming proxies currently loaded. "
				 "PCG only sees loaded proxies; unloaded cells are not in the world at all."),
			StreamingProxies));
	}
	if (MainLandscapes == 0 && StreamingProxies == 0)
	{
		Notes.Add(TEXT("There is no landscape in this level at all, so a surface sampler has nothing "
					   "to sample. Use PCGCreatePointsGrid for pure data, or open a level that has terrain."));
	}
	else
	{
		// 这条是实测里花了最多轮才摸出来的：SurfaceSampler 直连 Input 拿不到地形，
		// 必须显式用 PCGGetLandscape 当数据源接到 Surface 引脚
		Notes.Add(TEXT("To sample this landscape, do NOT wire the graph Input straight into a surface "
					   "sampler - it will report 'no surface found'. Add a PCGGetLandscape node and wire "
					   "it into the sampler's 'Surface' pin: PCGGetLandscape -> SurfaceSampler.Surface "
					   "-> StaticMeshSpawner -> Output."));
	}

	TArray<TSharedPtr<FJsonValue>> NotesJson;
	for (const FString& Note : Notes)
	{
		NotesJson.Add(MakeShared<FJsonValueString>(Note));
	}
	Data->SetArrayField(TEXT("notes"), NotesJson);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ======================================================================
// 注册
// ======================================================================

void FUAL_PCGCommands::RegisterCommands(
	TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	// 用 lambda 而不是函数指针，跟其它命令模块保持一致
	CommandMap.Add(TEXT("pcg.status"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_Status(Payload, RequestId);
	});

	CommandMap.Add(TEXT("pcg.create_graph"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CreateGraph(Payload, RequestId);
	});

	CommandMap.Add(TEXT("pcg.get_graph"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetGraph(Payload, RequestId);
	});

	CommandMap.Add(TEXT("pcg.list_node_types"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ListNodeTypes(Payload, RequestId);
	});

	CommandMap.Add(TEXT("pcg.get_node_schema"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetNodeSchema(Payload, RequestId);
	});

	CommandMap.Add(TEXT("pcg.add_node"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_AddNode(Payload, RequestId);
	});

	CommandMap.Add(TEXT("pcg.update_node"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_UpdateNode(Payload, RequestId);
	});

	CommandMap.Add(TEXT("pcg.remove_node"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_RemoveNode(Payload, RequestId);
	});

	CommandMap.Add(TEXT("pcg.connect_pins"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ConnectPins(Payload, RequestId);
	});

	CommandMap.Add(TEXT("pcg.disconnect_pins"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DisconnectPins(Payload, RequestId);
	});

	CommandMap.Add(TEXT("pcg.set_node_positions"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetNodePositions(Payload, RequestId);
	});

	CommandMap.Add(TEXT("pcg.spawn_volume"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SpawnVolume(Payload, RequestId);
	});

	CommandMap.Add(TEXT("pcg.execute"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_Execute(Payload, RequestId);
	});

	CommandMap.Add(TEXT("pcg.apply_graph"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ApplyGraph(Payload, RequestId);
	});

	CommandMap.Add(TEXT("pcg.diagnose"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_Diagnose(Payload, RequestId);
	});

	CommandMap.Add(TEXT("pcg.graph_parameters"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GraphParameters(Payload, RequestId);
	});

	CommandMap.Add(TEXT("pcg.describe_node"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DescribeNode(Payload, RequestId);
	});

	CommandMap.Add(TEXT("pcg.scene_report"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SceneReport(Payload, RequestId);
	});
}
