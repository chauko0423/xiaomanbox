#include "UAL_RegistryReady.h"
#include "UAL_CommandUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALRegistry, Log, All);

namespace
{
	struct FUAL_RegistrySnapshot
	{
		bool bHasSnapshot = false;
		int32 NumTotalAssets = 0;
		int32 NumProcessed = 0;
		int32 NumPendingDataLoad = 0;
		bool bDiscoveringFiles = false;
		/** FPlatformTime::Seconds() 的时刻；用来算 snapshot_age_ms —— 一直不动说明扫描卡住了 */
		double UpdatedAt = 0.0;
	};

	FUAL_RegistrySnapshot GSnapshot;
	FCriticalSection GSnapshotLock;
	FDelegateHandle GProgressHandle;

	IAssetRegistry& UAL_RegistryChecked()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	namespace Private
	{
		// int 重载优先：有 IsGathering（5.6+）就用它 —— 首次扫描和二次挂载扫描都能看见
		template <typename RegistryType>
		auto Gathering(const RegistryType& Registry, FString& OutCriterion, int)
			-> decltype(Registry.IsGathering())
		{
			OutCriterion = TEXT("IsGathering");
			return Registry.IsGathering();
		}

		// long 重载兜底：5.0–5.5 只有 IsLoadingAssets，只反映首次扫描
		template <typename RegistryType>
		auto Gathering(const RegistryType& Registry, FString& OutCriterion, long)
			-> decltype(Registry.IsLoadingAssets())
		{
			OutCriterion = TEXT("IsLoadingAssets");
			return Registry.IsLoadingAssets();
		}
	}
}

void FUAL_RegistryReady::Initialize()
{
	if (GProgressHandle.IsValid())
	{
		return;
	}
	IAssetRegistry& Registry = UAL_RegistryChecked();
	GProgressHandle = Registry.OnFileLoadProgressUpdated().AddLambda(
		[](const IAssetRegistry::FFileLoadProgressUpdateData& Data)
		{
			FScopeLock Lock(&GSnapshotLock);
			GSnapshot.bHasSnapshot = true;
			GSnapshot.NumTotalAssets = Data.NumTotalAssets;
			GSnapshot.NumProcessed = Data.NumAssetsProcessedByAssetRegistry;
			GSnapshot.NumPendingDataLoad = Data.NumAssetsPendingDataLoad;
			GSnapshot.bDiscoveringFiles = Data.bIsDiscoveringAssetFiles;
			GSnapshot.UpdatedAt = FPlatformTime::Seconds();
		});
}

void FUAL_RegistryReady::Shutdown()
{
	if (!GProgressHandle.IsValid())
	{
		return;
	}
	// 引擎退出时模块按依赖反序卸载，注册表理应还在；但别赌 —— 取不到就不摘，
	// 委托随模块一起消亡
	if (FAssetRegistryModule* Module = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
	{
		Module->Get().OnFileLoadProgressUpdated().Remove(GProgressHandle);
	}
	GProgressHandle.Reset();
	FScopeLock Lock(&GSnapshotLock);
	GSnapshot = FUAL_RegistrySnapshot();
}

bool FUAL_RegistryReady::IsReady(FString& OutCriterion)
{
	const IAssetRegistry& Registry = UAL_RegistryChecked();
	return !Private::Gathering(Registry, OutCriterion, 0);
}

TSharedPtr<FJsonObject> FUAL_RegistryReady::StatusJson()
{
	FString Criterion;
	const bool bReady = IsReady(Criterion);
	IAssetRegistry& Registry = UAL_RegistryChecked();

	FUAL_RegistrySnapshot Snapshot;
	{
		FScopeLock Lock(&GSnapshotLock);
		Snapshot = GSnapshot;
	}

	TSharedPtr<FJsonObject> Progress = MakeShared<FJsonObject>();
	Progress->SetNumberField(TEXT("total"), Snapshot.NumTotalAssets);
	Progress->SetNumberField(TEXT("processed"), Snapshot.NumProcessed);
	Progress->SetNumberField(TEXT("pending_data_load"), Snapshot.NumPendingDataLoad);
	Progress->SetBoolField(TEXT("discovering_files"), Snapshot.bDiscoveringFiles);
	Progress->SetNumberField(TEXT("snapshot_age_ms"),
		Snapshot.bHasSnapshot ? (FPlatformTime::Seconds() - Snapshot.UpdatedAt) * 1000.0 : -1.0);
	Progress->SetBoolField(TEXT("has_snapshot"), Snapshot.bHasSnapshot);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetBoolField(TEXT("ready"), bReady);
	Result->SetStringField(TEXT("criterion"), Criterion);
	Result->SetBoolField(TEXT("search_all_assets"), Registry.IsSearchAllAssets());
	Result->SetObjectField(TEXT("progress"), Progress);

	FString Note;
	if (Criterion == TEXT("IsLoadingAssets"))
	{
		Note = TEXT("On UE 5.0-5.5 the readiness criterion (IsLoadingAssets) only reflects the initial asset scan; a rescan triggered by mounting a new content folder or plugin is not detected. Results right after such a mount may be incomplete.");
	}
	else
	{
		Note = TEXT("IsGathering covers both the initial scan and later rescans.");
	}
	if (!Snapshot.bHasSnapshot)
	{
		Note += TEXT(" No progress event has been received yet (progress numbers are placeholders).");
	}
	Result->SetStringField(TEXT("note"), Note);
	return Result;
}

bool FUAL_RegistryReady::Ensure(const TSharedPtr<FJsonObject>& Payload, const FString& RequestId)
{
	FString OnBusy = TEXT("fail");
	if (Payload.IsValid())
	{
		Payload->TryGetStringField(TEXT("on_registry_busy"), OnBusy);
	}
	OnBusy = OnBusy.ToLower();
	if (OnBusy != TEXT("fail") && OnBusy != TEXT("wait"))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("on_registry_busy must be 'fail' or 'wait'"));
		return false;
	}

	FString Criterion;
	if (IsReady(Criterion))
	{
		return true;
	}

	if (OnBusy == TEXT("wait"))
	{
		// 调用方明确要求死等才等。注意它有官方承认的静默返回（见头文件），
		// 等完不代表一定齐 —— 但这是调用方自己选的
		UE_LOG(LogUALRegistry, Log, TEXT("Asset registry still scanning (%s); on_registry_busy=wait, blocking until complete"), *Criterion);
		UAL_RegistryChecked().WaitForCompletion();
		return true;
	}

	UAL_CommandUtils::SendError(RequestId, 503, TEXT("registry_not_ready"), StatusJson());
	return false;
}
