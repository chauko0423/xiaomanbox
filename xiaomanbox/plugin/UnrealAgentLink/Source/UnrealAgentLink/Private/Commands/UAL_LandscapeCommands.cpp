#include "UAL_LandscapeCommands.h"

// 引擎头文件放在 UAL_* 前面：UAL_CommandUtils.h 自己没包 AActor / FProperty，
// 在统一构建里靠别的文件先包进来。这个文件改动后 UBT 会把它拎出来单独编，那时就得自给自足
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/RuntimeVirtualTextureComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Editor/EditorEngine.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeImportHelper.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "LandscapeStreamingProxy.h"
#include "LandscapeSubsystem.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "RuntimeVirtualTextureSetBounds.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "VT/RuntimeVirtualTexture.h"
#include "VT/RuntimeVirtualTextureVolume.h"

#include "UAL_CommandUtils.h"
#include "UAL_LandscapeCompat.h"
#include "UAL_LandscapeLayout.h"
#include "UAL_ReflectCall.h"
#include "UAL_ScopedTransaction.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALLandscape, Log, All);

namespace
{
	// 尺寸换算（只有编辑器下拉框里那几档合法）在 UAL_LandscapeLayout.h，那边有单测

	/** 高度图 16 位的中值就是 0 高度（`LandscapeDataAccess::MidValue`） */
	constexpr uint16 FlatHeightValue = 32768;

	/**
	 * 高度缩放 100 时，16 位高度图能表示的总落差是 512 米
	 * （65536 × `LANDSCAPE_ZSCALE`(1/128) × 100 厘米）。落差按缩放线性变。
	 */
	constexpr double HeightRangeMetersAtScale100 = 512.0;

	using UALLandscapeLayout::FLandscapeLayout;
	using UALLandscapeLayout::MaxResolutionPerAxis;
	using UALLandscapeLayout::PickLayout;

	// ------------------------------------------------------------------
	// 找地形、找它的代理
	// ------------------------------------------------------------------

	FString LandscapeLabel(const ALandscape* Landscape)
	{
		return Landscape ? Landscape->GetActorLabel() : FString();
	}

	/** 按标签 / 对象名 / 路径找；没给名字且关卡里只有一块时就是它 */
	ALandscape* FindLandscape(UWorld* World, const FString& Identifier, FString& OutError)
	{
		TArray<ALandscape*> All;
		for (TActorIterator<ALandscape> It(World); It; ++It)
		{
			All.Add(*It);
		}

		TArray<FString> Names;
		for (const ALandscape* L : All)
		{
			Names.Add(LandscapeLabel(L));
		}

		if (All.Num() == 0)
		{
			OutError = TEXT("There is no landscape in the current level.");
			return nullptr;
		}
		if (Identifier.IsEmpty())
		{
			if (All.Num() == 1)
			{
				return All[0];
			}
			OutError = FString::Printf(
				TEXT("This level has %d landscapes; say which one with `landscape`: %s"),
				All.Num(), *FString::Join(Names, TEXT(", ")));
			return nullptr;
		}
		for (ALandscape* L : All)
		{
			if (L->GetActorLabel().Equals(Identifier, ESearchCase::IgnoreCase)
				|| L->GetName().Equals(Identifier, ESearchCase::IgnoreCase)
				|| L->GetPathName().Equals(Identifier, ESearchCase::IgnoreCase))
			{
				return L;
			}
		}
		OutError = FString::Printf(TEXT("No landscape named '%s'. Landscapes in this level: %s"),
			*Identifier, *FString::Join(Names, TEXT(", ")));
		return nullptr;
	}

	/**
	 * 同一块地形的全部已加载代理，含主地形自己。
	 *
	 * 按 LandscapeGuid 在世界里扫，不走 `ULandscapeInfo` 的代理列表 —— 那个列表
	 * 5.3 改过名字和容器（`Proxies` → `StreamingProxies` + `ForEachLandscapeProxy`），
	 * 按 Guid 扫在 5.0–5.8 上是同一行代码。
	 */
	TArray<ALandscapeProxy*> CollectProxies(ALandscape* Landscape)
	{
		TArray<ALandscapeProxy*> Result;
		UWorld* World = Landscape ? Landscape->GetWorld() : nullptr;
		if (!World)
		{
			return Result;
		}
		const FGuid Guid = Landscape->GetLandscapeGuid();
		for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
		{
			if (It->GetLandscapeGuid() == Guid)
			{
				Result.Add(*It);
			}
		}
		return Result;
	}

	/**
	 * 真正装着地形块的那些代理。
	 *
	 * World Partition 下切块之后，主地形自己一块都不剩，块全在流送代理上；
	 * 普通关卡里就是主地形自己。RVT 挂没挂上要按这些数 —— 数主地形没有意义。
	 */
	TArray<ALandscapeProxy*> ComponentOwners(const TArray<ALandscapeProxy*>& Proxies)
	{
		TArray<ALandscapeProxy*> Result;
		for (ALandscapeProxy* Proxy : Proxies)
		{
			if (Proxy && Proxy->LandscapeComponents.Num() > 0)
			{
				Result.Add(Proxy);
			}
		}
		return Result;
	}

	/**
	 * 地形的世界包围盒，未加载的区域也算上。
	 *
	 * `ALandscape::GetCompleteBounds()`（5.0 起就有）在 World Partition 下会翻
	 * Actor 描述符，把没加载的格子也算进去；再并上已加载的块 —— 刚建出来、
	 * 还没存过盘的代理不一定已经有描述符。
	 */
	FBox LandscapeWorldBounds(ALandscape* Landscape, const TArray<ALandscapeProxy*>& Proxies)
	{
		FBox Bounds(ForceInit);
#if WITH_EDITOR
		if (Landscape && Landscape->GetLandscapeInfo())
		{
			const FBox Complete = Landscape->GetCompleteBounds();
			if (Complete.IsValid)
			{
				Bounds += Complete;
			}
		}
#endif
		for (const ALandscapeProxy* Proxy : Proxies)
		{
			for (const ULandscapeComponent* Component : Proxy->LandscapeComponents)
			{
				if (Component)
				{
					Bounds += Component->Bounds.GetBox();
				}
			}
		}
		return Bounds;
	}

