#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * `sequence.diff` —— 两条 Level Sequence 逐绑定、逐轨道对比（只读）。
 *
 * 单独一个文件，不塞进 UAL_SequencerCommands.cpp：那个文件已经两千多行，
 * 而对比只读、自成一体，和写关键帧的那几条命令没有共享状态。
 */
class FUAL_SequenceDiffCommands
{
public:
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& Commands);
};
