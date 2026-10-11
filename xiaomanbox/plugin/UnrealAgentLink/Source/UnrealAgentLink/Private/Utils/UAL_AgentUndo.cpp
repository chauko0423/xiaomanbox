#include "UAL_AgentUndo.h"

#include "Editor.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "Editor/EditorEngine.h"
#include "Editor/TransBuffer.h"
#include "Misc/ConfigCacheIni.h"
#include "LevelEditor.h"
#include "UnrealEdMisc.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALUndo, Log, All);

namespace
{
	/**
	 * 现在还能碰 UObject 吗。
	 *
	 * 编辑器退出时，UObject 系统的销毁排在模块卸载**之前**：轮到
	 * ShutdownModule 的时候，下面这些裸指针指着的内存已经没了。这时候连
	 * RemoveFromRoot 都碰不得 —— 它要拿对象的内部下标去查全局对象表，
	 * 已销毁对象的下标是 -1，直接 assert：
	 *
	 *     Assertion failed: Index >= 0 [UObjectArray.h:943]
	 *
	 * 表现是「关编辑器时崩一下」。资产都存过了、看着没损失，但用户每次退出
	 * 都吃一个崩溃弹窗，而且这些崩溃报告会把真正该看的那几个淹掉 ——
	 * 2026-08-31 到 09-02 之间，它一条就占了本机崩溃记录的一半。
	 *
	 * 退出流程里跳过 RemoveFromRoot 没有任何代价：进程马上就没了，
	 * GC 根本身也一起没。
	 */
	bool UAL_CanTouchUObjects()
	{
		return UObjectInitialized() && !IsEngineExitRequested();
	}

	/** agent 自己那条撤销栈。第一次进事务作用域时才建 */
	UTransBuffer* GUALAgentBuffer = nullptr;

	/**
	 * 换进去之前 `GEditor->Trans` 指着谁。只在真的换了的时候有意义。
	 *
	 * **拿着它的这段时间必须 AddToRoot。**
	 * `UEditorEngine::Trans` 是 UPROPERTY，而且是全局撤销缓冲**唯一**的强引用。
	 * 我们一旦把 agent 缓冲写进 `Trans`，用户那条缓冲就没人引用了，下一次 GC
	 * 直接回收；等 PopScope 把这个裸指针写回去，`GEditor->Trans` 就成了野指针，
	 * 下一笔事务进 PushScope 解引用它当场崩（读 0x400 那种空基址加偏移）。
	 *
	 * 这不是理论风险 —— 它稳定复现在 actor.spawn 上：spawn 够重，
	 * 中间必然过一次 GC。
	 */
	UTransactor* GUALSavedGlobalBuffer = nullptr;

	/** 借走用户的缓冲：存下来并钉在 GC 根上 */
	void UAL_HoldGlobalBuffer(UTransactor* Buffer)
	{
		GUALSavedGlobalBuffer = Buffer;
		if (GUALSavedGlobalBuffer)
		{
			GUALSavedGlobalBuffer->AddToRoot();
		}
	}

	/** 还回去：写回 GEditor->Trans 之后再松开 GC 根，顺序不能反 */
	void UAL_ReleaseGlobalBuffer()
	{
		if (GUALSavedGlobalBuffer)
		{
			if (UAL_CanTouchUObjects())
			{
				GUALSavedGlobalBuffer->RemoveFromRoot();
			}
			GUALSavedGlobalBuffer = nullptr;
		}
	}

	/** 大于 0 表示正处在一笔（或多笔嵌套的）UAL 事务里 */
	int32 GUndoScopeDepth = 0;

	/** agent 缓冲此刻顶替着全局缓冲吗 */
	bool GbUALUndoOverrideActive = false;

	/**
	 * 「这一条之前的都算处理过了」。
	 *
	 * 记的是活跃事务条数，不是数组下标 —— 撤销会让活跃条数减少，
	 * 用下标的话水位线会指到栈外面去。
	 */
	int32 GUALUndoWatermark = 0;

	FDelegateHandle GUALUndoMapOpenedHandle;
	FDelegateHandle GUALUndoPreLoadMapHandle;
	FDelegateHandle GUALUndoTearDownHandle;

