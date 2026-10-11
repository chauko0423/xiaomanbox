#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 系统命令处理器
 * 包含: system.run_console_command, system.get_performance_stats, cmd.run_python, cmd.exec_console
 * 
 * 对应文档: 系统工具接口文档.md
 */
class FUAL_SystemCommands
{
public:
	/**
	 * 注册所有系统相关命令到 CommandMap
	 * @param CommandMap 命令映射表
	 */
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	// Public Handlers called by Dispatcher
	// cmd.run_python - 执行 Python 脚本
	static void Handle_RunPython(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// cmd.exec_console / system.run_console_command - 执行控制台指令
	static void Handle_ExecConsole(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// system.get_performance_stats - 获取性能统计
	static void Handle_GetPerformanceStats(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// system.manage_plugin - 查询或修改插件状态
	static void Handle_ManagePlugin(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// system.get_project_info - 获取项目信息(路径、Content目录等)
	static void Handle_GetProjectInfo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * system.capture_perf_trace —— 用引擎自带的 CSV Profiler 采一段时间的帧时序。
	 *
	 * `system.get_performance_stats` 只能看**此刻这一帧**；掉帧往往是偶发的尖峰，
	 * 一次快照大概率错过它。这个命令跑 `duration_seconds` 秒，把每帧的
	 * FrameTime / GameThreadTime / RenderThreadTime / GPUTime 都记下来，
	 * 回一段百分位统计（p50/p95/p99）和最卡的那一帧长什么样。
	 *
	 * 异步：立即返回不会，响应在 duration_seconds 之后才发。
	 *
	 * 请求: { "duration_seconds": 10, "gpu_stats": false }
	 * 响应: { ok, csv_path, frame_count, stats: { FrameTime: {avg,min,max,p50,p95,p99}, ... },
	 *         avg_fps, bottleneck, worst_frame, idle_sample }
	 */
	static void Handle_CapturePerfTrace(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * system.capture_insights_trace —— 触发一段 Unreal Insights trace，交出 .utrace 文件路径。
	 *
	 * 只负责录制，不负责分析——逐帧 CPU/GPU 事件的解析在 TS 侧调用
	 * `UnrealInsights.exe` 无头模式完成（见 ue_analyze_insights_trace）。
	 *
	 * 请求: { "duration_seconds": 10, "channels": "cpu,gpu,frame,bookmark" }
	 * 响应: { ok, utrace_path, channels, requested_duration_seconds, actual_elapsed_seconds }
	 */
	static void Handle_CaptureInsightsTrace(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
};
