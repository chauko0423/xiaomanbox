#include "UAL_EditorCommands.h"
#include "UAL_CommandUtils.h"
// 试玩报告里的关卡组成走它 —— 那份 JSON 与 level.list 必须是同一份，
// 抄一遍的话两条命令迟早会对同一件事给出两个答案
#include "UAL_LevelCommands.h"
#include "UAL_TouchedPackages.h"
#include "UAL_FocusContext.h"
#include "UAL_SavablePackage.h"
#include "UAL_WindowCapture.h"
#include "UAL_ScopedDialogAutoAnswer.h"

#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "IPythonScriptPlugin.h"
#include "ObjectTools.h"
#include "UnrealEdMisc.h"
#include "Containers/Ticker.h"
// PIE 采样帧量抓帧耗时用（每条退出路径都要记，包括失败那条）。
// Core 模块下，5.0–5.8 九版路径一致，不新增模块依赖
#include "Misc/ScopeExit.h"
#include "HAL/PlatformMemory.h"
#include "UObject/GarbageCollection.h"
#include "PlayInEditorDataTypes.h"
#include "GameFramework/PlayerController.h"
// PIE 截图取玩家相机 FOV 用（PlayerController.h 里只有前向声明）
#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"

#include "Editor.h"
#include "Engine/World.h"
#include "LevelEditor.h"
#include "SLevelViewport.h"
#include "LevelEditorViewport.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Brushes/SlateColorBrush.h"
#include "Styling/CoreStyle.h"
#include "Internationalization/Culture.h"
#include "Widgets/Docking/SDockTab.h"
#include "Framework/Docking/TabManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Slate/SceneViewport.h"
#include "HighResScreenshot.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "Misc/ConfigCacheIni.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/EnumProperty.h"
#include "Interfaces/Interface_PostProcessVolume.h"
#include "HAL/IConsoleManager.h"
// BuildProjectInfo 里的 runMode 用 FApp::IsUnattended / FApp::CanEverRender，
// IsRunningCommandlet 在 CoreGlobals。两个都在 Core 模块下，不新增模块依赖
#include "Misc/App.h"
#include "CoreGlobals.h"
#include "Interfaces/IPluginManager.h"
// BuildProjectInfo 里的 hasCode 用它。GameProjectGeneration 本来就在
// PrivateDependencyModuleNames 里，这里不新增任何模块依赖
#include "GameProjectUtils.h"
#include "GameMapsSettings.h"
#include "UAL_VersionCompat.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Slate/WidgetRenderer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
// 截图前的三道「等到位」：等资产编译、等着色器编译、把流送冲干净。
// 三个头文件都在已依赖的 Engine 模块下（5.0–5.8 九版路径一致），不新增模块依赖
#include "AssetCompilingManager.h"
#include "ShaderCompiler.h"
#include "ContentStreaming.h"
#include "EngineUtils.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Engine/Blueprint.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
// FLevelUtils::IsLevelVisible 用在 viewport.focus 的隐藏关卡预检那段。
// 在 Runtime/Engine/Public 下（不是 UnrealEd），我们已经依赖 Engine 模块
#include "LevelUtils.h"
// viewport.focus 看局部时自己定机位要用：MakeFromX 从方向向量直接得到朝向
#include "Math/RotationMatrix.h"
// 视线遮挡检测（LineTraceSingleByChannel）
#include "CollisionQueryParams.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUALEditor, Log, All);

/**
 * 截图任务上下文结构体（用于异步定时器回调）
 */
struct FScreenshotTaskContext
{
	FString RequestId;
	FString ScreenshotDir;
	TMap<FString, FDateTime> FilesBeforeMap;
	FDateTime CommandExecuteTime;
	int32 Width;
	int32 Height;
	int32 RetryCount;
	int32 MaxRetries;
	FTimerHandle TimerHandle;
	/** 拍的时候 PIE 在不在跑："pie" 或 "editor"。HighResShot 拍的是当前视口，
	 *  PIE 一开那就是游戏画面 —— 调用方要能分清自己拿到的是哪一个 */
	FString World;
};

// 静态任务列表（存储正在进行的截图任务）
static TMap<FString, TSharedPtr<FScreenshotTaskContext>> PendingScreenshotTasks;

/**
 * 定时器回调：检查截图文件是否生成
 */
static void CheckScreenshotFile(FString TaskId)
{
	TSharedPtr<FScreenshotTaskContext>* ContextPtr = PendingScreenshotTasks.Find(TaskId);
	if (!ContextPtr || !ContextPtr->IsValid())
	{
		UE_LOG(LogUALEditor, Warning, TEXT("Screenshot task %s not found"), *TaskId);
		return;
	}
	
	TSharedPtr<FScreenshotTaskContext> Context = *ContextPtr;
	Context->RetryCount++;
	
	UE_LOG(LogUALEditor, Log, TEXT("Checking screenshot... retry %d/%d"), Context->RetryCount, Context->MaxRetries);
	
	// 查找新生成的截图文件
	TArray<FString> FilesAfter;
	IFileManager::Get().FindFiles(FilesAfter, *FPaths::Combine(Context->ScreenshotDir, TEXT("HighresScreenshot*.png")), true, false);
	
	UE_LOG(LogUALEditor, Log, TEXT("Found %d files in dir, BeforeMap has %d entries, CommandTime: %s"), 
		FilesAfter.Num(), Context->FilesBeforeMap.Num(), *Context->CommandExecuteTime.ToString());
	
	FString NewScreenshotPath;
	
	// 策略 1: 优先查找新增的文件名
	for (const FString& File : FilesAfter)
	{
		if (!Context->FilesBeforeMap.Contains(File))
		{
			FString FullPath = FPaths::Combine(Context->ScreenshotDir, File);
			FDateTime FileTime = IFileManager::Get().GetTimeStamp(*FullPath);
			int64 FileSize = IFileManager::Get().FileSize(*FullPath);
			
			UE_LOG(LogUALEditor, Log, TEXT("New file candidate: %s, FileTime: %s, Size: %lld, CommandTime: %s"), 
				*File, *FileTime.ToString(), FileSize, *Context->CommandExecuteTime.ToString());
			
			if (FileTime >= Context->CommandExecuteTime && FileSize > 0)
			{
				NewScreenshotPath = FullPath;
				UE_LOG(LogUALEditor, Log, TEXT("Found NEW file: %s (size: %lld)"), *File, FileSize);
				break;
			}
			else
			{
				UE_LOG(LogUALEditor, Warning, TEXT("Skipping file %s: FileTime < CommandTime or Size <= 0"), *File);
			}
		}
	}
	
	// 策略 2: 查找时间戳被更新的文件
	if (NewScreenshotPath.IsEmpty())
	{
		for (const FString& File : FilesAfter)
		{
			FString FullPath = FPaths::Combine(Context->ScreenshotDir, File);
			FDateTime CurrentTime = IFileManager::Get().GetTimeStamp(*FullPath);
			FDateTime* PreviousTime = Context->FilesBeforeMap.Find(File);
			
			if (PreviousTime && CurrentTime > *PreviousTime && CurrentTime >= Context->CommandExecuteTime)
			{
				int64 FileSize = IFileManager::Get().FileSize(*FullPath);
				if (FileSize > 0)
				{
					NewScreenshotPath = FullPath;
					UE_LOG(LogUALEditor, Log, TEXT("Found UPDATED file: %s (size: %lld)"), *File, FileSize);
					break;
				}
			}
		}
	}
	
	// 找到文件 - 发送成功响应
	if (!NewScreenshotPath.IsEmpty())
	{
		UE_LOG(LogUALEditor, Log, TEXT("Screenshot captured: %s"), *NewScreenshotPath);
		
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("path"), NewScreenshotPath);
		Data->SetStringField(TEXT("filename"), FPaths::GetCleanFilename(NewScreenshotPath));
		Data->SetNumberField(TEXT("width"), Context->Width);
		Data->SetNumberField(TEXT("height"), Context->Height);
		Data->SetBoolField(TEXT("saved"), true);
		Data->SetStringField(TEXT("world"), Context->World);
		Data->SetStringField(TEXT("view"), TEXT("viewport"));
		Data->SetBoolField(TEXT("restore_app_window"), true); // 通知客户端恢复应用窗口
		
		UAL_CommandUtils::SendResponse(Context->RequestId, 200, Data);
		
		// 清理定时器和任务
		if (GEditor)
		{
			GEditor->GetTimerManager()->ClearTimer(Context->TimerHandle);
		}
		PendingScreenshotTasks.Remove(TaskId);
		return;
	}
	
	// 超时 - 发送错误响应
	if (Context->RetryCount >= Context->MaxRetries)
	{
		UE_LOG(LogUALEditor, Error, TEXT("Screenshot timeout after %d retries"), Context->MaxRetries);
		UAL_CommandUtils::SendError(Context->RequestId, 500, TEXT("截图超时：HighResShot 未生成截图文件。请确保已打开一个关卡/场景（Level）视口并置于前台，而非材质、蓝图等编辑器窗口。"));
		
		// 清理定时器和任务
		if (GEditor)
		{
			GEditor->GetTimerManager()->ClearTimer(Context->TimerHandle);
		}
		PendingScreenshotTasks.Remove(TaskId);
		return;
	}
	
	// 继续等待（定时器会自动触发下一次检查）
}
void FUAL_EditorCommands::RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("editor.screenshot"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_TakeScreenshot(Payload, RequestId);
	});

	// 别名 `take_screenshot` 已删（2026-09-16）：和上面 editor.screenshot 同一个
	// handler，不带域前缀，盒子这边从来没调过

	CommandMap.Add(TEXT("project.info"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetProjectInfo(Payload, RequestId);
	});

	// 兼容别名：早期/上层工具可能使用 editor.get_project_info
	CommandMap.Add(TEXT("editor.get_project_info"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetProjectInfo(Payload, RequestId);
	});

	CommandMap.Add(TEXT("project.get_config"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetConfig(Payload, RequestId);
	});

	CommandMap.Add(TEXT("project.set_config"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetConfig(Payload, RequestId);
	});

	CommandMap.Add(TEXT("project.analyze_uproject"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_AnalyzeUProject(Payload, RequestId);
	});

	CommandMap.Add(TEXT("editor.capture_app_window"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CaptureAppWindow(Payload, RequestId);
	});

	// editor.get_focus_context - 获取当前焦点编辑器上下文（蓝图/材质/关卡等）
	CommandMap.Add(TEXT("editor.get_focus_context"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetFocusContext(Payload, RequestId);
	});

	// editor.save - 把改动落盘（默认只存本插件改过的）
	CommandMap.Add(TEXT("editor.save"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_Save(Payload, RequestId);
	});

	// editor.get_dirty - 现在有哪些没存的改动
	CommandMap.Add(TEXT("editor.get_dirty"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetDirty(Payload, RequestId);
	});

	// editor.collect_garbage - 强制回收，报出回收了多少
	CommandMap.Add(TEXT("editor.collect_garbage"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CollectGarbage(Payload, RequestId);
	});

	// editor.restart - 重启编辑器（会掐断本连接，响应先发后重启）
	CommandMap.Add(TEXT("editor.restart"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_RestartEditor(Payload, RequestId);
	});

	// pie.run - 跑一次试玩，回一份「发生了什么」
	CommandMap.Add(TEXT("pie.run"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_RunPlaytest(Payload, RequestId);
	});

	// pie.stop - 让 pie.run 提前收尾（报告照常由 pie.run 那个请求回）
	CommandMap.Add(TEXT("pie.stop"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_StopPlaytest(Payload, RequestId);
	});

	// viewport.focus - 把视口镜头对准指定 Actor（等同于选中后按 F）
	CommandMap.Add(TEXT("viewport.focus"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_FocusViewport(Payload, RequestId);
	});
}

// ========== 从 UAL_CommandHandler.cpp 迁移以下函数 ==========
// 原始行号参考:
//   Handle_TakeScreenshot: 1984-2142
//   Handle_GetProjectInfo: 2144-2148
//   BuildProjectInfo:      2150-2278

/**
 * 正在跑的 PIE 世界。没在跑就是 nullptr。
 *
 * 定义放在这里而不是 pie.run 那一段：截图也要用它 —— PIE 一开，
 * 用户看的就是游戏世界，截编辑器世界等于答非所问。
 */
static UWorld* UAL_GetPlayWorld()
{
	if (!GEngine)
	{
		return nullptr;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE && Context.World())
		{
			return Context.World();
		}
	}
	return nullptr;
}

/**
 * 玩家此刻看到的机位。拿不到就返回 false，由调用方回落到视口相机。
 *
 * Simulate In Editor 没有玩家控制器，这时候回落是对的 —— 那种模式下
 * 用户看的本来就是编辑器视口那台相机，只是世界换成了游戏世界。
 */
static bool UAL_TryGetPlayerViewPoint(
	UWorld* PlayWorld,
	TOptional<FVector>& OutLocation,
	TOptional<FRotator>& OutRotation,
	TOptional<float>& OutFOV)
{
	APlayerController* PC = PlayWorld ? PlayWorld->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return false;
	}

	FVector Location;
	FRotator Rotation;
	PC->GetPlayerViewPoint(Location, Rotation);
	OutLocation = Location;
	OutRotation = Rotation;
	// 游戏相机的 FOV 常常和编辑器视口那台不一样（瞄准镜、过场、载具视角），
	// 不带上的话构图会对不上
	if (PC->PlayerCameraManager)
	{
		OutFOV = PC->PlayerCameraManager->GetFOVAngle();
	}
	return true;
}

/**
 * 用 SceneCapture2D 主动渲染一帧，不依赖编辑器视口重绘。
 *
 * ## 为什么要有这条路
 *
 * HighResShot 的本质是「排队等视口下一次重绘」。编辑器窗口没有焦点时虚幻不
 * 出新帧，那一帧永远不来 —— 真机上请求并没有丢，而是在用户自己点回编辑器
 * 之后 90 秒才拍成。盒子那侧试遍了 SetForegroundWindow、AttachThreadInput、
 * 关节流、解限帧，没有一个可靠：Windows 不允许后台进程真正抢焦点。
 *
 * SceneCapture 走的是另一条路：在游戏线程上**主动**把场景渲染到一张
 * RenderTarget，跟窗口可见性、焦点、节流都无关。失焦、被遮挡、最小化都能出图，
 * 而且是同步返回，不需要轮询目录找新文件。
 *
 * ## 与 HighResShot 的差别
 *
 * 它拍的是**场景**，不含编辑器 UI（大纲、细节面板、网格线、图标）。
 * 需要连界面一起拍时仍然走 HighResShot —— 那种需求本来就要求窗口可见。
 *
 * @return 成功则填好 OutPath 返回 true；失败返回 false 并填 OutError
 */
/**
 * 一次 SceneCapture 作业建好的东西。
 *
 * 拆成 Setup / Finish 两半，是为了让预热帧能**跨引擎帧**渲染 ——
 * 理由见 UAL_TickAsyncCapture 上面那段。PIE 收尾那条路等不到下一帧
 * （世界马上销毁），仍然走同步的 CaptureSceneToFile。
 */
struct FUALSceneCaptureSetup
{
	USceneCaptureComponent2D* Capture = nullptr;
	UTextureRenderTarget2D* RenderTarget = nullptr;
	/** 这次用的机位。注册流送视图时还要用一次，别再去问视口（可能已经被用户动过） */
	FVector ViewLocation = FVector::ZeroVector;
	FRotator ViewRotation = FRotator::ZeroRotator;
	float ViewFOV = 90.f;

	/**
	 * 机位是从哪儿来的。**必须回给调用方**。
	 *
	 * 取不到透视视口时这里会退回一台固定相机（原点上方 5 米，俯角 30°），
	 * 截图照样成功、照样回 `saved: true` —— 调用方拿到的是一张构图完全随机的图，
	 * 却没有任何迹象说明镜头不是它以为的那个。实测里就是这么白走一整轮的：
	 * 拿这张图去核对摆放，得出「东西不在场景里」的结论，然后去修一个不存在的问题。
	 *
	 * viewport = 用了编辑器视口那台相机；player = PIE 玩家视角；
	 * fallback = 没找到视口，用的是兜底机位，**画面内容不可据此下结论**。
	 */
	const TCHAR* CameraSource = TEXT("viewport");

	/**
	 * 这一帧的曝光是锁住的还是自动的，以及是谁定的。**也必须回给调用方**。
	 *
	 * 自动曝光下截图、视口、游戏各自收敛到不同亮度，还会把压暗的场景拉回来。
	 * 2026-09-26 科幻塔防：关卡美术在自动曝光下调了四轮灯，每张图都发白，最后认定
	 * 「是截图的偏差」收工 —— 它不知道这张图是不是自动曝光，只能猜。
	 *
	 * Exposure：manual（锁住了，明暗可信）/ auto。
	 * ExposureSource：viewport（编辑器视口自己的固定曝光）/ post_process_volume /
	 * project_setting（项目设置里关了自动曝光或设成手动）/ default（引擎默认的自动曝光）。
	 * 相机组件自己的后期不在这里看 —— PIE 时玩家相机若另设了曝光，以这里为下限参考。
	 */
	const TCHAR* Exposure = TEXT("auto");
	const TCHAR* ExposureSource = TEXT("default");
};

/**
 * 这台相机此刻吃到的曝光设置：先看编辑器视口的固定曝光，再看罩住机位的后期盒子
 * （优先级最高的那个说了算），最后看项目设置。
 */
static void UAL_ResolveExposure(
	UWorld* World,
	const FVector& ViewLocation,
	const FEditorViewportClient* SourceViewport,
	bool bEditorWorldCapture,
	FUALSceneCaptureSetup& OutSetup)
{
	if (bEditorWorldCapture && SourceViewport && SourceViewport->ExposureSettings.bFixed)
	{
		OutSetup.Exposure = TEXT("manual");
		OutSetup.ExposureSource = TEXT("viewport");
		return;
	}

	bool bFound = false;
	float BestPriority = 0.f;
	bool bBestManual = false;
	for (IInterface_PostProcessVolume* Volume : World->PostProcessVolumes)
	{
		if (!Volume)
		{
			continue;
		}
		const FPostProcessVolumeProperties Props = Volume->GetProperties();
		if (!Props.bIsEnabled || Props.BlendWeight <= 0.f || !Props.Settings)
		{
			continue;
		}
		if (!Props.bIsUnbound && !Volume->EncompassesPoint(ViewLocation, 0.f, nullptr))
		{
			continue;
		}
		const FPostProcessSettings& S = *Props.Settings;
		const bool bSaysMethod = S.bOverride_AutoExposureMethod;
		const bool bLocksRange = S.bOverride_AutoExposureMinBrightness && S.bOverride_AutoExposureMaxBrightness
			&& FMath::IsNearlyEqual(S.AutoExposureMinBrightness, S.AutoExposureMaxBrightness);
		if (!bSaysMethod && !bLocksRange)
		{
			continue;
		}
		if (!bFound || Props.Priority > BestPriority)
		{
			bFound = true;
			BestPriority = Props.Priority;
			bBestManual = (bSaysMethod && S.AutoExposureMethod == EAutoExposureMethod::AEM_Manual) || bLocksRange;
		}
	}
	if (bFound)
	{
		OutSetup.Exposure = bBestManual ? TEXT("manual") : TEXT("auto");
		OutSetup.ExposureSource = TEXT("post_process_volume");
		return;
	}

	static IConsoleVariable* DefaultAuto = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DefaultFeature.AutoExposure"));
	static IConsoleVariable* DefaultMethod = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DefaultFeature.AutoExposure.Method"));
	if ((DefaultAuto && DefaultAuto->GetInt() == 0) || (DefaultMethod && DefaultMethod->GetInt() == 2))
	{
		OutSetup.Exposure = TEXT("manual");
		OutSetup.ExposureSource = TEXT("project_setting");
	}
}

/**
 * 建好 RenderTarget 和 SceneCapture 组件，摆好机位和曝光。**不渲染**。
 *
 * `InWorld` 留空就截编辑器世界。PIE 在跑时要传游戏世界（`UAL_GetPlayWorld()`）
 * —— 编辑器世界和游戏世界是两个 World，拿错了截出来的是没在跑的那个场景，
 * 画面看着正常但和游戏里发生的事无关。
 *
 * `InViewLocation`/`InViewRotation`/`InViewFOV` 同理：PIE 时要的是
 * **玩家看到的**画面，而不是编辑器视口那台相机。给了就用给的，
 * 没给才回落到视口相机。
 */
static bool UAL_SetupSceneCapture(
	int32 Width,
	int32 Height,
	FUALSceneCaptureSetup& OutSetup,
	FString& OutError,
	UWorld* InWorld = nullptr,
	const TOptional<FVector>& InViewLocation = TOptional<FVector>(),
	const TOptional<FRotator>& InViewRotation = TOptional<FRotator>(),
	const TOptional<float>& InViewFOV = TOptional<float>())
{
#if WITH_EDITOR
	if (!GEditor)
	{
		OutError = TEXT("GEditor 不可用");
		return false;
	}

	UWorld* World = InWorld ? InWorld : GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		OutError = TEXT("拿不到世界（没有打开关卡？）");
		return false;
	}

	// 编辑器视口的曝光设置只对编辑器世界成立。截游戏世界时它是另一套后处理
	// （PIE 里的 PostProcessVolume、相机自己的设置），照抄视口的固定曝光会
	// 把画面调成和玩家看到的不一样
	const bool bEditorWorldCapture = (World == GEditor->GetEditorWorldContext().World());

	// 取当前透视视口的相机，让截出来的画面和用户看到的一致。
	// 拿不到就退回世界原点上方，总比直接失败强。
	FVector ViewLocation(0.f, 0.f, 500.f);
	FRotator ViewRotation(-30.f, 0.f, 0.f);
	float ViewFOV = 90.f;
	FEditorViewportClient* SourceViewport = nullptr;

	if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->IsPerspective())
	{
		SourceViewport = GCurrentLevelEditingViewportClient;
	}
	else
	{
		for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
		{
			if (Client && Client->IsPerspective())
			{
				SourceViewport = Client;
				break;
			}
		}
	}

	if (SourceViewport)
	{
		ViewLocation = SourceViewport->GetViewLocation();
		ViewRotation = SourceViewport->GetViewRotation();
		ViewFOV = SourceViewport->ViewFOV;
	}

	// 调用方显式给的相机优先（PIE 时是玩家视角）。放在视口那段之后覆盖，
	// 这样 FOV 之类没被覆盖的字段仍然沿用视口的合理值。
	if (InViewLocation.IsSet())
	{
		ViewLocation = InViewLocation.GetValue();
	}
	if (InViewRotation.IsSet())
	{
		ViewRotation = InViewRotation.GetValue();
	}
	if (InViewFOV.IsSet())
	{
		ViewFOV = InViewFOV.GetValue();
	}

	const bool bFoundViewport = SourceViewport != nullptr;
	if (!bFoundViewport)
	{
		UE_LOG(LogUALEditor, Warning, TEXT("SceneCapture: no perspective viewport, using fallback camera"));
	}

	// 机位来源要记下来带回去，不能只写进引擎日志 —— 调用方读不到那份日志。
	// 调用方显式给了机位（PIE 玩家视角）时，找没找到视口都无关紧要。
	OutSetup.CameraSource =
		InViewLocation.IsSet() ? TEXT("player") : (bFoundViewport ? TEXT("viewport") : TEXT("fallback"));
	UAL_ResolveExposure(World, ViewLocation, SourceViewport, bEditorWorldCapture, OutSetup);

	// RenderTarget：RGBA8_SRGB 出来的就是常规 8 位色，直接能编码成 PNG
	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
	if (!RenderTarget)
	{
		OutError = TEXT("创建 RenderTarget 失败");
		return false;
	}
	RenderTarget->RenderTargetFormat = RTF_RGBA8_SRGB;
	RenderTarget->ClearColor = FLinearColor::Black;
	RenderTarget->bAutoGenerateMips = false;
	RenderTarget->InitAutoFormat(Width, Height);
	RenderTarget->UpdateResourceImmediate(true);

	// 用独立组件而不是 SpawnActor：spawn 出来的 SceneCapture2D 会出现在大纲视图里，
	// 哪怕是 transient 也会闪一下，而且要负责销毁。组件注册到世界即可，不进大纲。
	USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(World);
	if (!Capture)
	{
		OutError = TEXT("创建 SceneCaptureComponent2D 失败");
		return false;
	}
	Capture->RegisterComponentWithWorld(World);
	Capture->SetWorldLocationAndRotation(ViewLocation, ViewRotation);
	Capture->TextureTarget = RenderTarget;
	Capture->FOVAngle = ViewFOV;
	// FinalColorLDR：已经过完整后处理和色调映射，最接近用户在视口里看到的样子
	Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	Capture->bAlwaysPersistRenderingState = true;

	// 曝光要照抄视口的，否则画面明暗和用户看到的不是一回事。
	//
	// 编辑器视口默认用**固定曝光**（视口自己的 ExposureSettings），而
	// SceneCapture 走的是引擎默认的**自动曝光**。这个关卡里一个
	// PostProcessVolume 都没有（真机查过），没有任何东西让两边对齐 ——
	// 实测 SceneCapture 比视口暗 20% 左右（平均亮度 143 vs 180）。
	//
	// 一开始以为是眼适应没收敛，改成连渲两帧，没有任何改善 —— 那个假设是错的。
	// 真正的原因是两边压根用的不是同一套曝光。
	if (bEditorWorldCapture && SourceViewport && SourceViewport->ExposureSettings.bFixed)
	{
		FPostProcessSettings& PP = Capture->PostProcessSettings;
		PP.bOverride_AutoExposureMethod = true;
		PP.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
		PP.bOverride_AutoExposureBias = true;
		PP.AutoExposureBias = SourceViewport->ExposureSettings.FixedEV100;
		// min == max 才是真的把自适应锁死，只设 Method 不够
		PP.bOverride_AutoExposureMinBrightness = true;
		PP.AutoExposureMinBrightness = 1.0f;
		PP.bOverride_AutoExposureMaxBrightness = true;
		PP.AutoExposureMaxBrightness = 1.0f;
		UE_LOG(LogUALEditor, Log, TEXT("SceneCapture: mirroring viewport fixed exposure EV100=%.2f"),
			SourceViewport->ExposureSettings.FixedEV100);
	}

	else
	{
		// 视口也在用自动曝光时，问题变成**收敛速度**：视口已经跑了成百上千帧、
		// 眼适应早就稳定了，而这个组件是这次新建的，从零开始。
		//
		// 真机踩过的两个坑都记在这里，别再重来：
		//   - 「渲两帧」不够。试过，亮度纹丝不动（143 vs 180）。
		//   - 抄视口的 ExposureSettings 也没用：它的 bFixed 是 false，
		//     根本没有固定曝光可抄（日志里那行 mirroring 从来没打印过）。
		//
		// 把适应速度调到极大，一帧之内直接收敛到目标曝光，再取第二帧。
		FPostProcessSettings& PP = Capture->PostProcessSettings;
		PP.bOverride_AutoExposureSpeedUp = true;
		PP.AutoExposureSpeedUp = 100.f;
		PP.bOverride_AutoExposureSpeedDown = true;
		PP.AutoExposureSpeedDown = 100.f;
	}

	OutSetup.Capture = Capture;
	OutSetup.RenderTarget = RenderTarget;
	OutSetup.ViewLocation = ViewLocation;
	OutSetup.ViewRotation = ViewRotation;
	OutSetup.ViewFOV = ViewFOV;
	return true;
