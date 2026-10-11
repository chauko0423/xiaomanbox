#include "UAL_MeshCommands.h"

#include "UAL_CommandUtils.h"
#include "UAL_MeshCompat.h"

#include "Animation/MorphTarget.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/StaticMeshSourceData.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "StaticMeshResources.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALMesh, Log, All);

namespace
{
	FString PathOf(const UObject* Obj)
	{
		return Obj ? Obj->GetPathName() : FString();
	}

	/**
	 * 「这个角色的脸朝网格的哪个方向」—— 从参考姿势的左右骨骼对推出来。
	 *
	 * ## 为什么需要这个
	 *
	 * 骨骼网格的「正面」不是引擎里存着的一个字段：导入时轴向怎么转全看
	 * DCC 那边，同一批素材里有朝 +X 的也有朝 +Y 的。以前工具一个字都不说，
	 * 调用方只能「摆好 → 试玩 → 截图目测」，而 T-pose 和运行时姿势不一样、
	 * 机位一远就糊，目测两次判错两次。
	 *
	 * ## 为什么用左右骨骼对，而不是别的
	 *
	 * 人形骨架里唯一稳定可测的方向量是**左右**：两只脚（或两条腿、两边
	 * 锁骨）在参考姿势下的连线就是角色的右轴，右轴叉上 Z 就是正前方
	 * （UE 左手系：Right × Up = Forward）。包围盒推不出朝向（人是左右
	 * 对称的），根骨骼旋转也推不出（多数骨架根骨骼是单位旋转）。
	 *
	 * ## 为什么骨骼名字认不出来时不猜
	 *
	 * 认不出就写 `known: false` 和原因，让调用方回到截图那条路。给一个
	 * 猜出来的角度比不给更坏 —— 调用方会拿它当事实去摆，错了还查不出
	 * 是哪一步错的。见「工具不回读校验就不许报 success」。
	 */
	TSharedPtr<FJsonObject> DescribeSkeletalFacing(const FReferenceSkeleton& RefSkeleton)
	{
		TSharedPtr<FJsonObject> Facing = MakeShared<FJsonObject>();

		const int32 BoneCount = RefSkeleton.GetNum();
		if (BoneCount <= 0)
		{
			Facing->SetBoolField(TEXT("known"), false);
			Facing->SetStringField(TEXT("reason"), TEXT("no bones"));
			return Facing;
		}

		// 参考姿势是每根骨骼相对父骨骼的，先顺着父链乘成组件空间
		const TArray<FTransform>& RefPose = RefSkeleton.GetRefBonePose();
		TArray<FTransform> ComponentSpace;
		ComponentSpace.SetNum(BoneCount);
		for (int32 Index = 0; Index < BoneCount; ++Index)
		{
			const FTransform& Local = RefPose.IsValidIndex(Index) ? RefPose[Index] : FTransform::Identity;
			const int32 Parent = RefSkeleton.GetParentIndex(Index);
			ComponentSpace[Index] = (Parent != INDEX_NONE && Parent < Index)
				? Local * ComponentSpace[Parent]
				: Local;
		}

		// 左右成对的候选，从下往上：脚最稳（站姿分得最开），手最不稳（T-pose
		// 里两只手是左右张开的，但 A-pose 里会往下垂，连线还是左右向，可用）
		struct FBonePair { const TCHAR* Left; const TCHAR* Right; };
		static const FBonePair Pairs[] = {
			{ TEXT("foot_l"), TEXT("foot_r") },
			{ TEXT("LeftFoot"), TEXT("RightFoot") },
			{ TEXT("calf_l"), TEXT("calf_r") },
			{ TEXT("LeftLeg"), TEXT("RightLeg") },
			{ TEXT("thigh_l"), TEXT("thigh_r") },
			{ TEXT("LeftUpLeg"), TEXT("RightUpLeg") },
			{ TEXT("clavicle_l"), TEXT("clavicle_r") },
			{ TEXT("LeftShoulder"), TEXT("RightShoulder") },
			{ TEXT("upperarm_l"), TEXT("upperarm_r") },
			{ TEXT("LeftArm"), TEXT("RightArm") },
			{ TEXT("hand_l"), TEXT("hand_r") },
			{ TEXT("LeftHand"), TEXT("RightHand") }
		};

		for (const FBonePair& Pair : Pairs)
		{
			const int32 LeftIndex = RefSkeleton.FindBoneIndex(FName(Pair.Left));
			const int32 RightIndex = RefSkeleton.FindBoneIndex(FName(Pair.Right));
			if (LeftIndex == INDEX_NONE || RightIndex == INDEX_NONE)
			{
				continue;
			}

			FVector Right = ComponentSpace[RightIndex].GetLocation() - ComponentSpace[LeftIndex].GetLocation();
			Right.Z = 0.0;	// 只要水平朝向；上下差是骨架的事，不是脸朝哪的事
			if (Right.IsNearlyZero())
			{
				continue;	// 这对骨骼在水平面上重合，问不出左右，换下一对
			}
			Right.Normalize();

			// UE 左手系：Right × Up = Forward
			const FVector Forward = FVector::CrossProduct(Right, FVector::UpVector).GetSafeNormal();
			if (Forward.IsNearlyZero())
			{
				continue;
			}

			const double YawOffset = FMath::RadiansToDegrees(FMath::Atan2(Forward.Y, Forward.X));

			Facing->SetBoolField(TEXT("known"), true);
			Facing->SetStringField(TEXT("from_bones"), FString::Printf(TEXT("%s / %s"), Pair.Left, Pair.Right));

			TSharedPtr<FJsonObject> ForwardJson = MakeShared<FJsonObject>();
			ForwardJson->SetNumberField(TEXT("x"), Forward.X);
			ForwardJson->SetNumberField(TEXT("y"), Forward.Y);
			ForwardJson->SetNumberField(TEXT("z"), Forward.Z);
			Facing->SetObjectField(TEXT("forward_local"), ForwardJson);

			Facing->SetNumberField(TEXT("yaw_offset"), YawOffset);
			// 这一句是给模型看的：它要的不是向量，是「Actor 的 yaw 该补多少」
			Facing->SetStringField(TEXT("hint"), FString::Printf(
				TEXT("mesh front points %.1f deg off the component +X axis; to make it face a world direction, set actor yaw = direction_yaw - (%.1f)"),
				YawOffset, YawOffset));
			return Facing;
		}

		Facing->SetBoolField(TEXT("known"), false);
		Facing->SetStringField(TEXT("reason"),
			TEXT("no left/right bone pair recognized in the reference skeleton; fall back to a front-on playtest screenshot"));
		return Facing;
	}

