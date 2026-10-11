#include "UAL_CommandHandler.h"
#include "UAL_NetworkManager.h"
#include "UAL_CommandUtils.h"
#include "UAL_ActorCommands.h"
#include "UAL_SystemCommands.h"
#include "UAL_LevelCommands.h"
#include "UAL_EditorCommands.h"
#include "UAL_BlueprintCommands.h"
#include "UAL_ContentBrowserCommands.h"
#include "UAL_ContentOrganizeCommands.h"
#include "UAL_ContentSafetyCommands.h"
#include "UAL_CppCommands.h"
#include "UAL_MaterialCommands.h"
#include "UAL_MeshCommands.h"
#include "UAL_AnimationCommands.h"
#include "UAL_InputCommands.h"
#include "UAL_PieBotCommands.h"
#include "UAL_MessageLogCommands.h"
#include "UAL_PCGCommands.h"
#include "UAL_LandscapeCommands.h"
#include "UAL_SequencerCommands.h"
#include "UAL_SequenceDiffCommands.h"
#include "UAL_ComponentInspectCommands.h"
#include "UAL_UndoCommands.h"
#include "UAL_WidgetCommands.h"
#include "UAL_AssetLockState.h"
#include "UAL_CommandScope.h"


#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Misc/MessageDialog.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALCommand, Log, All);

FUAL_CommandHandler::FUAL_CommandHandler()
{
	RegisterCommands();
}

TSharedPtr<FJsonObject> FUAL_CommandHandler::BuildProjectInfo() const
{
	return FUAL_EditorCommands::BuildProjectInfo();
}

void FUAL_CommandHandler::ProcessMessage(const FString& JsonPayload)
{
	if (!IsInGameThread())
	{
		// UE 5.7 修复：使用 Ticker 而不是 AsyncTask 调度到 GameThread
		// AsyncTask 会在 TaskGraph 上下文中执行，当后续调用 Interchange 导入时
		// 会触发 TaskGraph 递归保护断言崩溃 (++Queue(QueueIndex).RecursionGuard == 1)
		// 使用 Ticker 可以确保代码在正常的 Tick 上下文中执行，脱离 TaskGraph
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([this, Payload = JsonPayload](float DeltaTime) -> bool
			{
				ProcessMessage(Payload);
				return false; // 只执行一次
			}),
			0.0f // 立即在下一个 Tick 执行
		);
		return;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonPayload);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogUALCommand, Warning, TEXT("Invalid JSON payload: %s"), *JsonPayload);
		return;
	}

	FString Type, Method, RequestId;
	Root->TryGetStringField(TEXT("type"), Type);
	Root->TryGetStringField(TEXT("method"), Method);
	Root->TryGetStringField(TEXT("id"), RequestId);

	const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
	if (Type == TEXT("req"))
	{
		// JSON-RPC 风格：params；兼容旧字段 payload
		if (!Root->TryGetObjectField(TEXT("params"), ParamsObj))
		{
			Root->TryGetObjectField(TEXT("payload"), ParamsObj);
		}
	}
	else if (Type == TEXT("res"))
	{
		// JSON-RPC 风格：result；兼容旧字段 data；以及 payload 可能也被用于响应
		if (!Root->TryGetObjectField(TEXT("result"), ParamsObj))
		{
			if (!Root->TryGetObjectField(TEXT("data"), ParamsObj))
			{
				Root->TryGetObjectField(TEXT("payload"), ParamsObj);
			}
		}
	}

	UE_LOG(LogUALCommand, Display, TEXT("Recv message type=%s method=%s id=%s"), *Type, *Method, *RequestId);

	if (Type != TEXT("req"))
	{
		if (Type == TEXT("res"))
		{
			Handle_Response(Method, ParamsObj ? *ParamsObj : nullptr);
		}
		else
		{
			UE_LOG(LogUALCommand, Verbose, TEXT("Ignore non-request message: %s"), *Type);
		}
		return;
	}

	const FHandlerFunc* Handler = CommandMap.Find(Method);
	if (!Handler)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Unknown method: %s"), *Method));
		return;
	}

	/**
	 * 命令执行期间打开的作用域，管两件事：
	 *
	 *   1. 「哪些包被我们改脏了」的记录窗口（决定 editor.save 存什么）；
	 *   2. 把 `GEditor->Trans` 换成 agent 自己那条撤销栈，agent 的三十步
	 *      不会把用户的撤销历史埋掉，也才谈得上「把 AI 做的全撤了」。
	 *
	 * 放在这一处而不是每个写命令里各做一次：这里是全部命令的唯一入口，
	 * 所以连命令的副作用（比如编译蓝图顺带改脏依赖它的资产）也一并算进来。
	 *
	 * 用户自己在编辑器里的改动不经过这里，两条记账都不会把他算进来 ——
	 * 「只存 agent 自己改过的」「只撤 agent 自己做过的」都是这么来的。
	 */
	FUAL_CommandScope TouchScope;
	(*Handler)(ParamsObj ? *ParamsObj : MakeShared<FJsonObject>(), RequestId);
}

