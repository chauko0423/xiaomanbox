#include "UAL_LevelCommands.h"
#include "UAL_AgentUndo.h"
#include "UAL_CommandUtils.h"
#include "UAL_TouchedPackages.h"
#include "UAL_SavablePackage.h"
#include "UAL_VersionCompat.h"

#include "FileHelpers.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "Misc/PackageName.h"

#include "Editor.h"
#include "EditorLevelUtils.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingAlwaysLoaded.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
// GPixelFormats：5.0 在 RHI.h 里，5.5 起搬到了 Core 的 PixelFormat.h。
// RHI.h 两边都能拿到（新版本会把 PixelFormat.h 带进来）。
#include "RHI.h"
#include "EngineUtils.h"
#include "PhysicsEngine/BodySetup.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UAL_ScopedTransaction.h"

#if WITH_EDITOR
#include "Selection.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUALLevel, Log, All);

void FUAL_LevelCommands::RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("level.query_assets"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_QueryAssets(Payload, RequestId);
	});

	CommandMap.Add(TEXT("level.organize_actors"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_OrganizeActors(Payload, RequestId);
	});

	// level.get_current - 当前开着哪张关卡、脏没脏
	CommandMap.Add(TEXT("level.get_current"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetCurrentLevel(Payload, RequestId);
	});

	// level.save - 保存当前关卡（新关卡需要 path）
	CommandMap.Add(TEXT("level.save"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SaveLevel(Payload, RequestId);
	});

	// level.open - 打开关卡（有未保存改动时默认拒绝）
	CommandMap.Add(TEXT("level.open"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_OpenLevel(Payload, RequestId);
	});

	// level.new - 新建空关卡（有未保存改动时默认拒绝）
	CommandMap.Add(TEXT("level.new"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_NewLevel(Payload, RequestId);
	});

	// level.list - 世界由哪些关卡组成，各自的加载/可见标志
	CommandMap.Add(TEXT("level.list"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ListLevels(Payload, RequestId);
	});

	// level.set_streaming - 改子关卡的加载方式和可见标志
	CommandMap.Add(TEXT("level.set_streaming"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetLevelStreaming(Payload, RequestId);
	});
}

// ========== 从 UAL_CommandHandler.cpp 迁移以下函数 ==========

// ============================================================================
// level.query_assets —— 关卡里摆了些什么，以及谁最贵
// ============================================================================
//
// ## 为什么按资产聚合，而不是一个 Actor 一行
//
// 旧实现是一个 Actor 一行。同一个 SM_Rock 摆五百份，就是五百行一模一样的
// 「3968 三角形」—— 列表被刷屏，而用户真正要的那个数（**这个网格在这张关卡里
// 一共让引擎画了多少三角形**）一次都没出现过。排行榜按 Actor 排是错的口径：
// 要优化的对象是资产，不是摆放。
//
// 所以这里按资产路径聚合，出现次数、实例数、总面数各算一份，
// 并留最多三个 Actor 名字，方便回场景里定位。
//
// ## 为什么不再只看第一个静态网格组件
//
// 旧实现 `FindComponentByClass<UStaticMeshComponent>()` 只拿**第一个**组件，
// 多网格 Actor 的其余部分直接消失；实例化组件（植被、草）也只算一份，
// 而它一个组件就可能画五百株草 —— 「谁最贵」的答案会差两个数量级。
//
// ## 没进榜的东西要数出来
//
// 灯光、贴花、骨骼网格、Niagara 都有开销，但**没有和三角形可比的开销指标**，
// 硬塞进同一个排行只会得出错误结论。它们按类名计数放进 `not_ranked_by_class`：
// 告诉调用方「这里还有 240 个灯光没进榜」，比假装它们不存在诚实。
// 骨骼网格要逐个看开销，走 `content.asset_ranking`（那边按资产注册表标签排，
// 覆盖所有资产类型）。
//
// ## 贴图为什么只给要返回的那几个算
//
// 算贴图要走材质、把贴图对象取出来问尺寸和格式，是整个流程里唯一贵的一步。
// 排序截断之后只剩 limit 个资产，只给它们算 —— 用户看不到的那几千个算了也白算。
// 代价是**不能按贴图内存排序**（还没算就要排），所以这里没有 TextureMemory
// 这个排序键。全工程按贴图内存排行走 `content.asset_ranking`。

namespace
{
	/** 一个资产在本次查询范围内的汇总 */
	struct FUAL_LevelAssetUsage
	{
		FString Name;
		FString Path;
		FString Type;
		/** LOD0 面数，**单份**。总量看 TotalTriangles() */
		int32 Triangles = -1;
		/** 引擎估算的常驻资源大小。不是磁盘文件大小 —— 字段名别再叫 disk_size 了 */
		int64 ResourceBytes = 0;
		int32 LodCount = 0;
		int32 MaterialSlots = 0;
		bool bNanite = false;
		bool bMissingCollision = false;
		bool bCastsShadow = false;
		/** 多少个 Actor 用到它 */
		int32 ActorCount = 0;
		/** 实际画出来多少份。实例化组件按实例数算 */
		int64 InstanceCount = 0;
		TArray<FString> SampleActors;
		/** 第二阶段算贴图时还要用。同一次命令执行内不会被 GC */
		const UStaticMesh* Mesh = nullptr;
		int32 MaxTextureEdge = 0;
		int64 MaxTexturePixels = 0;
		int64 TextureBytes = 0;
		bool bTexturesAnalyzed = false;

		/** 这个资产在本关卡里一共贡献多少三角形 */
		int64 TotalTriangles() const
		{
			return Triangles > 0 ? (int64)Triangles * FMath::Max<int64>(InstanceCount, 1) : 0;
		}
	};

	/** 没有简单碰撞、也没开 complex-as-simple —— 玩家会直接穿过去 */
	bool UAL_MeshMissesCollision(const UStaticMesh* Mesh)
	{
		const UBodySetup* BodySetup = Mesh ? Mesh->GetBodySetup() : nullptr;
		if (!BodySetup)
		{
			return true;
		}
		const bool bHasSimple = BodySetup->AggGeom.GetElementCount() > 0;
		const bool bUsesComplex = BodySetup->GetCollisionTraceFlag() == CTF_UseComplexAsSimple;
		return !bHasSimple && !bUsesComplex;
	}

	/**
	 * 贴图字节数估算。
	 *
	 * 按像素格式真实的 block 尺寸算，不是「4 字节/像素」拍脑袋 ——
	 * 一张 4K 的 DXT1 实际是 8MB，按 RGBA8 算会报成 64MB，八倍。
	 * 用户照着这个数去砍贴图，砍的是没问题的东西。
	 *
	 * 格式表直接问引擎（GPixelFormats），不自己维护一份对照表：
	 * 新格式是引擎加的，我们的表只会越来越旧。
	 */
	int64 UAL_EstimateTextureBytes(const UTexture2D* Texture)
	{
		if (!Texture)
		{
			return 0;
		}
		const int64 Width = Texture->GetSizeX();
		const int64 Height = Texture->GetSizeY();
		if (Width <= 0 || Height <= 0)
		{
			return 0;
		}

		const FPixelFormatInfo& Info = GPixelFormats[Texture->GetPixelFormat()];
		const int64 BlockX = FMath::Max<int64>(Info.BlockSizeX, 1);
		const int64 BlockY = FMath::Max<int64>(Info.BlockSizeY, 1);
		const int64 Blocks = FMath::DivideAndRoundUp(Width, BlockX) * FMath::DivideAndRoundUp(Height, BlockY);
		const int64 BaseBytes = Blocks * FMath::Max<int64>(Info.BlockBytes, 1);

		// mip 链把总量抬到约 4/3。没开 mip 的会被高估三分之一，
		// 但漏算 mip 是系统性低估，两者里前者无害得多。
		return Texture->GetNumMips() > 1 ? BaseBytes * 4 / 3 : BaseBytes;
	}

	/**
	 * 这个网格的材质一共拖了多大的贴图进来（同一张贴图只算一次）。
	 *
	 * 除了最大长边，还要回**最大那张贴图的像素总数** —— 判「超大贴图」时
	 * `content.audit_optimization` 用的是面积（4096×2048）而不是单边：
	 * 4096×1 的分隔条按单边算是「超大贴图」，其实小得可怜。
	 * 两个命令对同一件事必须给同一个答案。
	 */
	void UAL_AnalyzeMeshTextures(const UStaticMesh* Mesh, int32& OutMaxEdge, int64& OutMaxPixels, int64& OutBytes)
	{
		OutMaxEdge = 0;
		OutMaxPixels = 0;
		OutBytes = 0;
		if (!Mesh)
		{
			return;
		}

		TSet<const UTexture2D*> Seen;
		for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
		{
			UMaterialInterface* Material = Slot.MaterialInterface;
			if (!Material)
			{
				continue;
			}

			TArray<UTexture*> Textures;
			UALCompat::GetUsedTextures(Material, Textures);
			for (UTexture* Texture : Textures)
			{
				const UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
				if (!Texture2D || Seen.Contains(Texture2D))
				{
					continue;
				}
				Seen.Add(Texture2D);
				const int64 Width = Texture2D->GetSizeX();
				const int64 Height = Texture2D->GetSizeY();
				OutMaxEdge = FMath::Max3(OutMaxEdge, (int32)Width, (int32)Height);
				OutMaxPixels = FMath::Max(OutMaxPixels, Width * Height);
				OutBytes += UAL_EstimateTextureBytes(Texture2D);
			}
		}
	}
}

