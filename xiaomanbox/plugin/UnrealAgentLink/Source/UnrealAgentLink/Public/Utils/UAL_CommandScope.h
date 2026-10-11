#pragma once

#include "CoreMinimal.h"

#include "UAL_TouchedPackages.h"

/**
 * RAII：一条 UAL 命令执行期间打开的记账窗口。
 *
 * 这期间被标脏的包算 agent 改的（决定 editor.save 存什么）。用户自己在编辑器里
 * 手动改东西时不在任何命令作用域内，因此不会被算进来。
 *
 * 由命令分发处唯一持有（见 UAL_CommandHandler::ProcessMessage）。
 *
 * ## 这里**不**换撤销缓冲
 *
 * 曾经换过 —— 每条命令进出各换一次 `GEditor->Trans`。那样一百三十条命令里
 * 绝大多数（查询、截图、编译、切关卡）明明不产生事务，也要走一遍换进换出，
 * 白白扛着「换出去的缓冲被 GC」「切关卡时缓冲攥着旧 World 的 actor」这两类
 * 风险，两个都真炸过。
 *
 * 现在换栈缩到事务本身的生命周期里，见 UAL_ScopedTransaction.h。
 */
struct UNREALAGENTLINK_API FUAL_CommandScope
{
	FUAL_CommandScope() { FUAL_TouchedPackages::PushScope(); }
	~FUAL_CommandScope() { FUAL_TouchedPackages::PopScope(); }

	FUAL_CommandScope(const FUAL_CommandScope&) = delete;
	FUAL_CommandScope& operator=(const FUAL_CommandScope&) = delete;
};
