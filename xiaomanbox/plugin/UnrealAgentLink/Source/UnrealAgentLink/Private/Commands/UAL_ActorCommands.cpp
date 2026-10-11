#include "UAL_ActorCommands.h"
#include "UAL_CommandUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "UAL_ScopedTransaction.h"
#include "Components/SceneComponent.h"
#include "Engine/EngineTypes.h"
#include "CollisionQueryParams.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALActor, Log, All);

namespace UAL_SnapToFloor
{
	/** 往下探多远（厘米）。1 公里够覆盖「摆到了两百米高空」这种手滑 */
	static constexpr double TraceDistance = 100000.0;
	/** 起点从脚底往上抬一点，免得起点正好埋在地面里、一开始就命中自己脚下那一层 */
	static constexpr double StartLift = 10.0;

	/**
	 * 把 Actor 放到它正下方那个面上。
	 *
	 * ## 为什么不用 GEditor->Exec(TEXT("SNAPTOFLOOR"))
	 *
	 * 因为引擎里没有这条 Exec 命令。`SnapToFloor` 是 LevelEditor 的 UI 命令
	 * （`FLevelEditorCommands::SnapToFloor`，绑 End 键，实现在
	 * `FLevelEditorActionCallbacks::SnapToFloor_Clicked`），从来没有注册成
	 * 控制台命令。所以那行 Exec 返回 false 然后什么都不做 —— 工具照旧回
	 * success，位置一动不动，调用方完全看不出来（实测里
	 * 提报方还以为是「射线打不到碰撞体」）。
	 *
	 * 这里改成自己打一条向下的射线：不依赖编辑器选中状态，PIE 里也能用，
	 * 而且能把「打到了什么、落差多少」如实回给调用方。
	 *
	 * ## 为什么按包围盒底面对齐，而不是按 Actor 原点
	 *
	 * 骨骼网格的原点常在脚底，静态网格的原点常在几何中心 —— 按原点贴地
	 * 会让一半的东西半截埋进地里。按包围盒底面对齐是「看起来站在地上」
	 * 这件事的直接表达。
	 *
	 * ## 为什么两个通道都试
	 *
	 * 关卡里的地面网格不一定在 WorldStatic 上有碰撞（美术摆的装饰件常常
	 * 只留 Visibility）。先试 WorldStatic，没命中再试 Visibility，并把命中的
	 * 通道写进回读 —— 落差对不上时，通道是第一个要看的东西。
	 */
	static bool SnapActor(AActor* Actor, UWorld* World, TSharedPtr<FJsonObject>& OutSnap)
	{
		OutSnap = MakeShared<FJsonObject>();
		if (!Actor || !World)
		{
			OutSnap->SetBoolField(TEXT("hit"), false);
			OutSnap->SetStringField(TEXT("reason"), TEXT("actor or world unavailable"));
			return false;
		}

		FVector Origin = FVector::ZeroVector;
		FVector Extent = FVector::ZeroVector;
		Actor->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent);

		const FVector ActorLocation = Actor->GetActorLocation();
		// 包围盒底面在世界空间的 z，以及它相对 Actor 原点的偏移
		const double BottomZ = Origin.Z - Extent.Z;
		const double BottomOffset = BottomZ - ActorLocation.Z;

		const FVector Start(Origin.X, Origin.Y, BottomZ + StartLift);
		const FVector End(Origin.X, Origin.Y, BottomZ - TraceDistance);

		FCollisionQueryParams Params(SCENE_QUERY_STAT(UAL_SnapToFloor), /*bTraceComplex=*/true);
		Params.AddIgnoredActor(Actor);

		static const ECollisionChannel Channels[] = { ECC_WorldStatic, ECC_Visibility };
		static const TCHAR* ChannelNames[] = { TEXT("WorldStatic"), TEXT("Visibility") };

		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Channels); ++Index)
		{
			FHitResult Hit;
			if (!World->LineTraceSingleByChannel(Hit, Start, End, Channels[Index], Params))
			{
				continue;
			}

			const double SurfaceZ = Hit.ImpactPoint.Z;
			const FVector Snapped(ActorLocation.X, ActorLocation.Y, SurfaceZ - BottomOffset);
			Actor->SetActorLocation(Snapped, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);

			OutSnap->SetBoolField(TEXT("hit"), true);
			OutSnap->SetStringField(TEXT("channel"), ChannelNames[Index]);
			OutSnap->SetNumberField(TEXT("surface_z"), SurfaceZ);
			OutSnap->SetNumberField(TEXT("moved_dz"), Snapped.Z - ActorLocation.Z);
			OutSnap->SetNumberField(TEXT("bottom_offset"), BottomOffset);
			OutSnap->SetStringField(TEXT("hit_actor"),
				Hit.GetActor() ? UAL_CommandUtils::GetActorFriendlyName(Hit.GetActor()) : TEXT(""));
			return true;
		}

		OutSnap->SetBoolField(TEXT("hit"), false);
		OutSnap->SetStringField(TEXT("reason"),
			TEXT("no collision below in WorldStatic or Visibility; the actor was left where it was"));
		OutSnap->SetNumberField(TEXT("traced_from_z"), Start.Z);
		OutSnap->SetNumberField(TEXT("traced_to_z"), End.Z);
		return false;
	}
}

