#pragma once

#include "CoreMinimal.h"

class UTransBuffer;

/**
 * 给 agent 单开一条撤销栈。
 *
 * ## 解决的是什么
 *
 * 编辑器只有一条全局撤销栈（`GEditor->Trans`）。agent 干一轮活动辄三十步，
 * 全压在这条栈上，带来三个问题：
 *
 *   1. 用户想撤销**自己**十分钟前手动做的操作，得先按三十次 Ctrl+Z；
 *   2. 「把 AI 刚做的全撤了」没有对应操作 —— 用户不知道该按几次；
 *   3. 没法从栈上分辨哪几步是 agent 干的。
 *
 * 换一条栈就三个一起解决：agent 的事务全进自己的缓冲，用户的 Ctrl+Z 碰不到；
 * 「全撤」= 把自己这条栈倒着走完；「agent 改了哪些资产」= 遍历自己这条栈。
 *
 * ## 怎么换的，以及**什么时候**换
 *
 * `GEditor->Trans` 是个普通的 UPROPERTY 指针，赋值即换。
 *
 * 换的窗口就是**一笔事务的生命周期** —— 由 `FUAL_ScopedTransaction` 开关，
 * 见 UAL_ScopedTransaction.h。不是「一条命令的生命周期」：换栈的唯一目的是让
 * 事务落到 agent 的缓冲上，那些根本不产生事务的命令（查询、截图、编译、切关卡）
 * 没有任何理由碰这套逻辑，碰了只是白扛风险。
 *
 * 用户在编辑器里手动操作时不经过这里，走的还是原来那条栈。
 *
 * ## 换的时候必须避开正开着的事务
 *
 * 一笔事务的 Begin 和 End 必须落在同一个缓冲上。所以进出作用域时都先看
 * `IsActive()`：有事务开着就**不换**（进），或者**先不换回去**（出，等下一条
 * 命令收尾时再还）。宁可让 agent 缓冲多顶一会儿，也不能把两条栈搅在一起。
 *
 * ## 已知边界：交叉编辑
 *
 * agent 改了 A，用户接着手动也改了 A，这时撤销 agent 那一步会把对象整体还原到
 * agent 动手之前 —— 用户那次修改跟着没了。这是「两条独立撤销栈」这个模型固有的，
 * 换谁做都一样。所以 `editor.undo` 会把受影响的包列出来，让人先看清楚再决定。
 *
 * ## 换地图时清空
 *
 * 事务里存着对象引用。地图一换，旧关卡的对象全没了，这时候撤销等于往废内存里写。
 * 所以挂了 `FEditorDelegates::OnMapOpened`，一换图就把 agent 缓冲整个清掉。
 */
class UNREALAGENTLINK_API FUAL_AgentUndo
{
public:
	/** 模块启动时挂事件（缓冲本身是第一次用到才建的，那时 GEditor 才一定就绪） */
	static void Initialize();
	/** 模块关闭时还原全局缓冲、摘事件、放掉 GC 根 */
	static void Shutdown();

	/**
	 * 事务作用域进出，允许嵌套；只在最外层真正换缓冲。
	 *
	 * 别直接调 —— 用 `FUAL_ScopedTransaction`，它靠构造/析构顺序保证
	 * 事务的 Begin 和 End 落在同一条缓冲上。
	 */
	static void PushScope();
	static void PopScope();

	/** agent 缓冲此刻是否顶替着 `GEditor->Trans` */
	static bool IsOverrideActive();

	/** 一步撤销记录 */
	struct FEntry
	{
		/** 0 是最早的一步 */
		int32 Index = 0;
		/** 事务标题，就是编辑器撤销菜单里显示的那句话 */
		FString Title;
		/** 事务来源上下文（引擎填的，比如某个编辑器模块名） */
		FString Context;
		/** 这一步动过的包（已排除临时包） */
		TArray<FString> Packages;
		/**
		 * 这一步动过的对象，用用户认得出的名字（Actor 取大纲里的标签）。
		 *
		 * 只有标题和包名的话，一条「批量修改Actor变换 · /Game/Maps/NewWorld」
		 * 谁也看不出动的是哪几个物件 —— 想只回滚别人越界改的那几步就无从下手。
		 */
		TArray<FString> Objects;
	};

	/** 还能撤的那些步骤，从早到晚。缓冲还没建起来时返回空 */
	static TArray<FEntry> DescribeUndoable();

	/** 还能撤几步 */
	static int32 UndoableCount();

	/** 撤销之后还能重做几步 */
	static int32 RedoableCount();

	/**
	 * agent 改过、且还没被撤掉的包路径。
	 *
	 * 存在的理由是补 `FUAL_TouchedPackages` 的洞：那边靠「包被标脏」事件 +
	 * 命令作用域记账，命令里派发出去、后续帧才落地的**异步写**落在作用域之外，
	 * 记不到。撤销栈没这个问题 —— 事务什么时候结束就什么时候进栈，跟帧无关。
	 *
	 * 只回报**水位线之后**的事务，见 `MarkSeen`。
	 */
	static TSet<FString> CollectTouchedPackagePaths();

