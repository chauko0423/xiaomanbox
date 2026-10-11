#include "UAL_ProxyDiagnostics.h"

#include "HAL/PlatformMisc.h"
#include "Runtime/Launch/Resources/Version.h"

/*
 * 问哪个函数，必须和这一版引擎的 Lws **实际读的那个**是同一个 —— 口径差一点，
 * 诊断就会在最需要它的时候说反话。两版的真实代码：
 *
 *   5.0–5.7  LwsWebSocketsManager.cpp: FHttpModule::Get().GetProxyAddress()
 *   5.8      LwsWebSocketsManager.cpp: FPlatformHttp::GetConfiguredProxyAddress()
 *
 * 差别不只是换了个名字：`GetProxyAddress()` 只有命令行和 ini 那一份，**不含系统
 * 代理**；5.8 那个新函数才把系统代理也算进去。所以同一台开着系统代理的机器，
 * 5.7 的编辑器连得上，5.8 的连不上 —— 2026-09-21 那位用户正是 5.8.2。
 * 照搬其中一个写法去查另一版，要么漏报要么误报。
 */
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
#include "PlatformHttp.h"
#else
#include "HttpModule.h"
#endif

namespace UAL_ProxyDiagnostics
{
	namespace
	{
		/** 从 ws://host:port/path 里取出 host，取不到就返回空 */
		FString HostOf(const FString& Url)
		{
			FString Rest = Url;

			const int32 SchemePos = Rest.Find(TEXT("://"));
			if (SchemePos != INDEX_NONE)
			{
				Rest = Rest.Mid(SchemePos + 3);
			}

			// 去掉 path / query
			int32 Cut = INDEX_NONE;
			if (Rest.FindChar(TEXT('/'), Cut))
			{
				Rest = Rest.Left(Cut);
			}
			if (Rest.FindChar(TEXT('?'), Cut))
			{
				Rest = Rest.Left(Cut);
			}

			// IPv6 字面量 [::1]:17860
			if (Rest.StartsWith(TEXT("[")))
			{
				int32 Close = INDEX_NONE;
				if (Rest.FindChar(TEXT(']'), Close))
				{
					return Rest.Mid(1, Close - 1);
				}
				return FString();
			}

			// host:port
			if (Rest.FindLastChar(TEXT(':'), Cut))
			{
				Rest = Rest.Left(Cut);
			}
			return Rest;
		}

		bool IsLoopbackHost(const FString& Host)
		{
			if (Host.IsEmpty())
			{
				return false;
			}
			if (Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
			{
				return true;
			}
			if (Host == TEXT("::1"))
			{
				return true;
			}
			// 整个 127.0.0.0/8 都是回环
			return Host.StartsWith(TEXT("127."));
		}

		bool NoProxyCoversHost(const FString& NoProxy, const FString& Host)
		{
			if (NoProxy.IsEmpty())
			{
				return false;
			}

			TArray<FString> Entries;
			NoProxy.ParseIntoArray(Entries, TEXT(","), true);
			for (FString Entry : Entries)
			{
				Entry.TrimStartAndEndInline();
				if (Entry.IsEmpty())
				{
					continue;
				}
				if (Entry == TEXT("*"))
				{
					return true;
				}
				// 回环的几种写法互相等价：NO_PROXY 里写了任一种，
				// 就当这台机器的本机连接都被排除了
				if (IsLoopbackHost(Entry) && IsLoopbackHost(Host))
				{
					return true;
				}
				if (Entry.Equals(Host, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
			return false;
		}

		/** 环境变量大小写两种写法都读，先大写 —— 和 curl / libwebsockets 的惯例一致 */
		FString EnvVar(const TCHAR* UpperName, const TCHAR* LowerName)
		{
			const FString Upper = FPlatformMisc::GetEnvironmentVariable(UpperName);
			if (!Upper.IsEmpty())
			{
				return Upper;
			}
			return FPlatformMisc::GetEnvironmentVariable(LowerName);
		}
	}

	FDiagnosis Diagnose(EProxySource Source, const FString& ProxyAddress, const FString& NoProxy, const FString& Url)
	{
		FDiagnosis Result;
		Result.Source = Source;
		Result.ProxyAddress = ProxyAddress;

		if (Source == EProxySource::None || ProxyAddress.IsEmpty())
		{
			Result.Source = EProxySource::None;
			Result.ProxyAddress.Reset();
			return Result;
		}

		const FString Host = HostOf(Url);
		if (!IsLoopbackHost(Host))
		{
			return Result;
		}

		if (Source == EProxySource::Environment && NoProxyCoversHost(NoProxy, Host))
		{
			return Result;
		}

		Result.bAtRisk = true;
		return Result;
	}

	FDiagnosis DiagnoseCurrentEnvironment(const FString& Url)
	{
		// 引擎配置路径优先：Lws 读的就是这一个，命中了就没必要再看环境变量
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
		const FString Configured = FPlatformHttp::GetConfiguredProxyAddress();
#else
		const FString Configured = FHttpModule::Get().GetProxyAddress();
#endif
		if (!Configured.IsEmpty())
		{
			return Diagnose(EProxySource::EngineConfig, Configured, FString(), Url);
		}

		const FString EnvProxy = [] {
			const FString Http = EnvVar(TEXT("HTTP_PROXY"), TEXT("http_proxy"));
			if (!Http.IsEmpty())
			{
				return Http;
			}
			const FString Https = EnvVar(TEXT("HTTPS_PROXY"), TEXT("https_proxy"));
			if (!Https.IsEmpty())
			{
				return Https;
			}
			return EnvVar(TEXT("ALL_PROXY"), TEXT("all_proxy"));
		}();

		return Diagnose(
			EProxySource::Environment,
			EnvProxy,
			EnvVar(TEXT("NO_PROXY"), TEXT("no_proxy")),
			Url);
	}

	FString DescribeRemedy(const FDiagnosis& Diagnosis, const FString& Url)
	{
		if (!Diagnosis.bAtRisk)
		{
			return FString();
		}

		const FString Where = Diagnosis.Source == EProxySource::EngineConfig
			? TEXT("引擎的 HTTP 代理配置（命令行 / [HTTP] HttpProxyAddress / 系统代理）")
			: TEXT("进程环境变量（HTTP_PROXY / HTTPS_PROXY / ALL_PROXY）");

		return FString::Printf(
			TEXT("检测到代理设置：%s = %s。引擎的 WebSocket 实现不对 localhost 例外，")
			TEXT("到 %s 的本机连接也会被送进这个代理，代理拒绝后表现就是虚幻盒子里「0 个工程」。")
			TEXT("解法：启动编辑器时加上 -ini:Engine:[HTTP]:HttpProxyAddress= ，")
			TEXT("并在该进程的环境里清掉 HTTP_PROXY / HTTPS_PROXY / ALL_PROXY（大小写两种都要）。")
			TEXT("不必重装插件，也不必停用其他 MCP。"),
			*Where,
			*Diagnosis.ProxyAddress,
			*Url);
	}
}