void FUAL_ActorCommands::RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("actor.spawn"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SpawnActor(Payload, RequestId);
	});

	// actor.spawn_batch 已删（2026-09-16）：它只是把 batch 改名成 instances 再转发给
	// actor.spawn，而 actor.spawn v2 本来就收 instances 数组。盒子这边从来没调过

	CommandMap.Add(TEXT("actor.destroy"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DestroyActor(Payload, RequestId);
	});

	// actor.destroy_batch 已删（2026-09-16）：同上，转发给 actor.destroy 的 targets

	CommandMap.Add(TEXT("actor.set_transform"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetTransformUnified(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.set_property"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetProperty(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.get_info"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetActorInfo(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.get"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetActor(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.inspect"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_InspectActor(Payload, RequestId);
	});
}

// ========== 从 UAL_CommandHandler.cpp 迁移以下函数 ==========
// 原始行号参考:
//   SpawnSingleActor:        2514-2654
//   Handle_SpawnActor:       2656-2730
//   DestroySingleActor:      2746-2788
//   Handle_DestroyActor:     2790-2892
//   Handle_GetActorInfo:     2943-3005
//   Handle_GetActor:         3007-3089
//   Handle_InspectActor:     3091-3170
//   Handle_SetProperty:      3172-3330
//   Handle_SetTransformUnified: 3332-3529

/**
 * 生成一个 Actor；失败时把原因写进 OutFailReason。
 *
 * 以前失败只回 nullptr，批量里那一项变成 `created[i] = null`，调用方只知道
 * 「第 3 个没建出来」，不知道是资产路径写错、类加载不到还是网格挂不上 ——
 * 只能原样重试，而原样重试必然再失败一次（AGENTS.md §5 第 14 条）。
 *
 * 做成文件内的自由函数而不是改成员签名：头文件在 Public 下，改它会牵动其他调用方。
 */
static TSharedPtr<FJsonObject> UAL_SpawnSingleActorWithReason(const TSharedPtr<FJsonObject>& Item, FString& OutFailReason)
{
	if (!Item.IsValid())
	{
		OutFailReason = TEXT("instance is not a JSON object");
		return nullptr;
	}

	FString PresetName, ClassPath, DesiredName, AssetId, MeshOverride;
	Item->TryGetStringField(TEXT("preset"), PresetName);
	Item->TryGetStringField(TEXT("class"), ClassPath);
	Item->TryGetStringField(TEXT("name"), DesiredName);
	Item->TryGetStringField(TEXT("asset_id"), AssetId);
	Item->TryGetStringField(TEXT("mesh"), MeshOverride);

	if (AssetId.IsEmpty() && PresetName.IsEmpty() && ClassPath.IsEmpty())
	{
		OutFailReason = TEXT("instance needs one of asset_id / preset / class");
		return nullptr; // Skip invalid
	}

	UWorld* World = UAL_CommandUtils::GetLiveWorld();
	if (!World)
	{
		OutFailReason = TEXT("World not available");
		return nullptr;
	}

	UAL_CommandUtils::FUALResolvedSpawnRequest Resolved;
	FString ResolveError;

	if (!AssetId.IsEmpty())
	{
		if (!UAL_CommandUtils::ResolveSpawnFromAssetId(AssetId, Resolved, ResolveError))
		{
			UE_LOG(LogUALActor, Warning, TEXT("Spawn failed to resolve asset_id=%s error=%s"), *AssetId, *ResolveError);
			OutFailReason = ResolveError.IsEmpty()
				? FString::Printf(TEXT("could not resolve asset_id '%s'"), *AssetId)
				: ResolveError;
			return nullptr;
		}
	}
	else if (!PresetName.IsEmpty())
	{
		UAL_CommandUtils::FUALSpawnPreset Preset;
		if (!UAL_CommandUtils::ResolvePreset(PresetName, Preset))
		{
			OutFailReason = FString::Printf(TEXT("unknown preset '%s'"), *PresetName);
			return nullptr;
		}
		Resolved.SpawnClass = Preset.Class;
		if (Preset.AssetPath)
		{
			Resolved.MeshPath = Preset.AssetPath;
		}
		Resolved.ResolvedType = Preset.Class ? Preset.Class->GetName() : TEXT("Preset");
		Resolved.SourceId = PresetName;
		Resolved.bFromAlias = true;
	}
	else if (!ClassPath.IsEmpty())
	{
		UObject* LoadedClassObj = StaticLoadObject(UClass::StaticClass(), nullptr, *ClassPath);
		if (LoadedClassObj)
		{
			Resolved.SpawnClass = Cast<UClass>(LoadedClassObj);
			if (Resolved.SpawnClass)
			{
				Resolved.ResolvedType = Resolved.SpawnClass->GetName();
			}
			Resolved.SourceId = ClassPath;
		}
	}

	if (Resolved.bFromAlias && PresetName.IsEmpty())
	{
		PresetName = Resolved.SourceId;
	}

	if (Resolved.SpawnClass == nullptr)
	{
		OutFailReason = !ClassPath.IsEmpty()
			? FString::Printf(TEXT("class '%s' could not be loaded"), *ClassPath)
			: FString(TEXT("could not resolve a class to spawn"));
		return nullptr;
	}

	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	FVector Scale = FVector(1, 1, 1);
	UAL_CommandUtils::ReadTransformFromItem(Item, Location, Rotation, Scale);

	// 名字被占用时退让到 `_1`、`_2`……，而且**对象名和 label 都要避开**。
	//
	// 以前只避开对象名，label 却照旧设成调用方要的那个：场上出现两个都叫
	// MyCube 的 Actor，返回里的 name（取 label）也还是 MyCube —— 调用方完全
	// 看不出改过名，按名字回读查到的可能是原来那个。反过来，对象名空着而 label
	// 被占（手动改过显示名的 Actor）时 SpawnActor 根本不会失败，只查对象名就漏了
	auto IsTaken = [World](const FString& Candidate)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->GetFName().ToString().Equals(Candidate, ESearchCase::IgnoreCase))
			{
				return true;
			}
#if WITH_EDITOR
			if (It->GetActorLabel().Equals(Candidate, ESearchCase::IgnoreCase))
			{
				return true;
			}
#endif
		}
		return false;
	};

	FString FinalName = DesiredName;
	int32 Suffix = 0;
	if (!DesiredName.IsEmpty())
	{
		while (Suffix < 100 && IsTaken(FinalName))
		{
			FinalName = FString::Printf(TEXT("%s_%d"), *DesiredName, ++Suffix);
		}
	}

	FActorSpawnParameters Params;
	if (!FinalName.IsEmpty())
	{
		Params.Name = FName(*FinalName);
		Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ReturnNull;
	}

	const FTransform SpawnTransform(Rotation, Location);
	AActor* Actor = World->SpawnActor(Resolved.SpawnClass, &SpawnTransform, Params);

	// 迭代器看不见已销毁、还没被 GC 的 Actor，它们仍占着对象名 —— 接着往后退让
	while (!Actor && !DesiredName.IsEmpty() && Suffix < 100)
	{
		FinalName = FString::Printf(TEXT("%s_%d"), *DesiredName, ++Suffix);
		Params.Name = FName(*FinalName);
		Actor = World->SpawnActor(Resolved.SpawnClass, &SpawnTransform, Params);
	}

	if (!Actor)
	{
		OutFailReason = FString::Printf(TEXT("SpawnActor returned null for class %s"), *Resolved.SpawnClass->GetName());
		return nullptr;
	}

	const TCHAR* MeshPath = nullptr;
	if (!MeshOverride.IsEmpty())
	{
		MeshPath = *MeshOverride;
	}
	else if (!Resolved.MeshPath.IsEmpty())
	{
		MeshPath = *Resolved.MeshPath;
	}

	if (!UAL_CommandUtils::SetStaticMeshIfNeeded(Actor, MeshPath))
	{
		OutFailReason = FString::Printf(TEXT("mesh '%s' could not be loaded or applied; the actor was removed"),
			MeshPath ? MeshPath : TEXT(""));
		Actor->Destroy();
		return nullptr;
	}

	Actor->SetActorScale3D(Scale);
#if WITH_EDITOR
	Actor->Modify();
	if (!FinalName.IsEmpty())
	{
		Actor->SetActorLabel(FinalName);
	}
#endif

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), UAL_CommandUtils::GetActorFriendlyName(Actor));
	if (!DesiredName.IsEmpty() && FinalName != DesiredName)
	{
		Data->SetStringField(TEXT("requested_name"), DesiredName);
		Data->SetBoolField(TEXT("renamed"), true);
	}
	Data->SetStringField(TEXT("path"), Actor->GetPathName());
	Data->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
	if (!AssetId.IsEmpty())
	{
		Data->SetStringField(TEXT("asset_id"), AssetId);
	}
	if (!Resolved.ResolvedType.IsEmpty())
	{
		Data->SetStringField(TEXT("type"), Resolved.ResolvedType);
	}
	if (!PresetName.IsEmpty())
	{
		Data->SetStringField(TEXT("preset"), PresetName);
	}
	return Data;
}

TSharedPtr<FJsonObject> FUAL_ActorCommands::SpawnSingleActor(const TSharedPtr<FJsonObject>& Item)
{
	FString Unused;
	return UAL_SpawnSingleActorWithReason(Item, Unused);
}

void FUAL_ActorCommands::Handle_SpawnActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	// 创建撤销事务，使生成操作可通过 Ctrl+Z 撤销
	FUAL_ScopedTransaction Transaction(UAL_CommandUtils::LText(TEXT("生成Actor"), TEXT("Spawn Actor")));
#endif

	const TArray<TSharedPtr<FJsonValue>>* Instances = nullptr;
	if (Payload->TryGetArrayField(TEXT("instances"), Instances) && Instances)
	{
		const int32 MaxBatch = UAL_CommandUtils::GetMaxBatchCreate();
		if (MaxBatch > 0 && Instances->Num() > MaxBatch)
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("field"), TEXT("instances"));
			Details->SetNumberField(TEXT("requested"), Instances->Num());
			Details->SetNumberField(TEXT("max"), MaxBatch);
			Details->SetStringField(TEXT("cvar"), TEXT("ual.MaxBatchCreate"));

			const FString Msg = FString::Printf(
				TEXT("%s: %d > %d"),
				*UAL_CommandUtils::LStr(TEXT("批量创建数量超过上限"), TEXT("Batch create size exceeds limit")),
				Instances->Num(),
				MaxBatch);

			UAL_CommandUtils::SendError(RequestId, 413, Msg, Details);
			return;
		}

		TArray<TSharedPtr<FJsonValue>> Created;
		TArray<TSharedPtr<FJsonValue>> Failed;
		int32 SuccessCount = 0;

		for (int32 Index = 0; Index < Instances->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Val = (*Instances)[Index];
			const TSharedPtr<FJsonObject> Item = Val.IsValid() ? Val->AsObject() : nullptr;
			FString FailReason;
			if (TSharedPtr<FJsonObject> Res = UAL_SpawnSingleActorWithReason(Item, FailReason))
			{
				Created.Add(MakeShared<FJsonValueObject>(Res));
				SuccessCount++;
			}
			else
			{
				// created[i] 保持 null（老调用方按下标对位），原因另记在 failed 里 ——
				// 光一个 null，调用方分不清「路径写错」和「引擎拒绝生成」
				Created.Add(MakeShared<FJsonValueNull>());
				TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
				Fail->SetNumberField(TEXT("index"), Index);
				Fail->SetStringField(TEXT("reason"), FailReason.IsEmpty() ? FString(TEXT("spawn failed")) : FailReason);
				Failed.Add(MakeShared<FJsonValueObject>(Fail));
			}
		}

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("created"), Created);
		Data->SetNumberField(TEXT("count"), SuccessCount);
		Data->SetNumberField(TEXT("failed_count"), Failed.Num());
		if (Failed.Num() > 0)
		{
			Data->SetArrayField(TEXT("failed"), Failed);
		}
		if (SuccessCount == 0)
		{
			// 一个都没建出来：错误正文里带上第一条原因，调用方不用再去翻 failed[]
			FString FirstReason;
			if (Failed.Num() > 0)
			{
				Failed[0]->AsObject()->TryGetStringField(TEXT("reason"), FirstReason);
			}
			Data->SetStringField(TEXT("error"), FString::Printf(
				TEXT("No actor spawned (%d requested). First failure: %s"), Instances->Num(), *FirstReason));
		}

		UAL_CommandUtils::SendResponse(RequestId, SuccessCount > 0 ? 200 : 500, Data);
		return;
	}

	// 兼容旧 batch 字段
	const TArray<TSharedPtr<FJsonValue>>* BatchCompat = nullptr;
	if (Payload->TryGetArrayField(TEXT("batch"), BatchCompat) && BatchCompat)
	{
		TSharedPtr<FJsonObject> CompatPayload = MakeShared<FJsonObject>();
		CompatPayload->SetArrayField(TEXT("instances"), *BatchCompat);
		Handle_SpawnActor(CompatPayload, RequestId);
		return;
	}

	FString SingleFailReason;
	TSharedPtr<FJsonObject> Data = UAL_SpawnSingleActorWithReason(Payload, SingleFailReason);
	if (Data.IsValid())
	{
#if WITH_EDITOR
		// 单体创建时尝试选中（批量时不选，避免闪烁）
		if (GEditor)
		{
			// 重新查找以执行Select
			FString Path;
			Data->TryGetStringField(TEXT("path"), Path);
			AActor* Actor = Cast<AActor>(StaticFindObject(AActor::StaticClass(), nullptr, *Path));
			if (Actor)
			{
				GEditor->SelectNone(false, true, false);
				GEditor->SelectActor(Actor, true, true);
				GEditor->NoteSelectionChange();
			}
		}
#endif
		UAL_CommandUtils::AddWorldInfo(Data);
		UAL_CommandUtils::SendResponse(RequestId, 200, Data);
	}
	else
	{
		UAL_CommandUtils::SendError(RequestId, 500, SingleFailReason.IsEmpty()
			? FString(TEXT("Spawn failed"))
			: FString::Printf(TEXT("Spawn failed: %s"), *SingleFailReason));
	}
}

