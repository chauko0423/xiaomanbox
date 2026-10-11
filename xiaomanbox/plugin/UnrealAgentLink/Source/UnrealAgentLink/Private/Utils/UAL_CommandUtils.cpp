#include "UAL_CommandUtils.h"
#include "UAL_NetworkManager.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"
#include "Engine/World.h"
#include "Engine/Engine.h"  // GEngine->GetWorldContexts()：找正在跑的 PIE 世界
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Engine/DirectionalLight.h"
#include "Engine/RectLight.h"
#include "Camera/CameraActor.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Components/SceneComponent.h"
// CollisionProfileName 不是反射属性，只能走 setter（见 TrySetDerivedProperty）
#include "Components/PrimitiveComponent.h"
#include "Engine/CollisionProfile.h"
#include "Serialization/JsonSerializer.h"
#include "JsonObjectConverter.h"
#include "Algo/Sort.h"
// 找同名的活类要遍历全部已加载的 UClass（见 UAL_FindLiveClassByName）
#include "UObject/UObjectIterator.h"
#include "Misc/EngineVersion.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/IConsoleManager.h"
// SetSimpleProperty 的兜底走 PropertyValueFromString（ImportText）
#include "Kismet2/BlueprintEditorUtils.h"

#if WITH_EDITOR
#include "Selection.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUALUtils, Log, All);

// 批量创建上限：默认 50，可在控制台/命令行设置：ual.MaxBatchCreate 50
// 注意：<= 0 表示不限制（用于调试/内网环境）。
static TAutoConsoleVariable<int32> CVarUALMaxBatchCreate(
	TEXT("ual.MaxBatchCreate"),
	50,
	TEXT("Max items allowed for batch create operations (<=0 means unlimited)."),
	ECVF_Default);

bool UAL_CommandUtils::IsZh()
{
	FString Name;
	if (const TSharedPtr<const FCulture> Culture = FInternationalization::Get().GetCurrentCulture())
	{
		Name = Culture->GetName();
	}
	return Name.StartsWith(TEXT("zh"));
}

FString UAL_CommandUtils::LStr(const TCHAR* Zh, const TCHAR* En)
{
	return IsZh() ? Zh : En;
}

FText UAL_CommandUtils::LText(const TCHAR* Zh, const TCHAR* En)
{
	return FText::FromString(LStr(Zh, En));
}

int32 UAL_CommandUtils::GetMaxBatchCreate()
{
	return CVarUALMaxBatchCreate.GetValueOnAnyThread();
}

UWorld* UAL_CommandUtils::GetTargetWorld()
{
#if WITH_EDITOR
	if (GEditor)
	{
		if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
		{
			return EditorWorld;
		}
	}
#endif
	return GWorld;
}

int32 UAL_CommandUtils::GetPlayWorldCount()
{
	int32 Count = 0;
#if WITH_EDITOR
	if (GEngine)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.World())
			{
				++Count;
			}
		}
	}
#endif
	return Count;
}

bool UAL_CommandUtils::IsPlayInProgress()
{
#if WITH_EDITOR
	// GEditor->PlayWorld 覆盖 PIE；bIsSimulatingInEditor 覆盖 Simulate（那时没有
	// PlayerController，但世界确实在 tick，读编辑器世界同样是错的）
	if (GEditor && (GEditor->PlayWorld != nullptr || GEditor->bIsSimulatingInEditor))
	{
		return true;
	}
#endif
	return GetPlayWorldCount() > 0;
}

UWorld* UAL_CommandUtils::GetLiveWorld(int32 PlayerIndex)
{
#if WITH_EDITOR
	if (GEngine)
	{
		UWorld* FirstPlayWorld = nullptr;
		int32 Seen = 0;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType != EWorldType::PIE || !Context.World())
			{
				continue;
			}
			if (!FirstPlayWorld)
			{
				FirstPlayWorld = Context.World();
			}
			if (Seen == PlayerIndex)
			{
				return Context.World();
			}
			++Seen;
		}
		if (FirstPlayWorld)
		{
			// 越界不算失败：多客户端时调用方多半只是没传 index。
			// 给第一个，但不谎称是它要的那个 —— 调用方靠 WorldKindName 与
			// GetPlayWorldCount 自己核对
			return FirstPlayWorld;
		}
	}
#endif
	return GetTargetWorld();
}

const TCHAR* UAL_CommandUtils::WorldKindName(const UWorld* World)
{
	if (!World)
	{
		return TEXT("unknown");
	}
	return World->WorldType == EWorldType::PIE ? TEXT("pie") : TEXT("editor");
}

void UAL_CommandUtils::AddWorldInfo(const TSharedPtr<FJsonObject>& Data)
{
	if (!Data.IsValid())
	{
		return;
	}

	const UWorld* World = GetLiveWorld();
	Data->SetStringField(TEXT("world"), WorldKindName(World));

	if (World && World->WorldType == EWorldType::PIE)
	{
		// 这句是给模型看的。只给一个 world:"pie" 字段，它多半会把
		// 「改成功了」直接转述给用户，而那个改动在用户按停止的瞬间就没了
		Data->SetStringField(TEXT("world_note"),
			LStr(TEXT("读写的是正在运行的游戏世界。这里的改动不会落盘，停止运行就没了；"
					  "要改到关卡里，先停止运行再操作。"),
				 TEXT("This targets the running PIE world. Changes here are discarded when play "
					  "stops; stop play first to modify the level itself.")));

		const int32 Count = GetPlayWorldCount();
		if (Count > 1)
		{
			// 多客户端时结论只针对其中一个世界，不说清楚就是在鼓励过度概括
			Data->SetNumberField(TEXT("play_world_count"), Count);
		}
	}
}

bool UAL_CommandUtils::RefuseDuringPlay(const FString& RequestId, const TCHAR* ZhWhat, const TCHAR* EnWhat)
{
	if (!IsPlayInProgress())
	{
		return false;
	}

	// 错误文案要带**怎么办**。只说「不行」会让模型换个工具再试一次，
	// 说了「先停 PIE」它才知道下一步做什么
	const FString Message = LStr(
		*FString::Printf(TEXT("游戏正在运行（PIE），现在不能%s —— 这是编辑器动作，")
			TEXT("对运行中的世界没有意义。请先停止运行再试。"), ZhWhat),
		*FString::Printf(TEXT("The game is running in PIE; %s is an editor-only action and ")
			TEXT("cannot be done now. Stop play mode first."), EnWhat));

	SendError(RequestId, 409, Message);
	return true;
}

FVector UAL_CommandUtils::ReadVector(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const FVector& DefaultValue)
{
	const TSharedPtr<FJsonObject>* Sub = nullptr;
	if (!Obj->TryGetObjectField(Field, Sub) || !Sub || !Sub->IsValid())
	{
		return DefaultValue;
	}

	double X = DefaultValue.X, Y = DefaultValue.Y, Z = DefaultValue.Z;
	(*Sub)->TryGetNumberField(TEXT("x"), X);
	(*Sub)->TryGetNumberField(TEXT("y"), Y);
	(*Sub)->TryGetNumberField(TEXT("z"), Z);
	return FVector(X, Y, Z);
}

FRotator UAL_CommandUtils::ReadRotator(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const FRotator& DefaultValue)
{
	const TSharedPtr<FJsonObject>* Sub = nullptr;
	if (!Obj->TryGetObjectField(Field, Sub) || !Sub || !Sub->IsValid())
	{
		return DefaultValue;
	}

	double Pitch = DefaultValue.Pitch, Yaw = DefaultValue.Yaw, Roll = DefaultValue.Roll;
	(*Sub)->TryGetNumberField(TEXT("pitch"), Pitch);
	(*Sub)->TryGetNumberField(TEXT("yaw"), Yaw);
	(*Sub)->TryGetNumberField(TEXT("roll"), Roll);
	return FRotator(Pitch, Yaw, Roll);
}

FVector UAL_CommandUtils::ReadVectorDirect(const TSharedPtr<FJsonObject>& Obj, const FVector& DefaultValue)
{
	if (!Obj.IsValid())
	{
		return DefaultValue;
	}
	double X = DefaultValue.X, Y = DefaultValue.Y, Z = DefaultValue.Z;
	Obj->TryGetNumberField(TEXT("x"), X);
	Obj->TryGetNumberField(TEXT("y"), Y);
	Obj->TryGetNumberField(TEXT("z"), Z);
	return FVector(X, Y, Z);
}

FRotator UAL_CommandUtils::ReadRotatorDirect(const TSharedPtr<FJsonObject>& Obj, const FRotator& DefaultValue)
{
	if (!Obj.IsValid())
	{
		return DefaultValue;
	}
	double Pitch = DefaultValue.Pitch, Yaw = DefaultValue.Yaw, Roll = DefaultValue.Roll;
	Obj->TryGetNumberField(TEXT("pitch"), Pitch);
	Obj->TryGetNumberField(TEXT("yaw"), Yaw);
	Obj->TryGetNumberField(TEXT("roll"), Roll);
	return FRotator(Pitch, Yaw, Roll);
}

bool UAL_CommandUtils::TryGetObjectFieldFlexible(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Field, TSharedPtr<FJsonObject>& OutObj)
{
	const TSharedPtr<FJsonObject>* Sub = nullptr;
	if (Parent->TryGetObjectField(Field, Sub) && Sub && Sub->IsValid())
	{
		OutObj = *Sub;
		return true;
	}

	FString AsString;
	if (Parent->TryGetStringField(Field, AsString) && !AsString.IsEmpty())
	{
		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(AsString);
		if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
		{
			OutObj = Parsed;
			return true;
		}

	}

	return false;
}

bool UAL_CommandUtils::ResolvePreset(const FString& Name, FUALSpawnPreset& OutPreset)
{
	static const TArray<FUALSpawnPreset> Presets = {
		{ TEXT("cube"), AStaticMeshActor::StaticClass(), TEXT("/Engine/BasicShapes/Cube.Cube") },
		{ TEXT("sphere"), AStaticMeshActor::StaticClass(), TEXT("/Engine/BasicShapes/Sphere.Sphere") },
		{ TEXT("cylinder"), AStaticMeshActor::StaticClass(), TEXT("/Engine/BasicShapes/Cylinder.Cylinder") },
		{ TEXT("cone"), AStaticMeshActor::StaticClass(), TEXT("/Engine/BasicShapes/Cone.Cone") },
		{ TEXT("plane"), AStaticMeshActor::StaticClass(), TEXT("/Engine/BasicShapes/Plane.Plane") },
		{ TEXT("point_light"), APointLight::StaticClass(), nullptr },
		{ TEXT("spot_light"), ASpotLight::StaticClass(), nullptr },
		{ TEXT("directional_light"), ADirectionalLight::StaticClass(), nullptr },
		{ TEXT("rect_light"), ARectLight::StaticClass(), nullptr },
		{ TEXT("camera"), ACameraActor::StaticClass(), nullptr },
	};

	for (const FUALSpawnPreset& Preset : Presets)
	{
		if (Preset.Key == FName(*Name))
		{
			OutPreset = Preset;
			return true;
		}
	}
	return false;
}

