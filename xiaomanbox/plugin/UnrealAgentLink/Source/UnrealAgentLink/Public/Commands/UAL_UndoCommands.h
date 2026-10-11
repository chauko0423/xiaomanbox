#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 撤销命令 —— 只作用在 agent 自己那条撤销栈上。
 *
 * 机制见 UAL_AgentUndo.h。要点：agent 的每一步都进它自己的缓冲，
 * **用户的撤销历史一步都不碰**。所以这里的「全撤」是「把 AI 干的全撤了」，
 * 不会连带把用户自己做的操作也撤掉。
 *
 * 反过来也成立：用户手动做的事不在 agent 栈上，这些命令撤不到它们。
 */
class FUAL_UndoCommands
{
public:
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	/**
	 * editor.undo_history —— agent 到目前为止做了哪几步、每步动了哪些资产。
	 *
	 * 请求: { "limit": 20 }          // 只要最近几步，默认 20，<=0 表示全要
	 * 响应: { ok, undoable, redoable, total,
	 *         entries: [{ step, title, context, packages: [...] }] }
	 *
	 * entries 里 step 越大越新，撤销从最大的那个开始往回走。
	 */
	static void Handle_UndoHistory(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * editor.undo —— 撤销 agent 做过的步骤。
	 *
	 * 请求: { "steps": 1 }           // 省略或 <=0 表示全撤
	 * 响应: { ok, action: "undo", steps_applied, step_titles: [...],
	 *         remaining, redoable, affected_packages: [...], affected_objects: [...] }
	 *
	 * affected_packages 是撤销动过的包，撤完它们会变脏（因为内存里的内容跟
	 * 磁盘上的又不一样了），需要再 editor.save 一次才算真的回退到位。
	 *
	 * affected_objects 是同一批事务里动过的对象，Actor 用大纲里的名字。
	 * 只有包名的话看不出「关卡里的哪个东西变了」，而撤销恰恰可能把 Actor 撤没。
	 */
	static void Handle_Undo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * editor.redo —— 把撤销掉的步骤重做回来。参数和响应结构同 editor.undo。
	 *
	 * 存在的理由：撤销要是不可逆，那它本身就成了一个不敢按的按钮。
	 */
	static void Handle_Redo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
};
