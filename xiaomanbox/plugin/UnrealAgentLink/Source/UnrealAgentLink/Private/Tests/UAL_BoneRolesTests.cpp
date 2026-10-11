#include "UAL_BoneRoles.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUALBoneRolesTest,
    "UnrealAgentLink.Animation.BoneRoles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUALBoneRolesTest::RunTest(const FString& Parameters)
{
    auto Names = [](std::initializer_list<const TCHAR*> List)
    {
        TArray<FName> Out;
        for (const TCHAR* Name : List) Out.Add(FName(Name));
        return Out;
    };
    auto Expect = [this](const UAL_BoneRoles::FBoneRoles& Roles, const TCHAR* Role, const TCHAR* Bone)
    {
        TestEqual(FString::Printf(TEXT("%s -> %s"), Role, Bone), Roles.Bone(Role).ToString(), FString(Bone));
    };

    // ── 归一化 ────────────────────────────────────────────────────────────
    TestEqual(TEXT("Biped 连字符"), UAL_BoneRoles::Normalize(TEXT("Bip001-L-Foot")), FString(TEXT("lfoot")));
    TestEqual(TEXT("Biped 空格"), UAL_BoneRoles::Normalize(TEXT("Bip01 R Toe0")), FString(TEXT("rtoe0")));
    TestEqual(TEXT("Mixamo 冒号"), UAL_BoneRoles::Normalize(TEXT("mixamorig:LeftUpLeg")), FString(TEXT("leftupleg")));
    TestEqual(TEXT("Mixamo 下划线"), UAL_BoneRoles::Normalize(TEXT("mixamorig_Hips")), FString(TEXT("hips")));
    TestEqual(TEXT("Blender 后缀"), UAL_BoneRoles::Normalize(TEXT("foot.L")), FString(TEXT("footl")));
    // 只剩前缀的是 Biped 根骨，不能剥成空串
    TestEqual(TEXT("Biped 根"), UAL_BoneRoles::Normalize(TEXT("Bip001")), FString(TEXT("bip001")));

    // ── Manny：行为和原来一字不差 ─────────────────────────────────────────
    {
        const auto Roles = UAL_BoneRoles::Resolve(Names({TEXT("root"), TEXT("pelvis"), TEXT("spine_01"), TEXT("spine_05"),
            TEXT("head"), TEXT("clavicle_l"), TEXT("upperarm_l"), TEXT("upperarm_twist_01_l"), TEXT("foot_l"), TEXT("ball_l")}), {});
        Expect(Roles, TEXT("chest"), TEXT("spine_05"));
        Expect(Roles, TEXT("upperarm_l"), TEXT("upperarm_l"));
        TestEqual(TEXT("Manny 不算猜的"), Roles.Roles.FindChecked(TEXT("foot_l")).Via, FString(TEXT("manny")));
    }

    // ── 用户反馈里那副 Biped ──────────────────────────────────────────────
    {
        const auto Roles = UAL_BoneRoles::Resolve(Names({TEXT("Bip001"), TEXT("Bip001-Pelvis"), TEXT("Bip001-Spine"),
            TEXT("Bip001-Spine1"), TEXT("Bip001-Spine2"), TEXT("Bip001-Neck"), TEXT("Bip001-Head"),
            TEXT("Bip001-L-Clavicle"), TEXT("Bip001-L-UpperArm"), TEXT("Bip001-L-Forearm"), TEXT("Bip001-L-Hand"),
            TEXT("Bip001-R-Clavicle"), TEXT("Bip001-L-Thigh"), TEXT("Bip001-L-Calf"), TEXT("Bip001-L-Foot"), TEXT("Bip001-L-Toe0")}), {});
        Expect(Roles, TEXT("pelvis"), TEXT("Bip001-Pelvis"));
        Expect(Roles, TEXT("chest"), TEXT("Bip001-Spine2"));
        Expect(Roles, TEXT("head"), TEXT("Bip001-Head"));
        Expect(Roles, TEXT("clavicle_r"), TEXT("Bip001-R-Clavicle"));
        Expect(Roles, TEXT("lowerarm_l"), TEXT("Bip001-L-Forearm"));
        Expect(Roles, TEXT("calf_l"), TEXT("Bip001-L-Calf"));
        Expect(Roles, TEXT("ball_l"), TEXT("Bip001-L-Toe0"));
        TestEqual(TEXT("Biped 标为按命名约定"), Roles.Roles.FindChecked(TEXT("pelvis")).Via, FString(TEXT("alias")));
        TestEqual(TEXT("缺的角色报出来"), Roles.Missing({TEXT("pelvis"), TEXT("hand_r")}), FString(TEXT("hand_r")));
        // 骨名或角色名都认
        Expect(Roles, TEXT("foot_l"), TEXT("Bip001-L-Foot"));
        TestEqual(TEXT("Lookup 角色名"), Roles.Lookup(TEXT("pelvis"), Names({TEXT("Bip001-Pelvis")})).ToString(), FString(TEXT("Bip001-Pelvis")));
    }

    // ── Mixamo：LeftArm 是上臂、LeftShoulder 是锁骨、LeftLeg 是小腿 ─────────
    {
        const auto Roles = UAL_BoneRoles::Resolve(Names({TEXT("mixamorig:Hips"), TEXT("mixamorig:Spine"), TEXT("mixamorig:Spine1"),
            TEXT("mixamorig:Spine2"), TEXT("mixamorig:LeftShoulder"), TEXT("mixamorig:LeftArm"), TEXT("mixamorig:LeftForeArm"),
            TEXT("mixamorig:LeftUpLeg"), TEXT("mixamorig:LeftLeg"), TEXT("mixamorig:LeftFoot"), TEXT("mixamorig:LeftToeBase")}), {});
        Expect(Roles, TEXT("pelvis"), TEXT("mixamorig:Hips"));
        Expect(Roles, TEXT("chest"), TEXT("mixamorig:Spine2"));
        Expect(Roles, TEXT("clavicle_l"), TEXT("mixamorig:LeftShoulder"));
        Expect(Roles, TEXT("upperarm_l"), TEXT("mixamorig:LeftArm"));
        Expect(Roles, TEXT("thigh_l"), TEXT("mixamorig:LeftUpLeg"));
        Expect(Roles, TEXT("calf_l"), TEXT("mixamorig:LeftLeg"));
        Expect(Roles, TEXT("ball_l"), TEXT("mixamorig:LeftToeBase"));
    }

    // ── 显式点名优先，写错就报错，不拿别名兜底 ────────────────────────────
    {
        const TArray<FName> Bones = Names({TEXT("Hips"), TEXT("Chest"), TEXT("Spine2")});
        const auto Pinned = UAL_BoneRoles::Resolve(Bones, {{TEXT("chest"), TEXT("Spine2")}});
        Expect(Pinned, TEXT("chest"), TEXT("Spine2"));
        TestEqual(TEXT("显式点名不算猜"), Pinned.Roles.FindChecked(TEXT("chest")).Via, FString(TEXT("explicit")));

        const auto Wrong = UAL_BoneRoles::Resolve(Bones, {{TEXT("chest"), TEXT("NoSuchBone")}, {TEXT("tail"), TEXT("Hips")}});
        TestEqual(TEXT("两条错误都报"), Wrong.Errors.Num(), 2);
        TestTrue(TEXT("点错的角色不被别名兜底"), Wrong.Bone(TEXT("chest")).IsNone());
    }

    // 整串相等：扭转骨不能被认成上臂
    {
        const auto Roles = UAL_BoneRoles::Resolve(Names({TEXT("Bip001-L-UpperArmTwist")}), {});
        TestTrue(TEXT("扭转骨不是上臂"), Roles.Bone(TEXT("upperarm_l")).IsNone());
    }

    return true;
}

#endif
