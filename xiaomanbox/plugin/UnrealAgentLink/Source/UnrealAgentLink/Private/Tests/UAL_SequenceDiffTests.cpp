#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "GameFramework/Actor.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "UObject/Package.h"

namespace UALSequenceDiff
{
	TSharedPtr<FJsonObject> Diff(const TSharedPtr<FJsonObject>& Payload, FString& Error);
}

// 具名命名空间：unity build 会把多个测试文件拼进一个翻译单元，匿名命名空间挡不住撞名
namespace UALSequenceDiffTest
{
	/** 给绑定加一条属性浮点轨，两帧关键帧 */
	FMovieSceneFloatChannel* AddFloatTrack(UMovieScene* MovieScene, const FGuid& Binding, const TCHAR* Property, float From, float To)
	{
		UMovieSceneFloatTrack* Track = MovieScene->AddTrack<UMovieSceneFloatTrack>(Binding);
		Track->SetPropertyNameAndPath(Property, Property);
		UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(Track->CreateNewSection());
		Section->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(24000)));
		Track->AddSection(*Section);
		FMovieSceneFloatChannel* Channel = Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0);
		Channel->AddLinearKey(FFrameNumber(0), From);
		Channel->AddLinearKey(FFrameNumber(12000), To);
		return Channel;
	}

	/** 测试可能在同一个编辑器里重跑，名字不能写死 */
	FName Unique(const TCHAR* Base)
	{
		return MakeUniqueObjectName(GetTransientPackage(), ULevelSequence::StaticClass(), FName(Base));
	}

	TSharedPtr<FJsonObject> Payload(const UObject* A, const UObject* B)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("base_path"), A->GetPathName());
		P->SetStringField(TEXT("compare_path"), B->GetPathName());
		return P;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUALSequenceDiffTest,
	"UnrealAgentLink.Sequencer.Diff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUALSequenceDiffTest::RunTest(const FString& Parameters)
{
	using namespace UALSequenceDiffTest;
	ULevelSequence* Base = NewObject<ULevelSequence>(GetTransientPackage(), Unique(TEXT("UALDiffTest_Base")));
	Base->Initialize();
	UMovieScene* BaseScene = Base->GetMovieScene();
	const FGuid Dog = BaseScene->AddPossessable(TEXT("Dog_01"), AActor::StaticClass());
	AddFloatTrack(BaseScene, Dog, TEXT("CustomDepthStencilValue"), 1.f, 3.f);

	// ── 原样复制：没有语义差异 ─────────────────────────────────────────────
	// 这就是反馈里 SHA256 对不上的那种情况该得到的回答 —— 内容没动
	ULevelSequence* Copy = DuplicateObject<ULevelSequence>(Base, GetTransientPackage(), Unique(TEXT("UALDiffTest_Copy")));
	FString Error;
	TSharedPtr<FJsonObject> Same = UALSequenceDiff::Diff(Payload(Base, Copy), Error);
	if (!TestTrue(TEXT("原样复制：对比成功 ") + Error, Same.IsValid())) return false;
	TestEqual(TEXT("原样复制：没有有差异的绑定"), Same->GetArrayField(TEXT("bindings")).Num(), 0);
	TestEqual(TEXT("原样复制：没有序列级差异"), Same->GetArrayField(TEXT("sequence_changes")).Num(), 0);
	TestEqual(TEXT("原样复制：一个绑定一致"), static_cast<int32>(Same->GetNumberField(TEXT("unchanged_bindings"))), 1);

	// ── 改一个关键帧的值：要报出来，而且说清是哪一帧 ─────────────────────
	{
		UMovieSceneFloatTrack* Track = Copy->GetMovieScene()->FindTrack<UMovieSceneFloatTrack>(Dog);
		UMovieSceneSection* Section = Track ? Track->GetAllSections()[0] : nullptr;
		FMovieSceneFloatChannel* Channel = Section ? Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0) : nullptr;
		if (!TestNotNull(TEXT("副本里找得到那条轨"), Channel)) return false;
		Channel->GetData().GetValues()[1].Value = 5.f;
	}
	TSharedPtr<FJsonObject> Changed = UALSequenceDiff::Diff(Payload(Base, Copy), Error);
	if (!TestTrue(TEXT("改值：对比成功"), Changed.IsValid())) return false;
	const TArray<TSharedPtr<FJsonValue>> Bindings = Changed->GetArrayField(TEXT("bindings"));
	if (!TestEqual(TEXT("改值：一个绑定有差异"), Bindings.Num(), 1)) return false;
	const TSharedPtr<FJsonObject> Entry = Bindings[0]->AsObject();
	TestEqual(TEXT("改值：按 GUID 对上"), Entry->GetStringField(TEXT("matched_by")), FString(TEXT("id")));
	const TSharedPtr<FJsonObject> TrackEntry = Entry->GetArrayField(TEXT("tracks"))[0]->AsObject();
	TestEqual(TEXT("改值：轨道状态"), TrackEntry->GetStringField(TEXT("status")), FString(TEXT("changed")));
	TestTrue(TEXT("改值：轨道按属性路径认"), TrackEntry->GetStringField(TEXT("track")).Contains(TEXT("CustomDepthStencilValue")));
	const FString Detail = TrackEntry->GetArrayField(TEXT("details"))[0]->AsString();
	TestTrue(TEXT("改值：细节写出新旧值 ") + Detail, Detail.Contains(TEXT("3 → 5")));

	// ── 覆盖检查：新显示角色没接描边轨 → 缺失；目标不在序列里 → 无法判断 ─────
	ULevelSequence* Target = NewObject<ULevelSequence>(GetTransientPackage(), Unique(TEXT("UALDiffTest_Target")));
	Target->Initialize();
	Target->GetMovieScene()->AddPossessable(TEXT("DogDisplay_01"), AActor::StaticClass());
	TSharedPtr<FJsonObject> Cover = Payload(Base, Target);
	TArray<TSharedPtr<FJsonValue>> Map;
	for (const TCHAR* To : { TEXT("DogDisplay_01"), TEXT("Ghost") })
	{
		TSharedPtr<FJsonObject> Pair = MakeShared<FJsonObject>();
		Pair->SetStringField(TEXT("base"), TEXT("Dog_01"));
		Pair->SetStringField(TEXT("compare"), To);
		Map.Add(MakeShared<FJsonValueObject>(Pair));
	}
	Cover->SetArrayField(TEXT("binding_map"), Map);
	TSharedPtr<FJsonObject> Coverage = UALSequenceDiff::Diff(Cover, Error);
	if (!TestTrue(TEXT("覆盖：对比成功"), Coverage.IsValid())) return false;
	TestEqual(TEXT("覆盖：模式"), Coverage->GetStringField(TEXT("mode")), FString(TEXT("coverage")));
	const TArray<TSharedPtr<FJsonValue>> Pairs = Coverage->GetArrayField(TEXT("coverage"));
	if (!TestEqual(TEXT("覆盖：两对"), Pairs.Num(), 2)) return false;
	const TSharedPtr<FJsonObject> First = Pairs[0]->AsObject();
	TestEqual(TEXT("覆盖：第一对比成了"), First->GetStringField(TEXT("status")), FString(TEXT("compared")));
	TestEqual(TEXT("覆盖：描边轨缺失"), First->GetArrayField(TEXT("items"))[0]->AsObject()->GetStringField(TEXT("status")), FString(TEXT("missing")));
	const TSharedPtr<FJsonObject> Second = Pairs[1]->AsObject();
	TestEqual(TEXT("覆盖：目标不在序列里"), Second->GetStringField(TEXT("status")), FString(TEXT("compare_not_found")));
	TestEqual(TEXT("覆盖：不说缺失，说无法判断"), Second->GetArrayField(TEXT("items"))[0]->AsObject()->GetStringField(TEXT("status")), FString(TEXT("unknown")));

	// 同一条序列内两个绑定对比（旧驱动 → 新显示层）也要能用：同一资产当 base 和 compare
	TSharedPtr<FJsonObject> SelfPayload = Payload(Base, Base);
	TSharedPtr<FJsonObject> SelfPair = MakeShared<FJsonObject>();
	SelfPair->SetStringField(TEXT("base"), TEXT("Dog_01"));
	SelfPair->SetStringField(TEXT("compare"), TEXT("Dog_01"));
	TArray<TSharedPtr<FJsonValue>> SelfMap;
	SelfMap.Add(MakeShared<FJsonValueObject>(SelfPair));
	SelfPayload->SetArrayField(TEXT("binding_map"), SelfMap);
	TSharedPtr<FJsonObject> Self = UALSequenceDiff::Diff(SelfPayload, Error);
	TestTrue(TEXT("自比：已继承"), Self.IsValid() &&
		Self->GetArrayField(TEXT("coverage"))[0]->AsObject()->GetArrayField(TEXT("items"))[0]->AsObject()->GetStringField(TEXT("status")) == TEXT("inherited"));

	return true;
}

#endif
