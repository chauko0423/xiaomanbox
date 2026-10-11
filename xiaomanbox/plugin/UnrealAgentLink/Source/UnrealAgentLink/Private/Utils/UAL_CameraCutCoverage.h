#pragma once

#include "CoreMinimal.h"

/**
 * 相机切轨的覆盖判定 —— 纯算术，不碰任何 UObject。
 *
 * ## 为什么单独一个文件
 *
 * 这套区间算术原来在 `UAL_SequencerCommands.cpp` 里抄了两份：
 * `InspectCameraCuts`（给 describe / camera_cuts 用）和 `AuditOne`（给 audit 用）。
 * 两份里的 bug 也是同一个 —— 判空隙时拿的是「前一个段的 end」而不是
 * 「到目前为止的最大 end」，于是嵌套的段（`[0,100)` 里套一个 `[10,20)`）
 * 会被报出一段根本不存在的空隙。
 *
 * 抽出来之后它只吃整数、只吐整数，可以脱离引擎直接测
 * （见 `Private/Tests/UAL_CameraCutCoverageTests.cpp`）。而一段
 * **有四次回归记录、两份实现、零个测试**的算术，是这个工具集里最该先钉住的东西。
 *
 * ## 区间约定
 *
 * 一律闭开 `[Start, End)`，和 `SectionRange()` 上报给模型的口径一致。
 * 所以 `[0,300)` 和 `[300,600)` 是**首尾相接**，不是空隙，也不是重叠。
 *
 * ## 判不出来就说判不出来
 *
 * 切轨上有开区间的段（一头无界）时，它到底盖到哪里算不出来。这时候
 * **不能报「没盖满」** —— 写入侧会照着这个结论去删用户的段。
 * 所以覆盖结论带一个 `bCoverageKnown`，判不出来时把覆盖相关的输出全清空，
 * 和这个文件对门的 `GetSequencerResolveWorld()` 是同一条原则：
 * 说不知道，好过说一个错的答案。
 */
namespace UAL_CameraCutCoverage
{
	/** 一个段在时间线上占的位置，闭开区间 [Start, End)，单位是 display 帧 */
	struct FCutRange
	{
		int32 Start = 0;
		int32 End = 0;

		bool operator==(const FCutRange& Other) const
		{
			return Start == Other.Start && End == Other.End;
		}
	};

	struct FCutCoverage
	{
		/** 段与段之间的洞，已裁到播放范围内。渲出来就是黑帧 */
		TArray<FCutRange> Gaps;
		/** 段与段的重叠。渲哪台相机是未定义的 */
		TArray<FCutRange> Overlaps;
		/** 播放范围开头没被盖住的部分 */
		TOptional<FCutRange> HeadUncovered;
		/** 播放范围结尾没被盖住的部分 */
		TOptional<FCutRange> TailUncovered;

		/** 盖满了整个播放范围。只有 bCoverageKnown 为真时才有意义 */
		bool bCoversPlayback = false;

		/**
		 * 覆盖情况判得出来吗。
		 *
		 * false 时 `bCoversPlayback` / `HeadUncovered` / `TailUncovered` / `Gaps`
		 * 全部不可信，而且已经被清空 —— **不要把「空的 Gaps」读成「没有空隙」**。
		 * `Overlaps` 不受影响：查到的重叠在任何情况下都是真的。
		 */
		bool bCoverageKnown = true;

		/** 判不出来的原因，可以直接给人看。bCoverageKnown 为真时是空串 */
		FString UnknownReason;
	};

	/**
	 * @param Ranges          切轨上两端都有界的段。顺序无所谓，内部会排
	 * @param PlaybackStart   播放范围起点（含）
	 * @param PlaybackEnd     播放范围终点（不含）
	 * @param UnboundedCount  因为一头无界而没能放进 Ranges 的段数。
	 *                        大于 0 就判不出覆盖 —— 无界的那头可能正好把洞填上了
	 */
	inline FCutCoverage Analyse(
		TArray<FCutRange> Ranges, int32 PlaybackStart, int32 PlaybackEnd, int32 UnboundedCount = 0)
	{
		FCutCoverage Result;

		// 两级排序。只按 Start 排的话，同起点的两段谁在前是不定的
		// （TArray::Sort 是 introsort，不保证稳定），结论会随机漂
		Ranges.Sort([](const FCutRange& A, const FCutRange& B)
		{
			return A.Start != B.Start ? A.Start < B.Start : A.End < B.End;
		});

		// 重叠只看段和段，和播放范围没关系，所以任何情况下都照报
		if (Ranges.Num() > 0)
		{
			int32 Reach = Ranges[0].End;
			for (int32 Index = 1; Index < Ranges.Num(); ++Index)
			{
				if (Ranges[Index].Start < Reach)
				{
					Result.Overlaps.Add({ Ranges[Index].Start, FMath::Min(Reach, Ranges[Index].End) });
				}
				Reach = FMath::Max(Reach, Ranges[Index].End);
			}
		}

		if (UnboundedCount > 0)
		{
			Result.bCoverageKnown = false;
			Result.UnknownReason = FString::Printf(
				TEXT("有 %d 个切轨段的时间范围有一头是无界的，算不出它盖到哪里"), UnboundedCount);
			return Result;
		}

		if (PlaybackEnd <= PlaybackStart)
		{
			// 引擎自己会把损坏或无界的播放范围强制成 [0, 0)
			// （UMovieScene::UpgradeTimeRanges，编辑器每次加载都跑）。
			// 空的范围没有「盖没盖满」可言 —— 不能顺着答「没盖满」
			Result.bCoverageKnown = false;
			Result.UnknownReason = FString::Printf(
				TEXT("播放范围是 [%d, %d)，本身就是空的"), PlaybackStart, PlaybackEnd);
			return Result;
		}

		if (Ranges.Num() == 0)
		{
			Result.HeadUncovered = FCutRange{ PlaybackStart, PlaybackEnd };
			return Result;
		}

		if (Ranges[0].Start > PlaybackStart)
		{
			Result.HeadUncovered = FCutRange{ PlaybackStart, FMath::Min(Ranges[0].Start, PlaybackEnd) };
		}

		// Reach = 到目前为止所有段盖到的最远处。**不是前一个段的 end** ——
		// 这正是原来两份实现共同的 bug：`[0,100)` 里套一个 `[10,20)` 时，
		// 拿 20 去和下一段的 100 比，会凭空报出一段 [20,100) 的空隙
		int32 Reach = Ranges[0].End;
		for (int32 Index = 1; Index < Ranges.Num(); ++Index)
		{
			if (Ranges[Index].Start > Reach)
			{
				// 播放范围之外的洞渲不到，不算问题，裁掉
				const int32 From = FMath::Max(Reach, PlaybackStart);
				const int32 To = FMath::Min(Ranges[Index].Start, PlaybackEnd);
				if (To > From)
				{
					Result.Gaps.Add({ From, To });
				}
			}
			Reach = FMath::Max(Reach, Ranges[Index].End);
		}

		if (Reach < PlaybackEnd)
		{
			Result.TailUncovered = FCutRange{ FMath::Max(Reach, PlaybackStart), PlaybackEnd };
		}

		Result.bCoversPlayback =
			!Result.HeadUncovered.IsSet() && !Result.TailUncovered.IsSet() && Result.Gaps.Num() == 0;

		return Result;
	}
}