bool UAL_CommandUtils::SetStaticMeshIfNeeded(AActor* Actor, const TCHAR* MeshPath)
{
	if (!MeshPath || !Actor)
	{
		return true;
	}

	AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(Actor);
	if (!MeshActor)
	{
		return true;
	}

	FSoftObjectPath MeshAssetPath(MeshPath);
	if (!MeshAssetPath.IsValid())
	{
		return false;
	}

	UObject* LoadedObj = MeshAssetPath.TryLoad();
	UStaticMesh* StaticMesh = Cast<UStaticMesh>(LoadedObj);
	if (!StaticMesh)
	{
		return false;
	}

	MeshActor->GetStaticMeshComponent()->SetStaticMesh(StaticMesh);
	return true;
}

bool UAL_CommandUtils::ResolveSpawnFromAssetId(const FString& AssetId, FUALResolvedSpawnRequest& OutResolved, FString& OutError)
{
	OutResolved = FUALResolvedSpawnRequest();
	OutError.Reset();

	if (AssetId.IsEmpty())
	{
		OutError = TEXT("asset_id is empty");
		return false;
	}

	// Level 1：Preset
	FUALSpawnPreset Preset;
	if (ResolvePreset(AssetId, Preset))
	{
		OutResolved.SpawnClass = Preset.Class;
		if (Preset.AssetPath)
		{
			OutResolved.MeshPath = Preset.AssetPath;
		}
		OutResolved.ResolvedType = Preset.Class ? Preset.Class->GetName() : TEXT("Preset");
		OutResolved.SourceId = AssetId;
		OutResolved.bFromAlias = true;
		return true;
	}

	// Level 2/3：Asset Path
	if (AssetId.StartsWith(TEXT("/")))
	{
		FSoftObjectPath SoftObjPath(AssetId);
		FSoftClassPath SoftClassPath(AssetId);

		UObject* LoadedObj = SoftObjPath.IsValid() ? SoftObjPath.TryLoad() : nullptr;

		if (UClass* LoadedClass = SoftClassPath.IsValid() ? SoftClassPath.TryLoadClass<AActor>() : nullptr)
		{
			OutResolved.SpawnClass = LoadedClass;
			OutResolved.ResolvedType = LoadedClass->GetName();
			OutResolved.SourceId = AssetId;
			return true;
		}

		if (LoadedObj)
		{
			// Blueprint
			if (UBlueprint* BP = Cast<UBlueprint>(LoadedObj))
			{
				if (BP->GeneratedClass && BP->GeneratedClass->IsChildOf(AActor::StaticClass()))
				{
					OutResolved.SpawnClass = BP->GeneratedClass;
					OutResolved.ResolvedType = BP->GeneratedClass->GetName();
					OutResolved.SourceId = AssetId;
					return true;
				}
			}

			// Class
			if (UClass* AsClass = Cast<UClass>(LoadedObj))
			{
				if (AsClass->IsChildOf(AActor::StaticClass()))
				{
					OutResolved.SpawnClass = AsClass;
					OutResolved.ResolvedType = AsClass->GetName();
					OutResolved.SourceId = AssetId;
					return true;
				}
			}

			// Static Mesh
			if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(LoadedObj))
			{
				OutResolved.SpawnClass = AStaticMeshActor::StaticClass();
				OutResolved.MeshPath = AssetId;
				OutResolved.ResolvedType = TEXT("StaticMeshActor");
				OutResolved.SourceId = AssetId;
				return true;
			}
		}

		OutError = FString::Printf(TEXT("Unsupported asset type or failed to load: %s"), *AssetId);
		return false;
	}

	// Fallback
	FString ClassError;
	if (UClass* ResolvedClass = ResolveClassFromIdentifier(AssetId, AActor::StaticClass(), ClassError))
	{
		OutResolved.SpawnClass = ResolvedClass;
		OutResolved.ResolvedType = ResolvedClass->GetName();
		OutResolved.SourceId = AssetId;
		return true;
	}

	OutError = ClassError.IsEmpty() ? FString::Printf(TEXT("Failed to resolve asset_id: %s"), *AssetId) : ClassError;
	return false;
}

void UAL_CommandUtils::ReadTransformFromItem(const TSharedPtr<FJsonObject>& Item, FVector& OutLocation, FRotator& OutRotation, FVector& OutScale)
{
	// Legacy
	OutLocation = ReadVector(Item, TEXT("location"), OutLocation);
	OutRotation = ReadRotator(Item, TEXT("rotation"), OutRotation);
	OutScale = ReadVector(Item, TEXT("scale"), OutScale);

	// New: transform { location/rotation/scale }
	const TSharedPtr<FJsonObject>* TransformObj = nullptr;
	if (Item->TryGetObjectField(TEXT("transform"), TransformObj) && TransformObj && TransformObj->IsValid())
	{
		TSharedPtr<FJsonObject> LocObj, RotObj, ScaleObj;
		if (TryGetObjectFieldFlexible(*TransformObj, TEXT("location"), LocObj))
		{
			OutLocation = ReadVectorDirect(LocObj, OutLocation);
		}
		if (TryGetObjectFieldFlexible(*TransformObj, TEXT("rotation"), RotObj))
		{
			OutRotation = ReadRotatorDirect(RotObj, OutRotation);
		}
		if (TryGetObjectFieldFlexible(*TransformObj, TEXT("scale"), ScaleObj))
		{
			OutScale = ReadVectorDirect(ScaleObj, OutScale);
		}
	}
}

namespace
{
	/**
	 * 这个类是不是一份**过期的同名残留**。
	 *
	 * 资产被重载（`ReloadPackage`）或热重载之后，旧包会被改名成
	 * `<原名>_DEADPACKAGE_N` 留在内存里 —— 如果还有别的东西引用着它，它就一直不死。
	 * 引擎在 `MarkPackageReplaced`（CoreUObject `PackageReload.cpp`）里给这个包
	 * **和包里所有对象**打上 `RF_NewerVersionExists`，这就是判据；
	 * 重新实例化留下的 `REINST_` 类走的是类标志 `CLASS_NewerVersionExists`。
	 *
	 * 为什么必须查：按名字找类的 `FindFirstObject` 不区分死活，同名的死类完全
	 * 可能排在活的前面。命中之后一切照常 —— 节点建出来了、回读也正常，
	 * 只有编译报一句「此蓝图（自身）并非是一个 X_C」，错误信息里没有半点
	 * 「你连到的是另一个同名类」的线索。2026-09-16 的用户反馈里，这条把人
	 * 引向了 Cast / self 引脚的错误方向，最后靠重启编辑器才清掉。
	 */
	bool UAL_IsStaleClass(const UClass* Cls)
	{
		return Cls
			&& (Cls->HasAnyFlags(RF_NewerVersionExists) || Cls->HasAnyClassFlags(CLASS_NewerVersionExists));
	}

	/**
	 * 同名的活类还在不在。只有在第一次命中的是死类时才走这条慢路。
	 *
	 * `RequiredPackage` 非空时**必须**同包才算数。调用方给的是完整资产路径时
	 * 一定要传它：叶子名在 UE 工程里重名太常见（/Game/Doors/BP_Door 和
	 * /Game/Props/BP_Door），只按名字换会把「点名要 A」悄悄换成 B ——
	 * 那正是这段代码本来要防的「连错同名类」，只是换了个方向。
	 */
	UClass* UAL_FindLiveClassByName(const FName& ClassName, const FName& RequiredPackage = NAME_None)
	{
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Candidate = *It;
			if (!Candidate || Candidate->GetFName() != ClassName || UAL_IsStaleClass(Candidate))
			{
				continue;
			}
			if (!RequiredPackage.IsNone() && Candidate->GetOutermost()->GetFName() != RequiredPackage)
			{
				continue;
			}
			return Candidate;
		}
		return nullptr;
	}
}

UClass* UAL_CommandUtils::ResolveClassFromIdentifier(const FString& Identifier, UClass* ExpectedBase, FString& OutError)
{
	if (Identifier.IsEmpty())
	{
		OutError = TEXT("Class identifier is empty");
		return nullptr;
	}

	UClass* ResolvedClass = nullptr;

	if (Identifier.StartsWith(TEXT("/")))
	{
		const FSoftClassPath SoftPath(Identifier);
		ResolvedClass = SoftPath.TryLoadClass<UObject>();
	}
	else
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
		ResolvedClass = FindFirstObject<UClass>(*Identifier, EFindFirstObjectOptions::NativeFirst);
		if (!ResolvedClass)
		{
			const FString WithUPrefix = FString::Printf(TEXT("U%s"), *Identifier);
			ResolvedClass = FindFirstObject<UClass>(*WithUPrefix, EFindFirstObjectOptions::NativeFirst);
		}
#else
		ResolvedClass = FindObject<UClass>(ANY_PACKAGE, *Identifier);
		if (!ResolvedClass)
		{
			const FString WithUPrefix = FString::Printf(TEXT("U%s"), *Identifier);
			ResolvedClass = FindObject<UClass>(ANY_PACKAGE, *WithUPrefix);
		}
#endif
	}

	// 按名字找到的可能是死包里的同名类。换一个活的；一个活的都没有就如实报，
	// 别把死类交出去 —— 拿它建出来的节点编译必挂，而报错完全不提这回事
	if (UAL_IsStaleClass(ResolvedClass))
	{
		const FName StaleName = ResolvedClass->GetFName();
		const FString StalePath = ResolvedClass->GetPathName();
		// 调用方点名了具体的包，就只在那个包里换 —— 跨包换等于悄悄给了它另一个资产。
		// 取的是**请求里**那个包名，不是死类现在所在的包：后者已经被引擎改成
		// `<原名>_DEADPACKAGE_N` 了，拿它去比永远比不中重新载入的那份
		const FName RequiredPackage = Identifier.StartsWith(TEXT("/"))
			? FName(*FSoftClassPath(Identifier).GetLongPackageName())
			: NAME_None;
		ResolvedClass = UAL_FindLiveClassByName(StaleName, RequiredPackage);
		if (!ResolvedClass)
		{
			OutError = FString::Printf(
				TEXT("Class '%s' only exists as a stale copy left behind by a package reload (%s). ")
				TEXT("Nothing can be built against it - restart the editor to clear it, then retry."),
				*Identifier, *StalePath);
			return nullptr;
		}
	}

	if (!ResolvedClass)
	{
		OutError = FString::Printf(TEXT("Class not found: %s"), *Identifier);
		return nullptr;
	}

	if (ExpectedBase && !ResolvedClass->IsChildOf(ExpectedBase))
	{
		OutError = FString::Printf(TEXT("%s is not a subclass of %s"), *Identifier, *ExpectedBase->GetName());
		return nullptr;
	}

	return ResolvedClass;
}

