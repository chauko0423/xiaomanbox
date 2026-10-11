#include "UAL_SequencerCommands.h"

#include "UAL_CameraCutCoverage.h"
#include "UAL_CommandUtils.h"
#include "UAL_ScopedTransaction.h"
#include "UAL_VersionCompat.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneTrack.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneIntegerChannel.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneSubSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneSpawnTrack.h"
#include "Tracks/MovieSceneSubTrack.h"

#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "FileHelpers.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/EngineVersion.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALSequencer, Log, All);

namespace
{
	// ========================================================================
	// 帧刻度转换
	// ========================================================================

	/**
	 * MovieScene 内部帧号 → 用户看到的帧号。
	 *
	 * 内部一切时间存在 tick resolution（通常 24000/1），对外说的帧是 display rate
	 * （通常 24/1 或 30/1）。**差三个数量级，弄反了不报错，只是数字大一千倍。**
	 */
	int32 TickToDisplay(const UMovieScene* MovieScene, const FFrameNumber& Tick)
	{
		if (!MovieScene)
		{
			return Tick.Value;
		}
		const FFrameRate TickRes = MovieScene->GetTickResolution();
		const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
		return FFrameRate::TransformTime(FFrameTime(Tick), TickRes, DisplayRate).FloorToFrame().Value;
	}

	/** 反向：用户帧号 → 内部帧号。写入路径用 */
	FFrameNumber DisplayToTick(const UMovieScene* MovieScene, int32 DisplayFrame)
	{
		if (!MovieScene)
		{
			return FFrameNumber(DisplayFrame);
		}
		const FFrameRate TickRes = MovieScene->GetTickResolution();
		const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
		return FFrameRate::TransformTime(FFrameTime(FFrameNumber(DisplayFrame)), DisplayRate, TickRes).FloorToFrame();
	}

	// ========================================================================
	// 载入与命名
	// ========================================================================

	ULevelSequence* LoadSequence(const FString& Path, FString& OutError)
	{
		if (Path.IsEmpty())
		{
			OutError = TEXT("sequence_path 是空的");
			return nullptr;
		}

		UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
		if (!Asset)
		{
			OutError = FString::Printf(TEXT("找不到资产：%s"), *Path);
			return nullptr;
		}

		ULevelSequence* Sequence = Cast<ULevelSequence>(Asset);
		if (!Sequence)
		{
			OutError = FString::Printf(
				TEXT("%s 不是 Level Sequence，实际是 %s"), *Path, *Asset->GetClass()->GetName());
			return nullptr;
		}

		if (!Sequence->GetMovieScene())
		{
			OutError = FString::Printf(TEXT("%s 没有 MovieScene（资产可能损坏）"), *Path);
			return nullptr;
		}

		return Sequence;
	}

	/**
	 * 绑定类型。
	 *
	 * 名字带 UAL 前缀不是洁癖：UMG 里有一个同名的 `EBindingKind`
	 * （`Blueprint/WidgetBlueprintGeneratedClass.h`），而本模块依赖 UMG。
	 * unity build 会把多个 .cpp 拼成一个翻译单元，匿名 namespace 挡不住这种撞名 ——
	 * 编译器报 C2872「不明确的符号」，五个位置一起炸。
	 */
	enum class EUALBindingKind : uint8
	{
		Spawnable,
		Possessable,
		Unknown
	};

