#include "UAL_MaterialCommands.h"
#include "UAL_VersionCompat.h"
#include "UAL_CommandUtils.h"
#include "Utils/UAL_PropertyPath.h"
#include "Misc/PackageName.h"
#include "Utils/UAL_PBRMaterialHelper.h"

#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Factories/MaterialFactoryNew.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionSine.h"
#include "Materials/MaterialExpressionCosine.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionAbs.h"
#include "Materials/MaterialExpressionFrac.h"
#include "Materials/MaterialExpressionFloor.h"
#include "Materials/MaterialExpressionCeil.h"
#include "Materials/MaterialExpressionMin.h"
#include "Materials/MaterialExpressionMax.h"
#include "Materials/MaterialExpressionSquareRoot.h"
#include "Materials/MaterialExpressionTextureObject.h"
#include "Materials/MaterialExpressionDotProduct.h"
#include "Materials/MaterialExpressionCrossProduct.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionBreakMaterialAttributes.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionPixelNormalWS.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionNoise.h"
// 命名重定向：图里一根线断成两半，读图的人必须能把两半配回去
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Engine/Texture2D.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Components/MeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
// 材质参数集合（material.parameter_collection）—— 一个开关控制全场景材质
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Factories/MaterialParameterCollectionFactoryNew.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "UObject/SavePackage.h"
#include "PropertyEditorModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor.h"
#include "AssetToolsModule.h"
#include "UAL_ScopedTransaction.h"
#include "MaterialEditorUtilities.h"
// 材质编辑器开着时，要改的是它正在编的那份副本而不是资产 —— 见 UAL_ResolveLiveMaterial
#include "IMaterialEditor.h"
// 替用户按一次「保存」要用它把 UI 命令查出来，见 UAL_ApplyMaterialEditorToAsset
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandList.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialShared.h"
#include "ShaderCompiler.h"

/*
 * Substrate 从 5.4 起才有（5.3 叫 Strata，实验性，类名和引脚都不一样，不接）。
 * 5.4 的头文件在 Classes/，5.5 起挪进 Public/，两边的 include 路径同样写
 * "Materials/MaterialExpressionSubstrate.h"，Engine 模块两个目录都导出。
 * 所有 Substrate 节点类都是 MinimalAPI，StaticClass() 链得上。
 */
#define UAL_WITH_SUBSTRATE (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4))
#if UAL_WITH_SUBSTRATE
#include "Materials/MaterialExpressionSubstrate.h"
#include "RenderUtils.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUALMaterial, Log, All);

/**
 * MPC 里按名字找参数，自己扫数组。
 *
 * **不能用 `UMaterialParameterCollection::GetScalarParameterIndexByName`** ——
 * 那几个方法没有 `ENGINE_API`，编得过但链不上（LNK2019），而且是在
 * 链接阶段才炸，看不出跟哪一行有关。`ScalarParameters` / `VectorParameters`
 * 是公开 UPROPERTY，直接扫是稳的，九个引擎版本行为一致。
 *
 * 返回索引，找不到给 INDEX_NONE。
 */
static int32 UAL_FindScalarParam(const UMaterialParameterCollection* Collection, const FName& Name)
{
	if (!Collection)
	{
		return INDEX_NONE;
	}
	for (int32 i = 0; i < Collection->ScalarParameters.Num(); ++i)
	{
		if (Collection->ScalarParameters[i].ParameterName == Name)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

static int32 UAL_FindVectorParam(const UMaterialParameterCollection* Collection, const FName& Name)
{
	if (!Collection)
	{
		return INDEX_NONE;
	}
	for (int32 i = 0; i < Collection->VectorParameters.Num(); ++i)
	{
		if (Collection->VectorParameters[i].ParameterName == Name)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

namespace
{
	/**
	 * 材质值的规范形态。
	 *
	 * 调用方的 schema 是个 union：数字、布尔、字符串、`{r,g,b,a?}`、`{x,y,z?,w?}`、
	 * `{u_tiling,...}`、2~4 个数的数组都合法。先全部归一到这一个结构，
	 * 各类节点再从里面取自己要的那一份。
	 */
	struct FUALMaterialValue
	{
		bool bHasScalar = false;
		float Scalar = 0.f;

		bool bHasColor = false;
		FLinearColor Color = FLinearColor(0.f, 0.f, 0.f, 1.f);

		bool bHasString = false;
		FString String;

		bool bHasUv = false;
		float UTiling = 1.f;
		float VTiling = 1.f;
	};

	/** 把原始 JSON 值解析成 FUALMaterialValue。失败时 OutError 说明是哪一步不认。 */
	bool ParseMaterialValue(const TSharedPtr<FJsonValue>& Value, FUALMaterialValue& Out, FString& OutError)
	{
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			OutError = TEXT("value is missing or null");
			return false;
		}

		switch (Value->Type)
		{
		case EJson::Boolean:
		{
			// 布尔必须先于数字判。FJsonValueBoolean::TryGetNumber 会把 true 变成 1.0，
			// 谁先问谁拿走 —— 按数字处理会让开关类的值悄悄变成一个标量。
			const bool bValue = Value->AsBool();
			Out.bHasScalar = true;
			Out.Scalar = bValue ? 1.f : 0.f;
			return true;
		}

		case EJson::Number:
		{
			const float N = (float)Value->AsNumber();
			Out.bHasScalar = true;
			Out.Scalar = N;
			// 标量喂给向量节点时按灰度铺开。「把颜色设成 0.5」得到中灰，
			// 比直接拒掉更接近调用方的本意
			Out.bHasColor = true;
			Out.Color = FLinearColor(N, N, N, 1.f);
			return true;
		}

		case EJson::String:
			Out.bHasString = true;
			Out.String = Value->AsString();
			return true;

		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
			if (Items.Num() < 2 || Items.Num() > 4)
			{
				OutError = FString::Printf(TEXT("numeric array must have 2-4 elements, got %d"), Items.Num());
				return false;
			}

			float Components[4] = { 0.f, 0.f, 0.f, 1.f };
			for (int32 i = 0; i < Items.Num(); ++i)
			{
				double N = 0.0;
				if (!Items[i].IsValid() || !Items[i]->TryGetNumber(N))
				{
					OutError = FString::Printf(TEXT("array element %d is not a number"), i);
					return false;
				}
				Components[i] = (float)N;
			}

			Out.bHasColor = true;
			Out.Color = FLinearColor(Components[0], Components[1], Components[2], Components[3]);
			Out.bHasScalar = true;
			Out.Scalar = Components[0];
			if (Items.Num() == 2)
			{
				Out.bHasUv = true;
				Out.UTiling = Components[0];
				Out.VTiling = Components[1];
			}
			return true;
		}

		case EJson::Object:
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid())
			{
				OutError = TEXT("value object is empty");
				return false;
			}

			// 早期版本的插件只认 `{"value": ...}` 这一种包法。没有任何现役调用方
			// 会这么传，但这个插件是公开项目，可能有人照着旧源码接了 ——
			// 拆一层再往下走，代价是三行
			if (Obj->Values.Num() == 1 && Obj->Values.Contains(TEXT("value")))
			{
				return ParseMaterialValue(Obj->Values[TEXT("value")], Out, OutError);
			}

			bool bMatched = false;
			double Component = 0.0;

			// {r,g,b,a?}
			if (Obj->HasField(TEXT("r")) || Obj->HasField(TEXT("g")) || Obj->HasField(TEXT("b")))
			{
				Out.bHasColor = true;
				Out.Color = FLinearColor(0.f, 0.f, 0.f, 1.f);
				if (Obj->TryGetNumberField(TEXT("r"), Component)) Out.Color.R = (float)Component;
				if (Obj->TryGetNumberField(TEXT("g"), Component)) Out.Color.G = (float)Component;
				if (Obj->TryGetNumberField(TEXT("b"), Component)) Out.Color.B = (float)Component;
				if (Obj->TryGetNumberField(TEXT("a"), Component)) Out.Color.A = (float)Component;
				bMatched = true;
			}
			// {x,y,z?,w?} —— 这一支以前完全没人认：`{x:1,y:0,z:0}` 会掉进
			// r/g/b 的读取里，三个字段全缺，于是「设成红色」变成设成纯黑
			else if (Obj->HasField(TEXT("x")) || Obj->HasField(TEXT("y")))
			{
				Out.bHasColor = true;
				Out.Color = FLinearColor(0.f, 0.f, 0.f, 1.f);
				if (Obj->TryGetNumberField(TEXT("x"), Component)) Out.Color.R = (float)Component;
				if (Obj->TryGetNumberField(TEXT("y"), Component)) Out.Color.G = (float)Component;
				if (Obj->TryGetNumberField(TEXT("z"), Component)) Out.Color.B = (float)Component;
				if (Obj->TryGetNumberField(TEXT("w"), Component)) Out.Color.A = (float)Component;
				bMatched = true;
			}

			// {u_tiling, v_tiling, u_offset, v_offset}
			if (Obj->HasField(TEXT("u_tiling")) || Obj->HasField(TEXT("v_tiling")))
			{
				Out.bHasUv = true;
				if (Obj->TryGetNumberField(TEXT("u_tiling"), Component)) Out.UTiling = (float)Component;
				if (Obj->TryGetNumberField(TEXT("v_tiling"), Component)) Out.VTiling = (float)Component;
				bMatched = true;
			}

			if (!bMatched)
			{
				// 不用 Values.GetKeys()：5.8 的键类型是 UE::FSharedString，取不进 TArray<FString>
				TArray<FString> Keys;
				for (const auto& KeyPair : Obj->Values)
				{
					Keys.Add(UAL_JsonKey(KeyPair.Key));
				}
				OutError = FString::Printf(
					TEXT("unrecognized value object (keys: %s). Expected {r,g,b,a?}, {x,y,z?,w?} or {u_tiling,v_tiling}"),
					Keys.Num() ? *FString::Join(Keys, TEXT(", ")) : TEXT("none"));
				return false;
			}
			return true;
		}

		default:
			OutError = TEXT("unsupported value type");
			return false;
		}
	}

	/**
	 * 引脚类型的可读名字。
	 *
	 * 读图最关键的一列一直是缺的：连线只给了「谁接到谁」，没给「接的是什么类型」。
	 * 而复杂材质里最常见的报错就是类型不匹配 —— float3 接进只吃 float 的引脚，
	 * 图看起来完全正常，编译才炸，报错里也只有引脚名。没有类型这一列，
	 * 读图的人只能在上百根连线里挨个猜。
	 *
	 * `EMaterialValueType` 是位域，而且每个引擎版本都在往里加东西。
	 * 只翻译长期稳定的那几位，剩下的原样给出位值 —— 认不出来好过翻译错。
	 */
	/*
	 * 引脚类型一律经这两个函数问，不要直接调 GetInputType / GetOutputType。
	 *
	 * 5.6 起引擎把类型查询挪到了 GetInputValueType / GetOutputValueType，
	 * 新写的节点（Substrate 全族都是）**只覆写新函数**；旧的 GetInputType
	 * 基类实现写死回 MCT_Float。于是 5.6+ 上照旧调旧函数，Substrate 节点的
	 * Background / Foreground / Top 这些明明只收 Substrate 的输入全被报成
	 * float(any)，模型照着去接一个数值，编译才报错。新函数的默认实现
	 * 回落到旧函数，所以 5.6+ 调新函数对老节点也是对的。
	 */
	uint32 UAL_InputTypeOf(UMaterialExpression* Expression, int32 Index)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
		return static_cast<uint32>(Expression->GetInputValueType(Index));
#else
		return Expression->GetInputType(Index);
#endif
	}

	uint32 UAL_OutputTypeOf(UMaterialExpression* Expression, int32 Index)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
		return static_cast<uint32>(Expression->GetOutputValueType(Index));
#else
		return Expression->GetOutputType(Index);
