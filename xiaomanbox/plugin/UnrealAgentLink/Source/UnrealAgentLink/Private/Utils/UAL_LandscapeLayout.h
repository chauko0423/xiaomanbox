#pragma once

#include "CoreMinimal.h"

/**
 * 地形尺寸换算：把「想要多少格」换成编辑器新建面板里合法的那几档组合。
 *
 * 单独成文件、不依赖引擎对象，是为了能单测（`Tests/UAL_LandscapeLayoutTests.cpp`）——
 * 回执里「要 1000 米、实际 1008 米」全靠它，换算错了只有开编辑器才看得出来。
 */
namespace UALLandscapeLayout
{
	/**
	 * 一个分区（section）几格、一块（component）几个分区。
	 *
	 * 合法值就是编辑器新建面板两个下拉框里那几档（`FLandscapeConfig` 的
	 * `SubsectionSizeQuadsValues` / `NumSectionValues`）。**顺序就是偏好**：
	 * Epic 推荐尺寸表（505 / 1009 / 2017 / 4033 用 63×2，8129 用 127×2）排最前，
	 * 过小的分区排最后 —— 分区越小块越多，渲染和剔除的开销越大。
	 */
	struct FSectionChoice
	{
		int32 QuadsPerSection;
		int32 SectionsPerComponent;
	};

	constexpr FSectionChoice SectionPreference[] = {
		{63, 2}, {127, 2}, {63, 1}, {127, 1}, {31, 2}, {255, 2},
		{31, 1}, {255, 1}, {15, 2}, {15, 1}, {7, 2}, {7, 1}};

	/** 每个方向最多 32 块 —— 编辑器面板和引擎导入都卡这个数 */
	constexpr int32 MaxComponentsPerAxis = 32;

	/**
	 * 每个方向最多多少个顶点。Epic 推荐尺寸表的最大一档是 8129，这里放到 8161
	 * （255×1×32+1）。再大就进了编辑器会走「分区域建」的范围 —— 那条路要先存盘、
	 * 不能撤销，第一版不做。TS 侧的 schema（`ue-landscape/index.ts` 的 `MAX_QUADS_PER_AXIS`）用同一个数挡在前面。
	 */
	constexpr int32 MaxResolutionPerAxis = 8161;

	struct FLandscapeLayout
	{
		int32 QuadsPerSection = 63;
		int32 SectionsPerComponent = 1;
		FIntPoint ComponentCount = FIntPoint(1, 1);

		int32 QuadsPerComponent() const { return QuadsPerSection * SectionsPerComponent; }
		FIntPoint Resolution() const
		{
			return FIntPoint(ComponentCount.X * QuadsPerComponent() + 1, ComponentCount.Y * QuadsPerComponent() + 1);
		}
	};

	/**
	 * 把「想要多少格」换算成离它最近的合法组合。
	 *
	 * 三轮，每轮都按偏好顺序取第一个满足条件的：
	 *   1. 恰好相等 —— 高度图尺寸正好合法时一个像素都不重采样
	 *   2. 每个方向误差在 3% 以内 —— 「1 公里」给 1008 米而不是去凑 7 格一个分区
	 *   3. 误差最小的那个
	 *
	 * 引擎自己的 `FLandscapeImportHelper::ChooseBestComponentSizeForImport` 只做
	 * 「恰好相等，否则往大凑」，1024 的高度图会被凑成 1072 —— 多拉伸 5%。
	 */
	inline FLandscapeLayout PickLayout(int32 TargetQuadsX, int32 TargetQuadsY)
	{
		TargetQuadsX = FMath::Max(1, TargetQuadsX);
		TargetQuadsY = FMath::Max(1, TargetQuadsY);
		const int32 ToleranceX = FMath::Max(1, TargetQuadsX * 3 / 100);
		const int32 ToleranceY = FMath::Max(1, TargetQuadsY * 3 / 100);

		struct FCandidate
		{
			FLandscapeLayout Layout;
			int32 ErrorX;
			int32 ErrorY;
		};
		TArray<FCandidate> Candidates;
		for (const FSectionChoice& Choice : SectionPreference)
		{
			const int32 PerComponent = Choice.QuadsPerSection * Choice.SectionsPerComponent;
			const int32 MaxCount = FMath::Min(MaxComponentsPerAxis, (MaxResolutionPerAxis - 1) / PerComponent);
			if (MaxCount < 1)
			{
				continue;
			}
			FLandscapeLayout Layout;
			Layout.QuadsPerSection = Choice.QuadsPerSection;
			Layout.SectionsPerComponent = Choice.SectionsPerComponent;
			Layout.ComponentCount.X = FMath::Clamp(FMath::RoundToInt((double)TargetQuadsX / PerComponent), 1, MaxCount);
			Layout.ComponentCount.Y = FMath::Clamp(FMath::RoundToInt((double)TargetQuadsY / PerComponent), 1, MaxCount);
			Candidates.Add({Layout,
				FMath::Abs(Layout.ComponentCount.X * PerComponent - TargetQuadsX),
				FMath::Abs(Layout.ComponentCount.Y * PerComponent - TargetQuadsY)});
		}

		for (const FCandidate& C : Candidates)
		{
			if (C.ErrorX == 0 && C.ErrorY == 0)
			{
				return C.Layout;
			}
		}
		for (const FCandidate& C : Candidates)
		{
			if (C.ErrorX <= ToleranceX && C.ErrorY <= ToleranceY)
			{
				return C.Layout;
			}
		}
		const FCandidate* Best = nullptr;
		for (const FCandidate& C : Candidates)
		{
			if (!Best || C.ErrorX + C.ErrorY < Best->ErrorX + Best->ErrorY)
			{
				Best = &C;
			}
		}
		return Best ? Best->Layout : FLandscapeLayout();
	}
}
