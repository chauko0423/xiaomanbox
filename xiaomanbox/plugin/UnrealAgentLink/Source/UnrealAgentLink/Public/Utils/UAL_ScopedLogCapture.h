#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Logging/LogVerbosity.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"

#include <initializer_list>

/**
 * 作用域内按类别 + 最低级别收日志行的 RAII 输出设备。
 *
 * ## 为什么要有它
 *
 * 引擎不少操作只返回一个 bool，真正的原因只写进了编辑器日志。典型的是
 * `IAssetTools::RenameAssets`：签不出、只读、不是最新版 —— 每一条它都知道，
 * 但 `ReportFailures(bWithDialog=false)` 只 `UE_LOG(LogAssetTools, Error, "{包名} - {原因}")`，
 * 用户和模型都看不到。挂一个临时输出设备把这段时间的相关行捞回响应里。
 *
 * 插件里已经有四个各自为政的 FOutputDevice 捕获（PIE、PCG、LiveCoding、LogInterceptor），
 * 这一份是给「一次同步调用前后」用的通用件，别再抄第五份。
 *
 * ## 线程
 *
 * 默认 `CanBeUsedOnAnyThread()` 为 false，所以 GLog 只在游戏线程上把行交给我们；
 * 别的线程打的行先被缓冲，析构前 `FlushThreadedLogs()` 一次把它们收进来。
 * 因此不需要锁。
 *
 * ## 上限
 *
 * 最多留 200 行、留最早的：第一条错误通常才是根因，后面多半是连锁反应。
 * 丢掉的行数记着，响应里如实报。
 */
class FUAL_ScopedLogCapture : public FOutputDevice
{
public:
	static constexpr int32 MaxLines = 200;

	/**
	 * @param InCategories   只收这些类别，如 {"LogAssetTools", "LogSourceControl"}
	 * @param InMinVerbosity 最低级别（含）。Warning 表示收 Warning / Error / Fatal
	 */
	FUAL_ScopedLogCapture(std::initializer_list<const TCHAR*> InCategories, ELogVerbosity::Type InMinVerbosity = ELogVerbosity::Warning)
		: MinVerbosity(InMinVerbosity)
	{
		for (const TCHAR* Category : InCategories)
		{
			Categories.Add(FName(Category));
		}
		if (GLog)
		{
			GLog->AddOutputDevice(this);
		}
	}

	virtual ~FUAL_ScopedLogCapture() override
	{
		if (GLog)
		{
			GLog->FlushThreadedLogs();
			GLog->RemoveOutputDevice(this);
		}
	}

	FUAL_ScopedLogCapture(const FUAL_ScopedLogCapture&) = delete;
	FUAL_ScopedLogCapture& operator=(const FUAL_ScopedLogCapture&) = delete;

	// GLog 调的是带 Time 的四参重载，基类默认转发到这个三参的；把其余重载带进来免得被隐藏
	using FOutputDevice::Serialize;

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		if (!Categories.Contains(Category))
		{
			return;
		}
		// 级别枚举越小越严重（Fatal=1, Error=2, Warning=3, ...），高位还可能带
		// SetColor / BreakOnLog 标志位，比较前先掩掉
		const ELogVerbosity::Type Level = static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
		if (Level == ELogVerbosity::NoLogging || Level > MinVerbosity)
		{
			return;
		}

		++TotalSeen;
		if (Lines.Num() >= MaxLines)
		{
			return;
		}
		Lines.Add(FString::Printf(TEXT("[%s] %s: %s"), *Category.ToString(), ToString(Level), V));
		if (Level == ELogVerbosity::Error || Level == ELogVerbosity::Fatal)
		{
			RawErrors.Add(FString(V));
		}
	}

	/** 已格式化的行：`[Category] Verbosity: text` */
	const TArray<FString>& GetLines() const { return Lines; }

	/** 超出上限被丢掉的行数 */
	int32 Dropped() const { return FMath::Max(0, TotalSeen - Lines.Num()); }

	/** Error / Fatal 行的**原文**（没有类别前缀），给按格式拆解用 */
	TArray<FString> ErrorsOnly() const { return RawErrors; }

	/** 直接放进响应的 `engine_log` 数组；丢了行就在末尾补一条说明 */
	TArray<TSharedPtr<FJsonValue>> ToJson() const
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Lines.Num() + 1);
		for (const FString& Line : Lines)
		{
			Out.Add(MakeShared<FJsonValueString>(Line));
		}
		if (Dropped() > 0)
		{
			Out.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("... %d more line(s) not captured (cap %d)"), Dropped(), MaxLines)));
		}
		return Out;
	}

private:
	TSet<FName> Categories;
	ELogVerbosity::Type MinVerbosity;
	TArray<FString> Lines;
	TArray<FString> RawErrors;
	int32 TotalSeen = 0;
};
