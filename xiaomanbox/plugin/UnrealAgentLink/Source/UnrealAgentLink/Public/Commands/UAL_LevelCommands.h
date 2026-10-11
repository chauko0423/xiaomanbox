#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 关卡工具命令处理器
 * 包含: level.query_assets
 * 
 * 对应文档: 关卡工具接口文档.md
 */
class FUAL_LevelCommands
{
public:
	/**
	 * 注册所有关卡相关命令到 CommandMap
	 * @param CommandMap 命令映射表
	 */
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	// Public Handlers called by Dispatcher
	// level.query_assets - 多维资产查询
	static void Handle_QueryAssets(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	// level.organize_actors - 批量组织Actor到文件夹
	static void Handle_OrganizeActors(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * level.get_current —— 当前开着哪张关卡、脏没脏。
	 *
	 * 请求: {}
	 * 响应: { ok, package, is_dirty, is_temporary, actor_count }
	 */
	static void Handle_GetCurrentLevel(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * level.save —— 保存当前关卡。
	 *
	 * 从没保存过的新关卡（还在 /Temp/ 下）需要一个 `path` 才能落盘。
	 *
	 * 请求: { "path": "/Game/Maps/MyLevel" }  // 已保存过的关卡可省略
	 * 响应: { ok, package, saved_as }
	 */
	static void Handle_SaveLevel(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * level.open —— 打开一张关卡。
	 *
	 * **会丢弃未保存的改动**，所以有脏东西时默认拒绝，除非显式 force=true。
	 * 这是整套工具里少数几个能让用户丢工作的操作之一。
	 *
	 * 请求: { "path": "/Game/Maps/MyLevel", "force": false }
	 * 响应: { ok, package } 或 409 + 脏包清单
	 */
	static void Handle_OpenLevel(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * level.new —— 新建一张空关卡。
	 *
	 * 同样会丢弃未保存的改动，同样默认拒绝。
	 *
	 * 请求: { "force": false, "save_as": "/Game/Maps/NewLevel" }
	 * 响应: { ok, package, saved }
	 */
	static void Handle_NewLevel(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * level.list —— 这个世界由哪些关卡组成，每个子关卡的加载/可见标志是什么。
	 *
	 * 「编辑器里好好的，运行起来不一样」最常见的成因就是子关卡的
	 * **编辑器可见** 与 **游戏加载** 是两个独立开关。此前整套命令答不了这个问题：
	 * actor.* 只能查 Actor，level.get_current 只回当前那一张。
	 *
	 * 请求: {}
	 * 响应: { ok, persistent, streaming_levels[], editor_only_levels[], is_world_partition }
	 */
	static void Handle_ListLevels(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * level.set_streaming —— 改一个子关卡的加载方式和可见标志。
	 *
	 * 等效于 Levels 窗格里那几个勾选框。改完会回读，回读不上就不报成功。
	 *
	 * 请求: { "level": "L_BaseEnvironment", "always_loaded": true,
	 *        "should_be_loaded": true, "should_be_visible": true, "visible_in_editor": true }
	 * 响应: { ok, level, before: {...}, after: {...}, changed[] }
	 */
	static void Handle_SetLevelStreaming(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * 一个世界的关卡组成，`level.list` 的响应体本身。
	 *
	 * 公开出来是给 `pie.run` 用的：试玩报告里要能回答「编辑器里有、游戏里没有
	 * 哪几层」。那一刻 PIE 世界马上就要销毁，只有在插件内部当场取到，
	 * 调用方事后无论如何都查不回来。
	 */
	static TSharedPtr<FJsonObject> BuildLevelComposition(class UWorld* World);
};
