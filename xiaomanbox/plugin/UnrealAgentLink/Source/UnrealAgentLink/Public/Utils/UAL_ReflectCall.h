#pragma once

#include "CoreMinimal.h"
#include "UObject/UnrealType.h"

/**
 * 靠反射调用「可选插件」的 API，不产生链接期依赖。
 *
 * ## 为什么需要这层
 *
 * 我们发的是预编译二进制（九个引擎版本各一份 zip），装到用户自己的工程里。
 * 只要 Build.cs 里写了 `PrivateDependencyModuleNames.Add("PCG")`，生成的 DLL
 * 就会硬导入 UnrealEditor-PCG.dll —— 用户工程没开 PCG 插件时，整个
 * UnrealAgentLink 都加载不起来。为了一个可选功能把所有老用户搞挂，不划算。
 *
 * 另一条路是在 .uplugin 里写 `{"Name":"PCG","Enabled":true}`，但那会**强制**
 * 给每个装了盒子的工程打开 PCG —— 同样是把我们的功能塞进别人的工程配置。
 *
 * 所以走反射：`/Script/PCG.PCGGraph` 这种路径在 PCG 没启用时 FindObject 返回
 * nullptr，命令干净地回一句「PCG 未启用」，主模块毫发无伤。
 *
 * ## 前提：目标 API 必须是反射可见的
 *
 * 只有 `UFUNCTION` 和 `UPROPERTY` 能这么调。用之前必须逐版本去引擎源码里确认，
 * 别凭印象。PCG 这边确认过的（5.6 / 5.7 / 5.8 逐字相同）：
 *   UPCGGraph::AddNodeOfType / AddEdge / RemoveNode / RemoveEdge   UFUNCTION(BlueprintCallable)
 *   UPCGGraph::Nodes / InputNode / OutputNode                      UPROPERTY
 *   UPCGNode::InputPins / OutputPins / SettingsInterface / PositionX / PositionY  UPROPERTY
 *   UPCGPin::Properties.Label / Edges                              UPROPERTY
 *   UPCGEdge::InputPin / OutputPin                                 UPROPERTY
 *
 * ## 参数缓冲为什么按 UFunction 的属性表动态构建
 *
 * 手写一个跟目标函数签名对应的 struct 也能喂给 ProcessEvent，但那要求我们猜对
 * 成员的大小和对齐 —— FName 的大小就随 UE_FNAME_OUTLINE_NUMBER 变。按属性表
 * 逐个 SetValue_InContainer 是唯一跨版本安全的做法。
 */
namespace UALReflect
{
	/**
	 * 按 `/Script/模块.类名` 查类。插件没启用时返回 nullptr。
	 * 不用 ANY_PACKAGE —— 那个在 5.1 起废弃，且同名类会撞。
	 */
	UClass* FindScriptClass(const TCHAR* PackageDotClass);

	/** 查某个类的全部非抽象子类（含自身之外的所有层级） */
	void FindDerivedClasses(UClass* Base, TArray<UClass*>& OutClasses);

	// ------------------------------------------------------------------
	// UPROPERTY 读
	// ------------------------------------------------------------------

	UObject* GetObjectProp(const UObject* Obj, const TCHAR* PropName);
	bool GetObjectArrayProp(const UObject* Obj, const TCHAR* PropName, TArray<UObject*>& Out);
	bool GetIntProp(const UObject* Obj, const TCHAR* PropName, int32& Out);
	bool GetBoolProp(const UObject* Obj, const TCHAR* PropName, bool& Out);
	bool GetNameProp(const UObject* Obj, const TCHAR* PropName, FName& Out);
	bool GetStringProp(const UObject* Obj, const TCHAR* PropName, FString& Out);

	/**
	 * 读结构体成员里的 FName，如 UPCGPin::Properties.Label。
	 * 只走一层结构体，够用且不用做路径解析。
	 */
	bool GetNameInStructProp(const UObject* Obj, const TCHAR* StructPropName, const TCHAR* MemberName, FName& Out);
	bool GetBoolInStructProp(const UObject* Obj, const TCHAR* StructPropName, const TCHAR* MemberName, bool& Out);

	// ------------------------------------------------------------------
	// UPROPERTY 写
	// ------------------------------------------------------------------

	bool SetIntProp(UObject* Obj, const TCHAR* PropName, int32 Value);
	bool SetStringProp(UObject* Obj, const TCHAR* PropName, const FString& Value);
	bool SetNameProp(UObject* Obj, const TCHAR* PropName, FName Value);
	/** 对象引用属性；Value 的类必须与属性声明的类兼容，不兼容时返回 false 不写 */
	bool SetObjectProp(UObject* Obj, const TCHAR* PropName, UObject* Value);