	TSharedPtr<FJsonObject> BoxJson(const FBox& Box)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("min"), UAL_CommandUtils::MakeVectorJson(Box.Min));
		Obj->SetObjectField(TEXT("max"), UAL_CommandUtils::MakeVectorJson(Box.Max));
		return Obj;
	}

	TSharedPtr<FJsonObject> IntPointJson(const FIntPoint& P)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), P.X);
		Obj->SetNumberField(TEXT("y"), P.Y);
		return Obj;
	}

	// ------------------------------------------------------------------
	// RVT
	// ------------------------------------------------------------------

	/**
	 * 两种 RVT：
	 *   - 颜色：网格和草从这里取地面颜色做融合、远处地形直接显示它省性能。
	 *           类型取编辑器里最常用的「Base Color, Normal, Roughness, Specular」
	 *   - 高度：草、水、贴花按地形高度对齐
	 */
	struct FRvtKind
	{
		const TCHAR* Key;
		const TCHAR* Suffix;
		ERuntimeVirtualTextureMaterialType Type;
	};

	const FRvtKind ColorRvt{TEXT("color"), TEXT("Color"), ERuntimeVirtualTextureMaterialType::BaseColor_Normal_Specular};
	const FRvtKind HeightRvt{TEXT("height"), TEXT("Height"), ERuntimeVirtualTextureMaterialType::WorldHeight};

	FString MaterialTypeName(ERuntimeVirtualTextureMaterialType Type)
	{
		if (const UEnum* Enum = StaticEnum<ERuntimeVirtualTextureMaterialType>())
		{
			return Enum->GetNameStringByValue((int64)Type);
		}
		return FString::FromInt((int32)Type);
	}

	/**
	 * 这张 RVT 算颜色、高度还是别的。
	 *
	 * 按枚举**名字**判颜色而不是列值：5.8 把一个废弃值换成了 `Mask4`、又加了
	 * `Displacement`，列值的写法在老版本上根本编不过。
	 */
	FString RvtKindOf(const URuntimeVirtualTexture* Vt)
	{
		if (!Vt)
		{
			return TEXT("other");
		}
		if (Vt->GetMaterialType() == ERuntimeVirtualTextureMaterialType::WorldHeight)
		{
			return TEXT("height");
		}
		return MaterialTypeName(Vt->GetMaterialType()).StartsWith(TEXT("BaseColor")) ? TEXT("color") : TEXT("other");
	}

	/** `MaterialType` 是 protected，没有 setter —— 按属性写，兼容枚举属性和字节属性两种声明 */
	bool SetRvtMaterialType(URuntimeVirtualTexture* Vt, ERuntimeVirtualTextureMaterialType Type)
	{
		FProperty* Prop = FindFProperty<FProperty>(URuntimeVirtualTexture::StaticClass(), TEXT("MaterialType"));
		if (!Prop)
		{
			return false;
		}
		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Vt);
		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, (int64)Type);
			return true;
		}
		if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			ByteProp->SetIntPropertyValue(ValuePtr, (uint64)Type);
			return true;
		}
		return false;
	}

	/** 世界坐标下的体积范围。RVT 体积是一个 0..1 的单位盒乘上组件变换 */
	FBox VolumeWorldBox(const URuntimeVirtualTextureComponent* Component)
	{
		return FBox(FVector::ZeroVector, FVector::OneVector).TransformBy(Component->GetComponentTransform());
	}

	/** 容差：1% 或 1 米取大 —— 引擎自己算边界时会做纹素对齐，差一点点是正常的 */
	bool CoversAxis(double VolumeMin, double VolumeMax, double LandMin, double LandMax)
	{
		const double Tolerance = FMath::Max(100.0, (LandMax - LandMin) * 0.01);
		return VolumeMin <= LandMin + Tolerance && VolumeMax >= LandMax - Tolerance;
	}

	TSharedPtr<FJsonObject> DescribeVolume(ARuntimeVirtualTextureVolume* Volume, const FBox& LandBounds)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("label"), Volume->GetActorLabel());
		URuntimeVirtualTextureComponent* Component = Volume->VirtualTextureComponent;
		if (!Component)
		{
			Obj->SetBoolField(TEXT("covers_xy"), false);
			Obj->SetBoolField(TEXT("covers_z"), false);
			return Obj;
		}
		const FBox Box = VolumeWorldBox(Component);
		Obj->SetObjectField(TEXT("bounds"), BoxJson(Box));
		const bool bLandValid = LandBounds.IsValid != 0;
		Obj->SetBoolField(TEXT("covers_xy"), bLandValid
			&& CoversAxis(Box.Min.X, Box.Max.X, LandBounds.Min.X, LandBounds.Max.X)
			&& CoversAxis(Box.Min.Y, Box.Max.Y, LandBounds.Min.Y, LandBounds.Max.Y));
		Obj->SetBoolField(TEXT("covers_z"), bLandValid
			&& CoversAxis(Box.Min.Z, Box.Max.Z, LandBounds.Min.Z, LandBounds.Max.Z));
		return Obj;
	}

	TArray<ARuntimeVirtualTextureVolume*> VolumesFor(UWorld* World, const URuntimeVirtualTexture* Vt)
	{
		TArray<ARuntimeVirtualTextureVolume*> Result;
		for (TActorIterator<ARuntimeVirtualTextureVolume> It(World); It; ++It)
		{
			if (It->VirtualTextureComponent && It->VirtualTextureComponent->GetVirtualTexture() == Vt)
			{
				Result.Add(*It);
			}
		}
		return Result;
	}

	/**
	 * 地形材质里有没有「输出到 RVT」那个节点，各个引脚接没接。
	 *
	 * RVT 最常见的失败就是这里：地形声明了往 RVT 里画、体积也摆对了，但材质根本没写
	 * —— 引擎不报错，RVT 是空的，融合出来一片黑。
	 *
	 * 节点类没有导出（5.0 连 MinimalAPI 都没有），所以按类名认、按属性读引脚。
	 * 会钻进材质函数（地形材质多半包了几层），**不看材质图层（Material Layers）**
	 * —— 这一点要在回执里说出来，不能让「没找到」被当成「确实没有」。
	 */
	struct FRvtOutputScan
	{
		bool bFound = false;
		bool bBaseColor = false;
		bool bNormal = false;
		bool bRoughness = false;
		bool bSpecular = false;
		bool bWorldHeight = false;
		int32 FunctionsSearched = 0;
	};

	bool IsInputConnected(const UObject* Expression, const TCHAR* InputName)
	{
		const FStructProperty* Prop = FindFProperty<FStructProperty>(Expression->GetClass(), InputName);
		if (!Prop)
		{
			return false;
		}
		const FExpressionInput* Input = Prop->ContainerPtrToValuePtr<FExpressionInput>(Expression);
		return Input && Input->Expression != nullptr;
	}

	void ScanForRvtOutput(const TArray<UMaterialExpression*>& Expressions, FRvtOutputScan& Scan,
		TSet<const UObject*>& Visited, int32 Depth)
	{
		for (UMaterialExpression* Expression : Expressions)
		{
			if (Expression->GetClass()->GetName() == TEXT("MaterialExpressionRuntimeVirtualTextureOutput"))
			{
				Scan.bFound = true;
				Scan.bBaseColor |= IsInputConnected(Expression, TEXT("BaseColor"));
				Scan.bNormal |= IsInputConnected(Expression, TEXT("Normal"));
				Scan.bRoughness |= IsInputConnected(Expression, TEXT("Roughness"));
				Scan.bSpecular |= IsInputConnected(Expression, TEXT("Specular"));
				Scan.bWorldHeight |= IsInputConnected(Expression, TEXT("WorldHeight"));
				continue;
			}
			// 函数调用节点把函数挂在 MaterialFunction 属性上。套娃深度给个上限，防环
			if (Depth >= 8)
			{
				continue;
			}
			UMaterialFunctionInterface* Function =
				Cast<UMaterialFunctionInterface>(UALReflect::GetObjectProp(Expression, TEXT("MaterialFunction")));
			if (Function && !Visited.Contains(Function))
			{
				Visited.Add(Function);
				++Scan.FunctionsSearched;
				ScanForRvtOutput(UALLandscapeCompat::GetExpressions(Function), Scan, Visited, Depth + 1);
			}
		}
	}

	TSharedPtr<FJsonObject> DescribeMaterialRvtOutput(const UMaterialInterface* MaterialInterface)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("material"), MaterialInterface ? MaterialInterface->GetPathName() : FString());
		const UMaterial* Material = MaterialInterface ? MaterialInterface->GetMaterial() : nullptr;
		if (!Material)
		{
			Obj->SetBoolField(TEXT("found"), false);
			Obj->SetStringField(TEXT("searched"), TEXT("no_material"));
			return Obj;
		}

		FRvtOutputScan Scan;
		TSet<const UObject*> Visited;
		ScanForRvtOutput(UALLandscapeCompat::GetExpressions(Material), Scan, Visited, 0);

		Obj->SetBoolField(TEXT("found"), Scan.bFound);
		Obj->SetStringField(TEXT("searched"), TEXT("graph_and_functions"));
		Obj->SetNumberField(TEXT("functions_searched"), Scan.FunctionsSearched);
		TSharedPtr<FJsonObject> Pins = MakeShared<FJsonObject>();
		Pins->SetBoolField(TEXT("base_color"), Scan.bBaseColor);
		Pins->SetBoolField(TEXT("normal"), Scan.bNormal);
		Pins->SetBoolField(TEXT("roughness"), Scan.bRoughness);
		Pins->SetBoolField(TEXT("specular"), Scan.bSpecular);
		Pins->SetBoolField(TEXT("world_height"), Scan.bWorldHeight);
		Obj->SetObjectField(TEXT("pins"), Pins);
		return Obj;
	}

	/** `r.VirtualTextures` 是只读 cvar，启动时定死 —— 改了项目设置要重启编辑器才生效 */
	bool IsProjectVirtualTexturingEnabled()
	{
		const IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VirtualTextures"));
		return CVar && CVar->GetInt() != 0;
	}

	/** 一块地形上挂着的全部 RVT，以及它们各自有没有体积罩住地形 */
	TSharedPtr<FJsonObject> DescribeRvtState(ALandscape* Landscape, const TArray<ALandscapeProxy*>& Proxies,
		const FBox& LandBounds)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("material_output"), DescribeMaterialRvtOutput(Landscape->LandscapeMaterial));

		const TArray<ALandscapeProxy*> Owners = ComponentOwners(Proxies);
		TArray<URuntimeVirtualTexture*> Assigned;
		for (const ALandscapeProxy* Proxy : Proxies)
		{
			for (URuntimeVirtualTexture* Vt : Proxy->RuntimeVirtualTextures)
			{
				if (Vt)
				{
					Assigned.AddUnique(Vt);
				}
			}
		}

		TArray<TSharedPtr<FJsonValue>> AssignedJson;
		for (URuntimeVirtualTexture* Vt : Assigned)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("asset"), Vt->GetOutermost()->GetName());
			Entry->SetStringField(TEXT("material_type"), MaterialTypeName(Vt->GetMaterialType()));
			Entry->SetStringField(TEXT("kind"), RvtKindOf(Vt));
			Entry->SetBoolField(TEXT("on_landscape_actor"), Landscape->RuntimeVirtualTextures.Contains(Vt));
			int32 With = 0;
			for (const ALandscapeProxy* Owner : Owners)
			{
				With += Owner->RuntimeVirtualTextures.Contains(Vt) ? 1 : 0;
			}
			Entry->SetNumberField(TEXT("proxies_with"), With);
			Entry->SetNumberField(TEXT("proxies_total"), Owners.Num());

			TArray<TSharedPtr<FJsonValue>> VolumesJson;
			for (ARuntimeVirtualTextureVolume* Volume : VolumesFor(Landscape->GetWorld(), Vt))
			{
				VolumesJson.Add(MakeShared<FJsonValueObject>(DescribeVolume(Volume, LandBounds)));
			}
			Entry->SetArrayField(TEXT("volumes"), VolumesJson);
			AssignedJson.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Obj->SetArrayField(TEXT("assigned"), AssignedJson);
		return Obj;
	}

	/** 地形的全部状态，全部从引擎读回 */
	TSharedPtr<FJsonObject> DescribeLandscape(ALandscape* Landscape, bool bWithRvt)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("label"), LandscapeLabel(Landscape));
		Obj->SetStringField(TEXT("name"), Landscape->GetName());
		Obj->SetStringField(TEXT("path"), Landscape->GetPathName());
		Obj->SetObjectField(TEXT("location"), UAL_CommandUtils::MakeVectorJson(Landscape->GetActorLocation()));
		const FVector Scale = Landscape->GetActorScale3D();
		Obj->SetObjectField(TEXT("scale"), UAL_CommandUtils::MakeVectorJson(Scale));
		Obj->SetNumberField(TEXT("quad_size_m"), Scale.X / 100.0);
		Obj->SetNumberField(TEXT("heightmap_range_m"), HeightRangeMetersAtScale100 * Scale.Z / 100.0);

		Obj->SetNumberField(TEXT("quads_per_section"), Landscape->SubsectionSizeQuads);
		Obj->SetNumberField(TEXT("sections_per_component"), Landscape->NumSubsections);
		Obj->SetNumberField(TEXT("component_size_quads"), Landscape->ComponentSizeQuads);

		if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
		{
			int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
			if (Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY) && Landscape->ComponentSizeQuads > 0)
			{
				// 已加载部分的范围。World Partition 下没加载的格子不在里面 —— 字段名里说清楚
				TSharedPtr<FJsonObject> Extent = MakeShared<FJsonObject>();
				Extent->SetObjectField(TEXT("resolution"), IntPointJson(FIntPoint(MaxX - MinX + 1, MaxY - MinY + 1)));
				Extent->SetObjectField(TEXT("component_count"), IntPointJson(FIntPoint(
					(MaxX - MinX) / Landscape->ComponentSizeQuads, (MaxY - MinY) / Landscape->ComponentSizeQuads)));
				Obj->SetObjectField(TEXT("loaded_extent"), Extent);
			}
		}

		const TArray<ALandscapeProxy*> Proxies = CollectProxies(Landscape);
		int32 StreamingProxies = 0;
		for (const ALandscapeProxy* Proxy : Proxies)
		{
			StreamingProxies += Proxy->IsA<ALandscapeStreamingProxy>() ? 1 : 0;
		}
		Obj->SetNumberField(TEXT("streaming_proxies_loaded"), StreamingProxies);

		const FBox Bounds = LandscapeWorldBounds(Landscape, Proxies);
		if (Bounds.IsValid)
		{
			Obj->SetObjectField(TEXT("bounds"), BoxJson(Bounds));
			const FVector SizeM = Bounds.GetSize() / 100.0;
			Obj->SetObjectField(TEXT("size_m"), UAL_CommandUtils::MakeVectorJson(SizeM));
		}

		Obj->SetStringField(TEXT("material"),
			Landscape->LandscapeMaterial ? Landscape->LandscapeMaterial->GetPathName() : FString());

		if (bWithRvt)
		{
			Obj->SetObjectField(TEXT("rvt"), DescribeRvtState(Landscape, Proxies, Bounds));
		}
		return Obj;
	}

	/** 请求里的 `color` / `height`：`true` 或 `{ asset_path }` */
	struct FRvtRequest
	{
		bool bWanted = false;
		FString AssetPath;
	};

	FRvtRequest ReadRvtRequest(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		FRvtRequest Request;
		if (!Obj.IsValid())
		{
			return Request;
		}
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Field);
		if (!Value.IsValid() || Value->IsNull())
		{
			return Request;
		}
		bool bFlag = false;
		if (Value->TryGetBool(bFlag))
		{
			Request.bWanted = bFlag;
			return Request;
		}
		const TSharedPtr<FJsonObject>* Sub = nullptr;
		if (Value->TryGetObject(Sub) && Sub && Sub->IsValid())
		{
			Request.bWanted = true;
			(*Sub)->TryGetStringField(TEXT("asset_path"), Request.AssetPath);
		}
		return Request;
	}

	FString NormalizePackagePath(const FString& In)
	{
		FString Path = UAL_CommandUtils::NormalizeAssetPath(In);
		// 允许传对象路径（/Game/X/RVT.RVT），统一成包路径。只认最后一个 / 之后的点 ——
		// 目录名里本来就可能有点（/Game/Env_v1.2/RVT）
		int32 Dot = INDEX_NONE;
		int32 Slash = INDEX_NONE;
		Path.FindLastChar(TEXT('/'), Slash);
		if (Path.FindLastChar(TEXT('.'), Dot) && Dot > Slash)
		{
			Path = Path.Left(Dot);
		}
		while (Path.EndsWith(TEXT("/")))
		{
			Path.LeftChopInline(1);
		}
		return Path;
	}

	/**
	 * 默认 RVT 资产名、体积标签里代表这块地形的那一段。
	 *
	 * 标签里的 ASCII 字母数字留着好认，再接 LandscapeGuid 的前 8 位保证唯一 ——
	 * 只用标签的话，两块都叫「Landscape」的地形、或者两个中文名（非 ASCII 全被换掉）
	 * 会撞到同一个资产路径，第二块就悄悄复用了第一块的 RVT 和体积。
	 */
	FString LandscapeAssetToken(const ALandscape* Landscape)
	{
		FString Label;
		for (const TCHAR Ch : LandscapeLabel(Landscape))
		{
			if (FChar::IsAlnum(Ch) && Ch < 128)
			{
				Label.AppendChar(Ch);
			}
		}
		const FString Guid = Landscape->GetLandscapeGuid().ToString(EGuidFormats::Digits).Left(8);
		return Label.IsEmpty() ? FString::Printf(TEXT("Landscape_%s"), *Guid)
							   : FString::Printf(TEXT("%s_%s"), *Label, *Guid);
	}

	/**
	 * 配一种 RVT：资产 → 挂到地形和每一块代理 → 体积罩住整块地形。
	 *
	 * 和编辑器「创建 RVT 体积」按钮做的是同一件事，多做两步：
	 *   - **逐块代理核对**。先改主地形；还没跟上的代理，5.3 起让它从主地形继承，
	 *     更老的版本没有继承这一说，直接写。写完逐块读回确认。
	 *   - **边界自己核对**。5.0–5.4 的 `RuntimeVirtualTexture::SetBounds` 只统计已加载的块，
	 *     World Partition 下没加载的区域会被漏掉、体积偏小、边缘发黑（5.5 起才修）。
	 *     设完之后拿地形的完整范围比一遍，不够就明说。
	 *
	 * 调用方负责开事务。失败时回 `ok:false` 加原因，已经做成的部分照实报。
	 */
	TSharedPtr<FJsonObject> SetupOneRvt(ALandscape* Landscape, const FRvtKind& Kind, const FRvtRequest& Request,
		const FString& AssetFolder)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("kind"), Kind.Key);
		UWorld* World = Landscape->GetWorld();

		auto Fail = [&Result](const FString& Why)
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), Why);
			return Result;
		};

		// 1. 资产
		const FString PackagePath = Request.AssetPath.IsEmpty()
			? FString::Printf(TEXT("%s/RVT_%s_%s"), *NormalizePackagePath(AssetFolder),
				*LandscapeAssetToken(Landscape), Kind.Suffix)
			: NormalizePackagePath(Request.AssetPath);
		if (!FPackageName::IsValidLongPackageName(PackagePath))
		{
			return Fail(FString::Printf(TEXT("'%s' is not a valid asset path (expected something like /Game/Landscape/RVT_Color)."), *PackagePath));
		}
		Result->SetStringField(TEXT("asset_path"), PackagePath);

		const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
		const FString ObjectPath = PackagePath + TEXT(".") + AssetName;

		UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath);
		if (!Existing && FPackageName::DoesPackageExist(PackagePath))
		{
			Existing = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}

		URuntimeVirtualTexture* Vt = nullptr;
		bool bCreated = false;
		if (Existing)
		{
			Vt = Cast<URuntimeVirtualTexture>(Existing);
			if (!Vt)
			{
				return Fail(FString::Printf(TEXT("An asset of class %s already exists at %s; it is not a runtime virtual texture. Nothing was changed for the %s RVT."),
					*Existing->GetClass()->GetName(), *PackagePath, Kind.Key));
			}
			if (RvtKindOf(Vt) != Kind.Key)
			{
				return Fail(FString::Printf(TEXT("%s is a %s RVT (material type %s), not a %s RVT. Pick another asset_path. Nothing was changed for the %s RVT."),
					*PackagePath, *RvtKindOf(Vt), *MaterialTypeName(Vt->GetMaterialType()), Kind.Key, Kind.Key));
			}
		}
		else
		{
			UPackage* Package = CreatePackage(*PackagePath);
			if (!Package)
			{
				return Fail(FString::Printf(TEXT("Could not create package %s."), *PackagePath));
			}
			Vt = NewObject<URuntimeVirtualTexture>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
			if (!Vt)
			{
				return Fail(FString::Printf(TEXT("Could not create the runtime virtual texture %s."), *PackagePath));
			}
			if (!SetRvtMaterialType(Vt, Kind.Type))
			{
				UE_LOG(LogUALLandscape, Warning, TEXT("Could not set MaterialType on %s"), *PackagePath);
			}
			Vt->PostEditChange();
			FAssetRegistryModule::AssetCreated(Vt);
			Vt->MarkPackageDirty();
			bCreated = true;
		}
		Result->SetBoolField(TEXT("asset_created"), bCreated);
		// 读回，不回显请求 —— 类型没写进去的话这里会露出来
		Result->SetStringField(TEXT("material_type"), MaterialTypeName(Vt->GetMaterialType()));
		if (RvtKindOf(Vt) != Kind.Key)
		{
			return Fail(FString::Printf(TEXT("Created %s but its material type reads back as %s, not the %s type. The asset is left in place; fix its Material Type by hand."),
				*PackagePath, *MaterialTypeName(Vt->GetMaterialType()), Kind.Key));
		}

		// 2. 挂到主地形和每一块代理
		FProperty* RvtProp = FindFProperty<FProperty>(ALandscapeProxy::StaticClass(), TEXT("RuntimeVirtualTextures"));
		const TArray<ALandscapeProxy*> Proxies = CollectProxies(Landscape);
		// 主地形排第一个：5.5 起它的改动会带着代理一起变，后面的代理就只剩核对
		TArray<ALandscapeProxy*> Ordered;
		Ordered.Add(Landscape);
		for (ALandscapeProxy* Proxy : Proxies)
		{
			if (Proxy != Landscape)
			{
				Ordered.Add(Proxy);
			}
		}
		for (ALandscapeProxy* Proxy : Ordered)
		{
			if (Proxy->RuntimeVirtualTextures.Contains(Vt))
			{
				continue;
			}
			// 代理优先走「从主地形继承」：5.3 起这个属性可被代理覆盖，直接改代理
			// 会让它和主地形不一致，之后主地形上再改 RVT 就不再传到它身上
			if (Proxy != Landscape && UALLandscapeCompat::InheritSharedProperties(Proxy, Landscape))
			{
				continue;
			}
			Proxy->PreEditChange(RvtProp);
			Proxy->RuntimeVirtualTextures.Add(Vt);
			FPropertyChangedEvent Event(RvtProp, EPropertyChangeType::ValueSet);
			Proxy->PostEditChangeProperty(Event);
		}

		const TArray<ALandscapeProxy*> Owners = ComponentOwners(Proxies);
		int32 With = 0;
		for (const ALandscapeProxy* Owner : Owners)
		{
			With += Owner->RuntimeVirtualTextures.Contains(Vt) ? 1 : 0;
		}
		Result->SetBoolField(TEXT("on_landscape_actor"), Landscape->RuntimeVirtualTextures.Contains(Vt));
		Result->SetNumberField(TEXT("proxies_with"), With);
		Result->SetNumberField(TEXT("proxies_total"), Owners.Num());

		// 3. 体积：有就复用（只重算边界），没有就生成
		TArray<ARuntimeVirtualTextureVolume*> Volumes = VolumesFor(World, Vt);
		bool bVolumeCreated = false;
		// 只复用对齐到这块地形（或还没对齐任何东西）的体积。同一个 RVT 已经给别的地形用着时
		// 不能把它的体积拽过来缩到这块地形上 —— 那块地形的 RVT 区域会直接变黑
		ARuntimeVirtualTextureVolume* Volume = nullptr;
		AActor* OtherOwner = nullptr;
		for (ARuntimeVirtualTextureVolume* Candidate : Volumes)
		{
			AActor* Aligned = Candidate && Candidate->VirtualTextureComponent
				? Candidate->VirtualTextureComponent->GetBoundsAlignActor().Get()
				: nullptr;
			if (Aligned == Landscape)
			{
				Volume = Candidate;
				break;
			}
			if (!Aligned && !Volume)
			{
				Volume = Candidate;
			}
			else if (Aligned && !OtherOwner)
			{
				OtherOwner = Aligned;
			}
		}
		if (!Volume && OtherOwner)
		{
			return Fail(FString::Printf(TEXT("The %s RVT asset %s already has a volume aligned to %s. Sharing one RVT between two landscapes would move that volume and blank the other landscape's RVT; give this landscape its own RVT asset instead."),
				Kind.Key, *Vt->GetPathName(), *OtherOwner->GetActorLabel()));
		}
		if (!Volume)
		{
			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transactional;
			Volume = World->SpawnActor<ARuntimeVirtualTextureVolume>(SpawnParams);
			if (!Volume || !Volume->VirtualTextureComponent)
			{
				return Fail(FString::Printf(TEXT("The %s RVT asset is ready and assigned to the landscape, but spawning its RuntimeVirtualTextureVolume failed. Without a volume the RVT stays empty."),
					Kind.Key));
			}
			Volume->SetActorLabel(FString::Printf(TEXT("RVTVolume_%s_%s"),
				*LandscapeAssetToken(Landscape), Kind.Suffix));
			bVolumeCreated = true;
		}

		URuntimeVirtualTextureComponent* Component = Volume->VirtualTextureComponent;
		Volume->Modify();
		Component->Modify();
		Component->SetVirtualTexture(Vt);
		Component->SetBoundsAlignActor(Landscape);
		RuntimeVirtualTexture::SetBounds(Component);

		const FBox LandBounds = LandscapeWorldBounds(Landscape, CollectProxies(Landscape));
		TSharedPtr<FJsonObject> VolumeJson = DescribeVolume(Volume, LandBounds);
		VolumeJson->SetBoolField(TEXT("created"), bVolumeCreated);
		Result->SetObjectField(TEXT("volume"), VolumeJson);

		bool bCoversXY = false;
		bool bCoversZ = false;
		VolumeJson->TryGetBoolField(TEXT("covers_xy"), bCoversXY);
		VolumeJson->TryGetBoolField(TEXT("covers_z"), bCoversZ);

		TArray<FString> Problems;
		if (With < Owners.Num())
		{
			Problems.Add(FString::Printf(TEXT("only %d of %d landscape proxies draw into it"), With, Owners.Num()));
		}
		if (!bCoversXY)
		{
			Problems.Add(UALLandscapeCompat::bSetBoundsSkipsUnloadedCells
				? TEXT("the volume does not cover the whole landscape horizontally (this engine version only measures loaded World Partition cells: load the whole landscape and run this again)")
				: TEXT("the volume does not cover the whole landscape horizontally (check the volume's rotation and Expand Bounds, or delete the volume and run this again)"));
		}
		if (Kind.Type == ERuntimeVirtualTextureMaterialType::WorldHeight && !bCoversZ)
		{
			Problems.Add(TEXT("the volume does not cover the landscape's full height range, so heights outside it clamp"));
		}
		if (Problems.Num() > 0)
		{
			return Fail(FString::Printf(TEXT("The %s RVT was set up but %s."), Kind.Key, *FString::Join(Problems, TEXT("; "))));
		}

		Result->SetBoolField(TEXT("ok"), true);
		return Result;
	}

	/** 按请求配颜色 / 高度两种，返回逐条结果和失败数 */
	TArray<TSharedPtr<FJsonValue>> SetupRequestedRvts(ALandscape* Landscape, const TSharedPtr<FJsonObject>& RvtPayload,
		int32& OutFailed)
	{
		TArray<TSharedPtr<FJsonValue>> Results;
		FString AssetFolder = TEXT("/Game/Landscape/RVT");
		RvtPayload->TryGetStringField(TEXT("asset_folder"), AssetFolder);

		for (const FRvtKind* Kind : {&ColorRvt, &HeightRvt})
		{
			const FRvtRequest Request = ReadRvtRequest(RvtPayload, Kind->Key);
			if (!Request.bWanted)
			{
				continue;
			}
			TSharedPtr<FJsonObject> One = SetupOneRvt(Landscape, *Kind, Request, AssetFolder);
			bool bOk = false;
			One->TryGetBoolField(TEXT("ok"), bOk);
			OutFailed += bOk ? 0 : 1;
			Results.Add(MakeShared<FJsonValueObject>(One));
		}
		return Results;
	}

	bool WantsAnyRvt(const TSharedPtr<FJsonObject>& RvtPayload)
	{
		return ReadRvtRequest(RvtPayload, ColorRvt.Key).bWanted || ReadRvtRequest(RvtPayload, HeightRvt.Key).bWanted;
	}

	void AddLevelFacts(UWorld* World, const TSharedPtr<FJsonObject>& Data)
	{
		const ULandscapeSubsystem* Subsystem = World->GetSubsystem<ULandscapeSubsystem>();
		Data->SetBoolField(TEXT("world_partition"), Subsystem && Subsystem->IsGridBased());
		Data->SetStringField(TEXT("level_package"), World->GetOutermost()->GetName());
		Data->SetBoolField(TEXT("project_virtual_texturing"), IsProjectVirtualTexturingEnabled());
	}
}

