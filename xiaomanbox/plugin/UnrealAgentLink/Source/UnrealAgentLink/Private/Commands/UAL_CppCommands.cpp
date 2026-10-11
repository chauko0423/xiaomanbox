#include "UAL_CppCommands.h"
#include "UAL_CommandUtils.h"

#include "Containers/Ticker.h"
#include "AddToProjectConfig.h"
#include "GameProjectUtils.h"
#include "HAL/FileManager.h"
#include "Misc/CompilationResult.h"
#include "Misc/FileHelper.h"
#include "Misc/HotReloadInterface.h"
#include "Misc/Paths.h"
#include "ModuleDescriptor.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

/*
 * Live Coding 只有 Windows 有（模块住在 Engine/Source/Developer/Windows/ 下），
 * 而且引擎自己的接法就是「只加 include 路径、不链接、运行时用 GetModulePtr 取」——
 * 见 UnrealEd.Build.cs / LevelEditor.Build.cs 里的
 *   if (Target.bWithLiveCoding) { PrivateIncludePathModuleNames.Add("LiveCoding"); }
 * 我们照抄，所以这里的调用点必须全部包在 WITH_LIVE_CODING 里。
 */
#if WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUALCpp, Log, All);

void FUAL_CppCommands::RegisterCommands(TMap<FString, FHandlerFunc>& CommandMap)
{
	CommandMap.Add(TEXT("cpp.probe"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_Probe(Payload, RequestId);
	});

	CommandMap.Add(TEXT("cpp.list_modules"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ListModules(Payload, RequestId);
	});

	CommandMap.Add(TEXT("cpp.compile"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_Compile(Payload, RequestId);
	});

	CommandMap.Add(TEXT("cpp.add_class"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_AddClass(Payload, RequestId);
	});
}

// ──────────────────────────── cpp.add_class ────────────────────────────