	/**
	 * 按 GUID 查绑定的显示名和类型。
	 *
	 * **不要用 `FMovieSceneBinding::GetName()`** —— 它在 5.7 被弃用，
	 * 返回的是 `BindingName_DEPRECATED`，编得过、跑得动、给你一个空串。
	 * `FindPossessable` / `FindSpawnable` 在 4.27–5.8 全程非弃用，
	 * 而且一次查询同时给出名字和类型，比 Python 那边先建一个 spawnable id 集合
	 * 再比对要直接。
	 */
	FString ResolveBindingName(UMovieScene* MovieScene, const FGuid& Guid, EUALBindingKind& OutKind)
	{
		OutKind = EUALBindingKind::Unknown;
		if (!MovieScene)
		{
			return FString();
		}

		if (const FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(Guid))
		{
			OutKind = EUALBindingKind::Spawnable;
			return Spawnable->GetName();
		}
		if (const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Guid))
		{
			OutKind = EUALBindingKind::Possessable;
			return Possessable->GetName();
		}
		return FString();
	}

	/**
	 * 这条绑定身上有没有生成轨道（`UMovieSceneSpawnTrack`）。
	 *
	 * 有生成轨道 = 这条绑定的对象由序列自己生成，不是关卡里既有的 Actor。
	 * 这是**与引擎版本无关**的事实判据，下面那条升级规则就建立在它上面。
	 */
	bool BindingHasSpawnTrack(const FMovieSceneBinding& Binding)
	{
		for (const UMovieSceneTrack* Track : Binding.GetTracks())
		{
			if (Cast<const UMovieSceneSpawnTrack>(Track))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * 绑定类型，带「生成轨道兜底」。
	 *
	 * `FindSpawnable` 只认 `MovieScene->Spawnables` 数组里那种老式 spawnable。
	 * 5.5 起引擎把生成体挪到了 custom binding（记在 possessable 那一侧），
	 * 于是同一个生成体在这里被判成 possessable —— **编得过、跑得动、答案是错的**。
	 *
	 * 后果是连着两条，而且都朝「让用户去修没坏的东西」的方向错：
	 *
	 *   1. `sequence_describe` 把生成体标成 `possessable`；
	 *   2. 判成 possessable 就会去编辑器世界里找对象 —— 生成体在编辑期
	 *      本来就不在关卡里，于是被报成「❌ 绑定已失效」，而它在 PIE 里工作正常。
	 *
	 * 2026-09-21 的用户反馈里 `DefeatCard` 就是这么被误报的。
	 *
	 * 不按版本号分支：`FindSpawnable` 认出来的算 spawnable，认不出来但挂着
	 * 生成轨道的也算 —— 这条对 5.0 到 5.8 一致成立，工作室改过的引擎分支也一样。
	 */
	FString ResolveBindingNameForBinding(
		UMovieScene* MovieScene,
		const FMovieSceneBinding& Binding,
		EUALBindingKind& OutKind)
	{
		const FString Name = ResolveBindingName(MovieScene, Binding.GetObjectGuid(), OutKind);
		if (OutKind != EUALBindingKind::Spawnable && BindingHasSpawnTrack(Binding))
		{
			OutKind = EUALBindingKind::Spawnable;
		}
		return Name;
	}

	const TCHAR* BindingKindToString(EUALBindingKind Kind)
	{
		switch (Kind)
		{
		case EUALBindingKind::Spawnable:   return TEXT("spawnable");
		case EUALBindingKind::Possessable: return TEXT("possessable");
		default:                        return TEXT("unknown");
		}
	}

	/**
	 * 解析绑定时用的世界。**必须是编辑器世界，不能退回 `GWorld`。**
	 *
	 * `UAL_CommandUtils::GetTargetWorld()` 拿不到编辑器世界时会退回 `GWorld` ——
	 * 那在 PIE 期间是 **PIE 世界**。拿 PIE 世界去解析编辑器序列里的绑定，
	 * 对不上是必然的，而结果会被报成「绑定已失效」。
	 *
	 * 这里宁可返回 nullptr —— 调用方会把它归入 unresolved（「这次没查」），
	 * 而不是 broken（「坏了，去修」）。**说不知道，好过说一个错的答案。**
	 */
	UWorld* GetSequencerResolveWorld()
	{
#if WITH_EDITOR
		if (GEditor)
		{
			return GEditor->GetEditorWorldContext().World();
		}
#endif
		return nullptr;
	}

	/**
	 * 解析一条绑定当前指向哪些对象，**带父级上下文**。
	 *
	 * ## 为什么不能一律拿 World 当上下文
	 *
	 * Sequencer 里给灯光调强度，会产生两条绑定：一条是 Actor（`DirectionalLight`），
	 * 一条是它的组件（`LightComponent0`），后者的 possessable 上带着指向前者的
	 * `ParentGuid`。**组件绑定是相对父对象解析的，不是相对世界。**
	 *
	 * 引擎自己把这条写在 `MovieSceneSequence.h` 的注释里：
	 *
	 * > if GetParentObject returns nullptr, the PlaybackContext will be used for
	 * > LocateBoundObjects, otherwise the object's parent will be used
	 *
	 * 一律传 World 的后果是**组件绑定永远解析不到** —— 于是每一条被调过属性的
	 * 灯光、相机、网格组件都会被报成「已失效」。这是最坏的一类错：
	 * 用户会去修一堆没坏的东西。首次真机测试就撞上了（SQ_Empty 里的
	 * LightComponent0 被误报），这段注释是那次的产物。
	 *
	 * @return 解析路径是否可用。false = 这台引擎上判不出来，
	 *         调用方必须归入 unresolved 而不是 broken。
	 */
	bool ResolveBindingObjects(
		ULevelSequence* Sequence, UMovieScene* MovieScene, const FGuid& Guid, UWorld* World,
		TArray<UObject*, TInlineAllocator<1>>& OutObjects, int32 Depth = 0)
	{
		OutObjects.Reset();
		if (!Sequence || !MovieScene)
		{
			return false;
		}

		// 组件层级不会有这么深；有环就是资产坏了，别在这里转到死
		if (Depth > 8)
		{
			return false;
		}

		UObject* Context = World;

		if (const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Guid))
		{
			const FGuid ParentGuid = Possessable->GetParent();
			if (ParentGuid.IsValid())
			{
				TArray<UObject*, TInlineAllocator<1>> ParentObjects;
				if (!ResolveBindingObjects(Sequence, MovieScene, ParentGuid, World, ParentObjects, Depth + 1))
				{
					return false;
				}
				if (ParentObjects.Num() == 0)
				{
					// 父级本身就解析不到 —— 子绑定确实也是断的，如实返回空。
					// 父那条会被单独报出来，用户从父级入手修才对。
					return true;
				}
				Context = ParentObjects[0];
			}
		}

		return UALCompat::LocateBoundObjects(Sequence, Guid, Context, OutObjects);
	}

	// ========================================================================
	// 段与通道
	// ========================================================================

	/**
	 * 段的时间范围，一律按**闭开区间** `[start, end)` 上报。
	 *
	 * UE 内部这件事不一致（MRQ 渲染不含末帧、AnimSequence 首尾都含、
	 * 播放头落在交界处显示下一段），是穿帮和错帧的常见来源。
	 * 算术归工具，不让模型自己推。。
	 *
	 * 无界的那一头返回 false，JSON 里写 null。
	 */
	void SectionRange(
		const UMovieSceneSection* Section, const UMovieScene* MovieScene,
		bool& bOutHasStart, int32& OutStart, bool& bOutHasEnd, int32& OutEnd)
	{
		bOutHasStart = false;
		bOutHasEnd = false;
		OutStart = 0;
		OutEnd = 0;
		if (!Section)
		{
			return;
		}

		const TRange<FFrameNumber> Range = Section->GetRange();
		if (Range.GetLowerBound().IsClosed())
		{
			bOutHasStart = true;
			OutStart = TickToDisplay(MovieScene, Range.GetLowerBoundValue());
		}
		if (Range.GetUpperBound().IsClosed())
		{
			bOutHasEnd = true;
			OutEnd = TickToDisplay(MovieScene, Range.GetUpperBoundValue());
		}
	}

	TSharedPtr<FJsonObject> MakeSectionJson(const UMovieSceneSection* Section, const UMovieScene* MovieScene)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		bool bHasStart = false, bHasEnd = false;
		int32 Start = 0, End = 0;
		SectionRange(Section, MovieScene, bHasStart, Start, bHasEnd, End);

		if (bHasStart) { Obj->SetNumberField(TEXT("start"), Start); }
		else           { Obj->SetField(TEXT("start"), MakeShared<FJsonValueNull>()); }
		if (bHasEnd)   { Obj->SetNumberField(TEXT("end"), End); }
		else           { Obj->SetField(TEXT("end"), MakeShared<FJsonValueNull>()); }

		return Obj;
	}

	/**
	 * 读一条通道的关键帧。
	 *
	 * 基类 `FMovieSceneChannel` 只给得出 key 数；取值要按通道类型下转。
	 * 认不出的类型只报数量不报值 —— **报一个错的值比不报值糟得多**。
	 */
	void ReadChannelKeys(
		FMovieSceneChannel* Channel, const FName& ChannelTypeName, const UMovieScene* MovieScene,
		int32 MaxKeys, TSharedPtr<FJsonObject>& OutJson, bool& bOutTruncated)
	{
		const int32 NumKeys = Channel ? Channel->GetNumKeys() : 0;
		OutJson->SetNumberField(TEXT("key_count"), NumKeys);

		if (!Channel || NumKeys == 0)
		{
			OutJson->SetArrayField(TEXT("keys"), TArray<TSharedPtr<FJsonValue>>());
			return;
		}
		if (NumKeys > MaxKeys)
		{
			// 截断如实上报，不悄悄少给：模型拿一份看起来完整的数据会错得无声无息
			bOutTruncated = true;
			return;
		}

		TArray<FFrameNumber> Times;
		TArray<FKeyHandle> Handles;
		Channel->GetKeys(TRange<FFrameNumber>::All(), &Times, &Handles);

		// 取值：按类型下转。类型名来自 FMovieSceneChannelEntry::GetChannelTypeName()
		TArray<TSharedPtr<FJsonValue>> KeyArray;
		KeyArray.Reserve(Times.Num());

		const FMovieSceneFloatChannel*   FloatCh  = ChannelTypeName == FMovieSceneFloatChannel::StaticStruct()->GetFName()
			? static_cast<const FMovieSceneFloatChannel*>(Channel) : nullptr;
		const FMovieSceneDoubleChannel*  DoubleCh = ChannelTypeName == FMovieSceneDoubleChannel::StaticStruct()->GetFName()
			? static_cast<const FMovieSceneDoubleChannel*>(Channel) : nullptr;
		const FMovieSceneBoolChannel*    BoolCh   = ChannelTypeName == FMovieSceneBoolChannel::StaticStruct()->GetFName()
			? static_cast<const FMovieSceneBoolChannel*>(Channel) : nullptr;
		const FMovieSceneIntegerChannel* IntCh    = ChannelTypeName == FMovieSceneIntegerChannel::StaticStruct()->GetFName()
			? static_cast<const FMovieSceneIntegerChannel*>(Channel) : nullptr;

		for (int32 Index = 0; Index < Times.Num(); ++Index)
		{
			TArray<TSharedPtr<FJsonValue>> Pair;
			Pair.Add(MakeShared<FJsonValueNumber>(TickToDisplay(MovieScene, Times[Index])));

			if (FloatCh && FloatCh->GetValues().IsValidIndex(Index))
			{
				Pair.Add(MakeShared<FJsonValueNumber>(FloatCh->GetValues()[Index].Value));
			}
			else if (DoubleCh && DoubleCh->GetValues().IsValidIndex(Index))
			{
				Pair.Add(MakeShared<FJsonValueNumber>(DoubleCh->GetValues()[Index].Value));
			}
			else if (BoolCh && BoolCh->GetValues().IsValidIndex(Index))
			{
				Pair.Add(MakeShared<FJsonValueBoolean>(BoolCh->GetValues()[Index]));
			}
			else if (IntCh && IntCh->GetValues().IsValidIndex(Index))
			{
				Pair.Add(MakeShared<FJsonValueNumber>(IntCh->GetValues()[Index]));
			}
			else
			{
				// 认不出的通道类型：报帧号，值给 null。不猜。
				Pair.Add(MakeShared<FJsonValueNull>());
			}

			KeyArray.Add(MakeShared<FJsonValueArray>(Pair));
		}

		OutJson->SetArrayField(TEXT("keys"), KeyArray);
	}

	/** 把一个段的所有通道读成 JSON 数组 */
	TArray<TSharedPtr<FJsonValue>> ReadSectionChannels(
		UMovieSceneSection* Section, const UMovieScene* MovieScene, int32 MaxKeys, bool& bOutTruncated)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		if (!Section)
		{
			return Result;
		}

		FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
		for (const FMovieSceneChannelEntry& Entry : Proxy.GetAllEntries())
		{
			const FName TypeName = Entry.GetChannelTypeName();
			TArrayView<FMovieSceneChannel* const> Channels = Entry.GetChannels();

#if WITH_EDITORONLY_DATA
			TArrayView<const FMovieSceneChannelMetaData> MetaData = Entry.GetMetaData();
#endif

			for (int32 Index = 0; Index < Channels.Num(); ++Index)
			{
				TSharedPtr<FJsonObject> ChannelJson = MakeShared<FJsonObject>();

				FString ChannelName;
#if WITH_EDITORONLY_DATA
				if (MetaData.IsValidIndex(Index))
				{
					ChannelName = MetaData[Index].Name.ToString();
				}
#endif
				if (ChannelName.IsEmpty())
				{
					ChannelName = FString::Printf(TEXT("%s[%d]"), *TypeName.ToString(), Index);
				}
				ChannelJson->SetStringField(TEXT("name"), ChannelName);

				ReadChannelKeys(Channels[Index], TypeName, MovieScene, MaxKeys, ChannelJson, bOutTruncated);
				Result.Add(MakeShared<FJsonValueObject>(ChannelJson));
			}
		}

		return Result;
	}

	// ========================================================================
	// 相机切轨体检
	// ========================================================================

	struct FCameraCutInfo
	{
		bool bExists = false;
		int32 SectionCount = 0;
		UAL_CameraCutCoverage::FCutCoverage Coverage;
	};

	/**
	 * 播放范围换算成 display 帧。
	 *
	 * 无界的那一头按 0 算，于是范围会变成空的 —— 覆盖判定会把空范围归入
	 * 「判不出来」而不是「没盖满」。这个区别要命：写入侧照着「没盖满」会去删段。
	 *
	 * 引擎自己在 `UMovieScene::UpgradeTimeRanges()`（编辑器每次加载都跑）里，
	 * 会把损坏或无界的播放范围强制成 `[0, 0)`，所以空范围是真会出现的状态。
	 */
	void PlaybackInDisplayFrames(const UMovieScene* MovieScene, int32& OutStart, int32& OutEnd)
	{
		OutStart = 0;
		OutEnd = 0;
		if (!MovieScene)
		{
			return;
		}
		const TRange<FFrameNumber> Range = MovieScene->GetPlaybackRange();
		if (Range.GetLowerBound().IsClosed())
		{
			OutStart = TickToDisplay(MovieScene, Range.GetLowerBoundValue());
		}
		if (Range.GetUpperBound().IsClosed())
		{
			OutEnd = TickToDisplay(MovieScene, Range.GetUpperBoundValue());
		}
	}

	/**
	 * 读切轨的段，把区间算术交给 `UAL_CameraCutCoverage`。
	 *
	 * 算术不留在这里，是因为它原来在本文件里有两份手抄本（这里一份、
	 * `AuditOne` 一份），连同一个 bug 也抄了两份。抽出去之后它能脱离引擎单测。
	 */
	FCameraCutInfo InspectCameraCuts(UMovieScene* MovieScene, int32 PlaybackStart, int32 PlaybackEnd)
	{
		FCameraCutInfo Info;
		if (!MovieScene)
		{
			return Info;
		}

		// 核心直接给了 GetCameraCutTrack()，不用像 Python 那边遍历再 isinstance
		UMovieSceneTrack* CutTrack = MovieScene->GetCameraCutTrack();
		if (!CutTrack)
		{
			return Info;
		}

		Info.bExists = true;
		const TArray<UMovieSceneSection*>& Sections = CutTrack->GetAllSections();
		Info.SectionCount = Sections.Num();

		TArray<UAL_CameraCutCoverage::FCutRange> Ranges;
		int32 Unbounded = 0;
		for (const UMovieSceneSection* Section : Sections)
		{
			bool bHasStart = false, bHasEnd = false;
			int32 Start = 0, End = 0;
			SectionRange(Section, MovieScene, bHasStart, Start, bHasEnd, End);
			if (bHasStart && bHasEnd)
			{
				Ranges.Add({ Start, End });
			}
			else
			{
				// 一头无界的段盖到哪里算不出来。悄悄丢掉它再说「没盖满」，
				// 等于给写入侧一个删段的理由 —— 记下来，让结论说「不知道」
				++Unbounded;
			}
		}

		Info.Coverage =
			UAL_CameraCutCoverage::Analyse(MoveTemp(Ranges), PlaybackStart, PlaybackEnd, Unbounded);
		return Info;
	}

	TSharedPtr<FJsonObject> MakeCameraCutsJson(const FCameraCutInfo& Info)
	{
		const UAL_CameraCutCoverage::FCutCoverage& Coverage = Info.Coverage;

		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetBoolField(TEXT("exists"), Info.bExists);
		Json->SetNumberField(TEXT("section_count"), Info.SectionCount);
		Json->SetBoolField(TEXT("covers_playback"), Coverage.bCoversPlayback);

		// 判不出来要单独说。只给 covers_playback=false 的话，模型会把
		// 「这次没查成」读成「查出来没盖满」，然后去修一个可能没坏的东西
		Json->SetBoolField(TEXT("coverage_known"), Coverage.bCoverageKnown);
		if (!Coverage.bCoverageKnown)
		{
			Json->SetStringField(TEXT("coverage_unknown_reason"), Coverage.UnknownReason);
		}

		auto RangesToJson = [](const TArray<UAL_CameraCutCoverage::FCutRange>& Ranges)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const UAL_CameraCutCoverage::FCutRange& Range : Ranges)
			{
				TArray<TSharedPtr<FJsonValue>> Pair;
				Pair.Add(MakeShared<FJsonValueNumber>(Range.Start));
				Pair.Add(MakeShared<FJsonValueNumber>(Range.End));
				Out.Add(MakeShared<FJsonValueArray>(Pair));
			}
			return Out;
		};

		Json->SetArrayField(TEXT("gaps"), RangesToJson(Coverage.Gaps));
		// 重叠原来算了又丢掉，于是 describe 在结构上永远报不出重叠，
		// 只有 audit 报得出 —— 同一条序列，两个工具两种说法
		Json->SetArrayField(TEXT("overlaps"), RangesToJson(Coverage.Overlaps));

		return Json;
	}

	// ========================================================================
	// 能力回报
	// ========================================================================

	/**
	 * 这台引擎实际支持什么。
	 *
	 * 走 C++ 之后大部分「这个方法在不在」的问题在编译期就没了，
	 * 但**仍然要回报** —— 否则模型会把「这个版本判不出来」误当成
	 * 「这个资产没有问题」。静默降级是不行的。
	 */
	TSharedPtr<FJsonObject> MakeCapabilitiesJson(bool bBindingResolution)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		// C++ 侧没有 get_tracks / get_master_tracks 之分，核心一直是同一个 API
		Json->SetStringField(TEXT("tracks_api"), TEXT("native"));
		// FindSpawnable / FindPossessable 全版本可用，类型判定不会失败
		Json->SetBoolField(TEXT("spawnable_detection"), true);
		Json->SetBoolField(TEXT("binding_resolution"), bBindingResolution);
		return Json;
	}
}

