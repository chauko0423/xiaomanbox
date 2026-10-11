#pragma once

#include "CoreMinimal.h"

/**
 * 回环连接被代理拦截的诊断。
 *
 * ## 为什么需要这个
 *
 * 引擎的 Lws WebSocket 实现会给**所有** ws 连接套 HTTP 代理，**不带 localhost
 * 例外**。所以只要机器上有代理（国内开发机几乎是标配），`ws://127.0.0.1:17860`
 * 这条本机连接也会被送进代理，代理拒绝，然后重连 ticker 每 5 秒失败一次。
 *
 * 代理从哪儿读，5.8 和之前不一样（5.8 起把系统代理也算进去了）—— 具体见
 * UAL_ProxyDiagnostics.cpp 顶部。
 *
 * 现象是盒子那头显示「0 个工程」，日志里只有一行 `Connection error`，没有任何
 * 线索指向代理。2026-09-21 有用户在这上面耗掉一整轮排查：重装插件、停用别的
 * MCP、怀疑端口冲突，全都不是原因。
 *
 * 这里不去修引擎的行为（那要自己实现一份 WebSocket 客户端），只负责在连不上的
 * 时候把「你这台机器有代理，而本机连接会走它」这句话说出来。
 */
namespace UAL_ProxyDiagnostics
{
	/** 代理设置是从哪儿读到的 —— 决定给什么解法，所以要报出来 */
	enum class EProxySource : uint8
	{
		None,
		/** 引擎自己的代理配置（命令行 / `[HTTP] HttpProxyAddress`；5.8 起还含系统代理） */
		EngineConfig,
		/** 进程环境变量 HTTP_PROXY / HTTPS_PROXY / ALL_PROXY */
		Environment
	};

	struct FDiagnosis
	{
		/** 目标是回环地址，且查到了会作用于它的代理 */
		bool bAtRisk = false;
		EProxySource Source = EProxySource::None;
		/** 查到的代理地址，原样带出来给日志 */
		FString ProxyAddress;
	};

	/**
	 * 纯判定：给定代理地址、NO_PROXY 和目标 URL，这条连接会不会被送进代理。
	 *
	 * `NoProxy` 只在 `Source == Environment` 时参与判断。环境变量那套约定里
	 * NO_PROXY 是配套的，但引擎配置路径下的代理是否遵守 NO_PROXY 没有验证过 ——
	 * 没验证过的事不当成「安全」。
	 */
	FDiagnosis Diagnose(EProxySource Source, const FString& ProxyAddress, const FString& NoProxy, const FString& Url);

	/** 读这台机器的实际设置（引擎代理配置 + 进程环境变量）做一次判定 */
	FDiagnosis DiagnoseCurrentEnvironment(const FString& Url);

	/** 判定结果拼成给用户看的一段话（含解法）。`bAtRisk == false` 时返回空串 */
	FString DescribeRemedy(const FDiagnosis& Diagnosis, const FString& Url);
}
