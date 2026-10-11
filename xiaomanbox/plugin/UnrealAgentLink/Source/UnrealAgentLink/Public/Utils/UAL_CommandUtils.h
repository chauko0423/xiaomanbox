#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
// 命令文件几乎都要读写 JSON 字符串。以前各自靠 unity build 同一块里别的 .cpp 顺带引进来 ——
// 新增一个 .cpp 就会把分块重新洗一遍，UAL_WidgetCommands.cpp 在 5.0 上就这样突然找不到 TJsonReader
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/**
 * 把 JSON 对象的键取成 FString。
 *
 * UE 5.8 起 FJsonObject 的键类型从 FString 换成了 UE::FSharedString（让反复出现的
 * 键共用一份字符串内存），所以 `FString Name = Pair.Key;` 这种写法在 5.8 上编不过。
 * 两种类型都支持 `operator*` 拿裸指针，绕这一道就能一份代码喂给 5.0–5.8。
 *
 * 不走引擎给的 UE_JSONOBJECT_LEGACY_STRING_KEYS 开关：它改的是 FJsonObject 的成员
 * 布局，而 Json 模块是 Epic 按默认值预编译好发下来的 —— 只在插件这边翻，两边对
 * FJsonObject 的认知就不一致了，编得过也会在运行时炸。
 */
template <typename JsonKeyType>
FORCEINLINE FString UAL_JsonKey(const JsonKeyType& Key)
{
	return FString(*Key);
}

class UAL_CommandUtils
{
public:
	static bool IsZh();
	static FString LStr(const TCHAR* Zh, const TCHAR* En);
	static FText LText(const TCHAR* Zh, const TCHAR* En);

	// 批量创建上限（默认 50，可通过控制台变量 ual.MaxBatchCreate 调整；<=0 表示不限制）
	static int32 GetMaxBatchCreate();

	/**
	 * 编辑器世界。**不管 PIE 有没有在跑，永远是关卡编辑器里那一个。**
	 *
	 * 只在动作确实针对编辑器时用（打开关卡、存盘、视口操作、Sequencer）。
	 * 「这个 Actor 现在在哪」这类问题不要用它 —— 见 GetLiveWorld()。
	 */
	static UWorld* GetTargetWorld();

	/**
	 * 当下该看的世界：**PIE 在跑就是正在跑的那个游戏，没跑才是编辑器世界。**
	 *
	 * ## 为什么必须有这个区分
	 *
	 * 在此之前，除截图外**每一条命令**都走 GetTargetWorld()，也就是永远读编辑器世界。
	 * 后果是：游戏跑起来之后问「玩家在哪」「门开了吗」「生成了几个敌人」，
	 * 拿回来的是关卡里那份**没在动的模板**，而且不报错。
	 *
	 * 模型据此下运行时结论，会得出完全错误的判断 —— 这比读不到更糟，
	 * 因为它看起来是成功的。截图那条路 2026 年就已经按 PIE 判了
	 * （见 UAL_EditorCommands.cpp 的 Handle_Screenshot），其余命令一直没跟上。
	 *
	 * 判据是客观的（有没有 PIE 世界），没有需要人来定的地方，所以是自动的，
	 * 不做成必填参数。但**返回里必须回报这次读的是哪个世界**（WorldKindName），
	 * 否则调用方分不清「Actor 不存在」和「在另一个世界里」。
	 *
	 * @param PlayerIndex 多客户端 PIE 时取第几个游戏实例，默认 0。
	 *                    越界时返回第一个，并不假装成功 —— 调用方要自己核对 WorldKindName。
	 */
	static UWorld* GetLiveWorld(int32 PlayerIndex = 0);

	/** PIE（或 Simulate）此刻在不在跑 */
	static bool IsPlayInProgress();

	/** 有几个 PIE 世界。多客户端时 > 1，调用方据此决定要不要要求显式 player_index */
	static int32 GetPlayWorldCount();

	/** 给返回值用的世界名：`pie` / `editor` / `unknown`。**每个碰世界的命令都要回报它** */
	static const TCHAR* WorldKindName(const UWorld* World);