	/** 引擎默认的撤销缓冲大小，跟 UEditorEngine::CreateTrans 里的常量一致 */
	constexpr int32 UAL_DEFAULT_UNDO_BUFFER_MB = 256;

	void UAL_OnUndoMapOpened(const FString& /*Filename*/, bool /*bAsTemplate*/)
	{
		// 换图之后旧关卡的对象已经没了，栈上那些事务再撤就是往废内存里写
		FUAL_AgentUndo::Reset(TEXT("Map changed"));
	}

	void UAL_OnPreLoadMap(const FString& /*MapName*/)
	{
		FUAL_AgentUndo::PrepareForMapChange();
	}

	/**
	 * 引擎要拆掉一个编辑器世界了（EditorDestroyWorld 的第一步就广播这个）。
	 *
	 * PreLoadMap 只在**加载**新地图时触发。「另存为」一张没存过的临时关卡不加载
	 * 任何地图：引擎把世界复制进新包、再直接拆旧的，PreLoadMap 一次都不来，
	 * 两条撤销缓冲里引用旧 actor 的事务原样留着，引擎的旧 World 泄漏检查
	 * （EditorServer.cpp 的 CheckForWorldGCLeaks）直接 fatal：
	 *
	 *     World Memory Leaks: 2 leaks objects and packages.
	 *     -> TransBuffer /Engine/Transient.TransBuffer_1::AddReferencedObjects( StaticMeshActor ... )
	 *
	 * 真机上 5.5 和 5.8 都是这样崩的：AI 在临时关卡里放了个预览球，用户点保存、
	 * 另存到 /Game/Maps，保存到一半编辑器就没了。新建关卡、打开关卡同样先拆世界，
	 * 挂在这里一处全覆盖；PreLoadMap 那一路留着不动，两边都清也只是多清一次空缓冲。
	 */
	void UAL_OnMapChanged(UWorld* /*World*/, EMapChangeType ChangeType)
	{
		if (ChangeType == EMapChangeType::TearDownWorld)
		{
			FUAL_AgentUndo::PrepareForMapChange();
		}
	}

	/** 值得记账的包吗（临时包既存不了也撤不出什么有意义的东西） */
	bool UAL_IsInterestingUndoPackage(const UPackage* Package)
	{
		return Package
			&& Package != GetTransientPackage()
			&& !Package->HasAnyFlags(RF_Transient)
			&& !Package->GetName().StartsWith(TEXT("/Engine/Transient"));
	}

	/**
	 * 这一步动过的某个对象，取**用户认得出的那个名字**。
	 *
	 * Actor 用大纲视图里的标签（Bld_Ind_B），不是内部名（StaticMeshActor_37）——
	 * 调用方拿它和自己的搭建记录比对，对不上的就是别人动的。
	 *
	 * 组件记到它所属的 Actor 头上：挪一次物件会把 RootComponent 一起带进事务，
	 * 两条都列等于把同一件事说两遍。
	 */
	FString UAL_DescribeUndoObject(const UObject* Object)
	{
		if (!Object)
		{
			return FString();
		}
		if (const AActor* Actor = Cast<AActor>(Object))
		{
			return Actor->GetActorLabel();
		}
		if (const UActorComponent* Component = Cast<UActorComponent>(Object))
		{
			const AActor* Owner = Component->GetOwner();
			return Owner ? Owner->GetActorLabel() : FString();
		}
		return Object->GetName();
	}

	/**
	 * 把某一笔事务里的包和对象并进 Out。
	 *
	 * 撤销/重做**之前**调用 —— 动完之后那笔事务就不在活跃区了，再取就取不到；
	 * 而「这一下到底动了谁」正是调用方最需要知道的一件事。
	 */
	void UAL_AccumulateTransaction(const FTransaction* Transaction, FUAL_AgentUndo::FTouched& Out)
	{
		if (!Transaction)
		{
			return;
		}

		TArray<UObject*> Objects;
		Transaction->GetTransactionObjects(Objects);
		for (const UObject* Object : Objects)
		{
			const UPackage* Package = Object ? Object->GetOutermost() : nullptr;
			if (!UAL_IsInterestingUndoPackage(Package))
			{
				continue;
			}
			Out.Packages.AddUnique(Package->GetName());

			const FString Label = UAL_DescribeUndoObject(Object);
			if (!Label.IsEmpty())
			{
				Out.Objects.AddUnique(Label);
			}
		}
	}

