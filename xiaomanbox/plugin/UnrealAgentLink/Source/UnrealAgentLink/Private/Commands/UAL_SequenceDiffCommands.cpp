#include "UAL_SequenceDiffCommands.h"

#include "UAL_CommandUtils.h"
#include "UAL_VersionCompat.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneTrack.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneByteChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneIntegerChannel.h"
#include "Sections/MovieSceneSubSection.h"
#include "Tracks/MovieSceneMaterialTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"

/**
 * `sequence.diff` —— 两条 Level Sequence 的语义对比，只读。
 *
 * ## 为什么要有
 *
 * 2026-09-24 买量定序器反馈里两处卡在同一件事上：
 *
 * 1. **受保护的原序列**：早期备份的 SHA256 和当前磁盘文件对不上
 *    （`UNCHANGED False`）。哈希只能说「字节不同」，说不出是重存了元数据，
 *    还是人手 K 的曲线被动了 —— 于是既不敢说「原版没动」，也不敢拿备份覆盖。
 * 2. **风格迁移的覆盖**：旧角色（隐藏的驱动）身上有描边 Stencil、覆层材质、
 *    材质槽 7 和槽 1 的溶解曲线；新换上的显示角色只接了动画。结构体检（audit）
 *    PASS，因为那查的是「能不能渲」，不是「原来有的状态新角色上还在不在」。
 *    这些是多轮专项查漏才拼出来的。
 *
 * 两件事是同一个比较，只是对齐方式不同，所以一个命令两种模式：
 *
 * - **diff**（不给 binding_map）：两条序列按绑定 GUID 对齐（复制出来的副本保留 GUID），
 *   对不上的再按「名字 + 父级名字」唯一匹配。报帧率、Tick 分辨率、播放范围、
 *   根轨道（相机切轨、子序列引用）和每个绑定的轨道、段、关键帧**数值与切线**的差异。
 *   人手 K 的曲线改没改，看的就是这一层。
 * - **coverage**（给 binding_map）：按调用方给的「源绑定 → 目标绑定」逐对比较，
 *   把每个绑定连同它的组件子绑定合成一组，逐条轨道判
 *   已继承 / 已适配（有这条轨道但内容不同）/ 缺失；目标绑定找不到就是「无法判断」——
 *   状态可能由运行时逻辑接管，序列里看不出来，不能说成缺失。
 *
 * ## 边界
 *
 * - 只比序列资产本身。子序列只比「引用的是哪一条」，不递归进去 —— 要比子镜头就对子序列再调一次。
 * - 不比 possessable 在关卡里绑的是哪个 Actor（那是关卡数据，不在序列资产里）。
 * - 两条序列 Tick 分辨率不同时，切线的数值单位不同，不比切线，回执里说明。
 * - 时间一律先换成秒再比，所以 30fps 的参考和 60fps 的目标也能对齐；报出来的帧号是各自序列的 display 帧。
 */
namespace UALSequenceDiff
{
	using FJson = TSharedPtr<FJsonObject>;
	using FValues = TArray<TSharedPtr<FJsonValue>>;

	/** 单条轨道最多列几条差异细节，其余只报数 —— 一条 4000 帧的曲线全改了，列 4000 行没有意义 */
	constexpr int32 MaxDetailsPerTrack = 6;
	/** 回执里最多列多少个有差异的绑定 */
	constexpr int32 MaxBindingEntries = 150;

	// ========================================================================
	// 签名：把轨道读成可比较的纯数据
	// ========================================================================

	struct FKeySig
	{
		double Seconds = 0;
		bool bHasValue = false;
		double Value = 0;
		bool bCurve = false;
		int32 Interp = 0, TangentMode = 0, WeightMode = 0;
		double Arrive = 0, Leave = 0, ArriveWeight = 0, LeaveWeight = 0;
	};

	struct FChannelSig
	{
		FString Name;
		FString Type;
		TArray<FKeySig> Keys;
		bool bHasDefault = false;
		double Default = 0;
	};

	struct FSectionSig
	{
		bool bHasStart = false, bHasEnd = false;
		double StartSec = 0, EndSec = 0;
		int32 StartFrame = 0, EndFrame = 0;
		int32 Row = 0;
		bool bActive = true;
		FString SubSequence;
		TArray<FChannelSig> Channels;
	};

	struct FTrackSig
	{
		/** 对齐用：类名 + 身份（属性路径 / 材质槽 / 显示名），同名的按出现顺序加 #n */
		FString Key;
		/** 给人看的 */
		FString Label;
		TArray<FSectionSig> Sections;
	};

	struct FSeq
	{
		ULevelSequence* Sequence = nullptr;
		UMovieScene* MovieScene = nullptr;
		FFrameRate Display, Tick;

		int32 ToDisplay(const FFrameNumber& T) const
		{
			return FFrameRate::TransformTime(FFrameTime(T), Tick, Display).FloorToFrame().Value;
		}
		double ToSeconds(const FFrameNumber& T) const { return Tick.AsSeconds(FFrameTime(T)); }
		int32 SecondsToDisplay(double Seconds) const { return Display.AsFrameTime(Seconds).FloorToFrame().Value; }
	};

