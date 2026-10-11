#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "Runtime/Launch/Resources/Version.h"

/**
 * `FSkeletalMeshLODInfo` / `FSkeletalMaterial` 的定义位置。
 *
 * 5.0：住在 `Engine/SkeletalMesh.h` 里（调用方已经包了那个）。
 * 5.1 起：拆到 `Engine/SkinnedAssetCommon.h`，而 `SkinnedAssetCommon.h`
 *         **5.0 上根本不存在**，无条件包会在 5.0 上编译失败。
 *
 * 为什么只有 5.4 需要这条：5.1/5.2/5.3/5.5–5.8 都能靠别的头文件传递包进来，
 * **只有 5.4 的传递链断了**，报 `error C2027: 使用了未定义类型 FSkeletalMeshLODInfo`。
 * 与其去猜哪个版本的传递链靠得住，不如显式包 —— 传递包含本来就不该依赖。
 */
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
#include "Engine/SkinnedAssetCommon.h"
#endif

/**
 * 网格 API 的跨版本适配层。**版本分支只允许出现在这个文件里。**
 *
 * ## 为什么要有这一层
 *
 * 我们支持 UE 5.0–5.8 九个版本。同一件事在不同版本上叫不同的名字，甚至根本
 * 不存在。如果让 `#if ENGINE_MINOR_VERSION` 散进业务代码，九个版本的分支会
 * 互相缠绕，改一处要在脑子里同时跑九遍。
 *
 * 这里把「意图」包成函数（`IsNaniteEnabled(Mesh)`），业务代码只调意图，
 * 读起来和没有版本分叉一样。
 *
 * ## 为什么用 `#if` 而不是运行时探测
 *
 * 这是选 C++ 而不是 Python 的全部理由：
 * 编译期分支意味着**引擎改了 API，我们发版前就会编译失败**，而不是等用户
 * 的编辑器抛异常。九个版本各编一次，等于九次静态检查。
 *
 * 例外：需要**可选插件**里的类型时不能用 `#if` —— 那会产生链接期依赖，
 * 让插件在没装该插件的工程里整个加载不起来。那种情况走 `UAL_ReflectCall`。
 * 本文件处理的都是 `Engine` 模块里的东西，永远在，所以 `#if` 是对的选择。
 *
 * ## 每加一条都要在九个版本上核对过
 *
 * 下面每个条件的边界版本号都是在本机逐个引擎 grep 头文件确认的，不是猜的。
 * 加新条目时照做 —— 猜错了在 5.2 上编译失败，那是最贵的一种返工。
 */
namespace UALMeshCompat
{
	/**
	 * 静态网格是否启用 Nanite。
	 *
	 * 实测（grep 各版本 `Engine/Classes/Engine/StaticMesh.h`）：
	 *   - `UStaticMesh::IsNaniteEnabled()`  5.0 ✗  5.1 ✗  5.2 ✗  5.3 ✓ … 5.8 ✓
	 *   - `UStaticMesh::NaniteSettings`     5.0–5.8 都是公开成员，
	 *                                       但 5.7 起标了 `UE_DEPRECATED`（将转私有）
	 *
	 * 所以 5.3+ 走访问器，5.0–5.2 直读成员 —— 那三个版本上它还没被弃用。
	 */
	inline bool IsNaniteEnabled(const UStaticMesh* Mesh)
	{
		if (!Mesh)
		{
			return false;
		}

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3)
		return Mesh->IsNaniteEnabled();
#else
		return Mesh->NaniteSettings.bEnabled != 0;
#endif
	}
}