void FUAL_CppCommands::Handle_AddClass(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	/*
	 * 纯蓝图工程一律拒。
	 *
	 * 引擎的 AddCodeToProject 在 bProjectHadCodeFiles == false 且 Live Coding
	 * 启用时会 FMessageDialog::Open（GameProjectUtils.cpp:4116），那是个模态框，
	 * 会把游戏线程停在那儿等人点确定 —— 我们的命令也跑在游戏线程上，
	 * 于是编辑器和这次调用一起卡死，直到用户手动点掉。
	 *
	 * 引擎在 Live Coding 关着时不弹窗，所以这条一刀切**比引擎保守**。
	 * 那是有意的：区分两种情况等于把「会不会卡死编辑器」押在一次探测上，
	 * 而这个场景（工程从零加第一个 C++ 类）一辈子只发生一次，
	 * 用户手动做一次的成本远低于我们探测错一次。
	 */
	if (!GameProjectUtils::ProjectHasCodeFiles())
	{
		UAL_CommandUtils::SendError(RequestId, 400,
			TEXT("这是纯蓝图工程，还没有任何 C++ 代码。第一个 C++ 类需要用户在编辑器里手动加一次")
			TEXT("（菜单 工具 > 新建 C++ 类 / Tools > New C++ Class），之后我就能接手了。"));
		return;
	}

	FString ClassName;
	if (!Payload->TryGetStringField(TEXT("name"), ClassName) || ClassName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("缺少字段: name"));
		return;
	}

	FString ParentClass, ModuleName, Location;
	Payload->TryGetStringField(TEXT("parent_class"), ParentClass);
	Payload->TryGetStringField(TEXT("module"), ModuleName);
	Payload->TryGetStringField(TEXT("location"), Location);

	// ── 选模块 ──
	//
	// **注意这里取的是副本，不是引用。别改回去。**
	//
	// GetCurrentProjectModules() 返回的是 IProjectManager 内部那个数组的引用
	// （GameProjectUtils.cpp:2562 直接返回 GetCurrentProjectModuleContextInfos()）。
	// 而 AddCodeToProject 在生成文件之前会调 UpdateProject() 改写 .uproject
	// （给新类的父类补依赖模块，GameProjectUtils.cpp:4053）—— 那一步会把模块表
	// 清掉重建，**我们手上指向数组元素的指针当场悬空**。
	//
	// 之后引擎拿这个已经悬空的 ModuleInfo 去做路径校验和格式化错误消息，
	// 于是 2026-09-08 真机验收案例 5 看到的现象是：FailedToAddCode ＋
	// 一串每次都不一样的乱码理由（`“頀笠੏` / `“⿎ண` / `“怒偗ਛ`），
	// 一个文件都没建出来。那串乱码的第一个字符恒定是 `“`，正是
	// SourcePathInvalidForModule 这条消息的开头，只是两个参数都读到了野内存。
	//
	// 受控实验（2026-09-08，独立的空工程 _ual-test/AddClassProbe55）：
	// 别的都不动，只把传给 AddCodeToProject 的模块信息从引用换成副本，
	// 结果就从 FailedToAddCode 变成 Succeeded，两个文件正常生成。
	//
	// 这是**用后即释**，所以它有时会「碰巧成功」—— 别拿一次成功当它没问题。
	const TArray<FModuleContextInfo> Modules = GameProjectUtils::GetCurrentProjectModules();
	if (Modules.Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 500,
			TEXT("工程报告有 C++ 代码，但一个模块都列不出来。请让用户确认工程能否正常编译。"));
		return;
	}
	const FModuleContextInfo* Module = nullptr;
	if (ModuleName.IsEmpty())
	{
		// 不给就用第一个 —— 那是工程的主游戏模块
		Module = &Modules[0];
	}
	else
	{
		Module = Modules.FindByPredicate([&ModuleName](const FModuleContextInfo& M)
		{
			return M.ModuleName.Equals(ModuleName, ESearchCase::IgnoreCase);
		});
	}
	if (!Module)
	{
		FString Available;
		for (const FModuleContextInfo& M : Modules)
		{
			Available += (Available.IsEmpty() ? TEXT("") : TEXT("、")) + M.ModuleName;
		}
		UAL_CommandUtils::SendError(RequestId, 400,
			FString::Printf(TEXT("找不到模块 %s。这个工程有：%s"), *ModuleName, *Available));
		return;
	}

	/*
	 * DisallowedHeaderNames 传空集。
	 *
	 * 引擎的对话框会把模块里已有的头文件名塞进来，用于提前发现重名。我们不扫 ——
	 * 重名时 AddCodeToProject 自己会因为文件已存在而失败，只是报错措辞差一点。
	 * ponytail: 为了一句更漂亮的错误去扫一遍源码目录不划算，真嫌错误看不懂再补。
	 */
	const TSet<FString> DisallowedHeaderNames;

	FText FailReason;
	if (!GameProjectUtils::IsValidClassNameForCreation(ClassName, *Module, DisallowedHeaderNames, FailReason))
	{
		UAL_CommandUtils::SendError(RequestId, 400,
			FString::Printf(TEXT("类名 %s 不合法：%s"), *ClassName, *FailReason.ToString()));
		return;
	}

	// ── 父类 ──
	FNewClassInfo ClassInfo(FNewClassInfo::EClassType::EmptyCpp);
	if (!ParentClass.IsEmpty())
	{
		FString ResolveError;
		// 复用已有的解析器：它认路径形式、认不带 U/A 前缀的名字，
		// 而且 5.3 前后的 FindObject/FindFirstObject 差异也在里面处理过了
		UClass* Base = UAL_CommandUtils::ResolveClassFromIdentifier(ParentClass, nullptr, ResolveError);
		if (!Base)
		{
			UAL_CommandUtils::SendError(RequestId, 400,
				FString::Printf(TEXT("找不到父类 %s：%s"), *ParentClass, *ResolveError));
			return;
		}
		if (!GameProjectUtils::IsValidBaseClassForCreation(Base, *Module))
		{
			UAL_CommandUtils::SendError(RequestId, 400,
				FString::Printf(TEXT("%s 不能作为父类（模块 %s 够不着它，或者它被标记为不可继承）。"),
					*Base->GetName(), *Module->ModuleName));
			return;
		}
		ClassInfo = FNewClassInfo(Base);
	}

	// ── 落点 ──
	//
	// 默认放模块根目录 —— 那是引擎的 Add C++ Class 对游戏模块的默认行为。
	// 显式要 public/private 才进子目录（插件模块和要被别的模块 include 的类需要）
	FString NewClassPath = Module->ModuleSourcePath;
	if (Location.Equals(TEXT("public"), ESearchCase::IgnoreCase))
	{
		NewClassPath = Module->ModuleSourcePath / TEXT("Public");
	}
	else if (Location.Equals(TEXT("private"), ESearchCase::IgnoreCase))
	{
		NewClassPath = Module->ModuleSourcePath / TEXT("Private");
	}

	FString HeaderPath, CppPath;
	GameProjectUtils::EReloadStatus ReloadStatus = GameProjectUtils::EReloadStatus::NotReloaded;
	const GameProjectUtils::EAddCodeToProjectResult Result = GameProjectUtils::AddCodeToProject(
		ClassName, NewClassPath, *Module, ClassInfo, DisallowedHeaderNames,
		HeaderPath, CppPath, FailReason, ReloadStatus);

	const TCHAR* ResultName = TEXT("Unknown");
	switch (Result)
	{
	case GameProjectUtils::EAddCodeToProjectResult::Succeeded:       ResultName = TEXT("Succeeded"); break;
	case GameProjectUtils::EAddCodeToProjectResult::InvalidInput:    ResultName = TEXT("InvalidInput"); break;
	case GameProjectUtils::EAddCodeToProjectResult::FailedToAddCode: ResultName = TEXT("FailedToAddCode"); break;
	case GameProjectUtils::EAddCodeToProjectResult::FailedToHotReload: ResultName = TEXT("FailedToHotReload"); break;
	}

	// 和 cpp.compile 同一条教训：非 200 会被盒子标成 ok:false，
	// 整个载荷被 callUe 吞掉，只剩一句「未提供失败原因」。
	// 建类跑完了就是 RPC 成功，成没成写在 result 里
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("result"), ResultName);
	Data->SetStringField(TEXT("module"), Module->ModuleName);
	Data->SetStringField(TEXT("header_path"), HeaderPath);
	Data->SetStringField(TEXT("cpp_path"), CppPath);
	Data->SetBoolField(TEXT("reloaded"), ReloadStatus == GameProjectUtils::EReloadStatus::Reloaded);
	Data->SetStringField(TEXT("fail_reason"), FailReason.ToString());

	UE_LOG(LogUALCpp, Log, TEXT("cpp.add_class %s -> %s"), *ClassName, ResultName);
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ───────────────────────────── cpp.compile ─────────────────────────────