	struct FBindingInfo
	{
		FGuid Guid;
		FGuid Parent;
		FString Name;
		FString Kind;
		FString Template;
		const FMovieSceneBinding* Binding = nullptr;
	};

	ULevelSequence* LoadDiffSequence(const FString& Path, FString& OutError)
	{
		if (Path.IsEmpty()) { OutError = TEXT("序列路径是空的"); return nullptr; }
		UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
		ULevelSequence* Sequence = Cast<ULevelSequence>(Asset);
		if (!Asset) { OutError = FString::Printf(TEXT("找不到资产：%s（只能比内容浏览器里的资产；磁盘上的备份文件要先放进工程再比）"), *Path); return nullptr; }
		if (!Sequence) { OutError = FString::Printf(TEXT("%s 不是 Level Sequence，实际是 %s"), *Path, *Asset->GetClass()->GetName()); return nullptr; }
		if (!Sequence->GetMovieScene()) { OutError = FString::Printf(TEXT("%s 没有 MovieScene（资产可能损坏）"), *Path); return nullptr; }
		return Sequence;
	}

	// 材质轨道的槽位身份：5.4 起是 FComponentMaterialInfo（槽名 / 槽号 / 覆层），之前只有槽号。
	// 用重载决议探测能力而不是版本号分支，理由见 UAL_VersionCompat.h
	template <typename TrackType>
	auto MaterialSlotKey(const TrackType* Track, int) -> decltype(Track->GetMaterialInfo().ToString())
	{
		return Track->GetMaterialInfo().ToString();
	}
	template <typename TrackType>
	auto MaterialSlotKey(const TrackType* Track, long) -> decltype(FString::FromInt(Track->GetMaterialIndex()))
	{
		return FString::Printf(TEXT("Material Element %d"), Track->GetMaterialIndex());
	}

	FString TrackIdentity(const UMovieSceneTrack* Track)
	{
		// 材质槽必须进身份：首 Boss 的溶解在槽 7 和槽 1 上，只比「有一条材质轨」看不出换错了槽
		if (const UMovieSceneComponentMaterialTrack* Material = Cast<UMovieSceneComponentMaterialTrack>(Track))
		{
			return MaterialSlotKey(Material, 0);
		}
		// 属性轨按属性路径认：CustomDepthStencilValue、OverlayMaterial、bHidden 这类状态就藏在这里
		if (const UMovieScenePropertyTrack* Property = Cast<UMovieScenePropertyTrack>(Track))
		{
			const FName Path = Property->GetPropertyPath();
			if (!Path.IsNone()) { return Path.ToString(); }
		}
#if WITH_EDITORONLY_DATA
		return Track->GetDisplayName().ToString();
#else
		return Track->GetName();
#endif
	}

	template <typename ChannelType>
	void ReadCurve(const ChannelType* Channel, const FSeq& Seq, FChannelSig& Out)
	{
		const auto Times = Channel->GetTimes();
		const auto Values = Channel->GetValues();
		for (int32 I = 0; I < Times.Num() && I < Values.Num(); ++I)
		{
			FKeySig Key;
			Key.Seconds = Seq.ToSeconds(Times[I]);
			Key.bHasValue = true;
			Key.bCurve = true;
			Key.Value = Values[I].Value;
			Key.Interp = static_cast<int32>(Values[I].InterpMode.GetValue());
			Key.TangentMode = static_cast<int32>(Values[I].TangentMode.GetValue());
			Key.WeightMode = static_cast<int32>(Values[I].Tangent.TangentWeightMode.GetValue());
			Key.Arrive = Values[I].Tangent.ArriveTangent;
			Key.Leave = Values[I].Tangent.LeaveTangent;
			Key.ArriveWeight = Values[I].Tangent.ArriveTangentWeight;
			Key.LeaveWeight = Values[I].Tangent.LeaveTangentWeight;
			Out.Keys.Add(Key);
		}
		const auto Default = Channel->GetDefault();
		if (Default.IsSet()) { Out.bHasDefault = true; Out.Default = Default.GetValue(); }
	}

	template <typename ChannelType>
	void ReadDiscrete(const ChannelType* Channel, const FSeq& Seq, FChannelSig& Out)
	{
		const auto Times = Channel->GetTimes();
		const auto Values = Channel->GetValues();
		for (int32 I = 0; I < Times.Num() && I < Values.Num(); ++I)
		{
			FKeySig Key;
			Key.Seconds = Seq.ToSeconds(Times[I]);
			Key.bHasValue = true;
			Key.Value = static_cast<double>(Values[I]);
			Out.Keys.Add(Key);
		}
		const auto Default = Channel->GetDefault();
		if (Default.IsSet()) { Out.bHasDefault = true; Out.Default = static_cast<double>(Default.GetValue()); }
	}

