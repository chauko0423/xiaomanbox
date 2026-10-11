#include "UAL_ContentOrganizeCommands.h"
#include "UAL_ContentSafetyCommands.h"
#include "UAL_VersionCompat.h"
#include "UAL_CommandUtils.h"
#include "UAL_RegistryReady.h"
#include "UAL_ScopedDialogAutoAnswer.h"
#include "UAL_ScopedLogCapture.h"
#include "UAL_SavablePackage.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ObjectTools.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "HAL/FileManager.h"
#include "Misc/AssetRegistryInterface.h"
#include "Components/ActorComponent.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALOrganize, Log, All);

// ============================================================================
// 公共小工具 —— 四条命令共用，全部只读注册表
// ============================================================================

namespace
{
	/**
	 * 这里**不等**注册表。注册表还在扫的时候查出来的是残缺结果，但死等会把游戏线程
	 * 卡上几分钟、RPC 超时后用户看到的是「AI 把编辑器搞死了」。每条命令在解析完参数后
	 * 先过 FUAL_RegistryReady::Ensure：没扫完就立刻回 503 + 进度，由盒子侧轮询重发。
	 */
	IAssetRegistry& UAL_Registry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	FString UAL_ClassNameOf(const FAssetData& AssetData)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		return AssetData.AssetClassPath.GetAssetName().ToString();
#else
		return AssetData.AssetClass.ToString();
#endif
	}

	FAssetData UAL_AssetByObjectPath(IAssetRegistry& Registry, const FString& ObjectPath)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		return Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
#else
		return Registry.GetAssetByObjectPath(FName(*ObjectPath));
#endif
	}

	/** 去掉 `.Object` 后缀和尾部斜杠，得到包名 */
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
	 * 把包名或对象路径解析成 FAssetData。
	 *
	 * 先补全再解析（同 content.move 的教训）：5.1+ 的 FSoftObjectPath 拿到只有包名的
	 * 路径会解析成**包**，IsValid 为真但不是资产。content.search 返回的正是短路径。
	 */
	bool UAL_ResolveAsset(IAssetRegistry& Registry, const FString& Path, FAssetData& Out)
	{
		const FString PackageName = UAL_ToPackageName(Path);
		if (PackageName.IsEmpty())
		{
			return false;
		}
		Out = UAL_AssetByObjectPath(Registry, PackageName + TEXT(".") + FPaths::GetBaseFilename(PackageName));
		if (Out.IsValid())
		{
			return true;
		}
		if (Path.Contains(TEXT(".")))
		{
			Out = UAL_AssetByObjectPath(Registry, Path);
			if (Out.IsValid())
			{
				return true;
			}
		}
		TArray<FAssetData> InPackage;
		Registry.GetAssetsByPackageName(FName(*PackageName), InPackage);
		if (InPackage.Num() > 0)
		{
			Out = InPackage[0];
			return true;
		}
		return false;
	}

	bool UAL_IsFolder(IAssetRegistry& Registry, const FString& Path)
	{
		const FString PackageName = UAL_ToPackageName(Path);
		if (Path.Contains(TEXT(".")))
		{
			return false;
		}
		// 有资产直接占着这个名字就不是目录
		TArray<FAssetData> InPackage;
		Registry.GetAssetsByPackageName(FName(*PackageName), InPackage);
		if (InPackage.Num() > 0)
		{
			return false;
		}
		return Registry.PathExists(PackageName);
	}

	/** 包在磁盘上的字节数；找不到回 0 —— 不猜 */
	int64 UAL_OrganizePackageFileSize(const FString& PackageName)
	{
		FString Filename;
		if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename, FPackageName::GetAssetPackageExtension()))
		{
			const int64 Size = IFileManager::Get().FileSize(*Filename);
			if (Size >= 0)
			{
				return Size;
			}
		}
		if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename, FPackageName::GetMapPackageExtension()))
		{
			const int64 Size = IFileManager::Get().FileSize(*Filename);
			if (Size >= 0)
			{
				return Size;
			}
		}
		return 0;
	}

	/** 包名 → 磁盘文件。资产和关卡的扩展名不同，两种都试 */
	bool UAL_PackageFilename(const FString& PackageName, FString& OutFilename)
	{
		if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, OutFilename, FPackageName::GetAssetPackageExtension())
			&& IFileManager::Get().FileExists(*OutFilename))
		{
			return true;
		}
		if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, OutFilename, FPackageName::GetMapPackageExtension())
			&& IFileManager::Get().FileExists(*OutFilename))
		{
			return true;
		}
		return false;
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

	TSharedPtr<FJsonObject> UAL_CountMap(const TMap<FString, int32>& Counts)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<FString> Keys;
		Counts.GetKeys(Keys);
		Keys.Sort();
		for (const FString& Key : Keys)
		{
			Obj->SetNumberField(Key, Counts[Key]);
		}
		return Obj;
	}

	bool UAL_ReadStringArray(const TSharedPtr<FJsonObject>& Payload, const TCHAR* Field, TArray<FString>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Payload->TryGetArrayField(Field, Array) || !Array)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			FString Str;
			if (Value.IsValid() && Value->TryGetString(Str) && !Str.IsEmpty())
			{
				Out.Add(Str);
			}
		}
		return true;
	}

	bool UAL_IsScriptOrEngine(const FString& PackageName)
	{
		return PackageName.StartsWith(TEXT("/Script/")) || PackageName.StartsWith(TEXT("/Engine/"))
			|| PackageName.StartsWith(TEXT("/Temp/"));
	}

	bool UAL_IsUnderFolder(const FString& PackageName, const FString& Folder)
	{
		return PackageName.Equals(Folder, ESearchCase::IgnoreCase)
			|| PackageName.StartsWith(Folder + TEXT("/"), ESearchCase::IgnoreCase);
	}

	/** 当前脏着、可保存的包名集合 —— 用来算「这条命令改脏了哪些」 */
	/** 判据统一在 `UAL_SavablePackage.h`（原来这里那份漏了 `/Script/` 编译包） */
	TSet<FName> UAL_DirtyPackageNames()
	{
		return UAL_DirtySavablePackageNames();
	}

	/** 存一个包。用 IsDirty 判成败 —— SavePackage 的返回类型在各版本上不一样 */
	bool UAL_SavePackage(UPackage* Package)
	{
		if (!Package)
		{
			return false;
		}
		FString Filename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
				Package->GetName(), Filename,
				Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension()))
		{
			return false;
		}
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		UPackage::SavePackage(Package, nullptr, *Filename, SaveArgs);
		return !Package->IsDirty();
	}
}

// ============================================================================
// content.naming_audit
// ============================================================================
//
// ## 前缀表来源
//
// Epic 官方《Recommended Asset Naming Conventions》（bEpic=true 的那些）。
// 官方没列但业内基本一致的补了四个（MaterialFunction → MF_、
// MaterialParameterCollection → MPC_、TextureRenderTarget2D → RT_、
// 蓝图函数库 → BFL_），标 bEpic=false，调用方看得出哪条是我们补的。
//
// 表里**没有**的类型（音频、字体、物理、关卡…）官方和各家风格都不一致，
// 猜一个只会把工程改成我们的口味。所以不猜：按 unknown_classes 报出来，
// 调用方要审就通过 `rules` 传自己的前缀进来。
//
// ## 蓝图要按父类分
//
// `Blueprint` 这一个类对应四个前缀（BP_ / AC_ / BI_ / BFL_）。靠注册表标签
// 就能分：BlueprintType 标签区分接口和函数库，NativeParentClass 标签给出最近的
// 原生父类 —— 原生类本来就在内存里，FindObject 一下就知道它是不是组件，
// 不用加载蓝图本身。

namespace
{
	struct FUAL_NamingRule
	{
		const TCHAR* ClassName;
		/** 规范前缀，建议名用它 */
		const TCHAR* Prefix;
		/** 也算合规的别名前缀，逗号分隔；可为空 */
		const TCHAR* Accepted;
		bool bEpic;
	};

	static const FUAL_NamingRule GDefaultNamingRules[] = {
		// General
		{ TEXT("Texture2D"), TEXT("T_"), TEXT(""), true },
		{ TEXT("TextureCube"), TEXT("T_"), TEXT("HDR_"), true },
		{ TEXT("Texture2DArray"), TEXT("T_"), TEXT(""), true },
		{ TEXT("VolumeTexture"), TEXT("T_"), TEXT(""), true },
		{ TEXT("TextureRenderTarget2D"), TEXT("RT_"), TEXT("T_"), false },
		{ TEXT("Material"), TEXT("M_"), TEXT("PPM_"), true },
		{ TEXT("MaterialInstanceConstant"), TEXT("MI_"), TEXT(""), true },
		{ TEXT("MaterialFunction"), TEXT("MF_"), TEXT(""), false },
		{ TEXT("MaterialParameterCollection"), TEXT("MPC_"), TEXT(""), false },
		// SKM_ 是 Epic 自己 5.x 模板用的写法（SKM_Manny / SKM_Quinn），
		// 判成违规再建议改成 SK_SKM_Manny 是纯噪声，收进别名
		{ TEXT("StaticMesh"), TEXT("SM_"), TEXT(""), true },
		{ TEXT("SkeletalMesh"), TEXT("SK_"), TEXT("SKM_"), true },
		{ TEXT("Skeleton"), TEXT("SKEL_"), TEXT(""), true },
		{ TEXT("PhysicsAsset"), TEXT("PHYS_"), TEXT("PA_"), true },
		{ TEXT("PhysicalMaterial"), TEXT("PM_"), TEXT(""), true },
		// Blueprints（Blueprint 本身按父类另判，见 UAL_ExpectedBlueprintPrefix）
		{ TEXT("AnimBlueprint"), TEXT("ABP_"), TEXT(""), true },
		{ TEXT("WidgetBlueprint"), TEXT("WBP_"), TEXT(""), true },
		{ TEXT("DataTable"), TEXT("DT_"), TEXT(""), true },
		{ TEXT("CurveTable"), TEXT("CT_"), TEXT(""), true },
		{ TEXT("UserDefinedEnum"), TEXT("E_"), TEXT(""), true },
		{ TEXT("UserDefinedStruct"), TEXT("F_"), TEXT("S_"), true },
		// Particles
		{ TEXT("NiagaraSystem"), TEXT("FXS_"), TEXT("NS_"), true },
		{ TEXT("NiagaraEmitter"), TEXT("FXE_"), TEXT("NE_"), true },
		{ TEXT("NiagaraScript"), TEXT("FXF_"), TEXT("NF_"), true },
		// Animation
		{ TEXT("ControlRigBlueprint"), TEXT("Rig_"), TEXT("CR_"), true },
		{ TEXT("AnimMontage"), TEXT("AM_"), TEXT(""), true },
		{ TEXT("AnimSequence"), TEXT("AS_"), TEXT("A_"), true },
		{ TEXT("BlendSpace"), TEXT("BS_"), TEXT(""), true },
		{ TEXT("BlendSpace1D"), TEXT("BS_"), TEXT(""), true },
		{ TEXT("AimOffsetBlendSpace"), TEXT("BS_"), TEXT("AO_"), true },
		// Cinematics
		{ TEXT("LevelSequence"), TEXT("LS_"), TEXT(""), true },
		// Media
		{ TEXT("FileMediaSource"), TEXT("MS_"), TEXT(""), true },
		{ TEXT("StreamMediaSource"), TEXT("MS_"), TEXT(""), true },
		{ TEXT("ImgMediaSource"), TEXT("MS_"), TEXT(""), true },
		{ TEXT("MediaPlayer"), TEXT("MP_"), TEXT(""), true },
		{ TEXT("MediaProfile"), TEXT("MPR_"), TEXT(""), true },
		// Other
		{ TEXT("LevelSnapshot"), TEXT("SNAP_"), TEXT(""), true },
		{ TEXT("RemoteControlPreset"), TEXT("RCP_"), TEXT(""), true },
		{ TEXT("DisplayClusterConfigurationData"), TEXT("NDC_"), TEXT(""), true },
	};

	struct FUAL_ResolvedRule
	{
		FString Prefix;
		TArray<FString> Accepted; // 含 Prefix 本身
		bool bEpic = true;
		bool bOverride = false;
	};