#else
	OutError = TEXT("SceneCapture 截图仅在编辑器模式可用");
	return false;
#endif
}

/**
 * 把 RenderTarget 读回来编码成 PNG 存盘，并销毁组件。
 *
 * 无论成败都会清理 Setup 里的东西 —— 调用方拿到返回值之后 Setup 就作废了。
 */
static bool UAL_FinishSceneCapture(
	FUALSceneCaptureSetup& Setup,
	int32 Width,
	int32 Height,
	FString& OutPath,
	FString& OutError,
	const FString& NameHint = FString())
{
#if WITH_EDITOR
	USceneCaptureComponent2D* Capture = Setup.Capture;
	UTextureRenderTarget2D* RenderTarget = Setup.RenderTarget;
	Setup.Capture = nullptr;
	Setup.RenderTarget = nullptr;

	if (!Capture || !RenderTarget)
	{
		OutError = TEXT("SceneCapture 作业已失效");
		return false;
	}

	// 跨帧那条路把两个对象都 AddToRoot 过（否则中途会被 GC 掉）。
	// 同步那条路没有 root，RemoveFromRoot 对没 root 的对象是无害的空操作
	RenderTarget->RemoveFromRoot();
	Capture->RemoveFromRoot();

	FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
	if (!Resource)
	{
		Capture->DestroyComponent();
		OutError = TEXT("RenderTarget 资源不可用");
		return false;
	}

	TArray<FColor> Pixels;
	FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
	ReadFlags.SetLinearToGamma(false);
	const bool bRead = Resource->ReadPixels(Pixels, ReadFlags);

	Capture->DestroyComponent();

	if (!bRead || Pixels.Num() == 0)
	{
		OutError = TEXT("从 RenderTarget 读回像素失败");
		return false;
	}

	// SceneCapture 出来的 alpha 常常是 0，直接编码会得到一张全透明的 PNG ——
	// 看起来"截图成功"，打开却是空白。统一压成不透明。
	for (FColor& Pixel : Pixels)
	{
		Pixel.A = 255;
	}

	IImageWrapperModule& ImageWrapperModule =
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!PngWrapper.IsValid() ||
		!PngWrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
	{
		OutError = TEXT("PNG 编码失败");
		return false;
	}

	const FString Dir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots/UAL")));
	IFileManager::Get().MakeDirectory(*Dir, true);
	// 文件名只精确到**秒**。一次试玩里按时间抓的几帧常常落在同一秒内，
	// 没有这个后缀它们会互相覆盖 —— 而覆盖之后一切照常成功：路径都在、
	// 文件也在，只是九个路径指向同一张图，拼出来是九格一模一样的画面，
	// 于是模型得出「整段时间里什么都没动」。调用方必须给得出区分的后缀。
	const FString Suffix = NameHint.IsEmpty() ? FString() : FString::Printf(TEXT("_%s"), *NameHint);
	const FString FileName = FString::Printf(TEXT("UAShot_%s%s.png"),
		*FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")), *Suffix);
	const FString FullPath = FPaths::Combine(Dir, FileName);

	if (!FFileHelper::SaveArrayToFile(PngWrapper->GetCompressed(100), *FullPath))
	{
		OutError = FString::Printf(TEXT("写文件失败：%s"), *FullPath);
		return false;
	}

	OutPath = FullPath;
	UE_LOG(LogUALEditor, Log, TEXT("SceneCapture screenshot saved: %s"), *FullPath);
	return true;
#else
	OutError = TEXT("SceneCapture 截图仅在编辑器模式可用");
	return false;
#endif
}

/**
 * 同步版：建组件、连渲两帧、读回存盘，全在一次调用里做完。
 *
 * **只有 PIE 收尾还该用它** —— 那里下一句就是 RequestEndPlayMap()，
 * 游戏世界马上销毁，等不到下一个引擎帧。代价是拿不到跨帧的预热
 * （连调 CaptureScene() 为什么没用，见 UAL_TickAsyncCapture），
 * 也来不及等编译和流送。试玩截图是「事后看一眼跑成什么样」，认这个代价。
 *
 * 普通截图走 Handle_TakeScreenshot 里的跨帧异步路径。
 */
static bool CaptureSceneToFile(
	int32 Width,
	int32 Height,
	FString& OutPath,
	FString& OutError,
	UWorld* InWorld = nullptr,
	const TOptional<FVector>& InViewLocation = TOptional<FVector>(),
	const TOptional<FRotator>& InViewRotation = TOptional<FRotator>(),
	const TOptional<float>& InViewFOV = TOptional<float>(),
	const FString& NameHint = FString())
{
	FUALSceneCaptureSetup Setup;
	if (!UAL_SetupSceneCapture(
		Width, Height, Setup, OutError, InWorld, InViewLocation, InViewRotation, InViewFOV))
	{
		return false;
	}

	Setup.Capture->CaptureScene();
	Setup.Capture->CaptureScene();

	return UAL_FinishSceneCapture(Setup, Width, Height, OutPath, OutError, NameHint);
}

// ============================================================================
// 跨帧异步截图：等编译 → 冲流送 → 预热若干帧 → 读回
// ============================================================================

bool FUAL_EditorCommands::CaptureAnimationPreview(UWorld* World, const FVector& Location, const FRotator& Rotation, FString& Path, FString& Error)
{
    FUALSceneCaptureSetup Setup;
    if (!UAL_SetupSceneCapture(1280, 720, Setup, Error, World, Location, Rotation, 45.0f)) return false;
    // A fresh preview has no exposure history. Fixed exposure makes poses comparable across calls.
    FPostProcessSettings& PP = Setup.Capture->PostProcessSettings;
    PP.bOverride_AutoExposureMethod = true; PP.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
    PP.bOverride_AutoExposureApplyPhysicalCameraExposure = true; PP.AutoExposureApplyPhysicalCameraExposure = false;
    PP.bOverride_AutoExposureBias = true; PP.AutoExposureBias = 1.0f;
    PP.bOverride_AutoExposureMinBrightness = true; PP.AutoExposureMinBrightness = 1.0f;
    PP.bOverride_AutoExposureMaxBrightness = true; PP.AutoExposureMaxBrightness = 1.0f;
    Setup.Capture->ShowFlags.SetAntiAliasing(false);
    Setup.Capture->CaptureScene();
    return UAL_FinishSceneCapture(Setup, 1280, 720, Path, Error);
}

#if WITH_EDITOR

/** 老路（定义在 Handle_TakeScreenshot 后面）：SceneCapture 建不起来时的回落 */
static void UAL_StartHighResShot(int32 Width, int32 Height, const FString& WorldLabel, const FString& RequestId);

/** 等着色器/资产编译的上限。到点就照拍，并在响应里说清楚还剩多少没编完 */
static constexpr double UAL_ScreenshotCompileWaitSeconds = 10.0;
/** StreamAllResources 的阻塞上限 */
static constexpr float UAL_ScreenshotStreamWaitSeconds = 2.0f;
/** 预热帧默认值（最终那帧之外还要多渲几帧） */
static constexpr int32 UAL_ScreenshotDefaultWarmupFrames = 4;
/** 整个作业的兜底截止，防止引擎 tick 停了之后请求永远不回 */
static constexpr double UAL_ScreenshotJobDeadlineSeconds = 30.0;

/** 一次异步截图作业的全部状态 */
struct FUALAsyncCaptureJob
{
	FString RequestId;
	int32 Width = 1920;
	int32 Height = 1080;
	FString WorldLabel;
	bool bPlayerView = false;

	TWeakObjectPtr<UWorld> World;
	TOptional<FVector> ViewLocation;
	TOptional<FRotator> ViewRotation;
	TOptional<float> ViewFOV;

	/** 已经建好组件、进入预热阶段了吗 */
	bool bCapturing = false;
	/** 请求的预热帧数，只用来回给调用方 */
	int32 WarmupFrames = 0;
	int32 WarmupFramesLeft = 0;
	FUALSceneCaptureSetup Setup;

	double StartTime = 0.0;
	double CompileDeadline = 0.0;
	double JobDeadline = 0.0;

	/** 开拍那一刻的编译/流送残留，原样回给调用方 */
	int32 PendingAssets = 0;
	int32 PendingShaders = 0;
	int32 StreamingInFlight = 0;
	double WaitSeconds = 0.0;

	FTSTicker::FDelegateHandle Ticker;
};

/** 还有多少资产在编译。分两个管理器：AssetCompilingManager 不管着色器 */
static void UAL_GetPendingCompiles(int32& OutAssets, int32& OutShaders)
{
	OutAssets = FAssetCompilingManager::Get().GetNumRemainingAssets();
	OutShaders = GShaderCompilingManager ? GShaderCompilingManager->GetNumRemainingJobs() : 0;
}

/**
 * 跨帧推进一次截图作业。
 *
 * ## 为什么预热帧必须分散到不同的引擎帧
 *
 * 曾经的做法是在一个函数里连调两次 `CaptureScene()`，注释里写着
 * 「第一帧把眼适应历史建起来，第二帧才是稳定画面」。实测毫无改善，
 * 而引擎源码解释了原因：
 *
 * - `SceneView.cpp` 里 `FSceneViewFamily::ConstructionValues` 的时间取自
 *   `World->GetTime()` —— 是**引擎上一次 tick 的时间和 DeltaTime**，
 *   不是墙上时钟。同一个游戏线程函数里的两次捕获拿到的是同一份时间。
 * - `SceneCaptureComponent.cpp::SetFrameUpdated()` 用 `GFrameCounter` 判定，
 *   同一帧里的第二次捕获被明确识别为 `bRepeatOfThisCapture`。
 *
 * 眼适应、Lumen 的表面缓存、VSM、TSR 的历史都是**每帧走一步**，
 * 而不是每秒走一步。不换帧，渲几次都停在原地 —— 这也是为什么在盒子那侧
 * sleep 一两秒同样没用：等待期间这个组件一帧都没渲。
 *
 * 所以：每个 tick 只渲一帧，让 `World->GetTime()` 真的往前走。
 *
 * ## 顺序为什么是这个
 *
 * 1. **先等编译**。着色器没编完，材质在画面里是灰的/默认棋盘 —— 这种图会让
 *    人（和模型）以为材质丢了。等的是确定的信号（剩余任务数归零），不是拍脑袋的秒数。
 * 2. **建组件、先渲一帧**，让这台相机真正渲染过一次。
 * 3. **再冲流送**。`IStreamingManager` 的视图信息只有 GameViewportClient 会喂
 *    （查过 5.5 引擎源码，SceneCaptureRendering.cpp 里一次都没调 AddViewInformation），
 *    编辑器窗口被遮挡时视口根本不出帧，流送器就不知道这个机位需要哪些贴图。
 *    所以这里**自己**把机位注册进去再 StreamAllResources，否则拍回来是糊的。
 * 4. **剩下的预热帧逐帧渲**，最后一帧读回。
 */
static bool UAL_TickAsyncCapture(TSharedPtr<FUALAsyncCaptureJob> Job)
{
	const double Now = FPlatformTime::Seconds();

	auto Fail = [&Job](const FString& Message)
	{
		if (Job->Setup.Capture || Job->Setup.RenderTarget)
		{
			FString Ignored;
			FString IgnoredError;
			UAL_FinishSceneCapture(Job->Setup, Job->Width, Job->Height, Ignored, IgnoredError);
		}
		UAL_CommandUtils::SendError(Job->RequestId, 500, Message);
		return false;
	};

	if (Now > Job->JobDeadline)
	{
		return Fail(TEXT("截图超时：引擎迟迟没有推进到下一帧"));
	}

	// 世界在作业中途没了（关了关卡、PIE 结束）。组件被 root 着不会被 GC，
	// 但它的世界已经无效，这时候 DestroyComponent 会去碰一个死掉的世界 ——
	// 只摘 root，剩下的交给 GC
	if (!Job->World.IsValid())
	{
		if (Job->Setup.RenderTarget) { Job->Setup.RenderTarget->RemoveFromRoot(); }
		if (Job->Setup.Capture) { Job->Setup.Capture->RemoveFromRoot(); }
		Job->Setup.RenderTarget = nullptr;
		Job->Setup.Capture = nullptr;
		UAL_CommandUtils::SendError(Job->RequestId, 500, TEXT("截图期间世界被销毁（关卡关闭或 PIE 结束）"));
		return false;
	}

	// —— 阶段一：等编译 ——
	if (!Job->bCapturing)
	{
		UAL_GetPendingCompiles(Job->PendingAssets, Job->PendingShaders);
		const bool bBusy = (Job->PendingAssets > 0 || Job->PendingShaders > 0);
		if (bBusy && Now < Job->CompileDeadline)
		{
			return true;  // 下一帧再看
		}

		Job->WaitSeconds = Now - Job->StartTime;
		if (bBusy)
		{
			UE_LOG(LogUALEditor, Warning,
				TEXT("Screenshot: compile still busy after %.1fs (assets=%d shaders=%d), capturing anyway"),
				Job->WaitSeconds, Job->PendingAssets, Job->PendingShaders);
		}

		FString SetupError;
		if (!UAL_SetupSceneCapture(
			Job->Width, Job->Height, Job->Setup, SetupError, Job->World.Get(),
			Job->ViewLocation, Job->ViewRotation, Job->ViewFOV))
		{
			// 回落到 HighResShot：它需要窗口可见，但总比直接报错强。
			// 响应改由那条路发出，这里只负责停掉 ticker
			UE_LOG(LogUALEditor, Warning,
				TEXT("SceneCapture setup failed (%s), falling back to HighResShot"), *SetupError);
			UAL_StartHighResShot(Job->Width, Job->Height, Job->WorldLabel, Job->RequestId);
			return false;
		}

		// GC 保护：组件和 RT 要活过接下来的好几帧
		Job->Setup.RenderTarget->AddToRoot();
		Job->Setup.Capture->AddToRoot();

		// 先渲一帧，让这台相机真的存在过；再把机位喂给流送器
		Job->Setup.Capture->CaptureScene();

		const float HalfFOVRad = FMath::DegreesToRadians(FMath::Clamp(Job->Setup.ViewFOV, 1.f, 179.f) * 0.5f);
		const float ScreenSize = static_cast<float>(Job->Width);
		const float FOVScreenSize = ScreenSize / FMath::Max(FMath::Tan(HalfFOVRad), KINDA_SMALL_NUMBER);
		IStreamingManager::Get().AddViewInformation(Job->Setup.ViewLocation, ScreenSize, FOVScreenSize);
		Job->StreamingInFlight =
			IStreamingManager::Get().StreamAllResources(UAL_ScreenshotStreamWaitSeconds);

		Job->bCapturing = true;
		return true;
	}

	// —— 阶段二：逐帧预热，最后一帧读回 ——
	if (Job->WarmupFramesLeft > 0)
	{
		Job->Setup.Capture->CaptureScene();
		--Job->WarmupFramesLeft;
		return true;
	}

	Job->Setup.Capture->CaptureScene();

	FString CapturedPath;
	FString CaptureError;
	if (!UAL_FinishSceneCapture(Job->Setup, Job->Width, Job->Height, CapturedPath, CaptureError))
	{
		return Fail(FString::Printf(TEXT("截图失败：%s"), *CaptureError));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("path"), CapturedPath);
	Data->SetStringField(TEXT("filename"), FPaths::GetCleanFilename(CapturedPath));
	Data->SetNumberField(TEXT("width"), Job->Width);
	Data->SetNumberField(TEXT("height"), Job->Height);
	Data->SetBoolField(TEXT("saved"), true);
	Data->SetStringField(TEXT("method"), TEXT("scene_capture"));
	// 截的是哪个世界必须回给调用方：同一张图，是「编辑器里的场景」
	// 还是「正在跑的游戏」，结论完全不同
	Data->SetStringField(TEXT("world"), Job->WorldLabel);
	Data->SetStringField(TEXT("view"), Job->bPlayerView ? TEXT("player") : TEXT("viewport"));
	// SceneCapture 不动窗口，客户端没有窗口要恢复
	Data->SetBoolField(TEXT("restore_app_window"), false);
	// 「这张图准备好了没有」必须原样回上去，不能自己咽掉：
	// 着色器没编完 / 贴图没流完，画面就是错的，而模型光看图分不出来
	Data->SetNumberField(TEXT("pending_assets"), Job->PendingAssets);
	Data->SetNumberField(TEXT("pending_shaders"), Job->PendingShaders);
	Data->SetNumberField(TEXT("streaming_in_flight"), Job->StreamingInFlight);
	Data->SetNumberField(TEXT("warmup_frames"), Job->WarmupFrames);
	Data->SetNumberField(TEXT("wait_ms"), FMath::RoundToInt(Job->WaitSeconds * 1000.0));

	// 这一帧到底是从哪儿拍的。
	//
	// 以前一个字都不回，于是「拍的是用户正看着的视口」和「没找到视口、
	// 用了原点上方那台兜底相机」在调用方眼里长得一模一样 —— 两种情况都是
	// success + saved。构图错的那张会被当成场景的真实样子拿去下结论。
	// 机位坐标一并回上去，调用方可以拿它和 Actor 坐标对照，确认镜头确实
	// 对着要看的东西（顺带也是一次单位核对：机位和物体不在同一个数量级
	// 就说明有一边搞错了米和厘米）。
	Data->SetStringField(TEXT("camera_source"), Job->Setup.CameraSource);
	// 曝光锁没锁：决定这张图的明暗能不能当真（见 FUALSceneCaptureSetup::Exposure）
	Data->SetStringField(TEXT("exposure"), Job->Setup.Exposure);
	Data->SetStringField(TEXT("exposure_source"), Job->Setup.ExposureSource);
	TSharedPtr<FJsonObject> CameraLocation = MakeShared<FJsonObject>();
	CameraLocation->SetNumberField(TEXT("x"), Job->Setup.ViewLocation.X);
	CameraLocation->SetNumberField(TEXT("y"), Job->Setup.ViewLocation.Y);
	CameraLocation->SetNumberField(TEXT("z"), Job->Setup.ViewLocation.Z);
	Data->SetObjectField(TEXT("camera_location"), CameraLocation);
	TSharedPtr<FJsonObject> CameraRotation = MakeShared<FJsonObject>();
	CameraRotation->SetNumberField(TEXT("pitch"), Job->Setup.ViewRotation.Pitch);
	CameraRotation->SetNumberField(TEXT("yaw"), Job->Setup.ViewRotation.Yaw);
	CameraRotation->SetNumberField(TEXT("roll"), Job->Setup.ViewRotation.Roll);
	Data->SetObjectField(TEXT("camera_rotation"), CameraRotation);

	UAL_CommandUtils::SendResponse(Job->RequestId, 200, Data);
	return false;
}

#endif  // WITH_EDITOR

void FUAL_EditorCommands::Handle_TakeScreenshot(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 使用 HighResShot 控制台命令截图（UE 编辑器内置方式，最可靠）
#if WITH_EDITOR
	// 1) 解析分辨率参数
	int32 Width = 1920;
	int32 Height = 1080;
	const TArray<TSharedPtr<FJsonValue>>* Resolution = nullptr;
	if (Payload->TryGetArrayField(TEXT("resolution"), Resolution) && Resolution && Resolution->Num() == 2)
	{
		Width = FMath::Max((int32)(*Resolution)[0]->AsNumber(), 64);
		Height = FMath::Max((int32)(*Resolution)[1]->AsNumber(), 64);
	}
	
	// 1.5) 默认走 SceneCapture —— 不依赖视口重绘，编辑器失焦/被遮挡也能出图。
	//
	// 只有明确要求连编辑器界面一起拍（show_ui=true）时才退回 HighResShot，
	// 那种需求本来就要求窗口可见。
	bool bShowUI = false;
	Payload->TryGetBoolField(TEXT("show_ui"), bShowUI);

	/**
	 * 截哪个世界，自己判断：PIE 在跑就截游戏世界，否则截编辑器世界。
	 *
	 * 为什么不让调用方指定：PIE 一开，用户屏幕上、脑子里的「当前画面」
	 * 就是游戏，这时候截编辑器世界拿回的是一张**看着正常但和游戏无关**的图 ——
	 * 模型分不出来，会拿它去下运行时的结论（角色跑到哪了、门开没开），
	 * 而那张图里这些统统没发生。判据是客观的，没有需要人来定的地方。
	 *
	 * `world=editor` 是唯一的例外口子：PIE 跑着的时候想看编辑器场景本身
	 * （比如确认关卡里的摆放）才用得上。
	 */
	FString WorldPref;
	Payload->TryGetStringField(TEXT("world"), WorldPref);
	UWorld* PlayWorld = WorldPref.Equals(TEXT("editor"), ESearchCase::IgnoreCase)
		? nullptr
		: UAL_GetPlayWorld();
	const bool bPieCapture = PlayWorld != nullptr;
	const TCHAR* WorldLabel = bPieCapture ? TEXT("pie") : TEXT("editor");

	if (!bShowUI)
	{
		// PIE 时用玩家视角；Simulate（没有玩家控制器）就回落到视口相机，
		// 但世界仍然是游戏世界
		TOptional<FVector> ViewLocation;
		TOptional<FRotator> ViewRotation;
		TOptional<float> ViewFOV;
		bool bPlayerView = false;
		if (bPieCapture)
		{
			bPlayerView = UAL_TryGetPlayerViewPoint(PlayWorld, ViewLocation, ViewRotation, ViewFOV);
		}

		UWorld* CaptureWorld = PlayWorld ? PlayWorld : GEditor->GetEditorWorldContext().World();
		if (!CaptureWorld)
		{
			// 拿不到世界（没打开关卡）—— 直接落到 HighResShot，它的超时文案会说清楚
			UE_LOG(LogUALEditor, Warning, TEXT("SceneCapture unavailable (no world), falling back to HighResShot"));
		}
		else
		{
			// 预热帧数：默认 4（外加最后那一帧）。调大能让 Lumen/VSM/TSR 攒到更多
			// 历史，画面更干净，代价是多等几帧
			int32 WarmupFrames = UAL_ScreenshotDefaultWarmupFrames;
			double RawWarmup = 0.0;
			if (Payload->TryGetNumberField(TEXT("warmup_frames"), RawWarmup))
			{
				WarmupFrames = FMath::Clamp(static_cast<int32>(RawWarmup), 0, 16);
			}

			TSharedPtr<FUALAsyncCaptureJob> Job = MakeShared<FUALAsyncCaptureJob>();
			Job->RequestId = RequestId;
			Job->Width = Width;
			Job->Height = Height;
			Job->WorldLabel = WorldLabel;
			Job->bPlayerView = bPlayerView;
			Job->World = CaptureWorld;
			Job->ViewLocation = ViewLocation;
			Job->ViewRotation = ViewRotation;
			Job->ViewFOV = ViewFOV;
			Job->WarmupFrames = WarmupFrames;
			Job->WarmupFramesLeft = WarmupFrames;

			const double Now = FPlatformTime::Seconds();
			Job->StartTime = Now;
			Job->CompileDeadline = Now + UAL_ScreenshotCompileWaitSeconds;
			Job->JobDeadline = Now + UAL_ScreenshotJobDeadlineSeconds;

			// 每帧一次（间隔 0）。机位在建组件那一刻定死、之后不再跟着视口走 ——
			// 预热期间相机不动，Lumen/TSR 的历史才攒得干净；这几帧的时间差
			// 在 60fps 下不到 100ms，PIE 里玩家也跑不出画面
			Job->Ticker = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([Job](float) { return UAL_TickAsyncCapture(Job); }),
				0.0f);

			UE_LOG(LogUALEditor, Log,
				TEXT("Screenshot async capture started: %s (warmup=%d)"), *RequestId, WarmupFrames);
			// 响应由 ticker 发出
			return;
		}
	}

	UAL_StartHighResShot(Width, Height, WorldLabel, RequestId);
	// 立即返回，不阻塞（响应将由定时器回调发送）
#else
	// 非编辑器模式不支持 HighResShot
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("HighResShot only available in editor mode"));
#endif
}