#endif
	}

	FString UAL_MaterialTypeName(uint32 Type)
	{
		if (Type == 0)
		{
			return FString();
		}

		TArray<FString> Names;
		uint32 Remaining = Type;

		// 四个 float 全通 = 「什么数值都收」，逐位列出来只会刷屏
		if ((Remaining & MCT_Float) == MCT_Float)
		{
			Names.Add(TEXT("float(any)"));
			Remaining &= ~(uint32)MCT_Float;
		}

		struct FTypeBit { uint32 Bit; const TCHAR* Name; };
		static const FTypeBit Table[] = {
			{ MCT_Float1, TEXT("float") },
			{ MCT_Float2, TEXT("float2") },
			{ MCT_Float3, TEXT("float3") },
			{ MCT_Float4, TEXT("float4") },
			{ MCT_StaticBool, TEXT("bool") },
			{ MCT_MaterialAttributes, TEXT("MaterialAttributes") },
			{ MCT_Texture2D, TEXT("Texture2D") },
#if UAL_WITH_SUBSTRATE
			// Substrate 节点之间、以及接 FrontMaterial 的那根线都是这个类型。
			// 不列出来的话读回来是一个裸的 "0x20000"，没人认得
			{ MCT_Substrate, TEXT("Substrate") },
#endif
			{ MCT_Unknown, TEXT("unknown") }
		};
		for (const FTypeBit& Entry : Table)
		{
			if ((Remaining & Entry.Bit) != 0)
			{
				Names.Add(Entry.Name);
				Remaining &= ~Entry.Bit;
			}
		}

		if (Remaining != 0)
		{
			Names.Add(FString::Printf(TEXT("0x%x"), Remaining));
		}
		return FString::Join(Names, TEXT("|"));
	}

	/**
	 * 通道字符串（"RG" / "XY" / "RGBA"）→ 四个通道位。
	 *
	 * 写入（`ApplyValueToNode`）和建节点前的校验（`Handle_AddMaterialNode`）
	 * 必须用**同一套**判断。两边各写一份的下场已经见过：建节点那边只看
	 * 「是不是字符串」，于是 `value: ""` 和 `value: "Q"` 一路通过校验、建出节点、
	 * 再在写值时失败 —— 而写值失败不是致命错，回的是 200 加一个
	 * `initial_value_applied:false`，图里就留下一个四位全 0、编译恒为 0 的死节点。
	 */
	bool UAL_ParseChannelMask(const FString& Text, bool& bR, bool& bG, bool& bB, bool& bA, FString& OutError)
	{
		bR = bG = bB = bA = false;
		int32 LastOrder = -1;
		for (const TCHAR Channel : Text)
		{
			int32 Order = -1;
			switch (FChar::ToUpper(Channel))
			{
			case 'R': case 'X': bR = true; Order = 0; break;
			case 'G': case 'Y': bG = true; Order = 1; break;
			case 'B': case 'Z': bB = true; Order = 2; break;
			case 'A': case 'W': bA = true; Order = 3; break;
			default:
				OutError = FString::Printf(
					TEXT("'%c' is not a channel - use letters from RGBA or XYZW, e.g. \"RG\" or \"XY\""),
					Channel);
				return false;
			}

			/*
			 * 通道只能**升序、不重复**地写。
			 *
			 * `UMaterialExpressionComponentMask` 底下只有四个 bool，压根没有换序的能力。
			 * 收下 "GR" 就等于把「把 x 和 y 换过来」悄悄做成「取 RG」—— 而
			 * set_node_value 的描述还专门告诉模型「回读拼法变了是正常的」，
			 * 于是它连回读都不会起疑。"RR" 同理，会变成单通道 R。
			 * 做不到的事就得回 400，不能降级成一件差不多的事。
			 */
			if (Order <= LastOrder)
			{
				OutError = FString::Printf(
					TEXT("'%s' repeats or reorders channels. ComponentMask can only keep channels in "
						 "RGBA order (it has no swizzle) - write \"RG\", not \"GR\" or \"RR\""),
					*Text);
				return false;
			}
			LastOrder = Order;
		}
		if (!bR && !bG && !bB && !bA)
		{
			// 空串走到这里。四位全 0 的遮罩编译出来恒为 0，和没设一样 ——
			// 收下它等于把这个节点悄悄变回废的
			OutError = TEXT("needs at least one channel, e.g. \"R\", \"RG\", \"RGB\" (xyzw spelling also accepted)");
			return false;
		}
		return true;
	}

	/**
	 * 这个网格组件值不值得替调用方**自动**挑中。
	 *
	 * 判据是**排除法**，不是白名单。写成「只认静态网格和骨骼网格」很顺手，
	 * 但那样一刀切掉的是一整批身上真有材质槽的组件：
	 * `UProceduralMeshComponent`、`UCableComponent`、`UGroomComponent`、
	 * `UBaseDynamicMeshComponent`（Geometry Script 产出的全部走它）——
	 * 它们都直接继承 `UMeshComponent`，一个都不是那两类的子类。
	 * 拿白名单去筛，等于让这个工具比引擎窄，用 Geometry Script 搭的关卡
	 * 会从「刷得上」变成「一个都刷不上」。
	 *
	 * 真正要挡的只有一样：`UWidgetComponent` 也继承 `UMeshComponent`，
	 * 而且它的 `GetNumMaterials()` 至少回 1 —— 于是「只有一块血条」的 Actor
	 * 会被当成有网格，材质刷到 UI 面片上还报成功。挡它一个就够了。
	 *
	 * 类是按名字找的，不 include UMG 的头：这里只是想认出一个类，
	 * 犯不上为它在编译期绑一个模块。找不到就当没有这个类（不排除任何东西），
	 * 退化方向是安全的 —— 顶多回到「血条也能被自动挑中」，不会误伤别的。
	 */
	/**
	 * `material.apply` 跳过一个 Actor 的几种**种类**。
	 *
	 * 归类汇总按它们做 key，所以每一个都必须是常量 —— 拼进 Actor 名字或者
	 * 组件名单的话，key 就跟着 Actor 数一起涨，汇总从「五条」变成「两百条」。
	 * 收在这里 + `Skip` 收 `const TCHAR*`，等于把这条约束交给编译器：
	 * 传 `FString::Printf(...)` 进去是编译错误，不再靠注释提醒。
	 */
	namespace UALSkipKind
	{
		const TCHAR* const ActorInvalid = TEXT("actor invalid");
		const TCHAR* const NoComponentWithName = TEXT("no component with that name");
		const TCHAR* const NoPaintableMesh = TEXT("no paintable mesh component");
		const TCHAR* const NoMaterialSlots = TEXT("mesh has no material slots");
		const TCHAR* const SlotOutOfRange = TEXT("slot_index out of range");
		const TCHAR* const SlotNameNotFound = TEXT("slot_name not found");
		const TCHAR* const DuplicateSlot = TEXT("same slot named twice");
	}

	/**
	 * 名字列表：**先排序再截断**。
	 *
	 * 截出来的这几个名字是要给调用方回填 `component_name` 用的，所以不能是
	 * 哈希序里随便的五个 —— `AActor::OwnedComponents` 是 TSet，同一个 Actor
	 * 两次跑列出来的五个可能不是同一批，照着第一次的报告去重试，
	 * 点的名字第二次根本没出现过。
	 *
	 * 代价是要把全部名字都取出来才能排（`GetName()` 每次一次 FString 分配）。
	 * 认这个代价：走到这儿的只有「要报告」的那些 Actor，不是每个 Actor 都走。
	 */
	FString UAL_JoinNamesCapped(const TArray<UMeshComponent*>& Components,
		int32 Cap, const UMeshComponent* Exclude = nullptr)
	{
		TArray<FString> Names;
		for (UMeshComponent* Candidate : Components)
		{
			if (!Candidate || Candidate == Exclude) continue;
			Names.Add(Candidate->GetName());
		}
		if (Names.Num() == 0) return TEXT("none");

		Names.Sort([](const FString& A, const FString& B)
		{
			return A.Compare(B, ESearchCase::CaseSensitive) < 0;
		});
		if (Names.Num() <= Cap) return FString::Join(Names, TEXT(", "));

		const int32 Extra = Names.Num() - Cap;
		Names.SetNum(Cap);
		return FString::Printf(TEXT("%s, ...and %d more"), *FString::Join(Names, TEXT(", ")), Extra);
	}

	bool UAL_IsPaintableMesh(UMeshComponent* Candidate)
	{
		if (!Candidate) return false;

		/*
		 * **只缓存成功的那次查找。**
		 *
		 * `FindObject` 只找**已经加载**的类，而函数内 `static` 一辈子只初始化一次 ——
		 * 这一段原来写成 `static ... = FindObject(...)`，于是本次会话第一次
		 * material.apply 只要赶在 `/Script/UMG` 注册之前，null 就被钉死一整个进程：
		 * 从此这个过滤器对谁都返回 true，血条 Actor 照样被自动挑中刷上材质，
		 * 还报「已应用到 1/1」—— 正是这个过滤器要挡的那件事。
		 * 「找不到就当没有这个类」这个退化方向是对的，错在把一次性的失手变成永久的。
		 */
		static UClass* WidgetComponentClass = nullptr;
		if (!WidgetComponentClass)
		{
			WidgetComponentClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.WidgetComponent"));
		}
		return !WidgetComponentClass || !Candidate->IsA(WidgetComponentClass);
	}

	/**
	 * 输入引脚的名字，和 `UAL_OutputPinName` 是一对。
	 *
	 * 输出那边的 FName 陷阱修了，输入这边一直是四份手抄：`get_graph` 读节点、
	 * `get_graph` 读连线、`connect_pins`、`disconnect_pins` 各写一遍
	 * `GetInputName(i).ToString()` 加 `IsEmpty()` 兜底。而**空 FName 的
	 * `ToString()` 是 "None" 不是空串**，所以那四处的 `<%d>` 兜底一次都没走到过，
	 * 只有 `search_nodes` 那份额外判了 `== "None"`。
	 *
	 * 结果是同一个无名输入，`material_search_nodes` 报 `<0>`、
	 * `material_get_graph` 报 `None`，而连线时两个都匹配不上 ——
	 * 错误消息里列着 `<0>`，却又拒绝 `<0>`。收到这里来，四处共用一份。
	 */
	FString UAL_InputPinName(UMaterialExpression* Expression, int32 InputIndex)
	{
		if (!Expression)
		{
			return FString();
		}
		const FString Name = Expression->GetInputName(InputIndex).ToString();
		return (Name.IsEmpty() || Name == TEXT("None")) ? FString::Printf(TEXT("<%d>"), InputIndex) : Name;
	}

	/**
	 * 无名输出的名字，按 mask 位推出来。
	 *
	 * `Constant3Vector` 的四个输出**一个名字都没有**，只靠 mask 位区分：
	 * (R,G,B) / (R) / (G) / (B) —— 引擎自己也是照这几位给引脚上色的
	 * （`MaterialGraphNode.cpp` 里那串 `MaskR && !MaskG && ...`）。
	 *
	 * 推出来的词和 `TextureSample` 自带的真名是同一套（RGB / R / G / B / A / RGBA），
	 * 所以两类节点在调用方眼里写法一致，不用记「这个节点的输出有名字、那个没有」。
	 */
	FString UAL_MaskDerivedName(const FExpressionOutput& Output)
	{
		if (Output.Mask == 0)
		{
			return FString();
		}
		FString Name;
		if (Output.MaskR) Name += TEXT("R");
		if (Output.MaskG) Name += TEXT("G");
		if (Output.MaskB) Name += TEXT("B");
		if (Output.MaskA) Name += TEXT("A");
		return Name;
	}

	/**
	 * 输出引脚的名字。
	 *
	 * 这里以前用 `OutputName.ToString().IsEmpty()` 判有没有名字 —— 而空 FName 的
	 * `ToString()` 是 **"None"**，不是空串。于是 `<索引>` 那条兜底永远走不到，
	 * 读图报出来的是一串**全叫 "None" 的输出**：四个输出哪个是哪个，调用方
	 * 只能靠顺序猜。同一个 FName 陷阱 `ResolveOutputIndex` 里踩过一次并写了注释，
	 * 这里漏了。
	 *
	 * 纯运算节点（Add / Multiply / Sine）那唯一一个整体输出叫 `Out` ——
	 * 比 `<0>` 好写，也和 `ResolveOutputIndex` 收的别名对得上。
	 */
	FString UAL_OutputPinName(UMaterialExpression* Expression, int32 OutputIndex)
	{
		if (!Expression)
		{
			return FString();
		}
		const TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
		if (!Outputs.IsValidIndex(OutputIndex))
		{
			return FString::Printf(TEXT("<%d>"), OutputIndex);
		}

		const FExpressionOutput& Output = Outputs[OutputIndex];
		if (!Output.OutputName.IsNone())
		{
			return Output.OutputName.ToString();
		}
		const FString FromMask = UAL_MaskDerivedName(Output);
		if (!FromMask.IsEmpty())
		{
			return FromMask;
		}
		return Outputs.Num() == 1 ? FString(TEXT("Out")) : FString::Printf(TEXT("<%d>"), OutputIndex);
	}

	/**
	 * 本 CPP 内的老名字，转发到类上的公开实现（见 UAL_MaterialCommands.h）。
	 *
	 * 编号规则搬到公开接口上了，因为 editor.get_focus_context 报「用户选中了
	 * 哪个材质节点」时必须用同一套 id。
	 */
	void UAL_BuildExpressionIds(UMaterial* Material, TMap<UMaterialExpression*, FString>& OutIds)
	{
		FUAL_MaterialCommands::BuildExpressionIds(Material, OutIds);
	}

	/**
	 * 改材质图之前，把该记的对象都记进事务。
	 *
	 * UE 5.1 起，节点数组和主节点的那一排引脚都搬进了 `GetEditorOnlyData()` ——
	 * 那是一个**独立的 UObject**。只调 `Material->Modify()` 记的是材质本身，
	 * 改到的却是另一个对象，于是事务里一个字节都没记。
	 *
	 * 后果不是「撤销撤不干净」，是**撤销栈里连这一步都不存在**：空事务会被引擎丢掉。
	 * 实测表现为「加了两个节点连了一根线，ue_undo_history 返回 undoable=0」，
	 * 而同一时间关卡里的操作记得好好的 —— 看起来像撤销栈只管关卡不管资产，
	 * 其实是这一行漏了。
	 */
	void UAL_ModifyMaterialGraph(UMaterial* Material)
	{
		if (!Material)
		{
			return;
		}

		/*
		 * 先把缺的 RF_Transactional 补上。
		 *
		 * 早期版本的 material.create 建材质时漏了这个标记，那批资产至今还在工程里。
		 * 没有它 Modify() 是空转，事务里什么都没记 —— 而「什么都没记」不等于
		 * 「什么都不会发生」：新建的节点对象自己是带标记的，于是撤销会把节点的
		 * 状态回退、却不把它从图里拿走，也不恢复被顶掉的连线。
		 *
		 * 实测的样子：撤销之后图里多出一个值被清零的节点，BaseColor 接在它身上，
		 * 原来接着的那个节点变成孤儿 —— 而材质照样编译通过，看不出任何异常。
		 * 半截撤销比完全撤不了更坏，因为它是静默的。
		 *
		 * 引擎自己建的材质一律带这个标记。这里把缺的补上，让老资产回到正常轨道，
		 * 存盘之后就永久修好了。
		 */
		Material->SetFlags(RF_Transactional);
		Material->Modify();
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		if (UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData())
		{
			Data->SetFlags(RF_Transactional);
			Data->Modify();
		}
#endif
	}

	/**
	 * 改完材质之后刷新界面。
	 *
	 * 对**资产**来说这基本是空操作 —— 资产的 `MaterialGraph` 是空的，那个指针只
	 * 建在材质编辑器自己那份副本身上（见下面 `UAL_MaterialEditorIsOpen` 的说明）。
	 * 留着它是为了材质函数预览之类确实带图的场景。
	 *
	 * 先 `RebuildGraph()`（材质 → 图）再刷新，顺序不能反：
	 * `FMaterialEditorUtilities::UpdateMaterialAfterGraphChange` 名字听着像
	 * 「材质变了，刷新界面」，实际做的是**反方向** —— 调
	 * `UMaterialGraph::LinkMaterialExpressionsFromGraph()` 拿图去覆盖材质，
	 * 而那个函数里有这么一段（引擎 MaterialGraph.cpp，5.6 的 515–518 行）：
	 *
	 *     else if (MaterialInput.Expression)
	 *     {
	 *         MaterialInput.Expression = NULL;
	 *     }
	 *
	 * 图里没连、材质里有连，它就把材质里那根清成 NULL。先对齐再刷新就不会踩到。
	 */
	void UAL_SyncMaterialEditor(UMaterial* Material)
	{
		if (!Material || !Material->MaterialGraph)
		{
			return;
		}
		Material->MaterialGraph->RebuildGraph();
		FMaterialEditorUtilities::UpdateMaterialAfterGraphChange(Material->MaterialGraph);
	}

	/**
	 * 这张材质此刻在材质编辑器里开着吗。
	 *
	 * ## 为什么开着就不能写
	 *
	 * 材质编辑器一打开就**把材质整个复制一份到临时包**，你在窗口里看到和编辑的
	 * 全程是那个副本（引擎 MaterialEditor.cpp 5.6 的 552 行）：
	 *
	 *     Material = (UMaterial*)StaticDuplicateObject(
	 *         OriginalMaterial, GetTransientPackage(), ...);
	 *
	 * 资产本身叫 `OriginalMaterial`，引擎自己的注释写着「**只有把副本的设置拷回来
	 * 时才会更新**」—— 也就是用户按「应用」的那一下。
	 *
	 * 于是我们从外面改资产会同时踩两个坑：
	 *
	 *   1. **看不到** —— 编辑器显示的是开窗那一刻的快照，我们写进资产它不知道。
	 *      用户看着满屏没连线的引脚，而 `material.get_graph` 读资产说连上了，
	 *      两边都没错，但没人说得清；
	 *   2. **会丢** —— 用户在编辑器里按一下「应用」，`UpdateOriginalMaterial()`
	 *      拿副本整个覆盖资产，AI 刚做的全没了，而且没有任何提示。
	 *
	 * 第 2 条是真会丢工作的，光警告拦不住，所以写命令直接拒。
	 *
	 * 真机上就是这么栽的：AI 连了 27 根线报成功，用户在开着的编辑器里看不到，
	 * 重读时连线掉回 20 根，主节点一根都没有。
	 */
	bool UAL_MaterialEditorIsOpen(UMaterial* Material)
	{
		if (!GEditor || !Material)
		{
			return false;
		}
		UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		return Subsystem && Subsystem->FindEditorsForAsset(Material).Num() > 0;
	}

	/**
	 * 拿到正在编辑这张材质的那个编辑器。没开着就返回 nullptr。
	 *
	 * 这个 static_cast 是安全的，先比过 `GetEditorName()` —— 这是引擎自己的写法，
	 * 见 MaterialEditor.cpp 里 `IMaterialEditor* MaterialAssetEditor =
	 * static_cast<IMaterialEditor*>(CurrentInstance);` 那一处，上面就是同样的比较。
	 */
	IMaterialEditor* UAL_FindMaterialEditor(UMaterial* Asset)
	{
		if (!GEditor || !Asset)
		{
			return nullptr;
		}
		UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!Subsystem)
		{
			return nullptr;
		}
		// bFocusIfOpen=false：只是查一下，别把用户的窗口抢到最前面
		IAssetEditorInstance* Instance = Subsystem->FindEditorForAsset(Asset, false);
		if (!Instance || Instance->GetEditorName() != FName(TEXT("MaterialEditor")))
		{
			return nullptr;
		}
		return static_cast<IMaterialEditor*>(Instance);
	}

	/**
	 * 定下这一条命令到底该改哪个 UMaterial 对象。
	 *
	 * ## 编辑器开着时，要改的不是资产
	 *
	 * 材质编辑器一打开就把材质整个复制一份到临时包，用户在窗口里看到和编辑的
	 * 全程是那个副本（引擎 MaterialEditor.cpp 5.6 的 552 行）：
	 *
	 *     Material = (UMaterial*)StaticDuplicateObject(
	 *         OriginalMaterial, GetTransientPackage(), ...);
	 *
	 * 资产本身叫 `OriginalMaterial`，引擎的注释写着「**只有把副本的设置拷回来时
	 * 才会更新**」—— 也就是用户按「应用」的那一下。
	 *
	 * 所以这时候绕过编辑器去改资产会同时踩两个坑：用户**看不到**（他看的是开窗
	 * 那一刻的快照），以及**会丢**（他按一下「应用」，副本整个覆盖资产，
	 * AI 做的全没了，还不带提示）。真机上就是这么栽的：连了 27 根线报成功，
	 * 重读时掉回 20 根，主节点一根都没有。
	 *
	 * 改成直接改**编辑器正在编的那一份**，两个坑一起没了，而且用户能眼看着
	 * 节点一个个长出来 —— 这比「让他先把窗口关掉」体验好得多。
	 *
	 * 编辑器没开时原样返回资产，行为和以前一模一样。
	 *
	 * @param OutEditor 开着的编辑器，没开是 nullptr。调用方写完要拿它刷新界面
	 */
	UMaterial* UAL_ResolveLiveMaterial(UMaterial* Asset, IMaterialEditor*& OutEditor)
	{
		OutEditor = UAL_FindMaterialEditor(Asset);
		if (!OutEditor)
		{
			return Asset;
		}
		UMaterial* Working = Cast<UMaterial>(OutEditor->GetMaterialInterface());
		// 拿不到副本就退回资产：宁可走老路径，也不能把命令整个失败掉
		return Working ? Working : Asset;
	}

	/**
	 * 写完之后让编辑器把改动显示出来。
	 *
	 * 顺序不能反。`UpdateMaterialAfterGraphChange` 名字听着像「材质变了，刷新界面」，
	 * 它实际做的是**反方向** —— 调 `LinkMaterialExpressionsFromGraph()` 拿图去覆盖
	 * 材质，而那个函数里有这么一段（引擎 MaterialGraph.cpp，5.6 的 515–518 行）：
	 *
	 *     else if (MaterialInput.Expression)
	 *     {
	 *         MaterialInput.Expression = NULL;
	 *     }
	 *
	 * 图里没连、材质里有连，它就把材质里那根清成 NULL。所以先 `RebuildGraph()`
	 * 把图对齐到材质（材质 → 图，正方向），再让它刷新预览和代码视图。
	 *
	 * `MarkMaterialDirty()` 让标题栏出现那个星号：改动此刻只在副本上，
	 * 要落到资产得走一次「应用」（见 `UAL_ApplyMaterialEditorToAsset`）。
	 */
	void UAL_RefreshMaterialEditor(IMaterialEditor* Editor, UMaterial* Working)
	{
		if (!Editor || !Working)
		{
			return;
		}
		if (Working->MaterialGraph)
		{
			Working->MaterialGraph->RebuildGraph();
		}
		Editor->UpdateMaterialAfterGraphChange();

		// MarkMaterialDirty 是 5.4 才加到 IMaterialEditor 上的（5.0–5.3 逐个查过头文件）。
		// 老版本上只是标题栏那个星号不出现，改动本身照样在副本上；落盘走的是
		// UAL_ApplyMaterialEditorToAsset，不依赖这个标记。
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
		Editor->MarkMaterialDirty();
#endif
	}

	/**
	 * 把编辑器里的改动应用回资产 —— 相当于替用户按一下「保存」。
	 *
	 * ## 为什么绕这么一圈
	 *
	 * 干这件事的函数是 `FAssetEditorToolkit::SaveAsset_Execute()`（材质编辑器重写了
	 * 它，里面就是「UpdateOriginalMaterial + 存包」），但它是 **protected**，
	 * 插件调不到。自己复刻 `UpdateOriginalMaterial` 也不行 —— 那函数一百多行，
	 * 还要摸 MaterialStatsManager 这类私有状态，九个引擎版本各写一遍必炸。
	 *
	 * 走得通的是**同一个 UI 命令**：工具箱的命令表是公开的（`GetToolkitCommands()`
	 * 在 FBaseToolkit 上），命令本身能按「上下文 + 名字」从输入绑定管理器里查到。
	 * 执行它和用户在那个窗口里按 Ctrl+S 是同一条路径。
	 *
	 * 上下文名 "AssetEditor"、命令名 "SaveAsset" 来自引擎的
	 * `FAssetEditorCommonCommands`，`FindCommandInContext` 的签名在 5.5 和 5.8 上
	 * 逐字相同。查不到就安静跳过：拿不到就当没开编辑器，最坏是改动留在编辑器里
	 * 等用户自己按保存，不会丢。
	 */
	void UAL_ApplyMaterialEditorToAsset(UMaterial* Asset)
	{
		IMaterialEditor* Editor = UAL_FindMaterialEditor(Asset);
		if (!Editor)
		{
			return;
		}

		const TSharedPtr<FUICommandInfo> SaveCommand =
			FInputBindingManager::Get().FindCommandInContext(
				FName(TEXT("AssetEditor")), FName(TEXT("SaveAsset")));
		if (!SaveCommand.IsValid())
		{
			UE_LOG(LogUALMaterial, Warning,
				TEXT("Could not find the AssetEditor.SaveAsset command; changes stay in the Material Editor until the user saves."));
			return;
		}

		Editor->GetToolkitCommands()->ExecuteAction(SaveCommand.ToSharedRef());
	}

	/** 材质主节点上的一根输入引脚。ExpectedType 是这一侧要什么类型 */
	struct FUALRootInput
	{
		const TCHAR* Name;
		FExpressionInput* Input;
		const TCHAR* ExpectedType;
	};

	/**
	 * 材质主节点的全部输入引脚，一处维护。
	 *
	 * 这张表以前在读图那边手写了一份，漏了 SubsurfaceColor 和 MaterialAttributes，
	 * 于是「用材质属性」的材质读回来没有任何连到主节点的线。删节点那边则是
	 * 压根没有这张表 —— 删掉一个接在 Metallic 上的节点，那根线还留着指向
	 * 已经不存在的节点，材质从此编不过，而删除操作报的是成功。
	 * 同一张表被两处漏掉过两次，所以收到这里来。
	 */
	TArray<FUALRootInput> UAL_CollectRootInputs(UMaterial* Material)
	{
		TArray<FUALRootInput> Out;
		if (!Material)
		{
			return Out;
		}
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		// 5.1 起主节点的输入挪进了 GetEditorOnlyData()，直接 Material->BaseColor 编不过
		UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
		if (!Data)
		{
			return Out;
		}
		Out.Append({
			{ TEXT("BaseColor"), &Data->BaseColor, TEXT("float3") },
			{ TEXT("Metallic"), &Data->Metallic, TEXT("float") },
			{ TEXT("Specular"), &Data->Specular, TEXT("float") },
			{ TEXT("Roughness"), &Data->Roughness, TEXT("float") },
			{ TEXT("EmissiveColor"), &Data->EmissiveColor, TEXT("float3") },
			{ TEXT("Opacity"), &Data->Opacity, TEXT("float") },
			{ TEXT("OpacityMask"), &Data->OpacityMask, TEXT("float") },
			{ TEXT("Normal"), &Data->Normal, TEXT("float3") },
			{ TEXT("WorldPositionOffset"), &Data->WorldPositionOffset, TEXT("float3") },
			{ TEXT("SubsurfaceColor"), &Data->SubsurfaceColor, TEXT("float3") },
			{ TEXT("AmbientOcclusion"), &Data->AmbientOcclusion, TEXT("float") },
			{ TEXT("MaterialAttributes"), &Data->MaterialAttributes, TEXT("MaterialAttributes") }
		});
#if UAL_WITH_SUBSTRATE
		/*
		 * Substrate 材质的全部着色都从这一根进主节点。
		 *
		 * 项目没开 Substrate 时也照样列在这里：这张表还要给读图、删节点、
		 * 清理未用节点当判据用。漏了它，清理会把整张 Substrate 图当成没接上的
		 * 死节点一起摘掉 —— 这正是以前的行为。「能不能往上连」另外判，
		 * 见 UAL_SubstrateUnavailableReason。
		 */
		Out.Add({ TEXT("FrontMaterial"), &Data->FrontMaterial, TEXT("Substrate") });
#endif
#else
		Out.Append({
			{ TEXT("BaseColor"), &Material->BaseColor, TEXT("float3") },
			{ TEXT("Metallic"), &Material->Metallic, TEXT("float") },
			{ TEXT("Specular"), &Material->Specular, TEXT("float") },
			{ TEXT("Roughness"), &Material->Roughness, TEXT("float") },
			{ TEXT("EmissiveColor"), &Material->EmissiveColor, TEXT("float3") },
			{ TEXT("Opacity"), &Material->Opacity, TEXT("float") },
			{ TEXT("OpacityMask"), &Material->OpacityMask, TEXT("float") },
			{ TEXT("Normal"), &Material->Normal, TEXT("float3") },
			{ TEXT("WorldPositionOffset"), &Material->WorldPositionOffset, TEXT("float3") },
			{ TEXT("SubsurfaceColor"), &Material->SubsurfaceColor, TEXT("float3") },
			{ TEXT("AmbientOcclusion"), &Material->AmbientOcclusion, TEXT("float") },
			{ TEXT("MaterialAttributes"), &Material->MaterialAttributes, TEXT("MaterialAttributes") }
		});
#endif
		return Out;
	}

	/** JSON 对象的全部键。5.8 的键类型是 UE::FSharedString，别直接 GetKeys（见 UAL_JsonKey） */
	TArray<FString> UAL_JsonKeys(const TSharedPtr<FJsonObject>& Obj)
	{
		TArray<FString> Keys;
		if (Obj.IsValid())
		{
			for (const auto& Pair : Obj->Values)
			{
				Keys.Add(UAL_JsonKey(Pair.Key));
			}
		}
		return Keys;
	}

	/** 这个 UPROPERTY 是不是一根输入引脚（FExpressionInput 一族）——那是连线，不是值 */
	bool UAL_IsPinProperty(const FProperty* Prop)
	{
		const FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		if (!StructProp || !StructProp->Struct)
		{
			return false;
		}
		const FString StructName = StructProp->Struct->GetName();
		return StructName.Contains(TEXT("ExpressionInput")) || StructName.Contains(TEXT("MaterialInput"));
	}

	/**
	 * 节点上可以按名字设的属性（细节面板里那一栏），给 search_nodes 当说明书。
	 *
	 * 只列节点**自己类上**声明的（UMaterialExpression 基类那些 Desc / bCollapsed
	 * 之类不列），跳过引脚。值取 CDO 上的默认值。
	 */
	TSharedPtr<FJsonObject> UAL_NodeEditableProperties(UMaterialExpression* Cdo)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!Cdo)
		{
			return Out;
		}
		for (TFieldIterator<FProperty> It(Cdo->GetClass()); It; ++It)
		{
			const FProperty* Prop = *It;
			if (!Prop->HasAnyPropertyFlags(CPF_Edit) || Prop->HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated) ||
				UAL_IsPinProperty(Prop))
			{
				continue;
			}
			const UClass* Owner = Prop->GetOwnerClass();
			if (!Owner || Owner == UMaterialExpression::StaticClass() || !Owner->IsChildOf(UMaterialExpression::StaticClass()))
			{
				continue;
			}
			FString ReadError;
			if (TSharedPtr<FJsonValue> Value = UALPropertyPath::GetByPath(Cdo, Prop->GetName(), ReadError))
			{
				Out->SetField(Prop->GetName(), Value);
			}
		}
		return Out;
	}

	/**
	 * 按属性名给节点设 UPROPERTY —— 节点细节面板里的那些设置。
	 *
	 * Substrate 的配方离不开它：皮肤要 Slab 的 SubsurfaceProfile，玻璃要
	 * SubSurfaceType=Simple Volume（5.6+），控成本要算子上的 bUseParameterBlending，
	 * Select 要 Threshold。这些都不是引脚、也不是 `value`，以前根本设不了，
	 * 模型只能退回 Python。
	 *
	 * 写入走 UALPropertyPath::SetByPath（枚举、资产路径、结构体字面量都认，
	 * 写错名字会给「你是不是想写 XXX」）。**任何一条失败就整体回 false**，
	 * 回滚由调用方负责；成功时 OutReadback 是写完之后从对象上读回来的值。
	 */
	bool UAL_ApplyNodeProperties(UMaterialExpression* Expression, const TSharedPtr<FJsonObject>& Props,
		TSharedPtr<FJsonObject>& OutReadback, FString& OutError)
	{
		OutReadback = MakeShared<FJsonObject>();
		if (!Expression || !Props.IsValid())
		{
			return true;
		}
		for (const auto& Pair : Props->Values)
		{
			const FString Key = UAL_JsonKey(Pair.Key);
			FString TopName = Key;
			int32 Cut = INDEX_NONE;
			if (TopName.FindChar(TEXT('.'), Cut) || TopName.FindChar(TEXT('['), Cut))
			{
				TopName.LeftInline(Cut);
			}
			if (UAL_IsPinProperty(FindFProperty<FProperty>(Expression->GetClass(), *TopName)))
			{
				OutError = FString::Printf(TEXT("'%s' is an input pin, not a setting - connect something to it instead"),
					*Key);
				return false;
			}

			FString SetError;
			if (!UALPropertyPath::SetByPath(Expression, Key, Pair.Value, SetError))
			{
				OutError = FString::Printf(TEXT("property '%s': %s"), *Key, *SetError);
				return false;
			}
			FString ReadError;
			if (TSharedPtr<FJsonValue> Readback = UALPropertyPath::GetByPath(Expression, Key, ReadError))
			{
				OutReadback->SetField(Key, Readback);
			}
		}
		return true;
	}

	/** 这个节点类是不是 Substrate 那一族（BSDF / 混合 / 转换 / 工具节点） */
	bool UAL_IsSubstrateClass(const UClass* Class)
	{
#if UAL_WITH_SUBSTRATE
		return Class && (Class->IsChildOf(UMaterialExpressionSubstrateBSDF::StaticClass()) ||
			Class->IsChildOf(UMaterialExpressionSubstrateUtilityBase::StaticClass()));
#else
		return false;
#endif
	}

	/**
	 * 从 Source 顺着输入往上游走，这根线里**可能**带着 Substrate 数据吗。
	 *
	 * 不能只看输出类型：5.4–5.7 上 MaterialFunctionCall、StaticSwitch 都没覆写
	 * GetOutputType，基类对不带 mask 的输出一律回 MCT_Float —— 引擎自带的
	 * Substrate 材质函数（SMF_Coated…）、在两个 Slab 之间切的静态开关都会被当成数值。
	 *
	 * 所以判「来源」：上游有 Substrate 节点就可能带；碰到数据来源在别处、
	 * 从图上看不到的节点（函数调用、命名重定向的 usage、函数输入）也按「可能」算，
	 * 交给编译器判。只有整条上游都是看得见的普通节点，才能断定是数值。
	 */
	bool UAL_MayCarrySubstrate(UMaterialExpression* Source)
	{
		// 上限只防病态大图，正常材质远到不了
		constexpr int32 MaxVisited = 4096;
		TSet<UMaterialExpression*> Seen;
		TArray<UMaterialExpression*> Stack = { Source };
		while (Stack.Num() > 0)
		{
			UMaterialExpression* Current = Stack.Pop();
			if (!Current || Seen.Contains(Current))
			{
				continue;
			}
			if (Seen.Num() >= MaxVisited)
			{
				return true;
			}
			Seen.Add(Current);

			const UClass* Class = Current->GetClass();
			if (UAL_IsSubstrateClass(Class) ||
				Class->IsChildOf(UMaterialExpressionMaterialFunctionCall::StaticClass()) ||
				Class->IsChildOf(UMaterialExpressionNamedRerouteUsage::StaticClass()) ||
				Class->GetFName() == TEXT("MaterialExpressionFunctionInput"))
			{
				return true;
			}
			const int32 InputCount = UALCompat::CountInputs(Current);
			for (int32 i = 0; i < InputCount; ++i)
			{
				if (FExpressionInput* In = Current->GetInput(i))
				{
					Stack.Add(In->Expression);
				}
			}
		}
		return false;
	}

	/** 当前编辑器里 Substrate 是否生效。r.Substrate 只在启动时读，运行中改了不算 */
	bool UAL_IsSubstrateEnabled()
	{
#if UAL_WITH_SUBSTRATE
		return Substrate::IsSubstrateEnabled();
#else
		return false;
#endif
	}

	/**
	 * Substrate 用不了的原因，用得了回空串。
	 *
	 * 没开 Substrate 时引擎自己的节点面板里根本不出现这些节点，编译器也不看
	 * FrontMaterial —— 节点建得出来、线连得上、编译还是绿的，画面却毫无变化，
	 * 回执也挑不出错。这种「成功」比一个 400 糟得多，所以建之前就挡住，
	 * 并且把怎么打开说清楚（改完必须重启编辑器，这一点最容易漏）。
	 */
	FString UAL_SubstrateUnavailableReason()
	{
#if UAL_WITH_SUBSTRATE
		if (UAL_IsSubstrateEnabled())
		{
			return FString();
		}
		return TEXT("Substrate is not enabled in this project, so Substrate nodes and the FrontMaterial pin "
			"would have no effect. Enable it with r.Substrate=True under [/Script/Engine.RendererSettings] "
			"in Config/DefaultEngine.ini (Project Settings > Rendering > Substrate), then restart the editor - "
			"it is read only at startup. Otherwise author the material with the legacy pins (BaseColor, Metallic, ...).");
#else
		return FString::Printf(TEXT("Substrate materials need Unreal Engine 5.4 or later; this editor is %d.%d. "
			"Author the material with the legacy pins (BaseColor, Metallic, ...)."),
			ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);
#endif
	}

	/**
	 * 这个材质此刻是不是按 Substrate 解释主节点。
	 *
	 * - 没开 Substrate：不是。
	 * - 5.6+（引擎做「隐藏转换」）：FrontMaterial 接了东西才是；空着的时候引擎在编译时
	 *   自己把 BaseColor 那一排转过去，主节点照普通材质的规矩用。
	 * - 5.4 / 5.5：开着 Substrate 就是 —— 普通引脚不受支持（5.5 改一次还会被自动转换挪走）。
	 */
	bool UAL_MaterialInSubstrateMode(UMaterial* Material)
	{
#if UAL_WITH_SUBSTRATE
		if (!Material || !UAL_IsSubstrateEnabled())
		{
			return false;
		}
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
		if (Substrate::IsHiddenMaterialAssetConversionEnabled())
		{
			const UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
			return Data && Data->FrontMaterial.IsConnected();
		}
#endif
		return true;
#else
		return false;
#endif
	}

	/**
	 * Substrate 开着时，主节点上这根引脚是不是被引擎忽略了。
	 *
	 * Substrate 下 BaseColor / Metallic / Roughness / Normal / Emissive… 这一排
	 * 全部不参与着色（材质编辑器里直接不显示），只有 FrontMaterial、
	 * OpacityMask（Masked 时）、WPO、AO 等少数几根还算数。
	 *
	 * 往被忽略的引脚上连线的后果比「没效果」更绕：图里还没有 Substrate 节点时，
	 * 第一根连到 BaseColor 的线会触发引擎的自动转换（`ConvertMaterialToSubstrateMaterial`），
	 * 它插一个 SubstrateShadingModels 节点、把这根线**挪**过去接到 FrontMaterial 上；
	 * 从此 FrontMaterial 有线，转换不再发生，后面连到 Metallic、Roughness 的线
	 * 全都静静躺在主节点上不起作用 —— 每一步都回成功。
	 *
	 * 判据直接用引擎的 `IsPropertyActiveInEditor`（材质编辑器灰掉引脚用的就是它），
	 * 它会看域、混合模式、曲面细分这些，不自己抄一份规则。
	 *
	 * **但只问 Substrate 顶替掉的那几根。** 这个函数一度对每根主节点引脚都问一遍，
	 * 于是 OpacityMask / Opacity 这种「因为混合模式还是 Opaque 才不生效」的也被拒了 ——
	 * 5.7/5.8 新工程默认开着 Substrate，老式的「先连 Opacity、再改 Translucent」
	 * 直接 400，报错还叫人去用 Substrate 节点。混合模式没对上是普通材质的老规矩，
	 * 没开 Substrate 时从来不拦，开了也不该拦。
	 *   · 着色那一排（BaseColor…Normal、SubsurfaceColor）：Substrate 下不生效就是被它顶替了
	 *   · Opacity：只有混合模式本来就会用它（半透明类）却仍不生效时，才是 Substrate
	 *     改用 Coverage 的缘故
	 *   · OpacityMask / WPO / AO / MaterialAttributes / FrontMaterial：生效条件和
	 *     普通材质一样，不归这里管
	 */
	bool UAL_RootPinIgnoredBySubstrate(UMaterial* Material, const TCHAR* PinName)
	{
#if UAL_WITH_SUBSTRATE
		/*
		 * 先问材质此刻是不是**按 Substrate 解释**，不是就一根都不拦。
		 *
		 * 真机撞过（5.8、Substrate 开着、普通半透明材质、FrontMaterial 空着）：
		 * Roughness 灰掉是因为半透明的光照模式（VolumetricNonDirectional）本来就不用它，
		 * 这是普通材质的老规矩；可这里只看「不生效」，就按「被 Substrate 顶替」拒了，
		 * AI 只好把粗糙度删掉，还跟用户说「开着 Substrate 只准用这几根引脚」。
		 * Unlit 材质的 BaseColor、各种着色模型下灰掉的引脚都是同一类。
		 * 「不生效」有很多原因，只有材质处在 Substrate 模式时，才是 Substrate 的缘故。
		 */
		if (!UAL_MaterialInSubstrateMode(Material))
		{
			return false;
		}
		// Substrate 模式下着色那一排一律不看 —— 全部改从 FrontMaterial 那棵树里来
		static const TCHAR* ShadingPins[] = {
			TEXT("BaseColor"), TEXT("Metallic"), TEXT("Specular"), TEXT("Roughness"),
			TEXT("EmissiveColor"), TEXT("Normal"), TEXT("SubsurfaceColor"),
		};
		for (const TCHAR* Shading : ShadingPins)
		{
			if (FCString::Stricmp(Shading, PinName) == 0)
			{
				return true;
			}
		}
		if (FCString::Stricmp(PinName, TEXT("Opacity")) == 0)
		{
			const EBlendMode Blend = Material->BlendMode;
			const bool bBlendUsesOpacity = Blend != BLEND_Opaque && Blend != BLEND_Masked && Blend != BLEND_Modulate;
			return bBlendUsesOpacity && !Material->IsPropertyActiveInEditor(MP_Opacity);
		}
#endif
		return false;
	}

	/**
	 * 「可用引脚」清单里该不该列这一根。建材质、读图两处的清单都走它，
	 * 和 connect_pins 会拒的那两种情况一一对应：没开 Substrate 时的 FrontMaterial、
	 * 开了 Substrate 时被引擎忽略的旧引脚。
	 */
	bool UAL_IsRootPinOffered(UMaterial* Material, const TCHAR* PinName)
	{
		if (FCString::Stricmp(PinName, TEXT("FrontMaterial")) == 0 && !UAL_IsSubstrateEnabled())
		{
			return false;
		}
		return !UAL_RootPinIgnoredBySubstrate(Material, PinName);
	}

	/**
	 * `node_type` → 表达式类，全插件只此一张表。
	 *
	 * 它原来是 `Handle_AddMaterialNode` 里的一个静态局部量，只有「建节点」那条路
	 * 看得见。`material.search_nodes` 要照着同一张表报「有哪些节点类型、各自什么
	 * 引脚」—— 两份表一旦分家，说明书和真正建得出来的东西就会漂移，
	 * 而漂移的表现是模型照着说明书写出一个必然 400 的 node_type。
	 */
	const TMap<FString, UClass*>& UAL_MaterialNodeTypeMap()
	{
		static TMap<FString, UClass*> NodeTypeMap;
		if (NodeTypeMap.Num() == 0)
		{
			NodeTypeMap.Add(TEXT("Constant"), UMaterialExpressionConstant::StaticClass());
			NodeTypeMap.Add(TEXT("Constant3Vector"), UMaterialExpressionConstant3Vector::StaticClass());
			NodeTypeMap.Add(TEXT("Constant4Vector"), UMaterialExpressionConstant4Vector::StaticClass());
			NodeTypeMap.Add(TEXT("ScalarParameter"), UMaterialExpressionScalarParameter::StaticClass());
			NodeTypeMap.Add(TEXT("VectorParameter"), UMaterialExpressionVectorParameter::StaticClass());
			NodeTypeMap.Add(TEXT("TextureSample"), UMaterialExpressionTextureSample::StaticClass());
			NodeTypeMap.Add(TEXT("TextureSampleParameter2D"), UMaterialExpressionTextureSampleParameter2D::StaticClass());
			NodeTypeMap.Add(TEXT("TextureCoordinate"), UMaterialExpressionTextureCoordinate::StaticClass());
			NodeTypeMap.Add(TEXT("Add"), UMaterialExpressionAdd::StaticClass());
			NodeTypeMap.Add(TEXT("Subtract"), UMaterialExpressionSubtract::StaticClass());
			NodeTypeMap.Add(TEXT("Multiply"), UMaterialExpressionMultiply::StaticClass());
			NodeTypeMap.Add(TEXT("Divide"), UMaterialExpressionDivide::StaticClass());
			NodeTypeMap.Add(TEXT("Lerp"), UMaterialExpressionLinearInterpolate::StaticClass());
			NodeTypeMap.Add(TEXT("Clamp"), UMaterialExpressionClamp::StaticClass());
			NodeTypeMap.Add(TEXT("Power"), UMaterialExpressionPower::StaticClass());
			NodeTypeMap.Add(TEXT("OneMinus"), UMaterialExpressionOneMinus::StaticClass());
			NodeTypeMap.Add(TEXT("Saturate"), UMaterialExpressionSaturate::StaticClass());
			NodeTypeMap.Add(TEXT("Fresnel"), UMaterialExpressionFresnel::StaticClass());
			NodeTypeMap.Add(TEXT("Time"), UMaterialExpressionTime::StaticClass());
			NodeTypeMap.Add(TEXT("Panner"), UMaterialExpressionPanner::StaticClass());
			NodeTypeMap.Add(TEXT("ComponentMask"), UMaterialExpressionComponentMask::StaticClass());
			NodeTypeMap.Add(TEXT("AppendVector"), UMaterialExpressionAppendVector::StaticClass());
			NodeTypeMap.Add(TEXT("Normalize"), UMaterialExpressionNormalize::StaticClass());

			// 基础数学节点。
			//
			// 原来这张表里没有 Sine —— 而「呼吸式闪烁」「上下浮动」「波动」这类
			// 最常见的材质效果全都要它。真机任务评测里就是卡在这：
			// 用户要一个闪烁的自发光材质，Time 节点建出来了，Sine 直接
			// 「Unknown node_type」，整个任务只能中止。
			//
			// 单标量 Constant 同样缺席（表里只有 Constant3Vector / Constant4Vector），
			// 于是「把粗糙度设成 0.2」这种最简单的需求也没有直接的节点可用。
			NodeTypeMap.Add(TEXT("Sine"), UMaterialExpressionSine::StaticClass());
			NodeTypeMap.Add(TEXT("Cosine"), UMaterialExpressionCosine::StaticClass());
			NodeTypeMap.Add(TEXT("Constant"), UMaterialExpressionConstant::StaticClass());
			NodeTypeMap.Add(TEXT("Constant2Vector"), UMaterialExpressionConstant2Vector::StaticClass());
			NodeTypeMap.Add(TEXT("Abs"), UMaterialExpressionAbs::StaticClass());
			NodeTypeMap.Add(TEXT("Frac"), UMaterialExpressionFrac::StaticClass());
			NodeTypeMap.Add(TEXT("Floor"), UMaterialExpressionFloor::StaticClass());
			NodeTypeMap.Add(TEXT("Ceil"), UMaterialExpressionCeil::StaticClass());
			NodeTypeMap.Add(TEXT("Min"), UMaterialExpressionMin::StaticClass());
			NodeTypeMap.Add(TEXT("Max"), UMaterialExpressionMax::StaticClass());
			NodeTypeMap.Add(TEXT("SquareRoot"), UMaterialExpressionSquareRoot::StaticClass());
			NodeTypeMap.Add(TEXT("TextureObject"), UMaterialExpressionTextureObject::StaticClass());
			NodeTypeMap.Add(TEXT("DotProduct"), UMaterialExpressionDotProduct::StaticClass());
			NodeTypeMap.Add(TEXT("CrossProduct"), UMaterialExpressionCrossProduct::StaticClass());

			// 调用方的 node_type 枚举里有 55 个，这张表原来只认 36 个 ——
			// 剩下 19 个每一个都是一次必然的 400。枚举对模型来说就是能力清单：
			// 要做溶解会选 Noise，要做边缘光会选 DepthFade，然后撞墙。
			// 下面这些都只差一行 StaticClass()，补上比砍掉划算。
			NodeTypeMap.Add(TEXT("StaticSwitchParameter"), UMaterialExpressionStaticSwitchParameter::StaticClass());
			NodeTypeMap.Add(TEXT("StaticBoolParameter"), UMaterialExpressionStaticBoolParameter::StaticClass());
			NodeTypeMap.Add(TEXT("BreakMaterialAttributes"), UMaterialExpressionBreakMaterialAttributes::StaticClass());
			NodeTypeMap.Add(TEXT("MakeMaterialAttributes"), UMaterialExpressionMakeMaterialAttributes::StaticClass());
			NodeTypeMap.Add(TEXT("WorldPosition"), UMaterialExpressionWorldPosition::StaticClass());
			NodeTypeMap.Add(TEXT("CameraPosition"), UMaterialExpressionCameraPositionWS::StaticClass());
			NodeTypeMap.Add(TEXT("VertexNormalWS"), UMaterialExpressionVertexNormalWS::StaticClass());
			NodeTypeMap.Add(TEXT("PixelNormalWS"), UMaterialExpressionPixelNormalWS::StaticClass());
			NodeTypeMap.Add(TEXT("PixelDepth"), UMaterialExpressionPixelDepth::StaticClass());
			NodeTypeMap.Add(TEXT("Noise"), UMaterialExpressionNoise::StaticClass());

			// 引用材质参数集合里的一个参数。没有它的话 material.parameter_collection
			// 建出来的 MPC 是个死物 —— 材质里引用不到，等于白建
			NodeTypeMap.Add(TEXT("CollectionParameter"), UMaterialExpressionCollectionParameter::StaticClass());

			// 调用材质函数。原来注释里写它「光有 StaticClass 建出来是废的」——
			// 那是因为当时没有传函数资产的路子。现在 add_node 认 function_path，
			// 建的时候就把函数挂上，节点是完整可用的
			NodeTypeMap.Add(TEXT("MaterialFunctionCall"), UMaterialExpressionMaterialFunctionCall::StaticClass());

#if UAL_WITH_SUBSTRATE
			/*
			 * Substrate 节点（5.4+）。node_type 一律带 Substrate 前缀：
			 * search_nodes 搜 "Substrate" 就能一次拿到整族，也不会和同名的旧节点
			 * （Add / Select）撞车。
			 *
			 * 各节点引脚名不在这里抄，search_nodes 从 CDO 现读，逐版本自然正确。
			 * 项目没开 Substrate 时建不了，见 UAL_SubstrateUnavailableReason。
			 *
			 * 不收 SubstrateConvertMaterialAttributes 和 SubstrateShadingModels：
			 * 那两个是引擎把旧材质自动转换时用的，手写新材质用不上，
			 * 列出来只会让模型绕开 Slab 去走转换那条路。
			 */
			NodeTypeMap.Add(TEXT("SubstrateSlabBSDF"), UMaterialExpressionSubstrateSlabBSDF::StaticClass());
			NodeTypeMap.Add(TEXT("SubstrateSimpleClearCoatBSDF"), UMaterialExpressionSubstrateSimpleClearCoatBSDF::StaticClass());
			NodeTypeMap.Add(TEXT("SubstrateUnlitBSDF"), UMaterialExpressionSubstrateUnlitBSDF::StaticClass());
			NodeTypeMap.Add(TEXT("SubstrateHairBSDF"), UMaterialExpressionSubstrateHairBSDF::StaticClass());
			NodeTypeMap.Add(TEXT("SubstrateEyeBSDF"), UMaterialExpressionSubstrateEyeBSDF::StaticClass());
			NodeTypeMap.Add(TEXT("SubstrateSingleLayerWaterBSDF"), UMaterialExpressionSubstrateSingleLayerWaterBSDF::StaticClass());
			NodeTypeMap.Add(TEXT("SubstrateVolumetricFogCloudBSDF"), UMaterialExpressionSubstrateVolumetricFogCloudBSDF::StaticClass());
			NodeTypeMap.Add(TEXT("SubstrateLightFunction"), UMaterialExpressionSubstrateLightFunction::StaticClass());
			NodeTypeMap.Add(TEXT("SubstratePostProcess"), UMaterialExpressionSubstratePostProcess::StaticClass());
			NodeTypeMap.Add(TEXT("SubstrateUI"), UMaterialExpressionSubstrateUI::StaticClass());
			NodeTypeMap.Add(TEXT("SubstrateConvertToDecal"), UMaterialExpressionSubstrateConvertToDecal::StaticClass());
			// 组合算子：左右混（按遮罩）、上下叠（涂层）、相加、按覆盖度加权
			NodeTypeMap.Add(TEXT("SubstrateHorizontalMixing"), UMaterialExpressionSubstrateHorizontalMixing::StaticClass());
			NodeTypeMap.Add(TEXT("SubstrateVerticalLayering"), UMaterialExpressionSubstrateVerticalLayering::StaticClass());
			NodeTypeMap.Add(TEXT("SubstrateAdd"), UMaterialExpressionSubstrateAdd::StaticClass());
			NodeTypeMap.Add(TEXT("SubstrateWeight"), UMaterialExpressionSubstrateWeight::StaticClass());
			// 工具节点：输出的是普通数值，喂给 Slab 的引脚
			NodeTypeMap.Add(TEXT("SubstrateMetalnessToDiffuseAlbedoF0"), UMaterialExpressionSubstrateMetalnessToDiffuseAlbedoF0::StaticClass());
			NodeTypeMap.Add(TEXT("SubstrateTransmittanceToMFP"), UMaterialExpressionSubstrateTransmittanceToMFP::StaticClass());
			NodeTypeMap.Add(TEXT("SubstrateHazinessToSecondaryRoughness"), UMaterialExpressionSubstrateHazinessToSecondaryRoughness::StaticClass());
			NodeTypeMap.Add(TEXT("SubstrateThinFilm"), UMaterialExpressionSubstrateThinFilm::StaticClass());
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
			NodeTypeMap.Add(TEXT("SubstrateSelect"), UMaterialExpressionSubstrateSelect::StaticClass());
#endif
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
			// 5.8 引擎自己标着 Experimental
			NodeTypeMap.Add(TEXT("SubstrateToonBSDF"), UMaterialExpressionSubstrateToonBSDF::StaticClass());
#endif
#endif
			// ObjectPosition / ActorPosition / Rotator / DepthFade / ScreenPosition 这五个类在部分引擎版本上没导出
			// （编得过链不上），已同步从调用方枚举里删掉。
			// 没补的四个（Custom / SceneTexture / MaterialFunctionCall / Comment）
			// 光有 StaticClass 建出来是废的：各自还要 code 字符串、SceneTexture id、
			// 函数资产、以及 Comment 根本不在 Expressions 数组里。
			// 它们已从调用方的枚举里删掉，有人真要用再补。
		}
		return NodeTypeMap;
	}
}

/**
 * 注册所有材质相关命令
 */
void FUAL_MaterialCommands::RegisterCommands(
	TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("material.create"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CreateMaterial(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.apply"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ApplyMaterial(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.describe"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DescribeMaterial(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.set_param"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetMaterialParam(Payload, RequestId);
	});

	// Phase 1: 材质图表编辑命令
	CommandMap.Add(TEXT("material.get_graph"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetMaterialGraph(Payload, RequestId);
	});

	// 节点说明书。写图之前查一批引脚签名，别靠建探针材质试出来
	CommandMap.Add(TEXT("material.search_nodes"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SearchMaterialNodes(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.add_node"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_AddMaterialNode(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.connect_pins"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ConnectMaterialPins(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.compile"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CompileMaterial(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.set_node_value"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetMaterialNodeValue(Payload, RequestId);
	});

	// material.disconnect_pins - 断线。此前只能连不能断，改图只好整个删了重建
	CommandMap.Add(TEXT("material.disconnect_pins"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DisconnectMaterialPins(Payload, RequestId);
	});

	// material.delete_unused_nodes - 清理对输出没贡献的节点
	CommandMap.Add(TEXT("material.delete_unused_nodes"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DeleteUnusedMaterialNodes(Payload, RequestId);
	});

	// material.parameter_collection - 材质参数集合（一个开关控制全场景材质）
	CommandMap.Add(TEXT("material.parameter_collection"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_MaterialParameterCollection(Payload, RequestId);
	});

	// material.create_function - 材质函数（一组节点复用到多个材质）
	CommandMap.Add(TEXT("material.create_function"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CreateMaterialFunction(Payload, RequestId);
	});

	// material.get_referencers - 反查谁在用这个资产
	CommandMap.Add(TEXT("material.get_referencers"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetMaterialReferencers(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.delete_node"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DeleteMaterialNode(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.set_node_positions"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetMaterialNodePositions(Payload, RequestId);
	});

	// Phase 2: 材质管理命令（智能容错）
	CommandMap.Add(TEXT("material.duplicate"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DuplicateMaterial(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.set_property"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetMaterialProperty(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.create_instance"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CreateMaterialInstance(Payload, RequestId);
	});

	// material.list / material.preview 已删（2026-09-16）：list 被 content.search
	// 带 filter_class 覆盖，preview 的属性并进了 material.describe。
	// 盒子这边两条都没调过，留着只会让人以为还有两件事可做

	UE_LOG(LogUALMaterial, Log, TEXT("Registered 14 material commands"));
}

// ============================================================================
// Handle_CreateMaterial - 创建 UMaterial（母材质）
// ============================================================================
void FUAL_MaterialCommands::Handle_CreateMaterial(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数: material_name
	FString MaterialName;
	if (!Payload->TryGetStringField(TEXT("material_name"), MaterialName) || MaterialName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: material_name"));
		return;
	}

	// 2. 解析可选参数
	FString DestinationPath = TEXT("/Game/Materials");
	Payload->TryGetStringField(TEXT("destination_path"), DestinationPath);

	FString BlendModeStr;
	Payload->TryGetStringField(TEXT("blend_mode"), BlendModeStr);

	FString ShadingModelStr;
	Payload->TryGetStringField(TEXT("shading_model"), ShadingModelStr);

	bool bTwoSided = false;
	Payload->TryGetBoolField(TEXT("two_sided"), bTwoSided);

	/**
	 * `fail_if_exists` —— 同名已存在就报错返回，不覆盖。
	 *
	 * 默认行为（false）是引擎自己的：`FactoryCreateNew` 建对象时**不查重**，
	 * 同名资产会被就地覆盖。对「AI 建一个新材质」这个用法，那是个陷阱：
	 * 调用方以为拿到的是新资产，实际拿到的是用户原来那个的躯壳 ——
	 * 于是「失败了就把这次新建的删掉」会删掉用户的东西。
	 *
	 * 调用方在 app 侧先查一次重是挡不住的：两个任务可以都查到路径空闲、
	 * 先后创建、后者覆盖前者，而两边拿到的 material_path 一模一样。
	 * 查重必须和创建在同一条命令里。
	 *
	 * 开了这个开关，「创建成功」本身就等于「这个资产是本次新建的」,
	 * 调用方的删除授权才有真凭据。
	 */
	bool bFailIfExists = false;
	Payload->TryGetBoolField(TEXT("fail_if_exists"), bFailIfExists);

	// 3. 创建材质包
	FString PackagePath = DestinationPath / MaterialName;

	if (bFailIfExists)
	{
		// 查两处：磁盘上的包，和已经加载进内存的对象。
		// 只查其中一处都会漏 —— 新建但没存盘的资产不在磁盘上，
		// 存了盘但没加载的资产不在内存里。
		const FString ObjectPath = PackagePath + TEXT(".") + MaterialName;
		const bool bPackageOnDisk = FPackageName::DoesPackageExist(PackagePath);
		const bool bObjectInMemory = FindObject<UObject>(nullptr, *ObjectPath) != nullptr;

		if (bPackageOnDisk || bObjectInMemory)
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("material_path"), PackagePath);
			Details->SetBoolField(TEXT("package_on_disk"), bPackageOnDisk);
			Details->SetBoolField(TEXT("object_in_memory"), bObjectInMemory);
			UAL_CommandUtils::SendError(
				RequestId, 409,
				FString::Printf(
					TEXT("fail_if_exists: an asset already exists at %s. Nothing was created or modified."),
					*PackagePath),
				Details);
			return;
		}
	}

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create package for material"));
		return;
	}

	// 4. 创建 UMaterial
	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "CreateMaterial", "Create Material"));

	UMaterialFactoryNew* MaterialFactory = NewObject<UMaterialFactoryNew>();
	UMaterial* NewMaterial = Cast<UMaterial>(MaterialFactory->FactoryCreateNew(
		UMaterial::StaticClass(),
		Package,
		FName(*MaterialName),
		RF_Public | RF_Standalone | RF_Transactional,
		nullptr,
		GWarn
	));

	if (!NewMaterial)
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create UMaterial"));
		return;
	}

	// 标记新对象的修改
	NewMaterial->PreEditChange(nullptr);
	NewMaterial->Modify();

	// 5. 设置材质属性
	if (!BlendModeStr.IsEmpty())
	{
		EBlendMode BlendMode;
		if (ParseBlendMode(BlendModeStr, BlendMode))
		{
			NewMaterial->BlendMode = BlendMode;
		}
	}

	if (!ShadingModelStr.IsEmpty())
	{
		EMaterialShadingModel ShadingModel;
		if (ParseShadingModel(ShadingModelStr, ShadingModel))
		{
			NewMaterial->SetShadingModel(ShadingModel);
		}
	}

	NewMaterial->TwoSided = bTwoSided;

	// 6. 标记已修改并保存
	FPropertyChangedEvent PropertyChangedEvent(nullptr, EPropertyChangeType::ValueSet);
	NewMaterial->PostEditChangeProperty(PropertyChangedEvent);
	
	// 强制刷新材质编辑器图表和逻辑
	if (NewMaterial && NewMaterial->MaterialGraph)
	{
		UAL_SyncMaterialEditor(NewMaterial);
	}
	
	NewMaterial->MarkPackageDirty();

	// 通知资产注册表
	FAssetRegistryModule::AssetCreated(NewMaterial);

	// 仅通知属性面板刷新
	if (GEditor)
	{
		FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor");
		if (PropertyModule)
		{
			PropertyModule->NotifyCustomizationModuleChanged();
		}
	}

	// 7. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material_name"), NewMaterial->GetName());
	Data->SetStringField(TEXT("material_path"), NewMaterial->GetPathName());
	// GetPathName() 回的是**对象路径**（/Game/X/M.M），而调用方手里通常是
	// **包路径**（/Game/X/M）—— 两者直接字符串比会把每一次正常创建都判成
	// 「建错位置」。这里额外回一份包路径，省掉调用方自己切字符串。
	Data->SetStringField(TEXT("material_package_path"), NewMaterial->GetOutermost()->GetName());
	Data->SetStringField(TEXT("material_type"), TEXT("UMaterial"));
	Data->SetStringField(TEXT("blend_mode"), StaticEnum<EBlendMode>()->GetNameStringByValue((int64)NewMaterial->BlendMode));
	Data->SetBoolField(TEXT("two_sided"), NewMaterial->TwoSided);

	// 返回可用的材质引脚。第四份手写清单在这里 —— 同样走 UAL_CollectRootInputs，
	// 建材质时说有哪些引脚、读图时说有哪些、实际能连哪些，必须是同一份
	TArray<TSharedPtr<FJsonValue>> AvailablePins;
	for (const FUALRootInput& Root : UAL_CollectRootInputs(NewMaterial))
	{
		if (UAL_IsRootPinOffered(NewMaterial, Root.Name))
		{
			AvailablePins.Add(MakeShared<FJsonValueString>(Root.Name));
		}
	}
	Data->SetArrayField(TEXT("available_pins"), AvailablePins);
	Data->SetBoolField(TEXT("substrate_enabled"), UAL_IsSubstrateEnabled());

	UE_LOG(LogUALMaterial, Log, TEXT("Created UMaterial: %s"), *NewMaterial->GetName());

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}


