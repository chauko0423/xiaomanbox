#pragma once

#include "CoreMinimal.h"
#include "IWebSocket.h"
#include "Runtime/Launch/Resources/Version.h"

// 线程间消息通知
DECLARE_MULTICAST_DELEGATE_OneParam(FUALOnMessageReceived, const FString&);
DECLARE_MULTICAST_DELEGATE(FUALOnConnected);
DECLARE_MULTICAST_DELEGATE(FUALOnDisconnected);

#include "Containers/Ticker.h"
using FTickerHandleType = FTSTicker::FDelegateHandle;
using FTickerDelegateType = FTickerDelegate;
#define UAL_CORE_TICKER FTSTicker::GetCoreTicker()

/**
 * 维护 WebSocket 连接、心跳、重连
 */
class FUAL_NetworkManager
{
public:
	static FUAL_NetworkManager& Get();

	// 初始化并连接到指定服务器
	void Init(const FString& ServerUrl);

	// 关闭连接并释放资源
	void Shutdown();

	// 发送消息（线程安全）
	void SendMessage(const FString& JsonData);

	// 接收消息回调（在 Socket 线程触发，外部需切到 GameThread）
	FUALOnMessageReceived& OnMessageReceived() { return MessageReceivedDelegate; }

	// 连接成功回调（在 Socket 线程触发，外部需切到 GameThread）
	FUALOnConnected& OnConnected() { return ConnectedDelegate; }

	/**
	 * 连接断开回调（在 Socket 线程触发，外部需切到 GameThread）。
	 *
	 * 有些状态是「盒子说了才算」的，断开之后插件手上那份就成了谎话 ——
	 * 比如资产锁的角标，盒子没了就没有任何东西还锁着，但角标不会自己消失。
	 * 断线即失效的状态一律挂这里清掉。
	 */
	FUALOnDisconnected& OnDisconnected() { return DisconnectedDelegate; }

	// 当前是否已连接
	bool IsConnected() const;

private:
	FUAL_NetworkManager() = default;

	void Connect();
	void BindSocketEvents();
	void CleanupSocket();

	void StartReconnectTimer();
	void StopReconnectTimer();
	bool TickReconnect(float DeltaTime);

	void StartHeartbeatTimer();
	void StopHeartbeatTimer();
	bool TickHeartbeat(float DeltaTime);

	void HandleOnMessage(const FString& Data);
	void HandleOnConnected();
	void HandleOnClosed(int32 StatusCode, const FString& Reason, bool bWasClean);
	void HandleOnConnectionError(const FString& Error);

	/** 只在「连着 → 断了」这个跳变上播一次，重连失败的每一次重试不再播 */
	void BroadcastDisconnectedOnce();

	/**
	 * 连不上时把「这台机器的代理会拦住本机连接」这句话说出来，整个会话只说一次。
	 *
	 * 详情见 UAL_ProxyDiagnostics.h。
	 */
	void LogProxyRemedyOnce();

private:
	FCriticalSection SendMutex;
	TSharedPtr<IWebSocket> Socket;
	FString TargetUrl;

	FTickerHandleType ReconnectTickerHandle;
	FTickerHandleType HeartbeatTickerHandle;
	bool bIsConnecting = false;
	/** 本次连接尝试的开始时刻，用于给「连接中」加超时（见 TickReconnect） */
	double ConnectStartedAt = 0.0;
	/** 超过这个秒数还没有任何回调，就当这次连接尝试失败并重来 */
	static constexpr double ConnectTimeoutSeconds = 15.0;
	bool bWantsReconnect = false;

	FUALOnMessageReceived MessageReceivedDelegate;
	FUALOnConnected ConnectedDelegate;
	FUALOnDisconnected DisconnectedDelegate;
	/** 上一次广播出去的是「连着」还是「断了」。防止重连失败时每次重试都播一遍断开 */
	bool bBroadcastedConnected = false;

	/** Init 时算好的代理告警文本；空串表示这台机器没有会拦住回环连接的代理 */
	FString ProxyRemedy;
	/** 代理告警已经播过了 */
	bool bLoggedProxyRemedy = false;
};