	/** 把一步的记账并进总账。只在那一步**真的做成了**之后调 */
	void UAL_MergeTouched(const FUAL_AgentUndo::FTouched& Step, FUAL_AgentUndo::FTouched& Out)
	{
		for (const FString& Package : Step.Packages)
		{
			Out.Packages.AddUnique(Package);
		}
		for (const FString& Object : Step.Objects)
		{
			Out.Objects.AddUnique(Object);
		}
	}

	/** 这一笔事务动过这些包里的任何一个吗 */
	bool UAL_TransactionTouches(const FTransaction* Transaction, const TSet<FString>& PackageNames)
	{
		if (!Transaction)
		{
			return false;
		}

		// 逐个对象看它属于哪个包，而不是 `FTransaction::ContainsObject(资产对象)`：
		// 一笔「改蓝图」的事务里躺着的是图、节点、变量描述这些子对象，资产对象
		// 自己不一定在里面，按对象比会漏掉整整一类。5.0 也没有 ContainsObject。
		TArray<UObject*> Objects;
		Transaction->GetTransactionObjects(Objects);
		for (const UObject* Object : Objects)
		{
			const UPackage* Package = Object ? Object->GetOutermost() : nullptr;
			if (Package && PackageNames.Contains(Package->GetName()))
			{
				return true;
			}
		}
		return false;
	}

	/** 现在还能撤几步 */
	int32 UAL_ActiveUndoCount()
	{
		return GUALAgentBuffer ? GUALAgentBuffer->GetQueueLength() - GUALAgentBuffer->GetUndoCount() : 0;
	}

	/**
	 * 保证 agent 缓冲此刻是当前缓冲，返回「调用前是不是就已经是了」。
	 *
	 * Undo / Redo 不在任何事务作用域里，所以这里必须自己换。也是防
	 * 别的调用路径 —— 换错了缓冲去撤销，撤的就是用户自己的历史。
	 */
	bool UAL_BorrowAgentBuffer(UTransactor*& OutPrevious)
	{
		OutPrevious = nullptr;
		if (!GEditor || !GUALAgentBuffer || GEditor->Trans == GUALAgentBuffer)
		{
			return true;
		}
		OutPrevious = GEditor->Trans;
		// 同 GUALSavedGlobalBuffer 的理由：从 GEditor->Trans 换出去的那一刻起
		// 它就没有强引用了，而 UndoTransaction 中间完全可能过一次 GC
		if (OutPrevious)
		{
			OutPrevious->AddToRoot();
		}
		GEditor->Trans = GUALAgentBuffer;
		return false;
	}

	void UAL_ReturnAgentBuffer(UTransactor* Previous)
	{
		if (!Previous)
		{
			return;
		}
		if (GEditor)
		{
			GEditor->Trans = Previous;
		}
		// 先写回再松根。GEditor 没了就只松根 —— 那种情况下引擎自己也在收尾了
		Previous->RemoveFromRoot();
	}
}

UTransBuffer* FUAL_AgentUndo::EnsureBuffer()
{
	if (GUALAgentBuffer)
	{
		return GUALAgentBuffer;
	}
	if (!GEditor || !GConfig)
	{
		// 命令行/无编辑器环境。不是错误，只是这套机制在那里没有意义
		return nullptr;
	}

	UTransBuffer* Buffer = NewObject<UTransBuffer>();
	if (!Buffer)
	{
		return nullptr;
	}

	// 跟引擎给全局缓冲定大小的方式保持一致，用户在 EditorPerProjectUserSettings
	// 里调过 UndoBufferSize 的话，agent 这条栈也照着来
	int32 UndoBufferMb = -1;
	if (!GConfig->GetInt(TEXT("Undo"), TEXT("UndoBufferSize"), UndoBufferMb, GEditorPerProjectIni) || UndoBufferMb < 0)
	{
		UndoBufferMb = UAL_DEFAULT_UNDO_BUFFER_MB;
	}

	// 不进 GC 根的话，它不被任何 UPROPERTY 引用（我们只有裸指针），
	// 下一次 GC 就没了
	Buffer->AddToRoot();
	Buffer->Initialize(static_cast<SIZE_T>(UndoBufferMb) * 1024 * 1024);

	GUALAgentBuffer = Buffer;
	UE_LOG(LogUALUndo, Log, TEXT("Agent undo buffer created (%d MB)"), UndoBufferMb);
	return GUALAgentBuffer;
}