void UAL_CommandUtils::AddUnmatchedTargets(const TSharedPtr<FJsonObject>& Data, const TArray<FString>& Unmatched)
{
	if (!Data.IsValid() || Unmatched.Num() == 0)
	{
		return;
	}

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FString& Name : Unmatched)
	{
		Items.Add(MakeShared<FJsonValueString>(Name));
	}
	Data->SetArrayField(TEXT("unmatched_targets"), Items);
	Data->SetNumberField(TEXT("unmatched_count"), Unmatched.Num());
}

bool UAL_CommandUtils::ResolveTargetsToActors(const TSharedPtr<FJsonObject>& Targets, UWorld* World, TSet<AActor*>& OutSet, FString& OutError, TArray<FString>* OutUnmatched)
{
	OutSet.Reset();
	OutError.Reset();
	if (OutUnmatched)
	{
		OutUnmatched->Reset();
	}

	if (!Targets.IsValid())
	{
		OutError = TEXT("Missing object: targets");
		return false;
	}

	bool bHasExplicitTargets = false;

	// selection: 当前编辑器选中的Actor
#if WITH_EDITOR
	bool bUseSelection = false;
	if (Targets->TryGetBoolField(TEXT("selection"), bUseSelection) && bUseSelection)
	{
		bHasExplicitTargets = true;
		if (GEditor)
		{
			for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
			{
				if (AActor* Selected = Cast<AActor>(*It))
				{
					OutSet.Add(Selected);
				}
			}
		}
	}
#endif

	// names
	const TArray<TSharedPtr<FJsonValue>>* NamesArr = nullptr;
	if (Targets->TryGetArrayField(TEXT("names"), NamesArr) && NamesArr && NamesArr->Num() > 0)
	{
		bHasExplicitTargets = true;
		for (const TSharedPtr<FJsonValue>& Val : *NamesArr)
		{
			FString Name;
			if (Val.IsValid() && Val->TryGetString(Name))
			{
				if (AActor* Actor = FindActorByLabel(World, Name))
				{
					OutSet.Add(Actor);
				}
				else if (OutUnmatched)
				{
					// 没对上的名字要留痕。丢掉的话，调用方分不清「这个名字写错了」
					// 和「这个 Actor 已经不在了」，也不知道少办了一个
					OutUnmatched->AddUnique(Name);
				}
			}
		}
	}

	// paths
	const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
	if (Targets->TryGetArrayField(TEXT("paths"), PathsArr) && PathsArr && PathsArr->Num() > 0)
	{
		bHasExplicitTargets = true;
		for (const TSharedPtr<FJsonValue>& Val : *PathsArr)
		{
			FString Path;
			if (Val.IsValid() && Val->TryGetString(Path))
			{
				if (AActor* Actor = Cast<AActor>(StaticFindObject(AActor::StaticClass(), nullptr, *Path)))
				{
					OutSet.Add(Actor);
				}
				else if (OutUnmatched)
				{
					OutUnmatched->AddUnique(Path);
				}
			}
		}
	}

	// filter
	const TSharedPtr<FJsonObject>* FilterObj = nullptr;
	Targets->TryGetObjectField(TEXT("filter"), FilterObj);
	FString FilterClassContains, FilterNamePattern;
	TArray<FString> FilterExcludeClasses;
	// property_match: 属性匹配规则数组 [{ name: "StaticMesh", value: "Cube" }, ...]
	TArray<TPair<FString, FString>> PropertyMatchRules;
	bool bHasFilter = false;
	if (FilterObj && (*FilterObj).IsValid())
	{
		(*FilterObj)->TryGetStringField(TEXT("class"), FilterClassContains);
		(*FilterObj)->TryGetStringField(TEXT("name_pattern"), FilterNamePattern);
		const TArray<TSharedPtr<FJsonValue>>* Excl = nullptr;
		if ((*FilterObj)->TryGetArrayField(TEXT("exclude_classes"), Excl) && Excl)
		{
			for (const TSharedPtr<FJsonValue>& V : *Excl)
			{
				FString S;
				if (V.IsValid() && V->TryGetString(S))
				{
					FilterExcludeClasses.Add(S);
				}
			}
		}
		// 解析 property_match 数组
		const TArray<TSharedPtr<FJsonValue>>* PropMatchArr = nullptr;
		if ((*FilterObj)->TryGetArrayField(TEXT("property_match"), PropMatchArr) && PropMatchArr)
		{
			for (const TSharedPtr<FJsonValue>& Item : *PropMatchArr)
			{
				if (!Item.IsValid() || Item->Type != EJson::Object) continue;
				const TSharedPtr<FJsonObject> Rule = Item->AsObject();
				FString Name, Value;
				if (Rule->TryGetStringField(TEXT("name"), Name) && Rule->TryGetStringField(TEXT("value"), Value))
				{
					PropertyMatchRules.Add(TPair<FString, FString>(Name, Value));
				}
			}
		}
		// 只要 filter 对象存在（即使是空对象 {}），就视为有效的过滤器
		// 空的 filter 将匹配所有 Actor（MatchFilter lambda 会返回 true）
		bHasFilter = true;
	}

	auto MatchFilter = [&](AActor* Actor) -> bool
	{
		if (!Actor) return false;
		if (!FilterClassContains.IsEmpty())
		{
			const FString Cls = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();
			if (!Cls.Contains(FilterClassContains, ESearchCase::IgnoreCase, ESearchDir::FromStart))
			{
				return false;
			}
		}
		if (!FilterNamePattern.IsEmpty())
		{
			const FString Nm = GetActorFriendlyName(Actor);
			if (!Nm.MatchesWildcard(FilterNamePattern))
			{
				return false;
			}
		}
		for (const FString& Ex : FilterExcludeClasses)
		{
			const FString Cls = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();
			if (Cls.Equals(Ex, ESearchCase::IgnoreCase))
			{
				return false;
			}
		}
		// property_match 检查：所有规则必须全部匹配（AND 逻辑）
		for (const TPair<FString, FString>& Rule : PropertyMatchRules)
		{
			if (!CheckPropertyMatch(Actor, Rule.Key, Rule.Value))
			{
				return false;
			}
		}
		return true;
	};

	if (OutSet.Num() > 0)
	{
		if (bHasFilter)
		{
			for (auto It = OutSet.CreateIterator(); It; ++It)
			{
				if (!MatchFilter(*It))
				{
					It.RemoveCurrent();
				}
			}
		}
	}
	else if (bHasExplicitTargets)
	{
		OutError = TEXT("No actor found matching the specified names/paths");
		return false;
	}
	else if (bHasFilter)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor && MatchFilter(Actor))
			{
				OutSet.Add(Actor);
			}
		}
	}
	else
	{
		OutError = TEXT("No valid selector provided: must specify selection, names, paths, or filter");
		return false;
	}

	if (OutSet.Num() == 0)
	{
		OutError = TEXT("No actor matched targets");
		return false;
	}

	return true;
}

namespace
{
	/** 匹配方式：类名全等，还是类名前缀 */
	enum class EUALSystemActorMatch : uint8
	{
		Exact,
		Prefix
	};

	struct FUALSystemActorRule
	{
		/** UClass::GetName() 的结果，即不带 A/U 前缀的类名 */
		const TCHAR* ClassName;
		EUALSystemActorMatch Match;
	};

	/**
	 * 默认不列出的系统 Actor 表。
	 *
	 * 收进这张表的判据只有一条：**它是引擎自己生成的记账对象，用户没放过它，
	 * 也不会想改它**。世界里可能有成百上千个这种东西，一次场景扫描全回来，
	 * 用户真正要找的那个就埋在里面了 —— 真机上 `filter: { class: "PCGVolume" }`
	 * 回过 144 个 WorldPartitionHLOD，目标 PCG 体积夹在几十行 HLOD 中间。
	 *
	 * 反过来，凡是用户会在关卡里亲手摆、亲手调的，都不该进这张表，哪怕它看着
	 * 也很「系统」—— 比如 NavMeshBoundsVolume（划导航范围的是人）、PostProcessVolume、
	 * WorldSettings（关卡设置要读要改）。
	 *
	 * 需要看它们时传 `include_system_actors: true`，一条都不会少。
	 */
	static const FUALSystemActorRule SystemActorRules[] = {
		// —— HLOD：World Partition 烘出来的合并代理网格 ——
		// 一个大世界动辄上百个，纯粹是渲染优化的产物，改它没有任何意义
		// （改了下次 Build HLOD 就冲掉）。这是压垮上下文的头号来源。
		{ TEXT("WorldPartitionHLOD"), EUALSystemActorMatch::Exact },
		// 非 World Partition 关卡的老式 HLOD 代理（Hierarchical LOD Outliner 生成），同上
		{ TEXT("LODActor"), EUALSystemActorMatch::Exact },

		// —— 导航：NavMeshBoundsVolume 的生成产物 ——
		// 用户放的是 NavMeshBoundsVolume（**不在**这张表里），这些是引擎据此烘出来的
		// 导航数据载体。RecastNavMesh 及其变体都继承自 ANavigationData。
		{ TEXT("RecastNavMesh"), EUALSystemActorMatch::Prefix },
		// 没有真实导航数据时引擎塞的占位物，纯内部对象
		{ TEXT("AbstractNavData"), EUALSystemActorMatch::Exact },
		// 导航数据的分块存储 Actor（World Partition 下按 Cell 切开的导航数据）
		{ TEXT("NavigationDataChunkActor"), EUALSystemActorMatch::Exact },

		// —— 物理 ——
		// 每个世界自带一个的兜底物理体积，代表「没被任何 PhysicsVolume 覆盖的地方」。
		// 用户没放它，也删不掉。
		{ TEXT("DefaultPhysicsVolume"), EUALSystemActorMatch::Exact },

		// —— GameplayDebugger ——
		// AGameplayDebuggerCategoryReplicator / AGameplayDebuggerPlayerManager：
		// 调试 HUD 的网络复制载体，随调试器开关自动增删，不是关卡内容。
		{ TEXT("GameplayDebugger"), EUALSystemActorMatch::Prefix },

		// —— World Partition 的其他记账对象 ——
		// 小地图的烘焙目标与其取景体积，属于编辑器工具链产物
		{ TEXT("WorldPartitionMiniMap"), EUALSystemActorMatch::Prefix },
		// 数据层（Data Layer）的容器 Actor，一个世界一个，改它要走 Data Layer 面板
		{ TEXT("WorldDataLayers"), EUALSystemActorMatch::Exact },

		// —— LevelInstance 的内部代理 ——
		// ALevelInstanceEditorInstanceActor：编辑关卡实例时临时挂出来的变换代理，
		// 只在编辑器里存在。用户看到并操作的是 LevelInstance 本体（**不在**这张表里）。
		{ TEXT("LevelInstanceEditorInstanceActor"), EUALSystemActorMatch::Exact }
	};
}

