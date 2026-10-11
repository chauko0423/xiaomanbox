// Copyright uebox.ai
//
// ⚠️ 一次性探针 —— 第 0 步验完即删，不要在此基础上写功能。
//
// 目的：证实或证伪 风险 1
//   「Slate 事件注入后，控件是否真的会响应」。
//
// 该假设撑着计划里第 7–9 步共 14 个命令、约 5 周工期，而在写这份探针之前
// **从未被实测过** —— 只验证了 FSlateApplication 上那几个函数存在。
// 函数存在不等于注入有效：事件路由要经过命中测试、窗口定位、焦点，
// 任何一环不认这个合成事件，注入就是静默失效（编得过、跑得动、什么也没发生）。
//
// 探针刻意**自带被点对象**（自己建一个只有一个按钮的小窗口），不去点编辑器里
// 现成的按钮 —— 后者的结果取决于当时开着什么窗口，证明不了机制本身。
//
// 用法：编辑器控制台敲 `UAL.SlateProbe`，看 LogUALSlateProbe 的输出。

#include "CoreMinimal.h"

#include "Containers/Ticker.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Layout/Children.h"
#include "Layout/Geometry.h"
#include "Layout/WidgetPath.h"
#include "Misc/CommandLine.h"
#include "Misc/DelayedAutoRegister.h"
#include "Misc/Parse.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALSlateProbe, Log, All);

namespace
{
	/** 递归数控件，并统计其中有多少个能报出自己的源码创建位置。 */
	int32 CountWidgets(const TSharedRef<SWidget>& Widget, int32 Depth, int32 MaxDepth, int32& OutWithLocation)
	{
		if (Depth > MaxDepth)
		{
			return 0;
		}

		int32 Count = 1;
		if (Widget->GetCreatedInLocation() != NAME_None)
		{
			++OutWithLocation;
		}

		if (FChildren* Children = Widget->GetChildren())
		{
			const int32 Num = Children->Num();
			for (int32 Index = 0; Index < Num; ++Index)
			{
				Count += CountWidgets(Children->GetChildAt(Index), Depth + 1, MaxDepth, OutWithLocation);
			}
		}
		return Count;
	}

	/** A 段：控件树能不能遍历、源码定位有没有值（决定 4a 可行性）。 */
	void ProbeTreeWalk()
	{
		TArray<TSharedRef<SWindow>> Windows = FSlateApplication::Get().GetInteractiveTopLevelWindows();

		int32 TotalWidgets = 0;
		int32 WithLocation = 0;
		for (const TSharedRef<SWindow>& Window : Windows)
		{
			TotalWidgets += CountWidgets(Window, 0, 30, WithLocation);
		}

		UE_LOG(LogUALSlateProbe, Display,
			TEXT("[A 树遍历] 顶层窗口 %d 个｜控件 %d 个｜带源码定位 %d 个"),
			Windows.Num(), TotalWidgets, WithLocation);

		if (TotalWidgets == 0)
		{
			UE_LOG(LogUALSlateProbe, Error, TEXT("[A 树遍历] 一个控件都没数到 —— 4a 不成立，整个 §2.7 要撤下"));
		}
	}

