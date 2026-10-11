#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Sequencer 命令处理器。
 *
 * ## 为什么这套东西从 Python 搬到了 C++
 *
 * 一句话：跨 5.0–5.8 的正确性要有机制保证，C++ 是编译失败（发版前，我们机器上），
 * Python 是 AttributeError 或者静默返回别的东西（用户报 bug 时，如果他肯报）。
 *
 * 搬迁过程本身给这个判断补了两条实证 —— 都是 Python 侧探测不出、
 * 编译器一眼看见的：
 *
 *   1. `FMovieSceneBinding::GetName()` 在 5.7 被弃用，返回的是
 *      `BindingName_DEPRECATED`。它**编得过、跑得动、返回空串** ——
 *      这正是「静默返回别的东西」的教科书案例。
 *      正解是按 GUID 查 `FindPossessable` / `FindSpawnable`，
 *      两者在 4.27–5.8 全程非弃用。见 `ResolveBindingName`。
 *   2. `UMovieSceneTrack::AddSection` 的签名从 `UMovieSceneSection&`（4.27）
 *      变成了 `FSectionParameter`（5.6+）。但 `FSectionParameter` 有一个
 *      非 explicit 的转换构造函数，所以 `Track->AddSection(*Section)`
 *      两边都编得过 —— **不需要 `#if`**。查过才知道，不查就会白加一层分支。
 *
 * ## 边界：只诊断，不出片
 *
 * 不做渲染出片。渲染是不可逆的资源消耗（几小时机器时间、几十 GB 磁盘），
 * 输出设置有专业判断，交付责任必须在人。。
 *
 * ## 帧的两套刻度，别搞混
 *
 * MovieScene 内部一切时间都存在 **tick resolution**（通常 24000/1），
 * 而用户和 TS 侧说的帧是 **display rate**（通常 24/1 或 30/1）。
 * 两者差三个数量级 —— 弄反了不会报错，只会得到一个大一千倍的帧号。
 *
 * 所有对外的帧号一律走 `TickToDisplay()` 转换。**不要直接把
 * `FFrameNumber::Value` 塞进 JSON。**
 *
 * ## JSON 形状是契约
 *
 * 每条命令的返回必须与 TS 侧的 interface 逐字对应：
 *   - `sequence.describe`    → `ue-sequencer/describe.ts` 的 `DescribeOutput`
 *   - `sequence.audit`       → `ue-sequencer/audit.ts` 的 `AuditOutput`
 *   - `sequence.camera_keys` → `ue-sequencer/cameraKeys.ts` 的 `CameraKeysOutput`
 *   - `sequence.camera_cuts` → `ue-sequencer/cameraCuts.ts` 的 `CameraCutsOutput`
 *
 * 迁移的整个前提是 **TS 逻辑层一行不动**（`retimePlan.ts` / `findings.ts` /
 * 各工具的格式化函数）。字段名漂一个字，那层就白留了。
 */
class FUAL_SequencerCommands
{
public:
	/** 注册所有 sequence.* 命令 */
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	// ========================================================================
	// 只读
	// ========================================================================

	/**
	 * sequence.describe —— 读一条 Level Sequence 的结构。
	 *
	 * 请求：
	 *   - sequence_path : 资产路径（必填）
	 *   - detail        : "outline" | "tracks" | "keys"
	 *   - bindings      : 只看这几个绑定（按名字）。detail=keys 时 TS 侧已强制必填
	 *   - max_bindings  : 绑定数上限，超出截断并置 truncated
	 *   - max_keys      : 每通道关键帧上限，同上
	 *
	 * 响应：见 `DescribeOutput`。
	 */
	static void Handle_Describe(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * sequence.audit —— 出片前体检。
	 *
	 * 比 describe 多的是**递归进子序列**，以及把问题按严重程度分级。
	 * 只诊断，不修 —— 修什么、改不改由人定。
	 */
	static void Handle_Audit(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// ========================================================================
	// 写入
	// ========================================================================

	/**
	 * sequence.camera_keys —— 写相机关键帧。
	 *
	 * 任意运镜都走这里：TS 侧把轨迹算成一串 `[frame, location, rotation]`，
	 * 这边只负责把它们写进通道。**工具不预设镜头形状**
	 * （原来的 `sequence_orbit` 把「镜头只能是圆」烧进了参数，已删）。
	 *
	 * 会清掉覆盖范围内的旧关键帧，并在响应里报出清掉了多少个 ——
	 * 危不危险由用户在审批环节判断，工具的责任是把代价说清楚。
	 */
	static void Handle_CameraKeys(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * sequence.camera_cuts —— 给已有序列补相机切轨。
	 *
	 * 没有切轨 = 渲出来全黑，这是社区里最高频的坑
	 * 。
	 */
	static void Handle_CameraCuts(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
};
