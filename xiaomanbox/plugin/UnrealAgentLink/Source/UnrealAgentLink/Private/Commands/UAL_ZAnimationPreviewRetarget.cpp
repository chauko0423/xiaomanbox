#include "Misc/EngineVersionComparison.h"
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
#include "UAL_CommandUtils.h"
#include "UAL_BoneRoles.h"
#include "UAL_EditorCommands.h"
#include "AnimPose.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/SkeletalMesh.h"
#include "PreviewScene.h"
#include "AssetCompilingManager.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
#include "Retargeter/IKRetargeter.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "AssetRegistry/AssetRegistryModule.h"

namespace UALAnimation
{
    using FJson = TSharedPtr<FJsonObject>;
    bool ResolveRoles(const FJson& Input, const TArray<FName>& Names, UAL_BoneRoles::FBoneRoles& Out, FString& Error);
    template<typename T> static T* Asset(const FJson& Input, const TCHAR* Field)
    {
        FString Path;
        return Input.IsValid() && Input->TryGetStringField(Field, Path) ? LoadObject<T>(nullptr, *Path) : nullptr;
    }

    FJson PreviewAsset(const FJson& Input, FString& FailureReason)
    {
        USkeletalMesh* Mesh = Asset<USkeletalMesh>(Input, TEXT("mesh"));
        UAnimSequence* Animation = Asset<UAnimSequence>(Input, TEXT("animation"));
        double Time = -1;
        Input->TryGetNumberField(TEXT("time"), Time);
        FString Camera = TEXT("three_quarter"); Input->TryGetStringField(TEXT("camera"), Camera);
        // 原来这里五种错误共用一句话，模型分不清是路径错、骨架不同还是时间越界，只能挨个猜
        if (!Mesh) { FailureReason = TEXT("mesh is not a loadable SkeletalMesh"); return nullptr; }
        if (!Animation) { FailureReason = TEXT("animation is not a loadable AnimSequence"); return nullptr; }
        if (Mesh->GetSkeleton() != Animation->GetSkeleton())
        {
            FailureReason = FString::Printf(TEXT("mesh and animation use different skeletons (%s vs %s); retarget first or pick a mesh on the animation's skeleton"),
                Mesh->GetSkeleton() ? *Mesh->GetSkeleton()->GetPathName() : TEXT("none"),
                Animation->GetSkeleton() ? *Animation->GetSkeleton()->GetPathName() : TEXT("none"));
            return nullptr;
        }
        if (!FMath::IsFinite(Time) || Time < 0 || Time > Animation->GetPlayLength())
        { FailureReason = FString::Printf(TEXT("time must be within [0, %.3f] seconds"), Animation->GetPlayLength()); return nullptr; }
        if (Camera != TEXT("front") && Camera != TEXT("side") && Camera != TEXT("three_quarter"))
        { FailureReason = TEXT("camera must be front, side or three_quarter"); return nullptr; }
        FAnimPose Reference;
        UAnimPoseExtensions::GetReferencePose(Animation->GetSkeleton(), Reference);
        TArray<FName> Names; UAnimPoseExtensions::GetBoneNames(Reference, Names);
        UAL_BoneRoles::FBoneRoles Roles;
        if (!ResolveRoles(Input, Names, Roles, FailureReason)) return nullptr;
        auto RefPosition = [&Reference](FName Name) { return UAnimPoseExtensions::GetBonePose(Reference, Name, EAnimPoseSpaces::World).GetLocation(); };

        // 机位朝向：认得出锁骨和左脚时按身体算（原来的做法）；认不出时退到网格 +Y ——
        // UE 骨骼网格的约定正面。以前这里直接 400，非 Manny 骨架一张图都拿不到。
        // 回执写明用的是哪种，模型据此判断「正面」图是不是真的正面
        FString Basis = TEXT("bone_roles");
        FVector Forward = FVector::ZeroVector;
        if (Roles.Missing({TEXT("clavicle_l"), TEXT("clavicle_r"), TEXT("foot_l"), TEXT("ball_l")}).IsEmpty())
        {
            Forward = FVector::CrossProduct(FVector::UpVector, RefPosition(Roles.Bone(TEXT("clavicle_r"))) - RefPosition(Roles.Bone(TEXT("clavicle_l")))).GetSafeNormal();
            if (FVector::DotProduct(Forward, RefPosition(Roles.Bone(TEXT("ball_l"))) - RefPosition(Roles.Bone(TEXT("foot_l")))) < 0) Forward *= -1;
        }
        if (Forward.IsNearlyZero()) { Forward = FVector::YAxisVector; Basis = TEXT("mesh_plus_y_assumed"); }

        // An isolated transient world avoids dirtying the open level or capturing unrelated actors.
        FPreviewScene Scene(FPreviewScene::ConstructionValues().SetEditor(true));
        USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(GetTransientPackage());
        Component->SetSkeletalMesh(Mesh);
        Component->SetAnimationMode(EAnimationMode::AnimationSingleNode);
        Scene.AddComponent(Component, FTransform::Identity);
        Component->SetAnimation(Animation);
        Component->SetPosition(static_cast<float>(Time), false);
        Component->TickAnimation(0.0f, false);
        Component->RefreshBoneTransforms();
        Component->UpdateBounds();
        FAssetCompilingManager::Get().FinishAllCompilation();
        const FBoxSphereBounds Bounds = Component->Bounds;
        const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward);
        const FVector ViewDirection = Camera == TEXT("front") ? Forward : Camera == TEXT("side") ? Right : (Forward + Right).GetSafeNormal();
        const FVector Center = Bounds.Origin;
        const double Distance = FMath::Max(100.0, Bounds.SphereRadius * 4.5);
        const FVector Location = Center + ViewDirection * Distance;
        Scene.SetLightDirection(FRotator(-25.0, (Center - Location).Rotation().Yaw - 30.0, 0.0));
        Scene.SetLightBrightness(5.0f);
        UDirectionalLightComponent* Fill = NewObject<UDirectionalLightComponent>();
        Fill->SetIntensity(2.5f); Fill->SetCastShadows(false);
        Scene.AddComponent(Fill, FTransform(FRotator(-10.0, (Center - Location).Rotation().Yaw + 60.0, 0.0)));
        FString Path, Error;
        const bool bCaptured = FUAL_EditorCommands::CaptureAnimationPreview(Scene.GetWorld(), Location, (Center - Location).Rotation(), Path, Error);
        Scene.RemoveComponent(Component);
        Scene.RemoveComponent(Fill);
        if (!bCaptured) { FailureReason = Error; return nullptr; }
        FJson Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("path"), Path);
        Result->SetNumberField(TEXT("time"), Time); Result->SetStringField(TEXT("camera"), Camera);
        Result->SetStringField(TEXT("camera_basis"), Basis);
        return Result;
    }

    FJson RetargetAsset(const FJson& Input, FString& FailureReason)
    {
        USkeletalMesh* SourceMesh = Asset<USkeletalMesh>(Input, TEXT("source_mesh"));
        USkeletalMesh* TargetMesh = Asset<USkeletalMesh>(Input, TEXT("target_mesh"));
        UAnimSequence* Animation = Asset<UAnimSequence>(Input, TEXT("animation"));
        FString Output; Input->TryGetStringField(TEXT("output_path"), Output);
        if (!SourceMesh || !TargetMesh || SourceMesh == TargetMesh || !Animation || Animation->GetSkeleton() != SourceMesh->GetSkeleton() || Animation->IsValidAdditive() || !Output.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(Output))
        { FailureReason = TEXT("Expected source/target meshes, a non-additive source sequence and a new /Game/ output package"); return nullptr; }
        if (FPackageName::DoesPackageExist(Output) || FindPackage(nullptr, *Output))
        { FailureReason = TEXT("Output already exists; inspect or choose a new output path"); return nullptr; }
        UIKRetargeter* Retargeter = nullptr;
        FString Existing;
        if (Input->TryGetStringField(TEXT("retargeter"), Existing))
        {
            Retargeter = LoadObject<UIKRetargeter>(nullptr, *Existing);
            if (!Retargeter) { FailureReason = TEXT("Retargeter not found"); return nullptr; }
        }
        else
        {
            const FString Folder = FPackageName::GetLongPackagePath(Output);
            const FString Stem = FPackageName::GetShortName(Output);
            const FString RigSourcePath = Folder / (Stem + TEXT("_SourceRig"));
            const FString RigTargetPath = Folder / (Stem + TEXT("_TargetRig"));
            const FString RetargetPath = Folder / (Stem + TEXT("_Retargeter"));
            for (const FString& Path : {RigSourcePath, RigTargetPath, RetargetPath})
                if (FPackageName::DoesPackageExist(Path) || FindPackage(nullptr, *Path))
                { FailureReason = TEXT("Generated rig path already exists; pass the existing retargeter explicitly"); return nullptr; }
            UIKRigDefinition* SourceRig = NewObject<UIKRigDefinition>(CreatePackage(*RigSourcePath), *FPackageName::GetShortName(RigSourcePath), RF_Public | RF_Standalone | RF_Transactional);
            UIKRigDefinition* TargetRig = NewObject<UIKRigDefinition>(CreatePackage(*RigTargetPath), *FPackageName::GetShortName(RigTargetPath), RF_Public | RF_Standalone | RF_Transactional);
            UIKRigController* SourceController = UIKRigController::GetController(SourceRig);
            UIKRigController* TargetController = UIKRigController::GetController(TargetRig);
            if (!SourceController->SetSkeletalMesh(SourceMesh) || !TargetController->SetSkeletalMesh(TargetMesh) || !SourceController->ApplyAutoGeneratedRetargetDefinition() || !TargetController->ApplyAutoGeneratedRetargetDefinition())
            { FailureReason = TEXT("Automatic rig characterization failed; no animation exported. Inspect generated rigs before retrying."); return nullptr; }
            Retargeter = NewObject<UIKRetargeter>(CreatePackage(*RetargetPath), *FPackageName::GetShortName(RetargetPath), RF_Public | RF_Standalone | RF_Transactional);
            UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
            Controller->SetIKRig(ERetargetSourceOrTarget::Source, SourceRig);
            Controller->SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);
            Controller->AutoMapChains(EAutoMapChainType::Exact, true);
            Controller->AutoAlignAllBones(ERetargetSourceOrTarget::Target);
            for (UObject* Object : {static_cast<UObject*>(SourceRig), static_cast<UObject*>(TargetRig), static_cast<UObject*>(Retargeter)})
            { FAssetRegistryModule::AssetCreated(Object); Object->MarkPackageDirty(); }
            if (!UEditorLoadingAndSavingUtils::SavePackages({SourceRig->GetOutermost(), TargetRig->GetOutermost(), Retargeter->GetOutermost()}, false))
            { FailureReason = TEXT("Rig save failed; no animation exported"); return nullptr; }
        }
        UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
        const UIKRigDefinition* SourceRig = Controller->GetIKRig(ERetargetSourceOrTarget::Source);
        const UIKRigDefinition* TargetRig = Controller->GetIKRig(ERetargetSourceOrTarget::Target);
        if (!SourceRig || !TargetRig || UIKRigController::GetController(SourceRig)->GetSkeletalMesh() != SourceMesh || UIKRigController::GetController(TargetRig)->GetSkeletalMesh() != TargetMesh)
        { FailureReason = TEXT("Retargeter rigs do not match the requested meshes; no animation exported"); return nullptr; }
        int32 Mapped = 0;
        FJson ChainMapping = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Unmapped;
        for (const FBoneChain& Chain : UIKRigController::GetController(TargetRig)->GetRetargetChains())
        {
            const FName SourceChain = Controller->GetSourceChain(Chain.ChainName);
            if (!SourceChain.IsNone()) { ++Mapped; ChainMapping->SetStringField(Chain.ChainName.ToString(), SourceChain.ToString()); }
            else Unmapped.Add(MakeShared<FJsonValueString>(Chain.ChainName.ToString()));
        }
        if (Mapped == 0) { FailureReason = TEXT("No mapped chains; no animation exported"); return nullptr; }
        FIKRetargetBatchOperationContext Context;
        Context.SourceMesh = SourceMesh; Context.TargetMesh = TargetMesh; Context.IKRetargetAsset = Retargeter;
        Context.AssetsToRetarget.Add(Animation); Context.bIncludeReferencedAssets = false;
        Context.bOverwriteExistingFiles = false;
        Context.NameRule.FolderPath = FPackageName::GetLongPackagePath(Output);
        Context.NameRule.ReplaceFrom = Animation->GetName(); Context.NameRule.ReplaceTo = FPackageName::GetShortName(Output);
        UIKRetargetBatchOperation* Operation = NewObject<UIKRetargetBatchOperation>();
        Operation->RunRetarget(Context);
        UAnimSequence* Created = LoadObject<UAnimSequence>(nullptr, *(Output + TEXT(".") + FPackageName::GetShortName(Output)));
        if (!Created || Created->GetSkeleton() != TargetMesh->GetSkeleton() || !Created->GetDataModel() || Created->GetDataModel()->GetNumberOfKeys() < 2)
        { FailureReason = TEXT("Retarget output readback failed; inspect output before retrying"); return nullptr; }
        if (!UEditorLoadingAndSavingUtils::SavePackages({Created->GetOutermost()}, false))
        { FailureReason = TEXT("Retarget output exists in memory but was not saved"); return nullptr; }
        const FString Filename = FPackageName::LongPackageNameToFilename(Output, FPackageName::GetAssetPackageExtension());
        const int64 Bytes = IFileManager::Get().FileSize(*Filename);
        if (Bytes <= 0) { FailureReason = TEXT("Saved output is missing or empty"); return nullptr; }
        FJson Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("path"), Created->GetPathName()); Result->SetNumberField(TEXT("bytes"), Bytes);
        Result->SetStringField(TEXT("retargeter"), Retargeter->GetPathName()); Result->SetNumberField(TEXT("mapped_chains"), Mapped);
        Result->SetObjectField(TEXT("chain_mapping"), ChainMapping);
        Result->SetArrayField(TEXT("unmapped_target_chains"), Unmapped);
        return Result;
    }
    void Preview(const FJson& Input, const FString& Id)
    {
        FString Error;
        FJson Result = PreviewAsset(Input, Error);
        if (Result.IsValid()) UAL_CommandUtils::SendResponse(Id, 200, Result);
        else UAL_CommandUtils::SendError(Id, 400, Error);
    }
    void Retarget(const FJson& Input, const FString& Id)
    {
        FString Error;
        FJson Result = RetargetAsset(Input, Error);
        if (Result.IsValid()) UAL_CommandUtils::SendResponse(Id, 200, Result);
        else UAL_CommandUtils::SendError(Id, 400, Error);
    }
}
#endif