namespace
{

/** 编译最多等多久。超时后如实说「不知道成没成」，不报成功 */
constexpr double kCompileTimeoutSeconds = 15.0 * 60.0;

/**
 * Live Coding 说「不编了」之后，再给结果日志多少秒才认输。
 *
 * 引擎清标志和打印结果不在同一个线程、也不在同一帧（见 FCompileJob::StoppedCompilingAt
 * 那段注释）。5 秒对一次帧回调是天文数字，而代价只是极少数真·Unknown 的场景多等 5 秒。
 */
constexpr double kResultLogGraceSeconds = 5.0;

/**
 * 引擎眼里「热重载得动」的模块有几个。
 *
 * 判据抄 DoHotReloadFromEditor 自己那两步（HotReload.cpp:761 → :517）：
 * 先取所有**已加载的游戏模块**，再看有没有对应的 `/Script/<模块名>` 包。
 *
 * 关键在第二步：**那个包只有在模块里声明过 UObject（UCLASS / USTRUCT / UENUM）
 * 时才存在。** 一个只有普通 C++ 类和函数的游戏模块，这里数出来就是 0，
 * 而引擎会在 13 毫秒内同步拒绝，一行都不编。
 *
 * 2026-09-08 真机验收案例 2 就翻在这里：测试工程的游戏模块一个 UObject 都没有，
 * 编辑器日志里只留下一句 `RebindPackages not possible (no packages specified)`，
 * 然后工具等一个永远不会广播的完成事件等到超时。
 */
int32 CountHotReloadablePackages()
{
	TArray<FModuleStatus> Statuses;
	FModuleManager::Get().QueryModules(Statuses);

	int32 Count = 0;
	for (const FModuleStatus& Status : Statuses)
	{
		if (!Status.bIsLoaded || !Status.bIsGameModule)
		{
			continue;
		}
		if (FindPackage(nullptr, *(TEXT("/Script/") + Status.Name)) != nullptr)
		{
			++Count;
		}
	}
	return Count;
}

/**
 * Live Coding 处在「已经关掉勾选，但会话还在这个进程里跑」的半吊子状态吗？
 *
 * 判据是两个状态位的组合，不是任何一个单独的位：
 *   HasStarted() == true       控制台进程还在（EState::Running 也算 started）
 *   IsEnabledForSession() == false   这个会话不再用它编译
 *
 * 用户是这么走进来的：在编辑器偏好设置里把 Enable Live Coding 取消勾选。
 * 引擎当场只做了 HideConsole()（LiveCodingModule.cpp:690-697），并且自己在日志里
 * 说得很清楚：
 *
 *   "Console will be hidden but remain running in the background.
 *    Restart to disable completely."
 *
 * **这个状态下热重载会把编辑器卡死。** 2026-09-08 真机验收案例 9 实测：
 * cpp.probe 报 hotreload（两个引擎守卫只看 IsEnabledForSession，都放行了），
 * 然后 cpp.compile 一发出去，游戏线程 20 分钟不动一帧，
 * 没有任何编译进程起来，编辑器点什么都没反应，最后只能关掉。
 *
 * 所以这里必须**当场拦住**：把 20 分钟的假死换成一句「请重启编辑器」。
 * 我原来在验收单里推断「关掉不重启也能编」，依据是那两个守卫的判据 ——
 * 推断错在只看了守卫放不放行，没看放行之后那条路还走不走得通。
 */
bool LiveCodingHalfDisabled()
{
#if WITH_LIVE_CODING
	if (ILiveCodingModule* Lc = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME))
	{
		return Lc->HasStarted() && !Lc->IsEnabledForSession();
	}
#endif
	return false;
}

/** 半吊子状态下给用户的那句话。探测和编译两处都要用 */
FString LiveCodingHalfDisabledHint()
{
	return TEXT("这个编辑器会话处在一个「关了但没关干净」的状态：Live Coding 的勾选被取消了，")
		TEXT("但它的会话还在这个进程里跑着（引擎自己在日志里说的：Console will be hidden ")
		TEXT("but remain running in the background. Restart to disable completely）。")
		TEXT("**这个状态下走热重载会把编辑器卡死**（实测：20 分钟不动一帧，只能强制关闭）。")
		TEXT("请让用户重启编辑器 —— 重启之后热重载就正常了，而且编不过时能拿到完整的文件名和行号。");
}

/** 热重载编不动时给用户的那句话。两处要用（探测和编译），所以抽出来 */
FString NoReloadablePackagesHint()
{
	return TEXT("这个工程当前加载的游戏模块里，没有任何一个声明过 UObject 类")
		TEXT("（UCLASS / USTRUCT / UENUM）。引擎的热重载是按 UObject 包来重新绑定的，")
		TEXT("模块里没有 UObject 就没有可绑定的包，引擎会直接拒绝，一行都不会编。")
		TEXT("解法：在这个模块里至少放一个 UCLASS（比如一个继承 AActor 的类），")
		TEXT("或者关掉编辑器用 IDE / 命令行完整编一次。");
}

/**
 * 只收 LogLiveCoding 的一次性输出设备。
 *
 * Live Coding 的结果**没有任何接口能查**：`OnPatchCompleteDelegate` 只在成功时
 * 广播（LiveCodingModule.cpp:898），`LastResults` 也没有 getter。
 * 唯一的出口是它自己 UE_LOG 出来的那四条固定文案，所以只能在这里捞。
 *
 * 只留 LogLiveCoding 一个类别 —— 编译期间引擎会刷大量别的日志，全存下来
 * 既占内存又要在后面过滤一遍。
 */
class FLiveCodingLogSink : public FOutputDevice
{
public:
	TArray<FString> Lines;

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type, const FName& Category) override
	{
		static const FName LiveCodingCategory(TEXT("LogLiveCoding"));
		if (Category == LiveCodingCategory)
		{
			Lines.Add(FString(V));
		}
	}
};

/** 回给盒子的 UBT 日志最多多长。一次失败的 Log.txt 通常二三十 KB，报错在尾部 */
constexpr int32 kMaxBuildLogChars = 256 * 1024;

/**
 * 读 Live Coding 这次编译的 UBT 日志。找不到或者是旧的就返回 false。
 *
 * Live Coding 控制台窗口里那些编译器报错，是它拉起的 UnrealBuildTool 的输出，
 * **UBT 自己会把同样的内容写进 Log.txt**（2026-09-29 在 5.8 上对过：窗口里的
 * error C2065 在 %LOCALAPPDATA%/UnrealBuildTool/Log.txt 里逐行都有）。
 * 编辑器这边的 LogLiveCoding 只有一句「失败了，去看控制台」，所以只能来这里读。
 *
 * 两个候选位置：安装版引擎写 LocalAppData，源码版 / 老引擎写 Engine/Programs 下。
 * 取**比这次编译开始时间新**的那一份 —— 同机别的 UBT（IDE 编译、打包）也写这个文件，
 * 读到上一次的旧日志会把别人的报错安到这次头上，那比读不到还糟。
 */
