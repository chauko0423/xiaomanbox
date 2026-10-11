#pragma once

#include "CoreMinimal.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"

/**
 * 作用域内替用户回答引擎模态框，每次回答都记下来。
 *
 * ## 为什么需要
 *
 * `IAssetTools::RenameAssets` 在改名前查 CDO 引用，命中就 `FMessageDialog::Open`
 * 弹一个确认框（AssetRenameManager.cpp，**不受 bWithDialog 控制**，九版都是无条件的）。
 * `FMessageDialog::Open` 的逻辑是：
 *
 *   - `!FApp::IsUnattended() && !GIsRunningUnattendedScript` → 走 `FCoreDelegates` 上的
 *     委托真弹窗（编辑器交互态会卡在游戏线程上，RPC 超时）；
 *   - 否则直接返回默认值 —— 而 CDO 那个框的默认值是 Cancel（5.0 是 No），
 *     整批改名静默中止，`RenameAssets` 返回 false，**一行日志都没有**。
 *
 * batch_move 为了躲签出 / 覆盖弹窗套着 `GIsRunningUnattendedScript = true`，所以今天
 * 走的是第二条路。`on_cdo_refs=proceed` 要的是「继续改名」，唯一能做到的办法是
 * **替用户回答**：作用域内把 unattended 放回 false（否则委托被绕过），把委托换成
 * 我们的处理器，对任何弹窗返回肯定答案，完了恢复。
 *
 * ## 为什么敢一律答「肯定」
 *
 * `bAutoCheckout=true, bWithDialog=false` 路径下 `RenameAssets` 只有两个
 * `FMessageDialog::Open`：CDO 警告，和软引用存盘确认（后者在 bAutoCheckout 下走不到）。
 * 肯定答案就是「继续改名」。每次回答的类型、标题、正文都记进响应的
 * `auto_answered_dialogs[]` —— 命令替用户点了什么，用户看得见。
 *
 * ## 委托的版本漂移
 *
 * 5.0–5.2 是 `FCoreDelegates::ModalErrorMessage`，三参 `(EAppMsgType, const FText&, const FText&)`；
 * 5.3 起改名 `ModalMessageDialog`，四参，多了 `EAppMsgCategory`（5.3+ 才有这个枚举，
 * 所以处理器的第一个参数用 `auto`，不能点名它）。用成员探测分支
 * （`decltype(FCoreDelegates::ModalMessageDialog)` 在不在），不用版本号。
 *
 * ## 边界
 *
 * 编辑器本身带 `-unattended` 启动时 `FApp::IsUnattended()` 为真，`FMessageDialog::Open`
 * 根本不看委托，这一套不起作用。用 `CanAnswer()` 事先问，响应里明说。
 */
class FUAL_ScopedDialogAutoAnswer
{
public:
	struct FAnswered
	{
		FString Type;
		FString Title;
		FString Message;
		FString Answer;
	};

	FUAL_ScopedDialogAutoAnswer()
		: UnattendedGuard(GIsRunningUnattendedScript, false)
	{
		Private::Bind<FCoreDelegates>(this, Restore, 0);
	}

	~FUAL_ScopedDialogAutoAnswer()
	{
		if (Restore)
		{
			Restore();
		}
	}

	FUAL_ScopedDialogAutoAnswer(const FUAL_ScopedDialogAutoAnswer&) = delete;
	FUAL_ScopedDialogAutoAnswer& operator=(const FUAL_ScopedDialogAutoAnswer&) = delete;

	/** `-unattended` 的编辑器不看委托，这时候替答不起作用 */
	static bool CanAnswer() { return !FApp::IsUnattended(); }

	const TArray<FAnswered>& Answered() const { return Dialogs; }

