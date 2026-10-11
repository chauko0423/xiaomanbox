#include "UAL_SystemCommands.h"
#include "UAL_CommandUtils.h"
#include "UAL_EditorHealth.h"

#include "IPythonScriptPlugin.h"
#include "Editor.h"
// get_project_info 里的 has_code 用它。GameProjectGeneration 本来就是
// PrivateDependencyModuleNames 里的依赖，这里不新增任何模块
#include "GameProjectUtils.h"
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Serialization/JsonSerializer.h"
#include "ProjectDescriptor.h"
#include "Misc/App.h"
#include "EngineStats.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "Containers/Ticker.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/FileManager.h"
#include "Async/Async.h"
#include "HAL/IConsoleManager.h"
// FStringOutputDevice 5.7 起搬进了自己的头文件，UnrealString.h 只在旧 include 顺序下才顺带包含它
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
#include "Misc/StringOutputDevice.h"
#endif

// 性能统计宏定义
#if defined(STATS) && STATS
extern ENGINE_API float GAverageFPS;
extern ENGINE_API float GAverageMS;
// UE 5.1+ 正式支持扩展统计变量（ENGINE_API导出）
// UE 5.0 中这些变量可能存在于引擎内部但未导出，无法直接访问
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
#define UAL_WITH_EXTENDED_AVG_STATS 0
extern ENGINE_API float GAverageGameTime;
extern ENGINE_API float GAverageDrawTime;
extern ENGINE_API float GAverageRHITTime;
extern ENGINE_API float GAverageGPUTime;
#else
// UE 5.0: 这些变量在引擎中可能存在但未通过ENGINE_API导出
// 直接声明会导致链接错误，因此无法在5.0中直接访问
// 用户可以通过 stat unit 等控制台命令查看这些信息
#define UAL_WITH_EXTENDED_AVG_STATS 0
#endif
#else
#define UAL_WITH_EXTENDED_AVG_STATS 0
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUALSystem, Log, All);

void FUAL_SystemCommands::RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("cmd.run_python"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_RunPython(Payload, RequestId);
	});

	// 别名 `cmd.exec_console` 已删（2026-09-16）：和下面这条指向同一个 handler，
	// 盒子这边从来没调过它。两个名字只会让下一个人以为是两件事
	CommandMap.Add(TEXT("system.run_console_command"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ExecConsole(Payload, RequestId);
	});

	CommandMap.Add(TEXT("system.get_performance_stats"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetPerformanceStats(Payload, RequestId);
	});

	CommandMap.Add(TEXT("system.manage_plugin"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ManagePlugin(Payload, RequestId);
	});

	CommandMap.Add(TEXT("system.get_project_info"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetProjectInfo(Payload, RequestId);
	});

	CommandMap.Add(TEXT("system.capture_perf_trace"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CapturePerfTrace(Payload, RequestId);
	});

	CommandMap.Add(TEXT("system.capture_insights_trace"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CaptureInsightsTrace(Payload, RequestId);
	});

	// 状态监控：只读快照，采集在 FUAL_EditorHealth 里常驻进行
	CommandMap.Add(TEXT("system.get_editor_health"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		UAL_CommandUtils::SendResponse(RequestId, 200, FUAL_EditorHealth::BuildReport());
	});
}

// ========== 从 UAL_CommandHandler.cpp 迁移以下函数 ==========
// 原始行号参考:
//   Handle_RunPython:           1631-1653
//   Handle_ExecConsole:         1655-1678
//   Handle_GetPerformanceStats: 1680-1719

void FUAL_SystemCommands::Handle_RunPython(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Script;
	if (!Payload->TryGetStringField(TEXT("script"), Script))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: script"));
		return;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	bool bExecuted = false;

	// 这里原来包着 `#if defined(WITH_PYTHON) && WITH_PYTHON`，而 **我们这个模块拿不到
	// 那个宏** —— `WITH_PYTHON` 是引擎 ThirdParty 模块 `Python3` 的 PublicDefinition，
	// 而 `PythonScriptPlugin` 把 `Python3` 放在 **PrivateDependencyModuleNames** 里，
	// 所以它不会传递给依赖 PythonScriptPlugin 的下游模块。
	// 实测：Intermediate 里生成的 Definitions.UnrealAgentLink.h 有 PYTHONSCRIPTPLUGIN_API，
	// 但**没有 WITH_PYTHON** —— 于是这整段一直被编译掉，cmd.run_python 永远回 ok:false。
	//
	// 不去补 `Python3` 依赖（ThirdParty 模块，会连带 include/库路径，为一个宏不值当）。
	// 引擎给了运行时等价物：`IsPythonAvailable()` 就是「这份引擎编进 Python 了没有」，
	// 比宏更准（九个版本的 IPythonScriptPlugin.h 都有它，逐版本核过）。
	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
	if (PythonPlugin && PythonPlugin->IsPythonAvailable())
	{
		// 使用扩展版本获取详细输出
		FPythonCommandEx PythonCommand;
		PythonCommand.Command = Script;
		PythonCommand.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
		PythonCommand.FileExecutionScope = EPythonFileExecutionScope::Public; // 共享环境，便于后续脚本访问变量
		
		bExecuted = PythonPlugin->ExecPythonCommandEx(PythonCommand);
		
		// 返回脚本结果（成功时为表达式结果，失败时为错误追踪）
		if (!PythonCommand.CommandResult.IsEmpty())
		{
			Data->SetStringField(TEXT("result"), PythonCommand.CommandResult);
		}
		
		// 收集日志输出（print()、unreal.log() 等）
		if (PythonCommand.LogOutput.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> LogArray;
			for (const FPythonLogOutputEntry& Entry : PythonCommand.LogOutput)
			{
				TSharedPtr<FJsonObject> LogEntry = MakeShared<FJsonObject>();
				LogEntry->SetStringField(TEXT("type"), LexToString(Entry.Type));
				LogEntry->SetStringField(TEXT("message"), Entry.Output);
				LogArray.Add(MakeShared<FJsonValueObject>(LogEntry));
			}
			Data->SetArrayField(TEXT("logs"), LogArray);
		}
	}
	else
	{
		Data->SetStringField(TEXT("result"), TEXT("这份引擎没有可用的 Python（PythonScriptPlugin 未加载或未编入 Python）"));
	}

	Data->SetBoolField(TEXT("ok"), bExecuted);
	UAL_CommandUtils::SendResponse(RequestId, bExecuted ? 200 : 500, Data);
}