void FUAL_LevelCommands::Handle_QueryAssets(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1) 解析参数
	FString ScopeType = TEXT("Level");
	FString ScopePath;
	if (const TSharedPtr<FJsonObject>* ScopeObj = nullptr; Payload->TryGetObjectField(TEXT("scope"), ScopeObj) && ScopeObj && ScopeObj->IsValid())
	{
		(*ScopeObj)->TryGetStringField(TEXT("type"), ScopeType);
		(*ScopeObj)->TryGetStringField(TEXT("path"), ScopePath);
	}

	const TSharedPtr<FJsonObject>* Conditions = nullptr;
	Payload->TryGetObjectField(TEXT("conditions"), Conditions);

	auto ReadNumber = [](const TSharedPtr<FJsonObject>* Obj, const TCHAR* Key, double DefaultValue) -> double
	{
		if (!Obj || !Obj->IsValid())
		{
			return DefaultValue;
		}
		double Val = DefaultValue;
		(*Obj)->TryGetNumberField(Key, Val);
		return Val;
	};

	auto ReadBool = [](const TSharedPtr<FJsonObject>* Obj, const TCHAR* Key, bool DefaultValue) -> bool
	{
		if (!Obj || !Obj->IsValid())
		{
			return DefaultValue;
		}
		bool Val = DefaultValue;
		(*Obj)->TryGetBoolField(Key, Val);
		return Val;
	};

	// 这里只留插件真的会用的条件。
	// 曾经还解析过 min_texture_size / max_texture_size / shader_complexity_index /
	// min_radius —— 四个都只是被读进变量，没有一条参与过筛选。宣传一个不生效的
	// 筛选条件，用户会以为「筛过了、没有」，而实际上一次都没筛。
	const double MinTriangles = ReadNumber(Conditions, TEXT("min_triangles"), -1.0);
	const double MinInstances = ReadNumber(Conditions, TEXT("min_instances"), -1.0);
	const bool bMissingCollisionOnly = ReadBool(Conditions, TEXT("missing_collision"), false);
	const bool bNaniteEnabled = ReadBool(Conditions, TEXT("nanite_enabled"), true); // 默认允许 Nanite，false 表示筛未开启
	const bool bShadowCasting = ReadBool(Conditions, TEXT("shadow_casting"), false);
	FString ClassFilter;
	if (Conditions && Conditions->IsValid())
	{
		(*Conditions)->TryGetStringField(TEXT("class_filter"), ClassFilter);
	}

	FString SortBy;
	Payload->TryGetStringField(TEXT("sort_by"), SortBy);
	int32 Limit = 20;
	Payload->TryGetNumberField(TEXT("limit"), Limit);
	if (Limit <= 0)
	{
		Limit = 20;
	}

	// 2) 收集目标 Actor
	TArray<AActor*> Candidates;
	UWorld* World = UAL_CommandUtils::GetLiveWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("No world available"));
		return;
	}

	if (ScopeType.Equals(TEXT("Level"), ESearchCase::IgnoreCase))
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			Candidates.Add(*It);
		}
	}
#if WITH_EDITOR
	else if (ScopeType.Equals(TEXT("Selection"), ESearchCase::IgnoreCase))
	{
		if (GEditor)
		{
			for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
			{
				if (AActor* Selected = Cast<AActor>(*It))
				{
					Candidates.Add(Selected);
				}
			}
		}
	}
#endif
	else
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Scope type not supported yet"));
		return;
	}

	// 3) 逐 Actor 走遍所有静态网格组件，按资产聚合
	TMap<FString, FUAL_LevelAssetUsage> ByAsset;
	TMap<FString, int32> NotRanked;
	int32 ActorsVisited = 0;

	for (AActor* Actor : Candidates)
	{
		if (!Actor)
		{
			continue;
		}
		if (!ClassFilter.IsEmpty() && !Actor->GetClass()->GetName().Contains(ClassFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		ActorsVisited++;

		TArray<UStaticMeshComponent*> MeshComponents;
		Actor->GetComponents<UStaticMeshComponent>(MeshComponents);

		// 「有网格但被 scope.path 排除掉了」和「压根没有网格」要分开：
		// 前者是调用方自己划的范围，报进 not_ranked_by_class 会让人以为
		// 那些 Actor 是没法排名的类型。
		bool bHasMesh = false;
		bool bRanked = false;
		for (UStaticMeshComponent* MeshComponent : MeshComponents)
		{
			UStaticMesh* Mesh = MeshComponent ? MeshComponent->GetStaticMesh() : nullptr;
			if (!Mesh)
			{
				continue;
			}
			bHasMesh = true;

			const FString Path = Mesh->GetPathName();

			// scope.path：只看这个目录下的资产。
			// 它曾经被解析进变量然后**从未使用** —— 和那四个假条件是同一个毛病，
			// 调用方传了目录、拿回整张关卡，然后以为「这个目录下就是这些」。
			if (!ScopePath.IsEmpty() && !Path.StartsWith(ScopePath))
			{
				continue;
			}
			FUAL_LevelAssetUsage& Usage = ByAsset.FindOrAdd(Path);
			if (Usage.Path.IsEmpty())
			{
				Usage.Name = Mesh->GetName();
				Usage.Path = Path;
				Usage.Type = TEXT("StaticMesh");
				Usage.Mesh = Mesh;
				Usage.ResourceBytes = Mesh->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal);
				Usage.MaterialSlots = Mesh->GetStaticMaterials().Num();
				if (const FStaticMeshRenderData* RenderData = Mesh->GetRenderData())
				{
					Usage.LodCount = RenderData->LODResources.Num();
					if (RenderData->LODResources.Num() > 0)
					{
						Usage.Triangles = RenderData->LODResources[0].GetNumTriangles();
					}
				}
				Usage.bNanite = UALCompat::IsNaniteEnabled(Mesh);
				Usage.bMissingCollision = UAL_MeshMissesCollision(Mesh);
			}

			// 实例化组件（植被、草、批量摆件）一个组件画很多份。
			// 记成一份，「谁最贵」的答案会差两个数量级。
			int64 Instances = 1;
			if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(MeshComponent))
			{
				Instances = Instanced->GetInstanceCount();
			}

			Usage.ActorCount++;
			Usage.InstanceCount += Instances;
			Usage.bCastsShadow = Usage.bCastsShadow || MeshComponent->CastShadow;
			if (Usage.SampleActors.Num() < 3)
			{
				Usage.SampleActors.AddUnique(Actor->GetActorLabel());
			}
			bRanked = true;
		}

		if (!bRanked && !bHasMesh)
		{
			NotRanked.FindOrAdd(Actor->GetClass()->GetName())++;
		}
	}

	// 4) 条件过滤
	TArray<FUAL_LevelAssetUsage> Results;
	Results.Reserve(ByAsset.Num());
	for (TPair<FString, FUAL_LevelAssetUsage>& Pair : ByAsset)
	{
		const FUAL_LevelAssetUsage& Item = Pair.Value;
		if (MinTriangles >= 0 && Item.Triangles >= 0 && Item.Triangles < MinTriangles)
		{
			continue;
		}
		if (MinInstances >= 0 && Item.InstanceCount < MinInstances)
		{
			continue;
		}
		if (bMissingCollisionOnly && !Item.bMissingCollision)
		{
			continue;
		}
		if (!bNaniteEnabled && Item.bNanite)
		{
			continue;
		}
		if (bShadowCasting && !Item.bCastsShadow)
		{
			continue;
		}
		Results.Add(Pair.Value);
	}

	const int32 MatchedCount = Results.Num();

	// 5) 排序。默认按**总面数**——单份面数再高，只摆了一个也不是瓶颈
	Results.StableSort([&SortBy](const FUAL_LevelAssetUsage& A, const FUAL_LevelAssetUsage& B)
	{
		auto GetKey = [&SortBy](const FUAL_LevelAssetUsage& X) -> int64
		{
			if (SortBy.Equals(TEXT("TriangleCount"), ESearchCase::IgnoreCase))
			{
				return X.Triangles;
			}
			if (SortBy.Equals(TEXT("ResourceSize"), ESearchCase::IgnoreCase))
			{
				return X.ResourceBytes;
			}
			if (SortBy.Equals(TEXT("InstanceCount"), ESearchCase::IgnoreCase))
			{
				return X.InstanceCount;
			}
			return X.TotalTriangles();
		};
		return GetKey(A) > GetKey(B);
	});

	const bool bTruncated = Results.Num() > Limit;
	if (bTruncated)
	{
		Results.SetNum(Limit);
	}

	// 6) 只给活下来的这几个算贴图（见文件头「贴图为什么只给要返回的那几个算」）
	for (FUAL_LevelAssetUsage& Item : Results)
	{
		UAL_AnalyzeMeshTextures(Item.Mesh, Item.MaxTextureEdge, Item.MaxTexturePixels, Item.TextureBytes);
		Item.bTexturesAnalyzed = true;
	}

	// 7) 构建响应
	TArray<TSharedPtr<FJsonValue>> AssetsJson;
	for (const FUAL_LevelAssetUsage& Item : Results)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Item.Name);
		Obj->SetStringField(TEXT("path"), Item.Path);
		Obj->SetStringField(TEXT("type"), Item.Type);

		TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
		if (Item.Triangles >= 0)
		{
			Stats->SetNumberField(TEXT("triangles"), Item.Triangles);
			Stats->SetNumberField(TEXT("total_triangles"), Item.TotalTriangles());
		}
		Stats->SetNumberField(TEXT("instance_count"), Item.InstanceCount);
		Stats->SetNumberField(TEXT("actor_count"), Item.ActorCount);
		Stats->SetNumberField(TEXT("resource_bytes"), Item.ResourceBytes);
		Stats->SetNumberField(TEXT("lod_count"), Item.LodCount);
		Stats->SetNumberField(TEXT("material_slots"), Item.MaterialSlots);
		Stats->SetBoolField(TEXT("nanite"), Item.bNanite);
		Stats->SetBoolField(TEXT("missing_collision"), Item.bMissingCollision);
		Stats->SetBoolField(TEXT("shadow_casting"), Item.bCastsShadow);
		if (Item.bTexturesAnalyzed)
		{
			Stats->SetNumberField(TEXT("max_texture_edge"), Item.MaxTextureEdge);
			Stats->SetNumberField(TEXT("max_texture_pixels"), Item.MaxTexturePixels);
			Stats->SetNumberField(TEXT("texture_bytes"), Item.TextureBytes);
		}
		Obj->SetObjectField(TEXT("stats"), Stats);

		TArray<TSharedPtr<FJsonValue>> SampleJson;
		for (const FString& Label : Item.SampleActors)
		{
			SampleJson.Add(MakeShared<FJsonValueString>(Label));
		}
		Obj->SetArrayField(TEXT("sample_actors"), SampleJson);

		AssetsJson.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> NotRankedJson = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Pair : NotRanked)
	{
		NotRankedJson->SetNumberField(Pair.Key, Pair.Value);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), AssetsJson.Num());
	Data->SetNumberField(TEXT("matched_assets"), MatchedCount);
	Data->SetNumberField(TEXT("actors_visited"), ActorsVisited);
	Data->SetBoolField(TEXT("truncated"), bTruncated);
	Data->SetArrayField(TEXT("assets"), AssetsJson);
	Data->SetObjectField(TEXT("not_ranked_by_class"), NotRankedJson);

	UAL_CommandUtils::AddWorldInfo(Data);
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_LevelCommands::Handle_OrganizeActors(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	// 大纲文件夹是编辑器世界的东西，PIE 世界里根本没有
	if (UAL_CommandUtils::RefuseDuringPlay(RequestId, TEXT("整理大纲"), TEXT("organizing the outliner")))
	{
		return;
	}
	// 创建撤销事务，使文件夹归类操作可通过 Ctrl+Z 撤销
	FUAL_ScopedTransaction Transaction(UAL_CommandUtils::LText(TEXT("组织Actor到文件夹"), TEXT("Organize Actors to Folder")));
#endif

	// 1) 解析参数
	FString FolderPath;
	if (!Payload->TryGetStringField(TEXT("folder_path"), FolderPath))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: folder_path"));
		return;
	}

	// 2) 构建targets对象（支持filter）
	TSharedPtr<FJsonObject> Targets = MakeShared<FJsonObject>();
	
	// 支持filter参数（兼容actor.get的filter格式）
	const TSharedPtr<FJsonObject>* FilterObj = nullptr;
	if (Payload->TryGetObjectField(TEXT("filter"), FilterObj) && FilterObj && FilterObj->IsValid())
	{
		Targets->SetObjectField(TEXT("filter"), *FilterObj);
	}
	// 也支持直接传class参数（简化用法）
	else
	{
		FString ClassFilter;
		if (Payload->TryGetStringField(TEXT("class"), ClassFilter) && !ClassFilter.IsEmpty())
		{
			TSharedPtr<FJsonObject> Filter = MakeShared<FJsonObject>();
			Filter->SetStringField(TEXT("class_contains"), ClassFilter);
			Targets->SetObjectField(TEXT("filter"), Filter);
		}
	}

	// 如果没有提供filter，默认匹配所有Actor
	if (!Targets->HasField(TEXT("filter")))
	{
		TSharedPtr<FJsonObject> Filter = MakeShared<FJsonObject>();
		Targets->SetObjectField(TEXT("filter"), Filter);
	}

	// 3) 获取目标World
	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("No world available"));
		return;
	}

	// 4) 解析targets获取匹配的Actor列表
	TSet<AActor*> TargetSet;
	FString TargetError;
	TArray<FString> Unmatched;
	if (!UAL_CommandUtils::ResolveTargetsToActors(Targets, World, TargetSet, TargetError, &Unmatched))
	{
		UAL_CommandUtils::SendError(RequestId, 404, TargetError);
		return;
	}

	if (TargetSet.Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 404, TEXT("No actors found matching the filter"));
		return;
	}

	// 5) 批量设置FolderPath
	TArray<AActor*> TargetArray = TargetSet.Array();
	Algo::Sort(TargetArray, [](AActor* A, AActor* B)
	{
		const FString NameA = UAL_CommandUtils::GetActorFriendlyName(A);
		const FString NameB = UAL_CommandUtils::GetActorFriendlyName(B);
		return NameA < NameB;
	});

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 SuccessCount = 0;

	for (AActor* Actor : TargetArray)
	{
		if (!Actor)
		{
			continue;
		}

		TSharedPtr<FJsonObject> ActorObj = UAL_CommandUtils::BuildActorInfo(Actor);
		if (!ActorObj.IsValid())
		{
			continue;
		}

#if WITH_EDITOR
		// 设置文件夹路径
		Actor->SetFolderPath(FName(*FolderPath));
		const FString ActualPath = Actor->GetFolderPath().ToString();
		
		SuccessCount++;
		ActorObj->SetStringField(TEXT("folder_path"), ActualPath);
#else
		// 非编辑器模式下不支持文件夹路径
		ActorObj->SetStringField(TEXT("error"), TEXT("Folder path is only available in editor mode"));
#endif

		Results.Add(MakeShared<FJsonValueObject>(ActorObj));
	}

	// 6) 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), SuccessCount);
	Data->SetNumberField(TEXT("total_found"), TargetSet.Num());
	Data->SetArrayField(TEXT("actors"), Results);
	UAL_CommandUtils::AddUnmatchedTargets(Data, Unmatched);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// 关卡的打开 / 新建 / 保存