	/** 响应里的 `auto_answered_dialogs` 数组 */
	TArray<TSharedPtr<FJsonValue>> ToJson() const
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FAnswered& Dialog : Dialogs)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("type"), Dialog.Type);
			Obj->SetStringField(TEXT("title"), Dialog.Title);
			Obj->SetStringField(TEXT("message"), Dialog.Message);
			Obj->SetStringField(TEXT("answer"), Dialog.Answer);
			Out.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Out;
	}

	/** 按按钮组合给出「继续」那一个答案 */
	static EAppReturnType::Type AffirmativeFor(EAppMsgType::Type Type)
	{
		switch (Type)
		{
		case EAppMsgType::Ok:                      return EAppReturnType::Ok;
		case EAppMsgType::YesNo:                   return EAppReturnType::Yes;
		case EAppMsgType::OkCancel:                return EAppReturnType::Ok;
		case EAppMsgType::YesNoCancel:             return EAppReturnType::Yes;
		case EAppMsgType::CancelRetryContinue:     return EAppReturnType::Continue;
		case EAppMsgType::YesNoYesAllNoAll:        return EAppReturnType::Yes;
		case EAppMsgType::YesNoYesAllNoAllCancel:  return EAppReturnType::Yes;
		case EAppMsgType::YesNoYesAll:             return EAppReturnType::Yes;
		default:                                   return EAppReturnType::Ok;
		}
	}

	static const TCHAR* MsgTypeName(EAppMsgType::Type Type)
	{
		switch (Type)
		{
		case EAppMsgType::Ok:                      return TEXT("Ok");
		case EAppMsgType::YesNo:                   return TEXT("YesNo");
		case EAppMsgType::OkCancel:                return TEXT("OkCancel");
		case EAppMsgType::YesNoCancel:             return TEXT("YesNoCancel");
		case EAppMsgType::CancelRetryContinue:     return TEXT("CancelRetryContinue");
		case EAppMsgType::YesNoYesAllNoAll:        return TEXT("YesNoYesAllNoAll");
		case EAppMsgType::YesNoYesAllNoAllCancel:  return TEXT("YesNoYesAllNoAllCancel");
		case EAppMsgType::YesNoYesAll:             return TEXT("YesNoYesAll");
		default:                                   return TEXT("Unknown");
		}
	}

	static const TCHAR* ReturnTypeName(EAppReturnType::Type Type)
	{
		switch (Type)
		{
		case EAppReturnType::No:       return TEXT("No");
		case EAppReturnType::Yes:      return TEXT("Yes");
		case EAppReturnType::YesAll:   return TEXT("YesAll");
		case EAppReturnType::NoAll:    return TEXT("NoAll");
		case EAppReturnType::Cancel:   return TEXT("Cancel");
		case EAppReturnType::Ok:       return TEXT("Ok");
		case EAppReturnType::Retry:    return TEXT("Retry");
		case EAppReturnType::Continue: return TEXT("Continue");
		default:                       return TEXT("Unknown");
		}
	}

	/** 委托处理器调这里；public 只是因为处理器是个自由 lambda */
	EAppReturnType::Type Answer(EAppMsgType::Type Type, const FText& Message, const FText& Title)
	{
		const EAppReturnType::Type Result = AffirmativeFor(Type);
		FAnswered Record;
		Record.Type = MsgTypeName(Type);
		Record.Title = Title.ToString();
		Record.Message = Message.ToString();
		Record.Answer = ReturnTypeName(Result);
		Dialogs.Add(Record);
		return Result;
	}

private:
	struct Private
	{
		// int 重载优先：5.3+ 的 ModalMessageDialog（四参）。第一个参数是 EAppMsgCategory，
		// 5.0–5.2 上没有这个类型，所以用 auto 接
		template <typename CoreDelegatesType>
		static auto Bind(FUAL_ScopedDialogAutoAnswer* Self, TFunction<void()>& OutRestore, int)
			-> decltype(CoreDelegatesType::ModalMessageDialog, void())
		{
			auto Saved = CoreDelegatesType::ModalMessageDialog;
			CoreDelegatesType::ModalMessageDialog.BindLambda(
				[Self](auto /*Category*/, EAppMsgType::Type Type, const FText& Message, const FText& Title)
				{
					return Self->Answer(Type, Message, Title);
				});
			OutRestore = [Saved]() { CoreDelegatesType::ModalMessageDialog = Saved; };
		}

		// long 重载兜底：5.0–5.2 的 ModalErrorMessage（三参）
		template <typename CoreDelegatesType>
		static auto Bind(FUAL_ScopedDialogAutoAnswer* Self, TFunction<void()>& OutRestore, long)
			-> decltype(CoreDelegatesType::ModalErrorMessage, void())
		{
			auto Saved = CoreDelegatesType::ModalErrorMessage;
			CoreDelegatesType::ModalErrorMessage.BindLambda(
				[Self](EAppMsgType::Type Type, const FText& Message, const FText& Title)
				{
					return Self->Answer(Type, Message, Title);
				});
			OutRestore = [Saved]() { CoreDelegatesType::ModalErrorMessage = Saved; };
		}
	};

	TArray<FAnswered> Dialogs;
	/** 先于 Restore 构造：unattended 放回 false 是委托被看见的前提 */
	TGuardValue<bool> UnattendedGuard;
	TFunction<void()> Restore;
};