bool UAL_CommandUtils::IsSystemActor(const AActor* Actor)
{
	if (!Actor || !Actor->GetClass())
	{
		return false;
	}

	const FString ClassName = Actor->GetClass()->GetName();
	for (const FUALSystemActorRule& Rule : SystemActorRules)
	{
		const bool bHit = Rule.Match == EUALSystemActorMatch::Exact
			? ClassName.Equals(Rule.ClassName, ESearchCase::IgnoreCase)
			: ClassName.StartsWith(Rule.ClassName, ESearchCase::IgnoreCase);
		if (bHit)
		{
			return true;
		}
	}
	return false;
}

int32 UAL_CommandUtils::ExcludeSystemActors(TSet<AActor*>& InOutSet, TMap<FString, int32>& OutRemovedByClass)
{
	int32 Removed = 0;
	for (auto It = InOutSet.CreateIterator(); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsSystemActor(Actor))
		{
			continue;
		}
		// 按真实类名记账，不按规则名 —— 前缀规则会命中好几个类，
		// 报「GameplayDebugger 2」不如报「GameplayDebuggerCategoryReplicator 1、
		// GameplayDebuggerPlayerManager 1」有用：后者能直接抄进 exclude_classes。
		OutRemovedByClass.FindOrAdd(Actor->GetClass()->GetName())++;
		It.RemoveCurrent();
		++Removed;
	}
	return Removed;
}

bool UAL_CommandUtils::IsScanTargets(const TSharedPtr<FJsonObject>& Targets)
{
	if (!Targets.IsValid())
	{
		return false;
	}

	bool bUseSelection = false;
	if (Targets->TryGetBoolField(TEXT("selection"), bUseSelection) && bUseSelection)
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (Targets->TryGetArrayField(TEXT("names"), Arr) && Arr && Arr->Num() > 0)
	{
		return false;
	}
	if (Targets->TryGetArrayField(TEXT("paths"), Arr) && Arr && Arr->Num() > 0)
	{
		return false;
	}

	return true;
}

void UAL_CommandUtils::AddSystemActorExclusionInfo(const TSharedPtr<FJsonObject>& Data, int32 Excluded, const TMap<FString, int32>& RemovedByClass)
{
	if (!Data.IsValid() || Excluded <= 0)
	{
		return;
	}

	Data->SetNumberField(TEXT("system_actors_excluded"), Excluded);

	TSharedPtr<FJsonObject> ByClass = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Pair : RemovedByClass)
	{
		ByClass->SetNumberField(Pair.Key, Pair.Value);
	}
	Data->SetObjectField(TEXT("system_actors_by_class"), ByClass);
}

bool UAL_CommandUtils::CheckPropertyMatch(AActor* Actor, const FString& PropName, const FString& ExpectedValue)
{
	if (!Actor || PropName.IsEmpty() || ExpectedValue.IsEmpty())
	{
		return false;
	}

	// 要搜索的对象列表：Actor 本身 -> RootComponent -> 其他常用组件
	TArray<UObject*> ObjectsToSearch;
	ObjectsToSearch.Add(Actor);
	if (Actor->GetRootComponent())
	{
		ObjectsToSearch.Add(Actor->GetRootComponent());
	}

	for (UObject* TargetObj : ObjectsToSearch)
	{
		if (!TargetObj) continue;

		FProperty* Prop = FindFProperty<FProperty>(TargetObj->GetClass(), *PropName);
		if (!Prop) continue;

		FString ActualValueStr;

		// 特殊处理：对象引用属性 (如 StaticMesh, Material)
		if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			UObject* RefObj = ObjProp->GetObjectPropertyValue_InContainer(TargetObj);
			if (RefObj)
			{
				// 获取资产名称 (如 "Cube", "SM_Rock_01")
				ActualValueStr = RefObj->GetName();
			}
		}
		// 软对象引用
		else if (FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Prop))
		{
			const FSoftObjectPtr* SoftPtr = SoftObjProp->GetPropertyValuePtr_InContainer(TargetObj);
			if (SoftPtr && !SoftPtr->IsNull())
			{
				// 从路径中提取资产名
				FString AssetPath = SoftPtr->ToString();
				int32 DotIndex;
				if (AssetPath.FindLastChar('.', DotIndex))
				{
					ActualValueStr = AssetPath.RightChop(DotIndex + 1);
				}
				else
				{
					ActualValueStr = FPaths::GetBaseFilename(AssetPath);
				}
			}
		}
		// 其他属性：转为字符串比较
		else
		{
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(TargetObj);
			if (ValuePtr)
			{
				// ExportText_InContainer 需要 6 个参数: Index, ValueStr, Container, DefaultContainer, Parent, PortFlags
				Prop->ExportText_InContainer(0, ActualValueStr, TargetObj, nullptr, TargetObj, PPF_None);
			}
		}

		// 执行模糊匹配（包含匹配，忽略大小写）
		if (!ActualValueStr.IsEmpty() && ActualValueStr.Contains(ExpectedValue, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}

	return false;
}

bool UAL_CommandUtils::ApplyStructValue(FStructProperty* StructProp, UObject* Target, const TSharedPtr<FJsonValue>& JsonValue)
{
	if (!StructProp || !Target || !JsonValue.IsValid() || JsonValue->Type != EJson::Object)
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Obj = JsonValue->AsObject();
	void* StructPtr = StructProp->ContainerPtrToValuePtr<void>(Target);
	if (!StructPtr || !Obj.IsValid())
	{
		return false;
	}

	const FName StructName = StructProp->Struct ? StructProp->Struct->GetFName() : NAME_None;
	if (StructName == TBaseStructure<FVector>::Get()->GetFName())
	{
		double X = 0, Y = 0, Z = 0;
		Obj->TryGetNumberField(TEXT("x"), X);
		Obj->TryGetNumberField(TEXT("y"), Y);
		Obj->TryGetNumberField(TEXT("z"), Z);
		*static_cast<FVector*>(StructPtr) = FVector(X, Y, Z);
		return true;
	}
	if (StructName == TBaseStructure<FRotator>::Get()->GetFName())
	{
		double Pitch = 0, Yaw = 0, Roll = 0;
		Obj->TryGetNumberField(TEXT("pitch"), Pitch);
		Obj->TryGetNumberField(TEXT("yaw"), Yaw);
		Obj->TryGetNumberField(TEXT("roll"), Roll);
		*static_cast<FRotator*>(StructPtr) = FRotator(Pitch, Yaw, Roll);
		return true;
	}

	return false;
}

const TArray<FString>& UAL_CommandUtils::GetDefaultInspectProps()
{
	static const TArray<FString> Defaults = {
		TEXT("Mobility"),
		TEXT("bHidden"),
		TEXT("CollisionProfileName"),
		TEXT("Tags")
	};
	return Defaults;
}

bool UAL_CommandUtils::TryCollectProperty(UObject* Obj, const FString& PropName, TSharedPtr<FJsonObject>& OutProps)
{
	if (!Obj || !OutProps.IsValid())
	{
		return false;
	}

	FProperty* Prop = FindFProperty<FProperty>(Obj->GetClass(), *PropName);
	if (!Prop)
	{
		return false;
	}

	if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditorOnly | CPF_DisableEditOnInstance))
	{
		return false;
	}

	const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);
	if (!ValuePtr)
	{
		return false;
	}

	TSharedPtr<FJsonValue> JsonValue = PropertyToJsonValueCompat(Prop, ValuePtr);
	if (!JsonValue.IsValid())
	{
		return false;
	}

	OutProps->SetField(PropName, JsonValue);
	return true;
}

void UAL_CommandUtils::CollectPropertyNames(UObject* Obj, TArray<FString>& OutNames)
{
	if (!Obj)
	{
		return;
	}

	for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop)
		{
			continue;
		}
		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditorOnly | CPF_DisableEditOnInstance))
		{
			continue;
		}
		if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintReadOnly))
		{
			continue;
		}
		OutNames.AddUnique(Prop->GetName());
	}
}

int32 UAL_CommandUtils::LevenshteinDistance(const FString& A, const FString& B)
{
	const int32 LenA = A.Len();
	const int32 LenB = B.Len();
	TArray<int32> Prev, Curr;
	Prev.SetNum(LenB + 1);
	Curr.SetNum(LenB + 1);

	for (int32 j = 0; j <= LenB; ++j)
	{
		Prev[j] = j;
	}

	for (int32 i = 1; i <= LenA; ++i)
	{
		Curr[0] = i;
		for (int32 j = 1; j <= LenB; ++j)
		{
			const int32 Cost = (FChar::ToLower(A[i - 1]) == FChar::ToLower(B[j - 1])) ? 0 : 1;
			Curr[j] = FMath::Min3(
				Curr[j - 1] + 1,
				Prev[j] + 1,
				Prev[j - 1] + Cost
			);
		}
		Swap(Prev, Curr);
	}
	return Prev[LenB];
}

void UAL_CommandUtils::SuggestProperties(const FString& Input, const TArray<FString>& Candidates, TArray<FString>& OutSuggestions, int32 MaxSuggestions)
{
	struct FScore
	{
		FString Name;
		int32 Distance = 0;
	};

	TArray<FScore> Scores;
	for (const FString& Cand : Candidates)
	{
		FScore S;
		S.Name = Cand;
		S.Distance = LevenshteinDistance(Input, Cand);
		Scores.Add(S);
	}

	Scores.Sort([](const FScore& L, const FScore& R)
	{
		if (L.Distance == R.Distance)
		{
			return L.Name < R.Name;
		}
		return L.Distance < R.Distance;
	});

	for (int32 i = 0; i < Scores.Num() && OutSuggestions.Num() < MaxSuggestions; ++i)
	{
		OutSuggestions.Add(Scores[i].Name);
	}
}

AActor* UAL_CommandUtils::FindActorByLabel(UWorld* World, const FString& Label)
{
	if (!World || Label.IsEmpty())
	{
		return nullptr;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
#if WITH_EDITOR
		if (Actor && Actor->GetActorLabel() == Label)
		{
			return Actor;
		}
#else
		if (Actor && Actor->GetName() == Label)
		{
			return Actor;
		}
#endif
	}
	return nullptr;
}

FString UAL_CommandUtils::GetActorFriendlyName(AActor* Actor)
{
	if (!Actor)
	{
		return FString();
	}
#if WITH_EDITOR
	return Actor->GetActorLabel();
#else
	return Actor->GetName();
#endif
}

FString UAL_CommandUtils::NormalizeAssetPath(const FString& InputPath)
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

	return Result;
}

TSharedPtr<FJsonObject> UAL_CommandUtils::MakeVectorJson(const FVector& Vec)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("x"), Vec.X);
	Obj->SetNumberField(TEXT("y"), Vec.Y);
	Obj->SetNumberField(TEXT("z"), Vec.Z);
	return Obj;
}

TSharedPtr<FJsonObject> UAL_CommandUtils::MakeRotatorJson(const FRotator& Rot)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("pitch"), Rot.Pitch);
	Obj->SetNumberField(TEXT("yaw"), Rot.Yaw);
	Obj->SetNumberField(TEXT("roll"), Rot.Roll);
	return Obj;
}

