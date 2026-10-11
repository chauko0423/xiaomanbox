#include "UAL_AssetLockState.h"

#include "UAL_CommandUtils.h"
#include "UAL_TouchedPackages.h"

#include "AssetRegistry/AssetData.h"
#include "ContentBrowserDelegates.h"
#include "ContentBrowserModule.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Styling/CoreStyle.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALAssetLock, Log, All);

namespace
{
	/** 当前被盒子锁着的包。FName 比较本来就不区分大小写，不用自己归一 */
	TSet<FName> GLockedPackages;

	/**
	 * 其中只读位真的翻上去了的那些 —— `GLockedPackages` 的子集。
	 *
	 * 只锁不拦和真拦住是**两句不同的话**，混着说必然有一句是假的。
	 */
	TSet<FName> GEnforcedPackages;

	/** 当前关卡正被 AI 改着。软锁：只提示，不翻只读位 */
	bool GLevelLocked = false;

	/**
	 * 已经就这个包提醒过了。
	 *
	 * 必须按包去重：用户在细节面板上拖一个滑块，`MarkPackageDirty` 一秒能发
	 * 几十次，不去重就是满屏通知。
	 */
	TSet<FName> GWarnedPackages;

	FDelegateHandle GExtraStateHandle;
	FDelegateHandle GDirtyHandle;
	FDelegateHandle GSavedHandle;

	/** 和 FUAL_ContentBrowserExt 一样的土办法本地化：插件不带 .locres */
	FString LockText(const FString& ZhText, const FString& EnText)
	{
		const FString CultureName = FInternationalization::Get().GetCurrentCulture()->GetName();
		return CultureName.StartsWith(TEXT("zh")) ? ZhText : EnText;
	}

	/**
	 * 角标和通知共用的那句解释。**分两档，因为事实就是两档。**
	 *
	 * 只读位没翻上去时（工程启用了版本控制、资产还没落盘、文件原本就只读），
	 * 用户按 Ctrl+S 是真存得进去的。这时说「不会被保存」就是假话，
	 * 而他会照着这句假话放弃自己的改动 —— 那是我们骗他丢的工作。
	 */
	FText LockExplanationImpl(FName PackageName)
	{
		if (FUAL_AssetLockState::IsEnforced(PackageName))
		{
			return FText::FromString(LockText(
				TEXT("虚幻盒子的 AI 正在修改这个资产，已锁定。\n你现在的改动保存会被拦下（文件已设为只读）—— 等 AI 做完会自动解锁，也可以到盒子里停掉这条会话。"),
				TEXT("Unreal Box AI is editing this asset and has locked it.\nSaving your changes will be blocked (the file is read-only). It unlocks when the AI finishes, or stop the session in Unreal Box.")));
		}

		return FText::FromString(LockText(
			TEXT("虚幻盒子的 AI 正在修改这个资产。\n你现在的改动和 AI 的会互相覆盖 —— 建议等它做完，或到盒子里停掉这条会话。"),
			TEXT("Unreal Box AI is editing this asset.\nYour changes and the AI's will overwrite each other — wait for it to finish, or stop the session in Unreal Box.")));
	}

	/**
	 * 锁状态和文案都是会变的，所以全绑成属性而不是构造时算一次。
	 *
	 * 构造时算的话有两个后果：解锁后角标不消失，以及只读位翻上去之后
	 * 提示还停在保守版本（「会互相覆盖」），用户以为自己还能存。
	 */
	TAttribute<EVisibility> LockVisibility(FName PackageName)
	{
		return TAttribute<EVisibility>::Create(
			TAttribute<EVisibility>::FGetter::CreateLambda([PackageName]()
			{
				return FUAL_AssetLockState::IsLocked(PackageName) ? EVisibility::Visible : EVisibility::Collapsed;
			}));
	}

	TAttribute<FText> LockTooltipAttribute(FName PackageName)
	{
		return TAttribute<FText>::Create(
			TAttribute<FText>::FGetter::CreateLambda([PackageName]()
			{
				return FUAL_AssetLockState::DescribeLock(PackageName);
			}));
	}