// ============================================================================
// Handle_ApplyMaterial - 将材质应用到 Actor
// ============================================================================
void FUAL_MaterialCommands::Handle_ApplyMaterial(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	/**
	 * 1. 要改的槽位清单。
	 *
	 * 两种写法归一成同一份清单，后面只有一条路径：
	 *   - 单槽：`material_path` + `slot_index`（默认 0）或 `slot_name`
	 *   - 多槽：`slots: [{slot_index | slot_name, material_path}, ...]`
	 *
	 * 多槽是 2026-09-24 用户反馈要的：一个 Boss 15 个槽，两组槽各用一种材质、
	 * 其余 7 个透明槽必须原样保留。单槽接口只能连调 8 次，
	 * 而「全量写 OverrideMaterials」会把透明槽一起覆盖掉。没点名的槽一律不碰。
	 */
	struct FSlotRequest
	{
		/** 按索引点名时有效；按槽名点名时是 INDEX_NONE，到了具体网格上再解析 */
		int32 Index = INDEX_NONE;
		FName Name = NAME_None;
		UMaterialInterface* Material = nullptr;
		FString MaterialPath;
	};
	TArray<FSlotRequest> SlotRequests;

	// 槽位选择器：slot_index 和 slot_name 二选一。
	// bDefaultToZero 只给单槽的老写法用 —— 多槽清单里默认成 0 等于悄悄改掉 0 号槽
	auto ReadSlotSelector = [](const TSharedPtr<FJsonObject>& Obj, bool bDefaultToZero,
		FSlotRequest& Out, FString& OutError) -> bool
	{
		FString SlotName;
		const bool bHasName = Obj->TryGetStringField(TEXT("slot_name"), SlotName) && !SlotName.IsEmpty();
		double IndexNum = 0;
		const bool bHasIndex = Obj->TryGetNumberField(TEXT("slot_index"), IndexNum);
		if (bHasName && bHasIndex)
		{
			OutError = TEXT("give either slot_index or slot_name, not both");
			return false;
		}
		if (bHasName)
		{
			Out.Name = FName(*SlotName);
			return true;
		}
		if (!bHasIndex && !bDefaultToZero)
		{
			OutError = TEXT("each slot needs slot_index or slot_name");
			return false;
		}
		// 先按 double 判再转：小数会被悄悄截断成别的槽，超出 int32 的值转换是未定义行为
		if (IndexNum < 0 || IndexNum > MAX_int32 || FMath::FloorToDouble(IndexNum) != IndexNum)
		{
			OutError = FString::Printf(TEXT("slot_index must be a whole number >= 0, got %s"), *FString::SanitizeFloat(IndexNum));
			return false;
		}
		Out.Index = static_cast<int32>(IndexNum);
		return true;
	};

	// 2. 加载材质。任何一个找不到就整单拒绝 —— 此时还什么都没改
	auto LoadMaterial = [](const FString& RawPath, FSlotRequest& Out) -> bool
	{
		Out.MaterialPath = NormalizePath(RawPath);
		Out.Material = LoadObject<UMaterialInterface>(nullptr, *Out.MaterialPath);
		return Out.Material != nullptr;
	};

	FString MaterialPath;
	const bool bHasMaterialPath = Payload->TryGetStringField(TEXT("material_path"), MaterialPath) && !MaterialPath.IsEmpty();
	const TArray<TSharedPtr<FJsonValue>>* SlotsArray = nullptr;
	const bool bBatch = Payload->TryGetArrayField(TEXT("slots"), SlotsArray) && SlotsArray;

	if (bBatch)
	{
		// 顶层的 slot_index / slot_name 在多槽写法里没有落处，悄悄忽略就是少改一个槽还报成功
		if (bHasMaterialPath || Payload->HasField(TEXT("slot_index")) || Payload->HasField(TEXT("slot_name")))
		{
			UAL_CommandUtils::SendError(RequestId, 400, TEXT(
				"With slots, put every slot inside slots; do not also give top-level material_path, slot_index or slot_name"));
			return;
		}
		if (SlotsArray->Num() == 0)
		{
			UAL_CommandUtils::SendError(RequestId, 400, TEXT("slots is empty"));
			return;
		}
		for (int32 i = 0; i < SlotsArray->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>* EntryPtr = nullptr;
			if (!(*SlotsArray)[i].IsValid() || !(*SlotsArray)[i]->TryGetObject(EntryPtr) || !EntryPtr || !EntryPtr->IsValid())
			{
				UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
					TEXT("slots[%d] must be an object like {\"slot_index\": 1, \"material_path\": \"/Game/M\"}"), i));
				return;
			}
			FSlotRequest Req;
			FString SelectorError;
			if (!ReadSlotSelector(*EntryPtr, false, Req, SelectorError))
			{
				UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(TEXT("slots[%d]: %s"), i, *SelectorError));
				return;
			}
			FString EntryPath;
			if (!(*EntryPtr)->TryGetStringField(TEXT("material_path"), EntryPath) || EntryPath.IsEmpty())
			{
				UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(TEXT("slots[%d]: missing material_path"), i));
				return;
			}
			if (!LoadMaterial(EntryPath, Req))
			{
				UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(
					TEXT("slots[%d]: material not found: %s (nothing was changed)"), i, *Req.MaterialPath));
				return;
			}
			// 字面上重复的点名当场拒绝：同一个槽给两种材质，哪个算数都是猜。
			// 槽名和索引指向同一个槽的情况要到具体网格上才知道，在下面按 Actor 查
			for (const FSlotRequest& Prev : SlotRequests)
			{
				if ((Req.Index != INDEX_NONE && Req.Index == Prev.Index) ||
					(!Req.Name.IsNone() && Req.Name == Prev.Name))
				{
					UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
						TEXT("slots[%d] names the same slot as an earlier entry"), i));
					return;
				}
			}
			SlotRequests.Add(Req);
		}
	}
	else
	{
		if (!bHasMaterialPath)
		{
			UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: material_path (or slots)"));
			return;
		}
		FSlotRequest Req;
		FString SelectorError;
		if (!ReadSlotSelector(Payload, true, Req, SelectorError))
		{
			UAL_CommandUtils::SendError(RequestId, 400, SelectorError);
			return;
		}
		if (!LoadMaterial(MaterialPath, Req))
		{
			UAL_CommandUtils::SendError(RequestId, 404,
				FString::Printf(TEXT("Material not found: %s"), *Req.MaterialPath));
			return;
		}
		MaterialPath = Req.MaterialPath;
		SlotRequests.Add(Req);
	}

	// 3. 解析 targets 选择器
	const TSharedPtr<FJsonObject>* TargetsObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("targets"), TargetsObj) || !TargetsObj || !TargetsObj->IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: targets"));
		return;
	}

	// 4. 获取目标世界
	UWorld* World = UAL_CommandUtils::GetLiveWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	// 5. 解析 Actor
	TSet<AActor*> TargetSet;
	FString TargetError;
	TArray<FString> Unmatched;
	if (!UAL_CommandUtils::ResolveTargetsToActors(*TargetsObj, World, TargetSet, TargetError, &Unmatched))
	{
		UAL_CommandUtils::SendError(RequestId, 404, TargetError);
		return;
	}

	// 6. 解析可选参数
	// 指定组件名时只碰这一个；不给就按优先级挑一个（根组件 → 骨骼网格 → 其余，
	// 同档按名字定序）。不是「第一个」—— GetComponents 的顺序是哈希序，靠不住
	FString ComponentName;
	Payload->TryGetStringField(TEXT("component_name"), ComponentName);

	// 7. 应用材质到每个 Actor
	//
	// 这里原来一个事务都没有 —— 换材质因此完全不进撤销栈：ue_undo 撤不掉，
	// 用户按 Ctrl+Z 也撤不掉，而工具照样报成功。实测里的表现是
	// 「把我这一轮做的全撤了」之后，球身上的材质还挂着。
	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "ApplyMaterial", "Apply Material"));

	int32 AppliedCount = 0;
	TArray<TSharedPtr<FJsonValue>> ActorsJson;
	/**
	 * 跳过的也要回。以前跳过原因只进 UE_LOG，响应里一个字没有，凑不出一个成功
	 * 就回 404 空手 —— 盒子那头只能说「未提供失败原因」。真机上给蓝图金币换
	 * 材质就是这样：模型换了三种写法，每次拿到的都是同一句空话。
	 */
	/*
	 * 明细要**封顶**。
	 *
	 * 一次 filter 命中几百个 Actor 是常事，而每条原因里还会带上这个 Actor 的
	 * 组件名单。全量回去就是十几万字符直接糊进模型上下文（真机上一个 filter
	 * 回过 144 个 HLOD Actor）。跳过总数照实给，明细给前 20 条 —— 后面的
	 * 都是同一句话的复读，付不起那个 token。
	 */
	const int32 MaxSkippedDetails = 20;
	int32 SkippedCount = 0;
	TArray<TSharedPtr<FJsonValue>> SkippedJson;
	/*
	 * 逐条明细封了顶，但**按原因归类的汇总不封顶**（原因的种类就那么几种）。
	 *
	 * 光有前 20 条会骗人：前 20 个都是「没有网格组件」，第 21 个开始变成
	 * 「槽位越界」，调用方照着前 20 条会得出「剩下的都一样」——而那是错的。
	 * 归类汇总很短，又是完整的，正好补上这个洞，也正好能塞进 error 字符串。
	 *
	 * 归类按**种类**（`UALSkipKind` 里那几个常量），不按格式化好的整句：
	 * 五句原因里有四句带 `%s`（组件名单、类名、槽位数），拿整句当 key
	 * 就是每个 Actor 一个 key，汇总跟着 Actor 数一起涨 —— 200 个 Actor
	 * 几十 KB 塞进 error，而 error 那条路（404）恰恰没人能再裁剪。
	 * Kind 的类型是 `const TCHAR*`，所以往里传 `FString::Printf(...)` 编不过，
	 * 这条约束靠类型守着，不靠注释。
	 *
	 * 每一类同时留**一句完整的原因当样本**。只留种类的话，error 里就只剩
	 * 「no renderable mesh component (x3)」这种干巴巴的分类名，而真正有用的
	 * 那半句（「Blueprint assets are not actors: … use blueprint_set_property …」）
	 * 全丢了 —— 封顶是对的，封成只剩分类名是把有用的一起扔了。
	 * 样本按种类存，所以长度照样是封死的。
	 */
	struct FSkipKindStat
	{
		int32 Count = 0;
		FString Example;
	};
	TMap<FString, FSkipKindStat> SkipReasonCounts;
	auto Skip = [&](AActor* Actor, const TCHAR* Kind, const FString& Reason)
	{
		// Actor 可能已经失效（见下面那处）—— 名字取不到就说「取不到」，
		// 不能因为取不到名字就把这条跳过丢掉不报
		const FString Name = IsValid(Actor)
			? UAL_CommandUtils::GetActorFriendlyName(Actor)
			: FString(TEXT("(invalid actor)"));
		++SkippedCount;
		FSkipKindStat& Stat = SkipReasonCounts.FindOrAdd(Kind);
		++Stat.Count;
		if (Stat.Example.IsEmpty()) Stat.Example = Reason;
		if (SkippedJson.Num() < MaxSkippedDetails)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Name);
			// 路径也要给。标签不唯一，五个都叫 Cube 的时候，光有名字既分不清
			// 是哪两个没成，也没法拿 targets.paths 单独重试那两个 ——
			// 而「重试没成的那几个」正是成功名单那边加路径的理由，这半边更需要
			if (IsValid(Actor)) Entry->SetStringField(TEXT("path"), Actor->GetPathName());
			Entry->SetStringField(TEXT("reason"), Reason);
			SkippedJson.Add(MakeShared<FJsonValueObject>(Entry));
		}
		// 日志不封顶 —— 那是给人排查的，不进模型上下文
		UE_LOG(LogUALMaterial, Warning, TEXT("material.apply skipped %s: %s"), *Name, *Reason);
	};

	for (AActor* Actor : TargetSet)
	{
		/*
		 * 失效的 target 也要记一笔。
		 *
		 * `target_count` 数的是 TargetSet.Num()，直接 continue 掉的那个
		 * 既不在 applied 里也不在 skipped 里 —— 调用方看到「应用 3/10、跳过 6」，
		 * 剩下那个凭空消失，而这正是这套回报要消灭的东西。
		 *
		 * 判的是 `IsValid` 不是 `!Actor`：解析那一步每次插入都过了空指针检查，
		 * 集合里根本不会有 nullptr；真正会发生的是**匹配之后被删掉**，
		 * 那留下的是一个非空的 pending-kill 指针，`!Actor` 拦不住它，
		 * 下一行 `GetComponents()` 就崩了。
		 */
		if (!IsValid(Actor))
		{
			Skip(Actor, UALSkipKind::ActorInvalid,
				TEXT("target actor is no longer valid (destroyed since it was matched)"));
			continue;
		}

		/*
		 * 候选只收**真正渲染几何体**的网格组件（静态网格 / 骨骼网格及其子类）。
		 *
		 * `GetComponents<UMeshComponent>` 收得太宽：`UWidgetComponent` 也继承自
		 * `UMeshComponent`，而且它的 `GetNumMaterials()` 至少回 1，槽位 0 是存在的 ——
		 * 于是「只有一块血条」的 Actor 会被当成有网格，材质刷到 UI 面片上，
		 * 还报成功；连「没有网格组件」那条跳过都轮不到，因为血条**就是**一个
		 * UMeshComponent。以前只在排序里给它降权，压不住「它是唯一候选」这一种。
		 *
		 * 挑不出来时把它们的名字报出去：真要刷那块面片，`component_name` 点名照样能刷 ——
		 * 能力没变窄，变的是不再由我们替它猜。
		 */
		TArray<UMeshComponent*> AllMeshComponents;
		Actor->GetComponents<UMeshComponent>(AllMeshComponents);
		TArray<UMeshComponent*> MeshComponents;
		for (UMeshComponent* Candidate : AllMeshComponents)
		{
			if (UAL_IsPaintableMesh(Candidate)) MeshComponents.Add(Candidate);
		}

		UMeshComponent* Mesh = nullptr;
		if (!ComponentName.IsEmpty())
		{
			// 点名时在**全部**网格组件里找 —— 用户点了名就按用户说的来
			for (UMeshComponent* Candidate : AllMeshComponents)
			{
				if (Candidate && Candidate->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
				{
					Mesh = Candidate;
					break;
				}
			}
			if (!Mesh)
			{
				Skip(Actor, UALSkipKind::NoComponentWithName,
					FString::Printf(TEXT("no mesh component named '%s' (mesh components on this actor: %s)"),
						*ComponentName, *UAL_JoinNamesCapped(AllMeshComponents, 5)));
				continue;
			}
		}
		else
		{
			/*
			 * 不指定组件名时挑哪一个 —— 「第一个」这个说法本身是靠不住的：
			 * `AActor::OwnedComponents` 是 `TSet`，`GetComponents` 的顺序是哈希序，
			 * 不是声明序，同一个 Actor 两次跑可能挑中不同的组件。
			 *
			 * 挑法一共**三档**：根组件 → 骨骼网格 → 其余（静态网格和别的可刷网格）；
			 * 同一档里按名字定序。
			 *
			 * 两个点是踩出来的：
			 *   · 顺序不能写成「静态网格优先」—— 带武器的角色蓝图（骨骼网格的身体
			 *     + 静态网格的剑）会**稳定地**刷到剑上还报成功，比哈希序更糟。
			 *   · 比名字用大小写敏感比较。`FString::operator<` 是 Stricmp，
			 *     `Mesh` 和 `mesh` 会判成相等，那就又掉回哈希序 ——
			 *     这套定序本来就是为了消灭哈希序。
			 *
			 * 同档有好几个也**不拒绝**：模块化 Actor（门框+门板）、带
			 * InstancedStaticMeshComponent 的 Actor 都是同档多个，拒绝会让
			 * 「把这批物件都刷一遍」整批失败，而 component_name 一次只能点一个名字。
			 * 改成如实回报挑中了谁、旁边还有谁（component / other_components）。
			 *
			 * 一趟扫出最小值，不排序：只读 [0] 却排完整个数组，是拿 O(n log n) 次
			 * `Rank`（每次两个 Cast 加一次 GetRootComponent）和每次比较两个
			 * `GetName()` 堆分配去换一个最小值 —— 30 个组件的模块化 Actor 上
			 * 大约 300 次分配，一个 200 个 Actor 的 filter 就是六万次，全在游戏线程上。
			 */
			USceneComponent* const RootComponent = Actor->GetRootComponent();
			auto Rank = [RootComponent](UMeshComponent* Candidate) -> int32
			{
				if (Candidate == RootComponent) return 0;
				if (Cast<USkinnedMeshComponent>(Candidate)) return 1;
				return 2;
			};

			int32 BestRank = MAX_int32;
			FString BestName;
			for (UMeshComponent* Candidate : MeshComponents)
			{
				const int32 CandidateRank = Rank(Candidate);
				if (CandidateRank > BestRank) continue;
				const FString CandidateName = Candidate->GetName();
				if (CandidateRank < BestRank ||
					CandidateName.Compare(BestName, ESearchCase::CaseSensitive) < 0)
				{
					Mesh = Candidate;
					BestRank = CandidateRank;
					BestName = CandidateName;
				}
			}

			if (!Mesh)
			{
				Skip(Actor, UALSkipKind::NoPaintableMesh,
					FString::Printf(TEXT("no paintable mesh component (class %s; mesh-like components: %s). ")
						TEXT("Blueprint assets are not actors: to change a component's default material use ")
						TEXT("blueprint_set_property with component_name and OverrideMaterials. ")
						TEXT("To target a UI widget component name it explicitly with component_name."),
						Actor->GetClass() ? *Actor->GetClass()->GetName() : TEXT("?"),
						*UAL_JoinNamesCapped(AllMeshComponents, 5)));
				continue;
			}
		}

		// 验证 slot 索引
		const int32 NumMaterials = Mesh->GetNumMaterials();
		if (NumMaterials == 0)
		{
			Skip(Actor, UALSkipKind::NoMaterialSlots,
				FString::Printf(TEXT("mesh component '%s' has no material slots (no mesh assigned yet?)"), *Mesh->GetName()));
			continue;
		}
		/**
		 * 把每条请求落到这个网格的槽位上，**全部**核对通过才动手。
		 *
		 * 槽名按网格解析：同名槽在不同网格上的索引可以不一样。
		 * 有一条不成立（越界、槽名不存在、槽名和索引撞到同一个槽）就整个 Actor 跳过，
		 * 一个槽都不改 —— 半套材质比原样更难收拾，而且回执说不清哪些已经变了。
		 */
		// 槽名单要拷一份整表，只在用得上时取：多槽回执、按槽名点名、或者要报错时。
		// 单槽按索引刷几百个 Actor 的常见路径上不取
		const bool bReportSlotNames = bBatch || !SlotRequests[0].Name.IsNone();
		TArray<FName> SlotNames;
		bool bSlotNamesLoaded = false;
		auto EnsureSlotNames = [&]()
		{
			if (!bSlotNamesLoaded)
			{
				SlotNames = Mesh->GetMaterialSlotNames();
				bSlotNamesLoaded = true;
			}
		};
		TArray<int32> ResolvedIndices;
		TArray<FString> SlotProblems;
		const TCHAR* ProblemKind = nullptr;
		for (const FSlotRequest& Req : SlotRequests)
		{
			int32 Index = Req.Index;
			if (!Req.Name.IsNone())
			{
				Index = Mesh->GetMaterialIndex(Req.Name);
				if (Index == INDEX_NONE)
				{
					SlotProblems.Add(FString::Printf(TEXT("slot_name '%s' not found"), *Req.Name.ToString()));
					if (!ProblemKind) ProblemKind = UALSkipKind::SlotNameNotFound;
					ResolvedIndices.Add(INDEX_NONE);
					continue;
				}
			}
			else if (Index >= NumMaterials)
			{
				SlotProblems.Add(FString::Printf(TEXT("slot_index %d out of range"), Index));
				if (!ProblemKind) ProblemKind = UALSkipKind::SlotOutOfRange;
				ResolvedIndices.Add(INDEX_NONE);
				continue;
			}
			if (ResolvedIndices.Contains(Index))
			{
				SlotProblems.Add(FString::Printf(TEXT("slot %d is named twice (by slot_name and slot_index)"), Index));
				if (!ProblemKind) ProblemKind = UALSkipKind::DuplicateSlot;
			}
			ResolvedIndices.Add(Index);
		}
		if (SlotProblems.Num() > 0)
		{
			// 槽名单封顶，理由同 UAL_JoinNamesCapped：这句会进模型上下文
			EnsureSlotNames();
			TArray<FString> NameList;
			for (int32 i = 0; i < SlotNames.Num() && i < 20; ++i)
			{
				NameList.Add(FString::Printf(TEXT("%d=%s"), i, *SlotNames[i].ToString()));
			}
			const FString NameListText = NameList.Num() > 0
				? FString::Printf(TEXT(" [%s%s]"), *FString::Join(NameList, TEXT(", ")), SlotNames.Num() > 20 ? TEXT(", ...") : TEXT(""))
				: FString();
			Skip(Actor, ProblemKind, FString::Printf(
				TEXT("%s on '%s', which has %d slot(s)%s; nothing was changed on this actor"),
				*FString::Join(SlotProblems, TEXT(", ")), *Mesh->GetName(), NumMaterials, *NameListText));
			continue;
		}

		// 应用材质。
		//
		// Modify() 要打在**组件**上、而且要在改之前 —— 材质挂在组件上，不在 Actor 上。
		// 事务记录的是 Modify() 被调用那一刻的状态：打在 Actor 上记不到组件的改动，
		// 改完再打记下来的是新值，撤销回去等于没撤。两种写法都不会报错。
#if WITH_EDITOR
		Mesh->Modify();
#endif
		// 逐槽回旧值和新值：调用方拿它核对，不用再读一遍整张槽位表
		TArray<TSharedPtr<FJsonValue>> SlotsJson;
		if (bReportSlotNames)
		{
			EnsureSlotNames();
		}
		for (int32 i = 0; i < SlotRequests.Num(); ++i)
		{
			const int32 Index = ResolvedIndices[i];
			const UMaterialInterface* Previous = Mesh->GetMaterial(Index);
			Mesh->SetMaterial(Index, SlotRequests[i].Material);

			TSharedPtr<FJsonObject> SlotInfo = MakeShared<FJsonObject>();
			SlotInfo->SetNumberField(TEXT("slot_index"), Index);
			if (SlotNames.IsValidIndex(Index))
			{
				SlotInfo->SetStringField(TEXT("slot_name"), SlotNames[Index].ToString());
			}
			SlotInfo->SetStringField(TEXT("previous"), Previous ? Previous->GetPathName() : FString());
			SlotInfo->SetStringField(TEXT("material"), SlotRequests[i].MaterialPath);
			SlotsJson.Add(MakeShared<FJsonValueObject>(SlotInfo));

			UE_LOG(LogUALMaterial, Log, TEXT("Applied material %s to %s (%s) at slot %d"),
				*SlotRequests[i].Material->GetName(), *UAL_CommandUtils::GetActorFriendlyName(Actor), *Mesh->GetName(), Index);
		}

		AppliedCount++;

		// 构建响应
		TSharedPtr<FJsonObject> ActorInfo = MakeShared<FJsonObject>();
		ActorInfo->SetStringField(TEXT("name"), UAL_CommandUtils::GetActorFriendlyName(Actor));
		ActorInfo->SetStringField(TEXT("path"), Actor->GetPathName());
		ActorInfo->SetStringField(TEXT("component"), Mesh->GetName());
		if (!bBatch)
		{
			ActorInfo->SetNumberField(TEXT("slot_index"), ResolvedIndices[0]);
		}
		ActorInfo->SetArrayField(TEXT("slots"), SlotsJson);
		/*
		 * 这个 Actor 上**还有别的网格组件**时必须说出来。
		 *
		 * 没指定 component_name 时我们只刷了挑中的那一个，回的却是「1/1 成功」——
		 * 模块化的门（门框+门板）只刷了一半，读起来却是全做完了。调用方要能
		 * 发现这件事、并且知道下一次该把哪个名字填进 component_name，
		 * 光报挑中的那个名字是不够的。
		 *
		 * 报的是**滤过的** MeshComponents（排掉了 UI 组件），不是 AllMeshComponents：
		 * 把血条那类东西当成「你还可以刷这个」推给调用方没有意义，
		 * 而 component_name 那条路不挑类 —— 它照着名字回来一调，材质就落到 UI 面片上了。
		 */
		if (ComponentName.IsEmpty() && MeshComponents.Num() > 1)
		{
			ActorInfo->SetNumberField(TEXT("mesh_component_count"), MeshComponents.Num());
			ActorInfo->SetStringField(TEXT("other_components"),
				UAL_JoinNamesCapped(MeshComponents, 5, Mesh));
		}
		ActorsJson.Add(MakeShared<FJsonValueObject>(ActorInfo));
	}

	// 8. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("applied_count"), AppliedCount);
	Data->SetNumberField(TEXT("target_count"), TargetSet.Num());
	if (bBatch)
	{
		Data->SetNumberField(TEXT("slot_request_count"), SlotRequests.Num());
	}
	else
	{
		Data->SetStringField(TEXT("material_path"), MaterialPath);
	}
	Data->SetArrayField(TEXT("actors"), ActorsJson);
	/** `原因 ×N、原因 ×M` —— 完整、去重、很短，正文和 error 都用它 */
	/**
	 * 每一类给**一句完整的样本原因**加个数，不是只给分类名。
	 *
	 * 分类名（"no renderable mesh component"）只说得出「是哪一类」，
	 * 说不出「那接下来该干嘛」；而真正有用的下一步就写在完整原因的后半句里
	 * （「Blueprint assets are not actors: … 用 blueprint_set_property …」）。
	 * 404 那条路上模型只看得到 error 一个字段，把它压缩成分类名，
	 * 等于把这套回报唯一的价值扔了。
	 *
	 * 长度还是封死的：条数等于种类数（五个），不随 Actor 数涨。
	 */
	auto ReasonSummary = [&SkipReasonCounts]() -> FString
	{
		/*
		 * 按**种类名**排，不是按拼好的那句话排。
		 *
		 * 两层哈希序要分开看：
		 *
		 * 一层是 `SkipReasonCounts` 的遍历顺序。它的 key 是 `const TCHAR*`（这么定
		 * 是为了让「把 Printf 的结果传进来」编不过），而 TMap 对指针按**地址**哈希，
		 * 地址受 ASLR 影响 —— 同一批失败，这次「A; B」，下次「B; A」。
		 *
		 * 另一层藏在 `Example` 里：那句样本原因取自**第一个**撞上这一类的 Actor，
		 * 而主循环遍历的是 `TSet<AActor*> TargetSet`，同样是指针哈希序。五个 Actor
		 * 同一类失败，这次样本里的网格名是 Cube_Mesh，下次是 StaticMeshComponent0。
		 *
		 * 所以拿拼好的字符串当排序键是不够的 —— 键本身就会变。改成按 `Pair.Key` 排：
		 * 种类名是编译期常量，稳的。样本正文仍可能换一个 Actor，但那是同一类的
		 * 同一件事，模型不会再把它当成两个不同的问题各重试一遍。
		 */
		TArray<TPair<FString, FString>> Parts;
		for (const TPair<FString, FSkipKindStat>& Pair : SkipReasonCounts)
		{
			const FString& Text = Pair.Value.Example.IsEmpty()
				? FString(Pair.Key)
				: Pair.Value.Example;
			Parts.Emplace(Pair.Key, Pair.Value.Count > 1
				? FString::Printf(TEXT("%s (x%d)"), *Text, Pair.Value.Count)
				: Text);
		}
		Parts.Sort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B)
		{
			return FCString::Strcmp(*A.Key, *B.Key) < 0;
		});

		TArray<FString> Joined;
		Joined.Reserve(Parts.Num());
		for (const TPair<FString, FString>& Part : Parts) Joined.Add(Part.Value);
		return FString::Join(Joined, TEXT("; "));
	};

	if (SkippedCount > 0)
	{
		Data->SetNumberField(TEXT("skipped_count"), SkippedCount);
		Data->SetArrayField(TEXT("skipped"), SkippedJson);
		// 逐条明细封了顶，归类汇总没封 —— 「剩下的是不是同一个原因」只有它答得了
		Data->SetStringField(TEXT("skipped_summary"), ReasonSummary());
	}
	if (AppliedCount == 0)
	{
		/*
		 * 一个都没应用上就是失败，而失败这条路上**原因必须写进 error 本身**。
		 *
		 * 上一版把原因从 error 里拿掉、只留一句「去看 skipped」，理由是别让同一批
		 * 文字在上下文里出现两次。那个推理漏了一件事：这里 SendResponse 发的是
		 * 404，`services/websocket/server.ts` 会给响应体盖上 ok:false，
		 * 盒子那边 `assertRpcOk` 直接抛异常 —— toOutcome 根本不会跑，
		 * skipped 一个字都到不了模型手上。于是「去看 skipped」变成一句空话，
		 * 正好退回这段代码最初要修的那个 bug。
		 *
		 * 用归类汇总而不是逐条，两个目的都能达到：原因在 error 里，长度还是有界的。
		 */
		Data->SetStringField(TEXT("error"),
			FString::Printf(TEXT("Material was not applied to any of the %d target(s). Reasons: %s"),
				TargetSet.Num(), SkippedCount > 0 ? *ReasonSummary() : TEXT("(none recorded)")));
	}

	UAL_CommandUtils::AddUnmatchedTargets(Data, Unmatched);
	UAL_CommandUtils::AddWorldInfo(Data);
	UAL_CommandUtils::SendResponse(RequestId, AppliedCount > 0 ? 200 : 404, Data);
}

// ============================================================================
// Handle_DescribeMaterial - 获取材质详细信息
// ============================================================================
void FUAL_MaterialCommands::Handle_DescribeMaterial(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数: path
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	// 2. 加载材质
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Material)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		return;
	}

	/*
	 * **照资产读，不要改读编辑器那份工作副本。**
	 *
	 * 「读副本」看着更贴近现场（add_node 加的参数确实只在副本上），可它会制造
	 * 两处对不上，而两处都是静默的：
	 *
	 * - `Cast<UMaterial>` 对材质实例不成立，实例这条路只能顺 Parent 走到**资产**。
	 *   于是 describe(M_Glass) 说 Translucent、describe(MI_Glass) 说 Opaque，
	 *   同一张材质两个答案，调用方没有任何办法判断哪个算数。
	 * - `material.set_param` 的参数表是从**保存过的父材质**推出来的，而且它不应用副本。
	 *   describe 报着副本里的 Metallic，set_param 回 400「没有这个参数」——
	 *   而 describe 的工具说明恰好写着「material_set_param 只认这里列出来的参数名」。
	 *
	 * 所以这里退回资产，两处重新一致；副本和资产不一样这件事改成**说出来**
	 * （下面的 has_open_material_editor / editor_note）。只读命令的本分是报准，
	 * 不是替用户把没保存的东西落盘。
	 */
	const bool bHasOpenMaterialEditor = UAL_FindMaterialEditor(Cast<UMaterial>(Material)) != nullptr;
	UMaterialInterface* ReadMaterial = Material;

	// 3. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), Material->GetName());
	Data->SetStringField(TEXT("path"), Material->GetPathName());
	Data->SetStringField(TEXT("class"), Material->GetClass()->GetName());

	// 读的是磁盘上那份。开着编辑器时必须说一句，否则调用方把这份回执
	// 当成「我刚才改的生效了没有」的答案，而它答的是另一个问题
	if (bHasOpenMaterialEditor)
	{
		Data->SetBoolField(TEXT("has_open_material_editor"), true);
		Data->SetStringField(TEXT("editor_note"),
			TEXT("This material is open in the Material Editor, and everything below is read from the asset "
				 "on disk. Graph edits made through this plugin land in that editor window first, so they "
				 "are NOT reflected here until ue_save (or the user pressing Apply). Use material_get_graph "
				 "to see the live graph."));
	}

	// 4. 如果是 MaterialInstance，获取父材质
	UMaterialInstance* MatInst = Cast<UMaterialInstance>(Material);
	if (MatInst && MatInst->Parent)
	{
		Data->SetStringField(TEXT("parent_material"), MatInst->Parent->GetPathName());
	}

	// 4b. 资产级属性。
	//
	// 这里原来只给参数三件套，不给混合模式 / 着色模型 / 双面 —— 而 material.create
	// 恰恰是设这三个的，「改之前先确认现状」因此落不了地。
	// 实例没有自己的这些属性，跟父材质走，所以取 GetMaterial()。
	if (UMaterial* BaseMaterial = ReadMaterial->GetMaterial())
	{
		Data->SetStringField(TEXT("blend_mode"),
			StaticEnum<EBlendMode>()->GetNameStringByValue((int64)BaseMaterial->BlendMode));
		// UMaterial::ShadingModel 在 5.5 起是 private，只能走 GetShadingModels()。
		// 那是个位域（Break/MakeMaterialAttributes 的图可以同时有好几个），
		// 报第一个命中的就够了 —— 这套工具只会一次设一个
		const FMaterialShadingModelField ShadingModels = BaseMaterial->GetShadingModels();
		for (int32 i = 0; i < MSM_NUM; ++i)
		{
			const EMaterialShadingModel Model = (EMaterialShadingModel)i;
			if (ShadingModels.HasShadingModel(Model))
			{
				Data->SetStringField(TEXT("shading_model"),
					StaticEnum<EMaterialShadingModel>()->GetNameStringByValue((int64)i));
				break;
			}
		}
		Data->SetBoolField(TEXT("two_sided"), BaseMaterial->TwoSided);

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		Data->SetNumberField(TEXT("node_count"), BaseMaterial->GetExpressions().Num());
#else
		Data->SetNumberField(TEXT("node_count"), BaseMaterial->Expressions.Num());
#endif
	}

	// 5. 获取材质参数
	TArray<FMaterialParameterInfo> ScalarInfos;
	TArray<FGuid> ScalarGuids;
	ReadMaterial->GetAllScalarParameterInfo(ScalarInfos, ScalarGuids);

	TArray<FMaterialParameterInfo> VectorInfos;
	TArray<FGuid> VectorGuids;
	ReadMaterial->GetAllVectorParameterInfo(VectorInfos, VectorGuids);

	TArray<FMaterialParameterInfo> TextureInfos;
	TArray<FGuid> TextureGuids;
	ReadMaterial->GetAllTextureParameterInfo(TextureInfos, TextureGuids);

	// 6. 构建标量参数列表
	TArray<TSharedPtr<FJsonValue>> ScalarParams;
	for (const FMaterialParameterInfo& Info : ScalarInfos)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Info.Name.ToString());
		
		float Value = 0.0f;
		if (ReadMaterial->GetScalarParameterValue(Info, Value))
		{
			ParamObj->SetNumberField(TEXT("value"), Value);
		}
		ScalarParams.Add(MakeShared<FJsonValueObject>(ParamObj));
	}
	Data->SetArrayField(TEXT("scalar_params"), ScalarParams);

	// 7. 构建向量参数列表
	TArray<TSharedPtr<FJsonValue>> VectorParams;
	for (const FMaterialParameterInfo& Info : VectorInfos)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Info.Name.ToString());
		
		FLinearColor Value;
		if (ReadMaterial->GetVectorParameterValue(Info, Value))
		{
			TSharedPtr<FJsonObject> ColorObj = MakeShared<FJsonObject>();
			ColorObj->SetNumberField(TEXT("r"), Value.R);
			ColorObj->SetNumberField(TEXT("g"), Value.G);
			ColorObj->SetNumberField(TEXT("b"), Value.B);
			ColorObj->SetNumberField(TEXT("a"), Value.A);
			ParamObj->SetObjectField(TEXT("value"), ColorObj);
		}
		VectorParams.Add(MakeShared<FJsonValueObject>(ParamObj));
	}
	Data->SetArrayField(TEXT("vector_params"), VectorParams);

	// 8. 构建贴图参数列表
	TArray<TSharedPtr<FJsonValue>> TextureParams;
	for (const FMaterialParameterInfo& Info : TextureInfos)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Info.Name.ToString());
		
		UTexture* Texture = nullptr;
		if (ReadMaterial->GetTextureParameterValue(Info, Texture) && Texture)
		{
			ParamObj->SetStringField(TEXT("value"), Texture->GetPathName());
		}
		else
		{
			ParamObj->SetStringField(TEXT("value"), TEXT(""));
		}
		TextureParams.Add(MakeShared<FJsonValueObject>(ParamObj));
	}
	Data->SetArrayField(TEXT("texture_params"), TextureParams);

	UE_LOG(LogUALMaterial, Log, TEXT("Described material: %s (Scalars: %d, Vectors: %d, Textures: %d)"),
		*Material->GetName(), ScalarInfos.Num(), VectorInfos.Num(), TextureInfos.Num());

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

