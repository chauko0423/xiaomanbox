#include "UAL_LandscapeLayout.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUALLandscapeLayoutTest,
    "UnrealAgentLink.Landscape.PickLayout",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUALLandscapeLayoutTest::RunTest(const FString& Parameters)
{
    using namespace UALLandscapeLayout;

    auto Expect = [this](const TCHAR* What, const FLandscapeLayout& L, int32 Quads, int32 Sections, int32 CountX, int32 CountY)
    {
        TestEqual(FString::Printf(TEXT("%s 每分区格数"), What), L.QuadsPerSection, Quads);
        TestEqual(FString::Printf(TEXT("%s 每块分区数"), What), L.SectionsPerComponent, Sections);
        TestEqual(FString::Printf(TEXT("%s X 块数"), What), L.ComponentCount.X, CountX);
        TestEqual(FString::Printf(TEXT("%s Y 块数"), What), L.ComponentCount.Y, CountY);
    };

    // Epic 推荐尺寸表：恰好相等时一格都不差
    Expect(TEXT("1009 顶点"), PickLayout(1008, 1008), 63, 2, 8, 8);
    Expect(TEXT("2017 顶点"), PickLayout(2016, 2016), 63, 2, 16, 16);
    Expect(TEXT("8129 顶点"), PickLayout(8128, 8128), 127, 2, 32, 32);

    // 「1 公里」：3% 以内取偏好最高的组合，而不是去凑小分区
    Expect(TEXT("1000 米"), PickLayout(1000, 1000), 63, 2, 8, 8);

    // 1024 的高度图：不往大凑成 1072，回到 1009
    const FLandscapeLayout From1024 = PickLayout(1023, 1023);
    TestEqual(TEXT("1024 图重采样到 1009"), From1024.Resolution().X, 1009);

    // 小地形退到小分区，而不是塞一个 126 格的大块
    const FLandscapeLayout Small = PickLayout(100, 100);
    TestTrue(TEXT("100 米误差不超过 3 格"), FMath::Abs(Small.Resolution().X - 101) <= 3);

    // 长方形两个方向分别算
    Expect(TEXT("1000 × 500 米"), PickLayout(1000, 500), 63, 2, 8, 4);

    // 上限：超出也不能越过每边 32 块、8161 个顶点
    for (const int32 Target : {1, 7, 9000, 20000})
    {
        const FLandscapeLayout L = PickLayout(Target, Target);
        TestTrue(FString::Printf(TEXT("%d 块数在 1..32"), Target),
            L.ComponentCount.X >= 1 && L.ComponentCount.X <= MaxComponentsPerAxis);
        TestTrue(FString::Printf(TEXT("%d 顶点不超上限"), Target), L.Resolution().X <= MaxResolutionPerAxis);
    }

    return true;
}

#endif