#if WITH_EDITOR

/**
 * 老路：HighResShot 控制台命令 + 轮询目录找新文件。
 *
 * 需要编辑器窗口可见 —— 排队等视口下一次重绘，最小化时那一帧永远不来。
 * 两种情况会走到这里：调用方明确要连编辑器界面一起拍（show_ui=true），
 * 或者 SceneCapture 那条路根本建不起来（没打开关卡之类）。
 */
static void UAL_StartHighResShot(int32 Width, int32 Height, const FString& WorldLabel, const FString& RequestId)
{
	// 2) 获取截图目录，找到最新文件用于对比
	const FString ScreenshotDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots/WindowsEditor")));
	IFileManager::Get().MakeDirectory(*ScreenshotDir, true);

	// 记录执行前目录中已有文件及其时间戳
	TMap<FString, FDateTime> FilesBeforeMap;
	TArray<FString> FilesBefore;
	IFileManager::Get().FindFiles(FilesBefore, *FPaths::Combine(ScreenshotDir, TEXT("HighresScreenshot*.png")), true, false);
	for (const FString& File : FilesBefore)
	{
		FString FullPath = FPaths::Combine(ScreenshotDir, File);
		FilesBeforeMap.Add(File, IFileManager::Get().GetTimeStamp(*FullPath));
	}

	// 3) 记录命令执行时间点（使用 UTC 时间与文件系统时间戳一致，减去 2 秒容差避免精度问题）
	FDateTime CommandExecuteTime = FDateTime::UtcNow() - FTimespan::FromSeconds(2);
	UE_LOG(LogUALEditor, Log, TEXT("Command execute time (with tolerance): %s"), *CommandExecuteTime.ToString());

	// 3.5) 窗口被最小化的话恢复它 —— 最小化时虚幻跳过绘制，HighResShot 等的那一帧永远不来。
	//
	// 这里原来还有 BringToFront() + SetWindowFocus()，已经删掉：**Windows 不允许
	// 非前台进程抢焦点**，那两句对一个失焦的编辑器窗口静默无效。真机日志里
	// "Editor window brought to front" 照常打印，而截图仍旧等了 90 秒 ——
	// 直到用户自己点回编辑器才拍成。留着只会让日志说谎。
	//
	// Restore() 不一样：那是进程对自己窗口的操作，最小化状态下确实能恢复渲染。
	//
	// 窗口只是被别的程序挡住（没最小化）时插件这边无能为力，
	// 由盒子那侧提前探测并告诉用户（见 tools/adapted/ue-editor/screenshot.ts）。
	if (FSlateApplication::IsInitialized())
	{
		TSharedPtr<SWindow> MainWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
		if (!MainWindow.IsValid())
		{
			// 如果没有活动窗口，尝试获取第一个顶层窗口
			TArray<TSharedRef<SWindow>> Windows = FSlateApplication::Get().GetInteractiveTopLevelWindows();
			if (Windows.Num() > 0)
			{
				MainWindow = Windows[0];
			}
		}

		if (MainWindow.IsValid() && MainWindow->GetNativeWindow().IsValid()
			&& MainWindow->GetNativeWindow()->IsMinimized())
		{
			MainWindow->Restore();
			UE_LOG(LogUALEditor, Log, TEXT("Editor window restored from minimized state for screenshot"));
		}
	}
	
	// 4) 执行 HighResShot 控制台命令
	FString Command = FString::Printf(TEXT("HighResShot %dx%d"), Width, Height);
	UE_LOG(LogUALEditor, Log, TEXT("Executing: %s, files before: %d"), *Command, FilesBefore.Num());
	GEngine->Exec(GEditor->GetWorld(), *Command);
	
	// 5) 创建异步任务上下文并设置定时器（非阻塞）
	TSharedPtr<FScreenshotTaskContext> Context = MakeShared<FScreenshotTaskContext>();
	Context->RequestId = RequestId;
	Context->ScreenshotDir = ScreenshotDir;
	Context->FilesBeforeMap = FilesBeforeMap;
	Context->CommandExecuteTime = CommandExecuteTime;
	Context->Width = Width;
	Context->Height = Height;
	Context->RetryCount = 0;
	Context->MaxRetries = 15;
	Context->World = WorldLabel;
	
	// 使用 RequestId 作为任务 ID
	FString TaskId = RequestId;
	PendingScreenshotTasks.Add(TaskId, Context);
	
	// 设置定时器：每 2 秒检查一次，最多检查 15 次（共 30 秒）
	if (GEditor)
	{
		GEditor->GetTimerManager()->SetTimer(
			Context->TimerHandle,
			FTimerDelegate::CreateLambda([TaskId]() { CheckScreenshotFile(TaskId); }),
			2.0f,  // 每 2 秒检查一次
			true,  // 循环
			2.0f   // 首次延迟 2 秒（给截图时间生成）
		);
		UE_LOG(LogUALEditor, Log, TEXT("Screenshot async timer started for request: %s"), *RequestId);
	}
	else
	{
		UE_LOG(LogUALEditor, Error, TEXT("Failed to get TimerManager"));
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to start screenshot timer"));
		PendingScreenshotTasks.Remove(TaskId);
	}
}

#endif  // WITH_EDITOR

void FUAL_EditorCommands::Handle_GetProjectInfo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// TODO: 从 UAL_CommandHandler.cpp 第 2144-2148 行迁移
	TSharedPtr<FJsonObject> Data = BuildProjectInfo();
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

TSharedPtr<FJsonObject> FUAL_EditorCommands::BuildProjectInfo()
{
	// 基础路径信息
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("projectName"), FApp::GetProjectName());
	Data->SetStringField(TEXT("projectPath"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	Data->SetStringField(TEXT("projectFile"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
	Data->SetStringField(TEXT("contentDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));
	Data->SetStringField(TEXT("configDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir()));
	Data->SetStringField(TEXT("savedDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()));
	Data->SetStringField(TEXT("pluginsDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectPluginsDir()));

	// 版本信息（兼容 5.0-5.7，直接读配置）
	FString ProjectVersion;
	GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("ProjectVersion"), ProjectVersion, GGameIni);
	if (!ProjectVersion.IsEmpty())
	{
		Data->SetStringField(TEXT("projectVersion"), ProjectVersion);
	}

	Data->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());

	/*
	 * 引擎源码在哪，工程源码在哪，这个工程有没有 C++。
	 *
	 * ## 为什么加 engineDir / engineSourceDir
	 *
	 * 上面那几条路径全指向工程内部，**没有一条指向引擎**。后果是模型想核对一个
	 * UE API 的真实签名时，不知道该往哪个目录 grep —— 只能凭记忆写。而 UE 的 API
	 * 在 5.0–5.8 之间是会漂的，凭记忆写出来编不过算走运，编过了但行为不对才是坏结果。
	 *
	 * 有了这两条，模型手上现成的 grep_local_files / find_local_files 就变成了
	 * 「能查证 API」的工具。四行代码换掉一整类幻觉。
	 *
	 * 不另做一个「查引擎 API」的专用工具，是因为那等于在模型和真实头文件之间
	 * 加一层我们自己维护的缓存，那层缓存迟早过期。
	 *
	 * ## hasCode
	 *
	 * 纯蓝图工程加不了 C++ 类 —— 引擎那条路（GameProjectUtils::AddCodeToProject）
	 * 在工程原先没有代码时会弹模态对话框，而我们的命令跑在游戏线程上，
	 * 弹出来就是编辑器和这次调用一起卡死。所以这件事必须在动手之前就知道。
	 *
	 * 判断用引擎自己的 ProjectHasCodeFiles()，不自己去数 Source 目录 ——
	 * 「什么算有 C++」的定义要和引擎保持一致，我们数出来的和它认的不一样就白搭。
	 */
	Data->SetStringField(TEXT("engineDir"), FPaths::ConvertRelativePathToFull(FPaths::EngineDir()));
	Data->SetStringField(TEXT("engineSourceDir"), FPaths::ConvertRelativePathToFull(FPaths::EngineSourceDir()));
	Data->SetStringField(TEXT("projectSourceDir"), FPaths::ConvertRelativePathToFull(FPaths::GameSourceDir()));
	Data->SetBoolField(TEXT("hasCode"), GameProjectUtils::ProjectHasCodeFiles());

	/*
	 * 当前跑着的这个插件，是从哪份源码编出来的。
	 *
	 * 少了这一条真的翻过车：改完代码还没出包，回归测试却跑在旧 DLL 上，
	 * 于是「修复没生效」——接下来整轮排查都在找一个不存在的 bug。
	 * 插件版本号解决不了这个问题，它几个月才动一次。
	 *
	 * 出包时会往插件根目录写一个 .ual-build，里面就是那次的源码指纹。
	 * 原样报出来，验证的第一步就能确认「我测的是不是我改的那份」。
	 * 开发机上直接编译的宿主工程没有这个文件，报 unknown —— 那也是有用的信息。
	 */
	FString PluginBuild = TEXT("unknown");
	if (TSharedPtr<IPlugin> UALPlugin = IPluginManager::Get().FindPlugin(TEXT("UnrealAgentLink")))
	{
		FString StampJson;
		if (FFileHelper::LoadFileToString(StampJson, *(UALPlugin->GetBaseDir() / TEXT(".ual-build"))))
		{
			TSharedPtr<FJsonObject> Stamp;
			const TSharedRef<TJsonReader<>> StampReader = TJsonReaderFactory<>::Create(StampJson);
			FString Fingerprint;
			if (FJsonSerializer::Deserialize(StampReader, Stamp) && Stamp.IsValid() &&
				Stamp->TryGetStringField(TEXT("fingerprint"), Fingerprint))
			{
				PluginBuild = Fingerprint;
			}
		}
	}
	Data->SetStringField(TEXT("pluginBuild"), PluginBuild);

	/*
	 * 这份引擎能不能跑 Python。和 Handle_RunPython / system.get_project_info 的
	 * capabilities.python 取的是同一个引擎 API（IsPythonAvailable），不是另写的常量。
	 * 盒子拿它在会话开头就告诉模型「别走 Python」，而不是让它撞到 ok:false 再猜。
	 */
	{
		IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
		Data->SetBoolField(TEXT("pythonAvailable"), PythonPlugin != nullptr && PythonPlugin->IsPythonAvailable());
	}

	/*
	 * 这个进程是不是「用户面前那个编辑器」。
	 *
	 * 插件在 StartupModule 里无条件连 17860，`-unattended -nullrhi` 的 commandlet
	 * 跑批也照连。真机上出过这一幕：用户把编辑器关了，一个跑完没退的验证进程
	 * 还占着连接，盒子照实报「MetaHumanDoubaoFullDuplex 连着」，模型于是对着一个
	 * 用户根本没打开的工程说话 —— 而且那句话在当时就是假的。
	 *
	 * 不在插件侧直接拒绝连接：无头编辑器是我们自己要用的一条路（拿它当考卷，
	 * 核对自研解析器读出来的字节对不对）。连可以连，但必须说清楚自己是什么，
	 * 算不算「当前工程」交给盒子判（见 `main/services/project/projectManager.ts`）。
	 *
	 * 三条判据的顺序是从确定到宽泛：commandlet 一定没界面；`-unattended` 是
	 * 「没人看着」的官方标志；`CanEverRender()` 为假意味着 `-nullrhi` 之类，
	 * 就算进程活着也没有可交互的视口。三条都不沾才算真编辑器。
	 */
	FString RunMode = TEXT("editor");
	if (IsRunningCommandlet())
	{
		RunMode = TEXT("commandlet");
	}
	else if (FApp::IsUnattended())
	{
		RunMode = TEXT("unattended");
	}
	else if (!FApp::CanEverRender())
	{
		RunMode = TEXT("headless");
	}
	Data->SetStringField(TEXT("runMode"), RunMode);
	Data->SetBoolField(TEXT("interactive"), RunMode == TEXT("editor"));

	/**
	 * ========== 读取 GameMapsSettings 开发心得 (2026-01) ==========
	 * 
	 * 【问题背景】
	 * 需要读取 Project Settings → Maps & Modes 中配置的 GameDefaultMap 等字段。
	 * 这些配置存储在 Config/DefaultGame.ini 的 [/Script/EngineSettings.GameMapsSettings] section。
	 * 
	 * 【坑点1：GConfig 方式无效】
	 * 尝试使用 GConfig->GetString(Section, Key, Value, GGameIni) 时返回空。
	 * 原因：GGameIni 指向的是运行时合并后的 Game.ini，而非项目的 DefaultGame.ini。
	 * 
	 * 【坑点2：LoadLocalIniFile 加载错误文件】
	 * FConfigCacheIni::LoadLocalIniFile(ConfigFile, "Game", ...) 加载的是合并后的 Game.ini，
	 * 不包含 /Script/EngineSettings.GameMapsSettings section。
	 * 
	 * 【坑点3：FConfigFile::Read() 只加载部分内容】
	 * 直接用 FConfigFile::Read(DefaultGame.ini) 只返回 1 个 section (GeneralProjectSettings)，
	 * 不包含 GameMapsSettings。原因不明，可能与 UE 配置解析机制有关。
	 * 
	 * 【坑点4：Section 路径易混淆】
	 * /Script/EngineSettings.GameMapsSettings  ← DefaultGame.ini 中的格式
	 * /Script/Engine.GameMapsSettings          ← 不正确
	 * 两者容易混淆，但都不是根本解决方案。
	 * 
	 * 【正确方案：使用 UGameMapsSettings API】
	 * 引擎内部使用静态方法 UGameMapsSettings::GetGameDefaultMap()。
	 * 需要：
	 *   1. 在 Build.cs 添加 "EngineSettings" 模块依赖
	 *   2. #include "GameMapsSettings.h"
	 *   3. 调用 UGameMapsSettings::GetGameDefaultMap() 或 GetDefault<UGameMapsSettings>()
	 * 
	 * 此方式兼容 UE 5.0 - 5.7，是最可靠的方案。
	 * ==========================================================
	 */
	
	// GameMaps 设置 - 使用 UGameMapsSettings API（引擎内部使用的方式）
	FString GameDefaultMap = UGameMapsSettings::GetGameDefaultMap();
	if (!GameDefaultMap.IsEmpty())
	{
		UE_LOG(LogUALEditor, Log, TEXT("GameDefaultMap = %s"), *GameDefaultMap);
		Data->SetStringField(TEXT("defaultMap"), GameDefaultMap);
	}
	else
	{
		UE_LOG(LogUALEditor, Log, TEXT("GameDefaultMap is not set"));
	}

	// EditorStartupMap - 通过 GetDefault 获取
	const UGameMapsSettings* GameMapsSettings = GetDefault<UGameMapsSettings>();
	if (GameMapsSettings)
	{
		FString EditorStartupMap = GameMapsSettings->EditorStartupMap.GetLongPackageName();
		if (!EditorStartupMap.IsEmpty())
		{
			UE_LOG(LogUALEditor, Log, TEXT("EditorStartupMap = %s"), *EditorStartupMap);
			Data->SetStringField(TEXT("editorStartupMap"), EditorStartupMap);
		}

		// TransitionMap
		FString TransitionMap = GameMapsSettings->TransitionMap.GetLongPackageName();
		if (!TransitionMap.IsEmpty())
		{
			Data->SetStringField(TEXT("transitionMap"), TransitionMap);
		}
	}


	// GeneralProjectSettings - 使用 GConfig 读取（兼容所有版本）
	FString CompanyName;
	if (GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("CompanyName"), CompanyName, GGameIni))
	{
		Data->SetStringField(TEXT("companyName"), CompanyName);
	}
	FString ProjectId;
	if (GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("ProjectID"), ProjectId, GGameIni))
	{
		Data->SetStringField(TEXT("projectId"), ProjectId);
	}
	FString SupportContact;
	if (GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("SupportContact"), SupportContact, GGameIni))
	{
		Data->SetStringField(TEXT("supportContact"), SupportContact);
	}

	// 扩展：当前打开的关卡名
#if WITH_EDITOR
	if (GEditor && GEditor->GetWorld())
	{
		FString CurrentLevelName = GEditor->GetWorld()->GetMapName();
		Data->SetStringField(TEXT("currentLevelName"), CurrentLevelName);
		
		// 当前关卡完整路径
		FString CurrentLevelPath = GEditor->GetWorld()->GetOutermost()->GetName();
		Data->SetStringField(TEXT("currentLevelPath"), CurrentLevelPath);
	}
#endif

	// 扩展：渲染设置（Nanite/Lumen）
	FString NaniteEnabled;
	if (GConfig->GetString(TEXT("/Script/Engine.RendererSettings"), TEXT("r.Nanite.ProjectEnabled"), NaniteEnabled, GEngineIni))
	{
		Data->SetBoolField(TEXT("naniteEnabled"), NaniteEnabled.Equals(TEXT("True"), ESearchCase::IgnoreCase) || NaniteEnabled == TEXT("1"));
	}
	FString LumenGI;
	if (GConfig->GetString(TEXT("/Script/Engine.RendererSettings"), TEXT("r.DynamicGlobalIlluminationMethod"), LumenGI, GEngineIni))
	{
		// 1 = Lumen, 0 = None/SSGI
		Data->SetBoolField(TEXT("lumenGIEnabled"), LumenGI == TEXT("1"));
		Data->SetStringField(TEXT("dynamicGIMethod"), LumenGI);
	}
	FString LumenReflections;
	if (GConfig->GetString(TEXT("/Script/Engine.RendererSettings"), TEXT("r.ReflectionMethod"), LumenReflections, GEngineIni))
	{
		// 1 = Lumen, 0 = None, 2 = SSR
		Data->SetBoolField(TEXT("lumenReflectionsEnabled"), LumenReflections == TEXT("1"));
		Data->SetStringField(TEXT("reflectionMethod"), LumenReflections);
	}


	// 读取 .uproject 以补充 EngineAssociation、TargetPlatforms、Modules
	FString ProjectFileContent;
	if (FFileHelper::LoadFileToString(ProjectFileContent, *FPaths::GetProjectFilePath()))
	{
		TSharedPtr<FJsonObject> ProjectJson;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ProjectFileContent);
		if (FJsonSerializer::Deserialize(Reader, ProjectJson) && ProjectJson.IsValid())
		{
			FString EngineAssociation;
			if (ProjectJson->TryGetStringField(TEXT("EngineAssociation"), EngineAssociation))
			{
				Data->SetStringField(TEXT("engineAssociation"), EngineAssociation);
			}

			const TArray<TSharedPtr<FJsonValue>>* TargetPlatforms = nullptr;
			if (ProjectJson->TryGetArrayField(TEXT("TargetPlatforms"), TargetPlatforms))
			{
				TArray<TSharedPtr<FJsonValue>> PlatformArray;
				for (const TSharedPtr<FJsonValue>& Value : *TargetPlatforms)
				{
					if (Value.IsValid() && Value->Type == EJson::String)
					{
						PlatformArray.Add(MakeShared<FJsonValueString>(Value->AsString()));
					}
				}
				if (PlatformArray.Num() > 0)
				{
					Data->SetArrayField(TEXT("targetPlatforms"), PlatformArray);
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
			if (ProjectJson->TryGetArrayField(TEXT("Modules"), Modules))
			{
				TArray<TSharedPtr<FJsonValue>> ModuleArray;
				for (const TSharedPtr<FJsonValue>& Value : *Modules)
				{
					if (Value.IsValid() && Value->Type == EJson::Object)
					{
						const TSharedPtr<FJsonObject> ModuleObj = Value->AsObject();
						if (ModuleObj.IsValid())
						{
							FString ModuleName;
							if (ModuleObj->TryGetStringField(TEXT("Name"), ModuleName))
							{
								ModuleArray.Add(MakeShared<FJsonValueString>(ModuleName));
							}
						}
					}
				}
				if (ModuleArray.Num() > 0)
				{
					Data->SetArrayField(TEXT("modules"), ModuleArray);
				}
			}

			// 插件列表：仅发送 .uproject 文件中声明的 Plugins（避免把 Engine/Plugins 也全部发出去）
			// - projectPlugins: 完整列表（包含 enabled 标记）
			// - enabledPlugins: 仅 enabled=true 的条目（为兼容旧字段名）
			// - enabledPluginNames: 仅名称列表（便于前端/服务端快速使用）
			const TArray<TSharedPtr<FJsonValue>>* ProjectPlugins = nullptr;
			if (ProjectJson->TryGetArrayField(TEXT("Plugins"), ProjectPlugins) && ProjectPlugins)
			{
				IPluginManager& PluginManager = IPluginManager::Get();

				TArray<TSharedPtr<FJsonValue>> ProjectPluginArray;
				TArray<TSharedPtr<FJsonValue>> EnabledPluginArray;
				TArray<TSharedPtr<FJsonValue>> EnabledPluginNames;

				for (const TSharedPtr<FJsonValue>& Value : *ProjectPlugins)
				{
					if (!Value.IsValid() || Value->Type != EJson::Object)
					{
						continue;
					}

					const TSharedPtr<FJsonObject> PluginDecl = Value->AsObject();
					if (!PluginDecl.IsValid())
					{
						continue;
					}

					FString PluginName;
					// .uproject 标准字段为 Name/Enabled；这里也兼容小写 name/enabled
					if (!PluginDecl->TryGetStringField(TEXT("Name"), PluginName))
					{
						PluginDecl->TryGetStringField(TEXT("name"), PluginName);
					}
					if (PluginName.IsEmpty())
					{
						continue;
					}

					bool bEnabled = true;
					if (!PluginDecl->TryGetBoolField(TEXT("Enabled"), bEnabled))
					{
						PluginDecl->TryGetBoolField(TEXT("enabled"), bEnabled);
					}

					TSharedPtr<FJsonObject> PluginObj = MakeShared<FJsonObject>();
					PluginObj->SetStringField(TEXT("name"), PluginName);
					PluginObj->SetBoolField(TEXT("enabled"), bEnabled);

					// 尝试补全插件元信息（如果插件在当前环境可解析）
					{
						const TSharedPtr<IPlugin> Plugin = PluginManager.FindPlugin(PluginName);
						if (Plugin.IsValid())
						{
							PluginObj->SetStringField(TEXT("versionName"), Plugin->GetDescriptor().VersionName);
							PluginObj->SetStringField(TEXT("category"), Plugin->GetDescriptor().Category);
							PluginObj->SetStringField(TEXT("baseDir"), Plugin->GetBaseDir());
						}
						else
						{
							// 保持字段存在，避免消费端做大量判空
							PluginObj->SetStringField(TEXT("versionName"), TEXT(""));
							PluginObj->SetStringField(TEXT("category"), TEXT(""));
							PluginObj->SetStringField(TEXT("baseDir"), TEXT(""));
						}
					}

					ProjectPluginArray.Add(MakeShared<FJsonValueObject>(PluginObj));
					if (bEnabled)
					{
						EnabledPluginArray.Add(MakeShared<FJsonValueObject>(PluginObj));
						EnabledPluginNames.Add(MakeShared<FJsonValueString>(PluginName));
					}
				}

				if (ProjectPluginArray.Num() > 0)
				{
					Data->SetArrayField(TEXT("projectPlugins"), ProjectPluginArray);
				}
				if (EnabledPluginArray.Num() > 0)
				{
					Data->SetArrayField(TEXT("enabledPlugins"), EnabledPluginArray);
				}
				if (EnabledPluginNames.Num() > 0)
				{
					Data->SetArrayField(TEXT("enabledPluginNames"), EnabledPluginNames);
				}
			}
		}
	}

	return Data;
}
// 临时文件，用于添加新函数
void FUAL_EditorCommands::Handle_GetConfig(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString ConfigName;
	if (!Payload->TryGetStringField(TEXT("config_name"), ConfigName))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: config_name"));
		return;
	}

	FString Section;
	if (!Payload->TryGetStringField(TEXT("section"), Section))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: section"));
		return;
	}

	FString Key;
	if (!Payload->TryGetStringField(TEXT("key"), Key))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: key"));
		return;
	}

	// 映射配置文件�?
	FString ConfigFileName;
	if (ConfigName.Equals(TEXT("Engine"), ESearchCase::IgnoreCase))
	{
		ConfigFileName = GEngineIni;
	}
	else if (ConfigName.Equals(TEXT("Game"), ESearchCase::IgnoreCase))
	{
		ConfigFileName = GGameIni;
	}
	else if (ConfigName.Equals(TEXT("Editor"), ESearchCase::IgnoreCase))
	{
		ConfigFileName = GEditorIni;
	}
	else if (ConfigName.Equals(TEXT("EditorPerProjectUserSettings"), ESearchCase::IgnoreCase))
	{
		ConfigFileName = GEditorPerProjectIni;
	}
	else
	{
		UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(TEXT("Unsupported config_name: %s. Supported: Engine, Game, Editor, EditorPerProjectUserSettings"), *ConfigName));
		return;
	}

	// 读取配置
	FString Value;
	if (!GConfig->GetString(*Section, *Key, Value, ConfigFileName))
	{
		// 配置项不存在，返回空值（不是错误�?
		Value = TEXT("");
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("config_name"), ConfigName);
	Result->SetStringField(TEXT("section"), Section);
	Result->SetStringField(TEXT("key"), Key);
	Result->SetStringField(TEXT("value"), Value);
	Result->SetStringField(TEXT("file_path"), ConfigFileName);

	UE_LOG(LogUALEditor, Log, TEXT("project.get_config: %s [%s] %s = %s"), *ConfigName, *Section, *Key, *Value);

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

namespace UALSetConfig
{
	/**
	 * 节名是 `/Script/<模块>.<类>` 时，找那个类和键对应的 config 属性。
	 *
	 * 键按属性名认，也按 `ConsoleVariable` 元数据认 —— RendererSettings 这类设置在 ini 里
	 * 的键是 `r.DefaultFeature.AutoExposure`，属性名却是 `bDefaultFeatureAutoExposure`。
	 */
	static FProperty* FindConfigProperty(const FString& Section, const FString& Key, UClass*& OutClass)
	{
		OutClass = nullptr;
		if (!Section.StartsWith(TEXT("/Script/")))
		{
			return nullptr;
		}
		UClass* Class = FindObject<UClass>(nullptr, *Section);
		if (!Class)
		{
			return nullptr;
		}
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop->HasAnyPropertyFlags(CPF_Config))
			{
				continue;
			}
			bool bMatches = Prop->GetName().Equals(Key, ESearchCase::IgnoreCase);
#if WITH_EDITORONLY_DATA
			bMatches = bMatches || Prop->GetMetaData(TEXT("ConsoleVariable")).Equals(Key, ESearchCase::IgnoreCase);
#endif
			if (bMatches)
			{
				OutClass = Class;
				return Prop;
			}
		}
		return nullptr;
	}

	/** 属性背后的枚举（TEnumAsByte 的字节属性或 enum class 属性）；不是枚举给 nullptr */
	static UEnum* EnumOf(const FProperty* Prop)
	{
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			return EnumProp->GetEnum();
		}
		if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			return ByteProp->Enum;
		}
		return nullptr;
	}

	/** 枚举属性当前值的数字写法。带 ConsoleVariable 的属性，引擎往 ini 里写的是这个 */
	static bool EnumNumber(const FProperty* Prop, const void* Container, FString& Out)
	{
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Container);
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			Out = LexToString(EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr));
			return true;
		}
		if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			if (ByteProp->Enum)
			{
				Out = LexToString(static_cast<int32>(ByteProp->GetPropertyValue(ValuePtr)));
				return true;
			}
		}
		return false;
	}

	/**
	 * 磁盘上读回来的值是不是就是属性现在的值。
	 *
	 * 带 ConsoleVariable 元数据的属性，引擎存盘时用 PPF_ConsoleVariable 导出：枚举写成
	 * 数字、布尔写成 1/0。拿引擎平常的写法（枚举名、True/False）去比，写成功了也对不上。
	 */
	static bool SameAsDisk(const FProperty* Prop, const void* Container, const FString& Expected, const FString& OnDisk)
	{
		if (OnDisk.Equals(Expected, ESearchCase::IgnoreCase))
		{
			return true;
		}
		FString Number;
		if (EnumNumber(Prop, Container, Number) && OnDisk.TrimStartAndEnd() == Number)
		{
			return true;
		}
		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			return FCString::ToBool(*OnDisk) == BoolProp->GetPropertyValue_InContainer(Container);
		}
		return false;
	}
}

