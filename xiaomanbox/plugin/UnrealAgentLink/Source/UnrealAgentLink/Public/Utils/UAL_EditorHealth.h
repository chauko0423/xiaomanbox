#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 编辑器状况采集 —— 盒子「状态监控」面板的数据源（system.get_editor_health）。
 *
 * ## 为什么不直接读引擎的「编辑器诊断」窗口
 *
 * 那个窗口来自 EditorPerformance 插件（Engine/Plugins/Experimental），它的
 * FKPIRegistry 是公开的，读出来和用户在编辑器里看到的一字不差。但：
 *
 *   1. 它是**插件模块**。硬链接会让我们的 DLL 硬导入它，用户禁用那个插件时
 *      我们整个加载失败 —— Build.cs 里 GameplayTags 那一段否决过同样的做法。
 *   2. 本机装的 5.1 / 5.3 / 5.4 源码里都没有这个插件，只有 5.5 有。
 *
 * 所以自己采，算法照抄它（5.5 EditorPerformanceModule.cpp），数值口径一致，
 * 5.0–5.8 一套代码。**预期值不在这里** —— 插件只报原始测量，好坏由盒子按
 * Epic 那份默认预期判，改阈值不用重新出包。
 *
 * ## 和 Epic 口径不同的地方（如实说）
 *
 * - 启动耗时：OnFEngineLoopInitComplete 时刻减 GStartTime，含加载默认关卡。
 *   Epic 用 OnEditorInitialized（5.0–5.2 没有）并扣掉了关卡加载。
 * - 资产注册表：从进程启动算到 OnFilesLoaded。Epic 从扫描开始算 —— 那个事件
 *   在我们模块加载之前就发过了。扫描几乎一开机就开始，差别是秒级。
 *   我们加载时已经扫完（小工程常见）就报 null，不编数字。
 * - 卡顿率：从启动完成起**累计**，不是 Epic 那种每秒清零的瞬时值 —— 面板是用户
 *   点开时才读，那一刻焦点在盒子上，瞬时值永远是 0。
 * - 插件数：报的是已启用插件数。Epic 那一项名叫 Plugin Count，数的其实是加载过的模块。
 * - 本地缓存命中：GatherDerivedDataCacheSummaryStats 5.3 才有，5.0–5.2 报不支持。
 */
class FUAL_EditorHealth
{
public:
	/** 模块启动时调用：挂启动、PIE、资产注册表事件，起卡顿采样。重复调用无副作用 */
	static void Initialize();
	/** 模块关闭时调用 */
	static void Shutdown();

	/** 当前快照。只读，永不阻塞 */
	static TSharedPtr<FJsonObject> BuildReport();
};
