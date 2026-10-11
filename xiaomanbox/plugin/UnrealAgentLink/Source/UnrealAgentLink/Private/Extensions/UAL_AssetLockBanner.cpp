#include "UAL_AssetLockBanner.h"

#include "UAL_AssetLockState.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Engine/World.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "SLevelViewport.h"
#include "Modules/ModuleManager.h"
#include "Styling/CoreStyle.h"
#include "ToolMenu.h"
#include "ToolMenuEntry.h"
#include "ToolMenuSection.h"
#include "ToolMenus.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/AssetEditorToolkitMenuContext.h"
#include "UObject/Package.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALAssetLockBanner, Log, All);

namespace
{
	/** 父工具栏的名字。每个资产编辑器的工具栏都以它为父，见 FAssetEditorToolkit::GenerateToolbar */
	const FName DefaultAssetEditorToolBar(TEXT("AssetEditor.DefaultToolBar"));

	const FName BannerOwner(TEXT("UnrealAgentLink.AssetLockBanner"));

	FDelegateHandle GLevelEditorCreatedHandle;
	FDelegateHandle GMapChangedHandle;
	FDelegateHandle GLockChangedHandle;

	/**
	 * 挂出去的关卡视口横幅，**每个关卡编辑器一条**。摘的时候要用同一个引用。
	 *
	 * 不能只留一份：引擎允许开第二个关卡编辑器窗口，覆盖掉第一份的话它就再也
	 * 摘不掉了 —— 而它绑的 lambda 指向本模块的代码，模块一卸载，下一帧绘制就崩。
	 */
	struct FLevelBanner
	{
		TWeakPtr<ILevelEditor> LevelEditor;
		/**
		 * **挂在哪个视口上就得从哪个视口摘。** `AddViewportOverlayWidget` 不传视口
		 * 时引擎回退到「此刻活动的那个」，而摘的时候活动的可能已经换了（四视图
		 * 布局、切页签）—— 那样摘不掉，横幅留在原视口上继续引用死代码。
		 */
		TWeakPtr<IAssetViewport> Viewport;
		/**
		 * **强引用**。视口那边虽然也持有它，但布局一重建就会连叠加层一起丢掉；
		 * 我们留着这一份，下一次 `EnsureLevelBanners` 才能把同一个 widget 重新
		 * 挂回去，而不是当成「已经没了」忘掉它。
		 */
		TSharedRef<SWidget> Banner;
	};

	TArray<FLevelBanner> GLevelOverlays;

	/**
	 * 一组包里有没有被锁的，有的话返回第一个。
	 *
	 * 一个编辑器窗口可能同时编辑多个资产（多选打开、蓝图带子对象）。任何一个
	 * 被锁就该提示 —— 报第一个即可，用户要的是「这里有东西被锁了」这个信号。
	 */
	FName FirstLocked(const TArray<FName>& PackageNames)
	{
		for (const FName& PackageName : PackageNames)
		{
			if (FUAL_AssetLockState::IsLocked(PackageName))
			{
				return PackageName;
			}
		}
		return NAME_None;
	}