namespace
{
	/**
	 * 把一个 `[{name/param, error, valid_values?}, …]` 数组拍成一行人话。
	 *
	 * 400 的时候这串字就是模型能看到的**全部**内容：`assertRpcOk` 在 toOutcome
	 * 之前就抛了，结构化字段一个都到不了渲染器。所以两件事都得带上 ——
	 * 哪条失败了（name + error），以及**该写什么才对**（valid_values）。
	 * 少了后半句，模型只能换个拼法再猜一遍。
	 *
	 * set_param 和 set_property 原来各抄了一份，只差键名。两份已经开始分岔
	 * （一份会带 valid_values，一份不会），所以并成一个。
	 *
	 * @param NameKey 条目里装名字的键：set_param 是 "param"，set_property 是 "name"
	 */
	FString UAL_JoinJsonErrors(const TArray<TSharedPtr<FJsonValue>>& Entries, const TCHAR* NameKey)
	{
		TArray<FString> Parts;
		for (const TSharedPtr<FJsonValue>& Entry : Entries)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(Obj) || !Obj) continue;

			FString Name, Reason;
			(*Obj)->TryGetStringField(NameKey, Name);
			(*Obj)->TryGetStringField(TEXT("error"), Reason);

			FString Line = FString::Printf(TEXT("%s: %s"), *Name, *Reason);

			// 合法值列表是这条消息里唯一能照着做的部分，别丢
			const TArray<TSharedPtr<FJsonValue>>* Valid = nullptr;
			if ((*Obj)->TryGetArrayField(TEXT("valid_values"), Valid) && Valid && Valid->Num() > 0)
			{
				TArray<FString> ValidText;
				for (const TSharedPtr<FJsonValue>& V : *Valid)
				{
					FString One;
					if (V.IsValid() && V->TryGetString(One)) ValidText.Add(One);
				}
				if (ValidText.Num() > 0)
				{
					Line += FString::Printf(TEXT(" (valid: %s)"), *FString::Join(ValidText, TEXT(", ")));
				}
			}
			Parts.Add(Line);
		}
		return FString::Join(Parts, TEXT("; "));
	}
}

// ============================================================================
// Handle_SetMaterialParam - 设置材质参数
// ============================================================================
void FUAL_MaterialCommands::Handle_SetMaterialParam(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数: path
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	// 2. 加载材质（必须是 MaterialInstanceConstant）
	UMaterialInstanceConstant* MatInst = LoadObject<UMaterialInstanceConstant>(nullptr, *MaterialPath);
	if (!MatInst)
	{
		UAL_CommandUtils::SendError(RequestId, 400, 
			FString::Printf(TEXT("Material must be a MaterialInstanceConstant: %s"), *MaterialPath));
		return;
	}

	// 3. 解析 params 对象
	const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("params"), ParamsObj) || !ParamsObj || !ParamsObj->IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: params"));
		return;
	}

	// 4. 先把父材质**真的有**哪些参数列出来。
	//
	// 引擎的 SetXxxParameterValueEditorOnly 不校验名字：找不到就往实例里
	// 新建一条记录，然后一切正常返回。于是把 "Roughness" 写成 "Roughnes"
	// 的结果是「更新成功」加一个纹丝不动的画面，没有任何线索。
	auto CollectNames = [MatInst](
		void (UMaterialInterface::*Getter)(TArray<FMaterialParameterInfo>&, TArray<FGuid>&) const) -> TMap<FString, FName>
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Guids;
		(MatInst->*Getter)(Infos, Guids);

		TMap<FString, FName> ByLowerName;
		for (const FMaterialParameterInfo& Info : Infos)
		{
			ByLowerName.Add(Info.Name.ToString().ToLower(), Info.Name);
		}
		return ByLowerName;
	};

	const TMap<FString, FName> ScalarNames = CollectNames(&UMaterialInterface::GetAllScalarParameterInfo);
	const TMap<FString, FName> VectorNames = CollectNames(&UMaterialInterface::GetAllVectorParameterInfo);
	const TMap<FString, FName> TextureNames = CollectNames(&UMaterialInterface::GetAllTextureParameterInfo);

	/*
	 * 写之前开事务 —— 这个 handler 以前是这个文件里**唯一**没开的那个。
	 *
	 * `SetXxxParameterValueEditorOnly` 自己不 Modify，而 Agent 的撤销缓冲区
	 * 按 `UAL_ScopedTransaction.h` 的写法只在事务存续期间挂上去。两件事一叠：
	 * 改完参数 `ue_undo` 和 Ctrl+Z 都一声不吭地什么也不做，而工具报的是成功。
	 * 同文件里另外十六个 handler（create / apply / add_node / set_property …）
	 * 全都开了事务，这里是漏的，不是有意为之。
	 *
	 * 位置卡在所有 400 分支**之后**：参数对象都没解析出来就开事务，等于往撤销
	 * 栈上压一条什么也没干的记录，用户按一次 Ctrl+Z 会以为撤掉了别的东西。
	 */
	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "SetMaterialParam", "Set Material Parameters"));
	/*
	 * `Modify()` 之前先确保 `RF_Transactional`，但**只在真的缺这个标记时才动它**。
	 *
	 * 为什么要补：没有这个标记 `Modify()` 什么都不记，事务是空的，
	 * `ue_undo` / Ctrl+Z 照样一声不吭 —— 这个 handler 本来要修的就是这件事。
	 * 而缺标记的资产真的存在：`UAL_ModifyMaterialGraph` 的注释写着「早期版本的
	 * material.create 建材质时漏了这个标记，那批资产至今还在工程里」。
	 * 那个 helper 只吃 `UMaterial*`，这里是 `UMaterialInstanceConstant*`，用不了。
	 *
	 * 为什么要加 `HasAnyFlags` 判断：`RF_Transactional` 属于 `RF_Load`，是**要存进
	 * 包里**的，而 `SetFlags` 本身不进事务、撤不回来。无条件写的话，一次
	 * 「参数名全拼错、什么都没设上」的 400 调用也会把用户资产的标记改掉，
	 * 并在他下次保存时落盘。只在缺的时候补，正常资产一个字节都不动。
	 */
	if (!MatInst->HasAnyFlags(RF_Transactional))
	{
		MatInst->SetFlags(RF_Transactional);
	}
	// `Modify()` 默认 bAlwaysMarkDirty=true，会**在校验之前**就把包弄脏，而 Cancel
	// 不回滚也就清不掉它。传 false：事务照样记录（撤销能用），脏标记留给真写成的那条路打
	MatInst->Modify(/*bAlwaysMarkDirty=*/false);

	// 5. 遍历并设置参数
	TArray<FString> UpdatedParams;
	TArray<TSharedPtr<FJsonValue>> Errors;

	auto AddError = [&Errors](const FString& Name, const FString& Reason)
	{
		TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
		Err->SetStringField(TEXT("param"), Name);
		Err->SetStringField(TEXT("error"), Reason);
		Errors.Add(MakeShared<FJsonValueObject>(Err));
	};

	for (const auto& Pair : (*ParamsObj)->Values)
	{
		const FString ParamName = UAL_JsonKey(Pair.Key);
		const FString LowerName = ParamName.ToLower();

		// 值的解析口径和 set_node_value / add_node 完全一致 ——
		// 这里原来是第三份各写各的实现，所以 {x,y,z} 会被当成 r/g/b 全缺省而设成纯黑、
		// 数组直接「无法识别」、布尔被 TryGetNumber 抢走当成标量
		FUALMaterialValue Value;
		FString ParseError;
		if (!ParseMaterialValue(Pair.Value, Value, ParseError))
		{
			AddError(ParamName, ParseError);
			continue;
		}

		if (const FName* Found = ScalarNames.Find(LowerName))
		{
			if (!Value.bHasScalar)
			{
				AddError(ParamName, TEXT("scalar parameter needs a number or boolean"));
				continue;
			}
			MatInst->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(*Found), Value.Scalar);
			UpdatedParams.Add(Found->ToString());
		}
		else if (const FName* FoundVector = VectorNames.Find(LowerName))
		{
			if (!Value.bHasColor)
			{
				AddError(ParamName, TEXT("vector parameter needs {r,g,b,a?}, {x,y,z?,w?}, a 2-4 element array or a number"));
				continue;
			}
			MatInst->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(*FoundVector), Value.Color);
			UpdatedParams.Add(FoundVector->ToString());
		}
		else if (const FName* FoundTexture = TextureNames.Find(LowerName))
		{
			if (!Value.bHasString)
			{
				AddError(ParamName, TEXT("texture parameter needs an asset path string"));
				continue;
			}
			UTexture* Texture = LoadObject<UTexture>(nullptr, *Value.String);
			if (!Texture)
			{
				AddError(ParamName, FString::Printf(TEXT("Texture not found: %s"), *Value.String));
				continue;
			}
			MatInst->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(*FoundTexture), Texture);
			UpdatedParams.Add(FoundTexture->ToString());
		}
		else
		{
			// 名字不存在时把**实际有哪些**列出来。只说「参数不存在」的话，
			// 调用方只能换个拼法反复猜
			TArray<FString> Known;
			for (const auto& Entry : ScalarNames) Known.Add(Entry.Value.ToString());
			for (const auto& Entry : VectorNames) Known.Add(Entry.Value.ToString());
			for (const auto& Entry : TextureNames) Known.Add(Entry.Value.ToString());
			AddError(ParamName, FString::Printf(
				TEXT("no such parameter on this material. Available: %s"),
				Known.Num() ? *FString::Join(Known, TEXT(", ")) : TEXT("(none - the parent material exposes no parameters)")));
		}
	}

	/*
	 * 6. 保存更改 —— 一条都没设上的话，这三件事**一件都不做**。
	 *
	 * `Cancel()` 只是把这笔事务从撤销栈上弹掉（`UTransBuffer::Cancel` 就是一个
	 * `UndoBuffer.Pop`），它**不回滚**已经发生的改动。所以脏标记不能靠它清 ——
	 * 上面那个 `Modify(false)` 就是为此才不打脏标记的，脏由这条写成功的路自己打。
	 * 一条都没设上时这里什么也不做：资产干干净净，没有假的「未保存改动」星号，
	 * 也不用为零改动跑一遍 `UMaterialInstance::PostEditChange` 的静态排列更新。
	 *
	 * 三屏之外的 `Handle_SetMaterialProperty` 就是把这两句一起关在
	 * `UpdatedProperties.Num() > 0` 里的，照它来。
	 */
	if (UpdatedParams.Num() > 0)
	{
		MatInst->PostEditChange();
		MatInst->MarkPackageDirty();
	}
	else
	{
		Transaction.Cancel();
	}

	// 7. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> UpdatedArray;
	for (const FString& Name : UpdatedParams)
	{
		UpdatedArray.Add(MakeShared<FJsonValueString>(Name));
	}
	Data->SetArrayField(TEXT("updated_params"), UpdatedArray);
	Data->SetArrayField(TEXT("errors"), Errors);
	Data->SetStringField(TEXT("material_path"), MaterialPath);

	/*
	 * 400 的时候**必须**再往顶层写一个 `error`。
	 *
	 * 盒子那边 `assertRpcOk` 只认顶层的 `error` / `message`，认不到就抛
	 * 「未提供失败原因」；而 `errors` 是个数组，既不是 `error` 也不是 `message`，
	 * 连 websocket 那层的兜底（error 缺失时从 message 抄）都接不上。
	 * 结果是：插件明明已经算出「参数名拼错了，可用的是 Roughness/Metallic/…」，
	 * 一个字都传不到模型手上，而且它也不知道另外两个其实设上了。
	 */
	if (Errors.Num() > 0)
	{
		Data->SetStringField(TEXT("error"),
			FString::Printf(TEXT("%d of %d parameter(s) failed: %s%s"),
				Errors.Num(), Errors.Num() + UpdatedParams.Num(),
				*UAL_JoinJsonErrors(Errors, TEXT("param")),
				UpdatedParams.Num() > 0
					? *FString::Printf(TEXT(". Applied: %s"), *FString::Join(UpdatedParams, TEXT(", ")))
					: TEXT("")));
	}

	UE_LOG(LogUALMaterial, Log, TEXT("Set %d parameters on material %s (%d failed)"),
		UpdatedParams.Num(), *MatInst->GetName(), Errors.Num());

	// 有任何一条没设上就回 400 —— 部分成功当成功报，等于把失败藏起来
	UAL_CommandUtils::SendResponse(RequestId, Errors.Num() == 0 ? 200 : 400, Data);
}

// ============================================================================
// Handle_GetMaterialGraph - 获取材质图表结构
// ============================================================================
void FUAL_MaterialCommands::Handle_GetMaterialGraph(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数: path
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	// 2. 加载材质（必须是 UMaterial，不能是 Instance）
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialPath);
	if (!Material)
	{
		// 尝试作为 MaterialInstance 加载并提示
		UMaterialInstance* MatInst = LoadObject<UMaterialInstance>(nullptr, *MaterialPath);
		if (MatInst)
		{
			UAL_CommandUtils::SendError(RequestId, 400, 
				TEXT("Cannot get graph from MaterialInstance. Use the parent Material path instead."));
			return;
		}
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		return;
	}

	/*
	 * 编辑器开着时，读的也要是它正在编的那一份。
	 *
	 * 写入已经改到副本上了（见 UAL_ResolveLiveMaterial），读这边如果还读资产，
	 * 就会出现「刚加的节点读不到」这种更难查的错位。读写必须盯同一个对象，
	 * 而且这样读到的就是用户屏幕上看到的东西。
	 */
	IMaterialEditor* GraphEditor = nullptr;
	Material = UAL_ResolveLiveMaterial(Material, GraphEditor);

	// 3. 解析可选参数
	bool bIncludeValues = true;
	Payload->TryGetBoolField(TEXT("include_values"), bIncludeValues);

	// 4. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material_path"), Material->GetPathName());
	Data->SetStringField(TEXT("material_name"), Material->GetName());

	// 材质主节点（那个「Material」输出节点）的位置。
	// 它不是 UMaterialExpression，不在 nodes 里，坐标存在材质自己身上。
	// 排版要靠它定锚点 —— 材质图是从右往左读的，所有表达式都排在它左边
	TSharedPtr<FJsonObject> RootPos = MakeShared<FJsonObject>();
	RootPos->SetNumberField(TEXT("x"), Material->EditorX);
	RootPos->SetNumberField(TEXT("y"), Material->EditorY);
	Data->SetObjectField(TEXT("material_node_position"), RootPos);

	// 5. 遍历所有材质表达式节点
	TArray<TSharedPtr<FJsonValue>> NodesJson;
	TArray<TSharedPtr<FJsonValue>> ConnectionsJson;
	TMap<UMaterialExpression*, FString> ExpressionToId;

	// 先把 id 全编好再遍历。命名重定向的 usage 要指向它的 declaration，
	// 而 declaration 可能排在后面 —— 边走边编号就指不过去
	UAL_BuildExpressionIds(Material, ExpressionToId);

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	for (UMaterialExpression* Expression : Material->GetExpressions())
#else
	for (UMaterialExpression* Expression : Material->Expressions)
#endif
	{
		if (!Expression) continue;

		const FString* FoundId = ExpressionToId.Find(Expression);
		FString NodeId = FoundId ? *FoundId : Expression->GetName();

		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("node_id"), NodeId);
		NodeObj->SetStringField(TEXT("class"), Expression->GetClass()->GetName());
		NodeObj->SetStringField(TEXT("display_name"), Expression->GetName());
		// 稳定 id。node_id 是数组下标派生的，删一个节点后面全体位移；
		// guid 跟着对象走，删改撤销都不变，改图前后隔了几步就该用它
		NodeObj->SetStringField(TEXT("guid"), Expression->GetMaterialExpressionId().ToString());

		// 位置
		TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
		PosObj->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
		PosObj->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);
		NodeObj->SetObjectField(TEXT("position"), PosObj);

		// 描述（如果有）
		if (!Expression->Desc.IsEmpty())
		{
			NodeObj->SetStringField(TEXT("description"), Expression->Desc);
		}

		// 这个节点有哪些输入引脚，各自连没连。
		//
		// 不给这个的话，调用方只能靠猜引脚名去调 connect_pins ——
		// 猜错拿一个 400，再换一个名字接着猜。Sine 的输入叫 Input、
		// Lerp 的叫 A/B/Alpha，没有通用规律，只能从图里读。
		TArray<TSharedPtr<FJsonValue>> InputsJson;
		const int32 NodeInputCount = UALCompat::CountInputs(Expression);
		for (int32 i = 0; i < NodeInputCount; ++i)
		{
			FExpressionInput* Input = Expression->GetInput(i);
			const FString InputName = UAL_InputPinName(Expression, i);

			TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
			InputObj->SetStringField(TEXT("name"), InputName);
			InputObj->SetBoolField(TEXT("is_connected"), Input && Input->Expression != nullptr);
			const FString InputType = UAL_MaterialTypeName(UAL_InputTypeOf(Expression, i));
			if (!InputType.IsEmpty())
			{
				InputObj->SetStringField(TEXT("type"), InputType);
			}
			InputsJson.Add(MakeShared<FJsonValueObject>(InputObj));
		}
		NodeObj->SetArrayField(TEXT("inputs"), InputsJson);

		// 输出引脚。多输出节点（BreakMaterialAttributes、TextureSample 的 RGB/R/G/B/A、
		// 各种材质函数）不给这个就没法正确填 connect_pins 的 source_pin
		TArray<TSharedPtr<FJsonValue>> OutputsJson;
		const TArray<FExpressionOutput>& NodeOutputs = Expression->GetOutputs();
		for (int32 i = 0; i < NodeOutputs.Num(); ++i)
		{
			TSharedPtr<FJsonObject> OutputObj = MakeShared<FJsonObject>();
			OutputObj->SetStringField(TEXT("name"), UAL_OutputPinName(Expression, i));
			OutputObj->SetNumberField(TEXT("index"), i);
			const FString OutputType = UAL_MaterialTypeName(UAL_OutputTypeOf(Expression, i));
			if (!OutputType.IsEmpty())
			{
				OutputObj->SetStringField(TEXT("type"), OutputType);
			}
			OutputsJson.Add(MakeShared<FJsonValueObject>(OutputObj));
		}
		NodeObj->SetArrayField(TEXT("outputs"), OutputsJson);

		// 命名重定向：图上是两个孤立节点，实际是一根线的两头。
		//
		// 不给配对信息的话，顺着连线往回追到一个 usage 就断了 —— 而复杂材质
		// 恰恰到处用它来避免连线横跨整张图。这里把两头都标出来，
		// usage 直接给出对应 declaration 的 node_id，追链不用再猜。
		if (UMaterialExpressionNamedRerouteDeclaration* Declaration =
				Cast<UMaterialExpressionNamedRerouteDeclaration>(Expression))
		{
			NodeObj->SetStringField(TEXT("reroute_kind"), TEXT("declaration"));
			NodeObj->SetStringField(TEXT("reroute_name"), Declaration->Name.ToString());
		}
		else if (UMaterialExpressionNamedRerouteUsage* Usage =
					Cast<UMaterialExpressionNamedRerouteUsage>(Expression))
		{
			NodeObj->SetStringField(TEXT("reroute_kind"), TEXT("usage"));
			if (Usage->Declaration)
			{
				NodeObj->SetStringField(TEXT("reroute_name"), Usage->Declaration->Name.ToString());
				const FString* DeclarationId = ExpressionToId.Find(Usage->Declaration);
				NodeObj->SetStringField(TEXT("reroute_declaration_node"),
					DeclarationId ? *DeclarationId : Usage->Declaration->GetName());
			}
			else
			{
				// 断掉的 usage 编译时会报错，读图时先说出来
				NodeObj->SetStringField(TEXT("reroute_error"),
					TEXT("declaration missing (this usage points at nothing)"));
			}
		}

		// include_values 以前被解析出来就再也没用过 —— 于是改完值没法读回来验证，
		// 接手一份已有材质也不知道每个常量现在是多少
		if (bIncludeValues)
		{
			if (TSharedPtr<FJsonValue> NodeValue = ReadNodeValue(Expression))
			{
				NodeObj->SetField(TEXT("value"), NodeValue);
			}

			// 只列和默认值不同的设置（SubSurfaceType、bUseParameterBlending……），
			// 全列的话每个节点都拖一长串默认值
			TSharedPtr<FJsonObject> Current = UAL_NodeEditableProperties(Expression);
			TSharedPtr<FJsonObject> Defaults = UAL_NodeEditableProperties(Expression->GetClass()->GetDefaultObject<UMaterialExpression>());
			TSharedPtr<FJsonObject> Changed = MakeShared<FJsonObject>();
			for (const FString& Key : UAL_JsonKeys(Current))
			{
				const TSharedPtr<FJsonValue> Value = Current->TryGetField(Key);
				const TSharedPtr<FJsonValue> Default = Defaults->TryGetField(Key);
				if (Value.IsValid() && (!Default.IsValid() || !FJsonValue::CompareEqual(*Value, *Default)))
				{
					Changed->SetField(Key, Value);
				}
			}
			if (Changed->Values.Num() > 0)
			{
				NodeObj->SetObjectField(TEXT("properties"), Changed);
			}
		}

		NodesJson.Add(MakeShared<FJsonValueObject>(NodeObj));
	}

	Data->SetArrayField(TEXT("nodes"), NodesJson);
	Data->SetNumberField(TEXT("node_count"), NodesJson.Num());

	// 6. 材质主节点可用引脚。
	//
	// 同样走 UAL_CollectRootInputs —— 这里手写过一份，比 connect_pins 认的多两个，
	// 于是读图说有 SubsurfaceColor、连的时候回 400 说没有。
	// 「可用引脚」这张清单和「实际能连的引脚」必须是同一份，否则就是在骗调用方。
	//
	// 例外是 connect_pins 会拒的那些，这里也不列：没开 Substrate 时的 FrontMaterial，
	// 开了 Substrate 时被引擎忽略的旧引脚（BaseColor 那一排）。
	// 已经接在它们上面的线照样出现在下面的 connections 里 —— 读图要如实。
	const bool bSubstrateEnabled = UAL_IsSubstrateEnabled();
	TArray<TSharedPtr<FJsonValue>> MaterialPins;
	for (const FUALRootInput& Root : UAL_CollectRootInputs(Material))
	{
		if (!UAL_IsRootPinOffered(Material, Root.Name))
		{
			continue;
		}
		MaterialPins.Add(MakeShared<FJsonValueString>(Root.Name));
	}
	Data->SetArrayField(TEXT("material_pins"), MaterialPins);

	// Substrate 开着时着色只看 FrontMaterial；FrontMaterial 空着，引擎才拿
	// BaseColor 那一排自动转换。调用方得先知道是哪种模式，才知道该往哪连
	Data->SetBoolField(TEXT("substrate_enabled"), bSubstrateEnabled);

	// 「使用材质属性」开着的时候，主节点上那一排引脚全是灰的，
	// 唯一有效的输入是 MaterialAttributes 那一根。
	//
	// 不说这件事的后果很具体：调用方看到 BaseColor 在 material_pins 里，
	// 就往上面连线 —— 连得上、不报错、编译也过，画面一点变化都没有。
	Data->SetBoolField(TEXT("use_material_attributes"), Material->bUseMaterialAttributes != 0);

	// 7. 真实连接关系。
	//
	// 这里原来硬编码 connection_count = 0、connections = []，不管图里实际连了
	// 多少根线。那比不给这两个字段更糟：读的人会当成「确认没有连接」。
	//
	// 后果是**调用方没法验证自己刚做的事**：连完线读回来是空的，
	// 于是要么以为失败重连一遍，要么以为成功但其实没连上。
	// blueprint.get_graph 一直都给 pins + linked_to，材质这边一直没有。
	// 图连不上就没法迭代，而迭代正是做材质的常态。
	auto NodeIdOf = [&ExpressionToId](UMaterialExpression* Expr) -> FString
	{
		if (!Expr) return FString();
		const FString* Found = ExpressionToId.Find(Expr);
		return Found ? *Found : Expr->GetName();
	};

	/**
	 * 一根连线。
	 *
	 * `from_output` 原来只有一个索引号 —— 对着一个「第 3 号输出」，
	 * 读的人既不知道它叫什么（填不回 connect_pins 的 source_pin），
	 * 也不知道它是什么类型（看不出类型匹不匹配）。两边的引脚名和类型都给上。
	 *
	 * 类型这里只如实给出两端各是什么，不替调用方判「这算不算不匹配」——
	 * 引擎的自动转换规则有不少特例，猜错了比不说更糟。
	 */
	auto AddConnection = [&ConnectionsJson, &NodeIdOf](UMaterialExpression* FromExpr, int32 FromOutput,
		const FString& ToNode, const FString& ToInput, const FString& ToType)
	{
		TSharedPtr<FJsonObject> Conn = MakeShared<FJsonObject>();
		Conn->SetStringField(TEXT("from_node"), NodeIdOf(FromExpr));
		Conn->SetNumberField(TEXT("from_output"), FromOutput);
		Conn->SetStringField(TEXT("from_pin"), UAL_OutputPinName(FromExpr, FromOutput));
		/*
		 * 序号必须先验范围再问类型。
		 *
		 * `GetOutputType` 的引擎实现是无保护的 `GetOutputs()[OutputIndex]`，
		 * TArray 的范围检查是 fatal —— 一个**只读**工具能把用户的编辑器崩掉。
		 * 而越界的 OutputIndex 是真的会存在：`MaterialFunctionCall` 在
		 * `UpdateFromFunctionResource` 里先 `Outputs.Reset()`，函数资产丢了或
		 * 还没加载就不再填回去，下游那根 FExpressionInput 却还记着 OutputIndex=0。
		 * 上一行的 `UAL_OutputPinName` 自己带 IsValidIndex，这一行漏了。
		 */
		if (FromExpr && FromExpr->GetOutputs().IsValidIndex(FromOutput))
		{
			const FString FromType = UAL_MaterialTypeName(UAL_OutputTypeOf(FromExpr, FromOutput));
			if (!FromType.IsEmpty())
			{
				Conn->SetStringField(TEXT("from_type"), FromType);
			}
		}
		Conn->SetStringField(TEXT("to_node"), ToNode);
		Conn->SetStringField(TEXT("to_input"), ToInput);
		if (!ToType.IsEmpty())
		{
			Conn->SetStringField(TEXT("to_type"), ToType);
		}
		ConnectionsJson.Add(MakeShared<FJsonValueObject>(Conn));
	};

	// 7a. 节点到节点
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	for (UMaterialExpression* Expression : Material->GetExpressions())
#else
	for (UMaterialExpression* Expression : Material->Expressions)
#endif
	{
		if (!Expression) continue;
		const int32 InputCount = UALCompat::CountInputs(Expression);
		for (int32 i = 0; i < InputCount; ++i)
		{
			FExpressionInput* Input = Expression->GetInput(i);
			if (!Input || !Input->Expression) continue;

			const FString InputName = UAL_InputPinName(Expression, i);
			AddConnection(Input->Expression, Input->OutputIndex,
				NodeIdOf(Expression), InputName, UAL_MaterialTypeName(UAL_InputTypeOf(Expression, i)));
		}
	}

	// 7b. 节点到材质主节点。
	// 名字和 connect_pins 接受的 target_pin 保持一致，读回来的东西
	// 才能直接拿去当参数用
	// 主节点上的输入引脚一律走 UAL_CollectRootInputs —— 这张表在这里手写过一份，
	// 漏了 SubsurfaceColor 和 MaterialAttributes，「用材质属性」的材质因此
	// 读回来是**零条**接到主节点的线，读图的人会得出完全错误的结论。
	//
	// to_type 就是主节点这一侧要什么，和连线里的 from_type 对着看即可。
	for (const FUALRootInput& Root : UAL_CollectRootInputs(Material))
	{
		if (Root.Input && Root.Input->Expression)
		{
			AddConnection(Root.Input->Expression, Root.Input->OutputIndex,
				TEXT("Material"), Root.Name, Root.ExpectedType);
			// 连着但引擎不看的线（Substrate 下的旧引脚）要标出来，
			// 否则读图的人会以为 Metallic 那根线在起作用
			if (UAL_RootPinIgnoredBySubstrate(Material, Root.Name))
			{
				ConnectionsJson.Last()->AsObject()->SetBoolField(TEXT("ignored_by_substrate"), true);
			}
		}
	}

	Data->SetNumberField(TEXT("connection_count"), ConnectionsJson.Num());
	Data->SetArrayField(TEXT("connections"), ConnectionsJson);

	/*
	 * 编辑器开着时要说一句「你读到的和用户看到的可能不一样」。
	 *
	 * 读的是资产，用户屏幕上是编辑器开窗那一刻的副本快照，两边天然会不同步。
	 * 不点破的话就会出现真机上那一幕：工具读出来「连着」，用户看着满屏空引脚，
	 * 模型于是自己编了个解释（「主节点的线不显示在连线清单里」），
	 * 把人带去查一个不存在的问题。
	 */
	if (GraphEditor)
	{
		Data->SetStringField(TEXT("note"),
			TEXT("This material is open in the Material Editor, so you are reading the copy that window is "
				 "showing - the user can watch your edits appear live. Those edits are not on disk yet; "
				 "call material.compile (or ue_save) to apply them to the asset."));
	}

	UE_LOG(LogUALMaterial, Log, TEXT("Got material graph: %s with %d nodes"),
		*Material->GetName(), NodesJson.Num());

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// Handle_SearchMaterialNodes - 材质节点说明书
// ============================================================================

/**
 * 按名字查材质节点类型，连引脚签名一起回。
 *
 * ## 为什么要有它
 *
 * 蓝图那边有 `blueprint.search_nodes`：给个关键字，回来的是函数的确切名字和
 * 每个引脚的名字/类型/方向，所以「查一批 → 写一整张图」成立。
 *
 * 材质这边**什么都没有**。引脚信息只能作为「建节点」的副作用拿到 ——
 * 想知道 TextureSample 的 UV 输入叫什么，唯一办法是先把节点建出来再 get_graph。
 * 真机上的实际做法是：新建一个探针材质，把要用的节点类型各放一个，读回来抄下
 * 引脚名，再回去写真正的图。一次任务为此多付了几十轮往返，而且探针材质
 * 留在了用户工程里（`material.delete` 删不掉被编辑器占着的资产）。
 *
 * 那些引脚名没有规律可循，猜不出来：Sine 的输入叫 `Input`、Lerp 的叫 `A/B/Alpha`、
 * TextureSample 的 UV 输入叫 **`Coordinates`** 而不是 `UVs`。
 *
 * ## 为什么读 CDO 而不是真建一个
 *
 * 引脚是**类**的属性，不是实例的：`Outputs` 在构造函数里就填好了，
 * 输入引脚由类上的 `FExpressionInput` 属性决定。所以类默认对象（CDO）
 * 上读到的和真建一个读到的一模一样，但不碰任何资产、不产生垃圾节点、
 * 不需要先有一个材质 —— 这条命令因此可以在没打开任何材质的情况下调。
 *
 * 唯一读不到的是**挂了资产之后才长出来的引脚**：MaterialFunctionCall 的引脚
 * 来自它调用的那个函数，CDO 上是空的。这种在 `note` 里说清楚，别让调用方
 * 以为这个节点就是没有引脚。
 */
