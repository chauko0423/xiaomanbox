#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 地形（Landscape）与地形 RVT（运行时虚拟纹理）命令。
 *
 * ## 已实现
 *
 *   - `landscape.create`     —— 新建一块地形（平地 / 从高度图），可顺带配好 RVT
 *   - `landscape.setup_rvt`  —— 给已有地形配颜色 RVT 和/或高度 RVT
 *   - `landscape.list`       —— 列出关卡里的地形，连同 RVT 体检的原始事实
 *
 * ## 为什么走 C++
 *
 * UE 的 Python 没有「新建地形」的入口：`ALandscapeProxy::Import` 只有 C++ 能调，
 * 用 Python 生成一个 Landscape Actor 只会得到一个没有地形块的空壳。
 *
 * 流程照抄编辑器自己的「新建地形」按钮
 * （`LandscapeEditorDetailCustomization_NewLandscape.cpp` 的 `OnCreateButtonClicked`）
 * 和地形细节面板的「创建 RVT 体积」按钮（`LandscapeProxyUIDetails.cpp`）。
 * 这两段在 5.0–5.8 里结构一致，用到的接口逐个版本核对过 —— 差异只有一处
 * `Import` 的签名，收在 `UAL_LandscapeCompat.h`。
 *
 * ## 依赖
 *
 * `Landscape`、`LandscapeEditor`、`VirtualTexturingEditor` 都是引擎本体模块，
 * 不是可选插件，编辑器构建里永远在 —— 所以直接链接，不像 PCG 那样走反射。
 *
 * ## 第一版不做
 *
 * 超大地形的「分区域建」（World Partition 下超过区域尺寸时编辑器会先存盘、
 * 且不可撤销）、笔刷雕刻、刷材质图层、构建 RVT 流送 mip、改已有地形的高度。
 */
class FUAL_LandscapeCommands
{
public:
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	static void Handle_Create(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	static void Handle_SetupRvt(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	static void Handle_List(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
};
