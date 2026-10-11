#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * `actor.inspect_components` —— 组件级只读回读：渲染状态（材质槽、覆层、描边 Stencil、
 * 显隐）+ 任意点路径属性，对象可以是场景里的 Actor，也可以是蓝图的 CDO 与组件模板。
 */
class FUAL_ComponentInspectCommands
{
public:
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& Commands);
};
