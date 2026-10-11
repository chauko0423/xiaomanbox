#include "UAL_VersionCompat.h"

#include "Modules/ModuleManager.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialExpression.h"
#include "MovieSceneSequence.h"

// UOL（Universal Object Locator）在的话，绑定解析要走它。见头文件里的长注释。
#if __has_include("UniversalObjectLocatorResolveParams.h")
	#include "UniversalObjectLocatorResolveParams.h"
	#define UAL_HAS_UOL_RESOLVE 1
#else
	#define UAL_HAS_UOL_RESOLVE 0
#endif

namespace UALCompat
{
	bool LocateBoundObjects(
		const UMovieSceneSequence* Sequence,
		const FGuid& ObjectId,
		UObject* Context,
		TArray<UObject*, TInlineAllocator<1>>& OutObjects)
	{
		OutObjects.Reset();
		if (!Sequence || !ObjectId.IsValid())
		{
			return false;
		}

		// 没有上下文不是「解析到 0 个对象」，是「这次根本没查」。
		//
		// 这两件事必须分开：前者是「绑定坏了，去修」，后者是「我不知道」。
		// 混为一谈的后果是用户拿着一份假的失效报告去修没坏的东西。
		//
		// 原来只有 5.3- 那条分支做了这个检查，UOL 分支漏了 —— 于是在 5.4+ 上
		// 「拿不到编辑器世界」会被静默报成「所有绑定都失效」，
		// 而 capabilities.binding_resolution 还照样回 true。
		if (!Context)
		{
			return false;
		}

#if UAL_HAS_UOL_RESOLVE
		// 5.4+ 的真实路径。旧的 (FGuid, UObject*, TArray&) 重载在这些版本上
		// 是个空函数体，调了等于没调 —— 每个绑定都会被判成失效。
		//
		// 注：5.5 上编译会报 C4996，说应该用再多收一个 SharedPlaybackState 的重载。
		// 暂时不跟，理由是那个重载要求先构造一份播放状态，对「只想知道这个绑定
		// 还指不指得到东西」这件事太重。**这是一条会到期的债** ——
		// 弃用说明写的是「下个版本就编不过」，等真编不过时（届时编译器会拦住我们，
		// 不会静默）再决定是接受那份开销，还是另找路径。
		// ── 解析标志必须带 Load，不能用 None ──────────────────────────────
		//
		// 引擎对这个标志的说明是：
		//   Load —— "whether the object should be loaded if it is not currently findable"
		//
		// 用 None 的意思是「只认此刻就找得到的对象」。对体检来说这是错的判据：
		// **World Partition 里没加载的格子中的 Actor 会被判成「绑定已失效」**，
		// 而它其实好好的，只是没加载。假的「已失效」会让用户去修没坏的东西。
		//
		// 代价是可能触发加载没加载的格子，大地图上体检会变慢。这个代价接受 ——
		// 慢一点的正确答案，好过快的错答案。
		//
		// 注：这不是某次实测暴露的 —— 是读 UOL 的标志定义时发现的。
		// WP 大地图上的验证还没做， 的待验证清单。
		using namespace UE::UniversalObjectLocator;
		FResolveParams ResolveParams(Context, ELocatorResolveFlags::Load);
		Sequence->LocateBoundObjects(ObjectId, ResolveParams, OutObjects);
		return true;
#else
		// 5.4 及更早：(FGuid, UObject*, TArray&) 才是真实现
		Sequence->LocateBoundObjects(ObjectId, Context, OutObjects);
		return true;
#endif
	}

	/**
	 * 获取压缩后的图像数据
	 * 兼容 UE 5.0-5.7：统一使用 GetCompressed(Quality) 返回值
	 */
	bool GetCompressedPNG(const TSharedPtr<IImageWrapper>& Wrapper, int32 Quality, TArray<uint8>& OutData)
	{
		if (!Wrapper.IsValid())
		{
			return false;
		}

		// 所有 UE5 版本都使用 GetCompressed(Quality) 返回 TArray64<uint8>
		const TArray64<uint8>& CompressedRef = Wrapper->GetCompressed(Quality);
		OutData.Reset(CompressedRef.Num());
		OutData.Append(CompressedRef.GetData(), CompressedRef.Num());
		return OutData.Num() > 0;
	}

	bool IsNaniteEnabled(const UStaticMesh* Mesh)
	{
		if (!Mesh)
		{
			return false;
		}
// UStaticMesh::IsNaniteEnabled() 是 **5.3** 才有的。
		// 逐版本查过引擎头文件确认，不要凭印象改这个数字。
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3)
		return Mesh->IsNaniteEnabled();
#else
		// 5.0–5.2 没有访问器，直接读设置结构
		return Mesh->NaniteSettings.bEnabled;
#endif
	}

	FString GetObjectPathString(const FAssetData& AssetData)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		return AssetData.GetObjectPathString();
#else
		return AssetData.ObjectPath.ToString();
#endif
	}

	int32 CountInputs(UMaterialExpression* Expression)
	{
		if (!Expression)
		{
			return 0;
		}
// UMaterialExpression::CountInputs() 是 **5.5** 才公开的。
		// 逐版本查过引擎头文件确认，不要凭印象改这个数字。
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
		return Expression->CountInputs();
#else
		// 5.0–5.4 上没有这个方法，遍历到第一个空输入为止。
		// 加个上限兜底：GetInput 的实现万一不返回 nullptr 就会转成死循环。
		constexpr int32 MaxInputs = 64;
		int32 Count = 0;
		while (Count < MaxInputs && Expression->GetInput(Count) != nullptr)
		{
			++Count;
		}
		return Count;
#endif
	}
}