/**
 * 将 FProperty 的值转换为 JSON 格式（跨版本兼容）
 * 
 * UE 版本差异:
 * - UE5.0~5.3: 使用 FJsonObjectConverter::UPropertyToJsonValue()
 * - UE5.4+: 可使用新的 PropertyToJsonValue (带 out 参数) API
 * 
 * @param Prop 属性指针
 * @param ValuePtr 属性值指针
 * @return JSON 值，失败返回 nullptr
 */
TSharedPtr<FJsonValue> UAL_CommandUtils::PropertyToJsonValueCompat(FProperty* Prop, const void* ValuePtr)
{
	// UE5.0~5.3 统一使用 UPropertyToJsonValue (返回值版本)
	// 注意：UE5.3 中不存在 FJsonObjectConverter::PropertyToJsonValue（带 out 参数的版本）
	return FJsonObjectConverter::UPropertyToJsonValue(Prop, ValuePtr, 0, 0, nullptr, nullptr);
}

TSharedPtr<FJsonObject> UAL_CommandUtils::BuildActorInfo(AActor* Actor)
{
	if (!Actor)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("name"), GetActorFriendlyName(Actor));
	Obj->SetStringField(TEXT("path"), Actor->GetPathName());
	Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

	// World Outliner 里所在的文件夹。
	//
	// `level.organize_actors` 能把 Actor 归类，返回值里也带着 folder_path，
	// 但**查询接口一直不返回这个字段** —— 于是调用方归完类没法确认结果，
	// 也答不出「这些灯放在哪个文件夹」。空字符串表示在根目录，是正常状态。
	Obj->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());

	// 蓝图实例：把**生成它的蓝图资产路径**一起带上。
	//
	// 之前只给 class 名（`BP_Door_C`）。类名不是资产路径，调用方想接着看
	// 蓝图内容就只能去内容浏览器里按名字碰运气 —— 实测里就是这么白绕的：
	// 先猜 `/Game/BP_Door`（描述返回空对象），再 content_search，再描述一遍，
	// 三轮才走到 `/Game/PartyMVP/Props/BP_Door`。带上这个字段之后，
	// 「我选中的这个是什么」一次调用就够了。
	//
	// 用 GetOutermost()->GetName() 而不是类路径：类路径是
	// `/Game/X/BP_Door.BP_Door_C`，蓝图工具要的是包名 `/Game/X/BP_Door`。
	// 原生 C++ Actor 不是 BPGC，不会有这个字段。
	if (const UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Actor->GetClass()))
	{
		if (const UPackage* OwningPackage = GeneratedClass->GetOutermost())
		{
			Obj->SetStringField(TEXT("blueprint_path"), OwningPackage->GetName());
		}
	}

	return Obj;
}

TSharedPtr<FJsonObject> UAL_CommandUtils::BuildActorInfoWithOptions(AActor* Actor, bool bIncludeTransform, bool bIncludeBounds)
{
	TSharedPtr<FJsonObject> Obj = BuildActorInfo(Actor);
	if (!Obj.IsValid())
	{
		return nullptr;
	}

	if (bIncludeTransform)
	{
		TSharedPtr<FJsonObject> TransformObj = MakeShared<FJsonObject>();
		TransformObj->SetObjectField(TEXT("location"), MakeVectorJson(Actor->GetActorLocation()));
		TransformObj->SetObjectField(TEXT("rotation"), MakeRotatorJson(Actor->GetActorRotation()));
		TransformObj->SetObjectField(TEXT("scale"), MakeVectorJson(Actor->GetActorScale3D()));
		Obj->SetObjectField(TEXT("transform"), TransformObj);
	}

	if (bIncludeBounds)
	{
		const FBox Bounds = Actor->GetComponentsBoundingBox(true);
		const FVector Size = Bounds.IsValid ? Bounds.GetSize() : FVector::ZeroVector;
		// `bounds` 的形状不动（裸的长宽高向量）—— 已经有调用方在读它
		Obj->SetObjectField(TEXT("bounds"), MakeVectorJson(Size));

		if (Bounds.IsValid)
		{
			// 角点单独给。尺寸答不了「原点在这个实例的哪个位置」——
			// 而那是把模块化网格拼到一起的前提：同一套件里地板可能从角点向 +X/+Y 展开，
			// 墙却是居中的。以前唯一能拿到 min/max 的路是 viewport.focus，
			// 那会把用户的镜头飞走、把选中也改掉，还只回整组的并集。
			Obj->SetObjectField(TEXT("bounds_min"), MakeVectorJson(Bounds.Min));
			Obj->SetObjectField(TEXT("bounds_max"), MakeVectorJson(Bounds.Max));
			// 原点相对几何体的偏移：Actor 位置减去包围盒角点。
			// pivot_offset.z ≈ 0 表示原点坐在几何体底面，≈ -size.z/2 表示原点在正中。
			Obj->SetObjectField(TEXT("pivot_offset"), MakeVectorJson(Actor->GetActorLocation() - Bounds.Min));
		}
	}

	return Obj;
}

bool UAL_CommandUtils::ShouldIncludeActor(const AActor* Actor, const FString& NameKeyword, bool bNameExact, const FString& ClassKeyword, bool bClassExact)
{
	if (!Actor)
	{
		return false;
	}

	if (!NameKeyword.IsEmpty())
	{
		const FString Name = GetActorFriendlyName(const_cast<AActor*>(Actor));
		const bool bMatchName = bNameExact
			? Name.Equals(NameKeyword, ESearchCase::IgnoreCase)
			: Name.Contains(NameKeyword, ESearchCase::IgnoreCase, ESearchDir::FromStart);
		if (!bMatchName)
		{
			return false;
		}
	}

	if (!ClassKeyword.IsEmpty())
	{
		const FString ClassName = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();
		const bool bMatchClass = bClassExact
			? ClassName.Equals(ClassKeyword, ESearchCase::IgnoreCase)
			: ClassName.Contains(ClassKeyword, ESearchCase::IgnoreCase, ESearchDir::FromStart);
		if (!bMatchClass)
		{
			return false;
		}
	}

	return true;
}

bool UAL_CommandUtils::ShouldIncludeActorAdvanced(
	const AActor* Actor,
	const FString& NameContains,
	const FString& NameNotContains,
	const FString& ClassContains,
	const FString& ClassNotContains,
	const FString& ClassExact,
	const TArray<FString>& ExcludeClasses)
{
	if (!Actor)
	{
		return false;
	}

	const FString Name = GetActorFriendlyName(const_cast<AActor*>(Actor));
	const FString ClassName = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();

	auto ContainsIgnoreCase = [](const FString& Src, const FString& Pattern)
	{
		return Src.Contains(Pattern, ESearchCase::IgnoreCase, ESearchDir::FromStart);
	};

	if (!ClassExact.IsEmpty() && !ClassName.Equals(ClassExact, ESearchCase::IgnoreCase))
	{
		return false;
	}

	if (!ClassContains.IsEmpty() && !ContainsIgnoreCase(ClassName, ClassContains))
	{
		return false;
	}
	if (!ClassNotContains.IsEmpty() && ContainsIgnoreCase(ClassName, ClassNotContains))
	{
		return false;
	}

	for (const FString& Exclude : ExcludeClasses)
	{
		if (ClassName.Equals(Exclude, ESearchCase::IgnoreCase))
		{
			return false;
		}
	}

	if (!NameContains.IsEmpty() && !ContainsIgnoreCase(Name, NameContains))
	{
		return false;
	}
	if (!NameNotContains.IsEmpty() && ContainsIgnoreCase(Name, NameNotContains))
	{
		return false;
	}

	return true;
}

FProperty* UAL_CommandUtils::FindWritableProperty(UObject* Obj, const FString& PropName)
{
	if (!Obj)
	{
		return nullptr;
	}
	FProperty* Prop = FindFProperty<FProperty>(Obj->GetClass(), *PropName);
	if (!Prop)
	{
		return nullptr;
	}
	if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditorOnly | CPF_DisableEditOnInstance))
	{
		return nullptr;
	}
	if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintReadOnly))
	{
		return nullptr;
	}
	return Prop;
}

FProperty* UAL_CommandUtils::FindWritablePropertyOnActorHierarchy(AActor* Actor, const FString& PropName, UObject*& OutTargetObj)
{
	OutTargetObj = nullptr;
	if (!Actor)
	{
		return nullptr;
	}

	// 1) Actor Self
	if (FProperty* Prop = FindWritableProperty(Actor, PropName))
	{
		OutTargetObj = Actor;
		return Prop;
	}

	// 2) RootComponent
	if (USceneComponent* RootComp = Actor->GetRootComponent())
	{
		if (FProperty* Prop = FindWritableProperty(RootComp, PropName))
		{
			OutTargetObj = RootComp;
			return Prop;
		}
	}

	// 3) Other Components
	for (UActorComponent* Comp : Actor->GetComponents())
	{
		if (!Comp)
		{
			continue;
		}
		if (FProperty* Prop = FindWritableProperty(Comp, PropName))
		{
			OutTargetObj = Comp;
			return Prop;
		}
	}

	return nullptr;
}

bool UAL_CommandUtils::SetNumericProperty(FNumericProperty* NumProp, UObject* Obj, const TSharedPtr<FJsonValue>& Value, FString& OutError)
{
	if (!NumProp || !Obj || !Value.IsValid())
	{
		return false;
	}
	if (Value->Type != EJson::Number)
	{
		OutError = TEXT("expects a number");
		return false;
	}
	const double Num = Value->AsNumber();
	void* Ptr = NumProp->ContainerPtrToValuePtr<void>(Obj);
	if (!Ptr)
	{
		return false;
	}
	if (NumProp->IsInteger())
	{
		NumProp->SetIntPropertyValue(Ptr, static_cast<int64>(Num));
	}
	else
	{
		NumProp->SetFloatingPointPropertyValue(Ptr, Num);
	}
	return true;
}

