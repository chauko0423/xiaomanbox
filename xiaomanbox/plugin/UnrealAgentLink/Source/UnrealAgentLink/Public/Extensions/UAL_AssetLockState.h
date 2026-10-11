#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 「这个资产 AI 正在改」—— 把盒子那边的资产锁画到虚幻编辑器里。
 *
 * ## 为什么需要
 *
 * 锁本身只活在盒子进程里（`src/main/agent-v3/core/assetLock.ts`），可用户此刻
 * 眼睛在虚幻编辑器上。他双击打开一个材质、改两个参数、按 Ctrl+S，然后改动被
 * agent 盖掉 —— 全程没有任何一处告诉过他这个资产正被人动着。
 *
 * 这个类**不拦任何操作**，只负责显示。拦保存是另一回事（要翻文件只读位），
 * 的 B 阶段。
 *
 * ## 显示成什么样
 *
 * 两层，从安静到打断：
 *
 *   1. **内容浏览器角标** —— 常驻，扫一眼就知道哪些资产 AI 在动。
 *      走 `FContentBrowserModule::AddAssetViewExtraStateGenerator`，一处代码覆盖
 *      **全部**资产类型：蓝图、材质、网格体、特效、关卡走的都是同一套图标壳，
 *      不需要按类型各写一遍。
 *   2. **改脏时弹一次通知** —— 用户真动手改了才打断。挂引擎的包标脏事件，
 *      靠 `FUAL_TouchedPackages::IsInCommandScope()` 把 agent 自己的改动摘出去。
 *
 * ## 文案的真假红线
 *
 * 锁分两种强度，文案必须跟着走，**不能一律说「不会被保存」**：
 *
 *   - **只锁不拦**（盒子没能翻只读位：工程启用了版本控制、资产还没落盘、
 *     文件原本就只读）—— 用户按 Ctrl+S 是真存得进去的，只是之后会被 agent
 *     盖掉。文案只能说「会互相覆盖」。
 *   - **真拦住了**（只读位翻上去并回读校验过）—— 这时才能说「保存会被拦下」。
 *
 * 哪些属于后者由盒子在 `locks.set` 的 `enforced` 字段里给。说错的代价是用户
 * 照着假话放弃自己的改动 —— 那是我们骗他丢的工作。
 */
class UNREALAGENTLINK_API FUAL_AssetLockState
{
public:
	using FHandlerFunc = TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>;

	/** 模块启动时挂内容浏览器扩展和包标脏事件 */
	static void Initialize();

	/** 模块关闭时摘掉，别留悬空委托 */
	static void Shutdown();

	/** 注册 `locks.set` 命令 */
	static void RegisterCommands(TMap<FString, FHandlerFunc>& CommandMap);

	/**
	 * 全量替换锁列表。
	 *
	 * 盒子每次锁表变动都推一次**全量**而不是增量：增量要求两边状态永远一致，
	 * 而 WebSocket 会断会重连；全量的话任何一次推送都能把这边拉回正确状态。
	 *
	 * @param PackagePaths 全部被锁的包
	 * @param EnforcedPaths 其中只读位真的翻上去了的那些（`PackagePaths` 的子集）
	 */
	static void SetLocked(
		const TArray<FString>& PackagePaths,
		const TArray<FString>& EnforcedPaths,
		bool bLevelLocked);

	/**
	 * 当前这张关卡正被 AI 改着。
	 *
	 * **不是包路径，是个布尔。** Actor 类工具（spawn / 移动 / 改属性）的参数里
	 * 一个资产路径都没有，盒子那边并不知道被改的是哪张关卡；而这边一直知道
	 * （`GetEditorWorldContext().World()`）。所以盒子只说「关卡被锁了」，
	 * 是哪张由我们自己回答。
	 *
	 * 关卡走**软锁**：只挡另一条 AI 会话、只出提示，不翻只读位 —— 用户在主视口里
	 * 干活是常态，把关卡的手动保存也拦下来，代价和收益不成比例。所以它永远进不了
	 * `IsEnforced`，文案自然停在保守那一档（「会互相覆盖」），这正是实情。
	 */
	static bool IsLevelLocked();

	/**
	 * 清空。
	 *
	 * 断线时必须调 —— 盒子没了就没有任何东西还锁着，但角标不会自己消失，
	 * 留在屏幕上就成了一排永远擦不掉的「AI 锁定中」。
	 */
	static void Clear();

	/** 某个包在不在锁里。`PackageName` 形如 `/Game/Materials/M_Rock` */
	static bool IsLocked(FName PackageName);

	/** 这个包的只读位是不是真翻上去了。决定文案说「会互相覆盖」还是「保存会被拦下」 */
	static bool IsEnforced(FName PackageName);

	/**
	 * 给用户看的那句解释，**分两档**（只锁不拦 / 真拦住了）。
	 *
	 * 角标、通知、编辑器窗口里的横幅共用这一份 —— 同一件事在三个地方说成三样
	 * 是最容易出假话的地方。
	 */
	static FText DescribeLock(FName PackageName);

	/** 胶囊第一行：只说「怎么了」，短到不换行 */
	static FText ShortLabel(FName PackageName);

	/**
	 * 胶囊第二行：只说「你该怎么办」。
	 *
	 * 关卡视口那枚是 `HitTestInvisible`（不能吃掉用户对场景的点击），而 Slate 的
	 * tooltip 靠命中测试触发 —— 那一枚永远弹不出 tooltip。没有这一行的话，
	 * 用户在关卡上只看得到「已锁定」，看不到「等它做完 / 去盒子里停掉」。
	 */
	static FText HintLine(FName PackageName);

	/**
	 * 锁列表变了。
	 *
	 * `FUAL_AssetLockBanner` 拿它当第三个「补挂横幅」的时机：视口布局一重建就会
	 * 把叠加层连横幅一起丢掉，而 agent 干活期间锁状态本来就在不停变，蹭这个事件
	 * 就不用再养一条常驻 ticker。走委托而不是让这边直接调那边，是为了不让
	 * State 反过来依赖 Banner。
	 */
	static FSimpleMulticastDelegate& OnChanged();

	/** 锁了几个（诊断用） */
	static int32 Num();
};