bool ReadUbtLogSince(const FDateTime& SinceUtc, FString& OutLog, FString& OutPath)
{
	TArray<FString> Candidates;
	const FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
	if (!LocalAppData.IsEmpty())
	{
		Candidates.Add(LocalAppData / TEXT("UnrealBuildTool/Log.txt"));
	}
	Candidates.Add(FPaths::EngineDir() / TEXT("Programs/UnrealBuildTool/Log.txt"));

	IFileManager& FM = IFileManager::Get();
	FDateTime Newest = FDateTime::MinValue();
	for (const FString& Candidate : Candidates)
	{
		const FDateTime Stamp = FM.GetTimeStamp(*Candidate);
		// 文件系统时间精度有限，放两秒余量
		if (Stamp > Newest && Stamp >= SinceUtc - FTimespan::FromSeconds(2))
		{
			Newest = Stamp;
			OutPath = FPaths::ConvertRelativePathToFull(Candidate);
		}
	}
	if (Newest == FDateTime::MinValue()) return false;

	// AllowWrite：万一 UBT 还没松手，别因为共享模式打不开
	if (!FFileHelper::LoadFileToString(OutLog, *OutPath, FFileHelper::EHashOptions::None, FILEREAD_AllowWrite))
	{
		return false;
	}
	if (OutLog.Len() > kMaxBuildLogChars)
	{
		OutLog = OutLog.Right(kMaxBuildLogChars);
	}
	return true;
}

/** 一次编译的在途状态。同一时间只允许一个，插件端和工具的 sequential 各挡一道 */
struct FCompileJob
{
	FString RequestId;
	FString Path;
	double StartedAt = 0.0;
	/** 和 StartedAt 同一刻，但用来跟文件时间戳比（那是 UTC 墙钟，不是单调时钟） */
	FDateTime StartedAtUtc;
	FDelegateHandle CompilerFinished;
	FTSTicker::FDelegateHandle Ticker;
	TUniquePtr<FLiveCodingLogSink> Sink;

	/**
	 * Live Coding 说「不编了」的那一刻。0 表示还在编。
	 *
	 * 存它是因为**「编译结束」和「结果日志」不是同一件事**：
	 * `LiveCodingPostCompile()` 在客户端命令线程上先把 GIsCompileActive 清掉
	 * （LiveCodingModule.cpp:1623），四条结果文案要等到游戏线程的
	 * `FLiveCodingModule::Tick()` 才打印（:933）。
	 * 我们轮询的 IsCompiling() 就是那个标志，所以它一变假就收工的话，
	 * 会跑在引擎打印之前，捞到一份空日志。
	 */
	double StoppedCompilingAt = 0.0;
};

TUniquePtr<FCompileJob> GJob;

/** 摘钩子。必须在发响应**之前**做，否则超时那条路会和真正的完成事件抢着回 */
void ClearJob()
{
	if (!GJob) return;

	if (GJob->Ticker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GJob->Ticker);
	}
	if (GJob->CompilerFinished.IsValid() && FModuleManager::Get().IsModuleLoaded(TEXT("HotReload")))
	{
		FModuleManager::LoadModuleChecked<IHotReloadInterface>(TEXT("HotReload"))
			.OnModuleCompilerFinished().Remove(GJob->CompilerFinished);
	}
	if (GJob->Sink && GLog)
	{
		GLog->RemoveOutputDevice(GJob->Sink.Get());
	}
	GJob.Reset();
}

/**
 * 发编译结果。**永远用 200。**
 *
 * 真机第一次跑就栽在这上面：编译如期失败，我按「失败就回 500」发了响应，
 * 结果模型收到的是一句
 *
 *     cpp.compile 失败：未提供失败原因（错误码 500）
 *
 * ——result、output、needs_full_rebuild 全没了。
 *
 * 原因在盒子这一侧：`services/websocket/server.ts` 见到 `code >= 400` 就往
 * 载荷里注入 `ok:false`，而 `defineUeTool` 的 `callUe` 见到 `ok === false`
 * 就直接抛异常，工具的 `toOutcome` 根本不会执行。也就是说
 * **非 200 会把整个业务载荷吞掉，只剩一句通用错误。**
 *
 * 所以这里的分界不是「编译成没成」，而是「这次 RPC 有没有完成它的工作」：
 * 编译跑完了并且我知道结果 = RPC 成功 = 200，好坏写在 `result` 字段里。
 * 只有真正的 RPC 级失败（没有 C++、已经在编了）才用 SendError 走 4xx，
 * 那条路本来就带 message，不会出现「未提供失败原因」。
 */
void RespondAndClear(const TSharedPtr<FJsonObject>& Data)
{
	constexpr int32 Code = 200;
	if (!GJob) return;
	const FString RequestId = GJob->RequestId;
	Data->SetStringField(TEXT("path"), GJob->Path);
	Data->SetNumberField(TEXT("duration_ms"),
		FMath::RoundToInt((FPlatformTime::Seconds() - GJob->StartedAt) * 1000.0));
	ClearJob();
	UAL_CommandUtils::SendResponse(RequestId, Code, Data);
}

/**
 * 改了 .Build.cs / .uproject 之后，热补丁和热重载都可能给出一个「成功」而实际
 * 没把新依赖链进去 —— 那正是这个项目最怕的假成功。
 *
 * 判法故意不存状态：比**磁盘上最新的 .Build.cs / .uproject** 和
 * **工程 Binaries 里最新的 .dll**。产物比配置旧，就说明配置改过还没完整重编。
 * 不记「上次编译时间」是因为那份状态跨不了重启，也管不住用户在 VS 里的改动。
 *
 * ponytail: 只报 needs_full_rebuild 不拦截。拦截要求判断永远正确，而误拦一次
 * 就是把一次合法编译堵死；如实说出来的代价小得多。真出现误报再收紧。
 */