	void AddBounds(const TSharedPtr<FJsonObject>& Data, const FBox& Box)
	{
		// size + min/max 都要给。
		//
		// 这里一度只给 size，理由是「没有消费者会用到网格在局部空间里的角点坐标」。
		// 那条理由是错的：局部空间的 min/max **就是原点相对几何体的位置** ——
		// 居中的网格 min.z ≈ -size.z/2，坐在地面上的 min.z ≈ 0，角点原点的 min ≈ 0。
		// 拼模块化套件（地板+墙+天花板）必须知道这个约定才算得出摆放坐标，
		// 而 size 一个数字答不了。实测里因此反复绕：先按「居中」假设拼一版，
		// 截图发现天花板悬在墙外，再逐个 viewport.focus 去反推 —— 而 focus 会飞镜头。
		TSharedPtr<FJsonObject> Bounds = MakeShared<FJsonObject>();
		Bounds->SetObjectField(TEXT("size"), UAL_CommandUtils::MakeVectorJson(Box.GetSize()));
		Bounds->SetObjectField(TEXT("min"), UAL_CommandUtils::MakeVectorJson(Box.Min));
		Bounds->SetObjectField(TEXT("max"), UAL_CommandUtils::MakeVectorJson(Box.Max));
		Data->SetObjectField(TEXT("bounds"), Bounds);
	}

	const TCHAR* CollisionComplexityToString(ECollisionTraceFlag Flag)
	{
		switch (Flag)
		{
		case CTF_UseSimpleAndComplex: return TEXT("simple_and_complex");
		case CTF_UseSimpleAsComplex:  return TEXT("simple_as_complex");
		case CTF_UseComplexAsSimple:  return TEXT("complex_as_simple");
		default:                      return TEXT("default");
		}
	}

	// ------------------------------------------------------------------
	// 静态网格
	// ------------------------------------------------------------------