	/**
	 * 一枚「胶囊」—— 两个面共用的视觉本体。
	 *
	 * ## 配色：不给画面着色
	 *
	 * 死中性的近黑底 + 一圈高饱和琥珀描边。上一版是给整块底色上橙，在深色编辑器里
	 * 必然糊成一片，更要命的是**它会改变用户正在看的画面**——用户看视口是为了判断
	 * 场景的颜色和明暗，提示不该参与这件事。
	 *
	 * @param CurrentLocked 每帧回调，返回此刻被锁的那个包（没有就是 NAME_None）。
	 *   **不能构造时算一次**：工具栏只在窗口打开时生成一次，而锁是几秒钟之后才加上的。
	 * @param bWithDetail 带不带第二行。视口那枚要带（它 tooltip 弹不出来），
	 *   工具栏那枚不带（省地方，完整说明走 tooltip）。
	 */
	TSharedRef<SWidget> MakeLockChip(TFunction<FName()> CurrentLocked, bool bWithDetail)
	{
		// 静态：刷子的内容是固定的，没必要每次建控件都造一个
		static const FSlateRoundedBoxBrush ChipBrush(
			FLinearColor(0.015f, 0.015f, 0.018f, 0.88f), // 近黑中性底
			5.0f,                                        // 圆角
			FLinearColor(1.0f, 0.62f, 0.13f, 0.55f),     // 琥珀描边
			1.0f);

		TSharedRef<SVerticalBox> Lines = SNew(SVerticalBox);

		Lines->AddSlot()
			.AutoHeight()
			[
				SNew(STextBlock)
					.Text(TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateLambda(
						[CurrentLocked]() { return FUAL_AssetLockState::ShortLabel(CurrentLocked()); })))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.94f, 0.94f, 0.95f)))
			];

		if (bWithDetail)
		{
			Lines->AddSlot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 1.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
						.Text(TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateLambda(
							[CurrentLocked]() { return FUAL_AssetLockState::HintLine(CurrentLocked()); })))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.60f, 0.60f, 0.63f)))
				];
		}

		return SNew(SBorder)
			.BorderImage(&ChipBrush)
			.Padding(FMargin(9.0f, 5.0f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 7.0f, 0.0f))
				[
					// 锁死 13x13：不限尺寸的话 Icons.Lock 按源图大小铺开，把胶囊撑成一块砖
					SNew(SBox)
						.WidthOverride(13.0f)
						.HeightOverride(13.0f)
						[
							SNew(SImage)
								.Image(FCoreStyle::Get().GetBrush(TEXT("Icons.Lock")))
								.ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.62f, 0.13f)))
						]
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					Lines
				]
			];
	}

	/** 资产编辑器工具栏上那枚。可命中，所以完整说明走 tooltip */
	TSharedRef<SWidget> MakeBanner(TArray<FName> PackageNames)
	{
		const auto CurrentLocked = [PackageNames]() { return FirstLocked(PackageNames); };

		return SNew(SBox)
			.VAlign(VAlign_Center)
			.ToolTipText(TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateLambda(
				[CurrentLocked]() { return FUAL_AssetLockState::DescribeLock(CurrentLocked()); })))
			.Visibility(TAttribute<EVisibility>::Create(TAttribute<EVisibility>::FGetter::CreateLambda(
				[CurrentLocked]()
				{
					return CurrentLocked().IsNone() ? EVisibility::Collapsed : EVisibility::Visible;
				})))
			[
				MakeLockChip(CurrentLocked, /*bWithDetail=*/false)
			];
	}

	/** 关卡视口横幅盯的是「当前这张关卡的包」，每帧重新问一次 —— 用户会切关卡 */
	TArray<FName> CurrentLevelPackages()
	{
		TArray<FName> Packages;
		if (GEditor)
		{
			if (const UWorld* World = GEditor->GetEditorWorldContext().World())
			{
				if (const UPackage* Package = World->GetOutermost())
				{
					Packages.Add(Package->GetFName());
				}
			}
		}
		return Packages;
	}

	TSharedRef<SWidget> MakeLevelBanner()
	{
		// 关卡不能像资产编辑器那样在构造时把包名定下来 —— 用户会切关卡，
		// 而这个横幅是挂在视口上、跟着编辑器活一辈子的
		// **不按包名匹配。** Actor 类工具（spawn / 移动 / 改属性）的参数里一个资产
		// 路径都没有，盒子那边并不知道被改的是哪张关卡，所以它只发一个
		// `level_locked` 布尔；是哪张由我们自己回答。
		//
		// 顺带也认「这张关卡的包本身被锁了」（比如模型直接调 content 工具动
		// 那个 .umap），两条任一成立就显示。
		const auto CurrentLocked = []() -> FName
		{
			const FName ByPackage = FirstLocked(CurrentLevelPackages());
			if (!ByPackage.IsNone())
			{
				return ByPackage;
			}
			if (!FUAL_AssetLockState::IsLevelLocked())
			{
				return NAME_None;
			}
			// 拿当前关卡的包名当「被锁的那个」交给文案层。它没进 enforced，
			// 所以自然落到保守那一档（「会互相覆盖」）—— 关卡是软锁，这正是实情
			const TArray<FName> Packages = CurrentLevelPackages();
			return Packages.Num() > 0 ? Packages[0] : NAME_None;
		};

		// **这层 SBox 是必须的，不是排版偏好。**
		//
		// 视口叠加层的槽是拉满的，而 `SBorder` 的 `HAlign/VAlign` 对齐的是它的
		// **子控件**，不会把边框自己缩小 —— 直接挂进去，那块半透明底色会铺满
		// 整个视口。上一版就是这么把整个场景刷成一片橙色的，用户什么都看不见。
		// SBox 会把子控件按它自己的期望尺寸摆到指定角落，胶囊才收得住。
		//
		// 摆顶部居中而不是左上：视口自己的统计信息、坐标轴、模式提示都在左上，
		// 挤在一起谁都读不清。
		return SNew(SBox)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Top)
			.Padding(FMargin(0.0f, 12.0f, 0.0f, 0.0f))
			.Visibility(TAttribute<EVisibility>::Create(TAttribute<EVisibility>::FGetter::CreateLambda(
				[CurrentLocked]()
				{
					// HitTestInvisible：胶囊浮在视口上，一点用户的点击都不能吃 ——
					// 「盒子让我点不了场景」比不提示糟得多。代价是 tooltip 弹不出来
					// （Slate 的 tooltip 靠命中测试），所以第二行直接写进可见文本
					return CurrentLocked().IsNone() ? EVisibility::Collapsed : EVisibility::HitTestInvisible;
				})))
			[
				MakeLockChip(CurrentLocked, /*bWithDetail=*/true)
			];
	}

	/** 这个关卡编辑器此刻的活动视口，拿不到就是「还没就绪」 */
	TSharedPtr<IAssetViewport> ActiveViewportOf(const TSharedPtr<ILevelEditor>& LevelEditor)
	{
		const TSharedPtr<SLevelViewport> Viewport = LevelEditor->GetActiveViewportInterface();
		return Viewport.IsValid() ? StaticCastSharedPtr<IAssetViewport>(Viewport) : nullptr;
	}

	/**
	 * 让活着的关卡编辑器挂着一条横幅 —— 幂等，可以随便多调。
	 *
	 * **必须能重复调，因为一次挂不上是常态。** `AddViewportOverlayWidget` 不传
	 * 视口时走 `GetActiveViewport()`，而那个函数遍历 `ViewportTabs`；插件是
	 * Default 阶段加载的，`OnLevelEditorCreated` 触发那一刻页签往往还没建出来，
	 * 于是引擎那个 `if / else if` 两个分支都不成立、**直接返回**，一声不响。
	 * 原先只在创建时挂一次，结果就是关卡横幅压根不出现，而日志里毫无痕迹。
	 *
	 * 改法：自己先把视口取出来，取不到就这轮不挂、等下一次触发；取得到就
	 * **显式传给引擎**，那条静默返回的分支根本走不到。
	 *
	 * 触发点三个（见 Initialize）：关卡编辑器建好、地图切换、锁状态变化。
	 * 前两个覆盖启动时序，最后一个覆盖「用户中途重建了视口布局」—— 布局重建会
	 * 把叠加层一起丢掉，而 agent 干活期间锁状态本来就在不停变。
	 */
	void EnsureLevelBanners()
	{
		// 关卡编辑器没了的条目直接丢。**视口没了的不能丢** —— 那说明布局重建过，
		// 该做的是把同一个 widget 重新挂上去，不是忘掉它
		GLevelOverlays.RemoveAll([](const FLevelBanner& Entry) { return !Entry.LevelEditor.IsValid(); });

		FLevelEditorModule* LevelEditorModule =
			FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
		if (!LevelEditorModule)
		{
			return;
		}

		const TSharedPtr<ILevelEditor> LevelEditor = LevelEditorModule->GetFirstLevelEditor();
		if (!LevelEditor.IsValid())
		{
			return;
		}

		const TSharedPtr<IAssetViewport> Viewport = ActiveViewportOf(LevelEditor);
		if (!Viewport.IsValid())
		{
			// 视口还没就绪。不挂 —— 挂了也是空操作，还会留下一条假装挂上了的记录
			return;
		}

		FLevelBanner* Existing = GLevelOverlays.FindByPredicate(
			[&LevelEditor](const FLevelBanner& Entry) { return Entry.LevelEditor.Pin() == LevelEditor; });

		if (Existing && Existing->Viewport.Pin() == Viewport)
		{
			return; // 已经挂在这个视口上了
		}

		if (Existing)
		{
			// 换视口了（或上次那个已经没了）：先从旧的上摘干净再挂新的，
			// 否则旧视口上会留一份摘不掉的
			if (const TSharedPtr<IAssetViewport> Stale = Existing->Viewport.Pin())
			{
				LevelEditor->RemoveViewportOverlayWidget(Existing->Banner, Stale);
			}
			LevelEditor->AddViewportOverlayWidget(Existing->Banner, Viewport);
			Existing->Viewport = Viewport;
			return;
		}

		const TSharedRef<SWidget> Banner = MakeLevelBanner();
		LevelEditor->AddViewportOverlayWidget(Banner, Viewport);
		GLevelOverlays.Add(FLevelBanner{ LevelEditor, Viewport, Banner });
	}

	void OnLevelEditorCreated(TSharedPtr<ILevelEditor>)
	{
		EnsureLevelBanners();
	}

	void OnMapChanged(UWorld*, EMapChangeType)
	{
		// 地图切换时视口一定已经就绪了 —— 这是「创建那一刻还没有视口」的兜底
		EnsureLevelBanners();
	}

	/**
	 * 这个资产编辑器窗口在编哪些对象。
	 *
	 * `UAssetEditorToolkitMenuContext::GetEditingObjects()` 是 5.1 才加的，5.0 上
	 * 这个上下文只有一个 `Toolkit` 弱指针，得自己去问 toolkit。
	 *
	 * 用重载决议做编译期能力探测，不用 `#if ENGINE_MINOR_VERSION`：工作室会改版本号，
	 * 方法在不在永远是准的。同 `UAL_VersionCompat.h` 里 `Private::RootTracks` 的写法。
	 */
	namespace Private
	{
		// int 重载优先：5.1+ 的上下文自己会答
		template <typename ContextType>
		auto EditingObjects(const ContextType* Context, int)
			-> decltype(Context->GetEditingObjects())
		{
			return Context->GetEditingObjects();
		}

		// long 重载兜底：5.0 只能顺着 Toolkit 摸过去
		template <typename ContextType>
		TArray<UObject*> EditingObjects(const ContextType* Context, long)
		{
			TArray<UObject*> Objects;
			if (const TSharedPtr<FAssetEditorToolkit> Toolkit = Context->Toolkit.Pin())
			{
				if (const TArray<UObject*>* Editing = Toolkit->GetObjectsCurrentlyBeingEdited())
				{
					Objects = *Editing;
				}
			}
			return Objects;
		}
	}

	void RegisterAssetEditorToolbar()
	{
		UToolMenus* ToolMenus = UToolMenus::Get();
		if (!ToolMenus)
		{
			return;
		}

		// 父菜单可能还没被任何资产编辑器注册过（第一个编辑器打开时才注册）。
		// ExtendMenu 允许提前扩展，注册时会把扩展合进去
		UToolMenu* Toolbar = ToolMenus->ExtendMenu(DefaultAssetEditorToolBar);
		if (!Toolbar)
		{
			return;
		}

		FToolMenuSection& Section = Toolbar->FindOrAddSection(TEXT("UnrealBoxAssetLock"));
		Section.AddDynamicEntry(
			TEXT("UnrealBoxAssetLockBanner"),
			FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& InSection)
			{
				// 上下文告诉我们「这个窗口在编辑什么」。拿不到就不加 ——
				// 宁可少一个横幅，也不能在不知道编辑什么的窗口上瞎报锁
				const UAssetEditorToolkitMenuContext* Context =
					InSection.FindContext<UAssetEditorToolkitMenuContext>();
				if (!Context)
				{
					return;
				}

				TArray<FName> PackageNames;
				for (const UObject* Object : Private::EditingObjects(Context, 0))
				{
					if (const UPackage* Package = Object ? Object->GetOutermost() : nullptr)
					{
						PackageNames.AddUnique(Package->GetFName());
					}
				}

				if (PackageNames.Num() == 0)
				{
					return;
				}

				InSection.AddEntry(FToolMenuEntry::InitWidget(
					TEXT("UnrealBoxAssetLockWidget"),
					MakeBanner(PackageNames),
					FText::GetEmpty(),
					/*bNoIndent=*/true,
					/*bSearchable=*/false));
			}));
	}
}

