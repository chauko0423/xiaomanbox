#include "UAL_WindowCapture.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUALWindowCapturePixelsTest,
    "UnrealAgentLink.Editor.WindowCapturePixels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUALWindowCapturePixelsTest::RunTest(const FString& Parameters)
{
    TArray<FColor> Pixels;
    Pixels.Add(FColor(12, 34, 56, 0));
    Pixels.Add(FColor(78, 90, 123, 10));
    TestTrue(TEXT("Valid image accepted"), UAL_WindowCapture::PreparePixels(Pixels, FIntPoint(2, 1)));
    TestEqual(TEXT("RGB preserved"), Pixels[0].R, uint8(12));
    TestEqual(TEXT("Transparent capture made visible"), Pixels[0].A, uint8(255));
    TestEqual(TEXT("Partial alpha made opaque"), Pixels[1].A, uint8(255));
    TestFalse(TEXT("Mismatched dimensions rejected"), UAL_WindowCapture::PreparePixels(Pixels, FIntPoint(2, 2)));
    TestFalse(TEXT("Negative dimensions rejected"), UAL_WindowCapture::PreparePixels(Pixels, FIntPoint(-2, -1)));
    TestFalse(TEXT("Overflow-sized image rejected"), UAL_WindowCapture::PreparePixels(Pixels, FIntPoint(MAX_int32, MAX_int32)));
    Pixels.Empty();
    TestFalse(TEXT("Empty capture rejected"), UAL_WindowCapture::PreparePixels(Pixels, FIntPoint(0, 0)));
    return true;
}
#endif