void FUAL_MaterialCommands::Handle_SearchMaterialNodes(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Query;
	Payload->TryGetStringField(TEXT("query"), Query);
	Query.TrimStartAndEndInline();

	int32 Limit = 12;
	Payload->TryGetNumberField(TEXT("limit"), Limit);
	Limit = FMath::Clamp(Limit, 1, 100);

	const TMap<FString, UClass*>& NodeTypeMap = UAL_MaterialNodeTypeMap();

	/*
	 * 按 node_type 的字面排序再截断。
	 *
	 * TMap 的遍历顺序跟着哈希走，同一个查询两次调用可能回不同的 12 条 ——
	 * 调用方会以为工具在抖。排序之后「limit 截掉的是哪些」至少是确定的。
	 */
	TArray<FString> Matched;
	for (const TPair<FString, UClass*>& Pair : NodeTypeMap)
	{
		if (Query.IsEmpty() || Pair.Key.Contains(Query, ESearchCase::IgnoreCase))
		{
			Matched.Add(Pair.Key);
		}
	}
	Matched.Sort();

	const int32 MatchCount = Matched.Num();
	if (MatchCount > Limit)
	{
		Matched.SetNum(Limit);
	}

	TArray<TSharedPtr<FJsonValue>> NodesJson;
	for (const FString& NodeType : Matched)
	{
		UClass* const* FoundClass = NodeTypeMap.Find(NodeType);
		if (!FoundClass || !*FoundClass)
		{
			continue;
		}
		UMaterialExpression* Cdo = (*FoundClass)->GetDefaultObject<UMaterialExpression>();
		if (!Cdo)
		{
			continue;
		}

		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("node_type"), NodeType);
		NodeObj->SetStringField(TEXT("class"), (*FoundClass)->GetName());

		// 输入引脚。名字和类型都照 get_graph 那一套来，两边读到的是同一个东西
		TArray<TSharedPtr<FJsonValue>> InputsJson;
		const int32 InputCount = UALCompat::CountInputs(Cdo);
		for (int32 i = 0; i < InputCount; ++i)
		{
			const FString InputName = UAL_InputPinName(Cdo, i);
			TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
			InputObj->SetStringField(TEXT("name"), InputName);
			const FString InputType = UAL_MaterialTypeName(UAL_InputTypeOf(Cdo, i));
			if (!InputType.IsEmpty())
			{
				InputObj->SetStringField(TEXT("type"), InputType);
			}
			InputsJson.Add(MakeShared<FJsonValueObject>(InputObj));
		}
		NodeObj->SetArrayField(TEXT("inputs"), InputsJson);

		// 输出引脚。无名输出这里已经按 mask 位推成了 R/G/B/A，
		// 和 connect_pins 认的是同一套名字 —— 读到什么就能照着写什么
		TArray<TSharedPtr<FJsonValue>> OutputsJson;
		const TArray<FExpressionOutput>& Outputs = Cdo->GetOutputs();
		for (int32 i = 0; i < Outputs.Num(); ++i)
		{
			TSharedPtr<FJsonObject> OutputObj = MakeShared<FJsonObject>();
			OutputObj->SetStringField(TEXT("name"), UAL_OutputPinName(Cdo, i));
			OutputObj->SetNumberField(TEXT("index"), i);
			const FString OutputType = UAL_MaterialTypeName(UAL_OutputTypeOf(Cdo, i));
			if (!OutputType.IsEmpty())
			{
				OutputObj->SetStringField(TEXT("type"), OutputType);
			}
			OutputsJson.Add(MakeShared<FJsonValueObject>(OutputObj));
		}
		NodeObj->SetArrayField(TEXT("outputs"), OutputsJson);

		/*
		 * 建这个节点还要带哪些字段。
		 *
		 * 引脚列全了也还不够 —— 少了 `function_path` 的 MaterialFunctionCall
		 * 一个引脚都没有，少了 `initial_value` 的 ComponentMask 是个恒为 0 的
		 * 死节点。这些「不给就废」的要求以前只写在各自的错误消息里，
		 * 也就是说只有先撞一次墙才看得到。
		 */
		TArray<TSharedPtr<FJsonValue>> Requires;
		FString Note;
		if ((*FoundClass)->IsChildOf(UMaterialExpressionParameter::StaticClass()) ||
			(*FoundClass)->IsChildOf(UMaterialExpressionTextureSampleParameter::StaticClass()))
		{
			Requires.Add(MakeShared<FJsonValueString>(TEXT("node_name")));
			Note = TEXT("node_name is the parameter name material_set_param will use later");
		}
		if ((*FoundClass)->IsChildOf(UMaterialExpressionMaterialFunctionCall::StaticClass()))
		{
			Requires.Add(MakeShared<FJsonValueString>(TEXT("function_path")));
			Note = TEXT("pins come from the function asset - this list is empty until function_path is set");
		}
		if ((*FoundClass)->IsChildOf(UMaterialExpressionCollectionParameter::StaticClass()))
		{
			Requires.Add(MakeShared<FJsonValueString>(TEXT("collection_path")));
			Requires.Add(MakeShared<FJsonValueString>(TEXT("node_name")));
			Note = TEXT("node_name must be a parameter that already exists inside the collection");
		}
		if ((*FoundClass)->IsChildOf(UMaterialExpressionComponentMask::StaticClass()))
		{
			Requires.Add(MakeShared<FJsonValueString>(TEXT("value")));
			Note = TEXT("value is the channels to keep: \"R\", \"RG\", \"RGB\", \"A\" (xyzw also accepted)");
		}
		if (Requires.Num() > 0)
		{
			NodeObj->SetArrayField(TEXT("requires"), Requires);
		}

		/*
		 * 有没有「值」可设 —— 拿 ReadNodeValue 当判据，不另写一份清单。
		 *
		 * 手写「这些类型能设值」会是第二份必然漂移的表：以后加一个能设值的类型
		 * 忘了同步，说明书就开始骗人。ReadNodeValue 和 ApplyValueToNode 支持的
		 * 是同一批类型（两者都在本文件里挨着改），而且它是纯读，
		 * 拿 CDO 调没有任何副作用。
		 */
		NodeObj->SetBoolField(TEXT("has_value"), ReadNodeValue(Cdo).IsValid());

		// 细节面板里能设的属性和默认值 —— add_node / set_node_value 的 properties 认的就是这些名字
		TSharedPtr<FJsonObject> Editable = UAL_NodeEditableProperties(Cdo);
		if (Editable->Values.Num() > 0)
		{
			NodeObj->SetObjectField(TEXT("properties"), Editable);
		}

		if (!Note.IsEmpty())
		{
			NodeObj->SetStringField(TEXT("note"), Note);
		}
		NodesJson.Add(MakeShared<FJsonValueObject>(NodeObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("ok"), true);
	Data->SetStringField(TEXT("query"), Query);
	Data->SetNumberField(TEXT("match_count"), MatchCount);
	Data->SetNumberField(TEXT("total_types"), NodeTypeMap.Num());
	Data->SetBoolField(TEXT("truncated"), MatchCount > NodesJson.Num());
	Data->SetArrayField(TEXT("nodes"), NodesJson);

	// 说明书里列着 Substrate 节点、项目却没开时，建的那一步必然 400 ——
	// 在查的时候就把原因给出来，省一次撞墙
	Data->SetBoolField(TEXT("substrate_enabled"), UAL_IsSubstrateEnabled());
	const FString SubstrateReason = UAL_SubstrateUnavailableReason();
	if (!SubstrateReason.IsEmpty() && Query.Contains(TEXT("Substrate"), ESearchCase::IgnoreCase))
	{
		Data->SetStringField(TEXT("substrate_note"), SubstrateReason);
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// Handle_AddMaterialNode - 添加材质表达式节点
// ============================================================================
void FUAL_MaterialCommands::Handle_AddMaterialNode(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: material_path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	FString NodeType;
	if (!Payload->TryGetStringField(TEXT("node_type"), NodeType) || NodeType.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: node_type"));
		return;
	}

	// 2. 加载材质
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialPath);
	if (!Material)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		return;
	}

	// Target the copy the Material Editor is holding, not the asset.
	// Must happen BEFORE any node/pin lookup - see UAL_ResolveLiveMaterial.
	IMaterialEditor* MatEditor = nullptr;
	Material = UAL_ResolveLiveMaterial(Material, MatEditor);

	// 3. 解析可选参数
	FString NodeName;
	Payload->TryGetStringField(TEXT("node_name"), NodeName);

	int32 PosX = 0, PosY = 0;
	const TSharedPtr<FJsonObject>* PositionObj = nullptr;
	if (Payload->TryGetObjectField(TEXT("position"), PositionObj) && PositionObj)
	{
		(*PositionObj)->TryGetNumberField(TEXT("x"), PosX);
		(*PositionObj)->TryGetNumberField(TEXT("y"), PosY);
	}

	FString TexturePath;
	Payload->TryGetStringField(TEXT("texture_path"), TexturePath);

	/*
	 * 归一化的结果**单独存一份，不要覆盖调用方发来的那个字符串**。
	 *
	 * 要归一化，是因为 `/Game/T_Noise.uasset`、反斜杠路径、末尾带空格这些
	 * NormalizePath 专门修的写法原样 LoadObject 一律失败，而回执是 200 +
	 * texture_applied:false —— 同一个写法换成 collection_path 却是好的。
	 *
	 * 但不能就地覆盖：`NormalizePath` 的默认前缀是 `/Game/Materials`，
	 * 所以裸名字 `T_Wood` 会被改写成 `/Game/Materials/T_Wood`，
	 * 而回执和日志印的就是这个改写后的值。模型发的是 `T_Wood`、
	 * 收到的是一条「/Game/Materials/T_Wood 加载失败」，于是跑去查一个
	 * 它从来没提过的路径（贴图其实在 /Game/Textures 下）—— 正是这段
	 * 想要消灭的那种白绕。两个都报出去，它才知道发生了什么。
	 */
	const FString ResolvedTexturePath = TexturePath.IsEmpty() ? TexturePath : NormalizePath(TexturePath);

	// 4. 根据 NodeType 创建表达式
	UMaterialExpression* NewExpression = nullptr;
	UClass* ExpressionClass = nullptr;

	// 表在匿名 namespace 的 UAL_MaterialNodeTypeMap() 里，search_nodes 用的是同一张
	const TMap<FString, UClass*>& NodeTypeMap = UAL_MaterialNodeTypeMap();

	UClass* const* FoundClass = NodeTypeMap.Find(NodeType);
	if (FoundClass)
	{
		ExpressionClass = *FoundClass;
	}
	else if (NodeType.StartsWith(TEXT("Substrate"), ESearchCase::IgnoreCase) && !UAL_WITH_SUBSTRATE)
	{
		// 5.4 以前表里没有这一族，回一句「版本不够」，别让人去比对建议列表
		UAL_CommandUtils::SendError(RequestId, 400, UAL_SubstrateUnavailableReason());
		return;
	}
	else
	{
		// 返回可用的节点类型列表
		TArray<TSharedPtr<FJsonValue>> Suggestions;
		for (const auto& Pair : NodeTypeMap)
		{
			Suggestions.Add(MakeShared<FJsonValueString>(Pair.Key));
		}
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown node_type: %s"), *NodeType));
		ErrorData->SetArrayField(TEXT("suggestions"), Suggestions);
		UAL_CommandUtils::SendResponse(RequestId, 400, ErrorData);
		return;
	}

	if (UAL_IsSubstrateClass(ExpressionClass))
	{
		const FString Reason = UAL_SubstrateUnavailableReason();
		if (!Reason.IsEmpty())
		{
			UAL_CommandUtils::SendError(RequestId, 400, Reason);
			return;
		}
	}

	/*
	 * ComponentMask 不给通道就是个死节点，所以挡在建之前。
	 *
	 * 它的 R/G/B/A 四位构造函数一个都不设，默认全 0，编译出来恒为 0 ——
	 * 而图上完全看不出来（节点标题就写着 Mask，看着挺正常）。建完再回警告
	 * 也不行：图里会留下一个必须手工清掉的废节点，而 apply_graph 是失败即停、
	 * 不回滚的，越早挡住越省事。
	 */
	if (ExpressionClass == UMaterialExpressionComponentMask::StaticClass())
	{
		/*
		 * 校验的是**内容**，不是「是不是字符串」。
		 *
		 * 只判类型的话 `value: ""` / `"Q"` / `"R,G"` 一路通过，节点照样建出来，
		 * 然后写值那一步失败 —— 而写值失败不致命（200 + initial_value_applied:false），
		 * 于是留下的正好是这道关卡要挡的那个四位全 0 的死节点。
		 */
		FString MaskText;
		const TSharedPtr<FJsonValue> MaskValue = Payload->TryGetField(TEXT("initial_value"));
		if (!MaskValue.IsValid() || !MaskValue->TryGetString(MaskText))
		{
			UAL_CommandUtils::SendError(RequestId, 400,
				TEXT("ComponentMask needs value set to the channels to keep, "
					 "e.g. \"R\", \"RG\", \"RGB\", \"A\" (xyzw spelling also accepted)"));
			return;
		}

		bool bMaskR = false, bMaskG = false, bMaskB = false, bMaskA = false;
		FString MaskError;
		if (!UAL_ParseChannelMask(MaskText, bMaskR, bMaskG, bMaskB, bMaskA, MaskError))
		{
			UAL_CommandUtils::SendError(RequestId, 400,
				FString::Printf(TEXT("ComponentMask value \"%s\" is not usable: %s"), *MaskText, *MaskError));
			return;
		}
	}

	/*
	 * 参数节点不给 node_name 同样是个静默的死节点，一样挡在建之前。
	 *
	 * 不给的话 `ParameterName` 停在引擎默认的 `Param`：建三个 ScalarParameter
	 * 就是三个都叫 Param，材质照样编译，`material_create_instance` 只暴露出一个，
	 * 之后 `material_set_param` 按你想要的名字去调会「全部成功」而画面纹丝不动
	 * （那条路也不校验参数名）。
	 *
	 * 而 `material_search_nodes` 的 `requires` 已经在告诉模型这条规则是强制的 ——
	 * 说明书说强制、实现却放行，两边必须对上。
	 */
	if (NodeName.IsEmpty() &&
		(ExpressionClass->IsChildOf(UMaterialExpressionParameter::StaticClass()) ||
		 ExpressionClass->IsChildOf(UMaterialExpressionTextureSampleParameter::StaticClass())))
	{
		UAL_CommandUtils::SendError(RequestId, 400,
			FString::Printf(TEXT("%s needs node_name set to the parameter name - without it the node is "
								 "created as the engine default 'Param', material_set_param can never "
								 "reach it, and nothing reports an error"), *NodeType));
		return;
	}

	/*
	 * 参数名不能和图里已有的撞 —— 撞了要在建之前就拒掉。
	 *
	 * 上面那道只管「没给名字」。给了名字但和已有参数重名，是同一类静默失败的
	 * 另一半：两个都叫 Roughness 的 ScalarParameter 都建得出来、都回 200，
	 * 而 `material_create_instance` 只暴露一个，`material_set_param("Roughness")`
	 * 只挪得动其中一个，另一个继续拿旧值喂图 —— 全程没有任何一处报错。
	 *
	 * 为什么不学引擎调 `ValidateParameterName`：它撞名时是**改名**
	 * （Roughness → Roughness_2），而本工具的约定是「node_name 就是参数名」，
	 * 悄悄改掉等于把调用方后面所有 set_param 都指偏。所以这里回 400 让调用方自己定。
	 *
	 * 两条判据都照引擎来，别自己发明：
	 *
	 * - **认参数名用 `HasAParameterName()` / `GetParameterName()`**，不要手写 Cast。
	 *   这两个虚函数 4.27~5.8 都在（`MaterialExpression.h`），`UMaterial::
	 *   RemoveExpressionParameter` 自己用的就是它们。手写两个 Cast 会漏掉
	 *   `FontSampleParameter`、`RuntimeVirtualTextureSampleParameter`、
	 *   `SparseVolumeTextureSampleParameter` —— 它们各自有 ParameterName，
	 *   但继承的是 FontSample / RVTSample / SparseVolumeTextureSample 那几棵树，
	 *   两个 Cast 都接不住，于是撞名照样放过去，正是这道检查要防的事。
	 *
	 * - **只比同一个类**，和 `UMaterialExpression::HasClassAndNameCollision`
	 *   （`return GetClass() == OtherExpression->GetClass();`）一致。引擎把标量参数
	 *   和向量参数放在**两张表**里，一个 ScalarParameter "Tint" 和一个
	 *   VectorParameter "Tint" 在 UE 里合法、`material_set_param` 两个都够得着，
	 *   跨类去拦就是拦掉一张本来能用的图。
	 */
	if (!NodeName.IsEmpty() &&
		(ExpressionClass->IsChildOf(UMaterialExpressionParameter::StaticClass()) ||
		 ExpressionClass->IsChildOf(UMaterialExpressionTextureSampleParameter::StaticClass())))
	{
		const FName WantedName(*NodeName);
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		for (UMaterialExpression* Existing : Material->GetExpressions())
#else
		for (UMaterialExpression* Existing : Material->Expressions)
#endif
		{
			// 类对不上就不是撞名。也顺手挡住了 NAME_None：不带参数名的节点
			// `GetParameterName()` 回的就是 None，不先过 HasAParameterName 的话，
			// `node_name: "None"` 会和图里每一个普通节点「撞名」
			if (!Existing || Existing->GetClass() != ExpressionClass || !Existing->HasAParameterName())
			{
				continue;
			}

			if (Existing->GetParameterName() == WantedName)
			{
				UAL_CommandUtils::SendError(RequestId, 400,
					FString::Printf(TEXT("A parameter named '%s' already exists in this material (%s). Two "
										 "parameters with the same name both compile, but material_set_param "
										 "can only ever reach one of them and nothing reports an error. Pick a "
										 "different node_name, or drop this node and reuse the existing one "
										 "(material_describe lists parameter names; material_get_graph does not)."),
						*NodeName, *Existing->GetClass()->GetName()));
				return;
			}
		}
	}

	// 5. 创建表达式并添加到材质
	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "AddNode", "Add Material Node"));

	Material->PreEditChange(nullptr);
	UAL_ModifyMaterialGraph(Material);

	/*
	 * RF_Transactional 不能漏。
	 *
	 * `UObject::Modify()` 只记录带这个标记的对象 —— 没有它，事务里就是空的，
	 * 空事务会被引擎直接丢掉，于是**撤销栈里连这一步都不存在**，而且全程没有任何报错。
	 *
	 * 这个标记还会顺着 RF_PropagateToSubObjects 传给子对象（材质的
	 * editor-only data 就是这么拿到的），所以建材质那边漏一次，
	 * 这份资产上后续所有改动就都撤不了了。引擎自己建表达式一律带这个标记。
	 */
	NewExpression = NewObject<UMaterialExpression>(Material, ExpressionClass, NAME_None, RF_Transactional);
	if (!NewExpression)
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create material expression"));
		return;
	}

	// 设置位置
	NewExpression->MaterialExpressionEditorX = PosX;
	NewExpression->MaterialExpressionEditorY = PosY;

	/*
	 * 节点属性（properties）在**加进图之前**设。
	 *
	 * 这时它还只是一个没人引用的新对象，设失败了取消事务、交给 GC 就干干净净；
	 * 加进图之后再失败，就得在一张失败即停、不回滚的 apply_graph 里留下一个
	 * 设了一半的节点。
	 */
	TSharedPtr<FJsonObject> PropertiesReadback;
	const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
	if (Payload->TryGetObjectField(TEXT("properties"), PropertiesObj) && PropertiesObj && (*PropertiesObj)->Values.Num() > 0)
	{
		FString PropertyError;
		if (!UAL_ApplyNodeProperties(NewExpression, *PropertiesObj, PropertiesReadback, PropertyError))
		{
			Transaction.Cancel();
			NewExpression->MarkAsGarbage();
			const TArray<FString> Editable = UAL_JsonKeys(UAL_NodeEditableProperties(ExpressionClass->GetDefaultObject<UMaterialExpression>()));
			UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
				TEXT("%s - node was not created. Settable properties on %s: %s"),
				*PropertyError, *NodeType, Editable.Num() ? *FString::Join(Editable, TEXT(", ")) : TEXT("(none)")));
			return;
		}
	}

	// 添加到材质
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	Material->GetExpressionCollection().AddExpression(NewExpression);
#else
	Material->Expressions.Add(NewExpression);
#endif

	/*
	 * 回指针不能漏 —— 漏了会把编辑器崩掉。
	 *
	 * 加进集合只完成了一半，引擎自己那条路（`MaterialEditingLibrary.cpp` 的
	 * `CreateMaterialExpressionEx` 和 `DuplicateMaterialExpression`，两处都是）
	 * 紧跟着就写这一行。它不是「表达式属于谁」的冗余记录 —— 材质编辑器建节点
	 * 预览时**拿它当断言条件**：
	 *
	 *     FMatExpressionPreview::FMatExpressionPreview(UMaterialExpression* InExpression)
	 *     {
	 *         check(InExpression->Material && InExpression->Material->GetExpressions().Contains(InExpression));
	 *
	 * 后半截我们满足，炸的是前半截。这句在 `MaterialEditor.cpp` 里的行号逐版都不同
	 * （5.1:205、5.2:204、5.3:204、5.4:222、5.5:240、5.6:263、5.7:268、5.8:333），
	 * 4.27 和 5.0 上它压根不在 .cpp 里，而是 `MaterialEditor.h`（66 / 70 行）里
	 * 内联的那个构造函数，判的是 `Expressions.Contains`。**搜那句 check，别按行号找。**
	 *
	 * 更要紧的是 **5.5 及以后根本走不到这句 check**：`GetExpressionPreview` 在造预览
	 * 之前先加了一道 `if (!Preview && MaterialExpression->Material->GetExpressions()
	 * .Contains(MaterialExpression))`，回指针是空的时候这里就是一次裸解引用。
	 * 也就是说在支持范围的后半段，这个 bug 是**访问违例**而不是断言 ——
	 * 不会因为 check() 在某些构建配置里被编掉就不见了。
	 *
	 * 触发路径是本函数末尾的 UAL_RefreshMaterialEditor →
	 * UpdateMaterialAfterGraphChange() → RefreshExpressionPreviews() →
	 * GetExpressionPreview() → new FMatExpressionPreview —— 也就是说
	 * **材质编辑器开着的时候，建一个节点就可能当场崩**，编辑器里那份未保存的
	 * 工作副本连同整个材质一起没。真机上就是这么栽的（2026-09-21 的用户报告）。
	 *
	 * 为什么不是每次都崩：`GetExpressionPreview` 有一道门禁 ——
	 * `!bHidePreviewWindow && !bCollapsed`（5.8 收进了 `ShouldShowPreview()`，
	 * 判据一样），折叠的节点不建预览就碰不到这条路。
	 * `UMaterialExpression` 基类默认 `bCollapsed = true`，所以 Multiply / Lerp
	 * 这些一直是安全的，ScalarParameter 也在自己的构造函数里设回了 true ——
	 * 那次崩溃前连着建成的 15 个 ScalarParameter 不是运气好。
	 * 而 `bCollapsed = false` 的类有十几个（5.1 是 15 个、5.2~5.5 是 17 个、
	 * 5.8 是 18 个），里面全是常用货：TextureSample、
	 * Constant3Vector、Constant4Vector、VectorParameter、CollectionParameter……
	 * 建到其中任何一个，第一个就炸。所以这不是「一次发太多节点」的问题，
	 * 发两个也一样，别把它当成批量上限能挡住的事。
	 */
	NewExpression->Material = Material;

	NewExpression->UpdateParameterGuid(true, true);

	// 6. 设置特定节点属性
	/*
	 * 贴图参数节点走的是**另一棵继承树**，得单独设名字。
	 *
	 * `UMaterialExpressionTextureSampleParameter` 继承的是
	 * `UMaterialExpressionTextureSample`，不是 `UMaterialExpressionParameter` ——
	 * 下面那个 Cast 接不住它。于是建节点前的那道「参数节点必须给 node_name」
	 * 会对它报 400 要名字，给了名字却又原样丢掉：节点建出来 ParameterName 还是空，
	 * material_create_instance 暴露不出它，material_set_param 永远够不着，
	 * 全程 200。要名字又不用名字，比不要还坏。
	 */
	if (UMaterialExpressionTextureSampleParameter* TexParamExpr =
			Cast<UMaterialExpressionTextureSampleParameter>(NewExpression))
	{
		if (!NodeName.IsEmpty())
		{
			TexParamExpr->ParameterName = FName(*NodeName);
		}
		FString GroupName;
		if (Payload->TryGetStringField(TEXT("group_name"), GroupName) && !GroupName.IsEmpty())
		{
			TexParamExpr->Group = FName(*GroupName);
		}
	}

	if (UMaterialExpressionParameter* ParamExpr = Cast<UMaterialExpressionParameter>(NewExpression))
	{
		if (!NodeName.IsEmpty())
		{
			ParamExpr->ParameterName = FName(*NodeName);
		}
		// group_name 以前是个纯装饰参数：调用方能传，这边从来没读过
		FString GroupName;
		if (Payload->TryGetStringField(TEXT("group_name"), GroupName) && !GroupName.IsEmpty())
		{
			ParamExpr->Group = FName(*GroupName);
		}
	}

	/**
	 * CollectionParameter 要两样东西才算配好：指向哪个 MPC，用它里面的哪个参数。
	 *
	 * 少任何一样，节点建出来是**灰的**（编译时报 "Missing Parameter Collection"），
	 * 而这两个都不是 add_node 的通用字段，所以在这里单独处理。
	 * 配不上时不静默 —— 灰节点在图里很难一眼看出来。
	 */
	FString AttachError;
	if (UMaterialExpressionCollectionParameter* CollectionExpr = Cast<UMaterialExpressionCollectionParameter>(NewExpression))
	{
		FString CollectionPath;
		Payload->TryGetStringField(TEXT("collection_path"), CollectionPath);

		if (CollectionPath.IsEmpty())
		{
			AttachError = TEXT("CollectionParameter needs collection_path (the MPC asset to read from)");
		}
		else if (UMaterialParameterCollection* Collection =
					LoadObject<UMaterialParameterCollection>(nullptr, *NormalizePath(CollectionPath)))
		{
			CollectionExpr->Collection = Collection;
			if (!NodeName.IsEmpty())
			{
				// 参数名必须是 MPC 里真实存在的一个 —— 拼错了不会报错，
				// 只会在编译时变成一个恒为 0 的输入
				const FName ParamName(*NodeName);
				const int32 ScalarIndex = UAL_FindScalarParam(Collection, ParamName);
				const int32 VectorIndex = UAL_FindVectorParam(Collection, ParamName);
				if (ScalarIndex != INDEX_NONE || VectorIndex != INDEX_NONE)
				{
					CollectionExpr->ParameterName = ParamName;
					// ParameterId 是节点认参数的真凭据（改名时靠它对上号）。
					// 只设名字不设 Id，重命名参数之后这个节点会静默失联
					CollectionExpr->ParameterId = ScalarIndex != INDEX_NONE
						? Collection->ScalarParameters[ScalarIndex].Id
						: Collection->VectorParameters[VectorIndex].Id;
				}
				else
				{
					AttachError = FString::Printf(
						TEXT("'%s' is not a parameter in %s - add it with material.parameter_collection first"),
						*NodeName, *Collection->GetName());
				}
			}
			else
			{
				AttachError = TEXT("CollectionParameter needs node_name set to the parameter name inside the collection");
			}
		}
		else
		{
			AttachError = FString::Printf(TEXT("Parameter collection not found: %s"), *CollectionPath);
		}
	}

	/**
	 * MaterialFunctionCall 要挂上函数资产才有引脚。
	 *
	 * 不挂的话节点建出来是空的（一个引脚都没有），后续 connect_pins 会报
	 * 「找不到引脚」，而根因在两步之前 —— 所以在这里就说清楚。
	 */
	if (UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(NewExpression))
	{
		FString FunctionPath;
		Payload->TryGetStringField(TEXT("function_path"), FunctionPath);

		if (FunctionPath.IsEmpty())
		{
			AttachError = TEXT("MaterialFunctionCall needs function_path (the MaterialFunction asset to call)");
		}
		else if (UMaterialFunctionInterface* Function =
					LoadObject<UMaterialFunctionInterface>(nullptr, *NormalizePath(FunctionPath)))
		{
			// SetMaterialFunction 会顺带按函数的输入输出重建引脚，
			// 直接赋值 MaterialFunction 字段是不够的
			FunctionCall->SetMaterialFunction(Function);
		}
		else
		{
			AttachError = FString::Printf(TEXT("Material function not found: %s"), *FunctionPath);
		}
	}

	// 记录贴图是否成功设置（用于返回给调用方）
	bool bTextureApplied = false;

	/*
	 * 认 `UMaterialExpressionTextureBase`，不是只认 `TextureSample`。
	 *
	 * `UMaterialExpressionTextureObject` 是 `TextureBase` 的**兄弟**不是子类，
	 * 所以原来那个 `Cast<TextureSample>` 对它是空的：贴图压根没挂上，
	 * 而 `texture_applied` 只看「给没给路径」照样回 false —— 于是模型收到的是
	 * 「贴图没设上（/Game/T_Noise 加载失败）」，跑去查一个根本不存在的路径问题。
	 * 而 `ApplyValueToNode` 早就是按 `TextureBase` 判的，能力一直在，只是这条路没接上。
	 */
	if (UMaterialExpressionTextureBase* TexExpr = Cast<UMaterialExpressionTextureBase>(NewExpression))
	{
		if (!TexturePath.IsEmpty())
		{
			UTexture* Texture = LoadObject<UTexture>(nullptr, *ResolvedTexturePath);
			if (Texture)
			{
				TexExpr->Texture = Texture;
				/*
				 * 挂完贴图**必须**跟一句 `AutoSetSampleType()`。
				 *
				 * `SamplerType` 是节点上另一个字段，赋 `Texture` 不会带着它走，
				 * 而它的默认值是 `SAMPLERTYPE_Color`。于是挂一张法线贴图的下场是：
				 * 这里回 `texture_applied: true`，接着 material.compile 报
				 * 「Sampler type is Color, should be Normal」—— 而这套工具里
				 * **没有任何一个**能改 SamplerType，模型只能对着一个它没设过的字段干瞪眼。
				 * 编辑器里手动挂是靠 `PostEditChangeProperty` 顺带调它，我们这条路没有，
				 * 所以得自己调（引擎自己的 MaterialEditingLibrary 也是这么写的）。
				 */
				TexExpr->AutoSetSampleType();
				bTextureApplied = true;
				UE_LOG(LogUALMaterial, Log, TEXT("Set texture for %s: %s (sampler %d)"),
					*NewExpression->GetClass()->GetName(), *ResolvedTexturePath,
					static_cast<int32>(TexExpr->SamplerType));
			}
			else
			{
				// 两个路径都印：发来的那个和实际去找的那个。只印一个的话，
				// 「我发的明明是 T_Wood」和「它说 /Game/Materials/T_Wood 找不到」
				// 之间那一步改写就是隐形的
				UE_LOG(LogUALMaterial, Warning, TEXT("Failed to load texture: %s (resolved from '%s')"),
					*ResolvedTexturePath, *TexturePath);
			}
		}
	}

	// 设置初始值。
	//
	// 这里原来是 `TryGetObjectField("initial_value")`，而调用方按 schema 传的是
	// **裸值**（`0.5` / `{r:1,g:0,b:0}` / `[1,0,0]`）—— 外层取对象直接失败，
	// ApplyInitialValueToNode 根本不会被调用。也就是说这个参数从来没生效过，
	// 而且全程 200：「建一个值为 0.2 的 Constant」拿到的一直是 0，没人会发现。
	const TSharedPtr<FJsonValue> InitialValue = Payload->TryGetField(TEXT("initial_value"));
	const bool bHasInitialValue = InitialValue.IsValid() && InitialValue->Type != EJson::Null;
	bool bInitialValueApplied = false;
	FString InitialValueError;

	if (bHasInitialValue)
	{
		bInitialValueApplied = ApplyValueToNode(NewExpression, InitialValue, InitialValueError);
		if (!bInitialValueApplied)
		{
			UE_LOG(LogUALMaterial, Warning, TEXT("initial_value not applied on %s: %s"),
				*NodeType, *InitialValueError);
		}
	}

	// 标记材质已修改并刷新编辑器
	FPropertyChangedEvent PropertyChangedEvent(nullptr, EPropertyChangeType::ValueSet);
	Material->PostEditChangeProperty(PropertyChangedEvent);
	
	// 强制刷新材质编辑器图表和逻辑
	if (Material && Material->MaterialGraph)
	{
		UAL_RefreshMaterialEditor(MatEditor, Material);
	}
	
	Material->MarkPackageDirty();

	// 通知编辑器
	if (GEditor)
	{
		// 刷新属性面板
		FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor");
		if (PropertyModule)
		{
			PropertyModule->NotifyCustomizationModuleChanged();
		}
	}

	/*
	 * 8. 生成节点 ID —— 走 `ExpressionIdFor`，不要自己按数组长度算。
	 *
	 * 那两个查找函数（`BuildExpressionIds` / `FindExpressionById`）都是
	 * `if (!Expression) continue;` **之后**才 `Index++`，也就是只数非空的；
	 * 而这里原来用的是数组总长度。材质里有一个空槽，两边就差一位：
	 * add_node 回 `..._10`，实际编号是 `..._9`，接着 connect_pins 按位置找不到，
	 * 对象名兜底也对不上 —— 404「Source node not found」，而 apply_graph
	 * 此时节点已经全建完了，不回滚。这个文件里有五处都在防空槽，说明它真会出现。
	 *
	 * 用单个查询而不是 `BuildExpressionIds` 建整张表：规则是同一套，但建表要给
	 * **每个**节点拼一次字符串，而 apply_graph 是一个节点一次 add_node ——
	 * 200 个节点就是两万次 Printf，全落在游戏线程上，就为了取其中一条。
	 *
	 * 空串意味着表达式压根不在这张图里（正常情况下不可能，它刚被加进去）。
	 * 这时候别拿 `GetName()` 兜底：`FindExpressionById` 的对象名分支同样是在
	 * 这张图里找，图里没有它就照样找不到，兜了个寂寞还让调用方以为拿到了可用的 id。
	 * 直接报 500，让失败停在这儿，而不是等到 connect_pins 再 404。
	 */
	const FString NodeId = ExpressionIdFor(Material, NewExpression);
	if (NodeId.IsEmpty())
	{
		// 这里**不要** Cancel。`UTransBuffer::Cancel` 只是把记录从撤销栈上弹掉，
		// 不回滚 —— 节点此时已经加进图里了，撤销记录反倒是用户唯一能把它删掉的东西。
		// （另外两处 Cancel 的前提是 NewObject 回了空，压根没东西要回滚，不是一回事。）
		UAL_CommandUtils::SendError(RequestId, 500,
			FString::Printf(TEXT("Node was created but is not in %s's expression list - cannot report a usable node_id"),
				*Material->GetName()));
		return;
	}

	// 9. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), NodeId);
	Data->SetStringField(TEXT("class"), NewExpression->GetClass()->GetName());
	Data->SetStringField(TEXT("display_name"), NewExpression->GetName());
	// 稳定 id，中间隔了删除或撤销之后要用它来指这个节点
	Data->SetStringField(TEXT("guid"), NewExpression->GetMaterialExpressionId().ToString());

	TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
	PosObj->SetNumberField(TEXT("x"), PosX);
	PosObj->SetNumberField(TEXT("y"), PosY);
	Data->SetObjectField(TEXT("position"), PosObj);

	// 简化的 pins 信息
	TArray<TSharedPtr<FJsonValue>> PinsJson;
	TSharedPtr<FJsonObject> OutputPin = MakeShared<FJsonObject>();
	OutputPin->SetStringField(TEXT("name"), TEXT("Default"));
	OutputPin->SetStringField(TEXT("type"), TEXT("Output"));
	PinsJson.Add(MakeShared<FJsonValueObject>(OutputPin));
	Data->SetArrayField(TEXT("pins"), PinsJson);

	// 贴图设置状态
	if (!TexturePath.IsEmpty())
	{
		// 回执里报**发来的那个**路径，另外单给一个 resolved_texture_path。
		// 反过来（只报改写后的）会让调用方以为自己发的就是那个，
		// 然后对着一个它没写过的路径排查
		Data->SetStringField(TEXT("texture_path"), TexturePath);
		if (ResolvedTexturePath != TexturePath)
		{
			Data->SetStringField(TEXT("resolved_texture_path"), ResolvedTexturePath);
		}
		Data->SetBoolField(TEXT("texture_applied"), bTextureApplied);
	}

	/**
	 * CollectionParameter / MaterialFunctionCall 没挂上资产时必须说出来。
	 *
	 * 两者的症状不一样但都很晚才暴露：前者节点是灰的、编译才报
	 * "Missing Parameter Collection"；后者节点一个引脚都没有、
	 * 下一步 connect_pins 会报「找不到引脚」而根因在两步之前。
	 * 在图里都一眼看不出来。
	 */
	const bool bNeedsAttachment =
		Cast<UMaterialExpressionCollectionParameter>(NewExpression) != nullptr ||
		Cast<UMaterialExpressionMaterialFunctionCall>(NewExpression) != nullptr;

	if (!AttachError.IsEmpty())
	{
		Data->SetBoolField(TEXT("collection_applied"), false);
		Data->SetStringField(TEXT("collection_error"), AttachError);
	}
	else if (bNeedsAttachment)
	{
		Data->SetBoolField(TEXT("collection_applied"), true);
	}

	// 初始值设置状态。节点已经建出来了，所以这里不整体失败 —— 但**必须**把没设上
	// 这件事说出来，否则调用方会拿着一个默认值的节点继续往下连线
	if (bHasInitialValue)
	{
		Data->SetBoolField(TEXT("initial_value_applied"), bInitialValueApplied);
		if (!bInitialValueApplied)
		{
			Data->SetStringField(TEXT("initial_value_error"), InitialValueError);
		}
	}

	// 节点当前的实际值，让调用方不用再调一次就能确认初始值落没落上
	if (TSharedPtr<FJsonValue> CurrentValue = ReadNodeValue(NewExpression))
	{
		Data->SetField(TEXT("value"), CurrentValue);
	}

	// 设过的属性回读值（引擎侧的，不是入参 —— 枚举会被规范成引擎的写法）
	if (PropertiesReadback.IsValid() && PropertiesReadback->Values.Num() > 0)
	{
		Data->SetObjectField(TEXT("properties"), PropertiesReadback);
	}

	UE_LOG(LogUALMaterial, Log, TEXT("Added node %s to material %s"),
		*NodeId, *Material->GetName());

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// Handle_ConnectMaterialPins - 连接材质节点引脚
// ============================================================================
void FUAL_MaterialCommands::Handle_ConnectMaterialPins(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: material_path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	FString SourceNode, SourcePin, TargetNode, TargetPin;
	if (!Payload->TryGetStringField(TEXT("source_node"), SourceNode) ||
		!Payload->TryGetStringField(TEXT("source_pin"), SourcePin) ||
		!Payload->TryGetStringField(TEXT("target_node"), TargetNode) ||
		!Payload->TryGetStringField(TEXT("target_pin"), TargetPin))
	{
		UAL_CommandUtils::SendError(RequestId, 400, 
			TEXT("Missing required fields: source_node, source_pin, target_node, target_pin"));
		return;
	}

	// 2. 加载材质
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialPath);
	if (!Material)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		return;
	}

	// Target the copy the Material Editor is holding, not the asset.
	// Must happen BEFORE any node/pin lookup - see UAL_ResolveLiveMaterial.
	IMaterialEditor* MatEditor = nullptr;
	Material = UAL_ResolveLiveMaterial(Material, MatEditor);

	// 3. 查找源节点（支持两种 ID 格式：计算的索引 ID 和 UE 对象实际名称）
	UMaterialExpression* SourceExpression = FindExpressionById(Material, SourceNode);

	if (!SourceExpression)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Source node not found: %s"), *SourceNode));
		return;
	}

	// 3b. 源引脚名 → 输出序号。
	//
	// 这一步以前根本不存在：接主节点时写死 0，接节点时只认纯数字。
	// TextureSample 的输出是 RGB(0) R(1) G(2) B(3) A(4) RGBA(5)，所以
	// 「Alpha 接 Opacity」实际接的是 RGB —— 而返回体照抄入参，看起来完全正确。
	int32 SourceOutputIndex = 0;
	TArray<FString> AvailableOutputs;
	if (!ResolveOutputIndex(SourceExpression, SourcePin, SourceOutputIndex, AvailableOutputs))
	{
		UAL_CommandUtils::SendError(RequestId, 400,
			FString::Printf(TEXT("Output pin '%s' not found on %s. Available outputs: %s"),
				*SourcePin, *SourceNode,
				AvailableOutputs.Num() ? *FString::Join(AvailableOutputs, TEXT(", ")) : TEXT("(none)")));
		return;
	}

	// 4. 连接到材质主节点
	if (TargetNode == TEXT("Material"))
	{
		// 使用 FUAL_ScopedTransaction 管理事务
		FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "ConnectPins", "Connect Material Pins"));

		Material->PreEditChange(nullptr);
		UAL_ModifyMaterialGraph(Material);

		// 映射 target_pin 到材质主节点的输入。
		//
		// 这张表走 UAL_CollectRootInputs，和 get_graph 读回来的是同一份 ——
		// 这里以前是自己手写的一份十个引脚的表，比 get_graph 少了
		// SubsurfaceColor 和 MaterialAttributes，于是出现「读图告诉你有这个引脚、
		// 连线却回 400 说没有」。读和写对不上，比两边都少更难查。
		const TArray<FUALRootInput> RootPins = UAL_CollectRootInputs(Material);
		if (RootPins.Num() == 0)
		{
			Transaction.Cancel();
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Material has no editor-only data"));
			return;
		}

		bool bConnected = false;
		TArray<FString> KnownRootPins;
		for (const FUALRootInput& Pin : RootPins)
		{
			KnownRootPins.Add(Pin.Name);
			// 大小写不敏感：调用方写 "basecolor" 时以前会撞一个 400，
			// 而引擎这边分不清大小写并没有任何好处
			if (!bConnected && Pin.Input && FString(Pin.Name).Equals(TargetPin, ESearchCase::IgnoreCase))
			{
				// FrontMaterial 始终在表里（读图和清理要用），但没开 Substrate 时连上去是白连
				if (FCString::Stricmp(Pin.Name, TEXT("FrontMaterial")) == 0)
				{
					const FString Reason = UAL_SubstrateUnavailableReason();
					if (!Reason.IsEmpty())
					{
						Transaction.Cancel();
						UAL_CommandUtils::SendError(RequestId, 400, Reason);
						return;
					}
#if UAL_WITH_SUBSTRATE
					/*
					 * 数值接进 FrontMaterial 连得上，要到编译才炸（"Could not find any Substrate
					 * operators or BSDFs" + "Cannot force a cast between non-numeric types"），
					 * 真机试过。类型报的是数值还不够（5.4–5.7 上函数调用、静态开关也报数值，
					 * 见 UAL_MayCarrySubstrate），还要整条上游都看不出 Substrate 来源才挡。
					 */
					const uint32 SourceType = SourceExpression->GetOutputs().IsValidIndex(SourceOutputIndex)
						? UAL_OutputTypeOf(SourceExpression, SourceOutputIndex) : 0u;
					if ((SourceType & MCT_Substrate) == 0 && (SourceType & MCT_Float) != 0 &&
						!UAL_MayCarrySubstrate(SourceExpression))
					{
						Transaction.Cancel();
						UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
							TEXT("FrontMaterial takes Substrate data, but %s outputs %s. Feed the value into a Substrate "
								 "BSDF first (e.g. SubstrateSlabBSDF.Diffuse Albedo, or SubstrateUnlitBSDF.EmissiveColor) "
								 "and connect that BSDF's Out to Material.FrontMaterial."),
							*SourceNode, *UAL_MaterialTypeName(SourceType)));
						return;
					}
#endif
				}
				if (UAL_RootPinIgnoredBySubstrate(Material, Pin.Name))
				{
					TArray<FString> ActivePins;
					for (const FUALRootInput& Other : RootPins)
					{
						if (!UAL_RootPinIgnoredBySubstrate(Material, Other.Name))
						{
							ActivePins.Add(Other.Name);
						}
					}
					// Opacity 的去处和着色那一排不一样：Substrate 用 Coverage 表达不透明度
					const bool bIsOpacity = FCString::Stricmp(Pin.Name, TEXT("Opacity")) == 0;
					const TCHAR* Instead = bIsOpacity
						? TEXT("Express opacity as coverage instead: SubstrateWeight (A = the BSDF, Weight = opacity 0..1) "
							   "-> Material.FrontMaterial. The Opacity pin only counts in AlphaComposite.")
						: TEXT("Shade through Substrate nodes instead: e.g. SubstrateSlabBSDF (Diffuse Albedo, F0, Roughness, "
							   "Normal, Emissive Color ...) -> Material.FrontMaterial.");
					Transaction.Cancel();
					UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
						TEXT("Substrate is enabled, and the Material output's %s pin is ignored for this material - "
							 "a link there would have no effect. %s Pins that do take effect here: %s"),
						Pin.Name, Instead, *FString::Join(ActivePins, TEXT(", "))));
					return;
				}
				Pin.Input->Connect(SourceOutputIndex, SourceExpression);
				bConnected = true;
			}
		}

		if (!bConnected)
		{
			Transaction.Cancel();
			UAL_CommandUtils::SendError(RequestId, 400,
				FString::Printf(TEXT("Unknown material pin: %s. Available pins: %s"),
					*TargetPin, *FString::Join(KnownRootPins, TEXT(", "))));
			return;
		}

		// 标记材质已修改并刷新编辑器
		FPropertyChangedEvent PropertyChangedEvent(nullptr, EPropertyChangeType::ValueSet);
		Material->PostEditChangeProperty(PropertyChangedEvent);
		
		// 强制刷新材质编辑器图表和逻辑
		if (Material && Material->MaterialGraph)
		{
			UAL_RefreshMaterialEditor(MatEditor, Material);
		}
		
		Material->MarkPackageDirty();

		// 通知编辑器
		if (GEditor)
		{
			// 刷新属性面板
			FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor");
			if (PropertyModule)
			{
				PropertyModule->NotifyCustomizationModuleChanged();
			}
		}

		// 构建响应
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
		// 报**实际用上的**输出序号和名字，而不是把入参照抄回去。
		// 照抄是这个接口以前最误导人的地方：写死接 0 号输出，却回一句
		// "from": "TextureSample_0.A"，看起来分毫不差
		ConnObj->SetStringField(TEXT("from"), FString::Printf(TEXT("%s.%d"), *SourceNode, SourceOutputIndex));
		// 名字**当场算**，不要去 AvailableOutputs 里取。
		//
		// 那个数组现在只在 ResolveOutputIndex 失败时才填（省掉成功路径上的上万次
		// FString 分配），而这里是成功路径 —— 照原样取的下场是 IsValidIndex 恒为 false、
		// from_output 恒为空串，上面这段注释承诺的「报实际用上的那个名字」当场作废。
		ConnObj->SetStringField(TEXT("from_output"),
			UAL_OutputPinName(SourceExpression, SourceOutputIndex));
		ConnObj->SetStringField(TEXT("to"), FString::Printf(TEXT("Material.%s"), *TargetPin));
		Data->SetObjectField(TEXT("connection"), ConnObj);

		UE_LOG(LogUALMaterial, Log, TEXT("Connected %s.%s -> Material.%s in %s"), 
			*SourceNode, *SourcePin, *TargetPin, *Material->GetName());

		UAL_CommandUtils::SendResponse(RequestId, 200, Data);
	}
	else
	{
		// 节点之间的连接。
		//
		// 这里原来直接回 501「尚未实现」，而那让**任何比「常量 → 材质输出」
		// 复杂一点的材质都做不出来**：Time→Sine→自发光、贴图→Multiply→基础色、
		// Panner→贴图 —— 材质图里最常见的几种结构全都要节点到节点。
		// 真机任务评测里「做一个呼吸式闪烁的材质」就是卡在这一步。

		// 用和源节点完全相同的两种 ID 口径找目标节点，
		// 否则调用方拿 add_node 返回的 id 在这里会找不到
		UMaterialExpression* TargetExpression = FindExpressionById(Material, TargetNode);

		if (!TargetExpression)
		{
			UAL_CommandUtils::SendError(RequestId, 404,
				FString::Printf(TEXT("Target node not found: %s"), *TargetNode));
			return;
		}

		// 找目标节点上叫这个名字的输入引脚。名字留空时接第一个输入 ——
		// 单输入节点（Sine / Abs / OneMinus 之类）占了大多数，
		// 要求调用方必须报出引脚名只会平添失败
		FExpressionInput* TargetInput = nullptr;
		FString MatchedInputName;
		TArray<FString> AvailableInputs;
		const int32 InputCount = UALCompat::CountInputs(TargetExpression);
		for (int32 i = 0; i < InputCount; ++i)
		{
			// 报出去的名字和拿来比对的**必须是同一个**。以前报的是 `<0>`、
			// 比的是原始 FName，于是错误消息里列着 `<0>` 又拒绝 `<0>`
			const FString InputName = UAL_InputPinName(TargetExpression, i);
			AvailableInputs.Add(InputName);

			const bool bNameMatches = !TargetPin.IsEmpty() && InputName.Equals(TargetPin, ESearchCase::IgnoreCase);
			const bool bTakeFirst = TargetPin.IsEmpty() && i == 0;
			if ((bNameMatches || bTakeFirst) && !TargetInput)
			{
				TargetInput = TargetExpression->GetInput(i);
				MatchedInputName = InputName;
			}
		}

		if (!TargetInput)
		{
			// 把这个节点**实际有哪些输入**列出来。只说「找不到引脚」的话，
			// 调用方只能换个名字反复猜
			UAL_CommandUtils::SendError(RequestId, 400,
				FString::Printf(TEXT("Input pin '%s' not found on %s. Available inputs: %s"),
					*TargetPin, *TargetNode,
					AvailableInputs.Num() ? *FString::Join(AvailableInputs, TEXT(", ")) : TEXT("(none)")));
			return;
		}

		// 输出序号在上面 ResolveOutputIndex 时已经按名字解析好了 ——
		// 这里原来又自己算了一遍，而且只认纯数字，"RGB"/"A" 一律退回 0

		// 节点到节点这条路径以前不开事务，改完撤不回来。
		// 主节点那条路径一直是有的，两边行为不一致本身就是个坑
		FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "ConnectPins", "Connect Material Pins"));

		UAL_ModifyMaterialGraph(Material);
		TargetExpression->Modify();
		TargetInput->Connect(SourceOutputIndex, SourceExpression);

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		Material->PostEditChange();
#else
		// 5.0 上这里原来一次 PostEditChange 都不发，改动不会传到渲染端
		FPropertyChangedEvent PropertyChangedEvent(nullptr, EPropertyChangeType::ValueSet);
		Material->PostEditChangeProperty(PropertyChangedEvent);