void FUAL_EditorCommands::Handle_SetConfig(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString ConfigName;
	if (!Payload->TryGetStringField(TEXT("config_name"), ConfigName))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: config_name"));
		return;
	}

	FString Section;
	if (!Payload->TryGetStringField(TEXT("section"), Section))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: section"));
		return;
	}

	FString Key;
	if (!Payload->TryGetStringField(TEXT("key"), Key))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: key"));
		return;
	}

	FString Value;
	if (!Payload->TryGetStringField(TEXT("value"), Value))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: value"));
		return;
	}

	// 映射配置文件�?
	FString ConfigFileName;
	if (ConfigName.Equals(TEXT("Engine"), ESearchCase::IgnoreCase))
	{
		ConfigFileName = GEngineIni;
	}
	else if (ConfigName.Equals(TEXT("Game"), ESearchCase::IgnoreCase))
	{
		ConfigFileName = GGameIni;
	}
	else if (ConfigName.Equals(TEXT("Editor"), ESearchCase::IgnoreCase))
	{
		ConfigFileName = GEditorIni;
	}
	else if (ConfigName.Equals(TEXT("EditorPerProjectUserSettings"), ESearchCase::IgnoreCase))
	{
		ConfigFileName = GEditorPerProjectIni;
	}
	else
	{
		UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(TEXT("Unsupported config_name: %s. Supported: Engine, Game, Editor, EditorPerProjectUserSettings"), *ConfigName));
		return;
	}

	/*
	 * 工程设置写进工程的 Config/Default*.ini，不写 GEngineIni。
	 *
	 * GEngineIni 在编辑器里是 Saved/Config/<平台>Editor/Engine.ini —— 本机的运行时那一层，
	 * 不进版本管理、不进打包。2026-09-26 科幻塔防真机：AI 把默认 GameMode 设了三遍，
	 * DefaultEngine.ini 里一个字都没有（打包版照样用模板的设置）；5.5 上连 Saved 那份
	 * 都没写成，回执报「写盘失败」，模型以为是文件只读。项目设置界面改的是 Default*.ini，
	 * 这里跟它走同一条路：
	 *
	 * 1. 节名对得上一个设置类、键对得上它的 config 属性（数组除外）：改类默认对象、
	 *    PostEditChangeProperty、UpdateSinglePropertyInConfigFile。编辑器里当场生效 ——
	 *    默认 GameMode 不用重启就换了，r.xxx 由 DeveloperSettings 推到控制台变量。
	 * 2. 其余的键：只把这一个键写进 Default*.ini（UpdateSinglePropertyInSection，
	 *    文件里其他内容原样保留），同时写进内存里的配置，这次会话读得到。
	 *
	 * 写完从磁盘重新读一遍 Default*.ini，读回来的不是写进去的值就照实报失败。
	 * EditorPerProjectUserSettings 本来就是每人一份的本机设置，走下面原来那条路。
	 */
	if (!ConfigName.Equals(TEXT("EditorPerProjectUserSettings"), ESearchCase::IgnoreCase))
	{
		FString Canonical = TEXT("Editor");
		if (ConfigName.Equals(TEXT("Engine"), ESearchCase::IgnoreCase))
		{
			Canonical = TEXT("Engine");
		}
		else if (ConfigName.Equals(TEXT("Game"), ESearchCase::IgnoreCase))
		{
			Canonical = TEXT("Game");
		}

		FString DefaultIni;
		FString Expected = Value;
		FString Via;
		FString DiskKey = Key;
		// 写设置时替用户答掉的引擎弹框，见下面 PostEditChangeProperty 那段
		TArray<TSharedPtr<FJsonValue>> AnsweredDialogs;
		UClass* SettingsClass = nullptr;
		FProperty* Prop = UALSetConfig::FindConfigProperty(Section, Key, SettingsClass);
		if (Prop && !Prop->IsA(FArrayProperty::StaticClass()))
		{
			UObject* CDO = SettingsClass->GetDefaultObject();
			// 枚举给的是数字（ini 里、get_config 读回来的就是数字）：换成枚举名再解析，
			// PropertyValueFromString 只认名字
			FString Parsed = Value;
			if (UEnum* Enum = UALSetConfig::EnumOf(Prop))
			{
				if (Value.IsNumeric())
				{
					const FString Name = Enum->GetNameStringByValue(FCString::Atoi64(*Value));
					if (!Name.IsEmpty())
					{
						Parsed = Name;
					}
				}
			}
			// 解析失败时引擎已经把属性写成了 0：先存一份，失败就原样放回去，不能报「没改」却改了
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);
			TArray<uint8> Backup;
			Backup.SetNumZeroed(Prop->GetSize());
			Prop->InitializeValue(Backup.GetData());
			Prop->CopyCompleteValue(Backup.GetData(), ValuePtr);
			const bool bParsed = FBlueprintEditorUtils::PropertyValueFromString(Prop, Parsed, reinterpret_cast<uint8*>(CDO), CDO);
			if (!bParsed)
			{
				Prop->CopyCompleteValue(ValuePtr, Backup.GetData());
			}
			Prop->DestroyValue(Backup.GetData());
			if (!bParsed)
			{
				UAL_CommandUtils::SendError(RequestId, 400,
					FString::Printf(TEXT("project.set_config: \"%s\" is not a valid value for [%s] %s (type %s)"),
						*Value, *Section, *Key, *Prop->GetCPPType()));
				return;
			}
			FPropertyChangedEvent Changed(Prop, EPropertyChangeType::ValueSet);
			{
				/*
				 * 设置对象的 PostEditChange 会弹模态框，必须替用户答掉。
				 *
				 * 渲染设置（URendererSettings::PostEditChangeProperty）里就有好几个
				 * `FMessageDialog::Open`：开 Substrate 的 Beta 警告、开光追问要不要顺带开
				 * 蒙皮缓存、开 Lumen 说已自动开网格距离场…… 编辑器交互态下这些框卡在
				 * 游戏线程上，这条 RPC 超时，连只读的 get_config 也跟着超时。
				 * 真机上就是这样：用户同意开 Substrate，AI 收到两次超时，以为没写进去，
				 * 其实值早就在弹框后面等着 —— 用户得自己去编辑器里找那个框点掉。
				 *
				 * 答「肯定」是对的：调用方要的正是把这个值设上。点了什么记进回执，
				 * 模型要把它原样告诉用户（比如「顺带开了蒙皮缓存」）。
				 */
				FUAL_ScopedDialogAutoAnswer DialogAnswer;
				CDO->PostEditChangeProperty(Changed);
				AnsweredDialogs = DialogAnswer.ToJson();
			}
			DefaultIni = CDO->GetDefaultConfigFilename();
			CDO->UpdateSinglePropertyInConfigFile(Prop, DefaultIni);
			// 引擎规范化后的写法（布尔是 True/False、软引用是完整路径），拿它和磁盘上的比
			FBlueprintEditorUtils::PropertyValueToString(Prop, reinterpret_cast<const uint8*>(CDO), Expected, CDO);
			Via = TEXT("settings_object");
#if WITH_EDITORONLY_DATA
			// 带 ConsoleVariable 的属性，引擎在 ini 里用控制台变量名当键
			const FString CVarName = Prop->GetMetaData(TEXT("ConsoleVariable"));
			if (!CVarName.IsEmpty())
			{
				DiskKey = CVarName;
			}
#endif
		}
		else
		{
			DefaultIni = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectConfigDir() / FString::Printf(TEXT("Default%s.ini"), *Canonical));
			FConfigFile Single;
			Single.SetString(*Section, *Key, *Value);
			Single.UpdateSinglePropertyInSection(*DefaultIni, *Key, *Section);
			GConfig->SetString(*Section, *Key, *Value, ConfigFileName);
			Via = TEXT("ini");
		}

		FConfigFile OnDisk;
		OnDisk.Read(DefaultIni);
		FString OnDiskValue;
		const bool bOnDisk = OnDisk.GetString(*Section, *DiskKey, OnDiskValue) ||
			OnDisk.GetString(*Section, *Key, OnDiskValue);
		const bool bPersistedToDefault = bOnDisk &&
			(Via == TEXT("settings_object")
				? UALSetConfig::SameAsDisk(Prop, SettingsClass->GetDefaultObject(), Expected, OnDiskValue)
				: OnDiskValue.Equals(Expected, ESearchCase::IgnoreCase));

		TSharedPtr<FJsonObject> DefaultResult = MakeShared<FJsonObject>();
		DefaultResult->SetStringField(TEXT("config_name"), ConfigName);
		DefaultResult->SetStringField(TEXT("section"), Section);
		DefaultResult->SetStringField(TEXT("key"), Key);
		DefaultResult->SetStringField(TEXT("requested_value"), Value);
		DefaultResult->SetBoolField(TEXT("read_back"), bOnDisk);
		if (bOnDisk)
		{
			DefaultResult->SetStringField(TEXT("value"), OnDiskValue);
		}
		DefaultResult->SetBoolField(TEXT("persisted"), bPersistedToDefault);
		DefaultResult->SetStringField(TEXT("file_path"), DefaultIni);
		DefaultResult->SetStringField(TEXT("via"), Via);
		DefaultResult->SetBoolField(TEXT("applied_live"), Via == TEXT("settings_object"));
		if (AnsweredDialogs.Num() > 0)
		{
			DefaultResult->SetArrayField(TEXT("auto_answered_dialogs"), AnsweredDialogs);
		}

		if (!bPersistedToDefault)
		{
			FString Why;
			if (IFileManager::Get().IsReadOnly(*DefaultIni))
			{
				Why = TEXT(" (the file is read-only, probably locked by source control)");
			}
			else if (bOnDisk)
			{
				Why = FString::Printf(TEXT(" (it reads back \"%s\")"), *OnDiskValue);
			}
			// 设置对象那条路先改了类默认对象、推了 PostEditChange：编辑器里已经换成新值了，
			// 只是没存进文件。回执要说出来，不能让模型以为什么都没变
			if (Via == TEXT("settings_object"))
			{
				Why += TEXT(". The new value IS already applied live in this editor session, but it is not saved and will be lost on restart");
			}
			UAL_CommandUtils::SendError(
				RequestId, 500,
				FString::Printf(TEXT("project.set_config: [%s] %s was not written to %s%s"),
					*Section, *Key, *DefaultIni, *Why),
				DefaultResult);
			return;
		}

		UE_LOG(LogUALEditor, Log, TEXT("project.set_config: %s [%s] %s = %s (via %s)"), *DefaultIni, *Section, *Key, *OnDiskValue, *Via);
		UAL_CommandUtils::SendResponse(RequestId, 200, DefaultResult);
		return;
	}

	// 设置配置（EditorPerProjectUserSettings：本机那一层）
	GConfig->SetString(*Section, *Key, *Value, ConfigFileName);

	// 刷新到磁盘。Flush 没有返回值（5.0-5.4 都是 void），写盘失败的唯一痕迹是
	// FConfigFile::Dirty 没被清掉 —— WriteInternal 末尾是 `Dirty = !bResult`。
	// -nowrite / NoSave 的文件也会留着 Dirty，那种情况同样算「没落盘」，照实说
	GConfig->Flush(false, ConfigFileName);
	const FConfigFile* WrittenFile = GConfig->FindConfigFile(ConfigFileName);
	const bool bPersisted = WrittenFile && !WrittenFile->Dirty;

	// 回执里的 value 从引擎读回，不回显请求（AGENTS.md §5 第 14 条）
	FString ReadBack;
	const bool bReadBack = GConfig->GetString(*Section, *Key, ReadBack, ConfigFileName);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("config_name"), ConfigName);
	Result->SetStringField(TEXT("section"), Section);
	Result->SetStringField(TEXT("key"), Key);
	Result->SetStringField(TEXT("requested_value"), Value);
	Result->SetBoolField(TEXT("read_back"), bReadBack);
	if (bReadBack)
	{
		Result->SetStringField(TEXT("value"), ReadBack);
	}
	Result->SetBoolField(TEXT("persisted"), bPersisted);
	Result->SetStringField(TEXT("file_path"), ConfigFileName);

	if (!bReadBack || ReadBack != Value)
	{
		UAL_CommandUtils::SendError(
			RequestId, 500,
			bReadBack
				? FString::Printf(TEXT("project.set_config: wrote \"%s\" but the engine reads back \"%s\" for [%s] %s"), *Value, *ReadBack, *Section, *Key)
				: FString::Printf(TEXT("project.set_config: the value was written but [%s] %s cannot be read back from %s"), *Section, *Key, *ConfigName),
			Result);
		return;
	}
	if (!bPersisted)
	{
		UAL_CommandUtils::SendError(
			RequestId, 500,
			FString::Printf(TEXT("project.set_config: [%s] %s is set in memory but writing %s to disk failed (read-only file, source control lock, or -nowrite). It will be lost when the editor restarts."), *Section, *Key, *ConfigFileName),
			Result);
		return;
	}

	UE_LOG(LogUALEditor, Log, TEXT("project.set_config: %s [%s] %s = %s"), *ConfigName, *Section, *Key, *ReadBack);

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

void FUAL_EditorCommands::Handle_AnalyzeUProject(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const FString ProjectFilePath = FPaths::GetProjectFilePath();
	FString ProjectFileContent;
	
	if (!FFileHelper::LoadFileToString(ProjectFileContent, *ProjectFilePath))
	{
		UAL_CommandUtils::SendError(RequestId, 500, FString::Printf(TEXT("Failed to read .uproject file: %s"), *ProjectFilePath));
		return;
	}

	TSharedPtr<FJsonObject> ProjectJson;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ProjectFileContent);
	
	if (!FJsonSerializer::Deserialize(Reader, ProjectJson) || !ProjectJson.IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to parse .uproject file as JSON"));
		return;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// 提取 EngineAssociation
	FString EngineAssociation;
	if (ProjectJson->TryGetStringField(TEXT("EngineAssociation"), EngineAssociation))
	{
		Result->SetStringField(TEXT("engine_association"), EngineAssociation);
	}

	// 提取 TargetPlatforms
	const TArray<TSharedPtr<FJsonValue>>* TargetPlatforms = nullptr;
	if (ProjectJson->TryGetArrayField(TEXT("TargetPlatforms"), TargetPlatforms))
	{
		TArray<TSharedPtr<FJsonValue>> PlatformArray;
		for (const TSharedPtr<FJsonValue>& Value : *TargetPlatforms)
		{
			if (Value.IsValid() && Value->Type == EJson::String)
			{
				PlatformArray.Add(MakeShared<FJsonValueString>(Value->AsString()));
			}
		}
		if (PlatformArray.Num() > 0)
		{
			Result->SetArrayField(TEXT("target_platforms"), PlatformArray);
		}
	}

	// 提取 Modules
	const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
	if (ProjectJson->TryGetArrayField(TEXT("Modules"), Modules))
	{
		TArray<TSharedPtr<FJsonValue>> ModuleArray;
		for (const TSharedPtr<FJsonValue>& Value : *Modules)
		{
			if (Value.IsValid() && Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> ModuleObj = Value->AsObject();
				if (ModuleObj.IsValid())
				{
					// 保留完整的模块对象（包含 Name, Type, LoadingPhase 等字段）
					ModuleArray.Add(MakeShared<FJsonValueObject>(ModuleObj));
				}
			}
		}
		if (ModuleArray.Num() > 0)
		{
			Result->SetArrayField(TEXT("modules"), ModuleArray);
		}
	}

	// 提取 Plugins（复�?BuildProjectInfo 中的逻辑�?
	const TArray<TSharedPtr<FJsonValue>>* ProjectPlugins = nullptr;
	if (ProjectJson->TryGetArrayField(TEXT("Plugins"), ProjectPlugins) && ProjectPlugins)
	{
		IPluginManager& PluginManager = IPluginManager::Get();

		TArray<TSharedPtr<FJsonValue>> PluginArray;

		for (const TSharedPtr<FJsonValue>& Value : *ProjectPlugins)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				continue;
			}

			const TSharedPtr<FJsonObject> PluginDecl = Value->AsObject();
			if (!PluginDecl.IsValid())
			{
				continue;
			}

			FString PluginName;
			if (!PluginDecl->TryGetStringField(TEXT("Name"), PluginName))
			{
				PluginDecl->TryGetStringField(TEXT("name"), PluginName);
			}
			if (PluginName.IsEmpty())
			{
				continue;
			}

			bool bEnabled = true;
			if (!PluginDecl->TryGetBoolField(TEXT("Enabled"), bEnabled))
			{
				PluginDecl->TryGetBoolField(TEXT("enabled"), bEnabled);
			}

			TSharedPtr<FJsonObject> PluginObj = MakeShared<FJsonObject>();
			PluginObj->SetStringField(TEXT("name"), PluginName);
			PluginObj->SetBoolField(TEXT("enabled"), bEnabled);

			// 尝试补全插件元信�?
			const TSharedPtr<IPlugin> Plugin = PluginManager.FindPlugin(PluginName);
			if (Plugin.IsValid())
			{
				PluginObj->SetStringField(TEXT("version_name"), Plugin->GetDescriptor().VersionName);
				PluginObj->SetStringField(TEXT("category"), Plugin->GetDescriptor().Category);
				PluginObj->SetStringField(TEXT("base_dir"), Plugin->GetBaseDir());
				PluginObj->SetStringField(TEXT("friendly_name"), Plugin->GetDescriptor().FriendlyName);
				PluginObj->SetStringField(TEXT("description"), Plugin->GetDescriptor().Description);
			}
			else
			{
				PluginObj->SetStringField(TEXT("version_name"), TEXT(""));
				PluginObj->SetStringField(TEXT("category"), TEXT(""));
				PluginObj->SetStringField(TEXT("base_dir"), TEXT(""));
				PluginObj->SetStringField(TEXT("friendly_name"), TEXT(""));
				PluginObj->SetStringField(TEXT("description"), TEXT(""));
			}

			PluginArray.Add(MakeShared<FJsonValueObject>(PluginObj));
		}

		if (PluginArray.Num() > 0)
		{
			Result->SetArrayField(TEXT("plugins"), PluginArray);
		}
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/**
 * 选出要抓的那扇窗口。
 *
 * ## 为什么不能只看 `GetActiveTopLevelWindow()`
 *
 * 这条命令是用户在虚幻盒子里敲一句话触发的。敲字的那一刻，编辑器的每扇窗口都
 * 已经失活 —— Slate 在 `ProcessWindowActivatedEvent` 收到 Deactivate 时会把
 * `ActiveTopLevelWindow` 直接 `Reset()`（SlateApplication.cpp）。于是这里拿到的
 * 永远是空，原来的兜底 `GetInteractiveTopLevelWindows()[0]` 又永远是主关卡窗口：
 * 用户开着蓝图编辑器说「看看我这张图」，拍回去的却是场景，而且模型分不出来。
 *
 * ## 取法，按优先级
 *
 * 1. 正在活跃的模态弹窗 —— 「界面上弹了个框」要的就是它。点了名也一样：模态
 *    挡着的时候别的窗口收不到输入，去 `FocusWindow()` 它是空操作，拍回来的图
 *    还可能被弹窗压着；回 `modal` 让调用方知道被挡了。
 * 2. `Requested` 点名的资产编辑器（名字或路径），通过它的 major tab 反查所在窗口。
 *    编辑器停靠在主窗口里也照样正确 —— 反查回来就是主窗口，`FocusWindow()` 会把
 *    那个标签页切到前面。`level` / `main` 点名主窗口；`pie` 点名游戏视口所在的
 *    窗口（试玩采样用）。编辑器开着但反查不到窗口（world-centric 那类没有 major
 *    tab）时退回主窗口、回 `fallback`，和「没开」分开 —— 否则报错里列的
 *    「已打开的编辑器」会包含点名的那个，调用方只会拿同一个名字反复重试。
 * 3. **用户最后用过的那扇窗口**：Slate 每次激活窗口都会
 *    `FSlateWindowHelper::BringWindowToFront` 把它挪到 `SlateWindows` 的末尾，
 *    所以从后往前扫第一扇普通、可见的窗口，就是失活前用户看着的那扇。
 *    引擎自己在前台时 `GetActiveTopLevelWindow()` 也是这一扇（激活先重排再记
 *    活跃），不用单独判。
 * 4. 主窗口 —— 只在上面全落空时。
 *
 * `OutSource` 把走的是哪一条回上去（modal / requested / last_active / fallback），
 * 和视口截图回 `camera_source` 是同一条规矩：一张图的结论对不对取决于它从哪儿拍的。
 */
