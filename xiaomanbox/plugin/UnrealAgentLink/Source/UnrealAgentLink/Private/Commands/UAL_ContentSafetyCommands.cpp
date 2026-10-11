#include "UAL_ContentSafetyCommands.h"
#include "UAL_RegistryReady.h"
#include "UAL_CommandUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "SourceControlOperations.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/ArchiveUObject.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALSafety, Log, All);

// ============================================================================
// 公共小工具
// ============================================================================

// 具名而不是匿名命名空间：这个模块开着 unity build，几个 .cpp 会拼进同一个翻译单元，
// 匿名命名空间里同名的小工具（UAL_Registry / UAL_ToPackageName …）会撞在一起
namespace UALSafety
{
	IAssetRegistry& UAL_Registry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	/** 去掉 `.Object` 后缀和尾部斜杠，得到包名（同 UAL_ContentOrganizeCommands.cpp） */
	FString UAL_ToPackageName(const FString& Path)
	{
		FString Out = Path.TrimStartAndEnd();
		int32 DotIndex = INDEX_NONE;
		if (Out.FindChar(TEXT('.'), DotIndex))
		{
			Out = Out.Left(DotIndex);
		}
		while (Out.Len() > 1 && Out.EndsWith(TEXT("/")))
		{
			Out.LeftChopInline(1);
		}
		return Out;
	}

	/**
	 * 把一个输入（资产包路径 / 对象路径 / 目录）解析成包名集合。只读注册表。
	 *
	 * 和 batch_move 规划时的解析结果一致：有资产占着这个名字就是资产，否则是目录就
	 * 递归展开；两者都不是记进 NotFound。
	 */
	void UAL_ResolveSources(IAssetRegistry& Registry, const FString& Input, TSet<FString>& Out, TArray<FString>& NotFound)
	{
		const FString PackageName = UALSafety::UAL_ToPackageName(Input);
		if (PackageName.IsEmpty())
		{
			NotFound.Add(Input);
			return;
		}
		TArray<FAssetData> InPackage;
		Registry.GetAssetsByPackageName(FName(*PackageName), InPackage);
		if (InPackage.Num() > 0)
		{
			Out.Add(InPackage[0].PackageName.ToString());
			return;
		}
		if (Registry.PathExists(PackageName))
		{
			TArray<FAssetData> InFolder;
			Registry.GetAssetsByPath(FName(*PackageName), InFolder, /*bRecursive=*/true);
			for (const FAssetData& Data : InFolder)
			{
				Out.Add(Data.PackageName.ToString());
			}
			return;
		}
		NotFound.Add(Input);
	}

	/** 读 `field` 数组里每个对象的 `key` 字符串 */
	void UAL_ReadObjectArrayField(const TSharedPtr<FJsonObject>& Payload, const TCHAR* Field, const TCHAR* Key, TArray<FString>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Payload->TryGetArrayField(Field, Array) || !Array)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj || !Obj->IsValid())
			{
				continue;
			}
			FString Str;
			if ((*Obj)->TryGetStringField(Key, Str) && !Str.IsEmpty())
			{
				Out.Add(Str);
			}
		}
	}

	void UAL_ReadStringArray(const TSharedPtr<FJsonObject>& Payload, const TCHAR* Field, TArray<FString>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Payload->TryGetArrayField(Field, Array) || !Array)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			FString Str;
			if (Value.IsValid() && Value->TryGetString(Str) && !Str.IsEmpty())
			{
				Out.Add(Str);
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> UAL_StringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(Value));
		}
		return Out;
	}
}

void UAL_CollectReferencers(IAssetRegistry& Registry, const FName PackageName, TArray<FString>& Out)
{
	TArray<FName> Referencers;
	Registry.GetReferencers(PackageName, Referencers, UE::AssetRegistry::EDependencyCategory::Package);
	for (const FName& Ref : Referencers)
	{
		const FString RefStr = Ref.ToString();
		if (!RefStr.StartsWith(TEXT("/Script/")))
		{
			Out.Add(RefStr);
		}
	}
}