	FSectionSig ReadSection(UMovieSceneSection* Section, const FSeq& Seq)
	{
		FSectionSig Sig;
		const TRange<FFrameNumber> Range = Section->GetRange();
		if (Range.GetLowerBound().IsClosed())
		{
			Sig.bHasStart = true;
			Sig.StartSec = Seq.ToSeconds(Range.GetLowerBoundValue());
			Sig.StartFrame = Seq.ToDisplay(Range.GetLowerBoundValue());
		}
		if (Range.GetUpperBound().IsClosed())
		{
			Sig.bHasEnd = true;
			Sig.EndSec = Seq.ToSeconds(Range.GetUpperBoundValue());
			Sig.EndFrame = Seq.ToDisplay(Range.GetUpperBoundValue());
		}
		Sig.Row = Section->GetRowIndex();
		Sig.bActive = Section->IsActive();
		// 总序列「改为引用风格副本」就体现在这里：子序列段引用的是哪一条
		if (const UMovieSceneSubSection* Sub = Cast<UMovieSceneSubSection>(Section))
		{
			Sig.SubSequence = Sub->GetSequence() ? Sub->GetSequence()->GetPathName() : TEXT("(空)");
		}

		FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
		for (const FMovieSceneChannelEntry& Entry : Proxy.GetAllEntries())
		{
			const FName Type = Entry.GetChannelTypeName();
			TArrayView<FMovieSceneChannel* const> Channels = Entry.GetChannels();
#if WITH_EDITORONLY_DATA
			TArrayView<const FMovieSceneChannelMetaData> MetaData = Entry.GetMetaData();
#endif
			for (int32 Index = 0; Index < Channels.Num(); ++Index)
			{
				FMovieSceneChannel* Channel = Channels[Index];
				if (!Channel) { continue; }
				FChannelSig Out;
				Out.Type = Type.ToString();
#if WITH_EDITORONLY_DATA
				if (MetaData.IsValidIndex(Index)) { Out.Name = MetaData[Index].Name.ToString(); }
#endif
				if (Out.Name.IsEmpty()) { Out.Name = FString::Printf(TEXT("%s[%d]"), *Out.Type, Index); }

				if (Type == FMovieSceneFloatChannel::StaticStruct()->GetFName())
					ReadCurve(static_cast<const FMovieSceneFloatChannel*>(Channel), Seq, Out);
				else if (Type == FMovieSceneDoubleChannel::StaticStruct()->GetFName())
					ReadCurve(static_cast<const FMovieSceneDoubleChannel*>(Channel), Seq, Out);
				else if (Type == FMovieSceneBoolChannel::StaticStruct()->GetFName())
					ReadDiscrete(static_cast<const FMovieSceneBoolChannel*>(Channel), Seq, Out);
				else if (Type == FMovieSceneByteChannel::StaticStruct()->GetFName())
					ReadDiscrete(static_cast<const FMovieSceneByteChannel*>(Channel), Seq, Out);
				else if (Type == FMovieSceneIntegerChannel::StaticStruct()->GetFName())
					ReadDiscrete(static_cast<const FMovieSceneIntegerChannel*>(Channel), Seq, Out);
				else
				{
					// 认不出的通道：只比帧位置，不比值。报一个错的值比不报糟
					TArray<FFrameNumber> Times;
					Channel->GetKeys(TRange<FFrameNumber>::All(), &Times, nullptr);
					for (const FFrameNumber& Time : Times)
					{
						FKeySig Key;
						Key.Seconds = Seq.ToSeconds(Time);
						Out.Keys.Add(Key);
					}
				}
				Sig.Channels.Add(MoveTemp(Out));
			}
		}
		return Sig;
	}

	TArray<FTrackSig> ReadTracks(const TArray<UMovieSceneTrack*>& Tracks, const FSeq& Seq)
	{
		TArray<FTrackSig> Out;
		TMap<FString, int32> Seen;
		for (UMovieSceneTrack* Track : Tracks)
		{
			if (!Track) { continue; }
			FTrackSig Sig;
			const FString Identity = TrackIdentity(Track);
			const FString ClassName = Track->GetClass()->GetName();
			const FString Base = ClassName + TEXT("|") + Identity;
			int32& Count = Seen.FindOrAdd(Base);
			++Count;
			Sig.Key = Count > 1 ? FString::Printf(TEXT("%s#%d"), *Base, Count) : Base;
			Sig.Label = FString::Printf(TEXT("%s（%s）%s"), *Identity, *ClassName.Replace(TEXT("MovieScene"), TEXT("")),
				Count > 1 ? *FString::Printf(TEXT(" #%d"), Count) : TEXT(""));
			for (UMovieSceneSection* Section : Track->GetAllSections())
			{
				if (Section) { Sig.Sections.Add(ReadSection(Section, Seq)); }
			}
			Out.Add(MoveTemp(Sig));
		}
		return Out;
	}

	// ========================================================================
	// 比较
	// ========================================================================

	bool Near(double A, double B, double Tolerance = 1e-4)
	{
		return FMath::Abs(A - B) <= Tolerance * FMath::Max(1.0, FMath::Max(FMath::Abs(A), FMath::Abs(B)));
	}

	FString RangeText(const FSectionSig& S)
	{
		return FString::Printf(TEXT("[%s, %s)"),
			S.bHasStart ? *FString::FromInt(S.StartFrame) : TEXT("−∞"),
			S.bHasEnd ? *FString::FromInt(S.EndFrame) : TEXT("+∞"));
	}

	FString FormatNum(double V) { return FString::SanitizeFloat(V, 0); }

