#pragma once

#include "CoreMinimal.h"

/**
 * 记录「**本插件**改脏了哪些包」。
 *
 * ## 为什么不能只用引擎的脏标记
 *
 * 保存工具的默认行为是「只存 agent 自己改过的」。引擎的脏包集合做不到这个
 * 区分 —— 用户自己在编辑器里改到一半的资产同样是脏的，一起存下去等于替
 * 用户做了保存决定，而他可能正改到一半、正准备撤销。
 *
 * ## 怎么区分的
 *
 * 挂在引擎的全局「包被标脏」事件上，但**只在一条 UAL 命令执行期间记录**。
 * 作用域由 `FUAL_CommandScope` 在命令分发处开关（见 UAL_CommandHandler）。
 * 用户在编辑器里手动改东西时不在任何命令的作用域内，自然不会被记进来。
 *
 * ## 第二条来源：agent 自己的撤销栈
 *
 * 上面那条路有个洞：命令里派发出去、在后续帧才把包改脏的**异步写**落在作用域
 * 之外，记不到。所以 `CollectDirty` 还会去 `FUAL_AgentUndo` 的撤销栈上捞一遍 ——
 * 事务是什么时候结束就什么时候进栈的，跟落在第几帧无关。
 *
 * 两条来源取并集。仍然记不到的只剩「既不在作用域内、又没开事务」的写法，
 * 那种情况下在回调里手动 `Touch`。
 */
class UNREALAGENTLINK_API FUAL_TouchedPackages
{
public:
	/** 模块启动时挂事件 */
	static void Initialize();
	/** 模块关闭时摘掉，别留悬空委托 */
	static void Shutdown();

	/** 命令作用域进出，允许嵌套 */
	static void PushScope();
	static void PopScope();

	/**
	 * 此刻是不是正在执行一条 UAL 命令。
	 *
	 * 资产锁的编辑器提示要靠它区分「agent 自己在改」和「用户在改」——
	 * 同一个包标脏事件两边都会触发，只有后者才该弹提醒。
	 * 见 `FUAL_AssetLockState` 与 C 阶段。
	 */
	static bool IsInCommandScope();

	/** 手动登记 —— 给异步写操作留的口子 */
	static void Touch(const UPackage* Package);

	/**
	 * 取当前还活着、且仍然是脏的那些包。
	 *
	 * 会顺带把已经被存过或已卸载的条目从集合里清掉，所以这个集合不会
	 * 无限增长。
	 */
	static TArray<UPackage*> CollectDirty();

	/** 记了多少个（不判断死活，仅用于诊断） */
	static int32 Num();

	/**
	 * 清空，比如存完之后。
	 *
	 * 会顺带在 agent 撤销栈上打一个水位线，否则早先改过又已经存盘的资产会一直
	 * 留在栈上被反复算成「agent 改的」。
	 */
	static void Clear();
};