void FUAL_SystemCommands::Handle_ExecConsole(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Command;
	if (!Payload->TryGetStringField(TEXT("command"), Command))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: command"));
		return;
	}

	bool bResult = false;
	// 接住命令的输出。
	//
	// 原来 Exec 不传输出设备，命令打印的东西全进了引擎日志，调用方只拿到
	// 一句 "OK"。后果是**查询类命令完全没用**：问 `r.ScreenPercentage`
	// 现在是多少，回一个 "OK"；`stat` 系列、`obj list`、各种 cvar 查询同理。
	// 调用方能执行命令却永远看不到结果，"跑个命令看看" 这件事办不成。
	FStringOutputDevice CommandOutput;
	if (GEngine)
	{
#if WITH_EDITOR
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
#else
		UWorld* World = GWorld;
#endif
		bResult = GEngine->Exec(World, *Command, CommandOutput);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("result"), bResult ? TEXT("OK") : TEXT("Failed"));

	FString Output = CommandOutput;
	Output.TrimStartAndEndInline();
	if (!Output.IsEmpty())
	{
		// 截断：`obj list` 之类能刷出上万行
		const int32 MaxChars = 8000;
		Data->SetStringField(TEXT("output"),
			Output.Len() > MaxChars ? Output.Right(MaxChars) : Output);
		if (Output.Len() > MaxChars)
		{
			Data->SetNumberField(TEXT("output_truncated_chars"), Output.Len() - MaxChars);
		}
	}
	else if (bResult)
	{
		// 说清楚是「这条命令本来就不打印东西」，而不是「输出丢了」——
		// 像 `stat unit` 是切换屏幕叠加显示的，本来就没有文字返回。
		Data->SetStringField(TEXT("output_note"),
			TEXT("该命令没有产生文字输出（切换类命令如 stat unit 属于正常情况；部分命令只写引擎日志，可用 messagelog 查看）。"));
	}

	// 失败要说为什么。
	//
	// 原来失败只回 "Failed"，盒子那边只能说「执行失败（错误码 500）」—— 调用方不知道是
	// 命令名打错了、还是参数不对，下次照样撞；经验系统也从这样的报错里学不到任何东西。
	// 命令名带引号：盒子按引号里的名字认「报错在说哪个命令」。
	if (!bResult)
	{
		FString Verb = Command;
		Verb.TrimStartAndEndInline();
		int32 SpaceIndex = INDEX_NONE;
		if (Verb.FindChar(TEXT(' '), SpaceIndex))
		{
			Verb.LeftInline(SpaceIndex);
		}

		FString Reason;
		if (!Output.IsEmpty())
		{
			Reason = Output.Left(2000);
		}
		else if (!IConsoleManager::Get().FindConsoleObject(*Verb))
		{
			Reason = FString::Printf(TEXT("Command not recognized: '%s'"), *Verb);
		}
		else
		{
			Reason = FString::Printf(TEXT("Command '%s' exists but did not accept these arguments"), *Verb);
		}
		Data->SetStringField(TEXT("error"), Reason);
	}

	UAL_CommandUtils::SendResponse(RequestId, bResult ? 200 : 500, Data);
}

void FUAL_SystemCommands::Handle_GetPerformanceStats(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	float Fps = 0.0f;
	float FrameMs = 0.0f;
	float GameThreadMs = 0.0f;
	float RenderThreadMs = 0.0f;
	float RHIMs = 0.0f;
	float GPUMs = 0.0f;

#if defined(STATS) && STATS
	Fps = GAverageFPS;
	FrameMs = GAverageMS;
#if UAL_WITH_EXTENDED_AVG_STATS
	// UE 5.1+: 使用正式导出的扩展统计变量
	GameThreadMs = GAverageGameTime;
	RenderThreadMs = GAverageDrawTime;
	RHIMs = GAverageRHITTime;
	GPUMs = GAverageGPUTime;
#else
	// UE 5.0: 扩展统计变量未通过ENGINE_API导出，无法直接访问
	// 这些变量在引擎内部可能存在，但由于未导出，插件无法访问
	// 因此 RenderThreadMs、RHIMs、GPUMs 将保持为 0
	// 
	// 说明：
	// - 这是UE 5.0的限制，不是插件的问题
	// - 用户可以通过控制台命令查看这些信息：
	//   * stat unit - 显示所有线程和GPU时间
	//   * stat scenerendering - 显示场景渲染统计
	//   * stat rhi - 显示RHI线程统计
	//   * stat game - 显示游戏线程统计
	// - 在UE 5.1+中，这些值可以正常获取
	GameThreadMs = FrameMs; // 使用FrameMs作为GameThreadMs的近似值
	RenderThreadMs = 0.0f;   // UE 5.0中无法获取，保持为0
	RHIMs = 0.0f;           // UE 5.0中无法获取，保持为0
	GPUMs = 0.0f;           // UE 5.0中无法获取，保持为0
#endif
#else
	const float DeltaSeconds = FApp::GetDeltaTime();
	if (DeltaSeconds > SMALL_NUMBER)
	{
		Fps = 1.0f / DeltaSeconds;
		FrameMs = DeltaSeconds * 1000.0f;
		GameThreadMs = FrameMs;
	}
#endif

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("fps"), Fps);
	Data->SetNumberField(TEXT("frame_ms"), FrameMs);
	Data->SetNumberField(TEXT("game_thread_ms"), GameThreadMs);
	Data->SetNumberField(TEXT("render_thread_ms"), RenderThreadMs);
	Data->SetNumberField(TEXT("rhi_thread_ms"), RHIMs);
	Data->SetNumberField(TEXT("gpu_ms"), GPUMs);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// system.manage_plugin —— 启用/禁用插件
// ============================================================================
//
// ## 这里出过一次「报成功但没生效」的事故（2026-08-31，UE 5.7）
//
// 老代码只调了 `IProjectManager::SetPluginEnabled()`，注释还写着「它会自动
// 处理 .uproject 文件更新」。**引擎自己的头文件写着相反的话**：
//
//   Potentially updates the current project descriptor, but does not save to
//   disk and may require restarting to load it.
//   @note Use IsCurrentProjectDirty() to tell whether the project was actually modified.
//     —— Runtime/Projects/Public/Interfaces/IProjectManager.h
//
// 它只改内存里的 FProjectDescriptor 并把 bIsCurrentProjectDirty 置位，落盘要
// 另外调 `SaveCurrentProjectToDisk()`（引擎自己在 AssetGuideline.cpp 里就是这么
// 成对写的）。于是用户拿到 "Plugin enabled. Restart required."，重启两次，
// `.uproject` 的 Plugins 数组里连这一项都没有。
//
// 所以这个命令现在做三件事，缺一件都不算成功：
//   1. SetPluginEnabled   改内存里的项目描述
//   2. SaveCurrentProjectToDisk  落盘
//   3. **重新从磁盘读一遍 .uproject**，确认那一条真的在里面
//
// 第 3 步是这次事故的核心教训：不回读的「成功」等于没有成功。
//
// ## 为什么「条目不存在」有时候也是对的
//
// `SetPluginEnabled` 在**目标状态和默认状态一致**时会把条目删掉（见
// ProjectManager.cpp 里那段 `DefaultEnabledPlugins.Contains(PluginName) == bEnabled`）：
// 禁用一个本来就默认关的插件，正确结果就是 .uproject 里什么都没有。
// 所以校验拿 `IsEnabledByDefault()` 兜底，否则会把正确结果误报成失败。

namespace
{
	/** 某个插件在磁盘上的 .uproject 里是什么状态 */
	enum class EUAL_UprojectPluginState : uint8
	{
		Enabled,
		Disabled,
		/** Plugins 数组里没有这一条 —— 等价于「跟默认状态一致」 */
		Absent,
		/** .uproject 读不出来，什么都不能断言 */
		Unreadable
	};

	const TCHAR* UAL_UprojectStateToString(EUAL_UprojectPluginState State)
	{
		switch (State)
		{
		case EUAL_UprojectPluginState::Enabled:  return TEXT("enabled");
		case EUAL_UprojectPluginState::Disabled: return TEXT("disabled");
		case EUAL_UprojectPluginState::Absent:   return TEXT("absent");
		default:                                 return TEXT("unreadable");
		}
	}

	/**
	 * 重新从磁盘解析 .uproject，看某个插件写成了什么。
	 *
	 * 走 `FProjectDescriptor::Load` 而不是自己解 JSON：格式细节（大小写、
	 * 平台白名单、旧版字段）归引擎管，我们只关心 Name/bEnabled。
	 */
	EUAL_UprojectPluginState UAL_ReadUprojectPluginState(const FString& UprojectPath, const FString& PluginName, FString& OutError)
	{
		FProjectDescriptor Descriptor;
		FText FailReason;
		if (!Descriptor.Load(UprojectPath, FailReason))
		{
			OutError = FailReason.ToString();
			return EUAL_UprojectPluginState::Unreadable;
		}

		for (const FPluginReferenceDescriptor& Ref : Descriptor.Plugins)
		{
			if (Ref.Name == PluginName)
			{
				return Ref.bEnabled ? EUAL_UprojectPluginState::Enabled : EUAL_UprojectPluginState::Disabled;
			}
		}
		return EUAL_UprojectPluginState::Absent;
	}
}

