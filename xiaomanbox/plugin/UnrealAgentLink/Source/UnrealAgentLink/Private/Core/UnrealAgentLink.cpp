// Copyright Epic Games, Inc. All Rights Reserved.

#include "UnrealAgentLink.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UnrealAgentLinkStyle.h"
#include "UnrealAgentLinkCommands.h"
#include "Misc/MessageDialog.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "ToolMenus.h"
#include "UAL_NetworkManager.h"
#include "UAL_CommandHandler.h"
#include "UAL_LogInterceptor.h"
#include "UAL_AgentUndo.h"
#include "UAL_TouchedPackages.h"
#include "UAL_RegistryReady.h"
#include "UAL_EditorHealth.h"
#include "UAL_ContentBrowserExt.h"
#include "UAL_LevelViewportExt.h"
#include "UAL_AssetLockState.h"
#include "UAL_AssetLockBanner.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

static const FName UnrealAgentLinkTabName("UnrealAgentLink");

#define LOCTEXT_NAMESPACE "FUnrealAgentLinkModule"

void FUnrealAgentLinkModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	
	FUnrealAgentLinkStyle::Initialize();
	FUnrealAgentLinkStyle::ReloadTextures();

	FUnrealAgentLinkCommands::Register();
	
	PluginCommands = MakeShareable(new FUICommandList);

	PluginCommands->MapAction(
		FUnrealAgentLinkCommands::Get().PluginAction,
		FExecuteAction::CreateRaw(this, &FUnrealAgentLinkModule::PluginButtonClicked),
		FCanExecuteAction());

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUnrealAgentLinkModule::RegisterMenus));

	// 挂在命令处理器之前：它靠引擎的包标脏事件记录「哪些包是我们改的」，
	// 漏挂的话 editor.save 会永远认为什么都没改过
	FUAL_TouchedPackages::Initialize();

	// agent 的每一步进它自己那条撤销栈，不压在用户的撤销历史上。
	// 这里只挂「换图就清空」的事件，缓冲本身是第一条命令进来时才建的
	FUAL_AgentUndo::Initialize();

	// 挂资产注册表的扫描进度事件。读注册表的命令不再死等，注册表没扫完就回 503 +
	// 这里记下的进度，让盒子侧轮询 content.registry_status 把进度讲给用户
	FUAL_RegistryReady::Initialize();

	// 状态监控的采集器：启动耗时要赶在 OnFEngineLoopInitComplete 之前挂上，所以放在模块启动里
	FUAL_EditorHealth::Initialize();

	// 内容浏览器的「AI 正在改这个资产」角标 + 用户动手改时的提醒。
	// 挂在 TouchedPackages 之后：它要靠那边的命令作用域区分「agent 改的」和
	// 「用户改的」，只有后者才该弹提醒
	FUAL_AssetLockState::Initialize();

	// 编辑器窗口里的横幅：资产编辑器工具栏 + 关卡视口。
	// 用户真正会动手的地方是打开着的那个窗口，他不会回头看内容浏览器
	FUAL_AssetLockBanner::Initialize();

	// 初始化核心组件
	CommandHandler = MakeUnique<FUAL_CommandHandler>();
	LogInterceptor = MakeShared<FUAL_LogInterceptor>();
	ContentBrowserExt = MakeUnique<FUAL_ContentBrowserExt>();
	LevelViewportExt = MakeUnique<FUAL_LevelViewportExt>();

	if (GLog && LogInterceptor.IsValid())
	{
		GLog->AddOutputDevice(LogInterceptor.Get());
	}

	// 默认连接到本地 Agent。命令行 -UALServer=ws://host:port 可以改 ——
	// 给测试脚本用：盒子占着 17860 时，实验工程连到别的端口，两边互不打扰
	FString DefaultUrl = TEXT("ws://127.0.0.1:17860");
	FString OverrideUrl;
	if (FParse::Value(FCommandLine::Get(), TEXT("UALServer="), OverrideUrl) && !OverrideUrl.IsEmpty())
	{
		DefaultUrl = OverrideUrl;
	}
	FUAL_NetworkManager::Get().OnMessageReceived().AddRaw(this, &FUnrealAgentLinkModule::HandleSocketMessage);
	FUAL_NetworkManager::Get().OnConnected().AddRaw(this, &FUnrealAgentLinkModule::HandleSocketConnected);
	FUAL_NetworkManager::Get().OnDisconnected().AddRaw(this, &FUnrealAgentLinkModule::HandleSocketDisconnected);
	FUAL_NetworkManager::Get().Init(DefaultUrl);

	// 注册内容浏览器菜单扩展
	if (ContentBrowserExt)
	{
		ContentBrowserExt->Register();
	}

	// 注册视口Actor右键菜单扩展
	if (LevelViewportExt)
	{
		LevelViewportExt->Register();
	}
}

void FUnrealAgentLinkModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.

	// 在关闭连接前发送项目关闭事件
	if (FUAL_NetworkManager::Get().IsConnected())
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("ver"), TEXT("1.0"));
		Root->SetStringField(TEXT("type"), TEXT("evt"));
		Root->SetStringField(TEXT("method"), TEXT("project.closed"));
		
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("projectName"), FApp::GetProjectName());
		Payload->SetStringField(TEXT("projectPath"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
		Root->SetObjectField(TEXT("payload"), Payload);

		FString OutJson;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

		FUAL_NetworkManager::Get().SendMessage(OutJson);
		
		// 等待一小段时间确保消息发送完成
		FPlatformProcess::Sleep(0.1f);
	}

	UToolMenus::UnRegisterStartupCallback(this);

	UToolMenus::UnregisterOwner(this);

	FUnrealAgentLinkStyle::Shutdown();

	FUnrealAgentLinkCommands::Unregister();

	FUAL_NetworkManager::Get().OnMessageReceived().RemoveAll(this);
	FUAL_NetworkManager::Get().OnConnected().RemoveAll(this);
	FUAL_NetworkManager::Get().OnDisconnected().RemoveAll(this);
	FUAL_NetworkManager::Get().Shutdown();

	if (GLog && LogInterceptor.IsValid())
	{
		GLog->RemoveOutputDevice(LogInterceptor.Get());
	}
	LogInterceptor.Reset();
	CommandHandler.Reset();

	// 摘掉包标脏委托，别在模块卸载后留个指向已卸载代码的悬空回调
	FUAL_TouchedPackages::Shutdown();

	// 必须把全局撤销缓冲还给编辑器再撒手 —— 少了这一步，编辑器会带着一条
	// 即将被 GC 的缓冲继续跑，用户下一次 Ctrl+Z 就崩
	FUAL_AgentUndo::Shutdown();

	// 摘掉注册表进度委托，别留一个指向已卸载代码的回调
	FUAL_RegistryReady::Shutdown();
	FUAL_EditorHealth::Shutdown();

	// 摘掉内容浏览器的角标生成器和包标脏回调，同上。
	// 横幅要先摘 —— 它挂在视口上，绑的 lambda 指向本模块的代码
	FUAL_AssetLockBanner::Shutdown();
	FUAL_AssetLockState::Shutdown();

	if (ContentBrowserExt)
	{
		ContentBrowserExt->Unregister();
		ContentBrowserExt.Reset();
	}

	if (LevelViewportExt)
	{
		LevelViewportExt->Unregister();
		LevelViewportExt.Reset();
	}
}

void FUnrealAgentLinkModule::PluginButtonClicked()
{
	const FString Status = FUAL_NetworkManager::Get().IsConnected() ? TEXT("已连接") : TEXT("未连接");
	FText DialogText = FText::FromString(FString::Printf(TEXT("UnrealAgentLink 状态：%s"), *Status));
	FMessageDialog::Open(EAppMsgType::Ok, DialogText);
}

void FUnrealAgentLinkModule::RegisterMenus()
{
	// Owner will be used for cleanup in call to UToolMenus::UnregisterOwner
	FToolMenuOwnerScoped OwnerScoped(this);

	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
		{
			FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");
			Section.AddMenuEntryWithCommandList(FUnrealAgentLinkCommands::Get().PluginAction, PluginCommands);
		}
	}

	{
		UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
		{
			FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("PluginTools");
			{
				FToolMenuEntry& Entry = Section.AddEntry(FToolMenuEntry::InitToolBarButton(FUnrealAgentLinkCommands::Get().PluginAction));
				Entry.SetCommandList(PluginCommands);
			}
		}
	}
}

void FUnrealAgentLinkModule::HandleSocketMessage(const FString& Data)
{
	// UE 5.3+ 修复：使用 Ticker 而不是 AsyncTask 调度到 GameThread
	// AsyncTask(GameThread) 虽然在 GameThread 上执行，但仍处于 TaskGraph 调度上下文中
	// 当后续代码触发 Interchange 导入时，Interchange 内部也使用 TaskGraph，
	// 导致 TaskGraph 递归保护断言崩溃：++Queue(QueueIndex).RecursionGuard == 1
	// 使用 Ticker 确保代码在正常的 Tick 上下文中执行，完全脱离 TaskGraph
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this, Data](float DeltaTime) -> bool
		{
			if (CommandHandler)
			{
				CommandHandler->ProcessMessage(Data);
			}
			return false; // 只执行一次
		}),
		0.0f // 下一个 Tick 立即执行
	);
}

void FUnrealAgentLinkModule::HandleSocketDisconnected()
{
	// 盒子没了就没有任何东西还锁着，可角标不会自己消失 —— 不清的话用户屏幕上
	// 会留下一排永远擦不掉的「AI 锁定中」，那正是「盒子把我工程搞坏了」的观感。
	//
	// 委托在 Socket 线程触发，锁表是 GameThread 上读的（Slate 绘制），
	// 所以照其他两个回调的老规矩切回去再动。
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float DeltaTime) -> bool
		{
			FUAL_AssetLockState::Clear();
			return false; // 只执行一次
		}),
		0.0f);
}

void FUnrealAgentLinkModule::HandleSocketConnected()
{
	// 同样使用 Ticker 切回 GameThread，保持一致性
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this](float DeltaTime) -> bool
		{
			if (!CommandHandler)
			{
				return false;
			}

			const TSharedPtr<FJsonObject> Payload = CommandHandler->BuildProjectInfo();
			if (!Payload.IsValid())
			{
				return false;
			}

			TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("ver"), TEXT("1.0"));
			Root->SetStringField(TEXT("type"), TEXT("evt"));
			Root->SetStringField(TEXT("method"), TEXT("project.info"));
			Root->SetObjectField(TEXT("payload"), Payload);

			FString OutJson;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
			FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

			FUAL_NetworkManager::Get().SendMessage(OutJson);
			return false; // 只执行一次
		}),
		0.0f
	);
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FUnrealAgentLinkModule, UnrealAgentLink)