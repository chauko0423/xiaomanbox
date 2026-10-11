#pragma once

#include "CoreMinimal.h"
#include "IImageWrapper.h"
#include "AssetRegistry/AssetData.h"
// GetUsedTextures 的兜底重载要在**模板定义处**就能看见 EMaterialQualityLevel 和
// GMaxRHIFeatureLevel —— 它们不依赖模板参数，不会推迟到实例化时才查找。
#include "SceneTypes.h"
#include "RHI.h"

class UStaticMesh;
class UMaterialExpression;
class UMovieSceneSequence;
class UMovieSceneTrack;
class UTexture;

/**
 * 版本兼容适配层，集中处理 5.0 - 5.7 API 差异
 */
namespace UALCompat
{
	/**
	 * 解析一个绑定当前指向哪些对象。空 = 绑定断了。
	 *
	 * ## 这条是本插件里最危险的一处版本差异
	 *
	 * `UMovieSceneSequence::LocateBoundObjects(FGuid, UObject* Context, TArray&)`
	 * 这个重载：
	 *
	 *   - 4.27 / 5.0 – 5.3：`PURE_VIRTUAL`，是**真正的实现**
	 *   - **5.4 起**：函数体是 `{}`，**空实现**。真家伙换成了收
	 *     `UE::UniversalObjectLocator::FResolveParams` 的那个重载
	 *
	 * 也就是说在 5.4+ 上照老写法调用，**编得过、跑得动、永远返回空** ——
	 * 于是「解析不到对象」的判定全线成立，每一个绑定都会被报成已失效。
	 * 用户会拿着一份「全部绑定都坏了」的体检报告去修一堆没坏的东西。
	 *
	 * 编译器不会拦这个（签名还在），是逐版本比对头文件才看见的。
	 *
	 * 逐版本实测（九个引擎都在本机）：
	 *
	 * | 版本 | 老重载 | UOL 头 |
	 * |---|---|---|
	 * | 5.0 / 5.2 | `PURE_VIRTUAL`（真实现） | 无 |
	 * | 5.4 / 5.5 | `{}`（空） | 有 |
	 * | 5.6 / 5.7 | `{}`（空） | 有 |
	 *
	 * 分支用 `__has_include` 而不是版本号：UOL 头出现的版本和老重载变空的版本
	 * **正好是同一个 5.4**，所以这一个门就够了，且它问的是「这引擎上有没有这个头」
	 * 这种事实，不依赖版本号是否被工作室改过。
	 *
	 * @param Sequence 序列
	 * @param ObjectId 绑定 GUID
	 * @param Context  解析上下文，通常是编辑器世界
	 * @param OutObjects 解析到的对象
	 * @return 解析路径是否可用。false = 这台引擎上判不出来，**调用方必须把结果
	 *         归入 unresolved 而不是 broken**。报一个假的「已失效」比不报更糟。
	 */
	bool LocateBoundObjects(
		const UMovieSceneSequence* Sequence,
		const FGuid& ObjectId,
		UObject* Context,
		TArray<UObject*, TInlineAllocator<1>>& OutObjects);

	/**
	 * 序列的顶层轨道（相机切轨、子序列轨道这些不挂在任何绑定下的轨道）。
	 *
	 * `UMovieScene::GetMasterTracks()` 在 5.x 改名成了 `GetTracks()`。
	 * **这一条和 Python 那边的 `get_master_tracks` → `get_tracks` 是同一次改名，
	 * 但它确实发生在引擎核心里，不只是绑定层。**
	 *
	 * 九个引擎逐个实测：
	 *
	 * | 版本 | `GetMasterTracks` | `GetTracks` |
	 * |---|:--:|:--:|
	 * | 5.0 / 5.1 | ✅ | ❌ |
	 * | **5.2** | ✅ | ✅ |（过渡期，两个都在）
	 * | 5.5 – 5.8 | ❌ | ✅ |
	 *
	 * 5.2 两个都有，所以「优先 `GetTracks`」的重载顺序在那一版也落在新名字上，
	 * 九个版本行为一致。
	 *
	 * 用重载决议做编译期能力探测，而不是 `#if ENGINE_MINOR_VERSION`：
	 * 「这个方法在不在」是编译器能直接回答的问题，比猜版本号可靠 ——
	 * 工作室常年跑自定义引擎分支，版本号靠不住，方法在不在永远是准的。
	 *
	 * 这是 Python 侧 `getattr(ext, "get_tracks", None) or getattr(...)` 的 C++ 对应物，
	 * 区别是它在编译期就定下来了，选错了编不过而不是运行时才炸。
	 */
	namespace Private
	{
		// int 重载优先：有 GetTracks 就用它
		template <typename MovieSceneType>
		auto RootTracks(const MovieSceneType* MovieScene, int)
			-> decltype(MovieScene->GetTracks())
		{
			return MovieScene->GetTracks();
		}