//
// 在这之前 level 命名空间只有 query_assets 和 organize_actors —— 能查、能
// 整理，但换不了关卡也存不了关卡。Actor 全部在"当前关卡"上操作，而当前
// 关卡是什么、改了之后怎么留下来，这套工具一句都答不上来。
// ============================================================================

namespace
{
	/** 关卡包还没落过盘（新建的空关卡在 /Temp/ 下） */
	bool UAL_IsTemporaryLevel(const UWorld* World)
	{
		return World && World->GetOutermost()->GetName().StartsWith(TEXT("/Temp/"));
	}

	/**
	 * 有脏东西就回 409 并列出来，返回 true 表示已经回过响应、调用方该直接 return。
	 *
	 * 判据和响应体都统一在 `UAL_SavablePackage.h`（`UAL_DirtyPackagesForDestructiveOp`
	 * / `UAL_UnsavedRefusalDetails`）。原来这些逻辑就写在这里，结果 `editor.restart`
	 * 各写了一份、漏掉了 `/Script/` 那道修正，同一个「永远被拦、只能 force」的坑
	 * 又踩了一遍。挪到头文件共用。
	 */
	bool UAL_RefuseIfDirty(const TSharedPtr<FJsonObject>& Payload, const FString& RequestId, const TCHAR* Action)
	{
		bool bForce = false;
		Payload->TryGetBoolField(TEXT("force"), bForce);
		if (bForce)
		{
			return false;
		}

		const FUAL_DestructiveOpDirty Dirty = UAL_DirtyPackagesForDestructiveOp();
		if (Dirty.Num() == 0)
		{
			return false;
		}

		UAL_CommandUtils::SendError(
			RequestId,
			409,
			FString::Printf(
				TEXT("Refusing to %s: %d package(s) have unsaved changes that would be lost."),
				Action, Dirty.Num()),
			UAL_UnsavedRefusalDetails(Dirty));
		return true;
	}

	TSharedPtr<FJsonObject> UAL_CurrentLevelJson()
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		UWorld* World = UAL_CommandUtils::GetTargetWorld();
		if (!World)
		{
			return Result;
		}

		UPackage* Package = World->GetOutermost();
		Result->SetStringField(TEXT("package"), Package->GetName());
		Result->SetStringField(TEXT("name"), World->GetName());
		Result->SetBoolField(TEXT("is_dirty"), Package->IsDirty());
		// 临时关卡存盘必须给路径，调用方要能提前知道
		Result->SetBoolField(TEXT("is_temporary"), UAL_IsTemporaryLevel(World));

		int32 ActorCount = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			++ActorCount;
		}
		Result->SetNumberField(TEXT("actor_count"), ActorCount);
		return Result;
	}
}

void FUAL_LevelCommands::Handle_GetCurrentLevel(const TSharedPtr<FJsonObject>& /*Payload*/, const FString RequestId)
{
	UWorld* World = UAL_CommandUtils::GetLiveWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 404, TEXT("No editor world is available"));
		return;
	}

	TSharedPtr<FJsonObject> Result = UAL_CurrentLevelJson();
	Result->SetBoolField(TEXT("ok"), true);
	UAL_CommandUtils::AddWorldInfo(Result);
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