	/**
	 * 把「到此为止的都已经处理过了」记下来。
	 *
	 * `editor.save` 存完之后会调它。不记水位线的话，agent 早先改过又已经存盘的
	 * 资产会一直留在栈上；等用户后来自己手动改脏同一个资产，它又会被算成
	 * 「agent 改的」而被下一次保存带走 —— 那正是这套记账要避免的事。
	 */
	static void MarkSeen();

	/**
	 * 这一次撤销/重做实际动了哪些包、哪些对象。
	 *
	 * 必须**在动作之前**从即将被撤/重做的那几笔事务上取：动完之后它们就移出
	 * 活跃区了，`CollectTouchedPackagePaths` 扫不到 —— 那正是 2026-09-16 那次
	 * 「撤销把关卡里的宝箱删了，返回体一个字没提」的直接原因。
	 */
	struct FTouched
	{
		/** 需要重新保存的包（撤销/重做把它们改脏了） */
		TArray<FString> Packages;
		/** 动过的对象，用大纲里的名字。「哪个 Actor 变了」只能从这里看出来 */
		TArray<FString> Objects;
	};

	/**
	 * 撤销 Steps 步（Steps <= 0 表示全撤）。
	 *
	 * 只动 agent 自己那条栈，用户的撤销历史一步都不碰。
	 * 撤销保留重做点（`bCanRedo=true`），撤多了可以用 Redo 走回来。
	 *
	 * @param OutTitles 实际撤掉的每一步的标题，从新到旧
	 * @param OutTouched 这几步动过的包和对象，取自撤销**之前**的事务内容
	 * @return 真正撤掉了几步
	 */
	static int32 Undo(int32 Steps, TArray<FString>& OutTitles, FTouched& OutTouched, FString& OutError);

	/** 重做 Steps 步（Steps <= 0 表示全部重做回来）。参数语义同 Undo */
	static int32 Redo(int32 Steps, TArray<FString>& OutTitles, FTouched& OutTouched, FString& OutError);

	/** 清空 agent 缓冲：撤销点全丢，**已经做出的改动保留** */
	static void Reset(const FString& Reason);

	/**
	 * agent 撤销栈上有几步动过这些包（撤销区 + 重做区都算）。
	 *
	 * ## 为什么删资产之前非问这一句不可
	 *
	 * 事务里存着**对象的强引用**，agent 改过的资产因此一直被这条栈拽着。
	 * 而引擎判断「谁拽着它」时只认**当前装着的**那条缓冲（`GEditor->Trans`）是
	 * 撤销缓冲（`ObjectTools::GatherObjectReferencersForDeletion` 里
	 * `Transactor == Referencer` 那一句）—— agent 这条平时不装着，于是被算成
	 * 一个普通的外部引用者，`ForceDeleteObjects` 直接判「资产正在使用中」而放弃。
	 *
	 * 2026-09-16 真机上就是这样：删一个 agent 自己刚建的测试蓝图，引擎日志里
	 * `TransBuffer_1 (root) (43)` 引用着它，删除返回 0，而回给模型的原因是
	 * 「多半开着编辑器标签页或被别的资产引用」—— 三条猜测全不对，模型换 Python
	 * 再删还是同一堵墙。
	 */
	static int32 CountStepsTouching(const TArray<FString>& PackageNames);

	/**
	 * 把动过这些包的撤销步骤从 agent 栈上摘掉，**其余步骤留着**。
	 *
	 * 用户自己的撤销历史（引擎那条缓冲）一步都不碰。
	 *
	 * ## 摘掉之后剩下的历史还准吗
	 *
	 * 撤销是一串差量，不是一堆独立条目。摘掉中间一步之后：比它更早的步骤照样
	 * 撤得回去（每笔事务存的是自己动手前的快照），但**只在被摘那步里出现过的
	 * 对象**会永远停在那一步之后的状态 —— 对正要被删掉的资产本身无所谓，
	 * 对同一步里被顺带改到的**别的**对象则是实打实的损失。
	 *
	 * 所以这件事只在调用方明确要求时做，并且要把摘掉的步骤标题回报给用户。
	 *
	 * @param OutTitles 被摘掉的步骤标题，从新到旧
	 * @return 摘掉几步
	 */
	static int32 DropStepsTouching(const TArray<FString>& PackageNames, TArray<FString>& OutTitles);

	/**
	 * 换关卡**之前**必须调：清空两条撤销缓冲，并把引擎自己那条还回 GEditor->Trans。
	 *
	 * 不调的话切关卡稳定崩溃 —— 缓冲里存着引用旧关卡 actor 的事务，
	 * 旧 World 回收不掉，引擎的「World Memory Leaks」检查直接 fatal。
	 * 已挂在 FCoreUObjectDelegates::PreLoadMap 上，level.open 里也会显式调一次。
	 */
	static void PrepareForMapChange();

private:
	/** 第一次用到时才建缓冲。GEditor 不在（比如 -nullrhi 的命令行环境）时返回 nullptr */
	static UTransBuffer* EnsureBuffer();
};