void FUAL_SystemCommands::Handle_ManagePlugin(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString PluginName;
	if (!Payload->TryGetStringField(TEXT("plugin_name"), PluginName) || PluginName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: plugin_name"));
		return;
	}

	FString ActionString;
	const bool bHasAction = Payload->TryGetStringField(TEXT("action"), ActionString);
	const FString NormalizedAction = bHasAction ? ActionString.ToLower() : TEXT("query");

	IPluginManager& PluginManager = IPluginManager::Get();
	const TSharedPtr<IPlugin> Plugin = PluginManager.FindPlugin(PluginName);
	if (!Plugin.IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Plugin '%s' not found"), *PluginName));
		return;
	}

	bool bSuccess = true;
	bool bRequiresRestart = false;
	FString Message;

	const bool bCurrentlyEnabled = Plugin->IsEnabled();
	const FString UprojectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());

	// 默认状态：条目从 .uproject 里消失时，「正确」指的是这个值。
	// bAllowEnginePluginsEnabledByDefault 跟着项目的 bDisableEnginePluginsByDefault 走，
	// 和 FProjectManager::GetDefaultEnabledPlugins 里的判据保持一致。
	const FProjectDescriptor* CurrentProject = IProjectManager::Get().GetCurrentProject();
	const bool bDefaultEnabled = Plugin->IsEnabledByDefault(
		CurrentProject ? !CurrentProject->bDisableEnginePluginsByDefault : true);

	/**
	 * 改状态 + 落盘。返回是否已经写到磁盘上（还没回读校验）。
	 *
	 * SetPluginEnabled 只动内存，落盘必须自己调 SaveCurrentProjectToDisk ——
	 * 见本文件上面那段事故说明。
	 */
	auto SetPluginEnabledAndSave = [&PluginName, &Message](bool bEnable, bool bForceWrite) -> bool
	{
		FText FailReason;
		if (!IProjectManager::Get().SetPluginEnabled(PluginName, bEnable, FailReason))
		{
			Message = FString::Printf(TEXT("Failed to update project descriptor: %s"), *FailReason.ToString());
			return false;
		}

		// bForceWrite：磁盘上的 .uproject 和目标不一致（比如被外部改过），
		// 内存描述符却已经是目标值、不算脏 —— 这时不写盘，磁盘就永远停在旧状态
		if (!bForceWrite && !IProjectManager::Get().IsCurrentProjectDirty())
		{
			// 描述符没变化：目标状态已经写在 .uproject 里了（或者和默认一致），
			// 不用写盘。后面的回读会确认这一点。
			return true;
		}

		if (!IProjectManager::Get().SaveCurrentProjectToDisk(FailReason))
		{
			// 最常见的原因是 .uproject 只读（版本控制没签出）。
			// 这里不弹签出对话框：命令是 Agent 发的，弹窗会把编辑器卡在那儿。
			Message = FString::Printf(TEXT("Failed to write .uproject: %s"), *FailReason.ToString());
			return false;
		}
		return true;
	};

	// 有没有真的动过状态 —— 决定要不要回读校验、要不要提示重启
	bool bAttemptedChange = false;
	bool bTargetEnabled = bCurrentlyEnabled;

	/**
	 * 「已经是目标状态、不用动」要按**磁盘上的 .uproject** 判断，不能看内存里的 IsEnabled()。
	 *
	 * 插件的启停要重启才生效，所以内存状态和 .uproject 经常对不上：刚 disable 过、
	 * 还没重启时，IsEnabled() 仍是 true。这时再 enable，老代码以为「已经启用」直接跳过写盘，
	 * 磁盘上还挂着那条 disable，回执却是 200 —— 重启后插件照样被关掉。
	 * 读不出来（Unreadable）就当不一致，老老实实走一遍写盘 + 回读。
	 */
	FString PreReadError;
	const EUAL_UprojectPluginState PreDiskState = UAL_ReadUprojectPluginState(UprojectPath, PluginName, PreReadError);
	auto DiskAlreadyAt = [PreDiskState, bDefaultEnabled](bool bEnable) -> bool
	{
		switch (PreDiskState)
		{
		case EUAL_UprojectPluginState::Enabled:
			return bEnable;
		case EUAL_UprojectPluginState::Disabled:
			return !bEnable;
		case EUAL_UprojectPluginState::Absent:
			return bDefaultEnabled == bEnable;
		default:
			return false;
		}
	};

	if (NormalizedAction == TEXT("enable") || NormalizedAction == TEXT("disable"))
	{
		bTargetEnabled = NormalizedAction == TEXT("enable");
		if (!DiskAlreadyAt(bTargetEnabled) || bCurrentlyEnabled != bTargetEnabled)
		{
			bAttemptedChange = true;
			bSuccess = SetPluginEnabledAndSave(bTargetEnabled, /*bForceWrite=*/!DiskAlreadyAt(bTargetEnabled));
		}
	}
	else if (NormalizedAction == TEXT("query"))
	{
		// no-op
	}
	else
	{
		UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(TEXT("Unsupported action: %s"), *ActionString));
		return;
	}

	// 回读校验：写完之后重新从磁盘读一遍，确认那一条真的在里面。
	// Query 也读，代价只有一次文件解析，换来的是「引擎里没加载但 .uproject
	// 已经写好了」这种状态能被如实报出来。
	FString ReadError;
	const EUAL_UprojectPluginState DiskState = UAL_ReadUprojectPluginState(UprojectPath, PluginName, ReadError);

	bool bUprojectVerified = false;
	switch (DiskState)
	{
	case EUAL_UprojectPluginState::Enabled:
		bUprojectVerified = bTargetEnabled;
		break;
	case EUAL_UprojectPluginState::Disabled:
		bUprojectVerified = !bTargetEnabled;
		break;
	case EUAL_UprojectPluginState::Absent:
		// 没有条目 = 用默认状态，只有当默认就是目标状态时才算数
		bUprojectVerified = (bDefaultEnabled == bTargetEnabled);
		break;
	default:
		bUprojectVerified = false;
		break;
	}

	if (bAttemptedChange && bSuccess)
	{
		if (bUprojectVerified)
		{
			// 只是把 .uproject 里挂着的反向改动撤回来时，运行中的编辑器本来就是目标状态，不用重启
			bRequiresRestart = bCurrentlyEnabled != bTargetEnabled;
			Message = FString::Printf(
				TEXT("Plugin %s in .uproject (verified on disk). %s"),
				bTargetEnabled ? TEXT("enabled") : TEXT("disabled"),
				bRequiresRestart
					? TEXT("Restart required.")
					: TEXT("The running editor already has it in this state, so no restart is needed."));
		}
		else
		{
			// 走到这儿说明写盘报了成功但磁盘上不是那么回事。
			// 宁可报失败：老代码就是在这里无条件回成功，让用户白重启了两次。
			bSuccess = false;
			Message = DiskState == EUAL_UprojectPluginState::Unreadable
				? FString::Printf(TEXT("Wrote .uproject but could not read it back to verify: %s"), *ReadError)
				: FString::Printf(TEXT("Wrote .uproject but the plugin entry is still '%s' on disk. Nothing was changed."),
					UAL_UprojectStateToString(DiskState));
		}
	}
	else if (!bAttemptedChange && NormalizedAction != TEXT("query"))
	{
		// 没动任何东西（磁盘和内存都已是目标）也要以回读为准：回读对不上就不许回 200
		if (!bUprojectVerified)
		{
			bSuccess = false;
			Message = DiskState == EUAL_UprojectPluginState::Unreadable
				? FString::Printf(TEXT("Could not read .uproject to verify the plugin state: %s"), *ReadError)
				: FString::Printf(TEXT("The plugin entry is '%s' in .uproject, which does not match the requested state."),
					UAL_UprojectStateToString(DiskState));
		}
		else
		{
			Message = bTargetEnabled
				? TEXT("Plugin is already enabled in .uproject (verified on disk). Nothing was changed.")
				: TEXT("Plugin is already disabled in .uproject (verified on disk). Nothing was changed.");
		}
	}

	const bool bIsEnabled = Plugin->IsEnabled();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("plugin_name"), Plugin->GetName());
	Result->SetBoolField(TEXT("is_enabled"), bIsEnabled);
	Result->SetBoolField(TEXT("requires_restart"), bRequiresRestart);
	Result->SetStringField(TEXT("friendly_name"), Plugin->GetDescriptor().FriendlyName);
	// 下面四个字段给盒子侧做二次校验用（它自己也会回读一遍 .uproject）
	Result->SetStringField(TEXT("uproject_path"), UprojectPath);
	Result->SetStringField(TEXT("uproject_state"), UAL_UprojectStateToString(DiskState));
	Result->SetBoolField(TEXT("uproject_verified"), bUprojectVerified);
	Result->SetBoolField(TEXT("default_enabled"), bDefaultEnabled);
	if (!Message.IsEmpty())
	{
		Result->SetStringField(TEXT("message"), Message);
	}

	if (!bSuccess)
	{
		UE_LOG(LogUALSystem, Error, TEXT("system.manage_plugin %s %s 失败: %s"), *NormalizedAction, *PluginName, *Message);
	}

	UAL_CommandUtils::SendResponse(RequestId, bSuccess ? 200 : 500, Result);
}