bool BuildConfigNewerThanBinaries()
{
	IFileManager& FM = IFileManager::Get();

	/*
	 * 直接比 FDateTime，不转时间戳数字。
	 *
	 * 初版写的是 `FMath::Max(Newest, Stamp.ToUnixTimestampDecimal())` ——
	 * 那个方法 **5.3 才有**，5.0/5.1/5.2 上直接编不过（真机出全套包时炸了三个版本）。
	 * 而这里根本不需要数字：FDateTime 自己就能比大小，换成比较更短也更没有版本风险。
	 *
	 * 这就是 说的那件事的实例 ——
	 * 跨九个版本的正确性不是靠写的时候小心，是靠编译器在发版前告诉我们。
	 */
	auto NewestTimeStamp = [&FM](const TArray<FString>& Files)
	{
		FDateTime Newest = FDateTime::MinValue();
		for (const FString& File : Files)
		{
			const FDateTime Stamp = FM.GetTimeStamp(*File);
			if (Stamp > Newest) Newest = Stamp;
		}
		return Newest;
	};

	TArray<FString> ConfigFiles;
	FM.FindFilesRecursive(ConfigFiles, *FPaths::GameSourceDir(), TEXT("*.Build.cs"), true, false);
	ConfigFiles.Add(FPaths::GetProjectFilePath());
	const FDateTime NewestConfig = NewestTimeStamp(ConfigFiles);
	if (NewestConfig == FDateTime::MinValue()) return false;

	TArray<FString> Binaries;
	FM.FindFilesRecursive(Binaries, *(FPaths::ProjectDir() / TEXT("Binaries")), TEXT("*.dll"), true, false);
	const FDateTime NewestBinary = NewestTimeStamp(Binaries);
	if (NewestBinary == FDateTime::MinValue()) return false;

	return NewestConfig > NewestBinary;
}

} // namespace

void FUAL_CppCommands::Handle_Compile(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (GJob)
	{
		UAL_CommandUtils::SendError(RequestId, 409, TEXT("已经有一次编译在进行中。"));
		return;
	}
	if (!GameProjectUtils::ProjectHasCodeFiles())
	{
		UAL_CommandUtils::SendError(RequestId, 400,
			TEXT("这个工程没有 C++ 源码，没有可编译的东西。先用 cpp.probe 看一眼。"));
		return;
	}

	/*
	 * 半吊子状态直接拒，别开工。
	 *
	 * 这条必须在选路**之前**：那时候 IsEnabledForSession() 已经是假了，
	 * 按常规判据会走热重载 —— 而那条路在这个状态下会把编辑器卡死 20 分钟。
	 * 拒绝的代价是一句话，走进去的代价是用户强制关编辑器。见 LiveCodingHalfDisabled()。
	 */
	if (LiveCodingHalfDisabled())
	{
		UAL_CommandUtils::SendError(RequestId, 409, LiveCodingHalfDisabledHint());
		return;
	}

	bool bLiveCoding = false;
#if WITH_LIVE_CODING
	ILiveCodingModule* Lc = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	// 只认 IsEnabledForSession —— 判据见 Handle_Probe 里那段长注释。
	// 判错的代价不对称：错走 Live Coding 会把用户的会话永久切过去
	bLiveCoding = (Lc != nullptr) && Lc->IsEnabledForSession();
#endif

	GJob = MakeUnique<FCompileJob>();
	GJob->RequestId = RequestId;
	GJob->Path = bLiveCoding ? TEXT("livecoding") : TEXT("hotreload");
	GJob->StartedAt = FPlatformTime::Seconds();
	GJob->StartedAtUtc = FDateTime::UtcNow();

	const bool bNeedsFullRebuild = BuildConfigNewerThanBinaries();

	// 超时兜底。轮询和委托都可能永远不到达（编辑器崩了、Live Coding 控制台被杀），
	// 那时候必须说「不知道成没成」而不是让调用方挂到自己超时
	GJob->Ticker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([bNeedsFullRebuild](float) -> bool
		{
			if (!GJob) return false;

			bool bDone = false;
			FString Result;
			FString Output;

#if WITH_LIVE_CODING
			if (GJob->Path == TEXT("livecoding"))
			{
				ILiveCodingModule* Module =
					FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
				if (!Module || !Module->IsCompiling())
				{
					Output = FString::Join(GJob->Sink->Lines, TEXT("\n"));
					// 引擎的四条固定文案（LiveCodingModule.cpp:937-950）。
					// 2026-09-03 在 5.0–5.8 九个引擎上逐版本 grep 过，字面量逐字相同
					// （5.8 换成 UE_LOGF，输出文本没变）。。
					//
					// 顺序有讲究，**别整理**：Success 分支带 re-instancing 时是
					// "Live coding succeeded, data type changes..."（Warning 级），
					// NoChanges 是 "Live coding succeeded, no code changes detected" ——
					// 两条都含 "succeeded"。先判 NoChanges，否则「没改动」会被报成
					// 「编译成功」，模型就会以为它的改动生效了。
					if (Output.Contains(TEXT("no code changes detected"))) Result = TEXT("NoChanges");
					else if (Output.Contains(TEXT("Live coding succeeded"))) Result = TEXT("Success");
					else if (Output.Contains(TEXT("Live coding canceled"))) Result = TEXT("Cancelled");
					else if (Output.Contains(TEXT("Live coding failed"))) Result = TEXT("Failure");

					/*
					 * **「不编了」不等于「结果已经打印了」——中间要留一个宽限窗口。**
					 *
					 * 引擎把这两件事拆在了两个线程上：
					 *   LiveCodingPostCompile()（客户端命令线程）先清 GIsCompileActive（:1623），
					 *   四条结果文案要等游戏线程的 FLiveCodingModule::Tick() 才打印（:933）。
					 * 而 IsCompiling() 读的就是那个标志。所以标志一变假就下结论，
					 * 有概率跑在打印前面，捞到一份空日志。
					 *
					 * 2026-09-08 真机验收案例 7 就是这么翻的：Live Coding 真的编译失败了，
					 * 我们回的是 Unknown 而不是 Failure。案例 6（成功）当时侥幸没翻 ——
					 * **同一个竞态，只是那次我们跑输了，反而拿到了对的结果。**
					 *
					 * 所以：匹配上任何一条就立刻收工；一条都没匹配上时**再等一会儿**，
					 * 等满 kResultLogGraceSeconds 还是空的，才承认 Unknown。
					 * 宁可多等两秒，也不要把一次真实的失败说成「不知道」。
					 */
					if (!Result.IsEmpty())
					{
						bDone = true;
					}
					else
					{
						if (GJob->StoppedCompilingAt == 0.0)
						{
							GJob->StoppedCompilingAt = FPlatformTime::Seconds();
						}
						else if (FPlatformTime::Seconds() - GJob->StoppedCompilingAt > kResultLogGraceSeconds)
						{
							bDone = true;
							Result = TEXT("Unknown");
						}
					}
				}
			}
#endif

			if (!bDone && FPlatformTime::Seconds() - GJob->StartedAt > kCompileTimeoutSeconds)
			{
				bDone = true;
				Result = TEXT("Timeout");
			}
			if (!bDone) return true;

			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("result"), Result);
			Data->SetStringField(TEXT("output"), Output);
			Data->SetBoolField(TEXT("needs_full_rebuild"), bNeedsFullRebuild);

			// 编不过（或没确认上）时把 UBT 日志带回去，解析在 TS 侧做，和热重载那条路一样
			if (GJob->Path == TEXT("livecoding") && (Result == TEXT("Failure") || Result == TEXT("Unknown")))
			{
				FString BuildLog, BuildLogPath;
				if (ReadUbtLogSince(GJob->StartedAtUtc, BuildLog, BuildLogPath))
				{
					Data->SetStringField(TEXT("build_log"), BuildLog);
					Data->SetStringField(TEXT("build_log_path"), BuildLogPath);
				}
			}

			RespondAndClear(Data);
			return false;
		}),
		0.5f);

	if (bLiveCoding)
	{
#if WITH_LIVE_CODING
		GJob->Sink = MakeUnique<FLiveCodingLogSink>();
		GLog->AddOutputDevice(GJob->Sink.Get());

		ELiveCodingCompileResult Kick = ELiveCodingCompileResult::Failure;
		// 不传 WaitForCompletion —— 那会在游戏线程上死等，编辑器整个卡住
		Lc->Compile(ELiveCodingCompileFlags::None, &Kick);
		if (Kick != ELiveCodingCompileResult::InProgress)
		{
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("result"),
				Kick == ELiveCodingCompileResult::CompileStillActive ? TEXT("CompileStillActive") : TEXT("NotStarted"));
			Data->SetStringField(TEXT("output"), TEXT(""));
			Data->SetStringField(TEXT("reason"),
				TEXT("Live Coding 没能启动这次编译，一行都没编。多半是它的控制台进程起不来")
				TEXT("（引擎设置里的 Live Coding 控制台路径不对，或者进程被杀了）。"));
			Data->SetBoolField(TEXT("needs_full_rebuild"), bNeedsFullRebuild);
			RespondAndClear(Data);
		}