void FUAL_CommandHandler::RegisterCommands()
{
	// 统一调用各模块的 RegisterCommands 函数
	// 新增命令只需在对应模块的 RegisterCommands 中添加即可
	
	FUAL_SystemCommands::RegisterCommands(CommandMap);
	FUAL_LevelCommands::RegisterCommands(CommandMap);
	FUAL_EditorCommands::RegisterCommands(CommandMap);
	FUAL_ActorCommands::RegisterCommands(CommandMap);
	FUAL_BlueprintCommands::RegisterCommands(CommandMap);
	FUAL_ContentBrowserCommands::RegisterCommands(CommandMap);
	FUAL_ContentOrganizeCommands::RegisterCommands(CommandMap);
	FUAL_ContentSafetyCommands::RegisterCommands(CommandMap);
	FUAL_CppCommands::RegisterCommands(CommandMap);
	FUAL_MaterialCommands::RegisterCommands(CommandMap);
	FUAL_MeshCommands::RegisterCommands(CommandMap);
	FUAL_AnimationCommands::RegisterCommands(CommandMap);
	FUAL_InputCommands::RegisterCommands(CommandMap);
	FUAL_PieBotCommands::RegisterCommands(CommandMap);
	FUAL_MessageLogCommands::RegisterCommands(CommandMap);
	FUAL_SequencerCommands::RegisterCommands(CommandMap);
	FUAL_SequenceDiffCommands::RegisterCommands(CommandMap);
	FUAL_ComponentInspectCommands::RegisterCommands(CommandMap);
	FUAL_UndoCommands::RegisterCommands(CommandMap);
	FUAL_WidgetCommands::RegisterCommands(CommandMap);
	// PCG 是可选插件，命令始终注册 —— PCG 没启用时由 pcg.status 如实汇报，
	// 而不是让命令整个消失（那样调用方只会拿到「未知方法」，看不出原因）
	FUAL_PCGCommands::RegisterCommands(CommandMap);
	FUAL_LandscapeCommands::RegisterCommands(CommandMap);

	// locks.set —— 盒子推「哪些资产 AI 正在改」，用来画内容浏览器角标。
	// 不是「命令」而是状态推送，但走同一条 req 通道，没必要为它另开协议
	FUAL_AssetLockState::RegisterCommands(CommandMap);
}