/**
 * system.get_project_info - 获取项目信息
 * 返回项目路径、Content目录等信息,用于外部工具快速定位项目资源
 */
void FUAL_SystemCommands::Handle_GetProjectInfo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const FString ProjectFilePath = FPaths::GetProjectFilePath();
	const FString ProjectDir = FPaths::ProjectDir();
	const FString ContentDir = FPaths::ProjectContentDir();
	const FString SavedDir = FPaths::ProjectSavedDir();
	const FString IntermediateDir = FPaths::ProjectIntermediateDir();
	const FString PluginsDir = FPaths::ProjectPluginsDir();
	
	// 获取项目名称
	FString ProjectName = FApp::GetProjectName();
	
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("ok"), true);
	Response->SetStringField(TEXT("project_name"), ProjectName);
	Response->SetStringField(TEXT("project_path"), ProjectFilePath);
	Response->SetStringField(TEXT("project_dir"), ProjectDir);
	Response->SetStringField(TEXT("content_dir"), ContentDir);
	Response->SetStringField(TEXT("saved_dir"), SavedDir);
	Response->SetStringField(TEXT("intermediate_dir"), IntermediateDir);
	Response->SetStringField(TEXT("plugins_dir"), PluginsDir);
	
	// 添加引擎版本信息
	Response->SetStringField(TEXT("engine_version"), FApp::GetBuildVersion());
	Response->SetNumberField(TEXT("engine_major"), ENGINE_MAJOR_VERSION);
	Response->SetNumberField(TEXT("engine_minor"), ENGINE_MINOR_VERSION);

	/*
	 * 引擎和工程源码的位置，以及这个工程有没有 C++。
	 *
	 * ## 为什么要有 engine_dir / engine_source_dir
	 *
	 * 上面那堆字段全是工程内部的路径，**没有一条指向引擎**。后果是模型即使想核对
	 * 一个 UE API 的真实签名，它也不知道该往哪个目录 grep —— 于是只能凭记忆写。
	 * 而 UE 的 API 在 5.0–5.8 之间是会漂的（`SequencerBindingProxy` 在 5.1 弃用后
	 * 返回父类实例那种事就发生过），凭记忆写出来的东西编不过是好结果，
	 * 编过了但行为不对才是坏结果。
	 *
	 * 有了这两条，模型手上现成的 grep / find 工具就变成了「能查证 API」的工具。
	 * 三行代码换掉一整类幻觉。
	 *
	 * ## 为什么这三条是绝对路径
	 *
	 * 上面的老字段（project_dir 之类）是 `FPaths` 的原样输出，
	 * 在编辑器里通常长成 `../../../` 开头的相对路径 —— 相对的是**引擎二进制目录**，
	 * 不是任何调用方的工作目录。盒子那边拿去拼路径必然拼错。
	 * 老字段不动（改了会影响现有调用方），新字段一律转成绝对路径。
	 *
	 * ## has_code
	 *
	 * 纯蓝图工程加不了 C++ 类（引擎那条路会弹模态对话框卡死编辑器），
	 * 所以这件事必须在动手之前就知道。判断用引擎自己的
	 * `GameProjectUtils::ProjectHasCodeFiles()`，不自己去数 Source 目录 ——
	 * 「什么算有 C++」的定义要和引擎保持一致。
	 */
	Response->SetStringField(TEXT("engine_dir"),
		FPaths::ConvertRelativePathToFull(FPaths::EngineDir()));
	Response->SetStringField(TEXT("engine_source_dir"),
		FPaths::ConvertRelativePathToFull(FPaths::EngineSourceDir()));
	Response->SetStringField(TEXT("project_source_dir"),
		FPaths::ConvertRelativePathToFull(FPaths::GameSourceDir()));
	Response->SetBoolField(TEXT("has_code"), GameProjectUtils::ProjectHasCodeFiles());

	/*
	 * 当前跑着的这个插件，是从哪份源码编出来的。
	 *
	 * 少了这一条真的翻过车：改完代码还没出包，回归测试却跑在旧 DLL 上，
	 * 于是「修复没生效」——接下来整轮排查都在找一个不存在的 bug。
	 * 版本号（VersionName）不解决这个问题，它几个月才动一次。
	 *
	 * 出包时会往插件根目录写一个 .ual-build，里面就是那次的源码指纹。
	 * 原样报出来，验证的第一步就能确认「我测的是不是我改的那份」。
	 * 开发机上直接编译的宿主工程没有这个文件，报 unknown —— 那也是有用的信息。
	 */
	FString PluginBuild = TEXT("unknown");
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealAgentLink")))
	{
		Response->SetStringField(TEXT("plugin_version"), Plugin->GetDescriptor().VersionName);

		FString StampJson;
		if (FFileHelper::LoadFileToString(StampJson, *(Plugin->GetBaseDir() / TEXT(".ual-build"))))
		{
			TSharedPtr<FJsonObject> Stamp;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(StampJson);
			FString Fingerprint;
			if (FJsonSerializer::Deserialize(Reader, Stamp) && Stamp.IsValid() &&
				Stamp->TryGetStringField(TEXT("fingerprint"), Fingerprint))
			{
				PluginBuild = Fingerprint;
			}
		}
	}
	Response->SetStringField(TEXT("plugin_build"), PluginBuild);

	/**
	 * 能力声明 —— 逐项告诉盒子「这一版插件到底有哪几道保护」。
	 *
	 * ## 为什么必须有
	 *
	 * 插件解析 payload 一律是「认识就读、不认识跳过」（`TryGetBoolField`）。
	 * 盒子发一个 `require_empty` 过来，**旧版插件根本不知道它是什么，
	 * 也不会因此拒绝执行** —— 盒子以为自己开了保险，插件照写不误。
	 *
	 * 而插件装在**用户的 UE 工程**里，不随盒子升级：盒子更新到最新，
	 * 用户工程里那份可能还是半年前的。「我编过新版 ZIP」和「用户此刻
	 * 连着的是新版插件」是两件事。
	 *
	 * 所以盒子在做会改用户资产的写入之前，必须来这里问一次；
	 * 字段缺失一律当**不支持**（旧插件的表现恰恰是什么都不说，
	 * 把沉默当默许等于这道闸从来没关过）。
	 *
	 * ## 这些标志不许在别处另写一份
	 *
	 * 每一项都必须和它所声明的那段代码同源。`UAL_VersionCompat.cpp` 里记着
	 * 一次教训：5.4+ 的绑定解析实际是坏的，`capabilities.binding_resolution`
	 * 却照样回 true —— 因为标志是另外硬编码的。改动实现时，**改到哪一项就
	 * 回来改哪一项**，别让它们各说各话。
	 *
	 * 现有项：
	 *   blueprint_require_empty —— blueprint.create_graph 支持 require_empty，
	 *                              不空就一个节点都不建（见 UAL_IsGraphEmptyForImport）
	 *   material_fail_if_exists —— material.create 支持 fail_if_exists，
	 *                              同名不覆盖、直接报错返回
	 *   python                  —— cmd.run_python 在这份引擎上跑得起来。取的是
	 *                              IPythonScriptPlugin::IsPythonAvailable()，和
	 *                              Handle_RunPython 里判断的是同一个值；false 时
	 *                              盒子该直接告诉模型别走 Python，而不是让它试
	 */
	TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();
	Capabilities->SetBoolField(TEXT("blueprint_require_empty"), true);
	Capabilities->SetBoolField(TEXT("material_fail_if_exists"), true);
	{
		IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
		Capabilities->SetBoolField(TEXT("python"), PythonPlugin != nullptr && PythonPlugin->IsPythonAvailable());
	}
	Response->SetObjectField(TEXT("capabilities"), Capabilities);

	UE_LOG(LogUALSystem, Log, TEXT("system.get_project_info: %s"), *ProjectName);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Response);
}