#endif
		if (Material->MaterialGraph)
		{
			Material->MaterialGraph->RebuildGraph();
		}
		Material->MarkPackageDirty();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
		ConnObj->SetStringField(TEXT("from"), FString::Printf(TEXT("%s.%d"), *SourceNode, SourceOutputIndex));
		ConnObj->SetStringField(TEXT("to"), FString::Printf(TEXT("%s.%s"), *TargetNode,
			MatchedInputName.IsEmpty() ? TEXT("<0>") : *MatchedInputName));
		Data->SetObjectField(TEXT("connection"), ConnObj);

		UE_LOG(LogUALMaterial, Log, TEXT("Connected %s -> %s.%s in %s"),
			*SourceNode, *TargetNode, *MatchedInputName, *Material->GetName());

		// Let the open Material Editor repaint - see UAL_RefreshMaterialEditor
		UAL_RefreshMaterialEditor(MatEditor, Material);

		UAL_CommandUtils::SendResponse(RequestId, 200, Data);
	}
}

// ============================================================================
// Handle_CompileMaterial - 编译材质
// ============================================================================
void FUAL_MaterialCommands::Handle_CompileMaterial(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	// 2. 加载材质
	UMaterialInterface* MaterialInterface = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!MaterialInterface)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		return;
	}

	// 3. 触发编译
	UMaterial* Material = Cast<UMaterial>(MaterialInterface);

	/*
	 * 编辑器开着时，这里就是改动**落回资产**的那一下。
	 *
	 * 前面那些写命令改的是编辑器手里的副本（见 UAL_ResolveLiveMaterial），
	 * 用户能眼看着节点长出来，但那份东西还没进资产 —— 引擎只在按「应用」时
	 * 才把副本拷回去。`SaveAsset_Execute()` 做的正是「应用 + 保存」，
	 * 是公开的工具箱接口。
	 *
	 * 放在 compile 而不是每个写命令后面：一次 material.apply_graph 要发几十条
	 * 写命令，每条都存一次盘既慢又把「只存 agent 改过的」那套记账搅乱；
	 * 而 apply_graph 本来就以一次 compile 收尾，这里正好是那个提交点。
	 */
	UAL_ApplyMaterialEditorToAsset(Material);

	if (Material)
	{
		// 强制重新编译
		Material->ForceRecompileForRendering();
	}

	// Post edit change 会触发编译
	MaterialInterface->PostEditChange();

	// 4. 等着色器真的编完。
	//
	// 编译是异步的，不等就去读错误列表只会读到空的 —— 这也是为什么以前
	// 这个接口「从没报过错」看起来很正常
	if (GShaderCompilingManager)
	{
		GShaderCompilingManager->FinishAllCompilation();
	}

	// 5. 读真正的编译错误。
	//
	// 这里原来只做了一件事：遍历节点看有没有 TextureSample 缺贴图，
	// 然后无条件 `compiled: true`。类型不匹配、引脚接错、着色模型和输出冲突 ——
	// 一概读不到，调用方永远看到「编译成功」，也就永远无法自我纠错。
	// 引擎自己有这个列表，材质编辑器就是这么取的。
	TArray<TSharedPtr<FJsonValue>> ErrorsJson;
	TArray<TSharedPtr<FJsonValue>> WarningsJson;
	TArray<TSharedPtr<FJsonValue>> ErrorNodesJson;

	if (Material)
	{
		if (const FMaterialResource* Resource = Material->GetMaterialResource(
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
			// 5.7 起这个重载收的是 EShaderPlatform，不再是 ERHIFeatureLevel
			GMaxRHIShaderPlatform
#else
			GMaxRHIFeatureLevel
#endif
			))
		{
			for (const FString& Error : Resource->GetCompileErrors())
			{
				ErrorsJson.Add(MakeShared<FJsonValueString>(Error));
			}

			// 错误落在**哪个节点**上。
			//
			// 只给错误文本的话，调用方拿到的是「BaseColor 引脚上的某处出错了」，
			// 然后要在上百个节点里自己找。引擎其实一直知道是谁 —— 材质编辑器
			// 就是靠这份列表把节点标红的，只是我们没往外传。
			//
			// 不按下标去和 CompileErrors 配对：两个数组只在旧翻译器里是并排增长的，
			// 新的 SM6 路径用 AddUnique 加错误、节点却是有条件才加，下标会错位。
			// 每个表达式自己身上的 LastErrorText 才是可靠的那一份。
			TMap<UMaterialExpression*, FString> ErrorNodeIds;
			UAL_BuildExpressionIds(Material, ErrorNodeIds);

			TSet<UMaterialExpression*> Reported;
			for (UMaterialExpression* ErrorExpression : Resource->GetErrorExpressions())
			{
				if (!ErrorExpression || Reported.Contains(ErrorExpression)) continue;
				Reported.Add(ErrorExpression);

				const FString* FoundId = ErrorNodeIds.Find(ErrorExpression);
				TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
				NodeObj->SetStringField(TEXT("node_id"),
					FoundId ? *FoundId : ErrorExpression->GetName());
				NodeObj->SetStringField(TEXT("class"), ErrorExpression->GetClass()->GetName());
				if (!ErrorExpression->LastErrorText.IsEmpty())
				{
					NodeObj->SetStringField(TEXT("error"), ErrorExpression->LastErrorText);
				}
				// 材质函数内部的节点不在这张 id 表里 —— 说清楚它在哪，
				// 免得调用方拿着一个 get_graph 里根本不存在的 id 去找
				if (!FoundId)
				{
					NodeObj->SetStringField(TEXT("note"),
						TEXT("not in this material's own graph - either inside a MaterialFunction, "
							 "or a leftover reference to a node that no longer exists"));
				}
				ErrorNodesJson.Add(MakeShared<FJsonValueObject>(NodeObj));
			}
		}

		// 缺贴图**是不是**编译错误，取决于这条链有没有真的被用到：
		// 接在 Opacity 上而混合模式是 Opaque 时，这一支根本不参与编译，引擎不吭声；
		// 一旦改成 Translucent，同一张图就会报 "Missing input texture" 并把节点标红。
		//
		// 所以这里只当 warning，真错误由上面的 GetCompileErrors() 负责 ——
		// 那条路径覆盖得比手写检查全，也不会在"其实用不到"的时候误报。
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		for (UMaterialExpression* Expression : Material->GetExpressions())
#else
		for (UMaterialExpression* Expression : Material->Expressions)
#endif
		{
			UMaterialExpressionTextureBase* TexExpr = Cast<UMaterialExpressionTextureBase>(Expression);
			if (TexExpr && TexExpr->Texture == nullptr)
			{
				WarningsJson.Add(MakeShared<FJsonValueString>(FString::Printf(
					TEXT("%s '%s' has no texture assigned"),
					*Expression->GetClass()->GetName(), *Expression->GetName())));
			}
		}
	}

	const bool bHasErrors = ErrorsJson.Num() > 0;

	// 6. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("compiled"), !bHasErrors);
	Data->SetBoolField(TEXT("has_errors"), bHasErrors);
	Data->SetStringField(TEXT("material_path"), MaterialInterface->GetPathName());
	Data->SetStringField(TEXT("material_name"), MaterialInterface->GetName());
	Data->SetArrayField(TEXT("errors"), ErrorsJson);
	Data->SetArrayField(TEXT("warnings"), WarningsJson);
	Data->SetArrayField(TEXT("error_nodes"), ErrorNodesJson);

	if (bHasErrors)
	{
		UE_LOG(LogUALMaterial, Warning, TEXT("Material compiled with %d errors: %s"), 
			ErrorsJson.Num(), *MaterialInterface->GetName());
	}
	else
	{
		UE_LOG(LogUALMaterial, Log, TEXT("Compiled material successfully: %s"), *MaterialInterface->GetName());
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// Handle_SetMaterialNodeValue - 设置材质节点值
// ============================================================================
void FUAL_MaterialCommands::Handle_SetMaterialNodeValue(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: material_path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	FString NodeId;
	if (!Payload->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: node_id"));
		return;
	}

	// 2. 加载材质
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialPath);
	if (!Material)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		return;
	}

	// Target the copy the Material Editor is holding, not the asset.
	// Must happen BEFORE any node/pin lookup - see UAL_ResolveLiveMaterial.
	IMaterialEditor* MatEditor = nullptr;
	Material = UAL_ResolveLiveMaterial(Material, MatEditor);

	// 3. 查找目标节点（支持两种 ID 格式：计算的索引 ID 和 UE 对象实际名称）
	UMaterialExpression* TargetExpression = FindExpressionById(Material, NodeId);

	if (!TargetExpression)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Node not found: %s"), *NodeId));
		return;
	}

	// 4. 设置节点值
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), NodeId);
	Data->SetStringField(TEXT("node_class"), TargetExpression->GetClass()->GetName());

	// 使用 FUAL_ScopedTransaction 管理事务
	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "SetNodeValue", "Set Material Node Value"));

	// 在修改值之前调用 PreEdit 和 Modify
	Material->PreEditChange(nullptr);
	UAL_ModifyMaterialGraph(Material);
	TargetExpression->Modify();

	// 旧值先读，改完再读一次 —— 调用方拿这两个才能确认自己改的是想改的那个节点
	if (TSharedPtr<FJsonValue> OldValue = ReadNodeValue(TargetExpression))
	{
		Data->SetField(TEXT("old_value"), OldValue);
	}

	/*
	 * `value` 和 `properties` 至少给一个。只给 properties 是正常用法：
	 * Slab、Vertical Layer 这些节点根本没有「值」，要改的全是细节面板里的设置。
	 *
	 * properties 先设：它失败时 value 还没动，整个请求可以干净地 400。
	 * 反过来的话 value 已经写进去了，Cancel 事务并不会把内存里的值改回来。
	 */
	const TSharedPtr<FJsonValue> ValueField = Payload->TryGetField(TEXT("value"));
	const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
	const bool bHasProperties = Payload->TryGetObjectField(TEXT("properties"), PropertiesObj) && PropertiesObj &&
		(*PropertiesObj)->Values.Num() > 0;

	if (!ValueField.IsValid() && !bHasProperties)
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Nothing to set: pass value, properties, or both"));
		return;
	}

	if (bHasProperties)
	{
		TSharedPtr<FJsonObject> Readback;
		FString PropertyError;
		if (!UAL_ApplyNodeProperties(TargetExpression, *PropertiesObj, Readback, PropertyError))
		{
			/*
			 * 前面几条可能已经写进内存了（Cancel 不回滚内存），所以这里要把
			 * 「部分写入」说出来，而不是装作什么都没发生。
			 */
			Transaction.Cancel();
			const TArray<FString> Editable = UAL_JsonKeys(UAL_NodeEditableProperties(TargetExpression->GetClass()->GetDefaultObject<UMaterialExpression>()));
			UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
				TEXT("Cannot set properties on node %s: %s. Properties listed before the failing one may already be "
					 "changed - re-read with material_get_graph. Settable properties: %s"),
				*NodeId, *PropertyError, Editable.Num() ? *FString::Join(Editable, TEXT(", ")) : TEXT("(none)")));
			return;
		}
		Data->SetObjectField(TEXT("properties"), Readback);
	}

	if (ValueField.IsValid())
	{
		FString ApplyError;
		const bool bModified = ApplyValueToNode(TargetExpression, ValueField, ApplyError);

		if (!bModified)
		{
			// 这里原来无论如何都回 200。事务已经 Cancel 了，却还告诉调用方成功 ——
			// 于是「把 Constant3Vector 设成红色」这种当时根本做不到的事，
			// 每一次都被汇报成做到了，用户拿到一个黑材质而没有任何线索。
			Transaction.Cancel();
			UAL_CommandUtils::SendError(RequestId, 400,
				FString::Printf(TEXT("Cannot set value on node %s: %s%s"), *NodeId, *ApplyError,
					bHasProperties ? TEXT(" (the properties in this request were already applied)") : TEXT("")));
			return;
		}
	}

	if (TSharedPtr<FJsonValue> NewValue = ReadNodeValue(TargetExpression))
	{
		Data->SetField(TEXT("new_value"), NewValue);
	}

	// 广播变更并刷新编辑器
	FPropertyChangedEvent PropertyChangedEvent(nullptr, EPropertyChangeType::ValueSet);

	// 必须刷新具体的节点 (这会让节点在图表中刷新显示)
	TargetExpression->PostEditChangeProperty(PropertyChangedEvent);

	// 最后再通知材质 (触发整体编译)
	Material->PostEditChangeProperty(PropertyChangedEvent);

	// 强制刷新材质编辑器图表和逻辑
	if (Material->MaterialGraph)
	{
		UAL_RefreshMaterialEditor(MatEditor, Material);
	}

	Material->MarkPackageDirty();

	if (GEditor)
	{
		FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor");
		if (PropertyModule)
		{
			PropertyModule->NotifyCustomizationModuleChanged();
		}
	}

	UE_LOG(LogUALMaterial, Log, TEXT("Set value for node %s in material %s"),
		*NodeId, *Material->GetName());

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// Handle_DeleteMaterialNode - 删除材质节点
// ============================================================================
void FUAL_MaterialCommands::Handle_DeleteMaterialNode(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: material_path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	FString NodeId;
	if (!Payload->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: node_id"));
		return;
	}

	// 2. 加载材质
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialPath);
	if (!Material)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		return;
	}

	// Target the copy the Material Editor is holding, not the asset.
	// Must happen BEFORE any node/pin lookup - see UAL_ResolveLiveMaterial.
	IMaterialEditor* MatEditor = nullptr;
	Material = UAL_ResolveLiveMaterial(Material, MatEditor);

	// 3. 查找目标节点（支持两种 ID 格式：计算的索引 ID 和 UE 对象实际名称）
	UMaterialExpression* TargetExpression = FindExpressionById(Material, NodeId);

	if (!TargetExpression)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Node not found: %s"), *NodeId));
		return;
	}

	// 4. 从材质中移除节点
	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "DeleteNode", "Delete Material Node"));
	
	Material->PreEditChange(nullptr);
	UAL_ModifyMaterialGraph(Material);

	/*
	 * 先把指向这个节点的线全断掉，再把它从图里拿走。
	 *
	 * 原来这里只做了后半段，`disconnected_count` 直接写死 0（注释写着「简化版本」）。
	 * 后果不是少了个计数，而是**删完材质就编不过了**：节点没了，别人输入上
	 * 那个指针还指着它，图里留下一根挂在空气上的线。而删除操作报的是成功。
	 *
	 * 实测里的样子：删掉一个接在 Metallic 上的节点，material_compile 继续报错，
	 * 报错节点是一个 get_graph 里根本找不到的 id —— 排查的人会一路怀疑到别处去。
	 */
	int32 DisconnectedCount = 0;
	auto ClearIfPointsAtTarget = [&](FExpressionInput* Input)
	{
		if (Input && Input->Expression == TargetExpression)
		{
			Input->Expression = nullptr;
			Input->OutputIndex = 0;
			++DisconnectedCount;
		}
	};

	// 别的节点的输入
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	for (UMaterialExpression* Expression : Material->GetExpressions())
#else
	for (UMaterialExpression* Expression : Material->Expressions)
#endif
	{
		if (!Expression || Expression == TargetExpression) continue;
		Expression->Modify();
		const int32 InputCount = UALCompat::CountInputs(Expression);
		for (int32 i = 0; i < InputCount; ++i)
		{
			ClearIfPointsAtTarget(Expression->GetInput(i));
		}
	}

	// 材质主节点的输入 —— 漏掉这一段就是上面说的那种「删完编不过」
	for (const FUALRootInput& Root : UAL_CollectRootInputs(Material))
	{
		ClearIfPointsAtTarget(Root.Input);
	}

	/*
	 * 删一个节点是**四句**，顺序和配套都不能省
	 * （引擎的 `MaterialEditor.cpp` 删除路径、`MaterialEditingLibrary.cpp` 都是这四句）：
	 *
	 *     MaterialExpression->Modify();
	 *     Material->GetExpressionCollection().RemoveExpression(MaterialExpression);
	 *     Material->RemoveExpressionParameter(MaterialExpression);
	 *     MaterialExpression->MarkAsGarbage();
	 *
	 * `Modify()` **必须排在最前面，而且必须打在被删的这个表达式身上**。
	 * 上面那个断连循环 `if (!Expression || Expression == TargetExpression) continue;`
	 * 恰好把它跳过了，于是它是全图唯一一个没进事务的对象 —— 而
	 * `EditorTransaction.cpp` 里撤销时那句 `Object->ClearGarbage()` 只对
	 * **有 FObjectRecord 的对象**（也就是被 Modify() 过的）执行。
	 * 少了它，撤销会把一个已经 MarkAsGarbage 的对象放回表达式数组，
	 * 下一次 GC 把那个槽位清成 null，而 `UMaterialGraph::RebuildGraph` 遍历
	 * `GetExpressions()` 时**不判空**就解引用 —— 删一个节点再 Ctrl+Z 就是一次崩溃。
	 *
	 * `RemoveExpressionParameter`：`Material->EditorParameters` 是个**裸 TMap，
	 * 不是 UPROPERTY**，GC 不会帮它清。不摘的话它会一直攥着一个已经删掉的表达式指针，
	 * 直到材质被重新打开才重建。
	 *
	 * `MarkAsGarbage`：告诉 GC 这个对象可以回收了。这一句在补上 `Material` 回指针
	 * 之后更要紧 —— 现在被删掉的表达式身上带着一个看着合法的材质指针，
	 * 却已经不在 `GetExpressions()` 里，正好是引擎断言「不可能出现」的那个状态。
	 */
	TargetExpression->Modify();

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	Material->GetExpressionCollection().RemoveExpression(TargetExpression);
#else
	Material->Expressions.Remove(TargetExpression);
#endif

	Material->RemoveExpressionParameter(TargetExpression);
	TargetExpression->MarkAsGarbage();

	// 5. 标记材质已修改并刷新编辑器
	FPropertyChangedEvent PropertyChangedEvent(nullptr, EPropertyChangeType::ValueSet);
	Material->PostEditChangeProperty(PropertyChangedEvent);
	
	// 强制刷新材质编辑器图表和逻辑
	if (Material && Material->MaterialGraph)
	{
		UAL_RefreshMaterialEditor(MatEditor, Material);
	}
	
	Material->MarkPackageDirty();

	// 通知编辑器
	if (GEditor)
	{
		// 刷新属性面板
		FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor");
		if (PropertyModule)
		{
			PropertyModule->NotifyCustomizationModuleChanged();
		}
	}

	// 6. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), NodeId);
	Data->SetNumberField(TEXT("disconnected_count"), DisconnectedCount);

	UE_LOG(LogUALMaterial, Log, TEXT("Deleted node %s from material %s"), 
		*NodeId, *Material->GetName());

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// Handle_SetMaterialNodePositions - 批量挪节点（排版落地）
// ============================================================================
void FUAL_MaterialCommands::Handle_SetMaterialNodePositions(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: material_path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	const TArray<TSharedPtr<FJsonValue>>* PositionsArray = nullptr;
	if (!Payload->TryGetArrayField(TEXT("positions"), PositionsArray) || !PositionsArray ||
		PositionsArray->Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing or empty required field: positions"));
		return;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialPath);
	if (!Material)
	{
		UAL_CommandUtils::SendError(RequestId, 404,
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		return;
	}

	// Target the copy the Material Editor is holding, not the asset.
	// Must happen BEFORE any node/pin lookup - see UAL_ResolveLiveMaterial.
	IMaterialEditor* MatEditor = nullptr;
	Material = UAL_ResolveLiveMaterial(Material, MatEditor);

	// 排版这件事用户十有八九会想反悔一次，得能 Ctrl+Z
	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "LayoutMaterialGraph", "Layout Material Graph"));
	UAL_ModifyMaterialGraph(Material);

	int32 Moved = 0;
	TArray<TSharedPtr<FJsonValue>> NotFound;

	for (const TSharedPtr<FJsonValue>& Value : *PositionsArray)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj || !(*Obj).IsValid()) continue;

		FString NodeId;
		if (!(*Obj)->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty()) continue;

		UMaterialExpression* Expression = FindExpressionById(Material, NodeId);
		if (!Expression)
		{
			// 挪不到的单列出来，而不是静默跳过 —— 调用方多半拿着一份过期的图在算坐标，
			// 那种情况下「成功挪了 3 个」会把真正的问题盖掉
			NotFound.Add(MakeShared<FJsonValueString>(NodeId));
			continue;
		}

		int32 X = Expression->MaterialExpressionEditorX;
		int32 Y = Expression->MaterialExpressionEditorY;
		(*Obj)->TryGetNumberField(TEXT("x"), X);
		(*Obj)->TryGetNumberField(TEXT("y"), Y);

		Expression->Modify();
		Expression->MaterialExpressionEditorX = X;
		Expression->MaterialExpressionEditorY = Y;
		++Moved;
	}

	if (Moved == 0)
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 404,
			TEXT("None of the given node_ids exist in this material. Call material.get_graph for the current ids."));
		return;
	}

	// 只挪位置不改逻辑，所以不需要 PostEditChange（那会触发整个材质重编译）。
	// 但图表要刷新，否则编辑器窗口里节点还停在原处
	if (Material->MaterialGraph)
	{
		Material->MaterialGraph->RebuildGraph();
	}
	Material->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("moved"), Moved);
	Data->SetArrayField(TEXT("not_found"), NotFound);
	Data->SetStringField(TEXT("material_path"), MaterialPath);

	UE_LOG(LogUALMaterial, Log, TEXT("Moved %d nodes in material %s (%d not found)"),
		Moved, *Material->GetName(), NotFound.Num());

	// Let the open Material Editor repaint - see UAL_RefreshMaterialEditor
	UAL_RefreshMaterialEditor(MatEditor, Material);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// 智能容错辅助函数
// ============================================================================

FString FUAL_MaterialCommands::NormalizePath(const FString& InputPath, const FString& DefaultPrefix)
{
	FString Result = InputPath.TrimStartAndEnd();
	
	// 移除可能的文件扩展名
	if (Result.EndsWith(TEXT(".uasset")))
	{
		Result = Result.LeftChop(7);
	}
	
	// 替换反斜杠为正斜杠
	Result.ReplaceInline(TEXT("\\"), TEXT("/"));
	
	// 移除多余的斜杠
	while (Result.Contains(TEXT("//")))
	{
		Result.ReplaceInline(TEXT("//"), TEXT("/"));
	}
	
	// 如果不以 /Game/ 开头，尝试智能补全
	if (!Result.StartsWith(TEXT("/Game/")))
	{
		// 如果以 / 开头但不是 /Game/，可能是其他路径
		if (!Result.StartsWith(TEXT("/")))
		{
			// 完全没有斜杠前缀，补全为默认路径
			Result = DefaultPrefix / Result;
		}
	}
	
	return Result;
}

TArray<FString> FUAL_MaterialCommands::FindSimilarAssets(const FString& PartialPath, const FString& AssetClass)
{
	TArray<FString> Results;
	
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	// 提取搜索名称
	FString SearchName = FPaths::GetBaseFilename(PartialPath);
	
	// 搜索资产
	TArray<FAssetData> AssetDataList;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	AssetRegistry.GetAssetsByClass(FTopLevelAssetPath(*AssetClass), AssetDataList);
#else
	AssetRegistry.GetAssetsByClass(FName(*AssetClass), AssetDataList);
#endif
	
	for (const FAssetData& AssetData : AssetDataList)
	{
		FString AssetName = AssetData.AssetName.ToString();
		// 简单的模糊匹配：名称包含搜索词
		if (AssetName.Contains(SearchName, ESearchCase::IgnoreCase))
		{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			Results.Add(AssetData.GetSoftObjectPath().ToString());
#else
			Results.Add(AssetData.ObjectPath.ToString());
#endif
			if (Results.Num() >= 5) break; // 最多返回5个
		}
	}
	
	return Results;
}

bool FUAL_MaterialCommands::ParseBlendMode(const FString& Value, EBlendMode& OutMode)
{
	FString LowerValue = Value.ToLower().TrimStartAndEnd();
	
	// 支持多种格式: 枚举名、中文、数字
	if (LowerValue == TEXT("opaque") || LowerValue == TEXT("0") || LowerValue == TEXT("不透明"))
	{
		OutMode = BLEND_Opaque;
		return true;
	}
	if (LowerValue == TEXT("masked") || LowerValue == TEXT("1") || LowerValue == TEXT("遮罩"))
	{
		OutMode = BLEND_Masked;
		return true;
	}
	if (LowerValue == TEXT("translucent") || LowerValue == TEXT("2") || LowerValue == TEXT("半透明"))
	{
		OutMode = BLEND_Translucent;
		return true;
	}
	if (LowerValue == TEXT("additive") || LowerValue == TEXT("3") || LowerValue == TEXT("叠加"))
	{
		OutMode = BLEND_Additive;
		return true;
	}
	if (LowerValue == TEXT("modulate") || LowerValue == TEXT("4") || LowerValue == TEXT("调制"))
	{
		OutMode = BLEND_Modulate;
		return true;
	}
	if (LowerValue == TEXT("alphacomposite") || LowerValue == TEXT("5"))
	{
		OutMode = BLEND_AlphaComposite;
		return true;
	}
	if (LowerValue == TEXT("alphaholdout") || LowerValue == TEXT("6"))
	{
		OutMode = BLEND_AlphaHoldout;
		return true;
	}
#if UAL_WITH_SUBSTRATE
	/*
	 * 彩色透射（彩色玻璃）。引擎枚举上写着 SUBSTRATE_ONLY：没开 Substrate 时
	 * 设上去不报错，渲染却不是那回事。所以没开就当成不认识，
	 * 让调用方从 valid_values 里看到它不在（GetValidBlendModes 同样按开关列）。
	 */
	if ((LowerValue == TEXT("translucentcoloredtransmittance") || LowerValue == TEXT("7")) && UAL_IsSubstrateEnabled())
	{
		OutMode = BLEND_TranslucentColoredTransmittance;
		return true;
	}
#endif
	
	return false;
}

bool FUAL_MaterialCommands::ParseShadingModel(const FString& Value, EMaterialShadingModel& OutModel)
{
	FString LowerValue = Value.ToLower().TrimStartAndEnd();
	
	if (LowerValue == TEXT("defaultlit") || LowerValue == TEXT("default") || LowerValue == TEXT("默认") || LowerValue == TEXT("默认光照"))
	{
		OutModel = MSM_DefaultLit;
		return true;
	}
	if (LowerValue == TEXT("unlit") || LowerValue == TEXT("无光照") || LowerValue == TEXT("自发光"))
	{
		OutModel = MSM_Unlit;
		return true;
	}
	if (LowerValue == TEXT("subsurface") || LowerValue == TEXT("次表面"))
	{
		OutModel = MSM_Subsurface;
		return true;
	}
	if (LowerValue == TEXT("clearcoat") || LowerValue == TEXT("清漆"))
	{
		OutModel = MSM_ClearCoat;
		return true;
	}
	if (LowerValue == TEXT("twosidedfoliage") || LowerValue == TEXT("双面植物"))
	{
		OutModel = MSM_TwoSidedFoliage;
		return true;
	}
	
	return false;
}

TArray<FString> FUAL_MaterialCommands::GetValidBlendModes()
{
	TArray<FString> Modes = { TEXT("Opaque"), TEXT("Masked"), TEXT("Translucent"), TEXT("Additive"), TEXT("Modulate"),
		TEXT("AlphaComposite"), TEXT("AlphaHoldout") };
	if (UAL_IsSubstrateEnabled())
	{
		Modes.Add(TEXT("TranslucentColoredTransmittance"));
	}
	return Modes;
}