// ============================================================================
// 签出预检
// ============================================================================
//
// 引擎的行为和直觉相反（AssetRenameManager.cpp，九版形状一致）：
//
//   - RenameAssets 是「自动签出、无对话框」模式，要签出的不只是被搬的资产，
//     还有**全部引用者**（搬 10 个材质、被 300 个关卡引用 = 签出 310 个文件）；
//   - 任何一个签不出来，整批一个都不动；
//   - 原因只写进编辑器日志。
//
// 所以事前把「源 + 引用者」按引擎同一套判定过一遍，谁会让整批中止、被谁占着、
// 文件在哪，都在响应里。这里**不代替用户签出**：签出会在别人的工作区留下痕迹。

// 具名而不是匿名命名空间：这个模块开着 unity build，几个 .cpp 会拼进同一个翻译单元，
// 匿名命名空间里同名的小工具（UAL_Registry / UAL_ToPackageName …）会撞在一起
namespace UALSafety
{
	/** 包名 → 本地绝对路径。.uasset 优先、.umap 兜底；都不在磁盘上时给 .uasset 的应有位置 */
	FString UAL_PreflightFilename(const FString& PackageName)
	{
		FString AssetFile;
		const bool bHasAssetPath = FPackageName::TryConvertLongPackageNameToFilename(PackageName, AssetFile, FPackageName::GetAssetPackageExtension());
		if (bHasAssetPath && IFileManager::Get().FileExists(*AssetFile))
		{
			return FPaths::ConvertRelativePathToFull(AssetFile);
		}
		FString MapFile;
		if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, MapFile, FPackageName::GetMapPackageExtension())
			&& IFileManager::Get().FileExists(*MapFile))
		{
			return FPaths::ConvertRelativePathToFull(MapFile);
		}
		return bHasAssetPath ? FPaths::ConvertRelativePathToFull(AssetFile) : FString();
	}

	/** Provider 回来的文件名和我们送进去的可能大小写 / 斜杠不同，比对前归一 */
	FString UAL_NormalizeForCompare(const FString& Filename)
	{
		FString Out = FPaths::ConvertRelativePathToFull(Filename);
		FPaths::NormalizeFilename(Out);
		return Out.ToLower();
	}

	void UAL_SetState(FUAL_PackageWriteState& State, const TCHAR* Name, bool bBlocks)
	{
		State.State = Name;
		State.bBlocks = bBlocks;
	}

	TSharedPtr<FJsonObject> UAL_WriteStateJson(const FUAL_PackageWriteState& State)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("package"), State.Package);
		Obj->SetStringField(TEXT("filename"), State.Filename);
		Obj->SetStringField(TEXT("state"), State.State);
		if (!State.CheckedOutBy.IsEmpty())
		{
			Obj->SetStringField(TEXT("checked_out_by"), State.CheckedOutBy);
		}
		Obj->SetStringField(TEXT("role"), State.Role);
		Obj->SetArrayField(TEXT("for"), UALSafety::UAL_StringArray(State.For));
		return Obj;
	}
}

