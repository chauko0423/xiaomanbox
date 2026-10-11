#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

/**
 * 「这个包能不能存」的唯一判据，以及在它之上数脏包。
 *
 * ## 为什么要抽出来
 *
 * 之前有四处各写了一份大同小异的过滤（editor.save、editor.get_dirty、
 * content.batch_move 的 dirty_after、content.fixup_redirectors 的 dirty_after），
 * 四份都只挡了 transient 和 `/Temp/`，**都漏了 `/Script/` 这类编译进来的包**。
 * 后果是三个入口给出三个互相矛盾的「未保存清单」：
 *
 *   - `content.fixup_redirectors` 报「还有 2 个包没保存，去 editor.save」；
 *   - `editor.save`（默认 scope=touched）说「没有需要你保存的改动」；
 *   - `editor.save scope=all` 去存那 2 个包，`/Script/SlateCore`、`/Script/MediaPlate`
 *     根本没有磁盘文件，`TryConvertLongPackageNameToFilename` 直接失败，
 *     整条命令报错 —— 用户看到的是「保存坏了」。
 *
 * 2026-09-09 真机上就是这么三连错的，模型最后放弃工具链路去翻磁盘。
 * 一处判据、四处共用，三个入口才可能对上账。
 *
 * ## `/Script/` 包为什么会是脏的
 *
 * 它们是模块编译进来的包（`PKG_CompiledIn`），装着 UClass / CDO 这些内存对象。
 * 编辑器改一次类默认值、动一次配置就可能把它标脏。它们**没有对应的磁盘文件**，
 * 任何时候都不该出现在「要保存的东西」里，也不该被数进「还有几处未保存」。
 *
 * 判据用 `PKG_CompiledIn` 标志（本机九个引擎逐版本核过：4.27 与 5.1–5.8 的
 * `ObjectMacros.h` 各命中，是 UE4 就有的老标志），外加一道 `/Script/` 前缀
 * 兜底 —— 两条只要中一条就不算可存。
 */

/** 这个包有没有对应的磁盘文件、该不该被我们保存 */
inline bool UAL_IsSavablePackage(const UPackage* Package)
{
	if (!Package || Package == GetTransientPackage())
	{
		return false;
	}

	// 编译进来的模块包（/Script/Engine、/Script/SlateCore …）没有磁盘文件。
	// 存它必然失败，数它必然让「还剩几处未保存」对不上账。
	if (Package->HasAnyPackageFlags(PKG_CompiledIn))
	{
		return false;
	}

	const FString Name = Package->GetName();
	if (Name.StartsWith(TEXT("/Script/")))
	{
		return false;
	}

	// `/Temp/` 下的未命名包（比如没保存过的新关卡）存不了 —— 存它需要用户
	// 先决定放哪儿，那是产品决策不是工具能替他做的
	return !Name.StartsWith(TEXT("/Temp/")) && !Name.StartsWith(TEXT("/Engine/Transient"));
}

/** 全引擎当前脏着、且真的能存的包 */
inline TArray<UPackage*> UAL_AllDirtySavablePackages()
{
	TArray<UPackage*> Dirty;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		UPackage* Package = *It;
		if (Package && Package->IsDirty() && UAL_IsSavablePackage(Package))
		{
			Dirty.Add(Package);
		}
	}
	return Dirty;
}

/** 同上，只要名字 —— 给「搬迁前后各数一次」这类比对用 */
inline TSet<FName> UAL_DirtySavablePackageNames()
{
	TSet<FName> Out;
	for (UPackage* Package : UAL_AllDirtySavablePackages())
	{
		Out.Add(Package->GetFName());
	}
	return Out;
}

/**
 * 快照之后新弄脏、且真的能存的包 —— 「这条命令新弄脏了什么」。
 *
 * 跑之前就脏的是用户自己改到一半的东西，不算命令的账，也不该劝调用方去存。
 * 差集写在这里一份，别再各命令各写一遍（batch_move 与 fixup_redirectors 都要）。
 */
inline TArray<FName> UAL_NewlyDirtySavablePackageNames(const TSet<FName>& Before)
{
	TArray<FName> Out;
	for (const FName& Name : UAL_DirtySavablePackageNames())
	{
		if (!Before.Contains(Name))
		{
			Out.Add(Name);
		}
	}
	return Out;
}

/**
 * 「这个脏包该不该拦下一次会丢东西的操作」—— 和上面那个是**两个**判断，别合并。
 *
 * 换关卡、重启编辑器会把内存里没存的东西全丢掉且无法撤销，所以默认要拦。
 * 但拦的判据不等于「能不能存」：
 *
 *   - `/Script/SlateCore` 这类模块包：存不了，也**不该拦** —— 用户没有任何办法
 *     让它变干净，拦了等于永远拦。
 *   - `/Temp/Untitled_1`：同样存不了（还没决定放哪），但**必须拦** ——
 *     那多半正是用户刚摆了半天 Actor 的新关卡，放过去就真没了。
 *
 * 两个判据方向相反，所以是两个函数。
 *
 * ## `/Script/SlateCore` 事件
 *
 * 实测（2026-08-31，UE 5.5）：`level.open` **每一次**都被 409 拦下，清单里唯一
 * 那条是 `/Script/SlateCore`。引擎在运行中把这类包标脏是家常便饭。后果比「多问
 * 一句」严重得多：用户被逼着每次都加 `force=true`，这道保护事实上永远放行，
 * 还养成了无脑 force 的习惯 —— 真有一张没保存的关卡时也一起丢了。
 *
 * 2026-09-09 复查发现 `editor.restart` 漏了这道修正（当时只改了 `level.open`），
 * 所以判据挪到这里共用，别再出现第三份。
 *
 * @param PackageName `UPackage::GetName()`，如 `/Script/SlateCore`、`/Game/Maps/Main`
 */