	/**
	 * 一次 UFUNCTION 调用。
	 *
	 * 用法：
	 *   UALReflect::FCall Call(Graph, TEXT("AddEdge"));
	 *   Call.Obj(TEXT("From"), FromNode).Name(TEXT("FromPinLabel"), TEXT("Out"));
	 *   if (Call.Invoke()) { UObject* NewNode = Call.OutObject(TEXT("ReturnValue")); }
	 *
	 * 参数名必须和引擎源码里的形参名逐字一致 —— 反射按名字找，拼错不会报错，
	 * 会静默传默认值。所以 Set* 系列在找不到形参时返回 false 并记 Warning。
	 */
	class FCall
	{
	public:
		FCall(UObject* InTarget, const TCHAR* FuncName);
		~FCall();

		FCall(const FCall&) = delete;
		FCall& operator=(const FCall&) = delete;

		/** 目标对象和函数都找到了才为真 */
		bool IsValid() const { return Target != nullptr && Function != nullptr; }

		/** 找不到函数时的说明，用于给调用方拼错误信息 */
		const FString& GetError() const { return Error; }

		FCall& Obj(const TCHAR* ParamName, UObject* Value);
		FCall& Cls(const TCHAR* ParamName, UClass* Value);
		FCall& Name(const TCHAR* ParamName, FName Value);
		FCall& Bool(const TCHAR* ParamName, bool Value);

		/**
		 * 数值形参。**float 和 double 都要认。**
		 *
		 * 同一个引擎函数的形参类型会跨版本变：`MakeInputActionValue` 的 X/Y/Z
		 * 在 5.0 上是 float，5.1 起是 double。只写一种，另一端会**静默传 0** ——
		 * 形参名对得上、类型对不上，CastField 返回空，没有任何报错。
		 */
		FCall& Num(const TCHAR* ParamName, double Value);

		/** 枚举形参。UHT 按声明形态给出 FByteProperty 或 FEnumProperty，两种都要认 */
		FCall& Enum(const TCHAR* ParamName, uint8 Value);

		/**
		 * 结构体形参：从别处**整块拷**进来。
		 *
		 * ## 为什么需要这个，而不是逐成员赋值
		 *
		 * 本文件开头说「按属性表逐个 SetValue_InContainer 是唯一跨版本安全的做法」，
		 * 那句话有个前提：**结构体的成员是 UPROPERTY**。
		 *
		 * `FInputActionValue` 不是。它是 `USTRUCT(BlueprintType)`，但内部
		 * 一个 UPROPERTY 都没有（九版实测），标的是 HasNativeMake/HasNativeBreak，
		 * 蓝图靠原生 Make 函数造它。于是拿不到成员的 FProperty，逐成员赋值这条路
		 * **在它身上直接不成立**。
		 *
		 * 唯一干净的解法是让引擎自己造好，我们只搬字节：
		 *   Make 函数调一次 → OutStruct 拿到返回的结构体 → StructFrom 搬进下一次调用。
		 *
		 * 类型不同就拒绝，不硬搬 —— 搬错了不会报错，只会在运行时给出乱值。
		 */
		FCall& StructFrom(const TCHAR* ParamName, const FStructProperty* SrcProp, const void* SrcValueAddr);

		/** 真正执行。IsValid() 为假时直接返回 false */
		bool Invoke();

		/** 取出参（含返回值，形参名固定叫 ReturnValue）。没调用过或类型不符返回 nullptr */
		UObject* OutObject(const TCHAR* ParamName) const;
		bool OutBool(const TCHAR* ParamName, bool DefaultValue = false) const;

		/**
		 * 取出参里的结构体：把属性和它在缓冲区里的地址一起给出去，供 StructFrom 搬运。
		 *
		 * ⚠️ 地址指向本对象的参数缓冲。**FCall 一析构就失效**，
		 * 要搬就在同一个作用域里搬完。
		 */
		bool OutStruct(const TCHAR* ParamName, const FStructProperty*& OutProp, const void*& OutAddr) const;

	private:
		FProperty* FindParam(const TCHAR* ParamName) const;

		UObject* Target = nullptr;
		UFunction* Function = nullptr;
		TArray<uint8> Buffer;
		FString Error;
		bool bInvoked = false;
	};
}