void UAL_PreflightPackages(const TArray<FUAL_PackageWriteState>& In, FUAL_PreflightResult& Out)
{
	Out.States = In;
	Out.BlockedCount = 0;
	for (FUAL_PackageWriteState& State : Out.States)
	{
		State.Filename = UALSafety::UAL_PreflightFilename(State.Package);
		State.CheckedOutBy.Empty();
		UALSafety::UAL_SetState(State, TEXT("writable"), false);
	}

	ISourceControlModule& SccModule = ISourceControlModule::Get();
	Out.bSccEnabled = SccModule.IsEnabled();

	if (Out.bSccEnabled)
	{
		ISourceControlProvider& Provider = SccModule.GetProvider();
		Out.SccProvider = Provider.GetName().ToString();
		Out.bSccAvailable = Provider.IsAvailable();

		// 状态取不到就按 scc_unavailable 报，不猜：5.5+ 的 RenameAssets 在 Provider 不可用时
		// 自己就会中断（RevisionControlIsNotAvailable），5.0–5.4 签出会失败，结果一样
		auto MarkAllUnavailable = [&Out]()
		{
			for (FUAL_PackageWriteState& State : Out.States)
			{
				UALSafety::UAL_SetState(State, TEXT("scc_unavailable"), true);
			}
		};

		TArray<FString> Files;
		for (const FUAL_PackageWriteState& State : Out.States)
		{
			if (!State.Filename.IsEmpty())
			{
				Files.Add(State.Filename);
			}
		}

		if (!Out.bSccAvailable)
		{
			MarkAllUnavailable();
		}
		else if (Files.Num() > 0)
		{
			// 先 FUpdateStatus 再 GetState(Use)，和 AutoCheckOut 一样：缓存里的状态可能是
			// 几分钟前的，别人刚签出的看不见
			const ECommandResult::Type StatusResult = Provider.Execute(ISourceControlOperation::Create<FUpdateStatus>(), Files);
			TArray<FSourceControlStateRef> SccStates;
			if (StatusResult != ECommandResult::Succeeded
				|| Provider.GetState(Files, SccStates, EStateCacheUsage::Use) != ECommandResult::Succeeded)
			{
				MarkAllUnavailable();
			}
			else
			{
				TMap<FString, FSourceControlStateRef> ByFile;
				for (const FSourceControlStateRef& SccState : SccStates)
				{
					ByFile.Add(UALSafety::UAL_NormalizeForCompare(SccState->GetFilename()), SccState);
				}

				int32 FileIndex = 0;
				for (FUAL_PackageWriteState& State : Out.States)
				{
					if (State.Filename.IsEmpty())
					{
						continue;
					}
					const FSourceControlStateRef* Found = ByFile.Find(UALSafety::UAL_NormalizeForCompare(State.Filename));
					// Provider 没按我们的文件名回（有的实现会重写路径）时退到按位置对应
					if (!Found && SccStates.Num() == Files.Num() && SccStates.IsValidIndex(FileIndex))
					{
						Found = &SccStates[FileIndex];
					}
					++FileIndex;
					if (!Found)
					{
						UALSafety::UAL_SetState(State, TEXT("scc_unavailable"), true);
						continue;
					}

					// 判定顺序照抄 AutoCheckOut：先看别人占着，再看不是最新，
					// 然后「不受管或已可写」放行，剩下的是能自动签出的
					const FSourceControlStateRef& SccState = *Found;
					FString Who;
					if (SccState->IsCheckedOutOther(&Who))
					{
						UALSafety::UAL_SetState(State, TEXT("checked_out_other"), true);
						State.CheckedOutBy = Who;
					}
					else if (!SccState->IsCurrent())
					{
						UALSafety::UAL_SetState(State, TEXT("not_at_head"), true);
					}
					else if (!SccState->IsSourceControlled() || SccState->CanEdit())
					{
						UALSafety::UAL_SetState(State, TEXT("writable"), false);
					}
					else
					{
						UALSafety::UAL_SetState(State, TEXT("needs_checkout"), false);
					}
				}
			}
		}
	}
	else
	{
		// 没开源码管理：引擎只看只读位，而且**不会**替你去掉只读
		for (FUAL_PackageWriteState& State : Out.States)
		{
			if (!State.Filename.IsEmpty()
				&& IFileManager::Get().FileExists(*State.Filename)
				&& IFileManager::Get().IsReadOnly(*State.Filename))
			{
				UALSafety::UAL_SetState(State, TEXT("readonly_no_scc"), true);
			}
		}
	}

	for (const FUAL_PackageWriteState& State : Out.States)
	{
		if (State.bBlocks)
		{
			++Out.BlockedCount;
		}
	}
}