	TSharedRef<SWidget> MakeLockIcon(const FAssetData& AssetData)
	{
		const FName PackageName = AssetData.PackageName;
		return SNew(SImage)
			.Image(FCoreStyle::Get().GetBrush(TEXT("Icons.Lock")))
			.ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.62f, 0.13f)))
			.ToolTipText(LockTooltipAttribute(PackageName))
			.Visibility(LockVisibility(PackageName));
	}

	TSharedRef<SWidget> MakeLockToolTip(const FAssetData& AssetData)
	{
		const FName PackageName = AssetData.PackageName;
		return SNew(STextBlock)
			.Text(LockTooltipAttribute(PackageName))
			.Visibility(LockVisibility(PackageName));
	}

	void ShowLockWarning(FName PackageName)
	{
		const FString ShortName = FPackageName::GetShortName(PackageName);

		FNotificationInfo Info(FText::FromString(LockText(
			FString::Printf(TEXT("「%s」正被 AI 修改"), *ShortName),
			FString::Printf(TEXT("\"%s\" is being edited by AI"), *ShortName))));
		Info.SubText = FUAL_AssetLockState::DescribeLock(PackageName);
		Info.Image = FCoreStyle::Get().GetBrush(TEXT("Icons.Lock"));
		Info.ExpireDuration = 8.0f;
		Info.bFireAndForget = true;

		FSlateNotificationManager::Get().AddNotification(Info);
	}

	void OnPackageMarkedDirty(UPackage* Package, bool /*bWasDirty*/)
	{
		if (!Package || GLockedPackages.Num() == 0)
		{
			return;
		}

		// agent 自己的改动同样会走这个事件。只有作用域**之外**的才是用户亲手改的，
		// 也只有那种才值得打断他
		if (FUAL_TouchedPackages::IsInCommandScope())
		{
			return;
		}

		if (Package == GetTransientPackage() || Package->HasAnyFlags(RF_Transient))
		{
			return;
		}

		const FName PackageName = Package->GetFName();
		if (!GLockedPackages.Contains(PackageName))
		{
			return;
		}

		bool bAlreadyWarned = false;
		GWarnedPackages.Add(PackageName, &bAlreadyWarned);
		if (bAlreadyWarned)
		{
			return;
		}

		ShowLockWarning(PackageName);
	}

	/** 上一次因为 Save All 被跳过而提醒的时刻，用来压掉一次保存里的重复提醒 */
	double GLastSkipWarnedAt = 0.0;

	/**
	 * 保存完之后回头看：有没有被只读位挡下、用户却不知道的。
	 *
	 * 引擎的 Save All 在 `FileHelpers.cpp` 里直接 `if (!bPkgReadOnly)` 跳过只读的包，
	 * **一句提示都没有**。用户按了 Save All，看到进度条走完，以为全存上了，
	 * 实际上被锁的那几个原封不动 —— 这比不拦还糟，因为他连「没存上」都不知道。
	 *
	 * 所以挂在「有包保存成功」这个事件上：只要这一次保存动作结束后，还有被我们
	 * 锁着且仍然脏的包，就说出来。
	 *
	 * **已知缺口**：如果这次 Save All 里**只有**被锁的包需要保存，一个成功事件都
	 * 不会发出来，这条路也就不会触发。那种情况下兜底的是用户改脏时已经弹过的
	 * 那条提醒（见 `OnPackageMarkedDirty`）。
	 */
	void OnPackageSaved(const FString& /*Filename*/, UPackage* /*Package*/, FObjectPostSaveContext)
	{
		if (GEnforcedPackages.Num() == 0 || FUAL_TouchedPackages::IsInCommandScope())
		{
			return;
		}

		// Save All 会为每个成功保存的包各发一次。这里只是想在一次保存动作之后
		// 说一句话，不是每个包说一次
		const double Now = FPlatformTime::Seconds();
		if (Now - GLastSkipWarnedAt < 2.0)
		{
			return;
		}

		TArray<FString> Blocked;
		for (const FName& PackageName : GEnforcedPackages)
		{
			const UPackage* Locked = FindPackage(nullptr, *PackageName.ToString());
			if (Locked && Locked->IsDirty())
			{
				Blocked.Add(FPackageName::GetShortName(PackageName));
			}
		}

		if (Blocked.Num() == 0)
		{
			return;
		}

		GLastSkipWarnedAt = Now;

		const FString Names = FString::Join(Blocked, TEXT("、"));
		FNotificationInfo Info(FText::FromString(LockText(
			FString::Printf(TEXT("有 %d 个资产没能存上"), Blocked.Num()),
			FString::Printf(TEXT("%d asset(s) were not saved"), Blocked.Num()))));
		Info.SubText = FText::FromString(LockText(
			FString::Printf(
				TEXT("%s 正被虚幻盒子的 AI 修改并已锁定，这次保存跳过了它们。\n等 AI 做完会自动解锁，也可以到盒子里停掉那条会话。"),
				*Names),
			FString::Printf(
				TEXT("%s are locked while Unreal Box AI edits them, so this save skipped them.\nThey unlock when the AI finishes, or stop the session in Unreal Box."),
				*Names)));
		Info.Image = FCoreStyle::Get().GetBrush(TEXT("Icons.Lock"));
		Info.ExpireDuration = 10.0f;
		Info.bFireAndForget = true;

		FSlateNotificationManager::Get().AddNotification(Info);
	}
}

