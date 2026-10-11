#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 编辑器命令处理器
 * 包含: editor.screenshot, take_screenshot, project.info, editor.get_project_info(兼容别名)
 * 
 * 对应文档: 编辑器工具接口文档.md
 */
class FUAL_EditorCommands
{
public:
    /** Reuse scene capture for a transient animation preview world. */
    static bool CaptureAnimationPreview(UWorld* World, const FVector& Location, const FRotator& Rotation, FString& Path, FString& Error);
	/**
	 * 注册所有编辑器相关命令到 CommandMap
	 * @param CommandMap 命令映射表
	 */
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	// Public Handlers called by Dispatcher
	// editor.screenshot / take_screenshot - 抓取当前视口截图
	static void Handle_TakeScreenshot(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// project.info - 获取项目信息
	static void Handle_GetProjectInfo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// project.get_config - 读取配置文件项
	static void Handle_GetConfig(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// project.set_config - 设置配置文件项
	static void Handle_SetConfig(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// project.analyze_uproject - 分析 .uproject 文件，返回模块和插件信息
	static void Handle_AnalyzeUProject(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// editor.capture_app_window - 截取整个编辑器应用窗口（Slate）
	static void Handle_CaptureAppWindow(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * editor.get_focus_context —— 用户此刻「指的是什么」。
	 *
	 * 请求: {}
	 * 响应: { focusedEditor:{type,name,path,isModified,parentClass?,parentMaterial?,lastActivationTime},
	 *         openEditors:[...], hasOpenEditors,
	 *         focusedGraph?:{name,path,node_count},           // 蓝图编辑器聚焦的那张图
	 *         selectedNodes?:[{node_id,class,title,pos_x,pos_y}], selectedNodeCount, selectedNodesTruncated,
	 *         selectedActors:[{name,label,class,path}], selectedActorCount, selectedActorsTruncated,
	 *         contentBrowser:{selectedAssets:[{name,path,class}], selectedAssetCount,
	 *                         selectedAssetsTruncated, selectedFolders:[...]} }
	 *
	 * 组装逻辑见 UAL_FocusContext.h，那里写了为什么不去遍历 Slate 控件树。
	 */
	static void Handle_GetFocusContext(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * editor.save —— 把改动落盘。
	 *
	 * 在它之前，这套工具**做的所有事都不落盘**：SavePackage 只在几个蓝图
	 * 处理器里内联出现过，材质、关卡、Actor 改完全靠用户自己去按保存。
	 *
	 * 请求: { "scope": "touched"|"all"|"list", "assets": ["/Game/..."] }
	 *   - touched（默认）：只存本插件改脏的包，不碰用户自己改到一半的东西
	 *   - all：等同编辑器的「保存所有」
	 *   - list：只存 assets 里点名的
	 * 响应: { ok, saved:[...], failed:[{path,error}], skipped_dirty_count, scope }
	 */
	static void Handle_Save(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * editor.get_dirty —— 现在有哪些没存的改动，谁改的。
	 *
	 * 保存之前想知道会存些什么、保存之后想确认干净了，都靠它。
	 * 响应里把「本插件改的」和「其他来源改的」分开列，因为默认只存前者。
	 *
	 * 请求: {}
	 * 响应: { ok, touched:[...], other:[...], touched_count, other_count }
	 */
	static void Handle_GetDirty(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * editor.collect_garbage —— 强制一次垃圾回收。
	 *
	 * 批量导入、批量编译、反复加载卸载资产之后内存会涨上去且不自动回落。
	 * 大工程里跑一轮 compile_all 常见涨几个 GB。
	 *
	 * 同步执行（不是 GEngine->ForceGarbageCollection 那个下一帧才跑的版本），
	 * 因为要能报出到底回收了多少 —— 报不出数就没法判断这一步有没有用。
	 *
	 * 请求: { "full_purge": true }
	 * 响应: { ok, objects_before, objects_after, objects_freed,
	 *         mb_before, mb_after, mb_freed, elapsed_ms }
	 */
	static void Handle_CollectGarbage(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * editor.restart —— 重启编辑器。
	 *
	 * ## 这个命令和别的都不一样
	 *
	 * 它会**掐断自己的连接**：编辑器进程退出，WebSocket 随之断开。所以响应
	 * 必须在真正重启**之前**发出去，否则调用方永远等不到回应，只能等超时，
	 * 然后把一次成功的重启当成失败。
	 *
	 * 实现上先回 `{ok:true, restarting:true}`，再挂一个 ticker 到下一帧才调
	 * `RestartEditor` —— 给消息一帧的时间flush 出去。
	 *
	 * 未保存的改动会**全部丢失**，所以和 level.open 一样：有脏东西时默认拒绝，
	 * 回 409 带清单，要显式 force 才执行。
	 *
	 * 请求: { "force": false }
	 * 响应: { ok, restarting: true } 或 409 + 脏包清单
	 */
	static void Handle_RestartEditor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * pie.run —— 跑一次 PIE 试玩，回一份报告。
	 *
	 * ## 为什么是「一次调用一份报告」而不是 start/stop 两个命令
	 *
	 * start/stop 两段式对调用方几乎没用：它把游戏跑起来了，然后看不见任何
	 * 东西。而且两段式会漏 —— 忘了调 stop，PIE 就一直挂着，之后所有引擎
	 * 操作都作用在一个正在运行的游戏上。
	 *
	 * 这里一次调用跑固定时长，回**这段时间里发生了什么**：PrintString 输出、
	 * 蓝图运行时错误、结束时的画面、生成了多少 Actor。
	 *
	 * ## 时序
	 *
	 * 不能在命令里阻塞等待 —— PIE 要靠编辑器 tick 才跑得动，阻塞住游戏就
	 * 一帧都不走。所以启动后挂 ticker，到时间再收尾、用存下来的 RequestId
	 * 补发响应（同 editor.restart 的做法）。
	 *
	 * 请求: { "duration_seconds": 5, "screenshot": true, "stop_on_error": false }
	 * 响应: { ok, ran, ended_by, elapsed_seconds, print_strings[], errors[],
	 *         warnings[], actors_spawned, screenshot_path }
	 */
	static void Handle_RunPlaytest(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * pie.stop —— 让正在跑的 `pie.run` 提前收尾。
	 *
	 * 只停**我们自己起的**那个会话：用户自己按的 Play 不归这里管（回 409），
	 * 这和 `pie.run` 不接管用户会话是同一条红线。
	 *
	 * 收尾走的是和「时间到」完全相同的那条路（截图、数 Actor、关卡组成），
	 * 报告照常由 `pie.run` 那个请求回，只是 `ended_by` 是 "requested"。
	 *
	 * 请求: { "reason"?: string }
	 * 响应: { ok, stopping: true }
	 */
	static void Handle_StopPlaytest(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/** 试玩会话里捕获到的一行日志，`pie.observe` 按游标增量读 */
	struct FPieLogLine
	{
		int32 Seq = 0;
		/** 开跑后第几秒（墙钟），PIE 还没起来时为 0 */
		double At = 0.0;
		/** "print" / "error" / "warning" */
		FString Kind;
		FString Text;
	};

	/**
	 * 读 `pie.run` 会话里 `SinceSeq` 之后的日志。没有会话时返回 false。
	 *
	 * `bOutDropped` 为真说明游标之后有一段已经被挤出缓冲区 —— 调用方拿到的
	 * 不是完整的一段，必须如实往上报，不能当成「中间什么也没打印」。
	 */
	static bool ReadPieSessionLogs(int32 SinceSeq, int32 MaxLines, TArray<FPieLogLine>& OutLines, int32& OutNextSeq, bool& bOutDropped);

	/** 有没有一个由 `pie.run` 起的会话正在跑；`OutElapsed` 是开跑后的秒数 */
	static bool IsPieSessionActive(double& OutElapsed, bool& bOutPlayStarted);

	/**
	 * viewport.focus —— 把关卡视口的镜头对准指定的 Actor（等同于选中后按 F）。
	 *
	 * ## 为什么是发控制台命令而不是自己算机位
	 *
	 * `CAMERA ALIGN` 最终走 `UEditorEngine::MoveViewportCamerasToActor`，那里有
	 * 130 行专门处理包围盒的边角情况，自己重写一份只会更差：
	 *   - 空 Actor / 点光源包围盒是个点 → 半径钳到 10，否则相机怼进物体里
	 *   - 触发器、音效范围这类「巨大但看不见」的组件 → 按 IgnoreBoundsForEditorFocus 跳过
	 *   - 子 Actor 容器 → EditorGetUnderlyingActors 递归收集
	 *   - 粒子发射器包围盒为空 → 用固定默认尺寸兜底
	 *   - 5.8 起被 Deformer 变形的骨骼网格 → 异步 GPU 回读后才聚焦
	 *
	 * ## 三个必须自己处理的坑
	 *
	 * **① 不能用 `NAME=` 参数。** 它走 `FindFirstObject`，认的是**内部对象名**，
	 * 而大纲里显示的是 ActorLabel。用户把椅子改名叫「主角座椅」只改了 Label，
	 * 内部名还是 StaticMeshActor_3 —— 传 Label 必然找不到。所以这里自己按
	 * Label 解析并 SelectActor，再发不带参数的 CAMERA ALIGN。
	 *
	 * **② 目标在隐藏关卡里时引擎会弹模态框**（EditorServer.cpp 里的
	 * FSuppressableWarningDialog::ShowModal）。模态框卡住游戏线程直到有人点确定，
	 * 而我们的命令是同步回响应的 —— 真踩上就是调用方干等到超时。
	 * 所以 exec 之前必须自己先过一遍 FLevelUtils::IsLevelVisible。
	 *
	 * **③ 镜头是「飞过去」不是「跳过去」。** FocusViewportOnBox 的 bInstant 默认
	 * 为 false，走 TransitionToLocation 做几百毫秒的插值动画。exec 一返回就读
	 * 相机拿到的是**起点**，紧接着截图拍到的是**半路**。所以这里挂 ticker 等
	 * UpdateTransition() 归零再回响应。
	 *
	 * ## 两条路：整体聚焦走引擎，看局部自己算
	 *
	 * 不带 region/direction/distance 时走上面那条 `CAMERA ALIGN`，白拿引擎的
	 * 包围盒智能。一旦要「只看这棵树的树干」，引擎那条路就没用了 —— 它只会把
	 * 整棵树框进画面，树干在图里只有几个像素。这时改走自己定机位：
	 * 按包围盒高度三等分取一截，`半径 / tan(FOV/2)` 算距离，
	 * `FRotationMatrix::MakeFromX` 从方向向量直接得到朝向。
	 *
	 * 自己定机位用的是 `SetViewLocation/SetViewRotation`，**立即生效不走动画**，
	 * 所以那条路上没有「回读拿到起点、截图拍到半路」的问题。
	 *
	 * ## 为什么 direction 只有三个值
	 *
	 * current / horizontal / top。曾经想加 front/back/left/right，砍了 ——
	 * 一个道具的「正面」是哪一面引擎并不知道（+X 只是约定，摆模型的人不遵守），
	 * 每加一个方向就是加一个有一半概率是错的猜测。
	 *
	 * region 非 whole 时 direction 默认 horizontal：看树干、看门、看柱子，
	 * 人的本能是走过去平视，不是从天上往下看。这条默认值是真机翻车换来的。
	 *
	 * 请求: { "targets": {...}, "all_viewports": false,
	 *         "region": "whole|top|middle|bottom", "direction": "current|horizontal|top",
	 *         "distance": 0 }
	 * 响应: { ok, focused[], camera{location,rotation,fov}, moved, transition_settled,
	 *         is_perspective, region, distance,
	 *         bounds{center,extent,min,max,radius,height},
	 *         aim{point,error_degrees,on_target},
	 *         occluded_by, occluded_by_self,
	 *         hidden_in_editor[], skipped_hidden_levels[] }
	 */
	static void Handle_FocusViewport(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// 构建项目信息（公开给外部使用）
public:
	static TSharedPtr<FJsonObject> BuildProjectInfo();
};
