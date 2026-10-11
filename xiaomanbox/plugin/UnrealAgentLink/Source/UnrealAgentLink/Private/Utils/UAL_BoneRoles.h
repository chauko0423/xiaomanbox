#pragma once

#include "CoreMinimal.h"

/**
 * 骨骼语义角色：「骨盆」「左脚」这类角色 → 这副骨架上实际叫什么名字。
 *
 * ## 为什么要有
 *
 * `anim.measure` 的默认指标和 `anim.preview` 的机位原来写死了 Manny 骨名
 * （pelvis / spine_05 / clavicle_l / foot_l / ball_l）。2026-09-24 用户反馈里是一副
 * 3ds Max Biped 骨架（Bip001-Pelvis、Bip001-L-Foot），七项默认指标全部
 * `unmeasurable: Missing bones: pelvis, spine_05`，预览直接 400 —— 同一次调用里
 * 显式传 `bones` 又量得出坐标。数据是能量的，是契约只认一种命名。
 *
 * ## 怎么认
 *
 * 1. 调用方用 `bone_map` 显式点名的，最优先；点了不存在的骨头报错，不猜
 * 2. 骨架里正好有 Manny 名字的，用它（原来的行为，一个字不变）
 * 3. 否则按常见命名约定找：先把骨名归一化（去掉 `mixamorig:` / `Bip001` 前缀、
 *    去掉分隔符、转小写），再**整串相等**地比对别名。
 *    整串相等是刻意的 —— `upperarm_twist_01_l` 不能被当成上臂
 *
 * 靠别名认出来的角色会在回执里单独列出，模型据此知道哪些是猜的。
 * 纯字符串逻辑，不碰 UObject，测试见 `Private/Tests/UAL_BoneRolesTests.cpp`。
 */
namespace UAL_BoneRoles
{
	/** 骨名归一化：去命名空间前缀、去分隔符、转小写、去 Biped / Mixamo 的根前缀 */
	inline FString Normalize(const FString& In)
	{
		FString Source = In;
		int32 Colon = INDEX_NONE;
		if (Source.FindLastChar(TEXT(':'), Colon))
		{
			Source.RightChopInline(Colon + 1);
		}

		FString Out;
		Out.Reserve(Source.Len());
		for (const TCHAR C : Source)
		{
			if (FChar::IsAlnum(C))
			{
				Out.AppendChar(FChar::ToLower(C));
			}
		}

		// Mixamo 导入后冒号常被换成下划线（mixamorig_LeftFoot），后面可以跟数字；
		// Biped 的前缀是 Bip + 至少一位数字（Bip001、Bip01）。只剩前缀本身时不剥 ——
		// 那是 Biped 的根骨，剥成空串会和别的根撞在一起
		auto Strip = [&Out](const TCHAR* Prefix, bool bNeedDigit)
		{
			if (!Out.StartsWith(Prefix))
			{
				return;
			}
			int32 I = FCString::Strlen(Prefix);
			const int32 DigitsFrom = I;
			while (I < Out.Len() && FChar::IsDigit(Out[I]))
			{
				++I;
			}
			if ((!bNeedDigit || I > DigitsFrom) && I < Out.Len())
			{
				Out.RightChopInline(I);
			}
		};
		Strip(TEXT("mixamorig"), false);
		Strip(TEXT("bip"), true);
		return Out;
	}

	struct FRoleSpec
	{
		const TCHAR* Role;
		/** Manny / MetaHuman 上的名字 */
		const TCHAR* Manny;
		/** 归一化后的别名，按优先级 */
		TArray<const TCHAR*> Aliases;
	};

	/** 单侧角色的基名与别名；展开成 _l / _r 两个角色 */
	struct FSidedSpec
	{
		const TCHAR* Base;
		TArray<const TCHAR*> Aliases;
	};

	inline const TArray<FRoleSpec>& CenterRoles()
	{
		static const TArray<FRoleSpec> Roles = {
			{ TEXT("pelvis"), TEXT("pelvis"), { TEXT("pelvis"), TEXT("hips"), TEXT("hip") } },
			// 胸：取脊柱最上面那节。Manny 是 spine_05，UE4 小白人是 spine_03，
			// Biped 是 Spine2/Spine3，Mixamo 是 Spine2
			{ TEXT("chest"), TEXT("spine_05"),
				{ TEXT("spine05"), TEXT("spine04"), TEXT("spine03"), TEXT("upperchest"), TEXT("chest"),
					TEXT("spine3"), TEXT("spine2"), TEXT("spine1") } },
			{ TEXT("head"), TEXT("head"), { TEXT("head") } },
		};
		return Roles;
	}

	inline const TArray<FSidedSpec>& SidedRoles()
	{
		static const TArray<FSidedSpec> Roles = {
			{ TEXT("clavicle"), { TEXT("clavicle"), TEXT("shoulder") } },
			{ TEXT("upperarm"), { TEXT("upperarm"), TEXT("arm") } },
			{ TEXT("lowerarm"), { TEXT("lowerarm"), TEXT("forearm") } },
			{ TEXT("hand"), { TEXT("hand") } },
			{ TEXT("thigh"), { TEXT("thigh"), TEXT("upleg"), TEXT("upperleg") } },
			{ TEXT("calf"), { TEXT("calf"), TEXT("leg"), TEXT("lowerleg"), TEXT("shin") } },
			{ TEXT("foot"), { TEXT("foot"), TEXT("ankle") } },
			{ TEXT("ball"), { TEXT("ball"), TEXT("toe0"), TEXT("toebase"), TEXT("toes"), TEXT("toe") } },
		};
		return Roles;
	}

