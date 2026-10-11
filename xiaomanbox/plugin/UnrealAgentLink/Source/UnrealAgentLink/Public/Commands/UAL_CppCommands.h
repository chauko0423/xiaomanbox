#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * C++ 工作流命令。
 *
 * 设计与取证。这里只放该文件 §5.1 里 P1 那两条：
 *
 *   - cpp.probe         这台机器、这个会话，C++ 能怎么编
 *   - cpp.list_modules  工程和插件各有哪些模块、源码在哪
 *
 * 两条都是只读的。编译（cpp.compile）和建类（cpp.add_class）是 P2/P3 的事，
 * 它们的前提是先能可靠回答「当前走哪条路」——也就是 cpp.probe。
 */
class FUAL_CppCommands
{
public:
	using FHandlerFunc = TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>;

	static void RegisterCommands(TMap<FString, FHandlerFunc>& CommandMap);

	/**
	 * cpp.probe —— 编译能力探测。
	 *
	 * 回答三件事：这个工程有没有 C++、这个会话该走哪条编译路、
	 * 以及走不通的话卡在哪。**不改变任何状态**，尤其不会启用 Live Coding。
	 */
	static void Handle_Probe(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * cpp.list_modules —— 列出工程模块（可选带上插件模块）。
	 */
	static void Handle_ListModules(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * cpp.compile —— 编译并热加载 C++ 改动。
	 *
	 * **异步**：立刻收下请求，编完了才用 RequestId 发响应。两条路都不阻塞游戏线程。
	 * 走哪条路由 cpp.probe 那套判据决定（只认 Live Coding 的 IsEnabledForSession）。
	 */
	static void Handle_Compile(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * cpp.add_class —— 新建一个 C++ 类（引擎的 Add C++ Class 走的是同一个函数）。
	 *
	 * 纯蓝图工程会**直接拒绝**：引擎那条路在工程原先没有代码时会弹模态对话框，
	 * 而命令跑在游戏线程上，弹出来就是编辑器和这次调用一起卡死。
	 */
	static void Handle_AddClass(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
};