	/** B 段：合成事件注入后按钮会不会真的响应（决定 4c/4d 可行性）。 */
	void ProbeInjection()
	{
		TSharedRef<bool> bClicked = MakeShared<bool>(false);

		TSharedRef<SButton> Button =
			SNew(SButton)
			.OnClicked_Lambda([bClicked]() -> FReply
			{
				*bClicked = true;
				return FReply::Handled();
			})
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("UAL Probe")))
			];

		TSharedRef<SWindow> Window =
			SNew(SWindow)
			.Title(FText::FromString(TEXT("UAL Slate Probe")))
			.ClientSize(FVector2D(240.0f, 80.0f))
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			[
				Button
			];

		FSlateApplication::Get().AddWindow(Window, true);

		// 几何要等界面走一遍布局才算得出来，所以延后 0.25 秒再点。
		// 立刻注入的话按钮的 AbsolutePosition 还是零，点到的是屏幕左上角。
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([Window, Button, bClicked](float) -> bool
			{
				FSlateApplication& App = FSlateApplication::Get();

				const FGeometry& Geometry = Button->GetTickSpaceGeometry();
				// 传 FVector2D 而不是 FVector2f —— 这是九个版本里唯一都收的类型。
				// 5.2+ 的形参是 UE::Slate::FDeprecateVector2DParameter（FVector2D 能隐式转进去），
				// 而 5.0/5.1 的形参就是 const FVector2D&，给 FVector2f 直接编不过。
				const FVector2D Center(Geometry.GetAbsolutePositionAtCoordinates(FVector2D(0.5f, 0.5f)));

				UE_LOG(LogUALSlateProbe, Display,
					TEXT("[B 注入] 按钮中心 (%.1f, %.1f)，尺寸 (%.1f, %.1f)"),
					Center.X, Center.Y,
					Geometry.GetAbsoluteSize().X, Geometry.GetAbsoluteSize().Y);

				App.SetCursorPos(Center);

				// ---- 诊断：先搞清楚 Slate 认不认这个坐标 ----
				// 注入失败有两种完全不同的原因，必须分开：
				//   (a) 命中测试根本没找到按钮 —— 是探针/窗口层级的问题
				//   (b) 找到了但事件没转成点击 —— 才是机制本身不行
				FWidgetPath PathToButton;
				const bool bFoundInHierarchy = App.FindPathToWidget(Button, PathToButton);
				UE_LOG(LogUALSlateProbe, Display, TEXT("[B 诊断] 按钮在控件层级里: %s"),
					bFoundInHierarchy ? TEXT("找得到") : TEXT("找不到"));

				const FWidgetPath UnderCursor =
					App.LocateWindowUnderMouse(Center, App.GetInteractiveTopLevelWindows());
				if (UnderCursor.IsValid())
				{
					UE_LOG(LogUALSlateProbe, Display, TEXT("[B 诊断] 该坐标下命中 %d 层，最内层是 %s"),
						UnderCursor.Widgets.Num(),
						*UnderCursor.Widgets.Last().Widget->GetTypeAsString());
				}
				else
				{
					UE_LOG(LogUALSlateProbe, Warning,
						TEXT("[B 诊断] 该坐标下命中测试为空 —— 窗口没进命中网格，是探针的问题不是机制的问题"));
				}

				// PressedButtons 在 FPointerEvent 里是按**指针**存的，
				// 这两个 TSet 必须活到 Process*Event 调用结束。
				TSet<FKey> Pressed;
				Pressed.Add(EKeys::LeftMouseButton);
				const TSet<FKey> Released;

				// 先发一次移动：Slate 内部维护「光标下的控件」，
				// 直接发按下会让悬停状态是空的，某些控件据此拒绝响应。
				const FPointerEvent MoveEvent(
					0, Center, Center, Released, EKeys::Invalid, 0.0f, FModifierKeysState());
				App.ProcessMouseMoveEvent(MoveEvent);

				const FPointerEvent DownEvent(
					0, Center, Center, Pressed, EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
				const bool bDownHandled = App.ProcessMouseButtonDownEvent(Window->GetNativeWindow(), DownEvent);

				const FPointerEvent UpEvent(
					0, Center, Center, Released, EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
				const bool bUpHandled = App.ProcessMouseButtonUpEvent(UpEvent);

				UE_LOG(LogUALSlateProbe, Display, TEXT("[B 诊断] 按下已处理=%s｜抬起已处理=%s"),
					bDownHandled ? TEXT("是") : TEXT("否"),
					bUpHandled ? TEXT("是") : TEXT("否"));

				if (*bClicked)
				{
					UE_LOG(LogUALSlateProbe, Display,
						TEXT("[B 注入] ✅ OnClicked 触发了 —— 4c/4d 成立，第 7–9 步按计划走"));
				}
				else
				{
					UE_LOG(LogUALSlateProbe, Error,
						TEXT("[B 注入] ❌ OnClicked 没触发 —— 保留 4a/4b（只读），砍掉第 9 步"));
				}

				Window->RequestDestroyWindow();
				return false; // 只跑一次
			}),
			0.25f);
	}

	void RunProbe()
	{
		if (!FSlateApplication::IsInitialized())
		{
			UE_LOG(LogUALSlateProbe, Error, TEXT("Slate 没初始化（无头模式？），探针跑不了"));
			return;
		}

		UE_LOG(LogUALSlateProbe, Display, TEXT("===== UAL Slate 探针开始 ====="));
		ProbeTreeWalk();
		ProbeInjection(); // 结论在 0.25 秒后的 ticker 里打印
	}
}

static FAutoConsoleCommand GUALSlateProbeCommand(
	TEXT("UAL.SlateProbe"),
	TEXT("一次性探针：验证 Slate 控件树遍历与合成事件注入是否可用（A 档第 0 步）"),
	FConsoleCommandDelegate::CreateStatic(&RunProbe));

// 带 `-UALSlateProbe` 启动时自动跑一次。
//
// 为什么不靠 `-ExecCmds="UAL.SlateProbe"`：那个的执行时机早于编辑器主界面建好，
// 跑的时候顶层窗口还没几个，A 段数出来的数字没有代表性。
// 这里挂在 EndOfEngineInit 之后再等 20 秒，确保编辑器完全起来了。
static FDelayedAutoRegisterHelper GUALSlateProbeAutoRun(
	EDelayedRegisterRunPhase::EndOfEngineInit,
	[]
	{
		if (!FParse::Param(FCommandLine::Get(), TEXT("UALSlateProbe")))
		{
			return;
		}

		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([](float) -> bool
			{
				RunProbe();
				return false;
			}),
			20.0f);
	});