	/**
	 * 蓝图各类前缀的别名。BPI_ / BPFL_ 这些是社区里同样常见的写法，
	 * 判成违规再建议改成 `BI_BPI_TouchInterface` 这种双前缀怪名，
	 * 比不改还糟 —— 收进别名当合规。
	 */
	void UAL_BlueprintAcceptedAliases(const FString& Prefix, TArray<FString>& Out)
	{
		Out.AddUnique(Prefix);
		if (Prefix == TEXT("BI_"))
		{
			Out.AddUnique(TEXT("BPI_"));
		}
		else if (Prefix == TEXT("BFL_"))
		{
			Out.AddUnique(TEXT("BPFL_"));
		}
		else if (Prefix == TEXT("AC_"))
		{
			Out.AddUnique(TEXT("BPC_"));
		}
	}

	/** 蓝图按父类和类型分前缀。返回空表示没法判（不报违规） */
	FString UAL_ExpectedBlueprintPrefix(const FAssetData& AssetData)
	{
		FString BlueprintType;
		AssetData.GetTagValue(FName(TEXT("BlueprintType")), BlueprintType);
		if (BlueprintType.Contains(TEXT("Interface")))
		{
			return TEXT("BI_");
		}
		if (BlueprintType.Contains(TEXT("FunctionLibrary")))
		{
			return TEXT("BFL_");
		}
		if (BlueprintType.Contains(TEXT("MacroLibrary")))
		{
			return TEXT("BML_");
		}

		FString NativeParent;
		if (AssetData.GetTagValue(FName(TEXT("NativeParentClass")), NativeParent) && !NativeParent.IsEmpty())
		{
			// 标签值形如 /Script/CoreUObject.Class'/Script/Engine.Actor'，剥掉外层
			const FString ObjectPath = FPackageName::ExportTextPathToObjectPath(NativeParent);
			if (UClass* NativeClass = FindObject<UClass>(nullptr, *ObjectPath))
			{
				if (NativeClass->IsChildOf(UActorComponent::StaticClass()))
				{
					return TEXT("AC_");
				}
			}
		}
		return TEXT("BP_");
	}

	FString UAL_PascalCase(const FString& Name)
	{
		if (Name.IsEmpty())
		{
			return Name;
		}
		FString Out = Name;
		Out[0] = FChar::ToUpper(Out[0]);
		return Out;
	}
}

void FUAL_ContentOrganizeCommands::Handle_NamingAudit(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString ScopePath = TEXT("/Game");
	Payload->TryGetStringField(TEXT("path"), ScopePath);
	ScopePath = UAL_ToPackageName(ScopePath);
	if (ScopePath.IsEmpty())
	{
		ScopePath = TEXT("/Game");
	}

	bool bRecursive = true;
	Payload->TryGetBoolField(TEXT("recursive"), bRecursive);

	FString ClassFilter;
	Payload->TryGetStringField(TEXT("class_filter"), ClassFilter);

	bool bPascalCase = false;
	Payload->TryGetBoolField(TEXT("pascal_case"), bPascalCase);

	bool bIncludeCompliant = false;
	Payload->TryGetBoolField(TEXT("include_compliant"), bIncludeCompliant);

	int32 Limit = 200;
	Payload->TryGetNumberField(TEXT("limit"), Limit);
	Limit = FMath::Clamp(Limit, 1, 2000);

	TArray<FString> IgnoreClasses;
	UAL_ReadStringArray(Payload, TEXT("ignore_classes"), IgnoreClasses);

	/**
	 * 按目录豁免。
	 *
	 * 模板 / 第三方 / Marketplace 目录用的是它们自己的命名惯例，按本表逐条报
	 * 只会淹没真正要改的那些（真机上 97 条模板动画淹掉了十几条项目自己的资产）。
	 * **不预设任何默认豁免目录** —— 哪些是模板只有用户知道，猜一个等于替他决定。
	 */
	TArray<FString> IgnorePaths;
	UAL_ReadStringArray(Payload, TEXT("ignore_paths"), IgnorePaths);
	for (FString& Ignored : IgnorePaths)
	{
		Ignored = UAL_ToPackageName(Ignored);
		while (Ignored.Len() > 1 && Ignored.EndsWith(TEXT("/")))
		{
			Ignored.LeftChopInline(1);
		}
	}

	// 规则表：默认表 + 调用方覆盖
	TMap<FString, FUAL_ResolvedRule> Rules;
	for (const FUAL_NamingRule& Rule : GDefaultNamingRules)
	{
		FUAL_ResolvedRule Resolved;
		Resolved.Prefix = Rule.Prefix;
		Resolved.bEpic = Rule.bEpic;
		Resolved.Accepted.Add(Rule.Prefix);
		FString AcceptedStr = Rule.Accepted;
		if (!AcceptedStr.IsEmpty())
		{
			TArray<FString> Parts;
			AcceptedStr.ParseIntoArray(Parts, TEXT(","), true);
			for (FString& Part : Parts)
			{
				Part.TrimStartAndEndInline();
				if (!Part.IsEmpty())
				{
					Resolved.Accepted.AddUnique(Part);
				}
			}
		}
		Rules.Add(Rule.ClassName, Resolved);
	}

	const TSharedPtr<FJsonObject>* Overrides = nullptr;
	if (Payload->TryGetObjectField(TEXT("rules"), Overrides) && Overrides && Overrides->IsValid())
	{
		for (const auto& Pair : (*Overrides)->Values)
		{
			FString Prefix;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(Prefix) || Prefix.IsEmpty())
			{
				continue;
			}
			FUAL_ResolvedRule Resolved;
			Resolved.Prefix = Prefix;
			Resolved.Accepted.Add(Prefix);
			Resolved.bEpic = false;
			Resolved.bOverride = true;
			Rules.Add(UAL_JsonKey(Pair.Key), Resolved);
		}
	}

	// 全部已知前缀（任何类型的）—— 用来认出「挂了别的类型的前缀」和剥旧前缀
	TSet<FString> KnownPrefixes;
	for (const auto& Pair : Rules)
	{
		for (const FString& Accepted : Pair.Value.Accepted)
		{
			KnownPrefixes.Add(Accepted);
		}
	}
	for (const TCHAR* BpPrefix : { TEXT("BP_"), TEXT("AC_"), TEXT("BI_"), TEXT("BFL_"), TEXT("BML_") })
	{
		KnownPrefixes.Add(BpPrefix);
	}

	if (!FUAL_RegistryReady::Ensure(Payload, RequestId))
	{
		return;
	}
	IAssetRegistry& Registry = UAL_Registry();

	FARFilter Filter;
	Filter.bRecursivePaths = bRecursive;
	Filter.bRecursiveClasses = true;
	Filter.PackagePaths.Add(FName(*ScopePath));
	if (!ClassFilter.IsEmpty())
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), *ClassFilter));
#else
		Filter.ClassNames.Add(FName(*ClassFilter));
#endif
	}

	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Violations;
	TArray<TSharedPtr<FJsonValue>> Compliant;
	TMap<FString, int32> ByReason;
	TMap<FString, int32> ByClass;
	TMap<FString, int32> UnknownClasses;
	TSet<FString> RulesUsed;
	int32 ViolationCount = 0;
	int32 CompliantCount = 0;
	int32 ConflictCount = 0;
	int32 AmbiguousCount = 0;
	int32 SkippedCount = 0;

	// 同一批建议里两个资产改到同一个名字也是冲突
	TSet<FString> SuggestedTargets;

	for (const FAssetData& AssetData : Assets)
	{
		const FString ClassName = UAL_ClassNameOf(AssetData);
		const FString Name = AssetData.AssetName.ToString();
		const FString PackageName = AssetData.PackageName.ToString();

		if (ClassName == TEXT("ObjectRedirector") || IgnoreClasses.Contains(ClassName))
		{
			++SkippedCount;
			continue;
		}

		bool bIgnoredByPath = false;
		for (const FString& Ignored : IgnorePaths)
		{
			// 前缀比对要带上斜杠，否则 /Game/Anim 会把 /Game/Animals 也豁免掉
			if (PackageName.Equals(Ignored, ESearchCase::IgnoreCase)
				|| PackageName.StartsWith(Ignored + TEXT("/"), ESearchCase::IgnoreCase))
			{
				bIgnoredByPath = true;
				break;
			}
		}
		if (bIgnoredByPath)
		{
			++SkippedCount;
			continue;
		}

		FString ExpectedPrefix;
		TArray<FString> Accepted;
		bool bEpicRule = true;

		if (const FUAL_ResolvedRule* Rule = Rules.Find(ClassName))
		{
			ExpectedPrefix = Rule->Prefix;
			Accepted = Rule->Accepted;
			bEpicRule = Rule->bEpic;
		}
		else if (ClassName == TEXT("Blueprint"))
		{
			ExpectedPrefix = UAL_ExpectedBlueprintPrefix(AssetData);
			UAL_BlueprintAcceptedAliases(ExpectedPrefix, Accepted);
		}
		else
		{
			UnknownClasses.FindOrAdd(ClassName)++;
			continue;
		}
		RulesUsed.Add(ClassName + TEXT(" -> ") + ExpectedPrefix);

		bool bCompliant = false;
		for (const FString& Prefix : Accepted)
		{
			if (Name.StartsWith(Prefix, ESearchCase::CaseSensitive))
			{
				bCompliant = true;
				break;
			}
		}

		if (bCompliant)
		{
			++CompliantCount;
			if (bIncludeCompliant && Compliant.Num() < Limit)
			{
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("path"), PackageName);
				Item->SetStringField(TEXT("class"), ClassName);
				Compliant.Add(MakeShared<FJsonValueObject>(Item));
			}
			continue;
		}

		/**
		 * 违规原因，从具体到笼统。
		 *
		 * ## `wrong_prefix` 剥掉的那一段可能是有意义的，所以两个建议都给
		 *
		 * 一个动画序列叫 `MF_Unarmed_Jog_Bwd`，这里的 `MF` 是「女版」，
		 * 不是材质函数。但 `MF_` 在 KnownPrefixes 里，老逻辑判 wrong_prefix、
		 * 把它剥掉，建议 `AS_Unarmed_Jog_Bwd` —— **语义丢了，而且没人提醒**。
		 * 照单执行的话，Male/Female 两套动画会重名撞在一起。
		 *
		 * 工具无法知道那两个字母是类型前缀还是别的意思，所以不装作知道：
		 * `suggested_name` 仍然给剥过的（多数情况是对的：一张贴图叫
		 * `SM_Rock` 就该改成 `T_Rock`），同时给 `suggested_name_keep`
		 * （不剥，`AS_MF_Unarmed_Jog_Bwd`）并标 `ambiguous:true`，
		 * 让上层把这几条单独摆出来请人看一眼。
		 */
		FString Reason = TEXT("missing_prefix");
		FString Base = Name;
		bool bAmbiguousStrip = false;
		for (const FString& Prefix : Accepted)
		{
			if (Name.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				Reason = TEXT("prefix_case");
				Base = Name.RightChop(Prefix.Len());
				break;
			}
		}
		if (Reason == TEXT("missing_prefix"))
		{
			for (const FString& Known : KnownPrefixes)
			{
				if (Name.StartsWith(Known, ESearchCase::IgnoreCase))
				{
					Reason = TEXT("wrong_prefix");
					Base = Name.RightChop(Known.Len());
					// 剥掉的是**别的类型**的前缀，不是本类型的别名 —— 那一段
					// 究竟是不是前缀，只有人知道
					bAmbiguousStrip = true;
					break;
				}
			}
		}
		while (Base.StartsWith(TEXT("_")))
		{
			Base.RightChopInline(1);
		}
		if (Base.IsEmpty())
		{
			Base = Name;
		}
		FString KeepBase = Name;
		if (bPascalCase)
		{
			Base = UAL_PascalCase(Base);
			KeepBase = UAL_PascalCase(KeepBase);
		}
		FString Suggested = ExpectedPrefix + Base;
		Suggested.ReplaceInline(TEXT("__"), TEXT("_"));
		FString SuggestedKeep = ExpectedPrefix + KeepBase;
		SuggestedKeep.ReplaceInline(TEXT("__"), TEXT("_"));

		const FString SuggestedPackage = FPackageName::GetLongPackagePath(PackageName) / Suggested;

		bool bConflict = false;
		{
			TArray<FAssetData> Existing;
			Registry.GetAssetsByPackageName(FName(*SuggestedPackage), Existing);
			bConflict = Existing.Num() > 0 || SuggestedTargets.Contains(SuggestedPackage.ToLower());
		}
		SuggestedTargets.Add(SuggestedPackage.ToLower());

		++ViolationCount;
		ByReason.FindOrAdd(Reason)++;
		ByClass.FindOrAdd(ClassName)++;
		if (bConflict)
		{
			++ConflictCount;
		}
		if (bAmbiguousStrip)
		{
			++AmbiguousCount;
		}

		if (Violations.Num() < Limit)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("path"), PackageName);
			Item->SetStringField(TEXT("name"), Name);
			Item->SetStringField(TEXT("class"), ClassName);
			Item->SetStringField(TEXT("expected_prefix"), ExpectedPrefix);
			Item->SetStringField(TEXT("reason"), Reason);
			Item->SetStringField(TEXT("suggested_name"), Suggested);
			Item->SetStringField(TEXT("suggested_path"), SuggestedPackage);
			if (bAmbiguousStrip)
			{
				// 剥掉的那一段可能是语义（MF_ = 女版，不是材质函数）。
				// 两个候选都给出来，让上层请人定，别替人选
				const FString KeepPackage = FPackageName::GetLongPackagePath(PackageName) / SuggestedKeep;
				Item->SetBoolField(TEXT("ambiguous"), true);
				Item->SetStringField(TEXT("suggested_name_keep"), SuggestedKeep);
				Item->SetStringField(TEXT("suggested_path_keep"), KeepPackage);

				// 第二候选也要查重。只查第一个的话，用户按建议选了 keep、
				// 结果那个名字早被占了 —— 到 batch_move 才报冲突，白跑一趟
				TArray<FAssetData> KeepExisting;
				Registry.GetAssetsByPackageName(FName(*KeepPackage), KeepExisting);
				if (KeepExisting.Num() > 0 || SuggestedTargets.Contains(KeepPackage.ToLower()))
				{
					Item->SetBoolField(TEXT("conflict_keep"), true);
				}
			}
			if (bConflict)
			{
				Item->SetBoolField(TEXT("conflict"), true);
			}
			if (!bEpicRule)
			{
				Item->SetStringField(TEXT("rule_source"), TEXT("extended"));
			}
			Violations.Add(MakeShared<FJsonValueObject>(Item));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("path"), ScopePath);
	Result->SetNumberField(TEXT("scanned"), Assets.Num());
	Result->SetNumberField(TEXT("compliant_count"), CompliantCount);
	Result->SetNumberField(TEXT("violation_count"), ViolationCount);
	Result->SetNumberField(TEXT("conflict_count"), ConflictCount);
	Result->SetNumberField(TEXT("ambiguous_count"), AmbiguousCount);
	Result->SetNumberField(TEXT("skipped_count"), SkippedCount);
	Result->SetArrayField(TEXT("violations"), Violations);
	if (bIncludeCompliant)
	{
		Result->SetArrayField(TEXT("compliant"), Compliant);
	}
	Result->SetObjectField(TEXT("by_reason"), UAL_CountMap(ByReason));
	Result->SetObjectField(TEXT("by_class"), UAL_CountMap(ByClass));
	Result->SetObjectField(TEXT("unknown_classes"), UAL_CountMap(UnknownClasses));
	TArray<FString> RulesUsedArray = RulesUsed.Array();
	RulesUsedArray.Sort();
	Result->SetArrayField(TEXT("rules_used"), UAL_StringArray(RulesUsedArray));
	Result->SetBoolField(TEXT("truncated"), ViolationCount > Violations.Num());
	Result->SetStringField(TEXT("rules_source"),
		TEXT("Epic 'Recommended Asset Naming Conventions' (dev.epicgames.com); entries marked rule_source=extended are widely used but not in Epic's table."));

	FString Note;
	if (ViolationCount == 0)
	{
		Note = TEXT("Every asset with a known rule already follows the convention.");
	}
	else
	{
		Note = FString::Printf(TEXT("%d asset(s) violate the convention. Feed {source: path, destination: suggested_path} pairs into content.batch_move to apply; items marked conflict=true need a different name first."), ViolationCount);
	}
	if (UnknownClasses.Num() > 0)
	{
		Note += TEXT(" unknown_classes lists asset types without a rule - they were NOT checked. Pass rules:{Class:'Prefix_'} to audit them.");
	}
	if (AmbiguousCount > 0)
	{
		Note += FString::Printf(
			TEXT(" %d suggestion(s) are marked ambiguous=true: the leading token that was stripped is a known prefix of ANOTHER asset type, but it may carry meaning here (e.g. MF_ on an animation can mean 'female', not 'material function'). Those entries also carry suggested_name_keep, which keeps the token. Have a human pick before renaming them."),
			AmbiguousCount);
	}
	Result->SetStringField(TEXT("note"), Note);

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// content.batch_move
// ============================================================================
//
// ## 为什么一次 RenameAssets 而不是循环调 content.move
//
// FAssetRenameManager 搬一批资产的流程是：找出所有引用者 → 加载它们 → 重命名 →
// 保存。引用者加载是最贵的一步（一个被 300 个关卡引用的材质，逐个搬 10 个材质
// 就要把那 300 个关卡加载 10 遍）。整批一起交给它，只加载一遍。
//
// ## 「先能看，再动手」
//
// dry_run 把整个计划（含每个资产被谁引用、目标处有没有冲突）算出来给调用方看，
// 而且**规划阶段不加载任何资产**。真正执行时才 GetAsset()。
//
// ## 报的是回读结果
//
// RenameAssets 返回一个 bool 代表整批；我们不用它当结论，而是逐个去注册表看
// 目标位置有没有出现资产。它说成功但资产不在新位置的情况见过（目标目录不存在）。