// ============================================================================
// system.capture_perf_trace —— 用引擎自带的 CSV Profiler 采一段时间的帧时序
// ============================================================================
//
// `system.get_performance_stats` 只读引擎的滚动平均值，本质是**此刻这一帧**
// 的快照。掉帧往往是偶发尖峰——加载了一个新贴图、生成了一批粒子——平均值会
// 把它抹平，快照大概率根本没落在那一帧上。
//
// CSV Profiler 是引擎自带的、专门解决这个问题的机制：逐帧记录 FrameTime /
// GameThreadTime / RenderThreadTime / GPUTime 等一串 stat，落盘成 CSV。
// 这里直接调 `FCsvProfiler` 的 C++ API（`BeginCapture` / `EndCapture`），
// 不走 `csvprofile start/stop` 控制台命令 —— 前者能拿到写完的文件名和一个
// 完成回调，后者只能靠猜文件名和轮询目录。
//
// ## 列名从哪确定
//
// 不是猜的。`FrameTime` 定义在 CsvProfiler.cpp（`CSV_CUSTOM_STAT_MINIMAL_GLOBAL`），
// `GameThreadTime` / `RenderThreadTime` / `GPUTime` / `RHIThreadTime` 定义在
// LaunchEngineLoop.cpp —— 都是**不需要任何 category 开关**就会写的全局 stat，
// 逐版本源码核对过，5.0–5.8 一致。`RHIThreadTime` 只在有 RHI 线程时才出现，
// 缺了就不报，不补 0。
//
// ## 文件格式从哪确定
//
// 同样是读源码定的，不是猜的（`FCsvStreamWriter::FinalizeNextRow` /
// `Finalize`）：第一行是表头 `EVENTS,<col1>,<col2>,...`；随后每帧一行；
// 文件末尾会**重复一次表头**，标志数据区结束；再往后是**一整行**的元数据，
// 形如 `[Key1],Value1,[Key2],Value2,...`（没有夹在中间的换行——这是这个格式
// 出了名容易解析错的地方）。我们只要数据区，元数据行直接跳过。

namespace
{
	/** 一列的百分位统计 */
	struct FUAL_ColumnStats
	{
		double Avg = 0.0;
		double Min = 0.0;
		double Max = 0.0;
		double P50 = 0.0;
		double P95 = 0.0;
		double P99 = 0.0;
	};

	FUAL_ColumnStats UAL_ComputeColumnStats(const TArray<double>& Values)
	{
		FUAL_ColumnStats Stats;
		if (Values.Num() == 0)
		{
			return Stats;
		}

		double Sum = 0.0;
		Stats.Min = Values[0];
		Stats.Max = Values[0];
		for (double V : Values)
		{
			Sum += V;
			Stats.Min = FMath::Min(Stats.Min, V);
			Stats.Max = FMath::Max(Stats.Max, V);
		}
		Stats.Avg = Sum / Values.Num();

		TArray<double> Sorted = Values;
		Sorted.Sort();
		auto Percentile = [&Sorted](double P) -> double
		{
			const int32 Index = FMath::Clamp(FMath::CeilToInt(P * Sorted.Num()) - 1, 0, Sorted.Num() - 1);
			return Sorted[Index];
		};
		Stats.P50 = Percentile(0.50);
		Stats.P95 = Percentile(0.95);
		Stats.P99 = Percentile(0.99);
		return Stats;
	}