// ============================================================================
// 注册
// ============================================================================

void FUAL_SequencerCommands::RegisterCommands(
	TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("sequence.describe"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_Describe(Payload, RequestId);
	});

	CommandMap.Add(TEXT("sequence.audit"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_Audit(Payload, RequestId);
	});

	CommandMap.Add(TEXT("sequence.camera_keys"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CameraKeys(Payload, RequestId);
	});

	CommandMap.Add(TEXT("sequence.camera_cuts"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CameraCuts(Payload, RequestId);
	});
}

// ============================================================================
// sequence.describe
// ============================================================================

void FUAL_SequencerCommands::Handle_Describe(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString SequencePath;
	if (!Payload->TryGetStringField(TEXT("sequence_path"), SequencePath))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: sequence_path"));
		return;
	}

	FString Detail = TEXT("outline");
	Payload->TryGetStringField(TEXT("detail"), Detail);

	const bool bWantTracks = (Detail == TEXT("tracks") || Detail == TEXT("keys"));
	const bool bWantKeys = (Detail == TEXT("keys"));

	/**
	 * `detail="names"`：只要名单。
	 *
	 * 没有它的时候，读这一侧是个闭环死角：绑定超过 60 条就会截断，而截断提示让你
	 * 「用 bindings 点名」—— 你得先知道名字，名字恰恰就在被截掉的那部分里。
	 * 2026-09-21 的用户反馈里那条序列有 268 个绑定，最后是自己写 Python 遍历读出来的。
	 *
	 * 名单这一层只回名字/类型/轨道数，一条绑定几十字节，上限放到 2000
	 * 仍然远小于 outline 的一次回传。不查解析（那是逐个进世界找对象，
	 * 名单不需要），所以也更快。
	 */
	const bool bNamesOnly = (Detail == TEXT("names"));

	int32 MaxBindings = bNamesOnly ? 2000 : 60;
	int32 MaxKeys = 200;
	{
		double Tmp = 0.0;
		if (Payload->TryGetNumberField(TEXT("max_bindings"), Tmp) && Tmp > 0) { MaxBindings = static_cast<int32>(Tmp); }
		if (Payload->TryGetNumberField(TEXT("max_keys"), Tmp) && Tmp > 0)     { MaxKeys = static_cast<int32>(Tmp); }
	}

	// bindings 过滤：按显示名点名
	TSet<FString> Wanted;
	const TArray<TSharedPtr<FJsonValue>>* WantedArray = nullptr;
	if (Payload->TryGetArrayField(TEXT("bindings"), WantedArray) && WantedArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *WantedArray)
		{
			FString Name;
			if (Value.IsValid() && Value->TryGetString(Name))
			{
				Wanted.Add(Name);
			}
		}
	}
	const bool bFilterBindings = Wanted.Num() > 0;

	FString LoadError;
	ULevelSequence* Sequence = LoadSequence(SequencePath, LoadError);
	if (!Sequence)
	{
		UAL_CommandUtils::SendError(RequestId, 404, LoadError);
		return;
	}

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	int32 PlaybackStart = 0, PlaybackEnd = 0;
	PlaybackInDisplayFrames(MovieScene, PlaybackStart, PlaybackEnd);

	UWorld* World = GetSequencerResolveWorld();

	// ── 绑定 ──────────────────────────────────────────────────────────────
	TArray<TSharedPtr<FJsonValue>> BindingArray;
	TArray<TSharedPtr<FJsonValue>> BrokenArray;
	TArray<TSharedPtr<FJsonValue>> UnresolvedArray;
	bool bTruncated = false;
	bool bBindingResolutionWorks = true;

	for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
	{
		const FGuid& Guid = Binding.GetObjectGuid();
		EUALBindingKind Kind = EUALBindingKind::Unknown;
		const FString Name = ResolveBindingNameForBinding(MovieScene, Binding, Kind);

		if (bFilterBindings && !Wanted.Contains(Name))
		{
			continue;
		}

		TSharedPtr<FJsonObject> BindingJson = MakeShared<FJsonObject>();
		BindingJson->SetStringField(TEXT("name"), Name);
		BindingJson->SetStringField(TEXT("id"), Guid.ToString());
		BindingJson->SetStringField(TEXT("type"), BindingKindToString(Kind));
		BindingJson->SetNumberField(TEXT("track_count"), Binding.GetTracks().Num());

		// bound_to：spawnable 的对象模板
		FString BoundTo;
		if (Kind == EUALBindingKind::Spawnable)
		{
			if (const FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(Guid))
			{
				if (const UObject* Template = Spawnable->GetObjectTemplate())
				{
					BoundTo = Template->GetPathName();
				}
			}
		}
		if (BoundTo.IsEmpty()) { BindingJson->SetField(TEXT("bound_to"), MakeShared<FJsonValueNull>()); }
		else                   { BindingJson->SetStringField(TEXT("bound_to"), BoundTo); }

		// 组件绑定的父级。
		//
		// 光有「LightComponent0」这个名字，用户和模型都看不出它是谁身上的组件 ——
		// 首次真机测试时就出现了这个困惑。带上父级名字，一眼能看出
		// 「LightComponent0 是 DirectionalLight 的组件」。
		if (const FMovieScenePossessable* Self = MovieScene->FindPossessable(Guid))
		{
			const FGuid ParentGuid = Self->GetParent();
			if (ParentGuid.IsValid())
			{
				EUALBindingKind ParentKind = EUALBindingKind::Unknown;
				const FString ParentName = ResolveBindingName(MovieScene, ParentGuid, ParentKind);
				if (!ParentName.IsEmpty())
				{
					BindingJson->SetStringField(TEXT("parent"), ParentName);
				}
			}
		}

		// 绑定是否还能解析到对象。
		//
		// **解析不了 ≠ 断链**：World Partition 里 actor 没加载、正在 PIE、
		// 拿不到编辑器世界，都会让好绑定看起来是坏的。报一个假的「已失效」
		// 会让用户去修一个没坏的东西 —— 所以判不出来归入 unresolved，分开报。
		if (Kind != EUALBindingKind::Spawnable && !bNamesOnly)
		{
			TArray<UObject*, TInlineAllocator<1>> Bound;
			const bool bResolvable = ResolveBindingObjects(Sequence, MovieScene, Guid, World, Bound);
			if (!bResolvable)
			{
				bBindingResolutionWorks = false;
				UnresolvedArray.Add(MakeShared<FJsonValueString>(Name));
			}
			else if (Bound.Num() == 0)
			{
				BrokenArray.Add(MakeShared<FJsonValueString>(Name));
			}
		}

		// ── 轨道 ──────────────────────────────────────────────────────────
		if (bWantTracks)
		{
			TArray<TSharedPtr<FJsonValue>> TrackArray;
			for (UMovieSceneTrack* Track : Binding.GetTracks())
			{
				if (!Track)
				{
					continue;
				}

				TSharedPtr<FJsonObject> TrackJson = MakeShared<FJsonObject>();
#if WITH_EDITORONLY_DATA
				TrackJson->SetStringField(TEXT("name"), Track->GetDisplayName().ToString());
#else
				TrackJson->SetStringField(TEXT("name"), Track->GetName());
#endif
				TrackJson->SetStringField(TEXT("type"), Track->GetClass()->GetName());

				TArray<TSharedPtr<FJsonValue>> SectionArray;
				for (UMovieSceneSection* Section : Track->GetAllSections())
				{
					TSharedPtr<FJsonObject> SectionJson = MakeSectionJson(Section, MovieScene);
					if (bWantKeys)
					{
						SectionJson->SetArrayField(
							TEXT("channels"), ReadSectionChannels(Section, MovieScene, MaxKeys, bTruncated));
					}
					SectionArray.Add(MakeShared<FJsonValueObject>(SectionJson));
				}
				TrackJson->SetArrayField(TEXT("sections"), SectionArray);
				TrackArray.Add(MakeShared<FJsonValueObject>(TrackJson));
			}
			BindingJson->SetArrayField(TEXT("tracks"), TrackArray);
		}

		BindingArray.Add(MakeShared<FJsonValueObject>(BindingJson));
		if (BindingArray.Num() >= MaxBindings)
		{
			bTruncated = true;
			break;
		}
	}

	// ── 组装 ──────────────────────────────────────────────────────────────
	TSharedPtr<FJsonObject> SequenceJson = MakeShared<FJsonObject>();
	SequenceJson->SetStringField(TEXT("path"), SequencePath);
	SequenceJson->SetStringField(
		TEXT("display_rate"), FString::Printf(TEXT("%d/%d"), DisplayRate.Numerator, DisplayRate.Denominator));
	SequenceJson->SetNumberField(TEXT("playback_start"), PlaybackStart);
	SequenceJson->SetNumberField(TEXT("playback_end"), PlaybackEnd);
	SequenceJson->SetNumberField(TEXT("duration_frames"), PlaybackEnd - PlaybackStart);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetObjectField(TEXT("sequence"), SequenceJson);
	Data->SetArrayField(TEXT("bindings"), BindingArray);
	Data->SetObjectField(TEXT("camera_cuts"), MakeCameraCutsJson(InspectCameraCuts(MovieScene, PlaybackStart, PlaybackEnd)));
	Data->SetArrayField(TEXT("broken_bindings"), BrokenArray);
	Data->SetArrayField(TEXT("unresolved_bindings"), UnresolvedArray);
	Data->SetObjectField(TEXT("capabilities"), MakeCapabilitiesJson(bBindingResolutionWorks));
	Data->SetBoolField(TEXT("truncated"), bTruncated);
	// 这条序列**一共**有多少绑定。回传的那份可能被过滤或截断过，
	// 少了这个数，调用方会把手里的条数当成全部
	Data->SetNumberField(TEXT("binding_total"), MovieScene->GetBindings().Num());
	// names 层没跑失效检查。不说的话，空的 broken 数组会被读成「没有失效的绑定」
	Data->SetBoolField(TEXT("resolution_checked"), !bNamesOnly);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// sequence.audit