bool UAL_CommandUtils::SetStructProperty(FStructProperty* StructProp, UObject* Obj, const TSharedPtr<FJsonValue>& Value, FString& OutError)
{
	if (!StructProp || !Obj || !Value.IsValid())
	{
		return false;
	}

	if (StructProp->Struct == TBaseStructure<FVector>::Get() ||
		StructProp->Struct == TBaseStructure<FRotator>::Get())
	{
		if (!ApplyStructValue(StructProp, Obj, Value))
		{
			OutError = TEXT("expects object with matching fields");
			return false;
		}
		return true;
	}

	if (StructProp->Struct == TBaseStructure<FLinearColor>::Get())
	{
		if (Value->Type != EJson::Object)
		{
			OutError = TEXT("expects object with r/g/b(/a)");
			return false;
		}
		const TSharedPtr<FJsonObject> ObjVal = Value->AsObject();
		double R = 0, G = 0, B = 0, A = 1.0;
		ObjVal->TryGetNumberField(TEXT("r"), R);
		ObjVal->TryGetNumberField(TEXT("g"), G);
		ObjVal->TryGetNumberField(TEXT("b"), B);
		ObjVal->TryGetNumberField(TEXT("a"), A);
		void* Ptr = StructProp->ContainerPtrToValuePtr<void>(Obj);
		if (!Ptr)
		{
			return false;
		}
		*static_cast<FLinearColor*>(Ptr) = FLinearColor(R, G, B, A);
		return true;
	}

	if (StructProp->Struct == TBaseStructure<FColor>::Get())
	{
		if (Value->Type != EJson::Object)
		{
			OutError = TEXT("expects object with r/g/b(/a)");
			return false;
		}
		const TSharedPtr<FJsonObject> ObjVal = Value->AsObject();
		
		double R = 0, G = 0, B = 0, A = 255.0;
		ObjVal->TryGetNumberField(TEXT("r"), R);
		ObjVal->TryGetNumberField(TEXT("g"), G);
		ObjVal->TryGetNumberField(TEXT("b"), B);
		const bool bHasAlpha = ObjVal->TryGetNumberField(TEXT("a"), A);
		
		// Auto-Detect Normalized Color
		const bool bIsNormalized = (R <= 1.0 && G <= 1.0 && B <= 1.0 && (!bHasAlpha || A <= 1.0));
		const bool bIsNotBlack = (R > 0.0 || G > 0.0 || B > 0.0);
		
		if (bIsNormalized && bIsNotBlack)
		{
			UE_LOG(LogUALUtils, Log, TEXT("[SmartFix] Detected 0-1 range for FColor, scaling by 255."));
			R *= 255.0;
			G *= 255.0;
			B *= 255.0;
			A = bHasAlpha ? A * 255.0 : 255.0;
		}
		else if (!bHasAlpha)
		{
			A = 255.0;
		}
		
		void* Ptr = StructProp->ContainerPtrToValuePtr<void>(Obj);
		if (!Ptr)
		{
			return false;
		}
		*static_cast<FColor*>(Ptr) = FColor(
			static_cast<uint8>(FMath::Clamp(R, 0.0, 255.0)),
			static_cast<uint8>(FMath::Clamp(G, 0.0, 255.0)),
			static_cast<uint8>(FMath::Clamp(B, 0.0, 255.0)),
			static_cast<uint8>(FMath::Clamp(A, 0.0, 255.0))
		);
		return true;
	}

	/**
	 * 其余结构体：按字段名通用导入。
	 *
	 * 上面四条手写分支之外，这里以前一律回 `unsupported struct type: X`，
	 * 而那句话既不说「能不能部分写」也不说替代路径。2026-09-21 的用户反馈里
	 * 一个任务连着撞上三种：`SingleAnimationPlayData`（骨骼网格全员 T-pose）、
	 * `PostProcessSettings`（曝光锁不上）、`IntPoint`（widget 尺寸），
	 * 三次都只能改用 Python 直写绕过去。
	 *
	 * `JsonObjectToUStruct` 是引擎自己那份按字段名的反射导入：
	 *
	 *   - **只写 JSON 里出现的字段**，其余保持原值 —— 所以
	 *     `{"bOverride_AutoExposureBias": true, "AutoExposureBias": 1.0}`
	 *     这种「只改两个字段」的写法是成立的，不需要整段给全；
	 *   - 对象引用字段收资产路径字符串（`AnimToPlay: "/Game/Anims/Idle.Idle"`）；
	 *   - 嵌套结构体递归处理。
	 *
	 * 上面四条手写分支不删：它们认的是 `{"x":..,"y":..}` / `{"r":..,"g":..}`
	 * 这种小写短名和 FColor 的 0–1 自动换算，通用导入按 UPROPERTY 的真名匹配，
	 * 认不出来。先特例后通用，两边都能用。
	 */
	if (Value->Type == EJson::Object)
	{
		void* Ptr = StructProp->ContainerPtrToValuePtr<void>(Obj);

		/**
		 * 先把键名对到 UPROPERTY 的真名上，大小写不敏感。
		 *
		 * 通用导入是按真名匹配的：`{"x":1,"y":2}` 写 `FIntPoint` 会一个字段都不中，
		 * 而它的真名是 `X` / `Y`。调用方写小写是常态（JSON 世界就是这个习惯），
		 * 为这个回一句「字段名错了」属于明知故犯。认不出来的键原样保留，
		 * 让下面的导入去报错。
		 */
		TSharedRef<FJsonObject> Normalized = MakeShared<FJsonObject>();
		{
			TMap<FString, FString> RealNames;
			for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
			{
				RealNames.Add(It->GetName().ToLower(), It->GetName());
			}
			for (const auto& Pair : Value->AsObject()->Values)
			{
				// 5.8 起 FJsonObject 的键是 UE::TSharedString 而不是 FString。两边都能
				// 解引用成 const TCHAR*，从那儿造 FString 在 5.0-5.8 上都成立
				const FString Key(*Pair.Key);
				const FString* Real = RealNames.Find(Key.ToLower());
				Normalized->SetField(Real ? *Real : Key, Pair.Value);
			}
		}

		/**
		 * 先写进一份副本，成了再整体拷回去。
		 *
		 * `JsonObjectToUStruct` 是**逐字段顺序写**的，中途失败就直接返回 false ——
		 * 前面那些字段已经落在对象上了。直接写 `Ptr` 的话，
		 * `{"bOverride_AutoExposureBias": true, "AutoExposureBias": "坏值"}`
		 * 会变成：override 标志被打开了、曝光值还是旧的，而调用方收到的是
		 * 「没设成」—— 他以为什么都没动，实际画面已经变了。
		 *
		 * 「要么全成、要么原样不动」比「一半生效还不告诉你」重要得多，
		 * 一次结构体大小的临时分配换这个，值。
		 */
		UScriptStruct* Struct = StructProp->Struct;

		// bWriteBack=false 是试导：只看能不能成，不落到对象上
		auto ImportViaScratch = [&](bool bWriteBack) -> bool
		{
			void* Scratch = FMemory::Malloc(Struct->GetStructureSize(), Struct->GetMinAlignment());
			Struct->InitializeStruct(Scratch);
			Struct->CopyScriptStruct(Scratch, Ptr);

			const bool bImported = FJsonObjectConverter::JsonObjectToUStruct(Normalized, Struct, Scratch, 0, 0);
			if (bImported && bWriteBack)
			{
				Struct->CopyScriptStruct(Ptr, Scratch);
			}

			Struct->DestroyStruct(Scratch);
			FMemory::Free(Scratch);
			return bImported;
		};

		/**
		 * `BodyInstance.CollisionProfileName` 不能按字段写。
		 *
		 * 它确实是 FBodyInstance 上的 UPROPERTY，通用导入写得进去 —— 但只改了名字，
		 * 预设里的碰撞开关、对象类型、各通道响应**都不会**载入，结果是「报成功、
		 * 碰撞没变」（头文件 TrySetDerivedProperty 注释里说的正是这个坑）。
		 * 2026-09-24 的用户反馈 `{"BodyInstance": {"CollisionProfileName": "NoCollision"}}`
		 * 在旧版本上被拒，通用导入上线后就会掉进这个坑，所以在这里截下来走 setter。
		 *
		 * 顺序：先试导其余字段（不成就原样不动地报错）→ 设预设（它会重置各通道）→
		 * 再把其余字段写上去，调用方同时给的自定义字段才不会被预设盖掉。
		 */
		UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Obj);
		// "Custom" 不是预设，是「用同一对象里给的各通道」的意思，没有模板可载，
		// setter 那条路会拒掉它 —— 照旧和 CollisionEnabled / CollisionResponses 一起按字段写
		FString ProfileStr;
		const bool bCustomProfile =
			Normalized->TryGetStringField(TEXT("CollisionProfileName"), ProfileStr) &&
			FName(*ProfileStr) == UCollisionProfile::CustomCollisionProfileName;
		if (Ptr && Struct && Primitive && Struct == FBodyInstance::StaticStruct() &&
			Normalized->HasField(TEXT("CollisionProfileName")) && !bCustomProfile)
		{
			const TSharedPtr<FJsonValue> Profile = Normalized->TryGetField(TEXT("CollisionProfileName"));
			Normalized->RemoveField(TEXT("CollisionProfileName"));
			const bool bHasRest = Normalized->Values.Num() > 0;

			if (!bHasRest || ImportViaScratch(false))
			{
				bool bHandled = false;
				if (!TrySetDerivedProperty(Obj, TEXT("CollisionProfileName"), Profile, OutError, bHandled))
				{
					return false;
				}
				if (!bHasRest || ImportViaScratch(true))
				{
					return true;
				}
			}
			// 其余字段导不进去：落到下面的统一报错
		}
		else if (Ptr && Struct && ImportViaScratch(true))
		{
			return true;
		}

		// 失败时把这个结构体有哪些字段说出来 —— 十有八九是字段名写错了。
		// 报错不是文档，列头几个就够定位（同 UAL_ListEnumNames 的处理）
		TArray<FString> FieldNames;
		for (TFieldIterator<FProperty> It(StructProp->Struct); It && FieldNames.Num() < 12; ++It)
		{
			FieldNames.Add(It->GetName());
		}
		OutError = FString::Printf(
			TEXT("could not set struct %s from the given object; its fields are: %s%s"),
			*StructProp->Struct->GetName(),
			FieldNames.Num() > 0 ? *FString::Join(FieldNames, TEXT(", ")) : TEXT("(none)"),
			FieldNames.Num() >= 12 ? TEXT(", ...") : TEXT(""));
		return false;
	}

	OutError = FString::Printf(
		TEXT("struct %s expects an object with its field names, got a non-object value"),
		*StructProp->Struct->GetName());
	return false;
}

namespace
{
	/** 这个枚举的全部取值，给错误信息用。太长就截断 —— 报错不是文档 */
	FString UAL_ListEnumNames(const UEnum* Enum)
	{
		TArray<FString> Names;
		for (int32 i = 0; i < Enum->NumEnums() - 1; ++i)
		{
			// 显示名而不是带命名空间的全名：调用方照着填回来的就是这个
			Names.Add(Enum->GetNameStringByIndex(i));
			if (Names.Num() >= 12)
			{
				Names.Add(TEXT("..."));
				break;
			}
		}
		return FString::Join(Names, TEXT(", "));
	}