	TSharedPtr<FJsonObject> UAL_ColumnStatsToJson(const FUAL_ColumnStats& Stats)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("avg"), Stats.Avg);
		Obj->SetNumberField(TEXT("min"), Stats.Min);
		Obj->SetNumberField(TEXT("max"), Stats.Max);
		Obj->SetNumberField(TEXT("p50"), Stats.P50);
		Obj->SetNumberField(TEXT("p95"), Stats.P95);
		Obj->SetNumberField(TEXT("p99"), Stats.P99);
		return Obj;
	}

	/**
	 * 只挑这几列报。CSV 里可能有几十列（内存、各种自定义 stat），但报告
	 * 只该回答「卡在哪」，不是把整份原始数据倒给调用方——那份数据还在
	 * `csv_path` 上，想深挖可以自己用 Session Frontend 打开。
	 */
	const TCHAR* UAL_TrackedColumns[] = {
		TEXT("FrameTime"),
		TEXT("GameThreadTime"),
		TEXT("RenderThreadTime"),
		TEXT("RHIThreadTime"),
		TEXT("GPUTime")
	};

	struct FUAL_CsvParseResult
	{
		bool bOk = false;
		FString Error;
		int32 FrameCount = 0;
		TMap<FString, FUAL_ColumnStats> ColumnStats;
		/** 面数最高（FrameTime 最大）的那一帧，各列原始值 */
		TMap<FString, double> WorstFrame;
	};

	/**
	 * 解析 CSV Profiler 的输出。
	 *
	 * 只認**数据区**：从表头到「表头第二次出现」之间的行。第二次出现的表头
	 * 之后是元数据（一整行、没有换行分隔的 `[Key],Value,...`），我们不需要
	 * 那部分，见文件头「文件格式从哪确定」。
	 */
	FUAL_CsvParseResult UAL_ParseCsvProfilerFile(const FString& FilePath)
	{
		FUAL_CsvParseResult Result;

		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *FilePath))
		{
			Result.Error = FString::Printf(TEXT("Could not read capture file: %s"), *FilePath);
			return Result;
		}
		if (Lines.Num() < 2)
		{
			Result.Error = TEXT("Capture file has no frame data (was the capture stopped before any frame completed?)");
			return Result;
		}

		TArray<FString> Header;
		Lines[0].ParseIntoArray(Header, TEXT(","), false);
		if (Header.Num() == 0 || Header[0] != TEXT("EVENTS"))
		{
			Result.Error = TEXT("Unrecognized capture file format (missing EVENTS header)");
			return Result;
		}

		// 列名 → 该列在 TrackedColumns 里的索引，没被跟踪的列直接不记录，省内存
		TMap<int32, FString> TrackedColumnIndices;
		for (int32 ColIdx = 1; ColIdx < Header.Num(); ++ColIdx)
		{
			for (const TCHAR* Tracked : UAL_TrackedColumns)
			{
				if (Header[ColIdx].Equals(Tracked, ESearchCase::CaseSensitive))
				{
					TrackedColumnIndices.Add(ColIdx, Header[ColIdx]);
					break;
				}
			}
		}

		TMap<FString, TArray<double>> RawValues;
		for (const TPair<int32, FString>& Pair : TrackedColumnIndices)
		{
			RawValues.Add(Pair.Value, TArray<double>());
		}

		double WorstFrameTime = -1.0;
		TMap<FString, double> WorstFrameValues;

		for (int32 LineIdx = 1; LineIdx < Lines.Num(); ++LineIdx)
		{
			const FString& Line = Lines[LineIdx];
			// 文件末尾重复一次表头，标志数据区到此为止（见文件头说明）
			if (Line.StartsWith(TEXT("EVENTS,")) || Line.Equals(TEXT("EVENTS")))
			{
				break;
			}

			TArray<FString> Cells;
			Line.ParseIntoArray(Cells, TEXT(","), false);
			if (Cells.Num() < Header.Num())
			{
				continue; // 半行或损坏行，跳过而不是让整个解析失败
			}

			Result.FrameCount++;

			TMap<FString, double> ThisFrame;
			for (const TPair<int32, FString>& Pair : TrackedColumnIndices)
			{
				const double Value = FCString::Atod(*Cells[Pair.Key]);
				RawValues[Pair.Value].Add(Value);
				ThisFrame.Add(Pair.Value, Value);
			}

			const double* ThisFrameTime = ThisFrame.Find(TEXT("FrameTime"));
			if (ThisFrameTime && *ThisFrameTime > WorstFrameTime)
			{
				WorstFrameTime = *ThisFrameTime;
				WorstFrameValues = ThisFrame;
			}
		}

		if (Result.FrameCount == 0)
		{
			Result.Error = TEXT("Capture file has no frame data (was the capture stopped before any frame completed?)");
			return Result;
		}

		for (const TPair<FString, TArray<double>>& Pair : RawValues)
		{
			Result.ColumnStats.Add(Pair.Key, UAL_ComputeColumnStats(Pair.Value));
		}
		Result.WorstFrame = WorstFrameValues;
		Result.bOk = true;
		return Result;
	}

	/** 谁在拖后腿：比较三个线程/GPU 的均值，同 ue_get_performance_stats 的判法 */
	FString UAL_DescribeBottleneck(const TMap<FString, FUAL_ColumnStats>& Stats)
	{
		const FUAL_ColumnStats* Game = Stats.Find(TEXT("GameThreadTime"));
		const FUAL_ColumnStats* Render = Stats.Find(TEXT("RenderThreadTime"));
		const FUAL_ColumnStats* Gpu = Stats.Find(TEXT("GPUTime"));
		if (!Game || !Render || !Gpu)
		{
			return TEXT("unknown");
		}
		const double MaxCpu = FMath::Max(Game->Avg, Render->Avg);
		if (Gpu->Avg > MaxCpu * 1.2)
		{
			return TEXT("GPU");
		}
		if (Game->Avg > Render->Avg * 1.2)
		{
			return TEXT("GameThread");
		}
		if (Render->Avg > Game->Avg * 1.2)
		{
			return TEXT("RenderThread");
		}
		return TEXT("balanced");
	}

	struct FUAL_CsvCaptureSession
	{
		bool bActive = false;
		bool bEndRequested = false;
		FString RequestId;
		double DurationSeconds = 0.0;
		double StartedAt = 0.0;
		bool bGpuStatsRequested = false;
		FDelegateHandle FinishedHandle;
		FTSTicker::FDelegateHandle TickerHandle;

		void Reset()
		{
			bActive = false;
			bEndRequested = false;
			RequestId.Empty();
			DurationSeconds = 0.0;
			StartedAt = 0.0;
			bGpuStatsRequested = false;
		}
	};

	FUAL_CsvCaptureSession GCsvSession;

	void UAL_TeardownCsvSession()
	{
		if (GCsvSession.FinishedHandle.IsValid() && FCsvProfiler::Get())
		{
			FCsvProfiler::Get()->OnCSVProfileFinished().Remove(GCsvSession.FinishedHandle);
			GCsvSession.FinishedHandle.Reset();
		}
		if (GCsvSession.TickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GCsvSession.TickerHandle);
			GCsvSession.TickerHandle.Reset();
		}
	}

	/** 解析结果 → 响应 JSON。跑在游戏线程上（调用方从 AsyncTask 切过来的） */
	void UAL_RespondWithCsvCapture(const FString& RequestId, const FString& RawCsvPath,
		double RequestedDuration, double ActualElapsed, bool bGpuStatsRequested)
	{
		/*
		 * 引擎给的这个文件名**可能是相对路径**，必须在出门前转成绝对路径。
		 *
		 * 来路：FCsvProfiler 用 FPaths::ProfilingDir() 拼输出名（CsvProfiler.cpp），
		 * 那一路最终落到 FPaths::ProjectDir()，而它在
		 * GenericPlatformMisc_GetProjectFilePathProjectDir 里被 DefaultConvertToRelativePath
		 * 相对化成 ../../../<工程>/ —— 工程和引擎在同一个盘时必然如此。
		 *
		 * 引擎自己读得到（按它的 BaseDir 解），盒子读不到：Node 的 stat 按**盒子的**
		 * 工作目录解，于是盘符换成了盒子所在的盘。真机上的表现是「某某盘的
		 * Saved/Profiling/CSV/Profile(...).csv 不存在」，而工程在另一个盘，路径里除盘符外
		 * 一个字都不差 —— 最难查的那种错。
		 *
		 * 同文件里的 insights trace 一直是转过的，只有这条漏了。
		 */
		const FString CsvPath = FPaths::ConvertRelativePathToFull(RawCsvPath);

		// 我们调 BeginCapture 时没要压缩，正常情况下拿到的就是明文 .csv。
		// 但 `csv.CompressionMode 1` 是个全局 cvar，用户本机可能自己开着——
		// 那种情况下拿到 .csv.gz，硬解析文本会解出乱码统计而不报错，
		// 比直接说「这个我们还不支持」糟得多。
		if (CsvPath.EndsWith(TEXT(".csv.gz")))
		{
			UAL_CommandUtils::SendError(RequestId, 500, FString::Printf(
				TEXT("Capture was written compressed (%s) because csv.CompressionMode is forced to 1 on this machine. ")
				TEXT("This tool only reads plain .csv. Run `csv.CompressionMode 0` and capture again, ")
				TEXT("or open the .gz file yourself in Session Frontend / CSVCollate."),
				*CsvPath));
			return;
		}

		const FUAL_CsvParseResult Parsed = UAL_ParseCsvProfilerFile(CsvPath);
		if (!Parsed.bOk)
		{
			// 文件写出来了但解析不出数据，不是「没连上」这类连接问题——
			// 把原始路径带回去，用户至少能自己打开看
			UAL_CommandUtils::SendError(RequestId, 500,
				FString::Printf(TEXT("Capture finished but could not be parsed: %s (file: %s)"), *Parsed.Error, *CsvPath));
			return;
		}

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("ok"), true);
		Data->SetStringField(TEXT("csv_path"), CsvPath);
		Data->SetNumberField(TEXT("frame_count"), Parsed.FrameCount);
		Data->SetNumberField(TEXT("requested_duration_seconds"), RequestedDuration);
		Data->SetNumberField(TEXT("actual_elapsed_seconds"), FMath::RoundToDouble(ActualElapsed * 10.0) / 10.0);
		Data->SetBoolField(TEXT("gpu_stats_enabled"), bGpuStatsRequested);

		TSharedPtr<FJsonObject> StatsJson = MakeShared<FJsonObject>();
		for (const TPair<FString, FUAL_ColumnStats>& Pair : Parsed.ColumnStats)
		{
			StatsJson->SetObjectField(Pair.Key, UAL_ColumnStatsToJson(Pair.Value));
		}
		Data->SetObjectField(TEXT("stats"), StatsJson);

		if (const FUAL_ColumnStats* FrameTime = Parsed.ColumnStats.Find(TEXT("FrameTime")))
		{
			Data->SetNumberField(TEXT("avg_fps"), FrameTime->Avg > 0.0 ? 1000.0 / FrameTime->Avg : 0.0);
			// p99 帧时间对应的瞬时帧率——不是「p99 fps」（那是另一个数，容易搞反）：
			// 这里回答的是「最卡的百分之一帧，帧率掉到多少」
			Data->SetNumberField(TEXT("p99_fps"), FrameTime->P99 > 0.0 ? 1000.0 / FrameTime->P99 : 0.0);
		}

		TSharedPtr<FJsonObject> WorstFrameJson = MakeShared<FJsonObject>();
		for (const TPair<FString, double>& Pair : Parsed.WorstFrame)
		{
			WorstFrameJson->SetNumberField(Pair.Key, Pair.Value);
		}
		Data->SetObjectField(TEXT("worst_frame"), WorstFrameJson);

		Data->SetStringField(TEXT("bottleneck"), UAL_DescribeBottleneck(Parsed.ColumnStats));

		// 和 ue_get_performance_stats 同一条纪律：渲染线程和 GPU 全程接近 0，
		// 说明编辑器视口那段时间根本没在渲染（失焦/后台降帧），这组数字
		// 不能用来下性能结论
		const FUAL_ColumnStats* Render = Parsed.ColumnStats.Find(TEXT("RenderThreadTime"));
		const FUAL_ColumnStats* Gpu = Parsed.ColumnStats.Find(TEXT("GPUTime"));
		const bool bIdleSample = Render && Gpu && Render->Max < 0.05 && Gpu->Max < 0.05;
		Data->SetBoolField(TEXT("idle_sample"), bIdleSample);

		FString Note = TEXT("采自编辑器，不等于打包后的运行表现。要评估实际性能应在 PIE 或独立运行下采集。");
		if (bIdleSample)
		{
			Note = TEXT("警告：渲染线程和 GPU 耗时全程接近 0，说明采样期间编辑器视口没有在渲染")
				TEXT("（窗口在后台或失去焦点会主动降帧）。这组数字不能用来判断性能问题，")
				TEXT("请让编辑器窗口置前、进入 PIE 运行，再重新采集。");
		}
		Data->SetStringField(TEXT("note"), Note);

		UAL_CommandUtils::SendResponse(RequestId, 200, Data);
	}
}