void FUAL_LandscapeCommands::RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("landscape.create"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_Create(Payload, RequestId);
	});
	CommandMap.Add(TEXT("landscape.setup_rvt"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetupRvt(Payload, RequestId);
	});
	CommandMap.Add(TEXT("landscape.list"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_List(Payload, RequestId);
	});
}

void FUAL_LandscapeCommands::Handle_Create(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (UAL_CommandUtils::RefuseDuringPlay(RequestId, TEXT("新建地形"), TEXT("creating a landscape")))
	{
		return;
	}
	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World || !World->GetCurrentLevel())
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("No editor world is open."));
		return;
	}
	if (!World->GetCurrentLevel()->bIsVisible)
	{
		// 编辑器自己的按钮在这种情况下直接不动
		UAL_CommandUtils::SendError(RequestId, 409,
			TEXT("The current level is hidden. Make it visible (or make a visible level current) first; nothing was created."));
		return;
	}

	// ---- 参数。凡是可能失败的检查都在动关卡之前做完 ----
	double QuadSizeM = 1.0;
	Payload->TryGetNumberField(TEXT("quad_size_m"), QuadSizeM);
	double HeightRangeM = HeightRangeMetersAtScale100;
	Payload->TryGetNumberField(TEXT("height_range_m"), HeightRangeM);
	if (QuadSizeM <= 0.0 || HeightRangeM <= 0.0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("quad_size_m and height_range_m must be positive."));
		return;
	}
	int32 GridSize = 2; // 编辑器新建面板的默认值（ULandscapeEditorObject::WorldPartitionGridSize）
	Payload->TryGetNumberField(TEXT("wp_grid_size"), GridSize);
	GridSize = FMath::Clamp(GridSize, 1, 16);

	FString MaterialPath;
	UMaterialInterface* Material = nullptr;
	if (Payload->TryGetStringField(TEXT("material"), MaterialPath) && !MaterialPath.IsEmpty())
	{
		// 统一成对象路径再加载一次。直接拿包路径（/Game/M_Ground）去 LoadObject 必然先失败，
		// 还会往输出日志里写一条 Failed to find object
		const FString Package = NormalizePackagePath(MaterialPath);
		Material = LoadObject<UMaterialInterface>(nullptr,
			*(Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package)));
		if (!Material)
		{
			UAL_CommandUtils::SendError(RequestId, 404,
				FString::Printf(TEXT("Material not found: %s. Nothing was created."), *MaterialPath));
			return;
		}
	}

	const TSharedPtr<FJsonObject>* RvtObj = nullptr;
	TSharedPtr<FJsonObject> RvtPayload = Payload->TryGetObjectField(TEXT("rvt"), RvtObj) && RvtObj ? *RvtObj : nullptr;

	FString HeightmapPath;
	Payload->TryGetStringField(TEXT("heightmap_path"), HeightmapPath);
	HeightmapPath = HeightmapPath.TrimStartAndEnd();

	FLandscapeLayout Layout;
	TArray<uint16> Heights;
	TSharedPtr<FJsonObject> HeightmapJson;

	if (!HeightmapPath.IsEmpty())
	{
		if (!FPaths::FileExists(HeightmapPath))
		{
			UAL_CommandUtils::SendError(RequestId, 404,
				FString::Printf(TEXT("Heightmap file not found: %s. Nothing was created."), *HeightmapPath));
			return;
		}
		FLandscapeImportDescriptor Descriptor;
		FText Message;
		ELandscapeImportResult Result = FLandscapeImportHelper::GetHeightmapImportDescriptor(
			HeightmapPath, /*bSingleFile*/ true, /*bFlipYAxis*/ false, Descriptor, Message);
		if (Result == ELandscapeImportResult::Error || Descriptor.ImportResolutions.Num() == 0)
		{
			UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
				TEXT("The engine could not read the heightmap %s: %s Supported: 16-bit grayscale PNG, .r16, .raw. Nothing was created."),
				*HeightmapPath, *Message.ToString()));
			return;
		}
		TArray<FString> ImportMessages;
		if (!Message.IsEmpty())
		{
			ImportMessages.Add(Message.ToString());
		}

		// 单文件只有一个候选分辨率；.raw/.r16 没有文件头，可能有好几个，取第一个并报出来
		const FLandscapeImportResolution Source = Descriptor.ImportResolutions[0];
		TArray<uint16> Raw;
		Result = FLandscapeImportHelper::GetHeightmapImportData(Descriptor, 0, Raw, Message);
		if (Result == ELandscapeImportResult::Error || Raw.Num() != (int32)(Source.Width * Source.Height))
		{
			UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
				TEXT("The engine could not decode the heightmap %s: %s Nothing was created."), *HeightmapPath, *Message.ToString()));
			return;
		}
		if (!Message.IsEmpty())
		{
			ImportMessages.AddUnique(Message.ToString());
		}
		if ((int32)Source.Width > MaxResolutionPerAxis || (int32)Source.Height > MaxResolutionPerAxis)
		{
			UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
				TEXT("The heightmap is %ux%u; the largest supported is %dx%d. Downscale it first. Nothing was created."),
				Source.Width, Source.Height, MaxResolutionPerAxis, MaxResolutionPerAxis));
			return;
		}

		Layout = PickLayout((int32)Source.Width - 1, (int32)Source.Height - 1);
		const FIntPoint Res = Layout.Resolution();
		const bool bResample = Res.X != (int32)Source.Width || Res.Y != (int32)Source.Height;
		if (bResample)
		{
			FLandscapeImportHelper::TransformHeightmapImportData(Raw, Heights, Source,
				FLandscapeImportResolution(Res.X, Res.Y), ELandscapeImportTransformType::Resample);
		}
		else
		{
			Heights = MoveTemp(Raw);
		}

		HeightmapJson = MakeShared<FJsonObject>();
		HeightmapJson->SetStringField(TEXT("path"), HeightmapPath);
		HeightmapJson->SetObjectField(TEXT("source_resolution"), IntPointJson(FIntPoint((int32)Source.Width, (int32)Source.Height)));
		HeightmapJson->SetBoolField(TEXT("resampled"), bResample);
		HeightmapJson->SetNumberField(TEXT("candidate_resolutions"), Descriptor.ImportResolutions.Num());
		if (ImportMessages.Num() > 0)
		{
			HeightmapJson->SetStringField(TEXT("engine_message"), FString::Join(ImportMessages, TEXT(" ")));
		}
	}
	else
	{
		double SizeXM = 0.0;
		if (!Payload->TryGetNumberField(TEXT("size_x_m"), SizeXM) || SizeXM <= 0.0)
		{
			UAL_CommandUtils::SendError(RequestId, 400,
				TEXT("Give either heightmap_path or size_x_m (edge length in meters)."));
			return;
		}
		double SizeYM = SizeXM;
		Payload->TryGetNumberField(TEXT("size_y_m"), SizeYM);
		Layout = PickLayout(FMath::RoundToInt(SizeXM / QuadSizeM), FMath::RoundToInt(SizeYM / QuadSizeM));
		const FIntPoint Res = Layout.Resolution();
		Heights.Init(FlatHeightValue, Res.X * Res.Y);
	}

	const FIntPoint Resolution = Layout.Resolution();
	const FVector Location = UAL_CommandUtils::ReadVector(Payload, TEXT("location"), FVector::ZeroVector);
	const FVector Scale(QuadSizeM * 100.0, QuadSizeM * 100.0, HeightRangeM / HeightRangeMetersAtScale100 * 100.0);

	FString Label;
	Payload->TryGetStringField(TEXT("label"), Label);
	Label = Label.TrimStartAndEnd();

	// ---- 建。以下照抄编辑器 OnCreateButtonClicked ----
	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALLandscape", "CreateLandscape", "Create Landscape"));

	// 地形的原点在左下角，编辑器把它往回挪半个尺寸，让给出的位置落在中心
	const int32 QuadsPerComponent = Layout.QuadsPerComponent();
	const FVector Offset(
		-Layout.ComponentCount.X * QuadsPerComponent / 2.0 * Scale.X,
		-Layout.ComponentCount.Y * QuadsPerComponent / 2.0 * Scale.Y,
		0.0);

	ALandscape* Landscape = World->SpawnActor<ALandscape>(Location + Offset, FRotator::ZeroRotator);
	if (!Landscape)
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("SpawnActor failed for Landscape; nothing was created."));
		return;
	}
	UALLandscapeCompat::EnableEditLayers(Landscape);
	Landscape->LandscapeMaterial = Material;
	Landscape->SetActorRelativeScale3D(Scale);
	// 编辑器原样：按尺寸定一个不会把 Lightmass 撑爆的光照 LOD
	Landscape->StaticLightingLOD = FMath::DivideAndRoundUp(
		FMath::CeilLogTwo((Resolution.X * Resolution.Y) / (2048 * 2048) + 1), (uint32)2);

	TMap<FGuid, TArray<uint16>> HeightDataPerLayer;
	HeightDataPerLayer.Add(FGuid(), MoveTemp(Heights));
	TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayersPerLayer;
	MaterialLayersPerLayer.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());

	UALLandscapeCompat::Import(Landscape, Resolution.X, Resolution.Y, Layout.SectionsPerComponent,
		Layout.QuadsPerSection, HeightDataPerLayer, *HeightmapPath, MaterialLayersPerLayer);

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!Info)
	{
		// Import 没建出 LandscapeInfo 就是没建成。拆掉空壳，关卡回到原样
		World->EditorDestroyActor(Landscape, true);
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 500,
			TEXT("The engine did not finish importing the landscape; the empty actor was removed and the level is unchanged."));
		return;
	}
	// 和编辑器一样取不重名的标签 —— 两块都叫 Landscape 时按名字就找不准了
	FActorLabelUtilities::SetActorLabelUnique(Landscape, Label.IsEmpty() ? FString(TEXT("Landscape")) : Label);
	Info->UpdateLayerInfoMap(Landscape);

	ULandscapeSubsystem* Subsystem = World->GetSubsystem<ULandscapeSubsystem>();
	const bool bWorldPartition = Subsystem && Subsystem->IsGridBased();
	if (bWorldPartition)
	{
		// World Partition 下切成一块块流送代理。不切的话地形还是一整块，
		// 流送和 PCG 采样都会出问题
		Subsystem->ChangeGridSize(Info, (uint32)GridSize);
	}

	int32 RvtFailed = 0;
	TArray<TSharedPtr<FJsonValue>> RvtResults;
	if (RvtPayload.IsValid() && WantsAnyRvt(RvtPayload))
	{
		RvtResults = SetupRequestedRvts(Landscape, RvtPayload, RvtFailed);
	}

	// ---- 回执：全部从引擎读回 ----
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	AddLevelFacts(World, Data);
	Data->SetObjectField(TEXT("landscape"), DescribeLandscape(Landscape, /*bWithRvt*/ RvtResults.Num() > 0));
	Data->SetBoolField(TEXT("undoable"), true);
	if (bWorldPartition)
	{
		Data->SetNumberField(TEXT("wp_grid_size"), GridSize);
	}
	if (HeightmapJson.IsValid())
	{
		HeightmapJson->SetObjectField(TEXT("resolution"), IntPointJson(Resolution));
		Data->SetObjectField(TEXT("heightmap"), HeightmapJson);
	}

	// 请求值只作为「你要的是什么」原样带回，和上面读回的实际值分开放
	TSharedPtr<FJsonObject> Requested = MakeShared<FJsonObject>();
	double RequestedX = 0.0, RequestedY = 0.0;
	if (Payload->TryGetNumberField(TEXT("size_x_m"), RequestedX))
	{
		Requested->SetNumberField(TEXT("size_x_m"), RequestedX);
		Requested->SetNumberField(TEXT("size_y_m"), Payload->TryGetNumberField(TEXT("size_y_m"), RequestedY) ? RequestedY : RequestedX);
	}
	Requested->SetNumberField(TEXT("quad_size_m"), QuadSizeM);
	Requested->SetNumberField(TEXT("height_range_m"), HeightRangeM);
	if (!MaterialPath.IsEmpty())
	{
		Requested->SetStringField(TEXT("material"), MaterialPath);
	}
	Data->SetObjectField(TEXT("requested"), Requested);

	if (RvtResults.Num() > 0)
	{
		Data->SetArrayField(TEXT("rvt_results"), RvtResults);
	}
	Data->SetNumberField(TEXT("failed_count"), RvtFailed);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_LandscapeCommands::Handle_SetupRvt(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	if (UAL_CommandUtils::RefuseDuringPlay(RequestId, TEXT("配置地形 RVT"), TEXT("setting up landscape RVT")))
	{
		return;
	}
	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("No editor world is open."));
		return;
	}
	if (!WantsAnyRvt(Payload))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Ask for at least one of color / height."));
		return;
	}

	FString Identifier;
	Payload->TryGetStringField(TEXT("landscape"), Identifier);
	FString FindError;
	ALandscape* Landscape = FindLandscape(World, Identifier.TrimStartAndEnd(), FindError);
	if (!Landscape)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FindError + TEXT(" Nothing was changed."));
		return;
	}

	int32 Failed = 0;
	TArray<TSharedPtr<FJsonValue>> Results;
	{
		FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALLandscape", "SetupLandscapeRvt", "Set Up Landscape RVT"));
		Results = SetupRequestedRvts(Landscape, Payload, Failed);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	AddLevelFacts(World, Data);
	Data->SetArrayField(TEXT("rvt_results"), Results);
	Data->SetNumberField(TEXT("failed_count"), Failed);
	Data->SetObjectField(TEXT("landscape"), DescribeLandscape(Landscape, /*bWithRvt*/ true));
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_LandscapeCommands::Handle_List(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("No editor world is open."));
		return;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	AddLevelFacts(World, Data);
	TArray<TSharedPtr<FJsonValue>> Landscapes;
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		Landscapes.Add(MakeShared<FJsonValueObject>(DescribeLandscape(*It, /*bWithRvt*/ true)));
	}
	Data->SetArrayField(TEXT("landscapes"), Landscapes);
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}