	/**
	 * JSON 值 → 枚举的整数值。名字和数字都认。
	 *
	 * 抽出来是因为枚举在反射里有**两种**形态：`FEnumProperty`，以及
	 * `TEnumAsByte<...>` 编出来的、带 `Enum` 的 `FByteProperty`。后者同时也是
	 * `FNumericProperty`，以前直接落进数字分支，于是
	 * `{"Mobility": "Movable"}` 被顶回一句 `expects a number` ——
	 * 而该填几没有任何地方说，调用方只能猜（2026-09-16 的用户反馈里猜的是 2，
	 * 猜对了，但那是运气）。
	 */
	bool UAL_ResolveEnumValue(const UEnum* Enum, const TSharedPtr<FJsonValue>& Value, int64& OutValue, FString& OutError)
	{
		if (Value->Type == EJson::Number)
		{
			OutValue = static_cast<int64>(Value->AsNumber());
			return true;
		}
		if (Value->Type != EJson::String)
		{
			OutError = FString::Printf(
				TEXT("expects an enum name or number (valid: %s)"), *UAL_ListEnumNames(Enum));
			return false;
		}

		const FString EnumName = Value->AsString();
		int64 Resolved = Enum->GetValueByNameString(EnumName);
		if (Resolved == INDEX_NONE)
		{
			// 命名空间枚举（EComponentMobility::Movable）填短名时走这条
			Resolved = Enum->GetValueByNameString(FString::Printf(TEXT("%s::%s"), *Enum->GetName(), *EnumName));
		}
		// 空串不进模糊匹配：`Contains("")` 恒真，会把空值静默认成第一个枚举项
		// （Mobility 变成 Static，还报成功）；引擎的 FindFirst 里还有
		// `check(!Search.IsEmpty())`，Development 构建下可能直接断言
		if (Resolved == INDEX_NONE && !EnumName.IsEmpty())
		{
			for (int32 i = 0; i < Enum->NumEnums() - 1; ++i)
			{
				const FString FullName = Enum->GetNameStringByIndex(i);
				if (FullName.Contains(EnumName, ESearchCase::IgnoreCase))
				{
					Resolved = Enum->GetValueByIndex(i);
					UE_LOG(LogUALUtils, Log, TEXT("[SmartFix] Fuzzy matched enum '%s' to '%s'"), *EnumName, *FullName);
					break;
				}
			}
		}
		if (Resolved == INDEX_NONE)
		{
			// 取值一起回 —— 只说「无效」等于让调用方再猜一轮
			OutError = FString::Printf(
				TEXT("Invalid enum value '%s' for %s (valid: %s)"),
				*EnumName, *Enum->GetName(), *UAL_ListEnumNames(Enum));
			return false;
		}

		OutValue = Resolved;
		return true;
	}
}

bool UAL_CommandUtils::TrySetDerivedProperty(
	UObject* Target,
	const FString& PropName,
	const TSharedPtr<FJsonValue>& Value,
	FString& OutError,
	bool& bOutHandled)
{
	bOutHandled = false;
	if (!Target || !Value.IsValid())
	{
		return false;
	}

	if (!PropName.Equals(TEXT("CollisionProfileName"), ESearchCase::IgnoreCase))
	{
		return false;
	}

	bOutHandled = true;

	UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Target);
	if (!Primitive)
	{
		// 名字归这里管，但这个对象上没有碰撞体 —— 说清楚是「放错地方了」，
		// 而不是含糊的「没有这个属性」
		OutError = TEXT("CollisionProfileName only exists on components that have collision (a primitive component such as a Sphere, Box or Static Mesh component)");
		return false;
	}

	if (Value->Type != EJson::String)
	{
		OutError = TEXT("CollisionProfileName expects a preset name string, e.g. \"Trigger\", \"BlockAll\", \"OverlapAllDynamic\", \"NoCollision\"");
		return false;
	}

	const FString ProfileName = Value->AsString();

	// 预设名写错的话引擎只在日志里嘀咕一句，组件留在原来的预设上 ——
	// 那就是一次「报了成功、行为没变」。先查一遍再设。
	FCollisionResponseTemplate Unused;
	if (!UCollisionProfile::Get()->GetProfileTemplate(FName(*ProfileName), Unused))
	{
		TArray<TSharedPtr<FName>> All;
		UCollisionProfile::GetProfileNames(All);
		TArray<FString> Names;
		for (const TSharedPtr<FName>& Entry : All)
		{
			if (Entry.IsValid())
			{
				Names.Add(Entry->ToString());
			}
		}
		OutError = FString::Printf(
			TEXT("No collision profile named '%s' in this project (available: %s)"),
			*ProfileName, *FString::Join(Names, TEXT(", ")));
		return false;
	}

	Primitive->Modify();
	// 一定要走这个 setter：它会把预设里的各通道响应一起载入。直接写
	// BodyInstance.CollisionProfileName 只改名字，碰撞行为不跟着变
	Primitive->SetCollisionProfileName(FName(*ProfileName));
	return true;
}

bool UAL_CommandUtils::SetSimpleProperty(FProperty* Prop, UObject* Obj, const TSharedPtr<FJsonValue>& Value, FString& OutError)
{
	if (!Prop || !Obj || !Value.IsValid())
	{
		return false;
	}

	// FEnumProperty
	if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		if (const UEnum* Enum = EnumProp->GetEnum())
		{
			int64 EnumValue = 0;
			if (!UAL_ResolveEnumValue(Enum, Value, EnumValue, OutError))
			{
				return false;
			}
			FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
			void* Ptr = EnumProp->ContainerPtrToValuePtr<void>(Obj);
			if (!Ptr || !UnderlyingProp)
			{
				return false;
			}
			UnderlyingProp->SetIntPropertyValue(Ptr, EnumValue);
			return true;
		}
	}

	/**
	 * `TEnumAsByte<E>` 也是枚举，但反射里它是 `FByteProperty`（而 FByteProperty
	 * 继承 FNumericProperty）。这一支必须排在数字分支**前面**，否则
	 * Mobility / 各种碰撞枚举这类最常设的属性只能填数字。
	 */
	if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
	{
		if (const UEnum* Enum = ByteProp->Enum)
		{
			int64 EnumValue = 0;
			if (!UAL_ResolveEnumValue(Enum, Value, EnumValue, OutError))
			{
				return false;
			}
			void* Ptr = ByteProp->ContainerPtrToValuePtr<void>(Obj);
			if (!Ptr)
			{
				return false;
			}
			ByteProp->SetIntPropertyValue(Ptr, EnumValue);
			return true;
		}
	}

	if (FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
	{
		return SetNumericProperty(NumProp, Obj, Value, OutError);
	}

	if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
	{
		bool bVal = false;
		if (Value->Type == EJson::Boolean)
		{
			bVal = Value->AsBool();
		}
		else if (Value->Type == EJson::String)
		{
			const FString StrVal = Value->AsString();
			bVal = StrVal.Equals(TEXT("true"), ESearchCase::IgnoreCase) ||
				   StrVal.Equals(TEXT("1"), ESearchCase::IgnoreCase) ||
				   StrVal.Equals(TEXT("yes"), ESearchCase::IgnoreCase);
			UE_LOG(LogUALUtils, Log, TEXT("[SmartFix] Converted string '%s' to bool: %s"), *StrVal, bVal ? TEXT("true") : TEXT("false"));
		}
		else if (Value->Type == EJson::Number)
		{
			bVal = Value->AsNumber() > 0;
			UE_LOG(LogUALUtils, Log, TEXT("[SmartFix] Converted number to bool: %s"), bVal ? TEXT("true") : TEXT("false"));
		}
		else
		{
			OutError = TEXT("expects a boolean (or string/number that can be converted)");
			return false;
		}
		
		void* Ptr = BoolProp->ContainerPtrToValuePtr<void>(Obj);
		if (!Ptr)
		{
			return false;
		}
		BoolProp->SetPropertyValue(Ptr, bVal);
		return true;
	}

	if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
	{
		if (Value->Type != EJson::String)
		{
			OutError = TEXT("expects a string");
			return false;
		}
		void* Ptr = StrProp->ContainerPtrToValuePtr<void>(Obj);
		if (!Ptr)
		{
			return false;
		}
		StrProp->SetPropertyValue(Ptr, Value->AsString());
		return true;
	}

	if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
	{
		if (Value->Type != EJson::String)
		{
			OutError = TEXT("expects a string");
			return false;
		}
		void* Ptr = NameProp->ContainerPtrToValuePtr<void>(Obj);
		if (!Ptr)
		{
			return false;
		}
		NameProp->SetPropertyValue(Ptr, FName(*Value->AsString()));
		return true;
	}

	if (FTextProperty* TextProp = CastField<FTextProperty>(Prop))
	{
		if (Value->Type != EJson::String)
		{
			OutError = TEXT("expects a string");
			return false;
		}
		void* Ptr = TextProp->ContainerPtrToValuePtr<void>(Obj);
		if (!Ptr)
		{
			return false;
		}
		TextProp->SetPropertyValue(Ptr, FText::FromString(Value->AsString()));
		return true;
	}

	if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		return SetStructProperty(StructProp, Obj, Value, OutError);
	}

	// === FObjectProperty 支持（硬引用，如 StaticMesh, Material 等） ===
	if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
	{
		// 从 JSON 获取资产路径字符串
		FString AssetPath;
		if (Value->Type == EJson::String)
		{
			AssetPath = Value->AsString();
		}
		else if (Value->Type == EJson::Object)
		{
			// 支持对象格式: { "path": "/Game/..." } 或 { "asset_path": "..." }
			const TSharedPtr<FJsonObject> ObjVal = Value->AsObject();
			if (!ObjVal->TryGetStringField(TEXT("path"), AssetPath))
			{
				ObjVal->TryGetStringField(TEXT("asset_path"), AssetPath);
			}
		}
		else if (Value->Type == EJson::Null)
		{
			// 允许设置为 null（清空引用）
			void* Ptr = ObjProp->ContainerPtrToValuePtr<void>(Obj);
			if (Ptr)
			{
				ObjProp->SetPropertyValue(Ptr, nullptr);
				UE_LOG(LogUALUtils, Log, TEXT("[SetSimpleProperty] Cleared object reference for '%s'"), *Prop->GetName());
				return true;
			}
			return false;
		}
		else
		{
			OutError = TEXT("expects a string (asset path) or object with 'path' field, or null");
			return false;
		}

		if (AssetPath.IsEmpty())
		{
			OutError = TEXT("empty asset path provided");
			return false;
		}

		// 尝试加载资产
		UObject* LoadedAsset = nullptr;
		
		// 尝试不同的路径格式
		TArray<FString> PathsToTry;
		PathsToTry.Add(AssetPath);  // 原始路径
		
		// 如果路径不包含资产名后缀，尝试添加
		if (!AssetPath.Contains(TEXT(".")))
		{
			FString BaseName = FPaths::GetBaseFilename(AssetPath);
			PathsToTry.Add(AssetPath + TEXT(".") + BaseName);
		}
		
		// 如果不是完整路径，尝试常见前缀
		if (!AssetPath.StartsWith(TEXT("/")))
		{
			PathsToTry.Add(TEXT("/Game/") + AssetPath);
			PathsToTry.Add(TEXT("/Engine/") + AssetPath);
		}

		for (const FString& PathToTry : PathsToTry)
		{
			LoadedAsset = LoadObject<UObject>(nullptr, *PathToTry);
			if (LoadedAsset)
			{
				UE_LOG(LogUALUtils, Log, TEXT("[SetSimpleProperty] Loaded asset from path: %s"), *PathToTry);
				break;
			}
		}

		if (!LoadedAsset)
		{
			// 尝试通过 AssetRegistry 查找（模糊匹配）
			IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
			
			TArray<FAssetData> AssetList;
			FString SearchName = FPaths::GetBaseFilename(AssetPath);
			
			// 尝试获取期望的资产类
			UClass* ExpectedClass = ObjProp->PropertyClass;
			FARFilter Filter;
			if (ExpectedClass)
			{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
				Filter.ClassPaths.Add(ExpectedClass->GetClassPathName());
#else
				Filter.ClassNames.Add(ExpectedClass->GetFName());
#endif
			}
			Filter.bRecursiveClasses = true;
			AssetRegistry.GetAssets(Filter, AssetList);
			
			// 查找匹配的资产
			for (const FAssetData& Asset : AssetList)
			{
				FString AssetName = Asset.AssetName.ToString();
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
				FString FoundPath = Asset.GetObjectPathString();
#else
				FString FoundPath = Asset.ObjectPath.ToString();
#endif
				// 精确匹配或包含匹配
				if (AssetName.Equals(SearchName, ESearchCase::IgnoreCase) ||
					FoundPath.Contains(AssetPath, ESearchCase::IgnoreCase))
				{
					LoadedAsset = Asset.GetAsset();
					if (LoadedAsset)
					{
						UE_LOG(LogUALUtils, Log, TEXT("[SetSimpleProperty] Found asset via registry: %s"), *FoundPath);
						break;
					}
				}
			}
		}

		if (!LoadedAsset)
		{
			OutError = FString::Printf(TEXT("Failed to load asset: %s (expected type: %s)"), 
				*AssetPath, ObjProp->PropertyClass ? *ObjProp->PropertyClass->GetName() : TEXT("Unknown"));
			return false;
		}

		// 验证类型兼容性
		if (ObjProp->PropertyClass && !LoadedAsset->IsA(ObjProp->PropertyClass))
		{
			OutError = FString::Printf(TEXT("Asset type mismatch: loaded '%s' but expected '%s'"),
				*LoadedAsset->GetClass()->GetName(), *ObjProp->PropertyClass->GetName());
			return false;
		}

		// 设置属性值
		void* Ptr = ObjProp->ContainerPtrToValuePtr<void>(Obj);
		if (!Ptr)
		{
			OutError = TEXT("Failed to get property value pointer");
			return false;
		}
		ObjProp->SetPropertyValue(Ptr, LoadedAsset);
		UE_LOG(LogUALUtils, Log, TEXT("[SetSimpleProperty] Successfully set object property '%s' to '%s'"),
			*Prop->GetName(), *LoadedAsset->GetPathName());
		return true;
	}

	// === FSoftObjectProperty 支持（软引用） ===
	if (FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Prop))
	{
		FString AssetPath;
		if (Value->Type == EJson::String)
		{
			AssetPath = Value->AsString();
		}
		else if (Value->Type == EJson::Null)
		{
			void* Ptr = SoftObjProp->ContainerPtrToValuePtr<void>(Obj);
			if (Ptr)
			{
				*static_cast<FSoftObjectPtr*>(Ptr) = FSoftObjectPtr();
				return true;
			}
			return false;
		}
		else
		{
			OutError = TEXT("expects a string (asset path) or null");
			return false;
		}

		void* Ptr = SoftObjProp->ContainerPtrToValuePtr<void>(Obj);
		if (!Ptr)
		{
			return false;
		}
		*static_cast<FSoftObjectPtr*>(Ptr) = FSoftObjectPath(AssetPath);
		UE_LOG(LogUALUtils, Log, TEXT("[SetSimpleProperty] Set soft object path to: %s"), *AssetPath);
		return true;
	}

	// === FSoftClassProperty 支持（软类引用） ===
	if (FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Prop))
	{
		FString ClassPath;
		if (Value->Type == EJson::String)
		{
			ClassPath = Value->AsString();
		}
		else
		{
			OutError = TEXT("expects a string (class path)");
			return false;
		}

		void* Ptr = SoftClassProp->ContainerPtrToValuePtr<void>(Obj);
		if (!Ptr)
		{
			return false;
		}
		*static_cast<FSoftObjectPtr*>(Ptr) = FSoftObjectPath(ClassPath);
		return true;
	}

	// === FClassProperty 支持（UClass 引用） ===
	if (FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
	{
		FString ClassName;
		if (Value->Type == EJson::String)
		{
			ClassName = Value->AsString();
		}
		else if (Value->Type == EJson::Null)
		{
			void* Ptr = ClassProp->ContainerPtrToValuePtr<void>(Obj);
			if (Ptr)
			{
				ClassProp->SetPropertyValue(Ptr, nullptr);
				return true;
			}
			return false;
		}
		else
		{
			OutError = TEXT("expects a string (class name/path)");
			return false;
		}

		// 尝试查找类
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
		UClass* FoundClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
#else
		UClass* FoundClass = FindObject<UClass>(ANY_PACKAGE, *ClassName);
#endif
		if (!FoundClass)
		{
			FoundClass = LoadObject<UClass>(nullptr, *ClassName);
		}
		if (!FoundClass)
		{
			OutError = FString::Printf(TEXT("Class not found: %s"), *ClassName);
			return false;
		}

		void* Ptr = ClassProp->ContainerPtrToValuePtr<void>(Obj);
		if (!Ptr)
		{
			return false;
		}
		ClassProp->SetPropertyValue(Ptr, FoundClass);
		return true;
	}

	/**
	 * 兜底：交给引擎自己的文本导入。
	 *
	 * 上面那串 `CastField` 是一份手抄的类型表，永远追不上属性系统 ——
	 * 数组就是它漏掉的那个：`blueprint_set_property` 对 `OverrideMaterials`
	 * 一直回「unsupported property type: ArrayProperty」，而 `material_apply`
	 * 的工具说明里正拿这个字段当**推荐写法**。文档承诺、实现没有，
	 * 2026-09-16 的用户反馈上就卡在这里。
	 *
	 * `PropertyValueFromString` 走的是 `ImportText`，数组、任意结构体、Map
	 * 全都认，且 5.0–5.8 九版签名一致。JSON 先翻成 UE 自己的字面量形式
	 * （数组 `(a,b)`、结构体 `(Key=Value)`），翻不动就让它返回 false，
	 * 不静默留个半成品。
	 */
	{
		const FString Literal = JsonValueToImportText(Value);
		if (!Literal.IsEmpty()
			&& FBlueprintEditorUtils::PropertyValueFromString(Prop, Literal, reinterpret_cast<uint8*>(Obj), Obj))
		{
			UE_LOG(LogUALUtils, Log, TEXT("[SetSimpleProperty] Imported '%s' into '%s' (%s)"),
				*Literal, *Prop->GetName(), *Prop->GetClass()->GetName());
			return true;
		}
		OutError = FString::Printf(
			TEXT("could not write '%s' into '%s' (%s)"),
			*Literal, *Prop->GetName(), *Prop->GetClass()->GetName());
	}
	return false;
}