void FUAL_AssetLockBanner::Initialize()
{
	if (GLevelEditorCreatedHandle.IsValid())
	{
		return;
	}

	FToolMenuOwnerScoped OwnerScoped(BannerOwner);
	RegisterAssetEditorToolbar();

	FLevelEditorModule& LevelEditorModule =
		FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));

	// 关卡编辑器可能已经建好了（插件是 Default 阶段加载，通常还没有，但热重载时会有）
	EnsureLevelBanners();

	GLevelEditorCreatedHandle =
		LevelEditorModule.OnLevelEditorCreated().AddStatic(&OnLevelEditorCreated);

	// 关卡编辑器刚建好那一刻视口页签往往还没有，挂不上去。地图切换时一定有了
	GMapChangedHandle = LevelEditorModule.OnMapChanged().AddStatic(&OnMapChanged);

	// 第三个触发点：锁状态一变就顺手补挂一次。覆盖「用户中途重建视口布局」——
	// 那会把叠加层连我们的横幅一起丢掉，而 agent 干活期间锁状态本来就在不停变
	GLockChangedHandle = FUAL_AssetLockState::OnChanged().AddStatic(&EnsureLevelBanners);
}

void FUAL_AssetLockBanner::Shutdown()
{
	if (FLevelEditorModule* LevelEditorModule =
			FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor")))
	{
		if (GLevelEditorCreatedHandle.IsValid())
		{
			LevelEditorModule->OnLevelEditorCreated().Remove(GLevelEditorCreatedHandle);
		}
		if (GMapChangedHandle.IsValid())
		{
			LevelEditorModule->OnMapChanged().Remove(GMapChangedHandle);
		}
	}
	GLevelEditorCreatedHandle.Reset();
	GMapChangedHandle.Reset();

	if (GLockChangedHandle.IsValid())
	{
		FUAL_AssetLockState::OnChanged().Remove(GLockChangedHandle);
		GLockChangedHandle.Reset();
	}

	// 视口横幅要**逐个**主动摘掉，否则模块卸载后它还挂在视口上，而它绑的 lambda
	// 指向已经卸载的代码 —— 下一帧绘制就崩
	for (const FLevelBanner& Entry : GLevelOverlays)
	{
		const TSharedPtr<ILevelEditor> LevelEditor = Entry.LevelEditor.Pin();
		if (!LevelEditor.IsValid())
		{
			continue;
		}
		// **原样传回挂上去时用的那个视口。** 不传的话引擎摘的是「此刻活动的那个」，
		// 用户中途切过页签就摘不掉，横幅留在原视口上继续引用已卸载的代码
		LevelEditor->RemoveViewportOverlayWidget(Entry.Banner, Entry.Viewport.Pin());
	}
	GLevelOverlays.Empty();

	if (UObjectInitialized())
	{
		UToolMenus::UnregisterOwner(BannerOwner);

		// **重刷一遍已经生成出去的工具栏。** UnregisterOwner 只把我们的条目从
		// 菜单**定义**里摘掉，此刻打开着的那些资产编辑器窗口上挂的是已经生成好的
		// SWidget —— 它三个 TAttribute 绑的 lambda 全部指向本模块的代码，模块一走
		// 下一帧绘制就崩。RefreshAllWidgets 让它们照新定义重建，横幅随之消失。
		//
		// **退出流程里不刷。** ShutdownModule 在每次正常关编辑器时都会跑
		// （「要重启」那句说的是*禁用插件*这个动作，不是退出），而这一下会把整个
		// 编辑器的菜单和工具栏全部重建一遍 —— 在别的模块已经卸到一半的时候。
		// 白花关机时间是轻的，撞上一个已经没了的 owner 就是崩在退出路径上，
		// 而用户只会说「关编辑器的时候崩了」。
		//
		// 真正需要它的只有 Live Coding / 热重载：模块换了、窗口还开着。
		//
		// ⚠️ **这里压着一个没核实过的假设**：退出时模块卸载发生在 Slate 拆完之后，
		// 所以那些还绑着本模块 lambda 的横幅不会再被绘制。假设不成立的话
		// （插件被别的模块提前卸载、或将来版本改了退出顺序），打开着的资产编辑器
		// 工具栏上那个 SBorder 下一帧就崩 —— 正是上面这段要防的事，只是换到了
		// 退出路径上。两个风险只能二选一，而选哪个应当由真机验过的顺序决定。
		// 已列进 的真机验收清单，别在验之前把这段删掉。
		if (!IsEngineExitRequested())
		{
			if (UToolMenus* ToolMenus = UToolMenus::Get())
			{
				ToolMenus->RefreshAllWidgets();
			}
		}
	}
}
