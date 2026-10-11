#include "UAL_EditorHealth.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"

// GatherDerivedDataCacheSummaryStats 5.3 才有（本机 5.0 / 5.1 / 5.2 源码里都没有）
#define UAL_WITH_DDC_SUMMARY (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3))
#if UAL_WITH_DDC_SUMMARY
#include "DerivedDataCacheUsageStats.h"
#endif

// 引擎定义在 UnrealEngine.cpp、带 ENGINE_API 导出，但 5.1 起没有公开头文件声明它。
// Epic 自己的 EditorPerformance 模块也是这样就地声明的
extern ENGINE_API float GAverageFPS;

namespace
{
	/** 低于这个帧率算一次卡顿采样。照抄 EditorPerformance 的 MinFPSForHitching */
	constexpr float UAL_HitchFps = 15.0f;
	constexpr float UAL_SampleIntervalSeconds = 0.1f;

	struct FHitchCounter
	{
		uint64 Samples = 0;
		uint64 Hitches = 0;

		void Add(bool bHitch)
		{
			++Samples;
			if (bHitch)
			{
				++Hitches;
			}
		}
	};

	struct FHealthState
	{
		bool bInitialized = false;
		/** 采集器挂上时离进程启动多少秒 —— 太晚（中途启用插件）时启动类数据拿不到 */
		double InitializedAtSeconds = 0.0;

		double StartupSeconds = -1.0;
		double AssetRegistrySeconds = -1.0;
		bool bAssetRegistryDoneBeforeUs = false;

		/** 启动完成之前的卡顿采样没有意义（窗口还在加载），完成后才开始数 */
		bool bEditorReady = false;
		FHitchCounter EditorHitch;
		FHitchCounter PieHitch;

		double PieStartAt = -1.0;
		double PieFirstEnterSeconds = -1.0;
		double PieLastEnterSeconds = -1.0;
		int32 PieEnterCount = 0;

		FDelegateHandle LoopInitHandle;
		FDelegateHandle FilesLoadedHandle;
		FDelegateHandle StartPieHandle;
		FDelegateHandle PostPieHandle;
		FDelegateHandle EndPieHandle;
		FDelegateHandle CancelPieHandle;
		FTSTicker::FDelegateHandle TickerHandle;
	};

	FHealthState GHealth;

	double SecondsSinceProcessStart()
	{
		return FPlatformTime::Seconds() - GStartTime;
	}

	bool SampleHitch(float /*DeltaTime*/)
	{
		// 焦点不在编辑器上时它会主动降帧，那不是卡。Epic 同样只在有焦点时采
		if (!GHealth.bEditorReady || !FApp::HasFocus())
		{
			return true;
		}
		// 进 PIE 的过渡期（PreBeginPIE → PostPIEStarted）在加载世界，那段单独算进场耗时。
		// 用 PreBeginPIE 不用 StartPIE：后者 5.0–5.3 都没有，前者 5.0–5.8 都在，而且同样是第一个发的
		if (GHealth.PieStartAt >= 0.0)
		{
			return true;
		}

		const bool bHitch = GAverageFPS > 0.0f && GAverageFPS < UAL_HitchFps;
		const bool bInPie = GEditor && GEditor->PlayWorld != nullptr;
		(bInPie ? GHealth.PieHitch : GHealth.EditorHitch).Add(bHitch);
		return true;
	}

	void SetOrNull(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, double Value)
	{
		if (Value >= 0.0)
		{
			Obj->SetNumberField(Field, Value);
		}
		else
		{
			Obj->SetField(Field, MakeShared<FJsonValueNull>());
		}
	}

	void SetHitch(const TSharedPtr<FJsonObject>& Obj, const TCHAR* PctField, const TCHAR* SamplesField, const FHitchCounter& Counter)
	{
		SetOrNull(Obj, PctField, Counter.Samples > 0 ? 100.0 * double(Counter.Hitches) / double(Counter.Samples) : -1.0);
		Obj->SetNumberField(SamplesField, double(Counter.Samples));
	}
}

void FUAL_EditorHealth::Initialize()
{
	if (GHealth.bInitialized)
	{
		return;
	}
	GHealth.bInitialized = true;
	GHealth.InitializedAtSeconds = SecondsSinceProcessStart();

	GHealth.LoopInitHandle = FCoreDelegates::OnFEngineLoopInitComplete.AddLambda([]()
	{
		GHealth.StartupSeconds = SecondsSinceProcessStart();
		GHealth.bEditorReady = true;
	});

	if (FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
	{
		IAssetRegistry& Registry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		if (Registry.IsLoadingAssets())
		{
			GHealth.FilesLoadedHandle = Registry.OnFilesLoaded().AddLambda([]()
			{
				GHealth.AssetRegistrySeconds = SecondsSinceProcessStart();
			});
		}
		else
		{
			GHealth.bAssetRegistryDoneBeforeUs = true;
		}
	}

	GHealth.StartPieHandle = FEditorDelegates::PreBeginPIE.AddLambda([](bool)
	{
		GHealth.PieStartAt = FPlatformTime::Seconds();
	});
	GHealth.PostPieHandle = FEditorDelegates::PostPIEStarted.AddLambda([](bool)
	{
		if (GHealth.PieStartAt < 0.0)
		{
			return;
		}
		const double Seconds = FPlatformTime::Seconds() - GHealth.PieStartAt;
		GHealth.PieStartAt = -1.0;
		if (GHealth.PieEnterCount == 0)
		{
			GHealth.PieFirstEnterSeconds = Seconds;
		}
		GHealth.PieLastEnterSeconds = Seconds;
		++GHealth.PieEnterCount;
	});
	// PreBeginPIE 之后 PIE 可能根本没起来（蓝图有编译错误时用户点了取消、PIE 授权方拒绝），
	// 这时不会有 PostPIEStarted。不在这里清掉的话 SampleHitch 一直当成「正在进 PIE」，再也不采样
	GHealth.EndPieHandle = FEditorDelegates::EndPIE.AddLambda([](bool)
	{
		GHealth.PieStartAt = -1.0;
	});
	GHealth.CancelPieHandle = FEditorDelegates::CancelPIE.AddLambda([]()
	{
		GHealth.PieStartAt = -1.0;
	});

	GHealth.TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateStatic(&SampleHitch), UAL_SampleIntervalSeconds);
}