void FUAL_LevelCommands::Handle_SaveLevel(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// PIE 期间存盘存的是编辑器世界，而用户以为存的是他刚在游戏里看到的东西
	if (UAL_CommandUtils::RefuseDuringPlay(RequestId, TEXT("保存关卡"), TEXT("saving the level")))
	{
		return;
	}

	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 404, TEXT("No editor world is available"));
		return;
	}

	FString TargetPath;
	Payload->TryGetStringField(TEXT("path"), TargetPath);

	// 从没存过的关卡不给路径就没法存 —— 替用户挑一个存放位置是越界的
	if (TargetPath.IsEmpty() && UAL_IsTemporaryLevel(World))
	{
		UAL_CommandUtils::SendError(
			RequestId,
			400,
			TEXT("This level has never been saved, so it has no file yet. Pass \"path\", for example \"/Game/Maps/MyLevel\"."));
		return;
	}

	bool bSaved = false;
	FString SavedAs;
	FString OutSavedFilename;

	// SaveLevel 存的是「当前关卡」—— 用户在 Levels 窗格里把某个子关卡设成当前时，
	// 落盘的是那个子关卡，不是持久关卡。回执得说实际存的是谁，不能拿持久关卡的包名顶上
	ULevel* LevelToSave = World->GetCurrentLevel();
	const bool bSavingPersistent = LevelToSave && LevelToSave->IsPersistentLevel();

	if (!TargetPath.IsEmpty())
	{
		// 另存为：路径可能带资产后缀，包名要的是前半段
		FString PackageName = TargetPath;
		int32 DotIndex = INDEX_NONE;
		if (PackageName.FindChar(TEXT('.'), DotIndex))
		{
			PackageName = PackageName.Left(DotIndex);
		}

		FString Filename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
				PackageName, Filename, FPackageName::GetMapPackageExtension()))
		{
			UAL_CommandUtils::SendError(
				RequestId, 400,
				FString::Printf(TEXT("Not a valid package path: %s"), *TargetPath));
			return;
		}

		bSaved = FEditorFileUtils::SaveLevel(LevelToSave, Filename, &OutSavedFilename);
	}
	else
	{
		bSaved = FEditorFileUtils::SaveLevel(LevelToSave, FString(), &OutSavedFilename);
	}

	if (!bSaved)
	{
		UAL_CommandUtils::SendError(
			RequestId, 500,
			TEXT("Saving the level failed - the file may be read-only or checked out by source control"));
		return;
	}

	// 包名以引擎实际写出的文件为准；拿不到文件名（老版本不回填）才退回到那张关卡当前所在的包
	if (OutSavedFilename.IsEmpty()
		|| !FPackageName::TryConvertFilenameToLongPackageName(OutSavedFilename, SavedAs))
	{
		SavedAs = LevelToSave ? LevelToSave->GetOutermost()->GetName() : World->GetOutermost()->GetName();
	}

	TSharedPtr<FJsonObject> Result = UAL_CurrentLevelJson();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("saved_as"), SavedAs);
	if (!OutSavedFilename.IsEmpty())
	{
		Result->SetStringField(TEXT("saved_file"), FPaths::ConvertRelativePathToFull(OutSavedFilename));
	}
	Result->SetBoolField(TEXT("saved_level_is_persistent"), bSavingPersistent);
	if (!bSavingPersistent)
	{
		// 存的是子关卡：持久关卡和其他子关卡的改动都还没落盘，上面的 is_dirty 说的是持久关卡
		Result->SetStringField(
			TEXT("note"),
			TEXT("The current level is a sublevel, so only that sublevel was saved. The persistent level and other sublevels were not saved; use editor.save to save them."));
	}
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

void FUAL_LevelCommands::Handle_OpenLevel(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 跑着 PIE 换关卡会把用户的会话直接搅烂
	if (UAL_CommandUtils::RefuseDuringPlay(RequestId, TEXT("打开关卡"), TEXT("opening a level")))
	{
		return;
	}

	FString TargetPath;
	if (!Payload->TryGetStringField(TEXT("path"), TargetPath) || TargetPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}

	FString PackageName = TargetPath;
	int32 DotIndex = INDEX_NONE;
	if (PackageName.FindChar(TEXT('.'), DotIndex))
	{
		PackageName = PackageName.Left(DotIndex);
	}

	// 先确认关卡真的存在，再谈丢不丢改动 —— 为一个打不开的关卡去问
	// "要不要丢弃未保存的东西"是白问一轮
	FString Filename;
	if (!FPackageName::DoesPackageExist(PackageName, &Filename))
	{
		UAL_CommandUtils::SendError(
			RequestId, 404,
			FString::Printf(TEXT("Level not found: %s"), *TargetPath));
		return;
	}

	if (UAL_RefuseIfDirty(Payload, RequestId, TEXT("open a different level")))
	{
		return;
	}

	// 换图前必须把撤销缓冲清干净 —— 里面存着引用旧关卡 actor 的事务，
	// 留着旧 World 就回收不掉，引擎的泄漏检查会在 LoadMap 里直接 fatal。
	// PreLoadMap 委托里也挂了同一个函数，这里再显式调一次是为了不依赖委托时序：
	// 它是这条崩溃路径的正面入口，值得多一道保险
	FUAL_AgentUndo::PrepareForMapChange();

	UEditorLoadingAndSavingUtils::LoadMap(Filename);

	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World || !World->GetOutermost()->GetName().Equals(PackageName))
	{
		UAL_CommandUtils::SendError(
			RequestId, 500,
			FString::Printf(TEXT("LoadMap did not end up on %s"), *PackageName));
		return;
	}

	// 换了关卡，之前记的那些"我们改脏的包"多半已经被卸载，留着没有意义
	FUAL_TouchedPackages::Clear();

	TSharedPtr<FJsonObject> Result = UAL_CurrentLevelJson();
	Result->SetBoolField(TEXT("ok"), true);
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

void FUAL_LevelCommands::Handle_NewLevel(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (UAL_CommandUtils::RefuseDuringPlay(RequestId, TEXT("新建关卡"), TEXT("creating a level")))
	{
		return;
	}

	if (UAL_RefuseIfDirty(Payload, RequestId, TEXT("create a new level")))
	{
		return;
	}

	// save_as 在建图之前就校验：路径不合法时先报错，别等新图建好了才静默变成 saved:false，
	// 那时旧关卡已经换掉了，调用方还以为照他给的路径存好了
	FString SaveAs;
	FString SaveAsPackage;
	FString SaveAsFilename;
	if (Payload->TryGetStringField(TEXT("save_as"), SaveAs) && !SaveAs.IsEmpty())
	{
		SaveAsPackage = SaveAs;
		int32 DotIndex = INDEX_NONE;
		if (SaveAsPackage.FindChar(TEXT('.'), DotIndex))
		{
			SaveAsPackage = SaveAsPackage.Left(DotIndex);
		}

		if (!FPackageName::TryConvertLongPackageNameToFilename(
				SaveAsPackage, SaveAsFilename, FPackageName::GetMapPackageExtension()))
		{
			UAL_CommandUtils::SendError(
				RequestId, 400,
				FString::Printf(TEXT("Not a valid package path for save_as: %s (expected something like \"/Game/Maps/MyLevel\"). No new level was created."), *SaveAs));
			return;
		}
	}

	// 传 false：要不要保存当前关卡由上面那道脏检查和调用方决定，
	// 不在这里偷偷替他存。
	// 必须看返回值：建图失败时 GetTargetWorld() 拿到的还是旧世界，非空检查拦不住
	UWorld* NewWorld = UEditorLoadingAndSavingUtils::NewBlankMap(/*bSaveExistingMap=*/false);
	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!NewWorld || !World || World != NewWorld)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Creating a new level failed - the editor is still on the previous level"));
		return;
	}

	FUAL_TouchedPackages::Clear();

	// 新关卡默认在 /Temp/ 下、没有文件。给了 save_as 就顺手落盘，
	// 否则调用方得记着"这张关卡还不存在"，很容易忘
	bool bSaved = false;
	FString SavedFilename;
	if (!SaveAsFilename.IsEmpty())
	{
		bSaved = FEditorFileUtils::SaveLevel(World->GetCurrentLevel(), SaveAsFilename, &SavedFilename);
	}

	TSharedPtr<FJsonObject> Result = UAL_CurrentLevelJson();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetBoolField(TEXT("saved"), bSaved);
	if (bSaved)
	{
		// 回执里的包名来自存完之后的世界，不回显 save_as
		Result->SetStringField(TEXT("saved_as"), World->GetOutermost()->GetName());
		if (!SavedFilename.IsEmpty())
		{
			Result->SetStringField(TEXT("saved_file"), FPaths::ConvertRelativePathToFull(SavedFilename));
		}
	}
	else if (!SaveAsFilename.IsEmpty())
	{
		// 给了 save_as 却没存上：新关卡已经建好（没法撤回），但存盘这一步失败了，要说出来
		Result->SetStringField(TEXT("save_error"),
			FString::Printf(TEXT("The new level was created but saving it to %s failed - the file may be read-only, checked out by source control, or the path is not writable."), *SaveAsPackage));
		Result->SetStringField(
			TEXT("note"),
			TEXT("The new level exists in memory only. Save it with level.save and a \"path\" before relying on it."));
	}
	else
	{
		Result->SetStringField(
			TEXT("note"),
			TEXT("The new level exists in memory only. Save it with level.save and a \"path\" before relying on it."));
	}
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// level.list / level.set_streaming —— 世界由哪些关卡组成，以及那几个勾选框
//
// ## 为什么非补不可
//
// 「编辑器里好好的，一按 Play 全变了」是这套工具最常撞上的一类问题，而它的
// 头号成因是：子关卡的**编辑器可见**和**游戏加载**是两个独立开关，默认值
// 还不一样。真实案例（2026-09-07，Studio 关卡）：灰色影棚背景、地面、
// 后期处理体积全在挂进来的 `L_BaseEnvironment` 子关卡里，编辑器视口照常显示，
// PIE 里整个环境不加载 —— 背景纯黑。
//
// 那一轮排查里，材质图、bHidden、构造脚本、蓝图全被逐个排除掉了，最后卡在
// 一个纯粹的信息缺口上：**没有任何一条命令能回答「这个世界由哪些关卡组成、
// 每个子关卡的加载和可见标志是什么」**。actor.* 只能查 Actor；level.get_current
// 只回当前那一张；Python 那边 `get_streaming_levels()` 之类根本没暴露；
// umap 的属性数据压在包体里，grep 只搜得到名字表。
//
// 而这件事在用户的 Levels 窗格里一直是明摆着的 —— 他十秒看完就定论了。
// 缺的从来不是本事，是这一条信息进不了工具链。
//
// ## 为什么两条命令一起给
//
// 只给 list 的话，模型能诊断却改不了，只能让用户自己去点 Levels 窗格 ——
// 那正是这次真实发生的事。诊断和修复要在同一条链上。
// ============================================================================

