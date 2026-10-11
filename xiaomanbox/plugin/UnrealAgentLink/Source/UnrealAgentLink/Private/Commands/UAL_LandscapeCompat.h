#pragma once

#include "CoreMinimal.h"
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Runtime/Launch/Resources/Version.h"

/**
 * 地形 / 材质图 API 的跨版本适配层。**版本分支只允许出现在这个文件里。**
 *
 * 写法照 `UAL_MeshCompat.h`：业务代码只调意图，`#if` 收在这里。
 * 用到的都是 `Landscape` / `Engine` 模块里的东西，5.0–5.8 永远在，所以用 `#if`
 * 而不是反射 —— 引擎改了签名，我们发版前编译就会失败。
 *
 * 每个边界版本号都是在本机九个引擎上逐个 grep 头文件确认的，不是猜的。
 */
namespace UALLandscapeCompat
{
	/**
	 * 把高度数据灌进一个刚 Spawn 出来的地形 —— 就是编辑器「新建地形」按钮最后那一步。
	 *
	 * `ALandscapeProxy::Import` 的最后一个参数（编辑层）：
	 *   - 5.0–5.4：只有 `const TArray<FLandscapeLayer>* = nullptr` 这一个版本
	 *   - 5.5–5.6：两个版本并存，指针版标了 `UE_DEPRECATED(5.5)`
	 *   - 5.7–5.8：只剩 `const TArrayView<const FLandscapeLayer>&`，没有默认值
	 *
	 * 所以 5.5 起传一个空的 TArrayView（和编辑器自己的调用一模一样），之前省略。
	 */
	inline void Import(
		ALandscape* Landscape,
		int32 SizeX,
		int32 SizeY,
		int32 SectionsPerComponent,
		int32 QuadsPerSection,
		const TMap<FGuid, TArray<uint16>>& HeightDataPerLayer,
		const TCHAR* HeightmapFileName,
		const TMap<FGuid, TArray<FLandscapeImportLayerInfo>>& MaterialLayersPerLayer)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
		Landscape->Import(FGuid::NewGuid(), 0, 0, SizeX - 1, SizeY - 1, SectionsPerComponent, QuadsPerSection,
			HeightDataPerLayer, HeightmapFileName, MaterialLayersPerLayer, ELandscapeImportAlphamapType::Additive,
			TArrayView<const FLandscapeLayer>());
#else
		Landscape->Import(FGuid::NewGuid(), 0, 0, SizeX - 1, SizeY - 1, SectionsPerComponent, QuadsPerSection,
			HeightDataPerLayer, HeightmapFileName, MaterialLayersPerLayer, ELandscapeImportAlphamapType::Additive);
#endif
	}

	/**
	 * 打开编辑层（编辑器新建面板里 "Enable Edit Layers" 默认勾着）。
	 *
	 * `ALandscape::bCanHaveLayersContent`：5.0–5.6 是普通成员；5.7 起改名
	 * `bCanHaveLayersContent_DEPRECATED` —— 编辑层从此恒开，不再有这个开关。
	 */
	inline void EnableEditLayers(ALandscape* Landscape)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
		(void)Landscape;
#else
		Landscape->bCanHaveLayersContent = true;
#endif
	}

	/**
	 * 让一块流送代理从主地形继承共享属性（RVT 列表在其中）。
	 *
	 * `ALandscapeProxy::SynchronizeSharedProperties` 5.3 起才有（5.2 ✗、5.3 ✓ … 5.8 ✓，
	 * 5.8 多一个带默认值的参数）。同一版本起这类属性可以被代理「覆盖」，直接改代理
	 * 的值会让它和主地形分叉 —— 所以能继承就继承。更老的版本返回 false，调用方直接写。
	 */
	inline bool InheritSharedProperties(ALandscapeProxy* Proxy, ALandscape* Parent)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3)
		Proxy->Modify();
		Proxy->SynchronizeSharedProperties(Parent);
		Proxy->MarkComponentsRenderStateDirty();
		return true;
#else
		(void)Proxy;
		(void)Parent;
		return false;
#endif
	}

	/**
	 * `RuntimeVirtualTexture::SetBounds` 在 World Partition 下只量已加载的格子。
	 *
	 * 5.5 起它对地形特判、改用 `GetCompleteBounds()` 把没加载的也算上
	 * （5.4 ✗、5.5 ✓ … 5.8 ✓）。体积没罩住时，回执据此给出不同的补救办法。
	 */
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
	constexpr bool bSetBoundsSkipsUnloadedCells = false;
#else
	constexpr bool bSetBoundsSkipsUnloadedCells = true;
#endif

	/**
	 * 材质图顶层的节点。
	 *
	 * `UMaterial::GetExpressions()` 5.1 起才有；5.0 是公开成员 `Expressions`。
	 * 与 `UAL_MaterialCommands.cpp` 里的分支同一条边界。
	 */
	inline TArray<UMaterialExpression*> GetExpressions(const UMaterial* Material)
	{
		TArray<UMaterialExpression*> Result;
		if (!Material)
		{
			return Result;
		}
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		for (UMaterialExpression* Expression : Material->GetExpressions())
#else
		for (UMaterialExpression* Expression : Material->Expressions)
#endif
		{
			if (Expression)
			{
				Result.Add(Expression);
			}
		}
		return Result;
	}

	/**
	 * 材质函数里的节点。
	 *
	 * 5.0：接口上的虚函数 `GetFunctionExpressions()`，返回指针（可能为空）。
	 * 5.1 起：`GetExpressions()`（ENGINE_API），旧名字标了 `UE_DEPRECATED(5.1)`。
	 */
	inline TArray<UMaterialExpression*> GetExpressions(const UMaterialFunctionInterface* Function)
	{
		TArray<UMaterialExpression*> Result;
		if (!Function)
		{
			return Result;
		}
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		for (UMaterialExpression* Expression : Function->GetExpressions())
		{
			if (Expression)
			{
				Result.Add(Expression);
			}
		}
#else
		if (const TArray<TObjectPtr<UMaterialExpression>>* Expressions = Function->GetFunctionExpressions())
		{
			for (UMaterialExpression* Expression : *Expressions)
			{
				if (Expression)
				{
					Result.Add(Expression);
				}
			}
		}
#endif
		return Result;
	}
}
