#include "UAL_CameraCutCoverage.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

using UAL_CameraCutCoverage::Analyse;
using UAL_CameraCutCoverage::FCutCoverage;
using UAL_CameraCutCoverage::FCutRange;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUALCameraCutCoverageTest,
    "UnrealAgentLink.Sequencer.CameraCutCoverage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUALCameraCutCoverageTest::RunTest(const FString& Parameters)
{
    auto CountsMatch = [this](const TCHAR* What, const TArray<FCutRange>& Actual, int32 Expected)
    {
        return TestEqual(What, Actual.Num(), Expected);
    };
    auto RangeIs = [this](const TCHAR* What, const FCutRange& Actual, int32 Start, int32 End)
    {
        TestEqual(FString(What) + TEXT(" start"), Actual.Start, Start);
        TestEqual(FString(What) + TEXT(" end"), Actual.End, End);
    };

    // ── 正常情况：段首尾相接，盖满播放范围 ────────────────────────────────
    //
    // 闭开区间下 [0,300) 和 [300,600) 是相接的。把它当成重叠或空隙，
    // 用户会被叫去修一条本来就是对的序列
    {
        const FCutCoverage C = Analyse({ { 0, 300 }, { 300, 600 } }, 0, 600);
        TestTrue(TEXT("相接：判得出来"), C.bCoverageKnown);
        TestTrue(TEXT("相接：盖满了"), C.bCoversPlayback);
        CountsMatch(TEXT("相接：没有空隙"), C.Gaps, 0);
        CountsMatch(TEXT("相接：没有重叠"), C.Overlaps, 0);
    }

    // 输入顺序不该影响结论
    {
        const FCutCoverage C = Analyse({ { 300, 600 }, { 0, 300 } }, 0, 600);
        TestTrue(TEXT("乱序输入：盖满了"), C.bCoversPlayback);
        CountsMatch(TEXT("乱序输入：没有空隙"), C.Gaps, 0);
    }

    // ── 一帧空隙 ──────────────────────────────────────────────────────────
    //
    // 时间线上肉眼看不出来，渲出来就是一帧黑
    {
        const FCutCoverage C = Analyse({ { 0, 300 }, { 301, 600 } }, 0, 600);
        TestFalse(TEXT("一帧空隙：没盖满"), C.bCoversPlayback);
        if (CountsMatch(TEXT("一帧空隙：报一处"), C.Gaps, 1))
        {
            RangeIs(TEXT("一帧空隙"), C.Gaps[0], 300, 301);
        }
    }

    // ── 回归：嵌套的段不许报出假空隙 ──────────────────────────────────────
    //
    // 原来两份实现都拿「前一个段的 end」去比下一段的 start。排序后是
    // [0,100) [10,20) [100,200)，于是拿 20 和 100 比，凭空报出 [20,100) 的空隙。
    // 后果不只是误报：camera_cuts 看到有空隙就会把整条切轨删了重建
    {
        const FCutCoverage C = Analyse({ { 0, 100 }, { 10, 20 }, { 100, 200 } }, 0, 200);
        CountsMatch(TEXT("嵌套段：没有空隙"), C.Gaps, 0);
        TestTrue(TEXT("嵌套段：盖满了"), C.bCoversPlayback);
        if (CountsMatch(TEXT("嵌套段：重叠照报"), C.Overlaps, 1))
        {
            RangeIs(TEXT("嵌套段重叠"), C.Overlaps[0], 10, 20);
        }
    }

    // 同起点不同长度：排序必须是确定的，否则结论会随机漂
    {
        const FCutCoverage C = Analyse({ { 0, 100 }, { 0, 50 }, { 100, 200 } }, 0, 200);
        TestTrue(TEXT("同起点：盖满了"), C.bCoversPlayback);
        CountsMatch(TEXT("同起点：没有空隙"), C.Gaps, 0);
        if (CountsMatch(TEXT("同起点：一处重叠"), C.Overlaps, 1))
        {
            RangeIs(TEXT("同起点重叠"), C.Overlaps[0], 0, 50);
        }
    }

    // ── 重叠 ──────────────────────────────────────────────────────────────
    {
        const FCutCoverage C = Analyse({ { 0, 300 }, { 250, 600 } }, 0, 600);
        TestTrue(TEXT("重叠：仍然盖满"), C.bCoversPlayback);
        if (CountsMatch(TEXT("重叠：报一处"), C.Overlaps, 1))
        {
            RangeIs(TEXT("重叠区间"), C.Overlaps[0], 250, 300);
        }
    }

    // ── 头尾没盖住 ────────────────────────────────────────────────────────
    {
        const FCutCoverage C = Analyse({ { 100, 600 } }, 0, 600);
        TestFalse(TEXT("头没盖：没盖满"), C.bCoversPlayback);
        if (TestTrue(TEXT("头没盖：报出来了"), C.HeadUncovered.IsSet()))
        {
            RangeIs(TEXT("头没盖"), C.HeadUncovered.GetValue(), 0, 100);
        }
        TestFalse(TEXT("头没盖：尾巴是好的"), C.TailUncovered.IsSet());
    }
    {
        const FCutCoverage C = Analyse({ { 0, 300 } }, 0, 600);
        if (TestTrue(TEXT("尾没盖：报出来了"), C.TailUncovered.IsSet()))
        {
            RangeIs(TEXT("尾没盖"), C.TailUncovered.GetValue(), 300, 600);
        }
    }

    // 一个可用段都没有 = 整段都没盖住
    {
        const FCutCoverage C = Analyse({}, 0, 600);
        TestTrue(TEXT("空轨道：判得出来"), C.bCoverageKnown);
        TestFalse(TEXT("空轨道：没盖满"), C.bCoversPlayback);
        if (TestTrue(TEXT("空轨道：整段都没盖"), C.HeadUncovered.IsSet()))
        {
            RangeIs(TEXT("空轨道"), C.HeadUncovered.GetValue(), 0, 600);
        }
    }

    // ── 播放范围之外的洞不是问题 ──────────────────────────────────────────
    //
    // 范围外的帧渲不到，报出来只会让用户去修一个不存在的问题
    {
        const FCutCoverage C = Analyse({ { 0, 100 }, { 200, 300 } }, 0, 100);
        CountsMatch(TEXT("范围外的洞：不报"), C.Gaps, 0);
        TestTrue(TEXT("范围外的洞：仍算盖满"), C.bCoversPlayback);
    }

    // ── 判不出来的两种情况 ────────────────────────────────────────────────
    //
    // 引擎在播放范围损坏或无界时会强制成 [0,0)（UMovieScene::UpgradeTimeRanges，
    // 编辑器每次加载都跑）。空范围没有「盖没盖满」可言 —— 不能顺口答「没盖满」，
    // 写入侧会照着这个结论去删用户的段
    {
        const FCutCoverage C = Analyse({ { 10, 300 } }, 0, 0);
        TestFalse(TEXT("退化范围：判不出来"), C.bCoverageKnown);
        TestFalse(TEXT("退化范围：不许说盖满"), C.bCoversPlayback);
        CountsMatch(TEXT("退化范围：不许报空隙"), C.Gaps, 0);
        TestFalse(TEXT("退化范围：不许报头没盖"), C.HeadUncovered.IsSet());
        TestTrue(TEXT("退化范围：给得出原因"), !C.UnknownReason.IsEmpty());
    }

    // 有一头无界的段：它可能正好把洞填上，所以覆盖判不出来。
    // 但已经查到的重叠在任何情况下都是真的，照报
    {
        const FCutCoverage C = Analyse({ { 0, 300 }, { 250, 400 } }, 0, 600, /*UnboundedCount=*/1);
        TestFalse(TEXT("无界段：判不出覆盖"), C.bCoverageKnown);
        CountsMatch(TEXT("无界段：不许报空隙"), C.Gaps, 0);
        TestFalse(TEXT("无界段：不许报尾没盖"), C.TailUncovered.IsSet());
        CountsMatch(TEXT("无界段：重叠照报"), C.Overlaps, 1);
        TestTrue(TEXT("无界段：给得出原因"), !C.UnknownReason.IsEmpty());
    }

    // ── 零长度段不破坏算术 ────────────────────────────────────────────────
    //
    // [0,0) 这种段确实会出现：上一轮返工时我们自己的 camera_cuts 就往
    // 退化的播放范围上建过一个
    {
        const FCutCoverage C = Analyse({ { 0, 0 }, { 0, 300 } }, 0, 300);
        TestTrue(TEXT("零长段：盖满了"), C.bCoversPlayback);
        CountsMatch(TEXT("零长段：没有空隙"), C.Gaps, 0);
    }

    return true;
}
#endif