TSharedPtr<FJsonObject> UAL_PreflightJson(const FUAL_PreflightResult& Result)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("scc_enabled"), Result.bSccEnabled);
	Obj->SetStringField(TEXT("scc_provider"), Result.SccProvider);
	Obj->SetBoolField(TEXT("scc_available"), Result.bSccAvailable);
	Obj->SetNumberField(TEXT("checked"), Result.States.Num());
	Obj->SetNumberField(TEXT("blocked"), Result.BlockedCount);

	TArray<TSharedPtr<FJsonValue>> Blocking;
	TArray<TSharedPtr<FJsonValue>> States;
	for (const FUAL_PackageWriteState& State : Result.States)
	{
		const TSharedPtr<FJsonObject> Json = UALSafety::UAL_WriteStateJson(State);
		if (State.bBlocks)
		{
			Blocking.Add(MakeShared<FJsonValueObject>(Json));
		}
		// 阻塞的全列（那是用户要去处理的），全量列表最多 500 条
		if (States.Num() < 500)
		{
			States.Add(MakeShared<FJsonValueObject>(Json));
		}
	}
	Obj->SetArrayField(TEXT("blocking"), Blocking);
	Obj->SetArrayField(TEXT("states"), States);
	Obj->SetBoolField(TEXT("states_truncated"), Result.States.Num() > States.Num());
	return Obj;
}

// ============================================================================
// CDO 引用预检
// ============================================================================
//
// 引擎改名前会查原生类 CDO 引用，命中就弹一个确认框（AssetRenameManager.cpp，
// 不受 bWithDialog 控制）。无人值守下这个框静默返回 Cancel，整批中止，一行日志
// 都没有 —— 事后没有任何证据，只能事前查。这里跑和 FindCDOReferences 同一套遍历。

// 具名而不是匿名命名空间：这个模块开着 unity build，几个 .cpp 会拼进同一个翻译单元，
// 匿名命名空间里同名的小工具（UAL_Registry / UAL_ToPackageName …）会撞在一起
namespace UALSafety
{
	/**
	 * 收一个对象序列化出来的全部软引用路径（只记包名）。
	 *
	 * 旗标照抄引擎的 FSoftObjectPathRenameSerializer：`ArIsModifyingWeakAndStrongReferences`
	 * **必须**为 true —— FSoftObjectProperty::SerializeItem 对「引用收集器」默认跳过软引用，
	 * 只在 IsModifyingWeakAndStrongReferences 或 IsPersistent 时放行（PropertySoftObjectPtr.cpp）。
	 * 我们的 operator<< 只读不写，所以这个旗标不会真的改到什么。
	 */
	class FUAL_SoftPathCollector : public FArchiveUObject
	{
	public:
		/** 包名（小写）→ 第一次遇到它时正在序列化的属性名 */
		TMap<FString, FString> Packages;

		FUAL_SoftPathCollector()
		{
			ArIsObjectReferenceCollector = true;
			ArIsModifyingWeakAndStrongReferences = true;
			SetIsSaving(true);
		}

		virtual FString GetArchiveName() const override { return TEXT("FUAL_SoftPathCollector"); }

		virtual bool ShouldSkipProperty(const FProperty* InProperty) const override
		{
			if (InProperty->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_IsPlainOldData))
			{
				return true;
			}
			const FFieldClass* PropertyClass = InProperty->GetClass();
			if (PropertyClass->GetCastFlags() & (CASTCLASS_FBoolProperty | CASTCLASS_FNameProperty | CASTCLASS_FStrProperty | CASTCLASS_FTextProperty | CASTCLASS_FMulticastDelegateProperty))
			{
				return true;
			}
			if (PropertyClass->GetCastFlags() & (CASTCLASS_FArrayProperty | CASTCLASS_FMapProperty | CASTCLASS_FSetProperty))
			{
				if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(InProperty))
				{
					return ShouldSkipProperty(ArrayProperty->Inner);
				}
				if (const FMapProperty* MapProperty = CastField<FMapProperty>(InProperty))
				{
					return ShouldSkipProperty(MapProperty->KeyProp) && ShouldSkipProperty(MapProperty->ValueProp);
				}
				if (const FSetProperty* SetProperty = CastField<FSetProperty>(InProperty))
				{
					return ShouldSkipProperty(SetProperty->ElementProp);
				}
			}
			return false;
		}

		virtual FArchive& operator<<(FSoftObjectPath& Value) override
		{
			if (Value.IsValid())
			{
				const FString Package = Value.GetLongPackageName();
				if (!Package.IsEmpty())
				{
					const FString Key = Package.ToLower();
					if (!Packages.Contains(Key))
					{
						const FProperty* Property = GetSerializedProperty();
						Packages.Add(Key, Property ? Property->GetName() : FString(TEXT("(serialized)")));
					}
				}
			}
			return *this;
		}
	};
}