namespace
{
	/** 编辑器所见与游戏所得对不上的两种方向 */
	enum class EUAL_LevelMismatch : uint8
	{
		/** 两边一致 */
		None,
		/** 编辑器里看得见，游戏里不加载 —— PIE 突然变黑/少东西的头号成因 */
		EditorOnly,
		/** 游戏里会加载，但编辑器里被藏着 —— 摆位置时看不见，跑起来突然冒出来 */
		GameOnly
	};

	/**
	 * 判这个子关卡两边对不对得上。
	 *
	 * 单独拎成纯函数是为了能被自动化测试钉住：判反了的代价不是「少一条提示」，
	 * 而是把模型**推向相反的结论** —— 明明是「游戏里不加载」，报成「编辑器里
	 * 藏起来了」，它接下来会去翻 bHidden 和材质，正好是这次白走的那两段路。
	 *
	 * @param bVisibleInEditor      `GetShouldBeVisibleInEditor()`
	 * @param bVisibleAtRuntime     **必须**由 `UAL_RuntimeVisible()` 算，不能用
	 *                              `ShouldBeVisible()` —— 见那个函数的注释
	 */
	EUAL_LevelMismatch UAL_ClassifyLevelMismatch(bool bVisibleInEditor, bool bVisibleAtRuntime)
	{
		if (bVisibleInEditor && !bVisibleAtRuntime)
		{
			return EUAL_LevelMismatch::EditorOnly;
		}
		if (!bVisibleInEditor && bVisibleAtRuntime)
		{
			return EUAL_LevelMismatch::GameOnly;
		}
		return EUAL_LevelMismatch::None;
	}

	const TCHAR* UAL_MismatchName(EUAL_LevelMismatch Mismatch)
	{
		switch (Mismatch)
		{
		case EUAL_LevelMismatch::EditorOnly: return TEXT("editor_only");
		case EUAL_LevelMismatch::GameOnly:   return TEXT("game_only");
		default:                             return TEXT("none");
		}
	}

	/**
	 * 「这一层在游戏里看得见吗」—— 自己算，**不要**调 `ULevelStreaming::ShouldBeVisible()`。
	 *
	 * 那个函数是按世界分支的（引擎 LevelStreaming.cpp，5.0–5.8 一字不差）：
	 *
	 *     if (GetWorld()->IsGameWorld()) { return bShouldBeVisible && ShouldBeLoaded(); }
	 *     return bShouldBeVisibleInEditor;   // 编辑器世界走这条
	 *
	 * 编辑器世界里它返回的**就是** `GetShouldBeVisibleInEditor()` 那一位。拿它和
	 * 编辑器位去比，等于拿一个数跟自己比 —— 永远相等，`mismatch` 永远是 none。
	 *
	 * 这不是理论问题：`Handle_ListLevels` 走 `GetLiveWorld()`，没跑 PIE 时就是编辑器
	 * 世界，也就是「先查一下再决定要不要跑」这个**主用法**。第一版就是这么写的，
	 * 结果整个功能在它最该起作用的路径上恒定失效，还倒过来断言「两边一致」，
	 * 把模型推回材质和 bHidden —— 正是它本来要终结的那两段弯路。
	 *
	 * 自己算就与世界无关：这两个值本身是存在关卡里的意图，不随读它的世界变。
	 */
	bool UAL_RuntimeVisibleFrom(bool bShouldBeVisibleFlag, bool bShouldBeLoaded)
	{
		return bShouldBeVisibleFlag && bShouldBeLoaded;
	}

	bool UAL_RuntimeVisible(const ULevelStreaming* Streaming)
	{
		return Streaming
			&& UAL_RuntimeVisibleFrom(Streaming->GetShouldBeVisibleFlag(), Streaming->ShouldBeLoaded());
	}

	/**
	 * 对不上的**原因**，而不只是方向。
	 *
	 * `editor_only` 是一个可见性判断，底下压着两种完全不同的病：这一层游戏里
	 * 根本不加载，和这一层加载了、只是没显示。前者要改流送方式，后者要改
	 * `should_be_visible` —— 只报 `editor_only` 的话，调用方对第二种会一直
	 * 开 `always_loaded=true`，而那个开关碰都碰不到 `bShouldBeVisible`，
	 * 于是回读依旧对不上，它就再试一次。
	 */
	const TCHAR* UAL_MismatchReason(const ULevelStreaming* Streaming, EUAL_LevelMismatch Mismatch)
	{
		if (Mismatch != EUAL_LevelMismatch::EditorOnly || !Streaming)
		{
			return nullptr;
		}
		return Streaming->ShouldBeLoaded() ? TEXT("loaded_but_hidden") : TEXT("not_loaded");
	}

	/** 数这一层关卡里的 Actor。`Actors` 里允许有空洞，不能直接用 Num() */
	int32 UAL_CountLevelActors(const ULevel* Level)
	{
		if (!Level)
		{
			return 0;
		}
		int32 Count = 0;
		for (const AActor* Actor : Level->Actors)
		{
			if (Actor)
			{
				++Count;
			}
		}
		return Count;
	}

	/**
	 * 关卡包名，去掉 PIE 前缀。
	 *
	 * PIE 世界里同一张关卡叫 `/Game/Maps/UEDPIE_0_L_Base`。原样回给调用方的话，
	 * 它按这个名字回头去 level.set_streaming 或 level.open 会全部落空 ——
	 * 那个包只在这次会话里存在。
	 */
	FString UAL_LevelPackageName(const ULevelStreaming* Streaming)
	{
		if (!Streaming)
		{
			return FString();
		}
		return UWorld::RemovePIEPrefix(Streaming->GetWorldAssetPackageFName().ToString());
	}

	/** 一个子关卡的全部状态。读的都是公开访问器，没有反射猜测 */
	TSharedPtr<FJsonObject> UAL_StreamingLevelJson(const ULevelStreaming* Streaming)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Streaming)
		{
			return Obj;
		}

		const FString PackageName = UAL_LevelPackageName(Streaming);
		Obj->SetStringField(TEXT("package"), PackageName);
		Obj->SetStringField(TEXT("name"), FPackageName::GetShortName(PackageName));

		// 「流送方式」就是 Levels 窗格里那一列。AlwaysLoaded 表示游戏一开始就在，
		// Dynamic 表示要靠蓝图/流送体积去加载 —— 没人去加载它就永远不出现
		const UClass* StreamingClass = Streaming->GetClass();
		Obj->SetStringField(TEXT("streaming_class"), StreamingClass ? StreamingClass->GetName() : TEXT("Unknown"));
		Obj->SetBoolField(TEXT("always_loaded"), Streaming->ShouldBeAlwaysLoaded());

		// 意图（游戏里打算怎样）。visible_at_runtime 自己算，理由见 UAL_RuntimeVisible
		const bool bShouldBeLoaded = Streaming->ShouldBeLoaded();
		const bool bVisibleAtRuntime = UAL_RuntimeVisible(Streaming);
		Obj->SetBoolField(TEXT("should_be_loaded"), bShouldBeLoaded);
		Obj->SetBoolField(TEXT("should_be_visible"), Streaming->GetShouldBeVisibleFlag());
		Obj->SetBoolField(TEXT("visible_at_runtime"), bVisibleAtRuntime);

		// 编辑器那一侧（Levels 窗格里的眼睛）
		const bool bVisibleInEditor = Streaming->GetShouldBeVisibleInEditor();
		Obj->SetBoolField(TEXT("visible_in_editor"), bVisibleInEditor);

		// 现状（此刻内存里到底是什么样）
		Obj->SetBoolField(TEXT("is_loaded"), Streaming->IsLevelLoaded());
		Obj->SetBoolField(TEXT("is_visible"), Streaming->IsLevelVisible());

		if (const ULevel* Loaded = Streaming->GetLoadedLevel())
		{
			Obj->SetNumberField(TEXT("actor_count"), UAL_CountLevelActors(Loaded));
		}

		const EUAL_LevelMismatch Mismatch = UAL_ClassifyLevelMismatch(bVisibleInEditor, bVisibleAtRuntime);
		Obj->SetStringField(TEXT("mismatch"), UAL_MismatchName(Mismatch));
		if (const TCHAR* Reason = UAL_MismatchReason(Streaming, Mismatch))
		{
			Obj->SetStringField(TEXT("mismatch_reason"), Reason);
		}

		return Obj;
	}

	/**
	 * 这个流送对象是 World Partition 自己生成的运行时格子吗。
	 *
	 * WP 在游戏世界里把格子塞进同一个 StreamingLevels 数组
	 * （`UWorldPartitionLevelStreamingDynamic::Load/Activate` 末尾的
	 * `PlayWorld->AddUniqueStreamingLevel(this)`），而 `ULevelStreaming` 的构造函数
	 * 把 `bShouldBeVisibleInEditor` 默认设成 true 且这条路上从不清掉 ——
	 * 于是每个「加载了但当前没激活」的格子都长得像 editor_only。
	 *
	 * 后果不是多几行噪音：试玩报告会点名一串自动生成的格子说它们「没进游戏世界」，
	 * 并让调用方去对这些**编辑器世界里根本不存在**的临时对象调 set_streaming。
	 * 那些调用只会 404，而真正的问题一个都没查出来。
	 *
	 * 按类名认而不是 Cast：WorldPartition 的运行时类不在本插件的依赖模块里，
	 * 为一句判断把整个模块拖进来不划算，而这个类名 5.0–5.8 没变过。
	 */
	bool UAL_IsWorldPartitionRuntimeCell(const ULevelStreaming* Streaming)
	{
		for (const UClass* Class = Streaming ? Streaming->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
		{
			if (Class->GetFName() == FName(TEXT("WorldPartitionLevelStreamingDynamic")))
			{
				return true;
			}
		}
		return false;
	}
}