void FUAL_AgentUndo::Initialize()
{
	if (!GUALUndoMapOpenedHandle.IsValid())
	{
		GUALUndoMapOpenedHandle = FEditorDelegates::OnMapOpened.AddStatic(&UAL_OnUndoMapOpened);
	}
	// OnMapOpened 是**换完之后**才触发的，而引擎的旧 World 泄漏检查在拆旧图的
	// 过程中就跑了 —— 只挂那一个救不了切关卡崩溃，必须再挂一个换图前的
	if (!GUALUndoPreLoadMapHandle.IsValid())
	{
		GUALUndoPreLoadMapHandle = FCoreUObjectDelegates::PreLoadMap.AddStatic(&UAL_OnPreLoadMap);
	}
	// 另存为 / 新建关卡不走 PreLoadMap，但都要先拆世界 —— 见 UAL_OnMapChanged
	if (!GUALUndoTearDownHandle.IsValid())
	{
		FLevelEditorModule& LevelEditor = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
		GUALUndoTearDownHandle = LevelEditor.OnMapChanged().AddStatic(&UAL_OnMapChanged);
	}
}

void FUAL_AgentUndo::PrepareForMapChange()
{
	/**
	 * 换关卡之前，把两条撤销缓冲都清空，并且把引擎自己那条还回 GEditor->Trans。
	 *
	 * ## 不做会怎样：切关卡必崩
	 *
	 * `UEditorEngine::ResetTransaction` 只重置**当前装着的**那一条：
	 *
	 *     void UEditorEngine::ResetTransaction(const FText& Reason)
	 *     {
	 *         if (Trans) { Trans->Reset(Reason); }
	 *     }
	 *
	 * 我们把 agent 缓冲换进 `Trans` 之后，引擎清掉的是我们这条；引擎自己那条
	 * 躺在 GUALSavedGlobalBuffer 里、被我们 AddToRoot 钉着，里面还存着一堆
	 * 引用旧关卡 actor 的事务 —— 旧 World 因此回收不掉，引擎的
	 * 「World Memory Leaks」检查直接 fatal。
	 *
	 * 反过来，如果换图那一刻我们的缓冲没装在 Trans 上，那没被清的就是我们这条，
	 * 结果一样。所以两条都得清。
	 *
	 * ## 为什么不能只靠 OnMapOpened
	 *
	 * 那个委托在**新图加载完之后**才触发，泄漏检查早跑过了。必须挂
	 * PreLoadMap（5.0–5.8 都有，逐版本确认过）。
	 */
	if (GEditor && GbUALUndoOverrideActive)
	{
		GEditor->Trans = GUALSavedGlobalBuffer;
	}
	GbUALUndoOverrideActive = false;

	// 引擎那条：清空之后再松开 GC 根。清空是关键 —— 光松根，它此刻仍被
	// GEditor->Trans 引用着，里面的旧 World 引用一样跑不掉
	if (GUALSavedGlobalBuffer)
	{
		GUALSavedGlobalBuffer->Reset(NSLOCTEXT("UALUndo", "MapChanging", "Map changing"));
	}
	UAL_ReleaseGlobalBuffer();

	if (GUALAgentBuffer)
	{
		GUALAgentBuffer->Reset(NSLOCTEXT("UALUndo", "MapChanging", "Map changing"));
	}

	// 作用域深度不动：换图可能发生在一笔事务**内部**，那笔事务的 PopScope
	// 还会来。把深度清零会让它减到负数，下一笔事务的 PushScope 就再也换不进去了
	GUALUndoWatermark = 0;

	UE_LOG(LogUALUndo, Log, TEXT("Undo buffers cleared ahead of a map change"));
}