void UAL_FindCdoReferences(const TArray<FString>& Packages, TArray<FUAL_CdoRef>& Out)
{
	if (Packages.Num() == 0)
	{
		return;
	}
	TMap<FString, FString> Wanted;
	for (const FString& Package : Packages)
	{
		Wanted.Add(Package.ToLower(), Package);
	}

	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* Cls = *ClassIt;
		// 传 false：不顺手创建 CDO。引擎读裸成员 ClassDefaultObject 正是为了不创建，
		// 但那个成员 5.6 起被标弃用；这个重载 5.0 和 5.8 的实现逐字相同
		UObject* CDO = Cls->GetDefaultObject(/*bCreateIfNeeded=*/false);
		if (!CDO || !CDO->HasAllFlags(RF_ClassDefaultObject) || !IsValidChecked(CDO) || Cls->ClassGeneratedBy != nullptr)
		{
			continue;
		}
		if (Cls->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists) || FKismetEditorUtilities::IsClassABlueprintSkeleton(Cls))
		{
			continue;
		}

		const FString ClassLabel = FString(Cls->GetPrefixCPP()) + Cls->GetName();

		// 硬引用：按包名比对，比引擎的指针比对宽 —— 引擎只认 UBlueprint 本身，
		// 而 FClassFinder 填进 DefaultPawnClass 的是生成类 BP_X_C，同一个包
		for (TFieldIterator<FObjectProperty> PropertyIt(Cls); PropertyIt; ++PropertyIt)
		{
			const UObject* Object = PropertyIt->GetPropertyValue(PropertyIt->ContainerPtrToValuePtr<UObject>(CDO));
			if (!Object)
			{
				continue;
			}
			const UPackage* Package = Object->GetOutermost();
			if (!Package)
			{
				continue;
			}
			if (const FString* Original = Wanted.Find(Package->GetName().ToLower()))
			{
				FUAL_CdoRef Ref;
				Ref.Asset = *Original;
				Ref.Class = ClassLabel;
				Ref.Property = PropertyIt->GetName();
				Ref.Kind = TEXT("hard");
				Out.Add(Ref);
			}
		}

		// 软引用：序列化 CDO，收 FSoftObjectPath。`/Game/X/BP_A.BP_A_C` 的包名是
		// `/Game/X/BP_A`，GetLongPackageName 处理得对
		UALSafety::FUAL_SoftPathCollector Collector;
		CDO->Serialize(Collector);
		for (const TPair<FString, FString>& Pair : Collector.Packages)
		{
			if (const FString* Original = Wanted.Find(Pair.Key))
			{
				FUAL_CdoRef Ref;
				Ref.Asset = *Original;
				Ref.Class = ClassLabel;
				Ref.Property = Pair.Value;
				Ref.Kind = TEXT("soft");
				Out.Add(Ref);
			}
		}
	}
}

