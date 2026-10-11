// 事件分发器节点的**名字解析**。只做字符串，不碰引擎对象 —— 所以它能被
// 自动化测试直接跑（UAL_DelegateNodeNameTests.cpp），而建节点那部分不能。
//
// 为什么需要它：编辑器里右键分发器出来的六项菜单，标题是拿属性名拼出来的
// （"Call On Chest Opened"），而建出来的节点是 UK2Node_CallDelegate 这一族 ——
// 存的是指向委托属性的引用，不是函数引用。照着标题去查函数表永远查不到。
// 引擎侧原文：Engine/Source/Editor/Kismet/Private/BPDelegateDragDropAction.cpp。

#pragma once

#include "CoreMinimal.h"

namespace UAL_DelegateNodeName
{
	enum class EKind : uint8
	{
		None,
		Call,   // 广播      UK2Node_CallDelegate
		Add,    // 绑定      UK2Node_AddDelegate
		Remove, // 解除绑定  UK2Node_RemoveDelegate
		Clear   // 解除所有  UK2Node_ClearDelegate
	};

	/** 名字比对用：去掉空格再小写。编辑器标题里的 "On Chest Opened" 要能对上属性名 OnChestOpened */
	inline FString Compact(const FString& In)
	{
		return In.Replace(TEXT(" "), TEXT("")).ToLower();
	}

	/**
	 * 从 class / member_name 里认出「这是个委托节点」，并剥出分发器名。
	 *
	 * 两种写法都认：
	 *   - class 直接点名：CallDispatcher / BindEvent / UnbindEvent / UnbindAllEvents
	 *     （引擎类名 CallDelegate / AddDelegate / RemoveDelegate / ClearDelegate 也认）；
	 *   - class=Function + member_name 写编辑器里看到的标题，如
	 *     "Call OnChestOpened"、"Bind Event to OnChestOpened"。工具文档和
	 *     event_dispatcher 的提示语长期是这么写的，所以这条必须能用。
	 *
	 * `NormalizedType` 是 UAL_NormalizeNodeType 的输出（去下划线 + 小写）。
	 *
	 * 认出来只说明「像」。class=Function 那条路上还要真找到委托属性才算数 ——
	 * 找不到就得退回去按普通函数解析，否则一个真叫 `Call...` 的函数会被劫走。
	 */
	inline EKind Parse(const FString& NormalizedType, const FString& Name, FString& OutDelegateName)
	{
		OutDelegateName = Name;

		if (NormalizedType == TEXT("calldelegate") || NormalizedType == TEXT("calldispatcher"))
		{
			return EKind::Call;
		}
		if (NormalizedType == TEXT("adddelegate") || NormalizedType == TEXT("bindevent"))
		{
			return EKind::Add;
		}
		if (NormalizedType == TEXT("removedelegate") || NormalizedType == TEXT("unbindevent"))
		{
			return EKind::Remove;
		}
		if (NormalizedType == TEXT("cleardelegate") || NormalizedType == TEXT("unbindallevents"))
		{
			return EKind::Clear;
		}

		if (NormalizedType != TEXT("function") && NormalizedType != TEXT("callfunction"))
		{
			return EKind::None;
		}

		// 前缀长的排前面 ——「Unbind all Events from」也以「Unbind」开头
		struct FPrefix
		{
			const TCHAR* Prefix;
			EKind Kind;
		};
		static const FPrefix Prefixes[] = {
			{ TEXT("Unbind all Events from "), EKind::Clear },
			{ TEXT("Unbind all Events "),      EKind::Clear },
			{ TEXT("Unbind all "),             EKind::Clear },
			{ TEXT("Unbind Event from "),      EKind::Remove },
			{ TEXT("Unbind Event "),           EKind::Remove },
			{ TEXT("Unbind "),                 EKind::Remove },
			{ TEXT("Bind Event to "),          EKind::Add },
			{ TEXT("Bind Event "),             EKind::Add },
			{ TEXT("Bind "),                   EKind::Add },
			{ TEXT("Call "),                   EKind::Call }
		};

		for (const FPrefix& Entry : Prefixes)
		{
			const FString Prefix(Entry.Prefix);
			if (Name.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				OutDelegateName = Name.RightChop(Prefix.Len()).TrimStartAndEnd();
				return Entry.Kind;
			}
		}

		return EKind::None;
	}
}