void FUAL_CommandHandler::Handle_Response(const FString& Method, const TSharedPtr<FJsonObject>& Payload)
{
	UE_LOG(LogUALCommand, Log, TEXT("Handle_Response: Method=%s"), *Method);

	if (!Payload.IsValid())
	{
		UE_LOG(LogUALCommand, Warning, TEXT("Handle_Response: Payload is invalid"));
		return;
	}

	bool bOk = true;
	Payload->TryGetBoolField(TEXT("ok"), bOk);

	int32 Count = 0;
	if (!Payload->TryGetNumberField(TEXT("count"), Count))
	{
		Count = 0;
	}

	FString ImportedPath;
	Payload->TryGetStringField(TEXT("importedPath"), ImportedPath);

	FString Error;
	Payload->TryGetStringField(TEXT("error"), Error);

	const bool bIsImportFolder = Method == TEXT("content.import_folder");
	const bool bIsImportAssets = Method == TEXT("content.import_assets");
	
	UE_LOG(LogUALCommand, Log, TEXT("Handle_Response: bIsImportFolder=%d, bIsImportAssets=%d"), bIsImportFolder, bIsImportAssets);

	if (!bIsImportFolder && !bIsImportAssets)
	{
		return; // 非导入相关响应不提示
	}

	const FString Title = bIsImportFolder
		? UAL_CommandUtils::LStr(TEXT("导入文件夹到虚幻盒子资产库"), TEXT("Import Folder to Unreal Box Asset Library"))
		: UAL_CommandUtils::LStr(TEXT("导入资产到虚幻盒子资产库"), TEXT("Import Assets to Unreal Box Asset Library"));

	FString Body;
	if (bOk)
	{
		if (!ImportedPath.IsEmpty())
		{
			Body = FString::Printf(TEXT("%s: %s"),
				*UAL_CommandUtils::LStr(TEXT("成功"), TEXT("Succeeded")),
				*ImportedPath);
		}
		else
		{
			Body = FString::Printf(TEXT("%s: %s (%d)"),
				*UAL_CommandUtils::LStr(TEXT("成功"), TEXT("Succeeded")),
				*UAL_CommandUtils::LStr(TEXT("资产已导入"), TEXT("Asset(s) imported")),
				Count);
		}
	}
	else
	{
		if (Error.IsEmpty())
		{
			Error = UAL_CommandUtils::LStr(TEXT("导入失败"), TEXT("Import failed"));
		}
		Body = FString::Printf(TEXT("%s (%s)"),
			*UAL_CommandUtils::LStr(TEXT("失败"), TEXT("Failed")),
			*Error);
	}

	UE_LOG(LogUALCommand, Log, TEXT("Handle_Response: Showing notification. Title=%s, Body=%s"), *Title, *Body);

	// Ensure run on game thread
	AsyncTask(ENamedThreads::GameThread, [Title, Body, bOk]()
	{
		FNotificationInfo Info(FText::FromString(Title));
		Info.SubText = FText::FromString(Body);
		Info.ExpireDuration = 5.0f;
		Info.FadeOutDuration = 1.0f;
		Info.bUseThrobber = false;
		Info.bFireAndForget = true;
		Info.bUseLargeFont = false;

#if WITH_EDITOR
		TSharedPtr<SNotificationItem> Notification;
		if (FSlateApplication::IsInitialized())
		{
			Notification = FSlateNotificationManager::Get().AddNotification(Info);
			if (Notification.IsValid())
			{
				Notification->SetCompletionState(bOk ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
				UE_LOG(LogUALCommand, Log, TEXT("Handle_Response: Notification created successfully"));
			}
			else
			{
				UE_LOG(LogUALCommand, Warning, TEXT("Handle_Response: Notification creation failed (Notification is invalid)"));
			}
		}
		else
		{
			UE_LOG(LogUALCommand, Warning, TEXT("Handle_Response: SlateApplication not initialized"));
		}

		if (!Notification.IsValid())
		{
			const FText DialogText = FText::Format(
				FText::FromString(TEXT("{0}\n{1}")),
				FText::FromString(Title),
				FText::FromString(Body));
			FMessageDialog::Open(EAppMsgType::Ok, DialogText);
		}
#else
		FSlateNotificationManager::Get().AddNotification(Info);
#endif
	});
}