static TSharedPtr<SWindow> UAL_PickWindowToCapture(
	const FString& Requested,
	bool bBringToFront,
	FString& OutSource,
	FString& OutError,
	int32& OutErrorCode)
{
	FSlateApplication& SlateApp = FSlateApplication::Get();

	if (TSharedPtr<SWindow> Modal = SlateApp.GetActiveModalWindow())
	{
		OutSource = TEXT("modal");
		return Modal;
	}

	if (!Requested.IsEmpty())
	{
		TSharedPtr<SWindow> Root = FGlobalTabmanager::Get()->GetRootWindow();

		if (Requested.Equals(TEXT("level"), ESearchCase::IgnoreCase) || Requested.Equals(TEXT("main"), ESearchCase::IgnoreCase))
		{
			if (Root.IsValid())
			{
				OutSource = TEXT("requested");
				return Root;
			}
		}

		// 试玩采样：游戏视口在哪扇窗口就拍哪扇。InProcess 的 PIE 视口在关卡编辑器里，
		// 用户最后用过的却可能是浮在外面的蓝图编辑器 —— 按「最后用过」采样会
		// 每一帧都拍到蓝图图表，游戏里发生的事一帧都没有
		if (Requested.Equals(TEXT("pie"), ESearchCase::IgnoreCase))
		{
			TSharedPtr<SWindow> GameWindow = (GEngine && GEngine->GameViewport) ? GEngine->GameViewport->GetWindow() : nullptr;
			if (GameWindow.IsValid())
			{
				OutSource = TEXT("requested");
				return GameWindow;
			}
			if (Root.IsValid())
			{
				OutSource = TEXT("fallback");
				return Root;
			}
		}

		UAssetEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		TArray<FString> OpenNames;
		if (Subsystem)
		{
			// "/Game/BP/BP_Door.BP_Door"、"/Game/BP/BP_Door"、"BP_Door" 都认
			const FString RequestedName = FPaths::GetBaseFilename(Requested);
			for (UObject* Asset : Subsystem->GetAllEditedAssets())
			{
				if (!Asset) continue;
				OpenNames.Add(Asset->GetName());

				const bool bMatch =
					Asset->GetName().Equals(RequestedName, ESearchCase::IgnoreCase) ||
					Asset->GetPathName().Equals(Requested, ESearchCase::IgnoreCase) ||
					Asset->GetOutermost()->GetName().Equals(Requested, ESearchCase::IgnoreCase);
				if (!bMatch) continue;

				IAssetEditorInstance* Instance = Subsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false);
				if (!Instance) continue;

				// 停靠成标签页时，光把窗口拿到前面还不够 —— 标签页本身也得切到前面，
				// 不然抓到的是同一扇窗口里别的标签
				if (bBringToFront)
				{
					Instance->FocusWindow();
				}

				TSharedPtr<FTabManager> TabManager = Instance->GetAssociatedTabManager();
				TSharedPtr<SDockTab> OwnerTab = TabManager.IsValid() ? TabManager->GetOwnerTab() : nullptr;
				TSharedPtr<SWindow> Window = OwnerTab.IsValid() ? OwnerTab->GetParentWindow() : nullptr;
				if (Window.IsValid())
				{
					OutSource = TEXT("requested");
					return Window;
				}

				// 编辑器开着，但反查不到它的窗口：退回主窗口并说明，别报「没开」
				if (Root.IsValid())
				{
					OutSource = TEXT("fallback");
					return Root;
				}
			}
		}

		OutErrorCode = 404;
		OutError = FString::Printf(
			TEXT("No open asset editor matches '%s'. Open editors: %s"),
			*Requested,
			OpenNames.Num() > 0 ? *FString::Join(OpenNames, TEXT(", ")) : TEXT("(none)"));
		return nullptr;
	}

	const TArray<TSharedRef<SWindow>>& Windows = SlateApp.GetTopLevelWindows();
	for (int32 Index = Windows.Num() - 1; Index >= 0; --Index)
	{
		const TSharedRef<SWindow>& Window = Windows[Index];
		if (Window->IsRegularWindow() && Window->IsVisible())
		{
			OutSource = TEXT("last_active");
			return Window;
		}
	}

	TArray<TSharedRef<SWindow>> Interactive = SlateApp.GetInteractiveTopLevelWindows();
	if (Interactive.Num() > 0)
	{
		OutSource = TEXT("fallback");
		return Interactive[0];
	}

	OutErrorCode = 404;
	OutError = TEXT("No valid window to capture");
	return nullptr;
}

/**
 * 抓一扇编辑器窗口，编码成 PNG 存盘。选哪扇见 `UAL_PickWindowToCapture`。
 *
 * ## 为什么抽成函数
 *
 * `pie.run` 的多帧采样要在游戏跑着的时候每隔几百毫秒抓一张**带界面**的画面，
 * 抓的和这里是同一块像素。各写一份的话，「最小化时 PrintWindow 回白图而且
 * 照样成功」这类坑要踩两遍，而踩中的表现是一张看着正常的空白图。
 *
 * ## bBringToFront
 *
 * 一次性命令要把窗口拿到前面 —— 用户问的就是「界面上弹的那个框」。
 * 试玩采样**绝不能**这么做：游戏跑着的时候每抓一帧抢一次焦点，等于每隔
 * 半秒往用户脸上弹一次窗，而且抢焦点本身会打断玩家输入。
 *
 * 最小化的还原成窗口一律要做，与 bBringToFront 无关：PrintWindow 对最小化
 * 窗口抓回来的是一张纯白图，并且照常返回成功，事后从像素里分不出来。
 */
static bool UAL_CaptureActiveWindowToFile(
	const FString& DesiredName,
	const FString& RequestedWindow,
	bool bBringToFront,
	FString& OutPath,
	FIntPoint& OutSize,
	FString& OutWindowTitle,
	FString& OutWindowSource,
	FString& OutError,
	int32& OutErrorCode)
{
    OutErrorCode = 500;

    if (!FSlateApplication::IsInitialized()) {
		OutError = TEXT("Slate Application is not initialized");
		return false;
	}
    FSlateApplication& SlateApp = FSlateApplication::Get();
    TSharedPtr<SWindow> TargetWindow = UAL_PickWindowToCapture(RequestedWindow, bBringToFront, OutWindowSource, OutError, OutErrorCode);
    if (!TargetWindow.IsValid())
    {
        return false;
    }
    OutWindowTitle = TargetWindow->GetTitle().ToString();

    // 确保窗口是可见的
	// Check IsMinimized via NativeWindow if possible
	if (TargetWindow->GetNativeWindow().IsValid() && TargetWindow->GetNativeWindow()->IsMinimized())
	{
		TargetWindow->Restore();
	}

	if (bBringToFront)
	{
		TargetWindow->BringToFront();

		if (TargetWindow->GetNativeWindow().IsValid())
		{
			TargetWindow->GetNativeWindow()->SetWindowFocus();
		}
	}

    TArray<FColor> Bitmap;
    FIntPoint TargetSize(0, 0);
    // Windows keeps GDI capture to preserve its HDR/Gamma handling.
#if PLATFORM_WINDOWS
    // 获取原生窗口句柄
    HWND Hwnd = nullptr;
    if (TargetWindow->GetNativeWindow().IsValid())
    {
        Hwnd = static_cast<HWND>(TargetWindow->GetNativeWindow()->GetOSWindowHandle());
    }

    if (!Hwnd)
    {
        OutError = TEXT("Failed to get native window handle");
        return false;
    }

    // 获取窗口客户区大小
    RECT ClientRect;
    GetClientRect(Hwnd, &ClientRect);
    int32 Width = ClientRect.right - ClientRect.left;
    int32 Height = ClientRect.bottom - ClientRect.top;
    TargetSize = FIntPoint(Width, Height);

    if (Width <= 0 || Height <= 0)
    {
        OutError = TEXT("Invalid window size");
        return false;
    }

    // 创建兼容 DC 和位图
    HDC WindowDC = GetDC(Hwnd);
    HDC MemDC = CreateCompatibleDC(WindowDC);
    HBITMAP HBitmap = CreateCompatibleBitmap(WindowDC, Width, Height);
    HBITMAP OldBitmap = (HBITMAP)SelectObject(MemDC, HBitmap);

    // 使用 PrintWindow 捕获窗口内容（包括 DWM 合成的内容）
    // PW_RENDERFULLCONTENT (0x00000002) 可以捕获 DWM 合成内容
    PrintWindow(Hwnd, MemDC, 0x00000002);

    // 准备位图信息
    BITMAPINFO BitmapInfo;
    ZeroMemory(&BitmapInfo, sizeof(BITMAPINFO));
    BitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    BitmapInfo.bmiHeader.biWidth = Width;
    BitmapInfo.bmiHeader.biHeight = -Height; // 负值表示自上而下
    BitmapInfo.bmiHeader.biPlanes = 1;
    BitmapInfo.bmiHeader.biBitCount = 32;
    BitmapInfo.bmiHeader.biCompression = BI_RGB;

    // 读取像素数据
    Bitmap.SetNum(Width * Height);
    GetDIBits(MemDC, HBitmap, 0, Height, Bitmap.GetData(), &BitmapInfo, DIB_RGB_COLORS);

    // 清理 GDI 资源
    SelectObject(MemDC, OldBitmap);
    DeleteObject(HBitmap);
    DeleteDC(MemDC);
    ReleaseDC(Hwnd, WindowDC);

#else
    FIntVector ScreenshotSize(0, 0, 0);
    if (!SlateApp.TakeScreenshot(TargetWindow.ToSharedRef(), Bitmap, ScreenshotSize))
    {
        OutError = TEXT("Failed to capture the editor window");
        return false;
    }
    TargetSize = FIntPoint(ScreenshotSize.X, ScreenshotSize.Y);
#endif

    if (!UAL_WindowCapture::PreparePixels(Bitmap, TargetSize))
    {
        OutError = TEXT("Window capture returned invalid pixels");
        return false;
    }

    // PNG 编码与保存
    IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
    TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

    if (ImageWrapper.IsValid() && ImageWrapper->SetRaw(Bitmap.GetData(), Bitmap.Num() * sizeof(FColor), TargetSize.X, TargetSize.Y, ERGBFormat::BGRA, 8))
    {
        TArray64<uint8> CompressedData = ImageWrapper->GetCompressed();

        FString CleanName = FPaths::GetCleanFilename(DesiredName);
        if (CleanName.IsEmpty()) CleanName = FDateTime::Now().ToString(TEXT("UAL_AppShot_%Y%m%d_%H%M%S.png"));

        FString OutputDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots/UAL")));
        IFileManager::Get().MakeDirectory(*OutputDir, true);
        FString OutputPath = DesiredName.IsEmpty() || FPaths::IsRelative(DesiredName) ?
             FPaths::Combine(OutputDir, CleanName) : DesiredName;

        // 自动创建目录
		if (!DesiredName.IsEmpty() && !FPaths::IsRelative(DesiredName))
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true);
		}

        if (FFileHelper::SaveArrayToFile(CompressedData, *OutputPath))
        {
            OutPath = OutputPath;
            OutSize = TargetSize;
            return true;
        }
    }

    OutError = TEXT("Failed to save image");
    return false;
}

void FUAL_EditorCommands::Handle_CaptureAppWindow(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
    FString DesiredName;
    Payload->TryGetStringField(TEXT("filepath"), DesiredName);

    // 可选：点名拍哪个资产编辑器（名字或路径），或 level / main 点名主窗口。
    // 不传就拍用户最后用过的那扇窗口，见 UAL_PickWindowToCapture
    FString RequestedWindow;
    Payload->TryGetStringField(TEXT("window"), RequestedWindow);
    RequestedWindow.TrimStartAndEndInline();

    FString OutputPath;
    FString Error;
    FString WindowTitle;
    FString WindowSource;
    FIntPoint Size(0, 0);
    int32 ErrorCode = 500;
    // 一次性命令要把窗口拿到前面：用户问的就是屏幕上那个弹窗
    if (!UAL_CaptureActiveWindowToFile(DesiredName, RequestedWindow, /*bBringToFront=*/true, OutputPath, Size, WindowTitle, WindowSource, Error, ErrorCode))
    {
        UAL_CommandUtils::SendError(RequestId, ErrorCode, Error);
        return;
    }

    UE_LOG(LogUALEditor, Log, TEXT("editor.capture_app_window: window='%s' (%s) -> %s"), *WindowTitle, *WindowSource, *OutputPath);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("path"), OutputPath);
    Data->SetStringField(TEXT("filename"), FPaths::GetCleanFilename(OutputPath));
    Data->SetNumberField(TEXT("width"), Size.X);
    Data->SetNumberField(TEXT("height"), Size.Y);
    Data->SetBoolField(TEXT("saved"), true);
    Data->SetStringField(TEXT("window_title"), WindowTitle);
    Data->SetStringField(TEXT("window_source"), WindowSource);
    UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

/**
 * 编辑器当前的焦点和选中状态。
 *
 * 除了「开着哪些资产编辑器」，还报聚焦的图、图里选中的节点、关卡里选中的 Actor、
 * 内容浏览器里选中的资产 —— 用户说「把**这个**节点改成 X」时，靠的就是这些。
 * 组装逻辑在 UAL_FocusContext.cpp。
 */
void FUAL_EditorCommands::Handle_GetFocusContext(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
    TSharedPtr<FJsonObject> Result = FUAL_FocusContext::Build();

    const TArray<TSharedPtr<FJsonValue>>* OpenEditors = nullptr;
    Result->TryGetArrayField(TEXT("openEditors"), OpenEditors);

    FString FocusedName = TEXT("none");
    const TSharedPtr<FJsonObject>* Focused = nullptr;
    if (Result->TryGetObjectField(TEXT("focusedEditor"), Focused) && Focused && Focused->IsValid())
    {
        (*Focused)->TryGetStringField(TEXT("name"), FocusedName);
    }

    UE_LOG(LogUALEditor, Log, TEXT("editor.get_focus_context: %d open editors, focused: %s"),
        OpenEditors ? OpenEditors->Num() : 0, *FocusedName);

    UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
    UAL_CommandUtils::SendError(RequestId, 501, TEXT("editor.get_focus_context is only available in editor mode"));
#endif
}

// ============================================================================
// editor.save / editor.get_dirty —— 落盘
// ============================================================================

namespace
{
	/**
	 * 全引擎当前脏着、且真的能存的包。
	 *
	 * 判据在 `UAL_SavablePackage.h` 里统一定义 —— 这里原来有一份自己的，
	 * 漏了 `/Script/` 编译包，于是 `scope=all` 会去存 `/Script/SlateCore`
	 * 这种根本没有磁盘文件的包并整条报错。原因见那个头文件的长注释。
	 */
	TArray<UPackage*> UAL_AllDirtyPackages()
	{
		return UAL_AllDirtySavablePackages();
	}

	TSharedPtr<FJsonObject> UAL_PackageEntry(UPackage* Package)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("package"), Package->GetName());
		// 关卡和资产在调用方眼里是两种东西，标出来省它一次判断
		Entry->SetBoolField(TEXT("is_level"), Package->ContainsMap());
		return Entry;
	}
}

void FUAL_EditorCommands::Handle_GetDirty(const TSharedPtr<FJsonObject>& /*Payload*/, const FString RequestId)
{
	const TArray<UPackage*> Touched = FUAL_TouchedPackages::CollectDirty();
	TSet<FName> TouchedNames;
	for (UPackage* Package : Touched)
	{
		TouchedNames.Add(Package->GetFName());
	}

	TArray<TSharedPtr<FJsonValue>> TouchedJson;
	for (UPackage* Package : Touched)
	{
		if (UAL_IsSavablePackage(Package))
		{
			TouchedJson.Add(MakeShared<FJsonValueObject>(UAL_PackageEntry(Package)));
		}
	}

	// 其他来源改脏的（多半是用户自己在编辑器里改的）单列一栏。
	// 混在一起会让调用方以为那些也是自己的产出，进而想去保存它们。
	TArray<TSharedPtr<FJsonValue>> OtherJson;
	for (UPackage* Package : UAL_AllDirtyPackages())
	{
		if (!TouchedNames.Contains(Package->GetFName()))
		{
			OtherJson.Add(MakeShared<FJsonValueObject>(UAL_PackageEntry(Package)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetNumberField(TEXT("touched_count"), TouchedJson.Num());
	Result->SetNumberField(TEXT("other_count"), OtherJson.Num());
	Result->SetArrayField(TEXT("touched"), TouchedJson);
	Result->SetArrayField(TEXT("other"), OtherJson);
	Result->SetStringField(
		TEXT("note"),
		TEXT("'touched' was modified by this plugin; 'other' was modified elsewhere (most likely by the user by hand). editor.save defaults to saving only 'touched'."));

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

void FUAL_EditorCommands::Handle_Save(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Scope = TEXT("touched");
	Payload->TryGetStringField(TEXT("scope"), Scope);
	Scope = Scope.ToLower();

	TArray<UPackage*> ToSave;

	// scope=list 时点了名却没存的条目，分两类都要回报：没加载的、加载了但没改动的。
	// 以前只有「一个都没解析到」才报 NotFound，列了 5 个存了 3 个时另外 2 个无声消失，
	// 调用方会以为 5 个都落盘了（AGENTS.md §5 第 14 条）
	TArray<TSharedPtr<FJsonValue>> NotLoadedJson;
	TArray<TSharedPtr<FJsonValue>> SkippedJson;
	auto AddSkipped = [&SkippedJson](const FString& Asset, const FString& Reason)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset"), Asset);
		Entry->SetStringField(TEXT("reason"), Reason);
		SkippedJson.Add(MakeShared<FJsonValueObject>(Entry));
	};

	if (Scope == TEXT("list"))
	{
		const TArray<TSharedPtr<FJsonValue>>* AssetsArray = nullptr;
		if (!Payload->TryGetArrayField(TEXT("assets"), AssetsArray) || !AssetsArray || AssetsArray->Num() == 0)
		{
			UAL_CommandUtils::SendError(RequestId, 400, TEXT("scope=\"list\" requires a non-empty \"assets\" array"));
			return;
		}

		int32 ResolvedCount = 0;
		for (const TSharedPtr<FJsonValue>& Value : *AssetsArray)
		{
			FString AssetPath;
			if (!Value.IsValid() || !Value->TryGetString(AssetPath) || AssetPath.IsEmpty())
			{
				AddSkipped(TEXT("(empty or non-string entry)"), TEXT("Not an asset path"));
				continue;
			}

			// 传进来的可能是 /Game/A/B 也可能是 /Game/A/B.B，包名要的是前者
			FString PackageName = AssetPath;
			int32 DotIndex = INDEX_NONE;
			if (PackageName.FindChar(TEXT('.'), DotIndex))
			{
				PackageName = PackageName.Left(DotIndex);
			}

			UPackage* Package = FindPackage(nullptr, *PackageName);
			if (!Package)
			{
				// 没加载就谈不上有改动，直说而不是静默跳过
				NotLoadedJson.Add(MakeShared<FJsonValueString>(AssetPath));
				continue;
			}
			++ResolvedCount;
			if (!Package->IsDirty())
			{
				// 加载了但没有未保存的改动：磁盘上已经是这个状态，不用存，但要说一声
				AddSkipped(AssetPath, TEXT("No unsaved changes - already up to date on disk"));
				continue;
			}
			ToSave.AddUnique(Package);
		}

		if (ResolvedCount == 0)
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("not_loaded"), NotLoadedJson);
			Details->SetNumberField(TEXT("not_loaded_count"), NotLoadedJson.Num());
			if (SkippedJson.Num() > 0)
			{
				Details->SetArrayField(TEXT("skipped"), SkippedJson);
				Details->SetNumberField(TEXT("skipped_count"), SkippedJson.Num());
			}
			UAL_CommandUtils::SendError(
				RequestId, 404,
				TEXT("None of the listed assets are loaded in memory, so none of them have unsaved changes."),
				Details);
			return;
		}
	}
	else if (Scope == TEXT("all"))
	{
		ToSave = UAL_AllDirtyPackages();
	}
	else if (Scope == TEXT("touched"))
	{
		for (UPackage* Package : FUAL_TouchedPackages::CollectDirty())
		{
			if (UAL_IsSavablePackage(Package))
			{
				ToSave.Add(Package);
			}
		}
	}
	else
	{
		UAL_CommandUtils::SendError(
			RequestId, 400,
			FString::Printf(TEXT("Unknown scope \"%s\". Use \"touched\" (default), \"all\" or \"list\"."), *Scope));
		return;
	}

	// 存之前先数一下总共脏了多少 —— 用来算「我们没碰的还剩几个」
	const int32 DirtyBefore = UAL_AllDirtyPackages().Num();

	TArray<TSharedPtr<FJsonValue>> SavedJson;
	TArray<TSharedPtr<FJsonValue>> FailedJson;

	/**
	 * 空包不是拿来存的，是拿来删的。
	 *
	 * One File Per Actor 关卡里删掉一个 Actor，它那个 __ExternalActors__ 包会变脏
	 * 但里面已经没有东西了。对它调 Save 必然失败（没有可写的对象），于是永远
	 * 「Still dirty」—— 新用户那一轮反复撞的就是这个。编辑器 Ctrl+S 能过，是因为
	 * FEditorFileUtils::SaveDirtyPackages 把这类包交给 ObjectTools::CleanupAfterSuccessfulDelete：
	 * 版本控制里标记删除/回退、通知资产注册表、卸载。这里走同一条路，而不是自己
	 * 删文件 —— 自己删的话 Perforce 里那份还开着 edit，下次同步 Actor 又活回来。
	 *
	 * 清理**排在存盘之后**：它内部会 UnloadPackages 再 CollectGarbage，而 ToSave
	 * 是一个裸 `UPackage*` 数组、不是 GC 根，中途回收会让后面那轮解引用踩到野指针。
	 * 所以这里只先挑出来记下名字，等其余的都存完再一次性交给引擎。
	 */
	struct FUAL_EmptyPackage
	{
		UPackage* Package = nullptr;
		FString PackageName;
		bool bIsLevel = false;
		bool bHadFile = false;
	};
	TArray<FUAL_EmptyPackage> EmptyPackages;
	TSet<UPackage*> EmptyHandled;
	for (UPackage* Package : ToSave)
	{
		if (!Package || !Package->IsDirty() || !UPackage::IsEmptyPackage(Package))
		{
			continue;
		}
		EmptyHandled.Add(Package);

		const FString PackageName = Package->GetName();
		FString Filename;
		const bool bHasFile = FPackageName::TryConvertLongPackageNameToFilename(
				PackageName, Filename,
				Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension())
			&& IFileManager::Get().FileExists(*Filename);
		// 只读文件没接版本控制时引擎的清理会弹模态框，先挡住并说清楚
		if (bHasFile && IFileManager::Get().IsReadOnly(*Filename))
		{
			TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
			Failure->SetStringField(TEXT("package"), PackageName);
			Failure->SetStringField(TEXT("file"), Filename);
			Failure->SetStringField(TEXT("error"), TEXT("This package is empty (its actor/asset was deleted) so its file should be removed, but the file is read-only on disk. Check it out from version control or clear the read-only flag, then save again"));
			FailedJson.Add(MakeShared<FJsonValueObject>(Failure));
			continue;
		}

		// 名字先记下来：清理之后这个指针可能已经被回收，不能再解引用
		FUAL_EmptyPackage Entry;
		Entry.Package = Package;
		Entry.PackageName = PackageName;
		Entry.bIsLevel = Package->ContainsMap();
		Entry.bHadFile = bHasFile;
		EmptyPackages.Add(Entry);
	}

	for (UPackage* Package : ToSave)
	{
		// 空包留到最后统一清理，这一轮跳过
		if (!Package || EmptyHandled.Contains(Package) || !Package->IsDirty())
		{
			continue;
		}

		const FString PackageName = Package->GetName();
		FString Filename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
				PackageName,
				Filename,
				Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension()))
		{
			TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
			Failure->SetStringField(TEXT("package"), PackageName);
			Failure->SetStringField(TEXT("error"), TEXT("Could not resolve a file path for this package"));
			FailedJson.Add(MakeShared<FJsonValueObject>(Failure));
			continue;
		}

		auto Fail = [&](const FString& Reason)
		{
			TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
			Failure->SetStringField(TEXT("package"), PackageName);
			Failure->SetStringField(TEXT("file"), Filename);
			Failure->SetStringField(TEXT("error"), Reason);
			FailedJson.Add(MakeShared<FJsonValueObject>(Failure));
		};

		IFileManager& FileManager = IFileManager::Get();
		const bool bFileExists = FileManager.FileExists(*Filename);

		// 只读先说只读。让 Save 自己去撞只会得到一个笼统的 Error，原因就丢了
		if (bFileExists && FileManager.IsReadOnly(*Filename))
		{
			Fail(TEXT("File is read-only on disk (checked out to someone else, or the read-only attribute is set). Check it out from version control or clear the flag, then save again"));
			continue;
		}

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;

		// UPackage::Save 九个版本都回 FSavePackageResultStruct，原因就在 Result 里。
		// 以前用的 SavePackage 只回 bool、返回值还被丢掉，失败一律套一句
		// 「可能只读或被签出」—— 对着它调用方除了原样重试没有别的路。
		const FSavePackageResultStruct SaveResult = UPackage::Save(Package, nullptr, *Filename, SaveArgs);

		if (SaveResult.Result == ESavePackageResult::Success && !Package->IsDirty())
		{
			SavedJson.Add(MakeShared<FJsonValueObject>(UAL_PackageEntry(Package)));
			continue;
		}

		FString Reason;
		switch (SaveResult.Result)
		{
		case ESavePackageResult::Success:
			Reason = TEXT("Engine reported success but the package is still dirty; something re-dirtied it during save (a construction script or a post-save hook). Try saving again");
			break;
		case ESavePackageResult::Canceled:
			Reason = TEXT("Save was canceled (an editor dialog or a save hook refused it)");
			break;
		case ESavePackageResult::Error:
			Reason = bFileExists
				? TEXT("Engine failed to write the file. The Output Log has the exact error (LogSavePackage); common causes: the file is locked by another process, or the disk is full")
				: TEXT("Engine failed to create the file. The Output Log has the exact error (LogSavePackage); common causes: the target folder is not writable, or the path is too long");
			break;
		default:
			Reason = FString::Printf(TEXT("Save returned result code %d (see LogSavePackage in the Output Log)"), static_cast<int32>(SaveResult.Result));
			break;
		}
		Fail(Reason);
	}

	/**
	 * 空包交给引擎清理，然后**回读确认**。
	 *
	 * 不能拿「调过 CleanupAfterSuccessfulDelete」当成删掉了：引擎对还被别处引用的
	 * 包会直接跳过（`if (bIsReferenced) { PackagesToDelete.RemoveAt(...); }`），
	 * 版本控制那步也可能失败，两种情况文件都还在。不查一眼就报 deleted，
	 * 等于对调用方说了句假话，而同一份响应里的 still_dirty_count 又会打自己的脸。
	 */
	if (EmptyPackages.Num() > 0)
	{
		TArray<UPackage*> ToCleanup;
		for (const FUAL_EmptyPackage& Empty : EmptyPackages)
		{
			ToCleanup.Add(Empty.Package);
		}
		ObjectTools::CleanupAfterSuccessfulDelete(ToCleanup, /*bPerformReferenceCheck=*/true);

		// 这之后 Empty.Package 可能已经被回收，只按名字查
		for (const FUAL_EmptyPackage& Empty : EmptyPackages)
		{
			if (FPackageName::DoesPackageExist(Empty.PackageName))
			{
				TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
				Failure->SetStringField(TEXT("package"), Empty.PackageName);
				Failure->SetStringField(
					TEXT("error"),
					TEXT("This package is empty (its actor/asset was deleted) and should have been removed, but its file is still on disk. The engine skips packages that something else still references, and a version-control delete can also fail - check the Output Log, then close whatever still holds it and save again"));
				FailedJson.Add(MakeShared<FJsonValueObject>(Failure));
				continue;
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("package"), Empty.PackageName);
			Entry->SetBoolField(TEXT("is_level"), Empty.bIsLevel);
			Entry->SetBoolField(TEXT("deleted"), true);
			Entry->SetStringField(TEXT("note"), Empty.bHadFile
				? TEXT("empty package: its file was removed (the actor/asset it held was deleted); version control was told to delete it")
				: TEXT("empty package that never reached disk: nothing to write, marked clean"));
			SavedJson.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	if (Scope == TEXT("touched") && FailedJson.Num() == 0)
	{
		// 全存下去了，记录清空。留着只会让下次 CollectDirty 白跑一遍
		FUAL_TouchedPackages::Clear();
	}

	const int32 DirtyAfter = UAL_AllDirtyPackages().Num();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), FailedJson.Num() == 0);
	Result->SetStringField(TEXT("scope"), Scope);
	Result->SetNumberField(TEXT("saved_count"), SavedJson.Num());
	Result->SetArrayField(TEXT("saved"), SavedJson);
	Result->SetNumberField(TEXT("failed_count"), FailedJson.Num());
	if (FailedJson.Num() > 0)
	{
		Result->SetArrayField(TEXT("failed"), FailedJson);
	}
	if (Scope == TEXT("list"))
	{
		// 始终带上，空数组也带：调用方靠它确认「点名的每一个都有下落」
		Result->SetNumberField(TEXT("not_loaded_count"), NotLoadedJson.Num());
		Result->SetArrayField(TEXT("not_loaded"), NotLoadedJson);
		Result->SetNumberField(TEXT("skipped_count"), SkippedJson.Num());
		Result->SetArrayField(TEXT("skipped"), SkippedJson);
	}

	/**
	 * 剩下几个没存的要说出来。
	 *
	 * 默认只存我们自己改的，所以用户手改的东西会原样留着脏 —— 那是对的行为，
	 * 但如果不报出来，调用方存完看到「成功」就会以为工程干净了，
	 * 然后可能去做重启编辑器之类会丢东西的事。
	 */
	Result->SetNumberField(TEXT("still_dirty_count"), DirtyAfter);
	if (DirtyAfter > 0 && Scope == TEXT("touched"))
	{
		Result->SetStringField(
			TEXT("note"),
			FString::Printf(
				TEXT("%d package(s) are still unsaved - they were modified outside this plugin (most likely by the user) and were deliberately left alone. Use editor.get_dirty to list them, or scope=\"all\" to save everything."),
				DirtyAfter));
	}
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);

	// DirtyBefore 只用于日志，帮定位「明明说存了却还是脏的」这类问题
	UE_LOG(LogTemp, Log, TEXT("[UAL] editor.save scope=%s: %d saved, %d failed, dirty %d -> %d"),
		*Scope, SavedJson.Num(), FailedJson.Num(), DirtyBefore, DirtyAfter);
}