TArray<FString> FUAL_MaterialCommands::GetValidShadingModels()
{
	return { TEXT("DefaultLit"), TEXT("Unlit"), TEXT("Subsurface"), TEXT("ClearCoat"), TEXT("TwoSidedFoliage") };
}

bool FUAL_MaterialCommands::ApplyValueToNode(
	UMaterialExpression* Expression, const TSharedPtr<FJsonValue>& RawValue, FString& OutError)
{
	if (!Expression)
	{
		OutError = TEXT("node is null");
		return false;
	}

	FUALMaterialValue Value;
	if (!ParseMaterialValue(RawValue, Value, OutError))
	{
		return false;
	}

	const FString ClassName = Expression->GetClass()->GetName();

	// --- 标量 ---
	if (UMaterialExpressionConstant* ConstExpr = Cast<UMaterialExpressionConstant>(Expression))
	{
		if (!Value.bHasScalar)
		{
			OutError = FString::Printf(TEXT("%s needs a number"), *ClassName);
			return false;
		}
		ConstExpr->R = Value.Scalar;
		return true;
	}
	if (UMaterialExpressionScalarParameter* ScalarParam = Cast<UMaterialExpressionScalarParameter>(Expression))
	{
		if (!Value.bHasScalar)
		{
			OutError = FString::Printf(TEXT("%s needs a number"), *ClassName);
			return false;
		}
		ScalarParam->DefaultValue = Value.Scalar;
		return true;
	}

	// --- 向量 / 颜色 ---
	//
	// 这四类以前一个都设不了：set_node_value 只认 Constant / ScalarParameter /
	// 贴图三种，走到别的类就取消事务然后**照样回 200**。
	// 也就是说「给材质一个红色」这件事在这套工具里做不到，而调用方每次都被告知做到了。
	if (UMaterialExpressionConstant2Vector* Const2 = Cast<UMaterialExpressionConstant2Vector>(Expression))
	{
		if (!Value.bHasColor)
		{
			OutError = FString::Printf(TEXT("%s needs a number, a 2-4 element array, or {r,g,b} / {x,y}"), *ClassName);
			return false;
		}
		Const2->R = Value.Color.R;
		Const2->G = Value.Color.G;
		return true;
	}
	if (UMaterialExpressionConstant3Vector* Const3 = Cast<UMaterialExpressionConstant3Vector>(Expression))
	{
		if (!Value.bHasColor)
		{
			OutError = FString::Printf(TEXT("%s needs a number, a 2-4 element array, or {r,g,b} / {x,y,z}"), *ClassName);
			return false;
		}
		Const3->Constant = FLinearColor(Value.Color.R, Value.Color.G, Value.Color.B, 1.f);
		return true;
	}
	if (UMaterialExpressionConstant4Vector* Const4 = Cast<UMaterialExpressionConstant4Vector>(Expression))
	{
		if (!Value.bHasColor)
		{
			OutError = FString::Printf(TEXT("%s needs a number, a 2-4 element array, or {r,g,b,a} / {x,y,z,w}"), *ClassName);
			return false;
		}
		Const4->Constant = Value.Color;
		return true;
	}
	if (UMaterialExpressionVectorParameter* VectorParam = Cast<UMaterialExpressionVectorParameter>(Expression))
	{
		if (!Value.bHasColor)
		{
			OutError = FString::Printf(TEXT("%s needs a number, a 2-4 element array, or {r,g,b,a} / {x,y,z,w}"), *ClassName);
			return false;
		}
		VectorParam->DefaultValue = Value.Color;
		return true;
	}

	/*
	 * --- 静态开关 ---
	 *
	 * `StaticSwitchParameter` 是 `StaticBoolParameter` 的子类，一个分支管两个。
	 *
	 * 这两个类型一直在 NODE_TYPES 和 PARAMETER_NODE_TYPES 里，schema 也收布尔，
	 * 可这里一个分支都没有 —— 于是「建一个默认开着的 UseSnow 开关」的下场是
	 * 200 + `initial_value_applied:false` + 一句「现在是默认值」，而且整套工具里
	 * **没有第二个**能设它。工具比后端窄，窄在这儿。
	 *
	 * `DefaultValue` 是个 `uint32:1` 位域，所以显式转 0/1，别直接塞 float。
	 */
	if (UMaterialExpressionStaticBoolParameter* StaticBool =
		Cast<UMaterialExpressionStaticBoolParameter>(Expression))
	{
		if (!Value.bHasScalar)
		{
			OutError = FString::Printf(TEXT("%s needs true/false (a number also works: 0 is off)"), *ClassName);
			return false;
		}
		StaticBool->DefaultValue = Value.Scalar != 0.f ? 1 : 0;
		return true;
	}

	// --- 贴图 ---
	// TextureBase 一次覆盖 TextureSample / TextureSampleParameter2D / TextureObject
	if (UMaterialExpressionTextureBase* TextureExpr = Cast<UMaterialExpressionTextureBase>(Expression))
	{
		if (!Value.bHasString)
		{
			OutError = FString::Printf(TEXT("%s needs a texture asset path string"), *ClassName);
			return false;
		}
		UTexture* Texture = LoadObject<UTexture>(nullptr, *Value.String);
		if (!Texture)
		{
			OutError = FString::Printf(TEXT("Texture not found: %s"), *Value.String);
			return false;
		}
		TextureExpr->Texture = Texture;
		// 同 add_node 那条路：不跟这一句，法线/遮罩贴图挂上去就是编译不过，
		// 而 SamplerType 这套工具改不了
		TextureExpr->AutoSetSampleType();
		return true;
	}

	// --- UV ---
	if (UMaterialExpressionTextureCoordinate* Coord = Cast<UMaterialExpressionTextureCoordinate>(Expression))
	{
		if (Value.bHasUv)
		{
			Coord->UTiling = Value.UTiling;
			Coord->VTiling = Value.VTiling;
			return true;
		}
		if (Value.bHasScalar)
		{
			// 单个数字表示两个方向平铺同样多，这是最常见的用法
			Coord->UTiling = Value.Scalar;
			Coord->VTiling = Value.Scalar;
			return true;
		}
		OutError = FString::Printf(TEXT("%s needs a number or {u_tiling, v_tiling}"), *ClassName);
		return false;
	}

	// --- 分量遮罩 ---
	//
	// ComponentMask 建得出来、却一直**设不了取哪几路**：R/G/B/A 四个位没有任何
	// 命令碰得到，而构造函数一个都不设（`UMaterialExpressionComponentMask` 的
	// ctor 只填 MenuCategories），四位全 0 编译出来恒为 0。
	//
	// 也就是说这个节点在这套工具里从加进 NodeTypeMap 那天起就是废的。真机上的
	// 绕法是 DotProduct(p, (1,0)) / DotProduct(p, (0,1)) 取分量 —— 一个节点的事
	// 变成两个，几十个节点的图直接翻倍。
	if (UMaterialExpressionComponentMask* MaskExpr = Cast<UMaterialExpressionComponentMask>(Expression))
	{
		if (!Value.bHasString)
		{
			OutError = FString::Printf(
				TEXT("%s needs a channel string like \"R\", \"RG\", \"RGB\", \"A\" (xyzw spelling also accepted)"),
				*ClassName);
			return false;
		}

		bool bR = false, bG = false, bB = false, bA = false;
		if (!UAL_ParseChannelMask(Value.String, bR, bG, bB, bA, OutError))
		{
			OutError = FString::Printf(TEXT("%s %s"), *ClassName, *OutError);
			return false;
		}

		MaskExpr->R = bR ? 1 : 0;
		MaskExpr->G = bG ? 1 : 0;
		MaskExpr->B = bB ? 1 : 0;
		MaskExpr->A = bA ? 1 : 0;
		return true;
	}

	// 落到这里说明这类节点根本没有「值」可设（Add / Multiply / Sine 之类都是纯运算）。
	// 明确说出来，别让调用方以为设成功了
	OutError = FString::Printf(
		TEXT("%s has no settable value. Nodes with values: Constant, Constant2/3/4Vector, ")
		TEXT("ScalarParameter, VectorParameter, StaticBoolParameter, StaticSwitchParameter, ")
		TEXT("TextureSample, TextureObject, TextureCoordinate, ComponentMask"),
		*ClassName);
	return false;
}

UMaterialExpression* FUAL_MaterialCommands::FindExpressionById(UMaterial* Material, const FString& NodeId)
{
	if (!Material || NodeId.IsEmpty()) return nullptr;

	/*
	 * 两种 id 长得一模一样，必须分先后。
	 *
	 * 方式1「位置 id」：ClassName_数组下标 —— get_graph / add_node 返回的就是它。
	 * 方式2「对象名」：UE 给对象起的名字，也长成 ClassName_数字，但那个数字是
	 *      每个 Outer 内部的自增计数，和数组下标**没有关系**。
	 *
	 * 两者在图没动过时通常重合，一旦经历删除 / 撤销 / 重加就会错位：
	 * 位置 1 上的那个节点，对象名可能正好叫 ..._2。原来这里在同一趟循环里
	 * 用 `||` 一起比，谁先出现谁被返回 —— 于是传进来一个位置 id ..._2，
	 * 返回的却是位置 1 那个节点。
	 *
	 * 表现出来是「连线报成功，回读却显示接在另一个节点上」，而且时对时错 ——
	 * 只有在两套编号错位时才发作，看起来像随机抖动。真机测试里抓到过：
	 * 同一个替换操作，连甲写到了乙身上，下一次连乙又是对的。
	 *
	 * 所以：位置 id 全表优先匹配，对象名只在没有任何位置 id 命中时兜底。
	 */
	/*
	 * GUID 排在最前面，因为它是唯一**稳的**那个。
	 *
	 * 引擎给每个 UMaterialExpression 都存了 MaterialExpressionGuid，在
	 * PostInitProperties（新建时）和 PostLoad（加载时）里生成，是个 UPROPERTY，
	 * 跟着对象走、跟着资产存盘 —— 删别的节点、撤销、重开工程都不会变。
	 *
	 * 上面那两种 id 都是位置或计数派生的，删一个节点后面全体位移。
	 * 调用方拿着一个隔了几步的 id 回来改图，改中的是另一个节点，而且不报错。
	 */
	FGuid RequestedGuid;
	if (FGuid::Parse(NodeId, RequestedGuid) && RequestedGuid.IsValid())
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		for (UMaterialExpression* Expression : Material->GetExpressions())
#else
		for (UMaterialExpression* Expression : Material->Expressions)
#endif
		{
			if (Expression && Expression->GetMaterialExpressionId() == RequestedGuid)
			{
				return Expression;
			}
		}
		return nullptr;
	}

	UMaterialExpression* MatchedByObjectName = nullptr;

	int32 Index = 0;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	for (UMaterialExpression* Expression : Material->GetExpressions())
#else
	for (UMaterialExpression* Expression : Material->Expressions)
#endif
	{
		if (!Expression) continue;

		const FString Computed = FString::Printf(TEXT("%s_%d"), *Expression->GetClass()->GetName(), Index++);
		if (Computed == NodeId)
		{
			return Expression;
		}
		if (!MatchedByObjectName && Expression->GetName() == NodeId)
		{
			MatchedByObjectName = Expression;
		}
	}
	return MatchedByObjectName;
}

bool FUAL_MaterialCommands::ResolveOutputIndex(
	UMaterialExpression* Expression, const FString& PinName, int32& OutIndex, TArray<FString>& OutAvailable)
{
	OutIndex = 0;
	if (!Expression) return false;

	const TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();

	/*
	 * 认的名字和 `get_graph` 报出来的**必须是同一套**，所以两边都走
	 * `UAL_OutputPinName`（真名优先，没真名就按 mask 位推 R/G/B/A）。
	 *
	 * 以前这里只比 `OutputName`，而 `Constant3Vector` 的四个输出一个名字都没有 ——
	 * 于是 `source_pin: "G"` 匹配不到任何一个，掉进最后那条「一个具名输出都没有
	 * 就接 0 号」的兜底，**静默接上 RGB**。返回体照抄入参，看起来完全正确，
	 * 图里却是另一根线。换句话说：这类节点的分量输出以前根本连不出去，
	 * 而且不报错。真机上的绕法是 DotProduct(p, (1,0)) 取分量，一个节点的事变两个。
	 */
	// 有没有输出带**真名**。带真名的节点（TextureSample、BreakMaterialAttributes）
	// 不吃下面那套整体输出别名 —— 详见别名那一段的注释
	bool bAnyNamed = false;
	for (int32 i = 0; i < Outputs.Num(); ++i)
	{
		if (!Outputs[i].OutputName.IsNone()) { bAnyNamed = true; break; }
	}

	/*
	 * 名字**只在失败时才拼**。
	 *
	 * `OutAvailable` 唯一的读者是调用方的 400 分支（拼进 available_outputs），
	 * 而绝大多数调用是成功的。原来这里无条件先建一个 `Names` 数组、再整体
	 * `Append` 进 `OutAvailable`，等于每次调用都把每个输出名各建两遍 FString ——
	 * 一个 35 输出的 BreakMaterialAttributes 就是 70 次堆分配，一次 200 根线的
	 * apply_graph 上万次，全在游戏线程上，而成功那条路一个都不读。
	 */
	auto FillAvailable = [&Expression, &Outputs, &OutAvailable]()
	{
		OutAvailable.Reserve(OutAvailable.Num() + Outputs.Num());
		for (int32 i = 0; i < Outputs.Num(); ++i)
		{
			OutAvailable.Add(UAL_OutputPinName(Expression, i));
		}
	};

	// 没给名字：取第一个输出。绝大多数节点只有这一个，强求调用方报出名字
	// 只会平添失败 —— 何况多数节点的唯一输出根本没有名字
	// 但**一个输出都没有的节点不能给 0**：那个 0 会被存进 FExpressionInput，
	// 下一次读图 `GetOutputType(0)` 就是无保护的 `GetOutputs()[0]`，直接崩编辑器。
	// 数字那条路有 IsValidIndex，这条空引脚的近路当时漏了。
	if (PinName.IsEmpty())
	{
		if (Outputs.Num() > 0) return true;
		FillAvailable();
		return false;
	}

	/*
	 * 纯数字当序号用，但**必须真的在范围内**。
	 *
	 * 这里原来对「一个输出都没有」的节点放行任意序号（`Outputs.Num() == 0 ||`）。
	 * 那条短路会崩编辑器：没挂函数资产的 MaterialFunctionCall 构造时就
	 * `Outputs.Empty()`，于是 `from: "fn.0"` 存下一个越界的 OutputIndex，
	 * 同一次调用里的排版步骤紧接着读图，`GetOutputType(0)` 的引擎实现是
	 * 无保护的 `GetOutputs()[OutputIndex]` —— TArray 的范围检查是 fatal。
	 */
	// `<N>` 是没有真名的输出在 available_outputs 和 get_graph 里的写法，和纯数字一样按序号收；
	// 以前单输出节点上写 `<0>` 会因为名字变成了 Out 而直接 400
	FString IndexText = PinName;
	if (IndexText.Len() > 2 && IndexText.StartsWith(TEXT("<")) && IndexText.EndsWith(TEXT(">")))
	{
		IndexText = IndexText.Mid(1, IndexText.Len() - 2);
	}
	/*
	 * 判「全是数字」要自己来，不能用 `FString::IsNumeric()`。
	 *
	 * 它认正负号和小数点（`CString.h` 里先跳过一个 '+'/'-'，剩下的数字和点都收），
	 * 于是 `"+"` / `"-"` / `"1.5"` 全算数字，`Atoi("+")` 是 0、`Atoi("1.5")` 是 1 ——
	 * `tex.<+>` 就这么静默接上 RGB，而正确答案是回一个 400 带上可用输出名单。
	 * 静默接错正是这个函数被重写的起因。
	 */
	bool bAllDigits = IndexText.Len() > 0;
	for (const TCHAR Ch : IndexText)
	{
		if (!FChar::IsDigit(Ch))
		{
			bAllDigits = false;
			break;
		}
	}
	if (bAllDigits)
	{
		const int32 Index = FCString::Atoi(*IndexText);
		if (Outputs.IsValidIndex(Index))
		{
			OutIndex = Index;
			return true;
		}
		FillAvailable();
		return false;
	}

	for (int32 i = 0; i < Outputs.Num(); ++i)
	{
		if (UAL_OutputPinName(Expression, i).Equals(PinName, ESearchCase::IgnoreCase))
		{
			OutIndex = i;
			return true;
		}
	}

	/*
	 * 「整体输出」的几种叫法，一律指 0 号。
	 *
	 * 这一段是给 `Add` / `Multiply` / `Constant3Vector` 这类**输出没有真名**的节点
	 * 留的：它们的名字要么是推出来的（RGB/R/G/B），要么是兜底的 `Out`，
	 * 而调用方历来写的是 `Default` / `RGB`。
	 *
	 * **只在没有任何真名输出时才收**（`!bAnyNamed`）—— 这一条不能少。
	 * 少了它，`BreakMaterialAttributes` 那 35 个真名输出上写 `.Default`
	 * 会静默接上 0 号 BaseColor（float3 灌进 Roughness，编译得过、画面不对、
	 * 返回体照抄入参）；`Constant4Vector` 上写 `.RGB` 会接上 4 通道的 RGBA。
	 * 有真名的节点上认不出的名字一律回 400，错误里带 available_outputs，
	 * 让调用方照着改，比替它猜一个强。
	 */
	/*
	 * `Outputs.Num() > 0` 这半句不能省。
	 *
	 * `bAnyNamed` 是遍历 Outputs 算出来的，数组是空的时候它**空着为假**，
	 * 于是这道闸对「一个输出都没有」的节点是敞开的：写 `fn.Out` 会走到下面
	 * `OutIndex = 0; return true;`，把一个越界的 OutputIndex 存进 FExpressionInput，
	 * 下一次读图 `GetOutputs()[0]` 直接崩编辑器。空引脚那条路和数字那条路
	 * 都各自挡了这件事，唯独别名这条当时没挡。
	 */
	if (!bAnyNamed && Outputs.Num() > 0)
	{
		static const TCHAR* WholeOutputAliases[] = {
			TEXT("Default"), TEXT("Out"), TEXT("Output"), TEXT("Value")
		};
		for (const TCHAR* Alias : WholeOutputAliases)
		{
			if (PinName.Equals(Alias, ESearchCase::IgnoreCase))
			{
				OutIndex = 0;
				return true;
			}
		}

		/*
		 * `RGB` / `RGBA` **只在单输出节点上**算整体别名。
		 *
		 * 它们不是「整体」的意思，是**通道宽度**的意思。多输出节点上收下的后果
		 * 很具体：`Constant4Vector` 的五路输出是 RGBA/R/G/B/A（全无真名，所以
		 * bAnyNamed 是 false，上面那道闸拦不住），写 `.RGB` 会接上 4 通道的 RGBA；
		 * 反过来 `Constant3Vector` 上写 `.RGBA` 会接上 3 通道那一路，alpha 悄悄没了。
		 * 那两种必须 400。
		 *
		 * 但**整条删掉是过头了**：`Add` / `Multiply` / `Lerp` / `Fresnel` 这些只有
		 * 一路输出，名字是兜底推出来的 `Out`，写 `.RGB` 指的就是那唯一一路，
		 * 不可能指错。删掉之后它们全变 400，而 apply_graph 失败即停不回滚 ——
		 * 前面建好的节点就那么留在用户的材质里；`.RGB` 还正是工具描述里给的例子。
		 * 按输出个数分开：只有一路时没有歧义，收下。
		 */
		if (Outputs.Num() == 1 &&
			(PinName.Equals(TEXT("RGB"), ESearchCase::IgnoreCase) ||
			 PinName.Equals(TEXT("RGBA"), ESearchCase::IgnoreCase)))
		{
			OutIndex = 0;
			return true;
		}
	}

	// 认不出来 —— 这是唯一真正需要 available_outputs 的地方，名单到这儿才拼
	FillAvailable();
	return false;
}

TSharedPtr<FJsonValue> FUAL_MaterialCommands::ReadNodeValue(UMaterialExpression* Expression)
{
	if (!Expression) return nullptr;

	auto ColorValue = [](const FLinearColor& Color, bool bIncludeAlpha) -> TSharedPtr<FJsonValue>
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("r"), Color.R);
		Obj->SetNumberField(TEXT("g"), Color.G);
		Obj->SetNumberField(TEXT("b"), Color.B);
		if (bIncludeAlpha) Obj->SetNumberField(TEXT("a"), Color.A);
		return MakeShared<FJsonValueObject>(Obj);
	};

	if (UMaterialExpressionConstant* ConstExpr = Cast<UMaterialExpressionConstant>(Expression))
	{
		return MakeShared<FJsonValueNumber>(ConstExpr->R);
	}
	if (UMaterialExpressionScalarParameter* ScalarParam = Cast<UMaterialExpressionScalarParameter>(Expression))
	{
		return MakeShared<FJsonValueNumber>(ScalarParam->DefaultValue);
	}
	if (UMaterialExpressionConstant2Vector* Const2 = Cast<UMaterialExpressionConstant2Vector>(Expression))
	{
		return ColorValue(FLinearColor(Const2->R, Const2->G, 0.f, 1.f), false);
	}
	if (UMaterialExpressionConstant3Vector* Const3 = Cast<UMaterialExpressionConstant3Vector>(Expression))
	{
		return ColorValue(Const3->Constant, false);
	}
	if (UMaterialExpressionConstant4Vector* Const4 = Cast<UMaterialExpressionConstant4Vector>(Expression))
	{
		return ColorValue(Const4->Constant, true);
	}
	if (UMaterialExpressionVectorParameter* VectorParam = Cast<UMaterialExpressionVectorParameter>(Expression))
	{
		return ColorValue(VectorParam->DefaultValue, true);
	}
	// 静态开关（StaticSwitchParameter 是 StaticBoolParameter 的子类，一并覆盖）。
	// 这里必须和 ApplyValueToNode 同步：search_nodes 的 has_value 拿本函数当判据，
	// 少一条就会对着一个其实设得上的类型说「不能设初始值」
	if (UMaterialExpressionStaticBoolParameter* StaticBool =
		Cast<UMaterialExpressionStaticBoolParameter>(Expression))
	{
		return MakeShared<FJsonValueBoolean>(StaticBool->DefaultValue != 0);
	}
	if (UMaterialExpressionTextureBase* TextureExpr = Cast<UMaterialExpressionTextureBase>(Expression))
	{
		return MakeShared<FJsonValueString>(
			TextureExpr->Texture ? TextureExpr->Texture->GetPathName() : FString());
	}
	if (UMaterialExpressionTextureCoordinate* Coord = Cast<UMaterialExpressionTextureCoordinate>(Expression))
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("u_tiling"), Coord->UTiling);
		Obj->SetNumberField(TEXT("v_tiling"), Coord->VTiling);
		return MakeShared<FJsonValueObject>(Obj);
	}

	/*
	 * 遮罩取哪几路，用和写入同一套写法回（"RG" / "X" / "RGBA"）。
	 *
	 * 不回读的话，「这个 ComponentMask 取的是 x 还是 y」在图上看不出来 ——
	 * 而取错通道是个静默错误：材质照样编译通过，画面只是不对。
	 */
	if (UMaterialExpressionComponentMask* MaskExpr = Cast<UMaterialExpressionComponentMask>(Expression))
	{
		FString Channels;
		if (MaskExpr->R) Channels += TEXT("R");
		if (MaskExpr->G) Channels += TEXT("G");
		if (MaskExpr->B) Channels += TEXT("B");
		if (MaskExpr->A) Channels += TEXT("A");
		return MakeShared<FJsonValueString>(Channels);
	}

	return nullptr;
}

// ============================================================================
// Handle_DuplicateMaterial - 复制材质（智能容错）
// ============================================================================
void FUAL_MaterialCommands::Handle_DuplicateMaterial(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析源路径（智能补全）
	FString SourcePathRaw;
	if (!Payload->TryGetStringField(TEXT("source_path"), SourcePathRaw) || SourcePathRaw.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: source_path"));
		return;
	}
	
	FString SourcePath = NormalizePath(SourcePathRaw);
	
	// 2. 尝试加载源材质
	UMaterialInterface* SourceMaterial = LoadObject<UMaterialInterface>(nullptr, *SourcePath);
	if (!SourceMaterial)
	{
		// 查找相似资产提供建议
		TArray<FString> SimilarAssets = FindSimilarAssets(SourcePathRaw, TEXT("MaterialInterface"));
		
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetBoolField(TEXT("success"), false);
		ErrorData->SetStringField(TEXT("error"), FString::Printf(TEXT("Material not found: %s"), *SourcePath));
		
		TArray<TSharedPtr<FJsonValue>> SuggestionArray;
		SuggestionArray.Add(MakeShared<FJsonValueString>(TEXT("检查路径是否正确，应以 /Game/ 开头")));
		SuggestionArray.Add(MakeShared<FJsonValueString>(TEXT("使用 material.describe 工具确认材质存在")));
		ErrorData->SetArrayField(TEXT("suggestions"), SuggestionArray);
		
		if (SimilarAssets.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> SimilarArray;
			for (const FString& Asset : SimilarAssets)
			{
				SimilarArray.Add(MakeShared<FJsonValueString>(Asset));
			}
			ErrorData->SetArrayField(TEXT("similar_assets"), SimilarArray);
		}
		
		UAL_CommandUtils::SendResponse(RequestId, 404, ErrorData);
		return;
	}
	
	// 3. 解析可选参数
	FString NewName;
	if (!Payload->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
	{
		NewName = SourceMaterial->GetName() + TEXT("_Copy");
	}
	
	FString DestPath;
	if (!Payload->TryGetStringField(TEXT("destination_path"), DestPath) || DestPath.IsEmpty())
	{
		DestPath = FPaths::GetPath(SourcePath);
	}
	else
	{
		DestPath = NormalizePath(DestPath);
	}
	
	// 4. 构建新资产路径并检查冲突
	FString NewAssetPath = DestPath / NewName;
	int32 CopyIndex = 1;
	while (LoadObject<UObject>(nullptr, *NewAssetPath))
	{
		NewAssetPath = DestPath / FString::Printf(TEXT("%s_%d"), *NewName, CopyIndex++);
		if (CopyIndex > 100)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Too many name conflicts"));
			return;
		}
	}
	
	/*
	 * 复制是从**资产**拷的，所以编辑器里那份没保存的改动不会进副本。
	 *
	 * 这里曾经调 `UAL_ApplyMaterialEditorToAsset` 想把改动先推回资产，那是错的：
	 * 那个函数走的是编辑器的 SaveAsset 命令，也就是
	 * 1) **整包写磁盘**，而 `UpdateOriginalMaterial` 是拿工作副本整个覆盖资产，
	 *    没有「只推我改的那部分」这回事 —— 用户自己那些打算丢掉的改动一起被落盘，
	 *    而存盘不可撤销，`ue_undo` 捞不回来；
	 * 2) 工作副本编不过时会弹**模态框**（compile errors 那个警告，九个版本都在），
	 *    而插件的命令是从 FTSTicker 上派发的、模态循环里根本不转 ——
	 *    编辑器卡死，这条 RPC 永远不回；
	 * 3) 它返回 void，用户点 Abort 时 `UpdateOriginalMaterial` 回 false、什么都没推，
	 *    调用方照样收 200 —— 正是它想修的那个「静默拿到旧数据」；
	 * 4) 它排在事务外面，复制失败回 500 的那条路上，源材质**已经被覆盖并落盘了**。
	 *
	 * 所以改成不写盘，只**如实说一句**：编辑器开着就在回执里讲清楚这次拷的是磁盘那份。
	 * 要把编辑器里的改动带进副本，调用方先 ue_save（或者用户点一下 Apply）再来。
	 */
	const bool bSourceHasOpenEditor =
		UAL_FindMaterialEditor(Cast<UMaterial>(SourceMaterial)) != nullptr;

	// 5. 执行复制 (使用 AssetTools 以更好支持编辑器集成)
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	
	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "DuplicateMaterial", "Duplicate Material"));

	UObject* DuplicatedAsset = AssetToolsModule.Get().DuplicateAsset(
		FPaths::GetBaseFilename(NewAssetPath), 
		FPaths::GetPath(NewAssetPath), 
		SourceMaterial);

	if (!DuplicatedAsset)
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to duplicate material"));
		return;
	}

	// 标记已创建
	FAssetRegistryModule::AssetCreated(DuplicatedAsset);
	
	// 6. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("source_path"), SourcePath);
	Data->SetStringField(TEXT("new_path"), NewAssetPath);
	Data->SetStringField(TEXT("new_name"), FPaths::GetBaseFilename(NewAssetPath));

	if (bSourceHasOpenEditor)
	{
		Data->SetBoolField(TEXT("source_had_open_editor"), true);
		Data->SetStringField(TEXT("source_note"),
			TEXT("The source material is open in the Material Editor. This copy was made from the asset on "
				 "disk, so anything changed in that window but not yet applied/saved is NOT in the copy. "
				 "Call ue_save (or have the user press Apply) first if you need those changes copied."));
	}

	UE_LOG(LogUALMaterial, Log, TEXT("Duplicated material %s to %s"), *SourcePath, *NewAssetPath);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// Handle_SetMaterialProperty - 设置材质属性（智能容错）
// ============================================================================
void FUAL_MaterialCommands::Handle_SetMaterialProperty(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析路径
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);
	
	// 2. 加载材质（必须是 UMaterial，不是 Instance）
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialPath);
	if (!Material)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Material not found or is MaterialInstance: %s"), *MaterialPath));
		return;
	}

	// Target the copy the Material Editor is holding, not the asset.
	// Must happen BEFORE any node/pin lookup - see UAL_ResolveLiveMaterial.
	IMaterialEditor* MatEditor = nullptr;
	Material = UAL_ResolveLiveMaterial(Material, MatEditor);
	
	// 3. 解析属性对象
	const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("properties"), PropertiesObj) || !PropertiesObj)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: properties"));
		return;
	}
	
	// 4. 处理各属性
	TArray<FString> UpdatedProperties;
	TArray<TSharedPtr<FJsonValue>> FailedProperties;

	// 使用 FUAL_ScopedTransaction 自动管理事务生命周期
	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "SetMaterialProperty", "Set Material Properties"));
	
	// 在修改任何属性前调用 PreEditChange 和 Modify。
	// 走统一的那个函数：老资产可能缺 RF_Transactional，改属性同样撤不回来
	Material->PreEditChange(nullptr);
	UAL_ModifyMaterialGraph(Material);

	// blend_mode
	FString BlendModeStr;
	if ((*PropertiesObj)->TryGetStringField(TEXT("blend_mode"), BlendModeStr))
	{
		EBlendMode NewMode;
		if (ParseBlendMode(BlendModeStr, NewMode))
		{
			Material->BlendMode = NewMode;
			UpdatedProperties.Add(TEXT("blend_mode"));
		}
		else
		{
			TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("name"), TEXT("blend_mode"));
			FailObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Invalid value: %s"), *BlendModeStr));
			TArray<TSharedPtr<FJsonValue>> ValidValues;
			for (const FString& V : GetValidBlendModes())
			{
				ValidValues.Add(MakeShared<FJsonValueString>(V));
			}
			FailObj->SetArrayField(TEXT("valid_values"), ValidValues);
			FailedProperties.Add(MakeShared<FJsonValueObject>(FailObj));
		}
	}
	
	// shading_model
	FString ShadingModelStr;
	if ((*PropertiesObj)->TryGetStringField(TEXT("shading_model"), ShadingModelStr))
	{
		EMaterialShadingModel NewModel;
		if (ParseShadingModel(ShadingModelStr, NewModel))
		{
			Material->SetShadingModel(NewModel);
			UpdatedProperties.Add(TEXT("shading_model"));
		}
		else
		{
			TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("name"), TEXT("shading_model"));
			FailObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Invalid value: %s"), *ShadingModelStr));
			TArray<TSharedPtr<FJsonValue>> ValidValues;
			for (const FString& V : GetValidShadingModels())
			{
				ValidValues.Add(MakeShared<FJsonValueString>(V));
			}
			FailObj->SetArrayField(TEXT("valid_values"), ValidValues);
			FailedProperties.Add(MakeShared<FJsonValueObject>(FailObj));
		}
	}
	
	// two_sided
	bool bTwoSided;
	if ((*PropertiesObj)->TryGetBoolField(TEXT("two_sided"), bTwoSided))
	{
		Material->TwoSided = bTwoSided;
		UpdatedProperties.Add(TEXT("two_sided"));
	}
	
	// 5. 标记材质已修改并刷新编辑器
	if (UpdatedProperties.Num() > 0)
	{
		// 广播属性变更 (触发重编译和 UI 更新)
		FPropertyChangedEvent PropertyChangedEvent(nullptr, EPropertyChangeType::ValueSet);
		Material->PostEditChangeProperty(PropertyChangedEvent);
		
		// 强制刷新材质编辑器图表和逻辑
		if (Material && Material->MaterialGraph)
		{
			UAL_RefreshMaterialEditor(MatEditor, Material);
		}
		
		Material->MarkPackageDirty();
		
		// 仅通知属性面板刷新
		if (GEditor)
		{
			FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor");
			if (PropertyModule)
			{
				PropertyModule->NotifyCustomizationModuleChanged();
			}
		}
	}
	else 
	{
		// 如果没有修改，取消事务
		Transaction.Cancel();
	}
	
	// 6. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material_path"), MaterialPath);
	
	TArray<TSharedPtr<FJsonValue>> UpdatedArray;
	for (const FString& Prop : UpdatedProperties)
	{
		UpdatedArray.Add(MakeShared<FJsonValueString>(Prop));
	}
	Data->SetArrayField(TEXT("updated_properties"), UpdatedArray);
	Data->SetArrayField(TEXT("failed_properties"), FailedProperties);
	
	// 当前状态
	TSharedPtr<FJsonObject> CurrentState = MakeShared<FJsonObject>();
	CurrentState->SetStringField(TEXT("blend_mode"), StaticEnum<EBlendMode>()->GetNameStringByValue((int64)Material->BlendMode));
	CurrentState->SetBoolField(TEXT("two_sided"), Material->TwoSided);
	Data->SetObjectField(TEXT("current_state"), CurrentState);
	
	/*
	 * 同 set_param：一条都没成时回的是 400，而 400 那条路上 toOutcome 根本不跑，
	 * `failed_properties` 那个渲染器就成了死代码 —— 原因得写进顶层 error 才到得了模型。
	 *
	 * 条件要和**下面那个 400 的条件一模一样**（都是 `UpdatedProperties.Num() == 0`），
	 * 不能再多一个 `FailedProperties.Num() > 0`：`properties: {}` 这种一条都没解析出来的
	 * 请求两个数组都是空的，照样回 400，而顶层没有 error 的 400 在盒子那边就是
	 * 一句「未提供失败原因」—— 正是这段代码要消灭的东西。
	 */
	if (UpdatedProperties.Num() == 0)
	{
		const FString Detail = UAL_JoinJsonErrors(FailedProperties, TEXT("name"));
		Data->SetStringField(TEXT("error"),
			Detail.IsEmpty()
				? TEXT("no property was changed: 'properties' had no recognizable entries. "
					   "Give at least one of blend_mode / shading_model / two_sided")
				: *FString::Printf(TEXT("no property was changed: %s"), *Detail));
	}

	UE_LOG(LogUALMaterial, Log, TEXT("Set %d properties on material %s"), UpdatedProperties.Num(), *Material->GetName());

	UAL_CommandUtils::SendResponse(RequestId, UpdatedProperties.Num() > 0 ? 200 : 400, Data);
}

