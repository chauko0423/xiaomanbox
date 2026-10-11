#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 资产注册表就绪判定 —— 所有「读注册表」的命令共用的一道门。
 *
 * ## 为什么不在命令里死等
 *
 * 第一轮的整理命令在开头 `IsLoadingAssets() → WaitForCompletion()`，另外几条
 * 内容浏览器命令干脆裸调 `WaitForCompletion()`。意图对（残缺注册表比慢更可怕：
 * 会漏资产、漏引用，然后报「没问题」），实现有三个问题：
 *
 *   1. `WaitForCompletion()` 在游戏线程上同步阻塞。十万资产的工程冷启动首次
 *      扫描是分钟级，这期间编辑器完全无响应；盒子侧的 RPC 超时是 180 秒，
 *      超时后模型收到「插件没有响应」，用户看到「AI 把编辑器搞死了」。
 *   2. 它有官方承认的静默返回（IAssetRegistry.h 的注释）：启动早期调用时可能
 *      什么都不等就回来，而调用方拿不到任何区别。
 *   3. `IsLoadingAssets()` 5.8 起被标为 legacy —— 它只反映**首次**扫描，
 *      中途挂载新目录触发的二次扫描看不见。官方推荐的 `IsGathering()` 5.6 才公开。
 *
 * 所以改成：命令默认**不等**，注册表没好就立刻回 503 + 进度，盒子侧轮询
 * `content.registry_status` 并把进度讲给用户，扫完自动重发。要死等的调用方
 * 显式传 `on_registry_busy=wait`。
 *
 * ## 判据
 *
 * 5.6+ 用 `IsGathering()`，5.0–5.5 只有 `IsLoadingAssets()`。用重载决议探测方法
 * 在不在（同 UAL_VersionCompat.h 的 `Private::RootTracks`），不用版本号 ——
 * 工作室常年跑自定义引擎分支，版本号靠不住，方法在不在永远是准的。
 * 响应里的 `criterion` 明说用的哪个；5.0–5.5 附盲区说明，不假装能检测二次扫描。
 *
 * ## 进度快照
 *
 * `OnFileLoadProgressUpdated()` 九版都在，四个字段一字未改。委托宏 5.0/5.1 是
 * `DECLARE_EVENT_OneParam`，5.2 起换成 `DECLARE_TS_MULTICAST_DELEGATE_OneParam`，
 * `AddLambda` / `Remove` 两者一致；但 5.2+ 是线程安全多播，回调不保证在游戏线程，
 * 所以快照用临界区保护。
 */
class FUAL_RegistryReady
{
public:
	/** 模块启动时挂进度事件。重复调用无副作用 */
	static void Initialize();
	/** 模块关闭时摘事件；注册表模块已卸载时安全跳过 */
	static void Shutdown();

	/**
	 * 注册表现在能不能给出完整结果。
	 * @param OutCriterion 用的判据名："IsGathering" 或 "IsLoadingAssets"
	 */
	static bool IsReady(FString& OutCriterion);

	/**
	 * `content.registry_status` 的响应体，也是 503 错误的 details。永不阻塞。
	 *
	 * { ok, ready, criterion, search_all_assets,
	 *   progress:{ total, processed, pending_data_load, discovering_files, snapshot_age_ms, has_snapshot },
	 *   note }
	 */
	static TSharedPtr<FJsonObject> StatusJson();

	/**
	 * 命令入口处调用：读 `on_registry_busy`（"fail" 默认 | "wait"）。
	 *
	 * 就绪 → true。`wait` → `WaitForCompletion()` 后 true。否则发 503 `registry_not_ready`
	 * （details = StatusJson）并返回 false；参数值不合法发 400。返回 false 时响应已经
	 * 发出，调用方直接 return 即可。
	 */
	static bool Ensure(const TSharedPtr<FJsonObject>& Payload, const FString& RequestId);
};