bool FUAL_ActorCommands::DestroySingleActor(const FString& Name, const FString& Path)
{
	if (Name.IsEmpty() && Path.IsEmpty())
	{
		return false;
	}

	UWorld* World = UAL_CommandUtils::GetLiveWorld();
	if (!World)
	{
		return false;
	}

	AActor* TargetActor = nullptr;
	if (!Path.IsEmpty())
	{
		TargetActor = FindObject<AActor>(nullptr, *Path);
		if (!TargetActor)
		{
			TargetActor = Cast<AActor>(StaticFindObject(AActor::StaticClass(), nullptr, *Path));
		}
	}

	if (!TargetActor && !Name.IsEmpty())
	{
		TargetActor = UAL_CommandUtils::FindActorByLabel(World, Name);
	}

	if (!TargetActor)
	{
		return false;
	}

#if WITH_EDITOR
	// 使用 EditorDestroyActor 以支持撤销操作
	if (World)
	{
		return World->EditorDestroyActor(TargetActor, true);
	}
#endif
	return TargetActor->Destroy();
}

void FUAL_ActorCommands::Handle_DestroyActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	// 创建撤销事务，使删除操作可通过 Ctrl+Z 撤销
	FUAL_ScopedTransaction Transaction(UAL_CommandUtils::LText(TEXT("删除Actor"), TEXT("Delete Actor")));