	/**
	 * 两条轨道逐段逐通道比。返回有没有差异，差异细节写进 Details（最多 MaxDetailsPerTrack 条）。
	 *
	 * `bCompareTangents` 为 false 时（两边 Tick 分辨率不同）跳过切线：切线的单位是「值 / tick」，
	 * 分辨率不同数值就不可比，硬比会把没动过的曲线报成改了。
	 */
	bool CompareTracks(const FTrackSig& A, const FTrackSig& B, const FSeq& SA, const FSeq& SB, bool bCompareTangents, TArray<FString>& Details)
	{
		int32 Differences = 0;
		auto Add = [&Details, &Differences](const FString& Line)
		{
			++Differences;
			if (Details.Num() < MaxDetailsPerTrack) { Details.Add(Line); }
		};

		if (A.Sections.Num() != B.Sections.Num())
		{
			Add(FString::Printf(TEXT("段数 %d → %d"), A.Sections.Num(), B.Sections.Num()));
		}
		for (int32 S = 0; S < FMath::Min(A.Sections.Num(), B.Sections.Num()); ++S)
		{
			const FSectionSig& X = A.Sections[S];
			const FSectionSig& Y = B.Sections[S];
			const FString Where = A.Sections.Num() > 1 ? FString::Printf(TEXT("第 %d 段"), S + 1) : TEXT("段");
			if (X.bHasStart != Y.bHasStart || X.bHasEnd != Y.bHasEnd ||
				(X.bHasStart && !Near(X.StartSec, Y.StartSec, 1e-6)) || (X.bHasEnd && !Near(X.EndSec, Y.EndSec, 1e-6)))
			{
				Add(FString::Printf(TEXT("%s范围 %s → %s"), *Where, *RangeText(X), *RangeText(Y)));
			}
			if (X.bActive != Y.bActive) { Add(FString::Printf(TEXT("%s%s"), *Where, Y.bActive ? TEXT("被启用") : TEXT("被停用"))); }
			if (X.SubSequence != Y.SubSequence) { Add(FString::Printf(TEXT("%s引用的子序列 %s → %s"), *Where, *X.SubSequence, *Y.SubSequence)); }

			// 通道按名字对齐；名字对不上时退到同位置同类型（编辑器外读不到元数据时名字是 类型[序号]）
			TSet<int32> MatchedY;
			for (int32 C = 0; C < X.Channels.Num(); ++C)
			{
				const FChannelSig& CX = X.Channels[C];
				int32 YIndex = Y.Channels.IndexOfByPredicate([&CX](const FChannelSig& O) { return O.Name == CX.Name; });
				if (YIndex == INDEX_NONE && Y.Channels.IsValidIndex(C) && Y.Channels[C].Type == CX.Type && !MatchedY.Contains(C)) { YIndex = C; }
				if (YIndex == INDEX_NONE) { Add(FString::Printf(TEXT("%s通道 %s 不见了"), *Where, *CX.Name)); continue; }
				MatchedY.Add(YIndex);
				const FChannelSig* CY = &Y.Channels[YIndex];

				if (CX.bHasDefault != CY->bHasDefault || (CX.bHasDefault && !Near(CX.Default, CY->Default)))
				{
					Add(FString::Printf(TEXT("通道 %s 的默认值 %s → %s"), *CX.Name,
						CX.bHasDefault ? *FormatNum(CX.Default) : TEXT("无"), CY->bHasDefault ? *FormatNum(CY->Default) : TEXT("无")));
				}
				if (CX.Keys.Num() != CY->Keys.Num())
				{
					Add(FString::Printf(TEXT("通道 %s 关键帧 %d → %d 个"), *CX.Name, CX.Keys.Num(), CY->Keys.Num()));
					continue;
				}
				int32 Moved = 0, Valued = 0, Tangents = 0;
				int32 FirstMoved = -1, FirstValued = -1, FirstTangent = -1;
				for (int32 K = 0; K < CX.Keys.Num(); ++K)
				{
					const FKeySig& KX = CX.Keys[K];
					const FKeySig& KY = CY->Keys[K];
					if (!Near(KX.Seconds, KY.Seconds, 1e-6)) { if (Moved++ == 0) FirstMoved = K; }
					if (KX.bHasValue && KY.bHasValue && !Near(KX.Value, KY.Value)) { if (Valued++ == 0) FirstValued = K; }
					if (KX.bCurve && KY.bCurve && (KX.Interp != KY.Interp || KX.TangentMode != KY.TangentMode ||
						(bCompareTangents && (KX.WeightMode != KY.WeightMode || !Near(KX.Arrive, KY.Arrive) || !Near(KX.Leave, KY.Leave) ||
							!Near(KX.ArriveWeight, KY.ArriveWeight) || !Near(KX.LeaveWeight, KY.LeaveWeight)))))
					{
						if (Tangents++ == 0) FirstTangent = K;
					}
				}
				if (Moved > 0)
				{
					Add(FString::Printf(TEXT("通道 %s：%d 个关键帧挪了位置（首个第 %d 帧 → 第 %d 帧）"), *CX.Name, Moved,
						SA.SecondsToDisplay(CX.Keys[FirstMoved].Seconds), SB.SecondsToDisplay(CY->Keys[FirstMoved].Seconds)));
				}
				if (Valued > 0)
				{
					Add(FString::Printf(TEXT("通道 %s：%d 个关键帧的值变了（首个在第 %d 帧：%s → %s）"), *CX.Name, Valued,
						SA.SecondsToDisplay(CX.Keys[FirstValued].Seconds), *FormatNum(CX.Keys[FirstValued].Value), *FormatNum(CY->Keys[FirstValued].Value)));
				}
				if (Tangents > 0)
				{
					Add(FString::Printf(TEXT("通道 %s：%d 个关键帧的插值或切线变了（首个在第 %d 帧）"), *CX.Name, Tangents,
						SA.SecondsToDisplay(CX.Keys[FirstTangent].Seconds)));
				}
			}
			for (int32 C = 0; C < Y.Channels.Num(); ++C)
			{
				if (!MatchedY.Contains(C)) { Add(FString::Printf(TEXT("%s多了通道 %s"), *Where, *Y.Channels[C].Name)); }
			}
		}
		if (Differences > Details.Num())
		{
			Details.Add(FString::Printf(TEXT("……另有 %d 处差异"), Differences - Details.Num()));
		}
		return Differences > 0;
	}