	/** 全部角色名，按 CenterRoles、SidedRoles(_l, _r) 的顺序 */
	inline TArray<FString> AllRoles()
	{
		TArray<FString> Out;
		for (const FRoleSpec& Spec : CenterRoles())
		{
			Out.Add(Spec.Role);
		}
		for (const FSidedSpec& Spec : SidedRoles())
		{
			Out.Add(FString(Spec.Base) + TEXT("_l"));
			Out.Add(FString(Spec.Base) + TEXT("_r"));
		}
		return Out;
	}

	struct FResolved
	{
		FName Bone;
		/** explicit | manny | alias */
		FString Via;
	};

	struct FBoneRoles
	{
		TMap<FString, FResolved> Roles;
		/** bone_map 里写错的条目。有这一项时调用方应当报错而不是静默忽略 */
		TArray<FString> Errors;

		FName Bone(const FString& Role) const
		{
			const FResolved* Found = Roles.Find(Role);
			return Found ? Found->Bone : NAME_None;
		}

		/**
		 * 骨名或角色名 → 骨名。
		 *
		 * 先当骨名找（骨架里真有这根骨头就用它），找不到再当角色名解析。
		 * 于是 Biped 上 `bones: ["pelvis"]` 也能量到 Bip001-Pelvis。
		 */
		FName Lookup(const FString& NameOrRole, const TArray<FName>& Bones) const
		{
			const FName AsBone(*NameOrRole);
			if (Bones.Contains(AsBone))
			{
				return AsBone;
			}
			return Bone(NameOrRole);
		}

		/** 缺哪些角色，逗号分隔；全在时为空串 */
		FString Missing(const TArray<FString>& Required) const
		{
			TArray<FString> Out;
			for (const FString& Role : Required)
			{
				if (Bone(Role).IsNone())
				{
					Out.Add(Role);
				}
			}
			return FString::Join(Out, TEXT(", "));
		}
	};

	/**
	 * 在一副骨架上解析全部角色。
	 *
	 * `Explicit` 的键必须是 `AllRoles()` 里的角色名，值必须是骨架里存在的骨名；
	 * 否则进 `Errors`，那个角色不会被别名兜底 —— 用户点了名却被悄悄换成
	 * 别的骨头，比报错更糟。
	 */
	inline FBoneRoles Resolve(const TArray<FName>& Bones, const TMap<FString, FString>& Explicit)
	{
		FBoneRoles Out;
		const TArray<FString> Known = AllRoles();

		// 归一化名 → 骨名；多根骨头归一化后撞名时取骨架里靠前的那根
		TMap<FString, FName> ByNormalized;
		for (const FName& Bone : Bones)
		{
			const FString Key = Normalize(Bone.ToString());
			if (!Key.IsEmpty() && !ByNormalized.Contains(Key))
			{
				ByNormalized.Add(Key, Bone);
			}
		}

		TSet<FString> Pinned;
		for (const TPair<FString, FString>& Entry : Explicit)
		{
			if (!Known.Contains(Entry.Key))
			{
				Out.Errors.Add(FString::Printf(TEXT("unknown role '%s'"), *Entry.Key));
				continue;
			}
			Pinned.Add(Entry.Key);
			const FName Bone(*Entry.Value);
			if (!Bones.Contains(Bone))
			{
				Out.Errors.Add(FString::Printf(TEXT("%s -> '%s' is not a bone of this skeleton"), *Entry.Key, *Entry.Value));
				continue;
			}
			Out.Roles.Add(Entry.Key, { Bone, TEXT("explicit") });
		}

		auto Assign = [&](const FString& Role, const TCHAR* Manny, const TArray<FString>& Aliases)
		{
			if (Pinned.Contains(Role))
			{
				return;
			}
			const FName MannyName(Manny);
			if (Bones.Contains(MannyName))
			{
				Out.Roles.Add(Role, { MannyName, TEXT("manny") });
				return;
			}
			for (const FString& Alias : Aliases)
			{
				if (const FName* Found = ByNormalized.Find(Alias))
				{
					Out.Roles.Add(Role, { *Found, TEXT("alias") });
					return;
				}
			}
		};

		for (const FRoleSpec& Spec : CenterRoles())
		{
			TArray<FString> Aliases;
			for (const TCHAR* Alias : Spec.Aliases)
			{
				Aliases.Add(Alias);
			}
			Assign(Spec.Role, Spec.Manny, Aliases);
		}

		for (const FSidedSpec& Spec : SidedRoles())
		{
			for (const TCHAR* Side : { TEXT("l"), TEXT("r") })
			{
				const FString Word = FCString::Strcmp(Side, TEXT("l")) == 0 ? TEXT("left") : TEXT("right");
				// foot_l（Manny、Blender 的 foot.L）、L Foot（Biped）、LeftFoot（Mixamo、Unity）、foot_left
				TArray<FString> Aliases;
				for (const TCHAR* Alias : Spec.Aliases)
				{
					Aliases.Add(FString(Alias) + Side);
					Aliases.Add(FString(Side) + Alias);
					Aliases.Add(Word + Alias);
					Aliases.Add(FString(Alias) + Word);
				}
				const FString Role = FString(Spec.Base) + TEXT("_") + Side;
				Assign(Role, *Role, Aliases);
			}
		}

		return Out;
	}
}