void FUAL_AgentUndo::Shutdown()
{
	if (GUALUndoMapOpenedHandle.IsValid())
	{
		FEditorDelegates::OnMapOpened.Remove(GUALUndoMapOpenedHandle);
		GUALUndoMapOpenedHandle.Reset();
	}
	if (GUALUndoPreLoadMapHandle.IsValid())
	{
		FCoreUObjectDelegates::PreLoadMap.Remove(GUALUndoPreLoadMapHandle);
		GUALUndoPreLoadMapHandle.Reset();
	}
	if (GUALUndoTearDownHandle.IsValid())
	{
		// 退出流程里 LevelEditor 可能已经先卸了：只在它还在时摘
		if (FLevelEditorModule* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor")))
		{
			LevelEditor->OnMapChanged().Remove(GUALUndoTearDownHandle);
		}
		GUALUndoTearDownHandle.Reset();
	}

	// 先把全局缓冲还回去再撒手。少了这一步，编辑器会带着一条即将被 GC 的
	// 缓冲继续跑，用户下一次 Ctrl+Z 就崩。
	// 退出流程里就别还了 —— 那时 GEditor 和两条缓冲都已经是野指针（见
	// UAL_CanTouchUObjects），而且也没有「下一次 Ctrl+Z」了
	if (GbUALUndoOverrideActive && GEditor && UAL_CanTouchUObjects())
	{
		GEditor->Trans = GUALSavedGlobalBuffer;
	}
	GbUALUndoOverrideActive = false;
	UAL_ReleaseGlobalBuffer();

	if (GUALAgentBuffer)
	{
		if (UAL_CanTouchUObjects())
		{
			GUALAgentBuffer->RemoveFromRoot();
		}
		GUALAgentBuffer = nullptr;
	}

	GUndoScopeDepth = 0;
	GUALUndoWatermark = 0;
}

void FUAL_AgentUndo::PushScope()
{
	if (++GUndoScopeDepth != 1)
	{
		// 嵌套，最外层已经换好了
		return;
	}

	if (GbUALUndoOverrideActive)
	{
		// 上一笔事务收尾时还有别的事务开着，没换回去（见 PopScope）。
		// 栈是好的，等那笔结束后自然会还，这里什么都不用做。
		return;
	}

	if (!GEditor)
	{
		return;
	}

	UTransBuffer* Buffer = EnsureBuffer();
	if (!Buffer || GEditor->Trans == Buffer)
	{
		return;
	}

	// 有事务正开着时不换。一笔事务的 Begin 和 End 必须落在同一个缓冲上，
	// 换了的话两条栈都会坏。
	// IsValid 挡的是「已标记待删但还没回收」那一档；真正被回收过的野指针
	// 靠的是借走时的 AddToRoot，不是这里
	if (GEditor->Trans && IsValid(GEditor->Trans) && GEditor->Trans->IsActive())
	{
		UE_LOG(LogUALUndo, Verbose, TEXT("Transaction in flight, this command shares the global undo buffer"));
		return;
	}

	UAL_HoldGlobalBuffer(GEditor->Trans);
	GEditor->Trans = Buffer;
	GbUALUndoOverrideActive = true;
}

void FUAL_AgentUndo::PopScope()
{
	// 不让它掉到负数：一次配对失误会让作用域永远关不上，
	// 表现为用户的撤销历史被 agent 的步骤淹掉，而那非常难查
	GUndoScopeDepth = FMath::Max(0, GUndoScopeDepth - 1);

	if (GUndoScopeDepth != 0 || !GbUALUndoOverrideActive || !GEditor)
	{
		return;
	}

	if (GEditor->Trans && IsValid(GEditor->Trans) && GEditor->Trans->IsActive())
	{
		// 我们这笔已经结束了（析构顺序保证的），却还有事务开着 —— 说明处理器里
		// 另开了一笔没关上的。现在换回去，它的 End 会落到用户的缓冲上，两条栈一起坏。
		// 宁可让 agent 缓冲多顶一会儿 —— 下一笔事务收尾时再还。
		UE_LOG(LogUALUndo, Warning, TEXT("A transaction is still open on scope exit; keeping the agent undo buffer active"));
		return;
	}

	// 先写回，再松开 GC 根 —— 反过来的话中间有一瞬间没人引用它
	GEditor->Trans = GUALSavedGlobalBuffer;
	UAL_ReleaseGlobalBuffer();
	GbUALUndoOverrideActive = false;
}

bool FUAL_AgentUndo::IsOverrideActive()
{
	return GbUALUndoOverrideActive;
}

int32 FUAL_AgentUndo::UndoableCount()
{
	return UAL_ActiveUndoCount();
}

int32 FUAL_AgentUndo::RedoableCount()
{
	return GUALAgentBuffer ? GUALAgentBuffer->GetUndoCount() : 0;
}

TArray<FUAL_AgentUndo::FEntry> FUAL_AgentUndo::DescribeUndoable()
{
	TArray<FEntry> Entries;
	if (!GUALAgentBuffer)
	{
		return Entries;
	}

	const int32 Count = UAL_ActiveUndoCount();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FTransaction* Transaction = GUALAgentBuffer->GetTransaction(Index);
		if (!Transaction)
		{
			continue;
		}

		const FTransactionContext Context = Transaction->GetContext();

		FEntry Entry;
		Entry.Index = Index;
		Entry.Title = Context.Title.ToString();
		Entry.Context = Context.Context;

		TArray<UObject*> Objects;
		Transaction->GetTransactionObjects(Objects);
		for (const UObject* Object : Objects)
		{
			const UPackage* Package = Object ? Object->GetOutermost() : nullptr;
			if (!UAL_IsInterestingUndoPackage(Package))
			{
				continue;
			}
			Entry.Packages.AddUnique(Package->GetName());

			const FString Label = UAL_DescribeUndoObject(Object);
			if (!Label.IsEmpty())
			{
				Entry.Objects.AddUnique(Label);
			}
		}

		Entries.Add(MoveTemp(Entry));
	}

	return Entries;
}