void FUAL_SystemCommands::Handle_CapturePerfTrace(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (!FCsvProfiler::Get())
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("CSV Profiler is not available in this build"));
		return;
	}

	if (GCsvSession.bActive || FCsvProfiler::Get()->IsCapturing())
	{
		UAL_CommandUtils::SendError(RequestId, 409,
			TEXT("A capture is already in progress (this tool, another tool, or a console `csvprofile start`). Wait for it to finish."));
		return;
	}

	double Duration = 10.0;
	Payload->TryGetNumberField(TEXT("duration_seconds"), Duration);
	// 硬顶 120 秒：这是「等一段时间收集数据」的工具，不是「录一段视频」，
	// 太长的话调用方等的时间比查性能问题本身还久
	Duration = FMath::Clamp(Duration, 1.0, 120.0);

	bool bGpuStats = false;
	Payload->TryGetBoolField(TEXT("gpu_stats"), bGpuStats);
	if (bGpuStats)
	{
		// GPU 计时默认关闭（有开销）。只在调用方明确要的时候打开，
		// 不在结束后关回去——是否要长期开着是用户的选择，我们不该替他做这个决定
		if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.GPUCsvStatsEnabled")))
		{
			CVar->Set(1);
		}
	}

	GCsvSession.Reset();
	GCsvSession.bActive = true;
	GCsvSession.RequestId = RequestId;
	GCsvSession.DurationSeconds = Duration;
	GCsvSession.StartedAt = FPlatformTime::Seconds();
	GCsvSession.bGpuStatsRequested = bGpuStats;

	GCsvSession.FinishedHandle = FCsvProfiler::Get()->OnCSVProfileFinished().AddLambda([](const FString& Filename)
	{
		// 不是我们这次会话触发的完成事件（例如用户自己在控制台敲了
		// csvprofile stop）——忽略，让守着超时的那个分支去处理我们自己的请求
		if (!GCsvSession.bActive)
		{
			return;
		}

		const FString CapturedRequestId = GCsvSession.RequestId;
		const double RequestedDuration = GCsvSession.DurationSeconds;
		const double ActualElapsed = FPlatformTime::Seconds() - GCsvSession.StartedAt;
		const bool bGpuStatsRequested = GCsvSession.bGpuStatsRequested;

		UAL_TeardownCsvSession();
		GCsvSession.Reset();

		// 这个回调跑在 CSV 处理线程上（FinalizeCsvFile 内部有
		// check(IsInCsvProcessingThread())）——读文件本身线程安全，
		// 但发 RPC 响应要回到游戏线程，和这个插件其余地方保持一致
		AsyncTask(ENamedThreads::GameThread, [CapturedRequestId, Filename, RequestedDuration, ActualElapsed, bGpuStatsRequested]()
		{
			UAL_RespondWithCsvCapture(CapturedRequestId, Filename, RequestedDuration, ActualElapsed, bGpuStatsRequested);
		});
	});

	FCsvProfiler::Get()->BeginCapture();

	GCsvSession.TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float /*DeltaTime*/) -> bool
		{
			if (!GCsvSession.bActive)
			{
				return false;
			}

			const double Elapsed = FPlatformTime::Seconds() - GCsvSession.StartedAt;

			if (!GCsvSession.bEndRequested && Elapsed >= GCsvSession.DurationSeconds)
			{
				GCsvSession.bEndRequested = true;
				if (FCsvProfiler::Get() && FCsvProfiler::Get()->IsCapturing())
				{
					FCsvProfiler::Get()->EndCapture();
				}
				// 已经不在录了（被外部提前叫停）：OnCSVProfileFinished 应该已经
				// 处理过了，bActive 会在下一轮被那个回调清掉，这里不用做别的
			}

			// 硬顶：EndCapture 之后 30 秒完成回调还没来，多半是引擎那边卡住了——
			// 与其让请求方永远等，不如报错让人重试，而不是无限期挂起一次 RPC
			if (GCsvSession.bEndRequested && Elapsed >= GCsvSession.DurationSeconds + 30.0)
			{
				const FString TimedOutRequestId = GCsvSession.RequestId;
				UAL_TeardownCsvSession();
				GCsvSession.Reset();
				UAL_CommandUtils::SendError(TimedOutRequestId, 500,
					TEXT("CSV capture did not finish writing within 30s after the requested duration. Check the log for CsvProfiler errors."));
				return false;
			}

			return true;
		}),
		0.1f);
}