	void DescribeStaticMesh(UStaticMesh* Mesh, const TSharedPtr<FJsonObject>& Data)
	{
		Data->SetStringField(TEXT("type"), TEXT("static"));

		const int32 NumLODs = Mesh->GetNumLODs();
		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();

		TArray<TSharedPtr<FJsonValue>> Lods;
		for (int32 Index = 0; Index < NumLODs; ++Index)
		{
			TSharedPtr<FJsonObject> Lod = MakeShared<FJsonObject>();
			Lod->SetNumberField(TEXT("index"), Index);
			Lod->SetNumberField(TEXT("triangles"), Mesh->GetNumTriangles(Index));
			Lod->SetNumberField(TEXT("vertices"), Mesh->GetNumVertices(Index));
			Lod->SetNumberField(TEXT("sections"), Mesh->GetNumSections(Index));
			Lod->SetNumberField(TEXT("uv_channels"), Mesh->GetNumUVChannels(Index));

			// 屏占比：LOD 之间必须单调递减，不然会跳变。审计要用这个值。
			if (RenderData && RenderData->LODResources.IsValidIndex(Index))
			{
				Lod->SetNumberField(TEXT("screen_size"), RenderData->ScreenSize[Index].Default);
			}

#if WITH_EDITORONLY_DATA
			// LOD 是导入的还是引擎生成的 —— 这条决定了「能不能覆盖」。
			// 美术手工导入的 LOD 被 generate_lods 盖掉是不可逆的（见设计文档 §5.3 红线 1）。
			if (Mesh->GetNumSourceModels() > Index)
			{
				const bool bRawEmpty = Mesh->GetSourceModel(Index).IsRawMeshEmpty();
				Lod->SetStringField(TEXT("source"), bRawEmpty ? TEXT("generated") : TEXT("imported"));
			}
#endif

			Lods.Add(MakeShared<FJsonValueObject>(Lod));
		}
		Data->SetArrayField(TEXT("lods"), Lods);

		// --- 材质槽 ---
		TArray<TSharedPtr<FJsonValue>> Slots;
		const TArray<FStaticMaterial>& Materials = Mesh->GetStaticMaterials();
		for (int32 Index = 0; Index < Materials.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> Slot = MakeShared<FJsonObject>();
			Slot->SetNumberField(TEXT("index"), Index);
			Slot->SetStringField(TEXT("slot_name"), Materials[Index].MaterialSlotName.ToString());
			Slot->SetStringField(TEXT("material"), PathOf(Materials[Index].MaterialInterface));
			Slots.Add(MakeShared<FJsonValueObject>(Slot));
		}
		Data->SetArrayField(TEXT("material_slots"), Slots);

		// --- 碰撞 ---
		TSharedPtr<FJsonObject> Collision = MakeShared<FJsonObject>();
		const UBodySetup* BodySetup = Mesh->GetBodySetup();
		const int32 PrimCount = BodySetup ? BodySetup->AggGeom.GetElementCount() : 0;
		Collision->SetNumberField(TEXT("primitives"), PrimCount);
		Collision->SetNumberField(TEXT("convex_hulls"), BodySetup ? BodySetup->AggGeom.ConvexElems.Num() : 0);
		// 这里原本还报 `UStaticMesh::GetNumSectionsWithCollision()`，去掉了：
		// 那个方法在引擎头文件里**没有 ENGINE_API**，插件链接不到它
		//（UE 5.5 上是 `error LNK2019: 无法解析的外部符号`）。
		// 它只是个锦上添花的字段 —— 「到底有没有碰撞」由下面的 has_any 回答，
		// 为了它去翻编辑器专用的 SectionInfoMap 不划算。
		Collision->SetStringField(TEXT("complexity"),
			BodySetup ? CollisionComplexityToString(BodySetup->CollisionTraceFlag) : TEXT("unknown"));
		// 「有没有碰撞」是审计第一条检查项，别让调用方自己去推
		Collision->SetBoolField(TEXT("has_any"), PrimCount > 0 ||
			(BodySetup && BodySetup->CollisionTraceFlag == CTF_UseComplexAsSimple));
		Data->SetObjectField(TEXT("collision"), Collision);

		// --- Nanite ---
		// 走 UALMeshCompat：IsNaniteEnabled() 在 5.0–5.2 上还不存在
		TSharedPtr<FJsonObject> Nanite = MakeShared<FJsonObject>();
		Nanite->SetBoolField(TEXT("enabled"), UALMeshCompat::IsNaniteEnabled(Mesh));
		Data->SetObjectField(TEXT("nanite"), Nanite);

		// --- 光照贴图 UV ---
		// 通道索引指向不存在的通道是烘焙报错的常见根因，这里直接判定给出，
		// 别让模型自己拿 lightmap_coordinate_index 和 uv_channels 去比。
		TSharedPtr<FJsonObject> Lightmap = MakeShared<FJsonObject>();
		const int32 LightmapIndex = Mesh->GetLightMapCoordinateIndex();
		const int32 Lod0Channels = NumLODs > 0 ? Mesh->GetNumUVChannels(0) : 0;
		Lightmap->SetNumberField(TEXT("coordinate_index"), LightmapIndex);
		Lightmap->SetNumberField(TEXT("lod0_uv_channels"), Lod0Channels);
		Lightmap->SetBoolField(TEXT("index_valid"), LightmapIndex >= 0 && LightmapIndex < Lod0Channels);
		Data->SetObjectField(TEXT("lightmap"), Lightmap);

		AddBounds(Data, Mesh->GetBoundingBox());

		// --- Socket ---
		TArray<TSharedPtr<FJsonValue>> Sockets;
		for (const UStaticMeshSocket* Socket : Mesh->Sockets)
		{
			if (!Socket)
			{
				continue;
			}
			TSharedPtr<FJsonObject> SocketJson = MakeShared<FJsonObject>();
			SocketJson->SetStringField(TEXT("name"), Socket->SocketName.ToString());
			Sockets.Add(MakeShared<FJsonValueObject>(SocketJson));
		}
		Data->SetArrayField(TEXT("sockets"), Sockets);
	}