TSet<FString> FUAL_AgentUndo::CollectTouchedPackagePaths()
{
	TSet<FString> Packages;
	if (!GUALAgentBuffer)
	{
		return Packages;
	}

	const int32 Count = UAL_ActiveUndoCount();

	// 撤销会让活跃条数缩到水位线以下，那时水位线跟着降 ——
	// 不降的话循环直接不进，撤销之后再改的东西就记不到了
	GUALUndoWatermark = FMath::Clamp(GUALUndoWatermark, 0, Count);

	for (int32 Index = GUALUndoWatermark; Index < Count; ++Index)
	{
		const FTransaction* Transaction = GUALAgentBuffer->GetTransaction(Index);
		if (!Transaction)
		{
			continue;
		}

		TArray<UObject*> Objects;
		Transaction->GetTransactionObjects(Objects);
		for (const UObject* Object : Objects)
		{
			const UPackage* Package = Object ? Object->GetOutermost() : nullptr;
			if (UAL_IsInterestingUndoPackage(Package))
			{
				Packages.Add(Package->GetName());
			}
		}
	}

	return Packages;
}

void FUAL_AgentUndo::MarkSeen()
{
	GUALUndoWatermark = UAL_ActiveUndoCount();
}

int32 FUAL_AgentUndo::Undo(int32 Steps, TArray<FString>& OutTitles, FTouched& OutTouched, FString& OutError)
{
	OutTitles.Reset();
	OutTouched = FTouched();

	if (!GEditor || !GUALAgentBuffer)
	{
		OutError = TEXT("The agent undo buffer is not available (no editor, or nothing has been done yet).");
		return 0;
	}

	const int32 Available = UAL_ActiveUndoCount();
	if (Available == 0)
	{
		// 以前这里静默回 0 步、ok=true。调用方说「撤 3 步」，回执是「成功，0 步」——
		// 模型会当成撤完了。OutError 非空时响应里 ok=false，这句话会原样带给调用方
		OutError = Steps > 0
			? FString::Printf(TEXT("Nothing to undo: %d step(s) requested, 0 available on the agent undo stack."), Steps)
			: FString(TEXT("Nothing to undo: the agent undo stack is empty."));
		return 0;
	}

	const int32 Target = (Steps <= 0) ? Available : FMath::Min(Steps, Available);

	UTransactor* Previous = nullptr;
	UAL_BorrowAgentBuffer(Previous);

	int32 Undone = 0;
	for (int32 Step = 0; Step < Target; ++Step)
	{
		// 标题和内容都要在撤销**之前**读 —— 撤完那一条就移出活跃区了。
		// 先记在临时的 Step 上：这一步万一撤不动，它就不该算进「动过什么」，
		// 否则调用方会拿着一个根本没变过的包去 ue_save
		const int32 TopIndex = UAL_ActiveUndoCount() - 1;
		FString Title;
		FTouched StepTouched;
		if (const FTransaction* Transaction = TopIndex >= 0 ? GUALAgentBuffer->GetTransaction(TopIndex) : nullptr)
		{
			Title = Transaction->GetContext().Title.ToString();
			UAL_AccumulateTransaction(Transaction, StepTouched);
		}

		// bCanRedo=true：撤多了还能走回来。撤销本身不该是个不可逆操作
		if (!GEditor->UndoTransaction(/*bCanRedo=*/true))
		{
			break;
		}

		UAL_MergeTouched(StepTouched, OutTouched);
		OutTitles.Add(Title.IsEmpty() ? TEXT("(untitled)") : Title);
		++Undone;
	}

	UAL_ReturnAgentBuffer(Previous);

	if (Undone < Target && OutError.IsEmpty())
	{
		OutError = FString::Printf(
			TEXT("Stopped after %d of %d steps - the editor refused to undo further."), Undone, Target);
	}
	// 要的比栈上有的多：以前被 FMath::Min 静默夹掉，回执只说「撤了 2 步」，
	// 看不出请求的是 5 步。响应结构在 UAL_UndoCommands.cpp，这里只能把请求数写进 error
	else if (Steps > Available && OutError.IsEmpty())
	{
		OutError = FString::Printf(
			TEXT("Requested %d steps but only %d were available; undid %d."), Steps, Available, Undone);
	}

	return Undone;
}