	/**
	 * 往返回值里盖一个「这次读/写的是哪个世界」的戳。
	 *
	 * **凡是走 GetLiveWorld() 的命令，发响应之前都要调它。**
	 * 没有这个戳，调用方分不清「这个 Actor 不存在」和「它在另一个世界里」；
	 * 而 PIE 一开一关，同一条查询会给出两个不同的答案。
	 *
	 * 盖三样：
	 *   world          `pie` / `editor`
	 *   world_note     PIE 时的一句人话：改动不落盘、停止就没了
	 *   play_world_count 多客户端 PIE 时 > 1，提醒调用方结论只针对其中一个
	 */
	static void AddWorldInfo(const TSharedPtr<FJsonObject>& Data);

	/**
	 * 「这条命令在 PIE 期间做不了」—— 是就发一条说清楚的错误并返回 true。
	 *
	 * 只给**纯编辑器动作**用：存关卡、开关卡、新建关卡、整理大纲文件夹。
	 * 这些东西在运行中的游戏里要么没有意义（PIE 世界没有大纲文件夹），
	 * 要么会把用户的会话搅烂（PIE 跑着换关卡）。
	 *
	 * **不要拿它去挡 spawn / set_property 这类**：在运行中的游戏里改东西
	 * 是正当的现场调试，挡掉等于把能力做窄了。那些命令的正解是照做，
	 * 但在返回里说清「这次改的是运行中的世界，停止就没了」。
	 *
	 * @return true = 已经发过错误响应，调用方直接 return
	 */
	static bool RefuseDuringPlay(const FString& RequestId, const TCHAR* ZhWhat, const TCHAR* EnWhat);

	static FVector ReadVector(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const FVector& DefaultValue = FVector::ZeroVector);
	static FRotator ReadRotator(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const FRotator& DefaultValue = FRotator::ZeroRotator);
	static FVector ReadVectorDirect(const TSharedPtr<FJsonObject>& Obj, const FVector& DefaultValue = FVector::ZeroVector);
	static FRotator ReadRotatorDirect(const TSharedPtr<FJsonObject>& Obj, const FRotator& DefaultValue = FRotator::ZeroRotator);
	
	static bool TryGetObjectFieldFlexible(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Field, TSharedPtr<FJsonObject>& OutObj);

	struct FUALSpawnPreset
	{
		FName Key;
		UClass* Class; // TSubclassOf<AActor>
		const TCHAR* AssetPath;
	};
	static bool ResolvePreset(const FString& Name, FUALSpawnPreset& OutPreset);
	
	static bool SetStaticMeshIfNeeded(AActor* Actor, const TCHAR* MeshPath);

	struct FUALResolvedSpawnRequest
	{
		UClass* SpawnClass = nullptr;
		FString MeshPath;
		FString ResolvedType;
		FString SourceId;
		bool bFromAlias = false;
	};
	static bool ResolveSpawnFromAssetId(const FString& AssetId, FUALResolvedSpawnRequest& OutResolved, FString& OutError);

	static void ReadTransformFromItem(const TSharedPtr<FJsonObject>& Item, FVector& OutLocation, FRotator& OutRotation, FVector& OutScale);

	static AActor* FindActorByLabel(UWorld* World, const FString& Label);
	static FString GetActorFriendlyName(AActor* Actor);

