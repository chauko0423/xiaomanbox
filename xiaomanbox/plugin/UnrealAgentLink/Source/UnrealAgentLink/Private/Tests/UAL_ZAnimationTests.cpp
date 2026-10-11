#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#if WITH_DEV_AUTOMATION_TESTS && !UE_VERSION_OLDER_THAN(5, 5, 0)
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Dom/JsonObject.h"
#include "ReferenceSkeleton.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

namespace UALAnimation
{
    TSharedPtr<FJsonObject> MeasureSequence(UAnimSequence* Anim, const TSharedPtr<FJsonObject>& Input, FString& Error);
    TSharedPtr<FJsonObject> WritePoseSequence(UAnimSequence* Target, UAnimSequence* Source, const TSharedPtr<FJsonObject>& Input, FString& Error);
    TSharedPtr<FJsonObject> PreviewAsset(const TSharedPtr<FJsonObject>& Input, FString& Error);
    TSharedPtr<FJsonObject> RetargetAsset(const TSharedPtr<FJsonObject>& Input, FString& Error);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUALAnimationMeasurements, "UnrealAgentLink.Animation.Measurements",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * 合成一段走位动画：13 根骨按 Manny 的顺序（root pelvis spine_05 head clavicle_l clavicle_r
 * upperarm_l lowerarm_l hand_l thigh_l calf_l foot_l ball_l），名字由调用方给 ——
 * 同一组姿势换成 Biped 骨名，七项读数必须一模一样。
 */
static UAnimSequence* SyntheticWalk(const TArray<FName>& Names)
{
    check(Names.Num() == 13);
    USkeleton* Skeleton = NewObject<USkeleton>();
    const double Knee = FMath::DegreesToRadians(21.0);
    const FVector Foot(40 * FMath::Sin(Knee), -10, 50 - 40 * FMath::Cos(Knee));
    TArray<FVector> Positions = {FVector::ZeroVector, FVector(0, 0, 100), FVector(0, 0, 140), FVector(0, 0, 175),
        FVector(0, -10, 150), FVector(0, 10, 150), FVector(0, -20, 145),
        FVector(0, -20 - 30 * FMath::Sin(FMath::DegreesToRadians(36.0)), 145 - 30 * FMath::Cos(FMath::DegreesToRadians(36.0))),
        FVector(0, -50, 100), FVector(0, -10, 90), FVector(0, -10, 50), Foot, Foot + FVector(15, 0, 0)};
    {
        FReferenceSkeletonModifier Modifier(Skeleton);
        for (int32 I = 0; I < Names.Num(); ++I)
            Modifier.Add(FMeshBoneInfo(Names[I], Names[I].ToString(), I == 0 ? INDEX_NONE : 0), FTransform(Positions[I]));
    }
    UAnimSequence* Animation = NewObject<UAnimSequence>();
    Animation->SetSkeleton(Skeleton);
    IAnimationDataController& Controller = Animation->GetController();
    Controller.InitializeModel();
    Controller.OpenBracket(FText::FromString(TEXT("Synthetic measurements")), false);
    Controller.SetFrameRate(FFrameRate(30, 1), false);
    Controller.SetNumberOfFrames(10, false);
    enum { Pelvis = 1, Chest = 2, Head = 3, HandL = 8, BallL = 12 };
    for (int32 B = 0; B < Names.Num(); ++B)
    {
        Controller.AddBoneCurve(Names[B], false);
        TArray<FVector3f> P, S; TArray<FQuat4f> Q;
        for (int32 Frame = 0; Frame <= 10; ++Frame)
        {
            FVector Position = Positions[B];
            FQuat Rotation = FQuat::Identity;
            if (B == Head) Rotation = FRotator(-10.5, 0, 0).Quaternion();
            if (B == Pelvis) Position.Z += 4.1 * Frame / 10.0;
            if (B == Chest) Position.Z += 2.3 * Frame / 10.0;
            if (B == HandL && Frame == 10) Position.X += 25;
            if (B == BallL) Position = Foot + FRotator(0, 11, 0).RotateVector(FVector(15, 0, 0));
            P.Add(FVector3f(Position)); Q.Add(FQuat4f(Rotation)); S.Add(FVector3f::OneVector);
        }
        Controller.SetBoneTrackKeys(Names[B], P, Q, S, false);
    }
    Controller.NotifyPopulated(); Controller.CloseBracket(false);
    return Animation;
}

bool FUALAnimationMeasurements::RunTest(const FString& Parameters)
{
    TArray<FName> Names = {TEXT("root"), TEXT("pelvis"), TEXT("spine_05"), TEXT("head"), TEXT("clavicle_l"), TEXT("clavicle_r"),
        TEXT("upperarm_l"), TEXT("lowerarm_l"), TEXT("hand_l"), TEXT("thigh_l"), TEXT("calf_l"), TEXT("foot_l"), TEXT("ball_l")};
    UAnimSequence* Animation = SyntheticWalk(Names);
    IAnimationDataController& Controller = Animation->GetController();
    FString Error;
    TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
    const auto Result = UALAnimation::MeasureSequence(Animation, Input, Error);
    if (!TestTrue(TEXT("Measurement succeeds: ") + Error, Result.IsValid())) return false;
    const auto Metrics = Result->GetObjectField(TEXT("metrics"));
    auto Check = [this](const TCHAR* Name, double Actual, double Expected) { TestTrue(Name, FMath::IsNearlyEqual(Actual, Expected, 0.1)); };
    Check(TEXT("Elbow 36"), Metrics->GetObjectField(TEXT("elbow_out_deg"))->GetObjectField(TEXT("l"))->GetNumberField(TEXT("max")), 36);
    Check(TEXT("Toe -11"), Metrics->GetObjectField(TEXT("toe_out_deg"))->GetObjectField(TEXT("l"))->GetNumberField(TEXT("max")), -11);
    Check(TEXT("Knee 21"), Metrics->GetObjectField(TEXT("knee_bend_deg"))->GetObjectField(TEXT("l"))->GetNumberField(TEXT("max")), 21);
    Check(TEXT("Head -10.5"), Metrics->GetObjectField(TEXT("gaze_pitch_deg"))->GetNumberField(TEXT("max")), -10.5);
    Check(TEXT("Pelvis range 4.1"), Metrics->GetObjectField(TEXT("vertical_range_cm"))->GetNumberField(TEXT("pelvis_range")), 4.1);
    Check(TEXT("Torso 0"), Metrics->GetObjectField(TEXT("torso_yaw_deg"))->GetNumberField(TEXT("max")), 0);
    Check(TEXT("Hand jump 25"), Metrics->GetObjectField(TEXT("hand_step_cm"))->GetObjectField(TEXT("l"))->GetNumberField(TEXT("max")), 25);
    TestEqual(TEXT("Jump frame"), Metrics->GetObjectField(TEXT("hand_step_cm"))->GetObjectField(TEXT("l"))->GetIntegerField(TEXT("max_frame")), 10);
    TestEqual(TEXT("Missing right hand is not zero"), Metrics->GetObjectField(TEXT("hand_step_cm"))->GetObjectField(TEXT("r"))->GetStringField(TEXT("status")), FString(TEXT("unmeasurable")));

    TArray<FVector3f> P, S; TArray<FQuat4f> Q;
    P.Init(FVector3f(0, 0, 100), 11); S.Init(FVector3f::OneVector, 11); Q.Init(FQuat4f(FRotator(0, 90, 0).Quaternion()), 11);
    Controller.SetBoneTrackKeys(TEXT("pelvis"), P, Q, S, false);
    const auto Turned = UALAnimation::MeasureSequence(Animation, Input, Error);
    TestEqual(TEXT("Turning cannot be called toe-out"), Turned->GetObjectField(TEXT("metrics"))->GetObjectField(TEXT("toe_out_deg"))->GetObjectField(TEXT("l"))->GetStringField(TEXT("status")), FString(TEXT("unmeasurable")));

    // A wrist twist leaves the seven positional summaries unchanged but must still be written and verified.
    UAnimSequence* Target = DuplicateObject<UAnimSequence>(Animation, GetTransientPackage());
    TArray<FTransform> Before;
    Target->GetDataModel()->GetBoneTrackTransforms(TEXT("hand_l"), Before);
    P.Init(FVector3f(0, -50, 100), 11); Q.Init(FQuat4f(FRotator(0, 0, 47).Quaternion()), 11);
    Controller.SetBoneTrackKeys(TEXT("hand_l"), P, Q, S, false);
    auto WriteInput = MakeShared<FJsonObject>();
    WriteInput->SetNumberField(TEXT("source_time"), 0);
    WriteInput->SetNumberField(TEXT("start_frame"), 4);
    WriteInput->SetNumberField(TEXT("end_frame"), 6);
    WriteInput->SetNumberField(TEXT("transition_frames"), 1);
    const auto Written = UALAnimation::WritePoseSequence(Target, Animation, WriteInput, Error);
    if (!TestTrue(TEXT("Pose write succeeds: ") + Error, Written.IsValid())) return false;
    TArray<FTransform> After;
    Target->GetDataModel()->GetBoneTrackTransforms(TEXT("hand_l"), After);
    const FTransform Expected(FRotator(0, 0, 47), FVector(0, -50, 100));
    for (int32 Frame = 0; Frame <= 10; ++Frame)
    {
        FTransform Wanted = Before[Frame];
        if (Frame >= 4 && Frame <= 6) Wanted = Expected;
        else if (Frame == 3 || Frame == 7) Wanted.Blend(Before[Frame], Expected, 0.5);
        TestTrue(FString::Printf(TEXT("Wrist local transform frame %d"), Frame), After[Frame].Equals(Wanted, 0.0001));
    }
    WriteInput->SetNumberField(TEXT("end_frame"), 99999);
    TestFalse(TEXT("Out-of-range write rejected"), UALAnimation::WritePoseSequence(Target, Animation, WriteInput, Error).IsValid());

    // Optional manual integration fixture: UE's ThirdPerson Characters copied to this test project's Content/Characters.
    // The synthetic assertions above always run; this additionally requires rendering and the template assets.
    if (FParse::Param(FCommandLine::Get(), TEXT("UALAnimationTemplateSmoke")))
    {
        const FString SourcePath = TEXT("/Game/Characters/Mannequins/Animations/Manny/MM_Idle.MM_Idle");
        auto RetargetInput = MakeShared<FJsonObject>();
        RetargetInput->SetStringField(TEXT("source_mesh"), TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny.SKM_Manny"));
        RetargetInput->SetStringField(TEXT("target_mesh"), TEXT("/Game/Characters/Mannequins/Meshes/SKM_Quinn.SKM_Quinn"));
        RetargetInput->SetStringField(TEXT("animation"), SourcePath);
        RetargetInput->SetStringField(TEXT("output_path"), TEXT("/Game/AnimationAcceptance/Idle_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const auto Retargeted = UALAnimation::RetargetAsset(RetargetInput, Error);
        if (!TestTrue(TEXT("Template retarget: ") + Error, Retargeted.IsValid())) return false;
        auto PreviewInput = MakeShared<FJsonObject>();
        PreviewInput->SetStringField(TEXT("mesh"), TEXT("/Game/Characters/Mannequins/Meshes/SKM_Quinn.SKM_Quinn"));
        PreviewInput->SetStringField(TEXT("animation"), Retargeted->GetStringField(TEXT("path")));
        PreviewInput->SetNumberField(TEXT("time"), 0);
        PreviewInput->SetStringField(TEXT("camera"), TEXT("three_quarter"));
        const auto Preview = UALAnimation::PreviewAsset(PreviewInput, Error);
        if (!TestTrue(TEXT("Template preview: ") + Error, Preview.IsValid())) return false;
        auto Evidence = MakeShared<FJsonObject>();
        Evidence->SetObjectField(TEXT("retarget"), Retargeted);
        Evidence->SetObjectField(TEXT("preview"), Preview);
        UAnimSequence* Output = LoadObject<UAnimSequence>(nullptr, *Retargeted->GetStringField(TEXT("path")));
        const auto Measured = UALAnimation::MeasureSequence(Output, Input, Error);
        if (!TestTrue(TEXT("Retargeted animation can be measured"), Measured.IsValid())) return false;
        Evidence->SetObjectField(TEXT("measure"), Measured);
        // Explicit reuse must preserve the existing retarget configuration.
        RetargetInput->SetStringField(TEXT("retargeter"), Retargeted->GetStringField(TEXT("retargeter")));
        RetargetInput->SetStringField(TEXT("output_path"), TEXT("/Game/AnimationAcceptance/Reuse_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const auto Reused = UALAnimation::RetargetAsset(RetargetInput, Error);
        if (!TestTrue(TEXT("Existing retargeter reuse: ") + Error, Reused.IsValid())) return false;
        Evidence->SetObjectField(TEXT("reuse"), Reused);
        FString Json;
        FJsonSerializer::Serialize(Evidence, TJsonWriterFactory<>::Create(&Json));
        TestTrue(TEXT("Write manual acceptance evidence"), FFileHelper::SaveStringToFile(Json, *(FPaths::ProjectSavedDir() / TEXT("animation-evidence.json"))));
    }
    return true;
}
// 2026-09-24 用户反馈：Biped 骨架上七项默认指标全部 unmeasurable（Missing bones: pelvis, spine_05）。
// 同一组姿势只换骨名，读数必须和 Manny 那份一致
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUALAnimationMeasurementsBiped, "UnrealAgentLink.Animation.MeasurementsBiped",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUALAnimationMeasurementsBiped::RunTest(const FString& Parameters)
{
    UAnimSequence* Animation = SyntheticWalk({TEXT("Bip001"), TEXT("Bip001-Pelvis"), TEXT("Bip001-Spine2"), TEXT("Bip001-Head"),
        TEXT("Bip001-L-Clavicle"), TEXT("Bip001-R-Clavicle"), TEXT("Bip001-L-UpperArm"), TEXT("Bip001-L-Forearm"),
        TEXT("Bip001-L-Hand"), TEXT("Bip001-L-Thigh"), TEXT("Bip001-L-Calf"), TEXT("Bip001-L-Foot"), TEXT("Bip001-L-Toe0")});
    FString Error;
    TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
    const auto Result = UALAnimation::MeasureSequence(Animation, Input, Error);
    if (!TestTrue(TEXT("Biped measurement succeeds: ") + Error, Result.IsValid())) return false;
    const auto Metrics = Result->GetObjectField(TEXT("metrics"));
    auto Check = [this](const TCHAR* Name, double Actual, double Expected) { TestTrue(Name, FMath::IsNearlyEqual(Actual, Expected, 0.1)); };
    Check(TEXT("Biped elbow 36"), Metrics->GetObjectField(TEXT("elbow_out_deg"))->GetObjectField(TEXT("l"))->GetNumberField(TEXT("max")), 36);
    Check(TEXT("Biped toe -11"), Metrics->GetObjectField(TEXT("toe_out_deg"))->GetObjectField(TEXT("l"))->GetNumberField(TEXT("max")), -11);
    Check(TEXT("Biped knee 21"), Metrics->GetObjectField(TEXT("knee_bend_deg"))->GetObjectField(TEXT("l"))->GetNumberField(TEXT("max")), 21);
    Check(TEXT("Biped head -10.5"), Metrics->GetObjectField(TEXT("gaze_pitch_deg"))->GetNumberField(TEXT("max")), -10.5);
    Check(TEXT("Biped pelvis range 4.1"), Metrics->GetObjectField(TEXT("vertical_range_cm"))->GetNumberField(TEXT("pelvis_range")), 4.1);
    Check(TEXT("Biped chest range 2.3"), Metrics->GetObjectField(TEXT("vertical_range_cm"))->GetNumberField(TEXT("chest_range")), 2.3);
    TestEqual(TEXT("Chest role reported"), Result->GetObjectField(TEXT("bone_roles"))->GetStringField(TEXT("chest")), FString(TEXT("Bip001-Spine2")));
    TestTrue(TEXT("Guessed roles are flagged"), Result->GetArrayField(TEXT("bone_roles_by_naming_convention")).Num() > 0);

    // 角色名当骨名用：bones: ["pelvis"] 量到 Bip001-Pelvis
    TArray<TSharedPtr<FJsonValue>> Bones = {MakeShared<FJsonValueString>(TEXT("pelvis"))};
    Input->SetArrayField(TEXT("bones"), Bones);
    const auto WithBones = UALAnimation::MeasureSequence(Animation, Input, Error);
    TestTrue(TEXT("Role name works in bones"), WithBones.IsValid() && WithBones->GetObjectField(TEXT("bone_positions_cm"))->HasTypedField<EJson::Array>(TEXT("pelvis")));

    // 点错名是报错，不是静默换骨头
    TSharedPtr<FJsonObject> Map = MakeShared<FJsonObject>();
    Map->SetStringField(TEXT("chest"), TEXT("NoSuchBone"));
    Input->SetObjectField(TEXT("bone_map"), Map);
    TestFalse(TEXT("Wrong bone_map is an error"), UALAnimation::MeasureSequence(Animation, Input, Error).IsValid());
    TestTrue(TEXT("Error names the role"), Error.Contains(TEXT("chest")));
    return true;
}

#endif