// ============================================================================
// editor.collect_garbage —— 强制回收
// ============================================================================

namespace
{
	/** 当前进程用了多少 MB（物理内存），拿不到就回 0 */
	double UAL_UsedPhysicalMB()
	{
		const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
		return static_cast<double>(Stats.UsedPhysical) / (1024.0 * 1024.0);
	}

	/** 当前 UObject 总数 */
	int32 UAL_CountObjects()
	{
		int32 Count = 0;
		for (FThreadSafeObjectIterator It; It; ++It)
		{
			++Count;
		}
		return Count;
	}
}

void FUAL_EditorCommands::Handle_CollectGarbage(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	bool bFullPurge = true;
	Payload->TryGetBoolField(TEXT("full_purge"), bFullPurge);

	const int32 ObjectsBefore = UAL_CountObjects();
	const double MBBefore = UAL_UsedPhysicalMB();
	const double StartSeconds = FPlatformTime::Seconds();

	/**
	 * 直接调 `CollectGarbage` 而不是 `GEngine->ForceGarbageCollection()`。
	 *
	 * 后者只是把「下一帧回收一下」记在标志位上，函数立刻返回 —— 那样这里
	 * 量到的前后差值全是 0，响应会说「回收了 0 个对象」，看起来像没生效。
	 * 报不出数的维护命令没有意义，所以要同步跑完再量。
	 */
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, bFullPurge);

	const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	const int32 ObjectsAfter = UAL_CountObjects();
	const double MBAfter = UAL_UsedPhysicalMB();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetNumberField(TEXT("objects_before"), ObjectsBefore);
	Result->SetNumberField(TEXT("objects_after"), ObjectsAfter);
	Result->SetNumberField(TEXT("objects_freed"), FMath::Max(0, ObjectsBefore - ObjectsAfter));
	Result->SetNumberField(TEXT("mb_before"), FMath::RoundToDouble(MBBefore));
	Result->SetNumberField(TEXT("mb_after"), FMath::RoundToDouble(MBAfter));
	// 内存可能不降反升（回收本身要分配、其他线程也在动），负数没有意义，夹到 0。
	// 真实情况写在 objects_freed 里，那个数才是这次回收实际干掉的东西。
	Result->SetNumberField(TEXT("mb_freed"), FMath::Max(0.0, FMath::RoundToDouble(MBBefore - MBAfter)));
	Result->SetNumberField(TEXT("elapsed_ms"), FMath::RoundToInt(ElapsedMs));
	Result->SetBoolField(TEXT("full_purge"), bFullPurge);

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// editor.restart —— 重启编辑器
// ============================================================================

void FUAL_EditorCommands::Handle_RestartEditor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	bool bForce = false;
	Payload->TryGetBoolField(TEXT("force"), bForce);

	/**
	 * 重启会把内存里没存的东西全部丢掉，且无法撤销。和 level.open 同样的规矩：
	 * 默认拒绝并把清单摆出来，让调用方先去保存。
	 *
	 * 判据必须和 level.open 用同一个（`UAL_SavablePackage.h`）。这里原来自己数了
	 * 一遍，只排 transient，**漏了 `/Script/` 模块包** —— 而引擎运行中把
	 * `/Script/SlateCore` 标脏是家常便饭，用户没有任何办法让它变干净。
	 * 结果是这道保护每次都拦、只能靠 `force=true` 绕过，久而久之变成无脑 force，
	 * 真有一张没保存的关卡时也一起丢了。level.open 2026-08-31 修过这个坑，
	 * 这里当时没跟上。
	 */
	if (!bForce)
	{
		const FUAL_DestructiveOpDirty Dirty = UAL_DirtyPackagesForDestructiveOp();
		if (Dirty.Num() > 0)
		{
			UAL_CommandUtils::SendError(
				RequestId,
				409,
				FString::Printf(
					TEXT("Refusing to restart the editor: %d package(s) have unsaved changes that would be lost."),
					Dirty.Num()),
				UAL_UnsavedRefusalDetails(Dirty));
			return;
		}
	}

	/**
	 * 先回响应，再重启。
	 *
	 * 重启会关掉编辑器进程，WebSocket 跟着断 —— 响应要是排在重启后面，
	 * 它永远发不出去，调用方只能等到超时，然后把一次**成功的**重启
	 * 记成失败。
	 */
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetBoolField(TEXT("restarting"), true);
	Result->SetStringField(
		TEXT("note"),
		TEXT("The editor is restarting now. This connection will drop; wait for it to come back before sending more commands."));
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);

	// 延到下一帧再重启，给上面那条响应留出发送时间。
	// 同一帧里直接调的话，消息还在发送队列里进程就没了。
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float /*DeltaTime*/) -> bool
		{
			// bWarn=false：我们上面已经自己做过脏检查了。留 true 会弹一个
			// 「要保存吗」的模态框 —— 无人值守时没人点，编辑器就卡在那里，
			// 比不重启更糟（同 SaveLevelAs 那个坑）。
			FUnrealEdMisc::Get().RestartEditor(/*bWarn=*/false);
			return false; // 只跑一次
		}),
		0.0f);
}

// ============================================================================
// pie.run —— PIE 试玩：跑一段时间，回一份「这段时间里发生了什么」
// ============================================================================

namespace
{
	/** 每一栏日志最多留多少条。蓝图里一个 Tick 死循环能在几秒刷出几十万行 */
	constexpr int32 UAL_PieMaxEntriesPerBucket = 200;

	/**
	 * 一次试玩最多抓几帧。
	 *
	 * 上限**不是**因为一张图装不下 —— 盒子那侧每九格切一张，帧数多了就多给
	 * 几张图，格子大小恒定（见 `contactSheet.ts` 文件头）。所以这个数不受
	 * 「一张图能放几格」约束。
	 *
	 * 真正的约束是**抓帧本身会打扰被测的那个运行**：每一帧都要把渲染结果从
	 * GPU 读回内存再编码成 PNG，游戏当场卡一下。抓得越密，测出来的「运行效果」
	 * 越不像真的运行效果 —— 到某个密度，这个工具测的就是它自己了。
	 *
	 * 36 是留给「跑满 60 秒也想看全程」的上限，不是推荐值。每一帧实际花了多久
	 * 都会回在 `capture_ms` 里：与其在这里拍一个我们没测过的数，不如把真实代价
	 * 交出去，让调用方和用户看着办。
	 */
	constexpr int32 UAL_PieMaxFrames = 36;

	/**
	 * 采样帧的渲染分辨率。**故意比收尾那张（1280×720）小。**
	 *
	 * 它们是拼图里的一格，最终显示宽度只有三四百像素，渲成 1280 宽纯粹是
	 * 多花一次回读和编码的时间 —— 而那个时间是从正在跑的游戏身上扣的。
	 * 640 已经比任何一格的显示尺寸都大，缩下去还有余量。
	 */
	constexpr int32 UAL_PieFrameWidth = 640;
	constexpr int32 UAL_PieFrameHeight = 360;

	/**
	 * 只在 PIE 会话窗口内缓冲日志。
	 *
	 * 为什么不用现成的 `FUAL_LogInterceptor`：那个把日志**流式**推成事件，
	 * 默认还关着。试玩要的是「这一次会话的快照」，随响应一起回 —— 流式推
	 * 过去得在 app 侧按时间重新拼装，边界和顺序都靠不住，而且调用方拿到的
	 * 是一堆事件而不是这次调用的结果。
	 */
	class FUAL_PieLogCapture : public FOutputDevice
	{
	public:
		TArray<FString> PrintStrings;
		TArray<FString> Errors;
		TArray<FString> Warnings;

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			// 这个类别就是蓝图 PrintString 的去处 —— 蓝图开发者唯一的调试手段。
			// 游戏里所有「我到这一步了」「血量是 3」都从这儿出，
			// 是整份报告里最有价值的一栏。
			static const FName BlueprintUserMessages(TEXT("LogBlueprintUserMessages"));
			// 网络层自己的日志不收，别把连接噪音混进游戏行为里
			static const FName NetworkLogCategory(TEXT("LogUALNetwork"));

			if (Category == NetworkLogCategory)
			{
				return;
			}

			const FString Line(V);

			if (Category == BlueprintUserMessages)
			{
				Push(PrintStrings, Line);
				PushTimeline(TEXT("print"), Line);
				return;
			}

			if (Verbosity == ELogVerbosity::Error || Verbosity == ELogVerbosity::Fatal)
			{
				const FString Tagged = FString::Printf(TEXT("[%s] %s"), *Category.ToString(), *Line);
				Push(Errors, Tagged);
				PushTimeline(TEXT("error"), Tagged);
			}
			else if (Verbosity == ELogVerbosity::Warning)
			{
				const FString Tagged = FString::Printf(TEXT("[%s] %s"), *Category.ToString(), *Line);
				Push(Warnings, Tagged);
				PushTimeline(TEXT("warning"), Tagged);
			}
		}

		bool HasErrors() const { return Errors.Num() > 0; }

		/**
		 * 按时间顺序的全部日志，给 `pie.observe` 按游标增量读。
		 *
		 * 和上面三个桶**分开存**，理由是两者要的东西相反：报告要「最早那几条」
		 * （第一条错误通常才是根因），而边跑边看的人要「刚才那一步之后冒出来的」——
		 * 桶满了之后新日志一条都进不来，拿桶做增量读，跑到后半段就永远是空的。
		 *
		 * 这里满了丢**最旧**的一截。Seq 不重排，所以读的一方能从「游标之后的第一条
		 * 已经不是游标本身」看出中间丢过东西。
		 */
		TArray<FUAL_EditorCommands::FPieLogLine> Timeline;
		int32 NextSeq = 0;
		/** PIE 真正起来的那一刻；之前的日志 At 记 0 */
		double PlayStartedAt = 0.0;

	private:
		static constexpr int32 TimelineCap = 2000;

		void PushTimeline(const TCHAR* Kind, const FString& Text)
		{
			if (Timeline.Num() >= TimelineCap)
			{
				Timeline.RemoveAt(0, TimelineCap / 4);
			}
			FUAL_EditorCommands::FPieLogLine& Entry = Timeline.AddDefaulted_GetRef();
			Entry.Seq = NextSeq++;
			Entry.At = PlayStartedAt > 0.0 ? FPlatformTime::Seconds() - PlayStartedAt : 0.0;
			Entry.Kind = Kind;
			Entry.Text = Text.Left(500);
		}

		static void Push(TArray<FString>& Bucket, const FString& Line)
		{
			// 留最早的：第一条错误通常才是根因，后面多半是它的连锁反应
			if (Bucket.Num() < UAL_PieMaxEntriesPerBucket)
			{
				Bucket.Add(Line);
			}
		}
	};

	/**
	 * 一次试玩会话的全部状态。
	 *
	 * 静态单例而不是每次 new：同一时间只允许一个会话（PIE 本来也只有一个），
	 * 并发进来的第二个请求要被明确拒绝，而不是把状态搅乱。
	 */
	struct FUAL_PieSession
	{
		bool bActive = false;
		FString RequestId;

		double StartRequestedAt = 0.0;
		double PlayStartedAt = 0.0;
		double DurationSeconds = 5.0;
		bool bStopOnError = false;
		bool bWantScreenshot = true;

		bool bPlayStarted = false;
		/** 收尾已经开始，防止 ticker 和 EndPIE 回调重复发响应 */
		bool bFinishing = false;
		FString EndedBy;

		/** `pie.stop` 要求提前收尾。ticker 下一次 tick 按「时间到」那条路收 */
		bool bStopRequested = false;
		FString StopReason;

		/**
		 * 固定步长。0 = 没开。
		 *
		 * 编辑器失焦时被节流到个位数 fps（实测 59.4 → 11.4），而位移跟的是
		 * **游戏时间**不是帧数 —— 同样注入 30 帧，两台机器上角色走出去的距离能差
		 * 1.74 倍。锁住步长之后每一帧都是同样长的游戏时间，「按住 12 帧」才在
		 * 任何机器、前台后台都是同一件事。
		 *
		 * 改的是 GEngine 上的运行时值，不写 ini；会话每条退出路径都恢复原值。
		 */
		float FixedFps = 0.0f;
		bool bFixedStepApplied = false;
		bool bPrevUseFixedFrameRate = false;
		float PrevFixedFrameRate = 30.0f;

		int32 ActorsAtStart = 0;
		FString ScreenshotPath;
		FString ScreenshotError;

		/**
		 * 收尾那一刻 PIE 世界的关卡组成。
		 *
		 * 必须在插件内部当场取：PIE 世界一销毁，「游戏里到底加载了哪几层」
		 * 事后无论如何都查不回来。而这正是「编辑器里好好的、跑起来背景全黑」
		 * 的头号答案 —— 环境整层挂在一个游戏里不加载的子关卡上。
		 * 不带上它，调用方拿到的就是一份「跑了 5 秒，没报错，画面是黑的」。
		 */
		TSharedPtr<FJsonObject> LevelsAtEnd;

		/**
		 * 定时注入的控制台命令。
		 *
		 * 试玩没法模拟真实按键，但绝大多数「触发一下试试」的诉求可以靠控制台
		 * 命令达成：`ce MyEvent` 触发玩家控制器上的自定义事件、
		 * 各种 exec 函数、CheatManager 命令。
		 *
		 * 按 At 升序排好，ticker 里顺序发。
		 */
		struct FScheduledCommand
		{
			double At = 0.0;
			FString Command;
			bool bFired = false;
			bool bAccepted = false;
		};
		TArray<FScheduledCommand> Commands;

		/**
		 * 按时间抓的帧。空数组 = 没开多帧采样，只有收尾那一张。
		 *
		 * 存在的理由：一张收尾截图**物理上不包含**「这段时间里发生了什么」。
		 * 角色有没有动、门什么时候开的、第几秒开始掉帧变黑 —— 这些信息在
		 * 单帧里根本不存在，而调用方拿到一张构图正常的图之后，会照着它
		 * 下一个关于「运行效果」的结论。
		 */
		struct FFrameShot
		{
			double At = 0.0;
			FString Path;
			FString Error;
			bool bCaptured = false;
			/**
			 * 抓这一帧花了多少毫秒 —— 这段时间游戏是**卡住的**。
			 *
			 * 必须量出来回上去：抓帧会打扰被测的那次运行，而「打扰了多少」
			 * 我们没在真机上测过。拍一个数写进文档是编的，量一个数回给调用方
			 * 才是真的 —— 它自己就能判断这次采样密度是不是已经把结论弄脏了。
			 */
			double CaptureMs = 0.0;
		};
		TArray<FFrameShot> Frames;

		/**
		 * 抓帧走哪条路。
		 *
		 * scene = SceneCapture 渲场景，跟窗口状态无关，但**不画 UMG 界面层**；
		 * window = 抓编辑器窗口那块像素，HUD、血条、Print String 的屏幕字都在，
		 * 代价是画面里连编辑器的面板工具栏一起进来，游戏画面只占其中一块。
		 */
		FString FrameMode = TEXT("scene");

		/** 这次会话的文件名前缀，保证同一秒里的几帧不互相覆盖 */
		FString FrameStamp;

		/** 「盒子正在试玩，请勿操作」横幅，挂在游戏视口上。视口先没了就不用摘 */
		TSharedPtr<SWidget> Banner;
		TWeakObjectPtr<UGameViewportClient> BannerViewport;

		TUniquePtr<FUAL_PieLogCapture> Capture;
		FDelegateHandle StartedHandle;
		FDelegateHandle EndedHandle;
		FTSTicker::FDelegateHandle TickerHandle;

		void Reset()
		{
			bActive = false;
			RequestId.Empty();
			StartRequestedAt = 0.0;
			PlayStartedAt = 0.0;
			bPlayStarted = false;
			bFinishing = false;
			EndedBy.Empty();
			bStopRequested = false;
			StopReason.Empty();
			FixedFps = 0.0f;
			bFixedStepApplied = false;
			bPrevUseFixedFrameRate = false;
			PrevFixedFrameRate = 30.0f;
			ActorsAtStart = 0;
			ScreenshotPath.Empty();
			ScreenshotError.Empty();
			LevelsAtEnd.Reset();
			Commands.Empty();
			Frames.Empty();
			FrameMode = TEXT("scene");
			FrameStamp.Empty();
			Banner.Reset();
			BannerViewport.Reset();
			Capture.Reset();
		}
	};

	FUAL_PieSession GPieSession;

	int32 UAL_CountActors(UWorld* World)
	{
		if (!World)
		{
			return 0;
		}
		int32 Count = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			++Count;
		}
		return Count;
	}

	/**
	 * 抓下这一刻的画面。成功填 Path，失败填 Error —— **两个都要回给调用方**。
	 *
	 * 失败的那一格必须说出来。悄悄跳过的话，调用方收到的是一条看起来连续的
	 * 时间轴，中间少了的那一秒会被当成「什么都没发生」，而它恰恰可能是
	 * 出问题的那一秒。
	 */
	void UAL_CapturePieFrame(FUAL_PieSession::FFrameShot& Shot, int32 Index)
	{
		const FString Stamp = GPieSession.FrameStamp;
		const double StartedAt = FPlatformTime::Seconds();
		// 无论成败都要记时间：失败那次照样卡了游戏
		ON_SCOPE_EXIT
		{
			Shot.CaptureMs = FMath::RoundToDouble((FPlatformTime::Seconds() - StartedAt) * 10000.0) / 10.0;
		};

		if (GPieSession.FrameMode == TEXT("window"))
		{
			// 试玩采样不抢焦点：游戏跑着的时候每隔几百毫秒把窗口弹到前面，
			// 既打断玩家也打断用户手上正在做的事
			const FString Name = FString::Printf(TEXT("UAL_PieFrame_%s_%02d.png"), *Stamp, Index);
			FIntPoint Size(0, 0);
			int32 Code = 500;
			FString Path;
			FString Error;
			FString WindowTitle;
			FString WindowSource;
			// 点名 pie：拍游戏视口所在的窗口，不跟着「用户最后用过的窗口」走
			if (UAL_CaptureActiveWindowToFile(Name, TEXT("pie"), /*bBringToFront=*/false, Path, Size, WindowTitle, WindowSource, Error, Code))
			{
				Shot.Path = Path;
			}
			else
			{
				Shot.Error = Error;
			}
			return;
		}

		UWorld* PlayWorld = UAL_GetPlayWorld();
		if (!PlayWorld)
		{
			// 世界没了还照常报个路径出去，就等于给调用方一张别的世界的图
			Shot.Error = TEXT("PIE world is gone");
			return;
		}

		// 机位取玩家视角 —— 要的是「玩家这一刻看到了什么」，
		// 而玩家每一帧都在动，这个必须每帧重新取
		TOptional<FVector> ViewLocation;
		TOptional<FRotator> ViewRotation;
		TOptional<float> ViewFOV;
		UAL_TryGetPlayerViewPoint(PlayWorld, ViewLocation, ViewRotation, ViewFOV);

		FString Path;
		FString Error;
		if (CaptureSceneToFile(
			UAL_PieFrameWidth, UAL_PieFrameHeight, Path, Error, PlayWorld,
			ViewLocation, ViewRotation, ViewFOV,
			FString::Printf(TEXT("pie%s_%02d"), *Stamp, Index)))
		{
			Shot.Path = Path;
		}
		else
		{
			Shot.Error = Error;
		}
	}

	TArray<TSharedPtr<FJsonValue>> UAL_ToJsonStrings(const TArray<FString>& Lines)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& Line : Lines)
		{
			Out.Add(MakeShared<FJsonValueString>(Line));
		}
		return Out;
	}

	/**
	 * 游戏视口顶上挂一条「盒子正在试玩，请勿操作」。
	 *
	 * 试玩机器人一跑就是几十秒，用户看见游戏窗口自己在动，顺手一碰鼠标键盘，
	 * 机器人的判断就脏了。
	 *
	 * 不进截图：scene 模式的帧和收尾截图走 SceneCapture，本来就不画界面层；
	 * window 模式抓的是整块窗口像素，横幅会一起进去被当成游戏 UI，所以那种模式不挂。
	 * 命中测试关掉，不挡任何点击。
	 */
	void UAL_ShowPieBanner()
	{
		if (GPieSession.FrameMode == TEXT("window"))
		{
			return;
		}
		UWorld* PlayWorld = UAL_GetPlayWorld();
		UGameViewportClient* Viewport = PlayWorld ? PlayWorld->GetGameViewport() : nullptr;
		if (!Viewport)
		{
			return;
		}

		const bool bChinese = FInternationalization::Get().GetCurrentCulture()->GetName().StartsWith(TEXT("zh"));
		const FText Message = bChinese
			? FText::FromString(TEXT("盒子正在试玩测试，请勿操作 · 按 Esc 中止"))
			: FText::FromString(TEXT("UEBox is playtesting - please don't touch · Esc to abort"));

		static const FSlateColorBrush Background(FLinearColor(0.f, 0.f, 0.f, 0.6f));
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Bold", 14);

		TSharedRef<SWidget> Banner =
			SNew(SBox)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Top)
			.Padding(FMargin(0.f, 16.f, 0.f, 0.f))
			.Visibility(EVisibility::HitTestInvisible)
			[
				SNew(SBorder)
				.BorderImage(&Background)
				.Padding(FMargin(16.f, 8.f))
				[
					SNew(STextBlock)
					.Text(Message)
					.Font(Font)
					.ColorAndOpacity(FLinearColor(1.f, 0.78f, 0.25f))
				]
			];

		// ZOrder 取大值，压在游戏自己的 HUD 上面
		Viewport->AddViewportWidgetContent(Banner, 1000);
		GPieSession.Banner = Banner;
		GPieSession.BannerViewport = Viewport;
	}

	void UAL_HidePieBanner()
	{
		if (GPieSession.Banner.IsValid())
		{
			if (UGameViewportClient* Viewport = GPieSession.BannerViewport.Get())
			{
				Viewport->RemoveViewportWidgetContent(GPieSession.Banner.ToSharedRef());
			}
		}
		GPieSession.Banner.Reset();
		GPieSession.BannerViewport.Reset();
	}

	/** 摘掉这次会话挂的所有钩子。每条退出路径都必须走它，否则下一次会串味 */
	void UAL_TeardownPieSession()
	{
		UAL_HidePieBanner();
		if (GPieSession.Capture && GLog)
		{
			GLog->RemoveOutputDevice(GPieSession.Capture.Get());
		}
		if (GPieSession.StartedHandle.IsValid())
		{
			FEditorDelegates::PostPIEStarted.Remove(GPieSession.StartedHandle);
			GPieSession.StartedHandle.Reset();
		}
		if (GPieSession.EndedHandle.IsValid())
		{
			FEditorDelegates::EndPIE.Remove(GPieSession.EndedHandle);
			GPieSession.EndedHandle.Reset();
		}
		if (GPieSession.TickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GPieSession.TickerHandle);
			GPieSession.TickerHandle.Reset();
		}
		// 固定步长是整个编辑器的状态，不恢复的话用户退出试玩之后编辑器一直锁帧
		if (GPieSession.bFixedStepApplied && GEngine)
		{
			GEngine->bUseFixedFrameRate = GPieSession.bPrevUseFixedFrameRate;
			GEngine->FixedFrameRate = GPieSession.PrevFixedFrameRate;
			GPieSession.bFixedStepApplied = false;
		}
	}

	/**
	 * 收尾：汇总 → 发响应 → 清场。
	 *
	 * `ActorsAtEnd` 只在我们主动收尾时拿得到 —— 那时 PIE 世界还在。被用户
	 * 按 Esc 打断时世界已经在销毁，数不出来，这时候不报这几个字段，
	 * 而不是报一个 0 让调用方以为「Actor 全没了」。
	 */
	void UAL_FinishPieSession(const TOptional<int32>& ActorsAtEnd)
	{
		if (!GPieSession.bActive)
		{
			return;
		}

		const FString RequestId = GPieSession.RequestId;
		const double Elapsed = GPieSession.bPlayStarted
			? FPlatformTime::Seconds() - GPieSession.PlayStartedAt
			: 0.0;

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetBoolField(TEXT("ran"), GPieSession.bPlayStarted);
		Result->SetStringField(TEXT("ended_by"), GPieSession.EndedBy);
		Result->SetNumberField(TEXT("elapsed_seconds"), FMath::RoundToDouble(Elapsed * 10.0) / 10.0);
		Result->SetNumberField(TEXT("requested_seconds"), GPieSession.DurationSeconds);
		if (!GPieSession.StopReason.IsEmpty())
		{
			Result->SetStringField(TEXT("stop_reason"), GPieSession.StopReason);
		}
		if (GPieSession.bFixedStepApplied)
		{
			// 回报实际用的步长：不说的话调用方不知道这次的「帧」有多长
			Result->SetNumberField(TEXT("fixed_fps"), GPieSession.FixedFps);
		}

		if (GPieSession.Capture)
		{
			Result->SetArrayField(TEXT("print_strings"), UAL_ToJsonStrings(GPieSession.Capture->PrintStrings));
			Result->SetArrayField(TEXT("errors"), UAL_ToJsonStrings(GPieSession.Capture->Errors));
			Result->SetArrayField(TEXT("warnings"), UAL_ToJsonStrings(GPieSession.Capture->Warnings));
			Result->SetNumberField(TEXT("error_count"), GPieSession.Capture->Errors.Num());
			Result->SetNumberField(TEXT("warning_count"), GPieSession.Capture->Warnings.Num());
		}

		if (ActorsAtEnd.IsSet())
		{
			Result->SetNumberField(TEXT("actors_at_start"), GPieSession.ActorsAtStart);
			Result->SetNumberField(TEXT("actors_at_end"), ActorsAtEnd.GetValue());
			Result->SetNumberField(TEXT("actors_spawned"), ActorsAtEnd.GetValue() - GPieSession.ActorsAtStart);
		}

		if (GPieSession.Commands.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Fired;
			for (const FUAL_PieSession::FScheduledCommand& Cmd : GPieSession.Commands)
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetNumberField(TEXT("at"), Cmd.At);
				Entry->SetStringField(TEXT("command"), Cmd.Command);
				Entry->SetBoolField(TEXT("fired"), Cmd.bFired);
				// 没发出去和「发了但引擎不认这条命令」是两回事，
				// 调用方要能分清是时序没到还是命令名写错了
				Entry->SetBoolField(TEXT("accepted"), Cmd.bAccepted);
				Fired.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Result->SetArrayField(TEXT("events"), Fired);
		}

		if (GPieSession.LevelsAtEnd.IsValid())
		{
			Result->SetObjectField(TEXT("levels"), GPieSession.LevelsAtEnd);
		}

		/**
		 * 按时间抓的帧。**没抓成的那些也要回**，理由见 UAL_CapturePieFrame。
		 *
		 * 被用户中途打断时后面几帧根本没机会拍（bCaptured 为 false），
		 * 这种也照样回一条 —— 「计划抓 9 帧、实际只有 4 帧」这件事
		 * 只有这里说得出来。
		 */
		if (GPieSession.Frames.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> FrameArray;
			for (const FUAL_PieSession::FFrameShot& Shot : GPieSession.Frames)
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetNumberField(TEXT("at"), FMath::RoundToDouble(Shot.At * 100.0) / 100.0);
				if (!Shot.Path.IsEmpty())
				{
					Entry->SetStringField(TEXT("path"), Shot.Path);
				}
				if (Shot.bCaptured)
				{
					// 这一帧让游戏卡了多久。采样密到一定程度，测出来的就是采样本身
					Entry->SetNumberField(TEXT("capture_ms"), Shot.CaptureMs);
				}
				if (!Shot.Error.IsEmpty())
				{
					Entry->SetStringField(TEXT("error"), Shot.Error);
				}
				else if (!Shot.bCaptured)
				{
					Entry->SetStringField(TEXT("error"), TEXT("The run ended before this frame was due"));
				}
				FrameArray.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Result->SetArrayField(TEXT("frames"), FrameArray);
			Result->SetStringField(TEXT("frame_mode"), GPieSession.FrameMode);
		}

		if (!GPieSession.ScreenshotPath.IsEmpty())
		{
			Result->SetStringField(TEXT("screenshot_path"), GPieSession.ScreenshotPath);
		}
		else if (!GPieSession.ScreenshotError.IsEmpty())
		{
			// 截图失败不影响这次试跑的结论，但要说一声 —— 不说的话调用方
			// 会以为自己忘了开截图
			Result->SetStringField(TEXT("screenshot_error"), GPieSession.ScreenshotError);
		}

		UAL_TeardownPieSession();
		GPieSession.Reset();

		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
	}
}

