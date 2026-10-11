#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 内容浏览器命令处理器
 * 管理 UE 编辑器内的文件与文件夹结构
 * 
 * 包含 5 个原子工具（CRUD + Describe）:
 * - content.search   : 搜索/浏览资产（支持通配符 "*" 列出所有资产）
 * - content.import   : 导入外部文件 (FBX/PNG/WAV 等)
 * - content.move     : 移动/重命名资产
 * - content.delete   : 删除资产/文件夹
 * - content.describe : 获取资产详情（含依赖和被引用关系）
 * 
 * 对应文档: 内容管理文档.md
 */
class FUAL_ContentBrowserCommands
{
public:
	/**
	 * 注册所有内容浏览器相关命令到 CommandMap
	 * @param CommandMap 命令映射表
	 */
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	// ========================================================================
	// Public Handlers (由 Dispatcher 调用)
	// ========================================================================
	
	/**
	 * content.search - 搜索/浏览资产
	 * 在 Content Browser 中查找匹配的资产路径，支持通配符查询
	 * 
	 * @param Payload 请求参数:
	 *   - query: 搜索关键词（可选，默认 "*" 列出所有资产）
	 *   - path: 目录路径限制，如 /Game/Blueprints（可选）
	 *   - filter_class: 类型过滤，如 Material, StaticMesh（可选）
	 *   - include_folders: 是否返回文件夹信息（可选，默认 false）
	 *   - limit: 返回数量限制（可选，默认 100，最大 500）
	 * @param RequestId 请求 ID
	 */
	static void Handle_SearchAssets(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * content.import - 导入外部文件
	 * 将磁盘上的文件导入到 UE 项目中
	 * 
	 * @param Payload 请求参数 (files, destination_path, overwrite)
	 * @param RequestId 请求 ID
	 */
	static void Handle_ImportAssets(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * content.move - 移动/重命名资产
	 * 移动资产或通过修改目标路径实现重命名
	 * 
	 * @param Payload 请求参数 (source_path, destination_path)
	 * @param RequestId 请求 ID
	 */
	static void Handle_MoveAsset(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * content.delete - 删除资产（只删资产，不删文件夹）
	 *
	 * 请求: { "paths": ["/Game/Foo/BP_Bar.BP_Bar"], "drop_agent_undo": false }
	 *   drop_agent_undo 默认 false：资产被 agent 自己的撤销栈拽着时**不删也不动栈**，
	 *   只如实回报要丢几步撤销；用户同意了再带 true 调一次。
	 *
	 * 响应: { ok, deleted_count, requested_count, engine_deleted_objects,
	 *         deleted:[...], dropped_agent_undo_steps:[...], failed:[{path, reason}], error }
	 *   reason 是**查出来的真原因**（只读位 / agent 撤销栈 / 具体的引用者名字），
	 *   不是猜的。deleted_count 按路径算，engine_deleted_objects 是引擎删掉的对象数。
	 *
	 * @param RequestId 请求 ID
	 */
	static void Handle_DeleteAssets(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * content.describe - 获取资产详情
	 * 返回资产的完整信息，包括依赖项和被引用项
	 * 
	 * @param Payload 请求参数 (path, include_dependencies, include_referencers)
	 * @param RequestId 请求 ID
	 */
	static void Handle_DescribeAsset(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * content.normalized_import - 规范化导入 uasset/umap 资产
	 * 将外部工程的资产导入到规范化的目录结构中
	 * 自动处理依赖闭包、包名重映射和引用修复
	 * 
	 * @param Payload 请求参数:
	 *   - files: 要导入的文件路径列表
	 *   - target_root: 可选，目标根目录（默认 /Game/Imported）
	 *   - use_pascal_case: 可选，是否使用 PascalCase（默认 true）
	 *   - auto_rename_on_conflict: 可选，冲突时是否自动重命名（默认 true）
	 * @param RequestId 请求 ID
	 */
	static void Handle_NormalizedImport(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * content.audit_optimization - 资产优化审计
	 * 检测 Nanite、Lumen 等功能的使用情况，提供优化建议
	 * 
	 * @param Payload 请求参数:
	 *   - check_type: 可选，检查类型 ("NaniteUsage", "LumenMaterials", "TextureSize", "All")
	 * @param RequestId 请求 ID
	 */
	static void Handle_AuditOptimization(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * content.fixup_redirectors —— 清理移动/重命名资产留下的重定向器。
	 *
	 * 移动或改名之后，原路径上会留一个 ObjectRedirector 把引用转发到新位置。
	 * 它能让旧引用继续工作，但会一直堆积：引用链多绕一跳、Content Browser 里
	 * 一堆看不见的垃圾、迁移工程时还会把整条链拖过去。
	 *
	 * 这个命令把引用者改成直接指向新资产，然后删掉重定向器。
	 *
	 * 请求: { "path": "/Game", "paths": ["/Game/Old/SM_A", "/Game/Old/"], "dry_run": false, "delete_broken": false,
	 *         "on_registry_busy": "fail"|"wait" }
	 *        paths 给了就只处理这些（重定向器包路径或目录），不扫整个 path；给了空数组就什么都不处理。
	 * 响应: { ok, path, found, broken_count, broken_left, fixed, remaining, deleted_broken, dry_run,
	 *         redirectors:[...], details:[{path,target,broken}], listed_note, not_redirectors:[...],
	 *         checkout:{ scc_enabled, scc_provider, scc_available, checked, blocked, blocking:[...], states:[...], states_truncated },
	 *         dirty_referencers:[...], saved_referencers:[...], left_on_disk:[...],
	 *         engine_log:[...], load_failed:[...], broken_after_load:[...], dirty_after, note }
	 *        checkout 里既有每个重定向器的**引用者**（role=referencer，FixupReferencers 要改写它们）也有
	 *        重定向器自己的包（role=redirector，引擎删它之前看它的签出状态）。
	 *        dirty_referencers：引用者里用户改到一半没存的 —— FixupReferencers 会把它们原样落盘。
	 *        broken 的判定按目标**对象路径**在不在注册表里；执行时只删预演和加载都判坏的，
	 *        加载后才发现坏的进 broken_after_load、不删。fixed 不含 deleted_broken。
	 *        dirty_after 只数这条命令新弄脏、引擎又没存成的包（引擎在删重定向器前会把改过的引用者存了）。
	 *        engine_log / load_failed / broken_after_load 只在执行时有。
	 *        名字清单（not_redirectors、dirty_referencers、saved_referencers、left_on_disk、load_failed、
	 *        broken_after_load）最多列 200 条，超出时另给 <字段名>_total 报总数。
	 *        UE 5.4+ 执行时会弹「Redirector Update Report」模态框由人点；弹不出来（脚本模式 / 无渲染器）时回 409。
	 */
	static void Handle_FixupRedirectors(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * content.asset_ranking —— 全工程资源占用排行。
	 *
	 * 和 `content.audit_optimization` 的分工：那个回「整体什么状况」（启用率、总量），
	 * 这个回「具体是哪几个资产」，逐条列出来、可排序。
	 *
	 * **不加载任何资产。** 数据全部来自资产注册表的标签（保存资产时写进 uasset
	 * 头里的），加上包文件在磁盘上的实际字节数。audit_optimization 那边是
	 * `AssetData.GetAsset()` 逐个加载，一万个资产的工程要跑上几分钟并且吃掉一大块内存；
	 * 这里几秒钟。
	 *
	 * 请求: { "path": "/Game", "class_filter": "StaticMesh", "sort_by": "DiskSize", "limit": 30 }
	 * 响应: { ok, scanned, returned, total_disk_size, assets:[...], by_class:{...}, notes:[...] }
	 */
	static void Handle_AssetRanking(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * content.size_map —— 一个资产连同它的依赖一共有多大。
	 *
	 * 编辑器里 Window → Developer Tools → Size Map 的等价物，回答的是
	 * 「这个蓝图/关卡拖了多少东西进来」。`content.describe` 只给一层直接依赖，
	 * 答不了这个问题 —— 真正吃内存的常常在第三层。
	 *
	 * 请求: { "path": "/Game/Maps/Main", "max_nodes": 5000 }
	 * 响应: { ok, root, total_size, dependency_count, top_contributors:[...], truncated }
	 */
	static void Handle_SizeMap(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
};