// ============================================================================
// system.capture_insights_trace —— 触发一段 Unreal Insights trace，把 .utrace 文件路径交出来
// ============================================================================
//
// 这个命令只做**触发和确认**，不做**分析**。逐帧 CPU/GPU 事件的解析需要
// TraceAnalysis 那一整套库，量级和这个插件的其余部分完全不是一回事——
// 真正的分析在 TS 侧调用随引擎分发的 `UnrealInsights.exe` 无头模式完成
// （`-OpenTraceFile=... -NoUI -AutoQuit -ExecOnAnalysisCompleteCmd=...`），
// 这里只负责：开始录、等够时长、停止、确认文件真的落盘了。
//
// ## 为什么不用 `Trace.Start` / `Trace.Stop` 控制台命令
//
// 直接调 `FTraceAuxiliary::Start/Stop` 能拿到一件控制台命令拿不到的东西：
// **我们自己指定目标文件路径**。控制台命令不给路径时，文件落在哪由引擎自己
// 决定（默认 Trace Store，位置随版本和配置变化），调用方只能去猜或者扫目录。
// 直接指定路径，拿到的文件在哪由我们自己说了算。
//
// ## 为什么没有「完成回调」
//
// `FCsvProfiler` 有 `OnCSVProfileFinished` 委托，这里没有对应写法——因为
// `FTraceAuxiliary::OnTraceStopped` 是 **5.1+ 才加的静态成员**，5.0 上根本
// 不存在。同一个坑（`GetUsedTextures`、`LocateBoundObjects`）已经踩过两次：
// 签名对不上会编不过，比运行时悄悄错好——这次干脆不用需要版本分支的 API，
// 停止后等一段固定的宽限期（2 秒），文件系统层面确认落盘，9 个版本一致。

namespace
{
	struct FUAL_InsightsCaptureSession
	{
		bool bActive = false;
		bool bStopRequested = false;
		FString RequestId;
		FString TargetPath;
		FString Channels;
		double DurationSeconds = 0.0;
		double StartedAt = 0.0;
		FTSTicker::FDelegateHandle TickerHandle;

		void Reset()
		{
			bActive = false;
			bStopRequested = false;
			RequestId.Empty();
			TargetPath.Empty();
			Channels.Empty();
			DurationSeconds = 0.0;
			StartedAt = 0.0;
		}
	};

	FUAL_InsightsCaptureSession GInsightsSession;

	/** 停止后给文件系统的宽限期。EConnectionType::File 是流式写的普通文件，
	 * 不像 CSV Profiler 那样有一步压缩+整体落盘，宽限期不需要很长。 */
	constexpr double UAL_InsightsFlushGraceSeconds = 2.0;

	void UAL_TeardownInsightsSession()
	{
		if (GInsightsSession.TickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GInsightsSession.TickerHandle);
			GInsightsSession.TickerHandle.Reset();
		}
	}

	void UAL_FinishInsightsSession()
	{
		const FString RequestId = GInsightsSession.RequestId;
		const FString TargetPath = GInsightsSession.TargetPath;
		const FString Channels = GInsightsSession.Channels;
		const double Duration = GInsightsSession.DurationSeconds;
		const double Elapsed = FPlatformTime::Seconds() - GInsightsSession.StartedAt;

		UAL_TeardownInsightsSession();
		GInsightsSession.Reset();

		if (!IFileManager::Get().FileExists(*TargetPath))
		{
			UAL_CommandUtils::SendError(RequestId, 500, FString::Printf(
				TEXT("Trace was stopped but no file appeared at the expected path: %s. ")
				TEXT("Check the log for Trace/TraceLog errors."),
				*TargetPath));
			return;
		}

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("ok"), true);
		Data->SetStringField(TEXT("utrace_path"), TargetPath);
		Data->SetStringField(TEXT("channels"), Channels);
		Data->SetNumberField(TEXT("requested_duration_seconds"), Duration);
		Data->SetNumberField(TEXT("actual_elapsed_seconds"), FMath::RoundToDouble(Elapsed * 10.0) / 10.0);
		Data->SetStringField(TEXT("note"),
			TEXT("这是原始 trace 文件，本命令不解析它。要拿到数字，用 ue_analyze_insights_trace 跑一次无头分析，")
			TEXT("或者自己用 Unreal Insights 打开这个文件。"));

		UAL_CommandUtils::SendResponse(RequestId, 200, Data);
	}
}

void FUAL_SystemCommands::Handle_CaptureInsightsTrace(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (GInsightsSession.bActive)
	{
		UAL_CommandUtils::SendError(RequestId, 409,
			TEXT("An Insights capture triggered by this tool is already in progress. Wait for it to finish."));
		return;
	}

	double Duration = 10.0;
	Payload->TryGetNumberField(TEXT("duration_seconds"), Duration);
	// 和 CSV 采集一样硬顶 120 秒——这是「等一段时间收集数据」的工具，
	// 不是无限期录制
	Duration = FMath::Clamp(Duration, 1.0, 120.0);

	FString Channels = TEXT("cpu,gpu,frame,bookmark");
	Payload->TryGetStringField(TEXT("channels"), Channels);

	const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString TargetDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Profiling"), TEXT("UnrealAgentLink")));
	IFileManager::Get().MakeDirectory(*TargetDir, /*Tree=*/true);
	const FString TargetPath = FPaths::Combine(TargetDir, FString::Printf(TEXT("Capture_%s.utrace"), *Timestamp));

	// 不预先查「是不是已经在录了」——那要用 IsConnected()，5.0 上不存在
	// （见文件头）。直接尝试开始，用返回值本身判断：接口文档说已有连接时
	// 「什么都不做」，返回值就是我们需要的信号，不用额外查一次状态。
	const bool bStarted = FTraceAuxiliary::Start(FTraceAuxiliary::EConnectionType::File, *TargetPath, *Channels);
	if (!bStarted)
	{
		UAL_CommandUtils::SendError(RequestId, 409, TEXT(
			"Could not start the trace. A trace connection may already be active "
			"(started via console `Trace.Start`, command line `-trace=`, or another tool), "
			"or the destination path is not writable. Run `Trace.Stop` first if one is already running."));
		return;
	}

	GInsightsSession.Reset();
	GInsightsSession.bActive = true;
	GInsightsSession.RequestId = RequestId;
	GInsightsSession.TargetPath = TargetPath;
	GInsightsSession.Channels = Channels;
	GInsightsSession.DurationSeconds = Duration;
	GInsightsSession.StartedAt = FPlatformTime::Seconds();

	GInsightsSession.TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float /*DeltaTime*/) -> bool
		{
			if (!GInsightsSession.bActive)
			{
				return false;
			}

			const double Elapsed = FPlatformTime::Seconds() - GInsightsSession.StartedAt;

			if (!GInsightsSession.bStopRequested && Elapsed >= GInsightsSession.DurationSeconds)
			{
				GInsightsSession.bStopRequested = true;
				FTraceAuxiliary::Stop();
				return true; // 继续跳几下，等宽限期过去再确认文件、发响应
			}

			if (GInsightsSession.bStopRequested &&
				Elapsed >= GInsightsSession.DurationSeconds + UAL_InsightsFlushGraceSeconds)
			{
				UAL_FinishInsightsSession();
				return false;
			}

			return true;
		}),
		0.1f);
}
