#include "UAL_AssetSourcePath.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUALAssetSourcePathTest,
    "UnrealAgentLink.Assets.DependencySourcePath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUALAssetSourcePathTest::RunTest(const FString& Parameters)
{
    TestEqual(TEXT("Mac external-volume dependency"),
        UAL_AssetSourcePath::DependencyFile(TEXT("/Volumes/Asset Library"), TEXT("/Game/Materials/M_Wall")),
        FString(TEXT("/Volumes/Asset Library/Game/Materials/M_Wall.uasset")));
    TestEqual(TEXT("Windows backslash source remains usable"),
        UAL_AssetSourcePath::DependencyFile(TEXT("D:\\Asset Library"), TEXT("/Game/Materials/M_Wall")),
        FString(TEXT("D:/Asset Library/Game/Materials/M_Wall.uasset")));
    TestEqual(TEXT("Case-sensitive path spelling preserved"),
        UAL_AssetSourcePath::DependencyFile(TEXT("/Volumes/UE"), TEXT("/Game/MixedCase/T_Wall")),
        FString(TEXT("/Volumes/UE/Game/MixedCase/T_Wall.uasset")));
    return true;
}
#endif
