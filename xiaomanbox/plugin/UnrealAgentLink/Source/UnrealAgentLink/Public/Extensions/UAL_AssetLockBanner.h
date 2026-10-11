#pragma once

#include "CoreMinimal.h"

/**
 * 「AI 正在改这个资产」的**窗口内**提示 —— 资产锁 C 阶段第三、四步。
 *
 * 内容浏览器的角标（`FUAL_AssetLockState`）解决的是「在资产列表里一眼看到」，
 * 但用户真正会动手的地方是**打开着的那个编辑器窗口**：他双击进了材质编辑器，
 * 视线在图表上，根本不会回头看内容浏览器。这个类负责把提示送到他眼前。
 *
 * ## 两个面，覆盖全部编辑器
 *
 * | 面 | 引擎接口 | 覆盖 |
 * | --- | --- | --- |
 * | 资产编辑器工具栏上的常驻横幅 | `UToolMenus` 扩 `AssetEditor.DefaultToolBar` | **全部**资产编辑器 |
 * | 关卡视口左上角的悬浮横幅 | `IToolkitHost::AddViewportOverlayWidget` | 关卡编辑器 |
 *
 * 第一条之所以能一网打尽：`FAssetEditorToolkit::GenerateToolbar` 给每个资产
 * 编辑器的工具栏都指定 `AssetEditor.DefaultToolBar` 当父菜单，并往上下文里塞了
 * `UAssetEditorToolkitMenuContext`（里面就是这个窗口在编辑哪些对象）。
 * 所以扩父菜单一次 = 蓝图、材质、网格体、Niagara、UMG、序列…… 全都有，
 * 不需要按编辑器类型各写一遍，将来引擎加了新编辑器也自动带上。
 *
 * ## 两个面的文案不一样，因为能不能弹 tooltip 不一样
 *
 * 工具栏那条是可命中的，短标签在横幅上、完整说明走 tooltip。关卡视口那条是
 * `HitTestInvisible`（不能吃掉用户对场景的点击），而 Slate 的 tooltip 靠命中
 * 测试触发 —— 挂上去也永远弹不出来，所以完整说明必须直接写在可见文本里。
 *
 * ## 为什么没做状态栏
 *
 * 设计里原本还列了「窗口底部状态栏一行字」（`UStatusBarSubsystem::PushStatusBarMessage`）。
 * 实现时放弃了：那个消息是一个**栈**，最上面那条盖住下面所有的。我们的锁提示
 * 要一直挂着（几分钟），推上去就等于在这段时间里把引擎自己的状态栏消息全压掉。
 * 为了多一个提示面而抢掉一个共享的显示位，不划算 —— 何况工具栏横幅比状态栏显眼。
 */
class UNREALAGENTLINK_API FUAL_AssetLockBanner
{
public:
	/** 挂工具栏扩展和关卡编辑器创建事件 */
	static void Initialize();

	/** 摘掉，别留悬空回调 */
	static void Shutdown();
};