#endif
		return;
	}

	// ── 热重载 ──
	//
	// 用 DoHotReloadFromEditor 而不是 RecompileModule：后者内部 MakeDialog() +
	// WaitForCompletion，在游戏线程上同步等 UBT 跑完（HotReload.cpp:661/690），
	// 编辑器会整个转圈。异步这条路的完成事件还顺带把 UBT 的完整输出带回来。
	IHotReloadInterface& Hr = FModuleManager::LoadModuleChecked<IHotReloadInterface>(TEXT("HotReload"));
	GJob->CompilerFinished = Hr.OnModuleCompilerFinished().AddLambda(
		[bNeedsFullRebuild](const FString& FinalOutput, ECompilationResult::Type CompileResult, bool)
		{
			if (!GJob || GJob->Path != TEXT("hotreload")) return;

			const bool bOk = !ECompilationResult::Failed(CompileResult);
			// UpToDate = UBT 判定没有要编的东西，一行都没编。和 Live Coding 那条路一样报 NoChanges，
			// 否则「改了代码却没进编译」会被说成编译成功，调用方就去验一个根本没生效的改动
			const TCHAR* ResultName = CompileResult == ECompilationResult::UpToDate
				? TEXT("NoChanges")
				: (bOk ? TEXT("Success") : TEXT("Failure"));
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("result"), ResultName);
			// UBT 的完整 stdout。诊断解析在 TS 侧做 —— 那是纯字符串处理，
			// 放在能跑单测的地方比放在要出九个包才能验的地方划算
			Data->SetStringField(TEXT("output"), FinalOutput);
			Data->SetNumberField(TEXT("compilation_result"), static_cast<int32>(CompileResult));
			Data->SetBoolField(TEXT("needs_full_rebuild"), bNeedsFullRebuild);
			RespondAndClear(Data);
		});

	/*
	 * **返回值必须看。**
	 *
	 * 异步这条路上，只有 ECompilationResult::Succeeded 代表「编译已经启动了」
	 * （HotReload.cpp:1041，那之前每一个提前 return 都是「什么都没做」）。
	 * 没启动的话 OnModuleCompilerFinished 永远不会广播 ——
	 * 不在这里收口，工具就会一直等到 20 分钟超时。
	 *
	 * 2026-09-08 真机验收案例 2 翻的就是这一条：引擎 13 毫秒同步拒绝，
	 * 日志里只有一句 RebindPackages not possible，而工具在那儿转圈。
	 * 这正是 §8.2 第 2 条要防的「永远不回响应」。
	 */
	const ECompilationResult::Type Kick = Hr.DoHotReloadFromEditor(EHotReloadFlags::None);
	if (Kick == ECompilationResult::Succeeded)
	{
		return; // 编译起来了，等委托
	}

	FString Reason;
	if (CountHotReloadablePackages() == 0)
	{
		Reason = NoReloadablePackagesHint();
	}
	else
	{
		// 别的拒绝原因（已经在编、包没绑定、编译器起不来）引擎只写日志不给理由码，
		// 如实报「引擎拒绝了，没说为什么」，让人去看编辑器日志，别猜
		Reason = FString::Printf(
			TEXT("引擎拒绝了这次热重载，一行都没编（ECompilationResult=%d）。")
			TEXT("引擎没有给出结构化的原因，具体那一行警告在编辑器的输出日志里，")
			TEXT("搜 RebindPackages 或 HotReload 就能看到。"),
			static_cast<int32>(Kick));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("result"), TEXT("NotStarted"));
	Data->SetStringField(TEXT("output"), TEXT(""));
	Data->SetStringField(TEXT("reason"), Reason);
	Data->SetNumberField(TEXT("compilation_result"), static_cast<int32>(Kick));
	Data->SetBoolField(TEXT("needs_full_rebuild"), bNeedsFullRebuild);
	RespondAndClear(Data);
}