TSharedPtr<FJsonObject> FUAL_LevelCommands::BuildLevelComposition(UWorld* World)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	if (!World)
	{
		return Data;
	}

	// 持久关卡：它永远在，没有加载标志可言，但调用方需要知道自己在哪张图上
	TSharedPtr<FJsonObject> Persistent = MakeShared<FJsonObject>();
	const FString PersistentPackage = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
	Persistent->SetStringField(TEXT("package"), PersistentPackage);
	Persistent->SetStringField(TEXT("name"), FPackageName::GetShortName(PersistentPackage));
	Persistent->SetNumberField(TEXT("actor_count"), UAL_CountLevelActors(World->PersistentLevel));
	Data->SetObjectField(TEXT("persistent"), Persistent);

	/**
	 * World Partition 的关卡在**编辑器里**没有手工挂的 StreamingLevels —— 它按空间
	 * 网格自动分块。不说清楚的话，调用方看到「0 个子关卡」会得出「这张图只有一层」
	 * 的错误结论，而 WP 图恰恰是「编辑器看得见、运行时不一定加载」的另一种成因
	 * （那边归数据层和距离管，不归这几个标志管）。
	 *
	 * 但在**游戏世界里** WP 会把运行时格子塞进同一个数组，见
	 * `UAL_IsWorldPartitionRuntimeCell`。所以这句「0 个是正常的」只对编辑器世界成立。
	 */
	const bool bPartitioned = World->IsPartitionedWorld();
	Data->SetBoolField(TEXT("is_world_partition"), bPartitioned);

	TArray<TSharedPtr<FJsonValue>> Levels;
	TArray<FString> EditorOnly;
	TArray<FString> GameOnly;
	int32 RuntimeCells = 0;

	for (ULevelStreaming* Streaming : World->GetStreamingLevels())
	{
		if (!Streaming)
		{
			continue;
		}

		// WP 自动生成的运行时格子：数出来但**不参与判定**，也不进两张点名清单。
		// 它们几乎全都长得像 editor_only（构造函数默认 bShouldBeVisibleInEditor=true），
		// 报出去就是一串假阳性 + 一串会 404 的修复建议
		if (UAL_IsWorldPartitionRuntimeCell(Streaming))
		{
			++RuntimeCells;
			continue;
		}

		TSharedPtr<FJsonObject> Entry = UAL_StreamingLevelJson(Streaming);
		Levels.Add(MakeShared<FJsonValueObject>(Entry));

		FString Mismatch;
		Entry->TryGetStringField(TEXT("mismatch"), Mismatch);
		FString Name;
		Entry->TryGetStringField(TEXT("name"), Name);
		if (Mismatch == TEXT("editor_only"))
		{
			EditorOnly.Add(Name);
		}
		else if (Mismatch == TEXT("game_only"))
		{
			GameOnly.Add(Name);
		}
	}

	Data->SetNumberField(TEXT("streaming_level_count"), Levels.Num());
	Data->SetArrayField(TEXT("streaming_levels"), Levels);
	if (RuntimeCells > 0)
	{
		// 数目要给，否则调用方会奇怪「跑着的时候明明加载了一堆东西，怎么一个都没列」
		Data->SetNumberField(TEXT("world_partition_runtime_cells"), RuntimeCells);
	}

	// 两张点名清单。模型不一定逐条读数组，而「哪一层对不上」就是这条命令的
	// 全部价值所在 —— 埋在二十行 JSON 里等于没给
	TArray<TSharedPtr<FJsonValue>> EditorOnlyJson;
	for (const FString& Name : EditorOnly)
	{
		EditorOnlyJson.Add(MakeShared<FJsonValueString>(Name));
	}
	Data->SetArrayField(TEXT("editor_only_levels"), EditorOnlyJson);

	TArray<TSharedPtr<FJsonValue>> GameOnlyJson;
	for (const FString& Name : GameOnly)
	{
		GameOnlyJson.Add(MakeShared<FJsonValueString>(Name));
	}
	Data->SetArrayField(TEXT("game_only_levels"), GameOnlyJson);

	return Data;
}

void FUAL_LevelCommands::Handle_ListLevels(const TSharedPtr<FJsonObject>& /*Payload*/, const FString RequestId)
{
	// 跟其它查询一致：PIE 跑着就读正在跑的那个世界。「游戏里到底加载了哪几层」
	// 正是这条命令最该回答的问题，这时候读编辑器世界等于答非所问
	UWorld* World = UAL_CommandUtils::GetLiveWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 404, TEXT("No world is available"));
		return;
	}

	TSharedPtr<FJsonObject> Data = BuildLevelComposition(World);
	Data->SetBoolField(TEXT("ok"), true);
	UAL_CommandUtils::AddWorldInfo(Data);
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

namespace
{
	/**
	 * 按名字找子关卡。短名（`L_BaseEnvironment`）和完整包路径都收。
	 *
	 * 只收完整路径是错的：调用方手里的名字多半来自 level.list 的 `name`，
	 * 或者用户嘴里说的那个词。只收短名同样是错的：两个目录下重名的子关卡
	 * 真实存在，那时候必须让调用方用完整路径把话说清楚，而不是随便挑一个。
	 *
	 * ## 必须扫完整个数组再下结论
	 *
	 * 第一版在循环里一撞见第二个同名短名就 `return nullptr`。两个后果，都在
	 * 「重名」这个唯一需要它的场景里发作：
	 *
	 *   1. 三个 `L_Env` 时，调用方照着错误提示传了完整路径 `/Game/C/L_Env`，
	 *      而它排在数组第三位 —— 循环在第二位就退出了，**精确匹配永远够不着**，
	 *      于是那条「拿完整路径重来」的活路根本走不通，模型只能原地打转。
	 *   2. `OutCandidates` 只装到退出那一刻，报给调用方的候选清单是残缺的 ——
	 *      偏偏这是它最需要看全的时候。
	 *
	 * 所以：先扫完、收齐，再判断。
	 *
	 * @param OutAmbiguous 出参：短名撞了多个，需要完整路径。和「压根没有」是两回事
	 */
	ULevelStreaming* UAL_FindStreamingLevel(UWorld* World, const FString& Wanted,
		TArray<FString>& OutCandidates, bool& bOutAmbiguous)
	{
		OutCandidates.Reset();
		bOutAmbiguous = false;
		if (!World)
		{
			return nullptr;
		}

		const FString WantedShort = FPackageName::GetShortName(Wanted);
		ULevelStreaming* Exact = nullptr;
		TArray<ULevelStreaming*> ShortMatches;

		for (ULevelStreaming* Streaming : World->GetStreamingLevels())
		{
			if (!Streaming)
			{
				continue;
			}
			const FString Package = UAL_LevelPackageName(Streaming);
			OutCandidates.Add(Package);

			if (!Exact && Package.Equals(Wanted, ESearchCase::IgnoreCase))
			{
				Exact = Streaming;
			}
			else if (FPackageName::GetShortName(Package).Equals(WantedShort, ESearchCase::IgnoreCase))
			{
				ShortMatches.Add(Streaming);
			}
		}

		// 完整路径优先：它是重名时唯一说得清的写法
		if (Exact)
		{
			return Exact;
		}
		if (ShortMatches.Num() == 1)
		{
			return ShortMatches[0];
		}
		bOutAmbiguous = ShortMatches.Num() > 1;
		return nullptr;
	}
}