// ============================================================================

namespace
{
	/** 一条 finding。code 是稳定 id，解释文案在 TS 的 FINDINGS 目录里 */
	TSharedPtr<FJsonValue> MakeFinding(
		const TCHAR* Code, const TCHAR* Severity, const FString& Evidence, bool bAutoFixable = false)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("code"), Code);
		Json->SetStringField(TEXT("severity"), Severity);
		Json->SetStringField(TEXT("evidence"), Evidence);
		Json->SetBoolField(TEXT("autoFixable"), bAutoFixable);
		return MakeShared<FJsonValueObject>(Json);
	}

	FString TrackDisplayName(const UMovieSceneTrack* Track)
	{
		if (!Track)
		{
			return FString();
		}
#if WITH_EDITORONLY_DATA
		return Track->GetDisplayName().ToString();
#else
		return Track->GetName();
#endif
	}

	/** 资产路径 → 给人看的短名（`/Game/Cine/Shot_01.Shot_01` → `Shot_01`） */
	FString ShortNameOf(const FString& Path)
	{
		FString Tail = Path;
		int32 SlashIndex = INDEX_NONE;
		if (Path.FindLastChar(TEXT('/'), SlashIndex))
		{
			Tail = Path.Mid(SlashIndex + 1);
		}
		int32 DotIndex = INDEX_NONE;
		if (Tail.FindChar(TEXT('.'), DotIndex))
		{
			Tail = Tail.Left(DotIndex);
		}
		return Tail;
	}

	/**
	 * 体检一条序列，递归进子序列。
	 *
	 * @param Seen 防环。**每个顶层路径一份，不跨顶层共享** —— 共享的话，
	 *             用户同时传 master 和它的子序列时，子序列已在 master 的递归里
	 *             被访问过，于是那条报告变成「零问题 = PASS」。
	 *             显式点名要查的东西被判成通过，是最坏的一种错。
	 */
	void AuditOne(
		const FString& Path, UWorld* World, bool bRecursive,
		TSet<FString>& Seen, TArray<TSharedPtr<FJsonValue>>& OutFindings, FString& OutError)
	{
		if (Seen.Contains(Path))
		{
			return;
		}
		Seen.Add(Path);

		ULevelSequence* Sequence = LoadSequence(Path, OutError);
		if (!Sequence)
		{
			return;
		}

		UMovieScene* MovieScene = Sequence->GetMovieScene();
		int32 PlaybackStart = 0, PlaybackEnd = 0;
		PlaybackInDisplayFrames(MovieScene, PlaybackStart, PlaybackEnd);

		// ── 相机切轨：黑帧的主要来源 ──────────────────────────────────────
		//
		// 和 describe / camera_cuts 共用 InspectCameraCuts。这里原来是它的
		// 第二份手抄本，两份的空隙判定里有同一个 bug（拿前一段的 end 当基准，
		// 嵌套的段会被报出一段不存在的空隙）
		const FCameraCutInfo Cuts = InspectCameraCuts(MovieScene, PlaybackStart, PlaybackEnd);
		if (!Cuts.bExists)
		{
			OutFindings.Add(MakeFinding(TEXT("no_camera_cut_track"), TEXT("breaks_render"), TEXT("整条序列")));
		}
		else if (Cuts.SectionCount == 0)
		{
			OutFindings.Add(MakeFinding(TEXT("camera_cut_empty"), TEXT("breaks_render"), TEXT("Camera Cuts 轨道")));
		}
		else
		{
			const UAL_CameraCutCoverage::FCutCoverage& Coverage = Cuts.Coverage;

			// 判不出来是「这次没查成」，不是「查出来没问题」，也不是「坏了」
			if (!Coverage.bCoverageKnown)
			{
				OutFindings.Add(MakeFinding(
					TEXT("camera_cut_coverage_unknown"), TEXT("unknown"), Coverage.UnknownReason));
			}
			if (Coverage.HeadUncovered.IsSet())
			{
				OutFindings.Add(MakeFinding(TEXT("camera_cut_not_covering"), TEXT("breaks_render"),
					FString::Printf(TEXT("播放范围开头第 %d–%d 帧"),
						Coverage.HeadUncovered->Start, Coverage.HeadUncovered->End)));
			}
			if (Coverage.TailUncovered.IsSet())
			{
				OutFindings.Add(MakeFinding(TEXT("camera_cut_not_covering"), TEXT("breaks_render"),
					FString::Printf(TEXT("播放范围结尾第 %d–%d 帧"),
						Coverage.TailUncovered->Start, Coverage.TailUncovered->End)));
			}
			for (const UAL_CameraCutCoverage::FCutRange& Gap : Coverage.Gaps)
			{
				OutFindings.Add(MakeFinding(TEXT("camera_cut_gap"), TEXT("breaks_render"),
					FString::Printf(TEXT("第 %d–%d 帧"), Gap.Start, Gap.End), true));
			}
			for (const UAL_CameraCutCoverage::FCutRange& Overlap : Coverage.Overlaps)
			{
				OutFindings.Add(MakeFinding(TEXT("camera_cut_overlap"), TEXT("breaks_render"),
					FString::Printf(TEXT("第 %d–%d 帧"), Overlap.Start, Overlap.End)));
			}
		}

		// ── 绑定 ──────────────────────────────────────────────────────────
		TSet<FString> CameraKinds;
		for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
		{
			const FGuid& Guid = Binding.GetObjectGuid();
			EUALBindingKind Kind = EUALBindingKind::Unknown;
			const FString Name = ResolveBindingNameForBinding(MovieScene, Binding, Kind);
			const TArray<UMovieSceneTrack*>& Tracks = Binding.GetTracks();

			if (Tracks.Num() == 0)
			{
				OutFindings.Add(MakeFinding(TEXT("binding_without_tracks"), TEXT("cosmetic"), Name));
			}

			// spawnable 由序列自己生成，不存在「找不到关卡里的 Actor」
			if (Kind != EUALBindingKind::Spawnable)
			{
				TArray<UObject*, TInlineAllocator<1>> Bound;
				if (!ResolveBindingObjects(Sequence, MovieScene, Guid, World, Bound))
				{
					OutFindings.Add(MakeFinding(TEXT("unresolved_binding"), TEXT("unknown"), Name));
				}
				else if (Bound.Num() == 0)
				{
					OutFindings.Add(MakeFinding(TEXT("broken_binding"), TEXT("breaks_render"), Name, true));
				}
			}

			// 相机混用 spawnable / possessable，切点上可能渲错实例
			FString ClassName;
			if (const FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(Guid))
			{
				if (const UObject* Template = Spawnable->GetObjectTemplate())
				{
					ClassName = Template->GetClass()->GetName();
				}
			}
			else if (const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Guid))
			{
				if (const UClass* PossessedClass = Possessable->GetPossessedObjectClass())
				{
					ClassName = PossessedClass->GetName();
				}
			}
			if (ClassName.Contains(TEXT("Camera")) || Name.Contains(TEXT("Camera")))
			{
				CameraKinds.Add(Kind == EUALBindingKind::Spawnable ? TEXT("spawnable") : TEXT("possessable"));
			}

			// 段越界 / 空轨道
			for (const UMovieSceneTrack* Track : Tracks)
			{
				if (!Track)
				{
					continue;
				}
				const FString TrackName = TrackDisplayName(Track);
				const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
				if (Sections.Num() == 0)
				{
					OutFindings.Add(MakeFinding(TEXT("empty_track"), TEXT("cosmetic"),
						FString::Printf(TEXT("%s → %s"), *Name, *TrackName)));
				}
				for (const UMovieSceneSection* Section : Sections)
				{
					bool bHasStart = false, bHasEnd = false;
					int32 Start = 0, End = 0;
					SectionRange(Section, MovieScene, bHasStart, Start, bHasEnd, End);
					if (!bHasStart || !bHasEnd)
					{
						continue;
					}
					if (End <= PlaybackStart || Start >= PlaybackEnd)
					{
						OutFindings.Add(MakeFinding(TEXT("section_out_of_range"), TEXT("breaks_preview"),
							FString::Printf(TEXT("%s → %s 的段 [%d, %d)"), *Name, *TrackName, Start, End)));
					}
				}
			}
		}

		if (CameraKinds.Num() > 1)
		{
			OutFindings.Add(MakeFinding(TEXT("mixed_camera_binding_types"), TEXT("breaks_preview"), TEXT("相机绑定")));
		}

		// ── 子序列：时长对不上 + 递归 ────────────────────────────────────
		//
		// 顶层轨道走 UALCompat::GetRootTracks —— 4.27 上这个访问器叫
		// GetMasterTracks，5.x 才改名 GetTracks。这条是**真的发生在引擎核心里**
		// 的改名，不像绑定 proxy 那些只在 Python 层。见 UAL_VersionCompat.h。
		TArray<FString> Children;
		for (UMovieSceneTrack* Track : UALCompat::GetRootTracks(MovieScene))
		{
			UMovieSceneSubTrack* SubTrack = Cast<UMovieSceneSubTrack>(Track);
			if (!SubTrack)
			{
				continue;
			}
			for (UMovieSceneSection* Section : SubTrack->GetAllSections())
			{
				UMovieSceneSubSection* SubSection = Cast<UMovieSceneSubSection>(Section);
				if (!SubSection)
				{
					continue;
				}
				UMovieSceneSequence* Sub = SubSection->GetSequence();
				if (!Sub || !Sub->GetMovieScene())
				{
					continue;
				}

				bool bHasStart = false, bHasEnd = false;
				int32 Start = 0, End = 0;
				SectionRange(Section, MovieScene, bHasStart, Start, bHasEnd, End);

				const UMovieScene* SubScene = Sub->GetMovieScene();
				const TRange<FFrameNumber> SubRange = SubScene->GetPlaybackRange();
				if (bHasStart && bHasEnd && SubRange.GetLowerBound().IsClosed() && SubRange.GetUpperBound().IsClosed())
				{
					const int32 Inner = TickToDisplay(SubScene, SubRange.GetUpperBoundValue())
						- TickToDisplay(SubScene, SubRange.GetLowerBoundValue());
					if ((End - Start) != Inner)
					{
						OutFindings.Add(MakeFinding(TEXT("subsequence_length_mismatch"), TEXT("breaks_preview"),
							FString::Printf(TEXT("%s：父层 %d 帧 / 子序列 %d 帧"), *Sub->GetName(), End - Start, Inner)));
					}
				}

				Children.Add(Sub->GetPathName());
			}
		}

		if (bRecursive)
		{
			for (const FString& Child : Children)
			{
				TArray<TSharedPtr<FJsonValue>> ChildFindings;
				FString ChildError;
				AuditOne(Child, World, bRecursive, Seen, ChildFindings, ChildError);

				if (!ChildError.IsEmpty())
				{
					OutFindings.Add(MakeFinding(TEXT("subsequence_unreadable"), TEXT("breaks_render"),
						FString::Printf(TEXT("%s：%s"), *Child, *ChildError)));
					continue;
				}

				// 证据前缀上子序列名。不带的话，一条挂 24 个 shot 的 master
				// 报出「Hero 绑定已失效」，用户不知道该去打开哪一条子序列修
				const FString Short = ShortNameOf(Child);
				for (const TSharedPtr<FJsonValue>& Value : ChildFindings)
				{
					const TSharedPtr<FJsonObject>* Obj = nullptr;
					if (Value.IsValid() && Value->TryGetObject(Obj) && Obj)
					{
						FString Evidence;
						(*Obj)->TryGetStringField(TEXT("evidence"), Evidence);
						(*Obj)->SetStringField(TEXT("evidence"), Short + TEXT(" → ") + Evidence);
					}
					OutFindings.Add(Value);
				}
			}
		}
	}
}