#endif

	// 新版：targets 选择器
	const TSharedPtr<FJsonObject>* TargetsObj = nullptr;
	if (Payload->TryGetObjectField(TEXT("targets"), TargetsObj) && TargetsObj && TargetsObj->IsValid())
	{
		UWorld* World = UAL_CommandUtils::GetLiveWorld();
		if (!World)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("World not available"));
			return;
		}

		TSet<AActor*> TargetSet;
		FString TargetError;
		TArray<FString> Unmatched;
		if (!UAL_CommandUtils::ResolveTargetsToActors(*TargetsObj, World, TargetSet, TargetError, &Unmatched))
		{
			UAL_CommandUtils::SendError(RequestId, 404, TargetError);
			return;
		}

		int32 SuccessCount = 0;
		TArray<TSharedPtr<FJsonValue>> Deleted;
		TArray<TSharedPtr<FJsonValue>> Failed;
		for (AActor* Actor : TargetSet)
		{
			if (!Actor)
			{
				continue;
			}
			const FString FriendlyName = UAL_CommandUtils::GetActorFriendlyName(Actor);
			const FString ActorPath = Actor->GetPathName();
			const FString ActorClass = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();

			bool bDestroyed = false;
#if WITH_EDITOR
			// 使用 EditorDestroyActor 以支持撤销操作
			bDestroyed = World->EditorDestroyActor(Actor, true);
#else
			bDestroyed = Actor->Destroy();
#endif
			if (bDestroyed)
			{
				SuccessCount++;
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("name"), FriendlyName);
				Obj->SetStringField(TEXT("path"), ActorPath);
				if (!ActorClass.IsEmpty())
				{
					Obj->SetStringField(TEXT("class"), ActorClass);
				}
				Deleted.Add(MakeShared<FJsonValueObject>(Obj));
			}
			else
			{
				// 找到了却没删掉（EditorDestroyActor 返回 false：WorldSettings、
				// 被锁的、引擎不让删的）。以前这种直接从两张表里消失，
				// 回执只剩「6 / 7」，调用方不知道是哪一个、为什么
				TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
				Fail->SetStringField(TEXT("name"), FriendlyName);
				Fail->SetStringField(TEXT("path"), ActorPath);
				if (!ActorClass.IsEmpty())
				{
					Fail->SetStringField(TEXT("class"), ActorClass);
				}
				Fail->SetStringField(TEXT("reason"), TEXT("the editor refused to destroy this actor"));
				Failed.Add(MakeShared<FJsonValueObject>(Fail));
			}
		}

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("count"), SuccessCount);
		Data->SetNumberField(TEXT("target_count"), TargetSet.Num());
		Data->SetArrayField(TEXT("deleted_actors"), Deleted);
		Data->SetNumberField(TEXT("failed_count"), Failed.Num());
		if (Failed.Num() > 0)
		{
			Data->SetArrayField(TEXT("failed"), Failed);
		}
		// 点名删 7 个只找到 6 个时，第 7 个的名字必须出现在响应里。
		// 这条链路是不可逆的：静默少删一个，调用方会告诉用户「删完了」
		UAL_CommandUtils::AddUnmatchedTargets(Data, Unmatched);

		// 找到了但一个都没删掉是「失败」，不是「没找到」—— 404 在调用方那里会被读成
		// 合法的空结果（见 TS 侧 EngineNotFoundError）
		int32 Code = 200;
		if (SuccessCount == 0)
		{
			Code = Failed.Num() > 0 ? 500 : 404;
			if (Failed.Num() > 0)
			{
				Data->SetStringField(TEXT("error"), FString::Printf(
					TEXT("None of the %d matched actors could be destroyed"), Failed.Num()));
			}
		}
		UAL_CommandUtils::SendResponse(RequestId, Code, Data);
		return;
	}

	// 兼容旧：name/path
	FString Label;
	FString Path;
	Payload->TryGetStringField(TEXT("name"), Label);
	Payload->TryGetStringField(TEXT("path"), Path);

	const bool bDestroyed = DestroySingleActor(Label, Path);
	if (!bDestroyed)
	{
		UAL_CommandUtils::SendError(RequestId, 404, TEXT("Actor not found or failed to destroy"));
		return;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("ok"), bDestroyed);
	Data->SetStringField(TEXT("name"), Label);
	if (!Path.IsEmpty())
	{
		Data->SetStringField(TEXT("path"), Path);
	}
	Data->SetNumberField(TEXT("count"), 1);

	TArray<TSharedPtr<FJsonValue>> Deleted;
	TSharedPtr<FJsonObject> DeletedObj = MakeShared<FJsonObject>();
	DeletedObj->SetStringField(TEXT("name"), Label);
	if (!Path.IsEmpty())
	{
		DeletedObj->SetStringField(TEXT("path"), Path);
	}
	Deleted.Add(MakeShared<FJsonValueObject>(DeletedObj));
	Data->SetArrayField(TEXT("deleted_actors"), Deleted);

	UAL_CommandUtils::AddWorldInfo(Data);
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_ActorCommands::Handle_GetActorInfo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const TSharedPtr<FJsonObject>* TargetsObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("targets"), TargetsObj) || !TargetsObj || !TargetsObj->IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing object: targets"));
		return;
	}

	bool bReturnTransform = true;
	Payload->TryGetBoolField(TEXT("return_transform"), bReturnTransform);

	bool bReturnBounds = false;
	Payload->TryGetBoolField(TEXT("return_bounds"), bReturnBounds);

	int32 Limit = 50;
	const bool bHasLimitField = Payload->TryGetNumberField(TEXT("limit"), Limit);
	const bool bCountOnly = bHasLimitField && Limit == 0;
	if (!bCountOnly && Limit <= 0)
	{
		Limit = 50;
	}

	bool bIncludeSystemActors = false;
	Payload->TryGetBoolField(TEXT("include_system_actors"), bIncludeSystemActors);

	UWorld* World = UAL_CommandUtils::GetLiveWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	TSet<AActor*> TargetSet;
	FString TargetError;
	TArray<FString> Unmatched;
	if (!UAL_CommandUtils::ResolveTargetsToActors(*TargetsObj, World, TargetSet, TargetError, &Unmatched))
	{
		// 同 Handle_GetActor：查询查不到东西是一个合法答案，不是故障。
		// `actor.get_info` 是盒子侧 ue_get_actor 真正调用的命令
		// （`actor.get` 反而没有工具在用），所以这里不改的话前面那处等于没改。
		if (TargetError == TEXT("No actor matched targets"))
		{
			TSharedPtr<FJsonObject> Empty = MakeShared<FJsonObject>();
			Empty->SetNumberField(TEXT("count"), 0);
			Empty->SetNumberField(TEXT("total_found"), 0);
			Empty->SetArrayField(TEXT("actors"), TArray<TSharedPtr<FJsonValue>>());
			UAL_CommandUtils::AddUnmatchedTargets(Empty, Unmatched);
			UAL_CommandUtils::SendResponse(RequestId, 200, Empty);
			return;
		}
		UAL_CommandUtils::SendError(RequestId, 404, TargetError);
		return;
	}

	// 扫场景时把引擎自己的记账对象摘掉。必须在 TotalFound / limit 截断**之前**做：
	// 放到后面就是先让 144 个 HLOD 把 50 条的额度吃光，再从残渣里过滤。
	TMap<FString, int32> SystemByClass;
	int32 SystemExcluded = 0;
	if (!bIncludeSystemActors && UAL_CommandUtils::IsScanTargets(*TargetsObj))
	{
		SystemExcluded = UAL_CommandUtils::ExcludeSystemActors(TargetSet, SystemByClass);
	}

	const int32 TotalFound = TargetSet.Num();

	TArray<AActor*> TargetArray = TargetSet.Array();
	Algo::Sort(TargetArray, [](AActor* A, AActor* B)
	{
		const FString NameA = UAL_CommandUtils::GetActorFriendlyName(A);
		const FString NameB = UAL_CommandUtils::GetActorFriendlyName(B);
		return NameA < NameB;
	});

	TArray<TSharedPtr<FJsonValue>> ActorsJson;
	if (!bCountOnly)
	{
		for (int32 Index = 0; Index < TargetArray.Num() && Index < Limit; ++Index)
		{
			if (TSharedPtr<FJsonObject> Info = UAL_CommandUtils::BuildActorInfoWithOptions(TargetArray[Index], bReturnTransform, bReturnBounds))
			{
				ActorsJson.Add(MakeShared<FJsonValueObject>(Info));
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), ActorsJson.Num());
	Data->SetNumberField(TEXT("total_found"), TotalFound);
	Data->SetArrayField(TEXT("actors"), ActorsJson);
	UAL_CommandUtils::AddSystemActorExclusionInfo(Data, SystemExcluded, SystemByClass);
	UAL_CommandUtils::AddUnmatchedTargets(Data, Unmatched);

	UAL_CommandUtils::AddWorldInfo(Data);
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_ActorCommands::Handle_GetActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 兼容旧参数，同时复用统一 targets 解析逻辑
	const TSharedPtr<FJsonObject>* TargetsObjPtr = nullptr;
	TSharedPtr<FJsonObject> Targets;
	if (Payload->TryGetObjectField(TEXT("targets"), TargetsObjPtr) && TargetsObjPtr && TargetsObjPtr->IsValid())
	{
		Targets = *TargetsObjPtr;
	}
	else
	{
		FString Label;
		Payload->TryGetStringField(TEXT("name"), Label);

		FString Path;
		Payload->TryGetStringField(TEXT("path"), Path);

		if (Label.IsEmpty() && Path.IsEmpty())
		{
			UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: name or path"));
			return;
		}

		Targets = MakeShared<FJsonObject>();
		if (!Label.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> Names;
			Names.Add(MakeShared<FJsonValueString>(Label));
			Targets->SetArrayField(TEXT("names"), Names);
		}
		if (!Path.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> Paths;
			Paths.Add(MakeShared<FJsonValueString>(Path));
			Targets->SetArrayField(TEXT("paths"), Paths);
		}
	}

	if (!Targets.IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing object: targets"));
		return;
	}

	UWorld* World = UAL_CommandUtils::GetLiveWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	TSet<AActor*> TargetSet;
	FString TargetError;
	TArray<FString> Unmatched;
	if (!UAL_CommandUtils::ResolveTargetsToActors(Targets, World, TargetSet, TargetError, &Unmatched))
	{
		// 「一个都没匹配到」对**查询**来说是一个合法答案，不是故障。
		// 报成 404 的话调用方分不清「场景里确实没有」和「查询本身出错了」，
		// 而「先查再改」正是最常见的用法 —— 查不到就报错会让它在
		// 「这东西存在吗」这一步卡住，同样的查询重试两次还会撞上熔断。
		// 选择器本身写错（既没给 names 也没给 filter）仍然是错误。
		if (TargetError == TEXT("No actor matched targets"))
		{
			TSharedPtr<FJsonObject> Empty = MakeShared<FJsonObject>();
			Empty->SetNumberField(TEXT("count"), 0);
			Empty->SetArrayField(TEXT("actors"), TArray<TSharedPtr<FJsonValue>>());
			UAL_CommandUtils::AddUnmatchedTargets(Empty, Unmatched);
			UAL_CommandUtils::SendResponse(RequestId, 200, Empty);
			return;
		}
		UAL_CommandUtils::SendError(RequestId, 404, TargetError);
		return;
	}

	TArray<AActor*> TargetArray = TargetSet.Array();
	Algo::Sort(TargetArray, [](AActor* A, AActor* B)
	{
		const FString NameA = UAL_CommandUtils::GetActorFriendlyName(A);
		const FString NameB = UAL_CommandUtils::GetActorFriendlyName(B);
		return NameA < NameB;
	});

	AActor* TargetActor = TargetArray.Num() > 0 ? TargetArray[0] : nullptr;
	if (!TargetActor)
	{
		UAL_CommandUtils::SendError(RequestId, 404, TEXT("Actor not found"));
		return;
	}

	if (TSharedPtr<FJsonObject> Info = UAL_CommandUtils::BuildActorInfo(TargetActor))
	{
		UAL_CommandUtils::AddUnmatchedTargets(Info, Unmatched);
		UAL_CommandUtils::AddWorldInfo(Info);
		UAL_CommandUtils::SendResponse(RequestId, 200, Info);
	}
	else
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to build actor info"));
	}
}