int32 FUAL_AgentUndo::Redo(int32 Steps, TArray<FString>& OutTitles, FTouched& OutTouched, FString& OutError)
{
	OutTitles.Reset();
	OutTouched = FTouched();

	if (!GEditor || !GUALAgentBuffer)
	{
		OutError = TEXT("The agent undo buffer is not available (no editor, or nothing has been done yet).");
		return 0;
	}

	const int32 Available = GUALAgentBuffer->GetUndoCount();
	if (Available == 0)
	{
		// 同 Undo：没东西可重做要明说，不能回「成功，0 步」
		OutError = Steps > 0
			? FString::Printf(TEXT("Nothing to redo: %d step(s) requested, 0 available."), Steps)
			: FString(TEXT("Nothing to redo."));
		return 0;
	}

	const int32 Target = (Steps <= 0) ? Available : FMath::Min(Steps, Available);

	UTransactor* Previous = nullptr;
	UAL_BorrowAgentBuffer(Previous);

	int32 Redone = 0;
	for (int32 Step = 0; Step < Target; ++Step)
	{
		// 重做的是活跃区之外的下一条，它的下标正好等于当前活跃条数。
		// 同 Undo：没重做成的那一步不算进「动过什么」
		const int32 NextIndex = UAL_ActiveUndoCount();
		FString Title;
		FTouched StepTouched;
		if (const FTransaction* Transaction = GUALAgentBuffer->GetTransaction(NextIndex))
		{
			Title = Transaction->GetContext().Title.ToString();
			UAL_AccumulateTransaction(Transaction, StepTouched);
		}

		if (!GEditor->RedoTransaction())
		{
			break;
		}

		UAL_MergeTouched(StepTouched, OutTouched);
		OutTitles.Add(Title.IsEmpty() ? TEXT("(untitled)") : Title);
		++Redone;
	}

	UAL_ReturnAgentBuffer(Previous);

	if (Redone < Target && OutError.IsEmpty())
	{
		OutError = FString::Printf(
			TEXT("Stopped after %d of %d steps - the editor refused to redo further."), Redone, Target);
	}
	else if (Steps > Available && OutError.IsEmpty())
	{
		OutError = FString::Printf(
			TEXT("Requested %d steps but only %d were available; redid %d."), Steps, Available, Redone);
	}

	return Redone;
}