TSharedPtr<FJsonObject> UAL_CdoRefsJson(int32 Checked, const TArray<FUAL_CdoRef>& Refs)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("checked"), Checked);
	TArray<TSharedPtr<FJsonValue>> Hits;
	for (const FUAL_CdoRef& Ref : Refs)
	{
		TSharedPtr<FJsonObject> Hit = MakeShared<FJsonObject>();
		Hit->SetStringField(TEXT("asset"), Ref.Asset);
		Hit->SetStringField(TEXT("class"), Ref.Class);
		Hit->SetStringField(TEXT("property"), Ref.Property);
		Hit->SetStringField(TEXT("kind"), Ref.Kind);
		Hits.Add(MakeShared<FJsonValueObject>(Hit));
	}
	Obj->SetArrayField(TEXT("hits"), Hits);
	return Obj;
}

// ============================================================================
// 命令
// ============================================================================

void FUAL_ContentSafetyCommands::Handle_RegistryStatus(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	UAL_CommandUtils::SendResponse(RequestId, 200, FUAL_RegistryReady::StatusJson());
}

void FUAL_ContentSafetyCommands::Handle_RegistryScan(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	TArray<FString> Paths;
	TArray<FString> Files;

	const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
	if (Payload.IsValid() && Payload->TryGetArrayField(TEXT("paths"), PathsArr) && PathsArr)
	{
		for (const TSharedPtr<FJsonValue>& Val : *PathsArr)
		{
			FString One;
			if (Val.IsValid() && Val->TryGetString(One) && !One.TrimStartAndEnd().IsEmpty())
			{
				Paths.AddUnique(UALSafety::UAL_ToPackageName(One));
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* FilesArr = nullptr;
	if (Payload.IsValid() && Payload->TryGetArrayField(TEXT("files"), FilesArr) && FilesArr)
	{
		for (const TSharedPtr<FJsonValue>& Val : *FilesArr)
		{
			FString One;
			if (Val.IsValid() && Val->TryGetString(One) && !One.TrimStartAndEnd().IsEmpty())
			{
				Files.AddUnique(One.TrimStartAndEnd());
			}
		}
	}

	if (Paths.Num() == 0 && Files.Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400,
			TEXT("paths 和 files 至少给一个。例：{\"paths\":[\"/Game/PolygonApocalypse\"]}"));
		return;
	}

	bool bForce = true;
	if (Payload.IsValid())
	{
		Payload->TryGetBoolField(TEXT("force"), bForce);
	}

	IAssetRegistry& Registry = UALSafety::UAL_Registry();

	if (Files.Num() > 0)
	{
		Registry.ScanFilesSynchronous(Files, bForce);
	}
	if (Paths.Num() > 0)
	{
		Registry.ScanPathsSynchronous(Paths, bForce);
	}

	// 回读：扫完之后每个目录下注册表里现在有多少条。
	// 没有这一段的话，「扫过了」和「扫了但一条都没进来」长得一模一样
	TArray<TSharedPtr<FJsonValue>> Registered;
	int32 TotalAssets = 0;
	for (const FString& Path : Paths)
	{
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*Path), Assets, /*bRecursive=*/true);
		TotalAssets += Assets.Num();

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), Path);
		Entry->SetNumberField(TEXT("assets"), Assets.Num());
		Registered.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("scanned_paths"), Paths.Num());
	Data->SetNumberField(TEXT("scanned_files"), Files.Num());
	Data->SetArrayField(TEXT("registered"), Registered);
	Data->SetNumberField(TEXT("total_assets"), TotalAssets);
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_ContentSafetyCommands::RegisterCommands(
	TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("content.registry_status"), &Handle_RegistryStatus);
	CommandMap.Add(TEXT("content.registry_scan"), &Handle_RegistryScan);
	// content.checkout_preflight / content.cdo_refs 已删（2026-09-16）：
	// 两条都是只读壳，包在 UAL_PreflightPackages / UAL_FindCdoReferences 外面，
	// 而 content.batch_move 直接调那两个函数、dry_run=true 时把同样的结果
	// 一起回出去。盒子这边没有任何地方单独调过它们
	UE_LOG(LogUALSafety, Log, TEXT("Registered 2 content safety commands"));
}
