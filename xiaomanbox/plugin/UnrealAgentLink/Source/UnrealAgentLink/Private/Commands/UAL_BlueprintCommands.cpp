#include "UAL_BlueprintCommands.h"
#include "UAL_CommandUtils.h"

#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
// search_nodes 要把控件树里的控件类也纳入搜索范围（见 UAL_CollectOwnedClasses）
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "AssetToolsModule.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "Engine/LevelScriptActor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UAL_ReflectCall.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "UAL_ScopedTransaction.h"
#include "UAL_TouchedPackages.h"
// blueprint.export_t3d / import_t3d —— 引擎自己的节点序列化（编辑器 Ctrl+C/V 走的同一对）
#include "EdGraphUtilities.h"
#include "UObject/UObjectHash.h"
// 遍历全部蓝图函数库要用（见 UAL_ForEachFunctionSource）
#include "UObject/UObjectIterator.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "K2Node.h"
#include "K2Node_Event.h"
// 组件事件绑定（blueprint.component_event）—— 触发式交互的入口
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CallFunction.h"
// 特种函数调用节点：数组库 / 数据表 / 材质参数集。
// 不是可选的美化 —— 用错类的话引脚永远定不了型，见 UAL_PickCallFunctionNodeClass
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallDataTableFunction.h"
#include "K2Node_CallMaterialParameterCollectionFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_InputAction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_Select.h"
#include "K2Node_CustomEvent.h"
// 事件分发器的四个节点。它们不是函数调用 —— 见 UAL_DelegateNodeName.h
#include "UAL_DelegateNodeName.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Timeline.h"
#include "K2Node_Self.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "Logging/TokenizedMessage.h"
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
#include "Misc/UObjectToken.h"
#endif
// Misc/Variant.h - removed due to compatibility issues

DEFINE_LOG_CATEGORY_STATIC(LogUALBlueprint, Log, All);

void FUAL_BlueprintCommands::RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	// blueprint.describe - 获取蓝图完整结构信息
	CommandMap.Add(TEXT("blueprint.describe"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DescribeBlueprint(Payload, RequestId);
	});
	
	CommandMap.Add(TEXT("blueprint.create"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CreateBlueprint(Payload, RequestId);
	});
	
	CommandMap.Add(TEXT("blueprint.add_component"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_AddComponentToBlueprint(Payload, RequestId);
	});
	
	CommandMap.Add(TEXT("blueprint.set_property"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetBlueprintProperty(Payload, RequestId);
	});

	CommandMap.Add(TEXT("blueprint.add_variable"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_AddVariableToBlueprint(Payload, RequestId);
	});

	CommandMap.Add(TEXT("blueprint.get_graph"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetBlueprintGraph(Payload, RequestId);
	});

	CommandMap.Add(TEXT("blueprint.list_graphs"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ListBlueprintGraphs(Payload, RequestId);
	});

	CommandMap.Add(TEXT("blueprint.create_function"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CreateFunctionGraph(Payload, RequestId);
	});

	CommandMap.Add(TEXT("blueprint.compile"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CompileBlueprint(Payload, RequestId);
	});

	CommandMap.Add(TEXT("blueprint.delete_node"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DeleteNode(Payload, RequestId);
	});

	CommandMap.Add(TEXT("blueprint.disconnect_pins"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DisconnectBlueprintPins(Payload, RequestId);
	});

	// blueprint.create_graph - 声明式整图写入（全有或全无）
	CommandMap.Add(TEXT("blueprint.create_graph"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CreateGraphDeclarative(Payload, RequestId);
	});

	// blueprint.search_nodes - 只读地查函数与引脚签名，写之前先查一批
	CommandMap.Add(TEXT("blueprint.search_nodes"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SearchBlueprintNodes(Payload, RequestId);
	});

	// blueprint.compile_all - 批量编译
	CommandMap.Add(TEXT("blueprint.compile_all"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CompileAllBlueprints(Payload, RequestId);
	});

	// blueprint.set_variable_meta - 改变量在细节面板里的表现（暴露/只读/分类/提示）
	CommandMap.Add(TEXT("blueprint.set_variable_meta"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetVariableMeta(Payload, RequestId);
	});

	// blueprint.remove_variable - 删变量，和 add_variable 配对
	CommandMap.Add(TEXT("blueprint.remove_variable"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_RemoveVariable(Payload, RequestId);
	});

	// blueprint.event_dispatcher - 建 / 列事件分发器
	CommandMap.Add(TEXT("blueprint.event_dispatcher"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_EventDispatcher(Payload, RequestId);
	});

	// blueprint.component_event - 列 / 绑组件事件（触发式交互的入口）
	CommandMap.Add(TEXT("blueprint.component_event"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ComponentEvent(Payload, RequestId);
	});

	// blueprint.function_signature - 改函数参数表 / 删函数
	CommandMap.Add(TEXT("blueprint.function_signature"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_FunctionSignature(Payload, RequestId);
	});

	// blueprint.set_parent_class - 改父类
	CommandMap.Add(TEXT("blueprint.set_parent_class"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetParentClass(Payload, RequestId);
	});

	// blueprint.set_node_positions - 批量挪节点（排版落地）
	CommandMap.Add(TEXT("blueprint.set_comment"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetComment(Payload, RequestId);
	});

	CommandMap.Add(TEXT("blueprint.set_node_positions"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetNodePositions(Payload, RequestId);
	});

	// blueprint.export_t3d / import_t3d - 走引擎自己的复制粘贴序列化（路线验证中）
	CommandMap.Add(TEXT("blueprint.export_t3d"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ExportNodesT3D(Payload, RequestId);
	});

	CommandMap.Add(TEXT("blueprint.import_t3d"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ImportNodesT3D(Payload, RequestId);
	});
}

// ============================================================================
// 图表/节点/变量辅助函数（仅本 CPP 内使用）
// ============================================================================

static FString UAL_GuidToString(const FGuid& Guid)
{
	return Guid.ToString(EGuidFormats::DigitsWithHyphens);
}

static FString UAL_PinDirToString(const EEdGraphPinDirection Dir)
{
	return Dir == EGPD_Input ? TEXT("Input") : TEXT("Output");
}

/**
 * 这张图是不是「空的」—— `require_empty` 的判据。
 *
 * ## 为什么不能用 `CanUserDeleteNode()`
 *
 * 那个方法答的是「用户能不能删这个节点」，**推不出「里面没有用户的逻辑」**。
 * 最直接的反例是 `UK2Node_Composite`：它有一个 `BoundGraph`，用户折叠进去的
 * 整片逻辑就装在里面。把它当骨架豁免掉，等于对着一张实际装满逻辑的图说
 * 「这是空的」，然后往里写。`MacroInstance`、`Tunnel` 同理。
 *
 * 所以这里用**正向白名单**：只认 FunctionEntry / FunctionResult 两种骨架，
 * 其余任何节点一律算非空。
 *
 * ## 事件图的骨架是「占位事件节点」，不是空
 *
 * 上面那个白名单是照函数图写的。事件图没有 Entry / Result ——
 * 它一建出来就带着引擎自己铺的几个未接线事件节点
 * （Actor 蓝图是 BeginPlay / Tick / ActorBeginOverlap，在编辑器里是灰的）。
 *
 * 只按函数图那套白名单判的话，**任何一张新建蓝图的事件图都不算空**，
 * 而「往全新蓝图的事件图里放片段」恰恰是最常见的用法。
 * 2026-09-12 真机上就撞到了：往刚建的蓝图粘，拒绝理由是
 * 「graph contains '事件Actor开始重叠' (K2Node_Event)」——
 * 拦的是引擎自己铺的占位节点，不是用户的任何东西。
 *
 * 引擎自己有这个判断：`UEdGraphNode::IsAutomaticallyPlacedGhostNode()`
 * （九个引擎版本上都有；编辑器粘贴事件节点时就是靠它认出该清掉哪个旧的）。
 * 所以白名单再加一条：**占位事件节点算骨架**。
 *
 * 注意这里**只认引擎的判断，不认「灰的」**。用户自己把一个写了逻辑的节点
 * 设成 Disabled 同样是灰的，那是用户的东西，一个字都不能覆盖。
 * `IsAutomaticallyPlacedGhostNode()` 答的是「这个节点是引擎自动铺的、用户没碰过」，
 * 正是需要的那个问题。
 *
 * ## 为什么不能只看连线
 *
 * 引擎把「连线」和「字面量」分开存：`Pin->LinkedTo` 是连线，
 * `DefaultValue` / `DefaultObject` / `DefaultTextValue` 是三个独立的默认值槽位。
 * 只查连线的话，一张「Entry + Result、返回引脚填了 42、没有任何连线」的图
 * 会被判成空图然后被写入。用户定义的参数与返回值另存在
 * `UK2Node_EditablePinBase::UserDefinedPins` 里，也得单独查。
 *
 * 五条检查（函数图）：
 *   ① Entry 的输出引脚没有连线
 *   ② Result 的输入引脚没有连线
 *   ③ Entry 上没有局部变量
 *   ④ Entry / Result 上没有用户定义引脚（= 用户没配过函数签名）
 *   ⑤ Entry / Result 的每个引脚上三个默认值槽位全空
 *
 * ⑤ 在普通自定义函数上其实被 ④ 盖住了（返回值引脚本身就是 UserDefinedPin），
 * 留着是**冗余保险**，用来挡还没枚举到的路径。**它没有被独立验证过。**
 *
 * @param OutReason 非空时写入「为什么判为非空」，直接回给调用方
 */
static bool UAL_IsGraphEmptyForImport(const UEdGraph* Graph, FString& OutReason)
{
	if (!Graph)
	{
		OutReason = TEXT("graph is null");
		return false;
	}

	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		/**
		 * 引擎自动铺的占位事件节点 —— 事件图的骨架，算空。
		 *
		 * 放行它安全，因为「用户往上挂了逻辑」这件事引擎自己会处理掉：
		 * `UEdGraphPin::MakeLinkTo()` 每次连线都会调
		 * `ConvertConnectedGhostNodesToRealNodes()`，把两端的占位节点**当场转正**
		 * （`SetEnabledState(Enabled)`），还会顺着连线递归转正一整条链
		 * （EdGraphPin.cpp:536 / :1808，读的是 5.5 的源码）。
		 * 所以一个还是 ghost 的节点，必然一根线都没连。
		 *
		 * 但引擎的判断本身只看两个标志位
		 * （`!bUserSetEnabledState && EnabledState == Disabled`，EdGraphNode.cpp:1054），
		 * **不看节点里有没有东西**。所以下面那几条检查对 ghost 节点**照样要跑** ——
		 * 万一哪天转正那条路有缺口，连线和字面量两关还能兜住。
		 * 真正没碰过的占位节点跑这几条一定过，不会误拦。
		 */
		const bool bIsGhostEventNode = Node->IsAutomaticallyPlacedGhostNode();

		const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node);
		const UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node);
		if (!Entry && !Result && !bIsGhostEventNode)
		{
			OutReason = FString::Printf(
				TEXT("graph contains '%s' (%s), which is not an empty-graph skeleton node"),
				*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
				*Node->GetClass()->GetName());
			return false;
		}

		// ③ 局部变量挂在 Entry 上
		if (Entry && Entry->LocalVariables.Num() > 0)
		{
			OutReason = FString::Printf(
				TEXT("function entry declares %d local variable(s)"),
				Entry->LocalVariables.Num());
			return false;
		}

		// ④ 函数签名（参数 / 返回值）挂在 UserDefinedPins 上
		if (const UK2Node_EditablePinBase* Editable = Cast<UK2Node_EditablePinBase>(Node))
		{
			if (Editable->UserDefinedPins.Num() > 0)
			{
				OutReason = FString::Printf(
					TEXT("'%s' declares %d user-defined pin(s) - the function signature is already configured"),
					*Node->GetClass()->GetName(),
					Editable->UserDefinedPins.Num());
				return false;
			}
		}

		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}

			// ①②
			if (Pin->LinkedTo.Num() > 0)
			{
				OutReason = FString::Printf(
					TEXT("pin '%s' on '%s' is already connected"),
					*Pin->PinName.ToString(),
					*Node->GetClass()->GetName());
				return false;
			}

			// ⑤ 三个槽位分别查 —— 少查一个就漏一类字面量
			if (!Pin->DefaultValue.IsEmpty() || Pin->DefaultObject != nullptr || !Pin->DefaultTextValue.IsEmpty())
			{
				OutReason = FString::Printf(
					TEXT("pin '%s' on '%s' carries a literal default value"),
					*Pin->PinName.ToString(),
					*Node->GetClass()->GetName());
				return false;
			}
		}
	}

	return true;
}

static UEdGraph* UAL_FindGraph(UBlueprint* Blueprint, const FString& GraphName)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	// 默认：事件图
	if (GraphName.IsEmpty() || GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
	{
		if (UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint))
		{
			return EventGraph;
		}
	}

	TArray<UEdGraph*> AllGraphs;
	// 手动收集所有图表 (所有 UE5 版本通用，GetAllGraphs 不存在)
	AllGraphs.Append(Blueprint->UbergraphPages);
	AllGraphs.Append(Blueprint->FunctionGraphs);
	AllGraphs.Append(Blueprint->DelegateSignatureGraphs);
	AllGraphs.Append(Blueprint->MacroGraphs);
	if (Blueprint->IntermediateGeneratedGraphs.Num() > 0)
	{
		AllGraphs.Append(Blueprint->IntermediateGeneratedGraphs);
	}
	for (UEdGraph* G : AllGraphs)
	{
		if (G && G->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			return G;
		}
	}
	return nullptr;
}

static TSharedPtr<FJsonObject> UAL_BuildGraphRefJson(UEdGraph* Graph, const FString& GraphType)
{
	if (!Graph)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
	GraphObj->SetStringField(TEXT("name"), Graph->GetName());
	GraphObj->SetStringField(TEXT("type"), GraphType);
	GraphObj->SetStringField(TEXT("title"), Graph->GetName());
	return GraphObj;
}

struct FUAL_GraphDiscoveryResult
{
	TArray<TSharedPtr<FJsonValue>> Graphs;
	TArray<TSharedPtr<FJsonValue>> EventGraphs;
	TArray<TSharedPtr<FJsonValue>> FunctionGraphs;
	TArray<TSharedPtr<FJsonValue>> MacroGraphs;
	TArray<TSharedPtr<FJsonValue>> DelegateGraphs;
	TArray<TSharedPtr<FJsonValue>> IntermediateGraphs;
	TArray<TSharedPtr<FJsonValue>> GraphNames;
	TSet<FString> SeenGraphNames;
	FString PreferredGraphName;

	void AddGraph(
		UEdGraph* Graph,
		const TCHAR* GraphType,
		TArray<TSharedPtr<FJsonValue>>& TypedArray,
		bool bPreferAsDefault = false)
	{
		if (!Graph)
		{
			return;
		}

		const FString GraphName = Graph->GetName();
		const FString GraphKey = GraphName.ToLower();
		if (SeenGraphNames.Contains(GraphKey))
		{
			return;
		}
		SeenGraphNames.Add(GraphKey);

		if (PreferredGraphName.IsEmpty() && (bPreferAsDefault || GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase)))
		{
			PreferredGraphName = GraphName;
		}
		if (PreferredGraphName.IsEmpty() && GraphName.Equals(TEXT("ConstructionScript"), ESearchCase::IgnoreCase))
		{
			PreferredGraphName = GraphName;
		}
		if (PreferredGraphName.IsEmpty())
		{
			PreferredGraphName = GraphName;
		}

		GraphNames.Add(MakeShared<FJsonValueString>(GraphName));

		if (TSharedPtr<FJsonObject> GraphObj = UAL_BuildGraphRefJson(Graph, GraphType))
		{
			Graphs.Add(MakeShared<FJsonValueObject>(GraphObj));
			TypedArray.Add(MakeShared<FJsonValueObject>(GraphObj));
		}
	}
};

static FUAL_GraphDiscoveryResult UAL_CollectBlueprintGraphs(UBlueprint* Blueprint)
{
	FUAL_GraphDiscoveryResult Result;
	if (!Blueprint)
	{
		return Result;
	}

	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		Result.AddGraph(Graph, TEXT("event"), Result.EventGraphs, true);
	}
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		Result.AddGraph(Graph, TEXT("function"), Result.FunctionGraphs);
	}
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		Result.AddGraph(Graph, TEXT("macro"), Result.MacroGraphs);
	}
	for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
	{
		Result.AddGraph(Graph, TEXT("delegate"), Result.DelegateGraphs);
	}
	for (UEdGraph* Graph : Blueprint->IntermediateGeneratedGraphs)
	{
		Result.AddGraph(Graph, TEXT("intermediate"), Result.IntermediateGraphs);
	}

	return Result;
}

static UEdGraphNode* UAL_FindNodeByGuid(UEdGraph* Graph, const FString& NodeId)
{
	if (!Graph || NodeId.IsEmpty())
	{
		return nullptr;
	}
	FGuid Guid;
	if (!FGuid::Parse(NodeId, Guid))
	{
		return nullptr;
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == Guid)
		{
			return Node;
		}
	}
	return nullptr;
}

static UEdGraphPin* UAL_FindPinByName(UEdGraphNode* Node, const FString& PinName)
{
	if (!Node || PinName.IsEmpty())
	{
		return nullptr;
	}
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin)
		{
			continue;
		}
		if (Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
		{
			return Pin;
		}
	}
	return nullptr;
}

static TArray<TSharedPtr<FJsonValue>> UAL_BuildPinsJson(UEdGraphNode* Node)
{
	TArray<TSharedPtr<FJsonValue>> Pins;
	if (!Node)
	{
		return Pins;
	}
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin)
		{
			continue;
		}
		TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
		PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PinObj->SetStringField(TEXT("dir"), UAL_PinDirToString(Pin->Direction));
		PinObj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		PinObj->SetStringField(TEXT("sub_category"), Pin->PinType.PinSubCategory.ToString());
		// UE 5.0+: 使用 ContainerType
		PinObj->SetBoolField(TEXT("is_array"), Pin->PinType.ContainerType == EPinContainerType::Array);
		PinObj->SetBoolField(TEXT("is_reference"), Pin->PinType.bIsReference);
		PinObj->SetBoolField(TEXT("is_const"), Pin->PinType.bIsConst);

		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			if (const UObject* Obj = Pin->PinType.PinSubCategoryObject.Get())
			{
				PinObj->SetStringField(TEXT("sub_category_object"), Obj->GetPathName());
			}
		}

		// 友好名（UI 可能展示的 label），但 Agent 不应当用它做连线依据
		PinObj->SetStringField(TEXT("friendly_name"), Pin->PinFriendlyName.ToString());

		/**
		 * 引脚上的字面量值。
		 *
		 * ## 为什么必须回
		 *
		 * 读图 → 改 → 整图写回，是这套接口的主要用法。而在此之前 get_graph
		 * **一个默认值都不回** —— 于是「读回来改一改写回去」会把图里所有
		 * 字面量（打印的文案、Delay 的秒数、勾选的布尔）**静默清空**，
		 * 因为调用方根本不知道它们存在过。
		 *
		 * 这是真机验证时发现的：想确认 pin_defaults 写进去没有，
		 * 结果返回里压根没有这个字段 —— 我看不到，Agent 同样看不到。
		 *
		 * 三个槽位分开回（对应 TrySetDefaultValue 的三种落值方式），
		 * 且只在非空时带上，避免给每个引脚都塞一个空串。
		 */
		if (!Pin->DefaultValue.IsEmpty())
		{
			PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		}
		if (Pin->DefaultObject)
		{
			PinObj->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
		}
		if (!Pin->DefaultTextValue.IsEmpty())
		{
			PinObj->SetStringField(TEXT("default_text"), Pin->DefaultTextValue.ToString());
		}

		TArray<TSharedPtr<FJsonValue>> LinkedToArray;
		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (!LinkedPin)
			{
				continue;
			}

			TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
			LinkObj->SetStringField(TEXT("pin_name"), LinkedPin->PinName.ToString());
			LinkObj->SetStringField(TEXT("dir"), UAL_PinDirToString(LinkedPin->Direction));
			if (UEdGraphNode* OwnerNode = LinkedPin->GetOwningNode())
			{
				LinkObj->SetStringField(TEXT("node_id"), UAL_GuidToString(OwnerNode->NodeGuid));
				LinkObj->SetStringField(TEXT("node_class"), OwnerNode->GetClass()->GetName());
				LinkObj->SetStringField(TEXT("node_title"), OwnerNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
			}
			LinkedToArray.Add(MakeShared<FJsonValueObject>(LinkObj));
		}
		PinObj->SetArrayField(TEXT("linked_to"), LinkedToArray);
		PinObj->SetNumberField(TEXT("linked_count"), LinkedToArray.Num());
		PinObj->SetBoolField(TEXT("is_connected"), LinkedToArray.Num() > 0);

		Pins.Add(MakeShared<FJsonValueObject>(PinObj));
	}
	return Pins;
}

/**
 * 把一个已有节点**翻译回创建它所需要的参数**。
 *
 * ## 为什么必须有这个
 *
 * 在此之前 get_graph 对一个节点只说三件事：`class`（K2Node_CallFunction）、
 * `title`（"打印字符串" —— 而且是**按编辑器语言本地化**的）、坐标。
 * 三件都不足以把它重新建出来：调用方不知道它调的是哪个函数、
 * 读的是哪个变量、监听的是哪个事件。
 *
 * 于是「读回来改一改整图写回去」这条主要用法根本不成立 ——
 * 能读懂，但写不回去。真机验证时才发现这一点。
 *
 * 现在补两个字段，**直接对应 create_graph 的入参**：
 *   - `write_as`：填进 `class` 的类型名（K2Node_IfThenElse → "Branch"）
 *   - `member_name`：填进 `member_name` 的那个值
 * 另外按需带上 `target_class` / `struct_type`。
 *
 * 认不出来的节点两个字段都不给 —— 宁可让调用方看见「这个节点重建不了」，
 * 也好过给一个猜的值让它写出一张不对的图。
 */
/** 定义在下面类型解析那一节里（和 UAL_ResolvePinType 成对），这里先用 */
static bool UAL_WritePinTypeJson(const FEdGraphPinType& PinType, const TSharedPtr<FJsonObject>& Out);

/**
 * 关键帧插值模式 ↔ JSON 里那个字符串，读写两头共用一份对照。
 *
 * 编辑器曲线面板上的五个选项就是这五个值。`auto` 是 RCIM_Cubic 配自动切线
 * （缓入缓出的那条 S 形），`user` / `break` 是用户手拉过切线的两种 ——
 * 它们的切线值必须一起带走，只回一个 "user" 写回去还是会被引擎重新算平。
 */
static const TCHAR* UAL_CurveInterpName(const FRichCurveKey& Key)
{
	if (Key.InterpMode == RCIM_Constant)
	{
		return TEXT("constant");
	}
	if (Key.InterpMode != RCIM_Cubic)
	{
		return TEXT("linear");
	}
	// RCTM_SmartAuto（5.5 起才有）不点名，落到 auto —— 少一个版本分支
	if (Key.TangentMode == RCTM_User)
	{
		return TEXT("user");
	}
	return Key.TangentMode == RCTM_Break ? TEXT("break") : TEXT("auto");
}

/** 反向：把 `interp` 字符串翻成引擎的两个枚举。认不出来就报错，不静默当线性 */
static bool UAL_CurveInterpFromString(
	const FString& Raw,
	ERichCurveInterpMode& OutInterp,
	ERichCurveTangentMode& OutTangent,
	FString& OutError)
{
	const FString T = Raw.ToLower();
	OutTangent = RCTM_Auto;
	if (T.IsEmpty() || T == TEXT("linear"))
	{
		OutInterp = RCIM_Linear;
		return true;
	}
	if (T == TEXT("constant") || T == TEXT("step"))
	{
		OutInterp = RCIM_Constant;
		return true;
	}
	if (T == TEXT("auto") || T == TEXT("cubic") || T == TEXT("ease"))
	{
		OutInterp = RCIM_Cubic;
		return true;
	}
	if (T == TEXT("user") || T == TEXT("break"))
	{
		OutInterp = RCIM_Cubic;
		OutTangent = (T == TEXT("break")) ? RCTM_Break : RCTM_User;
		return true;
	}
	OutError = FString::Printf(
		TEXT("unknown interp '%s' (linear | constant | auto | user | break)"), *Raw);
	return false;
}

/**
 * 把一个 Timeline 模板翻译回 apply_graph 的 `timeline` 入参。
 *
 * 没有这一段，「读回来改一改写回去」在 Timeline 上是瞎的：引脚说得出
 * 「有一条 Alpha 输出」，说不出它是怎么插值的。看不见现有曲线，调用方
 * 只能重打一条，把用户在编辑器里拉过的切线整条盖掉 —— 而返回里
 * 没有任何迹象说明它盖掉了什么。
 *
 * Vector / Color / Event 轨道不回：写那头只**追加** float 轨道、从不清空
 * 别的类型，所以漏掉它们不会造成静默丢失（对应的输出引脚仍然在 pins 里）。
 */
static TSharedPtr<FJsonObject> UAL_BuildTimelineJson(const UTimelineTemplate* Template)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("length"), Template->TimelineLength);
	Obj->SetBoolField(TEXT("loop"), Template->bLoop != 0);
	Obj->SetBoolField(TEXT("autoplay"), Template->bAutoPlay != 0);
	// 只在非默认时带上。这个模式下引擎忽略 TimelineLength，不说的话
	// 调用方会以为自己写回去的 length 生效了
	if (Template->LengthMode == TL_LastKeyFrame)
	{
		Obj->SetStringField(TEXT("length_mode"), TEXT("last_keyframe"));
	}

	TArray<TSharedPtr<FJsonValue>> Tracks;
	for (const FTTFloatTrack& Track : Template->FloatTracks)
	{
		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("name"), Track.GetTrackName().ToString());

		TArray<TSharedPtr<FJsonValue>> Keys;
		if (Track.CurveFloat)
		{
			for (const FRichCurveKey& Key : Track.CurveFloat->FloatCurve.Keys)
			{
				TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
				KeyObj->SetNumberField(TEXT("time"), Key.Time);
				KeyObj->SetNumberField(TEXT("value"), Key.Value);

				const FString Interp = UAL_CurveInterpName(Key);
				// 线性是默认值，不写；写回来时 interp 缺席就是线性
				if (Interp != TEXT("linear"))
				{
					KeyObj->SetStringField(TEXT("interp"), Interp);
				}
				if (Interp == TEXT("user") || Interp == TEXT("break"))
				{
					KeyObj->SetNumberField(TEXT("arrive_tangent"), Key.ArriveTangent);
					KeyObj->SetNumberField(TEXT("leave_tangent"), Key.LeaveTangent);
				}
				Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
			}
		}
		TrackObj->SetArrayField(TEXT("keys"), Keys);
		Tracks.Add(MakeShared<FJsonValueObject>(TrackObj));
	}
	Obj->SetArrayField(TEXT("tracks"), Tracks);
	return Obj;
}

/**
 * 自定义事件的「复制」选项 —— 细节面板里 Replicates 那个下拉框。
 *
 * 联机版的「客户端→服务器」请求只能靠它：蓝图里 RPC 就是一个打了网络标志的
 * 自定义事件。编辑器没有公开的 setter，面板自己也是直接改 FunctionFlags
 * （FBlueprintGraphActionDetails::SetNetFlags），这里照抄那套位运算。
 * Python 读写不到这个字段（不是 BlueprintVisible），所以只能插件来做。
 */
static const uint32 UAL_EventNetModeFlags = FUNC_Net | FUNC_NetServer | FUNC_NetClient | FUNC_NetMulticast;

static bool UAL_EventNetFlagsFromString(const FString& Mode, uint32& OutFlags)
{
	const FString M = Mode.ToLower();
	if (M == TEXT("none") || M.IsEmpty())
	{
		OutFlags = 0;
	}
	else if (M == TEXT("server"))
	{
		OutFlags = FUNC_Net | FUNC_NetServer;
	}
	else if (M == TEXT("client"))
	{
		OutFlags = FUNC_Net | FUNC_NetClient;
	}
	else if (M == TEXT("multicast"))
	{
		OutFlags = FUNC_Net | FUNC_NetMulticast;
	}
	else
	{
		return false;
	}
	return true;
}

/** 反过来：给读图用，回的串必须能原样填回 replication */
static const TCHAR* UAL_EventNetModeName(uint32 Flags)
{
	if (!(Flags & FUNC_Net))
	{
		return TEXT("None");
	}
	if (Flags & FUNC_NetServer)
	{
		return TEXT("Server");
	}
	if (Flags & FUNC_NetClient)
	{
		return TEXT("Client");
	}
	return TEXT("Multicast");
}

static void UAL_AnnotateNodeForRewrite(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObj)
{
	if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
	{
		const FName FuncName = CallNode->FunctionReference.GetMemberName();
		if (!FuncName.IsNone())
		{
			NodeObj->SetStringField(TEXT("write_as"), TEXT("Function"));
			// 带上类名才是 create_graph 认的完整格式（ClassName.FunctionName）
			if (UClass* OwnerClass = CallNode->FunctionReference.GetMemberParentClass(CallNode->GetBlueprintClassFromNode()))
			{
				NodeObj->SetStringField(
					TEXT("member_name"),
					FString::Printf(TEXT("%s.%s"), *OwnerClass->GetName(), *FuncName.ToString()));
			}
			else
			{
				NodeObj->SetStringField(TEXT("member_name"), FuncName.ToString());
			}
		}
		return;
	}

	// CustomEvent 必须排在 UK2Node_Event 前面：它是 Event 的子类，
	// 顺序反了的话自定义事件会被当成引擎事件，重建时找不到同名函数
	if (const UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
	{
		// 参数表必须一起回。只回名字的话，「读回来改一改写回去」这一轮会把
		// 参数全丢掉 —— 而参数一丢，接在那些引脚上的线也跟着断，
		// 调用方看到的是一张自己没改过却变了形的图
		TArray<TSharedPtr<FJsonValue>> Params;
		bool bAllRewritable = true;
		for (const TSharedPtr<FUserPinInfo>& Pin : CustomEvent->UserDefinedPins)
		{
			if (!Pin.IsValid())
			{
				continue;
			}
			TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
			ParamObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
			bAllRewritable &= UAL_WritePinTypeJson(Pin->PinType, ParamObj);
			Params.Add(MakeShared<FJsonValueObject>(ParamObj));
		}

		// 有一个参数翻译不回去，整个节点就重建不出来。这时候按本函数开头
		// 那条规矩办：两个字段都不给，让调用方看见「这个节点重写不了」，
		// 而不是给一份会静默少掉参数的说明
		if (!bAllRewritable)
		{
			return;
		}

		NodeObj->SetStringField(TEXT("write_as"), TEXT("CustomEvent"));
		NodeObj->SetStringField(TEXT("member_name"), CustomEvent->CustomFunctionName.ToString());
		if (Params.Num() > 0)
		{
			NodeObj->SetArrayField(TEXT("params"), Params);
		}
		// 复制选项同理：读回来不带，改一改写回去就把 RPC 悄悄变回了普通事件
		if (CustomEvent->FunctionFlags & FUNC_Net)
		{
			NodeObj->SetStringField(TEXT("replication"), UAL_EventNetModeName(CustomEvent->FunctionFlags));
			NodeObj->SetBoolField(TEXT("reliable"), (CustomEvent->FunctionFlags & FUNC_NetReliable) != 0);
		}
		return;
	}

	if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
	{
		const FName EventName = EventNode->EventReference.GetMemberName();
		if (!EventName.IsNone())
		{
			NodeObj->SetStringField(TEXT("write_as"), TEXT("Event"));
			NodeObj->SetStringField(TEXT("member_name"), EventName.ToString());
		}
		return;
	}

	// 读写别的蓝图的变量要带上类名（WBP_HUD_C.Health），不然写回去就变成
	// 在自己身上找这个变量 —— 找不到被拒，或者更糟，正好自己也有个同名的
	if (const UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
	{
		const bool bIsGet = Node->IsA<UK2Node_VariableGet>();
		if (bIsGet || Node->IsA<UK2Node_VariableSet>())
		{
			const FString VarName = VarNode->VariableReference.GetMemberName().ToString();
			UClass* OwnerClass = VarNode->VariableReference.IsSelfContext()
				? nullptr
				: VarNode->VariableReference.GetMemberParentClass(VarNode->GetBlueprintClassFromNode());
			NodeObj->SetStringField(TEXT("write_as"), bIsGet ? TEXT("VariableGet") : TEXT("VariableSet"));
			NodeObj->SetStringField(TEXT("member_name"), OwnerClass
				? FString::Printf(TEXT("%s.%s"), *OwnerClass->GetName(), *VarName)
				: VarName);
			return;
		}
	}

	if (const UK2Node_InputAction* InputNode = Cast<UK2Node_InputAction>(Node))
	{
		NodeObj->SetStringField(TEXT("write_as"), TEXT("InputAction"));
		NodeObj->SetStringField(TEXT("member_name"), InputNode->InputActionName.ToString());
		return;
	}

	if (const UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
	{
		NodeObj->SetStringField(TEXT("write_as"), TEXT("Cast"));
		if (const UClass* Target = CastNode->TargetType)
		{
			NodeObj->SetStringField(TEXT("target_class"), Target->GetPathName());
		}
		return;
	}

	if (const UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
	{
		// 宏名本身就能当类型用（type: "ForLoop"），重建时不用再指宏库
		if (const UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
		{
			NodeObj->SetStringField(TEXT("write_as"), MacroGraph->GetName());
		}
		return;
	}

	if (const UK2Node_MakeStruct* MakeStructNode = Cast<UK2Node_MakeStruct>(Node))
	{
		NodeObj->SetStringField(TEXT("write_as"), TEXT("MakeStruct"));
		if (const UScriptStruct* S = MakeStructNode->StructType)
		{
			NodeObj->SetStringField(TEXT("struct_type"), S->GetPathName());
		}
		return;
	}

	if (const UK2Node_BreakStruct* BreakStructNode = Cast<UK2Node_BreakStruct>(Node))
	{
		NodeObj->SetStringField(TEXT("write_as"), TEXT("BreakStruct"));
		if (const UScriptStruct* S = BreakStructNode->StructType)
		{
			NodeObj->SetStringField(TEXT("struct_type"), S->GetPathName());
		}
		return;
	}

	if (const UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node))
	{
		// 不带 write_as 的话回写会掉进逃生口，建出一个没绑模板的空节点
		NodeObj->SetStringField(TEXT("write_as"), TEXT("Timeline"));
		NodeObj->SetStringField(TEXT("member_name"), TimelineNode->TimelineName.ToString());
		// 曲线也一起回 —— 理由见 UAL_BuildTimelineJson
		if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(Node))
		{
			if (const UTimelineTemplate* Template =
					Blueprint->FindTimelineTemplateByVariableName(TimelineNode->TimelineName))
			{
				NodeObj->SetObjectField(TEXT("timeline"), UAL_BuildTimelineJson(Template));
			}
		}
		return;
	}
	// 增强输入事件节点：类走反射找，不链 InputBlueprintNodes 模块
	if (UClass* EnhancedInputNodeClass = UALReflect::FindScriptClass(TEXT("/Script/InputBlueprintNodes.K2Node_EnhancedInputAction")))
	{
		if (Node->IsA(EnhancedInputNodeClass))
		{
			NodeObj->SetStringField(TEXT("write_as"), TEXT("EnhancedInputAction"));
			if (const UObject* Action = UALReflect::GetObjectProp(Node, TEXT("InputAction")))
			{
				NodeObj->SetStringField(TEXT("member_name"), Action->GetPathName());
			}
			return;
		}
	}
	// 不需要额外参数的：类型名一填就能重建
	if (Node->IsA<UK2Node_IfThenElse>())          { NodeObj->SetStringField(TEXT("write_as"), TEXT("Branch")); return; }
	if (Node->IsA<UK2Node_ExecutionSequence>())   { NodeObj->SetStringField(TEXT("write_as"), TEXT("Sequence")); return; }
	if (Node->IsA<UK2Node_SpawnActorFromClass>()) { NodeObj->SetStringField(TEXT("write_as"), TEXT("SpawnActor")); return; }
	if (Node->IsA<UK2Node_MakeArray>())           { NodeObj->SetStringField(TEXT("write_as"), TEXT("MakeArray")); return; }
	if (Node->IsA<UK2Node_Select>())              { NodeObj->SetStringField(TEXT("write_as"), TEXT("Select")); return; }
	if (Node->IsA<UK2Node_Self>())                { NodeObj->SetStringField(TEXT("write_as"), TEXT("Self")); return; }
}

static TSharedPtr<FJsonObject> UAL_BuildNodeJson(UEdGraphNode* Node)
{
	if (!Node)
	{
		return nullptr;
	}
	TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
	NodeObj->SetStringField(TEXT("node_id"), UAL_GuidToString(Node->NodeGuid));
	NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
	// 注意 title 是**按编辑器语言本地化**的，只能给人看，不能拿来认节点
	NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
	NodeObj->SetNumberField(TEXT("pos_x"), Node->NodePosX);
	NodeObj->SetNumberField(TEXT("pos_y"), Node->NodePosY);
	/**
	 * 这个节点是不是引擎自己铺的默认事件节点（新建 Actor 蓝图自带的
	 * BeginPlay / Tick / ActorBeginOverlap 那几个灰色的）。
	 *
	 * 为什么要回这一条：事件图的「空不空」不能按 `Nodes.Num() == 0` 判 ——
	 * 按那条，**任何一张新建蓝图的事件图都不算空**，而「往全新蓝图里放片段」
	 * 恰恰是最常见的用法。引擎自己有这个判断（`UEdGraphNode::IsAutomaticallyPlacedGhostNode`，
	 * 编辑器粘贴事件节点时就是靠它认出该清掉哪个旧的），九个引擎版本上都有。
	 *
	 * **这里只是把它如实回出来给调用方看，判空条件还没改。**
	 * 改之前要先在真机上量一遍这几个节点到底长什么样。
	 */
	NodeObj->SetBoolField(TEXT("is_ghost_node"), Node->IsAutomaticallyPlacedGhostNode());

	/**
	 * 注释框的几何和内容。
	 *
	 * 没有这一段，调用方拿到的注释框只有一个左上角坐标 —— 既不知道它有多大，
	 * 也不知道它框住了谁。整理图的时候那就只有两个选择：当普通节点排（空框
	 * 飞到一边，原来框住的那段逻辑裸在外面），或者干脆不动它（图挪走了，
	 * 说明还留在原地）。两个都是错的，而用户手写的分组说明就这么没了。
	 *
	 * `nodes_under_comment` 来自引擎的 `NodesUnderComment`，它只在用户拖动或
	 * 缩放过这个注释框之后才会被填上，**可能是空的**。空的时候调用方要靠
	 * 几何包含关系自己判断 —— 现在它有 x/y/width/height，判得出来。
	 */
	if (const UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
	{
		NodeObj->SetStringField(TEXT("comment_text"), Comment->NodeComment);
		NodeObj->SetNumberField(TEXT("node_width"), Comment->NodeWidth);
		NodeObj->SetNumberField(TEXT("node_height"), Comment->NodeHeight);
		NodeObj->SetNumberField(TEXT("font_size"), Comment->FontSize);

		TArray<TSharedPtr<FJsonValue>> Under;
		for (UObject* Object : Comment->GetNodesUnderComment())
		{
			if (const UEdGraphNode* Inner = Cast<UEdGraphNode>(Object))
			{
				Under.Add(MakeShared<FJsonValueString>(UAL_GuidToString(Inner->NodeGuid)));
			}
		}
		NodeObj->SetArrayField(TEXT("nodes_under_comment"), Under);
	}

	UAL_AnnotateNodeForRewrite(Node, NodeObj);
	NodeObj->SetArrayField(TEXT("pins"), UAL_BuildPinsJson(Node));
	return NodeObj;
}

// ============================================================================
// Enhanced Input：按名字或路径找 Input Action 资产
// ============================================================================

// 定义在下面的节点创建区；这里的资产查找也要用它把不沾边的「你是不是想要」滤掉
static void UAL_KeepCloseSuggestions(const FString& Wanted, TArray<FString>& InOutSuggestions);

/**
 * 找 UInputAction 资产。接受 `IA_Jump` 这种裸名（走资产注册表，重名就报歧义）
 * 或 `/Game/Input/IA_Jump` 这种路径（缺 `.IA_Jump` 后缀会自动补）。
 *
 * EnhancedInput 是插件，这里不链它的模块，全走反射 —— 和 UAL_InputCommands
 * 一个做法：工程没启用它时返回 nullptr 并说清楚，而不是整个模块加载失败。
 */
static UObject* UAL_FindInputActionAsset(const FString& NameOrPath, FString& OutError)
{
	UClass* ActionClass = UALReflect::FindScriptClass(TEXT("/Script/EnhancedInput.InputAction"));
	if (!ActionClass)
	{
		OutError = TEXT("Enhanced Input plugin is not enabled in this project (no UInputAction class). Enable EnhancedInput with ue_manage_plugin, or use the legacy InputAction node");
		return nullptr;
	}

	if (NameOrPath.StartsWith(TEXT("/")))
	{
		FString ObjectPath = NameOrPath;
		if (!ObjectPath.Contains(TEXT(".")))
		{
			FString ShortName;
			ObjectPath.Split(TEXT("/"), nullptr, &ShortName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			ObjectPath += TEXT(".") + ShortName;
		}
		UObject* Loaded = LoadObject<UObject>(nullptr, *ObjectPath);
		if (Loaded && Loaded->IsA(ActionClass))
		{
			return Loaded;
		}
		OutError = Loaded
			? FString::Printf(TEXT("'%s' is a %s, not an Input Action"), *NameOrPath, *Loaded->GetClass()->GetName())
			: FString::Printf(TEXT("Input Action not found at %s"), *NameOrPath);
		return nullptr;
	}

	const FAssetRegistryModule& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FAssetData> Assets;
	// 子类也算：C++ 里派生过 UInputAction 的工程，资产登记的是子类；路径那条路走 IsA 本来就认
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	Registry.Get().GetAssetsByClass(ActionClass->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);
#else
	Registry.Get().GetAssetsByClass(ActionClass->GetFName(), Assets, /*bSearchSubClasses=*/true);
#endif

	TArray<const FAssetData*> Matches;
	TArray<FString> AllNames;
	for (const FAssetData& Asset : Assets)
	{
		const FString AssetName = Asset.AssetName.ToString();
		AllNames.Add(AssetName);
		if (AssetName.Equals(NameOrPath, ESearchCase::IgnoreCase))
		{
			Matches.Add(&Asset);
		}
	}

	if (Matches.Num() == 1)
	{
		UObject* Loaded = Matches[0]->GetAsset();
		if (Loaded)
		{
			return Loaded;
		}
		OutError = FString::Printf(TEXT("Input Action '%s' exists but failed to load"), *NameOrPath);
		return nullptr;
	}
	if (Matches.Num() > 1)
	{
		TArray<FString> Paths;
		for (const FAssetData* Match : Matches)
		{
			// PackageName 九个版本都有；GetObjectPathString 5.1 才出现
			Paths.Add(Match->PackageName.ToString());
		}
		OutError = FString::Printf(TEXT("Input Action name '%s' is ambiguous; pass the full path instead: %s"),
			*NameOrPath, *FString::Join(Paths, TEXT(", ")));
		return nullptr;
	}

	OutError = FString::Printf(TEXT("Input Action '%s' not found"), *NameOrPath);
	TArray<FString> Suggestions;
	UAL_CommandUtils::SuggestProperties(NameOrPath, AllNames, Suggestions, 5);
	UAL_KeepCloseSuggestions(NameOrPath, Suggestions);
	if (Suggestions.Num() > 0)
	{
		OutError += FString::Printf(TEXT(" (did you mean: %s?)"), *FString::Join(Suggestions, TEXT(", ")));
	}
	else if (AllNames.Num() > 0)
	{
		TArray<FString> Head;
		for (int32 i = 0; i < FMath::Min(15, AllNames.Num()); ++i) Head.Add(AllNames[i]);
		OutError += FString::Printf(TEXT(" (Input Actions in this project: %s%s)"),
			*FString::Join(Head, TEXT(", ")), AllNames.Num() > Head.Num() ? TEXT(", ...") : TEXT(""));
	}
	else
	{
		OutError += TEXT(" (this project has no Input Action assets yet)");
	}
	return nullptr;
}

// ============================================================================
// Timeline：模板、轨道、节点
// ============================================================================

/**
 * 找到或新建这个名字的 TimelineTemplate。
 *
 * 必须走 `FBlueprintEditorUtils::AddNewTimeline`。以前这里自己 NewObject：
 * outer 挂在 Blueprint 上、对象名直接用变量名。而引擎的约定是 outer 为
 * GeneratedClass、对象名带 `_Template` 后缀 ——
 * `UK2Node_Timeline::AllocateDefaultPins` 和编译器都通过
 * `FindTimelineTemplateByVariableName` 按这个约定去找。所以以前建出来的模板
 * 节点根本找不到：节点在图里、轨道引脚长不出来、编译说模板缺失。
 *
 * 5.0–5.8 九个版本都核过：AddNewTimeline / DoesSupportTimelines /
 * FindTimelineTemplateByVariableName 全在。
 */
static UTimelineTemplate* UAL_EnsureTimelineTemplate(UBlueprint* Blueprint, const FName TimelineName, bool& bOutCreated, FString& OutError)
{
	bOutCreated = false;
	if (!Blueprint)
	{
		OutError = TEXT("Invalid Blueprint");
		return nullptr;
	}
	if (UTimelineTemplate* Existing = Blueprint->FindTimelineTemplateByVariableName(TimelineName))
	{
		return Existing;
	}
	if (!FBlueprintEditorUtils::DoesSupportTimelines(Blueprint))
	{
		OutError = FString::Printf(
			TEXT("Blueprint '%s' does not support timelines (only Actor-derived blueprints do; function libraries, macro libraries and interfaces cannot have one)"),
			*Blueprint->GetName());
		return nullptr;
	}
	// AddNewTimeline 里 check 了 GeneratedClass，没有的话是断言崩编辑器，先挡住
	if (!Blueprint->GeneratedClass)
	{
		OutError = TEXT("Blueprint has no generated class yet; compile it once before adding a timeline");
		return nullptr;
	}
	// 名字校验走编辑器同一套（FKismetNameValidator）：变量、组件、父类属性、函数图
	// 都算重名。以前只查 NewVariables，叫 Mesh / Tags 的 Timeline 建得出来，到编译才炸，
	// 而编译已经在回滚决策点之后，坏节点和模板就留在图里了
	FKismetNameValidator NameValidator(Blueprint);
	const EValidatorResult NameResult = NameValidator.IsValid(TimelineName, /*bOriginal=*/false);
	if (NameResult != EValidatorResult::Ok)
	{
		OutError = FString::Printf(
			TEXT("'%s' cannot be used as a timeline name: %s"),
			*TimelineName.ToString(),
			*INameValidatorInterface::GetErrorString(TimelineName.ToString(), NameResult));
		return nullptr;
	}

	UTimelineTemplate* Template = FBlueprintEditorUtils::AddNewTimeline(Blueprint, TimelineName);
	if (!Template)
	{
		OutError = FString::Printf(TEXT("Failed to create timeline template '%s'"), *TimelineName.ToString());
		return nullptr;
	}
	bOutCreated = true;
	return Template;
}

/** 一条 float 轨道的声明：名字（就是节点上的输出引脚名）+ 关键帧 (time, value) */
/**
 * 一个关键帧。
 *
 * 插值模式先按字符串原样收着 —— 解析要能报错，而 `UAL_ParseNodeSpec`
 * 没有报错通道（和轨道名走同一个处理：在造节点那一层验，那里有 OutError）。
 */
struct FUAL_TimelineKeySpec
{
	float Time = 0.f;
	float Value = 0.f;
	FString Interp;
	/** 手拉过的切线只有 user / break 才有意义，没给就让引擎自己算 */
	bool bHasTangents = false;
	float ArriveTangent = 0.f;
	float LeaveTangent = 0.f;
};

struct FUAL_TimelineTrackSpec
{
	FString Name;
	TArray<FUAL_TimelineKeySpec> Keys;
};

/**
 * 一个 Timeline 模板在本批改动之前的样子 —— 整批失败时照它回填。
 *
 * ## 为什么必须自己存
 *
 * 整批回滚只删「这一批新建的节点」，而模板是挂在蓝图上的独立对象，
 * 删节点带不走对它的修改；`Transaction.Cancel()` 又只把记录从 undo 栈丢掉、
 * 不回放（见 `Handle_CreateGraphDeclarative` 的函数注释）。
 *
 * 没有这份快照，「改已有 Timeline」就是一个撤不回来的动作 —— 批次失败时
 * 响应说「蓝图没有任何改动」，而用户的曲线已经被改了。所以这件事以前
 * 整个禁掉。现在改成：先存，再改，失败照存的回填。
 */
struct FUAL_TimelineSnapshot
{
	UTimelineTemplate* Template = nullptr;
	/** 模板是这一批建出来的 —— 回退时它会连节点一起被删掉，不用也不该回填 */
	bool bTemplateCreated = false;
	/** 这一批在图里新放的节点；复用图里已有节点时是那个已有节点 */
	UK2Node_Timeline* Node = nullptr;
	/** Node 是这一批新建的（而不是复用的） */
	bool bNodeCreated = false;

	float TimelineLength = 0.f;
	TEnumAsByte<ETimelineLengthMode> LengthMode = TL_TimelineLength;
	bool bLoop = false;
	bool bAutoPlay = false;
	/** 改之前的条数：多出来的都是这一批加的，回退时砍掉 */
	int32 NumFloatTracks = 0;
	int32 NumDisplayTracks = 0;
	/** 被改过关键帧的曲线，连同它原来的整条关键帧表 */
	TArray<TPair<UCurveFloat*, TArray<FRichCurveKey>>> Curves;
};

/** 改模板之前先拍一张。只记我们会动的那几项 */
static void UAL_SnapshotTimeline(FUAL_TimelineSnapshot& Snapshot, UTimelineTemplate* Template)
{
	Snapshot.Template = Template;
	Snapshot.TimelineLength = Template->TimelineLength;
	Snapshot.LengthMode = Template->LengthMode;
	Snapshot.bLoop = Template->bLoop != 0;
	Snapshot.bAutoPlay = Template->bAutoPlay != 0;
	Snapshot.NumFloatTracks = Template->FloatTracks.Num();
	Snapshot.NumDisplayTracks = Template->GetNumDisplayTracks();
}

/** 照快照把模板回填回去。模板本身是这一批建的就什么都不做 —— 它会被连节点一起删掉 */
static void UAL_RestoreTimeline(const FUAL_TimelineSnapshot& Snapshot)
{
	UTimelineTemplate* Template = Snapshot.Template;
	if (!Template || Snapshot.bTemplateCreated)
	{
		return;
	}

	Template->Modify();
	Template->TimelineLength = Snapshot.TimelineLength;
	Template->LengthMode = Snapshot.LengthMode;
	Template->bLoop = Snapshot.bLoop;
	Template->bAutoPlay = Snapshot.bAutoPlay;

	// 倒着回填：同一条轨道在一次请求里写了两遍时，Curves 里会有它的两张快照，
	// 第二张拍的已经是被第一次写过的样子 —— 正着回填会停在那张上
	for (int32 i = Snapshot.Curves.Num() - 1; i >= 0; --i)
	{
		const TPair<UCurveFloat*, TArray<FRichCurveKey>>& Pair = Snapshot.Curves[i];
		if (Pair.Key)
		{
			Pair.Key->Modify();
			Pair.Key->FloatCurve.SetKeys(Pair.Value);
		}
	}

	// 显示顺序表先砍再砍轨道：RemoveDisplayTrack 会调整同类型轨道的索引，
	// 先动 FloatTracks 的话表里剩下的项就指向不存在的轨道了
	for (int32 i = Template->GetNumDisplayTracks() - 1; i >= Snapshot.NumDisplayTracks; --i)
	{
		Template->RemoveDisplayTrack(i);
	}
	if (Template->FloatTracks.Num() > Snapshot.NumFloatTracks)
	{
		Template->FloatTracks.SetNum(Snapshot.NumFloatTracks);
	}

	// 复用的节点上，轨道引脚是按改完之后的模板长出来的，得跟着退回去。
	// 新建的那个不用管 —— 它整个会被删掉
	if (Snapshot.Node && !Snapshot.bNodeCreated)
	{
		Snapshot.Node->Modify();
		Snapshot.Node->ReconstructNode();
	}
}

/**
 * 往模板上加一条 float 轨道；同名轨道已存在就改它。
 *
 * 曲线对象的 outer 和标志照抄编辑器（STimelineEditor::OnAddNewTrack）：
 * outer 是 GeneratedClass、RF_Public —— 关卡里的 Timeline 实例要引用它，
 * 不标 Public 存关卡时会报「引用了私有对象」。
 *
 * 关键帧默认线性，不是编辑器手点出来的 auto 切线：调用方给的是 (0,0)→(1,1)，
 * 拿到的就该是一条直线，而不是引擎替它算的 S 形。要 S 形就显式写 interp。
 */
static void UAL_AddTimelineFloatTrack(
	UBlueprint* Blueprint,
	UTimelineTemplate* Template,
	const FUAL_TimelineTrackSpec& TrackSpec,
	FUAL_TimelineSnapshot& Snapshot)
{
	const FName TrackName(*TrackSpec.Name);
	int32 TrackIndex = Template->FloatTracks.IndexOfByPredicate([&TrackName](const FTTFloatTrack& Candidate)
	{
		return Candidate.GetTrackName() == TrackName;
	});
	if (TrackIndex == INDEX_NONE)
	{
		FTTFloatTrack NewTrack;
		NewTrack.SetTrackName(TrackName, Template);
		TrackIndex = Template->FloatTracks.Add(NewTrack);
	}

	// 节点的轨道引脚是从显示顺序表（TrackDisplayOrder）长出来的，不是从 FloatTracks：
	// 漏掉这一步节点上一根轨道引脚都没有（编辑器的 OnAddNewTrack 也是这两步）。
	// 先查一遍再加：同一批里写了两条同名轨道时，第二条会落回第一条的序号上，
	// 不查就会往表里塞一条重复的
	bool bDisplayed = false;
	for (int32 i = 0; i < Template->GetNumDisplayTracks(); ++i)
	{
		const FTTTrackId Displayed = Template->GetDisplayTrackId(i);
		if (Displayed.TrackType == FTTTrackBase::TT_FloatInterp && Displayed.TrackIndex == TrackIndex)
		{
			bDisplayed = true;
			break;
		}
	}
	if (!bDisplayed)
	{
		Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_FloatInterp, TrackIndex));
	}

	FTTFloatTrack* Track = &Template->FloatTracks[TrackIndex];
	if (!Track->CurveFloat)
	{
		Track->CurveFloat = NewObject<UCurveFloat>(Blueprint->GeneratedClass, NAME_None, RF_Public);
	}
	Track->CurveFloat->Modify();
	if (TrackSpec.Keys.Num() > 0)
	{
		// 这条曲线原来的样子先存下来，整批失败时要照它回填
		Snapshot.Curves.Emplace(Track->CurveFloat, Track->CurveFloat->FloatCurve.Keys);

		// 给了关键帧就是整条曲线的定义，不和旧帧合并：调用方要 (0,0)→(1,1) 就得是一条直线，
		// 不是旧的 (0.5,1) 还夹在中间
		TArray<FRichCurveKey> NewKeys;
		NewKeys.Reserve(TrackSpec.Keys.Num());
		for (const FUAL_TimelineKeySpec& Spec : TrackSpec.Keys)
		{
			FRichCurveKey& Key = NewKeys.AddDefaulted_GetRef();
			Key.Time = Spec.Time;
			Key.Value = Spec.Value;
			// 到这里 Interp 已经在造节点那层验过一次了，不会失败
			FString Ignored;
			ERichCurveInterpMode Interp = RCIM_Linear;
			ERichCurveTangentMode Tangent = RCTM_Auto;
			UAL_CurveInterpFromString(Spec.Interp, Interp, Tangent, Ignored);
			Key.InterpMode = Interp;
			Key.TangentMode = Tangent;
			if (Spec.bHasTangents)
			{
				Key.ArriveTangent = Spec.ArriveTangent;
				Key.LeaveTangent = Spec.LeaveTangent;
			}
		}
		// SetKeys 不排序（AddKey 才按时间插），乱序的时间点会得到一条画不出来的曲线
		NewKeys.Sort([](const FRichCurveKey& A, const FRichCurveKey& B) { return A.Time < B.Time; });
		// SetKeys 内部会跑 AutoSetTangents：RCTM_Auto 的键由它算出 S 形，
		// user / break 的键它不碰，上面写进去的切线保得住
		Track->CurveFloat->FloatCurve.SetKeys(NewKeys);
	}
}

/**
 * 在图里放一个引用该模板的 Timeline 节点。
 *
 * TimelineName 和 TimelineGuid 必须在 Finalize **之前**给：Finalize 会调
 * AllocateDefaultPins，它按 TimelineName 找模板来长轨道引脚。先 Finalize 再
 * 赋名，节点上就只有 Play/Stop 那几个固定引脚，轨道一个都没有。
 */
static UK2Node_Timeline* UAL_PlaceTimelineNode(UEdGraph* Graph, const UTimelineTemplate* Template, const FName TimelineName, int32 PosX, int32 PosY)
{
	FGraphNodeCreator<UK2Node_Timeline> NodeCreator(*Graph);
	UK2Node_Timeline* TimelineNode = NodeCreator.CreateNode();
	TimelineNode->TimelineName = TimelineName;
	TimelineNode->TimelineGuid = Template->TimelineGuid;
	TimelineNode->NodePosX = PosX;
	TimelineNode->NodePosY = PosY;
	NodeCreator.Finalize();
	TimelineNode->ReconstructNode();
	return TimelineNode;
}

static bool UAL_ParseNodePosition(const TSharedPtr<FJsonObject>& Payload, int32& OutPosX, int32& OutPosY, bool& bOutHasExplicitPosition, bool& bOutForcePosition)
{
	bOutHasExplicitPosition = false;
	bOutForcePosition = false;
	OutPosX = 0;
	OutPosY = 0;
	if (!Payload.IsValid())
	{
		return false;
	}

	Payload->TryGetBoolField(TEXT("force_position"), bOutForcePosition);

	const TSharedPtr<FJsonObject>* PosObjPtr = nullptr;
	if (Payload->TryGetObjectField(TEXT("node_position"), PosObjPtr) && PosObjPtr && (*PosObjPtr).IsValid())
	{
		bOutHasExplicitPosition = true;
		(*PosObjPtr)->TryGetNumberField(TEXT("x"), OutPosX);
		(*PosObjPtr)->TryGetNumberField(TEXT("y"), OutPosY);
	}
	return true;
}

static void UAL_AutoLayoutIfNeeded(UEdGraph* Graph, bool& bHasExplicitPosition, bool bForcePosition, int32& PosX, int32& PosY)
{
	if (!Graph)
	{
		return;
	}

	// 兼容 add_node 的行为：显式给了 (0,0) 但没有 force_position=true 时，也按“未指定”处理，避免堆叠
	if (bHasExplicitPosition && !bForcePosition && PosX == 0 && PosY == 0)
	{
		bHasExplicitPosition = false;
	}

	if (bHasExplicitPosition)
	{
		return;
	}

	// 简化版智能排版：取主簇的 90% 分位 MaxX，放到右侧
	TArray<int32> Xs, Ys;
	Xs.Reserve(Graph->Nodes.Num());
	Ys.Reserve(Graph->Nodes.Num());
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		Xs.Add(Node->NodePosX);
		Ys.Add(Node->NodePosY);
	}

	auto Quantile = [](TArray<int32> Arr, float Q) -> int32
	{
		if (Arr.Num() == 0) return 0;
		Arr.Sort();
		const float IdxF = FMath::Clamp(Q, 0.0f, 1.0f) * float(Arr.Num() - 1);
		const int32 Lo = FMath::Clamp(int32(FMath::FloorToFloat(IdxF)), 0, Arr.Num() - 1);
		const int32 Hi = FMath::Clamp(int32(FMath::CeilToFloat(IdxF)), 0, Arr.Num() - 1);
		if (Lo == Hi) return Arr[Lo];
		const float Alpha = IdxF - float(Lo);
		return FMath::RoundToInt(FMath::Lerp(float(Arr[Lo]), float(Arr[Hi]), Alpha));
	};

	const int32 MaxX = (Xs.Num() > 0) ? Quantile(Xs, 0.90f) : 0;
	const int32 BaseY = (Ys.Num() > 0) ? Quantile(Ys, 0.10f) : 0;

	PosX = MaxX + 420;
	PosY = BaseY;
}

static bool UAL_LoadBlueprintByPathOrName(const FString& BlueprintPathOrName, UBlueprint*& OutBlueprint, FString& OutResolvedPath)
{
	OutBlueprint = nullptr;
	OutResolvedPath = BlueprintPathOrName;

	if (BlueprintPathOrName.IsEmpty())
	{
		return false;
	}

	// 1) 直接按路径加载
	if (BlueprintPathOrName.StartsWith(TEXT("/")))
	{
		FString ResolvedPath = BlueprintPathOrName;
		if (!ResolvedPath.Contains(TEXT(".")))
		{
			ResolvedPath = ResolvedPath + TEXT(".") + FPaths::GetBaseFilename(BlueprintPathOrName);
		}
		if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *ResolvedPath))
		{
			OutBlueprint = BP;
			OutResolvedPath = ResolvedPath;
			return true;
		}
	}

	// 2) AssetRegistry 模糊查找（兼容传入短名）
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TArray<FAssetData> AssetList;
	FARFilter Filter;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
#else
	Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
#endif
	Filter.bRecursiveClasses = true;
	AssetRegistry.GetAssets(Filter, AssetList);

	for (const FAssetData& Asset : AssetList)
	{
		FString AssetName = Asset.AssetName.ToString();
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		FString AssetPath = Asset.GetObjectPathString();
#else
		FString AssetPath = Asset.ObjectPath.ToString();
#endif
		if (AssetName.Equals(BlueprintPathOrName, ESearchCase::IgnoreCase) ||
			AssetPath.Contains(BlueprintPathOrName))
		{
			if (UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset()))
			{
				OutBlueprint = BP;
				OutResolvedPath = AssetPath;
				return true;
			}
		}
	}
	return false;
}

/**
 * 按名字找一个 `UScriptStruct` 或 `UEnum`。
 *
 * 版本分支照抄 `UAL_CommandUtils::ResolveClassFromIdentifier`（UAL_CommandUtils.cpp:480）——
 * `FindFirstObject` 是 5.1 才有的，5.0 上只能走 `ANY_PACKAGE`，
 * 那份分支在九个引擎上已经验过了，不另起炉灶。
 *
 * 前缀重试：用户写 `Transform`，引擎里那个类型的名字就是 `Transform`；
 * 但用户也可能写 `FTransform` / `EMyEnum`，所以两种都试一次。
 */
template <typename TType>
static TType* UAL_FindTypeByName(const FString& Name, const TCHAR* Prefix)
{
	if (Name.IsEmpty())
	{
		return nullptr;
	}

	// 全路径（/Script/CoreUObject.Transform）直接加载，不猜
	if (Name.StartsWith(TEXT("/")))
	{
		return LoadObject<TType>(nullptr, *Name);
	}

	// 用户可能带了前缀（FTransform / EMyEnum），引擎里存的是去掉前缀的名字
	FString Bare = Name;
	if (Bare.Len() > 1 && Bare[0] == Prefix[0] && FChar::IsUpper(Bare[1]))
	{
		Bare = Bare.RightChop(1);
	}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
	TType* Found = FindFirstObject<TType>(*Name, EFindFirstObjectOptions::NativeFirst);
	if (!Found && Bare != Name)
	{
		Found = FindFirstObject<TType>(*Bare, EFindFirstObjectOptions::NativeFirst);
	}
#else
	TType* Found = FindObject<TType>(ANY_PACKAGE, *Name);
	if (!Found && Bare != Name)
	{
		Found = FindObject<TType>(ANY_PACKAGE, *Bare);
	}
#endif
	return Found;
}

/**
 * 类型名 → `FEdGraphPinType`。**全仓库唯一一处**。
 *
 * 在它之前这件事有三份手抄（`create_function` 一份、`add_variable` 内联一份、
 * `event_dispatcher` / `function_signature` 共用一份），覆盖各不相同而且已经漂了 ——
 * 只有第三份认得 `Transform`。三份都只认四五个硬编码的结构体，
 * 于是「片段里有个 Transform 变量」这种再常见不过的情况，Agent 知道该建什么却建不出来。
 *
 * 现在结构体和枚举**按名字解析**，那几个硬编码分支跟着删掉 ——
 * 覆盖面是靠删代码换来的，不是靠加分支。
 *
 * @param TypeStr        类型名。标量关键字，或者结构体 / 枚举名，或者 object/class/soft_*
 * @param ClassOrStruct  `type=object/class/soft_*` 时指定类；其余情况留空
 * @param ContainerStr   ""|"array"|"set"|"map"，大小写不敏感
 */
static bool UAL_ResolvePinType(
	const FString& TypeStr,
	const FString& ClassOrStruct,
	const FString& ContainerStr,
	FEdGraphPinType& OutPinType,
	FString& OutError)
{
	OutError.Reset();
	OutPinType = FEdGraphPinType();
	OutPinType.PinCategory = NAME_None;
	OutPinType.PinSubCategory = NAME_None;
	OutPinType.PinSubCategoryObject = nullptr;

	const FString Container = ContainerStr.ToLower();
	if (Container.IsEmpty() || Container == TEXT("none") || Container == TEXT("single"))
	{
		OutPinType.ContainerType = EPinContainerType::None;
	}
	else if (Container == TEXT("array"))
	{
		OutPinType.ContainerType = EPinContainerType::Array;
	}
	else if (Container == TEXT("set"))
	{
		OutPinType.ContainerType = EPinContainerType::Set;
	}
	else if (Container == TEXT("map"))
	{
		// Map 的 value 类型（PinValueType）暂时固定成 Object —— 还没有调用方需要指定它。
		// ponytail: 真有片段用到带类型的 Map 再加一个 value_type 参数
		OutPinType.ContainerType = EPinContainerType::Map;
	}
	else
	{
		OutError = FString::Printf(
			TEXT("Unknown container '%s'. Use \"\" (single), \"array\", \"set\" or \"map\"."), *ContainerStr);
		return false;
	}

	const FString T = TypeStr.ToLower();

	if (T == TEXT("bool") || T == TEXT("boolean"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		return true;
	}
	if (T == TEXT("int") || T == TEXT("int32") || T == TEXT("integer"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		return true;
	}
	if (T == TEXT("int64"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
		return true;
	}
	// UE5 的浮点是 PC_Real 主类别 + PC_Float / PC_Double 子类别。
	// 引擎自己从属性推引脚类型就是这么写的（EdGraphSchema_K2.cpp:3503 的
	// GetPropertyCategoryInfo）。把 PC_Float 当主类别是 UE4 的旧形状 ——
	// 合并之前三份手抄里有两份是那么写的，只有事件分发器那份是对的。
	if (T == TEXT("float"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
		return true;
	}
	if (T == TEXT("double"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		return true;
	}
	if (T == TEXT("string") || T == TEXT("str"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
		return true;
	}
	if (T == TEXT("name"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
		return true;
	}
	if (T == TEXT("text"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
		return true;
	}
	// 裸 byte（不带枚举）。读回来的签名里会出现它，入参这头也得认，
	// 否则「读回来改一改写回去」在这个类型上走不通
	if (T == TEXT("byte") || T == TEXT("uint8"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		return true;
	}
	if (T == TEXT("object") || T == TEXT("class") || T == TEXT("soft_object") || T == TEXT("soft_class"))
	{
		FString ClsStr = ClassOrStruct;
		if (ClsStr.IsEmpty())
		{
			ClsStr = TEXT("/Script/Engine.Object");
		}
		FString ClassError;
		UClass* ObjClass = UAL_CommandUtils::ResolveClassFromIdentifier(ClsStr, UObject::StaticClass(), ClassError);
		if (!ObjClass)
		{
			OutError = ClassError;
			return false;
		}

		if (T == TEXT("class") || T == TEXT("soft_class"))
		{
			OutPinType.PinCategory = (T == TEXT("soft_class")) ? UEdGraphSchema_K2::PC_SoftClass : UEdGraphSchema_K2::PC_Class;
		}
		else
		{
			OutPinType.PinCategory = (T == TEXT("soft_object")) ? UEdGraphSchema_K2::PC_SoftObject : UEdGraphSchema_K2::PC_Object;
		}
		OutPinType.PinSubCategoryObject = ObjClass;
		return true;
	}

	// —— 到这里说明不是标量关键字，按结构体 / 枚举名去找 ——
	//
	// Vector / Rotator / Transform / LinearColor 这些以前是硬编码分支，现在一并走这条路：
	// 它们在引擎里就是普通的 UScriptStruct，名字分别是 Vector / Rotator / Transform / LinearColor。
	// 自定义结构体、FDataTableRowHandle、蓝图里定义的枚举，同样走这条，不用一个个加分支。
	//
	// `ClassOrStruct` 优先：调用方明确给了名字就用它，没给就拿 TypeStr 当名字。
	const FString TypeName = ClassOrStruct.IsEmpty() ? TypeStr : ClassOrStruct;

	if (UScriptStruct* Struct = UAL_FindTypeByName<UScriptStruct>(TypeName, TEXT("F")))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = Struct;
		return true;
	}

	if (UEnum* Enum = UAL_FindTypeByName<UEnum>(TypeName, TEXT("E")))
	{
		// 蓝图里枚举变量是 Byte + 枚举做 SubCategoryObject，不是 PC_Enum ——
		// PC_Enum 只在函数参数那种地方用，成员变量用它编不过
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		OutPinType.PinSubCategoryObject = Enum;
		return true;
	}

	OutError = FString::Printf(
		TEXT("Unsupported type: %s. Use a scalar keyword (bool/int/int64/float/double/string/name/text), ")
		TEXT("a struct or enum name (Vector, Transform, MyStruct, EMyEnum), ")
		TEXT("or an object reference: type must be exactly \"object\" (or class/soft_object/soft_class) ")
		TEXT("with the referenced class in the separate object_class field, e.g. {\"type\":\"object\",\"object_class\":\"StaticMesh\"}. ")
		TEXT("Colon forms like \"object:StaticMesh\" are not accepted."),
		*TypeStr);
	return false;
}

/**
 * 从一个 `{ name, type, object_class, container, is_array }` 参数对象里读出引脚类型。
 *
 * `container`（""/array/set/map）是新的写法；`is_array` 是老写法，还收，
 * 两个都给时 `container` 说了算。
 */
static bool UAL_ReadPinTypeFromParamObject(
	const TSharedPtr<FJsonObject>& Obj,
	const FString& TypeStr,
	const FString& ClassOrStruct,
	FEdGraphPinType& OutPinType,
	FString& OutError)
{
	FString Container;
	if (!Obj->TryGetStringField(TEXT("container"), Container))
	{
		bool bIsArray = false;
		Obj->TryGetBoolField(TEXT("is_array"), bIsArray);
		Container = bIsArray ? TEXT("array") : TEXT("");
	}
	return UAL_ResolvePinType(TypeStr, ClassOrStruct, Container, OutPinType, OutError);
}

/**
 * 把一个引脚类型写成**能原样填回去**的 `{ type, class?, container? }`。
 *
 * 这是 `UAL_ResolvePinType` 的反方向，全仓库唯一一处，两个函数成对改。
 *
 * 以前各处都是 `PinType.PinCategory.ToString()` —— 浮点回的是 `"real"`
 * （PC_Real 是引擎内部的主类别名），而入参那头只认 `"float"` / `"double"`。
 * 于是读回来的参数表不能直接写回去，调用方得自己猜一次翻译，猜错就是一趟失败往返。
 *
 * @return 这个类型能不能被填回去。`false` 表示只写了个信息性的类别名
 *         （delegate / wildcard 这类），调用方不该拿它去重建。
 */
/** 容器名，和 UAL_ResolvePinType 认的 container 入参同一套词；单值回 nullptr */
static const TCHAR* UAL_ContainerName(const FEdGraphPinType& PinType)
{
	switch (PinType.ContainerType)
	{
	case EPinContainerType::Array: return TEXT("array");
	case EPinContainerType::Set:   return TEXT("set");
	case EPinContainerType::Map:   return TEXT("map");
	default:                       return nullptr;
	}
}

/** 人读的类型名：bool / bool[] / set<bool> / map<bool, ...>。报错和回执摘要用 */
static FString UAL_DisplayTypeName(const FString& BaseType, const FEdGraphPinType& PinType)
{
	switch (PinType.ContainerType)
	{
	case EPinContainerType::Array: return BaseType + TEXT("[]");
	case EPinContainerType::Set:   return FString::Printf(TEXT("set<%s>"), *BaseType);
	case EPinContainerType::Map:   return FString::Printf(TEXT("map<%s, ...>"), *BaseType);
	default:                       return BaseType;
	}
}

static bool UAL_WritePinTypeJson(const FEdGraphPinType& PinType, const TSharedPtr<FJsonObject>& Out)
{
	// 容器先写，标量类型判断和它无关
	if (const TCHAR* ContainerName = UAL_ContainerName(PinType))
	{
		Out->SetStringField(TEXT("container"), ContainerName);
	}

	const FName Cat = PinType.PinCategory;

	auto WriteObjectLike = [&](const TCHAR* Keyword) -> bool
	{
		Out->SetStringField(TEXT("type"), Keyword);
		if (const UObject* Sub = PinType.PinSubCategoryObject.Get())
		{
			// 类名走完整路径：短名会撞名（Engine 和用户模块里都可能有 Character）
			Out->SetStringField(TEXT("object_class"), Sub->GetPathName());
			// `class` 是历史字段名，读的那几个调用方还在看它
			Out->SetStringField(TEXT("class"), Sub->GetName());
		}
		return true;
	};

	if (Cat == UEdGraphSchema_K2::PC_Boolean) { Out->SetStringField(TEXT("type"), TEXT("bool")); return true; }
	if (Cat == UEdGraphSchema_K2::PC_Int)     { Out->SetStringField(TEXT("type"), TEXT("int")); return true; }
	if (Cat == UEdGraphSchema_K2::PC_Int64)   { Out->SetStringField(TEXT("type"), TEXT("int64")); return true; }
	if (Cat == UEdGraphSchema_K2::PC_String)  { Out->SetStringField(TEXT("type"), TEXT("string")); return true; }
	if (Cat == UEdGraphSchema_K2::PC_Name)    { Out->SetStringField(TEXT("type"), TEXT("name")); return true; }
	if (Cat == UEdGraphSchema_K2::PC_Text)    { Out->SetStringField(TEXT("type"), TEXT("text")); return true; }

	if (Cat == UEdGraphSchema_K2::PC_Real)
	{
		Out->SetStringField(TEXT("type"),
			PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double ? TEXT("double") : TEXT("float"));
		return true;
	}
	// UE4 形状：老资产里浮点还是主类别。引擎自己也还认，所以读得到
	if (Cat == UEdGraphSchema_K2::PC_Float)  { Out->SetStringField(TEXT("type"), TEXT("float")); return true; }
	if (Cat == UEdGraphSchema_K2::PC_Double) { Out->SetStringField(TEXT("type"), TEXT("double")); return true; }

	if (Cat == UEdGraphSchema_K2::PC_Object)     { return WriteObjectLike(TEXT("object")); }
	if (Cat == UEdGraphSchema_K2::PC_Class)      { return WriteObjectLike(TEXT("class")); }
	if (Cat == UEdGraphSchema_K2::PC_SoftObject) { return WriteObjectLike(TEXT("soft_object")); }
	if (Cat == UEdGraphSchema_K2::PC_SoftClass)  { return WriteObjectLike(TEXT("soft_class")); }

	if (Cat == UEdGraphSchema_K2::PC_Struct)
	{
		if (const UObject* Sub = PinType.PinSubCategoryObject.Get())
		{
			// 结构体和枚举没有单独的关键字，名字本身就是类型名（Vector / MyStruct）
			Out->SetStringField(TEXT("type"), Sub->GetName());
			Out->SetStringField(TEXT("class"), Sub->GetName());
			return true;
		}
		Out->SetStringField(TEXT("type"), Cat.ToString());
		return false;
	}

	if (Cat == UEdGraphSchema_K2::PC_Byte)
	{
		if (const UObject* Sub = PinType.PinSubCategoryObject.Get())
		{
			Out->SetStringField(TEXT("type"), Sub->GetName());
			Out->SetStringField(TEXT("class"), Sub->GetName());
			return true;
		}
		Out->SetStringField(TEXT("type"), TEXT("byte"));
		return true;
	}

	// delegate / wildcard / exec / interface：写不回去。类别名照回，
	// 但明说它不可重建 —— 给个猜的关键字才是害人
	Out->SetStringField(TEXT("type"), Cat.ToString());
	return false;
}

/**
 * 一条用户定义引脚的规格（函数参数 / 自定义事件参数）。
 *
 * 校验和创建分成两步是故意的：**先全量校验，再动图**。建到一半才发现第三个
 * 参数类型不认的话，前两个已经长在节点上了 —— 调用方拿到一个错误，图里却多了
 * 半份参数表，重发一次又撞重名。
 */
struct FUAL_UserPinSpec
{
	FName Name;
	FEdGraphPinType Type;
};

/**
 * 校验一组 `{ name, type, object_class?, container? }` 参数对象。
 *
 * 出错就整组失败，不跳过 —— 静默跳过一条非法参数，调用方看到的是「建好了」，
 * 拿到的却是一张少了一个引脚的签名，连线随后莫名其妙地接不上。
 */
static bool UAL_ParseUserPinSpecs(
	const TArray<TSharedPtr<FJsonValue>>& Params,
	const TCHAR* FieldName,
	TArray<FUAL_UserPinSpec>& OutSpecs,
	FString& OutError)
{
	OutSpecs.Reset();

	for (int32 Index = 0; Index < Params.Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> Obj = Params[Index].IsValid() ? Params[Index]->AsObject() : nullptr;
		if (!Obj.IsValid())
		{
			OutError = FString::Printf(TEXT("%s[%d] is not an object. Each entry needs { \"name\": ..., \"type\": ... }."), FieldName, Index);
			return false;
		}

		FString Name, Type, ObjClass;
		Obj->TryGetStringField(TEXT("name"), Name);
		Obj->TryGetStringField(TEXT("type"), Type);
		if (!Obj->TryGetStringField(TEXT("object_class"), ObjClass))
		{
			Obj->TryGetStringField(TEXT("class"), ObjClass);
		}

		if (Name.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s[%d] is missing \"name\"."), FieldName, Index);
			return false;
		}
		if (Type.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s[%d] (\"%s\") is missing \"type\"."), FieldName, Index, *Name);
			return false;
		}

		FUAL_UserPinSpec Spec;
		Spec.Name = FName(*Name);

		FString TypeError;
		if (!UAL_ReadPinTypeFromParamObject(Obj, Type, ObjClass, Spec.Type, TypeError))
		{
			OutError = FString::Printf(TEXT("%s[%d] (\"%s\"): %s"), FieldName, Index, *Name, *TypeError);
			return false;
		}

		for (const FUAL_UserPinSpec& Seen : OutSpecs)
		{
			if (Seen.Name == Spec.Name)
			{
				OutError = FString::Printf(TEXT("%s has two parameters named \"%s\"."), FieldName, *Name);
				return false;
			}
		}

		OutSpecs.Add(MoveTemp(Spec));
	}

	return true;
}

/**
 * 把校验过的规格建成用户定义引脚。
 *
 * ## 为什么还要补默认值
 *
 * 新建的用户引脚要补一个**合类型的默认值**，否则编译不过。
 *
 * `CreateUserDefinedPin()` 建出来的 `FUserPinInfo::PinDefaultValue` 是**空串**，
 * 而 `UK2Node_FunctionResult::CreatePinFromUserDefinition()` 会把它原样喂给
 * `SetPinAutogeneratedDefaultValue()`（K2Node_FunctionResult.cpp:196）。
 * 空串对 float 这类不是合法字面量，编译器当场报
 * 「Result 的默认值 '' 无效：Unsupported type 浮点（单精度）」。
 *
 * 用户在编辑器里点「加返回值」不会踩到，是因为那条路上引擎自己补了默认值。
 * 我们直接调底层 API，就得自己补。
 *
 * 两步都要做：
 *   1. `SetPinAutogeneratedDefaultValueBasedOnType()` 给引脚算一个合类型的值；
 *   2. `UpdateUserDefinedPinDefaultValues()` 把算出来的值**回写进 UserDefinedPins**
 *      —— 不回写的话，下次重建引脚（改签名、重开蓝图）又会拿那个空串盖回去。
 *
 * 2026-09-07 真机抓到：`blueprint_create_function` 建一个带 `Result: Float`
 * 返回值的函数，函数体是空的，蓝图就编译不过。九个引擎上这两个 API 都在。
 */
static void UAL_CreateUserPins(
	UK2Node_EditablePinBase* Target,
	const TArray<FUAL_UserPinSpec>& Specs,
	EEdGraphPinDirection Direction)
{
	if (!Target)
	{
		return;
	}

	for (const FUAL_UserPinSpec& Spec : Specs)
	{
		UEdGraphPin* NewPin = Target->CreateUserDefinedPin(Spec.Name, Spec.Type, Direction);
		if (!NewPin)
		{
			continue;
		}
		if (const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(Target->GetSchema()))
		{
			K2Schema->SetPinAutogeneratedDefaultValueBasedOnType(NewPin);
		}
		Target->UpdateUserDefinedPinDefaultValues();
	}
}

// ============================================================================
// 节点工厂 —— 全仓库唯一一处「按类型名造节点」的实现
// ============================================================================

/**
 * 造一个节点需要的全部输入。
 *
 * ## 为什么要有这个结构体
 *
 * 这些字段以前是直接从 `Payload` 里读的，于是「造节点」这件事和「处理一个
 * add_node 请求」黏在了一起：声明式建图那条路没有自己的 Payload，读不到
 * `target_class` / `struct_type` / `macro_lib`，只能另写一份简化版工厂。
 *
 * 两份工厂各自认识的类型不一样（旧的那份只认 5 种），而调用方看到的 schema
 * 是同一份 —— 于是模型照着工具示例填 `CallFunction`，被简化版顶回来。
 * 真机评测里逮到过：批量建图失败后模型退回逐节点，30 分钟写 10 个节点。
 *
 * 把入参收进结构体，两条路就能共用同一个工厂，永远不会再分叉。
 */
struct FUAL_NodeSpec
{
	/** 节点类型名，大小写与下划线不敏感（"VariableGet" / "variable_get" 等价） */
	FString Type;
	/** 成员名：Event 的事件名、Function 的 ClassName.FuncName、Variable 的变量名 */
	FString Name;
	/** Cast / SpawnActor 的目标类 */
	FString TargetClass;
	/** MakeStruct / BreakStruct 的结构体 */
	FString StructType;
	/** 宏库路径，留空用引擎的 StandardMacros */
	FString MacroLib;
	/**
	 * 逃生口：直接点名一个 UK2Node 子类（如 "K2Node_MakeMap"）。
	 *
	 * 具名分派永远追不上引擎里几百个 K2Node 子类，而「不认识的类型」这个
	 * 错误类别本身才是要消灭的东西 —— 少一种类型，调用方就得退回逐节点那条
	 * 慢路。有了这个口子，没被具名覆盖的简单节点也能一次性建出来。
	 *
	 * 只适用于**不需要额外配置就能自己长出引脚的**节点：通用路径只做
	 * NewObject + AllocateDefaultPins。CallFunction 要函数引用、MacroInstance
	 * 要宏图、Cast 要目标类，这些仍然必须走具名分派。
	 */
	FString RawClass;

	/**
	 * CustomEvent 的参数表 —— 自定义事件红点上那一排输出引脚。
	 *
	 * 没有它，自定义事件只能是无参的，而「绑定到事件分发器」这条最常见的用法
	 * 恰好要求事件签名和分发器一致 —— 分发器带参数，事件建不出参数，那条路就断了。
	 * 2026-09-16 用户反馈到这里：`params` / `inputs` 两种写法都被静默吃掉。
	 *
	 * 原样带着 JSON 走、不在这里解析成 `FEdGraphPinType`，是因为解析会失败而
	 * `UAL_ParseNodeSpec` 没有报错通道（Timeline 轨道那段是同一个处理）。
	 * 类型解析放到造节点那一步，失败就顺着工厂的 `OutError` 回去，整批回滚。
	 */
	TArray<TSharedPtr<FJsonValue>> EventParams;

	/**
	 * CustomEvent 的复制模式：None / Server / Client / Multicast，空串＝没给。
	 * 联机 RPC 就是它 —— 没有这个字段，Agent 只能请用户去细节面板手点。
	 * 同样原样带着走，到造节点那一步再校验，错了顺着 OutError 整批回滚。
	 */
	FString EventReplication;
	bool bHasEventReliable = false;
	bool bEventReliable = false;

	bool bHasFirstIndex = false;
	int32 FirstIndex = 0;
	bool bHasLastIndex = false;
	int32 LastIndex = 0;

	int32 PosX = 0;
	int32 PosY = 0;
	/**
	 * 调用方到底给没给坐标。
	 *
	 * **不能用 `PosX != 0 || PosY != 0` 代替。** 分层布局算出来的第一个节点
	 * 几乎必然落在 (0,0)，拿值去判断的话它会被当成「没给位置」，然后被扔进
	 * 兜底网格 —— 而那个网格也从 (0,0) 往右铺，正好压在别的节点上。
	 *
	 * 真机上就是这么坏的：Event BeginPlay 跑到了它驱动的 Print String 右边，
	 * 一个走布局坐标、一个走兜底网格，两套坐标系互不相干。
	 */
	bool bHasPosition = false;

	/** Timeline 专用（type=Timeline 时 Name 就是 Timeline 的名字）；没给的字段不动模板上的现值 */
	bool bHasTimelineLength = false;
	float TimelineLength = 0.f;
	bool bHasTimelineLoop = false;
	bool bTimelineLoop = false;
	bool bHasTimelineAutoPlay = false;
	bool bTimelineAutoPlay = false;
	/** 原样收着，和 interp 一样：解析要能报错，这里没有报错通道 */
	FString TimelineLengthMode;
	TArray<FUAL_TimelineTrackSpec> TimelineTracks;
};

/** 常用函数库 —— 只给了函数名没给类名时**先**找这几个，命中率最高 */
static const TArray<FString>& UAL_CommonFunctionLibraries()
{
	static const TArray<FString> Libs = {
		TEXT("KismetMathLibrary"),
		TEXT("KismetSystemLibrary"),
		TEXT("GameplayStatics"),
		TEXT("KismetStringLibrary")
	};
	return Libs;
}

/**
 * 函数在**蓝图里显示的名字**。
 *
 * 这是整个函数解析最容易出错的地方，也是一次真实故障的根因：
 * 引擎里大量核心函数的 C++ 名带 `K2_` 前缀，蓝图里显示的是去掉前缀的那个 ——
 * `AActor::K2_GetActorLocation` 在图上叫 `GetActorLocation`。
 *
 * 调用方（模型）看到的、文档里写的、用户嘴里说的，全都是蓝图名。
 * 而我们原来只用 `FindFunctionByName` 按 C++ 名查，于是
 * `GetActorLocation` / `SetActorLocation` / `DestroyActor` 这一大批**必然找不到** ——
 * 报出去的是「Function not found」，但调用方并没有写错。
 *
 * 会话记录里 blueprint_add_node 的失败几乎全是这个原因。
 */
static FString UAL_BlueprintFunctionName(const UFunction* Func)
{
	if (!Func) return FString();

#if WITH_EDITOR
	// ScriptName / DisplayName 是权威来源，但只在编辑器下有元数据
	static const FName ScriptNameKey(TEXT("ScriptName"));
	static const FName DisplayNameKey(TEXT("DisplayName"));
	if (Func->HasMetaData(ScriptNameKey))
	{
		const FString Meta = Func->GetMetaData(ScriptNameKey);
		if (!Meta.IsEmpty()) return Meta;
	}
	if (Func->HasMetaData(DisplayNameKey))
	{
		// DisplayName 可能带空格（"Get Actor Location"），去掉后才是图上能打出来的标识
		const FString Meta = Func->GetMetaData(DisplayNameKey).Replace(TEXT(" "), TEXT(""));
		if (!Meta.IsEmpty()) return Meta;
	}
#endif

	// 没有元数据时退回去掉 K2_ 前缀 —— 这一条不依赖编辑器，打包后同样成立
	const FString Raw = Func->GetName();
	return Raw.StartsWith(TEXT("K2_")) ? Raw.RightChop(3) : Raw;
}

/** 这个名字是不是指向该函数。C++ 名和蓝图名都认，大小写不敏感 */
static bool UAL_FunctionMatchesName(const UFunction* Func, const FString& Name)
{
	if (!Func || Name.IsEmpty()) return false;
	if (Func->GetName().Equals(Name, ESearchCase::IgnoreCase)) return true;
	return UAL_BlueprintFunctionName(Func).Equals(Name, ESearchCase::IgnoreCase);
}

/** 在一个类（含父类）里按 C++ 名或蓝图名找函数 */
static UFunction* UAL_FindFunctionByAnyName(UClass* Cls, const FString& Name)
{
	if (!Cls || Name.IsEmpty()) return nullptr;

	// 先走引擎自己的精确查找，绝大多数情况一步命中，不用遍历
	if (UFunction* Exact = Cls->FindFunctionByName(FName(*Name)))
	{
		return Exact;
	}
	// K2_ 前缀是最常见的一种，直接试一次比全量遍历便宜
	if (UFunction* K2 = Cls->FindFunctionByName(FName(*(TEXT("K2_") + Name))))
	{
		return K2;
	}

	for (TFieldIterator<UFunction> It(Cls, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		if (UAL_FunctionMatchesName(*It, Name))
		{
			return *It;
		}
	}
	return nullptr;
}

/**
 * 只保留**真的像**的建议。
 *
 * `SuggestProperties` 不设距离阈值，永远回最近的 N 个 —— 哪怕一个都不沾边。
 * 真机记录里有一次：调用方想设 `SetCollisionProfileName`（那其实是个函数），
 * 我们回的建议是 `bHiddenInGame, bOnlyOwnerSee, CustomPrimitiveData` ——
 * 三个都和碰撞毫无关系。**这种建议比不给更坏**：它把调用方往错误方向带，
 * 而不是让它去想「是不是用错了命令」。
 *
 * 阈值取「编辑距离 < 名字长度的一半」：写错几个字母能捞回来，整个词都不对的不猜。
 * 函数解析那条路早就这么做了，这里抽出来给所有地方共用。
 */
static void UAL_KeepCloseSuggestions(const FString& Wanted, TArray<FString>& InOutSuggestions)
{
	const int32 MaxDistance = FMath::Max(2, Wanted.Len() / 2);
	InOutSuggestions.RemoveAll([&Wanted, MaxDistance](const FString& S)
	{
		return UAL_CommandUtils::LevenshteinDistance(Wanted, S) > MaxDistance;
	});
}

/**
 * 找蓝图事件。
 *
 * 事件的 C++ 名和图上显示的名字差得比普通函数还远：
 * `BeginPlay` → `ReceiveBeginPlay`、`Tick` → `ReceiveTick`、
 * `ActorBeginOverlap` → `ReceiveActorBeginOverlap`。
 *
 * 原来的做法是硬编码 BeginPlay / Tick 两条，其余一律盲加 `Receive` 前缀 ——
 * 父类上的自定义事件、接口事件、以及任何不守这个命名约定的事件全都找不到，
 * 而且报错报的是**猜出来的名字**。
 *
 * 现在先走和普通函数同一套的名字匹配（DisplayName 元数据是权威来源），
 * 匹配不上再退回 `Receive` 前缀猜测 —— 那条在没有元数据的打包环境下仍然有用。
 */
static UFunction* UAL_FindEventFunction(UClass* OwnerClass, const FString& Name)
{
	if (!OwnerClass || Name.IsEmpty()) return nullptr;

	if (UFunction* Direct = UAL_FindFunctionByAnyName(OwnerClass, Name))
	{
		return Direct;
	}
	if (!Name.StartsWith(TEXT("Receive")))
	{
		if (UFunction* Prefixed = UAL_FindFunctionByAnyName(OwnerClass, TEXT("Receive") + Name))
		{
			return Prefixed;
		}
	}
	return nullptr;
}

/**
 * 按 C++ 名或细节面板里显示的名字找属性。
 *
 * 和函数那边同一个病：UE 的细节面板对布尔属性**会去掉 `b` 前缀** ——
 * `bCastShadow` 显示成 "Cast Shadow"、`bHidden` 显示成 "Hidden"。
 * 调用方照着界面上看到的写，`FindPropertyByName` 就找不到。
 */
static FProperty* UAL_FindPropertyByAnyName(UClass* Cls, const FString& Name)
{
	if (!Cls || Name.IsEmpty()) return nullptr;

	if (FProperty* Exact = Cls->FindPropertyByName(FName(*Name)))
	{
		return Exact;
	}
	// 布尔属性最常见，直接试一次比全量遍历便宜
	if (FProperty* Flagged = Cls->FindPropertyByName(FName(*(TEXT("b") + Name))))
	{
		return Flagged;
	}

	for (TFieldIterator<FProperty> It(Cls, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop) continue;
		if (Prop->GetName().Equals(Name, ESearchCase::IgnoreCase)) return Prop;
#if WITH_EDITOR
		static const FName DisplayNameKey(TEXT("DisplayName"));
		if (Prop->HasMetaData(DisplayNameKey)
			&& Prop->GetMetaData(DisplayNameKey).Replace(TEXT(" "), TEXT("")).Equals(Name, ESearchCase::IgnoreCase))
		{
			return Prop;
		}
#endif
	}
	return nullptr;
}

/** 这个类上有没有同名的**函数** —— 用来识别「你把函数当属性设了」 */
static bool UAL_HasFunctionNamed(UClass* Cls, const FString& Name)
{
	return UAL_FindFunctionByAnyName(Cls, Name) != nullptr;
}

/** 这个类上能放的事件，按**图上显示的名字**收集 */
static void UAL_CollectEventNames(UClass* OwnerClass, TArray<FString>& OutNames)
{
	if (!OwnerClass) return;
	for (TFieldIterator<UFunction> It(OwnerClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		UFunction* Func = *It;
		if (!Func || !Func->HasAnyFunctionFlags(FUNC_BlueprintEvent)) continue;
		OutNames.AddUnique(UAL_BlueprintFunctionName(Func));
	}
}

/**
 * 遍历可作为函数来源的类：蓝图自身的类链 + 全部蓝图函数库。
 *
 * 原来只查四个硬编码的库（KismetMath / KismetSystem / GameplayStatics / KismetString）。
 * 引擎里的 `UBlueprintFunctionLibrary` 子类有几百个 —— `UEnhancedInputLibrary`、
 * `UWidgetBlueprintLibrary`、`UNiagaraFunctionLibrary` 全都不在名单里，
 * 调用方一用到就报「找不到」。
 *
 * 现在改成遍历全部已加载的函数库，常用的那四个仍然**排在前面**：
 * 同名函数在多个库里存在时，先命中的才是调用方多半想要的那个。
 */
/**
 * 这个蓝图**身上挂着的东西**的类：控件树里的控件、构造脚本里的组件。
 *
 * 为什么单独一份、而不是并进 `UAL_ForEachFunctionSource`：
 *
 * 那个函数同时是**解析**范围（`member_name` 不带类名时按它逐个类找）。把控件
 * 和组件并进去，`member_name: "SetVisibility"` 这种裸名就可能解析到某个控件上 ——
 * 建出来的节点 Target 引脚指着别人，而调用方以为是自己。解析范围只能宽在
 * 「显式写了类名」这一侧（那条路本来就按类名全局查，认识任何类）。
 *
 * 搜索不一样：搜索只是**告诉调用方有什么**，回多了顶多是噪音，回少了却是骗它。
 * 2026-09-16 的反馈里就是这样：`TextBlock.SetText` 搜不到（UTextBlock 既不是
 * 函数库、也不在 WBP 的父类链上），调用方遵守「不要猜函数名」这条规矩，
 * 改用了搜得到的 `KismetSystemLibrary.SetTextPropertyByName` ——
 * 那个节点走反射直写字段，屏幕永远不重画。工具把它推进了一个静默错的行为。
 */
static void UAL_CollectOwnedClasses(UBlueprint* Blueprint, TArray<UClass*>& OutClasses)
{
	if (!Blueprint) return;

	auto AddClass = [&OutClasses](UClass* Cls)
	{
		if (Cls && !OutClasses.Contains(Cls))
		{
			OutClasses.Add(Cls);
		}
	};

	// Widget Blueprint：控件树里每一个控件的类（TextBlock、ProgressBar、Border…）
	if (UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Blueprint))
	{
		if (WidgetBP->WidgetTree)
		{
			WidgetBP->WidgetTree->ForEachWidget([&AddClass](UWidget* Widget)
			{
				if (Widget)
				{
					AddClass(Widget->GetClass());
				}
			});
		}
	}

	// Actor 蓝图：构造脚本里挂的组件（StaticMeshComponent、PointLightComponent…）。
	// 同一个坑的另一半 —— `StaticMeshComponent.SetStaticMesh` 一样搜不到
	if (Blueprint->SimpleConstructionScript)
	{
		for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			if (Node)
			{
				AddClass(Node->ComponentClass);
			}
		}
	}

	// 父类上用 C++ 声明的组件（CharacterMovementComponent 这类）不在构造脚本里，
	// 得从 CDO 的属性上取
	if (UClass* Own = Blueprint->GeneratedClass ? Blueprint->GeneratedClass.Get() : Blueprint->ParentClass.Get())
	{
		for (TFieldIterator<FObjectProperty> It(Own, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FObjectProperty* Prop = *It;
			if (Prop && Prop->PropertyClass
				&& Prop->PropertyClass->IsChildOf(UActorComponent::StaticClass()))
			{
				AddClass(Prop->PropertyClass);
			}
		}
	}
}

static void UAL_ForEachFunctionSource(UBlueprint* Blueprint, TFunctionRef<bool(UClass*)> Visit)
{
	if (Blueprint)
	{
		UClass* Own = Blueprint->GeneratedClass ? Blueprint->GeneratedClass.Get() : Blueprint->ParentClass.Get();
		if (Own && !Visit(Own)) return;
	}

	TSet<UClass*> Visited;
	for (const FString& LibName : UAL_CommonFunctionLibraries())
	{
		FString ClassError;
		if (UClass* LibClass = UAL_CommandUtils::ResolveClassFromIdentifier(LibName, UObject::StaticClass(), ClassError))
		{
			Visited.Add(LibClass);
			if (!Visit(LibClass)) return;
		}
	}

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Cls = *It;
		if (!Cls || Visited.Contains(Cls)) continue;
		if (!Cls->IsChildOf(UBlueprintFunctionLibrary::StaticClass())) continue;
		// 抽象基类和被弃用的类回给调用方只会浪费它一轮
		if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) continue;
		if (!Visit(Cls)) return;
	}
}

/**
 * 一个函数调用该用哪个节点类。
 *
 * ## 为什么不能一律 `UK2Node_CallFunction`
 *
 * 编辑器从菜单里拖一个函数出来时，节点类**不是固定的** ——
 * `UBlueprintFunctionNodeSpawner::Create`（引擎
 * `Editor/BlueprintGraph/Private/BlueprintFunctionNodeSpawner.cpp`）按函数上的
 * 元数据挑一个特化子类。少了这一步，节点建出来是「对的形状、死的行为」：
 *
 * `Array_Length` / `Array_Get` / `Array_LastIndex` 这些数组库函数，
 * 它们的 TargetArray 引脚被 `UEdGraphSchema_K2::IsWildcardProperty` 建成
 * **wildcard**，等着连上线之后按源引脚定型。而定型这件事写在
 * `UK2Node_CallArrayFunction::NotifyPinConnectionListChanged` 里 ——
 * 普通的 `UK2Node_CallFunction` 没有这个覆写，于是连线连上了、引脚却永远是
 * `wildcard(None)`，编译报「Target Array 的类型尚未确定，请连接一些内容」，
 * 而东西明明连着。这个状态**在工具侧无法修复**：重编译、重载资产、撤销重做、
 * 重启编辑器全都无效（连线本身是好的，坏的是引脚类型），只有人到编辑器里把线
 * 断开重连一次才会触发定型。2026-09-16 的反馈里，用户就是被请去手动重连了三根线。
 *
 * 数据表和材质参数集函数同理：它们的资产引脚逻辑长在各自的节点类上。
 *
 * ## 为什么只挑这三个
 *
 * 引擎那份名单里还有 `UK2Node_PromotableOperator` 和
 * `UK2Node_CommutativeAssociativeBinaryOperator`。那两个是**体验**差异
 * （类型自动提升、右键加引脚），用普通节点建出来照样编译得过、跑得对；
 * 这三个是**功能**差异，用错了引脚根本没法用。
 * 加分支要能说清为什么，不能照抄一张名单。
 */
static TSubclassOf<UK2Node_CallFunction> UAL_PickCallFunctionNodeClass(const UFunction* Function)
{
	if (!Function)
	{
		return UK2Node_CallFunction::StaticClass();
	}
	if (Function->HasMetaData(FBlueprintMetadata::MD_ArrayParam))
	{
		return UK2Node_CallArrayFunction::StaticClass();
	}
	if (Function->HasMetaData(FBlueprintMetadata::MD_DataTablePin))
	{
		return UK2Node_CallDataTableFunction::StaticClass();
	}
	if (Function->HasMetaData(FBlueprintMetadata::MD_MaterialParameterCollectionFunction))
	{
		return UK2Node_CallMaterialParameterCollectionFunction::StaticClass();
	}
	return UK2Node_CallFunction::StaticClass();
}

/** 把类型名归一化：去下划线、转小写。"Variable_Get" / "variableget" → "variableget" */
static FString UAL_NormalizeNodeType(const FString& Raw)
{
	return Raw.Replace(TEXT("_"), TEXT("")).ToLower();
}

// ============================================================================
// 事件分发器节点：Call / Bind Event to / Unbind Event from / Unbind all
//
// 这四项在编辑器里是分发器的右键菜单，但它们**不是函数调用**：建出来的是
// UK2Node_CallDelegate / AddDelegate / RemoveDelegate / ClearDelegate，
// 身上存的是一个指向委托属性的 FMemberReference（DelegateReference），
// 不是 FunctionReference。引擎自己的写法见
// Engine/Source/Editor/Kismet/Private/BPDelegateDragDropAction.cpp:66。
//
// 屏幕上那句 "Call On Chest Opened" 只是拿属性名拼出来的**标题**
// （K2Node_MCDelegate.cpp:474 的 Format），函数表里永远查不到它。
// 此前 event_dispatcher 建完分发器回的提示语正是让调用方按 `Call <Name>`
// 去建 class=Function —— 而那条路只认真实函数名，于是六种写法全落在
// "Function not found"，用户只能回编辑器手连（2026-09-16 反馈）。
//
// 四个节点类和 SetFromProperty / CreateFromFunction 的签名在 5.0–5.8 九版一致
// （2026-09-16 逐版本核过头文件）。
// ============================================================================

// 名字解析（"Call OnChestOpened" → Call + OnChestOpened）在
// UAL_DelegateNodeName.h 里 —— 那部分纯字符串，单独拆出去是为了能被自动化测试跑到
using EUAL_DelegateNodeKind = UAL_DelegateNodeName::EKind;

/**
 * 找委托属性的搜索起点。
 *
 * 优先 Skeleton 类：分发器刚用 blueprint.event_dispatcher 建出来时，
 * GeneratedClass 上还没有它（要等编译），而调用方多半是建完当场就来连图的。
 * 引擎自己在 GetDelegateSignature 里也是这个偏好（K2Node_MCDelegate.cpp:124）。
 */
static UClass* UAL_DelegateSearchScope(UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return nullptr;
	}
	if (Blueprint->SkeletonGeneratedClass)
	{
		return Blueprint->SkeletonGeneratedClass;
	}
	if (Blueprint->GeneratedClass)
	{
		return Blueprint->GeneratedClass;
	}
	return Blueprint->ParentClass;
}

/** 在一个类上按名字找委托属性；顺带收齐候选名，找不到时要写进错误信息 */
static FMulticastDelegateProperty* UAL_FindDelegateProperty(
	UClass* Scope,
	const FString& Name,
	TArray<FString>& OutAvailable)
{
	if (!Scope)
	{
		return nullptr;
	}

	FMulticastDelegateProperty* Exact = nullptr;
	FMulticastDelegateProperty* Loose = nullptr;
	const FString Compact = UAL_DelegateNodeName::Compact(Name);

	for (TFieldIterator<FMulticastDelegateProperty> It(Scope, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		FMulticastDelegateProperty* Property = *It;
		if (!Property)
		{
			continue;
		}
		const FString PropertyName = Property->GetName();
		OutAvailable.AddUnique(PropertyName);

		if (!Exact && PropertyName.Equals(Name, ESearchCase::CaseSensitive))
		{
			Exact = Property;
		}
		else if (!Loose && UAL_DelegateNodeName::Compact(PropertyName) == Compact)
		{
			Loose = Property;
		}
	}

	return Exact ? Exact : Loose;
}

/**
 * 把 `ClassName.VarName` 里的类名解析成类，读写别的蓝图的变量用。
 *
 * 比函数那条路（只按已加载的类名精确查）宽，因为调用方最自然的写法是蓝图
 * 资产名 `WBP_HUD`，而类名叫 `WBP_HUD_C`，那个蓝图还可能压根没加载。
 * 依次试：自己 → 已加载的类（原名 / 补 _C）→ 按资产名或路径加载蓝图。
 *
 * 资产名要整名相等：UAL_LoadBlueprintByPathOrName 末尾那步是路径包含匹配，
 * `HUD` 会命中 `WBP_HUD`，在这里就等于悄悄读了另一个蓝图的变量。
 */
static UClass* UAL_ResolveVariableOwnerClass(UBlueprint* Blueprint, const FString& ClassPart, FString& OutError)
{
	if (Blueprint && Blueprint->GeneratedClass &&
		(ClassPart == Blueprint->GeneratedClass->GetName() || ClassPart == Blueprint->GetName()))
	{
		return Blueprint->GeneratedClass;
	}

	FString ClassError;
	if (UClass* Found = UAL_CommandUtils::ResolveClassFromIdentifier(ClassPart, UObject::StaticClass(), ClassError))
	{
		return Found;
	}

	const bool bHasClassSuffix = ClassPart.EndsWith(TEXT("_C"));
	if (!bHasClassSuffix && !ClassPart.StartsWith(TEXT("/")))
	{
		FString Ignored;
		if (UClass* Found = UAL_CommandUtils::ResolveClassFromIdentifier(ClassPart + TEXT("_C"), UObject::StaticClass(), Ignored))
		{
			return Found;
		}
	}

	FString AssetName = ClassPart;
	if (bHasClassSuffix)
	{
		AssetName.LeftChopInline(2);
	}
	UBlueprint* OwnerBP = nullptr;
	FString ResolvedPath;
	if (UAL_LoadBlueprintByPathOrName(AssetName, OwnerBP, ResolvedPath) && OwnerBP &&
		(AssetName.StartsWith(TEXT("/")) || OwnerBP->GetName().Equals(AssetName, ESearchCase::IgnoreCase)))
	{
		if (OwnerBP->GeneratedClass)
		{
			return OwnerBP->GeneratedClass;
		}
		OutError = FString::Printf(TEXT("Blueprint %s has no generated class yet - compile it once, then retry"), *OwnerBP->GetName());
		return nullptr;
	}

	// 死包里的同名类那种错误比「找不到」有用，原样带出去
	OutError = ClassError.Contains(TEXT("stale"))
		? ClassError
		: FString::Printf(
			TEXT("Class not found: %s (write the blueprint asset name like WBP_HUD, its path like /Game/UI/WBP_HUD, or a C++ class name)"),
			*ClassPart);
	return nullptr;
}

/**
 * 在别的类上找变量。蓝图类先查骨架类：刚用 blueprint_add_variable 加上、
 * 还没编译的变量只在骨架类上有，生成类要等编译 —— 引擎自己的
 * UK2Node_Variable::CreatePinForVariable 也是这么兜的。FindFProperty 带父类链。
 */
static FProperty* UAL_FindVariableOnClass(UClass* OwnerClass, FName VarName)
{
	if (UBlueprint* OwnerBP = UBlueprint::GetBlueprintFromClass(OwnerClass))
	{
		if (UClass* Skeleton = OwnerBP->SkeletonGeneratedClass)
		{
			if (FProperty* Property = FindFProperty<FProperty>(Skeleton, VarName))
			{
				return Property;
			}
		}
	}
	return FindFProperty<FProperty>(OwnerClass, VarName);
}

/**
 * 建一个读写别人变量的节点 —— 编辑器里从对象引脚拖出来 Get / Set 的那种，
 * 多一根 Target 引脚接那个对象。
 *
 * 和委托节点同一个讲究：SetFromProperty 要在 Finalize 之前，引脚是解析
 * VariableReference 长出来的。SetFromProperty 会把骨架类换成正式的生成类、
 * 顺手填上变量 GUID（变量改名后节点还认得），自己拼 SetExternalMember 两样都没有。
 */
template <typename TNode>
static TNode* UAL_SpawnExternalVariableNode(UEdGraph* Graph, const FProperty* Property, int32 PosX, int32 PosY)
{
	FGraphNodeCreator<TNode> NodeCreator(*Graph);
	TNode* Node = NodeCreator.CreateNode();
	Node->SetFromProperty(Property, /*bSelfContext=*/false, Property->GetOwnerClass());
	Node->NodePosX = PosX;
	Node->NodePosY = PosY;
	NodeCreator.Finalize();
	Node->ReconstructNode();
	return Node;
}

/**
 * 建一个委托节点。
 *
 * `SetFromProperty` 必须在 `Finalize()` 之前 —— 委托节点的引脚（Target，
 * 以及广播节点上照签名长出来的那几个参数）是解析 DelegateReference 得来的，
 * 先 Finalize 的话拿不到属性，节点上就只剩 exec 两根，连都没得连。
 * 引擎的 MakeMCDelegateNode 用 SpawnNode 的 InitializerFn 做的是同一件事。
 */
template <typename TNode>
static TNode* UAL_SpawnDelegateNode(
	UEdGraph* Graph,
	FMulticastDelegateProperty* Delegate,
	bool bSelfContext,
	UClass* OwnerClass,
	int32 PosX,
	int32 PosY)
{
	FGraphNodeCreator<TNode> NodeCreator(*Graph);
	TNode* Node = NodeCreator.CreateNode();
	Node->SetFromProperty(Delegate, bSelfContext, OwnerClass);
	Node->NodePosX = PosX;
	Node->NodePosY = PosY;
	NodeCreator.Finalize();
	Node->ReconstructNode();
	return Node;
}

/**
 * 造一个节点。
 *
 * 失败时写 `OutError` 并回 nullptr —— **不发响应**。这是它能被两条路共用的
 * 前提：批量那条路要把每个节点的失败收集起来一起回，不能谁先失败谁就把
 * 整个 WebSocket 响应发了。
 *
 * `bOutReused` 表示复用了图里本来就有的节点而不是新建。事件节点必须复用：
 * 新建的 Actor 蓝图 EventGraph 里本来就带着 BeginPlay / Tick / ActorBeginOverlap
 * 三个占位节点，再建一个就有两个 BeginPlay，编译不报错但运行时两条链都执行。
 *
 * `OutTimelineSnapshot` 给批量那条路用：Timeline 的模板是蓝图上的独立对象，
 * 删节点带不走对它的修改，整批回滚必须照这张快照回填（见 FUAL_TimelineSnapshot）。
 * 单节点那条路自带事务、没有「整批」可回滚，传 nullptr 即可。
 */
static UEdGraphNode* UAL_CreateNodeInternal(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const FUAL_NodeSpec& Spec,
	bool& bOutReused,
	FString& OutError,
	FUAL_TimelineSnapshot* OutTimelineSnapshot = nullptr
)
{
	bOutReused = false;

	if (!Blueprint || !Graph)
	{
		OutError = TEXT("Invalid Blueprint or Graph");
		return nullptr;
	}

	const FString T = UAL_NormalizeNodeType(Spec.Type);
	const int32 PosX = Spec.PosX;
	const int32 PosY = Spec.PosY;

	// 参数表只有自定义事件收。给错了地方就直说 ——
	// 悄悄忽略的话调用方以为建了个带参数的节点，回头连线才发现引脚不存在，
	// 而那时候它已经不记得参数是在哪一步丢的了
	if (Spec.EventParams.Num() > 0 && T != TEXT("customevent"))
	{
		OutError = FString::Printf(
			TEXT("\"params\" is only supported on class=CustomEvent (got \"%s\"). ")
			TEXT("Function graphs take their parameters from blueprint_create_function / blueprint.function_signature; ")
			TEXT("engine events (class=Event) have a fixed signature you cannot change."),
			*Spec.Type);
		return nullptr;
	}

	// 复制选项同理：引擎事件和函数的网络属性由声明方定，这里改不了
	if ((!Spec.EventReplication.IsEmpty() || Spec.bHasEventReliable) && T != TEXT("customevent"))
	{
		OutError = FString::Printf(
			TEXT("\"replication\" / \"reliable\" are only supported on class=CustomEvent (got \"%s\"). ")
			TEXT("In Blueprints an RPC is a CustomEvent with replication=Server/Client/Multicast."),
			*Spec.Type);
		return nullptr;
	}

	// ===== Event =====
	if (T == TEXT("event"))
	{
		UClass* OwnerClass = Blueprint->ParentClass ? Blueprint->ParentClass.Get() : AActor::StaticClass();
		UFunction* EventFunc = UAL_FindEventFunction(OwnerClass, Spec.Name);
		if (!EventFunc)
		{
			// 报**调用方传进来的**名字，不是我们猜出来的那个。原来打印的是
			// 加了 Receive 前缀之后的串（传 Foo 报 "ReceiveFoo not found"），
			// 调用方对着一个自己没写过的名字根本无从查起。
			OutError = FString::Printf(TEXT("Event not found: %s"), *Spec.Name);

			TArray<FString> Available;
			UAL_CollectEventNames(OwnerClass, Available);
			TArray<FString> Suggestions;
			UAL_CommandUtils::SuggestProperties(Spec.Name, Available, Suggestions, 5);
			UAL_KeepCloseSuggestions(Spec.Name, Suggestions);

			if (Suggestions.Num() > 0)
			{
				OutError += FString::Printf(TEXT(" (did you mean: %s?)"), *FString::Join(Suggestions, TEXT(", ")));
			}
			else if (Available.Num() > 0)
			{
				// 一个都不像时把前若干个列出来 —— 对事件来说这份清单不长，
				// 直接给全比让调用方再猜一轮便宜
				TArray<FString> Head;
				for (int32 i = 0; i < FMath::Min(15, Available.Num()); ++i) Head.Add(Available[i]);
				OutError += FString::Printf(TEXT(" (available on %s: %s%s)"),
					*OwnerClass->GetName(),
					*FString::Join(Head, TEXT(", ")),
					Available.Num() > Head.Num() ? TEXT(", ...") : TEXT(""));
			}
			// 拿 Input Action 资产名来建 Event 是最常见的一种错法，直接指路
			if (Spec.Name.StartsWith(TEXT("IA_"), ESearchCase::IgnoreCase))
			{
				OutError += TEXT(". That looks like an Input Action asset: use class=\"EnhancedInputAction\" with member_name=\"") + Spec.Name + TEXT("\"");
			}
			return nullptr;
		}

		// 同名事件已存在就复用 —— 对齐编辑器里手动放置的行为（会跳到已有那个）
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Event* Candidate = Cast<UK2Node_Event>(Node);
			if (Candidate && Candidate->EventReference.GetMemberName() == EventFunc->GetFName())
			{
				bOutReused = true;
				return Candidate;
			}
		}

		FGraphNodeCreator<UK2Node_Event> NodeCreator(*Graph);
		UK2Node_Event* EventNode = NodeCreator.CreateNode();
		EventNode->EventReference.SetExternalMember(EventFunc->GetFName(), OwnerClass);
		EventNode->bOverrideFunction = true;
		EventNode->NodePosX = PosX;
		EventNode->NodePosY = PosY;
		NodeCreator.Finalize();
		EventNode->ReconstructNode();
		return EventNode;
	}

	// ===== 事件分发器（Call / Bind Event to / Unbind Event from / Unbind all）=====
	// 放在 Function 前面：class=Function + member_name="Call OnChestOpened" 这种写法
	// 要先在这里被认出来。认不出属性会掉下去走普通函数解析，不会劫持真函数
	{
		FString DelegateName;
		const EUAL_DelegateNodeKind DelegateKind = UAL_DelegateNodeName::Parse(T, Spec.Name, DelegateName);
		const bool bExplicitDelegateClass =
			DelegateKind != EUAL_DelegateNodeKind::None && T != TEXT("function") && T != TEXT("callfunction");

		if (DelegateKind != EUAL_DelegateNodeKind::None)
		{
			// 别人身上的分发器写成 "BP_Chest.OnChestOpened" —— 跨 Actor 订阅是
			// 这四个节点最主要的用法，只认自己身上的等于没做
			FString ClassPart, NamePart;
			if (!DelegateName.Split(TEXT("."), &ClassPart, &NamePart))
			{
				NamePart = DelegateName;
			}

			UClass* Scope = nullptr;
			FString ScopeError;
			if (!ClassPart.IsEmpty())
			{
				Scope = UAL_CommandUtils::ResolveClassFromIdentifier(ClassPart, UObject::StaticClass(), ScopeError);
				if (!Scope)
				{
					Scope = UAL_CommandUtils::ResolveClassFromIdentifier(TEXT("U") + ClassPart, UObject::StaticClass(), ScopeError);
				}
			}
			else
			{
				Scope = UAL_DelegateSearchScope(Blueprint);
			}

			TArray<FString> Available;
			FMulticastDelegateProperty* Delegate = UAL_FindDelegateProperty(Scope, NamePart, Available);

			if (Delegate || bExplicitDelegateClass)
			{
				if (!Delegate)
				{
					OutError = FString::Printf(TEXT("Event dispatcher not found: %s"), *DelegateName);
					if (Available.Num() > 0)
					{
						TArray<FString> Head;
						for (int32 i = 0; i < FMath::Min(15, Available.Num()); ++i) Head.Add(Available[i]);
						OutError += FString::Printf(TEXT(" (available on %s: %s%s)"),
							Scope ? *Scope->GetName() : TEXT("?"),
							*FString::Join(Head, TEXT(", ")),
							Available.Num() > Head.Num() ? TEXT(", ...") : TEXT(""));
					}
					else if (!ScopeError.IsEmpty())
					{
						OutError += FString::Printf(TEXT(" (%s)"), *ScopeError);
					}
					OutError += TEXT(". Use blueprint.event_dispatcher (action=list) to see this blueprint's dispatchers, ")
						TEXT("or \"ClassName.DispatcherName\" for one on another class; ")
						TEXT("component events (OnComponentBeginOverlap etc.) go through blueprint.component_event");
					return nullptr;
				}

				// 广播和绑定要的是**两个不同的**标志位。放到编译期才发现的话，
				// 报的是「Event Dispatcher is not 'BlueprintCallable'」挂在某个节点上，
				// 而调用方这时早已不知道是哪一步的事了
				const bool bBroadcast = (DelegateKind == EUAL_DelegateNodeKind::Call);
				if (bBroadcast && !Delegate->HasAllPropertyFlags(CPF_BlueprintCallable))
				{
					OutError = FString::Printf(
						TEXT("Event dispatcher '%s' cannot be broadcast from blueprints (not BlueprintCallable) - you can only bind to it"),
						*Delegate->GetName());
					return nullptr;
				}
				if (!bBroadcast && !Delegate->HasAllPropertyFlags(CPF_BlueprintAssignable))
				{
					OutError = FString::Printf(
						TEXT("Event dispatcher '%s' cannot be bound from blueprints (not BlueprintAssignable)"),
						*Delegate->GetName());
					return nullptr;
				}

				UClass* OwnerClass = Delegate->GetOwnerClass();
				// 自己（或父类）身上的分发器不需要 Target 引脚上再接一个自己
				UClass* SelfScope = UAL_DelegateSearchScope(Blueprint);
				const bool bSelfContext = (OwnerClass == nullptr) || (SelfScope && SelfScope->IsChildOf(OwnerClass));

				switch (DelegateKind)
				{
				case EUAL_DelegateNodeKind::Call:
					return UAL_SpawnDelegateNode<UK2Node_CallDelegate>(Graph, Delegate, bSelfContext, OwnerClass, PosX, PosY);
				case EUAL_DelegateNodeKind::Add:
					return UAL_SpawnDelegateNode<UK2Node_AddDelegate>(Graph, Delegate, bSelfContext, OwnerClass, PosX, PosY);
				case EUAL_DelegateNodeKind::Remove:
					return UAL_SpawnDelegateNode<UK2Node_RemoveDelegate>(Graph, Delegate, bSelfContext, OwnerClass, PosX, PosY);
				case EUAL_DelegateNodeKind::Clear:
					return UAL_SpawnDelegateNode<UK2Node_ClearDelegate>(Graph, Delegate, bSelfContext, OwnerClass, PosX, PosY);
				default:
					break;
				}
			}
		}
	}

	// ===== Function =====
	// "CallFunction" 是调用方 schema 里的写法，"Function" 是插件历史写法 ——
	// 两个都认。以前只有 add_node 那条路认 function、声明式那条路两个都不认，
	// 而工具示例写的恰恰是 CallFunction。
	if (T == TEXT("function") || T == TEXT("callfunction"))
	{
		FString ClassPart, FuncPart;
		if (!Spec.Name.Split(TEXT("."), &ClassPart, &FuncPart))
		{
			FuncPart = Spec.Name;
		}

		UFunction* TargetFunc = nullptr;
		UClass* TargetClass = nullptr;

		if (!ClassPart.IsEmpty())
		{
			FString ClassError;

			// 自引用（`BP_Foo_C.MyFunc`）直接用这个蓝图自己的生成类。
			// 「我自己是谁」不需要按名字全局查，而按名字查是会出错的：内存里可能
			// 还躺着一个同名的旧类（资产重载留下的），查到它的话节点建得出来、
			// 编译却报「此蓝图（自身）并非是一个 BP_Foo_C」——
			// 一句完全不提「有两个同名类」的错误。
			if (Blueprint)
			{
				if (UClass* Own = Blueprint->GeneratedClass)
				{
					if (ClassPart == Own->GetName())
					{
						TargetClass = Own;
					}
				}
			}

			if (!TargetClass)
			{
				TargetClass = UAL_CommandUtils::ResolveClassFromIdentifier(ClassPart, UObject::StaticClass(), ClassError);
			}
			if (!TargetClass)
			{
				// 兼容 KismetSystemLibrary 这种不带 U 前缀的写法
				TargetClass = UAL_CommandUtils::ResolveClassFromIdentifier(TEXT("U") + ClassPart, UObject::StaticClass(), ClassError);
			}
			if (!TargetClass)
			{
				OutError = ClassError;
				return nullptr;
			}
			// 按蓝图名也要认得出来：调用方写的是图上显示的 GetActorLocation，
			// 而 C++ 里它叫 K2_GetActorLocation（见 UAL_BlueprintFunctionName）
			TargetFunc = UAL_FindFunctionByAnyName(TargetClass, FuncPart);
		}
		else
		{
			TargetClass = nullptr;
			TargetFunc = nullptr;
			UAL_ForEachFunctionSource(Blueprint, [&](UClass* Cls) -> bool
			{
				if (UFunction* F = UAL_FindFunctionByAnyName(Cls, FuncPart))
				{
					TargetClass = Cls;
					TargetFunc = F;
					return false; // 找到就停，常用库排在前面所以先命中的就是想要的
				}
				return true;
			});
		}

		if (!TargetFunc || !TargetClass)
		{
			// 近似匹配写进错误信息本身 —— 调用方拿到的是「你要的没有，这几个像」，
			// 而不是光一句「找不到」然后自己再猜一轮
			// 候选名收**蓝图名**而不是 C++ 名 —— 建议里回一个 `K2_GetActorLocation`，
			// 调用方照着填进去还是会被拒，等于把它带进第二个坑
			TArray<FString> Candidates;
			UAL_ForEachFunctionSource(Blueprint, [&Candidates](UClass* Cls) -> bool
			{
				for (TFieldIterator<UFunction> It(Cls, EFieldIteratorFlags::IncludeSuper); It; ++It)
				{
					UFunction* UF = *It;
					if (!UF) continue;
					// 蓝图里放不出来的函数建议了也没用
					if (!UF->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_BlueprintEvent))
					{
						continue;
					}
					Candidates.AddUnique(UAL_BlueprintFunctionName(UF));
				}
				return true;
			});

			/**
			 * 控件 / 组件身上的函数也当候选，但**按裸名参与匹配、按全名回**。
			 *
			 * 两件事各归各：匹配靠编辑距离（`SetTextt` → `SetText` 差 1，捞得回来；
			 * 而 `SetTextt` → `TextBlock.SetText` 差 10，阈值是 max(2, 长度/2)，
			 * 直接被扔掉），所以候选里放裸名；但建议里必须回 `TextBlock.SetText` ——
			 * 裸名不查这些类（理由见 `UAL_CollectOwnedClasses`），调用方照着裸名
			 * 填回来还是解析不到，等于把它带进第二个坑。
			 */
			TMap<FString, FString> QualifyOwned;
			TArray<UClass*> OwnedClasses;
			UAL_CollectOwnedClasses(Blueprint, OwnedClasses);
			for (UClass* Cls : OwnedClasses)
			{
				for (TFieldIterator<UFunction> It(Cls, EFieldIteratorFlags::IncludeSuper); It; ++It)
				{
					UFunction* UF = *It;
					if (!UF) continue;
					if (!UF->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_BlueprintEvent))
					{
						continue;
					}
					const FString BareName = UAL_BlueprintFunctionName(UF);
					// 函数库里已经有同名的就不抢：那个裸名本来就解析得到
					if (Candidates.Contains(BareName)) continue;

					UClass* Declaring = UF->GetOwnerClass() ? UF->GetOwnerClass() : Cls;
					Candidates.Add(BareName);
					QualifyOwned.Add(BareName, FString::Printf(
						TEXT("%s.%s"), *Declaring->GetName(), *BareName));
				}
			}

			TArray<FString> Suggestions;
			UAL_CommandUtils::SuggestProperties(FuncPart, Candidates, Suggestions, 5);

			// 只留真的像的那几个，理由见 UAL_KeepCloseSuggestions。
			// 一个都不像时不猜，改为指向 search_nodes（见下面的 else 分支）
			UAL_KeepCloseSuggestions(FuncPart, Suggestions);

			// 匹配完再把控件 / 组件那几个换成带类名的写法
			for (FString& Suggestion : Suggestions)
			{
				if (const FString* Qualified = QualifyOwned.Find(Suggestion))
				{
					Suggestion = *Qualified;
				}
			}

			OutError = FString::Printf(TEXT("Function not found: %s"), *Spec.Name);

			// 带空格的名字多半是从编辑器标题抄来的（"Call On Chest Opened"）。
			// 上面的委托分支已经试过并且没找到那个分发器，所以这里直接指路，
			// 而不是让它对着一份函数名清单继续猜
			if (Spec.Name.Contains(TEXT(" ")))
			{
				OutError += TEXT(" (names with spaces are editor node titles, not member names; ")
					TEXT("for event dispatchers use blueprint.event_dispatcher action=list to get the exact name)");
			}
			else if (Suggestions.Num() > 0)
			{
				OutError += FString::Printf(
					TEXT(" (did you mean: %s? use ClassName.FunctionName)"),
					*FString::Join(Suggestions, TEXT(", ")));
			}
			else
			{
				// 没有像的就别硬猜，直接告诉它去哪儿查
				OutError += TEXT(" (no similar name found - use blueprint.search_nodes to look up the exact member_name)");
			}
			return nullptr;
		}

		// 节点类按函数元数据挑，不能一律 CallFunction —— 见 UAL_PickCallFunctionNodeClass
		UClass* const NodeClass = UAL_PickCallFunctionNodeClass(TargetFunc);
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3)
		FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(*Graph);
		UK2Node_CallFunction* CallNode = NodeCreator.CreateNode(/*bSelectNewNode=*/true, NodeClass);
		CallNode->FunctionReference.SetExternalMember(TargetFunc->GetFName(), TargetClass);
		CallNode->NodePosX = PosX;
		CallNode->NodePosY = PosY;
		NodeCreator.Finalize();
#else
		// 5.0–5.2 的 FGraphNodeCreator::CreateNode 只收 bSelectNewNode，没有能指定
		// 节点类的那个重载（TSubclassOf<NodeType> 版本 5.3 才加）。用它就只能造出
		// 裸 UK2Node_CallFunction，等于把挑好的子类丢了。
		//
		// 所以这边自己造：NewObject + AddNode 是 UEdGraph::CreateNode 的原样展开
		// （见引擎 EdGraph.cpp，就是 NewObject(RF_Transactional) → 继承 RF_Transient
		// → AddNode 三步），CreateNode 本身在 5.0 是 protected，外面调不到。
		// 收尾再照 FGraphNodeCreator::Finalize 走一遍：CreateNewGuid /
		// PostPlacedNewNode / 没引脚才 AllocateDefaultPins —— 那三步从 5.0 到 5.8
		// 一个字没变，核对过引擎的 EdGraph.h。
		UK2Node_CallFunction* CallNode =
			NewObject<UK2Node_CallFunction>(Graph, NodeClass, NAME_None, RF_Transactional);
		if (!CallNode)
		{
			OutError = TEXT("failed to create the call-function node");
			return nullptr;
		}
		if (Graph->HasAnyFlags(RF_Transient))
		{
			CallNode->SetFlags(RF_Transient);
		}
		Graph->AddNode(CallNode, /*bUserAction=*/false, /*bSelectNewNode=*/true);
		CallNode->FunctionReference.SetExternalMember(TargetFunc->GetFName(), TargetClass);
		CallNode->NodePosX = PosX;
		CallNode->NodePosY = PosY;
		CallNode->CreateNewGuid();
		CallNode->PostPlacedNewNode();
		if (CallNode->Pins.Num() == 0)
		{
			CallNode->AllocateDefaultPins();
		}
#endif
		CallNode->ReconstructNode();
		return CallNode;
	}

	// ===== VariableGet / VariableSet =====
	if (T == TEXT("variableget") || T == TEXT("variableset"))
	{
		const bool bIsGet = (T == TEXT("variableget"));

		/**
		 * 别的蓝图身上的变量写成 `WBP_HUD.Health` —— 编辑器里从对象引脚拖出来
		 * 读写变量的那种节点。以前只认自己身上的，用户只好在对方蓝图里给每个
		 * 变量包一个 Get 函数再去调（2026-10-01 用户反馈）。
		 *
		 * 类名指的是自己或自己的父类时不算「别人」，去掉类名走下面自己那条路：
		 * 那条路还认组件，而且不需要 Target 上再接一个自己。
		 */
		FString VarNamePart = Spec.Name;
		FString OwnerPart;
		if (Spec.Name.Split(TEXT("."), &OwnerPart, &VarNamePart, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			UClass* OwnerClass = UAL_ResolveVariableOwnerClass(Blueprint, OwnerPart, OutError);
			if (!OwnerClass)
			{
				return nullptr;
			}

			UClass* SelfClass = Blueprint->GeneratedClass ? Blueprint->GeneratedClass.Get() : Blueprint->SkeletonGeneratedClass.Get();
			const bool bIsSelf = SelfClass && SelfClass->IsChildOf(OwnerClass);
			if (!bIsSelf)
			{
				const FName ExternalName(*VarNamePart);
				FProperty* Property = UAL_FindVariableOnClass(OwnerClass, ExternalName);
				UBlueprint* OwnerBP = UBlueprint::GetBlueprintFromClass(OwnerClass);
				const FString OwnerName = OwnerBP ? OwnerBP->GetName() : OwnerClass->GetName();

				// 控件只有勾了「是变量」才会在生成类上变成可见属性 —— UMG 跨蓝图
				// 读控件最常见的卡点。没勾时属性可能压根不在，也可能在但不可见，两处都要认
				const UWidgetBlueprint* OwnerWidgetBP = Cast<UWidgetBlueprint>(OwnerBP);
				const bool bIsHiddenWidget = OwnerWidgetBP && OwnerWidgetBP->WidgetTree &&
					OwnerWidgetBP->WidgetTree->FindWidget(ExternalName) &&
					(!Property || !Property->HasAnyPropertyFlags(CPF_BlueprintVisible));
				if (bIsHiddenWidget)
				{
					OutError = FString::Printf(
						TEXT("Widget '%s' in %s is not marked 'Is Variable', so other blueprints cannot reach it. ")
						TEXT("Tick 'Is Variable' on that widget in the UMG designer, compile %s, then retry"),
						*VarNamePart, *OwnerName, *OwnerName);
					return nullptr;
				}

				if (!Property)
				{
					OutError = FString::Printf(TEXT("Variable not found: %s has no variable named %s"), *OwnerName, *VarNamePart);

					UClass* ListClass = (OwnerBP && OwnerBP->SkeletonGeneratedClass) ? OwnerBP->SkeletonGeneratedClass.Get() : OwnerClass;
					TArray<FString> Available;
					for (TFieldIterator<FProperty> It(ListClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
					{
						if (It->HasAnyPropertyFlags(CPF_BlueprintVisible) && !It->HasMetaData(FBlueprintMetadata::MD_Private))
						{
							Available.AddUnique(It->GetName());
						}
					}
					TArray<FString> Suggestions;
					UAL_CommandUtils::SuggestProperties(VarNamePart, Available, Suggestions, 5);
					UAL_KeepCloseSuggestions(VarNamePart, Suggestions);
					if (Suggestions.Num() > 0)
					{
						OutError += FString::Printf(TEXT(" (did you mean: %s.%s?)"), *OwnerPart, *FString::Join(Suggestions, *FString::Printf(TEXT(", %s."), *OwnerPart)));
					}
					else
					{
						OutError += FString::Printf(TEXT(" (use blueprint_describe on %s to list its variables)"), *OwnerName);
					}
					return nullptr;
				}

				if (!Property->HasAnyPropertyFlags(CPF_BlueprintVisible))
				{
					OutError = FString::Printf(
						TEXT("'%s' exists on %s but is not exposed to blueprints (no BlueprintReadWrite / BlueprintReadOnly)"),
						*VarNamePart, *OwnerName);
					return nullptr;
				}

				// 编辑器的规矩（BlueprintActionFilter::IsFieldInaccessible）：
				// Private 只有自己能碰，Protected 只有子类能碰 —— 这里两样都不是
				const bool bIsPrivate = Property->HasMetaData(FBlueprintMetadata::MD_Private);
				if (bIsPrivate || Property->HasMetaData(FBlueprintMetadata::MD_Protected))
				{
					OutError = FString::Printf(
						TEXT("'%s' on %s is %s, so other blueprints cannot read or write it. ")
						TEXT("Make it public in %s, or call a public function on it instead"),
						*VarNamePart, *OwnerName, bIsPrivate ? TEXT("Private") : TEXT("Protected"), *OwnerName);
					return nullptr;
				}

				if (!bIsGet && Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
				{
					OutError = FString::Printf(
						TEXT("'%s' on %s is read-only to blueprints; it can be read (VariableGet) but not assigned (VariableSet)"),
						*VarNamePart, *OwnerName);
					if (Cast<UWidgetBlueprint>(OwnerBP) && CastField<FObjectProperty>(Property))
					{
						OutError += TEXT(". Widget variables are fixed references - get the widget and call its functions (SetText, SetVisibility...) instead");
					}
					return nullptr;
				}

				if (bIsGet)
				{
					return UAL_SpawnExternalVariableNode<UK2Node_VariableGet>(Graph, Property, PosX, PosY);
				}
				return UAL_SpawnExternalVariableNode<UK2Node_VariableSet>(Graph, Property, PosX, PosY);
			}
		}

		const FName VarName(*VarNamePart);
		const bool bIsUserVariable = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName) != INDEX_NONE;

		/**
		 * 组件也是变量：SCS 里每个组件在生成类上就是一个同名属性，编辑器里从
		 * Components 面板拖进图里的正是这种 VariableGet。以前只认 NewVariables，
		 * 「拿 Light 组件给它换材质」被顶回 Variable not found —— 真机上新用户
		 * 只能自己手连。父类（C++ 或父蓝图）声明的属性同理，骨架类的继承链里都有。
		 *
		 * 三条路最后都是 SetSelfMember(名字)：FMemberReference 在 self 语境下按名字
		 * 沿类继承链解析，编辑器自己也是这么建的。
		 */
		bool bIsComponent = false;
		if (!bIsUserVariable)
		{
			if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
			{
				for (USCS_Node* SCSNode : SCS->GetAllNodes())
				{
					if (SCSNode && SCSNode->GetVariableName() == VarName)
					{
						bIsComponent = true;
						break;
					}
				}
			}
		}

		FProperty* InheritedProperty = nullptr;
		if (!bIsUserVariable && !bIsComponent)
		{
			UClass* LookupClass = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->GeneratedClass.Get();
			if (!LookupClass)
			{
				LookupClass = Blueprint->ParentClass;
			}
			if (LookupClass)
			{
				InheritedProperty = FindFProperty<FProperty>(LookupClass, VarName);
			}
		}

		if (!bIsUserVariable && !bIsComponent && !InheritedProperty)
		{
			OutError = FString::Printf(TEXT("Variable not found: %s"), *VarNamePart);

			TArray<FString> Available;
			for (const FBPVariableDescription& Var : Blueprint->NewVariables)
			{
				Available.Add(Var.VarName.ToString());
			}
			if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
			{
				for (USCS_Node* SCSNode : SCS->GetAllNodes())
				{
					if (SCSNode)
					{
						Available.Add(SCSNode->GetVariableName().ToString());
					}
				}
			}
			TArray<FString> Suggestions;
			UAL_CommandUtils::SuggestProperties(VarNamePart, Available, Suggestions, 5);
			UAL_KeepCloseSuggestions(VarNamePart, Suggestions);
			if (Suggestions.Num() > 0)
			{
				OutError += FString::Printf(TEXT(" (did you mean: %s?)"), *FString::Join(Suggestions, TEXT(", ")));
			}
			else
			{
				OutError += FString::Printf(
					TEXT(" (variables and components on this blueprint: %s. Create a variable with blueprint_add_variable, a component with blueprint_add_component. ")
					TEXT("A variable on another blueprint is written as BlueprintName.VariableName, e.g. WBP_HUD.Health)"),
					Available.Num() > 0 ? *FString::Join(Available, TEXT(", ")) : TEXT("none"));
			}
			return nullptr;
		}

		// 组件变量能 Set：编辑器自己就允许（IsPropertyWritableInBlueprint 对组件没有特例），
		// 父蓝图声明的组件走下面的继承属性路径也一直放行 —— 自己的组件不该比它们窄
		if (InheritedProperty)
		{
			const FString OwnerName = InheritedProperty->GetOwnerClass() ? InheritedProperty->GetOwnerClass()->GetName() : TEXT("parent class");
			if (!InheritedProperty->HasAnyPropertyFlags(CPF_BlueprintVisible))
			{
				OutError = FString::Printf(
					TEXT("'%s' exists on %s but is not exposed to blueprints (no BlueprintReadWrite / BlueprintReadOnly)"),
					*VarNamePart, *OwnerName);
				return nullptr;
			}
			if (!bIsGet && InheritedProperty->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
			{
				OutError = FString::Printf(
					TEXT("'%s' is BlueprintReadOnly on %s; it can be read (VariableGet) but not assigned (VariableSet)"),
					*VarNamePart, *OwnerName);
				return nullptr;
			}
		}

		if (bIsGet)
		{
			FGraphNodeCreator<UK2Node_VariableGet> NodeCreator(*Graph);
			UK2Node_VariableGet* VarNode = NodeCreator.CreateNode();
			VarNode->VariableReference.SetSelfMember(VarName);
			VarNode->NodePosX = PosX;
			VarNode->NodePosY = PosY;
			NodeCreator.Finalize();
			VarNode->ReconstructNode();
			return VarNode;
		}

		FGraphNodeCreator<UK2Node_VariableSet> NodeCreator(*Graph);
		UK2Node_VariableSet* VarNode = NodeCreator.CreateNode();
		VarNode->VariableReference.SetSelfMember(VarName);
		VarNode->NodePosX = PosX;
		VarNode->NodePosY = PosY;
		NodeCreator.Finalize();
		VarNode->ReconstructNode();
		return VarNode;
	}

	// ===== InputAction =====
	if (T == TEXT("inputaction"))
	{
		FGraphNodeCreator<UK2Node_InputAction> NodeCreator(*Graph);
		UK2Node_InputAction* InputActionNode = NodeCreator.CreateNode();
		InputActionNode->InputActionName = FName(*Spec.Name);
		InputActionNode->NodePosX = PosX;
		InputActionNode->NodePosY = PosY;
		NodeCreator.Finalize();
		InputActionNode->ReconstructNode();
		return InputActionNode;
	}

	// ===== Branch =====
	if (T == TEXT("branch") || T == TEXT("if"))
	{
		FGraphNodeCreator<UK2Node_IfThenElse> NodeCreator(*Graph);
		UK2Node_IfThenElse* IfNode = NodeCreator.CreateNode();
		IfNode->NodePosX = PosX;
		IfNode->NodePosY = PosY;
		NodeCreator.Finalize();
		IfNode->ReconstructNode();
		return IfNode;
	}

	// ===== Sequence =====
	if (T == TEXT("sequence") || T == TEXT("executionsequence"))
	{
		FGraphNodeCreator<UK2Node_ExecutionSequence> NodeCreator(*Graph);
		UK2Node_ExecutionSequence* SequenceNode = NodeCreator.CreateNode();
		SequenceNode->NodePosX = PosX;
		SequenceNode->NodePosY = PosY;
		NodeCreator.Finalize();
		SequenceNode->ReconstructNode();
		return SequenceNode;
	}

	// ===== Cast =====
	if (T == TEXT("cast") || T == TEXT("castto"))
	{
		if (Spec.TargetClass.IsEmpty())
		{
			OutError = TEXT("Missing field: target_class for Cast node");
			return nullptr;
		}
		FString ClassError;
		UClass* TargetClass = UAL_CommandUtils::ResolveClassFromIdentifier(Spec.TargetClass, UObject::StaticClass(), ClassError);
		if (!TargetClass)
		{
			OutError = ClassError;
			return nullptr;
		}
		FGraphNodeCreator<UK2Node_DynamicCast> NodeCreator(*Graph);
		UK2Node_DynamicCast* CastNode = NodeCreator.CreateNode();
		CastNode->TargetType = TargetClass;
		CastNode->NodePosX = PosX;
		CastNode->NodePosY = PosY;
		NodeCreator.Finalize();
		CastNode->ReconstructNode();
		return CastNode;
	}

	// ===== SpawnActor =====
	if (T == TEXT("spawnactor") || T == TEXT("spawnactorfromclass"))
	{
		FGraphNodeCreator<UK2Node_SpawnActorFromClass> NodeCreator(*Graph);
		UK2Node_SpawnActorFromClass* SpawnNode = NodeCreator.CreateNode();
		SpawnNode->NodePosX = PosX;
		SpawnNode->NodePosY = PosY;

		/**
		 * 引脚必须**在 Finalize() 之前**自己分配一次，否则整个编辑器当场崩。
		 *
		 * `FGraphNodeCreator::Finalize()` 的顺序是（EdGraph.h:297）：
		 *
		 *     Node->CreateNewGuid();
		 *     Node->PostPlacedNewNode();          // ← 先
		 *     if (Node->Pins.Num() == 0)
		 *         Node->AllocateDefaultPins();    // ← 后
		 *
		 * 而 `UK2Node_SpawnActorFromClass::PostPlacedNewNode()` 上来就
		 * `GetScaleMethodPin()`，那是个 `FindPinChecked` —— 引脚还没分配，
		 * 返回 null，`check(Result)` 直接把编辑器打死
		 * （Assertion failed: Result，EdGraphNode.h:576）。
		 *
		 * 这不是偶发：只要建这一类节点就必崩。**5.2–5.8 七个版本都有**
		 * （5.0 / 5.1 的 SpawnActorFromClass 还没有 PostPlacedNewNode，不受影响）。
		 * 2026-09-07 步骤 0 摸底第一次跑真机就撞上了，崩溃报告在
		 * `<工程>/Saved/Crashes/UECC-Windows-06FD50E4...`。
		 *
		 * 先分配就没事：Finalize 里那个 `Pins.Num() == 0` 的判断会跳过重复分配，
		 * 而 PostPlacedNewNode 这时找得到 ScaleMethod 引脚了。
		 *
		 * 整个 BlueprintGraph 模块里只有两个节点类型有这个陷阱 ——
		 * 这个和 `UK2Node_AssignDelegate`（我们不建那个）。加新节点类型时，
		 * 先看一眼它的 `PostPlacedNewNode()` 碰不碰引脚。
		 */
		SpawnNode->AllocateDefaultPins();

		NodeCreator.Finalize();

		// Class 可以留空由引脚传入，所以给了才设
		if (!Spec.TargetClass.IsEmpty())
		{
			FString ClassError;
			if (UClass* TargetClass = UAL_CommandUtils::ResolveClassFromIdentifier(Spec.TargetClass, UObject::StaticClass(), ClassError))
			{
				if (UEdGraphPin* ClassPin = SpawnNode->GetClassPin())
				{
					ClassPin->DefaultObject = TargetClass;
					SpawnNode->ReconstructNode();
				}
			}
		}
		return SpawnNode;
	}

	// ===== 标准宏（ForLoop / WhileLoop / Gate / DoOnce / DoN / FlipFlop / IsValid / ForEach…）=====
	{
		// 类型名 → StandardMacros 里的图名。宏名本身可以直接当类型用
		// （`type: "ForLoop"`），也可以用 `type: "Macro", name: "ForLoop"`。
		static const TMap<FString, FString> MacroByType = {
			{ TEXT("forloop"),                 TEXT("ForLoop") },
			{ TEXT("whileloop"),               TEXT("WhileLoop") },
			{ TEXT("gate"),                    TEXT("Gate") },
			{ TEXT("doonce"),                  TEXT("DoOnce") },
			{ TEXT("don"),                     TEXT("DoN") },
			{ TEXT("flipflop"),                TEXT("FlipFlop") },
			{ TEXT("isvalid"),                 TEXT("IsValid") },
			{ TEXT("foreachloop"),             TEXT("ForEachLoop") },
			{ TEXT("foreachloopwithbreak"),    TEXT("ForEachLoopWithBreak") },
			{ TEXT("reverseforeachloop"),      TEXT("ReverseForEachLoop") }
		};

		const FString* MappedMacro = MacroByType.Find(T);
		if (MappedMacro || T == TEXT("macro"))
		{
			FString MacroName = MappedMacro ? *MappedMacro : Spec.Name;
			if (MacroName.IsEmpty() || MacroName.Equals(TEXT("Default"), ESearchCase::IgnoreCase))
			{
				OutError = TEXT("Missing macro name: pass type=\"ForLoop\" (etc.) or type=\"Macro\" with name=\"<MacroName>\"");
				return nullptr;
			}

			UBlueprint* MacroLib = nullptr;
			if (!Spec.MacroLib.IsEmpty())
			{
				FString UnusedPath;
				UAL_LoadBlueprintByPathOrName(Spec.MacroLib, MacroLib, UnusedPath);
			}
			if (!MacroLib)
			{
				MacroLib = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
			}
			if (!MacroLib)
			{
				OutError = TEXT("Could not find StandardMacros library");
				return nullptr;
			}

			UEdGraph* MacroGraph = UAL_FindGraph(MacroLib, MacroName);
			if (!MacroGraph)
			{
				OutError = FString::Printf(TEXT("Macro not found: %s in %s"), *MacroName, *MacroLib->GetName());
				return nullptr;
			}

			FGraphNodeCreator<UK2Node_MacroInstance> NodeCreator(*Graph);
			UK2Node_MacroInstance* MacroNode = NodeCreator.CreateNode();
			MacroNode->SetMacroGraph(MacroGraph);
			MacroNode->NodePosX = PosX;
			MacroNode->NodePosY = PosY;
			NodeCreator.Finalize();
			MacroNode->ReconstructNode();

			if (Spec.bHasFirstIndex)
			{
				if (UEdGraphPin* Pin = UAL_FindPinByName(MacroNode, TEXT("FirstIndex")))
				{
					Pin->DefaultValue = FString::FromInt(Spec.FirstIndex);
				}
			}
			if (Spec.bHasLastIndex)
			{
				if (UEdGraphPin* Pin = UAL_FindPinByName(MacroNode, TEXT("LastIndex")))
				{
					Pin->DefaultValue = FString::FromInt(Spec.LastIndex);
				}
			}
			return MacroNode;
		}
	}

	// ===== CustomEvent =====
	if (T == TEXT("customevent"))
	{
		// 参数先全部校验再建节点：建到一半失败的话，图里会留下一个带半截
		// 参数表的事件节点，而调用方收到的是「失败」
		TArray<FUAL_UserPinSpec> ParamSpecs;
		if (!UAL_ParseUserPinSpecs(Spec.EventParams, TEXT("params"), ParamSpecs, OutError))
		{
			return nullptr;
		}

		uint32 NetFlags = 0;
		if (!UAL_EventNetFlagsFromString(Spec.EventReplication, NetFlags))
		{
			OutError = FString::Printf(
				TEXT("Unknown replication '%s' on CustomEvent '%s'. Use None, Server (Run on Server), ")
				TEXT("Client (Run on owning Client) or Multicast"),
				*Spec.EventReplication, *Spec.Name);
			return nullptr;
		}
		// 编辑器里「可靠」勾选框只在选了复制模式后才出现，没复制的事件谈不上可靠
		if (Spec.bEventReliable && NetFlags == 0)
		{
			OutError = FString::Printf(
				TEXT("CustomEvent '%s': reliable=true needs replication=Server, Client or Multicast"), *Spec.Name);
			return nullptr;
		}

		FGraphNodeCreator<UK2Node_CustomEvent> NodeCreator(*Graph);
		UK2Node_CustomEvent* EventNode = NodeCreator.CreateNode();
		EventNode->CustomFunctionName = FName(*Spec.Name);
		EventNode->FunctionFlags &= ~(UAL_EventNetModeFlags | FUNC_NetReliable);
		EventNode->FunctionFlags |= NetFlags | (Spec.bEventReliable ? FUNC_NetReliable : 0);
		EventNode->NodePosX = PosX;
		EventNode->NodePosY = PosY;
		NodeCreator.Finalize();

		// 事件参数是节点的**输出**引脚：值从事件流出去给下游逻辑用。
		// 和函数入口节点同理（UK2Node_FunctionEntry 的用户引脚也是 EGPD_Output）。
		// 写成 EGPD_Input 的话引脚会长在红点的左边，连不出去
		UAL_CreateUserPins(EventNode, ParamSpecs, EGPD_Output);

		EventNode->ReconstructNode();
		return EventNode;
	}

	// ===== Select =====
	if (T == TEXT("select"))
	{
		FGraphNodeCreator<UK2Node_Select> NodeCreator(*Graph);
		UK2Node_Select* SelectNode = NodeCreator.CreateNode();
		SelectNode->NodePosX = PosX;
		SelectNode->NodePosY = PosY;
		NodeCreator.Finalize();
		SelectNode->ReconstructNode();
		return SelectNode;
	}

	// ===== MakeArray =====
	if (T == TEXT("makearray"))
	{
		FGraphNodeCreator<UK2Node_MakeArray> NodeCreator(*Graph);
		UK2Node_MakeArray* ArrayNode = NodeCreator.CreateNode();
		ArrayNode->NodePosX = PosX;
		ArrayNode->NodePosY = PosY;
		NodeCreator.Finalize();
		ArrayNode->ReconstructNode();
		return ArrayNode;
	}

	// ===== MakeStruct / BreakStruct =====
	if (T == TEXT("makestruct") || T == TEXT("breakstruct"))
	{
		if (Spec.StructType.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Missing field: struct_type for %s"), *Spec.Type);
			return nullptr;
		}
		UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *Spec.StructType);
		if (!Struct)
		{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
			Struct = FindFirstObject<UScriptStruct>(*Spec.StructType, EFindFirstObjectOptions::NativeFirst);
#else
			Struct = FindObject<UScriptStruct>(ANY_PACKAGE, *Spec.StructType);
#endif
		}
		if (!Struct)
		{
			OutError = FString::Printf(TEXT("Struct not found: %s"), *Spec.StructType);
			return nullptr;
		}

		if (T == TEXT("makestruct"))
		{
			FGraphNodeCreator<UK2Node_MakeStruct> NodeCreator(*Graph);
			UK2Node_MakeStruct* StructNode = NodeCreator.CreateNode();
			StructNode->StructType = Struct;
			StructNode->NodePosX = PosX;
			StructNode->NodePosY = PosY;
			NodeCreator.Finalize();
			StructNode->ReconstructNode();
			return StructNode;
		}

		FGraphNodeCreator<UK2Node_BreakStruct> NodeCreator(*Graph);
		UK2Node_BreakStruct* StructNode = NodeCreator.CreateNode();
		StructNode->StructType = Struct;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
		// UE5.6+ 使用 bMadeAfterOverridePinRemoval（已验证 5.6 源码）
		StructNode->bMadeAfterOverridePinRemoval = true;
#endif
		StructNode->NodePosX = PosX;
		StructNode->NodePosY = PosY;
		NodeCreator.Finalize();
		StructNode->ReconstructNode();
		return StructNode;
	}

	// ===== Self =====
	if (T == TEXT("self"))
	{
		FGraphNodeCreator<UK2Node_Self> NodeCreator(*Graph);
		UK2Node_Self* SelfNode = NodeCreator.CreateNode();
		SelfNode->NodePosX = PosX;
		SelfNode->NodePosY = PosY;
		NodeCreator.Finalize();
		SelfNode->ReconstructNode();
		return SelfNode;
	}

	// ===== EnhancedInputAction =====
	// 增强输入的动作事件节点。引脚：Triggered / Started / Ongoing / Canceled / Completed（出）、
	// ActionValue（类型跟着资产的 Value Type 走）、ElapsedSeconds、TriggeredSeconds、InputAction。
	//
	// 以前只有旧输入系统的 InputAction；模型走 raw_class 建 K2Node_EnhancedInputAction，
	// 节点是长出来了，但 InputAction 属性是空的 —— 编辑器里显示绑到 None，编译报错，
	// 新用户只能手动放一个。这里把资产引用一起带上，和编辑器右键菜单建出来的一样。
	// 类和属性全走反射，不链 InputBlueprintNodes 模块，工程没开 EnhancedInput 也不影响加载。
	if (T == TEXT("enhancedinputaction") || T == TEXT("enhancedinput") || T == TEXT("k2nodeenhancedinputaction"))
	{
		if (Spec.Name.IsEmpty())
		{
			OutError = TEXT("EnhancedInputAction needs member_name: the Input Action asset, e.g. IA_Jump or /Game/Input/Actions/IA_Jump");
			return nullptr;
		}
		UClass* NodeClass = UALReflect::FindScriptClass(TEXT("/Script/InputBlueprintNodes.K2Node_EnhancedInputAction"));
		if (!NodeClass)
		{
			OutError = TEXT("Enhanced Input plugin is not enabled in this project, its event node cannot be created. Enable EnhancedInput with ue_manage_plugin, or use the legacy InputAction node");
			return nullptr;
		}
		UObject* Action = UAL_FindInputActionAsset(Spec.Name, OutError);
		if (!Action)
		{
			return nullptr;
		}

		// 同一个动作在一张图里只该有一个事件节点，已有就复用，和 Event 一个做法
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->IsA(NodeClass) && UALReflect::GetObjectProp(Node, TEXT("InputAction")) == Action)
			{
				bOutReused = true;
				return Node;
			}
		}

		UK2Node* InputNode = NewObject<UK2Node>(Graph, NodeClass, NAME_None, RF_Transactional);
		if (!InputNode)
		{
			OutError = TEXT("Failed to instantiate K2Node_EnhancedInputAction");
			return nullptr;
		}
		Graph->AddNode(InputNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		InputNode->CreateNewGuid();
		// 资产要在 AllocateDefaultPins 之前挂上：ActionValue 引脚的类型是从资产上读的
		if (!UALReflect::SetObjectProp(InputNode, TEXT("InputAction"), Action))
		{
			FBlueprintEditorUtils::RemoveNode(Blueprint, InputNode, true);
			OutError = TEXT("Could not assign the Input Action to the node (InputAction property missing or type mismatch)");
			return nullptr;
		}
		InputNode->PostPlacedNewNode();
		InputNode->AllocateDefaultPins();
		InputNode->NodePosX = PosX;
		InputNode->NodePosY = PosY;
		return InputNode;
	}

	// ===== Timeline =====
	// 引脚：Play / PlayFromStart / Stop / Reverse / ReverseFromEnd / SetNewTime（入）、
	// NewTime（float 入）、Update / Finished（出）、Direction（出）、每条轨道一个同名 float 出。
	// "K2Node_Timeline" 是 get_graph 回出来的类名，读回来改一改写回去时会原样带上；
	// 不认它就掉进逃生口，建出一个没绑模板的空节点，编译还不报错
	if (T == TEXT("timeline") || T == TEXT("k2nodetimeline"))
	{
		if (Spec.Name.IsEmpty())
		{
			OutError = TEXT("Timeline needs member_name: the timeline's own name, e.g. DoorTimeline");
			return nullptr;
		}
		const FName TimelineName(*Spec.Name);

		// 轨道名和插值模式在这里验完 —— 解析那层没有报错通道，
		// 而「建到一半才发现第三个关键帧写了个不认识的 interp」是没法回头的
		for (const FUAL_TimelineTrackSpec& Track : Spec.TimelineTracks)
		{
			if (Track.Name.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Timeline '%s': every track needs a name (it becomes the node's output pin)"), *Spec.Name);
				return nullptr;
			}
			for (const FUAL_TimelineKeySpec& Key : Track.Keys)
			{
				ERichCurveInterpMode Interp = RCIM_Linear;
				ERichCurveTangentMode Tangent = RCTM_Auto;
				FString InterpError;
				if (!UAL_CurveInterpFromString(Key.Interp, Interp, Tangent, InterpError))
				{
					OutError = FString::Printf(
						TEXT("Timeline '%s' track '%s': %s"), *Spec.Name, *Track.Name, *InterpError);
					return nullptr;
				}
			}
		}

		ETimelineLengthMode LengthMode = TL_TimelineLength;
		if (!Spec.TimelineLengthMode.IsEmpty())
		{
			const FString Mode = Spec.TimelineLengthMode.ToLower();
			if (Mode == TEXT("last_keyframe") || Mode == TEXT("lastkeyframe"))
			{
				LengthMode = TL_LastKeyFrame;
			}
			else if (Mode != TEXT("length") && Mode != TEXT("timelinelength"))
			{
				OutError = FString::Printf(
					TEXT("Timeline '%s': unknown length_mode '%s' (length | last_keyframe)"),
					*Spec.Name, *Spec.TimelineLengthMode);
				return nullptr;
			}
		}

		const bool bHasTimelineSpec =
			Spec.TimelineTracks.Num() > 0 || Spec.bHasTimelineLength ||
			Spec.bHasTimelineLoop || Spec.bHasTimelineAutoPlay ||
			!Spec.TimelineLengthMode.IsEmpty();

		// 图里已有同名节点就复用它（读回来改一改写回去时会带着这个节点）
		UK2Node_Timeline* ExistingNode = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Timeline* Candidate = Cast<UK2Node_Timeline>(Node);
			if (Candidate && Candidate->TimelineName == TimelineName)
			{
				ExistingNode = Candidate;
				break;
			}
		}

		// 一个模板只能对应一个节点。别的图页上已有这个 Timeline 的节点时再建一个，
		// 删掉任一个都会连模板一起删（UK2Node_Timeline::DestroyNode → RemoveTimeline），
		// 这一批回滚时更是直接把用户原来的模板删掉 —— 所以直接拒绝，让调用方去那张图写
		if (!ExistingNode && Blueprint->FindTimelineTemplateByVariableName(TimelineName))
		{
			TArray<UEdGraph*> AllGraphs;
			Blueprint->GetAllGraphs(AllGraphs);
			for (UEdGraph* OtherGraph : AllGraphs)
			{
				if (!OtherGraph || OtherGraph == Graph) continue;
				for (UEdGraphNode* Node : OtherGraph->Nodes)
				{
					UK2Node_Timeline* Other = Cast<UK2Node_Timeline>(Node);
					if (Other && Other->TimelineName == TimelineName)
					{
						OutError = FString::Printf(
							TEXT("Timeline '%s' already has its node on graph '%s'. A timeline can only have one node: write to that graph (graph_name) or use a different timeline name"),
							*Spec.Name, *OtherGraph->GetName());
						return nullptr;
					}
				}
			}
		}

		bool bTemplateCreated = false;
		UTimelineTemplate* Template = UAL_EnsureTimelineTemplate(Blueprint, TimelineName, bTemplateCreated, OutError);
		if (!Template)
		{
			return nullptr;
		}

		/**
		 * 改**已有**的 Timeline 是允许的，代价是先拍一张快照。
		 *
		 * 这里以前整个禁掉，理由是撤不回来：整批回滚只删这一批新建的节点，
		 * 而模板是蓝图上的独立对象；`Transaction.Cancel()` 又只是把记录从
		 * undo 栈丢掉、不回放（`UTransBuffer::Cancel`）。于是批次失败时响应说
		 * 「蓝图没有任何改动」，而用户的 Timeline 已经被改了。
		 *
		 * 禁掉的代价是「读回来改一改写回去」在 Timeline 上只读不写：模型能看见
		 * 曲线（get_graph 现在会回），却只能干瞪眼。所以改成自己负责回退 ——
		 * 动模板之前存下它原来的样子，整批失败时照它回填（UAL_RestoreTimeline）。
		 */
		FUAL_TimelineSnapshot LocalSnapshot;
		FUAL_TimelineSnapshot& Snapshot = OutTimelineSnapshot ? *OutTimelineSnapshot : LocalSnapshot;
		UAL_SnapshotTimeline(Snapshot, Template);
		Snapshot.bTemplateCreated = bTemplateCreated;

		// 模板本来就在、这一批却要改它，而调用方又不走批量那条路（没给快照位置）——
		// 那就没有任何东西负责回退，宁可不做
		if (!bTemplateCreated && bHasTimelineSpec && !OutTimelineSnapshot)
		{
			OutError = FString::Printf(
				TEXT("Timeline '%s' already exists and this command cannot undo changes to an existing timeline. Use blueprint.create_graph (blueprint_apply_graph), which rolls the timeline back when the batch fails"),
				*Spec.Name);
			return nullptr;
		}

		Template->Modify();
		if (Spec.bHasTimelineLoop)
		{
			Template->bLoop = Spec.bTimelineLoop;
		}
		if (Spec.bHasTimelineAutoPlay)
		{
			Template->bAutoPlay = Spec.bTimelineAutoPlay;
		}

		float LastKeyTime = 0.f;
		for (const FUAL_TimelineTrackSpec& Track : Spec.TimelineTracks)
		{
			UAL_AddTimelineFloatTrack(Blueprint, Template, Track, Snapshot);
			for (const FUAL_TimelineKeySpec& Key : Track.Keys)
			{
				LastKeyTime = FMath::Max(LastKeyTime, Key.Time);
			}
		}

		// 时长：给了就照给的；没给就跟着最后一帧走 —— 引擎默认 5 秒，
		// 关键帧只到 1 秒的话后面 4 秒都在停着等 Finished
		if (Spec.bHasTimelineLength)
		{
			Template->TimelineLength = Spec.TimelineLength;
		}
		else if (LastKeyTime > 0.f)
		{
			Template->TimelineLength = LastKeyTime;
		}
		// length_mode 只在显式给了时才动 —— 用户在编辑器里选过「用最后一帧」的话，
		// 一次不相干的回写不该把它改掉
		if (!Spec.TimelineLengthMode.IsEmpty())
		{
			Template->LengthMode = LengthMode;
		}

		if (ExistingNode)
		{
			// 轨道变了，节点上的引脚要跟着重长。不重建的话新轨道的输出引脚
			// 在这一批里根本不存在，连线校验会报「pin not found」
			if (bHasTimelineSpec)
			{
				ExistingNode->Modify();
				ExistingNode->ReconstructNode();
			}
			Snapshot.Node = ExistingNode;
			bOutReused = true;
			return ExistingNode;
		}

		UK2Node_Timeline* PlacedNode = UAL_PlaceTimelineNode(Graph, Template, TimelineName, PosX, PosY);
		Snapshot.Node = PlacedNode;
		Snapshot.bNodeCreated = true;
		return PlacedNode;
	}

	// ===== 逃生口：按类名直接实例化 =====
	// 放在最后，具名分派优先 —— 具名那些能带上函数引用/宏图/目标类，通用路径不能。
	{
		const FString RawClassName = !Spec.RawClass.IsEmpty() ? Spec.RawClass : Spec.Type;
		if (RawClassName.StartsWith(TEXT("K2Node_")) || !Spec.RawClass.IsEmpty())
		{
			FString ClassError;
			UClass* NodeClass = UAL_CommandUtils::ResolveClassFromIdentifier(RawClassName, UK2Node::StaticClass(), ClassError);
			if (!NodeClass)
			{
				// 常见写法是漏了 U 前缀
				NodeClass = UAL_CommandUtils::ResolveClassFromIdentifier(TEXT("U") + RawClassName, UK2Node::StaticClass(), ClassError);
			}
			if (!NodeClass)
			{
				OutError = FString::Printf(TEXT("raw_class not found or not a UK2Node subclass: %s"), *RawClassName);
				return nullptr;
			}
			if (NodeClass->HasAnyClassFlags(CLASS_Abstract))
			{
				OutError = FString::Printf(TEXT("raw_class is abstract, cannot instantiate: %s"), *RawClassName);
				return nullptr;
			}

			UK2Node* RawNode = NewObject<UK2Node>(Graph, NodeClass, NAME_None, RF_Transactional);
			if (!RawNode)
			{
				OutError = FString::Printf(TEXT("Failed to instantiate raw_class: %s"), *RawClassName);
				return nullptr;
			}
			Graph->AddNode(RawNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			RawNode->CreateNewGuid();
			RawNode->PostPlacedNewNode();
			RawNode->AllocateDefaultPins();
			RawNode->NodePosX = PosX;
			RawNode->NodePosY = PosY;

			// 没长出任何引脚的节点几乎一定是需要额外配置的那一类（CallFunction、
			// MacroInstance…）。留在图里只会让调用方以为建成了，然后在连线时
			// 撞上「pin not found」，离真正的原因隔了一步。就地删掉、直说。
			if (RawNode->Pins.Num() == 0)
			{
				FBlueprintEditorUtils::RemoveNode(Blueprint, RawNode, true);
				OutError = FString::Printf(
					TEXT("raw_class '%s' created no pins — it needs configuration this generic path cannot supply. Use a named node type instead."),
					*RawClassName);
				return nullptr;
			}
			return RawNode;
		}
	}

	OutError = FString::Printf(
		TEXT("Unsupported node type: %s. Named types: Event, Function, VariableGet, VariableSet, InputAction, EnhancedInputAction, Branch, Sequence, Cast, SpawnActor, CustomEvent, Select, MakeArray, MakeStruct, BreakStruct, Self, Timeline, CallDispatcher, BindEvent, UnbindEvent, UnbindAllEvents, ForLoop, WhileLoop, Gate, DoOnce, DoN, FlipFlop, IsValid, ForEachLoop, ForEachLoopWithBreak, ReverseForEachLoop, Macro. Or pass raw_class=\"K2Node_<Something>\"."),
		*Spec.Type);
	return nullptr;
}

// ========== 从 UAL_CommandHandler.cpp 迁移以下函数 ==========
// 原始行号参考:
//   Handle_CreateBlueprint: 2279-2512

void FUAL_BlueprintCommands::Handle_CreateBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintName;
	if (!Payload->TryGetStringField(TEXT("name"), BlueprintName) || BlueprintName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: name"));
		return;
	}

	FString ParentClassStr;
	if (!Payload->TryGetStringField(TEXT("parent_class"), ParentClassStr))
	{
		Payload->TryGetStringField(TEXT("parentClass"), ParentClassStr);
	}
	if (ParentClassStr.IsEmpty())
	{
		ParentClassStr = TEXT("/Script/Engine.Actor");
	}

	FString PackagePath;
	if (!Payload->TryGetStringField(TEXT("path"), PackagePath))
	{
		FString Folder;
		if (!Payload->TryGetStringField(TEXT("folder"), Folder))
		{
			Folder = TEXT("/Game/UnrealAgent/Blueprints");
		}
		if (!Folder.StartsWith(TEXT("/")))
		{
			Folder = FString::Printf(TEXT("/Game/%s"), *Folder);
		}
		if (!Folder.EndsWith(TEXT("/")))
		{
			Folder += TEXT("/");
		}
		PackagePath = Folder + BlueprintName;
	}

	// 1. Resolve Parent Class
	FString ClassError;
	UClass* ParentClass = UAL_CommandUtils::ResolveClassFromIdentifier(ParentClassStr, AActor::StaticClass(), ClassError);
	if (!ParentClass)
	{
		UAL_CommandUtils::SendError(RequestId, 404, ClassError);
		return;
	}

	// 2. Check Package
	FString PackageName;
	if (!FPackageName::TryConvertFilenameToLongPackageName(PackagePath, PackageName))
	{
		PackageName = PackagePath;
	}
	
	// 2.1 检查蓝图是否已存在（避免覆盖导致崩溃）
	FString ExistingAssetPath = PackageName + TEXT(".") + BlueprintName;
	if (UBlueprint* ExistingBlueprint = LoadObject<UBlueprint>(nullptr, *ExistingAssetPath))
	{
		// 蓝图已存在，返回冲突错误
		TSharedPtr<FJsonObject> ConflictResult = MakeShared<FJsonObject>();
		ConflictResult->SetBoolField(TEXT("ok"), false);
		ConflictResult->SetStringField(TEXT("name"), BlueprintName);
		ConflictResult->SetStringField(TEXT("path"), PackageName);
		ConflictResult->SetStringField(TEXT("existing_class"), ExistingBlueprint->GeneratedClass ? ExistingBlueprint->GeneratedClass->GetPathName() : TEXT(""));
		ConflictResult->SetStringField(TEXT("message"), FString::Printf(TEXT("Blueprint '%s' already exists at path '%s'. Use blueprint.add_component to modify it, or delete it first."), *BlueprintName, *PackageName));
		
		UAL_CommandUtils::SendError(RequestId, 409, FString::Printf(TEXT(" '%s' 下已经存在同名蓝图 '%s'，请更换新的名字或路径。"), *PackageName,*BlueprintName ));
		return;
	}
	
	// 也检查 FindPackage 以防万一
	if (UPackage* ExistingPackage = FindPackage(nullptr, *PackageName))
	{
		if (UBlueprint* ExistingBP = FindObject<UBlueprint>(ExistingPackage, *BlueprintName))
		{
			UAL_CommandUtils::SendError(RequestId, 409, FString::Printf(TEXT("Blueprint '%s' already exists in package '%s'"), *BlueprintName, *PackageName));
			return;
		}
	}
	
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create package"));
		return;
	}

	// 3. Create Blueprint
	EBlueprintType BlueprintType = BPTYPE_Normal;
	if (ParentClass->IsChildOf(UInterface::StaticClass()))
	{
		BlueprintType = BPTYPE_Interface;
	}
	else if (ParentClass->IsChildOf(ALevelScriptActor::StaticClass()))
	{
		BlueprintType = BPTYPE_LevelScript;
	}
	else if (ParentClass->IsChildOf(UFunction::StaticClass())) // MacroLibrary etc handled differently, keep simple
	{
		BlueprintType = BPTYPE_FunctionLibrary;
	}

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*BlueprintName),
		BlueprintType,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);

	if (!Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create Blueprint asset"));
		return;
	}

	// 4. Add Components
	/** 写不进去的组件属性。不往响应里带的话，调用方以为组件配好了（见下面的注释） */
	TArray<FString> ComponentPropertyErrors;
	/** 实际挂接和请求不一致的地方。同理，不说的话调用方以为层级是它要的那个 */
	TArray<FString> ComponentAttachNotes;
	const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
	if (Payload->TryGetArrayField(TEXT("components"), Components) && Components)
	{
		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (SCS)
		{
			for (const TSharedPtr<FJsonValue>& CompVal : *Components)
			{
				const TSharedPtr<FJsonObject> CompObj = CompVal->AsObject();
				if (!CompObj.IsValid()) continue;

				FString CompType, CompName, AttachTo;
				CompObj->TryGetStringField(TEXT("component_type"), CompType);
				CompObj->TryGetStringField(TEXT("component_name"), CompName);
				CompObj->TryGetStringField(TEXT("attach_to"), AttachTo);

				if (CompType.IsEmpty()) continue;

				FString CompError;
				UClass* CompClass = UAL_CommandUtils::ResolveClassFromIdentifier(CompType, USceneComponent::StaticClass(), CompError);
				if (!CompClass)
				{
					UE_LOG(LogUALBlueprint, Warning, TEXT("Component class not found: %s"), *CompType);
					continue;
				}

				USCS_Node* NewNode = SCS->CreateNode(CompClass, FName(*CompName));
				if (NewNode)
				{
					// Attach
					if (AttachTo.IsEmpty() || AttachTo.Equals(TEXT("root"), ESearchCase::IgnoreCase) || AttachTo.Equals(TEXT("DefaultSceneRoot"), ESearchCase::IgnoreCase))
					{
						/**
						 * 「挂到根上」要真的挂上去。
						 *
						 * `SCS->AddNode()` 加的是**根级节点**：第一个成为 actor 的根，
						 * 第二个之后是平级的另一棵树 —— 谁都不挂，不跟 actor 走。
						 * 这个蓝图没有 DefaultSceneRoot 时（create 这条路就没有），
						 * 两个组件都走 AddNode 的结果是：root = 第一个组件，
						 * 第二个组件留在世界原点不动，`set_actor_location` 也拽不动它。
						 *
						 * 2026-09-21 的用户反馈：BP_Soldier_Red 的 Body 就是这么掉队的，
						 * 而 create 回的是「成功」，连一条 warning 都没有。
						 *
						 * 现在第一个根级组件当根，后面的挂到它下面 —— 和用户
						 * 改用 `blueprint_add_component` 时得到的层级一致。
						 */
						USCS_Node* ExistingRoot = SCS->GetAllNodes().Num() > 0 ? SCS->GetAllNodes()[0] : nullptr;
						if (ExistingRoot && ExistingRoot != NewNode)
						{
							ExistingRoot->AddChildNode(NewNode);
							ComponentAttachNotes.Add(FString::Printf(
								TEXT("%s: attached to root component '%s'"),
								*CompName, *ExistingRoot->GetVariableName().ToString()));
						}
						else
						{
							SCS->AddNode(NewNode); // 第一个组件：它就是根
						}
					}
					else
					{
						// Find parent
						USCS_Node* ParentNode = nullptr;
						for (USCS_Node* Node : SCS->GetAllNodes())
						{
							if (Node && Node->GetVariableName().ToString().Equals(AttachTo, ESearchCase::IgnoreCase))
							{
								ParentNode = Node;
								break;
							}
						}
						if (ParentNode)
						{
							ParentNode->AddChildNode(NewNode);
						}
						else
						{
							/**
							 * 要挂的父组件不存在。组件还是建出来（有用的兜底），
							 * 但**必须说** —— 层级决定行为，静默挂错地方比失败难查。
							 * 这段和 `Handle_AddComponentToBlueprint` 的处理保持一致。
							 */
							USCS_Node* FallbackRoot = SCS->GetAllNodes().Num() > 0 ? SCS->GetAllNodes()[0] : nullptr;
							if (FallbackRoot && FallbackRoot != NewNode)
							{
								FallbackRoot->AddChildNode(NewNode);
							}
							else
							{
								SCS->AddNode(NewNode);
							}
							ComponentAttachNotes.Add(FString::Printf(
								TEXT("%s: requested parent '%s' does not exist in this Blueprint; attached to the root instead"),
								*CompName, *AttachTo));
						}
					}

					// Set Transform
					if (USceneComponent* Template = Cast<USceneComponent>(NewNode->ComponentTemplate))
					{
						const FVector Loc = UAL_CommandUtils::ReadVectorDirect(CompObj->GetObjectField(TEXT("location")));
						const FRotator Rot = UAL_CommandUtils::ReadRotatorDirect(CompObj->GetObjectField(TEXT("rotation")));
						const FVector Scale = UAL_CommandUtils::ReadVectorDirect(CompObj->GetObjectField(TEXT("scale")), FVector(1, 1, 1));
						
						Template->SetRelativeLocation(Loc);
						Template->SetRelativeRotation(Rot);
						Template->SetRelativeScale3D(Scale);

						/**
						 * 组件属性。
						 *
						 * 这里以前是**第三份**手抄的类型表，只认 Str / Double / Float / Bool
						 * 四种，末尾一句 `// Add more as needed`：网格、材质数组、整数、
						 * 结构体全部被静默跳过 —— 不报错、不进 warnings、连 UE_LOG 都没有。
						 * 2026-09-16 的用户反馈里六个网格组件建完全是空的，门在关卡里是隐形的，
						 * 而 create 回的是「成功、无警告」。
						 *
						 * 现在走全仓库唯一那个 `SetSimpleProperty`（和 set_property 同一份），
						 * 写不进去的收集起来一起回。
						 */
						const TSharedPtr<FJsonObject>* Props = nullptr;
						if (CompObj->TryGetObjectField(TEXT("properties"), Props) && Props && Props->IsValid())
						{
							for (auto& Pair : (*Props)->Values)
							{
								const FString Key = UAL_JsonKey(Pair.Key);
								// 同 set_property：细节面板里显示的名字（布尔去掉 b 前缀）也要认
								FString PropError;

								// 反射优先。查不到才轮到「细节面板里在组件上、反射里却不是
								// 组件属性」的那几个（目前只有 CollisionProfileName）——
								// 反过来的话，自定义组件上真有一个同名属性就再也设不进去了
								if (FProperty* Prop = UAL_FindPropertyByAnyName(CompClass, Key))
								{
									if (!UAL_CommandUtils::SetSimpleProperty(Prop, Template, Pair.Value, PropError)
										&& PropError.IsEmpty())
									{
										PropError = TEXT("failed to set");
									}
								}
								else
								{
									bool bHandled = false;
									const bool bDerivedOk = UAL_CommandUtils::TrySetDerivedProperty(
										Template, Key, Pair.Value, PropError, bHandled);

									if (!bHandled)
									{
										PropError = TEXT("no such property on this component class");
									}
									else if (bDerivedOk)
									{
										PropError.Reset();
									}
									else if (PropError.IsEmpty())
									{
										PropError = TEXT("failed to set");
									}
								}
								if (!PropError.IsEmpty())
								{
									ComponentPropertyErrors.Add(FString::Printf(
										TEXT("%s.%s: %s"), *CompName, *Pair.Key, *PropError));
								}
							}
						}
					}
				}
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	
	// Save —— 返回值必须看。SavePackage 不抛异常，失败是通过返回值报的
	// （只读文件、被源码管理签出、磁盘满），丢掉它就等于声称「建好并存下了」
	// 而实际上盘上什么都没有。
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	const bool bSavedOk_CreateSaveResult = UPackage::Save(Package, Blueprint, *PackageFileName, SaveArgs).Result == ESavePackageResult::Success;
#else
	const bool bSavedOk_CreateSaveResult = UPackage::SavePackage(Package, Blueprint, *PackageFileName, SaveArgs);
#endif
	const bool bCreateSaved = bSavedOk_CreateSaveResult;
	if (!bCreateSaved)
	{
		UE_LOG(LogUALBlueprint, Warning, TEXT("Blueprint '%s' was created in memory but could not be saved to disk"), *PackageName);
	}

	// Asset Registry
	FAssetRegistryModule::AssetCreated(Blueprint);

	// 使用统一的结构构建函数，返回完整蓝图信息
	TSharedPtr<FJsonObject> Result = BuildBlueprintStructureJson(Blueprint, true, false);
	// 这里原来是硬编码的 true —— 蓝图存不下去时调用方照样被告知「已保存」
	Result->SetBoolField(TEXT("saved"), bCreateSaved);
	if (!bCreateSaved)
	{
		Result->SetStringField(
			TEXT("save_warning"),
			TEXT("The Blueprint exists in memory but could not be written to disk (read-only file, source control checkout, or disk full). It will be lost when the editor closes."));
	}
	// 写不进去的组件属性必须出现在这里。以前这个数组是个空占位，
	// 而属性是被静默跳过的 —— 组件建出来了但什么都没配上，回的却是「成功、无警告」
	TArray<TSharedPtr<FJsonValue>> CreateWarnings;
	for (const FString& PropIssue : ComponentPropertyErrors)
	{
		CreateWarnings.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("component property not applied - %s"), *PropIssue)));
	}
	for (const FString& AttachIssue : ComponentAttachNotes)
	{
		CreateWarnings.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("component attachment - %s"), *AttachIssue)));
	}
	Result->SetArrayField(TEXT("warnings"), CreateWarnings);

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/**
 * 为已存在的蓝图添加组件
 * 
 * 请求参数:
 *   - blueprint_name: 蓝图名称或路径（必填）
 *   - component_type: 组件类型（必填），如 StaticMeshComponent, PointLightComponent
 *   - component_name: 组件名称（必填）
 *   - location: 组件相对位置 [x, y, z]
 *   - rotation: 组件相对旋转 [pitch, yaw, roll]
 *   - scale: 组件相对缩放 [x, y, z]
 *   - attach_to: 附加到的父组件名称
 *   - component_properties: 组件属性键值对
 * 
 * 返回:
 *   - ok: 是否成功
 *   - blueprint_name: 蓝图名称
 *   - component_name: 添加的组件名称
 *   - component_class: 组件类型
 *   - attached: 是否已附加
 *   - saved: 是否已保存
 */
void FUAL_BlueprintCommands::Handle_AddComponentToBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数
	FString BlueprintName;
	if (!Payload->TryGetStringField(TEXT("blueprint_name"), BlueprintName) || BlueprintName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_name"));
		return;
	}

	FString ComponentType;
	if (!Payload->TryGetStringField(TEXT("component_type"), ComponentType) || ComponentType.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: component_type"));
		return;
	}

	FString ComponentName;
	if (!Payload->TryGetStringField(TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: component_name"));
		return;
	}

	// 2. 查找蓝图资产
	UBlueprint* Blueprint = nullptr;
	FString BlueprintPath;
	
	// 尝试直接加载（如果是完整路径）
	if (BlueprintName.StartsWith(TEXT("/")))
	{
		BlueprintPath = BlueprintName;
		if (!BlueprintPath.EndsWith(TEXT(".") + FPaths::GetBaseFilename(BlueprintPath)))
		{
			// 添加资产名称后缀
			BlueprintPath = BlueprintPath + TEXT(".") + FPaths::GetBaseFilename(BlueprintPath);
		}
		Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	}
	
	// 如果直接加载失败，通过 AssetRegistry 查找
	if (!Blueprint)
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		
		TArray<FAssetData> AssetList;
		FARFilter Filter;
		// UE 5.1+ 使用 ClassPaths，旧版本使用 ClassNames
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
#else
		Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
#endif
		Filter.bRecursiveClasses = true;
		AssetRegistry.GetAssets(Filter, AssetList);
		
		for (const FAssetData& Asset : AssetList)
		{
			FString AssetName = Asset.AssetName.ToString();
			// UE 5.1+ 使用 GetObjectPathString()，旧版本使用 ObjectPath.ToString()
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			FString AssetPath = Asset.GetObjectPathString();
#else
			FString AssetPath = Asset.ObjectPath.ToString();
#endif
			if (AssetName.Equals(BlueprintName, ESearchCase::IgnoreCase) ||
				AssetPath.Contains(BlueprintName))
			{
				Blueprint = Cast<UBlueprint>(Asset.GetAsset());
				BlueprintPath = AssetPath;
				break;
			}
		}
	}

	if (!Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
		return;
	}

	// 3. 解析组件类
	FString ClassError;
	UClass* ComponentClass = UAL_CommandUtils::ResolveClassFromIdentifier(ComponentType, UActorComponent::StaticClass(), ClassError);
	if (!ComponentClass)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Component class not found: %s. %s"), *ComponentType, *ClassError));
		return;
	}

	// 4. 获取 SimpleConstructionScript
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Blueprint does not have SimpleConstructionScript (not an Actor Blueprint?)"));
		return;
	}

	// 5. 检查组件名称是否已存在
	for (USCS_Node* ExistingNode : SCS->GetAllNodes())
	{
		if (ExistingNode && ExistingNode->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
		{
			UAL_CommandUtils::SendError(RequestId, 409, FString::Printf(TEXT("Component with name '%s' already exists in blueprint"), *ComponentName));
			return;
		}
	}

	// 6. 创建组件节点
	USCS_Node* NewNode = SCS->CreateNode(ComponentClass, FName(*ComponentName));
	if (!NewNode)
	{
		UAL_CommandUtils::SendError(RequestId, 500, FString::Printf(TEXT("Failed to create component node: %s"), *ComponentName));
		return;
	}

	// 7. 处理附加关系
	FString AttachTo;
	Payload->TryGetStringField(TEXT("attach_to"), AttachTo);
	
	/**
	 * 挂载。
	 *
	 * ## 这里以前会撒谎
	 *
	 * 上一版三条分支**一律 `bAttached = true`** —— 包括「你要挂的父组件根本
	 * 不存在，我改挂到根上了」那条。它只往 UE_LOG 里写一句警告，
	 * 而调用方看到的是 `attached: true`，以为自己要的层级建成了。
	 *
	 * 组件层级决定行为（门绕门轴转还是绕中心转），静默挂错地方比直接失败
	 * 难查得多 —— 调用方拿到 true 就不会再核对了。
	 *
	 * 现在如实回「实际挂在了谁下面」，和请求不一致时单独给一条 warning。
	 */
	bool bAttached = false;
	FString ActualParent;        // 实际挂在谁下面（空 = 挂在根上）
	FString AttachWarning;       // 和请求不一致时说明原因

	const bool bWantsRoot =
		AttachTo.IsEmpty()
		|| AttachTo.Equals(TEXT("root"), ESearchCase::IgnoreCase)
		|| AttachTo.Equals(TEXT("DefaultSceneRoot"), ESearchCase::IgnoreCase);

	if (bWantsRoot)
	{
		SCS->AddNode(NewNode);
		bAttached = true;
	}
	else
	{
		USCS_Node* ParentNode = nullptr;
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (Node && Node->GetVariableName().ToString().Equals(AttachTo, ESearchCase::IgnoreCase))
			{
				ParentNode = Node;
				break;
			}
		}

		if (ParentNode)
		{
			ParentNode->AddChildNode(NewNode);
			ActualParent = ParentNode->GetVariableName().ToString();
			bAttached = true;
		}
		else
		{
			// 挂到根上仍然是有用的兜底（组件建出来了），但**必须说**
			SCS->AddNode(NewNode);
			bAttached = false;

			TArray<FString> Available;
			for (USCS_Node* Node : SCS->GetAllNodes())
			{
				if (Node && Node != NewNode)
				{
					Available.Add(Node->GetVariableName().ToString());
				}
			}
			AttachWarning = FString::Printf(
				TEXT("Requested parent '%s' does not exist in this Blueprint; the component was attached to the root instead. Existing components: %s"),
				*AttachTo,
				Available.Num() > 0 ? *FString::Join(Available, TEXT(", ")) : TEXT("(none)"));
			UE_LOG(LogUALBlueprint, Warning, TEXT("%s"), *AttachWarning);
		}
	}

	// 8. 设置组件变换（仅对 SceneComponent）
	if (USceneComponent* SceneTemplate = Cast<USceneComponent>(NewNode->ComponentTemplate))
	{
		// 读取位置
		TSharedPtr<FJsonObject> LocationObj;
		if (UAL_CommandUtils::TryGetObjectFieldFlexible(Payload, TEXT("location"), LocationObj))
		{
			FVector Location = UAL_CommandUtils::ReadVectorDirect(LocationObj);
			SceneTemplate->SetRelativeLocation(Location);
		}
		
		// 读取旋转
		TSharedPtr<FJsonObject> RotationObj;
		if (UAL_CommandUtils::TryGetObjectFieldFlexible(Payload, TEXT("rotation"), RotationObj))
		{
			FRotator Rotation = UAL_CommandUtils::ReadRotatorDirect(RotationObj);
			SceneTemplate->SetRelativeRotation(Rotation);
		}
		
		// 读取缩放
		TSharedPtr<FJsonObject> ScaleObj;
		if (UAL_CommandUtils::TryGetObjectFieldFlexible(Payload, TEXT("scale"), ScaleObj))
		{
			FVector Scale = UAL_CommandUtils::ReadVectorDirect(ScaleObj, FVector(1, 1, 1));
			SceneTemplate->SetRelativeScale3D(Scale);
		}
	}

	// 9. 设置组件属性
	// 写不进去的必须回给调用方。以前只 UE_LOG 一句 —— 那行日志在编辑器的
	// 输出窗口里，模型和用户谁都看不见，于是组件建出来了、属性一个没配上，
	// 返回还是「Successfully added component」
	TArray<FString> PropertyErrors;
	const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
	if (Payload->TryGetObjectField(TEXT("component_properties"), PropsPtr) && PropsPtr && PropsPtr->IsValid())
	{
		UObject* Template = NewNode->ComponentTemplate;
		if (Template)
		{
			for (auto& Pair : (*PropsPtr)->Values)
			{
				const FString Key = UAL_JsonKey(Pair.Key);
				FString PropError;

				// 同 blueprint.create：反射优先，查不到才轮到 CollisionProfileName 这类
				if (FProperty* Prop = UAL_FindPropertyByAnyName(ComponentClass, Key))
				{
					if (!UAL_CommandUtils::SetSimpleProperty(Prop, Template, Pair.Value, PropError)
						&& PropError.IsEmpty())
					{
						PropError = TEXT("failed to set");
					}
				}
				else
				{
					bool bHandled = false;
					const bool bDerivedOk = UAL_CommandUtils::TrySetDerivedProperty(
						Template, Key, Pair.Value, PropError, bHandled);

					if (!bHandled)
					{
						PropError = TEXT("no such property on this component class");
					}
					else if (bDerivedOk)
					{
						PropError.Reset();
					}
					else if (PropError.IsEmpty())
					{
						PropError = TEXT("failed to set");
					}
				}
				if (!PropError.IsEmpty())
				{
					PropertyErrors.Add(FString::Printf(TEXT("%s: %s"), *Pair.Key, *PropError));
				}
			}
		}
	}

	// 10. 标记蓝图已修改并保存
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	
	UPackage* Package = Blueprint->GetOutermost();
	bool bSaved = false;
	if (Package)
	{
		const FString PackageName = Package->GetName();
		const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		// 使用兼容写法，SavePackage 在不同版本返回值类型不同
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		const bool bSavedOk_SaveResult = UPackage::Save(Package, Blueprint, *PackageFileName, SaveArgs).Result == ESavePackageResult::Success;
#else
		const bool bSavedOk_SaveResult = UPackage::SavePackage(Package, Blueprint, *PackageFileName, SaveArgs);
#endif
		/**
		 * 这里原来是 `bSaved = true; // 如果没有异常则认为保存成功`。
		 *
		 * SavePackage **不抛异常** —— 失败是通过返回值报的。文件只读、
		 * 被源码管理签出、磁盘满，全都会静默变成「saved: true」，
		 * 而用户重启编辑器之后才发现改动没了。
		 */
		bSaved = bSavedOk_SaveResult;
		if (!bSaved)
		{
			UE_LOG(LogUALBlueprint, Warning,
				TEXT("Failed to save package '%s' (the component was still added in memory)"), *PackageName);
		}
	}

	// 11. 构建响应
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Result->SetStringField(TEXT("blueprint_path"), BlueprintPath);
	Result->SetStringField(TEXT("component_name"), NewNode->GetVariableName().ToString());
	Result->SetStringField(TEXT("component_class"), ComponentClass->GetName());
	// attached 现在表示「**按你要求的**挂上了」，而不是「反正挂上了某处」
	Result->SetBoolField(TEXT("attached"), bAttached);
	// 实际挂在谁下面。空字符串 = 挂在根上，这和「不知道」是两回事
	Result->SetStringField(TEXT("attached_to"), ActualParent);
	if (!AttachWarning.IsEmpty())
	{
		Result->SetStringField(TEXT("attach_warning"), AttachWarning);
	}
	Result->SetBoolField(TEXT("saved"), bSaved);
	if (PropertyErrors.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> FailedProps;
		for (const FString& Issue : PropertyErrors)
		{
			FailedProps.Add(MakeShared<FJsonValueString>(Issue));
		}
		Result->SetArrayField(TEXT("failed_properties"), FailedProps);
	}
	Result->SetStringField(TEXT("message"),
		PropertyErrors.Num() > 0
			? FString::Printf(TEXT("Added component '%s' (%s) to blueprint '%s', but %d of its properties were NOT applied - see failed_properties"),
				*ComponentName, *ComponentClass->GetName(), *Blueprint->GetName(), PropertyErrors.Num())
			: AttachWarning.IsEmpty()
				? FString::Printf(TEXT("Successfully added component '%s' (%s) to blueprint '%s'"),
					*ComponentName, *ComponentClass->GetName(), *Blueprint->GetName())
				: FString::Printf(TEXT("Added component '%s' (%s) to blueprint '%s', but NOT where you asked: %s"),
					*ComponentName, *ComponentClass->GetName(), *Blueprint->GetName(), *AttachWarning));

	// 返回更新后的完整组件列表，让 AI 知道当前蓝图有哪些组件
	Result->SetArrayField(TEXT("all_components"), CollectComponentsInfo(Blueprint));

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/**
 * 写完之后从目标对象上把值读回来。回执里报的是引擎里此刻的值，不是请求里的原样 ——
 * 写进去被夹紧、被 setter 改写、编译后没留住，照原样回显都看不出来。
 *
 * 属性每次都按名字重新查：编译会重建蓝图生成类，蓝图里声明的变量对应的
 * FProperty 会换一份，写之前拿到的指针编译后不能再用。
 *
 * 返回 false 表示读不到（编译后属性没了 / 目标没了）。
 */
static bool UAL_ReadBackPropertyText(UObject* Target, UClass* TargetClass, const FString& PropName, FString& OutText, FString& OutType)
{
	if (!Target)
	{
		return false;
	}

	FProperty* Prop = UAL_FindPropertyByAnyName(TargetClass, PropName);
	if (!Prop)
	{
		Prop = UAL_FindPropertyByAnyName(Target->GetClass(), PropName);
	}
	if (Prop)
	{
		OutText.Reset();
		Prop->ExportText_InContainer(0, OutText, Target, nullptr, Target, PPF_None);
		OutType = Prop->GetClass()->GetName();
		return true;
	}

	// 反射里没有、只能走 setter 写的那几个（见 UAL_CommandUtils::TrySetDerivedProperty）
	if (PropName.Equals(TEXT("CollisionProfileName"), ESearchCase::IgnoreCase))
	{
		if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Target))
		{
			OutText = Primitive->GetCollisionProfileName().ToString();
			OutType = TEXT("CollisionProfileName");
			return true;
		}
	}
	return false;
}

/**
 * 设置蓝图属性（支持 CDO 默认值和 SCS 组件属性）
 *
 * 请求参数:
 *   - blueprint_path: 蓝图路径（必填）
 *   - component_name: 组件名称（可选，为空则修改蓝图默认值 CDO，否则修改指定组件）
 *   - properties: 属性键值对（必填）
 *   - auto_compile: 是否自动编译（可选，默认 true）
 *
 * 返回:
 *   - ok: 全部属性写成、（要求编译时）编译通过、并且存盘成功才为 true；任何一处失败都是 false
 *   - blueprint_path: 蓝图路径
 *   - target_type: 修改的目标类型（"cdo" 或 "component"）
 *   - component_name: 组件名称（仅当修改组件时）
 *   - modified_properties: 成功修改的属性列表，每项带 value —— 写完（编译后）从目标上读回来的值
 *   - failed_properties: 修改失败的属性列表（含错误信息）
 *   - compiled: 编译是否真的通过（没要求编译时为 false）
 *   - compile_status: 编译后的蓝图状态（仅当要求编译时）
 *   - saved: 是否已保存
 *   - undoable: 这次改动是否进了撤销栈
 *
 * 写入包在 FUAL_ScopedTransaction 里，编译前结束事务 —— 完整编译不能留在事务里，
 * 理由见 UAL_ScopedTransaction.h。
 */
void FUAL_BlueprintCommands::Handle_SetBlueprintProperty(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数 blueprint_path
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	// 2. 解析 properties
	const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
	if (!Payload->TryGetObjectField(TEXT("properties"), PropertiesPtr) || !PropertiesPtr || !(*PropertiesPtr).IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: properties"));
		return;
	}
	const TSharedPtr<FJsonObject>& Properties = *PropertiesPtr;

	// 3. 解析可选参数
	FString ComponentName;
	Payload->TryGetStringField(TEXT("component_name"), ComponentName);
	
	bool bAutoCompile = true;
	if (Payload->HasField(TEXT("auto_compile")))
	{
		bAutoCompile = Payload->GetBoolField(TEXT("auto_compile"));
	}

	// 4. 加载蓝图资产
	UBlueprint* Blueprint = nullptr;
	
	// 尝试直接加载
	if (BlueprintPath.StartsWith(TEXT("/")))
	{
		FString FullPath = BlueprintPath;
		if (!FullPath.Contains(TEXT(".")))
		{
			FullPath = FullPath + TEXT(".") + FPaths::GetBaseFilename(BlueprintPath);
		}
		Blueprint = LoadObject<UBlueprint>(nullptr, *FullPath);
	}
	
	// 如果直接加载失败，通过 AssetRegistry 查找
	if (!Blueprint)
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		
		TArray<FAssetData> AssetList;
		FARFilter Filter;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
#else
		Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
#endif
		Filter.bRecursiveClasses = true;
		AssetRegistry.GetAssets(Filter, AssetList);
		
		for (const FAssetData& Asset : AssetList)
		{
			FString AssetName = Asset.AssetName.ToString();
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			FString AssetPath = Asset.GetObjectPathString();
#else
			FString AssetPath = Asset.ObjectPath.ToString();
#endif
			if (AssetName.Equals(BlueprintPath, ESearchCase::IgnoreCase) ||
				AssetPath.Contains(BlueprintPath))
			{
				Blueprint = Cast<UBlueprint>(Asset.GetAsset());
				break;
			}
		}
	}

	if (!Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	// 5. 确定目标对象（CDO 或 SCS 组件）
	//
	// 写成 lambda 是因为编译后还要再找一次：编译会换掉 CDO（以及 CDO 下的子对象），
	// 读回值必须从编译后的那一个上读
	auto ResolveTarget = [&Blueprint, &ComponentName](UObject*& OutObject, UClass*& OutClass)
	{
		OutObject = nullptr;
		OutClass = nullptr;

		if (ComponentName.IsEmpty())
		{
			// 修改蓝图默认值（CDO）
			if (Blueprint->GeneratedClass)
			{
				OutObject = Blueprint->GeneratedClass->GetDefaultObject();
				OutClass = Blueprint->GeneratedClass;
			}
			return;
		}

		// 修改 SCS 组件属性
		if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
		{
			for (USCS_Node* Node : SCS->GetAllNodes())
			{
				if (Node && Node->ComponentTemplate &&
					Node->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
				{
					OutObject = Node->ComponentTemplate;
					OutClass = Node->ComponentClass;
					return;
				}
			}
		}

		// Fallback: 尝试在 CDO 中查找同名子对象（针对 C++ 继承的组件）
		if (Blueprint->GeneratedClass)
		{
			if (UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject())
			{
				TArray<UObject*> SubObjects;
				GetObjectsWithOuter(CDO, SubObjects, false);
				for (UObject* SubObj : SubObjects)
				{
					if (SubObj && SubObj->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
					{
						OutObject = SubObj;
						OutClass = SubObj->GetClass();
						return;
					}
				}
			}
		}
	};

	UObject* TargetObject = nullptr;
	FString TargetType;
	UClass* TargetClass = nullptr;
	ResolveTarget(TargetObject, TargetClass);

	if (ComponentName.IsEmpty())
	{
		if (!TargetObject)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Blueprint has no generated class, please compile it first"));
			return;
		}
		TargetType = TEXT("cdo");
		UE_LOG(LogUALBlueprint, Log, TEXT("[blueprint.set_property] Target: CDO of %s"), *Blueprint->GetName());
	}
	else
	{
		if (!TargetObject)
		{
			UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Component '%s' not found in blueprint '%s'"), *ComponentName, *Blueprint->GetName()));
			return;
		}
		TargetType = TEXT("component");
		UE_LOG(LogUALBlueprint, Log, TEXT("[blueprint.set_property] Target: Component '%s' in %s"), *ComponentName, *Blueprint->GetName());
	}

	// 6. 应用属性 —— 进事务，Ctrl+Z 能一次撤回整批。
	// TOptional 装着是为了编译前提前结束这一笔（完整编译不能在事务里，见 UAL_ScopedTransaction.h）
	TOptional<FUAL_ScopedTransaction> Transaction;
	Transaction.Emplace(NSLOCTEXT("UALBlueprint", "SetBlueprintProperty", "Set Blueprint Property"));
	const bool bUndoable = Transaction->IsOutstanding();

	Blueprint->Modify();
	TargetObject->Modify();

	TArray<TSharedPtr<FJsonValue>> FailedPropsArray;
	// 成功项的回执对象，值等编译完再从目标上读回来填进去
	TArray<TSharedPtr<FJsonObject>> ModifiedInfos;
	auto AddModified = [&ModifiedInfos](const FString& PropName)
	{
		TSharedPtr<FJsonObject> ModInfo = MakeShared<FJsonObject>();
		ModInfo->SetStringField(TEXT("property"), PropName);
		ModifiedInfos.Add(ModInfo);
	};

	for (auto& Pair : Properties->Values)
	{
		const FString PropName = UAL_JsonKey(Pair.Key);
		const TSharedPtr<FJsonValue>& PropValue = Pair.Value;

		// 查找属性。细节面板里显示的名字也认，见 UAL_FindPropertyByAnyName
		FProperty* Prop = UAL_FindPropertyByAnyName(TargetClass, PropName);
		if (!Prop && TargetObject)
		{
			Prop = UAL_FindPropertyByAnyName(TargetObject->GetClass(), PropName);
		}

		if (!Prop)
		{
			// 反射查不到，才轮到「细节面板里在这个对象上、反射里却不是它的属性」
			// 的那几个（目前只有 CollisionProfileName）。
			//
			// 顺序不能反：抢在反射前面的话，蓝图里自己声明一个同名变量就再也设不进去了
			{
				FString DerivedError;
				bool bHandled = false;
				const bool bDerivedOk =
					UAL_CommandUtils::TrySetDerivedProperty(TargetObject, PropName, PropValue, DerivedError, bHandled);
				if (bHandled)
				{
					if (bDerivedOk)
					{
						// 以前这里塞的是裸字符串，和其余成功项的对象形状不一样，调用方按 .property 取到的是空
						AddModified(PropName);
					}
					else
					{
						TSharedPtr<FJsonObject> DerivedFail = MakeShared<FJsonObject>();
						DerivedFail->SetStringField(TEXT("property"), PropName);
						DerivedFail->SetStringField(
							TEXT("error"), DerivedError.IsEmpty() ? TEXT("failed to set") : DerivedError);
						FailedPropsArray.Add(MakeShared<FJsonValueObject>(DerivedFail));
					}
					continue;
				}
			}

			TSharedPtr<FJsonObject> FailInfo = MakeShared<FJsonObject>();
			FailInfo->SetStringField(TEXT("property"), PropName);

			// 名字其实是个**函数**时，别再报「属性找不到」然后列一堆不相关的属性。
			// 真机记录：调用方想设 SetCollisionProfileName（那是函数），
			// 我们回的建议是 bHiddenInGame / bOnlyOwnerSee / CustomPrimitiveData ——
			// 三个都和碰撞无关，等于把它往错误方向又推了一把。
			UClass* LookupClass = TargetClass ? TargetClass : (TargetObject ? TargetObject->GetClass() : nullptr);
			if (LookupClass && UAL_HasFunctionNamed(LookupClass, PropName))
			{
				FailInfo->SetStringField(TEXT("error"), FString::Printf(
					TEXT("'%s' is a function, not a property - call it with a node (blueprint_apply_graph) instead of set_property"),
					*PropName));
				FailedPropsArray.Add(MakeShared<FJsonValueObject>(FailInfo));
				continue;
			}

			FailInfo->SetStringField(TEXT("error"), TEXT("Property not found"));

			// 提供建议 —— 但只留真的像的，理由见 UAL_KeepCloseSuggestions
			TArray<FString> AllProps;
			UAL_CommandUtils::CollectPropertyNames(TargetObject, AllProps);
			TArray<FString> Suggestions;
			UAL_CommandUtils::SuggestProperties(PropName, AllProps, Suggestions, 3);
			UAL_KeepCloseSuggestions(PropName, Suggestions);
			if (Suggestions.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> SuggestArr;
				for (const FString& Sug : Suggestions)
				{
					SuggestArr.Add(MakeShared<FJsonValueString>(Sug));
				}
				FailInfo->SetArrayField(TEXT("suggestions"), SuggestArr);
			}

			FailedPropsArray.Add(MakeShared<FJsonValueObject>(FailInfo));
			continue;
		}

		// 设置属性值
		FString PropError;
		bool bSuccess = UAL_CommandUtils::SetSimpleProperty(Prop, TargetObject, PropValue, PropError);
		
		if (bSuccess)
		{
			AddModified(PropName);
			UE_LOG(LogUALBlueprint, Log, TEXT("[blueprint.set_property] Set '%s' successfully"), *PropName);
		}
		else
		{
			TSharedPtr<FJsonObject> FailInfo = MakeShared<FJsonObject>();
			FailInfo->SetStringField(TEXT("property"), PropName);
			FailInfo->SetStringField(TEXT("error"), PropError.IsEmpty() ? TEXT("Failed to set property") : PropError);
			FailedPropsArray.Add(MakeShared<FJsonValueObject>(FailInfo));
			UE_LOG(LogUALBlueprint, Warning, TEXT("[blueprint.set_property] Failed to set '%s': %s"), *PropName, *PropError);
		}
	}

	// 7. 标记蓝图已修改，然后结束事务（下面的完整编译不能在事务里）
	const bool bAnyWritten = ModifiedInfos.Num() > 0;
	if (bAnyWritten)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}
	else
	{
		// 一项都没写成，净改动为零：撤掉这条空记录（引擎自己也只在这种情况下 Cancel）
		Transaction->Cancel();
	}
	Transaction.Reset();

	// 8. 编译蓝图（如果需要）
	// 原来调用完就无条件置 true —— 编译报错也回「已编译」。以编译后的真实状态为准，带警告也算通过
	bool bCompiled = false;
	FString CompileStatus;
	if (bAutoCompile && bAnyWritten)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		bCompiled = Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
		switch (Blueprint->Status)
		{
		case BS_UpToDate:             CompileStatus = TEXT("UpToDate"); break;
		case BS_UpToDateWithWarnings: CompileStatus = TEXT("UpToDateWithWarnings"); break;
		case BS_Dirty:                CompileStatus = TEXT("Dirty"); break;
		case BS_Error:                CompileStatus = TEXT("Error"); break;
		case BS_Unknown:              CompileStatus = TEXT("Unknown"); break;
		default:                      CompileStatus = TEXT("Other"); break;
		}
		UE_LOG(LogUALBlueprint, Log, TEXT("[blueprint.set_property] Blueprint compiled, status %s"), *CompileStatus);
	}

	// 9. 读回：从（编译后的）目标上把每个成功项的值读出来，回执报的是引擎里的值
	UObject* ReadTarget = nullptr;
	UClass* ReadClass = nullptr;
	ResolveTarget(ReadTarget, ReadClass);

	TArray<TSharedPtr<FJsonValue>> ModifiedPropsArray;
	for (const TSharedPtr<FJsonObject>& ModInfo : ModifiedInfos)
	{
		const FString PropName = ModInfo->GetStringField(TEXT("property"));
		FString ValueText;
		FString TypeName;
		if (UAL_ReadBackPropertyText(ReadTarget, ReadClass, PropName, ValueText, TypeName))
		{
			ModInfo->SetStringField(TEXT("type"), TypeName);
			ModInfo->SetStringField(TEXT("value"), ValueText);
			ModifiedPropsArray.Add(MakeShared<FJsonValueObject>(ModInfo));
		}
		else
		{
			// 写进去了，编译后却在目标上找不到 —— 状态说不清，不能算成功
			TSharedPtr<FJsonObject> FailInfo = MakeShared<FJsonObject>();
			FailInfo->SetStringField(TEXT("property"), PropName);
			FailInfo->SetStringField(TEXT("error"), bCompiled
				? TEXT("written, but it could not be read back from the target after compiling - the value may not have survived the compile")
				: TEXT("written, but it could not be read back from the target"));
			FailedPropsArray.Add(MakeShared<FJsonValueObject>(FailInfo));
		}
	}

	// 10. 保存蓝图（一项都没写成就不动磁盘）
	bool bSaved = false;
	UPackage* Package = bAnyWritten ? Blueprint->GetOutermost() : nullptr;
	if (Package)
	{
		const FString PackageName = Package->GetName();
		const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		const bool bSavedOk_PropSaveResult = UPackage::Save(Package, Blueprint, *PackageFileName, SaveArgs).Result == ESavePackageResult::Success;
#else
		const bool bSavedOk_PropSaveResult = UPackage::SavePackage(Package, Blueprint, *PackageFileName, SaveArgs);
#endif
		// 原来无条件置 true —— 存不下去也报「已保存」，用户重启编辑器才发现改动没了
		bSaved = bSavedOk_PropSaveResult;
		if (!bSaved)
		{
			UE_LOG(LogUALBlueprint, Warning, TEXT("Property changes applied in memory but package '%s' could not be saved"), *PackageName);
		}
	}

	// 11. 构建响应
	const int32 NumOk = ModifiedPropsArray.Num();
	const int32 NumFailed = FailedPropsArray.Num();
	const bool bCompileFailed = bAutoCompile && bAnyWritten && !bCompiled;
	const bool bSaveFailed = bAnyWritten && !bSaved;
	// 原来是「没有失败 或 有一项成功」—— 部分失败也回 ok:true。现在任何一处失败都不算成功
	const bool bOk = NumOk > 0 && NumFailed == 0 && !bCompileFailed && !bSaveFailed;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Result->SetStringField(TEXT("target_type"), TargetType);

	if (!ComponentName.IsEmpty())
	{
		Result->SetStringField(TEXT("component_name"), ComponentName);
	}

	Result->SetArrayField(TEXT("modified_properties"), ModifiedPropsArray);
	Result->SetArrayField(TEXT("failed_properties"), FailedPropsArray);
	Result->SetBoolField(TEXT("compiled"), bCompiled);
	if (!CompileStatus.IsEmpty())
	{
		Result->SetStringField(TEXT("compile_status"), CompileStatus);
	}
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("undoable"), bUndoable && bAnyWritten);

	// 生成消息。有任何失败时第一句就是「N succeeded / M failed」，不以成功开头
	const FString TargetLabel = ComponentName.IsEmpty() ? Blueprint->GetName() : ComponentName;
	FString Message;
	if (bOk)
	{
		Message = FString::Printf(TEXT("Set %d properties on %s '%s'"), NumOk, *TargetType, *TargetLabel);
	}
	else
	{
		Message = FString::Printf(TEXT("%d succeeded / %d failed on %s '%s'"), NumOk, NumFailed, *TargetType, *TargetLabel);
		if (NumFailed > 0)
		{
			Message += TEXT(" - see failed_properties");
		}
		if (bCompileFailed)
		{
			Message += FString::Printf(TEXT("; the blueprint failed to compile afterwards (status %s) - run blueprint_compile for the errors"), *CompileStatus);
		}
		if (bSaveFailed)
		{
			Message += TEXT("; changes are in memory only, the package could not be saved");
		}
	}
	Result->SetStringField(TEXT("message"), Message);

	const int32 Code = bOk ? 200 : (NumOk > 0 ? 207 : 400);
	UAL_CommandUtils::SendResponse(RequestId, Code, Result);
}

/**
 * 添加蓝图成员变量
 *
 * 请求参数:
 *   - blueprint_path: 蓝图路径（必填）
 *   - name: 变量名（必填）
 *   - type: 变量类型（必填），如 bool/int/float/string/name/text/vector/rotator/object
 *   - object_class: 当 type=object/class/soft_object/soft_class 时指定类（可选）
 *   - is_array: 是否数组（可选，默认 false）
 *   - default_value: 默认值（可选，字符串形式，走 BlueprintEditorUtils 解析）
 *
 * 响应:
 *   - ok: 是否成功
 *   - blueprint_path: 蓝图路径
 *   - variable: 变量信息
 */
void FUAL_BlueprintCommands::Handle_AddVariableToBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	FString VarNameStr;
	if (!Payload->TryGetStringField(TEXT("name"), VarNameStr) || VarNameStr.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: name"));
		return;
	}

	FString TypeStr;
	if (!Payload->TryGetStringField(TEXT("type"), TypeStr) || TypeStr.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: type"));
		return;
	}

	bool bIsArray = false;
	Payload->TryGetBoolField(TEXT("is_array"), bIsArray);

	FString ObjectClassStr;
	Payload->TryGetStringField(TEXT("object_class"), ObjectClassStr);

	FString DefaultValueStr;
	Payload->TryGetStringField(TEXT("default_value"), DefaultValueStr);

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	const FName VarName(*VarNameStr);

	// 避免重名
	if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName) != INDEX_NONE)
	{
		UAL_CommandUtils::SendError(RequestId, 409, FString::Printf(TEXT("Variable already exists: %s"), *VarNameStr));
		return;
	}

	// 类型解析走全仓库唯一那份 UAL_ResolvePinType（见文件上方）。
	// 这里以前是第二份手抄的类型表，只认四个硬编码的结构体，
	// 于是「片段里有个 Transform 变量」这种再常见不过的情况就卡死。
	FString Container;
	if (!Payload->TryGetStringField(TEXT("container"), Container))
	{
		Container = bIsArray ? TEXT("array") : TEXT("");
	}

	FEdGraphPinType PinType;
	FString TypeError;
	if (!UAL_ResolvePinType(TypeStr, ObjectClassStr, Container, PinType, TypeError))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TypeError);
		return;
	}

	FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, PinType);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	/**
	 * 默认值。
	 *
	 * 这里以前只写一句 `UE_LOG(Warning, "not fully supported")` 就算完，然后把
	 * 入参**原样回显**进响应。调用方看到的是「成功，default_value = 90.0」，
	 * CDO 里实际是 0，而且没有任何迹象。2026-09-16 的用户反馈里八个变量全是 0，
	 * 把一整轮「门为什么不动」的排查引到了完全错的方向。
	 *
	 * 正确的落法不是那个不存在的 `SetBlueprintVariableDefaultValue`，而是写进
	 * `FBPVariableDescription::DefaultValue` —— 编译时引擎自己会搬进 CDO
	 * （KismetCompiler.cpp `CreateClassVariablesFromBlueprint` → `SetPropertyDefaultValue`，
	 * 5.0–5.8 九版都在）。
	 *
	 * 但光写进去还不够：字面量不合法时 ImportText 会失败，引擎**静默留 0**。
	 * 所以先拿一块临时内存试导一次，认不出来就当场报错，不放过去。
	 */
	FString AppliedDefault;
	if (!DefaultValueStr.IsEmpty())
	{
		FProperty* NewProp = Blueprint->SkeletonGeneratedClass
			? Blueprint->SkeletonGeneratedClass->FindPropertyByName(VarName)
			: nullptr;

		FString DefaultError;
		if (!NewProp)
		{
			DefaultError = TEXT("the variable's property did not appear on the skeleton class, so the default value could not be verified");
		}
		else
		{
			// 对齐要按属性自己的要求来，TArray<uint8> 的缓冲区不保证对得上
			void* Scratch = FMemory::Malloc(NewProp->GetSize(), NewProp->GetMinAlignment());
			NewProp->InitializeValue(Scratch);
			if (FBlueprintEditorUtils::PropertyValueFromString_Direct(NewProp, DefaultValueStr, static_cast<uint8*>(Scratch)))
			{
				FBlueprintEditorUtils::PropertyValueToString_Direct(NewProp, static_cast<const uint8*>(Scratch), AppliedDefault);
			}
			else if (PinType.ContainerType == EPinContainerType::None)
			{
				DefaultError = FString::Printf(
					TEXT("'%s' is not a valid literal for type '%s'. Use Unreal's own text form: 90.0 for numbers, true/false for bool, (X=0,Y=0,Z=0) for structs, /Game/Path/Asset.Asset for asset references"),
					*DefaultValueStr, *TypeStr);
			}
			else
			{
				/**
				 * 容器变量要按容器说。以前这里只报标量类型名，于是 bool 数组填 "false"
				 * 得到的是「'false' 不是合法的 bool 字面量……bool 请写 true/false」——
				 * 前后两句自相矛盾，真正的原因（它是数组）一个字没提（2026-09-24 用户反馈）。
				 */
				// Map 的字面量是键值对的列表，和数组 / 集合的平铺列表不是一个写法
				const TCHAR* ListForm = PinType.ContainerType == EPinContainerType::Map
					? TEXT("a list of (Key,Value) pairs, e.g. ((\"A\",1),(\"B\",2))")
					: TEXT("a parenthesised list, e.g. (true,false) or (1.0,2.0)");
				DefaultError = FString::Printf(
					TEXT("'%s' is not a valid literal for '%s' (a %s variable, not a single %s). Container defaults use Unreal's text form: %s; leave default_value empty for an empty container. If you meant a single value, recreate the variable without container / is_array"),
					*DefaultValueStr, *UAL_DisplayTypeName(TypeStr, PinType), UAL_ContainerName(PinType), *TypeStr, ListForm);
			}
			NewProp->DestroyValue(Scratch);
			FMemory::Free(Scratch);
		}

		if (!DefaultError.IsEmpty())
		{
			// 变量确实建出来了，别假装什么都没发生 —— 说清楚建了但默认值没落地
			UAL_CommandUtils::SendError(
				RequestId, 400,
				FString::Printf(TEXT("Variable '%s' was created but its default_value was not applied: %s"),
					*VarNameStr, *DefaultError));
			return;
		}

		const int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName);
		if (VarIndex != INDEX_NONE)
		{
			Blueprint->Modify();
			Blueprint->NewVariables[VarIndex].DefaultValue = DefaultValueStr;
			// 默认值只在**完整编译**时才搬进 CDO（骨架编译不搬）。不编译就返回的话，
			// 这个值要等到别人下一次编译才生效，而调用方以为它已经生效了
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
	TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
	VarObj->SetStringField(TEXT("name"), VarNameStr);
	VarObj->SetStringField(TEXT("type"), PinType.PinCategory.ToString());
	// 容器从解析出来的 PinType 读，不回显入参：`container` 优先于 `is_array`，
	// 两个都传且矛盾时回显 is_array 会把一个数组说成标量（2026-09-24 用户反馈）
	VarObj->SetBoolField(TEXT("is_array"), PinType.ContainerType == EPinContainerType::Array);
	if (const TCHAR* ContainerName = UAL_ContainerName(PinType))
	{
		VarObj->SetStringField(TEXT("container"), ContainerName);
	}
	if (PinType.PinSubCategoryObject.IsValid())
	{
		if (const UObject* Obj = PinType.PinSubCategoryObject.Get())
		{
			VarObj->SetStringField(TEXT("sub_category_object"), Obj->GetPathName());
		}
	}
	// 回**引擎读出来的那个值**，不是入参的回声 —— 回声正是这条缺陷原来的样子
	if (!AppliedDefault.IsEmpty())
	{
		VarObj->SetStringField(TEXT("default_value"), AppliedDefault);
	}
	Result->SetObjectField(TEXT("variable"), VarObj);

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/**
 * 获取蓝图图表信息（节点、引脚元数据）
 *
 * 请求参数:
 *   - blueprint_path: 蓝图路径（必填）
 *   - graph_name: 图表名（可选，默认 EventGraph）
 *
 * 响应:
 *   - ok
 *   - blueprint_path
 *   - graph_name
 *   - nodes: [{ node_id, class, title, pos_x, pos_y, pins:[...] }]
 */
void FUAL_BlueprintCommands::Handle_GetBlueprintGraph(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 兼容 blueprint_path 和 blueprint_name
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		if (!Payload->TryGetStringField(TEXT("blueprint_name"), BlueprintPath) || BlueprintPath.IsEmpty())
		{
			UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path (or blueprint_name). Example: {\"blueprint_path\": \"/Game/Blueprints/BP_Hero\"}"));
			return;
		}
	}

	FString GraphName;
	Payload->TryGetStringField(TEXT("graph_name"), GraphName);

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	UEdGraph* Graph = UAL_FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		return;
	}

	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}
		if (TSharedPtr<FJsonObject> NodeObj = UAL_BuildNodeJson(Node))
		{
			Nodes.Add(MakeShared<FJsonValueObject>(NodeObj));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("graph_name"), Graph->GetName());
	Result->SetArrayField(TEXT("nodes"), Nodes);
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/**
 * 在蓝图图表中添加节点
 *
 * 请求参数:
 *   - blueprint_path: 蓝图路径（必填）
 *   - graph_name: 图表名（可选，默认 EventGraph）
 *   - node_type: Function/Event/VariableGet/VariableSet（必填）
 *   - node_name: 节点名（必填）
 *   - node_position: {x,y}（可选，默认 0,0）
 *
 * 响应:
 *   - ok, node_id, pins[]
 */
void FUAL_BlueprintCommands::Handle_ListBlueprintGraphs(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		if (!Payload->TryGetStringField(TEXT("blueprint_name"), BlueprintPath) || BlueprintPath.IsEmpty())
		{
			UAL_CommandUtils::SendError(
				RequestId,
				400,
				TEXT("Missing required field: blueprint_path (or blueprint_name). Example: {\"blueprint_path\": \"/Game/Blueprints/BP_Hero\"}")
			);
			return;
		}
	}

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	const FUAL_GraphDiscoveryResult GraphDiscovery = UAL_CollectBlueprintGraphs(Blueprint);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
	Result->SetNumberField(TEXT("graph_count"), GraphDiscovery.Graphs.Num());
	Result->SetArrayField(TEXT("graph_names"), GraphDiscovery.GraphNames);
	Result->SetArrayField(TEXT("graphs"), GraphDiscovery.Graphs);
	Result->SetArrayField(TEXT("event_graphs"), GraphDiscovery.EventGraphs);
	Result->SetArrayField(TEXT("function_graphs"), GraphDiscovery.FunctionGraphs);
	Result->SetArrayField(TEXT("macro_graphs"), GraphDiscovery.MacroGraphs);
	Result->SetArrayField(TEXT("delegate_graphs"), GraphDiscovery.DelegateGraphs);
	Result->SetArrayField(TEXT("intermediate_graphs"), GraphDiscovery.IntermediateGraphs);
	if (!GraphDiscovery.PreferredGraphName.IsEmpty())
	{
		Result->SetStringField(TEXT("preferred_graph"), GraphDiscovery.PreferredGraphName);
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/**
 * 创建蓝图函数图表（用户自定义函数）
 *
 * 请求参数:
 *   - blueprint_path: 蓝图路径（必填）
 *   - function_name: 函数名（必填）
 *   - inputs: [{ name, type, object_class?, is_array? }]（可选）
 *   - outputs: [{ name, type, object_class?, is_array? }]（可选）
 *   - pure: 是否纯函数（可选，默认 false）
 *
 * 响应:
 *   - ok
 *   - graph_name
 *   - entry_node_id
 *   - result_node_id
 */
void FUAL_BlueprintCommands::Handle_CreateFunctionGraph(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	FString FunctionName;
	if (!Payload->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: function_name"));
		return;
	}

	bool bPure = false;
	Payload->TryGetBoolField(TEXT("pure"), bPure);

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	// 重名检测：函数图表是否已存在
	{
		UEdGraph* Existing = UAL_FindGraph(Blueprint, FunctionName);
		if (Existing)
		{
			UAL_CommandUtils::SendError(RequestId, 409, FString::Printf(TEXT("Function graph already exists: %s"), *FunctionName));
			return;
		}
	}

	/**
	 * 参数表先校验，再动蓝图。
	 *
	 * 这里以前是「解析不了就打条日志跳过」：调用方要三个参数、拿到两个，
	 * 返回却是 ok=true —— 少掉的那个要等到后面连线连不上才暴露，
	 * 而那时候已经看不出是哪一步丢的了。
	 *
	 * 校验提到建图之前，是为了失败时蓝图一点没动：建到一半再报错的话，
	 * 图里会留下一个签名只有半截的函数，重发一次还撞重名。
	 */
	TArray<FUAL_UserPinSpec> InputSpecs;
	TArray<FUAL_UserPinSpec> OutputSpecs;
	{
		FString ParamError;
		const TArray<TSharedPtr<FJsonValue>>* ParamArray = nullptr;
		if (Payload->TryGetArrayField(TEXT("inputs"), ParamArray) && ParamArray
			&& !UAL_ParseUserPinSpecs(*ParamArray, TEXT("inputs"), InputSpecs, ParamError))
		{
			UAL_CommandUtils::SendError(RequestId, 400, ParamError);
			return;
		}

		ParamArray = nullptr;
		if (Payload->TryGetArrayField(TEXT("outputs"), ParamArray) && ParamArray
			&& !UAL_ParseUserPinSpecs(*ParamArray, TEXT("outputs"), OutputSpecs, ParamError))
		{
			UAL_CommandUtils::SendError(RequestId, 400, ParamError);
			return;
		}
	}

	// 1) 创建函数图表
	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint,
		FName(*FunctionName),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);
	if (!NewGraph)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create function graph"));
		return;
	}

	// 添加到蓝图 FunctionGraphs
	// 添加到蓝图 FunctionGraphs
	// UE5.0~5.3 的 AddFunctionGraph 需要显式模板参数
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph, /*bIsUserCreated*/ true, nullptr);

	// 2) 获取/创建入口与返回节点
	UK2Node_FunctionEntry* EntryNode = nullptr;
	UK2Node_FunctionResult* ResultNode = nullptr;
	{
		TArray<UK2Node_FunctionEntry*> EntryNodes;
		NewGraph->GetNodesOfClass(EntryNodes);
		if (EntryNodes.Num() > 0)
		{
			EntryNode = EntryNodes[0];
		}

		TArray<UK2Node_FunctionResult*> ResultNodes;
		NewGraph->GetNodesOfClass(ResultNodes);
		if (ResultNodes.Num() > 0)
		{
			ResultNode = ResultNodes[0];
		}
	}

	auto EnsureEntryNode = [&]() -> UK2Node_FunctionEntry*
	{
		if (EntryNode)
		{
			return EntryNode;
		}
		FGraphNodeCreator<UK2Node_FunctionEntry> Creator(*NewGraph);
		UK2Node_FunctionEntry* N = Creator.CreateNode();
		N->NodePosX = 0;
		N->NodePosY = 0;
		Creator.Finalize();
		N->CreateNewGuid();
		N->PostPlacedNewNode();
		N->AllocateDefaultPins();
		N->ReconstructNode();
		EntryNode = N;
		return EntryNode;
	};

	auto EnsureResultNode = [&]() -> UK2Node_FunctionResult*
	{
		if (ResultNode)
		{
			return ResultNode;
		}
		FGraphNodeCreator<UK2Node_FunctionResult> Creator(*NewGraph);
		UK2Node_FunctionResult* N = Creator.CreateNode();
		N->NodePosX = 400;
		N->NodePosY = 0;
		Creator.Finalize();
		N->CreateNewGuid();
		N->PostPlacedNewNode();
		N->AllocateDefaultPins();
		N->ReconstructNode();
		ResultNode = N;
		return ResultNode;
	};

	EntryNode = EnsureEntryNode();
	ResultNode = EnsureResultNode();

	// 3) 设置 pure（best-effort：不同版本字段可能不同）
	if (bPure && EntryNode)
	{
		// 尝试设置 ExtraFlags |= FUNC_BlueprintPure
		if (FIntProperty* ExtraFlagsProp = FindFProperty<FIntProperty>(EntryNode->GetClass(), TEXT("ExtraFlags")))
		{
			const int32 Cur = ExtraFlagsProp->GetPropertyValue_InContainer(EntryNode);
			ExtraFlagsProp->SetPropertyValue_InContainer(EntryNode, Cur | (int32)FUNC_BlueprintPure);
		}
		// 尝试设置 bIsPureFunc = true
		if (FBoolProperty* PureProp = FindFProperty<FBoolProperty>(EntryNode->GetClass(), TEXT("bIsPureFunc")))
		{
			PureProp->SetPropertyValue_InContainer(EntryNode, true);
		}
		EntryNode->ReconstructNode();
	}

	// 4) 创建 inputs/outputs 用户定义引脚（类型在动蓝图之前已经全部校验过）
	if (EntryNode && ResultNode)
	{
		// 入口节点的用户引脚是**输出**方向（函数体从这里取值），结果节点反之。
		// 这一条反直觉，写反了引脚会长在错误的一侧
		UAL_CreateUserPins(EntryNode, InputSpecs, EGPD_Output);
		UAL_CreateUserPins(ResultNode, OutputSpecs, EGPD_Input);
		EntryNode->ReconstructNode();
		ResultNode->ReconstructNode();
	}

	// 5) 编译更新
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	// 6) 返回
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("graph_name"), FunctionName);
	Result->SetStringField(TEXT("entry_node_id"), EntryNode ? UAL_GuidToString(EntryNode->NodeGuid) : TEXT(""));
	Result->SetStringField(TEXT("result_node_id"), ResultNode ? UAL_GuidToString(ResultNode->NodeGuid) : TEXT(""));
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/**
 * 编译蓝图并可选保存
 *
 * 请求参数:
 *   - blueprint_path: 蓝图路径（必填）
 *   - save: 编译成功后是否保存（可选，默认 true）
 *
 * 响应:
 *   - ok: 是否编译成功（状态为 UpToDate）
 *   - status: 编译状态 (UpToDate / Dirty / Error / Unknown / Other)
 *   - saved: 是否已保存
 *   - path: 蓝图路径
 */
void FUAL_BlueprintCommands::Handle_CompileBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析参数
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing blueprint_path"));
		return;
	}

	bool bSave = true;
	Payload->TryGetBoolField(TEXT("save"), bSave);

	// 2. 加载蓝图
	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath = BlueprintPath;

	if (BlueprintPath.StartsWith(TEXT("/")))
	{
		if (!BlueprintPath.Contains(TEXT(".")))
		{
			ResolvedPath = BlueprintPath + TEXT(".") + FPaths::GetBaseFilename(BlueprintPath);
		}
		Blueprint = LoadObject<UBlueprint>(nullptr, *ResolvedPath);
	}

	// 如果直接加载失败，通过 AssetRegistry 查找
	if (!Blueprint)
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		TArray<FAssetData> AssetList;
		FARFilter Filter;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
#else
		Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
#endif
		Filter.bRecursiveClasses = true;
		AssetRegistry.GetAssets(Filter, AssetList);

		for (const FAssetData& Asset : AssetList)
		{
			FString AssetName = Asset.AssetName.ToString();
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			FString AssetPath = Asset.GetObjectPathString();
#else
			FString AssetPath = Asset.ObjectPath.ToString();
#endif
			if (AssetName.Equals(BlueprintPath, ESearchCase::IgnoreCase) ||
				AssetPath.Contains(BlueprintPath))
			{
				Blueprint = Cast<UBlueprint>(Asset.GetAsset());
				ResolvedPath = AssetPath;
				break;
			}
		}
	}

	if (!Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	// 3. 执行编译（带诊断日志）
	FCompilerResultsLog ResultsLog;
	ResultsLog.bSilentMode = true;
	ResultsLog.bLogInfoOnly = false;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	ResultsLog.bAnnotateMentionedNodes = true;
#endif
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2)
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &ResultsLog);
#else
	// UE5.0/5.1 兼容
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &ResultsLog);
#endif

	// 3.1 收集 diagnostics（用于 Agent 自修复）
	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	for (const TSharedRef<FTokenizedMessage>& Msg : ResultsLog.Messages)
	{
		TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();

		FString SeverityStr = TEXT("Info");
		switch (Msg->GetSeverity())
		{
		case EMessageSeverity::Error:   SeverityStr = TEXT("Error"); break;
		case EMessageSeverity::Warning: SeverityStr = TEXT("Warning"); break;
		case EMessageSeverity::Info:    SeverityStr = TEXT("Info"); break;
		default:                        SeverityStr = TEXT("Other"); break;
		}
		D->SetStringField(TEXT("type"), SeverityStr);
		D->SetStringField(TEXT("message"), Msg->ToText().ToString());

		// 尝试绑定 node_id（best-effort）
		FString NodeId;
		FString PinName;
		UObject* FoundObj = nullptr;

		for (const TSharedRef<IMessageToken>& Tok : Msg->GetMessageTokens())
		{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			if (Tok->GetType() == EMessageToken::Object)
			{
				const TSharedRef<FUObjectToken> ObjTok = StaticCastSharedRef<FUObjectToken>(Tok);
				FoundObj = ObjTok->GetObject().Get();
				if (FoundObj)
				{
					break;
				}
			}
#else
			// UE5.0: FObjectToken/FUObjectToken 不可用，跳过对象绑定
			(void)Tok;
#endif
		}

		if (UEdGraphNode* AsNode = Cast<UEdGraphNode>(FoundObj))
		{
			NodeId = UAL_GuidToString(AsNode->NodeGuid);
		}

		if (!NodeId.IsEmpty())
		{
			D->SetStringField(TEXT("node_id"), NodeId);
		}
		if (!PinName.IsEmpty())
		{
			D->SetStringField(TEXT("pin"), PinName);
		}

		Diagnostics.Add(MakeShared<FJsonValueObject>(D));
	}

	// 4. 检查结果状态
	//
	// BS_UpToDateWithWarnings 也算编译通过。漏掉它的后果不小：带警告的蓝图
	// （未使用的变量、废弃节点……）在实际工程里非常常见，原来会落进 default
	// 分支报成 status="Other" 且 ok=false —— 调用方看到的是「编译失败」，
	// 于是去修一个根本不存在的错误，还因为 bCompileSuccess 为假而不保存。
	const bool bCompileSuccess =
		(Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);

	FString StatusStr;
	switch (Blueprint->Status)
	{
	case BS_UpToDate:             StatusStr = TEXT("UpToDate"); break;
	case BS_UpToDateWithWarnings: StatusStr = TEXT("UpToDateWithWarnings"); break;
	case BS_Dirty:                StatusStr = TEXT("Dirty"); break;
	case BS_Error:                StatusStr = TEXT("Error"); break;
	case BS_Unknown:              StatusStr = TEXT("Unknown"); break;
	default:                      StatusStr = TEXT("Other"); break;
	}

	// 5. 保存（仅在请求要求且编译成功时执行，避免写入坏蓝图）
	bool bSaved = false;
	if (bSave && bCompileSuccess)
	{
		if (UPackage* Package = Blueprint->GetOutermost())
		{
			const FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			const bool bSavedOk_CompileSaveResult = UPackage::Save(Package, Blueprint, *PackageFileName, SaveArgs).Result == ESavePackageResult::Success;
#else
			const bool bSavedOk_CompileSaveResult = UPackage::SavePackage(Package, Blueprint, *PackageFileName, SaveArgs);
#endif
			// 原来无条件置 true。这个工具的卖点就是「编译并保存」——
			// 保存失败却报 saved:true，等于把「改动已落盘」这件事整个说反了
			bSaved = bSavedOk_CompileSaveResult;
			if (!bSaved)
			{
				UE_LOG(LogUALBlueprint, Warning, TEXT("Blueprint compiled but package '%s' could not be saved"), *Package->GetName());
			}
		}
	}

	// 6. 返回结果
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), bCompileSuccess);
	Result->SetStringField(TEXT("status"), StatusStr);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetStringField(TEXT("path"), Blueprint->GetPathName());
	Result->SetArrayField(TEXT("diagnostics"), Diagnostics);

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// 辅助函数实现
// ============================================================================

/**
 * 收集蓝图的所有组件信息（包括 SCS 添加的和父类继承的）
 */
/**
 * 收集蓝图的组件，**带上真实的挂载层级**。
 *
 * ## 为什么不能读 ParentComponentOrVariableName
 *
 * 上一版每个组件的 `attach_to` 直接取 `Node->ParentComponentOrVariableName`，
 * 结果是**只要组件挂在本蓝图的另一个组件下，这个字段就永远是 None**。
 *
 * 查引擎源码（SCS_Node.cpp）：那个字段只有 `SetParent()` 会写，而 SetParent
 * 只在挂到**本蓝图 SCS 之外**的东西上时才被调用（继承来的组件、原生组件）。
 * 蓝图内部组件之间的层级走的是完全另一条路 —— `AddChildNode()` 往
 * `ChildNodes` 里塞，**碰都不碰** ParentComponentOrVariableName：
 *
 *     void USCS_Node::AddChildNode(USCS_Node* InNode, bool bAddToAllNodes)
 *     {
 *         ChildNodes.Add(InNode);   // 层级只记在这儿
 *     }
 *
 * 后果不是「字段不准」，是**读错了地方**：调用方拿到一片 None，没法知道
 * 门是绕门轴转还是绕中心转，只能退回去用 Python 反射 SCS ——
 * 实测里为了回答「组件挂在谁下面」要绕三次工具调用。
 *
 * 现在从 `GetRootNodes()` 递归 `GetChildNodes()` 走真正的那棵树，
 * 父子关系由遍历本身决定；根节点才回落到 ParentComponentOrVariableName
 * （那时候它指向的是原生/继承组件，正是它有意义的场合）。
 */
TArray<TSharedPtr<FJsonValue>> FUAL_BlueprintCommands::CollectComponentsInfo(UBlueprint* Blueprint)
{
	TArray<TSharedPtr<FJsonValue>> ComponentsArray;
	if (!Blueprint) return ComponentsArray;

	TSet<FString> AddedNames; // 避免重复

	auto MakeScsEntry = [](USCS_Node* Node, const FString& ParentName, int32 Depth)
	{
		TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
		CompObj->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
		CompObj->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT("Unknown"));
		CompObj->SetStringField(TEXT("class_path"), Node->ComponentClass ? Node->ComponentClass->GetPathName() : TEXT(""));
		CompObj->SetStringField(TEXT("source"), TEXT("added"));
		CompObj->SetBoolField(TEXT("editable"), true);
		// 空字符串表示「它就是根，没有父组件」—— 和「查不到」要能区分开
		CompObj->SetStringField(TEXT("attach_to"), ParentName);
		// 层级深度：让调用方不用自己重建树就能看懂结构
		CompObj->SetNumberField(TEXT("depth"), Depth);
		// 父组件是不是原生的（C++ 里声明的），影响能不能改它
		CompObj->SetBoolField(TEXT("attach_parent_is_native"), Node->bIsParentComponentNative);
		// 挂在骨骼 socket 上时才有值
		if (!Node->AttachToName.IsNone())
		{
			CompObj->SetStringField(TEXT("attach_socket"), Node->AttachToName.ToString());
		}
		return CompObj;
	};

	if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
	{
		// 递归走真正的那棵树。用显式栈而不是递归 lambda —— 组件层级理论上
		// 可以很深，而且这样也不用为了自递归去绕 std::function。
		struct FPending
		{
			USCS_Node* Node;
			FString ParentName;
			int32 Depth;
		};
		TArray<FPending> Stack;

		/**
		 * `FName` 为空时 `ToString()` 给的是字面量 **"None"**，不是空串。
		 * 直接透传出去，调用方看到的是「有个叫 None 的父组件」——
		 * 又一个「看起来像数据、实际表示没有」的字段。空的就回空串。
		 */
		auto ParentNameOf = [](const USCS_Node* Node) -> FString
		{
			return Node->ParentComponentOrVariableName.IsNone()
				? FString()
				: Node->ParentComponentOrVariableName.ToString();
		};

		for (USCS_Node* Root : SCS->GetRootNodes())
		{
			if (!Root) continue;
			// 根节点的父亲在本 SCS 之外（原生或继承的组件），
			// 这正是 ParentComponentOrVariableName 唯一有意义的场合
			Stack.Push({ Root, ParentNameOf(Root), 0 });
		}

		while (Stack.Num() > 0)
		{
			const FPending Current = Stack.Pop();
			if (!Current.Node) continue;

			const FString CompName = Current.Node->GetVariableName().ToString();
			if (AddedNames.Contains(CompName)) continue;  // 环形引用兜底
			AddedNames.Add(CompName);

			ComponentsArray.Add(MakeShared<FJsonValueObject>(
				MakeScsEntry(Current.Node, Current.ParentName, Current.Depth)));

			for (USCS_Node* Child : Current.Node->GetChildNodes())
			{
				if (Child)
				{
					Stack.Push({ Child, CompName, Current.Depth + 1 });
				}
			}
		}

		// 兜底：理论上每个节点都能从根走到，但真出现挂不上树的节点也不能
		// 静默吞掉 —— 少报一个组件比报错层级更难查
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (!Node) continue;
			const FString CompName = Node->GetVariableName().ToString();
			if (AddedNames.Contains(CompName)) continue;
			AddedNames.Add(CompName);

			TSharedPtr<FJsonObject> Orphan = MakeScsEntry(
				Node,
				Node->ParentComponentOrVariableName.IsNone()
					? FString()
					: Node->ParentComponentOrVariableName.ToString(),
				0);
			Orphan->SetBoolField(TEXT("detached_from_tree"), true);
			ComponentsArray.Add(MakeShared<FJsonValueObject>(Orphan));
		}
	}

	// 父类继承来的组件（从 CDO 拿）。这些也要带挂载关系 ——
	// 上一版对它们**连 attach_to 都不设**，调用方拿到的是「字段不存在」。
	if (Blueprint->GeneratedClass)
	{
		if (UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject())
		{
			TArray<UObject*> SubObjects;
			GetObjectsWithOuter(CDO, SubObjects, false);

			for (UObject* SubObj : SubObjects)
			{
				UActorComponent* Component = Cast<UActorComponent>(SubObj);
				if (!Component) continue;

				const FString CompName = Component->GetName();
				if (AddedNames.Contains(CompName)) continue;

				TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
				CompObj->SetStringField(TEXT("name"), CompName);
				CompObj->SetStringField(TEXT("class"), Component->GetClass()->GetName());
				CompObj->SetStringField(TEXT("class_path"), Component->GetClass()->GetPathName());
				CompObj->SetStringField(TEXT("source"), TEXT("inherited"));
				// 继承的组件改不了挂载关系，如实标出来 ——
				// 否则调用方会尝试去挂它然后拿到一句语焉不详的失败
				CompObj->SetBoolField(TEXT("editable"), false);

				// 场景组件才有父子关系；非场景组件（如 MovementComponent）本来就没有
				FString ParentName;
				if (const USceneComponent* SceneComp = Cast<USceneComponent>(Component))
				{
					if (const USceneComponent* AttachParent = SceneComp->GetAttachParent())
					{
						ParentName = AttachParent->GetName();
					}
				}
				CompObj->SetStringField(TEXT("attach_to"), ParentName);

				ComponentsArray.Add(MakeShared<FJsonValueObject>(CompObj));
				AddedNames.Add(CompName);
			}
		}
	}

	return ComponentsArray;
}

/**
 * 收集蓝图的变量列表
 */
TArray<TSharedPtr<FJsonValue>> FUAL_BlueprintCommands::CollectVariablesInfo(UBlueprint* Blueprint)
{
	TArray<TSharedPtr<FJsonValue>> VariablesArray;
	if (!Blueprint) return VariablesArray;

	// 遍历 NewVariables（蓝图定义的变量）
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), Var.VarName.ToString());
		VarObj->SetStringField(TEXT("type"), Var.VarType.PinCategory.ToString());
		// 没有这两个字段，MeshComponent 和 MeshComponent[] 读出来一模一样
		VarObj->SetBoolField(TEXT("is_array"), Var.VarType.ContainerType == EPinContainerType::Array);
		if (const TCHAR* ContainerName = UAL_ContainerName(Var.VarType))
		{
			VarObj->SetStringField(TEXT("container"), ContainerName);
		}
		if (const UObject* Sub = Var.VarType.PinSubCategoryObject.Get())
		{
			VarObj->SetStringField(TEXT("sub_category_object"), Sub->GetPathName());
		}
		// 以前这里写死 true。按细节面板那个勾算：有 Edit 且没有 DisableEditOnInstance
		// （和 UAL_BuildVariableMetaJson 的 instance_editable 同一口径）
		VarObj->SetBoolField(TEXT("editable"),
			(Var.PropertyFlags & CPF_Edit) != 0 && (Var.PropertyFlags & CPF_DisableEditOnInstance) == 0);
		
		// 尝试获取默认值
		if (!Var.DefaultValue.IsEmpty())
		{
			VarObj->SetStringField(TEXT("default_value"), Var.DefaultValue);
		}
		
		VariablesArray.Add(MakeShared<FJsonValueObject>(VarObj));
	}

	return VariablesArray;
}

/**
 * 构建蓝图结构 JSON 对象
 * 被 describe、create、add_component 复用
 */
TSharedPtr<FJsonObject> FUAL_BlueprintCommands::BuildBlueprintStructureJson(
	UBlueprint* Blueprint, 
	bool bIncludeVariables, 
	bool bIncludeComponentDetails)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!Blueprint)
	{
		Result->SetBoolField(TEXT("ok"), false);
		Result->SetStringField(TEXT("error"), TEXT("Invalid blueprint"));
		return Result;
	}

	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("name"), Blueprint->GetName());
	Result->SetStringField(TEXT("path"), Blueprint->GetPathName());
	
	// 父类信息
	if (Blueprint->ParentClass)
	{
		Result->SetStringField(TEXT("parent_class"), Blueprint->ParentClass->GetName());
		Result->SetStringField(TEXT("parent_class_path"), Blueprint->ParentClass->GetPathName());
	}
	
	// 生成的类
	if (Blueprint->GeneratedClass)
	{
		Result->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass->GetPathName());
	}

	// 组件列表
	Result->SetArrayField(TEXT("components"), CollectComponentsInfo(Blueprint));
	
	// 变量列表（可选）
	if (bIncludeVariables)
	{
		Result->SetArrayField(TEXT("variables"), CollectVariablesInfo(Blueprint));
	}

	const FUAL_GraphDiscoveryResult GraphDiscovery = UAL_CollectBlueprintGraphs(Blueprint);
	Result->SetNumberField(TEXT("graph_count"), GraphDiscovery.Graphs.Num());
	Result->SetArrayField(TEXT("graph_names"), GraphDiscovery.GraphNames);
	Result->SetArrayField(TEXT("graphs"), GraphDiscovery.Graphs);
	Result->SetArrayField(TEXT("event_graphs"), GraphDiscovery.EventGraphs);
	Result->SetArrayField(TEXT("function_graphs"), GraphDiscovery.FunctionGraphs);
	Result->SetArrayField(TEXT("macro_graphs"), GraphDiscovery.MacroGraphs);
	Result->SetArrayField(TEXT("delegate_graphs"), GraphDiscovery.DelegateGraphs);
	Result->SetArrayField(TEXT("intermediate_graphs"), GraphDiscovery.IntermediateGraphs);
	Result->SetBoolField(TEXT("has_event_graph"), GraphDiscovery.EventGraphs.Num() > 0);
	if (!GraphDiscovery.PreferredGraphName.IsEmpty())
	{
		Result->SetStringField(TEXT("preferred_graph"), GraphDiscovery.PreferredGraphName);
	}
	
	// 编译状态。BS_UpToDateWithWarnings 要单列，否则 describe 报 "Other"，
	// 调用方会把一个只是带警告的正常蓝图当成状态异常（同 Handle_CompileBlueprint）
	FString StatusStr;
	switch (Blueprint->Status)
	{
	case BS_UpToDate:             StatusStr = TEXT("UpToDate"); break;
	case BS_UpToDateWithWarnings: StatusStr = TEXT("UpToDateWithWarnings"); break;
	case BS_Dirty:                StatusStr = TEXT("Dirty"); break;
	case BS_Error:                StatusStr = TEXT("Error"); break;
	case BS_Unknown:              StatusStr = TEXT("Unknown"); break;
	default:                      StatusStr = TEXT("Other"); break;
	}
	Result->SetStringField(TEXT("compile_status"), StatusStr);

	return Result;
}

// ============================================================================
// blueprint.describe 命令处理
// ============================================================================

/**
 * 获取蓝图完整结构信息
 *
 * 请求参数:
 *   - blueprint_path: 蓝图路径（必填）
 *
 * 响应:
 *   - ok: 是否成功
 *   - name: 蓝图名称
 *   - path: 蓝图完整路径
 *   - parent_class: 父类名称
 *   - parent_class_path: 父类完整路径
 *   - components: 组件列表（包含名称、类型、来源等）
 *   - variables: 变量列表（包含名称、类型、默认值等）
 *   - compile_status: 编译状态
 */
void FUAL_BlueprintCommands::Handle_DescribeBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析参数（兼容 blueprint_path 和 blueprint_name）
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		// 尝试 blueprint_name 作为备选
		if (!Payload->TryGetStringField(TEXT("blueprint_name"), BlueprintPath) || BlueprintPath.IsEmpty())
		{
			UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path (or blueprint_name). Example: {\"blueprint_path\": \"/Game/Blueprints/BP_Hero\"}"));
			return;
		}
	}

	// 2. 加载蓝图
	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath = BlueprintPath;

	if (BlueprintPath.StartsWith(TEXT("/")))
	{
		if (!BlueprintPath.Contains(TEXT(".")))
		{
			ResolvedPath = BlueprintPath + TEXT(".") + FPaths::GetBaseFilename(BlueprintPath);
		}
		Blueprint = LoadObject<UBlueprint>(nullptr, *ResolvedPath);
	}

	// 如果直接加载失败，通过 AssetRegistry 查找
	if (!Blueprint)
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		TArray<FAssetData> AssetList;
		FARFilter Filter;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
#else
		Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
#endif
		Filter.bRecursiveClasses = true;
		AssetRegistry.GetAssets(Filter, AssetList);

		for (const FAssetData& Asset : AssetList)
		{
			FString AssetName = Asset.AssetName.ToString();
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			FString AssetPath = Asset.GetObjectPathString();
#else
			FString AssetPath = Asset.ObjectPath.ToString();
#endif
			if (AssetName.Equals(BlueprintPath, ESearchCase::IgnoreCase) ||
				AssetPath.Contains(BlueprintPath))
			{
				Blueprint = Cast<UBlueprint>(Asset.GetAsset());
				ResolvedPath = AssetPath;
				break;
			}
		}
	}

	if (!Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	// 3. 构建并返回结构信息
	TSharedPtr<FJsonObject> Result = BuildBlueprintStructureJson(Blueprint, true, false);
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

void FUAL_BlueprintCommands::Handle_DeleteNode(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: blueprint_path"));
		return;
	}

	FString NodeId;
	if (!Payload->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: node_id"));
		return;
	}

	FString GraphName;
	Payload->TryGetStringField(TEXT("graph_name"), GraphName);

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath))
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	UEdGraph* Graph = UAL_FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		return;
	}

	UEdGraphNode* Node = UAL_FindNodeByGuid(Graph, NodeId);
	if (!Node)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Node not found with ID: %s"), *NodeId));
		return;
	}

	// 记录节点信息用于返回
	FString NodeClass = Node->GetClass()->GetName();
	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

	// 删除是不可逆操作里最该能撤回的一种，进事务
	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALBlueprint", "DeleteNode", "Delete Blueprint Node"));

	Blueprint->Modify();
	Graph->Modify();

	// 使用编辑器工具删除节点（会自动处理连线断开等）
	FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_path"), ResolvedPath);
	Result->SetStringField(TEXT("graph_name"), Graph->GetName());
	Result->SetStringField(TEXT("node_id"), NodeId); // Return the ID of the deleted node
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Deleted node %s (%s)"), *NodeTitle, *NodeClass));
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/** 定义在下面 create_graph 那一段里 —— 「引脚找不到」的报错两处共用同一份 */
static FString UAL_ListPins(const UEdGraphNode* Node);

void FUAL_BlueprintCommands::Handle_DisconnectBlueprintPins(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath, NodeId, PinName, GraphName, OtherNodeId, OtherPinName;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: blueprint_path"));
		return;
	}
	if (!Payload->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: node_id"));
		return;
	}
	if (!Payload->TryGetStringField(TEXT("pin"), PinName) || PinName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: pin"));
		return;
	}
	Payload->TryGetStringField(TEXT("graph_name"), GraphName);
	Payload->TryGetStringField(TEXT("other_node_id"), OtherNodeId);
	Payload->TryGetStringField(TEXT("other_pin"), OtherPinName);

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath))
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	UEdGraph* Graph = UAL_FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		return;
	}

	UEdGraphNode* Node = UAL_FindNodeByGuid(Graph, NodeId);
	if (!Node)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Node not found with ID: %s"), *NodeId));
		return;
	}

	UEdGraphPin* Pin = UAL_FindPinByName(Node, PinName);
	if (!Pin)
	{
		// 引脚名写错是这条命令最常见的失败，把真名一起回去，省一轮 get_graph
		UAL_CommandUtils::SendError(
			RequestId, 404,
			FString::Printf(TEXT("Pin '%s' not found on node %s (available: %s)"),
				*PinName, *NodeId, *UAL_ListPins(Node)));
		return;
	}

	// 两个 other_* 要么都给要么都不给：只给一半多半是调用方以为自己在指定「断哪一根」，
	// 而我们却断了整个引脚 —— 那是静默做了比它要求更多的事
	const bool bWantsSingle = !OtherNodeId.IsEmpty() || !OtherPinName.IsEmpty();
	UEdGraphPin* OtherPin = nullptr;
	if (bWantsSingle)
	{
		if (OtherNodeId.IsEmpty() || OtherPinName.IsEmpty())
		{
			UAL_CommandUtils::SendError(
				RequestId, 400,
				TEXT("other_node_id and other_pin must be given together (omit both to break every link on this pin)"));
			return;
		}
		UEdGraphNode* OtherNode = UAL_FindNodeByGuid(Graph, OtherNodeId);
		if (!OtherNode)
		{
			UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Node not found with ID: %s"), *OtherNodeId));
			return;
		}
		OtherPin = UAL_FindPinByName(OtherNode, OtherPinName);
		if (!OtherPin)
		{
			UAL_CommandUtils::SendError(
				RequestId, 404,
				FString::Printf(TEXT("Pin '%s' not found on node %s (available: %s)"),
					*OtherPinName, *OtherNodeId, *UAL_ListPins(OtherNode)));
			return;
		}
	}

	const int32 Before = Pin->LinkedTo.Num();

	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALBlueprint", "DisconnectPins", "Disconnect Blueprint Pins"));
	Blueprint->Modify();
	Graph->Modify();

	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	if (OtherPin)
	{
		K2Schema->BreakSinglePinLink(Pin, OtherPin);
	}
	else
	{
		K2Schema->BreakPinLinks(*Pin, /*bSendsNodeNotifcation=*/true);
	}

	// 结构性的：断线可能让下游的通配引脚重新变回未定型，骨架要跟着重编
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_path"), ResolvedPath);
	Result->SetStringField(TEXT("graph_name"), Graph->GetName());
	Result->SetNumberField(TEXT("broken"), Before - Pin->LinkedTo.Num());
	Result->SetNumberField(TEXT("remaining"), Pin->LinkedTo.Num());
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// blueprint.create_graph — 声明式整图写入（全有或全无）
// ============================================================================

/**
 * 从一条 JSON 节点定义里解析出 `FUAL_NodeSpec`。
 *
 * 字段名同时认两套：`class`/`type`、`member_name`/`name`。调用方的 schema 用
 * 前者，插件历史用后者，两边都别再为这个来回改。
 */
static FUAL_NodeSpec UAL_ParseNodeSpec(const TSharedPtr<FJsonObject>& NodeObj)
{
	FUAL_NodeSpec Spec;

	if (!NodeObj->TryGetStringField(TEXT("class"), Spec.Type))
	{
		NodeObj->TryGetStringField(TEXT("type"), Spec.Type);
	}
	if (!NodeObj->TryGetStringField(TEXT("member_name"), Spec.Name))
	{
		NodeObj->TryGetStringField(TEXT("name"), Spec.Name);
	}
	NodeObj->TryGetStringField(TEXT("target_class"), Spec.TargetClass);
	NodeObj->TryGetStringField(TEXT("struct_type"), Spec.StructType);
	NodeObj->TryGetStringField(TEXT("macro_lib"), Spec.MacroLib);
	NodeObj->TryGetStringField(TEXT("raw_class"), Spec.RawClass);

	// CustomEvent 的参数表：`params` 和 `inputs` 两个名字都认。
	// 前者贴着「事件参数」的说法，后者和 blueprint.create_function 的字段一致 ——
	// 调用方两种都会写，只认一个就等于另一个被静默吃掉，
	// 而「静默吃掉」正是这个字段当初被反馈上来的原因。
	const TArray<TSharedPtr<FJsonValue>>* ParamsArrayPtr = nullptr;
	if (NodeObj->TryGetArrayField(TEXT("params"), ParamsArrayPtr) && ParamsArrayPtr)
	{
		Spec.EventParams = *ParamsArrayPtr;
	}
	else if (NodeObj->TryGetArrayField(TEXT("inputs"), ParamsArrayPtr) && ParamsArrayPtr)
	{
		Spec.EventParams = *ParamsArrayPtr;
	}

	NodeObj->TryGetStringField(TEXT("replication"), Spec.EventReplication);
	Spec.bHasEventReliable = NodeObj->TryGetBoolField(TEXT("reliable"), Spec.bEventReliable);

	int32 Tmp = 0;
	if (NodeObj->TryGetNumberField(TEXT("first_index"), Tmp))
	{
		Spec.bHasFirstIndex = true;
		Spec.FirstIndex = Tmp;
	}
	if (NodeObj->TryGetNumberField(TEXT("last_index"), Tmp))
	{
		Spec.bHasLastIndex = true;
		Spec.LastIndex = Tmp;
	}

	const TSharedPtr<FJsonObject>* PosObjPtr = nullptr;
	if (NodeObj->TryGetObjectField(TEXT("position"), PosObjPtr) && PosObjPtr && (*PosObjPtr).IsValid())
	{
		(*PosObjPtr)->TryGetNumberField(TEXT("x"), Spec.PosX);
		(*PosObjPtr)->TryGetNumberField(TEXT("y"), Spec.PosY);
		// 字段在就算给了，哪怕值是 (0,0)
		Spec.bHasPosition = true;
	}

	// timeline: { length?, length_mode?, loop?, autoplay?,
	//             tracks?: [{ name, keys: [{ time, value, interp?, arrive_tangent?, leave_tangent? }] }] }
	const TSharedPtr<FJsonObject>* TimelineObjPtr = nullptr;
	if (NodeObj->TryGetObjectField(TEXT("timeline"), TimelineObjPtr) && TimelineObjPtr && (*TimelineObjPtr).IsValid())
	{
		const TSharedPtr<FJsonObject>& Timeline = *TimelineObjPtr;
		double Length = 0.0;
		if (Timeline->TryGetNumberField(TEXT("length"), Length))
		{
			Spec.bHasTimelineLength = true;
			Spec.TimelineLength = static_cast<float>(Length);
		}
		Timeline->TryGetStringField(TEXT("length_mode"), Spec.TimelineLengthMode);
		bool bFlag = false;
		if (Timeline->TryGetBoolField(TEXT("loop"), bFlag))
		{
			Spec.bHasTimelineLoop = true;
			Spec.bTimelineLoop = bFlag;
		}
		if (Timeline->TryGetBoolField(TEXT("autoplay"), bFlag))
		{
			Spec.bHasTimelineAutoPlay = true;
			Spec.bTimelineAutoPlay = bFlag;
		}

		const TArray<TSharedPtr<FJsonValue>>* TracksArray = nullptr;
		if (Timeline->TryGetArrayField(TEXT("tracks"), TracksArray) && TracksArray)
		{
			for (const TSharedPtr<FJsonValue>& TrackVal : *TracksArray)
			{
				const TSharedPtr<FJsonObject>* TrackObj = nullptr;
				if (!TrackVal.IsValid() || !TrackVal->TryGetObject(TrackObj) || !TrackObj || !(*TrackObj).IsValid())
				{
					// 占个空名进去，让创建那头报「轨道没名字」，而不是静默少一条轨道
					Spec.TimelineTracks.AddDefaulted();
					continue;
				}
				FUAL_TimelineTrackSpec Track;
				(*TrackObj)->TryGetStringField(TEXT("name"), Track.Name);

				const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
				if ((*TrackObj)->TryGetArrayField(TEXT("keys"), KeysArray) && KeysArray)
				{
					for (const TSharedPtr<FJsonValue>& KeyVal : *KeysArray)
					{
						const TSharedPtr<FJsonObject>* KeyObj = nullptr;
						if (!KeyVal.IsValid() || !KeyVal->TryGetObject(KeyObj) || !KeyObj || !(*KeyObj).IsValid())
						{
							continue;
						}
						double Time = 0.0, Value = 0.0;
						(*KeyObj)->TryGetNumberField(TEXT("time"), Time);
						(*KeyObj)->TryGetNumberField(TEXT("value"), Value);

						FUAL_TimelineKeySpec Key;
						Key.Time = static_cast<float>(Time);
						Key.Value = static_cast<float>(Value);
						(*KeyObj)->TryGetStringField(TEXT("interp"), Key.Interp);

						// 两根切线要么都给要么都不给：只给一根等于让引擎拿另一根的
						// 自动值去配一根手填的，画出来是谁都没要过的形状
						double Arrive = 0.0, Leave = 0.0;
						if ((*KeyObj)->TryGetNumberField(TEXT("arrive_tangent"), Arrive)
							&& (*KeyObj)->TryGetNumberField(TEXT("leave_tangent"), Leave))
						{
							Key.bHasTangents = true;
							Key.ArriveTangent = static_cast<float>(Arrive);
							Key.LeaveTangent = static_cast<float>(Leave);
						}
						Track.Keys.Add(MoveTemp(Key));
					}
				}
				Spec.TimelineTracks.Add(MoveTemp(Track));
			}
		}
	}

	return Spec;
}

/**
 * 按引脚的**真实类型**写默认值，并把引擎的校验结果原样带出来。
 *
 * ## 为什么不能直接写 Pin->DefaultValue
 *
 * 上一版是自己按 PinCategory 分流：object 类写 DefaultObject、text 类写
 * DefaultTextValue、其余写 DefaultValue。三件事都错了：
 *
 * 1. **少了 `Node->PinDefaultValueChanged(&Pin)`。** 这个回调才是关键 ——
 *    SpawnActor 的 Class 引脚一变，节点要靠它重新长出「暴露的属性」那批引脚；
 *    Cast 的目标类一变，输出引脚类型要靠它更新。直接赋值这些全都不会发生，
 *    于是调用方紧接着要连的那些引脚**根本不存在**，而报错停在「pin not found」，
 *    离真正的原因隔了一层。
 * 2. **少了校验。** 写一个解析不了的值进去，编译期不报错，运行时是个空引用。
 * 3. **分流规则是抄的，不是引擎的。** 引擎自己用 `GetPinDefaultValuesFromString`
 *    按 PinType 拆成三个槽位，规则比「看 category」细（软引用、枚举、结构体
 *    字面量都有各自的走法）。
 *
 * 所以这里改成：用引擎的解析器拆值 → 用引擎的校验器验 → 用引擎的
 * `TrySetDefaultValue` 落值。`TrySetDefaultValue` 自己是 void 且校验失败时
 * **静默不做事**，所以必须先手动验一次才能拿到错误原因。
 */
static bool UAL_SetPinDefault(
	const UEdGraphSchema_K2* K2Schema,
	UEdGraphPin* Pin,
	const FString& Value,
	FString& OutError)
{
	if (!Pin || !K2Schema)
	{
		OutError = TEXT("null pin or schema");
		return false;
	}

	// 已连线的引脚上设默认值不会报错，但那个值永远不会生效 —— 静默无效比报错难查
	if (Pin->LinkedTo.Num() > 0)
	{
		OutError = FString::Printf(
			TEXT("pin '%s' is connected, so a default value would never be used"),
			*Pin->PinName.ToString());
		return false;
	}

	FString UseDefaultValue;
	TObjectPtr<UObject> UseDefaultObject = nullptr;
	FText UseDefaultText;
	K2Schema->GetPinDefaultValuesFromString(
		Pin->PinType,
		Pin->GetOwningNodeUnchecked(),
		Value,
		UseDefaultValue,
		UseDefaultObject,
		UseDefaultText,
		/*bPreserveTextIdentity=*/false);

	const FString ValidationError = K2Schema->IsPinDefaultValid(Pin, UseDefaultValue, UseDefaultObject, UseDefaultText);
	if (!ValidationError.IsEmpty())
	{
		OutError = FString::Printf(
			TEXT("pin '%s' rejected value '%s': %s"),
			*Pin->PinName.ToString(), *Value, *ValidationError);
		return false;
	}

	// 走引擎的落值路径，`PinDefaultValueChanged` 等回调才会触发
	K2Schema->TrySetDefaultValue(*Pin, Value);
	return true;
}

/** `Then(Output)` —— 报错时列可用引脚用，方向必须带上，同名不同向很常见 */
static FString UAL_DescribePinBrief(const UEdGraphPin* Pin)
{
	return FString::Printf(TEXT("%s(%s)"), *Pin->PinName.ToString(), *UAL_PinDirToString(Pin->Direction));
}

/** 把一个节点上的引脚列成一行，给「引脚找不到」的报错用 */
static FString UAL_ListPins(const UEdGraphNode* Node)
{
	TArray<FString> Names;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && !Pin->bHidden)
		{
			Names.Add(UAL_DescribePinBrief(Pin));
		}
	}
	return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : TEXT("(none)");
}

/** 一条待建的连线，validate 阶段解析好，apply 阶段直接用 */
struct FUAL_PendingConnection
{
	int32 Index = 0;
	FString FromLabel;
	FString ToLabel;
	UEdGraphPin* FromPin = nullptr;
	UEdGraphPin* ToPin = nullptr;
	// 这里**不**存 CanCreateConnection 的判定：校验那一轮的结论到应用时可能已经过期
	// （前面的线会给通配引脚定型），副作用警告一律在应用前现查一次

	/**
	 * 校验时两端都还是 wildcard，被引擎拒了 —— 留到最后再连。
	 *
	 * 这一批里的类型传播在校验阶段**看不到**：ForEach 的 Array Element、
	 * 数组函数的 TargetArray 是在上游引脚**真的连上**之后，由引擎的
	 * `NotifyPinConnectionListChanged` 定型的，而校验跑在一根线都没连的时候。
	 * 于是「gaoc.OutActors → foreach.Array」和「foreach.Array Element → cast.Object」
	 * 写在同一批里，后者必败 —— 图其实是对的。
	 *
	 * 所以两端都是 wildcard 的 DISALLOW 不当错误，排到别的线都连完之后重试一次。
	 * 那时上游已经定型，引擎给的是真正的答案。仍然不行才算错，整批照常回滚。
	 */
	bool bDeferred = false;
};

/** 两端都还没定型 —— 这种拒绝多半只是顺序问题，值得等别的线连完再试一次 */
static bool UAL_IsUntypedWildcardPair(const UEdGraphPin* A, const UEdGraphPin* B)
{
	return A && B
		&& A->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard
		&& B->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard;
}

/**
 * 声明式整图写入。
 *
 * ## 真正的「全有或全无」是怎么做到的
 *
 * 上一版靠 `FScopedTransaction::Cancel()` 回滚 —— **那是错的**。读引擎源码
 * （UnrealEd/Private/EditorTransaction.cpp，`UTransBuffer::Cancel`）：它把
 * 事务从 undo 栈里弹掉丢弃，**从不调用 `Apply()`**。也就是说改动照样留在图里，
 * 只是连 Ctrl+Z 都撤不回来了 —— 比不用事务还糟。
 *
 * 引擎自己用 Cancel 的地方（BlueprintEditor.cpp:10422）也印证了这一点：
 * 只在「一个节点都没建」时调用，用来避免往 undo 栈里塞一条空记录。
 *
 * 所以这里的原子性靠**显式回退**：建过的节点全记下来，出错就逐个
 * `RemoveNode` 删回去，然后再 Cancel（此时净改动确实为零，语义与引擎一致）。
 *
 * 顺序也是为这个服务的：
 *
 *   1. 建节点（可回退：删掉即可）
 *   2. 写引脚默认值（可回退：随节点一起删）
 *   3. **只校验不连线**（`CanCreateConnection`，零改动）
 *   4. —— 决策点：有错就回退，图回到调用前 ——
 *   5. 真正连线
 *   6. `clear_existing` 的删除放在这里，不在最前面 ——
 *      放前面的话，后面任何一步失败都恢复不了被删的节点
 *
 * ## 事务不是永远可用的
 *
 * `FScopedTransaction` 在 `GEditor` 缺席 / `CanTransact()` 为假 / 已在事务中时
 * 整个是空操作（ScopedTransaction.cpp:19），且**不报错**。这种情况下用户
 * 撤不了这次改动，所以响应里如实回一个 `undoable`，不假装。
 */
void FUAL_BlueprintCommands::Handle_CreateGraphDeclarative(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// ===== 1. 必填参数 =====
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	/**
	 * `nodes` 可以是空数组 —— 那是「两个已有节点之间补一根线」这一种用法。
	 *
	 * 在此之前空数组被直接顶回（`must not have fewer than 1 items`），于是补一根线
	 * 得先塞一个用不上的节点陪跑、连完再删，两次调用做一件事。
	 *
	 * **但字段本身仍是必填的**：完全不写 nodes 多半是漏了，不是想补线。
	 */
	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (!Payload->TryGetArrayField(TEXT("nodes"), NodesArray) || !NodesArray)
	{
		UAL_CommandUtils::SendError(
			RequestId, 400,
			TEXT("Missing required field: nodes. Send [] to only add connections between existing nodes - ")
			TEXT("but then reference them by their node GUID (from blueprint.get_graph's node_id, or the ")
			TEXT("created_nodes[].node_id of the last successful apply), NOT by labels from a previous call: ")
			TEXT("labels are remembered only within a single request."));
		return;
	}

	const int32 MaxBatch = UAL_CommandUtils::GetMaxBatchCreate();
	if (MaxBatch > 0 && NodesArray->Num() > MaxBatch)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("field"), TEXT("nodes"));
		Details->SetNumberField(TEXT("count"), NodesArray->Num());
		Details->SetNumberField(TEXT("max"), MaxBatch);
		Details->SetStringField(TEXT("cvar"), TEXT("ual.MaxBatchCreate"));
		UAL_CommandUtils::SendError(
			RequestId, 400,
			FString::Printf(TEXT("Batch size %d exceeds limit %d"), NodesArray->Num(), MaxBatch),
			Details);
		return;
	}

	// ===== 2. 可选参数 =====
	FString GraphName;
	Payload->TryGetStringField(TEXT("graph_name"), GraphName);

	bool bClearExisting = false;
	Payload->TryGetBoolField(TEXT("clear_existing"), bClearExisting);

	// 空 nodes + clear_existing = 把整张图清空、什么都不建。
	// 这两个参数各自都合理，凑在一起几乎一定是失误，而代价是一整张图
	if (bClearExisting && NodesArray->Num() == 0)
	{
		UAL_CommandUtils::SendError(
			RequestId, 400,
			TEXT("clear_existing with an empty nodes array would wipe the whole graph and build nothing. ")
			TEXT("Send the nodes you want to keep, or drop clear_existing if you only meant to add connections."));
		return;
	}

	/**
	 * `require_empty` —— 目标图必须是空的，否则一个节点都不建。
	 *
	 * 调用方（盒子的「片段放进工程」）在发这条命令之前会先 get_graph 探一次，
	 * 但那是**两次独立的远程调用**：探完到写入之间，用户可能在编辑器里加了东西，
	 * 另一个任务也可能在写同一张图。那道缝在调用方那边关不上 —— 检查必须
	 * 和写入在同一条命令里，也就是这里。
	 *
	 * 不加这个的后果不是「多几个节点」：
	 *   - 同名事件节点会被**复用**（见 UAL_CreateNodeInternal 里的 Event 分支），
	 *     片段逻辑会挂到用户原有的 BeginPlay 下面；
	 *   - 连线会顶掉引脚上已有的连线（CONNECT_RESPONSE_BREAK_OTHERS_*）；
	 *   - 事务未必可用（见函数注释的 undoable），撤销兜不了底。
	 *
	 * 默认 false 是为了不改既有调用方的行为。
	 */
	bool bRequireEmpty = false;
	Payload->TryGetBoolField(TEXT("require_empty"), bRequireEmpty);

	bool bCompile = true;
	Payload->TryGetBoolField(TEXT("compile"), bCompile);

	const TArray<TSharedPtr<FJsonValue>>* ConnectionsArray = nullptr;
	Payload->TryGetArrayField(TEXT("connections"), ConnectionsArray);

	// ===== 3. 加载蓝图与图表 =====
	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	UEdGraph* Graph = UAL_FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(
			RequestId, 404,
			FString::Printf(TEXT("Graph not found: %s. This command does not create graphs - use blueprint.create_function first."),
				GraphName.IsEmpty() ? TEXT("EventGraph") : *GraphName));
		return;
	}

	const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
	if (!K2Schema)
	{
		UAL_CommandUtils::SendError(
			RequestId, 400,
			FString::Printf(TEXT("Graph '%s' is not a K2 (Blueprint) graph"), *Graph->GetName()));
		return;
	}

	// 编译中的蓝图不能改图 —— 改了会和编译器抢同一份数据
	if (Blueprint->bBeingCompiled)
	{
		UAL_CommandUtils::SendError(
			RequestId, 409,
			TEXT("Blueprint is currently compiling; retry in a moment"));
		return;
	}

	// require_empty 的检查放在**进事务之前**：不空就直接返回，
	// 连事务都不开，图上一个字节都没动过。
	if (bRequireEmpty)
	{
		FString NotEmptyReason;
		if (!UAL_IsGraphEmptyForImport(Graph, NotEmptyReason))
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("graph_name"), Graph->GetName());
			Details->SetStringField(TEXT("reason"), NotEmptyReason);
			UAL_CommandUtils::SendError(
				RequestId, 409,
				FString::Printf(
					TEXT("require_empty: graph '%s' is not empty - %s. Nothing was written."),
					*Graph->GetName(), *NotEmptyReason),
				Details);
			return;
		}
	}

	// ===== 4. 进事务 =====
	// 事务只负责「让用户能一次 Ctrl+Z 撤销整批」。原子性**不靠它**（见函数注释）。
	//
	// 用 TOptional 装是为了能在**编译之前**提前结束这一笔 —— 理由见下面
	// 「编译必须在事务之外」那一段。
	TOptional<FUAL_ScopedTransaction> Transaction;
	Transaction.Emplace(NSLOCTEXT("UnrealAgentLink", "ApplyBlueprintGraph", "Apply Blueprint Graph"));
	const bool bUndoable = Transaction->IsOutstanding();

	Blueprint->Modify();
	Graph->Modify();

	TArray<FString> Errors;
	TArray<FString> Warnings;

	// clear_existing 要删的那批，先记下来，**真的删在最后**
	TArray<UEdGraphNode*> PreExistingNodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node)
		{
			PreExistingNodes.Add(Node);
		}
	}

	// ===== 5. 建节点（全部记下来，出错要逐个删回去）=====
	TMap<FString, UEdGraphNode*> NodeIdMap;
	TArray<UEdGraphNode*> CreatedNodes;
	TArray<TSharedPtr<FJsonValue>> CreatedNodesInfo;
	/** 动过的 Timeline 模板，出错时照它们回填（模板不随节点删除而复原，见 FUAL_TimelineSnapshot） */
	TArray<FUAL_TimelineSnapshot> TimelineSnapshots;

	// 没给坐标时的兜底排布。正式布局由 app 侧算好后当 position 传进来，
	// 这里只保证「没算过的图不会全叠在原点」。
	int32 NextPosX = 0;
	int32 NextPosY = 0;
	const int32 StepX = 350;
	const int32 StepY = 200;

	for (int32 i = 0; i < NodesArray->Num(); ++i)
	{
		const TSharedPtr<FJsonValue>& NodeVal = (*NodesArray)[i];
		const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
		if (!NodeVal.IsValid() || !NodeVal->TryGetObject(NodeObjPtr) || !NodeObjPtr || !(*NodeObjPtr).IsValid())
		{
			Errors.Add(FString::Printf(TEXT("nodes[%d]: not an object"), i));
			continue;
		}
		const TSharedPtr<FJsonObject>& NodeObj = *NodeObjPtr;

		FString NodeId;
		NodeObj->TryGetStringField(TEXT("id"), NodeId);
		if (NodeId.IsEmpty())
		{
			NodeId = FString::Printf(TEXT("node_%d"), i);
		}
		if (NodeIdMap.Contains(NodeId))
		{
			// 重复 id 会让连线静默连到错的节点上 —— 宁可整批失败
			Errors.Add(FString::Printf(TEXT("nodes[%d]: duplicate id '%s'"), i, *NodeId));
			continue;
		}

		FUAL_NodeSpec Spec = UAL_ParseNodeSpec(NodeObj);
		if (Spec.Type.IsEmpty())
		{
			Errors.Add(FString::Printf(TEXT("nodes[%d] '%s': missing class/type"), i, *NodeId));
			continue;
		}

		if (!Spec.bHasPosition)
		{
			Spec.PosX = NextPosX;
			Spec.PosY = NextPosY;
			NextPosX += StepX;
			if (NextPosX > StepX * 4)
			{
				NextPosX = 0;
				NextPosY += StepY;
			}
		}

		FString CreateError;
		bool bReused = false;
		// 快照先进数组再传引用：Timeline 之外的节点不会碰它，留在数组里
		// 也只是一条 Template==nullptr 的空记录，回退时直接跳过
		FUAL_TimelineSnapshot& Snapshot = TimelineSnapshots.AddDefaulted_GetRef();
		UEdGraphNode* NewNode = UAL_CreateNodeInternal(Blueprint, Graph, Spec, bReused, CreateError, &Snapshot);
		if (!NewNode)
		{
			Errors.Add(FString::Printf(TEXT("nodes[%d] '%s': %s"), i, *NodeId, *CreateError));
			continue;
		}

		// 复用的节点（图里本来就有的 BeginPlay 之类）**不能记进回退名单** ——
		// 回退时把它删掉等于顺手毁了用户原有的图
		if (!bReused)
		{
			CreatedNodes.Add(NewNode);
		}

		NodeIdMap.Add(NodeId, NewNode);

		TSharedPtr<FJsonObject> NodeInfo = MakeShared<FJsonObject>();
		NodeInfo->SetStringField(TEXT("id"), NodeId);
		NodeInfo->SetStringField(TEXT("node_id"), UAL_GuidToString(NewNode->NodeGuid));
		NodeInfo->SetStringField(TEXT("class"), NewNode->GetClass()->GetName());
		if (bReused)
		{
			NodeInfo->SetBoolField(TEXT("reused"), true);
		}
		// pins 不在这里填：此刻默认值还没写、线还没连、也还没编译，
		// 这时的引脚快照全是「空的初始态」。回执末尾按 GUID 重新找节点再填
		CreatedNodesInfo.Add(MakeShared<FJsonValueObject>(NodeInfo));
	}

	// ===== 6. 引脚默认值 =====
	// 必须排在连线校验**之前**：有些节点会因为默认值变化重新长引脚
	// （SpawnActor 设了 Class 才会有那批暴露出来的属性引脚）。
	// 顺序反了的话，要连的引脚在校验时还不存在。
	for (int32 i = 0; i < NodesArray->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
		if (!(*NodesArray)[i].IsValid() || !(*NodesArray)[i]->TryGetObject(NodeObjPtr) || !NodeObjPtr)
		{
			continue;
		}
		const TSharedPtr<FJsonObject>& NodeObj = *NodeObjPtr;

		const TSharedPtr<FJsonObject>* PinDefaultsPtr = nullptr;
		if (!NodeObj->TryGetObjectField(TEXT("pin_defaults"), PinDefaultsPtr) || !PinDefaultsPtr || !(*PinDefaultsPtr).IsValid())
		{
			continue;
		}

		FString NodeId;
		NodeObj->TryGetStringField(TEXT("id"), NodeId);
		if (NodeId.IsEmpty())
		{
			NodeId = FString::Printf(TEXT("node_%d"), i);
		}
		UEdGraphNode** NodePtr = NodeIdMap.Find(NodeId);
		if (!NodePtr || !*NodePtr)
		{
			continue;  // 这个节点上一步就没建成，错误已经记过了
		}
		UEdGraphNode* Node = *NodePtr;
		Node->Modify();

		for (const auto& Pair : (*PinDefaultsPtr)->Values)
		{
			FString PinValue;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(PinValue))
			{
				Errors.Add(FString::Printf(TEXT("nodes '%s': pin_defaults['%s'] must be a string"), *NodeId, *Pair.Key));
				continue;
			}

			UEdGraphPin* Pin = UAL_FindPinByName(Node, UAL_JsonKey(Pair.Key));
			if (!Pin)
			{
				Errors.Add(FString::Printf(TEXT("nodes '%s': pin '%s' not found (available: %s)"),
					*NodeId, *Pair.Key, *UAL_ListPins(Node)));
				continue;
			}

			FString PinError;
			if (!UAL_SetPinDefault(K2Schema, Pin, PinValue, PinError))
			{
				Errors.Add(FString::Printf(TEXT("nodes '%s': %s"), *NodeId, *PinError));
			}
		}
	}

	// ===== 7. 连线：只校验，不改图 =====
	TArray<FUAL_PendingConnection> Pending;
	if (ConnectionsArray)
	{
		for (int32 i = 0; i < ConnectionsArray->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& ConnVal = (*ConnectionsArray)[i];
			if (!ConnVal.IsValid())
			{
				Errors.Add(FString::Printf(TEXT("connections[%d]: invalid entry"), i));
				continue;
			}

			// 两种写法都认：["a.Then","b.execute"] 与 {"from":..,"to":..}
			FString FromStr, ToStr;
			const TArray<TSharedPtr<FJsonValue>>* ConnArr = nullptr;
			const TSharedPtr<FJsonObject>* ConnObjPtr = nullptr;
			if (ConnVal->TryGetArray(ConnArr) && ConnArr && ConnArr->Num() >= 2)
			{
				FromStr = (*ConnArr)[0]->AsString();
				ToStr = (*ConnArr)[1]->AsString();
			}
			else if (ConnVal->TryGetObject(ConnObjPtr) && ConnObjPtr && (*ConnObjPtr).IsValid())
			{
				(*ConnObjPtr)->TryGetStringField(TEXT("from"), FromStr);
				(*ConnObjPtr)->TryGetStringField(TEXT("to"), ToStr);
			}

			if (FromStr.IsEmpty() || ToStr.IsEmpty())
			{
				Errors.Add(FString::Printf(
					TEXT("connections[%d]: expected {\"from\":\"nodeId.PinName\",\"to\":\"nodeId.PinName\"}"), i));
				continue;
			}

			// 引脚名里可能带点号，所以从右边切
			FString FromNodeId, FromPinName, ToNodeId, ToPinName;
			if (!FromStr.Split(TEXT("."), &FromNodeId, &FromPinName, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
				|| !ToStr.Split(TEXT("."), &ToNodeId, &ToPinName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
			{
				Errors.Add(FString::Printf(TEXT("connections[%d]: bad pin path, expected \"nodeId.PinName\""), i));
				continue;
			}

			/**
			 * 节点 id 先在本次请求里找，找不到再当成图里已有节点的 GUID 查。
			 *
			 * 这一步让「读回来改一改写回去」真正可用：往已有逻辑里插一个节点，
			 * 必须能连到图里原来就有的节点上。get_graph 回的就是这些 GUID，
			 * 直接抄过来即可。
			 */
			auto ResolveNode = [&](const FString& Id) -> UEdGraphNode*
			{
				if (UEdGraphNode** Found = NodeIdMap.Find(Id))
				{
					return *Found;
				}
				return UAL_FindNodeByGuid(Graph, Id);
			};

			UEdGraphNode* FromNode = ResolveNode(FromNodeId);
			UEdGraphNode* ToNode = ResolveNode(ToNodeId);
			if (!FromNode || !ToNode)
			{
				const FString& MissingId = FromNode ? ToNodeId : FromNodeId;
				Errors.Add(FString::Printf(
					TEXT("connections[%d]: '%s' is neither a node id defined in this request's nodes array nor an existing node GUID. ")
					TEXT("Fix: either add this node to 'nodes' with id \"%s\" (any label you like), or look up the existing node's ")
					TEXT("GUID with blueprint.get_graph and reference it as \"<GUID>.<PinName>\". Labels from a previous apply_graph call do not carry over."),
					i, *MissingId, *MissingId));
				continue;
			}

			UEdGraphPin* FromPin = UAL_FindPinByName(FromNode, FromPinName);
			UEdGraphPin* ToPin = UAL_FindPinByName(ToNode, ToPinName);
			if (!FromPin || !ToPin)
			{
				UEdGraphNode* MissingOn = FromPin ? ToNode : FromNode;
				const FString& MissingPin = FromPin ? ToPinName : FromPinName;
				const FString& MissingId = FromPin ? ToNodeId : FromNodeId;
				Errors.Add(FString::Printf(TEXT("connections[%d]: pin '%s' not found on '%s' (available: %s)"),
					i, *MissingPin, *MissingId, *UAL_ListPins(MissingOn)));
				continue;
			}

			/**
			 * 用 `CanCreateConnection` 而不是直接 `TryCreateConnection`。
			 *
			 * 两个理由：
			 *
			 * 1. **校验和改图必须分开。** 只有先把所有连线都验一遍，才能在
			 *    「一条都还没连」的时候决定整批放弃 —— 这是原子性的前提。
			 * 2. **它的返回远不止成功/失败。** DISALLOW 带着引擎自己写的原因
			 *    （类型不匹配、方向不对、会成环…），比我们瞎猜一句准得多；
			 *    而 BREAK_OTHERS_* 表示这一连会**顺手断开别的连线**，
			 *    MAKE_WITH_CONVERSION_NODE 表示引擎会**插一个转换节点**。
			 *    后两种如果不说，调用方回头一看图和自己写的不一样，
			 *    却不知道是谁改的。
			 */
			const FPinConnectionResponse Response = K2Schema->CanCreateConnection(FromPin, ToPin);
			bool bDeferred = false;
			if (Response.Response == CONNECT_RESPONSE_DISALLOW)
			{
				// 两端都还是 wildcard：这条拒绝多半只是「上游还没连上」，
				// 排到最后重试（见 FUAL_PendingConnection::bDeferred）
				if (UAL_IsUntypedWildcardPair(FromPin, ToPin))
				{
					bDeferred = true;
				}
				else
				{
					const FString Reason = Response.Message.IsEmpty()
						? TEXT("connection not allowed")
						: Response.Message.ToString();
					Errors.Add(FString::Printf(
						TEXT("connections[%d]: cannot connect '%s' (%s %s) -> '%s' (%s %s): %s"),
						i,
						*FromStr, *FromPin->PinType.PinCategory.ToString(), *UAL_PinDirToString(FromPin->Direction),
						*ToStr, *ToPin->PinType.PinCategory.ToString(), *UAL_PinDirToString(ToPin->Direction),
						*Reason));
					continue;
				}
			}

			FUAL_PendingConnection Conn;
			Conn.Index = i;
			Conn.FromLabel = FromStr;
			Conn.ToLabel = ToStr;
			Conn.FromPin = FromPin;
			Conn.ToPin = ToPin;
			Conn.bDeferred = bDeferred;
			Pending.Add(Conn);
		}
	}

	// ===== 8. 回滚：把这一批建过的全删掉，图回到调用前 =====
	//
	// 做成 lambda 是因为**有两个时机用得上**：一是校验就发现了错（下面紧接着），
	// 二是延后的那几根 wildcard 连线最后仍然连不上（第 9 步末尾）。第二种时候
	// 别的线已经连上了，所以要先把这一批连出来的线断掉 —— 断的是「这一批连的
	// 这一根」（`BreakLinkTo`），不是 `BreakAllPinLinks`，否则会顺手拆了
	// 用户图里原有的线，而那些线不是我们连的，也就轮不到我们断。
	//
	// 连线开始前图上有哪些节点。回滚时**多出来的那些**要一起删 ——
	// 引擎连线时可能自己插一个转换节点（CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE），
	// 那个节点不在 CreatedNodes 里，光断线带不走它，会留在图上而响应却说「没有任何改动」。
	TSet<UEdGraphNode*> NodesBeforeConnect;

	auto RollbackAndRespond = [&](const TArray<FString>& InErrors,
		const TArray<FUAL_PendingConnection>& MadeLinks)
	{
		for (int32 i = MadeLinks.Num() - 1; i >= 0; --i)
		{
			const FUAL_PendingConnection& Made = MadeLinks[i];
			if (Made.FromPin && Made.ToPin)
			{
				Made.FromPin->BreakLinkTo(Made.ToPin);
			}
		}

		// 引擎自己插的转换节点
		if (NodesBeforeConnect.Num() > 0)
		{
			TArray<UEdGraphNode*> Inserted;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && !NodesBeforeConnect.Contains(Node))
				{
					Inserted.Add(Node);
				}
			}
			for (UEdGraphNode* Node : Inserted)
			{
				FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile=*/true);
			}
		}

		/**
		 * Timeline 节点删起来和别的不一样：`UK2Node_Timeline::DestroyNode` 会
		 * 顺手把模板也删了（→ `FBlueprintEditorUtils::RemoveTimeline`）。
		 * 模板是这一批建的时候这正是我们要的；模板**本来就在**（图上没节点、
		 * 只有一个孤立模板的情况）就不行 —— 那等于回滚顺手毁了用户的曲线。
		 * 那几个改走 `UEdGraph::RemoveNode`：它只把节点摘出图，不碰模板。
		 */
		TSet<UEdGraphNode*> DetachOnly;
		for (const FUAL_TimelineSnapshot& Snapshot : TimelineSnapshots)
		{
			if (Snapshot.Template && !Snapshot.bTemplateCreated && Snapshot.bNodeCreated && Snapshot.Node)
			{
				DetachOnly.Add(Snapshot.Node);
			}
		}

		// 倒着删：后建的先删，减少节点之间的引用还没断就被删掉的机会。
		// bDontRecompile=true —— 每删一个都编译一次会让回退慢到不可接受，
		// 而且回退结束后本来就什么都不用编译（净改动为零）。
		for (int32 i = CreatedNodes.Num() - 1; i >= 0; --i)
		{
			if (UEdGraphNode* Node = CreatedNodes[i])
			{
				if (DetachOnly.Contains(Node))
				{
					Graph->RemoveNode(Node);
					continue;
				}
				FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile=*/true);
			}
		}

		// 模板上的改动删节点带不走，照快照回填（含复用节点的引脚重建）
		bool bCreatedAnyTimeline = false;
		for (int32 i = TimelineSnapshots.Num() - 1; i >= 0; --i)
		{
			bCreatedAnyTimeline |= TimelineSnapshots[i].bTemplateCreated;
			UAL_RestoreTimeline(TimelineSnapshots[i]);
		}

		/**
		 * 建过 Timeline 的话，回退完必须重编一次骨架，否则**名字还被占着**。
		 *
		 * `AddNewTimeline` 结尾会 `MarkBlueprintAsStructurallyModified`，骨架类上
		 * 因此长出一个同名属性；回退时 `DestroyNode` 走的
		 * `RemoveTimeline(..., bDontRecompile=true)` 只把模板从 `Blueprint->Timelines`
		 * 摘掉，**不重编骨架**。而 `FKismetNameValidator` 的名字表是从
		 * `SkeletonGeneratedClass` 的属性上取的（BlueprintEditorUtils.cpp
		 * `GetClassVariableList`），于是那个属性继续占着名字。
		 *
		 * 用户看到的就是：响应说「蓝图没有任何改动」，改完重发却报
		 * 「'DoorSwing' 命名已被使用」，而图里根本找不到这个 Timeline ——
		 * 最后只能把整个蓝图删掉重建。2026-09-16 的反馈里就是这么绕的。
		 */
		if (bCreatedAnyTimeline)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}

		// 净改动为零，这时候 Cancel 才是它真正的语义（别往 undo 栈里塞空记录）
		Transaction->Cancel();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), false);
		Result->SetBoolField(TEXT("rolled_back"), true);
		Result->SetStringField(TEXT("blueprint_path"), ResolvedPath);
		Result->SetStringField(TEXT("graph_name"), Graph->GetName());
		Result->SetNumberField(TEXT("created_count"), 0);
		Result->SetNumberField(TEXT("connection_count"), 0);
		Result->SetBoolField(TEXT("compiled"), false);
		Result->SetStringField(
			TEXT("message"),
			TEXT("Nothing was applied - the graph is unchanged. Fix the errors and resend the complete node/connection set."));

		TArray<TSharedPtr<FJsonValue>> ErrorsArray;
		for (const FString& Err : InErrors)
		{
			ErrorsArray.Add(MakeShared<FJsonValueString>(Err));
		}
		Result->SetArrayField(TEXT("errors"), ErrorsArray);

		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
	};

	if (Errors.Num() > 0)
	{
		RollbackAndRespond(Errors, TArray<FUAL_PendingConnection>());
		return;
	}

	// ===== 9. 真正连线 =====
	// 到这里每一条都已经被 CanCreateConnection 放行过（延后的那几根除外），
	// 正常不会失败；万一失败也如实记成 warning，不谎报成功。
	int32 ConnectionCount = 0;
	/** 这一批真的连上的线。延后那几根最后连不上时，要照着它一根根断回去 */
	TArray<FUAL_PendingConnection> MadeLinks;
	/** 延后重试之后仍然连不上的 —— 这时候才算真错，整批回滚 */
	TArray<FString> DeferredErrors;

	// 回滚要靠它认出引擎插进来的转换节点（见 RollbackAndRespond）
	//
	// 一根根 Add 而不是 Append(Graph->Nodes)：Graph->Nodes 是
	// TArray<TObjectPtr<UEdGraphNode>>，TSet::Append 认这个类型是 5.5 才有的事，
	// 5.0–5.4 上直接编译不过（error C2664）。range-for 每个版本都能把
	// TObjectPtr 隐式转成裸指针。
	for (UEdGraphNode* ExistingNode : Graph->Nodes)
	{
		NodesBeforeConnect.Add(ExistingNode);
	}

	auto ApplyConnection = [&](const FUAL_PendingConnection& Conn)
	{
		/**
		 * 警告要按**这一刻**的判据发，不能用校验那一轮存下来的。
		 *
		 * 校验发生在一根线都还没连的时候，那时通配引脚全是 wildcard，什么都接得上。
		 * 而连线是会定型的（数组节点的 TargetArray 一接上就把 Item 一起定死），
		 * 于是排在后面的那根线，真正应用时面对的是已经定了型的引脚 ——
		 * 引擎可能改为插一个转换节点，或者干脆拒绝。用旧判据就会漏掉这条警告，
		 * 调用方回头读图发现多了个自己没写的节点，而返回里一个字都没提。
		 */
		const FPinConnectionResponse Live = K2Schema->CanCreateConnection(Conn.FromPin, Conn.ToPin);
		const FString LiveNote = Live.Message.ToString();

		/**
		 * 延后的那几根到这一刻才第一次拿到有效答案。
		 *
		 * 校验那轮它们两端都是 wildcard，引擎给不出真结论；现在上游已经连上、
		 * 类型已经传播过来了。还是 DISALLOW 就是真连不上，记成错误整批回滚 ——
		 * 不能像普通连线那样降成 warning，否则用户拿到的是一张缺了线的图
		 * 配一句 ok:true。
		 */
		if (Conn.bDeferred && Live.Response == CONNECT_RESPONSE_DISALLOW)
		{
			DeferredErrors.Add(FString::Printf(
				TEXT("connections[%d]: cannot connect '%s' (%s %s) -> '%s' (%s %s): %s"),
				Conn.Index,
				*Conn.FromLabel, *Conn.FromPin->PinType.PinCategory.ToString(),
				*UAL_PinDirToString(Conn.FromPin->Direction),
				*Conn.ToLabel, *Conn.ToPin->PinType.PinCategory.ToString(),
				*UAL_PinDirToString(Conn.ToPin->Direction),
				LiveNote.IsEmpty() ? TEXT("connection not allowed") : *LiveNote));
			return;
		}

		// 副作用先说清楚：这两种情况下图会和调用方写的不完全一样
		if (Live.Response == CONNECT_RESPONSE_BREAK_OTHERS_A
			|| Live.Response == CONNECT_RESPONSE_BREAK_OTHERS_B
			|| Live.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB)
		{
			// 执行输出引脚只能有一根线（引擎自己的规矩，见 EdGraphSchema_K2.cpp
			// 的 bBreakExistingDueToExecOutput），所以这条不能只说「顶掉了一条」——
			// 得把「那怎么办」一起说了。2026-09-16 的反馈里，调用方看到这行
			// 只知道少了一条线，不知道扇出本来就要靠 Sequence
			const bool bExecOutput = Conn.FromPin && Conn.FromPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
			Warnings.Add(FString::Printf(
				TEXT("connections[%d] '%s' -> '%s': replaced an existing connection on that pin%s%s"),
				Conn.Index, *Conn.FromLabel, *Conn.ToLabel,
				LiveNote.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *LiveNote),
				bExecOutput
					? TEXT(". An exec output pin can only drive one node: to run two things in order, insert a Sequence node and use its Then_0 / Then_1 pins")
					: TEXT("")));
		}
		else if (Live.Response == CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE)
		{
			Warnings.Add(FString::Printf(
				TEXT("connections[%d] '%s' -> '%s': the engine inserted a conversion node, so the graph has one more node than you specified"),
				Conn.Index, *Conn.FromLabel, *Conn.ToLabel));
		}
		else if (Live.Response == CONNECT_RESPONSE_MAKE_WITH_PROMOTION)
		{
			Warnings.Add(FString::Printf(
				TEXT("connections[%d] '%s' -> '%s': pin type was promoted to match"),
				Conn.Index, *Conn.FromLabel, *Conn.ToLabel));
		}

		if (K2Schema->TryCreateConnection(Conn.FromPin, Conn.ToPin))
		{
			ConnectionCount++;
			MadeLinks.Add(Conn);
		}
		else if (Conn.bDeferred)
		{
			// 延后这几根不许降级成 warning，理由同上
			DeferredErrors.Add(FString::Printf(
				TEXT("connections[%d]: '%s' -> '%s' still could not be connected after the other wires were made%s"),
				Conn.Index, *Conn.FromLabel, *Conn.ToLabel,
				LiveNote.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *LiveNote)));
		}
		else
		{
			// 校验时能连、现在连不上，多半是前面几根线把引脚定了型。
			// 把引擎此刻给的理由带上，否则调用方只知道「失败了」
			Warnings.Add(FString::Printf(
				TEXT("connections[%d] '%s' -> '%s': validated but failed to apply%s"),
				Conn.Index, *Conn.FromLabel, *Conn.ToLabel,
				LiveNote.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *LiveNote)));
		}
	};

	/**
	 * 两轮：先连能立刻确定的，再连延后的。
	 *
	 * 顺序就是这个修复的全部内容 —— 第一轮连上 `gaoc.OutActors → foreach.Array`
	 * 的那一刻，引擎把 ForEach 的 Array Element 定型成 AActor；第二轮
	 * `foreach.Array Element → cast.Object` 就连得上了。以前这两根写在同一批里
	 * 必败，调用方只能拆成两次调用发。
	 */
	for (const FUAL_PendingConnection& Conn : Pending)
	{
		if (!Conn.bDeferred)
		{
			ApplyConnection(Conn);
		}
	}
	for (const FUAL_PendingConnection& Conn : Pending)
	{
		if (Conn.bDeferred)
		{
			ApplyConnection(Conn);
		}
	}

	// 延后的最终还是连不上：这才是真错。图已经改了，所以回滚要连线一起断
	if (DeferredErrors.Num() > 0)
	{
		RollbackAndRespond(DeferredErrors, MadeLinks);
		return;
	}

	// ===== 10. clear_existing 的删除放到最后 =====
	// 放在最前面的话，中途任何一步失败都恢复不了这些被删的节点 ——
	// 那就等于「失败后图被清空了」，比不原子还糟。
	int32 RemovedCount = 0;
	if (bClearExisting)
	{
		for (UEdGraphNode* Node : PreExistingNodes)
		{
			if (!Node || CreatedNodes.Contains(Node))
			{
				continue;
			}
			/**
			 * 删不删得，问节点自己。
			 *
			 * 这里原来硬编码「跳过 FunctionEntry / FunctionResult」。那两个确实
			 * 删不得（删了图就废了），但**不止那两个** —— Tunnel、Composite
			 * 同样会返回 false。`CanUserDeleteNode()` 是引擎自己的判据，
			 * 覆盖现在和以后所有这类节点，不用我们跟着引擎版本追。
			 */
			if (!Node->CanUserDeleteNode())
			{
				continue;
			}
			// 被复用的事件节点也要留 —— 它现在是本次逻辑的一部分
			if (NodeIdMap.FindKey(Node) != nullptr)
			{
				continue;
			}
			FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile=*/true);
			RemovedCount++;
		}
	}

	// ===== 11. 落地 =====
	// MarkBlueprintAsStructurallyModified 内部会同步跑一次骨架编译，
	// 所以整批只调一次（旧的逐节点接口是每加一个节点调一次）。
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	// 蓝图正开在编辑器里时，不通知的话用户看到的还是改之前的图 ——
	// 界面和磁盘不一致，而且完全没有报错提示
	Graph->NotifyGraphChanged();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_path"), ResolvedPath);
	Result->SetStringField(TEXT("graph_name"), Graph->GetName());
	Result->SetNumberField(TEXT("created_count"), CreatedNodesInfo.Num());
	Result->SetNumberField(TEXT("connection_count"), ConnectionCount);
	if (RemovedCount > 0)
	{
		Result->SetNumberField(TEXT("removed_count"), RemovedCount);
	}
	Result->SetArrayField(TEXT("nodes"), CreatedNodesInfo);
	// 事务拿不到时用户撤不回这次改动，如实说，不假装
	Result->SetBoolField(TEXT("undoable"), bUndoable);

	/**
	 * 编译必须在事务**之外**跑 —— 否则撤销这一步会把关卡里的实例一起删掉。
	 *
	 * 完整编译（不是骨架编译）会走 `FBlueprintCompileReinstancer`：关卡里这个类的
	 * 每个实例都被「重新生成一个、销毁旧的」替换掉。销毁走的是
	 * `UWorld::EditorDestroyActor(OldActor, bShouldModifyLevel=true)`，而
	 * `UWorld::RemoveActor`（World.cpp，5.5 在 2536 行）里写着：
	 *
	 *     if (bShouldModifyLevel && GUndo) { ModifyLevel(CheckLevel); }
	 *
	 * `GUndo` 只在有事务开着时非空。所以事务里编译 = 把整个关卡的 Actor 列表
	 * 快照进这一笔事务；撤销时列表被还原成编译前的样子，里面指的是已经被
	 * 重新生成流程换掉的旧对象 —— 用户看到的结果就是**关卡里的 Actor 凭空消失**，
	 * 紧接着一次保存就落盘了。
	 *
	 * 2026-09-16 的反馈里就是这么丢的宝箱：撤销三步「Apply Blueprint Graph」，
	 * 关卡里的 BP_TreasureChest 实例跟着没了，返回体里一个字都没提。
	 *
	 * 用户在编辑器里点「编译」本来也不在事务里，所以放到事务外才是和引擎一致的行为。
	 */
	Transaction.Reset();

	if (bCompile)
	{
		FCompilerResultsLog CompileResults;
		CompileResults.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompileResults);

		Result->SetBoolField(TEXT("compiled"), true);
		Result->SetNumberField(TEXT("compile_error_count"), CompileResults.NumErrors);
		Result->SetNumberField(TEXT("compile_warning_count"), CompileResults.NumWarnings);

		// 编译诊断按**调用方给的 id** 回传，不是引擎 GUID ——
		// 调用方手里只有自己写的 id，回 GUID 等于让它再查一次图去对号
		TMap<UEdGraphNode*, FString> NodeToCallerId;
		for (const auto& Pair : NodeIdMap)
		{
			NodeToCallerId.Add(Pair.Value, Pair.Key);
		}

		TArray<TSharedPtr<FJsonValue>> Diagnostics;
		for (const TSharedRef<FTokenizedMessage>& Message : CompileResults.Messages)
		{
			const EMessageSeverity::Type Severity = Message->GetSeverity();
			if (Severity != EMessageSeverity::Error && Severity != EMessageSeverity::Warning)
			{
				continue;
			}

			TSharedPtr<FJsonObject> Diag = MakeShared<FJsonObject>();
			Diag->SetStringField(TEXT("severity"), Severity == EMessageSeverity::Error ? TEXT("error") : TEXT("warning"));
			Diag->SetStringField(TEXT("message"), Message->ToText().ToString());

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			for (const TSharedRef<IMessageToken>& Token : Message->GetMessageTokens())
			{
				if (Token->GetType() != EMessageToken::Object)
				{
					continue;
				}
				const TSharedRef<FUObjectToken> ObjectToken = StaticCastSharedRef<FUObjectToken>(Token);
				UEdGraphNode* Node = Cast<UEdGraphNode>(ObjectToken->GetObject().Get());
				if (!Node)
				{
					continue;
				}
				if (const FString* CallerId = NodeToCallerId.Find(Node))
				{
					Diag->SetStringField(TEXT("node"), *CallerId);
				}
				Diag->SetStringField(TEXT("node_id"), UAL_GuidToString(Node->NodeGuid));
				break;
			}
#endif
			Diagnostics.Add(MakeShared<FJsonValueObject>(Diag));
		}
		Result->SetArrayField(TEXT("diagnostics"), Diagnostics);
	}
	else
	{
		Result->SetBoolField(TEXT("compiled"), false);
	}

	// 引擎替我们做了但调用方没要求的事，必须说 —— 否则它回头读图会发现
	// 和自己写的不一样，却不知道是谁改的
	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningsArray;
		for (const FString& W : Warnings)
		{
			WarningsArray.Add(MakeShared<FJsonValueString>(W));
		}
		Result->SetArrayField(TEXT("warnings"), WarningsArray);
	}

	/**
	 * 引脚快照放到最后：默认值、连线、清旧节点、编译都做完之后再读。
	 *
	 * 以前是建完节点当场就序列化，回执里 `default_value` 是 None、`linked_count` 全是 0，
	 * 和同一份回执里的 `connection_count` 自相矛盾（2026-09-24 用户反馈），
	 * 调用方只能再读一遍整图才敢信。
	 *
	 * 按 GUID 重新找节点而不是拿 NodeIdMap 里的指针：编译可能 ReconstructNode，
	 * 引脚对象会整批换掉，节点本身保留 GUID。
	 */
	for (const TSharedPtr<FJsonValue>& InfoValue : CreatedNodesInfo)
	{
		const TSharedPtr<FJsonObject> NodeInfo = InfoValue.IsValid() ? InfoValue->AsObject() : nullptr;
		if (!NodeInfo.IsValid())
		{
			continue;
		}
		if (UEdGraphNode* FinalNode = UAL_FindNodeByGuid(Graph, NodeInfo->GetStringField(TEXT("node_id"))))
		{
			NodeInfo->SetArrayField(TEXT("pins"), UAL_BuildPinsJson(FinalNode));
		}
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// blueprint.search_nodes — 写之前先查一批
// ============================================================================

/**
 * 按名字搜可用的函数，连**完整引脚签名**一起回。
 *
 * ## 为什么必须有这个命令
 *
 * 在它之前，引脚信息只能作为「建节点」的副作用拿到 —— 想知道
 * `PrintString` 有哪些引脚，唯一办法是先把这个节点建出来再看返回。
 * 于是调用方被迫「建一个 → 看一眼 → 连一根」地往返，一张十来个节点的图
 * 要几十个来回，中间任何一步走偏都得从头对账。
 *
 * 有了这个只读命令，「查一批 → 写一整张」才成立。
 *
 * 搜索范围：蓝图自己的父类链 + 常用函数库。只回蓝图里放得出来的，
 * 回了也连不上的函数属于噪音。
 */
void FUAL_BlueprintCommands::Handle_SearchBlueprintNodes(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Query;
	Payload->TryGetStringField(TEXT("query"), Query);
	if (Query.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: query"));
		return;
	}

	int32 Limit = 12;
	Payload->TryGetNumberField(TEXT("limit"), Limit);
	Limit = FMath::Clamp(Limit, 1, 50);

	// blueprint_path 可选：给了就把它的父类链也纳入搜索范围（成员函数在那里），
	// 不给就只搜通用函数库。
	UBlueprint* Blueprint = nullptr;
	FString BlueprintPath;
	if (Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) && !BlueprintPath.IsEmpty())
	{
		FString ResolvedPath;
		UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath);
	}

	struct FCandidate
	{
		UFunction* Function = nullptr;
		UClass* OwnerClass = nullptr;
		int32 Score = 0;
	};
	TArray<FCandidate> Candidates;

	const FString QueryLower = Query.ToLower();

	auto ScoreName = [&QueryLower](const FString& Name) -> int32
	{
		const FString NameLower = Name.ToLower();
		if (NameLower == QueryLower) return 100;
		if (NameLower.StartsWith(QueryLower)) return 75;
		if (NameLower.Contains(QueryLower)) return 50;
		return 0;
	};

	auto Collect = [&](UClass* Cls)
	{
		if (!Cls) return;
		for (TFieldIterator<UFunction> It(Cls, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			UFunction* Func = *It;
			if (!Func) continue;

			// 蓝图里放不出来的函数回了也没用
			if (!Func->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_BlueprintEvent))
			{
				continue;
			}

			// 两个名字都参与打分，取高的那个：调用方可能按蓝图里看到的
			// GetActorLocation 搜，也可能按文档里的 K2_GetActorLocation 搜
			const int32 Score = FMath::Max(ScoreName(Func->GetName()), ScoreName(UAL_BlueprintFunctionName(Func)));
			if (Score == 0) continue;

			const bool bAlready = Candidates.ContainsByPredicate(
				[Func](const FCandidate& C) { return C.Function == Func; });
			if (bAlready) continue;

			Candidates.Add({ Func, Cls, Score });
		}
	};

	// 搜索范围和解析范围必须**用同一套**。原来这里只扫四个硬编码的库，
	// 于是出现最坏的一种组合：search_nodes 说"没有这个函数"，
	// 而它其实存在于第五个库里 —— 调用方被自己的工具骗着放弃了正确答案。
	UAL_ForEachFunctionSource(Blueprint, [&Collect](UClass* Cls) -> bool
	{
		Collect(Cls);
		return true;
	});

	// 这个蓝图身上挂的控件 / 组件的类也要搜 —— 它们既不是函数库也不在父类链上，
	// 而调用方要调的恰恰是它们身上的函数（TextBlock.SetText、
	// StaticMeshComponent.SetStaticMesh）。理由见 UAL_CollectOwnedClasses
	TArray<UClass*> OwnedClasses;
	UAL_CollectOwnedClasses(Blueprint, OwnedClasses);
	for (UClass* Cls : OwnedClasses)
	{
		Collect(Cls);
	}

	Candidates.Sort([](const FCandidate& A, const FCandidate& B)
	{
		if (A.Score != B.Score) return A.Score > B.Score;
		return A.Function->GetName() < B.Function->GetName();
	});

	TArray<TSharedPtr<FJsonValue>> Results;
	for (int32 i = 0; i < Candidates.Num() && Results.Num() < Limit; ++i)
	{
		const FCandidate& C = Candidates[i];

		// 回声明这个函数的类，而不是搜到它的那个子类 —— 更接近编辑器里显示的样子
		UClass* DeclaringClass = C.Function->GetOwnerClass() ? C.Function->GetOwnerClass() : C.OwnerClass;

		// `name` / `member_name` 一律回**蓝图名** —— 这两个字段的唯一用途就是
		// 被原样填回 add_node / apply_graph。回 C++ 名（K2_GetActorLocation）
		// 的话，调用方照抄一遍还是被拒，这个工具就白查了。
		const FString BpName = UAL_BlueprintFunctionName(C.Function);

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), BpName);
		// 直接可以填进 create_graph 的 member_name，省调用方一步拼接
		Entry->SetStringField(
			TEXT("member_name"),
			FString::Printf(TEXT("%s.%s"), *DeclaringClass->GetName(), *BpName));
		Entry->SetStringField(TEXT("class"), DeclaringClass->GetName());
		// C++ 名只在和蓝图名不同的时候回，供排查用；相同就别占带宽
		if (!BpName.Equals(C.Function->GetName()))
		{
			Entry->SetStringField(TEXT("cpp_name"), C.Function->GetName());
		}
		Entry->SetBoolField(TEXT("is_pure"), C.Function->HasAnyFunctionFlags(FUNC_BlueprintPure));

#if WITH_EDITOR
		const FString Tooltip = C.Function->GetToolTipText().ToString();
		if (!Tooltip.IsEmpty())
		{
			// 截断到一行 —— 这里是给「挑哪个」用的，不是文档
			Entry->SetStringField(TEXT("summary"), Tooltip.Left(160).Replace(TEXT("\n"), TEXT(" ")));
		}
#endif

		// 引脚签名：这才是这个命令存在的意义 —— 不建节点就知道怎么连
		TArray<TSharedPtr<FJsonValue>> Params;
		for (TFieldIterator<FProperty> ParamIt(C.Function); ParamIt && (ParamIt->PropertyFlags & CPF_Parm); ++ParamIt)
		{
			FProperty* Param = *ParamIt;
			TSharedPtr<FJsonObject> ParamJson = MakeShared<FJsonObject>();
			ParamJson->SetStringField(TEXT("name"), Param->GetName());
			ParamJson->SetStringField(TEXT("type"), Param->GetCPPType());
			const bool bIsOut = Param->HasAnyPropertyFlags(CPF_ReturnParm)
				|| (Param->HasAnyPropertyFlags(CPF_OutParm) && !Param->HasAnyPropertyFlags(CPF_ConstParm));
			ParamJson->SetStringField(TEXT("dir"), bIsOut ? TEXT("Output") : TEXT("Input"));
			Params.Add(MakeShared<FJsonValueObject>(ParamJson));
		}
		Entry->SetArrayField(TEXT("params"), Params);

		Results.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("query"), Query);
	Result->SetNumberField(TEXT("match_count"), Results.Num());
	Result->SetNumberField(TEXT("total_candidates"), Candidates.Num());
	Result->SetArrayField(TEXT("functions"), Results);
	if (Candidates.Num() > Results.Num())
	{
		// 截断要说出来，否则调用方会把「前 12 个」当成「全部」
		Result->SetStringField(
			TEXT("note"),
			FString::Printf(TEXT("Showing %d of %d matches - narrow the query or raise limit."), Results.Num(), Candidates.Num()));
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// blueprint.compile_all —— 批量编译
// ============================================================================

void FUAL_BlueprintCommands::Handle_CompileAllBlueprints(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Scope = TEXT("touched");
	Payload->TryGetStringField(TEXT("scope"), Scope);
	Scope = Scope.ToLower();

	int32 Limit = 200;
	Payload->TryGetNumberField(TEXT("limit"), Limit);
	Limit = FMath::Clamp(Limit, 1, 2000);

	TArray<UBlueprint*> Targets;

	if (Scope == TEXT("touched"))
	{
		// 只编我们自己改脏的。改完一批蓝图之后最常见的诉求就是这个，
		// 而全工程扫一遍在大工程里要几分钟。
		for (UPackage* Package : FUAL_TouchedPackages::CollectDirty())
		{
			ForEachObjectWithOuter(Package, [&Targets](UObject* Object)
			{
				if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
				{
					Targets.AddUnique(Blueprint);
				}
			}, /*bIncludeNestedObjects=*/false);
		}
	}
	else if (Scope == TEXT("all"))
	{
		FString SearchPath = TEXT("/Game");
		Payload->TryGetStringField(TEXT("path"), SearchPath);

		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

		FARFilter Filter;
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(FName(*SearchPath));
		Filter.bRecursiveClasses = true;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
#else
		Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
#endif

		TArray<FAssetData> Assets;
		AssetRegistryModule.Get().GetAssets(Filter, Assets);

		for (const FAssetData& Asset : Assets)
		{
			if (Targets.Num() >= Limit)
			{
				break;
			}
			// 这一步会把蓝图加载进内存，是全量编译真正的开销所在
			if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset()))
			{
				Targets.AddUnique(Blueprint);
			}
		}
	}
	else
	{
		UAL_CommandUtils::SendError(
			RequestId, 400,
			FString::Printf(TEXT("Unknown scope \"%s\". Use \"touched\" (default) or \"all\"."), *Scope));
		return;
	}

	const bool bTruncated = (Targets.Num() >= Limit);

	int32 TotalErrors = 0;
	int32 TotalWarnings = 0;
	TArray<TSharedPtr<FJsonValue>> Failures;

	for (UBlueprint* Blueprint : Targets)
	{
		if (!Blueprint)
		{
			continue;
		}

		FCompilerResultsLog Results;
		Results.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);

		TotalErrors += Results.NumErrors;
		TotalWarnings += Results.NumWarnings;

		// 编过的不进返回 —— 一个大工程几百个蓝图全列出来，真正出问题的
		// 那几个反而被淹掉了。只报有问题的。
		if (Results.NumErrors == 0 && Results.NumWarnings == 0)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
		Failure->SetStringField(TEXT("path"), Blueprint->GetPathName());
		Failure->SetNumberField(TEXT("errors"), Results.NumErrors);
		Failure->SetNumberField(TEXT("warnings"), Results.NumWarnings);

		TArray<TSharedPtr<FJsonValue>> Messages;
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			const EMessageSeverity::Type Severity = Message->GetSeverity();
			if (Severity != EMessageSeverity::Error && Severity != EMessageSeverity::Warning)
			{
				continue;
			}
			// 每个蓝图最多带 5 条，够定位问题，不至于把响应撑爆
			if (Messages.Num() >= 5)
			{
				break;
			}
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("severity"), Severity == EMessageSeverity::Error ? TEXT("error") : TEXT("warning"));
			Entry->SetStringField(TEXT("message"), Message->ToText().ToString());
			Messages.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Failure->SetArrayField(TEXT("messages"), Messages);
		Failures.Add(MakeShared<FJsonValueObject>(Failure));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), TotalErrors == 0);
	Result->SetStringField(TEXT("scope"), Scope);
	Result->SetNumberField(TEXT("compiled_count"), Targets.Num());
	Result->SetNumberField(TEXT("error_count"), TotalErrors);
	Result->SetNumberField(TEXT("warning_count"), TotalWarnings);
	Result->SetArrayField(TEXT("failures"), Failures);

	if (Targets.Num() == 0)
	{
		Result->SetStringField(
			TEXT("note"),
			Scope == TEXT("touched")
				? TEXT("No Blueprints have been modified by this plugin since the last save. Use scope=\"all\" to compile the whole project.")
				: TEXT("No Blueprints were found under the given path."));
	}

	// 截断了必须说 —— 不说的话「0 个错误」读起来像是全工程都过了，
	// 而实际上只编了前 N 个
	if (bTruncated)
	{
		Result->SetBoolField(TEXT("truncated"), true);
		Result->SetStringField(
			TEXT("truncated_note"),
			FString::Printf(TEXT("Stopped at the limit of %d Blueprints - more may exist and were NOT compiled."), Limit));
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// blueprint.set_node_positions —— 批量挪节点
// ============================================================================

void FUAL_BlueprintCommands::Handle_SetNodePositions(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* PositionsArray = nullptr;
	if (!Payload->TryGetArrayField(TEXT("positions"), PositionsArray) || !PositionsArray || PositionsArray->Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing or empty required field: positions"));
		return;
	}

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	FString GraphName;
	Payload->TryGetStringField(TEXT("graph_name"), GraphName);
	UEdGraph* Graph = UAL_FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(
			RequestId, 404,
			FString::Printf(TEXT("Graph not found: %s"), GraphName.IsEmpty() ? TEXT("EventGraph") : *GraphName));
		return;
	}

	// 挪节点要能被 Ctrl+Z 撤销 —— 排版这件事用户十有八九会想反悔一次
	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UnrealAgentLink", "LayoutBlueprintGraph", "Layout Blueprint Graph"));
	Blueprint->Modify();
	Graph->Modify();

	int32 Moved = 0;
	TArray<TSharedPtr<FJsonValue>> NotFound;

	for (const TSharedPtr<FJsonValue>& Value : *PositionsArray)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj || !(*Obj).IsValid())
		{
			continue;
		}

		FString NodeId;
		if (!(*Obj)->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
		{
			continue;
		}

		UEdGraphNode* Node = UAL_FindNodeByGuid(Graph, NodeId);
		if (!Node)
		{
			// 挪不到的节点单列出来，而不是静默跳过 —— 调用方多半是拿着一份
			// 过期的图在算坐标，那种情况下「成功挪了 3 个」会掩盖真正的问题
			NotFound.Add(MakeShared<FJsonValueString>(NodeId));
			continue;
		}

		int32 X = Node->NodePosX;
		int32 Y = Node->NodePosY;
		(*Obj)->TryGetNumberField(TEXT("x"), X);
		(*Obj)->TryGetNumberField(TEXT("y"), Y);

		Node->Modify();
		Node->NodePosX = X;
		Node->NodePosY = Y;
		++Moved;
	}

	// 只挪位置不改结构，用 MarkBlueprintAsModified 而不是
	// MarkBlueprintAsStructurallyModified —— 后者会触发重编译，
	// 排一次版重编一次蓝图，大蓝图上是好几秒的白等
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), NotFound.Num() == 0);
	Result->SetStringField(TEXT("blueprint_path"), ResolvedPath);
	Result->SetStringField(TEXT("graph_name"), Graph->GetName());
	Result->SetNumberField(TEXT("moved"), Moved);
	if (NotFound.Num() > 0)
	{
		Result->SetArrayField(TEXT("not_found"), NotFound);
		Result->SetStringField(
			TEXT("note"),
			TEXT("Some node ids were not in this graph - re-read it with blueprint.get_graph, the layout you computed may be stale."));
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/**
 * blueprint.set_comment —— 建一个注释框，或者改一个已有的。
 *
 * ## 为什么要有它
 *
 * 注释框是蓝图里唯一的分组手段，也是用户手写的说明文字在图上的载体。
 * 在它之前，整个工具集**读不出注释框有多大、框住了谁，更写不了它**：
 * 排版只好绕着它走（原地不动），于是图整理完，说明还贴在原来的位置上，
 * 指着一片空白。
 *
 * ## 边界
 *
 * `bounds` 给了就按 bounds 来 —— 调用方（排版）自己算过每个节点多大，
 * 比这边准。`enclose_nodes` 是给「框住这几个节点」这种说法用的，尺寸按节点
 * 数量和引脚数估，宽松一点也不碍事：注释框是给人看的，不参与编译。
 */
void FUAL_BlueprintCommands::Handle_SetComment(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	FString GraphName;
	Payload->TryGetStringField(TEXT("graph_name"), GraphName);
	UEdGraph* Graph = UAL_FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(
			RequestId, 404,
			FString::Printf(TEXT("Graph not found: %s"), GraphName.IsEmpty() ? TEXT("EventGraph") : *GraphName));
		return;
	}

	FString NodeId;
	Payload->TryGetStringField(TEXT("node_id"), NodeId);

	UEdGraphNode_Comment* Comment = nullptr;
	bool bCreated = false;

	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UnrealAgentLink", "SetBlueprintComment", "Set Blueprint Comment"));
	Blueprint->Modify();
	Graph->Modify();

	if (!NodeId.IsEmpty())
	{
		UEdGraphNode* Existing = UAL_FindNodeByGuid(Graph, NodeId);
		if (!Existing)
		{
			UAL_CommandUtils::SendError(
				RequestId, 404,
				FString::Printf(TEXT("Node not found in this graph: %s"), *NodeId));
			return;
		}
		Comment = Cast<UEdGraphNode_Comment>(Existing);
		if (!Comment)
		{
			UAL_CommandUtils::SendError(
				RequestId, 400,
				FString::Printf(TEXT("Node %s is a %s, not a comment box. Omit node_id to create a new comment."),
					*NodeId, *Existing->GetClass()->GetName()));
			return;
		}
		Comment->Modify();
	}
	else
	{
		Comment = NewObject<UEdGraphNode_Comment>(Graph);
		Comment->SetFlags(RF_Transactional);
		Graph->AddNode(Comment, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Comment->CreateNewGuid();
		Comment->PostPlacedNewNode();
		Comment->AllocateDefaultPins();
		// 新注释框先给一个能看见的默认大小，下面有 bounds / enclose_nodes 再覆盖
		Comment->NodeWidth = 400;
		Comment->NodeHeight = 200;
		bCreated = true;
	}

	FString Text;
	if (Payload->TryGetStringField(TEXT("text"), Text))
	{
		Comment->NodeComment = Text;
	}
	else if (bCreated)
	{
		Comment->NodeComment = TEXT("Comment");
	}

	int32 FontSize = 0;
	if (Payload->TryGetNumberField(TEXT("font_size"), FontSize) && FontSize > 0)
	{
		Comment->FontSize = FMath::Clamp(FontSize, 1, 1000);
	}

	const TSharedPtr<FJsonObject>* ColorObj = nullptr;
	if (Payload->TryGetObjectField(TEXT("color"), ColorObj) && ColorObj && (*ColorObj).IsValid())
	{
		FLinearColor Color = Comment->CommentColor;
		(*ColorObj)->TryGetNumberField(TEXT("r"), Color.R);
		(*ColorObj)->TryGetNumberField(TEXT("g"), Color.G);
		(*ColorObj)->TryGetNumberField(TEXT("b"), Color.B);
		(*ColorObj)->TryGetNumberField(TEXT("a"), Color.A);
		Comment->CommentColor = Color;
	}

	// enclose_nodes：把点到的节点重新登记为「框在里面的」，并按它们算一个框。
	// 登记这一步不能省 —— 用户之后在编辑器里拖动这个注释框时，引擎靠
	// NodesUnderComment 决定带走谁，空的话框一动、里面的节点全留在原地
	const TArray<TSharedPtr<FJsonValue>>* EncloseArray = nullptr;
	bool bHasEnclosed = false;
	int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
	TArray<TSharedPtr<FJsonValue>> EnclosedIds;
	TArray<TSharedPtr<FJsonValue>> NotFound;

	if (Payload->TryGetArrayField(TEXT("enclose_nodes"), EncloseArray) && EncloseArray)
	{
		Comment->ClearNodesUnderComment();
		for (const TSharedPtr<FJsonValue>& Value : *EncloseArray)
		{
			FString InnerId;
			if (!Value.IsValid() || !Value->TryGetString(InnerId) || InnerId.IsEmpty())
			{
				continue;
			}
			UEdGraphNode* Inner = UAL_FindNodeByGuid(Graph, InnerId);
			if (!Inner || Inner == Comment)
			{
				NotFound.Add(MakeShared<FJsonValueString>(InnerId));
				continue;
			}

			Comment->AddNodeUnderComment(Inner);
			EnclosedIds.Add(MakeShared<FJsonValueString>(UAL_GuidToString(Inner->NodeGuid)));

			// 节点的真实尺寸只有编辑器里的 Slate widget 知道（这里拿不到），
			// 所以按引脚数估一个 —— 估宽了只是框大一点，估窄了会把节点露在外面
			const int32 EstimatedWidth = Inner->NodeWidth > 0 ? Inner->NodeWidth : 260;
			const int32 EstimatedHeight =
				Inner->NodeHeight > 0 ? Inner->NodeHeight : (60 + Inner->Pins.Num() * 28);

			if (!bHasEnclosed)
			{
				MinX = Inner->NodePosX;
				MinY = Inner->NodePosY;
				MaxX = Inner->NodePosX + EstimatedWidth;
				MaxY = Inner->NodePosY + EstimatedHeight;
				bHasEnclosed = true;
			}
			else
			{
				MinX = FMath::Min(MinX, Inner->NodePosX);
				MinY = FMath::Min(MinY, Inner->NodePosY);
				MaxX = FMath::Max(MaxX, Inner->NodePosX + EstimatedWidth);
				MaxY = FMath::Max(MaxY, Inner->NodePosY + EstimatedHeight);
			}
		}

		if (bHasEnclosed)
		{
			// 上边多留一条：注释框的标题栏画在框体上方，不留就压住第一排节点
			const int32 Padding = 40;
			const int32 TitleBar = 48;
			Comment->NodePosX = MinX - Padding;
			Comment->NodePosY = MinY - Padding - TitleBar;
			Comment->NodeWidth = (MaxX - MinX) + Padding * 2;
			Comment->NodeHeight = (MaxY - MinY) + Padding * 2 + TitleBar;
		}
	}

	// bounds 最后处理：调用方明确给了框，就以它为准
	const TSharedPtr<FJsonObject>* BoundsObj = nullptr;
	if (Payload->TryGetObjectField(TEXT("bounds"), BoundsObj) && BoundsObj && (*BoundsObj).IsValid())
	{
		int32 Value = 0;
		if ((*BoundsObj)->TryGetNumberField(TEXT("x"), Value)) Comment->NodePosX = Value;
		if ((*BoundsObj)->TryGetNumberField(TEXT("y"), Value)) Comment->NodePosY = Value;
		if ((*BoundsObj)->TryGetNumberField(TEXT("width"), Value)) Comment->NodeWidth = FMath::Max(Value, 32);
		if ((*BoundsObj)->TryGetNumberField(TEXT("height"), Value)) Comment->NodeHeight = FMath::Max(Value, 32);
	}

	// 注释框不参与编译，用 MarkBlueprintAsModified 就够 ——
	// 结构性修改会触发一次完整重编译，大蓝图上那是好几秒的白等
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), NotFound.Num() == 0);
	Result->SetStringField(TEXT("blueprint_path"), ResolvedPath);
	Result->SetStringField(TEXT("graph_name"), Graph->GetName());
	Result->SetStringField(TEXT("node_id"), UAL_GuidToString(Comment->NodeGuid));
	Result->SetBoolField(TEXT("created"), bCreated);
	Result->SetStringField(TEXT("text"), Comment->NodeComment);
	Result->SetNumberField(TEXT("x"), Comment->NodePosX);
	Result->SetNumberField(TEXT("y"), Comment->NodePosY);
	Result->SetNumberField(TEXT("width"), Comment->NodeWidth);
	Result->SetNumberField(TEXT("height"), Comment->NodeHeight);
	Result->SetArrayField(TEXT("enclosed_nodes"), EnclosedIds);
	if (NotFound.Num() > 0)
	{
		Result->SetArrayField(TEXT("not_found"), NotFound);
		Result->SetStringField(
			TEXT("note"),
			TEXT("Some enclose_nodes ids were not in this graph - re-read it with blueprint.get_graph."));
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// blueprint.set_variable_meta / remove_variable / event_dispatcher
//
// 这三条补的是同一个洞：变量和事件此前**只能加，不能调、不能删**。
// 而蓝图变量最常见的用途恰恰是「暴露到细节面板让策划改」——
// 做不到这个，建出来的变量只能在图里用，等于少了一半价值。
// ============================================================================

/**
 * 找一条成员变量。找不到时把现有变量名一起回，让调用方能自己纠正 ——
 * 只说「没找到」的话，下一步只能靠猜或者重读整个蓝图。
 */
static FBPVariableDescription* UAL_FindMemberVariable(UBlueprint* Blueprint, const FName& VarName, FString& OutAvailable)
{
	TArray<FString> Names;
	FBPVariableDescription* Found = nullptr;

	for (FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		Names.Add(Var.VarName.ToString());
		if (Var.VarName == VarName)
		{
			Found = &Var;
		}
	}

	OutAvailable = FString::Join(Names, TEXT(", "));
	return Found;
}

/** 变量当前的元数据快照，改完回给调用方核对 */
static TSharedPtr<FJsonObject> UAL_BuildVariableMetaJson(const FBPVariableDescription& Var)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("name"), Var.VarName.ToString());
	Obj->SetStringField(TEXT("type"), Var.VarType.PinCategory.ToString());
	// 实例可编辑 = 有 Edit 标记且没有 DisableEditOnInstance。
	// 细节面板上那个勾就是这两个标记的组合，单看其中一个会得出相反的结论。
	Obj->SetBoolField(TEXT("instance_editable"),
		(Var.PropertyFlags & CPF_Edit) != 0 && (Var.PropertyFlags & CPF_DisableEditOnInstance) == 0);
	Obj->SetBoolField(TEXT("blueprint_read_only"), (Var.PropertyFlags & CPF_BlueprintReadOnly) != 0);
	Obj->SetStringField(TEXT("category"), Var.Category.ToString());
	// 联机复制。RepNotify 是 Replicated 的加强版（多一个变化时的回调），
	// 所以三态用一个字段表达，不是两个布尔
	Obj->SetStringField(TEXT("replication"),
		(Var.PropertyFlags & CPF_RepNotify) != 0 ? TEXT("RepNotify") :
		(Var.PropertyFlags & CPF_Net) != 0 ? TEXT("Replicated") : TEXT("None"));

	for (const FBPVariableMetaDataEntry& Entry : Var.MetaDataArray)
	{
		if (Entry.DataKey == FBlueprintMetadata::MD_Tooltip)
		{
			Obj->SetStringField(TEXT("tooltip"), Entry.DataValue);
		}
		else if (Entry.DataKey == FBlueprintMetadata::MD_ExposeOnSpawn)
		{
			Obj->SetBoolField(TEXT("expose_on_spawn"), Entry.DataValue.ToBool());
		}
	}
	return Obj;
}

void FUAL_BlueprintCommands::Handle_SetVariableMeta(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	FString VarNameStr;
	if (!Payload->TryGetStringField(TEXT("name"), VarNameStr) || VarNameStr.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: name"));
		return;
	}

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	const FName VarName(*VarNameStr);
	FString Available;
	if (!UAL_FindMemberVariable(Blueprint, VarName, Available))
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(
			TEXT("Variable '%s' not found on %s (available: %s)"), *VarNameStr, *Blueprint->GetName(), *Available));
		return;
	}

	// 只改传进来的字段。没传的原样不动 —— 想只改分类的调用方不该被迫
	// 把其余几项一起重报，重报就意味着可能报错。
	TArray<TSharedPtr<FJsonValue>> Applied;
	auto Note = [&Applied](const TCHAR* What) { Applied.Add(MakeShared<FJsonValueString>(What)); };

	bool bInstanceEditable = false;
	if (Payload->TryGetBoolField(TEXT("instance_editable"), bInstanceEditable))
	{
		// 引擎这个接口是反过来的：bNewBlueprintOnly=true 表示「只在蓝图内可见」，
		// 也就是**不**暴露给实例。传反了勾选状态正好相反，而且不报错。
		FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VarName, !bInstanceEditable);
		Note(TEXT("instance_editable"));
	}

	bool bReadOnly = false;
	if (Payload->TryGetBoolField(TEXT("blueprint_read_only"), bReadOnly))
	{
		FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(Blueprint, VarName, bReadOnly);
		Note(TEXT("blueprint_read_only"));
	}

	FString Category;
	if (Payload->TryGetStringField(TEXT("category"), Category))
	{
		FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, VarName, nullptr, FText::FromString(Category));
		Note(TEXT("category"));
	}

	FString Tooltip;
	if (Payload->TryGetStringField(TEXT("tooltip"), Tooltip))
	{
		FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, FBlueprintMetadata::MD_Tooltip, Tooltip);
		Note(TEXT("tooltip"));
	}

	bool bExposeOnSpawn = false;
	if (Payload->TryGetBoolField(TEXT("expose_on_spawn"), bExposeOnSpawn))
	{
		if (bExposeOnSpawn)
		{
			// ExposeOnSpawn 只有在变量实例可编辑时才真的会出现在 SpawnActor 节点上。
			// 单独设它而不管可见性，用户会看到「设了没反应」。
			FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VarName, false);
			FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, FBlueprintMetadata::MD_ExposeOnSpawn, TEXT("true"));
		}
		else
		{
			FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(Blueprint, VarName, nullptr, FBlueprintMetadata::MD_ExposeOnSpawn);
		}
		Note(TEXT("expose_on_spawn"));
	}

	/**
	 * 联机复制。
	 *
	 * 没有 `SetVariableReplication` 这样的现成工具，编辑器自己也是直接改
	 * `PropertyFlags` —— 所以这里也直接改，但**必须重新取一次变量指针**：
	 * 上面几个 Set* 调用可能重排过 NewVariables 数组，早先拿的指针会失效。
	 */
	FString Replication;
	if (Payload->TryGetStringField(TEXT("replication"), Replication))
	{
		FString Unused;
		if (FBPVariableDescription* Var = UAL_FindMemberVariable(Blueprint, VarName, Unused))
		{
			const FString Mode = Replication.ToLower();
			if (Mode == TEXT("none"))
			{
				Var->PropertyFlags &= ~(CPF_Net | CPF_RepNotify);
				Var->RepNotifyFunc = NAME_None;
			}
			else if (Mode == TEXT("replicated"))
			{
				Var->PropertyFlags |= CPF_Net;
				Var->PropertyFlags &= ~CPF_RepNotify;
				Var->RepNotifyFunc = NAME_None;
			}
			else if (Mode == TEXT("repnotify"))
			{
				Var->PropertyFlags |= (CPF_Net | CPF_RepNotify);
				// RepNotify 必须有一个回调函数名，UE 的约定是 OnRep_<变量名>。
				// 不设的话编译报「RepNotify function not found」，而这个名字
				// 是能推出来的，没理由让调用方再传一次
				Var->RepNotifyFunc = FName(*FString::Printf(TEXT("OnRep_%s"), *VarNameStr));
			}
			else
			{
				UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
					TEXT("Unknown replication mode '%s'. Use None, Replicated or RepNotify"), *Replication));
				return;
			}
			Note(TEXT("replication"));
		}
	}

	if (Applied.Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT(
			"Nothing to change. Provide at least one of: instance_editable, blueprint_read_only, category, tooltip, expose_on_spawn, replication"));
		return;
	}

	// 只标脏，不走 MarkBlueprintAsStructurallyModified：那个会先跑一遍骨架编译，
	// 紧接着下面的完整编译又会重建一次，白编一遍
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	/**
	 * 改完就编译，不留给调用方。
	 *
	 * 上面那几个接口改的都是 NewVariables 里的变量描述，结构性修改只重建骨架类；
	 * 关卡里摆着的实例认的是 GeneratedClass 上的 FProperty 标记，不编译就还是旧的。
	 * 2026-09-24 的用户反馈：回执写着「实例可编辑=true」，紧接着给实例赋值被拒
	 * `cannot be edited on instances`，编一次才好 —— 回执先于事实报了成功。
	 * 联机复制同理，必须编译才生效。
	 */
	FCompilerResultsLog CompileResults;
	CompileResults.bSilentMode = true;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompileResults);

	// 改完重新查一遍再回：上面几个接口都可能被引擎二次调整
	// （比如 expose_on_spawn 会顺带打开可见性），回请求里的值会骗人。
	FString AfterAvailable;
	FBPVariableDescription* After = UAL_FindMemberVariable(Blueprint, VarName, AfterAvailable);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("name"), VarNameStr);
	Result->SetArrayField(TEXT("applied"), Applied);
	Result->SetBoolField(TEXT("compiled"), true);
	Result->SetNumberField(TEXT("compile_error_count"), CompileResults.NumErrors);
	if (After)
	{
		TSharedPtr<FJsonObject> VarJson = UAL_BuildVariableMetaJson(*After);

		// 实例可编辑以生成类上的属性为准 —— 实例赋值时引擎查的就是它。
		// 编译有错时生成类可能没换新，这时两边会不一致，照实回
		const FProperty* ClassProp = Blueprint->GeneratedClass
			? FindFProperty<FProperty>(Blueprint->GeneratedClass, VarName)
			: nullptr;
		if (ClassProp)
		{
			VarJson->SetBoolField(TEXT("instance_editable"),
				ClassProp->HasAnyPropertyFlags(CPF_Edit) && !ClassProp->HasAnyPropertyFlags(CPF_DisableEditOnInstance));
		}
		else
		{
			Result->SetStringField(TEXT("warning"), TEXT(
				"The variable is not on the compiled class yet (compile failed?). Instances cannot use the new settings until blueprint_compile succeeds."));
		}
		Result->SetObjectField(TEXT("variable"), VarJson);
	}
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

void FUAL_BlueprintCommands::Handle_RemoveVariable(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	FString VarNameStr;
	if (!Payload->TryGetStringField(TEXT("name"), VarNameStr) || VarNameStr.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: name"));
		return;
	}

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	const FName VarName(*VarNameStr);
	FString Available;
	if (!UAL_FindMemberVariable(Blueprint, VarName, Available))
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(
			TEXT("Variable '%s' not found on %s (available: %s)"), *VarNameStr, *Blueprint->GetName(), *Available));
		return;
	}

	FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VarName);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("name"), VarNameStr);
	Result->SetBoolField(TEXT("removed"), true);
	// 引用它的 Get/Set 节点会变成孤儿节点，编译才会报出来。
	// 不说的话调用方会以为删干净了。
	Result->SetStringField(TEXT("note"),
		TEXT("Nodes that referenced this variable are now orphaned - run blueprint.compile to see which graphs broke."));
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/**
 * 事件分发器 / 函数签名的参数类型解析。
 *
 * 只比通用的 `UAL_ResolvePinType` 多一件事：`type=object/class` 没给类名时默认 `Actor`
 * 而不是 `UObject` —— 事件分发器传的几乎都是场景里的东西，
 * 给 UObject 的话接收端还得再 Cast 一次。
 *
 * 这里以前是第三份手抄的类型表。合并掉了，但**它才是对的那一份**：
 * 另外两份把 float 写成 PC_Float 主类别（UE4 的旧形状），
 * 只有这份用了 PC_Real + 子类别。合并后统一按引擎的写法来。
 */
static bool UAL_ResolveDelegateParamType(const FString& TypeStr, const FString& ClassOrStruct, FEdGraphPinType& OutType, FString& OutError)
{
	const FString T = TypeStr.ToLower();
	const bool bWantsObject = (T == TEXT("object") || T == TEXT("class") || T == TEXT("soft_object") || T == TEXT("soft_class"));
	const FString EffectiveClass = (bWantsObject && ClassOrStruct.IsEmpty()) ? TEXT("/Script/Engine.Actor") : ClassOrStruct;
	return UAL_ResolvePinType(TypeStr, EffectiveClass, FString(), OutType, OutError);
}


void FUAL_BlueprintCommands::Handle_EventDispatcher(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	FString Action = TEXT("list");
	Payload->TryGetStringField(TEXT("action"), Action);
	Action = Action.ToLower();

	// ── list ────────────────────────────────────────────────────────────
	if (Action == TEXT("list"))
	{
		TArray<TSharedPtr<FJsonValue>> Dispatchers;
		for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
		{
			if (!Graph)
			{
				continue;
			}

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Graph->GetName());

			// 签名图的入口节点上挂着参数列表 —— 这是唯一能拿到参数名的地方
			TArray<TSharedPtr<FJsonValue>> Params;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node);
				if (!Entry)
				{
					continue;
				}
				for (const TSharedPtr<FUserPinInfo>& Pin : Entry->UserDefinedPins)
				{
					if (!Pin.IsValid())
					{
						continue;
					}
					TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
					P->SetStringField(TEXT("name"), Pin->PinName.ToString());
					// 同 UAL_AppendSignatureJson：回能填回去的类型串。
					// 绑定分发器要建一个签名一致的 CustomEvent，这份参数表会被
					// 原样抄进 blueprint_apply_graph 的 params 里
					UAL_WritePinTypeJson(Pin->PinType, P);
					Params.Add(MakeShared<FJsonValueObject>(P));
				}
			}
			Obj->SetArrayField(TEXT("params"), Params);
			Dispatchers.Add(MakeShared<FJsonValueObject>(Obj));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
		Result->SetArrayField(TEXT("dispatchers"), Dispatchers);
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	if (Action != TEXT("add"))
	{
		UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
			TEXT("Unknown action '%s'. Use \"add\" or \"list\""), *Action));
		return;
	}

	// ── add ─────────────────────────────────────────────────────────────
	FString NameStr;
	if (!Payload->TryGetStringField(TEXT("name"), NameStr) || NameStr.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: name (for action=add)"));
		return;
	}

	const FName DesiredName(*NameStr);
	for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
	{
		if (Graph && Graph->GetFName() == DesiredName)
		{
			UAL_CommandUtils::SendError(RequestId, 409, FString::Printf(
				TEXT("Event dispatcher already exists: %s"), *NameStr));
			return;
		}
	}

	// 先把参数类型全解析出来再动图 —— 解析到一半失败的话，蓝图里会留下一个
	// 没有参数的半成品分发器，比直接失败更难收拾
	struct FParamSpec
	{
		FName Name;
		FEdGraphPinType Type;
	};
	TArray<FParamSpec> ParamSpecs;

	const TArray<TSharedPtr<FJsonValue>>* ParamsJson = nullptr;
	if (Payload->TryGetArrayField(TEXT("params"), ParamsJson) && ParamsJson)
	{
		for (const TSharedPtr<FJsonValue>& Value : *ParamsJson)
		{
			const TSharedPtr<FJsonObject>* ParamObj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(ParamObj) || !ParamObj)
			{
				continue;
			}

			FString PName, PType, PClass;
			(*ParamObj)->TryGetStringField(TEXT("name"), PName);
			(*ParamObj)->TryGetStringField(TEXT("type"), PType);
			(*ParamObj)->TryGetStringField(TEXT("class"), PClass);

			if (PName.IsEmpty() || PType.IsEmpty())
			{
				UAL_CommandUtils::SendError(RequestId, 400, TEXT("Each param needs both \"name\" and \"type\""));
				return;
			}

			FParamSpec Spec;
			Spec.Name = FName(*PName);
			FString TypeError;
			if (!UAL_ResolveDelegateParamType(PType, PClass, Spec.Type, TypeError))
			{
				UAL_CommandUtils::SendError(RequestId, 400, TypeError);
				return;
			}
			ParamSpecs.Add(Spec);
		}
	}

	// 分发器 = 一个 MCDelegate 类型的成员变量 + 一张签名图。
	// 只建变量不建图的话，编辑器里能看见名字但连不出 Call / Bind 节点。
	FEdGraphPinType DelegateType;
	DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, DesiredName, DelegateType))
	{
		UAL_CommandUtils::SendError(RequestId, 500, FString::Printf(
			TEXT("Failed to create dispatcher variable '%s' - the name may collide with an existing member"), *NameStr));
		return;
	}

	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, DesiredName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	// 签名图不给用户编辑：它只描述参数，里面放逻辑不会被执行
	NewGraph->bEditable = false;

	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	K2Schema->CreateDefaultNodesForGraph(*NewGraph);
	K2Schema->CreateFunctionGraphTerminators(*NewGraph, static_cast<UClass*>(nullptr));
	K2Schema->AddExtraFunctionFlags(NewGraph, (FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public));
	K2Schema->MarkFunctionEntryAsEditable(NewGraph, true);

	Blueprint->DelegateSignatureGraphs.Add(NewGraph);

	// 参数挂在签名图的入口节点上
	TArray<TSharedPtr<FJsonValue>> AddedParams;
	for (UEdGraphNode* Node : NewGraph->Nodes)
	{
		UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node);
		if (!Entry)
		{
			continue;
		}

		for (const FParamSpec& Spec : ParamSpecs)
		{
			// 补默认值，理由同 blueprint.create_function 里那段注释：
			// CreateUserDefinedPin 留下的是空串，float 这类会让编译当场报错
			UEdGraphPin* NewPin = Entry->CreateUserDefinedPin(Spec.Name, Spec.Type, EGPD_Output);
			if (NewPin)
			{
				if (const UEdGraphSchema_K2* K2 = Cast<UEdGraphSchema_K2>(Entry->GetSchema()))
				{
					K2->SetPinAutogeneratedDefaultValueBasedOnType(NewPin);
				}
				Entry->UpdateUserDefinedPinDefaultValues();
			}
			AddedParams.Add(MakeShared<FJsonValueString>(Spec.Name.ToString()));
		}
		break;
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("name"), NameStr);
	Result->SetBoolField(TEXT("created"), true);
	Result->SetArrayField(TEXT("params"), AddedParams);
	Result->SetStringField(TEXT("note"),
		FString::Printf(
			TEXT("Broadcast it with node class \"CallDispatcher\", member_name \"%s\"; ")
			TEXT("subscribe with class \"BindEvent\" and wire a CustomEvent of the same signature into its \"Delegate\" pin. ")
			TEXT("(class \"Function\" with member_name \"Call %s\" / \"Bind Event to %s\" also works.)"),
			*NameStr, *NameStr, *NameStr));
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// blueprint.component_event
//
// 「走进触发区就开门」「点一下就捡起来」——这类需求的入口全在组件事件上。
// 此前一个都绑不了：组件加得上，但没办法在它的事件上挂逻辑，于是所有
// 触发式交互只能退化成 Tick 里每帧算距离。那是错的写法，而模型在没有
// 别的选择时就会那么写。
// ============================================================================

/**
 * 按变量名找蓝图上的一个组件。
 *
 * 找的是 SCS 节点而不是 CDO 上的属性：用户在组件面板里看到的名字就是
 * SCS 的变量名，而继承自父类的组件不在这个蓝图的 SCS 里 —— 后者绑不了，
 * 所以这里找不到就是真的绑不了，不是我们查漏了。
 */
static USCS_Node* UAL_FindComponentNode(UBlueprint* Blueprint, const FName& ComponentName, TArray<FString>& OutAvailable)
{
	USCS_Node* Found = nullptr;
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return nullptr;
	}

	for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
	{
		if (!Node)
		{
			continue;
		}
		OutAvailable.Add(Node->GetVariableName().ToString());
		if (Node->GetVariableName() == ComponentName)
		{
			Found = Node;
		}
	}
	return Found;
}

/** 委托签名 → JSON 参数列表。绑之前先让调用方看清楚事件会给它什么 */
static TArray<TSharedPtr<FJsonValue>> UAL_DelegateParamsJson(const FMulticastDelegateProperty* Delegate)
{
	TArray<TSharedPtr<FJsonValue>> Params;
	if (!Delegate || !Delegate->SignatureFunction)
	{
		return Params;
	}

	for (TFieldIterator<FProperty> It(Delegate->SignatureFunction); It && (It->PropertyFlags & CPF_Parm); ++It)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), It->GetName());
		Obj->SetStringField(TEXT("type"), It->GetCPPType());
		Params.Add(MakeShared<FJsonValueObject>(Obj));
	}
	return Params;
}

void FUAL_BlueprintCommands::Handle_ComponentEvent(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	FString ComponentNameStr;
	if (!Payload->TryGetStringField(TEXT("component_name"), ComponentNameStr) || ComponentNameStr.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: component_name"));
		return;
	}

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	const FName ComponentName(*ComponentNameStr);
	TArray<FString> AvailableComponents;
	USCS_Node* ComponentNode = UAL_FindComponentNode(Blueprint, ComponentName, AvailableComponents);
	if (!ComponentNode || !ComponentNode->ComponentTemplate)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(
			TEXT("Component '%s' not found on %s (available: %s)"),
			*ComponentNameStr, *Blueprint->GetName(),
			AvailableComponents.Num() ? *FString::Join(AvailableComponents, TEXT(", ")) : TEXT("(none)")));
		return;
	}

	UClass* ComponentClass = ComponentNode->ComponentTemplate->GetClass();

	// 只收 BlueprintAssignable 的委托。其余的蓝图里绑不上，
	// 列出来只会让调用方挑一个然后失败
	TMap<FString, FMulticastDelegateProperty*> Bindable;
	for (TFieldIterator<FMulticastDelegateProperty> It(ComponentClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_BlueprintAssignable))
		{
			Bindable.Add(It->GetName(), *It);
		}
	}

	FString Action = TEXT("list");
	Payload->TryGetStringField(TEXT("action"), Action);
	Action = Action.ToLower();

	// ── list ────────────────────────────────────────────────────────────
	if (Action == TEXT("list"))
	{
		TArray<TSharedPtr<FJsonValue>> Events;
		for (const TPair<FString, FMulticastDelegateProperty*>& Pair : Bindable)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Pair.Key);
			Obj->SetArrayField(TEXT("params"), UAL_DelegateParamsJson(Pair.Value));
			Events.Add(MakeShared<FJsonValueObject>(Obj));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
		Result->SetStringField(TEXT("component_name"), ComponentNameStr);
		Result->SetStringField(TEXT("component_class"), ComponentClass->GetName());
		Result->SetArrayField(TEXT("events"), Events);
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	if (Action != TEXT("add"))
	{
		UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
			TEXT("Unknown action '%s'. Use \"add\" or \"list\""), *Action));
		return;
	}

	// ── add ─────────────────────────────────────────────────────────────
	FString EventNameStr;
	if (!Payload->TryGetStringField(TEXT("event_name"), EventNameStr) || EventNameStr.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: event_name (for action=add)"));
		return;
	}

	FMulticastDelegateProperty** DelegateFound = Bindable.Find(EventNameStr);
	if (!DelegateFound || !*DelegateFound)
	{
		TArray<FString> Names;
		Bindable.GetKeys(Names);
		// 把这个组件**实际有哪些**可绑事件列出来。事件名很长又容易记混
		// （OnComponentBeginOverlap vs OnBeginOverlap），只说「没有」等于让调用方猜
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(
			TEXT("Event '%s' is not bindable on %s (available: %s)"),
			*EventNameStr, *ComponentClass->GetName(),
			Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("(none)")));
		return;
	}
	FMulticastDelegateProperty* DelegateProperty = *DelegateFound;

	FString GraphName;
	Payload->TryGetStringField(TEXT("graph_name"), GraphName);
	UEdGraph* Graph = UAL_FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(
			TEXT("Graph not found: %s"), GraphName.IsEmpty() ? TEXT("EventGraph") : *GraphName));
		return;
	}

	// 已经绑过就复用。重复绑同一个事件，编译不报错但两条链都会执行 ——
	// 和 BeginPlay 重复建节点是同一类坑
	const FName EventName(*EventNameStr);
	if (const UK2Node_ComponentBoundEvent* Existing =
			FKismetEditorUtilities::FindBoundEventForComponent(Blueprint, EventName, ComponentName))
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
		Result->SetStringField(TEXT("node_id"), Existing->NodeGuid.ToString());
		Result->SetStringField(TEXT("event_name"), EventNameStr);
		Result->SetBoolField(TEXT("reused"), true);
		Result->SetArrayField(TEXT("pins"), UAL_BuildPinsJson(const_cast<UK2Node_ComponentBoundEvent*>(Existing)));
		Result->SetStringField(TEXT("note"),
			TEXT("This event was already bound - reusing the existing node instead of creating a second one."));
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	// 组件在生成类上对应一个 FObjectProperty，节点靠它认自己绑的是谁。
	// 骨架类先查：改完还没编译时只有它是最新的
	FObjectProperty* ComponentProperty = nullptr;
	for (UClass* Candidate : { Blueprint->SkeletonGeneratedClass.Get(), Blueprint->GeneratedClass.Get() })
	{
		if (!Candidate)
		{
			continue;
		}
		ComponentProperty = FindFProperty<FObjectProperty>(Candidate, ComponentName);
		if (ComponentProperty)
		{
			break;
		}
	}

	if (!ComponentProperty)
	{
		UAL_CommandUtils::SendError(RequestId, 500, FString::Printf(
			TEXT("Component '%s' has no variable on the generated class yet. Compile the blueprint once (blueprint.compile) and retry."),
			*ComponentNameStr));
		return;
	}

	UK2Node_ComponentBoundEvent* NewNode = NewObject<UK2Node_ComponentBoundEvent>(Graph);
	NewNode->InitializeComponentBoundEventParams(ComponentProperty, DelegateProperty);
	NewNode->CreateNewGuid();

	// 放在图里已有内容的下方，别压在别的节点上
	int32 MaxY = 0;
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		MaxY = FMath::Max(MaxY, Node ? Node->NodePosY : 0);
	}
	NewNode->NodePosX = 0;
	NewNode->NodePosY = Graph->Nodes.Num() > 0 ? MaxY + 200 : 0;

	Graph->AddNode(NewNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
	NewNode->PostPlacedNewNode();
	NewNode->AllocateDefaultPins();

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("graph_name"), Graph->GetName());
	Result->SetStringField(TEXT("node_id"), NewNode->NodeGuid.ToString());
	Result->SetStringField(TEXT("event_name"), EventNameStr);
	Result->SetBoolField(TEXT("reused"), false);
	// 引脚原样回。调用方下一步就是把 then 接到自己的逻辑上，
	// 还要用 OtherActor 之类的数据引脚 —— 让它再查一次图是白花一个来回
	Result->SetArrayField(TEXT("pins"), UAL_BuildPinsJson(NewNode));
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// blueprint.function_signature / blueprint.set_parent_class
//
// 补的还是同一件事：建完就定死。函数加不了参数也删不掉，父类选错了改不回来 ——
// 两者都只能连着蓝图重来一遍，而「把图抄过去」本身就容易出错。
// ============================================================================

/** 函数图的入口节点。参数表挂在它身上 */
static UK2Node_FunctionEntry* UAL_FindFunctionEntry(UEdGraph* Graph)
{
	if (!Graph)
	{
		return nullptr;
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
		{
			return Entry;
		}
	}
	return nullptr;
}

/** 当前参数表快照。改完回给调用方核对，省一次读图 */
static void UAL_AppendSignatureJson(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Out)
{
	TArray<TSharedPtr<FJsonValue>> Inputs;
	TArray<TSharedPtr<FJsonValue>> Outputs;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		UK2Node_EditablePinBase* Editable = Cast<UK2Node_EditablePinBase>(Node);
		if (!Editable)
		{
			continue;
		}
		// 入口节点上的用户引脚是函数的**输入**，结果节点上的是**输出** ——
		// 两者都是 UK2Node_EditablePinBase，靠节点类型区分，不是靠引脚方向
		const bool bIsEntry = Node->IsA<UK2Node_FunctionEntry>();

		for (const TSharedPtr<FUserPinInfo>& Pin : Editable->UserDefinedPins)
		{
			if (!Pin.IsValid())
			{
				continue;
			}
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("name"), Pin->PinName.ToString());
			// 回的是**能原样填回 add_param 的**类型串，不是引擎的内部类别名。
			// 以前这里回 PinCategory（浮点回 "real"），调用方照抄回来会被顶回去
			UAL_WritePinTypeJson(Pin->PinType, P);
			(bIsEntry ? Inputs : Outputs).Add(MakeShared<FJsonValueObject>(P));
		}
	}

	Out->SetArrayField(TEXT("inputs"), Inputs);
	Out->SetArrayField(TEXT("outputs"), Outputs);
}

void FUAL_BlueprintCommands::Handle_FunctionSignature(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	FString GraphName;
	if (!Payload->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: graph_name (the function name)"));
		return;
	}

	FString Action;
	if (!Payload->TryGetStringField(TEXT("action"), Action) || Action.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400,
			TEXT("Missing required field: action (add_param | remove_param | remove_function)"));
		return;
	}
	Action = Action.ToLower();

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	UEdGraph* Graph = UAL_FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		TArray<FString> Names;
		for (const UEdGraph* G : Blueprint->FunctionGraphs)
		{
			if (G)
			{
				Names.Add(G->GetName());
			}
		}
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(
			TEXT("Function graph '%s' not found on %s (available: %s)"),
			*GraphName, *Blueprint->GetName(),
			Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("(none)")));
		return;
	}

	// ── remove_function ─────────────────────────────────────────────────
	if (Action == TEXT("remove_function"))
	{
		// 事件图删不得：它不是函数，删掉整个蓝图的事件逻辑就没了，
		// 而且这多半是调用方把 graph_name 填错了
		if (Graph->GetName().Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
		{
			UAL_CommandUtils::SendError(RequestId, 400,
				TEXT("Refusing to delete EventGraph - it is not a function. Pass the name of a function graph."));
			return;
		}

		FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
		Result->SetStringField(TEXT("graph_name"), GraphName);
		Result->SetStringField(TEXT("action"), Action);
		Result->SetStringField(TEXT("note"),
			TEXT("Call sites of this function are now orphaned - run blueprint.compile to see which graphs broke."));
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	UK2Node_FunctionEntry* Entry = UAL_FindFunctionEntry(Graph);
	if (!Entry)
	{
		UAL_CommandUtils::SendError(RequestId, 500, FString::Printf(
			TEXT("Graph '%s' has no function entry node - it may not be a function graph"), *GraphName));
		return;
	}

	const TSharedPtr<FJsonObject>* ParamObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("param"), ParamObj) || !ParamObj)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: param"));
		return;
	}

	FString ParamName;
	if (!(*ParamObj)->TryGetStringField(TEXT("name"), ParamName) || ParamName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("param needs a \"name\""));
		return;
	}

	FString Direction = TEXT("in");
	(*ParamObj)->TryGetStringField(TEXT("direction"), Direction);
	const bool bIsOutput = Direction.ToLower() == TEXT("out");

	// 输出参数挂在结果节点上，不是入口节点。挂错地方的话参数建出来了，
	// 但函数调用节点上根本看不到它
	UK2Node_EditablePinBase* Target = Entry;
	if (bIsOutput)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
			{
				Target = ResultNode;
				break;
			}
		}
		if (Target == Entry)
		{
			UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
				TEXT("Function '%s' has no result node, so it cannot take output parameters. "
					 "Give it a return value in the editor first, or use an input parameter."), *GraphName));
			return;
		}
	}

	// ── remove_param ────────────────────────────────────────────────────
	if (Action == TEXT("remove_param"))
	{
		const FName PinName(*ParamName);
		TArray<FString> Existing;
		bool bFound = false;
		for (const TSharedPtr<FUserPinInfo>& Pin : Target->UserDefinedPins)
		{
			if (Pin.IsValid())
			{
				Existing.Add(Pin->PinName.ToString());
				bFound |= Pin->PinName == PinName;
			}
		}

		if (!bFound)
		{
			UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(
				TEXT("Parameter '%s' not found on %s (%s side). Existing: %s"),
				*ParamName, *GraphName, bIsOutput ? TEXT("output") : TEXT("input"),
				Existing.Num() ? *FString::Join(Existing, TEXT(", ")) : TEXT("(none)")));
			return;
		}

		Target->RemoveUserDefinedPinByName(PinName);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
		Result->SetStringField(TEXT("graph_name"), GraphName);
		Result->SetStringField(TEXT("action"), Action);
		UAL_AppendSignatureJson(Graph, Result);
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	if (Action != TEXT("add_param"))
	{
		UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
			TEXT("Unknown action '%s'. Use add_param, remove_param or remove_function"), *Action));
		return;
	}

	// ── add_param ───────────────────────────────────────────────────────
	FString TypeStr;
	if (!(*ParamObj)->TryGetStringField(TEXT("type"), TypeStr) || TypeStr.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("param needs a \"type\" for add_param"));
		return;
	}

	FString ClassOrStruct;
	(*ParamObj)->TryGetStringField(TEXT("class"), ClassOrStruct);

	// 和事件分发器共用一套类型解析，两处支持的类型必须一致 ——
	// 不一致的话「分发器能传 vector，函数不能」这种事没人说得清为什么
	FEdGraphPinType PinType;
	FString TypeError;
	if (!UAL_ResolveDelegateParamType(TypeStr, ClassOrStruct, PinType, TypeError))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TypeError);
		return;
	}

	const FName PinName(*ParamName);
	for (const TSharedPtr<FUserPinInfo>& Pin : Target->UserDefinedPins)
	{
		if (Pin.IsValid() && Pin->PinName == PinName)
		{
			UAL_CommandUtils::SendError(RequestId, 409, FString::Printf(
				TEXT("Parameter '%s' already exists on %s"), *ParamName, *GraphName));
			return;
		}
	}

	// 入口节点的用户引脚是输出方向（函数体从这里取值），结果节点反之。
	// 这一条反直觉，写反了引脚会长在错误的一侧
	UEdGraphPin* NewPin = Target->CreateUserDefinedPin(PinName, PinType, bIsOutput ? EGPD_Input : EGPD_Output);

	// 补默认值，理由同 blueprint.create_function 里那段注释：
	// CreateUserDefinedPin 留下的是空串，float 这类会让编译当场报错
	if (NewPin)
	{
		if (const UEdGraphSchema_K2* K2 = Cast<UEdGraphSchema_K2>(Target->GetSchema()))
		{
			K2->SetPinAutogeneratedDefaultValueBasedOnType(NewPin);
		}
		Target->UpdateUserDefinedPinDefaultValues();
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("graph_name"), GraphName);
	Result->SetStringField(TEXT("action"), Action);
	UAL_AppendSignatureJson(Graph, Result);
	Result->SetStringField(TEXT("note"),
		TEXT("Existing call sites of this function need re-wiring for the new pin - run blueprint.compile."));
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

void FUAL_BlueprintCommands::Handle_SetParentClass(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	FString ParentClassName;
	if (!Payload->TryGetStringField(TEXT("parent_class"), ParentClassName) || ParentClassName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: parent_class"));
		return;
	}

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	FString ClassError;
	UClass* NewParent = UAL_CommandUtils::ResolveClassFromIdentifier(ParentClassName, UObject::StaticClass(), ClassError);
	if (!NewParent)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(
			TEXT("Parent class not found: %s (%s)"), *ParentClassName, *ClassError));
		return;
	}

	// 自己不能当自己的父类，也不能挂到自己的子类上 —— 循环继承会让引擎在
	// 编译时崩，而不是报错
	if (NewParent == Blueprint->GeneratedClass || (Blueprint->GeneratedClass && NewParent->IsChildOf(Blueprint->GeneratedClass)))
	{
		UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
			TEXT("'%s' derives from this blueprint - reparenting to it would create a cycle"), *ParentClassName));
		return;
	}

	const FString OldParent = Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : TEXT("(none)");
	if (Blueprint->ParentClass == NewParent)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
		Result->SetStringField(TEXT("old_parent"), OldParent);
		Result->SetStringField(TEXT("new_parent"), NewParent->GetName());
		Result->SetStringField(TEXT("note"), TEXT("Already had this parent - nothing changed."));
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	TOptional<FUAL_ScopedTransaction> Transaction;
	Transaction.Emplace(NSLOCTEXT("UALBlueprint", "Reparent", "Reparent Blueprint"));
	Blueprint->Modify();
	Blueprint->ParentClass = NewParent;

	// 换父类之后必须刷节点再编译。少了 RefreshAllNodes，引用旧父类成员的节点
	// 会保持旧的引脚布局，编译报出来的错会指向一堆看起来无关的地方
	FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	// 编译在事务外跑：事务里做完整编译会把关卡 Actor 列表卷进这一笔，
	// 撤销时实例会凭空消失。详见 Handle_CreateGraphDeclarative 里同一处的长注释。
	Transaction.Reset();

	FCompilerResultsLog ResultsLog;
	ResultsLog.bSilentMode = true;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &ResultsLog);

	// 改父类**大概率**会打断一批节点。不把编译结果带回去，调用方会拿到一个
	// 「成功」，然后在别处撞见一堆莫名其妙的编译错
	TArray<TSharedPtr<FJsonValue>> Messages;
	for (const TSharedRef<FTokenizedMessage>& Msg : ResultsLog.Messages)
	{
		if (Msg->GetSeverity() == EMessageSeverity::Error || Msg->GetSeverity() == EMessageSeverity::Warning)
		{
			Messages.Add(MakeShared<FJsonValueString>(Msg->ToText().ToString()));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("old_parent"), OldParent);
	Result->SetStringField(TEXT("new_parent"), NewParent->GetName());
	Result->SetNumberField(TEXT("compile_errors"), ResultsLog.NumErrors);
	Result->SetNumberField(TEXT("compile_warnings"), ResultsLog.NumWarnings);
	Result->SetArrayField(TEXT("messages"), Messages);
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// blueprint.export_t3d / blueprint.import_t3d
//
// 这一对走的是**引擎自己的节点序列化**，也就是编辑器里 Ctrl+C / Ctrl+V
// 那条路（FEdGraphUtilities::ExportNodesToText / ImportNodesFromText，
// FBlueprintEditor::PasteNodesHere 调的同一对函数）。
//
// 和 get_graph / create_graph 那条结构化路线的根本区别：
// 结构化路线是「读侧描述节点 → 写侧照描述**重建**一个默认形态的节点」，
// 所以用户改过形状的东西（手动加过输出引脚的 Sequence、折叠成 Composite 的
// 一整块逻辑）写回去就变回默认形态，多出来的部分静默丢失。
// T3D 这条路不重建，引擎自己反序列化 —— 编辑器里复制粘贴保得住的，这里都保得住。
//
// **目前只用于路线验证，还没有接到任何产品路径上。**
// 验通了再决定要不要把「片段放进当前工程」整条线换过来。
// ============================================================================

/** T3D 文本的回传上限。超了截断并置 truncated，绝不静默截。 */
static constexpr int32 UAL_T3D_MAX_CHARS = 200000;

void FUAL_BlueprintCommands::Handle_ExportNodesT3D(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	FString GraphName;
	Payload->TryGetStringField(TEXT("graph_name"), GraphName);
	UEdGraph* Graph = UAL_FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(
			RequestId, 404,
			FString::Printf(TEXT("Graph not found: %s"), GraphName.IsEmpty() ? TEXT("EventGraph") : *GraphName));
		return;
	}

	// node_ids 不给就导整张图。给了就只导点到的那几个 ——
	// 和编辑器里「框选一部分再 Ctrl+C」是同一个语义：
	// 指向选区外的连线会变成悬空端口，这是预期行为，不是丢数据。
	TSet<UObject*> NodesToExport;
	TArray<TSharedPtr<FJsonValue>> ExportedIds;
	TArray<TSharedPtr<FJsonValue>> NotFound;

	const TArray<TSharedPtr<FJsonValue>>* NodeIdsArray = nullptr;
	if (Payload->TryGetArrayField(TEXT("node_ids"), NodeIdsArray) && NodeIdsArray && NodeIdsArray->Num() > 0)
	{
		for (const TSharedPtr<FJsonValue>& Value : *NodeIdsArray)
		{
			FString NodeId;
			if (!Value.IsValid() || !Value->TryGetString(NodeId) || NodeId.IsEmpty())
			{
				continue;
			}

			UEdGraphNode* Node = UAL_FindNodeByGuid(Graph, NodeId);
			if (!Node)
			{
				// 找不到的单列出来，不静默跳过 —— 调用方多半拿着一份过期的图
				NotFound.Add(MakeShared<FJsonValueString>(NodeId));
				continue;
			}

			NodesToExport.Add(Node);
			ExportedIds.Add(MakeShared<FJsonValueString>(UAL_GuidToString(Node->NodeGuid)));
		}
	}
	else
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			NodesToExport.Add(Node);
			ExportedIds.Add(MakeShared<FJsonValueString>(UAL_GuidToString(Node->NodeGuid)));
		}
	}

	if (NodesToExport.Num() == 0)
	{
		UAL_CommandUtils::SendError(
			RequestId, 400,
			TEXT("Nothing to export - the graph is empty, or none of the given node_ids are in it."));
		return;
	}

	FString ExportedText;
	FEdGraphUtilities::ExportNodesToText(NodesToExport, ExportedText);

	const int32 FullLength = ExportedText.Len();
	const bool bTruncated = FullLength > UAL_T3D_MAX_CHARS;
	if (bTruncated)
	{
		// 截断过的 T3D **粘不回去**，所以必须明说，不能让调用方拿去当完整文本用
		ExportedText = ExportedText.Left(UAL_T3D_MAX_CHARS);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), NotFound.Num() == 0);
	Result->SetStringField(TEXT("blueprint_path"), ResolvedPath);
	Result->SetStringField(TEXT("graph_name"), Graph->GetName());
	Result->SetNumberField(TEXT("node_count"), NodesToExport.Num());
	Result->SetArrayField(TEXT("exported_node_ids"), ExportedIds);
	Result->SetStringField(TEXT("text"), ExportedText);
	Result->SetNumberField(TEXT("text_length"), FullLength);
	Result->SetBoolField(TEXT("truncated"), bTruncated);
	if (bTruncated)
	{
		Result->SetStringField(
			TEXT("note"),
			TEXT("Text was truncated and is NOT valid for import - export a smaller node_ids subset instead."));
	}
	if (NotFound.Num() > 0)
	{
		Result->SetArrayField(TEXT("not_found"), NotFound);
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

void FUAL_BlueprintCommands::Handle_ImportNodesT3D(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	FString TextToImport;
	if (!Payload->TryGetStringField(TEXT("text"), TextToImport) || TextToImport.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: text"));
		return;
	}

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	FString GraphName;
	Payload->TryGetStringField(TEXT("graph_name"), GraphName);
	UEdGraph* Graph = UAL_FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(
			RequestId, 404,
			FString::Printf(TEXT("Graph not found: %s"), GraphName.IsEmpty() ? TEXT("EventGraph") : *GraphName));
		return;
	}

	// ===== 1. 先问引擎收不收这段文本 =====
	// 编辑器灰掉「粘贴」菜单用的就是这个判断。放在最前面，免得走到事务里才发现。
	if (!FEdGraphUtilities::CanImportNodesFromText(Graph, TextToImport))
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("graph_name"), Graph->GetName());
		Details->SetNumberField(TEXT("text_length"), TextToImport.Len());
		UAL_CommandUtils::SendError(
			RequestId, 400,
			FString::Printf(
				TEXT("This graph will not accept that text (CanImportNodesFromText said no). ")
				TEXT("Either it is not node export text, or those node types are not allowed in graph '%s'. Nothing was written."),
				*Graph->GetName()),
			Details);
		return;
	}

	// ===== 2. require_empty =====
	// 和 create_graph 共用同一个判据函数，同样在**进事务之前**查：不空就一个字都不写
	bool bRequireEmpty = false;
	Payload->TryGetBoolField(TEXT("require_empty"), bRequireEmpty);
	if (bRequireEmpty)
	{
		FString NotEmptyReason;
		if (!UAL_IsGraphEmptyForImport(Graph, NotEmptyReason))
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("graph_name"), Graph->GetName());
			Details->SetStringField(TEXT("reason"), NotEmptyReason);
			UAL_CommandUtils::SendError(
				RequestId, 409,
				FString::Printf(
					TEXT("require_empty: graph '%s' is not empty - %s. Nothing was written."),
					*Graph->GetName(), *NotEmptyReason),
				Details);
			return;
		}
	}

	int32 OffsetX = 0;
	int32 OffsetY = 0;
	Payload->TryGetNumberField(TEXT("offset_x"), OffsetX);
	Payload->TryGetNumberField(TEXT("offset_y"), OffsetY);

	// ===== 3. 进事务 =====
	// TOptional 是为了能在编译之前提前结束这一笔，见下面 Transaction.Reset() 处
	TOptional<FUAL_ScopedTransaction> Transaction;
	Transaction.Emplace(NSLOCTEXT("UnrealAgentLink", "ImportBlueprintNodesT3D", "Paste Blueprint Nodes"));
	const bool bUndoable = Transaction->IsOutstanding();

	Blueprint->Modify();
	Graph->Modify();

	TSet<UEdGraphNode*> ImportedNodes;
	FEdGraphUtilities::ImportNodesFromText(Graph, TextToImport, ImportedNodes);

	if (ImportedNodes.Num() == 0)
	{
		// CanImport 说行、实际一个都没进来 —— 报成功会骗人。
		// 撤销事务，免得在用户的撤销栈上留一条什么都没干的记录
		Transaction->Cancel();
		UAL_CommandUtils::SendError(
			RequestId, 422,
			TEXT("The engine accepted the text but imported zero nodes. Nothing was written."));
		return;
	}

	// ===== 4. 收尾，照编辑器 PasteNodesHere 的顺序来 =====
	TArray<TSharedPtr<FJsonValue>> NodesJson;
	TArray<TSharedPtr<FJsonValue>> SelfContextUnresolved;
	TArray<TSharedPtr<FJsonValue>> RemovedGhostEvents;
	bool bStructural = false;

	for (UEdGraphNode* Node : ImportedNodes)
	{
		if (!Node)
		{
			continue;
		}

		Node->NodePosX += OffsetX;
		Node->NodePosY += OffsetY;

		// 换一个新 GUID —— 不换的话，同一段文本粘两次，
		// 图里会出现两个 GUID 相同的节点，之后按 id 找节点全部失准
		Node->CreateNewGuid();

		if (const UK2Node* K2Node = Cast<UK2Node>(Node))
		{
			if (K2Node->NodeCausesStructuralBlueprintChange())
			{
				bStructural = true;
			}
		}

		// 编辑器在这一步会弹模态框让用户手工修这类节点。
		// 自动化路径不能弹框，所以只如实报告，让调用方决定怎么办。
		if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
		{
			if (CallNode->FunctionReference.IsSelfContext() && !CallNode->GetTargetFunction())
			{
				SelfContextUnresolved.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("%s (%s)"),
						*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
						*UAL_GuidToString(Node->NodeGuid))));
			}
		}

		/**
		 * 粘进来一个事件节点时，如果图里原来就有一个**引擎自己铺的**同名事件
		 * （新建 Actor 蓝图自带的那三个灰色 BeginPlay / Tick / ActorBeginOverlap），
		 * 要把那个旧的清掉。不清的话蓝图里出现两个 BeginPlay，编译直接报重复事件。
		 *
		 * 编辑器的粘贴在这一步做的就是这件事（`PasteNodesHere` 里
		 * `AreEventNodesIdentical` 那一段），这里照抄。
		 *
		 * **只清引擎自己铺的那种。** 编辑器那边写的是 `ensure(...)`，
		 * 也就是「本来就不该走到这儿」；我们改成硬条件：用户自己放的同名事件
		 * 一个都不动，只把清掉的记下来回给调用方 —— 删用户的节点得让他看得见。
		 */
		if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			TArray<UK2Node_Event*> ExistingEventNodes;
			FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_Event>(Blueprint, ExistingEventNodes);

			for (UK2Node_Event* Existing : ExistingEventNodes)
			{
				if (!Existing || Existing == EventNode || !Existing->bOverrideFunction)
				{
					continue;
				}
				if (!Existing->IsAutomaticallyPlacedGhostNode())
				{
					continue;
				}
				if (!UK2Node_Event::AreEventNodesIdentical(EventNode, Existing))
				{
					continue;
				}

				RemovedGhostEvents.Add(MakeShared<FJsonValueString>(
					Existing->GetNodeTitle(ENodeTitleType::ListView).ToString()));
				Existing->DestroyNode();
			}
		}

		TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
		NodeJson->SetStringField(TEXT("id"), UAL_GuidToString(Node->NodeGuid));
		NodeJson->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		NodeJson->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		NodeJson->SetNumberField(TEXT("x"), Node->NodePosX);
		NodeJson->SetNumberField(TEXT("y"), Node->NodePosY);
		NodeJson->SetBoolField(TEXT("is_ghost_node"), Node->IsAutomaticallyPlacedGhostNode());
		NodesJson.Add(MakeShared<FJsonValueObject>(NodeJson));
	}

	// 结构性改动（事件、函数入口那类）要重跑骨架编译，普通的不用 ——
	// 每次都走结构性的话，大蓝图上每粘一次就白等好几秒
	if (bStructural)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}
	else
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	// 蓝图正开在编辑器里时，不通知的话用户看到的还是改之前的图
	Graph->NotifyGraphChanged();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_path"), ResolvedPath);
	Result->SetStringField(TEXT("graph_name"), Graph->GetName());
	Result->SetNumberField(TEXT("imported_count"), NodesJson.Num());
	Result->SetArrayField(TEXT("nodes"), NodesJson);
	Result->SetBoolField(TEXT("structural"), bStructural);
	// 事务拿不到时用户撤不回这次改动，如实说，不假装
	Result->SetBoolField(TEXT("undoable"), bUndoable);

	if (RemovedGhostEvents.Num() > 0)
	{
		// 删了东西就得说，哪怕删的是引擎自己铺的占位节点
		Result->SetArrayField(TEXT("removed_ghost_events"), RemovedGhostEvents);
		Result->SetStringField(
			TEXT("removed_ghost_events_note"),
			TEXT("These were engine-placed placeholder event nodes in the target graph, replaced by the pasted ones. ")
			TEXT("The editor's own paste does the same thing. User-placed event nodes are never touched."));
	}

	if (SelfContextUnresolved.Num() > 0)
	{
		Result->SetArrayField(TEXT("self_context_unresolved"), SelfContextUnresolved);
		Result->SetStringField(
			TEXT("self_context_note"),
			TEXT("These nodes call a function on the source blueprint's own class, which does not exist here. ")
			TEXT("They are pasted but broken. The editor's own paste shows a fixup dialog at this point; ")
			TEXT("this command cannot, so it reports instead."));
	}

	// 编译在事务外跑：事务里做完整编译会把关卡 Actor 列表卷进这一笔，
	// 撤销时实例会凭空消失。详见 Handle_CreateGraphDeclarative 里同一处的长注释。
	Transaction.Reset();

	bool bCompile = false;
	Payload->TryGetBoolField(TEXT("compile"), bCompile);
	if (bCompile)
	{
		FCompilerResultsLog CompileResults;
		CompileResults.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompileResults);

		Result->SetBoolField(TEXT("compiled"), true);
		Result->SetNumberField(TEXT("compile_error_count"), CompileResults.NumErrors);
		Result->SetNumberField(TEXT("compile_warning_count"), CompileResults.NumWarnings);

		TArray<TSharedPtr<FJsonValue>> Diagnostics;
		for (const TSharedRef<FTokenizedMessage>& Message : CompileResults.Messages)
		{
			const EMessageSeverity::Type Severity = Message->GetSeverity();
			if (Severity != EMessageSeverity::Error && Severity != EMessageSeverity::Warning)
			{
				continue;
			}

			TSharedPtr<FJsonObject> Diag = MakeShared<FJsonObject>();
			Diag->SetStringField(TEXT("severity"), Severity == EMessageSeverity::Error ? TEXT("error") : TEXT("warning"));
			Diag->SetStringField(TEXT("message"), Message->ToText().ToString());
			Diagnostics.Add(MakeShared<FJsonValueObject>(Diag));
		}
		Result->SetArrayField(TEXT("diagnostics"), Diagnostics);
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}