namespace
{
	struct FUAL_MoveItem
	{
		FString SourceInput;
		FAssetData Source;
		FString SourcePackage;
		FString DestFolder;
		FString DestName;
		FString DestPackage;
		FString ClassName;
		/** 引用者包名（不含 /Script/）。签出预检要的是名字，不只是个数 */
		TArray<FString> Referencers;
		int32 ReferencerCount = 0;
		/** planned / skipped / error / moved / failed */
		FString Status = TEXT("planned");
		FString Error;
		bool bAutoRenamed = false;
		/** 落盘后的指纹（只在 moved 且文件在磁盘上时填），给回滚账本判「这之后有没有被人动过」 */
		FString File;
		int64 Bytes = -1;
		FString MTime;
	};

	bool UAL_DestinationOccupied(IAssetRegistry& Registry, const FString& DestPackage)
	{
		TArray<FAssetData> Existing;
		Registry.GetAssetsByPackageName(FName(*DestPackage), Existing);
		return Existing.Num() > 0;
	}

	TSharedPtr<FJsonObject> UAL_MoveItemJson(const FUAL_MoveItem& Item)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("source"), Item.SourcePackage.IsEmpty() ? Item.SourceInput : Item.SourcePackage);
		if (!Item.DestPackage.IsEmpty())
		{
			Obj->SetStringField(TEXT("destination"), Item.DestPackage);
		}
		if (!Item.ClassName.IsEmpty())
		{
			Obj->SetStringField(TEXT("class"), Item.ClassName);
		}
		Obj->SetNumberField(TEXT("referencers"), Item.ReferencerCount);
		Obj->SetStringField(TEXT("status"), Item.Status);
		if (!Item.Error.IsEmpty())
		{
			Obj->SetStringField(TEXT("error"), Item.Error);
		}
		if (Item.bAutoRenamed)
		{
			Obj->SetBoolField(TEXT("auto_renamed"), true);
		}
		if (!Item.File.IsEmpty())
		{
			Obj->SetStringField(TEXT("file"), Item.File);
			Obj->SetNumberField(TEXT("bytes"), (double)Item.Bytes);
			Obj->SetStringField(TEXT("mtime"), Item.MTime);
		}
		return Obj;
	}

	/**
	 * 签出预检的输入：每个源按 source，每个引用者按 referencer 并记下它是为了哪些源
	 * 进来的。同一个包既是源又是别的源的引用者时保留 source 身份，只补 for。
	 */
	TArray<FUAL_PackageWriteState> UAL_BuildPreflightInput(const TArray<FUAL_MoveItem>& Items)
	{
		TArray<FUAL_PackageWriteState> In;
		TMap<FString, int32> IndexByPackage;
		for (const FUAL_MoveItem& Item : Items)
		{
			if (Item.Status != TEXT("planned"))
			{
				continue;
			}
			FUAL_PackageWriteState State;
			State.Package = Item.SourcePackage;
			State.Role = TEXT("source");
			IndexByPackage.Add(Item.SourcePackage.ToLower(), In.Add(State));
		}
		for (const FUAL_MoveItem& Item : Items)
		{
			if (Item.Status != TEXT("planned"))
			{
				continue;
			}
			for (const FString& Referencer : Item.Referencers)
			{
				if (int32* Existing = IndexByPackage.Find(Referencer.ToLower()))
				{
					In[*Existing].For.AddUnique(Item.SourcePackage);
					continue;
				}
				FUAL_PackageWriteState State;
				State.Package = Referencer;
				State.Role = TEXT("referencer");
				State.For.Add(Item.SourcePackage);
				IndexByPackage.Add(Referencer.ToLower(), In.Add(State));
			}
		}
		return In;
	}

	/** 搬完落盘的文件在哪、多大、什么时候写的。资产和关卡的扩展名不同，两种都试 */
	void UAL_FillFileFingerprint(FUAL_MoveItem& Item)
	{
		FString Filename;
		if (!UAL_PackageFilename(Item.DestPackage, Filename))
		{
			return;
		}
		Item.File = FPaths::ConvertRelativePathToFull(Filename);
		Item.Bytes = IFileManager::Get().FileSize(*Filename);
		Item.MTime = IFileManager::Get().GetTimeStamp(*Filename).ToIso8601();
	}
}