void FUAL_EditorHealth::Shutdown()
{
	if (!GHealth.bInitialized)
	{
		return;
	}
	FCoreDelegates::OnFEngineLoopInitComplete.Remove(GHealth.LoopInitHandle);
	FEditorDelegates::PreBeginPIE.Remove(GHealth.StartPieHandle);
	FEditorDelegates::PostPIEStarted.Remove(GHealth.PostPieHandle);
	FEditorDelegates::EndPIE.Remove(GHealth.EndPieHandle);
	FEditorDelegates::CancelPIE.Remove(GHealth.CancelPieHandle);
	if (GHealth.FilesLoadedHandle.IsValid())
	{
		if (FAssetRegistryModule* Module = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
		{
			Module->Get().OnFilesLoaded().Remove(GHealth.FilesLoadedHandle);
		}
	}
	FTSTicker::GetCoreTicker().RemoveTicker(GHealth.TickerHandle);
	GHealth = FHealthState();
}

TSharedPtr<FJsonObject> FUAL_EditorHealth::BuildReport()
{
	TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetBoolField(TEXT("ok"), true);
	Report->SetNumberField(TEXT("collector_started_at_seconds"), GHealth.InitializedAtSeconds);

	SetOrNull(Report, TEXT("startup_seconds"), GHealth.StartupSeconds);
	SetOrNull(Report, TEXT("asset_registry_seconds"), GHealth.AssetRegistrySeconds);
	Report->SetBoolField(TEXT("asset_registry_done_before_collector"), GHealth.bAssetRegistryDoneBeforeUs);

	Report->SetNumberField(TEXT("enabled_plugin_count"), double(IPluginManager::Get().GetEnabledPlugins().Num()));

	SetHitch(Report, TEXT("editor_hitch_pct"), TEXT("editor_hitch_samples"), GHealth.EditorHitch);
	SetHitch(Report, TEXT("pie_hitch_pct"), TEXT("pie_hitch_samples"), GHealth.PieHitch);

	SetOrNull(Report, TEXT("pie_first_enter_seconds"), GHealth.PieFirstEnterSeconds);
	SetOrNull(Report, TEXT("pie_last_enter_seconds"), GHealth.PieLastEnterSeconds);
	Report->SetNumberField(TEXT("pie_enter_count"), GHealth.PieEnterCount);

	double LocalHitPct = -1.0;
	double LocalLookups = -1.0;
	bool bDdcSupported = false;
#if UAL_WITH_DDC_SUMMARY && ENABLE_COOK_STATS
	bDdcSupported = true;
	{
		FDerivedDataCacheSummaryStats Summary;
		GatherDerivedDataCacheSummaryStats(Summary);
		double LocalHits = -1.0, LocalTotal = -1.0, ZenHits = -1.0, ZenTotal = -1.0;
		for (const FDerivedDataCacheSummaryStat& Stat : Summary.Stats)
		{
			if (Stat.Key == TEXT("LocalGetHits")) LocalHits = FCString::Atod(*Stat.Value);
			else if (Stat.Key == TEXT("LocalGetTotal")) LocalTotal = FCString::Atod(*Stat.Value);
			else if (Stat.Key == TEXT("ZenLocalGetHits")) ZenHits = FCString::Atod(*Stat.Value);
			else if (Stat.Key == TEXT("ZenLocalGetTotal")) ZenTotal = FCString::Atod(*Stat.Value);
		}
		// Epic 看的是 LocalGetHitPct。本地层是 Zen 的配置里 Local* 可能是 0，那就看 ZenLocal*
		if (LocalTotal <= 0.0 && ZenTotal > 0.0)
		{
			LocalHits = ZenHits;
			LocalTotal = ZenTotal;
		}
		if (LocalTotal > 0.0 && LocalHits >= 0.0)
		{
			LocalHitPct = 100.0 * LocalHits / LocalTotal;
			LocalLookups = LocalTotal;
		}
	}
#endif
	Report->SetBoolField(TEXT("ddc_supported"), bDdcSupported);
	SetOrNull(Report, TEXT("local_ddc_hit_pct"), LocalHitPct);
	SetOrNull(Report, TEXT("local_ddc_lookups"), LocalLookups);

	const FPlatformMemoryStats Mem = FPlatformMemory::GetStats();
	constexpr double GB = 1024.0 * 1024.0 * 1024.0;
	Report->SetNumberField(TEXT("available_memory_gb"), double(Mem.AvailablePhysical) / GB);
	Report->SetNumberField(TEXT("total_memory_gb"), double(Mem.TotalPhysical) / GB);

	return Report;
}