	FJson TrackEntry(const FString& Label, const TCHAR* Status, const TArray<FString>& Details)
	{
		FJson Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("track"), Label);
		Entry->SetStringField(TEXT("status"), Status);
		if (Details.Num() > 0)
		{
			FValues Lines;
			for (const FString& Line : Details) { Lines.Add(MakeShared<FJsonValueString>(Line)); }
			Entry->SetArrayField(TEXT("details"), Lines);
		}
		return Entry;
	}

	/** diff 模式：两组轨道按 Key 对齐，只回有差异的 */
	FValues DiffTrackSets(const TArray<FTrackSig>& A, const TArray<FTrackSig>& B, const FSeq& SA, const FSeq& SB, bool bTangents)
	{
		FValues Out;
		for (const FTrackSig& TA : A)
		{
			const FTrackSig* TB = B.FindByPredicate([&TA](const FTrackSig& O) { return O.Key == TA.Key; });
			if (!TB) { Out.Add(MakeShared<FJsonValueObject>(TrackEntry(TA.Label, TEXT("only_in_base"), {}))); continue; }
			TArray<FString> Details;
			if (CompareTracks(TA, *TB, SA, SB, bTangents, Details))
			{
				Out.Add(MakeShared<FJsonValueObject>(TrackEntry(TA.Label, TEXT("changed"), Details)));
			}
		}
		for (const FTrackSig& TB : B)
		{
			if (!A.ContainsByPredicate([&TB](const FTrackSig& O) { return O.Key == TB.Key; }))
			{
				Out.Add(MakeShared<FJsonValueObject>(TrackEntry(TB.Label, TEXT("only_in_compare"), {})));
			}
		}
		return Out;
	}

	// ========================================================================
	// 绑定
	// ========================================================================

	TArray<FBindingInfo> ListBindings(UMovieScene* MovieScene)
	{
		TArray<FBindingInfo> Out;
		for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
		{
			FBindingInfo Info;
			Info.Guid = Binding.GetObjectGuid();
			Info.Binding = &Binding;
			if (const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Info.Guid))
			{
				Info.Name = Possessable->GetName();
				Info.Kind = TEXT("possessable");
				Info.Parent = Possessable->GetParent();
			}
			else if (const FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(Info.Guid))
			{
				Info.Name = Spawnable->GetName();
				Info.Kind = TEXT("spawnable");
				if (const UObject* Template = Spawnable->GetObjectTemplate())
				{
					Info.Template = Template->GetClass()->GetPathName();
				}
			}
			if (Info.Name.IsEmpty()) { Info.Name = Info.Guid.ToString(); }
			Out.Add(Info);
		}
		return Out;
	}

	const FBindingInfo* FindByGuid(const TArray<FBindingInfo>& List, const FGuid& Guid)
	{
		return List.FindByPredicate([&Guid](const FBindingInfo& B) { return B.Guid == Guid; });
	}

	FString ParentName(const TArray<FBindingInfo>& List, const FBindingInfo& Info)
	{
		const FBindingInfo* Parent = Info.Parent.IsValid() ? FindByGuid(List, Info.Parent) : nullptr;
		return Parent ? Parent->Name : FString();
	}

	FString DisplayName(const TArray<FBindingInfo>& List, const FBindingInfo& Info)
	{
		const FString Parent = ParentName(List, Info);
		return Parent.IsEmpty() ? Info.Name : Parent + TEXT(" / ") + Info.Name;
	}

	/** 名字或 GUID → 绑定。重名时返回 nullptr 并在 Ambiguous 里列出候选 GUID */
	const FBindingInfo* Resolve(const TArray<FBindingInfo>& List, const FString& NameOrGuid, TArray<FString>& Ambiguous)
	{
		FGuid Guid;
		if (FGuid::Parse(NameOrGuid, Guid))
		{
			return FindByGuid(List, Guid);
		}
		const FBindingInfo* Found = nullptr;
		for (const FBindingInfo& B : List)
		{
			if (B.Name != NameOrGuid && DisplayName(List, B) != NameOrGuid) { continue; }
			Ambiguous.Add(B.Guid.ToString());
			Found = &B;
		}
		if (Ambiguous.Num() > 1) { return nullptr; }
		Ambiguous.Reset();
		return Found;
	}

	/** 一个绑定连同它下面的组件子绑定（递归）的全部轨道 */
	TArray<UMovieSceneTrack*> SubtreeTracks(const TArray<FBindingInfo>& List, const FBindingInfo& Root)
	{
		TArray<UMovieSceneTrack*> Out;
		TArray<FGuid> Queue = { Root.Guid };
		TSet<FGuid> Visited;
		while (Queue.Num() > 0)
		{
			const FGuid Current = Queue.Pop();
			if (Visited.Contains(Current)) { continue; }
			Visited.Add(Current);
			if (const FBindingInfo* Info = FindByGuid(List, Current))
			{
				Out.Append(Info->Binding->GetTracks());
			}
			for (const FBindingInfo& Child : List)
			{
				if (Child.Parent == Current) { Queue.Add(Child.Guid); }
			}
		}
		return Out;
	}

	FJson SequenceJson(const FSeq& Seq, int32 BindingCount)
	{
		FJson Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("path"), Seq.Sequence->GetPathName());
		Out->SetStringField(TEXT("display_rate"), FString::Printf(TEXT("%d/%d"), Seq.Display.Numerator, Seq.Display.Denominator));
		Out->SetStringField(TEXT("tick_resolution"), FString::Printf(TEXT("%d/%d"), Seq.Tick.Numerator, Seq.Tick.Denominator));
		const TRange<FFrameNumber> Range = Seq.MovieScene->GetPlaybackRange();
		const int32 Start = Range.GetLowerBound().IsClosed() ? Seq.ToDisplay(Range.GetLowerBoundValue()) : 0;
		const int32 End = Range.GetUpperBound().IsClosed() ? Seq.ToDisplay(Range.GetUpperBoundValue()) : 0;
		Out->SetNumberField(TEXT("playback_start"), Start);
		Out->SetNumberField(TEXT("playback_end"), End);
		Out->SetNumberField(TEXT("duration_seconds"), Seq.Display.AsSeconds(FFrameTime(End - Start)));
		Out->SetNumberField(TEXT("binding_count"), BindingCount);
		return Out;
	}

	void SequenceLevelChanges(const FSeq& A, const FSeq& B, FValues& Changes)
	{
		auto Add = [&Changes](const TCHAR* Field, const FString& X, const FString& Y)
		{
			if (X == Y) { return; }
			FJson C = MakeShared<FJsonObject>();
			C->SetStringField(TEXT("field"), Field);
			C->SetStringField(TEXT("base"), X);
			C->SetStringField(TEXT("compare"), Y);
			Changes.Add(MakeShared<FJsonValueObject>(C));
		};
		Add(TEXT("display_rate"), FString::Printf(TEXT("%d/%d"), A.Display.Numerator, A.Display.Denominator),
			FString::Printf(TEXT("%d/%d"), B.Display.Numerator, B.Display.Denominator));
		Add(TEXT("tick_resolution"), FString::Printf(TEXT("%d/%d"), A.Tick.Numerator, A.Tick.Denominator),
			FString::Printf(TEXT("%d/%d"), B.Tick.Numerator, B.Tick.Denominator));
		auto RangeSeconds = [](const FSeq& S)
		{
			const TRange<FFrameNumber> R = S.MovieScene->GetPlaybackRange();
			return FString::Printf(TEXT("%s–%s 秒"),
				R.GetLowerBound().IsClosed() ? *FString::SanitizeFloat(S.ToSeconds(R.GetLowerBoundValue()), 0) : TEXT("?"),
				R.GetUpperBound().IsClosed() ? *FString::SanitizeFloat(S.ToSeconds(R.GetUpperBoundValue()), 0) : TEXT("?"));
		};
		Add(TEXT("playback_range"), RangeSeconds(A), RangeSeconds(B));
	}

	// ========================================================================
	// 命令
	// ========================================================================

	/** 纯函数：给命令和自动化测试共用。失败时返回 nullptr 并写 Error */
	FJson Diff(const TSharedPtr<FJsonObject>& Payload, FString& Error)
	{
		FString BasePath, ComparePath;
		Payload->TryGetStringField(TEXT("base_path"), BasePath);
		Payload->TryGetStringField(TEXT("compare_path"), ComparePath);

		FSeq A, B;
		A.Sequence = LoadDiffSequence(BasePath, Error);
		if (!A.Sequence) { Error = TEXT("base：") + Error; return nullptr; }
		B.Sequence = LoadDiffSequence(ComparePath, Error);
		if (!B.Sequence) { Error = TEXT("compare：") + Error; return nullptr; }
		for (FSeq* S : { &A, &B })
		{
			S->MovieScene = S->Sequence->GetMovieScene();
			S->Display = S->MovieScene->GetDisplayRate();
			S->Tick = S->MovieScene->GetTickResolution();
		}
		const bool bTangents = A.Tick == B.Tick;

		const TArray<FBindingInfo> BA = ListBindings(A.MovieScene);
		const TArray<FBindingInfo> BB = ListBindings(B.MovieScene);

		FJson Data = MakeShared<FJsonObject>();
		Data->SetObjectField(TEXT("base"), SequenceJson(A, BA.Num()));
		Data->SetObjectField(TEXT("compare"), SequenceJson(B, BB.Num()));
		FValues Notes;
		if (!bTangents)
		{
			Notes.Add(MakeShared<FJsonValueString>(TEXT("两条序列的 Tick 分辨率不同，切线数值不可比，这次只比了插值模式、关键帧位置和值。")));
		}
		bool bTruncated = false;

		const TArray<TSharedPtr<FJsonValue>>* MapArray = nullptr;
		const bool bCoverage = Payload->TryGetArrayField(TEXT("binding_map"), MapArray) && MapArray && MapArray->Num() > 0;
		Data->SetStringField(TEXT("mode"), bCoverage ? TEXT("coverage") : TEXT("diff"));

		if (bCoverage)
		{
			FValues Coverage;
			for (const TSharedPtr<FJsonValue>& Value : *MapArray)
			{
				const TSharedPtr<FJsonObject>* Pair = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Pair) || !Pair) { continue; }
				FString BaseName, CompareName;
				(*Pair)->TryGetStringField(TEXT("base"), BaseName);
				(*Pair)->TryGetStringField(TEXT("compare"), CompareName);

				FJson Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("base"), BaseName);
				Entry->SetStringField(TEXT("compare"), CompareName);

				TArray<FString> AmbA, AmbB;
				const FBindingInfo* From = Resolve(BA, BaseName, AmbA);
				const FBindingInfo* To = Resolve(BB, CompareName, AmbB);
				auto Ambiguity = [](const TArray<FString>& Guids)
				{
					FValues V;
					for (const FString& G : Guids) { V.Add(MakeShared<FJsonValueString>(G)); }
					return V;
				};
				if (!From)
				{
					Entry->SetStringField(TEXT("status"), AmbA.Num() > 1 ? TEXT("base_ambiguous") : TEXT("base_not_found"));
					if (AmbA.Num() > 1) { Entry->SetArrayField(TEXT("candidates"), Ambiguity(AmbA)); }
					Coverage.Add(MakeShared<FJsonValueObject>(Entry));
					continue;
				}
				const TArray<FTrackSig> SourceTracks = ReadTracks(SubtreeTracks(BA, *From), A);
				FValues Items;
				if (!To)
				{
					// 目标绑定不在序列里：状态可能由运行时逻辑（附着、状态同步组件）接管，
					// 序列里看不出来。说「缺失」会让人去补一条其实已经有人管的轨道
					Entry->SetStringField(TEXT("status"), AmbB.Num() > 1 ? TEXT("compare_ambiguous") : TEXT("compare_not_found"));
					if (AmbB.Num() > 1) { Entry->SetArrayField(TEXT("candidates"), Ambiguity(AmbB)); }
					for (const FTrackSig& T : SourceTracks)
					{
						Items.Add(MakeShared<FJsonValueObject>(TrackEntry(T.Label, TEXT("unknown"), {})));
					}
					Entry->SetArrayField(TEXT("items"), Items);
					Coverage.Add(MakeShared<FJsonValueObject>(Entry));
					continue;
				}
				const TArray<FTrackSig> TargetTracks = ReadTracks(SubtreeTracks(BB, *To), B);
				Entry->SetStringField(TEXT("status"), TEXT("compared"));
				for (const FTrackSig& T : SourceTracks)
				{
					const FTrackSig* Match = TargetTracks.FindByPredicate([&T](const FTrackSig& O) { return O.Key == T.Key; });
					if (!Match)
					{
						Items.Add(MakeShared<FJsonValueObject>(TrackEntry(T.Label, TEXT("missing"), {})));
						continue;
					}
					TArray<FString> Details;
					const bool bChanged = CompareTracks(T, *Match, A, B, bTangents, Details);
					Items.Add(MakeShared<FJsonValueObject>(TrackEntry(T.Label, bChanged ? TEXT("adapted") : TEXT("inherited"), Details)));
				}
				FValues Extra;
				for (const FTrackSig& T : TargetTracks)
				{
					if (!SourceTracks.ContainsByPredicate([&T](const FTrackSig& O) { return O.Key == T.Key; }))
					{
						Extra.Add(MakeShared<FJsonValueString>(T.Label));
					}
				}
				Entry->SetArrayField(TEXT("items"), Items);
				Entry->SetArrayField(TEXT("extra_in_compare"), Extra);
				Coverage.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Data->SetArrayField(TEXT("coverage"), Coverage);
		}
		else
		{
			FValues SequenceChanges;
			SequenceLevelChanges(A, B, SequenceChanges);
			Data->SetArrayField(TEXT("sequence_changes"), SequenceChanges);
			Data->SetArrayField(TEXT("root_tracks"), DiffTrackSets(
				ReadTracks(UALCompat::GetRootTracks(A.MovieScene), A), ReadTracks(UALCompat::GetRootTracks(B.MovieScene), B), A, B, bTangents));

			TSet<FString> Filter;
			const TArray<TSharedPtr<FJsonValue>>* FilterArray = nullptr;
			if (Payload->TryGetArrayField(TEXT("bindings"), FilterArray) && FilterArray)
			{
				for (const TSharedPtr<FJsonValue>& V : *FilterArray) { FString S; if (V.IsValid() && V->TryGetString(S)) Filter.Add(S); }
			}
			auto Wanted = [&Filter](const TArray<FBindingInfo>& List, const FBindingInfo* Info)
			{
				return Filter.Num() == 0 || (Info && (Filter.Contains(Info->Name) || Filter.Contains(DisplayName(List, *Info))));
			};

			// 先按 GUID 对齐（副本保留 GUID），再按「父级 / 名字」唯一匹配
			TMap<FGuid, FGuid> Pairs;
			TSet<FGuid> TakenB;
			for (const FBindingInfo& X : BA)
			{
				if (FindByGuid(BB, X.Guid)) { Pairs.Add(X.Guid, X.Guid); TakenB.Add(X.Guid); }
			}
			for (const FBindingInfo& X : BA)
			{
				if (Pairs.Contains(X.Guid)) { continue; }
				const FString Key = DisplayName(BA, X);
				const FBindingInfo* Only = nullptr;
				int32 Hits = 0;
				for (const FBindingInfo& Y : BB)
				{
					if (!TakenB.Contains(Y.Guid) && DisplayName(BB, Y) == Key) { Only = &Y; ++Hits; }
				}
				if (Hits == 1 && BA.FilterByPredicate([&](const FBindingInfo& O) { return DisplayName(BA, O) == Key; }).Num() == 1)
				{
					Pairs.Add(X.Guid, Only->Guid);
					TakenB.Add(Only->Guid);
				}
			}

			FValues Bindings;
			int32 Unchanged = 0;
			auto Push = [&Bindings, &bTruncated](const FJson& Entry)
			{
				if (Bindings.Num() >= MaxBindingEntries) { bTruncated = true; return; }
				Bindings.Add(MakeShared<FJsonValueObject>(Entry));
			};
			for (const FBindingInfo& X : BA)
			{
				const FGuid* Paired = Pairs.Find(X.Guid);
				const FBindingInfo* Y = Paired ? FindByGuid(BB, *Paired) : nullptr;
				if (!Wanted(BA, &X) && !Wanted(BB, Y)) { continue; }
				FJson Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("base"), DisplayName(BA, X));
				if (!Y)
				{
					Entry->SetStringField(TEXT("status"), TEXT("only_in_base"));
					Entry->SetNumberField(TEXT("track_count"), X.Binding->GetTracks().Num());
					Push(Entry);
					continue;
				}
				Entry->SetStringField(TEXT("compare"), DisplayName(BB, *Y));
				Entry->SetStringField(TEXT("matched_by"), X.Guid == Y->Guid ? TEXT("id") : TEXT("name"));
				TArray<FString> BindingChanges;
				if (X.Kind != Y->Kind) { BindingChanges.Add(FString::Printf(TEXT("绑定类型 %s → %s"), *X.Kind, *Y->Kind)); }
				if (X.Template != Y->Template) { BindingChanges.Add(FString::Printf(TEXT("spawnable 模板 %s → %s"), *X.Template, *Y->Template)); }
				if (X.Name != Y->Name) { BindingChanges.Add(FString::Printf(TEXT("改名 %s → %s"), *X.Name, *Y->Name)); }
				const FValues Tracks = DiffTrackSets(ReadTracks(X.Binding->GetTracks(), A), ReadTracks(Y->Binding->GetTracks(), B), A, B, bTangents);
				if (Tracks.Num() == 0 && BindingChanges.Num() == 0) { ++Unchanged; continue; }
				Entry->SetStringField(TEXT("status"), TEXT("changed"));
				if (BindingChanges.Num() > 0)
				{
					FValues Lines;
					for (const FString& L : BindingChanges) { Lines.Add(MakeShared<FJsonValueString>(L)); }
					Entry->SetArrayField(TEXT("binding_changes"), Lines);
				}
				Entry->SetArrayField(TEXT("tracks"), Tracks);
				Push(Entry);
			}
			for (const FBindingInfo& Y : BB)
			{
				if (TakenB.Contains(Y.Guid) || !Wanted(BB, &Y)) { continue; }
				FJson Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("compare"), DisplayName(BB, Y));
				Entry->SetStringField(TEXT("status"), TEXT("only_in_compare"));
				Entry->SetNumberField(TEXT("track_count"), Y.Binding->GetTracks().Num());
				Push(Entry);
			}
			Data->SetArrayField(TEXT("bindings"), Bindings);
			Data->SetNumberField(TEXT("unchanged_bindings"), Unchanged);
			Data->SetBoolField(TEXT("filtered"), Filter.Num() > 0);
		}

		Data->SetArrayField(TEXT("notes"), Notes);
		Data->SetBoolField(TEXT("truncated"), bTruncated);
		return Data;
	}

	void Handle(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		FString Error;
		const FJson Data = Diff(Payload, Error);
		if (Data.IsValid()) { UAL_CommandUtils::SendResponse(RequestId, 200, Data); }
		else { UAL_CommandUtils::SendError(RequestId, 404, Error); }
	}
}

void FUAL_SequenceDiffCommands::RegisterCommands(
	TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& Commands)
{
	Commands.Add(TEXT("sequence.diff"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		UALSequenceDiff::Handle(Payload, RequestId);
	});
}