void FUAL_ActorCommands::Handle_InspectActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const TSharedPtr<FJsonObject>* TargetsObjPtr = nullptr;
	if (!Payload->TryGetObjectField(TEXT("targets"), TargetsObjPtr) || !TargetsObjPtr || !TargetsObjPtr->IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing object: targets"));
		return;
	}
	const TSharedPtr<FJsonObject> Targets = *TargetsObjPtr;

	// properties: 若为空/缺省则使用默认白名单
	TArray<FString> WantedProps;
	const TArray<TSharedPtr<FJsonValue>>* PropsArr = nullptr;
	if (Payload->TryGetArrayField(TEXT("properties"), PropsArr) && PropsArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *PropsArr)
		{
			FString PropName;
			if (V.IsValid() && V->TryGetString(PropName) && !PropName.IsEmpty())
			{
				WantedProps.Add(PropName);
			}
		}
	}
	if (WantedProps.Num() == 0)
	{
		WantedProps = UAL_CommandUtils::GetDefaultInspectProps();
	}

	bool bIncludeSystemActors = false;
	Payload->TryGetBoolField(TEXT("include_system_actors"), bIncludeSystemActors);

	UWorld* World = UAL_CommandUtils::GetLiveWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	TSet<AActor*> TargetSet;
	FString TargetError;
	TArray<FString> Unmatched;
	if (!UAL_CommandUtils::ResolveTargetsToActors(Targets, World, TargetSet, TargetError, &Unmatched))
	{
		// 「一个都没匹配到」对**查询**来说是一个合法答案，不是故障。
		// 报成 404 的话调用方分不清「场景里确实没有」和「查询本身出错了」，
		// 而「先查再改」正是最常见的用法 —— 查不到就报错会让它在
		// 「这东西存在吗」这一步卡住，同样的查询重试两次还会撞上熔断。
		// 选择器本身写错（既没给 names 也没给 filter）仍然是错误。
		if (TargetError == TEXT("No actor matched targets"))
		{
			TSharedPtr<FJsonObject> Empty = MakeShared<FJsonObject>();
			Empty->SetNumberField(TEXT("count"), 0);
			Empty->SetArrayField(TEXT("actors"), TArray<TSharedPtr<FJsonValue>>());
			UAL_CommandUtils::AddUnmatchedTargets(Empty, Unmatched);
			UAL_CommandUtils::SendResponse(RequestId, 200, Empty);
			return;
		}
		UAL_CommandUtils::SendError(RequestId, 404, TargetError);
		return;
	}

	// 同 actor.get_info：扫场景时摘掉引擎的记账对象。内省这条路更受不了它们 ——
	// 每个 Actor 还要带一组属性值，144 个 HLOD 能把上下文顶穿。
	TMap<FString, int32> SystemByClass;
	int32 SystemExcluded = 0;
	if (!bIncludeSystemActors && UAL_CommandUtils::IsScanTargets(Targets))
	{
		SystemExcluded = UAL_CommandUtils::ExcludeSystemActors(TargetSet, SystemByClass);
	}

	TArray<AActor*> TargetArray = TargetSet.Array();
	Algo::Sort(TargetArray, [](AActor* A, AActor* B)
	{
		const FString NameA = UAL_CommandUtils::GetActorFriendlyName(A);
		const FString NameB = UAL_CommandUtils::GetActorFriendlyName(B);
		return NameA < NameB;
	});

	TArray<TSharedPtr<FJsonValue>> Results;
	for (AActor* Actor : TargetArray)
	{
		if (!Actor)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Obj = UAL_CommandUtils::BuildActorInfo(Actor);
		if (!Obj.IsValid())
		{
			continue;
		}

		if (TSharedPtr<FJsonObject> Props = UAL_CommandUtils::BuildSelectedProps(Actor, WantedProps))
		{
			Obj->SetObjectField(TEXT("props"), Props);
		}

		Results.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), Results.Num());
	Data->SetArrayField(TEXT("actors"), Results);
	UAL_CommandUtils::AddSystemActorExclusionInfo(Data, SystemExcluded, SystemByClass);
	UAL_CommandUtils::AddUnmatchedTargets(Data, Unmatched);

	UAL_CommandUtils::AddWorldInfo(Data);
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_ActorCommands::Handle_SetProperty(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	// 创建撤销事务，使属性修改操作可通过 Ctrl+Z 撤销
	FUAL_ScopedTransaction Transaction(UAL_CommandUtils::LText(TEXT("修改Actor属性"), TEXT("Modify Actor Property")));
#endif

	const TSharedPtr<FJsonObject>* TargetsObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("targets"), TargetsObj) || !TargetsObj || !TargetsObj->IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing object: targets"));
		return;
	}

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("properties"), PropsObj) || !PropsObj || !PropsObj->IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing object: properties"));
		return;
	}

	UWorld* World = UAL_CommandUtils::GetLiveWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	TSet<AActor*> TargetSet;
	FString TargetError;
	TArray<FString> Unmatched;
	if (!UAL_CommandUtils::ResolveTargetsToActors(*TargetsObj, World, TargetSet, TargetError, &Unmatched))
	{
		UAL_CommandUtils::SendError(RequestId, 404, TargetError);
		return;
	}

	TArray<AActor*> TargetArray = TargetSet.Array();
	Algo::Sort(TargetArray, [](AActor* A, AActor* B)
	{
		const FString NameA = UAL_CommandUtils::GetActorFriendlyName(A);
		const FString NameB = UAL_CommandUtils::GetActorFriendlyName(B);
		return NameA < NameB;
	});

	int32 SuccessActors = 0;
	int32 FailedWrites = 0;
	TArray<TSharedPtr<FJsonValue>> ActorResults;

	for (AActor* Actor : TargetArray)
	{
		if (!Actor)
		{
			continue;
		}

		TSharedPtr<FJsonObject> ActorObj = UAL_CommandUtils::BuildActorInfo(Actor);
		if (!ActorObj.IsValid())
		{
			continue;
		}

		TSharedPtr<FJsonObject> Updated = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Errors;
		TArray<TSharedPtr<FJsonValue>> ReadbackUnavailable;

		TArray<FString> CandidateNames;
		UAL_CommandUtils::CollectPropertyNames(Actor, CandidateNames);
		if (USceneComponent* RootComp = Actor->GetRootComponent())
		{
			UAL_CommandUtils::CollectPropertyNames(RootComp, CandidateNames);
		}
		for (UActorComponent* Comp : Actor->GetComponents())
		{
			UAL_CommandUtils::CollectPropertyNames(Comp, CandidateNames);
		}

		for (const auto& Pair : (*PropsObj)->Values)
		{
			const FString PropName = UAL_JsonKey(Pair.Key);
			const TSharedPtr<FJsonValue>& DesiredValue = Pair.Value;

			// 特殊处理 ActorLabel：它是 Editor-Only 属性，需要调用专用函数 SetActorLabel
			// 该函数会自动处理名称冲突（自动添加后缀），而不是简单的内存读写
#if WITH_EDITOR
			if (PropName.Equals(TEXT("ActorLabel"), ESearchCase::IgnoreCase) ||
				PropName.Equals(TEXT("Label"), ESearchCase::IgnoreCase))
			{
				FString NewLabel;
				if (DesiredValue.IsValid() && DesiredValue->TryGetString(NewLabel) && !NewLabel.IsEmpty())
				{
					const FString OldLabel = Actor->GetActorLabel();
					Actor->SetActorLabel(NewLabel);
					const FString FinalLabel = Actor->GetActorLabel();
					
					Updated->SetStringField(TEXT("ActorLabel"), FinalLabel);
					
					// 如果最终标签与请求的不同，说明发生了名称冲突自动加后缀
					if (!FinalLabel.Equals(NewLabel))
					{
						TSharedPtr<FJsonObject> Warning = MakeShared<FJsonObject>();
						Warning->SetStringField(TEXT("property"), TEXT("ActorLabel"));
						Warning->SetStringField(TEXT("warning"), TEXT("Name conflict resolved with suffix"));
						Warning->SetStringField(TEXT("requested"), NewLabel);
						Warning->SetStringField(TEXT("actual"), FinalLabel);
						Errors.Add(MakeShared<FJsonValueObject>(Warning));
					}
				}
				else
				{
					TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
					Err->SetStringField(TEXT("property"), PropName);
					Err->SetStringField(TEXT("error"), TEXT("ActorLabel must be a non-empty string"));
					Errors.Add(MakeShared<FJsonValueObject>(Err));
				}
				continue;
			}
#endif

			// ========== 特殊属性拦截白名单 ==========
			// 以下属性在用户眼中是"属性"，但在 C++ 底层是"函数调用"或需要触发状态重建
			// 直接通过反射修改内存值不会生效，或不会触发渲染/物理更新

			// 1. FolderPath (世界大纲文件夹)
			// 直接改变量不会刷新大纲视图，必须调用 SetFolderPath
#if WITH_EDITOR
			if (PropName.Equals(TEXT("FolderPath"), ESearchCase::IgnoreCase))
			{
				FString NewPath;
				if (DesiredValue.IsValid() && DesiredValue->TryGetString(NewPath))
				{
					Actor->SetFolderPath(FName(*NewPath));
					Updated->SetStringField(TEXT("FolderPath"), Actor->GetFolderPath().ToString());
				}
				else
				{
					TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
					Err->SetStringField(TEXT("property"), PropName);
					Err->SetStringField(TEXT("error"), TEXT("FolderPath must be a string"));
					Errors.Add(MakeShared<FJsonValueObject>(Err));
				}
				continue;
			}
#endif

			// 2. SimulatePhysics (物理模拟)
			// 这是 RootComponent (UPrimitiveComponent) 的属性，必须调用 SetSimulatePhysics 触发物理状态重建
			if (PropName.Equals(TEXT("SimulatePhysics"), ESearchCase::IgnoreCase) ||
				PropName.Equals(TEXT("bSimulatePhysics"), ESearchCase::IgnoreCase))
			{
				bool bSimulate = false;
				if (DesiredValue.IsValid() && DesiredValue->TryGetBool(bSimulate))
				{
					UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
					if (PrimComp)
					{
						PrimComp->SetSimulatePhysics(bSimulate);
						Updated->SetBoolField(TEXT("SimulatePhysics"), PrimComp->IsSimulatingPhysics());
					}
					else
					{
						TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
						Err->SetStringField(TEXT("property"), PropName);
						Err->SetStringField(TEXT("error"), TEXT("Actor has no UPrimitiveComponent as RootComponent"));
						Errors.Add(MakeShared<FJsonValueObject>(Err));
					}
				}
				else
				{
					TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
					Err->SetStringField(TEXT("property"), PropName);
					Err->SetStringField(TEXT("error"), TEXT("SimulatePhysics must be a boolean"));
					Errors.Add(MakeShared<FJsonValueObject>(Err));
				}
				continue;
			}

			// 3. Mobility (移动性)
			// 修改移动性涉及光照失效、导航网格失效等，必须调用 SetMobility
			if (PropName.Equals(TEXT("Mobility"), ESearchCase::IgnoreCase))
			{
				USceneComponent* RootComp = Actor->GetRootComponent();
				if (RootComp)
				{
					FString MobilityStr;
					EComponentMobility::Type NewMobility = RootComp->Mobility;
					bool bValidInput = false;

					if (DesiredValue.IsValid() && DesiredValue->TryGetString(MobilityStr))
					{
						if (MobilityStr.Equals(TEXT("Static"), ESearchCase::IgnoreCase))
						{
							NewMobility = EComponentMobility::Static;
							bValidInput = true;
						}
						else if (MobilityStr.Equals(TEXT("Stationary"), ESearchCase::IgnoreCase))
						{
							NewMobility = EComponentMobility::Stationary;
							bValidInput = true;
						}
						else if (MobilityStr.Equals(TEXT("Movable"), ESearchCase::IgnoreCase))
						{
							NewMobility = EComponentMobility::Movable;
							bValidInput = true;
						}
					}
					else
					{
						// 尝试数字输入
						int32 MobilityInt = 0;
						if (DesiredValue.IsValid() && DesiredValue->TryGetNumber(MobilityInt))
						{
							if (MobilityInt >= 0 && MobilityInt <= 2)
							{
								NewMobility = static_cast<EComponentMobility::Type>(MobilityInt);
								bValidInput = true;
							}
						}
					}

					if (bValidInput)
					{
						RootComp->SetMobility(NewMobility);
						
						// 返回字符串形式的移动性
						FString ResultMobility;
						switch (RootComp->Mobility)
						{
						case EComponentMobility::Static: ResultMobility = TEXT("Static"); break;
						case EComponentMobility::Stationary: ResultMobility = TEXT("Stationary"); break;
						case EComponentMobility::Movable: ResultMobility = TEXT("Movable"); break;
						default: ResultMobility = TEXT("Unknown"); break;
						}
						Updated->SetStringField(TEXT("Mobility"), ResultMobility);
					}
					else
					{
						TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
						Err->SetStringField(TEXT("property"), PropName);
						Err->SetStringField(TEXT("error"), TEXT("Mobility must be 'Static', 'Stationary', or 'Movable'"));
						Errors.Add(MakeShared<FJsonValueObject>(Err));
					}
				}
				else
				{
					TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
					Err->SetStringField(TEXT("property"), PropName);
					Err->SetStringField(TEXT("error"), TEXT("Actor has no RootComponent"));
					Errors.Add(MakeShared<FJsonValueObject>(Err));
				}
				continue;
			}

			// 4. Hidden / bHidden (运行时显隐)
			// 调用 SetActorHiddenInGame，处理网络同步和子组件递归显隐
			// 注意：这是运行时隐藏，在编辑器视图中可能仍然可见
			if (PropName.Equals(TEXT("Hidden"), ESearchCase::IgnoreCase) ||
				PropName.Equals(TEXT("bHidden"), ESearchCase::IgnoreCase) ||
				PropName.Equals(TEXT("HiddenInGame"), ESearchCase::IgnoreCase) ||
				PropName.Equals(TEXT("bHiddenInGame"), ESearchCase::IgnoreCase))
			{
				bool bHidden = false;
				if (DesiredValue.IsValid() && DesiredValue->TryGetBool(bHidden))
				{
					Actor->SetActorHiddenInGame(bHidden);
					Updated->SetBoolField(TEXT("bHidden"), Actor->IsHidden());
				}
				else
				{
					TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
					Err->SetStringField(TEXT("property"), PropName);
					Err->SetStringField(TEXT("error"), TEXT("bHidden must be a boolean"));
					Errors.Add(MakeShared<FJsonValueObject>(Err));
				}
				continue;
			}

			// 5. HiddenInEditor / bHiddenEd (编辑器模式显隐)
			// 调用 SetIsTemporarilyHiddenInEditor，在编辑器视图中立即隐藏/显示
#if WITH_EDITOR
			if (PropName.Equals(TEXT("HiddenInEditor"), ESearchCase::IgnoreCase) ||
				PropName.Equals(TEXT("bHiddenInEditor"), ESearchCase::IgnoreCase) ||
				PropName.Equals(TEXT("bHiddenEd"), ESearchCase::IgnoreCase))
			{
				bool bHidden = false;
				if (DesiredValue.IsValid() && DesiredValue->TryGetBool(bHidden))
				{
					Actor->SetIsTemporarilyHiddenInEditor(bHidden);
					Updated->SetBoolField(TEXT("bHiddenInEditor"), Actor->IsTemporarilyHiddenInEditor());
				}
				else
				{
					TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
					Err->SetStringField(TEXT("property"), PropName);
					Err->SetStringField(TEXT("error"), TEXT("bHiddenInEditor must be a boolean"));
					Errors.Add(MakeShared<FJsonValueObject>(Err));
				}
				continue;
			}
#endif

			// 6. Tags (Actor 标签数组)
			// Actor::Tags 是 TArray<FName>，需要特殊处理
			// 支持：1) 数组覆盖 ["tag1", "tag2"]
			//       2) 单字符串添加 "tag1"
			//       3) 对象操作 { "add": ["tag1"], "remove": ["tag2"] }
			if (PropName.Equals(TEXT("Tags"), ESearchCase::IgnoreCase))
			{
				bool bSuccess = false;
				
				// 尝试解析为数组（覆盖模式）
				const TArray<TSharedPtr<FJsonValue>>* TagsArray = nullptr;
				if (DesiredValue.IsValid() && DesiredValue->TryGetArray(TagsArray) && TagsArray)
				{
					Actor->Tags.Empty();
					for (const TSharedPtr<FJsonValue>& TagVal : *TagsArray)
					{
						FString TagStr;
						if (TagVal.IsValid() && TagVal->TryGetString(TagStr) && !TagStr.IsEmpty())
						{
							Actor->Tags.AddUnique(FName(*TagStr));
						}
					}
					bSuccess = true;
				}
				// 尝试解析为对象（增删模式）
				else if (const TSharedPtr<FJsonObject>* TagsObj = nullptr; 
						 DesiredValue.IsValid() && DesiredValue->TryGetObject(TagsObj) && TagsObj && TagsObj->IsValid())
				{
					// 处理 add
					const TArray<TSharedPtr<FJsonValue>>* AddArray = nullptr;
					if ((*TagsObj)->TryGetArrayField(TEXT("add"), AddArray) && AddArray)
					{
						for (const TSharedPtr<FJsonValue>& TagVal : *AddArray)
						{
							FString TagStr;
							if (TagVal.IsValid() && TagVal->TryGetString(TagStr) && !TagStr.IsEmpty())
							{
								Actor->Tags.AddUnique(FName(*TagStr));
							}
						}
					}
					// 处理 remove
					const TArray<TSharedPtr<FJsonValue>>* RemoveArray = nullptr;
					if ((*TagsObj)->TryGetArrayField(TEXT("remove"), RemoveArray) && RemoveArray)
					{
						for (const TSharedPtr<FJsonValue>& TagVal : *RemoveArray)
						{
							FString TagStr;
							if (TagVal.IsValid() && TagVal->TryGetString(TagStr) && !TagStr.IsEmpty())
							{
								Actor->Tags.Remove(FName(*TagStr));
							}
						}
					}
					bSuccess = true;
				}
				// 尝试解析为单字符串（添加单个标签）
				else
				{
					FString SingleTag;
					if (DesiredValue.IsValid() && DesiredValue->TryGetString(SingleTag) && !SingleTag.IsEmpty())
					{
						Actor->Tags.AddUnique(FName(*SingleTag));
						bSuccess = true;
					}
				}

				if (bSuccess)
				{
					// 返回当前所有标签
					TArray<TSharedPtr<FJsonValue>> TagsJson;
					for (const FName& Tag : Actor->Tags)
					{
						TagsJson.Add(MakeShared<FJsonValueString>(Tag.ToString()));
					}
					Updated->SetArrayField(TEXT("Tags"), TagsJson);
				}
				else
				{
					TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
					Err->SetStringField(TEXT("property"), PropName);
					Err->SetStringField(TEXT("error"), TEXT("Tags must be a string, array of strings, or object with 'add'/'remove' arrays"));
					Errors.Add(MakeShared<FJsonValueObject>(Err));
				}
				continue;
			}

			// ========== 通用属性处理 ==========
			UObject* TargetObj = nullptr;
			FProperty* Prop = UAL_CommandUtils::FindWritablePropertyOnActorHierarchy(Actor, PropName, TargetObj);

			if (!Prop)
			{
				TArray<FString> Suggestions;
				UAL_CommandUtils::SuggestProperties(PropName, CandidateNames, Suggestions);

				TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
				Err->SetStringField(TEXT("property"), PropName);
				Err->SetStringField(TEXT("error"), TEXT("Property not found"));
				if (Suggestions.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> SuggestVals;
					for (const FString& S : Suggestions)
					{
						SuggestVals.Add(MakeShared<FJsonValueString>(S));
					}
					Err->SetArrayField(TEXT("suggestions"), SuggestVals);
				}
				Errors.Add(MakeShared<FJsonValueObject>(Err));
				continue;
			}

			/*
			 * 改之前先 Modify **被改的那个对象**。
			 *
			 * 事务记录的是 Modify() 被调用那一刻的状态，所以这一行的位置和对象
			 * 都不能错：FindWritablePropertyOnActorHierarchy 找到的 TargetObj
			 * 常常是组件（灯的颜色和强度就在 PointLightComponent 上，不在 Actor 上），
			 * 只对 Actor 调 Modify 记不到组件的改动；等改完再调，记下来的是新值，
			 * 撤销回去等于没撤。
			 *
			 * 这两种写法都不报错，事务里没东西时引擎会把整笔丢掉 ——
			 * 表现就是「改了灯的颜色，撤销历史里连这一步都不存在」。
			 */
#if WITH_EDITOR
			TargetObj->Modify();
#endif

			FString TypeError;
			if (UAL_CommandUtils::SetSimpleProperty(Prop, TargetObj, DesiredValue, TypeError))
			{
				// 通知引擎属性已更改，触发渲染刷新
#if WITH_EDITOR
				FPropertyChangedEvent ChangedEvent(Prop, EPropertyChangeType::ValueSet);
				TargetObj->PostEditChangeProperty(ChangedEvent);
				
				// 如果目标对象是组件，还需要标记组件渲染状态为脏
				if (UActorComponent* Comp = Cast<UActorComponent>(TargetObj))
				{
					Comp->MarkRenderStateDirty();
				}
#endif
				if (TSharedPtr<FJsonValue> JsonValue = UAL_CommandUtils::PropertyToJsonValueCompat(Prop, Prop->ContainerPtrToValuePtr<void>(TargetObj)))
				{
					Updated->SetField(PropName, JsonValue);
				}
				else
				{
					// 写进去了但读不回来（结构体/对象引用等序列化不了的类型）。
					// 以前这里把请求值原样当回读值塞进 updated —— 那等于替引擎作答，
					// 引擎实际夹紧/改写过的值调用方永远看不到。宁可不给值，明说读不回来
					ReadbackUnavailable.Add(MakeShared<FJsonValueString>(PropName));
				}
				continue;
			}

			TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
			Err->SetStringField(TEXT("property"), PropName);
			Err->SetStringField(TEXT("error"), TypeError.IsEmpty() ? TEXT("Failed to set property") : TypeError);
			if (TSharedPtr<FJsonValue> Current = UAL_CommandUtils::PropertyToJsonValueCompat(Prop, Prop->ContainerPtrToValuePtr<void>(TargetObj)))
			{
				Err->SetStringField(TEXT("expected_type"), Prop->GetClass()->GetName());
				Err->SetStringField(TEXT("current_value"), UAL_CommandUtils::JsonValueToString(Current));
			}
			Errors.Add(MakeShared<FJsonValueObject>(Err));
		}

		if (Updated->Values.Num() > 0 || ReadbackUnavailable.Num() > 0)
		{
			Actor->Modify();
			SuccessActors++;
			ActorObj->SetObjectField(TEXT("updated"), Updated);
		}
		if (ReadbackUnavailable.Num() > 0)
		{
			ActorObj->SetArrayField(TEXT("readback_unavailable"), ReadbackUnavailable);
		}
		if (Errors.Num() > 0)
		{
			ActorObj->SetArrayField(TEXT("errors"), Errors);
		}
		// errors 里混着 warning（如 ActorLabel 撞名加了后缀）—— 那条其实写成了，不算失败
		for (const TSharedPtr<FJsonValue>& ErrVal : Errors)
		{
			const TSharedPtr<FJsonObject>* ErrObj = nullptr;
			if (ErrVal.IsValid() && ErrVal->TryGetObject(ErrObj) && ErrObj && (*ErrObj)->HasField(TEXT("error")))
			{
				FailedWrites++;
			}
		}

		ActorResults.Add(MakeShared<FJsonValueObject>(ActorObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), SuccessActors);
	// failed_count 数的是「没写进去的属性」条数，不是 Actor 数：
	// 3 个 Actor 各有 1 条写失败时，count 依旧是 3，只看 count 会以为全成了
	Data->SetNumberField(TEXT("failed_count"), FailedWrites);
	Data->SetArrayField(TEXT("actors"), ActorResults);
	UAL_CommandUtils::AddUnmatchedTargets(Data, Unmatched);

	UAL_CommandUtils::AddWorldInfo(Data);

	// 一条都没写进去就是失败。以前这里恒回 200，调用方看到的是「成功修改 0 个」
	if (SuccessActors == 0)
	{
		Data->SetBoolField(TEXT("ok"), false);
		Data->SetStringField(TEXT("error"), FString::Printf(
			TEXT("No property was applied (%d property write(s) failed); see actors[].errors"), FailedWrites));
		UAL_CommandUtils::SendResponse(RequestId, 400, Data);
		return;
	}
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_ActorCommands::Handle_SetTransformUnified(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	UWorld* World = UAL_CommandUtils::GetLiveWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	const TSharedPtr<FJsonObject>* TargetsObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("targets"), TargetsObj) || !TargetsObj || !TargetsObj->IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing object: targets"));
		return;
	}

	const TSharedPtr<FJsonObject>* OpObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("operation"), OpObj) || !OpObj || !OpObj->IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing object: operation"));
		return;
	}

	const TSharedPtr<FJsonObject>& Targets = *TargetsObj;
	const TSharedPtr<FJsonObject>& Operation = *OpObj;

	// space
	FString SpaceStr;
	Operation->TryGetStringField(TEXT("space"), SpaceStr);
	const bool bLocalSpace = SpaceStr.Equals(TEXT("Local"), ESearchCase::IgnoreCase);

	bool bSnapToFloor = false;
	Operation->TryGetBoolField(TEXT("snap_to_floor"), bSnapToFloor);

	// set / add / multiply
	TSharedPtr<FJsonObject> SetObj, AddObj, MulObj;
	UAL_CommandUtils::TryGetObjectFieldFlexible(Operation, TEXT("set"), SetObj);
	UAL_CommandUtils::TryGetObjectFieldFlexible(Operation, TEXT("add"), AddObj);
	UAL_CommandUtils::TryGetObjectFieldFlexible(Operation, TEXT("multiply"), MulObj);

	// snap_to_floor 写错位置是真机上撞到过的一种：写进 operation.set 里，插件在
	// operation 顶层读不到，于是什么都不做还回 success。宁可当场 400 报出来。
	auto MisplacedSnap = [](const TSharedPtr<FJsonObject>& Obj)
	{
		bool bUnused = false;
		return Obj.IsValid() && Obj->TryGetBoolField(TEXT("snap_to_floor"), bUnused);
	};
	if (MisplacedSnap(SetObj) || MisplacedSnap(AddObj) || MisplacedSnap(MulObj))
	{
		UAL_CommandUtils::SendError(RequestId, 400,
			TEXT("snap_to_floor goes on operation itself, not inside set/add/multiply: { \"snap_to_floor\": true, \"set\": { ... } }"));
		return;
	}

	// snap_to_floor 单独给也是一条完整的操作（「就把他放到地上，别的不动」）。
	// 以前这里必须同时给 set/add/multiply，导致文档里的写法直接 400。
	if (!SetObj.IsValid() && !AddObj.IsValid() && !MulObj.IsValid() && !bSnapToFloor)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing operation fields: set/add/multiply/snap_to_floor"));
		return;
	}

	int32 SnapMissCount = 0;

	TSet<AActor*> TargetSet;
	FString TargetError;
	TArray<FString> Unmatched;
	if (!UAL_CommandUtils::ResolveTargetsToActors(Targets, World, TargetSet, TargetError, &Unmatched))
	{
		UAL_CommandUtils::SendError(RequestId, 404, TargetError);
		return;
	}