// ============================================================================
// Handle_CreateMaterialInstance - 创建材质实例（智能容错）
// ============================================================================
void FUAL_MaterialCommands::Handle_CreateMaterialInstance(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析父材质路径
	FString ParentPathRaw;
	if (!Payload->TryGetStringField(TEXT("parent_path"), ParentPathRaw) || ParentPathRaw.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: parent_path"));
		return;
	}
	FString ParentPath = NormalizePath(ParentPathRaw);
	
	// 2. 加载父材质
	UMaterialInterface* ParentMaterial = LoadObject<UMaterialInterface>(nullptr, *ParentPath);
	if (!ParentMaterial)
	{
		TArray<FString> SimilarAssets = FindSimilarAssets(ParentPathRaw, TEXT("MaterialInterface"));
		
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetBoolField(TEXT("success"), false);
		ErrorData->SetStringField(TEXT("error"), FString::Printf(TEXT("Parent material not found: %s"), *ParentPath));
		
		if (SimilarAssets.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> SimilarArray;
			for (const FString& Asset : SimilarAssets)
			{
				SimilarArray.Add(MakeShared<FJsonValueString>(Asset));
			}
			ErrorData->SetArrayField(TEXT("similar_materials"), SimilarArray);
		}
		
		TArray<TSharedPtr<FJsonValue>> Suggestions;
		Suggestions.Add(MakeShared<FJsonValueString>(TEXT("确保父材质路径正确")));
		Suggestions.Add(MakeShared<FJsonValueString>(TEXT("使用 material.describe 检查材质是否存在")));
		ErrorData->SetArrayField(TEXT("suggestions"), Suggestions);
		
		UAL_CommandUtils::SendResponse(RequestId, 404, ErrorData);
		return;
	}
	
	// 3. 解析可选参数
	FString InstanceName;
	if (!Payload->TryGetStringField(TEXT("instance_name"), InstanceName) || InstanceName.IsEmpty())
	{
		InstanceName = TEXT("MI_") + ParentMaterial->GetName();
	}
	
	FString DestPath;
	if (!Payload->TryGetStringField(TEXT("destination_path"), DestPath) || DestPath.IsEmpty())
	{
		DestPath = FPaths::GetPath(ParentPath);
	}
	else
	{
		DestPath = NormalizePath(DestPath);
	}
	
	/*
	 * 实例是照**父材质资产**建的，参数表也从那儿来 —— 编辑器里还没保存的参数
	 * 不会出现在这个实例上。
	 *
	 * 这里曾经调 `UAL_ApplyMaterialEditorToAsset` 先把改动推回资产，已经撤掉了：
	 * 那条路会整包写磁盘（连用户打算丢掉的改动一起）、编不过时弹模态框把编辑器
	 * 和这条 RPC 一起卡死、失败了还不声不响回 200。理由完整写在 material.duplicate
	 * 那边同一段注释里。
	 *
	 * 改成不写盘、只如实报一句：父材质开着编辑器时在回执里讲清楚参数表来自磁盘那份。
	 */
	const bool bParentHasOpenEditor =
		UAL_FindMaterialEditor(Cast<UMaterial>(ParentMaterial)) != nullptr;

	// 4. 创建材质实例
	FString PackageName = DestPath / InstanceName;
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create package"));
		return;
	}
	
	// 4. 创建材质实例 (使用 Factory 和事务)
	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "CreateMaterialInstance", "Create Material Instance"));

	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
	Factory->InitialParent = ParentMaterial;

	UMaterialInstanceConstant* NewInstance = Cast<UMaterialInstanceConstant>(
		Factory->FactoryCreateNew(
			UMaterialInstanceConstant::StaticClass(),
			Package,
			*InstanceName,
			RF_Public | RF_Standalone | RF_Transactional,
			nullptr,
			GWarn
		));

	if (!NewInstance)
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create material instance"));
		return;
	}
	
	// 5. 标记包已修改
	NewInstance->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewInstance);

	
	// 6. 收集可用参数信息
	TArray<TSharedPtr<FJsonValue>> ScalarParams;
	TArray<TSharedPtr<FJsonValue>> VectorParams;
	TArray<TSharedPtr<FJsonValue>> TextureParams;
	
	TArray<FMaterialParameterInfo> ParamInfos;
	TArray<FGuid> ParamIds;
	
	ParentMaterial->GetAllScalarParameterInfo(ParamInfos, ParamIds);
	for (const FMaterialParameterInfo& Info : ParamInfos)
	{
		ScalarParams.Add(MakeShared<FJsonValueString>(Info.Name.ToString()));
	}
	
	ParamInfos.Empty();
	ParamIds.Empty();
	ParentMaterial->GetAllVectorParameterInfo(ParamInfos, ParamIds);
	for (const FMaterialParameterInfo& Info : ParamInfos)
	{
		VectorParams.Add(MakeShared<FJsonValueString>(Info.Name.ToString()));
	}
	
	ParamInfos.Empty();
	ParamIds.Empty();
	ParentMaterial->GetAllTextureParameterInfo(ParamInfos, ParamIds);
	for (const FMaterialParameterInfo& Info : ParamInfos)
	{
		TextureParams.Add(MakeShared<FJsonValueString>(Info.Name.ToString()));
	}
	
	// 7. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("instance_path"), PackageName);
	Data->SetStringField(TEXT("instance_name"), InstanceName);
	Data->SetStringField(TEXT("parent_path"), ParentPath);
	
	TSharedPtr<FJsonObject> AvailableParams = MakeShared<FJsonObject>();
	AvailableParams->SetArrayField(TEXT("scalar_params"), ScalarParams);
	AvailableParams->SetArrayField(TEXT("vector_params"), VectorParams);
	AvailableParams->SetArrayField(TEXT("texture_params"), TextureParams);
	Data->SetObjectField(TEXT("available_params"), AvailableParams);

	if (bParentHasOpenEditor)
	{
		Data->SetBoolField(TEXT("parent_had_open_editor"), true);
		Data->SetStringField(TEXT("parent_note"),
			TEXT("The parent material is open in the Material Editor. available_params comes from the asset "
				 "on disk, so parameters added in that window but not yet applied/saved are missing here and "
				 "material_set_param will reject them. Call ue_save (or have the user press Apply) on the "
				 "parent, then create the instance."));
	}

	UE_LOG(LogUALMaterial, Log, TEXT("Created material instance %s from %s"), *InstanceName, *ParentMaterial->GetName());
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// material.disconnect_pins / material.delete_unused_nodes
//
// 补的是同一个洞：图**只能往上加，不能往回收**。
// 只能连不能断 → 改图只好整个删了重建；不能清理 → 试错留下的死节点
// 会一直躺在图里，每次 get_graph 都要重读一遍，模型还会被它们带偏。
// ============================================================================

namespace
{
	// 主节点输入表原来在这里还有一份（漏了 SubsurfaceColor 和 MaterialAttributes）。
	// 全文件统一用文件开头那个 UAL_CollectRootInputs —— 同一张表漏过两次就够了。

	/** 图里的全部表达式。5.1 起换成了 GetExpressions() */
	TArray<UMaterialExpression*> UAL_AllExpressions(UMaterial* Material)
	{
		TArray<UMaterialExpression*> Out;
		if (!Material)
		{
			return Out;
		}
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		for (UMaterialExpression* Expression : Material->GetExpressions())
#else
		for (UMaterialExpression* Expression : Material->Expressions)
#endif
		{
			if (Expression)
			{
				Out.Add(Expression);
			}
		}
		return Out;
	}

	/** 改完图之后统一走这一套刷新，少一步渲染端就看不到改动 */
	void UAL_RefreshMaterial(UMaterial* Material)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		Material->PostEditChange();
#else
		// 5.0 上不发这个，改动不会传到渲染端 —— 图看着对，球还是旧的
		FPropertyChangedEvent PropertyChangedEvent(nullptr, EPropertyChangeType::ValueSet);
		Material->PostEditChangeProperty(PropertyChangedEvent);
#endif
		if (Material->MaterialGraph)
		{
			Material->MaterialGraph->RebuildGraph();
		}
		Material->MarkPackageDirty();
	}
}

void FUAL_MaterialCommands::Handle_DisconnectMaterialPins(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: material_path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	FString TargetNode;
	if (!Payload->TryGetStringField(TEXT("target_node"), TargetNode) || TargetNode.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400,
			TEXT("Missing required field: target_node (use \"Material\" for the main material node)"));
		return;
	}

	FString TargetPin;
	Payload->TryGetStringField(TEXT("target_pin"), TargetPin);

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialPath);
	if (!Material)
	{
		UAL_CommandUtils::SendError(RequestId, 404,
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		return;
	}

	// Target the copy the Material Editor is holding, not the asset.
	// Must happen BEFORE any node/pin lookup - see UAL_ResolveLiveMaterial.
	IMaterialEditor* MatEditor = nullptr;
	Material = UAL_ResolveLiveMaterial(Material, MatEditor);

	FExpressionInput* Input = nullptr;
	FString MatchedPin;
	TArray<FString> Available;

	if (TargetNode.Equals(TEXT("Material"), ESearchCase::IgnoreCase))
	{
		if (TargetPin.IsEmpty())
		{
			UAL_CommandUtils::SendError(RequestId, 400,
				TEXT("target_pin is required when target_node is \"Material\""));
			return;
		}
		for (const FUALRootInput& Root : UAL_CollectRootInputs(Material))
		{
			Available.Add(Root.Name);
			if (Root.Input && TargetPin.Equals(Root.Name, ESearchCase::IgnoreCase))
			{
				Input = Root.Input;
				MatchedPin = Root.Name;
			}
		}
	}
	else
	{
		UMaterialExpression* Expression = FindExpressionById(Material, TargetNode);
		if (!Expression)
		{
			UAL_CommandUtils::SendError(RequestId, 404,
				FString::Printf(TEXT("Node not found: %s"), *TargetNode));
			return;
		}

		// 名字留空时断第一个输入 —— 和 connect_pins 的口径保持一致，
		// 单输入节点占多数，强制报出引脚名只会平添失败
		const int32 InputCount = UALCompat::CountInputs(Expression);
		for (int32 i = 0; i < InputCount; ++i)
		{
			// 报出去的名字就是拿来比对的那个 —— 两者分家的话，错误消息会列出
			// `<0>` 然后又拒绝 `<0>`
			const FString Name = UAL_InputPinName(Expression, i);
			Available.Add(Name);

			const bool bMatches = !TargetPin.IsEmpty() && Name.Equals(TargetPin, ESearchCase::IgnoreCase);
			const bool bTakeFirst = TargetPin.IsEmpty() && i == 0;
			if ((bMatches || bTakeFirst) && !Input)
			{
				Input = Expression->GetInput(i);
				MatchedPin = Name;
			}
		}
	}

	if (!Input)
	{
		// 把实际有哪些输入列出来。只说「找不到」的话调用方只能换名字反复猜
		UAL_CommandUtils::SendError(RequestId, 400,
			FString::Printf(TEXT("Input pin '%s' not found on %s. Available inputs: %s"),
				*TargetPin, *TargetNode,
				Available.Num() ? *FString::Join(Available, TEXT(", ")) : TEXT("(none)")));
		return;
	}

	// 本来就没接线不算错：调用方常常只是想「确保这里是断的」，
	// 报成失败会逼它先查一次再决定要不要调
	const bool bWasConnected = Input->Expression != nullptr;
	const FString WasConnectedTo = bWasConnected ? Input->Expression->GetName() : FString();

	if (bWasConnected)
	{
		FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "DisconnectPins", "Disconnect Material Pins"));
		UAL_ModifyMaterialGraph(Material);
		Input->Expression = nullptr;
		Input->OutputIndex = 0;
		UAL_RefreshMaterial(Material);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("disconnected"), bWasConnected);
	Data->SetStringField(TEXT("target_node"), TargetNode);
	Data->SetStringField(TEXT("target_pin"), MatchedPin.IsEmpty() ? TEXT("<0>") : *MatchedPin);
	if (bWasConnected)
	{
		Data->SetStringField(TEXT("was_connected_to"), WasConnectedTo);
	}
	else
	{
		Data->SetStringField(TEXT("note"), TEXT("That input was already empty - nothing to do."));
	}

	UE_LOG(LogUALMaterial, Log, TEXT("Disconnected %s.%s in %s"), *TargetNode, *MatchedPin, *Material->GetName());
	// Let the open Material Editor repaint - see UAL_RefreshMaterialEditor
	UAL_RefreshMaterialEditor(MatEditor, Material);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_MaterialCommands::Handle_DeleteUnusedMaterialNodes(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: material_path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	// 默认只报不删。删除不可逆，先让调用方看一眼名单 ——
	// 尤其是模型调用时，「顺手清理一下」很容易把它自己还要用的中间节点清掉
	bool bDryRun = true;
	Payload->TryGetBoolField(TEXT("dry_run"), bDryRun);

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialPath);
	if (!Material)
	{
		UAL_CommandUtils::SendError(RequestId, 404,
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		return;
	}

	// Target the copy the Material Editor is holding, not the asset.
	// Must happen BEFORE any node/pin lookup - see UAL_ResolveLiveMaterial.
	IMaterialEditor* MatEditor = nullptr;
	Material = UAL_ResolveLiveMaterial(Material, MatEditor);

	// 从主节点各输入反向走可达性
	TSet<UMaterialExpression*> Reachable;
	TArray<UMaterialExpression*> Frontier;

	for (const FUALRootInput& Root : UAL_CollectRootInputs(Material))
	{
		if (Root.Input && Root.Input->Expression)
		{
			Frontier.Add(Root.Input->Expression);
		}
	}

	while (Frontier.Num() > 0)
	{
		UMaterialExpression* Current = Frontier.Pop();
		if (!Current || Reachable.Contains(Current))
		{
			continue;
		}
		Reachable.Add(Current);

		const int32 InputCount = UALCompat::CountInputs(Current);
		for (int32 i = 0; i < InputCount; ++i)
		{
			if (FExpressionInput* In = Current->GetInput(i))
			{
				if (In->Expression)
				{
					Frontier.Add(In->Expression);
				}
			}
		}
	}

	// node_id 必须和 get_graph / add_node 同一套（数组下标派生）。原来回的是
	// `GetName()` —— 对象名的后缀和下标对不上（add_node 回 `..._4`，这里报 `..._0`），
	// 模型拿它去 delete_node / set_node_value，FindExpressionById 按下标就命中了**别的**节点。
	// 编号要在删除之前取：删完下标就变了
	TMap<UMaterialExpression*, FString> ExpressionToId;
	UAL_BuildExpressionIds(Material, ExpressionToId);

	TArray<UMaterialExpression*> Unused;
	TArray<TSharedPtr<FJsonValue>> UnusedJson;
	for (UMaterialExpression* Expression : UAL_AllExpressions(Material))
	{
		if (Reachable.Contains(Expression))
		{
			continue;
		}
		Unused.Add(Expression);

		const FString* FoundId = ExpressionToId.Find(Expression);
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("node_id"), FoundId ? *FoundId : Expression->GetName());
		// 稳定 id：dry_run 和真删之间图要是动过，下标会移位，guid 不会
		Obj->SetStringField(TEXT("guid"), Expression->GetMaterialExpressionId().ToString());
		Obj->SetStringField(TEXT("class"), Expression->GetClass()->GetName());
		UnusedJson.Add(MakeShared<FJsonValueObject>(Obj));
	}

	int32 DeletedCount = 0;
	if (!bDryRun && Unused.Num() > 0)
	{
		FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "DeleteUnusedNodes", "Delete Unused Material Nodes"));
		Material->PreEditChange(nullptr);
		UAL_ModifyMaterialGraph(Material);

		for (UMaterialExpression* Expression : Unused)
		{
			/*
			 * `Modify()` 在前，理由同 material.delete_node 那段注释（撤销要靠它）。
			 *
			 * **但这里不跟 `MarkAsGarbage()`**，和单删那条路不一样 —— 因为这条路
			 * 判「没用上」判得不准：可达性只顺着 `FExpressionInput` 走，而
			 * `NamedRerouteUsage` 是靠一个 `TObjectPtr<Declaration>` 连过去的、没有输入；
			 * `UMaterialExpressionCustomOutput` 那一族（ClearCoatNormal、BentNormal、
			 * ThinTranslucent…）压根不接主节点，编译器是扫 `GetExpressions()` 找到它们的；
			 * `UAL_CollectRootInputs` 那张表也还缺 Anisotropy / Tangent / Refraction /
			 * PixelDepthOffset / CustomizedUVs（FrontMaterial 已补上 —— 缺它的时候，
			 * 这里会把整张 Substrate 图当成没用上的节点一起摘掉）。
			 *
			 * 判错一个节点，只从数组里摘掉还能靠撤销捞回来；再 MarkAsGarbage
			 * 就是下一次 GC 之后彻底没了。判据补齐之前，这一步按可恢复的来。
			 */
			Expression->Modify();

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			Material->GetExpressionCollection().RemoveExpression(Expression);
#else
			Material->Expressions.Remove(Expression);
#endif
			Material->RemoveExpressionParameter(Expression);
			++DeletedCount;
		}

		UAL_RefreshMaterial(Material);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("dry_run"), bDryRun);
	Data->SetNumberField(TEXT("unused_count"), Unused.Num());
	Data->SetArrayField(TEXT("unused"), UnusedJson);
	Data->SetNumberField(TEXT("deleted_count"), DeletedCount);
	if (bDryRun && Unused.Num() > 0)
	{
		Data->SetStringField(TEXT("note"),
			TEXT("Nothing was deleted. Review the list, then call again with dry_run=false."));
	}

	UE_LOG(LogUALMaterial, Log, TEXT("Unused nodes in %s: %d (deleted %d)"),
		*Material->GetName(), Unused.Num(), DeletedCount);
	// Let the open Material Editor repaint - see UAL_RefreshMaterialEditor
	UAL_RefreshMaterialEditor(MatEditor, Material);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// material.parameter_collection
//
// 「一个开关控制全场景材质」的标准做法：昼夜、季节、队伍配色、受击闪白。
// 此前没有 MPC，这类需求只能退化成「逐个材质实例改参数」——
// 材质一多就不可行，而且运行时根本改不了。
// ============================================================================

namespace
{
	/** MPC 当前的参数快照，回给调用方核对 */
	TSharedPtr<FJsonObject> UAL_CollectionToJson(UMaterialParameterCollection* Collection)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("collection_path"), Collection->GetPathName());
		Obj->SetStringField(TEXT("collection_name"), Collection->GetName());

		TArray<TSharedPtr<FJsonValue>> Scalars;
		for (const FCollectionScalarParameter& Param : Collection->ScalarParameters)
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("name"), Param.ParameterName.ToString());
			P->SetNumberField(TEXT("value"), Param.DefaultValue);
			Scalars.Add(MakeShared<FJsonValueObject>(P));
		}

		TArray<TSharedPtr<FJsonValue>> Vectors;
		for (const FCollectionVectorParameter& Param : Collection->VectorParameters)
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("name"), Param.ParameterName.ToString());
			TSharedPtr<FJsonObject> V = MakeShared<FJsonObject>();
			V->SetNumberField(TEXT("r"), Param.DefaultValue.R);
			V->SetNumberField(TEXT("g"), Param.DefaultValue.G);
			V->SetNumberField(TEXT("b"), Param.DefaultValue.B);
			V->SetNumberField(TEXT("a"), Param.DefaultValue.A);
			P->SetObjectField(TEXT("value"), V);
			Vectors.Add(MakeShared<FJsonValueObject>(P));
		}

		Obj->SetArrayField(TEXT("scalars"), Scalars);
		Obj->SetArrayField(TEXT("vectors"), Vectors);
		// MPC 的硬上限。超了引擎会静默丢弃后面的，所以主动报出来
		Obj->SetNumberField(TEXT("scalar_slots_left"), 16 - Collection->ScalarParameters.Num());
		Obj->SetNumberField(TEXT("vector_slots_left"), 16 - Collection->VectorParameters.Num());
		return Obj;
	}

	/*
	 * 这里原来有个 `UAL_ParseLinearColor`，已经删掉。
	 *
	 * 它是这个文件里第三份「JSON → 值」的实现，而且是最松的一份：对**任何**
	 * JSON 对象都回 true，缺的分量当 0。唯一的调用方（MPC 向量参数）已经改走
	 * `ParseMaterialValue`，留着它只会等下一个人再踩一次同样的坑。
	 */
}

void FUAL_MaterialCommands::Handle_MaterialParameterCollection(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Action = TEXT("list");
	Payload->TryGetStringField(TEXT("action"), Action);
	Action = Action.ToLower();

	UMaterialParameterCollection* Collection = nullptr;

	// ── create ──────────────────────────────────────────────────────────
	if (Action == TEXT("create"))
	{
		FString CollectionName;
		if (!Payload->TryGetStringField(TEXT("collection_name"), CollectionName) || CollectionName.IsEmpty())
		{
			UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: collection_name (for action=create)"));
			return;
		}

		FString DestinationPath = TEXT("/Game/Materials");
		Payload->TryGetStringField(TEXT("destination_path"), DestinationPath);

		const FString PackagePath = DestinationPath / CollectionName;
		UPackage* Package = CreatePackage(*PackagePath);
		if (!Package)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create package for parameter collection"));
			return;
		}

		FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "CreateMPC", "Create Material Parameter Collection"));

		UMaterialParameterCollectionFactoryNew* Factory = NewObject<UMaterialParameterCollectionFactoryNew>();
		Collection = Cast<UMaterialParameterCollection>(Factory->FactoryCreateNew(
			UMaterialParameterCollection::StaticClass(),
			Package,
			FName(*CollectionName),
			RF_Public | RF_Standalone | RF_Transactional,
			nullptr,
			GWarn));

		if (!Collection)
		{
			Transaction.Cancel();
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create UMaterialParameterCollection"));
			return;
		}

		FAssetRegistryModule::AssetCreated(Collection);
	}
	else
	{
		FString CollectionPath;
		if (!Payload->TryGetStringField(TEXT("collection_path"), CollectionPath) || CollectionPath.IsEmpty())
		{
			UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: collection_path"));
			return;
		}
		CollectionPath = NormalizePath(CollectionPath);

		Collection = LoadObject<UMaterialParameterCollection>(nullptr, *CollectionPath);
		if (!Collection)
		{
			UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(
				TEXT("Material parameter collection not found: %s"), *CollectionPath));
			return;
		}
	}

	// ── list 到这里就够了 ───────────────────────────────────────────────
	if (Action == TEXT("list"))
	{
		TSharedPtr<FJsonObject> Data = UAL_CollectionToJson(Collection);
		UAL_CommandUtils::SendResponse(RequestId, 200, Data);
		return;
	}

	if (Action != TEXT("create") && Action != TEXT("set"))
	{
		UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
			TEXT("Unknown action '%s'. Use \"create\", \"list\" or \"set\""), *Action));
		return;
	}

	// ── create / set 共用的写参数路径 ───────────────────────────────────
	/*
	 * 这段要**自己开事务**。
	 *
	 * 上面 create 分支里那个事务在 `if` 的花括号处就结束了（它只保住「建出资产」
	 * 这一步），而 `action=set` 那条路压根没开过。于是下面这个 `Collection->Modify()`
	 * 一直在往一个不存在的事务上记录，等于没记 —— 改完 MPC 参数
	 * `ue_undo` / Ctrl+Z 一声不吭地什么都不做，工具却报成功。
	 *
	 * create 会因此分成两步撤销（先退参数、再退建资产）。这是可以接受的：
	 * 把事务提到函数顶上倒是能并成一步，但那样 list 和几条 400/404 也会各压一条
	 * 空记录进撤销栈，用户按一次 Ctrl+Z 撤掉的会是别的东西。
	 */
	FUAL_ScopedTransaction ParamTransaction(
		NSLOCTEXT("UALMaterial", "SetMPCParams", "Set Material Parameter Collection Values"));
	// 同 set_param：只在真的缺标记时才补。`RF_Transactional` 要落盘而 SetFlags 撤不回，
	// 无条件写会让一次什么都没改的调用也改掉用户资产。
	// （MPC 走 UMaterialParameterCollectionFactoryNew 建出来时本来就带这个标记，
	//  所以这里基本是个空操作，留着只为兜住手工建/导入的那些。）
	if (!Collection->HasAnyFlags(RF_Transactional))
	{
		Collection->SetFlags(RF_Transactional);
	}
	// 同 set_param：脏标记留给真写成的那条路打，别在校验之前就打上
	Collection->Modify(/*bAlwaysMarkDirty=*/false);

	TArray<FString> Rejected;
	/** 真正写进去几条。一条都没有就把事务取消掉，别在撤销栈上留一条空记录 */
	int32 WrittenCount = 0;

	const TArray<TSharedPtr<FJsonValue>>* ScalarsJson = nullptr;
	if (Payload->TryGetArrayField(TEXT("scalars"), ScalarsJson) && ScalarsJson)
	{
		for (const TSharedPtr<FJsonValue>& Entry : *ScalarsJson)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(Obj) || !Obj)
			{
				continue;
			}

			FString Name;
			if (!(*Obj)->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
			{
				continue;
			}
			// 和下面的向量一样走 ParseMaterialValue：返回值丢了的话，
			// 非数字的 value 会悄悄变成 0.0 再报一句「已更新」
			FUALMaterialValue Parsed;
			FString ParseError;
			if (!ParseMaterialValue((*Obj)->TryGetField(TEXT("value")), Parsed, ParseError)
				|| !Parsed.bHasScalar)
			{
				Rejected.Add(FString::Printf(TEXT("scalar '%s': value must be a number (%s)"),
					*Name, ParseError.IsEmpty() ? TEXT("got a non-numeric value") : *ParseError));
				continue;
			}
			const float Value = Parsed.Scalar;

			const int32 Existing = UAL_FindScalarParam(Collection, FName(*Name));
			if (Existing != INDEX_NONE)
			{
				Collection->ScalarParameters[Existing].DefaultValue = static_cast<float>(Value);
				++WrittenCount;
				continue;
			}

			// 16 是引擎的硬上限，超了会被静默丢弃 —— 主动挡下来并报出去，
			// 否则调用方拿到一个「成功」，然后在材质里怎么也引用不到这个参数
			if (Collection->ScalarParameters.Num() >= 16)
			{
				Rejected.Add(FString::Printf(TEXT("scalar '%s' (collection is full, 16 max)"), *Name));
				continue;
			}

			FCollectionScalarParameter Param;
			Param.ParameterName = FName(*Name);
			Param.DefaultValue = static_cast<float>(Value);
			Param.Id = FGuid::NewGuid();
			Collection->ScalarParameters.Add(Param);
			++WrittenCount;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* VectorsJson = nullptr;
	if (Payload->TryGetArrayField(TEXT("vectors"), VectorsJson) && VectorsJson)
	{
		for (const TSharedPtr<FJsonValue>& Entry : *VectorsJson)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(Obj) || !Obj)
			{
				continue;
			}

			FString Name;
			if (!(*Obj)->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
			{
				continue;
			}

			/*
			 * 解析走 `ParseMaterialValue`，**不要**用 `UAL_ParseLinearColor`。
			 *
			 * 两件事一起修：
			 *
			 * 一是返回值原来被丢了，而 `Value` 初始化成 White —— `value: 0.5`、
			 * `value: "red"` 这些 schema 放行、解析器拒绝的写法全都变成
			 * 「把这个参数设成纯白」外加一句「已更新」。
			 *
			 * 二是光接住返回值还不够：`UAL_ParseLinearColor` 对**任何** JSON 对象
			 * 都回 true，缺的 r/g/b 一律当 0。而本工具的 schema（MaterialValueSchema）
			 * 明明收 `{x,y,z}` 和 `{u_tiling,…}`，于是 `{x:1,y:0.5,z:0}` 落进来是
			 * 一个纯黑，照样报「已更新」—— 只把返回值接住的话这一条根本挡不下。
			 * `ParseMaterialValue` 是 set_param / set_node_value 共用的那份，
			 * xyz 和数组都认，别再留第三份各写各的实现。
			 */
			FUALMaterialValue Parsed;
			FString ParseError;
			if (!ParseMaterialValue((*Obj)->TryGetField(TEXT("value")), Parsed, ParseError)
				|| !Parsed.bHasColor)
			{
				Rejected.Add(FString::Printf(
					TEXT("vector '%s': value must be {r,g,b,a?}, {x,y,z?,w?}, a 2-4 element array "
						 "or a number (%s)"),
					*Name, ParseError.IsEmpty() ? TEXT("got a value with no color components") : *ParseError));
				continue;
			}
			const FLinearColor Value = Parsed.Color;

			const int32 Existing = UAL_FindVectorParam(Collection, FName(*Name));
			if (Existing != INDEX_NONE)
			{
				Collection->VectorParameters[Existing].DefaultValue = Value;
				++WrittenCount;
				continue;
			}

			if (Collection->VectorParameters.Num() >= 16)
			{
				Rejected.Add(FString::Printf(TEXT("vector '%s' (collection is full, 16 max)"), *Name));
				continue;
			}

			FCollectionVectorParameter Param;
			Param.ParameterName = FName(*Name);
			Param.DefaultValue = Value;
			Param.Id = FGuid::NewGuid();
			Collection->VectorParameters.Add(Param);
			++WrittenCount;
		}
	}

	/*
	 * 同 set_param：一条都没写进去的话，取消事务，而且**后面两句也不做**。
	 *
	 * `Cancel()` 不回滚，只是把事务从撤销栈上弹掉。取消完还照常
	 * `PostEditChange()` + `MarkPackageDirty()` 的话，留给用户的是一个
	 * 「有未保存改动」的星号加零改动，而且撤不掉；`UMaterialParameterCollection`
	 * 的 PostEditChange 还会把默认资源重新上传一遍，纯属白干。
	 */
	/*
	 * 新建的集合**一定要**走一次 PostEditChange，哪怕一个参数都没写。
	 *
	 * 刚 new 出来的 UObject 不会走 PostLoad，而 PostInitProperties 只分配
	 * DefaultResource；把它注册给渲染线程和各个 World 的只有 PostEditChange 这一条路。
	 * 所以条件是「写了东西 **或者** 这次是 create」，不能只看 WrittenCount。
	 */
	if (WrittenCount > 0 || Action == TEXT("create"))
	{
		Collection->PostEditChange();
		Collection->MarkPackageDirty();
	}
	// 一条都没写进去就取消事务 —— 和上面那两件事**分开判**。
	// 新建的集合必须走一次 PostEditChange（刚 new 出来的 UObject 不走 PostLoad，
	// 注册给渲染线程和各个 World 的只有这一条路），但那不等于撤销栈上该留一条记录：
	// 空集合的事务里只有一个快照，用户按 Ctrl+Z 会看到第一下什么也没发生
	if (WrittenCount == 0)
	{
		ParamTransaction.Cancel();
	}

	TSharedPtr<FJsonObject> Data = UAL_CollectionToJson(Collection);
	Data->SetBoolField(TEXT("created"), Action == TEXT("create"));
	if (Rejected.Num() > 0)
	{
		Data->SetStringField(TEXT("rejected"), FString::Join(Rejected, TEXT("; ")));
	}
	// 建出来只是第一步：不在材质里引用它，这个 MPC 什么也不影响
	Data->SetStringField(TEXT("note"),
		TEXT("Reference a parameter from a material with material.add_node node_type=\"CollectionParameter\", "
			"collection_path=<this path>, node_name=<parameter name>. Then material.compile."));

	UE_LOG(LogUALMaterial, Log, TEXT("Parameter collection %s: %d scalars, %d vectors"),
		*Collection->GetName(), Collection->ScalarParameters.Num(), Collection->VectorParameters.Num());
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// material.create_function / material.get_referencers
// ============================================================================

void FUAL_MaterialCommands::Handle_CreateMaterialFunction(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString FunctionName;
	if (!Payload->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: function_name"));
		return;
	}

	FString DestinationPath = TEXT("/Game/Materials");
	Payload->TryGetStringField(TEXT("destination_path"), DestinationPath);

	const FString PackagePath = DestinationPath / FunctionName;
	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create package for material function"));
		return;
	}

	FUAL_ScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "CreateMaterialFunction", "Create Material Function"));

	UMaterialFunctionFactoryNew* Factory = NewObject<UMaterialFunctionFactoryNew>();
	UMaterialFunction* Function = Cast<UMaterialFunction>(Factory->FactoryCreateNew(
		UMaterialFunction::StaticClass(),
		Package,
		FName(*FunctionName),
		RF_Public | RF_Standalone | RF_Transactional,
		nullptr,
		GWarn));

	if (!Function)
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create UMaterialFunction"));
		return;
	}

	FString Description;
	if (Payload->TryGetStringField(TEXT("description"), Description) && !Description.IsEmpty())
	{
		Function->Description = Description;
	}

	// 默认让它出现在材质编辑器的右键菜单里。建出来却在菜单里找不到，
	// 用户会以为没建成功
	Function->bExposeToLibrary = true;

	Function->PostEditChange();
	Function->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Function);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("function_path"), Function->GetPathName());
	Data->SetStringField(TEXT("function_name"), FunctionName);
	// 空函数什么也不做。这一步不说清楚，调用方会直接拿去 add_node 然后奇怪为什么没效果
	Data->SetStringField(TEXT("note"),
		TEXT("The function is empty. Add expressions to it the same way as a material, ending in a "
			 "FunctionOutput, then reference it with material.add_node node_type=\"MaterialFunctionCall\" "
			 "and function_path set to this path."));

	UE_LOG(LogUALMaterial, Log, TEXT("Created material function: %s"), *Function->GetPathName());
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_MaterialCommands::Handle_GetMaterialReferencers(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString AssetPath;
	if (!Payload->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: asset_path"));
		return;
	}
	AssetPath = NormalizePath(AssetPath);

	int32 Limit = 50;
	Payload->TryGetNumberField(TEXT("limit"), Limit);
	Limit = FMath::Clamp(Limit, 1, 500);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// 引用关系记在**包**上，不是对象上。`/Game/M_Wood.M_Wood` 这种带对象名的
	// 路径直接拿去查会一条都查不到，而且不报错 —— 看起来就是「没人用」
	FString PackageName = AssetPath;
	int32 DotIndex;
	if (PackageName.FindChar(TEXT('.'), DotIndex))
	{
		PackageName = PackageName.Left(DotIndex);
	}

	TArray<FName> Referencers;
	AssetRegistry.GetReferencers(FName(*PackageName), Referencers);

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FName& Referencer : Referencers)
	{
		if (Items.Num() >= Limit)
		{
			break;
		}

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("path"), Referencer.ToString());

		// 顺带把类型带上：调用方多半要区分「被材质实例引用」和「被关卡引用」，
		// 只给路径的话它还得逐个再查一次
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPackageName(Referencer, Assets);
		if (Assets.Num() > 0)
		{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			Obj->SetStringField(TEXT("class"), Assets[0].AssetClassPath.GetAssetName().ToString());
#else
			Obj->SetStringField(TEXT("class"), Assets[0].AssetClass.ToString());
#endif
		}
		Items.Add(MakeShared<FJsonValueObject>(Obj));
	}

	/**
	 * 未保存的包查不到 —— 这一条不说出来会害人。
	 *
	 * AssetRegistry 的引用关系是从**已落盘的包**里读的。刚在编辑器里建好、
	 * 或者刚改完还没存的材质，它的引用关系压根不在注册表里。
	 * 于是「这张贴图还有人用吗」会答「没有」，用户照着这个结论删掉，
	 * 下次打开工程就是一片丢失引用。
	 *
	 * 真机验证第一次跑就撞上了：M_VerifyGraph 正引用着 MPC，
	 * 反查 MPC 却回 0 个引用者 —— 因为两个都是刚建的，一个都没存。
	 *
	 * 引擎不提供「补上内存里的引用」这条路，所以只能如实报告：
	 * 有未保存的包时，这个答案**可能不全**。
	 */
	int32 DirtyCount = 0;
	TArray<FString> DirtySample;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		UPackage* Package = *It;
		if (!Package || !Package->IsDirty())
		{
			continue;
		}
		// 只数用户内容。Transient / 引擎包常年是脏的，算进去等于这条警告
		// 永远亮着，而永远亮着的警告没人看
		const FString Name = Package->GetName();
		if (!Name.StartsWith(TEXT("/Game/")))
		{
			continue;
		}
		++DirtyCount;
		if (DirtySample.Num() < 5)
		{
			DirtySample.Add(Name);
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetNumberField(TEXT("referencer_count"), Referencers.Num());
	Data->SetArrayField(TEXT("referencers"), Items);
	Data->SetBoolField(TEXT("truncated"), Referencers.Num() > Items.Num());
	Data->SetNumberField(TEXT("unsaved_package_count"), DirtyCount);

	if (DirtyCount > 0)
	{
		Data->SetStringField(TEXT("incomplete_reason"), FString::Printf(
			TEXT("%d unsaved package(s) exist (e.g. %s). References from unsaved packages are not in "
				 "the asset registry yet, so this list may be incomplete. Save first before relying on "
				 "it to delete anything."),
			DirtyCount,
			DirtySample.Num() ? *FString::Join(DirtySample, TEXT(", ")) : TEXT("?")));
	}

	UE_LOG(LogUALMaterial, Log, TEXT("%s has %d referencers (%d unsaved packages)"),
		*PackageName, Referencers.Num(), DirtyCount);
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// 节点编号 —— 全仓库唯一一份实现
// ============================================================================

/** 编号的**拼法**只此一份，下面两个函数都从这里拼，免得规则分家 */
static FString UAL_FormatExpressionId(UMaterialExpression* Expression, int32 Index)
{
	return FString::Printf(TEXT("%s_%d"), *Expression->GetClass()->GetName(), Index);
}

void FUAL_MaterialCommands::BuildExpressionIds(UMaterial* Material, TMap<UMaterialExpression*, FString>& OutIds)
{
	if (!Material)
	{
		return;
	}
	int32 Index = 0;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	for (UMaterialExpression* Expression : Material->GetExpressions())
#else
	for (UMaterialExpression* Expression : Material->Expressions)
#endif
	{
		if (!Expression) continue;
		OutIds.Add(Expression, UAL_FormatExpressionId(Expression, Index++));
	}
}

FString FUAL_MaterialCommands::ExpressionIdFor(UMaterial* Material, UMaterialExpression* Target)
{
	if (!Material || !Target)
	{
		return FString();
	}
	// 跳空槽、按数组序 —— 和 BuildExpressionIds 一字不差，差的只是不建表
	int32 Index = 0;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	for (UMaterialExpression* Expression : Material->GetExpressions())
#else
	for (UMaterialExpression* Expression : Material->Expressions)
#endif
	{
		if (!Expression) continue;
		if (Expression == Target)
		{
			return UAL_FormatExpressionId(Expression, Index);
		}
		++Index;
	}
	return FString();
}
