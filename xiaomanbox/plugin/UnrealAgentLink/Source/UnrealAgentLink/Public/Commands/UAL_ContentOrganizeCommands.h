#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 内容浏览器**整理**命令：命名规范、批量搬迁、依赖闭包、跨工程迁移。
 *
 * 和 UAL_ContentBrowserCommands（单个资产的增删改查）的分工：那边一次动一个资产，
 * 这边一次动一批，并且每一条都先能「只看不做」（dry_run）。
 *
 * 四条命令全部只读资产注册表来做规划，**规划阶段不加载任何资产**；
 * 只有 batch_move 真正执行时才把要搬的资产载进内存，而且是一次
 * `IAssetTools::RenameAssets` 搬完整批 —— 引用者只加载一遍、只重写一遍，
 * 这是批量搬迁快于逐个调 content.move 的全部原因。
 *
 * - content.naming_audit : 按类型前缀表体检命名，给出建议的新名字（只读）
 * - content.batch_move   : 一批资产 / 整个目录搬到新位置，可选顺手清理重定向器并落盘
 * - content.dependencies : 一个资产或目录的依赖 / 被引用闭包，含跨目录引用与断链
 * - content.migrate      : 连同依赖闭包一起拷到另一个工程的 Content 目录
 */
class FUAL_ContentOrganizeCommands
{
public:
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	/**
	 * content.naming_audit —— 命名规范体检。
	 *
	 * 前缀表以 Epic 官方推荐为底
	 * （https://dev.epicgames.com/documentation/en-us/unreal-engine/recommended-asset-naming-conventions-in-unreal-engine-projects），
	 * 补了几个官方没列、但业内基本一致的（MF_ / MPC_ / RT_ / BFL_）。表里没有的类型
	 * 不猜，按 unknown_classes 报出来让调用方决定。
	 *
	 * 请求: { path, recursive, class_filter, rules:{Class:"Prefix_"}, ignore_classes:[], pascal_case, include_compliant, limit }
	 * 响应: { ok, scanned, compliant_count, violation_count, violations:[{path,name,class,expected_prefix,reason,suggested_name,suggested_path,conflict}],
	 *         by_reason, by_class, unknown_classes, rules_used, truncated }
	 */
	static void Handle_NamingAudit(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * content.batch_move —— 批量移动 / 重命名。
	 *
	 * 规划完先过两道预检（见 UAL_ContentSafetyCommands.h），dry_run 和执行都带：
	 *   - checkout：源 + 全部引用者能不能写（RenameAssets 会一并签出引用者，一个签不出整批不动）
	 *   - cdo_refs：源被哪些原生 C++ 类默认值引用（引擎会弹确认框，无人值守下静默取消整批）
	 * 执行时 on_blocked / on_cdo_refs 默认 fail：有阻塞就一个都不发给 RenameAssets，
	 * 回 ok:false + reason。proceed 在 cdo_refs 那一关是「替用户点确定」，每次回答记进响应。
	 *
	 * 请求: { moves:[{source,destination}], folder_moves:[{source_folder,destination_folder}],
	 *         dry_run, on_conflict:"fail"|"skip"|"auto_rename", fixup_redirectors, save,
	 *         on_blocked:"fail"|"proceed", on_cdo_refs:"fail"|"proceed", on_registry_busy:"fail"|"wait" }
	 * 响应: { ok, dry_run, planned, moved, skipped, failed, errors, items:[...], conflicts:[...],
	 *         checkout:{ scc_enabled, scc_provider, scc_available, checked, blocked, blocking:[...], states:[...], states_truncated },
	 *         cdo_refs:{ checked, hits:[{asset,class,property,kind}] },
	 *         reason?:"checkout_blocked"|"cdo_referenced",
	 *         engine_log:[...], auto_answered_dialogs:[{type,title,message,answer}],
	 *         redirectors_found, redirectors_fixed, saved_count, save_failed, dirty_after, elapsed_ms, notes }
	 *        items[] 里 moved 且已落盘的条目多三个字段 file（绝对路径）/ bytes / mtime（ISO 8601），给回滚账本做指纹；
	 *        failed 条目的 error 尽量回填引擎日志里 "{包名} - {原因}" 的原因。
	 */
	static void Handle_BatchMove(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * content.dependencies —— 依赖 / 被引用闭包。
	 *
	 * 请求: { path, direction:"dependencies"|"referencers"|"both"|"unreferenced", recursive, max_depth,
	 *         hard_only, include_engine, max_nodes, limit, include_edges }
	 * 响应: { ok, root, scope_is_folder, root_count, dependencies:{...}, referencers:{...}, unreferenced:[...], notes }
	 */
	static void Handle_Dependencies(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * content.migrate —— 跨工程迁移（等价于编辑器右键 Migrate，但无对话框、逐文件回报）。
	 *
	 * 请求: { paths:[...], destination, include_dependencies, on_conflict:"skip"|"overwrite", dry_run, save_first }
	 * 响应: { ok, dry_run, destination_content_dir, planned, copied, skipped, failed, external_skipped,
	 *         total_bytes, files:[...], unsaved_sources, notes, elapsed_ms }
	 */
	static void Handle_Migrate(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
};