void FUAL_SequencerCommands::Handle_Audit(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const TArray<TSharedPtr<FJsonValue>>* PathArray = nullptr;
	if (!Payload->TryGetArrayField(TEXT("paths"), PathArray) || !PathArray || PathArray->Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: paths"));
		return;
	}

	bool bRecursive = true;
	Payload->TryGetBoolField(TEXT("recursive"), bRecursive);

	UWorld* World = GetSequencerResolveWorld();
	bool bBindingResolutionWorks = World != nullptr;

	TArray<TSharedPtr<FJsonValue>> Reports;
	for (const TSharedPtr<FJsonValue>& Value : *PathArray)
	{
		FString Path;
		if (!Value.IsValid() || !Value->TryGetString(Path) || Path.IsEmpty())
		{
			continue;
		}

		TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("path"), Path);

		// seen 每条顶层路径一份 —— 理由见 AuditOne 的注释
		TSet<FString> Seen;
		TArray<TSharedPtr<FJsonValue>> Findings;
		FString Error;
		AuditOne(Path, World, bRecursive, Seen, Findings, Error);

		if (!Error.IsEmpty())
		{
			// 一条读不出来不能带走其余的 —— 一轮 24 条里坏一条，
			// 用户要的是另外 23 条的结论，而不是整个调用失败
			Report->SetArrayField(TEXT("findings"), TArray<TSharedPtr<FJsonValue>>());
			Report->SetStringField(TEXT("error"), Error);
		}
		else
		{
			Report->SetArrayField(TEXT("findings"), Findings);
		}

		Reports.Add(MakeShared<FJsonValueObject>(Report));
	}

	TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();
	Capabilities->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
	Capabilities->SetBoolField(TEXT("binding_resolution"), bBindingResolutionWorks);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("reports"), Reports);
	Data->SetObjectField(TEXT("capabilities"), Capabilities);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// 写入侧
// ============================================================================

namespace
{
	/**
	 * 找关卡里叫这个名字的 Actor。
	 *
	 * 按 Actor 标签（Outliner 里显示的那个名字）找，不是按内部名 ——
	 * 用户和模型说的都是标签。
	 */
	AActor* FindActorByLabelInWorld(UWorld* World, const FString& Label)
	{
		if (!World)
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor && Actor->GetActorLabel() == Label)
			{
				return Actor;
			}
		}
		return nullptr;
	}

	/**
	 * 只查不建。
	 *
	 * 和 `CreateSequenceAsset` 分开，是为了让 handler 能「先把所有会失败的判断
	 * 做完，再开始动手」。合在一起的话，路径指向一个非 Level Sequence 的资产
	 * 这种纯校验错误，也要等到相机已经生成进关卡之后才发现。
	 *
	 * @return nullptr 且 OutError 为空 = 那条路径上还没有东西，可以新建
	 */
	ULevelSequence* FindExistingSequence(const FString& Path, FString& OutError)
	{
		UObject* Existing = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
		if (!Existing)
		{
			return nullptr;
		}

		ULevelSequence* Sequence = Cast<ULevelSequence>(Existing);
		if (!Sequence)
		{
			OutError = FString::Printf(
				TEXT("%s 已存在但不是 Level Sequence，实际是 %s"), *Path, *Existing->GetClass()->GetName());
			return nullptr;
		}
		if (!Sequence->GetMovieScene())
		{
			OutError = FString::Printf(TEXT("%s 没有 MovieScene（资产可能损坏）"), *Path);
			return nullptr;
		}
		return Sequence;
	}

	/** 在指定路径新建一条序列。调用方要先用 `FindExistingSequence` 确认那里是空的 */
	ULevelSequence* CreateSequenceAsset(const FString& Path, double Fps, FString& OutError)
	{
		// /Game/Cine/SQ_Shot01.SQ_Shot01 和 /Game/Cine/SQ_Shot01 都要能接
		FString PackagePath = Path;
		int32 DotIndex = INDEX_NONE;
		if (PackagePath.FindChar(TEXT('.'), DotIndex))
		{
			PackagePath = PackagePath.Left(DotIndex);
		}

		FString FolderPath, AssetName;
		if (!PackagePath.Split(TEXT("/"), &FolderPath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
		{
			OutError = FString::Printf(TEXT("路径解析不了：%s"), *Path);
			return nullptr;
		}

		// 直接 NewObject + Initialize()，不走 ULevelSequenceFactoryNew ——
		// 那个工厂住在 LevelSequenceEditor **插件**里且头文件没公开，
		// 依赖它等于给九个版本各添一份不确定性。工厂本身干的也就是这两步。
		UPackage* Package = CreatePackage(*PackagePath);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("建不出包：%s"), *PackagePath);
			return nullptr;
		}
		Package->FullyLoad();

		ULevelSequence* Sequence = NewObject<ULevelSequence>(
			Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!Sequence)
		{
			OutError = FString::Printf(TEXT("新建序列失败：%s"), *Path);
			return nullptr;
		}
		Sequence->Initialize();
		FAssetRegistryModule::AssetCreated(Sequence);

		if (!Sequence->GetMovieScene())
		{
			OutError = FString::Printf(TEXT("新建的序列没有 MovieScene：%s"), *Path);
			return nullptr;
		}

		// 帧率只在新建时设一次。改已有序列的帧率会让所有既有关键帧的时间点漂移，
		// 那是用户没要求的破坏性操作。
		Sequence->GetMovieScene()->SetDisplayRate(FFrameRate(FMath::RoundToInt(Fps), 1));

		return Sequence;
	}

	/**
	 * 只查不建：这条序列里有没有已经绑到该 Actor 的绑定。
	 *
	 * 单独拆出来是给「先探后改」用的 —— handler 要在动手之前就知道绑定在不在，
	 * 才能顺着它找到已有的 Transform 段去验通道数。
	 */
	FGuid FindPossessableFor(ULevelSequence* Sequence, AActor* Actor, UWorld* World)
	{
		UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
		if (!MovieScene || !Actor)
		{
			return FGuid();
		}

		for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
		{
			TArray<UObject*, TInlineAllocator<1>> Bound;
			if (ResolveBindingObjects(Sequence, MovieScene, Binding.GetObjectGuid(), World, Bound))
			{
				for (UObject* Object : Bound)
				{
					if (Object == Actor)
					{
						return Binding.GetObjectGuid();
					}
				}
			}
		}
		return FGuid();
	}

	/** 找这条序列里已经绑到该 Actor 的绑定；没有就新建一条 possessable */
	FGuid EnsurePossessable(ULevelSequence* Sequence, AActor* Actor, UWorld* World)
	{
		// 已有绑定就复用，否则每调一次就多一条重复绑定
		const FGuid Existing = FindPossessableFor(Sequence, Actor, World);
		if (Existing.IsValid())
		{
			return Existing;
		}

		UMovieScene* MovieScene = Sequence->GetMovieScene();
		MovieScene->Modify();
		const FGuid Guid = MovieScene->AddPossessable(Actor->GetActorLabel(), Actor->GetClass());
		Sequence->Modify();
		Sequence->BindPossessableObject(Guid, *Actor, World);
		return Guid;
	}

	/** 按插值方式往 double 通道里打一个键 */
	void AddDoubleKey(FMovieSceneDoubleChannel* Channel, FFrameNumber Time, double Value, const FString& Interpolation)
	{
		if (!Channel)
		{
			return;
		}
		if (Interpolation == TEXT("constant"))
		{
			Channel->AddConstantKey(Time, Value);
		}
		else if (Interpolation == TEXT("cubic"))
		{
			Channel->AddCubicKey(Time, Value);
		}
		else
		{
			Channel->AddLinearKey(Time, Value);
		}
	}

	/** 拿到（必要时新建）序列的相机切轨 */
	UMovieSceneCameraCutTrack* EnsureCameraCutTrack(UMovieScene* MovieScene)
	{
		if (UMovieSceneCameraCutTrack* Existing = Cast<UMovieSceneCameraCutTrack>(MovieScene->GetCameraCutTrack()))
		{
			return Existing;
		}
		return Cast<UMovieSceneCameraCutTrack>(
			MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass()));
	}

	/**
	 * 把序列资产存盘。
	 *
	 * 不存盘用户关掉编辑器就全丢了，而模型以为自己成功了
	 * （ 第 3 条）。
	 *
	 * 走 UnrealEd 的 `UEditorLoadingAndSavingUtils` 而不是
	 * `UEditorAssetLibrary` —— 后者住在 `EditorScriptingUtilities` **插件**里，
	 * 用户没启用那个插件就没有；前者是引擎内置模块，一定在。
	 */
	bool SaveSequenceAsset(ULevelSequence* Sequence)
	{
		if (!Sequence)
		{
			return false;
		}
		Sequence->MarkPackageDirty();
		UPackage* Package = Sequence->GetOutermost();
		if (!Package)
		{
			return false;
		}
		return UEditorLoadingAndSavingUtils::SavePackages({ Package }, /*bOnlyDirty=*/false);
	}
}