void FUAL_AssetLockState::Initialize()
{
	if (GDirtyHandle.IsValid() || GExtraStateHandle.IsValid())
	{
		return;
	}

	GDirtyHandle = UPackage::PackageMarkedDirtyEvent.AddStatic(&OnPackageMarkedDirty);

	// Save All 静默跳过只读包，引擎一句话都不说。补上我们自己的提示
	GSavedHandle = UPackage::PackageSavedWithContextEvent.AddStatic(&OnPackageSaved);

	// 内容浏览器的额外状态生成器：一处注册，全部资产类型的图标上都能出角标。
	// 蓝图 / 材质 / 网格体 / 特效 / 关卡走的是同一套 item 壳，不需要按类型各写一遍
	FContentBrowserModule& ContentBrowser =
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	GExtraStateHandle = ContentBrowser.AddAssetViewExtraStateGenerator(FAssetViewExtraStateGenerator(
		FOnGenerateAssetViewExtraStateIndicators::CreateStatic(&MakeLockIcon),
		FOnGenerateAssetViewExtraStateIndicators::CreateStatic(&MakeLockToolTip)));
}

void FUAL_AssetLockState::Shutdown()
{
	if (GDirtyHandle.IsValid())
	{
		UPackage::PackageMarkedDirtyEvent.Remove(GDirtyHandle);
		GDirtyHandle.Reset();
	}

	if (GSavedHandle.IsValid())
	{
		UPackage::PackageSavedWithContextEvent.Remove(GSavedHandle);
		GSavedHandle.Reset();
	}

	if (GExtraStateHandle.IsValid())
	{
		// 关编辑器时内容浏览器模块可能已经先卸载了。取指针而不是 LoadModuleChecked，
		// 那个会在关闭流程里把编辑器 check 崩
		if (FContentBrowserModule* ContentBrowser =
				FModuleManager::GetModulePtr<FContentBrowserModule>(TEXT("ContentBrowser")))
		{
			ContentBrowser->RemoveAssetViewExtraStateGenerator(GExtraStateHandle);
		}
		GExtraStateHandle.Reset();
	}

	GLockedPackages.Empty();
	GEnforcedPackages.Empty();
	GLevelLocked = false;
	GWarnedPackages.Empty();
}

void FUAL_AssetLockState::RegisterCommands(TMap<FString, FHandlerFunc>& CommandMap)
{
	CommandMap.Add(TEXT("locks.set"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		auto ReadStringArray = [&Payload](const TCHAR* Field, TArray<FString>& Out)
		{
			const TArray<TSharedPtr<FJsonValue>>* Raw = nullptr;
			if (!Payload.IsValid() || !Payload->TryGetArrayField(Field, Raw) || !Raw)
			{
				return;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Raw)
			{
				FString Path;
				if (Value.IsValid() && Value->TryGetString(Path))
				{
					Out.Add(Path);
				}
			}
		};

		TArray<FString> Paths;
		TArray<FString> Enforced;
		ReadStringArray(TEXT("paths"), Paths);
		// 缺 enforced 就当一个都没拦住 —— 这是保守的那一边。反过来（默认全拦住）
		// 会让界面对着旧版盒子说假话
		ReadStringArray(TEXT("enforced"), Enforced);

		// 缺 paths 字段一律当空列表，而不是报错保持原样：盒子推的是全量，
		// 一次没解析出来就把角标停在过时状态上，比清掉更糟
		// 缺 level_locked 当 false —— 保守那一边。旧盒子不发这个字段
		bool bLevelLocked = false;
		if (Payload.IsValid())
		{
			Payload->TryGetBoolField(TEXT("level_locked"), bLevelLocked);
		}

		SetLocked(Paths, Enforced, bLevelLocked);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("locked"), Num());
		UAL_CommandUtils::SendResponse(RequestId, 200, Data);
	});
}

