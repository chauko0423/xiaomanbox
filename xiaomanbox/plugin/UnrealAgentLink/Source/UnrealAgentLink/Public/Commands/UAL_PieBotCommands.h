#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 试玩机器人的感知与动作原语。
 *
 * ## 决策不在这里
 *
 * 盒子那侧的 `ue_autoplay` 在 `pie.run` 跑着的时候循环调这几条：看一眼（observe）→
 * 决定下一步 → 转向 / 注入 / 点按钮 → 再看一眼。**策略写在 TS 里**，插件只给原语。
 * 这样策略能换（规则、判定模型、大模型），每一步的状态和选择也都在盒子那侧落盘，
 * 以后拿同一批决策点回放比较不同的策略。
 *
 * ## 都只作用于正在跑的游戏世界
 *
 * 游戏没跑时一律回 409，不退回编辑器世界 —— 「玩家在哪」这个问题在编辑器世界里
 * 没有答案，退回去只会给出一个看着合理的错误位置。
 *
 * 支持的命令：
 * - pie.observe      玩家 pawn / 输入模式 / 屏幕上的按钮 / 点名 Actor 的位置 / 增量日志
 * - pie.set_view     直接设控制器朝向（绕开鼠标灵敏度，机器人要的是「朝哪」不是「甩多少」）
 * - pie.click_widget 触发屏幕上某个按钮的 OnClicked
 * - pie.nav_path     导航网格寻路 / 随机可达点
 * - pie.scene        整关的玩法对象：蓝图变量、事件名、触发区、导航可达性
 * - pie.move_to      连续移动（逐帧执行，不再一米一停）/ pie.move_stop
 * - pie.plan_path    没有导航网格时的网格 A* 路径规划
 */
class FUAL_PieBotCommands
{
public:
	using FHandlerFunc = TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>;

	static void RegisterCommands(TMap<FString, FHandlerFunc>& CommandMap);

	static void Handle_Observe(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	static void Handle_SetView(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * 直接广播按钮的 OnClicked，**不走 Slate 命中测试**。
	 *
	 * 所以它证明的是「点了之后逻辑对不对」，证明不了「玩家点得到」——
	 * 按钮被别的层挡住、尺寸为零、在屏幕外，这里照样点得动。返回里带这句免责，
	 * 和动作层注入（input.inject_action）绕开键位映射是同一类事。
	 */
	static void Handle_ClickWidget(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	static void Handle_NavPath(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * pie.scene —— 整个游戏世界里的玩法对象（不限距离）：类链、标签、组件、接口、
	 * 蓝图事件名、蓝图变量当前值、是不是触发区、从玩家走过去走不走得到；外加玩家自己的蓝图变量。
	 *
	 * 只读。机器人照样只按玩家的方式行动（移动、输入），这份清单是它的「世界知识」。
	 */
	static void Handle_Scene(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * pie.move_to —— 沿一串路径点连续移动（逐帧平滑转向、跑动中起跳、边缘刹停、被挡报告）。
	 * 立刻返回；进度在 pie.observe 的 move 字段里。新的 move_to 会替换正在进行的那个。
	 */
	static void Handle_MoveTo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	static void Handle_MoveStop(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	static TSharedPtr<FJsonObject> BuildMoveStatus();

	/**
	 * pie.plan_path —— 没有导航网格时的路径规划：角色与目标周围铺网格，逐格判断有没有地、
	 * 站不站得下，相邻格按台阶 / 跳跃 / 落差定通不通，A* 找路再拉直成拐点。
	 */
	static void Handle_PlanPath(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
};