void FUAL_EditorCommands::Handle_RunPlaytest(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!GEditor)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("GEditor is not available"));
		return;
	}

	// 同一时间只跑一个会话。第二个请求悄悄接受会把两次的日志和计时搅在一起，
	// 报出来的东西谁都对不上。
	if (GPieSession.bActive)
	{
		UAL_CommandUtils::SendError(
			RequestId, 409,
			TEXT("A playtest is already running. Wait for it to finish before starting another."));
		return;
	}

	// 用户自己按了 Play：接管会把他的会话掐掉
	if (GEditor->PlayWorld != nullptr || GEditor->bIsSimulatingInEditor)
	{
		UAL_CommandUtils::SendError(
			RequestId, 409,
			TEXT("The editor is already in Play/Simulate mode. Stop it before running a playtest."));
		return;
	}

	double Duration = 5.0;
	Payload->TryGetNumberField(TEXT("duration_seconds"), Duration);
	// 硬顶 60 秒：蓝图里一个死循环就能把编辑器挂死，没有上限的话
	// 「跑一下试试」可能再也回不来
	Duration = FMath::Clamp(Duration, 0.5, 60.0);

	bool bStopOnError = false;
	Payload->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);

	bool bWantScreenshot = true;
	Payload->TryGetBoolField(TEXT("screenshot"), bWantScreenshot);

	// 固定步长，见 FUAL_PieSession::FixedFps。不传 = 不锁，保持老行为
	double FixedFps = 0.0;
	Payload->TryGetNumberField(TEXT("fixed_fps"), FixedFps);
	FixedFps = FixedFps > 0.0 ? FMath::Clamp(FixedFps, 15.0, 120.0) : 0.0;

	/**
	 * 多帧采样。
	 *
	 * `frames` 给个数，时间由这里均分；`frame_times` 给确切的秒数，
	 * 用来把帧钉在**事件前后**（第 2 秒发了 ce OpenDoor，就在 1.9 / 2.1 / 2.5
	 * 各抓一张），比平均撒更容易看出那件事到底发生没有。
	 */
	FString FrameMode = TEXT("scene");
	Payload->TryGetStringField(TEXT("frame_mode"), FrameMode);
	FrameMode = FrameMode.ToLower();
	if (FrameMode != TEXT("window"))
	{
		FrameMode = TEXT("scene");
	}

	TArray<double> FrameTimes;
	const TArray<TSharedPtr<FJsonValue>>* TimesArray = nullptr;
	if (Payload->TryGetArrayField(TEXT("frame_times"), TimesArray) && TimesArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *TimesArray)
		{
			double At = 0.0;
			if (Value.IsValid() && Value->TryGetNumber(At))
			{
				// 超出时长的帧永远等不到。夹回**刚好在结尾**而不是丢掉：
				// 调用方多半是想要「最后那一刻」，丢掉的话它得到的是一张
				// 少了一格、而且没人告诉它为什么少的拼图
				FrameTimes.Add(FMath::Clamp(At, 0.0, Duration));
			}
		}
		FrameTimes.Sort();
		if (FrameTimes.Num() > UAL_PieMaxFrames)
		{
			FrameTimes.SetNum(UAL_PieMaxFrames);
		}
	}

	int32 FrameCount = 0;
	Payload->TryGetNumberField(TEXT("frames"), FrameCount);
	FrameCount = FMath::Clamp(FrameCount, 0, UAL_PieMaxFrames);

	TArray<FUAL_PieSession::FFrameShot> Frames;
	if (FrameTimes.Num() > 0)
	{
		for (double At : FrameTimes)
		{
			FUAL_PieSession::FFrameShot Shot;
			Shot.At = At;
			Frames.Add(Shot);
		}
	}
	else if (FrameCount > 0)
	{
		// 均分到整段时间上，第一帧在 0（刚 BeginPlay 完的样子），
		// 最后一帧压在结尾那一刻 —— 收尾的那次 tick 会先抓帧再停，
		// 所以它拿得到，不会变成一个永远等不到的时刻
		for (int32 Index = 0; Index < FrameCount; ++Index)
		{
			FUAL_PieSession::FFrameShot Shot;
			Shot.At = FrameCount == 1 ? Duration : Duration * Index / (FrameCount - 1);
			Frames.Add(Shot);
		}
	}

	// 定时注入的控制台命令
	TArray<FUAL_PieSession::FScheduledCommand> Commands;
	const TArray<TSharedPtr<FJsonValue>>* EventsArray = nullptr;
	if (Payload->TryGetArrayField(TEXT("events"), EventsArray) && EventsArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *EventsArray)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj || !(*Obj).IsValid())
			{
				continue;
			}
			FUAL_PieSession::FScheduledCommand Cmd;
			(*Obj)->TryGetStringField(TEXT("command"), Cmd.Command);
			(*Obj)->TryGetNumberField(TEXT("at"), Cmd.At);
			if (Cmd.Command.IsEmpty())
			{
				continue;
			}
			Cmd.At = FMath::Max(0.0, Cmd.At);
			Commands.Add(Cmd);
		}
		// 按时间排好，ticker 里就能顺序发，不用每帧全表扫
		Commands.Sort([](const FUAL_PieSession::FScheduledCommand& A,
		                 const FUAL_PieSession::FScheduledCommand& B)
		{
			return A.At < B.At;
		});
	}

	// 排在时长之后的命令永远发不出去。与其让它静默不执行，不如当场拒绝 ——
	// 调用方多半是把 at 和 duration 写反了，静默丢掉会让它对着一份
	// 「命令发了但什么都没发生」的报告查半天。
	for (const FUAL_PieSession::FScheduledCommand& Cmd : Commands)
	{
		if (Cmd.At >= Duration)
		{
			UAL_CommandUtils::SendError(
				RequestId, 400,
				FString::Printf(
					TEXT("events[at=%.1f] \"%s\" is scheduled at or after the end of the run (duration_seconds=%.1f), so it would never fire. Raise duration_seconds or lower \"at\"."),
					Cmd.At, *Cmd.Command, Duration));
			return;
		}
	}

	GPieSession.Reset();
	GPieSession.bActive = true;
	GPieSession.RequestId = RequestId;
	GPieSession.DurationSeconds = Duration;
	GPieSession.bStopOnError = bStopOnError;
	GPieSession.bWantScreenshot = bWantScreenshot;
	GPieSession.Commands = MoveTemp(Commands);
	GPieSession.Frames = MoveTemp(Frames);
	GPieSession.FrameMode = FrameMode;
	// 带上毫秒。文件名本身只精确到秒，同一秒里抓的几帧会互相覆盖 ——
	// 覆盖之后一切照常成功，只是拼出来九格一模一样
	GPieSession.FrameStamp = FDateTime::Now().ToString(TEXT("%H%M%S%s"));
	GPieSession.StartRequestedAt = FPlatformTime::Seconds();

	// 日志捕获必须在 PIE 起来**之前**挂上：BeginPlay 里的 PrintString 和
	// 启动期的错误是最有价值的部分，晚挂一步就全漏了。
	GPieSession.Capture = MakeUnique<FUAL_PieLogCapture>();
	if (GLog)
	{
		GLog->AddOutputDevice(GPieSession.Capture.Get());
	}

	if (FixedFps > 0.0 && GEngine)
	{
		GPieSession.bPrevUseFixedFrameRate = GEngine->bUseFixedFrameRate;
		GPieSession.PrevFixedFrameRate = GEngine->FixedFrameRate;
		GEngine->bUseFixedFrameRate = true;
		GEngine->FixedFrameRate = static_cast<float>(FixedFps);
		GPieSession.FixedFps = static_cast<float>(FixedFps);
		GPieSession.bFixedStepApplied = true;
	}

	GPieSession.StartedHandle = FEditorDelegates::PostPIEStarted.AddLambda([](bool /*bIsSimulating*/)
	{
		if (!GPieSession.bActive)
		{
			return;
		}
		GPieSession.bPlayStarted = true;
		GPieSession.PlayStartedAt = FPlatformTime::Seconds();
		if (GPieSession.Capture)
		{
			GPieSession.Capture->PlayStartedAt = GPieSession.PlayStartedAt;
		}
		GPieSession.ActorsAtStart = UAL_CountActors(UAL_GetPlayWorld());
		UAL_ShowPieBanner();
	});

	/**
	 * 用户随时可能按 Esc，游戏自己也可能 QuitGame。
	 *
	 * 不挂这个钩子的话，那些情况下 PIE 已经结束了而 ticker 还在等时间到，
	 * 最后报一份「跑满了 5 秒」的假报告。
	 */
	GPieSession.EndedHandle = FEditorDelegates::EndPIE.AddLambda([](bool /*bIsSimulating*/)
	{
		// bFinishing 为真说明是我们自己叫停的，收尾已经在走了
		if (!GPieSession.bActive || GPieSession.bFinishing)
		{
			return;
		}
		GPieSession.bFinishing = true;
		GPieSession.EndedBy = TEXT("stopped_externally");
		// 世界正在销毁，Actor 数不可信 —— 不报比报个 0 强
		UAL_FinishPieSession(TOptional<int32>());
	});

	FRequestPlaySessionParams Params;
	Params.WorldType = EPlaySessionWorldType::PlayInEditor;
	Params.SessionDestination = EPlaySessionDestinationType::InProcess;
	GEditor->RequestPlaySession(Params);
	// RequestPlaySession 只是排队，真正启动在下一帧。这里踢一下让它尽快开始。
	GEditor->StartQueuedPlaySessionRequest();

	GPieSession.TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float /*Delta*/) -> bool
		{
			if (!GPieSession.bActive || GPieSession.bFinishing)
			{
				return false;
			}

			const double Now = FPlatformTime::Seconds();

			// 还没起来：给 30 秒启动窗口。大关卡加载确实要这么久，
			// 但一直起不来就得报错，不能无限等。
			if (!GPieSession.bPlayStarted)
			{
				if (Now - GPieSession.StartRequestedAt > 30.0)
				{
					GPieSession.bFinishing = true;
					GPieSession.EndedBy = TEXT("failed_to_start");
					UAL_FinishPieSession(TOptional<int32>());
					return false;
				}
				return true;
			}

			const double Elapsed = Now - GPieSession.PlayStartedAt;

			/**
			 * 到点就把命令发进 PIE 世界。
			 *
			 * 用 `GEngine->Exec` 而不是 PlayerController：前者对世界本身生效，
			 * 玩家控制器还没生成好的那一瞬间也不会漏掉。日志由本次会话的
			 * 捕获器照常收着，所以命令引发的 PrintString 和错误都会进报告。
			 */
			UWorld* CmdWorld = nullptr;
			for (FUAL_PieSession::FScheduledCommand& Cmd : GPieSession.Commands)
			{
				if (Cmd.bFired || Cmd.At > Elapsed)
				{
					// 已排序，后面的只会更晚
					if (!Cmd.bFired) break;
					continue;
				}
				if (!CmdWorld)
				{
					CmdWorld = UAL_GetPlayWorld();
				}
				Cmd.bFired = true;
				if (CmdWorld && GEngine)
				{
					Cmd.bAccepted = GEngine->Exec(CmdWorld, *Cmd.Command);
				}
			}

			/**
			 * 到点就抓一帧。**必须排在「时间到了没有」的判断之前** ——
			 * 最后一帧的时刻正好等于 duration，跟收尾落在同一次 tick 上，
			 * 排在后面它就永远抓不到。
			 */
			for (int32 Index = 0; Index < GPieSession.Frames.Num(); ++Index)
			{
				FUAL_PieSession::FFrameShot& Shot = GPieSession.Frames[Index];
				if (Shot.bCaptured)
				{
					continue;
				}
				if (Shot.At > Elapsed)
				{
					// 已按时间排序，后面的只会更晚
					break;
				}
				Shot.bCaptured = true;
				UAL_CapturePieFrame(Shot, Index);
			}

			const bool bTimeUp = Elapsed >= GPieSession.DurationSeconds;
			const bool bErrorStop = GPieSession.bStopOnError
				&& GPieSession.Capture
				&& GPieSession.Capture->HasErrors();

			const bool bStopRequested = GPieSession.bStopRequested;

			if (!bTimeUp && !bErrorStop && !bStopRequested)
			{
				return true;
			}

			GPieSession.bFinishing = true;
			GPieSession.EndedBy = bErrorStop ? TEXT("error")
				: bStopRequested ? TEXT("requested")
				: TEXT("duration");

			// 截图和数 Actor 都必须赶在 RequestEndPlayMap **之前** ——
			// PIE 世界一销毁这两样就都没了
			UWorld* PlayWorld = UAL_GetPlayWorld();
			const int32 ActorsAtEnd = UAL_CountActors(PlayWorld);

			// 关卡组成同理，也只有这一刻取得到
			if (PlayWorld)
			{
				GPieSession.LevelsAtEnd = FUAL_LevelCommands::BuildLevelComposition(PlayWorld);
			}

			if (GPieSession.bWantScreenshot && PlayWorld)
			{
				// 相机取玩家视角而不是编辑器视口那台 —— 要的是「玩家看到了什么」
				TOptional<FVector> ViewLocation;
				TOptional<FRotator> ViewRotation;
				TOptional<float> ViewFOV;
				UAL_TryGetPlayerViewPoint(PlayWorld, ViewLocation, ViewRotation, ViewFOV);

				FString ShotPath;
				FString ShotError;
				if (CaptureSceneToFile(1280, 720, ShotPath, ShotError, PlayWorld, ViewLocation, ViewRotation, ViewFOV))
				{
					GPieSession.ScreenshotPath = ShotPath;
				}
				else
				{
					GPieSession.ScreenshotError = ShotError;
				}
			}

			GEditor->RequestEndPlayMap();

			// 响应现在就发。PIE 要到下一帧才真停，但报告需要的东西
			// （日志、截图、Actor 数）此刻已经全部拿到了，没必要多等一帧。
			// teardown 会把 EndPIE 钩子摘掉，所以它不会再重复触发一次。
			UAL_FinishPieSession(ActorsAtEnd);
			return false;
		}),
		0.0f);
}