/**
 * JSON → UE 的属性字面量（`ImportText` 认的那种）。
 *
 * 数组是 `(a,b,c)`，结构体/Map 是 `(Key=Value,...)`，字符串加引号并转义。
 * 只负责形状，值合不合法由 `ImportText` 说了算。
 */
FString UAL_CommandUtils::JsonValueToImportText(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return FString();
	}

	switch (Value->Type)
	{
	case EJson::Boolean:
		return Value->AsBool() ? TEXT("true") : TEXT("false");

	case EJson::Number:
		return FString::SanitizeFloat(Value->AsNumber());

	case EJson::String:
		return FString::Printf(TEXT("\"%s\""), *Value->AsString().ReplaceCharWithEscapedChar());

	case EJson::Array:
	{
		TArray<FString> Parts;
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			const FString Part = JsonValueToImportText(Item);
			if (Part.IsEmpty())
			{
				return FString();  // 有一项翻不动，整个就别翻 —— 半个数组比报错还糟
			}
			Parts.Add(Part);
		}
		return FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(",")));
	}

	case EJson::Object:
	{
		TArray<FString> Parts;
		for (const auto& Pair : Value->AsObject()->Values)
		{
			const FString Part = JsonValueToImportText(Pair.Value);
			if (Part.IsEmpty())
			{
				return FString();
			}
			Parts.Add(FString::Printf(TEXT("%s=%s"), *Pair.Key, *Part));
		}
		return FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(",")));
	}

	default:
		return FString();
	}
}

FString UAL_CommandUtils::JsonValueToString(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return TEXT("null");
	}
	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer, false);
	Writer->Close();
	return Out;
}

TSharedPtr<FJsonObject> UAL_CommandUtils::BuildSelectedProps(AActor* Actor, const TArray<FString>& WantedProps)
{
	if (!Actor)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();

	auto TryCollect = [&](UObject* Obj, const FString& PropName) -> bool
	{
		return TryCollectProperty(Obj, PropName, Props);
	};

	for (const FString& PropName : WantedProps)
	{
		if (PropName.IsEmpty())
		{
			continue;
		}

		// Self
		if (TryCollect(Actor, PropName))
		{
			continue;
		}

		// RootComponent
		if (USceneComponent* RootComp = Actor->GetRootComponent())
		{
			if (TryCollect(RootComp, PropName))
			{
				continue;
			}
		}

		// Other Components
		for (UActorComponent* Comp : Actor->GetComponents())
		{
			if (!Comp)
			{
				continue;
			}
			if (TryCollect(Comp, PropName))
			{
				break;
			}
		}
	}

	return Props;
}

void UAL_CommandUtils::SendResponse(const FString& RequestId, int32 Code, const TSharedPtr<FJsonObject>& Data)
{
	if (RequestId.IsEmpty())
	{
		return;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("ver"), TEXT("1.0"));
	Root->SetStringField(TEXT("type"), TEXT("res"));
	Root->SetStringField(TEXT("id"), RequestId);
	Root->SetNumberField(TEXT("code"), Code);
	if (Data.IsValid())
	{
		Root->SetObjectField(TEXT("result"), Data);
	}

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	
	FUAL_NetworkManager::Get().SendMessage(OutputString);
}

void UAL_CommandUtils::SendError(const FString& RequestId, int32 Code, const FString& Message)
{
	TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
	ErrObj->SetStringField(TEXT("message"), Message);
	SendResponse(RequestId, Code, ErrObj);
}

void UAL_CommandUtils::SendError(const FString& RequestId, int32 Code, const FString& Message, const TSharedPtr<FJsonObject>& Details)
{
	TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
	ErrObj->SetStringField(TEXT("message"), Message);
	if (Details.IsValid())
	{
		ErrObj->SetObjectField(TEXT("details"), Details);
	}
	SendResponse(RequestId, Code, ErrObj);
}

void UAL_CommandUtils::SendEvent(const FString& Method, const TSharedPtr<FJsonObject>& Payload)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("ver"), TEXT("1.0"));
	Root->SetStringField(TEXT("type"), TEXT("evt"));
	Root->SetStringField(TEXT("method"), Method);
	if (Payload.IsValid())
	{
		Root->SetObjectField(TEXT("payload"), Payload);
	}

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	
	FUAL_NetworkManager::Get().SendMessage(OutputString);
}