	// ------------------------------------------------------------------
	// 骨骼网格
	// ------------------------------------------------------------------

	void DescribeSkeletalMesh(USkeletalMesh* Mesh, const TSharedPtr<FJsonObject>& Data)
	{
		Data->SetStringField(TEXT("type"), TEXT("skeletal"));

		const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
		const int32 NumLODs = Mesh->GetLODNum();

		TArray<TSharedPtr<FJsonValue>> Lods;
		for (int32 Index = 0; Index < NumLODs; ++Index)
		{
			TSharedPtr<FJsonObject> Lod = MakeShared<FJsonObject>();
			Lod->SetNumberField(TEXT("index"), Index);

			if (RenderData && RenderData->LODRenderData.IsValidIndex(Index))
			{
				const FSkeletalMeshLODRenderData& LodData = RenderData->LODRenderData[Index];
				Lod->SetNumberField(TEXT("vertices"), LodData.GetNumVertices());
				Lod->SetNumberField(TEXT("sections"), LodData.RenderSections.Num());

				int32 Triangles = 0;
				for (const FSkelMeshRenderSection& Section : LodData.RenderSections)
				{
					Triangles += Section.NumTriangles;
				}
				Lod->SetNumberField(TEXT("triangles"), Triangles);
			}

			if (const FSkeletalMeshLODInfo* Info = Mesh->GetLODInfo(Index))
			{
				Lod->SetNumberField(TEXT("screen_size"), Info->ScreenSize.Default);
			}

			Lods.Add(MakeShared<FJsonValueObject>(Lod));
		}
		Data->SetArrayField(TEXT("lods"), Lods);

		// --- 材质槽 ---
		TArray<TSharedPtr<FJsonValue>> Slots;
		const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
		for (int32 Index = 0; Index < Materials.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> Slot = MakeShared<FJsonObject>();
			Slot->SetNumberField(TEXT("index"), Index);
			Slot->SetStringField(TEXT("slot_name"), Materials[Index].MaterialSlotName.ToString());
			Slot->SetStringField(TEXT("material"), PathOf(Materials[Index].MaterialInterface));
			Slots.Add(MakeShared<FJsonValueObject>(Slot));
		}
		Data->SetArrayField(TEXT("material_slots"), Slots);

		// --- 骨架 ---
		Data->SetStringField(TEXT("skeleton"), PathOf(Mesh->GetSkeleton()));

		// 骨骼**只给摘要，不给全表**。几百根骨骼倒进上下文没有信息量，
		// 真要找某根骨头，让模型按名字问第二次。
		const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();
		const int32 BoneCount = RefSkeleton.GetNum();
		TSharedPtr<FJsonObject> Bones = MakeShared<FJsonObject>();
		Bones->SetNumberField(TEXT("count"), BoneCount);
		if (BoneCount > 0)
		{
			Bones->SetStringField(TEXT("root"), RefSkeleton.GetBoneName(0).ToString());

			int32 MaxDepth = 0;
			for (int32 Index = 0; Index < BoneCount; ++Index)
			{
				int32 Depth = 0;
				int32 Parent = RefSkeleton.GetParentIndex(Index);
				while (Parent != INDEX_NONE)
				{
					++Depth;
					Parent = RefSkeleton.GetParentIndex(Parent);
				}
				MaxDepth = FMath::Max(MaxDepth, Depth);
			}
			Bones->SetNumberField(TEXT("max_depth"), MaxDepth);
		}
		Data->SetObjectField(TEXT("bones"), Bones);

		// --- 朝向 ---
		Data->SetObjectField(TEXT("facing"), DescribeSkeletalFacing(RefSkeleton));

		// --- 物理资产 ---
		// 没有物理资产 = 布娃娃、逐骨骼命中、准确包围盒全部不工作，而且引擎不报错。
		// 审计要用，所以这里给一个明确的布尔，不要只给路径让调用方判空。
		UPhysicsAsset* PhysicsAsset = Mesh->GetPhysicsAsset();
		Data->SetStringField(TEXT("physics_asset"), PathOf(PhysicsAsset));
		Data->SetBoolField(TEXT("has_physics_asset"), PhysicsAsset != nullptr);

		// --- Socket ---
		TArray<TSharedPtr<FJsonValue>> Sockets;
		for (const USkeletalMeshSocket* Socket : Mesh->GetMeshOnlySocketList())
		{
			if (!Socket)
			{
				continue;
			}
			TSharedPtr<FJsonObject> SocketJson = MakeShared<FJsonObject>();
			SocketJson->SetStringField(TEXT("name"), Socket->SocketName.ToString());
			SocketJson->SetStringField(TEXT("bone"), Socket->BoneName.ToString());
			Sockets.Add(MakeShared<FJsonValueObject>(SocketJson));
		}
		Data->SetArrayField(TEXT("sockets"), Sockets);

		// --- Morph Target ---
		TArray<TSharedPtr<FJsonValue>> Morphs;
		for (const UMorphTarget* Morph : Mesh->GetMorphTargets())
		{
			if (Morph)
			{
				Morphs.Add(MakeShared<FJsonValueString>(Morph->GetName()));
			}
		}
		Data->SetArrayField(TEXT("morph_targets"), Morphs);

		AddBounds(Data, Mesh->GetImportedBounds().GetBox());
	}
}

// ======================================================================
// 注册
// ======================================================================

void FUAL_MeshCommands::RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("mesh.describe"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_Describe(Payload, RequestId);
	});
}

// ======================================================================
// mesh.describe
// ======================================================================

void FUAL_MeshCommands::Handle_Describe(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	Path = UAL_CommandUtils::NormalizeAssetPath(Path);

	UObject* Asset = LoadObject<UObject>(nullptr, *Path);
	if (!Asset)
	{
		UAL_CommandUtils::SendError(RequestId, 404,
			FString::Printf(TEXT("Mesh asset not found: %s"), *Path));
		return;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("path"), Asset->GetPathName());
	Data->SetStringField(TEXT("name"), Asset->GetName());

	if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
	{
		DescribeStaticMesh(StaticMesh, Data);
	}
	else if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset))
	{
		DescribeSkeletalMesh(SkeletalMesh, Data);
	}
	else
	{
		// 说清楚它实际是什么 —— 「不是网格」这句话没法让调用方决定下一步
		UAL_CommandUtils::SendError(RequestId, 400,
			FString::Printf(TEXT("Asset is not a static or skeletal mesh: %s (class %s)"),
				*Path, *Asset->GetClass()->GetName()));
		return;
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}