void FUAL_ContentOrganizeCommands::Handle_BatchMove(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const double StartTime = FPlatformTime::Seconds();

	bool bDryRun = false;
	Payload->TryGetBoolField(TEXT("dry_run"), bDryRun);

	FString OnConflict = TEXT("fail");
	Payload->TryGetStringField(TEXT("on_conflict"), OnConflict);
	OnConflict = OnConflict.ToLower();
	if (OnConflict != TEXT("fail") && OnConflict != TEXT("skip") && OnConflict != TEXT("auto_rename"))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("on_conflict must be one of: fail, skip, auto_rename"));
		return;
	}

	bool bFixupRedirectors = true;
	Payload->TryGetBoolField(TEXT("fixup_redirectors"), bFixupRedirectors);

	bool bSave = true;
	Payload->TryGetBoolField(TEXT("save"), bSave);

	// 两道闸默认 fail：有问题就不动手。代价是「AI 更容易说不行」，收益是
	// 「AI 不会悄悄做半件事」
	FString OnBlocked = TEXT("fail");
	Payload->TryGetStringField(TEXT("on_blocked"), OnBlocked);
	OnBlocked = OnBlocked.ToLower();
	if (OnBlocked != TEXT("fail") && OnBlocked != TEXT("proceed"))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("on_blocked must be 'fail' or 'proceed'"));
		return;
	}
	FString OnCdoRefs = TEXT("fail");
	Payload->TryGetStringField(TEXT("on_cdo_refs"), OnCdoRefs);
	OnCdoRefs = OnCdoRefs.ToLower();
	if (OnCdoRefs != TEXT("fail") && OnCdoRefs != TEXT("proceed"))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("on_cdo_refs must be 'fail' or 'proceed'"));
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* MovesArray = nullptr;
	Payload->TryGetArrayField(TEXT("moves"), MovesArray);
	const TArray<TSharedPtr<FJsonValue>>* FolderMovesArray = nullptr;
	Payload->TryGetArrayField(TEXT("folder_moves"), FolderMovesArray);

	const int32 MoveInputs = MovesArray ? MovesArray->Num() : 0;
	const int32 FolderInputs = FolderMovesArray ? FolderMovesArray->Num() : 0;
	if (MoveInputs + FolderInputs == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Provide a non-empty 'moves' and/or 'folder_moves' array"));
		return;
	}

	if (!FUAL_RegistryReady::Ensure(Payload, RequestId))
	{
		return;
	}
	IAssetRegistry& Registry = UAL_Registry();

	TArray<FUAL_MoveItem> Items;
	TArray<FString> Notes;

	// ── 1. 展开输入成逐资产的计划 ─────────────────────────────────────────
	auto AddAssetMove = [&](const FString& SourceInput, const FString& DestFolder, const FString& DestName)
	{
		FUAL_MoveItem Item;
		Item.SourceInput = SourceInput;
		if (!UAL_ResolveAsset(Registry, SourceInput, Item.Source))
		{
			Item.Status = TEXT("error");
			Item.Error = TEXT("Source asset not found");
			Items.Add(Item);
			return;
		}
		Item.SourcePackage = Item.Source.PackageName.ToString();
		Item.ClassName = UAL_ClassNameOf(Item.Source);
		Item.DestFolder = DestFolder;
		Item.DestName = DestName.IsEmpty() ? Item.Source.AssetName.ToString() : DestName;
		Item.DestPackage = Item.DestFolder / Item.DestName;

		if (Item.ClassName == TEXT("ObjectRedirector"))
		{
			Item.Status = TEXT("error");
			Item.Error = TEXT("Source is a redirector, not an asset. Run content.fixup_redirectors first, then move the real asset.");
		}
		else if (Item.ClassName == TEXT("World"))
		{
			// 关卡改名要连 __ExternalActors__ / __ExternalObjects__ 一起搬，RenameAssets
			// 不管这个；搬完 World Partition 关卡会丢 Actor。明确拒绝，不装作能做。
			Item.Status = TEXT("error");
			Item.Error = TEXT("Levels (World) are not moved by this command - renaming a level must also relocate its external actor packages. Use the level tools.");
		}
		else if (Item.DestPackage.Equals(Item.SourcePackage, ESearchCase::CaseSensitive))
		{
			Item.Status = TEXT("skipped");
			Item.Error = TEXT("Destination equals source");
		}
		else if (!Item.DestFolder.StartsWith(TEXT("/")) || Item.DestFolder.Contains(TEXT(".")))
		{
			Item.Status = TEXT("error");
			Item.Error = TEXT("Destination must be a package path like /Game/Folder/Name");
		}
		else
		{
			UAL_CollectReferencers(Registry, Item.Source.PackageName, Item.Referencers);
			Item.ReferencerCount = Item.Referencers.Num();
		}
		Items.Add(Item);
	};

	if (MovesArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *MovesArray)
		{
			const TSharedPtr<FJsonObject>* MoveObj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(MoveObj) || !MoveObj || !MoveObj->IsValid())
			{
				continue;
			}
			FString Source, Destination;
			(*MoveObj)->TryGetStringField(TEXT("source"), Source);
			(*MoveObj)->TryGetStringField(TEXT("destination"), Destination);
			if (Source.IsEmpty() || Destination.IsEmpty())
			{
				FUAL_MoveItem Item;
				Item.SourceInput = Source;
				Item.Status = TEXT("error");
				Item.Error = TEXT("Each move needs both 'source' and 'destination'");
				Items.Add(Item);
				continue;
			}

			// destination 以 / 结尾、或者本身是一个已存在的目录 → 保留原名搬进去；
			// 否则最后一段就是新名字
			const bool bTrailingSlash = Destination.EndsWith(TEXT("/"));
			const FString DestPackage = UAL_ToPackageName(Destination);
			if (bTrailingSlash || UAL_IsFolder(Registry, DestPackage))
			{
				AddAssetMove(Source, DestPackage, FString());
			}
			else
			{
				AddAssetMove(Source, FPackageName::GetLongPackagePath(DestPackage), FPackageName::GetShortName(DestPackage));
			}
		}
	}

	if (FolderMovesArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *FolderMovesArray)
		{
			const TSharedPtr<FJsonObject>* MoveObj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(MoveObj) || !MoveObj || !MoveObj->IsValid())
			{
				continue;
			}
			FString SourceFolder, DestFolder;
			(*MoveObj)->TryGetStringField(TEXT("source_folder"), SourceFolder);
			(*MoveObj)->TryGetStringField(TEXT("destination_folder"), DestFolder);
			SourceFolder = UAL_ToPackageName(SourceFolder);
			DestFolder = UAL_ToPackageName(DestFolder);
			if (SourceFolder.IsEmpty() || DestFolder.IsEmpty())
			{
				Notes.Add(TEXT("A folder_moves entry was missing source_folder or destination_folder and was ignored."));
				continue;
			}
			if (!Registry.PathExists(SourceFolder))
			{
				FUAL_MoveItem Item;
				Item.SourceInput = SourceFolder;
				Item.Status = TEXT("error");
				Item.Error = TEXT("Source folder does not exist in the asset registry");
				Items.Add(Item);
				continue;
			}
			if (UAL_IsUnderFolder(DestFolder, SourceFolder))
			{
				FUAL_MoveItem Item;
				Item.SourceInput = SourceFolder;
				Item.Status = TEXT("error");
				Item.Error = TEXT("destination_folder is inside source_folder");
				Items.Add(Item);
				continue;
			}

			TArray<FAssetData> InFolder;
			Registry.GetAssetsByPath(FName(*SourceFolder), InFolder, /*bRecursive=*/true);
			if (InFolder.Num() == 0)
			{
				Notes.Add(FString::Printf(TEXT("%s contains no assets - nothing to move (empty folders are not moved)."), *SourceFolder));
				continue;
			}
			for (const FAssetData& AssetData : InFolder)
			{
				const FString PackagePath = AssetData.PackagePath.ToString();
				FString Relative = PackagePath.Mid(SourceFolder.Len());
				while (Relative.StartsWith(TEXT("/")))
				{
					Relative.RightChopInline(1);
				}
				const FString TargetFolder = Relative.IsEmpty() ? DestFolder : DestFolder / Relative;
				AddAssetMove(AssetData.PackageName.ToString(), TargetFolder, FString());
			}
		}
	}

	// ── 2. 冲突：目标已存在、或者同一批里两个搬到同一个名字 ─────────────────
	TArray<TSharedPtr<FJsonValue>> ConflictsJson;
	TSet<FString> PlannedTargets;
	for (FUAL_MoveItem& Item : Items)
	{
		if (Item.Status != TEXT("planned"))
		{
			continue;
		}
		auto Occupied = [&](const FString& Package) -> bool
		{
			const FString Lower = Package.ToLower();
			if (PlannedTargets.Contains(Lower))
			{
				return true;
			}
			// 目标被同一批里要搬走的资产占着也算冲突：RenameAssets 不保证顺序，而且
			// 搬走后原位会留一个重定向器，名字照样被占着。A→B、B→C 这类链式搬迁分两次调。
			return UAL_DestinationOccupied(Registry, Package);
		};

		if (Occupied(Item.DestPackage))
		{
			if (OnConflict == TEXT("auto_rename"))
			{
				const FString BaseName = Item.DestName;
				int32 Suffix = 1;
				while (Occupied(Item.DestFolder / FString::Printf(TEXT("%s_%d"), *BaseName, Suffix)) && Suffix < 1000)
				{
					++Suffix;
				}
				Item.DestName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix);
				Item.DestPackage = Item.DestFolder / Item.DestName;
				Item.bAutoRenamed = true;
			}
			else
			{
				TSharedPtr<FJsonObject> Conflict = MakeShared<FJsonObject>();
				Conflict->SetStringField(TEXT("source"), Item.SourcePackage);
				Conflict->SetStringField(TEXT("destination"), Item.DestPackage);
				ConflictsJson.Add(MakeShared<FJsonValueObject>(Conflict));
				Item.Status = TEXT("skipped");
				Item.Error = TEXT("Destination already exists");
			}
		}
		if (Item.Status == TEXT("planned"))
		{
			PlannedTargets.Add(Item.DestPackage.ToLower());
		}
	}

	int32 PlannedCount = 0;
	int32 SkippedCount = 0;
	int32 ErrorCount = 0;
	for (const FUAL_MoveItem& Item : Items)
	{
		if (Item.Status == TEXT("planned")) ++PlannedCount;
		else if (Item.Status == TEXT("skipped")) ++SkippedCount;
		else ++ErrorCount;
	}

	// ── 2b. 两道预检：签出 / CDO 引用 ────────────────────────────────────
	//
	// 都只读注册表和内存里已有的 CDO，**不加载资产**，dry_run 的承诺不变。
	// dry_run 和执行都带：dry_run 要让人事前看见，执行时它就是闸。
	FUAL_PreflightResult Preflight;
	TArray<FUAL_CdoRef> CdoRefs;
	TArray<FString> PlannedSources;
	if (PlannedCount > 0)
	{
		UAL_PreflightPackages(UAL_BuildPreflightInput(Items), Preflight);
		for (const FUAL_MoveItem& Item : Items)
		{
			if (Item.Status == TEXT("planned"))
			{
				PlannedSources.Add(Item.SourcePackage);
			}
		}
		UAL_FindCdoReferences(PlannedSources, CdoRefs);
	}

	auto BuildItemsJson = [&]() -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		// 最多列 500 条；几千个资产的整目录搬迁把每条都塞进响应只会撑爆上下文
		for (const FUAL_MoveItem& Item : Items)
		{
			if (Out.Num() >= 500)
			{
				break;
			}
			Out.Add(MakeShared<FJsonValueObject>(UAL_MoveItemJson(Item)));
		}
		return Out;
	};

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetNumberField(TEXT("planned"), PlannedCount);
	Result->SetNumberField(TEXT("skipped"), SkippedCount);
	Result->SetNumberField(TEXT("errors"), ErrorCount);
	Result->SetArrayField(TEXT("conflicts"), ConflictsJson);
	Result->SetStringField(TEXT("on_conflict"), OnConflict);
	if (PlannedCount > 0)
	{
		Result->SetObjectField(TEXT("checkout"), UAL_PreflightJson(Preflight));
		Result->SetObjectField(TEXT("cdo_refs"), UAL_CdoRefsJson(PlannedSources.Num(), CdoRefs));
	}

	// on_conflict=fail：有冲突就整批不动。半批搬完的工程比没搬更难收拾。
	if (OnConflict == TEXT("fail") && ConflictsJson.Num() > 0 && !bDryRun)
	{
		Result->SetBoolField(TEXT("ok"), false);
		Result->SetNumberField(TEXT("moved"), 0);
		Result->SetArrayField(TEXT("items"), BuildItemsJson());
		Result->SetStringField(TEXT("error"),
			FString::Printf(TEXT("%d destination(s) already exist. Nothing was moved. Re-run with on_conflict=skip or auto_rename, or choose different names."), ConflictsJson.Num()));
		UAL_CommandUtils::SendResponse(RequestId, 409, Result);
		return;
	}

	if (bDryRun || PlannedCount == 0)
	{
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetNumberField(TEXT("moved"), 0);
		Result->SetArrayField(TEXT("items"), BuildItemsJson());
		Result->SetBoolField(TEXT("items_truncated"), Items.Num() > 500);
		Result->SetNumberField(TEXT("elapsed_ms"), (FPlatformTime::Seconds() - StartTime) * 1000.0);
		if (bDryRun)
		{
			Notes.Add(TEXT("Dry run - nothing was changed. Re-run with dry_run=false to move."));
		}
		else
		{
			Notes.Add(TEXT("Nothing to move after resolving inputs."));
		}
		Result->SetArrayField(TEXT("notes"), UAL_StringArray(Notes));
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	// ── 3. 闸：先签出、再 CDO。有阻塞就一个都不发给 RenameAssets ─────────
	//
	// 引擎自己也会因为这两件事整批中止，区别是它把原因藏在日志里（签出），
	// 或者干脆不留痕迹（CDO 弹窗被静默取消）。在这里拦下来，原因就在响应里；
	// 而且这一处闸绕不过去，回滚复用 batch_move 时自动带上。
	auto RespondBlocked = [&](const TCHAR* Reason, const FString& Note)
	{
		Result->SetBoolField(TEXT("ok"), false);
		Result->SetStringField(TEXT("reason"), Reason);
		Result->SetNumberField(TEXT("moved"), 0);
		Result->SetNumberField(TEXT("failed"), 0);
		Result->SetArrayField(TEXT("items"), BuildItemsJson());
		Result->SetBoolField(TEXT("items_truncated"), Items.Num() > 500);
		Result->SetNumberField(TEXT("elapsed_ms"), (FPlatformTime::Seconds() - StartTime) * 1000.0);
		Notes.Add(Note);
		Result->SetArrayField(TEXT("notes"), UAL_StringArray(Notes));
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
	};
	if (Preflight.BlockedCount > 0 && OnBlocked == TEXT("fail"))
	{
		RespondBlocked(TEXT("checkout_blocked"), FString::Printf(
			TEXT("%d of %d package(s) cannot be written (checkout.blocking). RenameAssets checks out the moved assets and every referencer, and one failure aborts the whole batch - so nothing was sent to it. Check out / sync / clear read-only first, or pass on_blocked=proceed to let the engine try anyway."),
			Preflight.BlockedCount, Preflight.States.Num()));
		return;
	}
	if (CdoRefs.Num() > 0 && OnCdoRefs == TEXT("fail"))
	{
		RespondBlocked(TEXT("cdo_referenced"), FString::Printf(
			TEXT("%d reference(s) from native C++ class defaults (cdo_refs.hits). The engine asks for confirmation before renaming such assets, and an unattended rename silently cancels the whole batch - so nothing was sent to it. Fix the code/config paths first, or pass on_cdo_refs=proceed to have the dialog answered on your behalf."),
			CdoRefs.Num()));
		return;
	}

	// ── 4. 执行：一次 RenameAssets ───────────────────────────────────────
	// 无人值守：签出 / 覆盖之类的模态框没人点，编辑器会卡死在那儿。
	// 例外是 on_cdo_refs=proceed 且真有命中：CDO 确认框在无人值守下会被静默取消
	// （默认值 Cancel），要「继续」只能替用户回答 —— 见 UAL_ScopedDialogAutoAnswer.h
	TUniquePtr<TGuardValue<bool>> UnattendedGuard;
	TUniquePtr<FUAL_ScopedDialogAutoAnswer> DialogAnswer;
	if (CdoRefs.Num() > 0 && OnCdoRefs == TEXT("proceed"))
	{
		DialogAnswer = MakeUnique<FUAL_ScopedDialogAutoAnswer>();
		if (!FUAL_ScopedDialogAutoAnswer::CanAnswer())
		{
			Notes.Add(TEXT("on_cdo_refs=proceed: this editor runs with -unattended, so FMessageDialog never consults the dialog delegate and the CDO confirmation cannot be answered - expect the engine to cancel the batch."));
		}
	}
	else
	{
		UnattendedGuard = MakeUnique<TGuardValue<bool>>(GIsRunningUnattendedScript, true);
	}

	const TSet<FName> DirtyBefore = UAL_DirtyPackageNames();

	TArray<FAssetRenameData> RenameData;
	RenameData.Reserve(PlannedCount);
	for (FUAL_MoveItem& Item : Items)
	{
		if (Item.Status != TEXT("planned"))
		{
			continue;
		}
		UObject* Object = Item.Source.GetAsset();
		if (!Object)
		{
			Item.Status = TEXT("failed");
			Item.Error = TEXT("Failed to load source asset");
			continue;
		}
		RenameData.Add(FAssetRenameData(Object, Item.DestFolder, Item.DestName));
	}

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	IAssetTools& AssetTools = AssetToolsModule.Get();

	// 引擎把失败原因写进日志而不是返回值：签不出、只读、不是最新版，每一条
	// ReportFailures 都只 UE_LOG(LogAssetTools, Error, "{包名} - {原因}")。
	// 改名 + 清重定向器 + 落盘这一段全收，Error 行拆开回填到 items[].error
	TUniquePtr<FUAL_ScopedLogCapture> LogCapture = MakeUnique<FUAL_ScopedLogCapture>(
		std::initializer_list<const TCHAR*>{ TEXT("LogAssetTools"), TEXT("LogSourceControl") }, ELogVerbosity::Warning);

	bool bRenameReported = false;
	if (RenameData.Num() > 0)
	{
		bRenameReported = AssetTools.RenameAssets(RenameData);
	}

	// ── 5. 回读：目标位置到底有没有 ───────────────────────────────────────
	int32 MovedCount = 0;
	int32 FailedCount = 0;
	TArray<FString> OldPackages;
	TArray<FString> NewPackages;
	for (FUAL_MoveItem& Item : Items)
	{
		if (Item.Status != TEXT("planned"))
		{
			if (Item.Status == TEXT("failed")) ++FailedCount;
			continue;
		}
		const FAssetData Moved = UAL_AssetByObjectPath(Registry, Item.DestPackage + TEXT(".") + Item.DestName);
		if (Moved.IsValid() && UAL_ClassNameOf(Moved) != TEXT("ObjectRedirector"))
		{
			Item.Status = TEXT("moved");
			++MovedCount;
			OldPackages.Add(Item.SourcePackage);
			NewPackages.Add(Item.DestPackage);
		}
		else
		{
			Item.Status = TEXT("failed");
			Item.Error = bRenameReported
				? TEXT("RenameAssets reported success but the asset is not at the destination")
				: TEXT("RenameAssets failed (see editor log / message log)");
			++FailedCount;
		}
	}

	// 引擎的失败原因：ReportFailures 打的 Error 行是 "{包名} - {原因}"，按包名回填。
	// 只认 Error 行 —— AutoCheckOut 有一条 Warning 是签出**成功**时才打的（守卫写反了，
	// 九版都这样），「有 Warning 就是失败」不成立
	if (FailedCount > 0)
	{
		for (const FString& ErrorLine : LogCapture->ErrorsOnly())
		{
			FString Package, Reason;
			if (!ErrorLine.Split(TEXT(" - "), &Package, &Reason))
			{
				continue;
			}
			Package.TrimStartAndEndInline();
			Reason.TrimStartAndEndInline();
			for (FUAL_MoveItem& Item : Items)
			{
				if (Item.Status == TEXT("failed") && Item.SourcePackage.Equals(Package, ESearchCase::IgnoreCase))
				{
					Item.Error = Reason;
				}
			}
		}
	}

	// ── 6. 只数这一批留下的重定向器，**不清理** ──────────────────────────
	//
	// ## 为什么这一步只数不做：`FixupReferencers` 在无人值守下必崩
	//
	// 真机崩溃三次（2026-09-03 UE 5.5 一次 1 条；2026-09-09 UE 5.8 两次，
	// 一次 22 条一次上百条）。前两次的结论写的是「版本控制签出弹框惹的祸」，
	// **那个结论是错的** —— 第三次工程没开版本控制，照崩。
	//
	// 真正的原因在引擎源码里，和签出、只读、版本控制**都无关**：
	// `AssetFixUpRedirectors.cpp` 的 `ExecuteFixUp` 收尾时**无条件**弹一个
	// 「Redirector Update Report」模态框（5.8 是第 938–939 行）：
	//
	//     TSharedPtr<SFixupRedirectorsReport> Dialog = SNew(SFixupRedirectorsReport, ...);
	//     const bool bDoDelete = Dialog->ShowModalDialog(LOCTEXT("RedirectorUpdateReport", ...));
	//
	// 而 `SModalEditorDialog::ShowModalDialog`（`Dialogs.h:273-281`）是这么写的：
	//
	//     TOptional<ResultType> Result;
	//     OnFinished.BindLambda([&Result](ResultType In){ Result.Emplace(...); });
	//     UE::Private::ShowModalDialogWindow(Window.ToSharedRef());   // → EditorAddModalWindow
	//     return MoveTemp(Result.GetValue());                        // ← 这里
	//
	// 无人值守下 `AddModalWindow` 直接取消窗口（打那行 LogSlate Warning）、
	// `OnFinished` 从不触发 → `Result` 一直没被设置 → `GetValue()` 断言 →
	// **编辑器当场崩掉**。`bCheckoutDialogPrompt=false` 管不到它，它在那个分支之外。
	//
	// 这个报告框是 **5.4 引进的**（本机九版核过：5.0–5.3 的
	// `AssetFixUpRedirectors.cpp` 里 `SFixupRedirectorsReport` 零命中，5.4–5.8 各 16 处）。
	// 也就是说 5.4 起 `IAssetTools::FixupReferencers` 在无人值守下**一调必崩**，
	// 和参数、和工程配置都无关 —— 这解释了为什么它「稳定复现」。
	//
	// 三条补救路都不通：替答（那是 Slate 原生窗口，不走 `FMessageDialog` 委托）、
	// 事前预检（崩因和文件状态无关，预检看什么都拦不住）、事后判断（崩了才知道）。
	// 所以这一步**不做**，只把留下的重定向器数出来、报给调用方。
	//
	// 留下重定向器是**安全**的：引擎本来就靠它转发旧路径，工程照常能用。
	// 要清理走 `content.fixup_redirectors` —— 那条命令不套无人值守保护，
	// 报告框会真的弹出来，由坐在编辑器前的人点一下（见那边的长注释）。
	int32 RedirectorsFound = 0;
	const int32 RedirectorsFixed = 0;
	TArray<FString> RedirectorsLeft;
	if (MovedCount > 0)
	{
		for (const FString& OldPackage : OldPackages)
		{
			TArray<FAssetData> AtOld;
			Registry.GetAssetsByPackageName(FName(*OldPackage), AtOld);
			for (const FAssetData& Data : AtOld)
			{
				if (UAL_ClassNameOf(Data) == TEXT("ObjectRedirector"))
				{
					++RedirectorsFound;
					if (RedirectorsLeft.Num() < 200)
					{
						RedirectorsLeft.Add(OldPackage);
					}
				}
			}
		}
	}

	// ── 7. 落盘：搬过去的包 + 这条命令改脏的引用者 ────────────────────────
	int32 SavedCount = 0;
	TArray<FString> SaveFailed;
	if (bSave)
	{
		TSet<UPackage*> ToSave;
		for (const FString& NewPackage : NewPackages)
		{
			if (UPackage* Package = FindPackage(nullptr, *NewPackage))
			{
				if (Package->IsDirty())
				{
					ToSave.Add(Package);
				}
			}
		}
		for (UPackage* Package : UAL_AllDirtySavablePackages())
		{
			// 搬迁之前就脏的不碰：那是用户自己改到一半的东西
			if (!DirtyBefore.Contains(Package->GetFName()))
			{
				ToSave.Add(Package);
			}
		}
		for (UPackage* Package : ToSave)
		{
			if (UAL_SavePackage(Package))
			{
				++SavedCount;
			}
			else
			{
				SaveFailed.Add(Package->GetName());
			}
		}
	}

	// 到这里引擎该打的日志都打完了，收口；unattended 标志和弹窗委托也还回去
	const TArray<TSharedPtr<FJsonValue>> EngineLog = LogCapture->ToJson();
	LogCapture.Reset();
	UnattendedGuard.Reset();
	TArray<TSharedPtr<FJsonValue>> AutoAnswered;
	if (DialogAnswer)
	{
		AutoAnswered = DialogAnswer->ToJson();
		DialogAnswer.Reset();
	}

	// 落盘后的指纹（file / bytes / mtime）。RenameAssets 自己就会存搬过去的包
	// （PromptForCheckoutAndSave，bPromptToSave=false），所以只看磁盘上有没有；
	// save=false 不回 —— 那种账本记 saved:false，不可自动回滚
	if (bSave)
	{
		for (FUAL_MoveItem& Item : Items)
		{
			if (Item.Status == TEXT("moved"))
			{
				UAL_FillFileFingerprint(Item);
			}
		}
	}

	const int32 DirtyAfter = UAL_DirtyPackageNames().Num();

	Result->SetBoolField(TEXT("ok"), MovedCount > 0 && FailedCount == 0);
	Result->SetNumberField(TEXT("moved"), MovedCount);
	Result->SetNumberField(TEXT("failed"), FailedCount);
	Result->SetArrayField(TEXT("items"), BuildItemsJson());
	Result->SetBoolField(TEXT("items_truncated"), Items.Num() > 500);
	Result->SetNumberField(TEXT("redirectors_found"), RedirectorsFound);
	Result->SetNumberField(TEXT("redirectors_fixed"), RedirectorsFixed);
	Result->SetNumberField(TEXT("saved_count"), SavedCount);
	if (SaveFailed.Num() > 0)
	{
		Result->SetArrayField(TEXT("save_failed"), UAL_StringArray(SaveFailed));
	}
	Result->SetNumberField(TEXT("dirty_after"), DirtyAfter);
	Result->SetNumberField(TEXT("elapsed_ms"), (FPlatformTime::Seconds() - StartTime) * 1000.0);
	Result->SetArrayField(TEXT("engine_log"), EngineLog);
	if (OnCdoRefs == TEXT("proceed") && CdoRefs.Num() > 0)
	{
		Result->SetArrayField(TEXT("auto_answered_dialogs"), AutoAnswered);
		Notes.Add(FString::Printf(TEXT("on_cdo_refs=proceed: answered %d engine dialog(s) on your behalf (auto_answered_dialogs). Code/config paths that pointed at the old names still need a Find/Replace."), AutoAnswered.Num()));
	}
	if (Preflight.BlockedCount > 0 && OnBlocked == TEXT("proceed"))
	{
		Notes.Add(FString::Printf(TEXT("on_blocked=proceed: %d package(s) were flagged as not writable (checkout.blocking) and the engine was asked to try anyway; see engine_log for what it did."), Preflight.BlockedCount));
	}

	if (FailedCount > 0)
	{
		Notes.Add(FString::Printf(TEXT("%d asset(s) failed to move; see items[].error and engine_log."), FailedCount));
	}
	if (RedirectorsFound > 0)
	{
		Result->SetArrayField(TEXT("redirectors_left"), UAL_StringArray(RedirectorsLeft));
		Notes.Add(FString::Printf(
			TEXT("%d redirector(s) were left at the old paths ON PURPOSE - this command never cleans them up. Reason: IAssetTools::FixupReferencers always opens a modal 'Redirector Update Report' window on UE 5.4+, and in unattended mode that window is cancelled while the engine reads its unset TOptional result - which CRASHES the editor (reproduced on 5.5 and 5.8, unrelated to revision control). Redirectors are harmless: the engine follows them and the project works normally. To remove them, run content.fixup_redirectors - that command runs interactively and the report window really pops up for a human to click."),
			RedirectorsFound));
	}
	if (!bFixupRedirectors && MovedCount > 0)
	{
		Notes.Add(TEXT("fixup_redirectors is accepted for compatibility but no longer does anything here (see the redirector note above)."));
	}
	if (!bSave && MovedCount > 0)
	{
		Notes.Add(TEXT("save=false: rewritten referencers are still unsaved - call editor.save."));
	}
	else if (DirtyAfter > 0)
	{
		Notes.Add(FString::Printf(TEXT("%d package(s) are still dirty (modified outside this command, left alone)."), DirtyAfter));
	}
	Result->SetArrayField(TEXT("notes"), UAL_StringArray(Notes));

	UE_LOG(LogUALOrganize, Log, TEXT("content.batch_move: planned=%d moved=%d failed=%d skipped=%d redirectors %d/%d saved=%d in %.0fms"),
		PlannedCount, MovedCount, FailedCount, SkippedCount, RedirectorsFixed, RedirectorsFound, SavedCount,
		(FPlatformTime::Seconds() - StartTime) * 1000.0);

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// content.dependencies
// ============================================================================
//
// content.describe 只给一层直接依赖；content.size_map 给闭包但只关心体积。
// 这里回答的是整理工程时真正要问的三件事：
//
//   1. 这个目录搬走 / 删掉，**外面**谁会断（external_referencers）
//   2. 这个目录要迁到别的工程，**外面**得跟着带哪些（external_dependencies）
//   3. 哪些引用指向了根本不存在的包（missing）—— 这是「加载时报 Failed to load」的根因
//
// 全程只读注册表，一个资产都不加载。

namespace
{
	struct FUAL_DepNode
	{
		FString Package;
		FString Class;
		int32 Depth = 0;
		int64 DiskSize = 0;
		bool bExternal = false;
	};

	struct FUAL_DepWalk
	{
		TArray<FUAL_DepNode> Nodes;
		TArray<FString> Missing;
		TArray<TPair<FString, FString>> Edges;
		int32 MaxDepthReached = 0;
		bool bTruncated = false;
		TMap<FString, int32> ByClass;
		int64 TotalSize = 0;
		int32 ExternalCount = 0;
	};

	/**
	 * 从根集合出发做 BFS。bReferencers=false 走依赖，true 走被引用。
	 * 根本身不进结果。
	 */
	FUAL_DepWalk UAL_WalkGraph(
		IAssetRegistry& Registry,
		const TArray<FName>& Roots,
		const FString& ScopeFolder,
		bool bReferencers,
		bool bRecursive,
		int32 MaxDepth,
		bool bHardOnly,
		bool bIncludeEngine,
		int32 MaxNodes,
		bool bIncludeEdges)
	{
		FUAL_DepWalk Walk;
		TSet<FName> Visited;
		for (const FName& Root : Roots)
		{
			Visited.Add(Root);
		}

		const UE::AssetRegistry::FDependencyQuery Query = bHardOnly
			? UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard)
			: UE::AssetRegistry::FDependencyQuery();

		TArray<TPair<FName, int32>> Frontier;
		for (const FName& Root : Roots)
		{
			Frontier.Add(TPair<FName, int32>(Root, 0));
		}

		int32 Cursor = 0;
		while (Cursor < Frontier.Num())
		{
			const FName Current = Frontier[Cursor].Key;
			const int32 Depth = Frontier[Cursor].Value;
			++Cursor;

			if (Depth >= MaxDepth || (!bRecursive && Depth >= 1))
			{
				continue;
			}

			TArray<FName> Next;
			if (bReferencers)
			{
				Registry.GetReferencers(Current, Next, UE::AssetRegistry::EDependencyCategory::Package, Query);
			}
			else
			{
				Registry.GetDependencies(Current, Next, UE::AssetRegistry::EDependencyCategory::Package, Query);
			}

			for (const FName& NextName : Next)
			{
				const FString NextStr = NextName.ToString();
				if (NextStr.StartsWith(TEXT("/Script/")))
				{
					continue;
				}
				if (!bIncludeEngine && NextStr.StartsWith(TEXT("/Engine/")))
				{
					continue;
				}
				if (bIncludeEdges)
				{
					Walk.Edges.Add(TPair<FString, FString>(Current.ToString(), NextStr));
				}
				if (Visited.Contains(NextName))
				{
					continue;
				}
				Visited.Add(NextName);

				if (Walk.Nodes.Num() >= MaxNodes)
				{
					Walk.bTruncated = true;
					continue;
				}

				TArray<FAssetData> InPackage;
				Registry.GetAssetsByPackageName(NextName, InPackage);
				if (InPackage.Num() == 0)
				{
					// 依赖走的时候，指向一个注册表里没有的包 = 断链。
					// 被引用方向不会出现这种情况（引用者自己就是注册表里的包）。
					if (!bReferencers)
					{
						Walk.Missing.Add(NextStr);
					}
					continue;
				}

				FUAL_DepNode Node;
				Node.Package = NextStr;
				Node.Class = UAL_ClassNameOf(InPackage[0]);
				Node.Depth = Depth + 1;
				Node.DiskSize = UAL_OrganizePackageFileSize(NextStr);
				Node.bExternal = !ScopeFolder.IsEmpty() && !UAL_IsUnderFolder(NextStr, ScopeFolder);
				Walk.MaxDepthReached = FMath::Max(Walk.MaxDepthReached, Node.Depth);
				Walk.TotalSize += Node.DiskSize;
				Walk.ByClass.FindOrAdd(Node.Class)++;
				if (Node.bExternal)
				{
					++Walk.ExternalCount;
				}
				Walk.Nodes.Add(Node);
				Frontier.Add(TPair<FName, int32>(NextName, Depth + 1));
			}
		}
		return Walk;
	}

	TSharedPtr<FJsonObject> UAL_WalkJson(const FUAL_DepWalk& Walk, int32 Limit, bool bIncludeEdges, const TCHAR* ExternalKey)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("count"), Walk.Nodes.Num());
		Obj->SetNumberField(TEXT("total_disk_size"), (double)Walk.TotalSize);
		Obj->SetNumberField(TEXT("max_depth_reached"), Walk.MaxDepthReached);
		Obj->SetBoolField(TEXT("truncated"), Walk.bTruncated);
		Obj->SetObjectField(TEXT("by_class"), UAL_CountMap(Walk.ByClass));

		// 按深度、再按体积排：先看直接的、大的
		TArray<const FUAL_DepNode*> Sorted;
		Sorted.Reserve(Walk.Nodes.Num());
		for (const FUAL_DepNode& Node : Walk.Nodes)
		{
			Sorted.Add(&Node);
		}
		Sorted.Sort([](const FUAL_DepNode& A, const FUAL_DepNode& B)
		{
			if (A.Depth != B.Depth) return A.Depth < B.Depth;
			return A.DiskSize > B.DiskSize;
		});

		TArray<TSharedPtr<FJsonValue>> NodesJson;
		TArray<TSharedPtr<FJsonValue>> ExternalJson;
		for (const FUAL_DepNode* Node : Sorted)
		{
			TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>();
			N->SetStringField(TEXT("path"), Node->Package);
			N->SetStringField(TEXT("class"), Node->Class);
			N->SetNumberField(TEXT("depth"), Node->Depth);
			N->SetNumberField(TEXT("disk_size"), (double)Node->DiskSize);
			if (Node->bExternal)
			{
				N->SetBoolField(TEXT("external"), true);
				if (ExternalJson.Num() < Limit)
				{
					ExternalJson.Add(MakeShared<FJsonValueObject>(N));
				}
			}
			if (NodesJson.Num() < Limit)
			{
				NodesJson.Add(MakeShared<FJsonValueObject>(N));
			}
		}
		Obj->SetArrayField(TEXT("nodes"), NodesJson);
		Obj->SetBoolField(TEXT("nodes_truncated"), Walk.Nodes.Num() > NodesJson.Num());
		if (ExternalKey)
		{
			Obj->SetNumberField(FString(ExternalKey) + TEXT("_count"), Walk.ExternalCount);
			Obj->SetArrayField(ExternalKey, ExternalJson);
		}
		if (Walk.Missing.Num() > 0)
		{
			Obj->SetArrayField(TEXT("missing"), UAL_StringArray(Walk.Missing));
		}
		if (bIncludeEdges)
		{
			TArray<TSharedPtr<FJsonValue>> EdgesJson;
			for (const TPair<FString, FString>& Edge : Walk.Edges)
			{
				if (EdgesJson.Num() >= Limit * 4)
				{
					break;
				}
				TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
				E->SetStringField(TEXT("from"), Edge.Key);
				E->SetStringField(TEXT("to"), Edge.Value);
				EdgesJson.Add(MakeShared<FJsonValueObject>(E));
			}
			Obj->SetArrayField(TEXT("edges"), EdgesJson);
			Obj->SetBoolField(TEXT("edges_truncated"), Walk.Edges.Num() > EdgesJson.Num());
		}
		return Obj;
	}
}

