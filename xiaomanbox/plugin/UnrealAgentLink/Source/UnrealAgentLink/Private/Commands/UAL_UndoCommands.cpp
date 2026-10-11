#include "UAL_UndoCommands.h"

#include "UAL_AgentUndo.h"
#include "UAL_CommandUtils.h"

namespace
{
	/** 历史默认只回最近这么多步。全量在长会话里能到几百条，塞满上下文没有意义 */
	constexpr int32 UAL_DEFAULT_UNDO_HISTORY_LIMIT = 20;

	/**
	 * 一步最多列几个对象名。
	 *
	 * 一次批量操作可能动上百个 Actor，全列会把历史撑爆；而调用方要的只是
	 * 「这一步动的是不是我记得的那几个」—— 几个名字加一个总数就够判断。
	 */
	constexpr int32 UAL_UNDO_OBJECT_PREVIEW = 8;

	int32 UAL_ReadUndoSteps(const TSharedPtr<FJsonObject>& Payload)
	{
		if (!Payload.IsValid())
		{
			return 0;
		}
		double Steps = 0.0;
		if (!Payload->TryGetNumberField(TEXT("steps"), Steps))
		{
			// 没给就是全部。这是最常被要的那个语义（「把刚才做的撤了」）
			return 0;
		}
		return static_cast<int32>(Steps);
	}

	TArray<TSharedPtr<FJsonValue>> UAL_UndoJsonStrings(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(Value));
		}
		return Out;
	}

	/**
	 * 撤销/重做走的是同一段收尾逻辑，只有动词不同。
	 *
	 * `affected_packages` 是两部分的并集：
	 *   1. 这一次撤销/重做**自己**动过的包（`Touched`，在动作之前从那几笔事务上取的）；
	 *   2. 栈上还剩的、水位线之后的那些包。
	 *
	 * 第 1 部分不能靠 `CollectTouchedPackagePaths` 拿 —— 它只扫还在活跃区的事务，
	 * 而刚撤掉的那几笔正好已经移出去了。少了它，「撤销三步、关卡里的宝箱跟着没了」
	 * 会返回一个**空的** affected_packages，调用方完全看不出关卡被动过
	 * （2026-09-16 的用户反馈就是这么丢的东西）。
	 *
	 * `affected_objects` 是同一批事务里的对象名。只有包名的话，
	 * 「/Game/DoorDemo/L_DoorDemo 被动了」看不出动的是哪个 Actor。
	 */
	void UAL_RespondWithUndoResult(
		const FString& RequestId,
		const TCHAR* Action,
		int32 Applied,
		const TArray<FString>& Titles,
		const FUAL_AgentUndo::FTouched& Touched,
		const FString& Error)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), Error.IsEmpty());
		Result->SetStringField(TEXT("action"), Action);
		Result->SetNumberField(TEXT("steps_applied"), Applied);
		Result->SetArrayField(TEXT("step_titles"), UAL_UndoJsonStrings(Titles));
		Result->SetNumberField(TEXT("remaining"), FUAL_AgentUndo::UndoableCount());
		Result->SetNumberField(TEXT("redoable"), FUAL_AgentUndo::RedoableCount());

		TSet<FString> AffectedSet = FUAL_AgentUndo::CollectTouchedPackagePaths();
		AffectedSet.Append(Touched.Packages);
		TArray<FString> Affected = AffectedSet.Array();
		Affected.Sort();
		Result->SetArrayField(TEXT("affected_packages"), UAL_UndoJsonStrings(Affected));

		TArray<FString> AffectedObjects = Touched.Objects;
		AffectedObjects.Sort();
		Result->SetArrayField(TEXT("affected_objects"), UAL_UndoJsonStrings(AffectedObjects));

		if (!Error.IsEmpty())
		{
			Result->SetStringField(TEXT("error"), Error);
		}

		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
	}
}

void FUAL_UndoCommands::RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("editor.undo_history"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_UndoHistory(Payload, RequestId);
	});

	CommandMap.Add(TEXT("editor.undo"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_Undo(Payload, RequestId);
	});

	CommandMap.Add(TEXT("editor.redo"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_Redo(Payload, RequestId);
	});
}

void FUAL_UndoCommands::Handle_UndoHistory(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	int32 Limit = UAL_DEFAULT_UNDO_HISTORY_LIMIT;
	if (Payload.IsValid())
	{
		double Raw = 0.0;
		if (Payload->TryGetNumberField(TEXT("limit"), Raw))
		{
			Limit = static_cast<int32>(Raw);
		}
	}

	const TArray<FUAL_AgentUndo::FEntry> Entries = FUAL_AgentUndo::DescribeUndoable();

	// 截的是尾巴不是头 —— 想看的永远是「刚才做了什么」
	const int32 First = (Limit > 0 && Entries.Num() > Limit) ? Entries.Num() - Limit : 0;

	TArray<TSharedPtr<FJsonValue>> EntriesJson;
	for (int32 Index = First; Index < Entries.Num(); ++Index)
	{
		const FUAL_AgentUndo::FEntry& Entry = Entries[Index];

		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("step"), Entry.Index);
		Json->SetStringField(TEXT("title"), Entry.Title);
		Json->SetStringField(TEXT("context"), Entry.Context);
		Json->SetArrayField(TEXT("packages"), UAL_UndoJsonStrings(Entry.Packages));

		// 这一步动过谁。只有标题和关卡名的话，一条「批量修改Actor变换」谁也认不出
		// 动的是哪几个物件 —— 也就没法只回滚其中某几步。
		TArray<FString> Preview = Entry.Objects;
		if (Preview.Num() > UAL_UNDO_OBJECT_PREVIEW)
		{
			Preview.SetNum(UAL_UNDO_OBJECT_PREVIEW);
		}
		Json->SetArrayField(TEXT("objects"), UAL_UndoJsonStrings(Preview));
		Json->SetNumberField(TEXT("object_count"), Entry.Objects.Num());
		EntriesJson.Add(MakeShared<FJsonValueObject>(Json));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetNumberField(TEXT("undoable"), Entries.Num());
	Result->SetNumberField(TEXT("redoable"), FUAL_AgentUndo::RedoableCount());
	Result->SetNumberField(TEXT("returned"), EntriesJson.Num());
	Result->SetArrayField(TEXT("entries"), EntriesJson);
	Result->SetStringField(
		TEXT("note"),
		TEXT("These are this plugin's own steps only. The user's manual edits live on a separate undo stack and are never listed or undone here."));

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

void FUAL_UndoCommands::Handle_Undo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	TArray<FString> Titles;
	FUAL_AgentUndo::FTouched Touched;
	FString Error;
	const int32 Undone = FUAL_AgentUndo::Undo(UAL_ReadUndoSteps(Payload), Titles, Touched, Error);
	UAL_RespondWithUndoResult(RequestId, TEXT("undo"), Undone, Titles, Touched, Error);
}

void FUAL_UndoCommands::Handle_Redo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	TArray<FString> Titles;
	FUAL_AgentUndo::FTouched Touched;
	FString Error;
	const int32 Redone = FUAL_AgentUndo::Redo(UAL_ReadUndoSteps(Payload), Titles, Touched, Error);
	UAL_RespondWithUndoResult(RequestId, TEXT("redo"), Redone, Titles, Touched, Error);
}