void FUAL_LevelCommands::Handle_SetLevelStreaming(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 这是编辑器动作。PIE 里改了也只活到用户按停止那一刻，而调用方多半会
	// 把「改好了」原样转述给用户
	if (UAL_CommandUtils::RefuseDuringPlay(RequestId, TEXT("改子关卡的流送设置"), TEXT("changing level streaming settings")))
	{
		return;
	}

	FString Wanted;
	if (!Payload->TryGetStringField(TEXT("level"), Wanted) || Wanted.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: level"));
		return;
	}

	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 404, TEXT("No editor world is available"));
		return;
	}

	TArray<FString> Candidates;
	bool bAmbiguous = false;
	ULevelStreaming* Streaming = UAL_FindStreamingLevel(World, Wanted, Candidates, bAmbiguous);
	if (!Streaming)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> CandidateJson;
		for (const FString& Package : Candidates)
		{
			CandidateJson.Add(MakeShared<FJsonValueString>(Package));
		}
		Details->SetArrayField(TEXT("available_levels"), CandidateJson);
		Details->SetBoolField(TEXT("ambiguous"), bAmbiguous);

		// 「重名」和「压根没有」要分开说：前者的解法是换个写法重来，
		// 后者是这张图里就没这个东西。混成一句话，调用方会对着一个
		// 明明存在的关卡以为它不存在
		UAL_CommandUtils::SendError(
			RequestId, bAmbiguous ? 409 : 404,
			bAmbiguous
				? FString::Printf(
					TEXT("\"%s\" is ambiguous - more than one streaming level has that short name. ")
					TEXT("Pass one of the full package paths listed in available_levels."),
					*Wanted)
				: FString::Printf(
					TEXT("No streaming level matches \"%s\". See available_levels for what this map has."),
					*Wanted),
			Details);
		return;
	}

	const TSharedPtr<FJsonObject> Before = UAL_StreamingLevelJson(Streaming);

	bool bWantAlwaysLoaded = false;
	const bool bHasAlwaysLoaded = Payload->TryGetBoolField(TEXT("always_loaded"), bWantAlwaysLoaded);
	bool bWantLoaded = false;
	const bool bHasLoaded = Payload->TryGetBoolField(TEXT("should_be_loaded"), bWantLoaded);
	bool bWantVisible = false;
	const bool bHasVisible = Payload->TryGetBoolField(TEXT("should_be_visible"), bWantVisible);
	bool bWantEditorVisible = false;
	const bool bHasEditorVisible = Payload->TryGetBoolField(TEXT("visible_in_editor"), bWantEditorVisible);

	if (!bHasAlwaysLoaded && !bHasLoaded && !bHasVisible && !bHasEditorVisible)
	{
		UAL_CommandUtils::SendError(
			RequestId, 400,
			TEXT("Nothing to change. Pass at least one of: always_loaded, should_be_loaded, should_be_visible, visible_in_editor."));
		return;
	}

	bool bClassChanges = bHasAlwaysLoaded && bWantAlwaysLoaded != Streaming->ShouldBeAlwaysLoaded();

	TArray<FString> Changed;

	/**
	 * 事务只包得住「改标志」那一半。
	 *
	 * 换流送方式的内部做法是把旧的 ULevelStreaming 从世界里摘掉、按新类重新加一个
	 * （见引擎 SetStreamingClassForLevel），撤销栈接不住这种对象替换 ——
	 * 引擎自己的 Levels 窗格也没给它开事务。开了反而更糟：Ctrl+Z 会回到一个
	 * 半拉状态。所以这里如实分两种情况，并把 undoable 回给调用方。
	 */
	TUniquePtr<FUAL_ScopedTransaction> Transaction;
	if (!bClassChanges)
	{
		Transaction = MakeUnique<FUAL_ScopedTransaction>(
			UAL_CommandUtils::LText(TEXT("修改子关卡流送设置"), TEXT("Change Level Streaming Settings")));
		Streaming->Modify();
	}

	/**
	 * 编辑器可见性**先处理**，而且要真的把关卡加载出来。
	 *
	 * 顺序不是随意的：换流送方式要求关卡此刻是加载着的（下面那道 409），而
	 * 「怎么把它加载出来」的唯一答案就是这一步。第一版把它放在换类之后，于是
	 * 那条错误提示指向的补救动作发生在它自己被拒之后，形成死循环：
	 *   always_loaded → 409「先 visible_in_editor=true」→ 照做，回 200 但
	 *   is_loaded 仍是 false → 再试 always_loaded → 同一个 409。
	 * 提上来之后，`{always_loaded:true, visible_in_editor:true}` 一次调用就能成。
	 */
	if (bHasEditorVisible)
	{
		if (ULevel* Loaded = Streaming->GetLoadedLevel())
		{
			// 已经是想要的状态就别碰：SetLevelVisibility 末尾是
			// `FlushLevelStreaming(); check(Level->bIsVisible == bShouldBeVisible);`
			// —— 一次进不去目标状态就是断言崩溃。无谓地调它等于白担一次风险
			if (Streaming->IsLevelVisible() != bWantEditorVisible)
			{
				UEditorLevelUtils::SetLevelVisibility(Loaded, bWantEditorVisible, /*bForceLayersVisible=*/false);
			}
		}
		else
		{
			// 还没加载：设标志只是登记意图，得 flush 一次才真的把包读进来。
			// 不 flush 的话这一步「成功」了但什么都没发生
			Streaming->SetShouldBeVisibleInEditor(bWantEditorVisible);
			if (bWantEditorVisible)
			{
				Streaming->SetShouldBeLoaded(true);
				World->FlushLevelStreaming();
			}
		}
	}

	/**
	 * 换流送方式要求关卡此刻是加载着的 —— 引擎那边
	 * `UEditorLevelUtils::SetStreamingClassForLevel` 上来就 `check(Level)`，
	 * 传一个没加载的进去是**当场断言崩溃**，不是返回失败。
	 */
	if (bClassChanges && !Streaming->IsLevelLoaded())
	{
		UAL_CommandUtils::SendError(
			RequestId, 409,
			FString::Printf(
				TEXT("\"%s\" is not loaded right now, and changing its streaming method requires a loaded level. ")
				TEXT("Retry with visible_in_editor=true in the same call - that loads it first."),
				*Wanted));
		return;
	}

	if (bClassChanges)
	{
		UClass* NewClass = bWantAlwaysLoaded
			? ULevelStreamingAlwaysLoaded::StaticClass()
			: ULevelStreamingDynamic::StaticClass();

		/**
		 * 换类会把两样东西弄丢，都得自己接住。
		 *
		 * 1. **游戏侧的两个标志。** `SetStreamingClassForLevel` 只搬
		 *    LevelTransform / EditorStreamingVolumes / MinTimeBetweenVolumeUnloadRequests
		 *    / LevelColor / Keywords / FolderPath，不搬 bShouldBeLoaded 和
		 *    bShouldBeVisible。而 `ULevelStreamingDynamic` 的构造函数是空的，
		 *    两个位都是 0 —— 于是「改错了再调一次就回去了」根本不成立：
		 *    always_loaded=false 换回去之后，这一层在游戏里**彻底不加载了**。
		 * 2. **编辑器的当前关卡。** 内部的 `AddLevelToWorld` 末尾会
		 *    `SetCurrentLevel(NewLevel)`。不还原的话，紧接着那句「记得 ue_save_level」
		 *    存的是这个子关卡，而流送设置在持久关卡里 —— 存了个寂寞，
		 *    还回报「已保存 <持久关卡名>」。之后 spawn 的 Actor 也会落进子关卡。
		 */
		const bool bPrevShouldBeLoaded = Streaming->ShouldBeLoaded();
		const bool bPrevShouldBeVisible = Streaming->GetShouldBeVisibleFlag();
		ULevel* PrevCurrentLevel = World->GetCurrentLevel();

		// 这个调用会把旧对象换掉，返回的才是新的那一个。继续用旧指针
		// 读回来的会是一个已经不在世界里的孤儿 —— 回读全对、实际没生效
		ULevelStreaming* Replaced = UEditorLevelUtils::SetStreamingClassForLevel(Streaming, NewClass);

		if (PrevCurrentLevel && World->GetCurrentLevel() != PrevCurrentLevel)
		{
			World->SetCurrentLevel(PrevCurrentLevel);
		}

		if (!Replaced)
		{
			// 走到这里世界**已经被改过了**：旧的流送对象被摘掉、新的加了进去，
			// 只是最后那次按 ULevel* 的回查没找着。说「什么都没改」是假话，
			// 而且会让调用方以为可以放心重试。脏标记照打 —— 地图结构确实变了
			if (UPackage* Package = World->GetOutermost())
			{
				Package->MarkPackageDirty();
				FUAL_TouchedPackages::Touch(Package);
			}
			UAL_CommandUtils::SendError(
				RequestId, 500,
				TEXT("The streaming method was swapped but the new streaming level could not be found afterwards. ")
				TEXT("The map WAS modified and is now in an uncertain state - call level.list to see what it looks like, ")
				TEXT("and do not save until you have checked."));
			return;
		}
		Streaming = Replaced;

		// 把两个游戏侧标志接回来。用户显式传了的以他为准（下面那两段会覆盖）
		Streaming->SetShouldBeLoaded(bPrevShouldBeLoaded);
		Streaming->SetShouldBeVisible(bPrevShouldBeVisible);
	}

	if (bHasLoaded)
	{
		Streaming->SetShouldBeLoaded(bWantLoaded);
	}
	if (bHasVisible)
	{
		Streaming->SetShouldBeVisible(bWantVisible);
	}

	// 回读。设进去和生效是两回事 —— ULevelStreamingAlwaysLoaded 上的
	// should_be_loaded 永远是 true（基类 SetShouldBeLoaded 的函数体是空的），
	// 写 false 进去它照样是 true
	const TSharedPtr<FJsonObject> After = UAL_StreamingLevelJson(Streaming);

	/**
	 * `changed` 只记**真的变了**的字段。
	 *
	 * 第一版是「传了这个键就记一笔」，于是对一个固定加载的关卡写
	 * `should_be_loaded=false` 会回 `changed:["should_be_loaded"]` 而回读仍是 true。
	 * 回读没撒谎，但 `changed` 撒了，而它更短、更容易被当成结论读。
	 */
	auto BoolField = [](const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key) -> bool
	{
		bool Value = false;
		return Obj.IsValid() && Obj->TryGetBoolField(Key, Value) ? Value : false;
	};

	// 直接对着 before/after 逐字段比，而不是「谁调了 setter 谁记一笔」——
	// 后者算的是我们做了什么，前者算的是引擎接受了什么，只有后一个对调用方有用
	static const TCHAR* const TrackedFields[] = {
		TEXT("always_loaded"), TEXT("should_be_loaded"), TEXT("should_be_visible"),
		TEXT("visible_in_editor"), TEXT("is_loaded")
	};
	Changed.Reset();
	for (const TCHAR* Field : TrackedFields)
	{
		if (BoolField(Before, Field) != BoolField(After, Field))
		{
			Changed.Add(Field);
		}
	}

	/**
	 * 一件都没真的改到就别打脏标记。
	 *
	 * 打了的话，下一次 level.open 会被「有 N 处未保存的改动」拦下，而那 N 处里
	 * 有一处是这次什么都没干的调用凭空造出来的。模型很可能就此改用 force=true，
	 * 于是用户**真正**没保存的东西被一起丢掉 —— 为一个从未发生的改动。
	 */
	const bool bAnythingChanged = Changed.Num() > 0;

	// 流送设置存在持久关卡的包里。不标脏的话改动不会被保存工具看见，
	// 用户重开工程就回到原样 —— 而工具已经报过「改好了」
	UPackage* WorldPackage = bAnythingChanged ? World->GetOutermost() : nullptr;
	if (WorldPackage)
	{
		WorldPackage->MarkPackageDirty();
		FUAL_TouchedPackages::Touch(WorldPackage);
	}

	Transaction.Reset();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("ok"), true);
	Data->SetStringField(TEXT("level"), UAL_LevelPackageName(Streaming));
	Data->SetObjectField(TEXT("before"), Before);
	Data->SetObjectField(TEXT("after"), After);
	Data->SetBoolField(TEXT("undoable"), !bClassChanges);
	// 什么都没变就没什么要存的。恒回 true 会让调用方去存一张它没改过的图
	Data->SetBoolField(TEXT("needs_save"), bAnythingChanged);

	TArray<TSharedPtr<FJsonValue>> ChangedJson;
	for (const FString& Field : Changed)
	{
		ChangedJson.Add(MakeShared<FJsonValueString>(Field));
	}
	Data->SetArrayField(TEXT("changed"), ChangedJson);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// 单元测试
