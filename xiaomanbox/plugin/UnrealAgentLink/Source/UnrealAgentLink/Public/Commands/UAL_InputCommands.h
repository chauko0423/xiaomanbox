#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 输入映射查询。
 *
 * 回答一个问题：**这个游戏现在（或者说，在这个工程里）能按什么。**
 *
 * ## 为什么这不是「读一下 DefaultInput.ini」
 *
 * Enhanced Input 的映射由**当前挂着哪些 IMC** 决定，而 IMC 是游戏逻辑随时加减的：
 * 主菜单一套、跑图一套、开车一套、打开背包再压一套。所以「W 是前进」这句话
 * 只在某个时刻成立。
 *
 * 读资产只能得到*可能的*绑定；要知道*此刻*能按什么，必须读运行时。
 * 两者必须用 `source` 字段分清楚 —— 混为一谈就会出现
 * 「工具说 E 是交互，注入了没反应」，因为那个 IMC 在这个关卡压根没被加进来。
 *
 * 设计 与 §7.1。
 *
 * 支持的命令：
 * - input.map: 列出当前生效（或工程里定义）的输入映射
 */
class FUAL_InputCommands
{
public:
	using FHandlerFunc = TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>;

	static void RegisterCommands(TMap<FString, FHandlerFunc>& CommandMap);

	/** input.map —— 当前能按什么 */
	static void Handle_GetInputMap(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * input.inject_action —— 按「动作」注入（L1）。
	 *
	 * 直接把值塞进 Enhanced Input 的注入队列，**绕开键位映射**。
	 * 好处是稳、不依赖窗口焦点；代价是它证明不了键位绑对没有 ——
	 * 一个根本没绑跳跃键的工程，这条路照样能让角色跳起来。
	 * 所以返回里一定带这句免责，别让调用方拿它当「键位正常」的证据。
	 */
	static void Handle_InjectAction(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * input.inject_key —— 按「键」注入（L2）。
	 *
	 * 从游戏视口发一次真实按键，走完整条链路：视口 → PlayerInput → 键位映射 → 动作。
	 * 这是唯一能验出「键位到底绑没绑上」的路径，也是唯一会**撞上 UI 那道闸**的路径
	 * （2026-09-03 实测：菜单打开时这条路归零，而 L1 照常生效）。
	 */
	static void Handle_InjectKey(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
};