void FUAL_SequencerCommands::Handle_CameraKeys(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString SequencePath, CameraLabel;
	if (!Payload->TryGetStringField(TEXT("sequence_path"), SequencePath) ||
		!Payload->TryGetStringField(TEXT("camera_label"), CameraLabel))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: sequence_path / camera_label"));
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* KeyArray = nullptr;
	if (!Payload->TryGetArrayField(TEXT("keys"), KeyArray) || !KeyArray || KeyArray->Num() < 2)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("keys 至少要两个"));
		return;
	}

	// 关键帧先整批解析、验完，再动手。
	//
	// 原来是边写边 continue：不是对象的、缺 frame 的键悄悄跳过，回执里没有一个字；
	// location / rotation 都没给的键照样 ++，key_count 说写了 N 个、通道里其实少几个。
	// 全部跳过时 MinFrame 停在 MAX_int32，播放范围被设成一段乱码 —— 而那时序列和
	// 相机都已经建出来了。现在坏键逐条报原因；一个能写的都没有就 400，什么都不动
	struct FParsedCameraKey
	{
		int32 DisplayFrame = 0;
		bool bHasLocation = false;
		bool bHasRotation = false;
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
	};
	TArray<FParsedCameraKey> ParsedKeys;
	TArray<TSharedPtr<FJsonValue>> SkippedKeyReasons;
	auto SkipKey = [&SkippedKeyReasons](int32 Index, const FString& Reason)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("index"), Index);
		Item->SetStringField(TEXT("reason"), Reason);
		SkippedKeyReasons.Add(MakeShared<FJsonValueObject>(Item));
	};
	for (int32 Index = 0; Index < KeyArray->Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& Value = (*KeyArray)[Index];
		const TSharedPtr<FJsonObject>* KeyObj = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(KeyObj) || !KeyObj || !KeyObj->IsValid())
		{
			SkipKey(Index, TEXT("不是对象"));
			continue;
		}

		double FrameNumber = 0.0;
		if (!(*KeyObj)->TryGetNumberField(TEXT("frame"), FrameNumber))
		{
			SkipKey(Index, TEXT("缺 frame 或不是数字"));
			continue;
		}

		FParsedCameraKey Parsed;
		Parsed.DisplayFrame = static_cast<int32>(FrameNumber);
		// 用 TryGetObjectField 判断而不是 HasField：location 给成字符串或数组时
		// ReadVector 会退回零向量，原来会往通道里悄悄写一个 (0,0,0) 的键
		const TSharedPtr<FJsonObject>* LocationObj = nullptr;
		const TSharedPtr<FJsonObject>* RotationObj = nullptr;
		Parsed.bHasLocation = (*KeyObj)->TryGetObjectField(TEXT("location"), LocationObj) &&
			LocationObj && LocationObj->IsValid();
		Parsed.bHasRotation = (*KeyObj)->TryGetObjectField(TEXT("rotation"), RotationObj) &&
			RotationObj && RotationObj->IsValid();
		if (!Parsed.bHasLocation && !Parsed.bHasRotation)
		{
			SkipKey(Index, FString::Printf(
				TEXT("第 %d 帧 location 和 rotation 都没给（或不是对象），这个键写不出任何东西"),
				Parsed.DisplayFrame));
			continue;
		}
		if (Parsed.bHasLocation)
		{
			Parsed.Location = UAL_CommandUtils::ReadVector(*KeyObj, TEXT("location"));
		}
		if (Parsed.bHasRotation)
		{
			Parsed.Rotation = UAL_CommandUtils::ReadRotator(*KeyObj, TEXT("rotation"));
		}
		ParsedKeys.Add(Parsed);
	}
	if (ParsedKeys.Num() == 0)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetNumberField(TEXT("skipped_keys"), SkippedKeyReasons.Num());
		Details->SetArrayField(TEXT("skipped_key_reasons"), SkippedKeyReasons);
		UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(
			TEXT("%d 个关键帧没有一个能写：每个键都要有数字 frame，并且至少给 location 或 rotation 之一。")
			TEXT("什么都没有改动。"),
			KeyArray->Num()),
			Details);
		return;
	}

	double Fps = 30.0;
	Payload->TryGetNumberField(TEXT("fps"), Fps);
	FString Interpolation = TEXT("linear");
	Payload->TryGetStringField(TEXT("interpolation"), Interpolation);
	bool bReplaceExisting = true;
	Payload->TryGetBoolField(TEXT("replace_existing_keys"), bReplaceExisting);
	bool bWantCameraCuts = true;
	Payload->TryGetBoolField(TEXT("camera_cuts"), bWantCameraCuts);
	// 切轨上已经有别人排好的段时，删掉重建要显式点名
	bool bRebuildCameraCuts = false;
	Payload->TryGetBoolField(TEXT("rebuild_camera_cuts"), bRebuildCameraCuts);
	double FocalLength = 0.0;
	const bool bHasFocalLength = Payload->TryGetNumberField(TEXT("focal_length_mm"), FocalLength);

	// 同样走严格版：退回 GWorld 的话，PIE 期间相机会被生成进 PIE 世界，
	// 停止 PIE 时连同关卡一起丢掉，而工具已经报了「已保存」。
	UWorld* World = GetSequencerResolveWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500,
			TEXT("拿不到编辑器世界，无法写入相机关键帧。如果正在 PIE，请先停止运行。"));
		return;
	}

	TArray<TSharedPtr<FJsonValue>> Warnings;

	// ══ 先探，不动手 ══════════════════════════════════════════════════════
	//
	// 这一段只读。所有会返回 4xx/5xx 的判断都要在这里做完。
	//
	// **一旦开始改就没有回退可言。** `UTransBuffer::Cancel` 做的是
	// `UndoBuffer.Pop()` —— 把事务记录从撤销栈上丢掉，**从不调用
	// `FTransaction::Apply()`**。所以在改过东西之后 Cancel，改动留在原地，
	// 而且连 Ctrl+Z 都没了，比让它提交还糟。事务在这里的作用是
	// 「让用户事后能撤」，不是「出错时回滚」。
	//
	// 这个 handler 原来有三个 500 出口（建不出轨道 / 建不出段 / 通道数不够），
	// 全都在相机已经生成进关卡、绑定和轨道已经加完之后 —— 出错就给用户
	// 留下一台野相机，没有事务，也没人告诉他。

	FString Error;
	ULevelSequence* Sequence = FindExistingSequence(SequencePath, Error);
	if (!Error.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 409, Error);
		return;
	}
	const bool bSequenceCreated = (Sequence == nullptr);

	AActor* Camera = FindActorByLabelInWorld(World, CameraLabel);
	const bool bCameraCreated = (Camera == nullptr);

	// 焦距只在新建相机时设一次，不做变焦动画 —— 变焦是创作决定，不由工具替用户做。
	// 复用的相机不是 CineCamera 时设不了，这句提前说，免得混在结果末尾
	if (bHasFocalLength && !bCameraCreated && !Cast<ACineCameraActor>(Camera))
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT("复用的相机不是 CineCameraActor，焦距没有设置")));
	}

	// 唯一一个「全都建好了才发现不行」的失败点：Transform 段上挂了通道覆盖
	//（例如 Perlin 噪声容器）时，GetChannels<FMovieSceneDoubleChannel>() 拿不到
	// 预期的 9 条。序列和相机都已存在的话，顺着已有绑定能提前验掉；
	// 两者要新建时通道排布是引擎定死的，验不验一个样
	if (Sequence && Camera)
	{
		const FGuid ExistingGuid = FindPossessableFor(Sequence, Camera, World);
		UMovieScene3DTransformTrack* ExistingTrack = ExistingGuid.IsValid()
			? Sequence->GetMovieScene()->FindTrack<UMovieScene3DTransformTrack>(ExistingGuid)
			: nullptr;
		if (ExistingTrack && ExistingTrack->GetAllSections().Num() > 0)
		{
			if (UMovieScene3DTransformSection* ExistingSection =
					Cast<UMovieScene3DTransformSection>(ExistingTrack->GetAllSections()[0]))
			{
				const int32 ChannelCount =
					ExistingSection->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>().Num();
				if (ChannelCount < 6)
				{
					UAL_CommandUtils::SendError(RequestId, 409, FString::Printf(
						TEXT("这条相机的 Transform 段只有 %d 条 double 通道，预期至少 6 条 —— ")
						TEXT("段上多半挂了通道覆盖（比如 Perlin 噪声）。什么都没有改动。"),
						ChannelCount));
					return;
				}
			}
		}
	}

	// ══ 从这里开始动手 ════════════════════════════════════════════════════
	//
	// 事务包住全部写入，让用户一次 Ctrl+Z 撤得干净。往下只有一个地方
	// 允许 Cancel：序列资产没建成，那时还什么都没改。
	// 装在 TOptional 里，是为了能在存盘之前 Reset() 把事务先结束掉。
	// 存盘本身不可撤销，而 SaveCurrentLevel() 还会触发一轮 GC 和对话框 ——
	// 夹在事务里是 UAL_ScopedTransaction.h 头注释点名的那类坑
	TOptional<FUAL_ScopedTransaction> Transaction;
	Transaction.Emplace(NSLOCTEXT("UALSequencer", "CameraKeys", "Write Camera Keys"));

	if (bSequenceCreated)
	{
		Sequence = CreateSequenceAsset(SequencePath, Fps, Error);
		if (!Sequence)
		{
			Transaction->Cancel(); // 还没改过任何东西，这时取消是安全的
			UAL_CommandUtils::SendError(RequestId, 500, Error);
			return;
		}
	}
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	MovieScene->Modify();

	if (bCameraCreated)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transactional;
		ACineCameraActor* NewCamera = World->SpawnActor<ACineCameraActor>(
			ACineCameraActor::StaticClass(), FTransform::Identity, SpawnParams);
		if (!NewCamera)
		{
			// 这里**不能** Cancel：上面可能已经新建了序列资产。Cancel 会把那条
			// 撤销记录丢掉，资产反而留在工程里 —— 让事务照常提交，用户撤得掉
			UAL_CommandUtils::SendError(RequestId, 500,
				bSequenceCreated
					? TEXT("新建 CineCameraActor 失败。序列资产已经建出来了，可以 Ctrl+Z 撤掉。")
					: TEXT("新建 CineCameraActor 失败，序列没有改动。"));
			return;
		}
		NewCamera->SetActorLabel(CameraLabel);
		Camera = NewCamera;

		if (bHasFocalLength)
		{
			if (UCineCameraComponent* Component = NewCamera->GetCineCameraComponent())
			{
				Component->Modify();
				Component->SetCurrentFocalLength(static_cast<float>(FocalLength));
			}
		}
	}

	const FGuid CameraGuid = EnsurePossessable(Sequence, Camera, World);

	// ── 变换轨道 ──────────────────────────────────────────────────────────
	UMovieScene3DTransformTrack* TransformTrack = MovieScene->FindTrack<UMovieScene3DTransformTrack>(CameraGuid);
	if (!TransformTrack)
	{
		TransformTrack = MovieScene->AddTrack<UMovieScene3DTransformTrack>(CameraGuid);
	}
	if (!TransformTrack)
	{
		// 同样不能 Cancel —— 绑定已经加进去了。照常提交，说清楚留下了什么
		UAL_CommandUtils::SendError(RequestId, 500,
			TEXT("建不出 Transform 轨道。相机绑定已经加进序列了，可以 Ctrl+Z 撤掉这一笔。"));
		return;
	}
	TransformTrack->Modify();

	UMovieScene3DTransformSection* Section = TransformTrack->GetAllSections().Num() > 0
		? Cast<UMovieScene3DTransformSection>(TransformTrack->GetAllSections()[0])
		: nullptr;
	if (!Section)
	{
		Section = Cast<UMovieScene3DTransformSection>(TransformTrack->CreateNewSection());
		if (Section)
		{
			TransformTrack->AddSection(*Section);
		}
	}
	if (!Section)
	{
		UAL_CommandUtils::SendError(RequestId, 500,
			TEXT("建不出 Transform 段。相机绑定和轨道已经加进序列了，可以 Ctrl+Z 撤掉这一笔。"));
		return;
	}
	// 没有 RF_Transactional 的话 Modify() 什么都不记，事务是空的，Ctrl+Z 一声不吭。
	// 但**只在真的缺这个标记时才补**：它属于 RF_Load，会被存进包里，而 SetFlags
	// 本身不进事务、撤不回来 —— 无条件写会改到用户的资产，并在他下次保存时落盘。
	// 同样的判断在 UAL_MaterialCommands 里有更长的说明
	if (!Section->HasAnyFlags(RF_Transactional))
	{
		Section->SetFlags(RF_Transactional);
	}
	Section->Modify();

	// 9 条 double 通道：0-2 位移、3-5 旋转、6-8 缩放。这是引擎固定的排布。
	//
	// 5.0 起变换通道就是 double（LWC），不是 float —— 九个版本一致，本机逐版本验过。
	// 已有的段在上面那段先探里验过了；这里兜住「新建的段也不对」的情况
	TArrayView<FMovieSceneDoubleChannel*> Channels =
		Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
	if (Channels.Num() < 6)
	{
		UAL_CommandUtils::SendError(RequestId, 500,
			FString::Printf(
				TEXT("Transform 段只有 %d 条 double 通道，预期至少 6 条。")
				TEXT("相机绑定和轨道已经加进序列了，可以 Ctrl+Z 撤掉这一笔。"),
				Channels.Num()));
		return;
	}

	int32 ReplacedKeys = 0;
	if (bReplaceExisting)
	{
		// 覆盖用户的东西要报数 —— 危不危险由用户在审批环节判断，
		// 工具的责任是把代价说清楚
		for (int32 Index = 0; Index < 6; ++Index)
		{
			ReplacedKeys += Channels[Index]->GetNumKeys();
			Channels[Index]->Reset();
		}
	}

	// ── 写键 ──────────────────────────────────────────────────────────────
	int32 WrittenKeys = 0;
	int32 MinFrame = MAX_int32;
	int32 MaxFrame = MIN_int32;

	// 坏键在「先探」里已经筛掉了，这里每个键至少写一组通道，MinFrame 一定会被改写
	for (const FParsedCameraKey& Key : ParsedKeys)
	{
		const FFrameNumber Tick = DisplayToTick(MovieScene, Key.DisplayFrame);
		MinFrame = FMath::Min(MinFrame, Key.DisplayFrame);
		MaxFrame = FMath::Max(MaxFrame, Key.DisplayFrame);

		if (Key.bHasLocation)
		{
			AddDoubleKey(Channels[0], Tick, Key.Location.X, Interpolation);
			AddDoubleKey(Channels[1], Tick, Key.Location.Y, Interpolation);
			AddDoubleKey(Channels[2], Tick, Key.Location.Z, Interpolation);
		}
		if (Key.bHasRotation)
		{
			AddDoubleKey(Channels[3], Tick, Key.Rotation.Roll, Interpolation);
			AddDoubleKey(Channels[4], Tick, Key.Rotation.Pitch, Interpolation);
			AddDoubleKey(Channels[5], Tick, Key.Rotation.Yaw, Interpolation);
		}
		++WrittenKeys;
	}

	// 回执里的键数从通道里读回来，不拿循环计数冒充（AGENTS.md §5 第 14 条）。
	// 六条通道上出现过的不同时刻就是这条轨道现在的关键帧数；
	// replace_existing_keys=false 时会把原有的键一起算进去，这正是轨道的真实状态
	TSet<int32> KeyTicks;
	for (int32 Index = 0; Index < 6; ++Index)
	{
		for (const FFrameNumber& Time : Channels[Index]->GetTimes())
		{
			KeyTicks.Add(Time.Value);
		}
	}
	const int32 TrackKeyCount = KeyTicks.Num();

	// ── 播放范围与段范围 ──────────────────────────────────────────────────
	double PlaybackEndRaw = 0.0;
	const int32 PlaybackEnd = Payload->TryGetNumberField(TEXT("playback_end_frame"), PlaybackEndRaw)
		? static_cast<int32>(PlaybackEndRaw)
		: MaxFrame + 1;
	const int32 PlaybackStart = FMath::Min(MinFrame, 0);

	const TRange<FFrameNumber> Range(
		DisplayToTick(MovieScene, PlaybackStart), DisplayToTick(MovieScene, PlaybackEnd));
	MovieScene->SetPlaybackRange(Range);
	Section->SetRange(Range);

	// ── 相机切轨 ──────────────────────────────────────────────────────────
	//
	// 和 sequence.camera_cuts 同一条规矩：切轨上已经有别人排好的段时，
	// **不默认删**。默认删的后果是一条多机位序列被这个工具顺手抹成单机位，
	// 而用户要的只是「给这台相机打一串关键帧」。见 red-lines.md 第 1、3 条。
	bool bCameraCutBound = false;
	bool bKeptExistingCuts = false;
	int32 RemovedCutSections = 0;
	if (bWantCameraCuts)
	{
		UMovieSceneCameraCutTrack* CutTrack = EnsureCameraCutTrack(MovieScene);
		const int32 ExistingCuts = CutTrack ? CutTrack->GetAllSections().Num() : 0;

		if (CutTrack && ExistingCuts > 0 && !bRebuildCameraCuts)
		{
			bKeptExistingCuts = true;
			// 关键帧照写 —— 那是用户要的。只是不动他的剪辑
			Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("相机切轨上已经有 %d 个段，没有动它们，所以这台相机还没有被切进画面。")
				TEXT("要让它从头盖到尾（会删掉那 %d 个段），带上 rebuild_camera_cuts=true 再调一次；")
				TEXT("或者在 Sequencer 里手动把某一段指到「%s」。"),
				ExistingCuts, ExistingCuts, *CameraLabel)));
		}
		else if (CutTrack)
		{
			// 走到这里只有两种可能：切轨本来是空的，或者用户显式点名要重建。
			// 叠加而不是重建的话，会留下旧相机的切段，渲出来在切点跳到别的机位
			CutTrack->Modify();
			TArray<UMovieSceneSection*> OldSections = CutTrack->GetAllSections();
			for (UMovieSceneSection* Old : OldSections)
			{
				CutTrack->RemoveSection(*Old);
				++RemovedCutSections;
			}
			if (UMovieSceneCameraCutSection* CutSection = CutTrack->AddNewCameraCut(
					UE::MovieScene::FRelativeObjectBindingID(CameraGuid), Range.GetLowerBoundValue()))
			{
				CutSection->SetRange(Range);
				bCameraCutBound = true;
			}
			if (RemovedCutSections > 0)
			{
				Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
					TEXT("原有的 %d 个切轨段已被删除，换成了「%s」从头盖到尾的一整段。"),
					RemovedCutSections, *CameraLabel)));
			}
		}

		// 「没建成」和「有意没动」是两回事，混成一句话用户没法判断下一步
		if (!bCameraCutBound && !bKeptExistingCuts)
		{
			Warnings.Add(MakeShared<FJsonValueString>(TEXT("相机切轨没建成，现在渲出来是黑的")));
		}
	}

	// ── 存盘 ──────────────────────────────────────────────────────────────
	//
	// 不存盘用户关掉编辑器就全丢了，而模型以为自己成功了。
	// 关卡先存 —— 万一后面崩了，至少不会带走新建的相机。
	//
	// 先把事务结束掉：存盘不是可撤销操作，而 SaveCurrentLevel() 还会走一轮 GC。
	Transaction.Reset();

	bool bLevelSaved = true;
	if (bCameraCreated)
	{
		bLevelSaved = FEditorFileUtils::SaveCurrentLevel();
		if (!bLevelSaved)
		{
			Warnings.Add(MakeShared<FJsonValueString>(TEXT("关卡没保存成功，新建的相机在重启编辑器后会丢失")));
		}
		else
		{
			// 红线第 4 条：不许产生序列之外的副作用，确实要产生就得明说。
			// SaveCurrentLevel() 保存的是整个当前关卡，用户手上别的未保存改动
			// 会一起落盘 —— 引擎没有「只存这一个 Actor」的接口
			Warnings.Add(MakeShared<FJsonValueString>(
				TEXT("为了让新建的相机留得住，整个当前关卡被保存了一次 —— ")
				TEXT("你在这个关卡里别的未保存改动也一起落盘了。")));
		}
	}

	// 存盘结果原来直接丢掉，模型拿到的回执和存成功时一模一样
	const bool bSequenceSaved = SaveSequenceAsset(Sequence);
	if (!bSequenceSaved)
	{
		Warnings.Add(MakeShared<FJsonValueString>(
			TEXT("序列资产没保存成功，关键帧只在编辑器内存里 —— 关掉编辑器前要手动保存这条序列，否则会丢。")));
	}

	// ── 回传 ──────────────────────────────────────────────────────────────
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("sequence_path"), SequencePath);
	Data->SetStringField(TEXT("camera_label"), CameraLabel);
	Data->SetBoolField(TEXT("camera_created"), bCameraCreated);
	Data->SetBoolField(TEXT("sequence_created"), bSequenceCreated);
	// key_count 是从通道读回的轨道键数；written_keys 是这次真正写进去的请求键数
	Data->SetNumberField(TEXT("key_count"), TrackKeyCount);
	Data->SetNumberField(TEXT("written_keys"), WrittenKeys);
	Data->SetNumberField(TEXT("skipped_keys"), SkippedKeyReasons.Num());
	if (SkippedKeyReasons.Num() > 0)
	{
		Data->SetArrayField(TEXT("skipped_key_reasons"), SkippedKeyReasons);
	}
	Data->SetBoolField(TEXT("sequence_saved"), bSequenceSaved);
	Data->SetNumberField(TEXT("replaced_keys"), ReplacedKeys);
	Data->SetBoolField(TEXT("camera_cut_bound"), bCameraCutBound);
	Data->SetBoolField(TEXT("kept_existing_cuts"), bKeptExistingCuts);
	Data->SetNumberField(TEXT("removed_cut_sections"), RemovedCutSections);
	Data->SetBoolField(TEXT("level_saved"), bLevelSaved);

	TArray<TSharedPtr<FJsonValue>> RangeArray;
	RangeArray.Add(MakeShared<FJsonValueNumber>(PlaybackStart));
	RangeArray.Add(MakeShared<FJsonValueNumber>(PlaybackEnd));
	Data->SetArrayField(TEXT("range"), RangeArray);

	if (Warnings.Num() > 0)
	{
		Data->SetArrayField(TEXT("warnings"), Warnings);
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// sequence.camera_cuts
// ============================================================================

void FUAL_SequencerCommands::Handle_CameraCuts(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString SequencePath;
	if (!Payload->TryGetStringField(TEXT("sequence_path"), SequencePath))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: sequence_path"));
		return;
	}

	FString CameraLabel;
	const bool bHasLabel = Payload->TryGetStringField(TEXT("camera_label"), CameraLabel) && !CameraLabel.IsEmpty();

	// 切轨上已经有内容时，把它们全删了重建要显式点名。默认拒绝，见下面的守卫
	bool bRebuild = false;
	Payload->TryGetBoolField(TEXT("rebuild"), bRebuild);

	FString Error;
	ULevelSequence* Sequence = LoadSequence(SequencePath, Error);
	if (!Sequence)
	{
		UAL_CommandUtils::SendError(RequestId, 404, Error);
		return;
	}
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	UWorld* World = GetSequencerResolveWorld();

	// ── 挑绑定 ────────────────────────────────────────────────────────────
	//
	// 点名了就按名字找；没点名且只有一条相机绑定就用它。
	// 有多条却没点名时**不猜** —— 选错机位比不选更糟，把候选列出来让人定。
	FGuid TargetGuid;
	FString TargetName;
	TArray<TSharedPtr<FJsonValue>> Candidates;

	for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
	{
		const FGuid& Guid = Binding.GetObjectGuid();
		EUALBindingKind Kind = EUALBindingKind::Unknown;
		const FString Name = ResolveBindingNameForBinding(MovieScene, Binding, Kind);

		if (bHasLabel)
		{
			if (Name == CameraLabel)
			{
				TargetGuid = Guid;
				TargetName = Name;
			}
			continue;
		}

		// 没点名：按类名或绑定名里带 Camera 认相机
		FString ClassName;
		if (const FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(Guid))
		{
			if (const UObject* Template = Spawnable->GetObjectTemplate())
			{
				ClassName = Template->GetClass()->GetName();
			}
		}
		else if (const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Guid))
		{
			if (const UClass* PossessedClass = Possessable->GetPossessedObjectClass())
			{
				ClassName = PossessedClass->GetName();
			}
		}
		if (ClassName.Contains(TEXT("Camera")) || Name.Contains(TEXT("Camera")))
		{
			Candidates.Add(MakeShared<FJsonValueString>(Name));
			if (!TargetGuid.IsValid())
			{
				TargetGuid = Guid;
				TargetName = Name;
			}
		}
	}

	if (!TargetGuid.IsValid())
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("candidates"), Candidates);
		UAL_CommandUtils::SendError(RequestId, 404,
			bHasLabel
				? FString::Printf(TEXT("序列里没有叫「%s」的绑定"), *CameraLabel)
				: TEXT("序列里找不到相机绑定，请用 camera_label 点名"),
			Details);
		return;
	}
	if (!bHasLabel && Candidates.Num() > 1)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("candidates"), Candidates);
		UAL_CommandUtils::SendError(RequestId, 409,
			FString::Printf(TEXT("序列里有 %d 条相机绑定，请用 camera_label 点名要切到哪一台"), Candidates.Num()),
			Details);
		return;
	}

	// 绑定断了的话，补了切轨照样是黑的 —— 要在结果里说清楚，别让用户以为修好了
	bool bBindingBroken = false;
	{
		TArray<UObject*, TInlineAllocator<1>> Bound;
		if (ResolveBindingObjects(Sequence, MovieScene, TargetGuid, World, Bound) && Bound.Num() == 0)
		{
			bBindingBroken = true;
		}
	}

	// ── 先把不该动手的情况挡掉 ────────────────────────────────────────────
	const TRange<FFrameNumber> Playback = MovieScene->GetPlaybackRange();
	int32 PlaybackStart = 0, PlaybackEnd = 0;
	PlaybackInDisplayFrames(MovieScene, PlaybackStart, PlaybackEnd);

	// 空的播放范围上建切段，建出来的是个零长度的段，一帧都盖不到 ——
	// 而且原来的代码会因此判定「没盖满」，顺手把已有的段全删了。
	// 引擎在 UpgradeTimeRanges() 里会把损坏或无界的范围强制成 [0,0)，真会遇到。
	//
	// 两端是否有界要单独查：下面 AddNewCameraCut 要拿 GetLowerBoundValue()，
	// 在无界的那一头上取值是未定义的。UpgradeTimeRanges 保证加载进来的范围
	// 两端都有界，但那是个不该默默依赖的不变量
	if (!Playback.GetLowerBound().IsClosed() || !Playback.GetUpperBound().IsClosed()
		|| PlaybackEnd <= PlaybackStart)
	{
		UAL_CommandUtils::SendError(RequestId, 409,
			FString::Printf(
				TEXT("这条序列的播放范围用不了（算出来是 [%d, %d)，空的或者有一头无界）。")
				TEXT("先在 Sequencer 里把播放范围拉出明确的长度再来补切轨 —— ")
				TEXT("往这种范围上建的切段一帧都盖不到。什么都没有改动。"),
				PlaybackStart, PlaybackEnd));
		return;
	}

	const FCameraCutInfo Before = InspectCameraCuts(MovieScene, PlaybackStart, PlaybackEnd);
	const UAL_CameraCutCoverage::FCutCoverage& Coverage = Before.Coverage;

	// 判不出覆盖就什么都不做。把「判不出来」当成「没盖满」，
	// 等于拿一个不确定的结论去删用户的东西
	if (Before.bExists && !Coverage.bCoverageKnown)
	{
		UAL_CommandUtils::SendError(RequestId, 409,
			FString::Printf(TEXT("这条序列的切轨覆盖情况判不出来：%s。什么都没有改动。"),
				*Coverage.UnknownReason));
		return;
	}

	const bool bAlreadyCovered = Before.bExists && Before.SectionCount > 0 && Coverage.bCoversPlayback;

	// 切轨上已经有段、但没盖满 —— 这里原来会把已有的段**全删了重建**成一整段。
	// 一条排好的三机位序列只要有一帧对不齐，调一次这个工具就只剩一台相机。
	// 现在写入有事务了（Ctrl+Z 撤得回内存里的状态），但这一笔紧接着就存盘 ——
	// 磁盘上那一版已经换掉了，不是白做一场的事。
	//
	// `red-lines.md` 第 1 条「发现问题只报告，不动手」、第 3 条「删除永远是
	// 用户手动做的」—— 所以默认拒绝，要删必须显式点名 rebuild=true。
	if (!bAlreadyCovered && Before.SectionCount > 0 && !bRebuild)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetObjectField(TEXT("camera_cuts"), MakeCameraCutsJson(Before));
		UAL_CommandUtils::SendError(RequestId, 409,
			FString::Printf(
				TEXT("相机切轨上已经有 %d 个段，没盖满播放范围。这个工具补全的做法是把它们")
				TEXT("**全部删掉**，重建成「%s」从头盖到尾的一整段 —— 如果那是排好的多机位剪辑，")
				TEXT("这一下就没了，而且会立刻存盘落到磁盘上。\n\n")
				TEXT("确认要这么做：带上 rebuild=true 再调一次。\n")
				TEXT("只想知道差在哪里：用 sequence_audit，它会逐段报出空隙和重叠。"),
				Before.SectionCount, *TargetName),
			Details);
		return;
	}

	// ══ 从这里开始动手 ════════════════════════════════════════════════════
	//
	// 上面全是只读判断，走到这里说明该拒的都拒完了。事务包住整笔写入，
	// 让用户一次 Ctrl+Z 撤得干净 —— 这个 handler 原来一个事务都没有，
	// 删掉的切轨段是彻底撤不回来的。
	//
	// 中途失败时**不 Cancel**：`UTransBuffer::Cancel` 只把记录 Pop 掉，
	// 不调 `FTransaction::Apply()`，改动照样留在原地，反而连撤都撤不了。
	TArray<TSharedPtr<FJsonValue>> Warnings;
	int32 RemovedSections = 0;
	// 本来就盖满时什么都没改、也没存，不发这个字段
	bool bSequenceSaved = false;
	if (!bAlreadyCovered)
	{
		// 事务开在自己的作用域里，存盘留在它外面 —— 存盘本身不可撤销，
		// 夹在事务里只会把无关的东西卷进这条撤销记录
		{
			FUAL_ScopedTransaction Transaction(
				NSLOCTEXT("UALSequencer", "CameraCuts", "Bind Camera Cut Track"));

			MovieScene->Modify();
			UMovieSceneCameraCutTrack* CutTrack = EnsureCameraCutTrack(MovieScene);
			if (!CutTrack)
			{
				Transaction.Cancel(); // 还没改过任何东西，这时取消是安全的
				UAL_CommandUtils::SendError(RequestId, 500, TEXT("建不出相机切轨"));
				return;
			}
			CutTrack->Modify();

			// 走到这里只有两种可能：切轨本来就是空的，或者用户显式给了 rebuild=true。
			// 补空隙而不是重建，会在切点跳到别的机位，那不是「用这台相机」的意思
			TArray<UMovieSceneSection*> OldSections = CutTrack->GetAllSections();
			for (UMovieSceneSection* Old : OldSections)
			{
				CutTrack->RemoveSection(*Old);
				++RemovedSections;
			}

			UMovieSceneCameraCutSection* CutSection = CutTrack->AddNewCameraCut(
				UE::MovieScene::FRelativeObjectBindingID(TargetGuid), Playback.GetLowerBoundValue());
			if (!CutSection)
			{
				UAL_CommandUtils::SendError(RequestId, 500, FString::Printf(
					TEXT("切轨段建不出来。%s序列还没有存盘，Ctrl+Z 可以撤回。"),
					RemovedSections > 0
						? *FString::Printf(TEXT("原有的 %d 个切轨段已经被删掉了，"), RemovedSections)
						: TEXT("")));
				return;
			}
			CutSection->SetRange(Playback);
		}

		// 存盘结果原来直接丢掉，TS 侧照样说「序列已存盘」
		bSequenceSaved = SaveSequenceAsset(Sequence);
		if (!bSequenceSaved)
		{
			Warnings.Add(MakeShared<FJsonValueString>(
				TEXT("序列资产没保存成功，切轨只在编辑器内存里 —— 关掉编辑器前要手动保存这条序列，否则会丢。")));
		}
	}

	// 删了用户的东西必须报数，而且要报在结果里而不是只写日志
	if (RemovedSections > 0)
	{
		Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
			TEXT("原有的 %d 个切轨段已被删除，换成了「%s」从头盖到尾的一整段。"),
			RemovedSections, *TargetName)));
	}

	if (bBindingBroken)
	{
		Warnings.Add(MakeShared<FJsonValueString>(
			TEXT("这条绑定解析不到关卡里的对象。切轨已经补上，但渲出来仍然会是黑的，要先手动重绑。")));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("sequence_path"), SequencePath);
	Data->SetStringField(TEXT("camera_binding"), TargetName);
	Data->SetBoolField(TEXT("already_covered"), bAlreadyCovered);
	Data->SetBoolField(TEXT("binding_broken"), bBindingBroken);
	Data->SetNumberField(TEXT("removed_sections"), RemovedSections);
	if (!bAlreadyCovered)
	{
		Data->SetBoolField(TEXT("sequence_saved"), bSequenceSaved);
	}

	TArray<TSharedPtr<FJsonValue>> RangeArray;
	RangeArray.Add(MakeShared<FJsonValueNumber>(PlaybackStart));
	RangeArray.Add(MakeShared<FJsonValueNumber>(PlaybackEnd));
	Data->SetArrayField(TEXT("range"), RangeArray);

	if (Candidates.Num() > 0)
	{
		Data->SetArrayField(TEXT("candidates"), Candidates);
	}
	if (Warnings.Num() > 0)
	{
		Data->SetArrayField(TEXT("warnings"), Warnings);
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}
