#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 静态 / 骨骼网格命令。
 *
 * 设计与裁决。一句话版本：**这块走 C++ 不走 Python**，
 * 理由是跨九个引擎版本的正确性需要编译器来保证，而不是靠运行时 getattr 探测
 * ——— Python 那条路上 API 变了只会在用户机器上炸，C++ 在我们发版前就炸。
 *
 * ## 已实现
 *
 *   - `mesh.describe` —— 一次读完一个网格资产的全部信息
 *
 * ## 为什么只有一个命令，而 Epic 有 38 个
 *
 * Epic 的 38 个里 24 个是纯 getter（`get_lod_count`、`get_triangle_count`、
 * `get_bounds`……）。按那个粒度，模型问一句「这模型能不能优化」要连着调七八次。
 * 我们把读取合成一个 `mesh.describe`，一次答完。
 *
 * 后续（见设计文档 §7）：`mesh.audit`（批量体检，走 AssetRegistry 标签，
 * 不加载资产）、`mesh.optimize`、`mesh.sockets`。
 *
 * ## 版本兼容策略
 *
 * 本文件里用到的访问器**全部在引擎源码里逐个确认过**（比对 UE 5.6 头文件），
 * 且都带 `UE_DEPRECATED(4.27, "…use GetXxx()…")` 标记 —— 说明这套 Get/Set
 * 访问器 4.27 就已存在，覆盖我们支持的 5.0–5.8 全区间。
 *
 * 需要编辑器子系统的写操作（生成 LOD、生成碰撞）不在这个文件里。那些 API
 * 在 5.6 之前不存在，届时走 `UAL_ReflectCall`（反射，不产生链接期依赖），
 * 不要往 Build.cs 里加 `StaticMeshEditor` —— 那会让插件在老引擎上整个加载不起来。
 */
class FUAL_MeshCommands
{
public:
	/** 注册所有网格相关命令到 CommandMap */
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	/** mesh.describe —— 读取一个静态/骨骼网格资产的完整信息 */
	static void Handle_Describe(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
};