		// long 重载兜底：只有 GetMasterTracks 的老版本走这里
		template <typename MovieSceneType>
		auto RootTracks(const MovieSceneType* MovieScene, long)
			-> decltype(MovieScene->GetMasterTracks())
		{
			return MovieScene->GetMasterTracks();
		}
	}

	template <typename MovieSceneType>
	const TArray<UMovieSceneTrack*>& GetRootTracks(const MovieSceneType* MovieScene)
	{
		return Private::RootTracks(MovieScene, 0);
	}

	/**
	 * 一个材质用到了哪些贴图。
	 *
	 * ## 又一个「编得过、跑得动、永远返回空」
	 *
	 * `UMaterialInterface::GetUsedTextures(TArray&, EMaterialQualityLevel::Type, bool,
	 * ERHIFeatureLevel::Type, bool)` 这个五参重载：
	 *
	 *   - 5.0 – 5.6：真实现
	 *   - **5.7 起**：声明成 `final {}`，**空实现**。真家伙换成了收
	 *     `TOptional<EMaterialQualityLevel::Type>` / `TOptional<EShaderPlatform>` 的新重载
	 *
	 * 和 Sequencer 那条 `LocateBoundObjects` 是同一个形态的坑：签名还在、编译器不吭声，
	 * 只是回来的数组永远是空的。后果是「这个网格用了多少贴图」在 5.7/5.8 上恒等于 0 ——
	 * 用户会拿到一份「贴图零开销」的体检报告。
	 *
	 * 分支同样用重载决议而不是版本号：新重载的三个参数都有默认值，所以
	 * `GetUsedTextures(Out)` 这个单参调用**只在 5.7+ 上成立**；5.0 – 5.6 的五参版本
	 * 缺参数编不过，自动落到兜底重载。问的是「这引擎上有没有这个调用形态」，
	 * 不依赖版本号是否被工作室改过。
	 */
	namespace Private
	{
		// int 重载优先：5.7+ 的新形态（单参可调用）
		template <typename MaterialType>
		auto UsedTextures(const MaterialType* Material, TArray<UTexture*>& OutTextures, int)
			-> decltype(Material->GetUsedTextures(OutTextures))
		{
			return Material->GetUsedTextures(OutTextures);
		}

		// long 重载兜底：5.0 – 5.6 的五参形态
		template <typename MaterialType>
		auto UsedTextures(const MaterialType* Material, TArray<UTexture*>& OutTextures, long)
			-> decltype(Material->GetUsedTextures(OutTextures, EMaterialQualityLevel::Num, true, GMaxRHIFeatureLevel, true))
		{
			return Material->GetUsedTextures(OutTextures, EMaterialQualityLevel::Num, true, GMaxRHIFeatureLevel, true);
		}
	}

	template <typename MaterialType>
	void GetUsedTextures(const MaterialType* Material, TArray<UTexture*>& OutTextures)
	{
		Private::UsedTextures(Material, OutTextures, 0);
	}

	/**
	 * PNG 压缩兼容：5.1+ 支持双参 GetCompressed，5.0 仅有返回引用的单参版本。
	 */
	bool GetCompressedPNG(const TSharedPtr<IImageWrapper>& Wrapper, int32 Quality, TArray<uint8>& OutData);

	/**
	 * 静态网格有没有开 Nanite。
	 *
	 * `UStaticMesh::IsNaniteEnabled()` 是 5.1 才加的；5.0 上要自己读
	 * NaniteSettings。5.0 没有 Nanite 的这个访问器不代表没有 Nanite。
	 */
	bool IsNaniteEnabled(const UStaticMesh* Mesh);

	/**
	 * 资产的对象路径字符串。
	 *
	 * 5.1 起 `FAssetData::ObjectPath`（FName）被 `GetObjectPathString()` 取代，
	 * 5.0 上只有前者。两边返回的字符串是一样的。
	 */
	FString GetObjectPathString(const FAssetData& AssetData);

	/**
	 * 材质表达式有几个输入引脚。
	 *
	 * `UMaterialExpression::CountInputs()` 是 5.1 才公开的；5.0 上遍历
	 * `GetInput(i)` 直到拿到空指针。
	 */
	int32 CountInputs(UMaterialExpression* Expression);
}