void FUAL_EditorCommands::Handle_StopPlaytest(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!GPieSession.bActive)
	{
		// 用户自己按的 Play 不归我们停 —— 和 pie.run 不接管用户会话是同一条线
		UAL_CommandUtils::SendError(
			RequestId, 409,
			TEXT("No playtest started by pie.run is running. A Play session the user started is not ours to stop."));
		return;
	}

	if (!GPieSession.bFinishing)
	{
		GPieSession.bStopRequested = true;
		FString Reason;
		if (Payload.IsValid() && Payload->TryGetStringField(TEXT("reason"), Reason))
		{
			GPieSession.StopReason = Reason.Left(200);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetBoolField(TEXT("stopping"), true);
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

bool FUAL_EditorCommands::ReadPieSessionLogs(int32 SinceSeq, int32 MaxLines, TArray<FPieLogLine>& OutLines, int32& OutNextSeq, bool& bOutDropped)
{
	OutLines.Reset();
	bOutDropped = false;
	OutNextSeq = FMath::Max(0, SinceSeq);
	if (!GPieSession.bActive || !GPieSession.Capture)
	{
		return false;
	}

	const TArray<FPieLogLine>& Timeline = GPieSession.Capture->Timeline;
	OutNextSeq = GPieSession.Capture->NextSeq;
	if (Timeline.Num() > 0 && Timeline[0].Seq > SinceSeq)
	{
		bOutDropped = true;
	}

	for (const FPieLogLine& Line : Timeline)
	{
		if (Line.Seq < SinceSeq)
		{
			continue;
		}
		if (OutLines.Num() >= MaxLines)
		{
			// 一次读不完就把游标停在没读的那条上，而不是跳过去 —— 跳过的那一截
			// 下次谁都读不到了，而调用方以为自己看全了
			OutNextSeq = Line.Seq;
			break;
		}
		OutLines.Add(Line);
	}
	return true;
}

bool FUAL_EditorCommands::IsPieSessionActive(double& OutElapsed, bool& bOutPlayStarted)
{
	OutElapsed = 0.0;
	bOutPlayStarted = GPieSession.bActive && GPieSession.bPlayStarted;
	if (bOutPlayStarted)
	{
		OutElapsed = FPlatformTime::Seconds() - GPieSession.PlayStartedAt;
	}
	return GPieSession.bActive && !GPieSession.bFinishing;
}

// ============================================================================
// viewport.focus / viewport.get_camera
// ============================================================================

#if WITH_EDITOR

/**
 * 拿当前该操作的关卡视口。
 *
 * `CAMERA ALIGN ACTIVEVIEWPORTONLY` 内部动的就是 `GCurrentLevelEditingViewportClient`
 * （见引擎 MoveViewportCamerasToBox），所以回读也必须读同一个，否则报的是
 * 另一个没动过的视口，看着像「聚焦失败」。
 *
 * 那个全局量为空时（比如从没点过任何视口）退回第一个可用的，但要如实告诉
 * 调用方「这不是活动视口」—— 因为这时 exec 很可能什么也没动。
 */
static FLevelEditorViewportClient* UAL_GetFocusViewport(bool& bOutIsActiveClient)
{
	bOutIsActiveClient = false;
	if (!GEditor)
	{
		return nullptr;
	}

	if (GCurrentLevelEditingViewportClient)
	{
		bOutIsActiveClient = true;
		return GCurrentLevelEditingViewportClient;
	}

	for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
	{
		if (Client)
		{
			return Client;
		}
	}
	return nullptr;
}

/** 把视口相机序列化成 JSON */
static TSharedPtr<FJsonObject> UAL_MakeCameraJson(FLevelEditorViewportClient* Client)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	if (!Client)
	{
		return Json;
	}
	Json->SetObjectField(TEXT("location"), UAL_CommandUtils::MakeVectorJson(Client->GetViewLocation()));
	Json->SetObjectField(TEXT("rotation"), UAL_CommandUtils::MakeRotatorJson(Client->GetViewRotation()));
	Json->SetNumberField(TEXT("fov"), Client->ViewFOV);
	return Json;
}

/**
 * 看整个 Actor，还是只看它的某一截。
 *
 * 起因是实测：聚焦整棵树一发命中，但「我要看树干」就完全对不上焦 ——
 * 包围盒聚焦只会把整棵树框进画面，树干在画面里只有几个像素。调用方只能
 * 退回去手写相机坐标，而那条路全是坑（见 UE Python 的 Rotator 位置参数顺序）。
 *
 * 按包围盒高度三等分：树干 = Bottom，树冠 = Top，箱子中段 = Middle。
 * 这个划法对「树干 / 屋顶 / 底座」这类说法覆盖得足够好，而且没有任何猜测成分。
 */
enum class EUALFocusRegion : uint8
{
	Whole,
	Top,
	Middle,
	Bottom
};

/**
 * 相机往哪个方向看。
 *
 * **故意只有三个**。曾经想加 front/back/left/right/three_quarter，砍掉了：
 * 一个道具的「正面」是哪一面我们并不知道（+X 只是引擎约定，美术摆模型时
 * 根本不遵守），每加一个方向就是加一个有一半概率是错的猜测。
 * 这三个的语义不依赖任何猜测：
 *   - Current    保持视口当前朝向（等同按 F，只调远近）
 *   - Horizontal 水平平视，俯仰角归零 —— 看树干、看门、看柱子就是这个
 *   - Top        正上方俯视 —— 看布局、看占地
 */
enum class EUALFocusDir : uint8
{
	Current,
	Horizontal,
	Top
};

/** viewport.focus 的等待上下文：走引擎那条路时镜头是飞过去的，得等它落地才能回响应 */
struct FUALFocusPending
{
	FString RequestId;
	FLevelEditorViewportClient* Client = nullptr;
	FVector StartLocation = FVector::ZeroVector;
	FBox TargetBox = FBox(ForceInit);
	/** 实际瞄准的那个点。整体聚焦时就是包围盒中心，看局部时是那一截的中心 */
	FVector AimPoint = FVector::ZeroVector;
	TArray<TSharedPtr<FJsonValue>> Focused;
	TArray<TSharedPtr<FJsonValue>> HiddenInEditor;
	TArray<TSharedPtr<FJsonValue>> SkippedHiddenLevels;
	/** names / paths 里一个 Actor 都没对上的那些字符串。少认了谁必须说出来 */
	TArray<FString> UnmatchedTargets;
	/** 目标 Actor，用于判断挡住视线的是别人还是它自己 */
	TArray<TWeakObjectPtr<AActor>> TargetActors;
	bool bAllViewports = false;
	bool bIsActiveClient = true;
	EUALFocusRegion Region = EUALFocusRegion::Whole;
};

/** 取包围盒的某一截。按高度（Z）三等分 */
static FBox UAL_RegionBox(const FBox& Box, EUALFocusRegion Region)
{
	if (!Box.IsValid || Region == EUALFocusRegion::Whole)
	{
		return Box;
	}

	const double Height = Box.Max.Z - Box.Min.Z;
	const double Third = Height / 3.0;
	FBox Slice = Box;
	switch (Region)
	{
	case EUALFocusRegion::Bottom:
		Slice.Max.Z = Box.Min.Z + Third;
		break;
	case EUALFocusRegion::Middle:
		Slice.Min.Z = Box.Min.Z + Third;
		Slice.Max.Z = Box.Min.Z + Third * 2.0;
		break;
	case EUALFocusRegion::Top:
		Slice.Min.Z = Box.Min.Z + Third * 2.0;
		break;
	default:
		break;
	}
	return Slice;
}

/**
 * 站多远能把这一截拍全。
 *
 * 就是引擎 FocusViewportOnBox 里那条式子：半径 / tan(FOV/2)。
 *
 * 半径的取法对「看局部」做了收窄：**不用包围球半径**，而用
 * `max(竖直半高, 两个水平半径里较小的那个)`。理由是看局部时基本都是侧着看，
 * 迎面看到的横向尺寸是较窄的那条边 —— 拿整棵树的树冠宽度去算距离，
 * 会把相机退到几十米外，树干还是只有几个像素，等于没聚焦。
 */
static double UAL_FramingDistance(const FBox& Box, EUALFocusRegion Region, double FovDegrees, double AspectRatio)
{
	const FVector Extent = Box.GetExtent();
	double Radius;
	if (Region == EUALFocusRegion::Whole)
	{
		Radius = Extent.Size();
	}
	else
	{
		Radius = FMath::Max(Extent.Z, FMath::Min(Extent.X, Extent.Y));
	}

	// 引擎那边的最小值是 10，沿用 —— 否则零尺寸目标（空 Actor、点光源）
	// 会算出距离 0，相机直接怼进物体里
	Radius = FMath::Max(Radius, 10.0);

	// 宽屏时竖直方向看得更少，按引擎的做法用宽高比补偿
	if (AspectRatio > 1.0)
	{
		Radius *= AspectRatio;
	}

	const double HalfFov = FMath::DegreesToRadians(FMath::Clamp(FovDegrees, 5.0, 170.0) / 2.0);
	// 1.15 的留边：正好贴边的构图看着很憋屈，而且目标稍微超出包围盒就被裁掉
	return (Radius / FMath::Tan(HalfFov)) * 1.15;
}

/**
 * 从相机到瞄准点之间有没有东西挡着。
 *
 * **不排除目标自己** —— 恰恰相反，「树冠把树干裹住了」正是最需要报出来的情况，
 * 排除掉目标就永远报不出它。
 *
 * ⚠️ 这一条只在「挡路的东西有碰撞体」时有效。植被、装饰面片经常整个没有碰撞，
 * 射线直接穿过去，这时候会**假报「没挡住」**。所以返回值只在为真时可信：
 * 说挡住了就是真挡住了，说没挡住不等于看得见。
 */
static bool UAL_TraceOcclusion(UWorld* World, const FVector& From, const FVector& To, FString& OutBlockerLabel, bool& bOutIsSelf, const TArray<TWeakObjectPtr<AActor>>& Targets)
{
	OutBlockerLabel.Reset();
	bOutIsSelf = false;
	if (!World)
	{
		return false;
	}

	const double TotalDist = FVector::Dist(From, To);
	if (TotalDist < KINDA_SMALL_NUMBER)
	{
		return false;
	}

	FHitResult Hit;
	// 不用 SCENE_QUERY_STAT 宏：它在不同版本里定义在不同头文件，直接给 FName 更稳
	FCollisionQueryParams Params(FName(TEXT("UALFocusOcclusion")), /*bTraceComplex=*/false);
	if (!World->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Params))
	{
		return false;
	}

	// 打在瞄准点附近不算遮挡 —— 那就是打到目标本体表面了，本来就该打到。
	// 用总距离的 5% 做容差：目标表面到中心点总有一段距离
	if (Hit.Distance > TotalDist * 0.95)
	{
		return false;
	}

	AActor* Blocker = Hit.GetActor();
	if (!Blocker)
	{
		return false;
	}

	for (const TWeakObjectPtr<AActor>& Target : Targets)
	{
		if (Target.Get() == Blocker)
		{
			bOutIsSelf = true;
			break;
		}
	}
	OutBlockerLabel = UAL_CommandUtils::GetActorFriendlyName(Blocker);
	return true;
}

/**
 * 等相机落地的上限。
 *
 * 引擎的过渡曲线是几百毫秒量级，1.5 秒足够走完。到点还没停就如实报
 * `transition_settled: false` —— **不报失败**：5.8 上目标里有被 Deformer
 * 变形的骨骼网格时，聚焦要先等一次异步 GPU 回读，那属于「还没到」不是「没成功」。
 */
static constexpr double UAL_FOCUS_SETTLE_TIMEOUT = 1.5;

static void UAL_FinishFocus(TSharedPtr<FUALFocusPending> Pending, bool bSettled)
{
	if (!Pending.IsValid())
	{
		return;
	}

	FLevelEditorViewportClient* Client = Pending->Client;
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("focused"), Pending->Focused);
	Data->SetObjectField(TEXT("camera"), UAL_MakeCameraJson(Client));
	Data->SetBoolField(TEXT("transition_settled"), bSettled);
	Data->SetBoolField(TEXT("all_viewports"), Pending->bAllViewports);
	Data->SetBoolField(TEXT("is_active_viewport"), Pending->bIsActiveClient);

	if (Client)
	{
		const FVector Now = Client->GetViewLocation();
		// 相机确实挪了位置才算动过。用 1 厘米做阈值而不是精确比较：
		// 过渡曲线收尾有浮点残差，精确比较会把「已经到位」误判成「还在动」
		Data->SetBoolField(TEXT("moved"), FVector::Dist(Now, Pending->StartLocation) > 1.0);
		Data->SetBoolField(TEXT("is_perspective"), Client->IsPerspective());

		if (Pending->TargetBox.IsValid)
		{
			const FVector Center = Pending->TargetBox.GetCenter();
			const FVector Extent = Pending->TargetBox.GetExtent();

			// 把包围盒整个回传。
			//
			// 只回「相机在哪、离多远」是不够的 —— 调用方想知道「这棵树的树冠是不是
			// 又宽又低、树干能不能从下面露出来」时，还得再调 actor 查询和 mesh 描述
			// 两趟。尺寸信息在这里本来就是现成的，白送。
			TSharedPtr<FJsonObject> Bounds = MakeShared<FJsonObject>();
			Bounds->SetObjectField(TEXT("center"), UAL_CommandUtils::MakeVectorJson(Center));
			Bounds->SetObjectField(TEXT("extent"), UAL_CommandUtils::MakeVectorJson(Extent));
			Bounds->SetObjectField(TEXT("min"), UAL_CommandUtils::MakeVectorJson(Pending->TargetBox.Min));
			Bounds->SetObjectField(TEXT("max"), UAL_CommandUtils::MakeVectorJson(Pending->TargetBox.Max));
			Bounds->SetNumberField(TEXT("radius"), Extent.Size());
			Bounds->SetNumberField(TEXT("height"), Extent.Z * 2.0);
			Data->SetObjectField(TEXT("bounds"), Bounds);

			// 兼容早先的字段名，别让已经在用的调用点一夜之间读不到东西
			Data->SetObjectField(TEXT("target_center"), UAL_CommandUtils::MakeVectorJson(Center));
			Data->SetNumberField(TEXT("target_radius"), Extent.Size());
			// 相机离目标多远。包围盒炸掉时（Landscape、带巨大隐藏组件的蓝图）
			// 这个数会大得离谱 —— 报出来，调用方看到就知道这张图不用信
			Data->SetNumberField(TEXT("distance"), FVector::Dist(Now, Pending->AimPoint));
		}

		// 朝向自检：相机的正前方到底有没有对着瞄准点。
		//
		// 这条是**这次真机翻车的直接对策**。当时相机位置设对了、回读也一致，
		// 唯独旋转被塞错了槽位，结果镜头垂直朝下拍地面 —— 而所有中间步骤
		// 都「成功」了，只有人眼看图才发现。现在把这件事变成一个数字。
		const FVector ToAim = Pending->AimPoint - Now;
		if (ToAim.SizeSquared() > KINDA_SMALL_NUMBER)
		{
			const FVector Forward = Client->GetViewRotation().Vector();
			const double CosAngle = FVector::DotProduct(Forward, ToAim.GetSafeNormal());
			const double ErrorDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(CosAngle, -1.0, 1.0)));

			TSharedPtr<FJsonObject> Aim = MakeShared<FJsonObject>();
			Aim->SetObjectField(TEXT("point"), UAL_CommandUtils::MakeVectorJson(Pending->AimPoint));
			Aim->SetNumberField(TEXT("error_degrees"), ErrorDeg);
			// 半个 FOV 之内就算目标落在画面里。这不是「构图好看」的判据，
			// 是「镜头到底有没有朝着它」的判据
			Aim->SetBoolField(TEXT("on_target"), ErrorDeg <= FMath::Max(1.0, Client->ViewFOV * 0.5));
			Data->SetObjectField(TEXT("aim"), Aim);
		}

		// 视线上有没有东西挡着。为真时可信，为假时只说明「射线没打到有碰撞的东西」
		if (Pending->TargetBox.IsValid && Client->IsPerspective())
		{
			FString Blocker;
			bool bSelf = false;
			if (UAL_TraceOcclusion(GEditor ? GEditor->GetEditorWorldContext().World() : nullptr,
					Now, Pending->AimPoint, Blocker, bSelf, Pending->TargetActors))
			{
				Data->SetStringField(TEXT("occluded_by"), Blocker);
				Data->SetBoolField(TEXT("occluded_by_self"), bSelf);
			}
		}
	}
	else
	{
		Data->SetBoolField(TEXT("moved"), false);
	}

	if (Pending->HiddenInEditor.Num() > 0)
	{
		// 目标本身在编辑器里是隐藏的：镜头会正确对过去，但画面上什么都没有。
		// 不算错误，但必须说 —— 不说的话调用方会拿着一张空图去改别的东西
		Data->SetArrayField(TEXT("hidden_in_editor"), Pending->HiddenInEditor);
	}
	if (Pending->SkippedHiddenLevels.Num() > 0)
	{
		Data->SetArrayField(TEXT("skipped_hidden_levels"), Pending->SkippedHiddenLevels);
	}
	// 点名 7 个只对上 6 个：以前 focused 里就少一条，没有任何字段说少了谁。
	// 调用方看见「成功」，以为镜头框住的是全部 7 个
	UAL_CommandUtils::AddUnmatchedTargets(Data, Pending->UnmatchedTargets);

	const TCHAR* RegionName = TEXT("whole");
	switch (Pending->Region)
	{
	case EUALFocusRegion::Top:    RegionName = TEXT("top");    break;
	case EUALFocusRegion::Middle: RegionName = TEXT("middle"); break;
	case EUALFocusRegion::Bottom: RegionName = TEXT("bottom"); break;
	default: break;
	}
	Data->SetStringField(TEXT("region"), RegionName);

	UAL_CommandUtils::SendResponse(Pending->RequestId, 200, Data);
}

#endif // WITH_EDITOR

void FUAL_EditorCommands::Handle_FocusViewport(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	if (!GEditor)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("GEditor 不可用"));
		return;
	}

	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("拿不到世界（没有打开关卡？）"));
		return;
	}

	TSharedPtr<FJsonObject> Targets;
	if (!UAL_CommandUtils::TryGetObjectFieldFlexible(Payload, TEXT("targets"), Targets) || !Targets.IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400,
			TEXT("缺少 targets。例：{\"names\":[\"BP_Chair\"]} 或 {\"selection\":true}"));
		return;
	}

	TSet<AActor*> ActorSet;
	FString ResolveError;
	TArray<FString> Unmatched;
	if (!UAL_CommandUtils::ResolveTargetsToActors(Targets, World, ActorSet, ResolveError, &Unmatched))
	{
		UAL_CommandUtils::SendError(RequestId, 400, ResolveError);
		return;
	}
	if (ActorSet.Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 404,
			TEXT("targets 没匹配到任何 Actor。names 认的是大纲里显示的名字（ActorLabel）"));
		return;
	}

	// 关卡可见性预检。**必须在 exec 之前做** —— 引擎在 MoveViewportCamerasToActor
	// 里遇到隐藏关卡的 Actor 会弹一个模态框（FSuppressableWarningDialog::ShowModal），
	// 模态框卡住游戏线程直到有人点确定，而我们是同步回响应的：真踩上就是
	// 调用方干等到超时、用户莫名其妙看见个弹窗
	TArray<AActor*> Visible;
	TArray<TSharedPtr<FJsonValue>> SkippedHiddenLevels;
	TArray<TSharedPtr<FJsonValue>> HiddenInEditor;
	TArray<TSharedPtr<FJsonValue>> FocusedNames;

	for (AActor* Actor : ActorSet)
	{
		if (!Actor)
		{
			continue;
		}
		const FString Label = UAL_CommandUtils::GetActorFriendlyName(Actor);
		if (!FLevelUtils::IsLevelVisible(Actor->GetLevel()))
		{
			SkippedHiddenLevels.Add(MakeShared<FJsonValueString>(Label));
			continue;
		}
		if (Actor->IsHiddenEd())
		{
			HiddenInEditor.Add(MakeShared<FJsonValueString>(Label));
		}
		Visible.Add(Actor);
		FocusedNames.Add(MakeShared<FJsonValueString>(Label));
	}

	if (Visible.Num() == 0)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("hidden_levels"), SkippedHiddenLevels);
		UAL_CommandUtils::SendError(RequestId, 409,
			TEXT("目标都在隐藏的关卡里。先在关卡面板里把关卡显示出来再聚焦"), Details);
		return;
	}

	bool bIsActiveClient = false;
	FLevelEditorViewportClient* Client = UAL_GetFocusViewport(bIsActiveClient);
	if (!Client)
	{
		UAL_CommandUtils::SendError(RequestId, 409,
			TEXT("没有可用的关卡视口。请先打开一个关卡编辑器视口"));
		return;
	}

	// 选中目标，再发**不带参数**的 CAMERA ALIGN。
	//
	// 不用 `CAMERA ALIGN NAME=xxx`：那条路走 FindFirstObject，认的是内部对象名，
	// 而我们（和用户）手里的是大纲显示的 ActorLabel —— 在改过名的 Actor 上两者必然不同。
	//
	// 选中状态**故意不还原**：这跟人在编辑器里点一下再按 F 是同一个结果，
	// 用户看得见「AI 在看哪个东西」。响应里的 focused 会如实列出改成选中了谁
	GEditor->SelectNone(false, true, false);
	for (AActor* Actor : Visible)
	{
		GEditor->SelectActor(Actor, true, false);
	}
	GEditor->NoteSelectionChange();

	TSharedPtr<FUALFocusPending> Pending = MakeShared<FUALFocusPending>();
	Pending->RequestId = RequestId;
	Pending->Client = Client;
	Pending->StartLocation = Client->GetViewLocation();
	Pending->Focused = FocusedNames;
	Pending->HiddenInEditor = HiddenInEditor;
	Pending->SkippedHiddenLevels = SkippedHiddenLevels;
	Pending->UnmatchedTargets = Unmatched;
	Pending->bIsActiveClient = bIsActiveClient;

	bool bAllViewports = false;
	if (Payload.IsValid())
	{
		Payload->TryGetBoolField(TEXT("all_viewports"), bAllViewports);
	}
	Pending->bAllViewports = bAllViewports;

	for (AActor* Actor : Visible)
	{
		Pending->TargetActors.Add(Actor);
	}

	// 目标的整体包围盒。整体聚焦时它只用来回报体检数字（真正的取景交给引擎），
	// 看局部时它是取景的依据
	FBox TargetBox(ForceInit);
	for (AActor* Actor : Visible)
	{
		FVector Origin, Extent;
		Actor->GetActorBounds(false, Origin, Extent);
		TargetBox += FBox(Origin - Extent, Origin + Extent);
	}
	Pending->TargetBox = TargetBox;
	Pending->AimPoint = TargetBox.IsValid ? TargetBox.GetCenter() : FVector::ZeroVector;

	// ---- region / direction / distance ----
	EUALFocusRegion Region = EUALFocusRegion::Whole;
	EUALFocusDir Direction = EUALFocusDir::Current;
	bool bDirectionGiven = false;
	double ExplicitDistance = 0.0;
	bool bDistanceGiven = false;

	if (Payload.IsValid())
	{
		FString RegionStr;
		if (Payload->TryGetStringField(TEXT("region"), RegionStr))
		{
			RegionStr = RegionStr.ToLower();
			if (RegionStr == TEXT("top")) Region = EUALFocusRegion::Top;
			else if (RegionStr == TEXT("middle")) Region = EUALFocusRegion::Middle;
			else if (RegionStr == TEXT("bottom")) Region = EUALFocusRegion::Bottom;
			else if (RegionStr != TEXT("whole") && !RegionStr.IsEmpty())
			{
				UAL_CommandUtils::SendError(RequestId, 400,
					TEXT("region 只认 whole / top / middle / bottom（按包围盒高度三等分）"));
				return;
			}
		}

		FString DirStr;
		if (Payload->TryGetStringField(TEXT("direction"), DirStr))
		{
			DirStr = DirStr.ToLower();
			bDirectionGiven = true;
			if (DirStr == TEXT("horizontal")) Direction = EUALFocusDir::Horizontal;
			else if (DirStr == TEXT("top")) Direction = EUALFocusDir::Top;
			else if (DirStr == TEXT("current")) Direction = EUALFocusDir::Current;
			else
			{
				UAL_CommandUtils::SendError(RequestId, 400,
					TEXT("direction 只认 current / horizontal / top。不做 front/left/right —— ")
					TEXT("一个道具的「正面」是哪一面引擎并不知道，那种参数一半概率是错的"));
				return;
			}
		}

		double D = 0.0;
		if (Payload->TryGetNumberField(TEXT("distance"), D) && D > 0.0)
		{
			ExplicitDistance = D;
			bDistanceGiven = true;
		}
	}
	Pending->Region = Region;

	// 看局部时默认水平平视。
	//
	// 这条默认值是真机翻车换来的：当时的任务是「看这棵树的树干」，相机保持了
	// 视口原来的俯视角度，于是连拍五六张全是俯视草地和树冠 —— 距离是对的，
	// 角度从头到尾没对过。看树干、看门、看柱子这类「物体的某一截」，
	// 人的本能就是走过去平视，不是从天上往下看。
	if (Region != EUALFocusRegion::Whole && !bDirectionGiven)
	{
		Direction = EUALFocusDir::Horizontal;
	}

	// 整体聚焦 + 不指定朝向和距离 —— 走引擎那条路。
	// 它那边处理了子 Actor、粒子发射器、巨大但看不见的组件等一堆边角情况，
	// 自己重写只会更差，所以能用就用
	const bool bUseEngineFocus =
		Region == EUALFocusRegion::Whole && Direction == EUALFocusDir::Current && !bDistanceGiven;

	if (!bUseEngineFocus)
	{
		if (!TargetBox.IsValid)
		{
			UAL_CommandUtils::SendError(RequestId, 409,
				TEXT("目标算不出包围盒（可能一个可渲染组件都没有），没法定位机位"));
			return;
		}
		if (!Client->IsPerspective())
		{
			UAL_CommandUtils::SendError(RequestId, 409,
				TEXT("当前是正交视口（顶视/侧视那种），指定 region/direction/distance 需要透视视口。")
				TEXT("先把视口切回 Perspective，或者不带这些参数走默认聚焦"));
			return;
		}

		const FBox Slice = UAL_RegionBox(TargetBox, Region);
		const FVector Aim = Slice.GetCenter();
		Pending->AimPoint = Aim;

		// 视口的真实宽高比。拿错了横向物体会被裁掉两头
		double Aspect = 16.0 / 9.0;
		if (FViewport* Vp = Client->Viewport)
		{
			const FIntPoint Size = Vp->GetSizeXY();
			if (Size.X > 0 && Size.Y > 0)
			{
				Aspect = static_cast<double>(Size.X) / static_cast<double>(Size.Y);
			}
		}

		const double Distance = bDistanceGiven
			? ExplicitDistance
			: UAL_FramingDistance(Slice, Region, Client->ViewFOV, Aspect);

		// 相机看向的单位向量。**不手写 atan2、不手搓 FRotator** ——
		// FRotationMatrix::MakeFromX 从一个方向向量直接得到朝向，
		// 各分量塞错槽位这种事在这里根本没有发生的余地
		FVector ViewDir;
		switch (Direction)
		{
		case EUALFocusDir::Top:
			ViewDir = FVector(0.0, 0.0, -1.0);
			break;
		case EUALFocusDir::Horizontal:
		{
			// 保持视口当前的水平朝向，只把俯仰压平
			FVector Flat = Client->GetViewRotation().Vector();
			Flat.Z = 0.0;
			ViewDir = Flat.IsNearlyZero() ? FVector(1.0, 0.0, 0.0) : Flat.GetSafeNormal();
			break;
		}
		case EUALFocusDir::Current:
		default:
			ViewDir = Client->GetViewRotation().Vector();
			break;
		}

		const FVector NewLocation = Aim - ViewDir * Distance;
		const FRotator NewRotation = FRotationMatrix::MakeFromX(Aim - NewLocation).Rotator();

		// SetViewLocation/SetViewRotation 是**立即生效**的，不走过渡动画。
		// 所以这条路上回读当场就有效，也不存在「截图拍到半路」的问题
		if (bAllViewports)
		{
			for (FLevelEditorViewportClient* Other : GEditor->GetLevelViewportClients())
			{
				if (Other && Other->IsPerspective())
				{
					Other->SetViewLocation(NewLocation);
					Other->SetViewRotation(NewRotation);
					Other->Invalidate();
				}
			}
		}
		else
		{
			Client->SetViewLocation(NewLocation);
			Client->SetViewRotation(NewRotation);
			Client->Invalidate();
		}

		UAL_FinishFocus(Pending, true);
		return;
	}

	// ACTIVEVIEWPORTONLY 不能省：不带它引擎会把**所有**视口都挪走，
	// 用户开着四视图的话四个全被抢
	const TCHAR* Command = bAllViewports ? TEXT("CAMERA ALIGN") : TEXT("CAMERA ALIGN ACTIVEVIEWPORTONLY");
	if (!GEditor->Exec(World, Command))
	{
		// Exec_Camera 只在「找不到目标元素」时返回 false。我们刚选中过，正常不该走到这
		UE_LOG(LogUALEditor, Warning, TEXT("CAMERA ALIGN returned false (selection had %d actors)"), Visible.Num());
	}

	// 镜头是**飞过去**的不是跳过去的（FocusViewportOnBox 的 bInstant 默认 false，
	// 走 TransitionToLocation 做插值动画）。此刻读相机拿到的是起点，
	// 紧接着截图拍到的是半路 —— 挂 ticker 等 UpdateTransition() 说停了再回响应
	const double StartTime = FPlatformTime::Seconds();
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([Pending, StartTime](float) -> bool
		{
			FLevelEditorViewportClient* Live = Pending->Client;

			// 视口在等待期间被关掉了（用户切了布局）。指针已经悬空，
			// 只能用还活着的那部分信息回一个诚实的响应
			bool bStillAlive = false;
			if (GEditor && Live)
			{
				for (FLevelEditorViewportClient* Candidate : GEditor->GetLevelViewportClients())
				{
					if (Candidate == Live)
					{
						bStillAlive = true;
						break;
					}
				}
			}
			if (!bStillAlive)
			{
				Pending->Client = nullptr;
				UAL_FinishFocus(Pending, false);
				return false;
			}

			// UpdateTransition() 按墙上时间读曲线，不是按调用次数步进，
			// 所以视口自己的 Tick 也在调它并不冲突
			const bool bAnimating = Live->GetViewTransform().UpdateTransition();
			const double Waited = FPlatformTime::Seconds() - StartTime;

			if (!bAnimating)
			{
				UAL_FinishFocus(Pending, true);
				return false;
			}
			if (Waited >= UAL_FOCUS_SETTLE_TIMEOUT)
			{
				UAL_FinishFocus(Pending, false);
				return false;
			}
			return true;
		}),
		0.0f);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("viewport.focus 仅在编辑器中可用"));
#endif
}