//
// 跑法：编辑器 → Tools → Session Frontend → Automation → 勾 UnrealAgentLink，
// 或命令行 -ExecCmds="Automation RunTests UnrealAgentLink;Quit"。
//
// 只测纯函数：脏包过滤那条判断没有引擎依赖，而它一旦判错，代价是用户
// **不可撤销地**丢掉未保存的关卡（判太松），或者被逼着次次加 force、
// 最后同样丢掉（判太紧）。两个方向都得钉住。
// ============================================================================

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUAL_DirtyPackageFilterTest,
	"UnrealAgentLink.Level.DirtyPackageFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUAL_DirtyPackageFilterTest::RunTest(const FString& /*Parameters*/)
{
	// 这一条就是这个过滤器存在的理由：真机上 level.open 每次都被它拦下
	TestTrue(TEXT("/Script/SlateCore 是引擎模块包，不是用户能保存的东西"),
		UAL_IsIgnorableDirtyPackage(TEXT("/Script/SlateCore")));
	TestTrue(TEXT("插件自己的模块包同理"),
		UAL_IsIgnorableDirtyPackage(TEXT("/Script/UnrealAgentLink")));
	TestTrue(TEXT("transient 包只活在内存里，存不下来"),
		UAL_IsIgnorableDirtyPackage(TEXT("/Engine/Transient")));

	// 排除之后必须仍然如实拦人 —— 否则这道保护就成了摆设，
	// 而它拦的是「不可撤销地丢掉用户这一轮的活」
	TestFalse(TEXT("用户的关卡照拦"),
		UAL_IsIgnorableDirtyPackage(TEXT("/Game/Maps/Main")));
	TestFalse(TEXT("用户的资产照拦"),
		UAL_IsIgnorableDirtyPackage(TEXT("/Game/Blueprints/BP_Door")));
	TestFalse(TEXT("引擎自带内容是真资产，不是模块包"),
		UAL_IsIgnorableDirtyPackage(TEXT("/Engine/BasicShapes/Cube")));

	// /Temp/ 下住着刚新建、还没存过的空关卡 —— 那往往正是用户刚摆了半天
	// Actor 的那张图。把它排掉等于把保护挖空，所以这条必须是 false
	TestFalse(TEXT("/Temp/ 下的临时关卡不排除：它可能正是用户在编辑的那张"),
		UAL_IsIgnorableDirtyPackage(TEXT("/Temp/Untitled_1")));

	// 前缀要带斜杠。没有的话 /ScriptedThing 这种用户路径会被误伤
	TestFalse(TEXT("只认 /Script/ 这个前缀，不误伤名字相近的用户路径"),
		UAL_IsIgnorableDirtyPackage(TEXT("/ScriptedSequences/Intro")));

	return true;
}

/**
 * 子关卡「编辑器所见 vs 游戏所得」的判定。
 *
 * 这条判反了不是少给一句提示，而是把模型推向相反的方向：本该说「游戏里
 * 不加载」的时候说成「编辑器里藏起来了」，它接下来会去翻 bHidden、材质、
 * 构造脚本 —— 2026-09-07 那次真实排查白走的正是那两段路。
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUAL_LevelMismatchTest,
	"UnrealAgentLink.Level.StreamingMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUAL_LevelMismatchTest::RunTest(const FString& /*Parameters*/)
{
	// 就是 L_BaseEnvironment 那一档：视口里是灰色影棚，PIE 里背景纯黑
	TestTrue(TEXT("编辑器可见 + 运行时不可见 = editor_only"),
		UAL_ClassifyLevelMismatch(/*bVisibleInEditor=*/true, /*bVisibleAtRuntime=*/false)
			== EUAL_LevelMismatch::EditorOnly);

	// 反方向同样要认出来：摆位置时看不见，跑起来突然冒出来
	TestTrue(TEXT("编辑器隐藏 + 运行时可见 = game_only"),
		UAL_ClassifyLevelMismatch(false, true) == EUAL_LevelMismatch::GameOnly);

	TestTrue(TEXT("两边都可见 = 没有不一致"),
		UAL_ClassifyLevelMismatch(true, true) == EUAL_LevelMismatch::None);

	// 两边都关着也是一致的。报成不一致会让模型去「修」一个用户故意关掉的关卡
	TestTrue(TEXT("两边都关着 = 没有不一致"),
		UAL_ClassifyLevelMismatch(false, false) == EUAL_LevelMismatch::None);

	// 名字要和响应字段对得上，改了枚举忘了改字符串的话调用方会看到 "none"
	TestEqual(TEXT("editor_only 的字符串名"),
		FString(UAL_MismatchName(EUAL_LevelMismatch::EditorOnly)), FString(TEXT("editor_only")));
	TestEqual(TEXT("game_only 的字符串名"),
		FString(UAL_MismatchName(EUAL_LevelMismatch::GameOnly)), FString(TEXT("game_only")));
	TestEqual(TEXT("一致时的字符串名"),
		FString(UAL_MismatchName(EUAL_LevelMismatch::None)), FString(TEXT("none")));

	return true;
}

/**
 * 「游戏里看不看得见」这个数是怎么来的。
 *
 * 上面那个测试只钉了真值表的方向，钉不住**参数是谁给的** —— 而第一版的
 * 缺陷恰恰全在这一层：`bVisibleAtRuntime` 传的是 `ULevelStreaming::ShouldBeVisible()`，
 * 那个函数在编辑器世界里返回的就是编辑器那一位，于是两个参数变成同一个数，
 * 判定恒为 None，整个功能在主用法上死掉而六条断言全绿。
 *
 * 所以这里单独钉住这个推导：它必须是两个**游戏侧**标志的与，与读它的世界无关。
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUAL_LevelRuntimeVisibleTest,
	"UnrealAgentLink.Level.RuntimeVisible",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUAL_LevelRuntimeVisibleTest::RunTest(const FString& /*Parameters*/)
{
	TestTrue(TEXT("要加载、也要显示，才在游戏里看得见"),
		UAL_RuntimeVisibleFrom(/*bShouldBeVisibleFlag=*/true, /*bShouldBeLoaded=*/true));

	// L_BaseEnvironment 那一档：显示标志开着，但游戏里压根不加载它
	TestFalse(TEXT("不加载就一定看不见，哪怕显示标志是开的"),
		UAL_RuntimeVisibleFrom(true, false));

	// 逻辑/音频子关卡那一档：加载了、在跑，只是没显示
	TestFalse(TEXT("加载了但显示标志关着，同样看不见"),
		UAL_RuntimeVisibleFrom(false, true));

	TestFalse(TEXT("两个都关着"),
		UAL_RuntimeVisibleFrom(false, false));

	// 把两级拼起来：这正是真机上那张 Studio 关卡的形状 ——
	// 编辑器眼睛开着、游戏侧不加载，必须判成 editor_only
	TestTrue(TEXT("编辑器可见 + 游戏不加载 => editor_only"),
		UAL_ClassifyLevelMismatch(
			/*bVisibleInEditor=*/true,
			UAL_RuntimeVisibleFrom(/*bShouldBeVisibleFlag=*/true, /*bShouldBeLoaded=*/false))
			== EUAL_LevelMismatch::EditorOnly);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