void FUAL_AssetLockState::SetLocked(
	const TArray<FString>& PackagePaths,
	const TArray<FString>& EnforcedPaths,
	bool bLevelLocked)
{
	GLevelLocked = bLevelLocked;

	const auto Fill = [](const TArray<FString>& Source, TSet<FName>& Target)
	{
		Target.Reset();
		for (const FString& Path : Source)
		{
			const FString Trimmed = Path.TrimStartAndEnd();
			if (!Trimmed.IsEmpty())
			{
				Target.Add(FName(*Trimmed));
			}
		}
	};

	Fill(PackagePaths, GLockedPackages);
	Fill(EnforcedPaths, GEnforcedPackages);

	// enforced 必须是 locked 的子集。盒子那边理应保证，但这里再收一次口 ——
	// 「说保存会被拦下、实际锁都没了」是最坏的一种假话
	GEnforcedPackages = GEnforcedPackages.Intersect(GLockedPackages);

	// 解锁过的包要能在下次上锁时重新提醒。不清的话用户在一次会话里被提醒过之后，
	// 后面无论 AI 再动它多少次都不会再有任何提示
	for (auto It = GWarnedPackages.CreateIterator(); It; ++It)
	{
		if (!GLockedPackages.Contains(*It))
		{
			It.RemoveCurrent();
		}
	}

	UE_LOG(LogUALAssetLock, Verbose, TEXT("Locked packages: %d"), GLockedPackages.Num());

	FUAL_AssetLockState::OnChanged().Broadcast();
}

void FUAL_AssetLockState::Clear()
{
	GLockedPackages.Reset();
	GEnforcedPackages.Reset();
	GLevelLocked = false;
	GWarnedPackages.Reset();
}

bool FUAL_AssetLockState::IsLocked(FName PackageName)
{
	return GLockedPackages.Contains(PackageName);
}

bool FUAL_AssetLockState::IsLevelLocked()
{
	return GLevelLocked;
}

bool FUAL_AssetLockState::IsEnforced(FName PackageName)
{
	return GEnforcedPackages.Contains(PackageName);
}

FText FUAL_AssetLockState::DescribeLock(FName PackageName)
{
	return LockExplanationImpl(PackageName);
}

FSimpleMulticastDelegate& FUAL_AssetLockState::OnChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}

FText FUAL_AssetLockState::ShortLabel(FName PackageName)
{
	// 第一行只说「怎么了」。「你该怎么办」在 HintLine 里 —— 挤在一行里的话
	// 胶囊会横着长到三四百像素，浮在视口上就成了遮挡
	if (IsEnforced(PackageName))
	{
		return FText::FromString(LockText(
			TEXT("虚幻盒子 AI 正在编辑 · 已锁定"),
			TEXT("Unreal Box AI is editing · locked")));
	}

	return FText::FromString(LockText(
		TEXT("虚幻盒子 AI 正在编辑"),
		TEXT("Unreal Box AI is editing")));
}

FText FUAL_AssetLockState::HintLine(FName PackageName)
{
	if (IsEnforced(PackageName))
	{
		return FText::FromString(LockText(
			TEXT("保存会被拦下 · 做完自动解锁，也可到盒子里停掉这条会话"),
			TEXT("Saving is blocked · unlocks when it finishes, or stop the session in Unreal Box")));
	}

	// 只锁不拦时保存是**真存得进去**的，只是之后会被盖掉。说成存不进去就是假话，
	// 而用户会照着这句假话放弃自己的改动（见 UAL_AssetLockState.h 的文案红线）
	return FText::FromString(LockText(
		TEXT("你的改动会和它互相覆盖 · 建议等它做完"),
		TEXT("Your changes and its will overwrite each other · better to wait")));
}

int32 FUAL_AssetLockState::Num()
{
	return GLockedPackages.Num();
}
