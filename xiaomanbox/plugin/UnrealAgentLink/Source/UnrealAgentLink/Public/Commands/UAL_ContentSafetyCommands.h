#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class IAssetRegistry;

/**
 * 内容浏览器整理的**安全网**：只读的预检命令 + 给 batch_move / fixup_redirectors
 * 复用的预检函数。
 *
 * 第一轮的整理命令在「有版本控制、有 C++ 代码、注册表还没扫完」的真实工程里会
 * **报成功而实际没做对**。这里把三种情况
 * 变成事前看得见的东西：
 *
 * - content.registry_status    : 注册表扫没扫完、扫到哪了（永不阻塞）
 * - content.checkout_preflight : 一批包 + 它们的引用者能不能写（签出 / 只读 / 别人占着）
 * - content.cdo_refs           : 哪些资产被原生 C++ 类的默认值引用着（改名会弹窗 / 静默取消）
 *
 * 三条都**不加载任何资产**。
 */

// ============================================================================
// 签出预检
// ============================================================================

struct FUAL_PackageWriteState
{
	FString Package;
	/** 本地绝对路径；两种扩展名都不在磁盘上（没存过的新资产）时是 .uasset 的应有位置 */
	FString Filename;
	/** writable | needs_checkout | checked_out_other | not_at_head | readonly_no_scc | scc_unavailable */
	FString State;
	/** checked_out_other 时是谁占着 */
	FString CheckedOutBy;
	/** 这一个包会不会让引擎把整批中止 */
	bool bBlocks = false;
	/** source | referencer | redirector（fixup_redirectors：要删的重定向器自己的包） */
	FString Role;
	/** 引用者是为了哪些被搬的源包才进来的（RenameAssets 会一并签出引用者）；redirector 角色为空 */
	TArray<FString> For;
};

struct FUAL_PreflightResult
{
	bool bSccEnabled = false;
	FString SccProvider;
	bool bSccAvailable = false;
	TArray<FUAL_PackageWriteState> States;
	int32 BlockedCount = 0;
};

/**
 * 判定逻辑照抄 `FAssetRenameManager::AutoCheckOut`（九版形状一致）：
 * 源码管理开着 → `FUpdateStatus` 后逐个看 `IsCheckedOutOther` / `!IsCurrent` /
 * `!IsSourceControlled() || CanEdit()`；没开 → `FileExists && IsReadOnly`。
 * Provider 不可用或状态取不到 → 全部 `scc_unavailable`，不猜。
 *
 * @param In  只需填 Package / Role / For；Filename / State / CheckedOutBy / bBlocks 由这里填
 */
void UAL_PreflightPackages(const TArray<FUAL_PackageWriteState>& In, FUAL_PreflightResult& Out);

/**
 * { scc_enabled, scc_provider, scc_available, checked, blocked,
 *   blocking:[{package, filename, state, checked_out_by?, role, for:[...]}],
 *   states:[...同上，最多 500 条], states_truncated }
 */
TSharedPtr<FJsonObject> UAL_PreflightJson(const FUAL_PreflightResult& Result);

// ============================================================================
// CDO 引用预检
// ============================================================================

struct FUAL_CdoRef
{
	/** 被引用的资产包名 */
	FString Asset;
	/** 引用它的原生类，带前缀（如 ATP_ThirdPersonGameMode） */
	FString Class;
	/** 硬引用是属性名；软引用是序列化时正在写的属性名，拿不到就 "(serialized)" */
	FString Property;
	/** hard | soft */
	FString Kind;
};

/**
 * 跑和引擎 `FAssetRenameManager::FindCDOReferences` 同一套遍历，只报告不改名。
 * 只看原生 C++ 类（引擎自己的规则：跳过 ClassGeneratedBy != nullptr 的蓝图类、
 * 弃用类、骨架类）。硬引用按包名比对属性值（CDO 硬引用着的资产在 CDO 构造时
 * 就已经加载了，不用加载任何东西）；软引用序列化 CDO 收 FSoftObjectPath。
 *
 * @param Packages 要查的资产包名（大小写不敏感）
 */
void UAL_FindCdoReferences(const TArray<FString>& Packages, TArray<FUAL_CdoRef>& Out);

/** { checked, hits:[{asset, class, property, kind}] } */
TSharedPtr<FJsonObject> UAL_CdoRefsJson(int32 Checked, const TArray<FUAL_CdoRef>& Refs);

// ============================================================================
// 共用小工具
// ============================================================================

/** 一个包的引用者包名，去掉 /Script/（那是原生类，不是资产）。只读注册表 */
void UAL_CollectReferencers(IAssetRegistry& Registry, const FName PackageName, TArray<FString>& Out);

class FUAL_ContentSafetyCommands
{
public:
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	/**
	 * content.registry_status —— 注册表扫描状态，永不阻塞。
	 *
	 * 请求: {}
	 * 响应: { ok, ready, criterion, search_all_assets,
	 *         progress:{ total, processed, pending_data_load, discovering_files, snapshot_age_ms, has_snapshot }, note }
	 */
	static void Handle_RegistryStatus(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * content.registry_scan —— 把一批目录/文件重新扫进资产注册表，同步返回。
	 *
	 * 请求: { paths:["/Game/PolygonApocalypse", ...], files:["D:/Proj/Content/.../X.uasset", ...], force (默认 true) }
	 * 响应: { ok, scanned_paths, scanned_files, registered:[{path, assets}], total_assets }
	 *
	 * 为什么需要它：盒子把 .uasset **直接拷进 Content 目录**（不过引擎的导入 API），
	 * 编辑器开着时这批文件在注册表里可能一条都没有 —— 于是 content.search 回
	 * 「0 个匹配（这就是全部）」，而磁盘上明明躺着，渲染也正常。三处口径互相打架，
	 * 调用方只能绕开注册表去数磁盘文件。
	 *
	 * `registered` 是**回读**：扫完之后按目录数一遍注册表里现在有多少条。
	 * 不回读就没法区分「扫过了」和「扫了但什么都没找到」。
	 */
	static void Handle_RegistryScan(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * content.checkout_preflight —— 一批包能不能写。
	 *
	 * 请求: { packages:[...], moves:[{source,destination}], folder_moves:[{source_folder,destination_folder}],
	 *         include_referencers (默认 true), on_registry_busy }
	 *        三种输入任给其一或组合；只取源，目标不管。
	 * 响应: { ok, sources, not_found:[...], scc_enabled, scc_provider, scc_available, checked, blocked,
	 *         blocking:[...], states:[...], states_truncated, note }
	 */

	/**
	 * content.cdo_refs —— 哪些资产被原生类默认值引用。
	 *
	 * 请求: { paths:[...]（资产包路径 / 对象路径 / 目录）, on_registry_busy }
	 * 响应: { ok, checked, hits:[{asset, class, property, kind}], not_found:[...], note }
	 */
};