#if WITH_EDITOR
	FUAL_ScopedTransaction Transaction(UAL_CommandUtils::LText(TEXT("批量修改Actor变换"), TEXT("Batch Modify Actor Transform")));
#endif

	int32 AffectedCount = 0;
	int32 FailedCount = 0;
	TArray<TSharedPtr<FJsonValue>> Affected;
	TArray<TSharedPtr<FJsonValue>> Failed;
	const int32 MaxReport = 100;

	for (AActor* Actor : TargetSet)
	{
		if (!Actor)
		{
			continue;
		}

		FVector NewLocation = Actor->GetActorLocation();
		FRotator NewRotation = Actor->GetActorRotation();
		FVector NewScale = Actor->GetActorScale3D();

		if (SetObj.IsValid())
		{
			TSharedPtr<FJsonObject> LocObj, RotObj, ScaleObj;
			if (UAL_CommandUtils::TryGetObjectFieldFlexible(SetObj, TEXT("location"), LocObj))
			{
				NewLocation = UAL_CommandUtils::ReadVectorDirect(LocObj, NewLocation);
			}
			if (UAL_CommandUtils::TryGetObjectFieldFlexible(SetObj, TEXT("rotation"), RotObj))
			{
				NewRotation = UAL_CommandUtils::ReadRotatorDirect(RotObj, NewRotation);
			}
			if (UAL_CommandUtils::TryGetObjectFieldFlexible(SetObj, TEXT("scale"), ScaleObj))
			{
				NewScale = UAL_CommandUtils::ReadVectorDirect(ScaleObj, NewScale);
			}
		}

		if (AddObj.IsValid())
		{
			TSharedPtr<FJsonObject> LocObj, RotObj, ScaleObj;
			if (UAL_CommandUtils::TryGetObjectFieldFlexible(AddObj, TEXT("location"), LocObj))
			{
				FVector Delta = UAL_CommandUtils::ReadVectorDirect(LocObj, FVector::ZeroVector);
				if (bLocalSpace)
				{
					// 使用已累积的旋转结果，避免同一请求中先设置旋转再按旧旋转位移导致偏差
					Delta = NewRotation.RotateVector(Delta);
				}
				NewLocation += Delta;
			}
			if (UAL_CommandUtils::TryGetObjectFieldFlexible(AddObj, TEXT("rotation"), RotObj))
			{
				FRotator Delta = UAL_CommandUtils::ReadRotatorDirect(RotObj, FRotator::ZeroRotator);
				if (bLocalSpace)
				{
					FQuat Curr = NewRotation.Quaternion();
					FQuat Dq = Delta.Quaternion();
					NewRotation = (Curr * Dq).Rotator();
				}
				else
				{
					NewRotation += Delta;
				}
			}
			if (UAL_CommandUtils::TryGetObjectFieldFlexible(AddObj, TEXT("scale"), ScaleObj))
			{
				NewScale += UAL_CommandUtils::ReadVectorDirect(ScaleObj, FVector::ZeroVector);
			}
		}

		if (MulObj.IsValid())
		{
			TSharedPtr<FJsonObject> LocObj, RotObj, ScaleObj;
			if (UAL_CommandUtils::TryGetObjectFieldFlexible(MulObj, TEXT("location"), LocObj))
			{
				const FVector Mul = UAL_CommandUtils::ReadVectorDirect(LocObj, FVector(1, 1, 1));
				NewLocation.X *= Mul.X;
				NewLocation.Y *= Mul.Y;
				NewLocation.Z *= Mul.Z;
			}
			if (UAL_CommandUtils::TryGetObjectFieldFlexible(MulObj, TEXT("rotation"), RotObj))
			{
				const FRotator MulRot = UAL_CommandUtils::ReadRotatorDirect(RotObj, FRotator(1, 1, 1));
				NewRotation.Roll *= MulRot.Roll;
				NewRotation.Pitch *= MulRot.Pitch;
				NewRotation.Yaw *= MulRot.Yaw;
			}
			if (UAL_CommandUtils::TryGetObjectFieldFlexible(MulObj, TEXT("scale"), ScaleObj))
			{
				const FVector Mul = UAL_CommandUtils::ReadVectorDirect(ScaleObj, FVector(1, 1, 1));
				NewScale.X *= Mul.X;
				NewScale.Y *= Mul.Y;
				NewScale.Z *= Mul.Z;
			}
		}

		Actor->Modify();
		const bool bMoved = Actor->SetActorLocationAndRotation(NewLocation, NewRotation, false, nullptr, ETeleportType::TeleportPhysics);
		Actor->SetActorScale3D(NewScale);

		/*
		 * 写完立刻回读，和目标值比一遍。
		 *
		 * SetActorLocationAndRotation 在没有 RootComponent 时返回 false、什么都不动；
		 * 挂在父级下、被约束的组件也可能落不到目标上。以前回执里写的是这里算出来的
		 * NewLocation —— 引擎没动，回执照样说「到了」（AGENTS.md §5 第 14 条）。
		 *
		 * 旋转用四元数比：同一个朝向有多种欧拉角写法（pitch 100 ≡ pitch 80 + yaw/roll 180），
		 * 按分量比会把引擎规范化过的等价旋转误判成没到位。
		 * 比较要在贴地**之前**做 —— 贴地本来就会改 z。
		 */
		FString MismatchReason;
		if (!bMoved)
		{
			MismatchReason = Actor->GetRootComponent()
				? TEXT("the engine refused to move this actor")
				: TEXT("actor has no root component, so it has no transform to set");
		}
		else
		{
			TArray<FString> Off;
			if (!Actor->GetActorLocation().Equals(NewLocation, 0.1))
			{
				Off.Add(TEXT("location"));
			}
			if (!Actor->GetActorQuat().Equals(NewRotation.Quaternion(), 1.e-4f))
			{
				Off.Add(TEXT("rotation"));
			}
			if (!Actor->GetActorScale3D().Equals(NewScale, 1.e-3f))
			{
				Off.Add(TEXT("scale"));
			}
			if (Off.Num() > 0)
			{
				MismatchReason = FString::Printf(
					TEXT("%s did not land on the requested value (attached to a parent, constrained, or clamped by the engine)"),
					*FString::Join(Off, TEXT("/")));
			}
		}

		TSharedPtr<FJsonObject> SnapInfo;
		if (bSnapToFloor && MismatchReason.IsEmpty())
		{
			if (!UAL_SnapToFloor::SnapActor(Actor, World, SnapInfo))
			{
				SnapMissCount++;
			}
		}

		if (!MismatchReason.IsEmpty())
		{
			FailedCount++;
			if (Failed.Num() < MaxReport)
			{
				TSharedPtr<FJsonObject> Fail = MakeShared<FJsonObject>();
				Fail->SetStringField(TEXT("name"), UAL_CommandUtils::GetActorFriendlyName(Actor));
				Fail->SetStringField(TEXT("path"), Actor->GetPathName());
				Fail->SetStringField(TEXT("reason"), MismatchReason);
				Failed.Add(MakeShared<FJsonValueObject>(Fail));
			}
		}
		else
		{
			AffectedCount++;
		}

		if (Affected.Num() < MaxReport)
		{
			TSharedPtr<FJsonObject> Obj = UAL_CommandUtils::BuildActorInfo(Actor);
			if (Obj.IsValid())
			{
				if (SnapInfo.IsValid())
				{
					Obj->SetObjectField(TEXT("snap"), SnapInfo);
				}
				// 回执里的变换一律是引擎此刻的值（贴地之后），不是这里算出来的目标值
				Obj->SetObjectField(TEXT("location"), UAL_CommandUtils::MakeVectorJson(Actor->GetActorLocation()));
				Obj->SetObjectField(TEXT("rotation"), UAL_CommandUtils::MakeRotatorJson(Actor->GetActorRotation()));
				Obj->SetObjectField(TEXT("scale"), UAL_CommandUtils::MakeVectorJson(Actor->GetActorScale3D()));
				if (!MismatchReason.IsEmpty())
				{
					// 没到位的那个，把目标值也给出来，调用方才看得出差在哪
					TSharedPtr<FJsonObject> Requested = MakeShared<FJsonObject>();
					Requested->SetObjectField(TEXT("location"), UAL_CommandUtils::MakeVectorJson(NewLocation));
					Requested->SetObjectField(TEXT("rotation"), UAL_CommandUtils::MakeRotatorJson(NewRotation));
					Requested->SetObjectField(TEXT("scale"), UAL_CommandUtils::MakeVectorJson(NewScale));
					Obj->SetObjectField(TEXT("requested"), Requested);
					Obj->SetBoolField(TEXT("applied"), false);
					Obj->SetStringField(TEXT("warning"), MismatchReason);
				}
				Affected.Add(MakeShared<FJsonValueObject>(Obj));
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), AffectedCount);
	Data->SetNumberField(TEXT("failed_count"), FailedCount);
	if (Failed.Num() > 0)
	{
		Data->SetArrayField(TEXT("failed"), Failed);
	}
	if (Affected.Num() > 0)
	{
		Data->SetArrayField(TEXT("actors"), Affected);
		Data->SetNumberField(TEXT("reported"), Affected.Num());
		Data->SetNumberField(TEXT("report_limit"), MaxReport);
	}
	if (bSnapToFloor)
	{
		Data->SetNumberField(TEXT("snap_missed"), SnapMissCount);
	}
	UAL_CommandUtils::AddUnmatchedTargets(Data, Unmatched);

	UAL_CommandUtils::AddWorldInfo(Data);

	// 一个都没到位就是失败，不能回 200 让调用方读成「变换完成」
	if (AffectedCount == 0 && FailedCount > 0)
	{
		Data->SetBoolField(TEXT("ok"), false);
		FString FirstReason;
		if (Failed.Num() > 0)
		{
			Failed[0]->AsObject()->TryGetStringField(TEXT("reason"), FirstReason);
		}
		Data->SetStringField(TEXT("error"), FString::Printf(
			TEXT("None of the %d actors reached the requested transform: %s"), FailedCount, *FirstReason));
		UAL_CommandUtils::SendResponse(RequestId, 500, Data);
		return;
	}
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}
