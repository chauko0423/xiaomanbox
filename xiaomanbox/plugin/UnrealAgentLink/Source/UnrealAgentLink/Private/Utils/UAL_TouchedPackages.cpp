#include "UAL_TouchedPackages.h"

#include "UAL_AgentUndo.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALTouched, Log, All);

namespace
{
	/**
	 * 存包名而不是指针。
	 *
	 * 包对象会被 GC，弱指针一失效就只剩「有个东西没了」，连是哪个都说不出。
	 * 存名字则随时能 `FindPackage` 回来，找不到就说明它已经不在内存里 ——
	 * 那种情况下本来也没什么可存的。
	 */
	TSet<FName> GTouchedPackageNames;

	/** 大于 0 表示正在执行 UAL 命令，此时才记录 */
	int32 GScopeDepth = 0;

	FDelegateHandle GTouchedDirtyHandle;

	void OnTouchedPackageMarkedDirty(UPackage* Package, bool /*bWasDirty*/)
	{
		if (GScopeDepth <= 0 || !Package)
		{
			// 用户自己在编辑器里改的东西走这条路径被丢掉 —— 这正是目的
			return;
		}

		// 临时包（未落盘的 /Temp/、/Engine/Transient）存不了也不该存
		if (Package == GetTransientPackage() || Package->HasAnyFlags(RF_Transient))
		{
			return;
		}

		GTouchedPackageNames.Add(Package->GetFName());
	}
}

void FUAL_TouchedPackages::Initialize()
{
	if (GTouchedDirtyHandle.IsValid())
	{
		return;
	}
	GTouchedDirtyHandle = UPackage::PackageMarkedDirtyEvent.AddStatic(&OnTouchedPackageMarkedDirty);
}

void FUAL_TouchedPackages::Shutdown()
{
	if (GTouchedDirtyHandle.IsValid())
	{
		UPackage::PackageMarkedDirtyEvent.Remove(GTouchedDirtyHandle);
		GTouchedDirtyHandle.Reset();
	}
	GTouchedPackageNames.Empty();
	GScopeDepth = 0;
}

void FUAL_TouchedPackages::PushScope()
{
	++GScopeDepth;
}

void FUAL_TouchedPackages::PopScope()
{
	// 不让它掉到负数：一次配对失误会把记录窗口永久关死，
	// 表现为「保存工具永远说没有改动」，而那非常难查
	GScopeDepth = FMath::Max(0, GScopeDepth - 1);
}

bool FUAL_TouchedPackages::IsInCommandScope()
{
	return GScopeDepth > 0;
}

void FUAL_TouchedPackages::Touch(const UPackage* Package)
{
	if (Package && Package != GetTransientPackage())
	{
		GTouchedPackageNames.Add(Package->GetFName());
	}
}

TArray<UPackage*> FUAL_TouchedPackages::CollectDirty()
{
	TArray<UPackage*> Result;
	TArray<FName> Stale;

	/**
	 * 先把 agent 撤销栈上的包并进来。
	 *
	 * 包标脏事件那条路记不到「命令里派发出去、后续帧才落地」的异步写 ——
	 * 那时候作用域早关了。撤销栈没这个问题，事务什么时候结束就什么时候进栈。
	 *
	 * 并进 GTouchedPackageNames 而不是单独拼一份结果：底下那段循环已经在做
	 * 「还活着吗、还脏吗、不脏就剔掉」的过滤，两条来源该走同一套过滤。
	 */
	for (const FString& PackagePath : FUAL_AgentUndo::CollectTouchedPackagePaths())
	{
		GTouchedPackageNames.Add(FName(*PackagePath));
	}

	for (const FName& Name : GTouchedPackageNames)
	{
		UPackage* Package = FindPackage(nullptr, *Name.ToString());
		if (!Package)
		{
			// 已经不在内存里了，留着只会让集合越攒越大
			Stale.Add(Name);
			continue;
		}
		if (!Package->IsDirty())
		{
			// 已经被存过（可能是用户自己按的 Ctrl+S），不用再存一遍
			Stale.Add(Name);
			continue;
		}
		Result.Add(Package);
	}

	for (const FName& Name : Stale)
	{
		GTouchedPackageNames.Remove(Name);
	}

	return Result;
}

int32 FUAL_TouchedPackages::Num()
{
	return GTouchedPackageNames.Num();
}

void FUAL_TouchedPackages::Clear()
{
	GTouchedPackageNames.Empty();

	// 撤销栈那条来源不受 Empty 影响 —— 事务还在栈上。打个水位线告诉它
	// 「到这儿为止的都处理过了」，否则下一次 CollectDirty 又会把它们捞回来
	FUAL_AgentUndo::MarkSeen();
}
