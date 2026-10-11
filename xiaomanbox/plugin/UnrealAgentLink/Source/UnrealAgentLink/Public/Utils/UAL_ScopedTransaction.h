#pragma once

#include "CoreMinimal.h"
#include "ScopedTransaction.h"

#include "UAL_AgentUndo.h"

/**
 * `FScopedTransaction` 的替身：事务开着的这段时间，把 agent 那条撤销栈换进来。
 *
 * ## 为什么把换栈挪到这里，而不是放在命令分发处
 *
 * 上一版是每条命令进出都换一次 `GEditor->Trans`。那样暴露面最大 —— 一百三十
 * 条命令里绝大多数根本不产生事务（查询、截图、编译、切关卡），却全都要走一遍
 * 换进换出。而这条路径出事的代价极高：换出去的那条缓冲失去唯一强引用会被 GC；
 * 切关卡时缓冲里攥着旧关卡的 actor，旧 World 回收不掉，引擎的泄漏检查直接 fatal。
 * 这两个都真的炸过。
 *
 * 换栈的**唯一目的**是让事务落到 agent 的缓冲上。那就只在事务真正存在的那段
 * 时间换 —— 窗口从「整条命令」缩到「这一笔事务」，不产生事务的命令一概不碰。
 *
 * 这也不会漂：不是维护一张「哪些命令算写命令」的名单（名单一定会跟代码脱节），
 * 而是谁开事务谁自动带上。新增写命令照常写 `FUAL_ScopedTransaction`，不用记得
 * 去哪儿登记。
 *
 * ## 顺序靠继承保证
 *
 * C++ 规定基类先于成员构造、后于成员析构。所以：
 *   进：PushScope（换栈） → Inner 构造（Begin 事务）
 *   出：Inner 析构（End 事务） → PopScope（换回去）
 * 事务的 Begin 和 End 必然落在同一条缓冲上，这是不能出错的一条。
 *
 * 用法和 `FScopedTransaction` 完全一样，直接改类名即可。
 *
 * ## 一条硬规则：**不要在事务里做完整的蓝图编译**
 *
 * `FKismetEditorUtilities::CompileBlueprint` 会重新生成关卡里所有实例（销毁旧的、
 * 生成新的）。销毁那一步是 `UWorld::EditorDestroyActor(..., bShouldModifyLevel=true)`，
 * 它在 `GUndo` 非空（＝有事务开着）时会把整个关卡的 Actor 列表快照进当前事务。
 * 于是撤销这一步 = 关卡 Actor 列表被还原成编译前的样子，里面指的全是已经被换掉的
 * 旧对象 —— **关卡里的 Actor 凭空消失**，再保存一次就落盘了。
 *
 * 要在一条命令里既改图又编译，就用 `TOptional<FUAL_ScopedTransaction>` 装着，
 * 编译之前 `Reset()` 把事务先结束掉。现成例子：`Handle_CreateGraphDeclarative`。
 * 骨架编译（`MarkBlueprintAsStructurallyModified`）不重新生成实例，可以留在事务里。
 */
namespace UALTransactionDetail
{
	/** 只为了「先构造、后析构」的排序而存在，别直接用 */
	struct FUndoScopeGuard
	{
		FUndoScopeGuard() { FUAL_AgentUndo::PushScope(); }
		~FUndoScopeGuard() { FUAL_AgentUndo::PopScope(); }

		FUndoScopeGuard(const FUndoScopeGuard&) = delete;
		FUndoScopeGuard& operator=(const FUndoScopeGuard&) = delete;
	};
}

class FUAL_ScopedTransaction : private UALTransactionDetail::FUndoScopeGuard
{
public:
	explicit FUAL_ScopedTransaction(const FText& SessionName, const bool bShouldActuallyTransact = true)
		: Inner(SessionName, bShouldActuallyTransact)
	{
	}

	FUAL_ScopedTransaction(const TCHAR* TransactionContext, const FText& SessionName, UObject* PrimaryObject, const bool bShouldActuallyTransact = true)
		: Inner(TransactionContext, SessionName, PrimaryObject, bShouldActuallyTransact)
	{
	}

	/** 取消这一笔。可重入，和 FScopedTransaction 语义一致 */
	void Cancel() { Inner.Cancel(); }

	/** 还没被取消吗 */
	bool IsOutstanding() const { return Inner.IsOutstanding(); }

private:
	FScopedTransaction Inner;
};