void FUAL_ContentOrganizeCommands::Handle_Dependencies(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required parameter: path (an asset or a folder)"));
		return;
	}

	FString Direction = TEXT("dependencies");
	Payload->TryGetStringField(TEXT("direction"), Direction);
	Direction = Direction.ToLower();
	if (Direction != TEXT("dependencies") && Direction != TEXT("referencers") && Direction != TEXT("both") && Direction != TEXT("unreferenced"))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("direction must be one of: dependencies, referencers, both, unreferenced"));
		return;
	}

	bool bRecursive = true;
	Payload->TryGetBoolField(TEXT("recursive"), bRecursive);
	int32 MaxDepth = 8;
	Payload->TryGetNumberField(TEXT("max_depth"), MaxDepth);
	MaxDepth = FMath::Clamp(MaxDepth, 1, 64);
	bool bHardOnly = false;
	Payload->TryGetBoolField(TEXT("hard_only"), bHardOnly);
	bool bIncludeEngine = false;
	Payload->TryGetBoolField(TEXT("include_engine"), bIncludeEngine);
	int32 MaxNodes = 2000;
	Payload->TryGetNumberField(TEXT("max_nodes"), MaxNodes);
	MaxNodes = FMath::Clamp(MaxNodes, 1, 20000);
	int32 Limit = 200;
	Payload->TryGetNumberField(TEXT("limit"), Limit);
	Limit = FMath::Clamp(Limit, 1, 2000);
	bool bIncludeEdges = false;
	Payload->TryGetBoolField(TEXT("include_edges"), bIncludeEdges);

	if (!FUAL_RegistryReady::Ensure(Payload, RequestId))
	{
		return;
	}
	IAssetRegistry& Registry = UAL_Registry();

	TArray<FName> Roots;
	FString ScopeFolder;
	FString RootLabel;
	bool bIsFolder = false;
	int32 RootAssetCount = 0;

	if (UAL_IsFolder(Registry, Path))
	{
		bIsFolder = true;
		ScopeFolder = UAL_ToPackageName(Path);
		RootLabel = ScopeFolder;
		TArray<FAssetData> InFolder;
		Registry.GetAssetsByPath(FName(*ScopeFolder), InFolder, /*bRecursive=*/true);
		TSet<FName> Unique;
		for (const FAssetData& Data : InFolder)
		{
			Unique.Add(Data.PackageName);
		}
		Roots = Unique.Array();
		RootAssetCount = InFolder.Num();
	}
	else
	{
		FAssetData Asset;
		if (!UAL_ResolveAsset(Registry, Path, Asset))
		{
			UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Neither an asset nor a folder: %s"), *Path));
			return;
		}
		Roots.Add(Asset.PackageName);
		RootLabel = Asset.PackageName.ToString();
		RootAssetCount = 1;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("root"), RootLabel);
	Result->SetBoolField(TEXT("scope_is_folder"), bIsFolder);
	Result->SetNumberField(TEXT("root_count"), Roots.Num());
	Result->SetNumberField(TEXT("root_asset_count"), RootAssetCount);
	Result->SetStringField(TEXT("direction"), Direction);
	Result->SetBoolField(TEXT("hard_only"), bHardOnly);

	TArray<FString> Notes;

	if (Roots.Num() == 0)
	{
		Notes.Add(TEXT("The folder contains no assets."));
		Result->SetArrayField(TEXT("notes"), UAL_StringArray(Notes));
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	if (Direction == TEXT("dependencies") || Direction == TEXT("both"))
	{
		const FUAL_DepWalk Walk = UAL_WalkGraph(Registry, Roots, ScopeFolder, false, bRecursive, MaxDepth, bHardOnly, bIncludeEngine, MaxNodes, bIncludeEdges);
		Result->SetObjectField(TEXT("dependencies"), UAL_WalkJson(Walk, Limit, bIncludeEdges, bIsFolder ? TEXT("external") : nullptr));
		if (Walk.Missing.Num() > 0)
		{
			Notes.Add(FString::Printf(TEXT("%d dependency package(s) do not exist in the asset registry (dependencies.missing) - these are broken references and will fail to load."), Walk.Missing.Num()));
		}
		if (bIsFolder)
		{
			Notes.Add(FString::Printf(TEXT("%d dependency package(s) live outside %s (dependencies.external) - migrating this folder must bring them along."), Walk.ExternalCount, *ScopeFolder));
		}
		if (Walk.bTruncated)
		{
			Notes.Add(TEXT("Dependency walk stopped at max_nodes; totals are a lower bound."));
		}
	}

	if (Direction == TEXT("referencers") || Direction == TEXT("both"))
	{
		const FUAL_DepWalk Walk = UAL_WalkGraph(Registry, Roots, ScopeFolder, true, bRecursive, MaxDepth, bHardOnly, bIncludeEngine, MaxNodes, bIncludeEdges);
		Result->SetObjectField(TEXT("referencers"), UAL_WalkJson(Walk, Limit, bIncludeEdges, bIsFolder ? TEXT("external") : nullptr));
		if (bIsFolder)
		{
			Notes.Add(FString::Printf(TEXT("%d referencer package(s) live outside %s (referencers.external) - deleting this folder would break them; moving it leaves redirectors they will follow."), Walk.ExternalCount, *ScopeFolder));
		}
		else if (Walk.Nodes.Num() == 0)
		{
			Notes.Add(TEXT("Nothing references this asset - it is safe to delete as far as the asset registry knows (soft string references in code/config are not tracked)."));
		}
	}

	if (Direction == TEXT("unreferenced"))
	{
		TArray<TSharedPtr<FJsonValue>> Unreferenced;
		int64 UnreferencedBytes = 0;
		int32 UnreferencedCount = 0;
		for (const FName& Root : Roots)
		{
			TArray<FName> Refs;
			Registry.GetReferencers(Root, Refs, UE::AssetRegistry::EDependencyCategory::Package);
			bool bHasRealReferencer = false;
			for (const FName& Ref : Refs)
			{
				const FString RefStr = Ref.ToString();
				if (RefStr.StartsWith(TEXT("/Script/")))
				{
					continue;
				}
				bHasRealReferencer = true;
				break;
			}
			if (bHasRealReferencer)
			{
				continue;
			}
			TArray<FAssetData> InPackage;
			Registry.GetAssetsByPackageName(Root, InPackage);
			if (InPackage.Num() == 0)
			{
				continue;
			}
			const FString ClassName = UAL_ClassNameOf(InPackage[0]);
			// 关卡本来就没人引用；重定向器另有命令管
			if (ClassName == TEXT("World") || ClassName == TEXT("ObjectRedirector"))
			{
				continue;
			}
			++UnreferencedCount;
			const int64 Size = UAL_OrganizePackageFileSize(Root.ToString());
			UnreferencedBytes += Size;
			if (Unreferenced.Num() < Limit)
			{
				TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>();
				N->SetStringField(TEXT("path"), Root.ToString());
				N->SetStringField(TEXT("class"), ClassName);
				N->SetNumberField(TEXT("disk_size"), (double)Size);
				Unreferenced.Add(MakeShared<FJsonValueObject>(N));
			}
		}
		Result->SetArrayField(TEXT("unreferenced"), Unreferenced);
		Result->SetNumberField(TEXT("unreferenced_count"), UnreferencedCount);
		Result->SetNumberField(TEXT("unreferenced_disk_size"), (double)UnreferencedBytes);
		Result->SetBoolField(TEXT("unreferenced_truncated"), UnreferencedCount > Unreferenced.Num());
		Notes.Add(TEXT("'Unreferenced' means no other package references it in the asset registry. Assets loaded by path from code, config, or Blueprints via string (e.g. LoadObject, Asset Manager primary assets, GameMode defaults) are NOT counted - confirm before deleting."));
	}

	Result->SetArrayField(TEXT("notes"), UAL_StringArray(Notes));
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// content.migrate
// ============================================================================
//
// 编辑器右键 Migrate 做的事就是：算依赖闭包 → 把包文件按相对路径拷到目标工程的
// Content 下。IAssetTools::MigratePackages 5.1 起才有无对话框的重载，而且返回 void，
// 拷了什么、漏了什么一个字不说。这里自己拷：九个版本一份代码，每个文件都回读。
//
// 依赖闭包和 content.dependencies 用同一套注册表遍历，不加载资产。
// 插件内容（/PluginName/...）只在目标工程也装了同名插件时才拷，否则列进
// external_skipped 让调用方自己决定。/Engine 和 /Script 不拷 —— 每个工程都有。

namespace
{
	struct FUAL_MigrateFile
	{
		FString Package;
		FString SourceFile;
		FString DestFile;
		int64 Bytes = 0;
		/** planned / copied / skipped_exists / failed / external_skipped */
		FString Status = TEXT("planned");
		FString Error;
	};

	/**
	 * 把调用方给的 destination 归一成目标工程的 Content 目录。
	 * 接受：.uproject 文件路径、工程目录、Content 目录本身。
	 */
	bool UAL_ResolveDestinationContent(const FString& Input, FString& OutContentDir, FString& OutProjectDir, FString& OutError)
	{
		FString Dest = Input;
		FPaths::NormalizeDirectoryName(Dest);
		if (Dest.EndsWith(TEXT(".uproject"), ESearchCase::IgnoreCase))
		{
			if (!FPaths::FileExists(Dest))
			{
				OutError = FString::Printf(TEXT("Project file not found: %s"), *Dest);
				return false;
			}
			OutProjectDir = FPaths::GetPath(Dest);
			OutContentDir = OutProjectDir / TEXT("Content");
		}
		else if (FPaths::GetCleanFilename(Dest).Equals(TEXT("Content"), ESearchCase::IgnoreCase))
		{
			OutContentDir = Dest;
			OutProjectDir = FPaths::GetPath(Dest);
		}
		else
		{
			OutProjectDir = Dest;
			OutContentDir = Dest / TEXT("Content");
		}

		if (!FPaths::DirectoryExists(OutProjectDir))
		{
			OutError = FString::Printf(TEXT("Destination project directory does not exist: %s"), *OutProjectDir);
			return false;
		}
		// 目标得像个 UE 工程：有 .uproject 或者已经有 Content 目录。
		// 否则一个手滑就把几百个 uasset 拷进用户的下载目录。
		TArray<FString> ProjectFiles;
		IFileManager::Get().FindFiles(ProjectFiles, *(OutProjectDir / TEXT("*.uproject")), true, false);
		if (ProjectFiles.Num() == 0 && !FPaths::DirectoryExists(OutContentDir))
		{
			OutError = FString::Printf(TEXT("%s does not look like an Unreal project (no .uproject and no Content folder)"), *OutProjectDir);
			return false;
		}
		return true;
	}
}

void FUAL_ContentOrganizeCommands::Handle_Migrate(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const double StartTime = FPlatformTime::Seconds();

	TArray<FString> Paths;
	UAL_ReadStringArray(Payload, TEXT("paths"), Paths);
	if (Paths.Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing or empty 'paths' array (assets or folders)"));
		return;
	}
	FString Destination;
	if (!Payload->TryGetStringField(TEXT("destination"), Destination) || Destination.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required parameter: destination (target project's .uproject, project dir, or Content dir)"));
		return;
	}
	bool bIncludeDependencies = true;
	Payload->TryGetBoolField(TEXT("include_dependencies"), bIncludeDependencies);
	FString OnConflict = TEXT("skip");
	Payload->TryGetStringField(TEXT("on_conflict"), OnConflict);
	OnConflict = OnConflict.ToLower();
	if (OnConflict != TEXT("skip") && OnConflict != TEXT("overwrite"))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("on_conflict must be 'skip' or 'overwrite'"));
		return;
	}
	bool bDryRun = false;
	Payload->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bSaveFirst = true;
	Payload->TryGetBoolField(TEXT("save_first"), bSaveFirst);

	FString DestContentDir, DestProjectDir, DestError;
	if (!UAL_ResolveDestinationContent(Destination, DestContentDir, DestProjectDir, DestError))
	{
		UAL_CommandUtils::SendError(RequestId, 400, DestError);
		return;
	}
	FString OwnContent = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
	FPaths::NormalizeDirectoryName(OwnContent);
	FString DestContentFull = FPaths::ConvertRelativePathToFull(DestContentDir);
	FPaths::NormalizeDirectoryName(DestContentFull);
	if (DestContentFull.Equals(OwnContent, ESearchCase::IgnoreCase))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Destination is this project's own Content folder. Use content.batch_move to move within the project."));
		return;
	}

	if (!FUAL_RegistryReady::Ensure(Payload, RequestId))
	{
		return;
	}
	IAssetRegistry& Registry = UAL_Registry();

	// ── 1. 根集合 ─────────────────────────────────────────────────────────
	TSet<FName> RootSet;
	TArray<FString> NotFound;
	int32 LevelCount = 0;
	for (const FString& Path : Paths)
	{
		if (UAL_IsFolder(Registry, Path))
		{
			TArray<FAssetData> InFolder;
			Registry.GetAssetsByPath(FName(*UAL_ToPackageName(Path)), InFolder, /*bRecursive=*/true);
			for (const FAssetData& Data : InFolder)
			{
				RootSet.Add(Data.PackageName);
				if (UAL_ClassNameOf(Data) == TEXT("World")) ++LevelCount;
			}
			continue;
		}
		FAssetData Asset;
		if (UAL_ResolveAsset(Registry, Path, Asset))
		{
			RootSet.Add(Asset.PackageName);
			if (UAL_ClassNameOf(Asset) == TEXT("World")) ++LevelCount;
		}
		else
		{
			NotFound.Add(Path);
		}
	}
	if (RootSet.Num() == 0)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("not_found"), UAL_StringArray(NotFound));
		UAL_CommandUtils::SendError(RequestId, 404, TEXT("None of the given paths resolved to an asset or a folder"), Details);
		return;
	}

	// One File Per Actor 关卡的 Actor 存在 __ExternalActors__/__ExternalObjects__ 下，
	// 注册表不把它们记成关卡的依赖。不带上的话，关卡在目标工程里打开是空的。
	TArray<FName> RootArray = RootSet.Array();
	for (const FName& Root : RootArray)
	{
		TArray<FAssetData> InPackage;
		Registry.GetAssetsByPackageName(Root, InPackage);
		if (InPackage.Num() == 0 || UAL_ClassNameOf(InPackage[0]) != TEXT("World"))
		{
			continue;
		}
		const FString RootStr = Root.ToString();
		const FName Mount = FPackageName::GetPackageMountPoint(RootStr);
		const FString MountPrefix = TEXT("/") + Mount.ToString();
		const FString Relative = RootStr.Mid(MountPrefix.Len()); // "/Maps/Main"
		for (const TCHAR* Bucket : { TEXT("/__ExternalActors__"), TEXT("/__ExternalObjects__") })
		{
			TArray<FAssetData> External;
			Registry.GetAssetsByPath(FName(*(MountPrefix + Bucket + Relative)), External, /*bRecursive=*/true);
			for (const FAssetData& Data : External)
			{
				RootSet.Add(Data.PackageName);
			}
		}
	}

	// ── 2. 依赖闭包 ───────────────────────────────────────────────────────
	TSet<FName> Closure = RootSet;
	TArray<FString> Missing;
	if (bIncludeDependencies)
	{
		TArray<FName> Frontier = RootSet.Array();
		int32 Cursor = 0;
		while (Cursor < Frontier.Num())
		{
			const FName Current = Frontier[Cursor++];
			TArray<FName> Deps;
			Registry.GetDependencies(Current, Deps, UE::AssetRegistry::EDependencyCategory::Package);
			for (const FName& Dep : Deps)
			{
				const FString DepStr = Dep.ToString();
				if (UAL_IsScriptOrEngine(DepStr) || Closure.Contains(Dep))
				{
					continue;
				}
				TArray<FAssetData> InPackage;
				Registry.GetAssetsByPackageName(Dep, InPackage);
				if (InPackage.Num() == 0)
				{
					Missing.AddUnique(DepStr);
					continue;
				}
				Closure.Add(Dep);
				Frontier.Add(Dep);
			}
		}
	}

	// ── 3. 每个包 → 源文件 / 目标文件 ─────────────────────────────────────
	TArray<FUAL_MigrateFile> Files;
	TArray<FString> UnsavedSources;
	int64 TotalBytes = 0;
	int32 ExternalSkipped = 0;

	for (const FName& PackageName : Closure)
	{
		FUAL_MigrateFile File;
		File.Package = PackageName.ToString();

		if (!UAL_PackageFilename(File.Package, File.SourceFile))
		{
			File.Status = TEXT("failed");
			File.Error = TEXT("No file on disk for this package (unsaved new asset?)");
			Files.Add(File);
			continue;
		}

		const FName Mount = FPackageName::GetPackageMountPoint(File.Package);
		const FString MountStr = Mount.ToString();
		const FString MountPrefix = TEXT("/") + MountStr + TEXT("/");
		FString RelativePackage = File.Package.Mid(MountPrefix.Len());
		const FString Extension = FPaths::GetExtension(File.SourceFile, /*bIncludeDot=*/true);

		if (MountStr == TEXT("Game"))
		{
			File.DestFile = DestContentDir / (RelativePackage + Extension);
		}
		else
		{
			// 插件内容：目标工程装了同名插件才拷（Plugins/<Name>/Content 或再深一层）
			FString PluginContent;
			TArray<FString> Candidates;
			Candidates.Add(DestProjectDir / TEXT("Plugins") / MountStr / TEXT("Content"));
			TArray<FString> SubDirs;
			IFileManager::Get().FindFiles(SubDirs, *(DestProjectDir / TEXT("Plugins") / TEXT("*")), false, true);
			for (const FString& Sub : SubDirs)
			{
				Candidates.Add(DestProjectDir / TEXT("Plugins") / Sub / MountStr / TEXT("Content"));
			}
			for (const FString& Candidate : Candidates)
			{
				if (FPaths::DirectoryExists(Candidate))
				{
					PluginContent = Candidate;
					break;
				}
			}
			if (PluginContent.IsEmpty())
			{
				File.Status = TEXT("external_skipped");
				File.Error = FString::Printf(TEXT("Content of plugin '%s' - destination project has no such plugin"), *MountStr);
				++ExternalSkipped;
				Files.Add(File);
				continue;
			}
			File.DestFile = PluginContent / (RelativePackage + Extension);
		}

		File.Bytes = IFileManager::Get().FileSize(*File.SourceFile);
		TotalBytes += FMath::Max<int64>(File.Bytes, 0);

		if (UPackage* Loaded = FindPackage(nullptr, *File.Package))
		{
			if (Loaded->IsDirty())
			{
				UnsavedSources.Add(File.Package);
			}
		}

		if (IFileManager::Get().FileExists(*File.DestFile) && OnConflict == TEXT("skip"))
		{
			File.Status = TEXT("skipped_exists");
		}
		Files.Add(File);
	}

	auto FilesJson = [&]() -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FUAL_MigrateFile& File : Files)
		{
			if (Out.Num() >= 500)
			{
				break;
			}
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("package"), File.Package);
			Obj->SetStringField(TEXT("status"), File.Status);
			if (!File.DestFile.IsEmpty())
			{
				Obj->SetStringField(TEXT("destination_file"), File.DestFile);
			}
			Obj->SetNumberField(TEXT("bytes"), (double)File.Bytes);
			Obj->SetBoolField(TEXT("is_root"), RootSet.Contains(FName(*File.Package)));
			if (!File.Error.IsEmpty())
			{
				Obj->SetStringField(TEXT("error"), File.Error);
			}
			Out.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Out;
	};

	auto CountStatus = [&](const TCHAR* Status) -> int32
	{
		int32 Count = 0;
		for (const FUAL_MigrateFile& File : Files)
		{
			if (File.Status == Status) ++Count;
		}
		return Count;
	};

	TArray<FString> Notes;
	if (NotFound.Num() > 0)
	{
		Notes.Add(FString::Printf(TEXT("%d input path(s) were not found and ignored."), NotFound.Num()));
	}
	if (Missing.Num() > 0)
	{
		Notes.Add(FString::Printf(TEXT("%d dependency package(s) are missing from this project (broken references) and cannot be migrated."), Missing.Num()));
	}
	if (LevelCount > 0)
	{
		Notes.Add(TEXT("Levels included: their One-File-Per-Actor packages under __ExternalActors__/__ExternalObjects__ were added automatically."));
	}
	if (ExternalSkipped > 0)
	{
		Notes.Add(FString::Printf(TEXT("%d file(s) belong to plugins the destination project does not have (external_skipped). Install those plugins there first, or accept that the references will be broken."), ExternalSkipped));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetStringField(TEXT("destination_content_dir"), DestContentDir);
	Result->SetNumberField(TEXT("root_count"), RootSet.Num());
	Result->SetNumberField(TEXT("planned"), CountStatus(TEXT("planned")));
	Result->SetNumberField(TEXT("total_bytes"), (double)TotalBytes);
	Result->SetNumberField(TEXT("external_skipped"), ExternalSkipped);
	if (Missing.Num() > 0)
	{
		Result->SetArrayField(TEXT("missing"), UAL_StringArray(Missing));
	}
	if (NotFound.Num() > 0)
	{
		Result->SetArrayField(TEXT("not_found"), UAL_StringArray(NotFound));
	}

	if (bDryRun)
	{
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetNumberField(TEXT("copied"), 0);
		Result->SetNumberField(TEXT("skipped"), CountStatus(TEXT("skipped_exists")));
		Result->SetNumberField(TEXT("failed"), CountStatus(TEXT("failed")));
		Result->SetArrayField(TEXT("files"), FilesJson());
		Result->SetBoolField(TEXT("files_truncated"), Files.Num() > 500);
		if (UnsavedSources.Num() > 0)
		{
			Result->SetArrayField(TEXT("unsaved_sources"), UAL_StringArray(UnsavedSources));
			Notes.Add(FString::Printf(TEXT("%d source package(s) have unsaved changes; with save_first=true they will be saved before copying."), UnsavedSources.Num()));
		}
		Notes.Add(TEXT("Dry run - nothing was copied."));
		Result->SetArrayField(TEXT("notes"), UAL_StringArray(Notes));
		Result->SetNumberField(TEXT("elapsed_ms"), (FPlatformTime::Seconds() - StartTime) * 1000.0);
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	// ── 4. 先落盘再拷：磁盘上的文件才是被拷的那份 ────────────────────────
	TArray<FString> SaveFailed;
	if (bSaveFirst && UnsavedSources.Num() > 0)
	{
		TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
		for (const FString& PackageName : UnsavedSources)
		{
			if (!UAL_SavePackage(FindPackage(nullptr, *PackageName)))
			{
				SaveFailed.Add(PackageName);
			}
		}
	}

	// ── 5. 拷 + 回读 ──────────────────────────────────────────────────────
	int32 Copied = 0;
	for (FUAL_MigrateFile& File : Files)
	{
		if (File.Status != TEXT("planned"))
		{
			continue;
		}
		const FString DestDir = FPaths::GetPath(File.DestFile);
		if (!IFileManager::Get().DirectoryExists(*DestDir) && !IFileManager::Get().MakeDirectory(*DestDir, /*Tree=*/true))
		{
			File.Status = TEXT("failed");
			File.Error = TEXT("Could not create destination directory");
			continue;
		}
		const uint32 CopyResult = IFileManager::Get().Copy(*File.DestFile, *File.SourceFile, /*Replace=*/true, /*EvenIfReadOnly=*/true);
		const int64 DestSize = IFileManager::Get().FileSize(*File.DestFile);
		const int64 SrcSize = IFileManager::Get().FileSize(*File.SourceFile);
		if (CopyResult == COPY_OK && DestSize == SrcSize && DestSize >= 0)
		{
			File.Status = TEXT("copied");
			File.Bytes = DestSize;
			++Copied;
		}
		else
		{
			File.Status = TEXT("failed");
			File.Error = FString::Printf(TEXT("Copy failed (result=%u, dest size %lld vs source %lld)"), CopyResult, DestSize, SrcSize);
		}
	}

	const int32 Failed = CountStatus(TEXT("failed"));
	Result->SetBoolField(TEXT("ok"), Failed == 0 && Copied > 0);
	Result->SetNumberField(TEXT("copied"), Copied);
	Result->SetNumberField(TEXT("skipped"), CountStatus(TEXT("skipped_exists")));
	Result->SetNumberField(TEXT("failed"), Failed);
	Result->SetArrayField(TEXT("files"), FilesJson());
	Result->SetBoolField(TEXT("files_truncated"), Files.Num() > 500);
	if (UnsavedSources.Num() > 0)
	{
		Result->SetArrayField(TEXT("unsaved_sources"), UAL_StringArray(UnsavedSources));
		if (!bSaveFirst)
		{
			Notes.Add(FString::Printf(TEXT("%d source package(s) had unsaved changes and save_first=false - the copied files are the older on-disk versions."), UnsavedSources.Num()));
		}
	}
	if (SaveFailed.Num() > 0)
	{
		Result->SetArrayField(TEXT("save_failed"), UAL_StringArray(SaveFailed));
		Notes.Add(FString::Printf(TEXT("%d package(s) could not be saved before copying; their on-disk (older) versions were copied."), SaveFailed.Num()));
	}
	if (Copied > 0)
	{
		Notes.Add(TEXT("Open the destination project and let its asset registry scan; redirectors inside the copied set were copied as-is and can be cleaned there with content.fixup_redirectors."));
	}
	Result->SetArrayField(TEXT("notes"), UAL_StringArray(Notes));
	Result->SetNumberField(TEXT("elapsed_ms"), (FPlatformTime::Seconds() - StartTime) * 1000.0);

	UE_LOG(LogUALOrganize, Log, TEXT("content.migrate: roots=%d closure=%d copied=%d failed=%d external_skipped=%d -> %s"),
		RootSet.Num(), Closure.Num(), Copied, Failed, ExternalSkipped, *DestContentDir);

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// 注册
// ============================================================================

void FUAL_ContentOrganizeCommands::RegisterCommands(
	TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("content.naming_audit"), &Handle_NamingAudit);
	CommandMap.Add(TEXT("content.batch_move"), &Handle_BatchMove);
	CommandMap.Add(TEXT("content.dependencies"), &Handle_Dependencies);
	CommandMap.Add(TEXT("content.migrate"), &Handle_Migrate);
	UE_LOG(LogUALOrganize, Log, TEXT("Registered 4 content organize commands"));
}
