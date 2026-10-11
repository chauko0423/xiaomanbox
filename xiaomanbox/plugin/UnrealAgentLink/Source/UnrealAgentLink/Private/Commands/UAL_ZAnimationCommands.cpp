#include "UAL_AnimationCommands.h"
#include "UAL_CommandUtils.h"
#include "UAL_BoneRoles.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Misc/EngineVersionComparison.h"

#if !UE_VERSION_OLDER_THAN(5, 5, 0)
#include "AnimPose.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "FileHelpers.h"
#include "ScopedTransaction.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"

namespace UALAnimation
{
    void Preview(const TSharedPtr<FJsonObject>& Input, const FString& Id);
    void Retarget(const TSharedPtr<FJsonObject>& Input, const FString& Id);
    using FJson = TSharedPtr<FJsonObject>;
    using FValues = TArray<TSharedPtr<FJsonValue>>;
    static FJson Failure(const FString& Reason)
    {
        FJson R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("status"), TEXT("unmeasurable"));
        R->SetStringField(TEXT("reason"), Reason);
        return R;
    }
    static double Angle(const FVector& A, const FVector& B)
    {
        return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(A.GetSafeNormal(), B.GetSafeNormal()), -1.0, 1.0)));
    }
    static double SignedAngle(const FVector& A, const FVector& B)
    {
        return FMath::RadiansToDegrees(FMath::Atan2(FVector::CrossProduct(A, B).Z, FVector::DotProduct(A, B)));
    }
    static FVector Horizontal(FVector V) { V.Z = 0; return V.GetSafeNormal(); }
    static FTransform Bone(const FAnimPose& P, const FName Name)
    {
        return UAnimPoseExtensions::GetBonePose(P, Name, EAnimPoseSpaces::World);
    }
    static FVector Position(const FAnimPose& P, const FName Name) { return Bone(P, Name).GetLocation(); }
    static FString Missing(const TArray<FName>& Names, const TArray<FName>& Required)
    {
        TArray<FString> Result;
        for (FName Name : Required) if (Name.IsNone() || !Names.Contains(Name)) Result.Add(Name.ToString());
        return FString::Join(Result, TEXT(", "));
    }
    /**
     * 解析骨骼角色（见 UAL_BoneRoles.h）。bone_map 写错时返回 false 并给出原因 ——
     * 用户点了名却被悄悄换成别的骨头，比报错更糟。
     */
    bool ResolveRoles(const FJson& Input, const TArray<FName>& Names, UAL_BoneRoles::FBoneRoles& Out, FString& Error)
    {
        TMap<FString, FString> Explicit;
        const TSharedPtr<FJsonObject>* Map = nullptr;
        if (Input.IsValid() && Input->TryGetObjectField(TEXT("bone_map"), Map) && Map && Map->IsValid())
            for (const auto& Entry : (*Map)->Values)
            {
                FString Bone;
                if (!Entry.Value.IsValid() || !Entry.Value->TryGetString(Bone)) { Error = TEXT("bone_map values must be bone names"); return false; }
                Explicit.Add(UAL_JsonKey(Entry.Key), Bone);
            }
        Out = UAL_BoneRoles::Resolve(Names, Explicit);
        if (Out.Errors.Num() > 0) { Error = TEXT("bone_map: ") + FString::Join(Out.Errors, TEXT("; ")); return false; }
        return true;
    }
    /** 回执里的角色表：模型据此知道每个角色落在哪根骨头上、哪些是按命名约定猜的 */
    static void WriteRoles(const FJson& Result, const UAL_BoneRoles::FBoneRoles& Roles)
    {
        FJson Map = MakeShared<FJsonObject>();
        FValues Guessed, Unresolved;
        for (const FString& Role : UAL_BoneRoles::AllRoles())
        {
            const UAL_BoneRoles::FResolved* Found = Roles.Roles.Find(Role);
            if (!Found) { Unresolved.Add(MakeShared<FJsonValueString>(Role)); continue; }
            Map->SetStringField(Role, Found->Bone.ToString());
            if (Found->Via == TEXT("alias")) Guessed.Add(MakeShared<FJsonValueString>(Role));
        }
        Result->SetObjectField(TEXT("bone_roles"), Map);
        Result->SetArrayField(TEXT("bone_roles_by_naming_convention"), Guessed);
        Result->SetArrayField(TEXT("bone_roles_unresolved"), Unresolved);
    }
    static FJson Series(const TArray<double>& Values, const TArray<int32>& Frames, const TCHAR* Unit)
    {
        if (Values.IsEmpty()) return Failure(TEXT("No measurable samples"));
        FJson R = MakeShared<FJsonObject>();
        FValues Samples;
        double Min = Values[0], Max = Values[0];
        int32 MaxIndex = 0;
        for (int32 I = 0; I < Values.Num(); ++I)
        {
            Samples.Add(MakeShared<FJsonValueNumber>(Values[I]));
            Min = FMath::Min(Min, Values[I]);
            if (Values[I] > Max) { Max = Values[I]; MaxIndex = I; }
        }
        R->SetStringField(TEXT("status"), TEXT("measured"));
        R->SetStringField(TEXT("unit"), Unit);
        R->SetArrayField(TEXT("values"), Samples);
        R->SetNumberField(TEXT("min"), Min);
        R->SetNumberField(TEXT("max"), Max);
        R->SetNumberField(TEXT("max_frame"), Frames[MaxIndex]);
        return R;
    }
    static UAnimSequence* LoadAnimation(const FJson& Input, const TCHAR* Field)
    {
        FString Path;
        if (!Input.IsValid() || !Input->TryGetStringField(Field, Path)) return nullptr;
        return LoadObject<UAnimSequence>(nullptr, *Path);
    }

    FJson MeasureSequence(UAnimSequence* Anim, const FJson& Input, FString& Error)
    {
        if (!Anim || !Anim->GetSkeleton() || !Anim->GetDataModel())
        {
            Error = TEXT("Expected an animation sequence with a skeleton and source data"); return nullptr;
        }
        const IAnimationDataModel* Model = Anim->GetDataModel();
        FJson Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("path"), Anim->GetPathName());
        Result->SetNumberField(TEXT("fps"), Model->GetFrameRate().AsDecimal());
        Result->SetNumberField(TEXT("frames"), Model->GetNumberOfKeys());
        TArray<FName> Tracks;
        Model->GetBoneTrackNames(Tracks);
        Result->SetNumberField(TEXT("tracks"), Tracks.Num());
        FValues MissingTracks;
        const FReferenceSkeleton& RefSkel = Anim->GetSkeleton()->GetReferenceSkeleton();
        for (int32 I = 0; I < RefSkel.GetNum(); ++I)
            if (!Tracks.Contains(RefSkel.GetBoneName(I))) MissingTracks.Add(MakeShared<FJsonValueString>(RefSkel.GetBoneName(I).ToString()));
        Result->SetArrayField(TEXT("missing_tracks"), MissingTracks);
        TArray<FName> Names;
        for (int32 I = 0; I < RefSkel.GetNum(); ++I) Names.Add(RefSkel.GetBoneName(I));
        UAL_BoneRoles::FBoneRoles Roles;
        if (!ResolveRoles(Input, Names, Roles, Error)) return nullptr;
        WriteRoles(Result, Roles);
        auto R = [&Roles](const TCHAR* Role) { return Roles.Bone(Role); };
        bool bDescribe = false;
        Input->TryGetBoolField(TEXT("describe"), bDescribe);
        if (bDescribe) return Result;

        TArray<int32> Frames;
        const FValues* RequestedFrames = nullptr;
        if (Input->TryGetArrayField(TEXT("frames"), RequestedFrames))
        {
            for (const auto& V : *RequestedFrames)
            {
                const double N = V->AsNumber();
                if (!FMath::IsFinite(N) || N < 0 || N >= Model->GetNumberOfKeys() || N != FMath::FloorToDouble(N))
                { Error = TEXT("Frame index out of range"); return nullptr; }
                Frames.AddUnique(static_cast<int32>(N));
            }
            Frames.Sort();
        }
        else for (int32 I = 0; I < Model->GetNumberOfKeys(); ++I) Frames.Add(I);
        if (Frames.IsEmpty() || Frames.Num() > 10000)
        { Error = TEXT("Select 1-10000 frames"); return nullptr; }

        FAnimPose Ref;
        UAnimPoseExtensions::GetReferencePose(Anim->GetSkeleton(), Ref);
        const FString MissingAxes = Roles.Missing({TEXT("clavicle_l"), TEXT("clavicle_r"), TEXT("foot_l"), TEXT("ball_l"), TEXT("pelvis")});
        FVector Forward = FVector::ZeroVector;
        if (MissingAxes.IsEmpty())
        {
            Forward = Horizontal(FVector::CrossProduct(FVector::UpVector, Position(Ref, R(TEXT("clavicle_r"))) - Position(Ref, R(TEXT("clavicle_l")))));
            if (FVector::DotProduct(Forward, Horizontal(Position(Ref, R(TEXT("ball_l"))) - Position(Ref, R(TEXT("foot_l"))))) < 0) Forward *= -1;
        }
        FAnimPoseEvaluationOptions Options;
        Options.bExtractRootMotion = true;
        TArray<double> Times;
        FValues FrameJson;
        for (int32 Frame : Frames)
        {
            Times.Add(FMath::Min(Model->GetFrameRate().AsSeconds(Frame), static_cast<double>(Anim->GetPlayLength())));
            FrameJson.Add(MakeShared<FJsonValueNumber>(Frame));
        }
        TArray<FAnimPose> Poses;
        UAnimPoseExtensions::GetAnimPoseAtTimeIntervals(Anim, Times, Options, Poses);
        if (Poses.Num() != Frames.Num() || Poses.ContainsByPredicate([](const FAnimPose& P) { return !P.IsValid(); }))
        { Error = TEXT("Pose evaluation failed"); return nullptr; }
        Result->SetArrayField(TEXT("sample_frames"), FrameJson);
        Result->SetStringField(TEXT("space"), TEXT("component"));
        Result->SetBoolField(TEXT("root_motion_enabled"), Anim->bEnableRootMotion);
        Result->SetBoolField(TEXT("bExtractRootMotion"), true);
        Result->SetStringField(TEXT("root_motion_note"), TEXT("Engine evaluation defaults retained; component displacements may include whole-body motion. Head pitch is not eye gaze."));
        FBox RootBounds(ForceInit);
        for (const auto& P : Poses) RootBounds += Position(P, RefSkel.GetBoneName(0));
        Result->SetObjectField(TEXT("root_displacement_range_cm"), UAL_CommandUtils::MakeVectorJson(RootBounds.GetSize()));
        bool bFixedFacing = !Forward.IsNearlyZero() && MissingAxes.IsEmpty();
        if (bFixedFacing)
        {
            const FVector PelvisForward = Bone(Ref, R(TEXT("pelvis"))).GetRotation().UnrotateVector(Forward);
            for (const auto& P : Poses)
            {
                const FVector Facing = Horizontal(Bone(P, R(TEXT("pelvis"))).GetRotation().RotateVector(PelvisForward));
                if (Facing.IsNearlyZero() || Angle(Forward, Facing) > 5.0) bFixedFacing = false;
            }
        }
        TArray<FString> Selected = {TEXT("elbow_out_deg"), TEXT("toe_out_deg"), TEXT("knee_bend_deg"), TEXT("gaze_pitch_deg"), TEXT("vertical_range_cm"), TEXT("torso_yaw_deg"), TEXT("hand_step_cm")};
        const FValues* RequestedMetrics = nullptr;
        if (Input->TryGetArrayField(TEXT("metrics"), RequestedMetrics))
        {
            TArray<FString> Filtered;
            for (const auto& V : *RequestedMetrics)
            {
                if (!Selected.Contains(V->AsString())) { Error = TEXT("Unknown metric"); return nullptr; }
                Filtered.AddUnique(V->AsString());
            }
            Selected = MoveTemp(Filtered);
        }
        FJson Metrics = MakeShared<FJsonObject>();
        for (const FString& Metric : Selected)
        {
            const bool bPaired = Metric == TEXT("elbow_out_deg") || Metric == TEXT("toe_out_deg") || Metric == TEXT("knee_bend_deg") || Metric == TEXT("hand_step_cm");
            const TArray<FString> Sides = bPaired ? TArray<FString>{TEXT("l"), TEXT("r")} : TArray<FString>{TEXT("")};
            FJson Entries = MakeShared<FJsonObject>();
            for (const FString& Side : Sides)
            {
                TArray<FString> RequiredRoles;
                auto Limb = [&Side](const TCHAR* Base) { return FString(Base) + TEXT("_") + Side; };
                if (Metric == TEXT("elbow_out_deg")) RequiredRoles = {Limb(TEXT("upperarm")), Limb(TEXT("lowerarm"))};
                if (Metric == TEXT("toe_out_deg")) RequiredRoles = {Limb(TEXT("foot")), Limb(TEXT("ball"))};
                if (Metric == TEXT("knee_bend_deg")) RequiredRoles = {Limb(TEXT("thigh")), Limb(TEXT("calf")), Limb(TEXT("foot"))};
                if (Metric == TEXT("gaze_pitch_deg")) RequiredRoles = {TEXT("head")};
                if (Metric == TEXT("vertical_range_cm")) RequiredRoles = {TEXT("pelvis"), TEXT("chest")};
                if (Metric == TEXT("torso_yaw_deg")) RequiredRoles = {TEXT("clavicle_l"), TEXT("clavicle_r")};
                if (Metric == TEXT("hand_step_cm")) RequiredRoles = {Limb(TEXT("hand"))};
                TArray<FName> Required;
                for (const FString& Role : RequiredRoles) Required.Add(Roles.Bone(Role));
                const FString Absent = Roles.Missing(RequiredRoles);
                const bool bNeedsFacing = Metric == TEXT("elbow_out_deg") || Metric == TEXT("toe_out_deg") || Metric == TEXT("torso_yaw_deg");
                FJson Entry;
                if (!Absent.IsEmpty()) Entry = Failure(TEXT("Missing bone roles: ") + Absent + TEXT(" (no bone matched the Manny name or common naming conventions; name them with bone_map)"));
                else if ((bNeedsFacing && !bFixedFacing) || (Metric == TEXT("gaze_pitch_deg") && Forward.IsNearlyZero()))
                    Entry = Failure(TEXT("Missing/degenerate reference axes or pelvis facing differs by more than 5 degrees; fixed-facing metric is not applicable. ") + MissingAxes);
                else
                {
                    TArray<double> Values;
                    TArray<int32> ValueFrames;
                    bool bDegenerate = false;
                    double PelvisMin = DBL_MAX, PelvisMax = -DBL_MAX, ChestMin = DBL_MAX, ChestMax = -DBL_MAX;
                    for (int32 I = 0; I < Poses.Num(); ++I)
                    {
                        const FAnimPose& P = Poses[I];
                        double Value = 0;
                        if (Metric == TEXT("elbow_out_deg"))
                        {
                            FVector Arm = Position(P, Required[1]) - Position(P, Required[0]);
                            Arm -= Forward * FVector::DotProduct(Arm, Forward);
                            bDegenerate |= Arm.IsNearlyZero(); Value = Angle(Arm, -FVector::UpVector);
                        }
                        else if (Metric == TEXT("toe_out_deg"))
                        {
                            const FVector Toe = Horizontal(Position(P, Required[1]) - Position(P, Required[0]));
                            bDegenerate |= Toe.IsNearlyZero(); Value = SignedAngle(Forward, Toe) * (Side == TEXT("r") ? 1 : -1);
                        }
                        else if (Metric == TEXT("knee_bend_deg"))
                        {
                            FVector A = Position(P, Required[1]) - Position(P, Required[0]), B = Position(P, Required[2]) - Position(P, Required[1]);
                            bDegenerate |= A.IsNearlyZero() || B.IsNearlyZero(); Value = Angle(A, B);
                        }
                        else if (Metric == TEXT("gaze_pitch_deg"))
                        {
                            const FVector LocalForward = Bone(Ref, Required[0]).GetRotation().UnrotateVector(Forward);
                            Value = Bone(P, Required[0]).GetRotation().RotateVector(LocalForward).Rotation().Pitch;
                        }
                        else if (Metric == TEXT("vertical_range_cm"))
                        {
                            const double Pelvis = Position(P, Required[0]).Z, Chest = Position(P, Required[1]).Z;
                            PelvisMin = FMath::Min(PelvisMin, Pelvis); PelvisMax = FMath::Max(PelvisMax, Pelvis);
                            ChestMin = FMath::Min(ChestMin, Chest); ChestMax = FMath::Max(ChestMax, Chest);
                            Value = Pelvis;
                        }
                        else if (Metric == TEXT("torso_yaw_deg"))
                        {
                            FVector Facing = Horizontal(FVector::CrossProduct(FVector::UpVector, Position(P, Required[1]) - Position(P, Required[0])));
                            const FVector RefFacing = Horizontal(FVector::CrossProduct(FVector::UpVector, Position(Ref, Required[1]) - Position(Ref, Required[0])));
                            if (FVector::DotProduct(RefFacing, Forward) < 0) Facing *= -1;
                            bDegenerate |= Facing.IsNearlyZero(); Value = -SignedAngle(Forward, Facing);
                        }
                        else if (Metric == TEXT("hand_step_cm"))
                        {
                            // Gaps are not adjacent frames and must not be presented as frame jumps.
                            if (I == 0 || Frames[I] != Frames[I - 1] + 1) continue;
                            Value = FVector::Distance(Position(P, Required[0]), Position(Poses[I - 1], Required[0]));
                        }
                        Values.Add(Value); ValueFrames.Add(Frames[I]);
                    }
                    Entry = bDegenerate ? Failure(TEXT("Zero-length measurement vector")) : Series(Values, ValueFrames, Metric.EndsWith(TEXT("cm")) ? TEXT("cm") : TEXT("deg"));
                    if (Metric == TEXT("vertical_range_cm"))
                    { Entry->SetNumberField(TEXT("pelvis_range"), PelvisMax - PelvisMin); Entry->SetNumberField(TEXT("chest_range"), ChestMax - ChestMin); }
                }
                if (bPaired) Entries->SetObjectField(Side, Entry); else Entries = Entry;
            }
            Metrics->SetObjectField(Metric, Entries);
        }
        Result->SetObjectField(TEXT("metrics"), Metrics);
        const FValues* RequestedBones = nullptr;
        if (Input->TryGetArrayField(TEXT("bones"), RequestedBones))
        {
            FJson Sequences = MakeShared<FJsonObject>();
            for (const auto& V : *RequestedBones)
            {
                // 骨名或角色名都认：Biped 上 bones: ["pelvis"] 量的是 Bip001-Pelvis
                const FName Name = Roles.Lookup(V->AsString(), Names);
                if (Name.IsNone()) { Sequences->SetObjectField(V->AsString(), Failure(TEXT("Missing bone"))); continue; }
                FValues Points;
                for (const auto& P : Poses) Points.Add(MakeShared<FJsonValueObject>(UAL_CommandUtils::MakeVectorJson(Position(P, Name))));
                Sequences->SetArrayField(V->AsString(), Points);
            }
            Result->SetObjectField(TEXT("bone_positions_cm"), Sequences);
        }
        const FValues* AngleBones = nullptr;
        if (Input->TryGetArrayField(TEXT("angle_bones"), AngleBones))
        {
            TArray<FName> Required;
            for (const auto& V : *AngleBones) Required.Add(Roles.Lookup(V->AsString(), Names));
            if (Required.Num() != 4 || !Missing(Names, Required).IsEmpty()) Result->SetObjectField(TEXT("angle_deg"), Failure(TEXT("Four existing bones required")));
            else
            {
                TArray<double> Values; bool bValid = true;
                for (const auto& P : Poses)
                {
                    const FVector A = Position(P, Required[1]) - Position(P, Required[0]), B = Position(P, Required[3]) - Position(P, Required[2]);
                    bValid &= !A.IsNearlyZero() && !B.IsNearlyZero(); Values.Add(Angle(A, B));
                }
                Result->SetObjectField(TEXT("angle_deg"), bValid ? Series(Values, Frames, TEXT("deg")) : Failure(TEXT("Zero-length vector")));
            }
        }
        return Result;
    }

    static void Measure(const FJson& Input, const FString& Id)
    {
        FString Error;
        const FJson Result = MeasureSequence(LoadAnimation(Input, TEXT("path")), Input, Error);
        if (Result.IsValid()) UAL_CommandUtils::SendResponse(Id, 200, Result);
        else UAL_CommandUtils::SendError(Id, 400, Error);
    }

    FJson WritePoseSequence(UAnimSequence* Target, UAnimSequence* Source, const FJson& Input, FString& Error)
    {
        if (!Target || !Source || !Target->GetSkeleton() || Target->GetSkeleton() != Source->GetSkeleton() || Target->IsValidAdditive() || Source->IsValidAdditive())
        { Error = TEXT("Source and target must be non-additive sequences with the same skeleton"); return nullptr; }
        double StartValue = -1, EndValue = -1, Time = -1, TransitionValue = 0;
        Input->TryGetNumberField(TEXT("start_frame"), StartValue); Input->TryGetNumberField(TEXT("end_frame"), EndValue);
        Input->TryGetNumberField(TEXT("source_time"), Time); Input->TryGetNumberField(TEXT("transition_frames"), TransitionValue);
        if (!FMath::IsFinite(StartValue) || !FMath::IsFinite(EndValue) || !FMath::IsFinite(Time) || !FMath::IsFinite(TransitionValue))
        { Error = TEXT("Frame indices, transition and time must be finite"); return nullptr; }
        const IAnimationDataModel* Model = Target->GetDataModel();
        if (!Model || !Source->GetDataModel() || StartValue < 0 || EndValue < StartValue || EndValue >= Model->GetNumberOfKeys() || Time < 0 || Time > Source->GetPlayLength() || TransitionValue < 0 || StartValue != FMath::FloorToDouble(StartValue) || EndValue != FMath::FloorToDouble(EndValue) || TransitionValue != FMath::FloorToDouble(TransitionValue))
        { Error = TEXT("Invalid frame interval, transition or source time"); return nullptr; }
        const int32 Start = static_cast<int32>(StartValue), End = static_cast<int32>(EndValue), Transition = static_cast<int32>(TransitionValue);
        const int32 Count = Model->GetNumberOfKeys();
        if (Count > 10000 || Transition > Count)
        { Error = TEXT("Pose write supports at most 10000 keys; transition must fit the sequence"); return nullptr; }
        const FReferenceSkeleton& Ref = Target->GetSkeleton()->GetReferenceSkeleton();
        FAnimPose SourcePose;
        FAnimPoseEvaluationOptions Options;
        Options.EvaluationType = EAnimDataEvalType::Source;
        Options.bShouldRetarget = false;
        UAnimPoseExtensions::GetAnimPoseAtTime(Source, Time, Options, SourcePose);
        if (!SourcePose.IsValid()) { Error = TEXT("Source pose evaluation failed"); return nullptr; }
        TArray<TArray<FTransform>> Before, Expected;
        TArray<bool> HadTrack;
        for (int32 BoneIndex = 0; BoneIndex < Ref.GetRawBoneNum(); ++BoneIndex)
        {
            const FName Name = Ref.GetBoneName(BoneIndex);
            TArray<FTransform> Keys;
            const bool bHasTrack = Model->IsValidBoneTrackName(Name);
            HadTrack.Add(bHasTrack);
            if (bHasTrack) Model->GetBoneTrackTransforms(Name, Keys);
            else Keys.Init(Ref.GetRefBonePose()[BoneIndex], Count);
            if (Keys.Num() != Count) { Error = TEXT("Unexpected source track key count; nothing written"); return nullptr; }
            Before.Add(Keys);
            const FTransform Pose = UAnimPoseExtensions::GetBonePose(SourcePose, Name, EAnimPoseSpaces::Local);
            for (int32 Frame = 0; Frame < Count; ++Frame)
            {
                const int32 Distance = Frame < Start ? Start - Frame : Frame > End ? Frame - End : 0;
                const double Alpha = Distance == 0 ? 1.0 : Transition > 0 ? FMath::Max(0.0, 1.0 - static_cast<double>(Distance) / (Transition + 1)) : 0;
                if (Alpha > 0) Keys[Frame].Blend(Before.Last()[Frame], Pose, Alpha);
            }
            Expected.Add(MoveTemp(Keys));
        }
        FScopedTransaction Transaction(NSLOCTEXT("UnrealAgentLink", "WriteAnimationPose", "Write animation pose"));
        IAnimationDataController& Controller = Target->GetController();
        Controller.OpenBracket(FText::FromString(TEXT("UnrealAgent animation pose")));
        auto SetKeys = [&Controller](FName Name, const TArray<FTransform>& Keys)
        {
            TArray<FVector3f> Positions, Scales; TArray<FQuat4f> Rotations;
            for (const FTransform& Key : Keys) { Positions.Add(FVector3f(Key.GetLocation())); Rotations.Add(FQuat4f(Key.GetRotation())); Scales.Add(FVector3f(Key.GetScale3D())); }
            return Controller.SetBoneTrackKeys(Name, Positions, Rotations, Scales);
        };
        bool bValid = true;
        for (int32 B = 0; B < Ref.GetRawBoneNum(); ++B)
        {
            const FName Name = Ref.GetBoneName(B);
            if (!HadTrack[B] && !Controller.AddBoneCurve(Name)) { bValid = false; break; }
            if (!SetKeys(Name, Expected[B])) { bValid = false; break; }
        }
        if (bValid)
        {
            for (int32 B = 0; B < Ref.GetRawBoneNum() && bValid; ++B)
            {
                TArray<FTransform> Actual;
                Model->GetBoneTrackTransforms(Ref.GetBoneName(B), Actual);
                if (Actual.Num() != Count) { bValid = false; break; }
                for (int32 F = 0; F < Count; ++F) if (!Actual[F].Equals(Expected[B][F], 0.0001)) { bValid = false; break; }
            }
        }
        if (!bValid)
        {
            // Cancel() alone does not undo changes. Explicitly restore tracks before dropping the transaction.
            // 还原本身也会失败：原来 SetKeys 的返回值直接丢掉，却照样报「original tracks restored」。
            // 现在每条轨道还原后按写入时同样的容差读回比对，没还原上的逐个记名
            TArray<FString> NotRestored;
            for (int32 B = 0; B < Ref.GetRawBoneNum(); ++B)
            {
                const FName Name = Ref.GetBoneName(B);
                if (HadTrack[B])
                {
                    bool bRestored = SetKeys(Name, Before[B]);
                    TArray<FTransform> Actual;
                    if (bRestored) Model->GetBoneTrackTransforms(Name, Actual);
                    bRestored = bRestored && Actual.Num() == Count;
                    for (int32 F = 0; bRestored && F < Count; ++F) bRestored = Actual[F].Equals(Before[B][F], 0.0001);
                    if (!bRestored) NotRestored.Add(Name.ToString());
                }
                else if (Model->IsValidBoneTrackName(Name))
                {
                    Controller.RemoveBoneTrack(Name);
                    if (Model->IsValidBoneTrackName(Name)) NotRestored.Add(Name.ToString());
                }
            }
            Controller.CloseBracket();
            if (NotRestored.Num() == 0)
            {
                Transaction.Cancel();
                Error = TEXT("Pose readback failed; original tracks restored and verified by readback, not saved"); return nullptr;
            }
            // 没还原干净时**不** Cancel：Cancel 只把撤销记录丢掉、不回滚，
            // 丢了这条记录用户连 Ctrl+Z 都没有。让事务照常提交，并把没还原的骨头说出来
            const int32 Shown = FMath::Min(NotRestored.Num(), 8);
            FString Names = FString::Join(TArray<FString>(NotRestored.GetData(), Shown), TEXT(", "));
            if (NotRestored.Num() > Shown) Names += FString::Printf(TEXT(" ... (+%d)"), NotRestored.Num() - Shown);
            Error = FString::Printf(
                TEXT("Pose readback failed, and restoring the original tracks also failed for %d bone(s): %s. ")
                TEXT("The animation in memory is modified but NOT saved. Do not save it; press Ctrl+Z in the editor, ")
                TEXT("or discard changes and reload the asset, then inspect before retrying."),
                NotRestored.Num(), *Names);
            return nullptr;
        }
        Controller.CloseBracket();
        FJson Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("path"), Target->GetPathName());
        Result->SetBoolField(TEXT("verified_local_transforms"), true);
        Result->SetBoolField(TEXT("saved"), false);
        Result->SetNumberField(TEXT("verified_frames"), Count);
        Result->SetNumberField(TEXT("verified_bones"), Ref.GetRawBoneNum());
        return Result;
    }
    static void WritePose(const FJson& Input, const FString& Id)
    {
        UAnimSequence* Target = LoadAnimation(Input, TEXT("path"));
        FString Error;
        FJson Result = WritePoseSequence(Target, LoadAnimation(Input, TEXT("source")), Input, Error);
        if (!Result.IsValid()) { UAL_CommandUtils::SendError(Id, 400, Error); return; }
        if (!UEditorLoadingAndSavingUtils::SavePackages({Target->GetOutermost()}, false))
        { UAL_CommandUtils::SendError(Id, 500, TEXT("Pose verified in memory but saving failed; inspect before retrying")); return; }
        Result->SetBoolField(TEXT("saved"), true);
        UAL_CommandUtils::SendResponse(Id, 200, Result);
    }
}
#endif

void FUAL_AnimationCommands::RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& Commands)
{
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
    Commands.Add(TEXT("anim.measure"), UALAnimation::Measure);
    Commands.Add(TEXT("anim.write_pose"), UALAnimation::WritePose);
    Commands.Add(TEXT("anim.preview"), UALAnimation::Preview);
    Commands.Add(TEXT("anim.retarget"), UALAnimation::Retarget);
#else
    for (const FString& Method : {TEXT("anim.measure"), TEXT("anim.write_pose"), TEXT("anim.preview"), TEXT("anim.retarget")})
        Commands.Add(Method, [](const TSharedPtr<FJsonObject>&, const FString Id) { UAL_CommandUtils::SendError(Id, 501, TEXT("Animation tools require UE 5.5 or later")); });
#endif
}