/**
 * 把一组模块序列化成 JSON 数组。
 *
 * `source_path` 给的是模块 Source 目录的绝对路径 —— 调用方（盒子里的 agent）
 * 拿它去 grep/读文件，相对路径在那边一定拼错：引擎的相对路径相对的是
 * 引擎二进制目录，不是任何调用方的工作目录。
 */
static void AppendModules(
	TArray<TSharedPtr<FJsonValue>>& Out,
	const TArray<FModuleContextInfo>& Modules,
	const TCHAR* Origin)
{
	for (const FModuleContextInfo& Module : Modules)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Module.ModuleName);
		Item->SetStringField(TEXT("source_path"),
			FPaths::ConvertRelativePathToFull(Module.ModuleSourcePath));
		// Runtime / Editor / Developer / Program …
		// 类型不是装饰：Runtime 模块 include UnrealEd 会在打包时炸，而编辑器里编得过去
		Item->SetStringField(TEXT("type"), EHostType::ToString(Module.ModuleType));
		Item->SetStringField(TEXT("origin"), Origin);
		Out.Add(MakeShared<FJsonValueObject>(Item));
	}
}

void FUAL_CppCommands::Handle_ListModules(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	bool bIncludePlugins = false;
	if (Payload.IsValid())
	{
		Payload->TryGetBoolField(TEXT("include_plugins"), bIncludePlugins);
	}

	TArray<TSharedPtr<FJsonValue>> Modules;
	AppendModules(Modules, GameProjectUtils::GetCurrentProjectModules(), TEXT("project"));
	if (bIncludePlugins)
	{
		AppendModules(Modules, GameProjectUtils::GetCurrentProjectPluginModules(), TEXT("plugin"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("modules"), Modules);
	Data->SetNumberField(TEXT("count"), Modules.Num());
	Data->SetBoolField(TEXT("has_code"), GameProjectUtils::ProjectHasCodeFiles());

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_CppCommands::Handle_Probe(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	const bool bHasCode = GameProjectUtils::ProjectHasCodeFiles();
	Data->SetBoolField(TEXT("has_code"), bHasCode);

	/*
	 * 声明了几个模块。**这个数和 has_code 会不一致，而且不一致是有意义的。**
	 *
	 * 真机上撞到过（2026-09-03，一个叫 ProbeHost 的宿主工程）：
	 *   has_code = false，但 GetCurrentProjectModules() 返回了一个模块。
	 *
	 * 两者的数据源不同 —— ProjectHasCodeFiles() 数的是**磁盘上的源文件**，
	 * GetCurrentProjectModules() 读的是 **.uproject 里的 Modules 段**。
	 * 所以「.uproject 声明了 MyGame 模块，但 Source/ 目录根本不存在」是个
	 * 能稳定出现的状态：模板生成到一半、源码被删过、或者工程是从别处拷来的。
	 *
	 * 只报 has_code 的话，这种工程会被说成「纯蓝图工程」——那是错的，
	 * 而且会把用户引向「手动加第一个 C++ 类」，可他的问题是工程本身坏了。
	 */
	const int32 DeclaredModuleCount = GameProjectUtils::GetCurrentProjectModules().Num();
	Data->SetNumberField(TEXT("declared_module_count"), DeclaredModuleCount);
	Data->SetStringField(TEXT("engine_dir"), FPaths::ConvertRelativePathToFull(FPaths::EngineDir()));
	Data->SetStringField(TEXT("engine_source_dir"), FPaths::ConvertRelativePathToFull(FPaths::EngineSourceDir()));
	Data->SetStringField(TEXT("project_source_dir"), FPaths::ConvertRelativePathToFull(FPaths::GameSourceDir()));

	// ── Live Coding ────────────────────────────────────────────────────────
	//
	// 这四个状态位长得很像，但只有 enabled_for_session 能用来选路，理由见下面
	// compile_path 那段。全都报出来是为了让「为什么走不了这条路」有据可查。
	TSharedPtr<FJsonObject> LiveCoding = MakeShared<FJsonObject>();
	bool bLcAvailable = false;
	bool bLcEnabledForSession = false;

#if WITH_LIVE_CODING
	if (ILiveCodingModule* Lc = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME))
	{
		bLcAvailable = true;
		bLcEnabledForSession = Lc->IsEnabledForSession();
		LiveCoding->SetBoolField(TEXT("started"), Lc->HasStarted());
		LiveCoding->SetBoolField(TEXT("enabled_by_default"), Lc->IsEnabledByDefault());
		LiveCoding->SetBoolField(TEXT("enabled_for_session"), bLcEnabledForSession);
		LiveCoding->SetBoolField(TEXT("can_enable"), Lc->CanEnableForSession());
		LiveCoding->SetBoolField(TEXT("is_compiling"), Lc->IsCompiling());
		LiveCoding->SetStringField(TEXT("enable_error"), Lc->GetEnableErrorText().ToString());
	}
#endif

	LiveCoding->SetBoolField(TEXT("available"), bLcAvailable);
	if (!bLcAvailable)
	{
		// 非 Windows（Target.bWithLiveCoding 为假）或模块没加载。
		// 说清楚是「这台机器没有」而不是「查询失败」——后者会让模型去重试。
		LiveCoding->SetStringField(TEXT("unavailable_reason"),
			TEXT("这个平台或这份引擎构建没有 Live Coding 模块（它只在 Windows 上有）。"));
	}
	Data->SetObjectField(TEXT("live_coding"), LiveCoding);

	// ── HotReload ──────────────────────────────────────────────────────────
	TSharedPtr<FJsonObject> HotReload = MakeShared<FJsonObject>();
	bool bHotReloadUsable = false;
	if (FModuleManager::Get().IsModuleLoaded(TEXT("HotReload")))
	{
		IHotReloadInterface& Hr = FModuleManager::LoadModuleChecked<IHotReloadInterface>(TEXT("HotReload"));
		bHotReloadUsable = true;
		HotReload->SetBoolField(TEXT("any_game_module_loaded"), Hr.IsAnyGameModuleLoaded());
		HotReload->SetBoolField(TEXT("is_compiling"), Hr.IsCurrentlyCompiling());
		/*
		 * **`any_game_module_loaded` 为真不代表热重载编得动。**
		 *
		 * 引擎要的不是「有游戏模块」，是「有能重新绑定的 UObject 包」。
		 * 模块里一个 UCLASS 都没有的话，前者真、后者 0，引擎同步拒绝。
		 * 只报前者就会给出一个自信但错误的结论 —— 真机验收案例 2 上撞到过。
		 */
		HotReload->SetNumberField(TEXT("reloadable_package_count"), CountHotReloadablePackages());
	}
	HotReload->SetBoolField(TEXT("available"), bHotReloadUsable);
	Data->SetObjectField(TEXT("hot_reload"), HotReload);

	/*
	 * ── 选路 ───────────────────────────────────────────────────────────────
	 *
	 * **只认 IsEnabledForSession()。** started / enabled_by_default /
	 * can_enable 三个都不算数：
	 *
	 *   - started        说的是控制台进程起没起，不是「这个会话在用它」
	 *   - enabled_by_default 说的是设置里的默认值，用户可以在会话中途关掉
	 *   - can_enable     说的是「能不能开」，不是「开着没有」
	 *
	 * 判错的代价是不对称的，所以只能认最严的那一个：
	 * ILiveCodingModule::Compile() 一进门就 EnableForSession(true)
	 * （LiveCodingModule.cpp:697），在没启动时会把 Live Coding **启动起来**并弹出
	 * 控制台窗口。一个明确关掉了 Live Coding、打算用热重载的用户，只要我们判错一次
	 * 并调了 Compile()，他的会话就被永久切成 Live Coding 模式 ——
	 * FHotReloadModule::RecompileModule 从此一律返回 false
	 * （"Unable to hot-reload modules while Live Coding is enabled."），
	 * 不重启编辑器出不来。
	 *
	 * 反过来判错（该走 Live Coding 却走了 HotReload）代价小得多：
	 * 那边会干干净净地拒绝，什么都没被改变。
	 *
	 * 所以：**我们永远不主动开启 Live Coding。开不开是用户的编辑器设置。**
	 */
	const TCHAR* CompilePath = TEXT("none");
	if (!bHasCode)
	{
		CompilePath = TEXT("none");
	}
	else if (bLcEnabledForSession)
	{
		CompilePath = TEXT("livecoding");
	}
	else if (LiveCodingHalfDisabled())
	{
		// 两条路都走不了：Live Coding 这个会话不用了，热重载会卡死。
		// 报 hotreload 就是一个自信但错误的结论 —— 案例 9 上真发生过
		CompilePath = TEXT("none");
	}
	else if (bHotReloadUsable)
	{
		CompilePath = TEXT("hotreload");
	}
	Data->SetStringField(TEXT("compile_path"), CompilePath);

	/*
	 * 把「为什么是这条路」也说出来。
	 *
	 * 只回一个枚举的话，模型遇到 none 只能猜；而这三种 none 的处置完全不同：
	 * 纯蓝图工程要请用户手动加第一个类，没有热重载模块是环境问题，
	 * 两条路都不可用则要报告而不是重试。
	 */
	FString Why;
	if (!bHasCode && DeclaredModuleCount > 0)
	{
		// 见上面 declared_module_count 那段注释：这是个坏掉的工程，不是纯蓝图工程。
		// 把两个信号都摆出来，别让人以为「加一个类就好了」
		Why = FString::Printf(
			TEXT(".uproject 声明了 %d 个模块，但磁盘上找不到源码文件（Source 目录不存在或是空的）。")
			TEXT("这不是纯蓝图工程，是一个源码缺失的 C++ 工程 —— 加新类解决不了，")
			TEXT("需要用户确认这个工程是不是拷贝时漏了 Source 目录。"),
			DeclaredModuleCount);
	}
	else if (!bHasCode)
	{
		Why = TEXT("这是纯蓝图工程（没有 C++ 源码，.uproject 里也没有声明模块）。要加第一个 C++ 类，需要用户在编辑器里手动做一次 File > New C++ Class。");
	}
	else if (bLcEnabledForSession)
	{
		Why = TEXT("这个会话启用了 Live Coding，编译走它。编不过时会从 UnrealBuildTool 的日志（Log.txt）里捞出带文件名和行号的报错；个别情况（日志找不到、或者失败发生在打补丁阶段）只能拿到一句失败。");
	}
	else if (LiveCodingHalfDisabled())
	{
		Why = LiveCodingHalfDisabledHint();
	}
	else if (bHotReloadUsable && CountHotReloadablePackages() == 0)
	{
		// 路是对的，但这条路现在走不通。先说堵在哪，别先说它的优点 ——
		// 模型只读第一句就会去调 cpp_compile
		Why = TEXT("编译本该走热重载，但现在走不通：") + NoReloadablePackagesHint();
	}
	else if (bHotReloadUsable)
	{
		Why = TEXT("这个会话没有启用 Live Coding，编译走热重载。这条路编不过时能拿到完整的编译器输出（文件名、行号、原文）。");
	}
	else
	{
		Why = TEXT("Live Coding 没有为这个会话启用，热重载模块也没加载 —— 两条路都不可用。");
	}
	Data->SetStringField(TEXT("compile_path_reason"), Why);

	UE_LOG(LogUALCpp, Log, TEXT("cpp.probe: has_code=%d path=%s"), bHasCode ? 1 : 0, CompilePath);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}