inline bool UAL_IsIgnorableDirtyPackage(const FString& PackageName)
{
	// 模块包。调用处还会看 PKG_CompiledIn 标志（同一件事的另一种问法），
	// 这里按名字判是为了这段逻辑本身能被单独测到
	if (PackageName.StartsWith(TEXT("/Script/")))
	{
		return true;
	}

	// transient 包：只活在内存里，存不下来，也不该拦着用户换关卡
	if (PackageName.StartsWith(TEXT("/Engine/Transient")))
	{
		return true;
	}

	return false;
}

/**
 * 会被「丢弃未保存改动」的操作波及的脏包，**按存不存得了分两栏**。
 *
 * 只排「用户存不了、也不该为它负责」的那些；排完之后剩下的必须如实列出来，
 * 否则这道保护就成了摆设。
 *
 * ## 为什么要分栏
 *
 * 拦得住 ≠ 存得了。`/Temp/Untitled_1` 和它底下的 `__ExternalActors__` 包
 * 正是这个组合：必须拦（那多半是用户刚摆了半天的新关卡），却过不了
 * `UAL_IsSavablePackage`（没决定放哪儿，`editor.save` 存不了它们）。
 *
 * 而三处拒绝信息一直只会说一句「先 editor.save，再重试」。落到这些包上
 * 那是**死循环**：存不动，重试还是被拦，调用方试完只剩「全丢」的
 * `force=true` 一条路 —— 而清单里往往同时躺着它刚做完、还没存的资产。
 * 2026-09-17 的实测反馈就是这么被逼到 force 的。
 *
 * 分栏之后，调用方能对两类说两句不同的话：能存的去存，存不了的要么
 * `level.save` 给个路径另存，要么明确丢弃。判据仍然复用
 * `UAL_IsSavablePackage`，不另写一份 —— 这个文件开头那笔「四处各写一份」
 * 的账不能再记第二次。
 */
struct FUAL_DestructiveOpDirty
{
	/** 能存：调用方先去 `editor.save` */
	TArray<FString> Savable;

	/** 存不了：没落过盘的临时包，只有「另存为」或「丢弃」两条路 */
	TArray<FString> Unsavable;

	int32 Num() const { return Savable.Num() + Unsavable.Num(); }
};

inline FUAL_DestructiveOpDirty UAL_DirtyPackagesForDestructiveOp()
{
	FUAL_DestructiveOpDirty Out;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		UPackage* Package = *It;
		if (!Package || !Package->IsDirty() || Package == GetTransientPackage())
		{
			continue;
		}
		// PKG_CompiledIn 就是「这个包是编进二进制的模块包」，比名字前缀更权威；
		// RF_Transient 的包按定义就不落盘。两个都是「存不下来」，不该拦人
		if (Package->HasAnyPackageFlags(PKG_CompiledIn) || Package->HasAnyFlags(RF_Transient))
		{
			continue;
		}
		const FString Name = Package->GetName();
		if (UAL_IsIgnorableDirtyPackage(Name))
		{
			continue;
		}
		(UAL_IsSavablePackage(Package) ? Out.Savable : Out.Unsavable).Add(Name);
	}
	return Out;
}

/**
 * 409 拒绝响应的 `details`，`level.open` / `level.new` / `editor.restart` 共用一份。
 *
 * 三处各拼各的必然漂移 —— 上一轮的 `/Script/` 修正就是只改了 `level.open`，
 * `editor.restart` 拖到 2026-09-09 才补上。
 *
 * `unsaved` 保持「全部包名的扁平数组」这个老形状：老版本的盒子只认这个字段，
 * 换形状会让它一句话都读不出来。分栏信息以**新增**字段 `unsavable` 给出，
 * 老盒子看不见它，行为退回今天的样子。
 */
inline TSharedPtr<FJsonObject> UAL_UnsavedRefusalDetails(const FUAL_DestructiveOpDirty& Dirty, int32 MaxListed = 50)
{
	TArray<TSharedPtr<FJsonValue>> AllJson;
	TArray<TSharedPtr<FJsonValue>> UnsavableJson;

	// 能存的排在前面：那才是调用方下一步该动手的部分
	for (const FString& Name : Dirty.Savable)
	{
		if (AllJson.Num() >= MaxListed)
		{
			break;
		}
		AllJson.Add(MakeShared<FJsonValueString>(Name));
	}
	for (const FString& Name : Dirty.Unsavable)
	{
		UnsavableJson.Add(MakeShared<FJsonValueString>(Name));
		if (AllJson.Num() >= MaxListed)
		{
			continue;
		}
		AllJson.Add(MakeShared<FJsonValueString>(Name));
	}

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetArrayField(TEXT("unsaved"), AllJson);
	Details->SetNumberField(TEXT("unsaved_count"), Dirty.Num());
	Details->SetArrayField(TEXT("unsavable"), UnsavableJson);
	Details->SetStringField(
		TEXT("hint"),
		Dirty.Unsavable.Num() > 0
			? TEXT("The packages listed in 'unsavable' have never been written to disk (an unsaved new level and its external actors), so editor.save cannot save them - retrying after a save will fail the same way. Give that level a path with level.save, or pass force=true to discard everything listed. Discarded changes cannot be undone.")
			: TEXT("Call editor.save first, or pass force=true to discard these changes. Discarded changes cannot be undone."));
	return Details;
}