void FUAL_AgentUndo::Reset(const FString& Reason)
{
	if (!GUALAgentBuffer)
	{
		return;
	}

	UTransactor* Previous = nullptr;
	UAL_BorrowAgentBuffer(Previous);
	GUALAgentBuffer->Reset(FText::FromString(Reason));
	UAL_ReturnAgentBuffer(Previous);

	GUALUndoWatermark = 0;
	UE_LOG(LogUALUndo, Log, TEXT("Agent undo buffer reset: %s"), *Reason);
}

int32 FUAL_AgentUndo::CountStepsTouching(const TArray<FString>& PackageNames)
{
	if (!GUALAgentBuffer || PackageNames.Num() == 0)
	{
		return 0;
	}

	const TSet<FString> Wanted(PackageNames);
	int32 Count = 0;
	// 整条队列都要扫，不只是还能撤的那一段：重做区里的事务同样攥着对象引用，
	// 同样能把删除顶回来
	for (int32 Index = 0; Index < GUALAgentBuffer->GetQueueLength(); ++Index)
	{
		if (UAL_TransactionTouches(GUALAgentBuffer->GetTransaction(Index), Wanted))
		{
			++Count;
		}
	}
	return Count;
}

int32 FUAL_AgentUndo::DropStepsTouching(const TArray<FString>& PackageNames, TArray<FString>& OutTitles)
{
	OutTitles.Reset();

	if (!GUALAgentBuffer || PackageNames.Num() == 0)
	{
		return 0;
	}

	// 有事务正开着时一步都不动：那笔事务的记录还在往队尾的条目里写，
	// 此刻改队列等于在别人写的时候抽走它的纸
	if (GUALAgentBuffer->IsActive())
	{
		UE_LOG(LogUALUndo, Warning, TEXT("DropStepsTouching skipped: a transaction is still open"));
		return 0;
	}

	const TSet<FString> Wanted(PackageNames);
	int32 Dropped = 0;

	// 从后往前删，前面的下标才不会因为删除而错位
	for (int32 Index = GUALAgentBuffer->UndoBuffer.Num() - 1; Index >= 0; --Index)
	{
		const FTransaction& Transaction = GUALAgentBuffer->UndoBuffer[Index].Get();
		if (!UAL_TransactionTouches(&Transaction, Wanted))
		{
			continue;
		}

		OutTitles.Add(Transaction.GetContext().Title.ToString());

		// 队尾那 UndoCount 条是「撤过、还能重做」的。摘掉的如果落在那一段里，
		// 重做计数得跟着减 —— 不减的话 GetQueueLength() - UndoCount 会算出
		// 比实际少的可撤步数，后面每一次 Undo 都撤错那一步
		const int32 RedoRegionStart = GUALAgentBuffer->UndoBuffer.Num() - GUALAgentBuffer->UndoCount;
		if (Index >= RedoRegionStart)
		{
			GUALAgentBuffer->UndoCount = FMath::Max(0, GUALAgentBuffer->UndoCount - 1);
		}

		GUALAgentBuffer->UndoBuffer.RemoveAt(Index);
		++Dropped;
	}

	if (Dropped > 0)
	{
		// 水位线是个下标，队列短了就得跟着收，否则 CollectTouchedPackagePaths
		// 的循环直接不进，后面改的东西一个都记不到（见 MarkSeen 的说明）
		GUALUndoWatermark = FMath::Clamp(GUALUndoWatermark, 0, UAL_ActiveUndoCount());

		// 撤销历史面板照着这个事件刷新。不发的话它会一直显示已经不存在的步骤
		GUALAgentBuffer->OnUndoBufferChanged().Broadcast();

		UE_LOG(LogUALUndo, Log, TEXT("Dropped %d agent undo step(s) holding %d package(s)"),
			Dropped, PackageNames.Num());
	}

	return Dropped;
}