	static TSharedPtr<FJsonObject> MakeVectorJson(const FVector& Vec);
	static TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& Rot);
	
	static TSharedPtr<FJsonValue> PropertyToJsonValueCompat(FProperty* Prop, const void* ValuePtr);
	
	static TSharedPtr<FJsonObject> BuildActorInfo(AActor* Actor);
	static TSharedPtr<FJsonObject> BuildActorInfoWithOptions(AActor* Actor, bool bIncludeTransform, bool bIncludeBounds);

	static bool ShouldIncludeActor(const AActor* Actor, const FString& NameKeyword, bool bNameExact, const FString& ClassKeyword, bool bClassExact);

	static bool ShouldIncludeActorAdvanced(
		const AActor* Actor,
		const FString& NameContains,
		const FString& NameNotContains,
		const FString& ClassContains,
		const FString& ClassNotContains,
		const FString& ClassExact,
		const TArray<FString>& ExcludeClasses);

	static UClass* ResolveClassFromIdentifier(const FString& Identifier, UClass* ExpectedBase, FString& OutError);

	/**
	 * `targets` 选择器 → 一组 Actor。
	 *
	 * `OutUnmatched` 给了的话，`names` / `paths` 里**一个 Actor 都没对上的那些字符串**
	 * 会原样收进去。传 nullptr 等于不关心。
	 *
	 * 为什么必须有这个出参：点名 7 个只认出 6 个时，少的那一个以前是**静默丢掉**的 ——
	 * 响应里只有认出来的 6 个，没有任何字段说第 7 个没找到。调用方（AI 或脚本）
	 * 拿到一个「成功」，以为 7 个都办了。这条路上挂着 actor.destroy：
	 * 删 7 个只删掉 6 个而不吭声，剩下那个要到很久以后才被发现还活着。
	 */
	static bool ResolveTargetsToActors(const TSharedPtr<FJsonObject>& Targets, UWorld* World, TSet<AActor*>& OutSet, FString& OutError, TArray<FString>* OutUnmatched = nullptr);

	/**
	 * 把 `ResolveTargetsToActors` 收集到的「没对上的名字」写进响应。
	 *
	 * 空数组时**什么都不加** —— 正常情况下不该有这个字段占位。
	 */
	static void AddUnmatchedTargets(const TSharedPtr<FJsonObject>& Data, const TArray<FString>& Unmatched);

	/**
	 * 这个 Actor 是不是引擎自己的记账对象。
	 *
	 * 判据是一张具名表（见 .cpp 里的 SystemActorRules），不是启发式 —— 表里每一条
	 * 都写了为什么在表里。按类名字符串匹配而不是 UClass 指针：这份源码要同时编过
	 * 5.0–5.8，其中几个类在老版本上根本不存在（如 WorldPartitionMiniMapVolume），
	 * 拿指针就得加 #if 版本分支和模块依赖，按名字匹配则「查无此类 = 不匹配」，天然安全。
	 */
	static bool IsSystemActor(const AActor* Actor);

	/**
	 * 从结果集里剔除系统 Actor。
	 *
	 * @param OutRemovedByClass 按**真实类名**分组的剔除计数，用来如实回报
	 *                          「另有 N 个未列出」—— 静默过滤会让调用方以为
	 *                          「场景里就这些」，那是更难查的一类错。
	 * @return 剔除的总数
	 */
	static int32 ExcludeSystemActors(TSet<AActor*>& InOutSet, TMap<FString, int32>& OutRemovedByClass);

	/**
	 * targets 是不是「扫场景」——没点名任何具体 Actor，只给了 filter（或什么都没给）。
	 *
	 * 只有扫场景才该隐藏系统 Actor。调用方按 names/paths 点名要一个 HLOD、
	 * 或者用户自己在大纲里选中了它，那就是真的想要它，这时候还过滤属于抗命。
	 */
	static bool IsScanTargets(const TSharedPtr<FJsonObject>& Targets);

	/** 把「隐藏了多少系统 Actor」写进响应；一个都没隐藏时不写，免得平白多两个字段 */
	static void AddSystemActorExclusionInfo(const TSharedPtr<FJsonObject>& Data, int32 Excluded, const TMap<FString, int32>& RemovedByClass);

	/**
	 * 检查 Actor 的属性是否匹配指定条件
	 * @param Actor 要检查的 Actor
	 * @param PropName 属性名（会自动在 Actor 和 RootComponent 上搜索）
	 * @param ExpectedValue 期望匹配的值（模糊包含匹配，忽略大小写）
	 * @return 是否匹配
	 */
	static bool CheckPropertyMatch(AActor* Actor, const FString& PropName, const FString& ExpectedValue);

	static bool ApplyStructValue(FStructProperty* StructProp, UObject* Target, const TSharedPtr<FJsonValue>& JsonValue);

	static const TArray<FString>& GetDefaultInspectProps();
	
	static bool TryCollectProperty(UObject* Obj, const FString& PropName, TSharedPtr<FJsonObject>& OutProps);
	static void CollectPropertyNames(UObject* Obj, TArray<FString>& OutNames);

	static int32 LevenshteinDistance(const FString& A, const FString& B);
	static void SuggestProperties(const FString& Input, const TArray<FString>& Candidates, TArray<FString>& OutSuggestions, int32 MaxSuggestions = 5);

	static FProperty* FindWritableProperty(UObject* Obj, const FString& PropName);
	static FProperty* FindWritablePropertyOnActorHierarchy(AActor* Actor, const FString& PropName, UObject*& OutTargetObj);

	static bool SetNumericProperty(FNumericProperty* NumProp, UObject* Obj, const TSharedPtr<FJsonValue>& Value, FString& OutError);
	static bool SetStructProperty(FStructProperty* StructProp, UObject* Obj, const TSharedPtr<FJsonValue>& Value, FString& OutError);
	static bool SetSimpleProperty(FProperty* Prop, UObject* Obj, const TSharedPtr<FJsonValue>& Value, FString& OutError);

	/**
	 * 那些**不能靠反射直接写**的属性。
	 *
	 * 目前只有一个：`CollisionProfileName`。它在细节面板里就摆在组件上，
	 * 但反射里它根本不是组件类的属性 —— 真身是 `FBodyInstance` 内部一个私有
	 * `UPROPERTY`（引擎 `PhysicsEngine/BodyInstance.h`）。所以按名字找必然
	 * 落空，回一句 `no such property on this component class` ——
	 * 话是对的，人看了只会以为工具错了（2026-09-16 的用户反馈就卡在这儿，
	 * 最后被迫改成在 BeginPlay 里用节点运行时设）。
	 *
	 * 也不能退而求其次去写 `BodyInstance.CollisionProfileName`：那样只改了名字，
	 * 预设里的各通道响应**不会**被加载，结果是「设成功了但碰撞行为不对」——
	 * 比报错还糟。只有 `UPrimitiveComponent::SetCollisionProfileName` 会一并载入。
	 *
	 * @param bOutHandled true 表示这个名字归这里管（不管成没成功），调用方
	 *                    就不要再走反射那条路了
	 * @return 设成功了没有
	 */
	static bool TrySetDerivedProperty(
		UObject* Target,
		const FString& PropName,
		const TSharedPtr<FJsonValue>& Value,
		FString& OutError,
		bool& bOutHandled);

	static FString JsonValueToString(const TSharedPtr<FJsonValue>& Value);
	/** JSON → UE 属性字面量（ImportText 认的那种）。翻不动返回空串 */
	static FString JsonValueToImportText(const TSharedPtr<FJsonValue>& Value);
	static TSharedPtr<FJsonObject> BuildSelectedProps(AActor* Actor, const TArray<FString>& WantedProps);

	/**
	 * 资产路径归一：去首尾空白、去 `.uasset`、反斜杠换正斜杠、连续斜杠压成一个。
	 * 不动对象名后缀（`/Game/X/A.A`）—— 要包路径的调用方自己按最后一个 `/` 之后的 `.` 截。
	 * `FUAL_MaterialCommands::NormalizePath` 是同形状的一份，那个文件太大暂未收过来。
	 */
	static FString NormalizeAssetPath(const FString& InputPath);

	// Network Helpers
	static void SendResponse(const FString& RequestId, int32 Code, const TSharedPtr<FJsonObject>& Data = nullptr);
	static void SendError(const FString& RequestId, int32 Code, const FString& Message);
	// 带结构化 details 的错误（更“有人情味”，便于 Agent 自修复）
	static void SendError(const FString& RequestId, int32 Code, const FString& Message, const TSharedPtr<FJsonObject>& Details);

	/**
	 * 发送事件通知（无需 RequestId，用于订阅类推送）
	 * @param Method 事件方法名，如 "messagelog.changed"
	 * @param Payload 事件数据
	 */
	static void SendEvent(const FString& Method, const TSharedPtr<FJsonObject>& Payload);
};
