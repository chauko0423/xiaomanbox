// Copyright uebox.ai
//
// ⚠️ 一次性探针 —— 第 0 步验完即删，不要在此基础上写功能。
//
// 目的：证实或证伪 第 0 步的六个假设。
// 那六条撑着后面约六周的工作量，而全部只是**读源码推出来的**，一次都没真跑过。
//
//   0a  PIE 里注入「动作」，角色真的会动吗？反射造 FInputActionValue 那条链路走得通吗？
//   0b  暂停 → 恢复 → 注入 → 再暂停，动作会被吃掉还是重复触发？
//   0c  从视口发一次真实按键，能不能走完键位映射把角色驱动起来？
//   0d  编辑器失焦被节流时，PIE 一秒还能跑几帧？
//   0e  游戏处于 UIOnly（菜单打开）时，动作注入还生效吗？视口按键呢？
//   0f  旧输入系统（Action/Axis Mapping）上，视口按键能不能驱动？
//
// 0e 是最重要的一条。设计文档 §12.8 从两处源码推出「UI 抢了输入时我们照样注入得进去，
// 而玩家一动不动」—— 如果成立，这套工具最大的风险就不是做不出来，是**做出来会骗人**。
// 推断再确定也只是推断，必须真跑一次。
//
// ## 三条刻意的取舍
//
// 1. **全程走反射**（UALReflect），不给 Build.cs 加 EnhancedInput 依赖。
//    硬链会让用户工程没启用该插件时整个 UnrealAgentLink 加载不起来 ——
//    UAL_ReflectCall.h 开头已经为 PCG 否决过这笔交易。探针要验的正是
//    「反射这条路走不走得通」，所以它自己更不能走捷径。
//
// 2. **不猜资产名，也不信顺序。** 不去找叫 IA_Move 的东西，而是把运行时
//    EnhancedActionMappings 里**全部** Axis2D 动作逐个注入一遍，谁真的推动了角色谁算数。
//    两个测试工程的输入资产路径和数量都不一样（5.8 在 /Game/Input/，
//    5.3 在 /Game/ThirdPerson/Input/），硬编路径的探针换个工程就废了；
//    而第一版「挑第一个 Axis2D」在 5.8 上挑中了 IA_MouseLook，直接给出假否定。
//
// 3. **每段都自带对照组，且基线在前。** 「空转 30 帧量基线 → 注入 30 帧」。
//    基线必须在注入**之前** —— 放在之后会量到角色的刹车惯性（实测 55–85 单位），
//    把成功的样本判成失败。
//
// ## 用法
//
//   1) 打开工程，按 Play（PIE 必须已经在跑）
//   2) 编辑器控制台敲：UAL.PieInputProbe
//   3) 把 LogUALPieProbe 的输出整段贴回来
//
// 单独跑某一段：UAL.PieInputProbe l1 / uionly / viewport / pause / throttle / legacy / map

#include "CoreMinimal.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
// UPlayerInput 的完整定义：只有前向声明时，TObjectPtr<UPlayerInput>::Get() 拿到的
// T* 没法转成 UObject*（编译器不知道继承关系）
#include "GameFramework/PlayerInput.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"
// 见 UAL_InputCommands.cpp 同处注释：5.0/5.1 没有 InputKeyEventArgs.h，
// FInputKeyEventArgs 在 UnrealClient.h 里；5.2+ 由 UnrealClient.h 转include。
#include "UnrealClient.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "UObject/UnrealType.h"

#include "UAL_ReflectCall.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALPieProbe, Log, All);

namespace
{
	// ========================================================================
	// 反射调用：本地版
	// ========================================================================
	//
	// 为什么不直接扩 UALReflect::FCall：
	//
	// 它现在只有 Obj / Cls / Name / Bool 四个入参 setter，缺我们需要的
	// 数值、枚举和**结构体**。而结构体那条是真正的拦路虎 ——
	// FInputActionValue 是 USTRUCT 但**内部一个 UPROPERTY 都没有**
	// （九版实测），「按属性表逐个赋值」这条 UALReflect 赖以跨版本安全的
	// 路子在它身上直接不成立，只能让引擎自己造好再整块搬字节。
	//
	// 探针是要被删掉的，往共用文件里塞东西会给别的会话添乱。
	// 等 0a 通过，再把这几个 setter 正式提进 UAL_ReflectCall（落地路径第 2.5 步）。

	/** 一次 UFUNCTION 调用。参数缓冲的构造/销毁照抄 UALReflect::FCall 的写法 */
	class FProbeCall
	{
	public:
		FProbeCall(UObject* InTarget, const TCHAR* FuncName)
		{
			if (!InTarget || !FuncName)
			{
				Error = TEXT("target or function name is null");
				return;
			}

			UFunction* Found = InTarget->FindFunction(FName(FuncName));
			if (!Found)
			{
				Error = FString::Printf(TEXT("%s 上没有反射函数 '%s'"),
					*InTarget->GetClass()->GetName(), FuncName);
				return;
			}

			Target = InTarget;
			Function = Found;

			// 光 Memzero 对 TArray/FString 这类有构造语义的形参不安全
			Buffer.SetNumUninitialized(Function->ParmsSize);
			FMemory::Memzero(Buffer.GetData(), Function->ParmsSize);
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				It->InitializeValue_InContainer(Buffer.GetData());
			}
		}

		~FProbeCall()
		{
			if (!Function || Buffer.Num() == 0)
			{
				return;
			}
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				It->DestroyValue_InContainer(Buffer.GetData());
			}
		}

		FProbeCall(const FProbeCall&) = delete;
		FProbeCall& operator=(const FProbeCall&) = delete;

		bool IsValid() const { return Target != nullptr && Function != nullptr; }
		const FString& GetError() const { return Error; }

		FProperty* FindParam(const TCHAR* ParamName) const
		{
			if (!Function || !ParamName)
			{
				return nullptr;
			}
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				if (It->GetName() == ParamName)
				{
					return *It;
				}
			}
			return nullptr;
		}

		FProbeCall& Obj(const TCHAR* ParamName, UObject* Value)
		{
			if (FObjectPropertyBase* Prop = CastField<FObjectPropertyBase>(FindParam(ParamName)))
			{
				Prop->SetObjectPropertyValue(Prop->ContainerPtrToValuePtr<void>(Buffer.GetData()), Value);
			}
			else
			{
				Warn(ParamName, TEXT("object"));
			}
			return *this;
		}

		/**
		 * 数值形参。**float 和 double 都要认** ——
		 * MakeInputActionValue 的 X/Y/Z 在 5.0 上是 float，5.1 起是 double。
		 * 只写一种，另一端会静默传 0（形参名对得上、类型对不上，Cast 返回空）。
		 */
		FProbeCall& Num(const TCHAR* ParamName, double Value)
		{
			FProperty* Param = FindParam(ParamName);
			if (FDoubleProperty* AsDouble = CastField<FDoubleProperty>(Param))
			{
				AsDouble->SetPropertyValue(AsDouble->ContainerPtrToValuePtr<void>(Buffer.GetData()), Value);
			}
			else if (FFloatProperty* AsFloat = CastField<FFloatProperty>(Param))
			{
				AsFloat->SetPropertyValue(AsFloat->ContainerPtrToValuePtr<void>(Buffer.GetData()),
					static_cast<float>(Value));
			}
			else
			{
				Warn(ParamName, TEXT("float/double"));
			}
			return *this;
		}

		/** 枚举形参。UHT 会按声明形态给出 FByteProperty 或 FEnumProperty，两种都要认 */
		FProbeCall& Enum(const TCHAR* ParamName, uint8 Value)
		{
			FProperty* Param = FindParam(ParamName);
			if (FByteProperty* AsByte = CastField<FByteProperty>(Param))
			{
				AsByte->SetPropertyValue(AsByte->ContainerPtrToValuePtr<void>(Buffer.GetData()), Value);
			}
			else if (FEnumProperty* AsEnum = CastField<FEnumProperty>(Param))
			{
				void* Addr = AsEnum->ContainerPtrToValuePtr<void>(Buffer.GetData());
				AsEnum->GetUnderlyingProperty()->SetIntPropertyValue(Addr, static_cast<int64>(Value));
			}
			else
			{
				Warn(ParamName, TEXT("enum"));
			}
			return *this;
		}

		/**
		 * 结构体形参：从别处整块拷进来。
		 *
		 * 这是 FInputActionValue 唯一能走的路 —— 它没有 UPROPERTY，
		 * 拿不到成员的 FProperty，只能保证「同一个 UScriptStruct」然后按字节搬。
		 * 类型不同就拒绝，不硬搬：搬错了不会报错，只会在运行时给出乱值。
		 */
		FProbeCall& StructFrom(const TCHAR* ParamName, const FStructProperty* SrcProp, const void* SrcValueAddr)
		{
			FStructProperty* Dst = CastField<FStructProperty>(FindParam(ParamName));
			if (!Dst || !SrcProp || !SrcValueAddr)
			{
				Warn(ParamName, TEXT("struct"));
				return *this;
			}
			if (Dst->Struct != SrcProp->Struct)
			{
				UE_LOG(LogUALPieProbe, Warning,
					TEXT("形参 '%s' 是 %s，源是 %s —— 类型不同，不搬"),
					ParamName,
					Dst->Struct ? *Dst->Struct->GetName() : TEXT("<null>"),
					SrcProp->Struct ? *SrcProp->Struct->GetName() : TEXT("<null>"));
				return *this;
			}
			Dst->Struct->CopyScriptStruct(Dst->ContainerPtrToValuePtr<void>(Buffer.GetData()), SrcValueAddr);
			return *this;
		}

		bool Invoke()
		{
			if (!IsValid())
			{
				return false;
			}
			Target->ProcessEvent(Function, Buffer.GetData());
			bInvoked = true;
			return true;
		}

		/** 取出参里的结构体：把属性和它在缓冲里的地址一起给出去，供 StructFrom 搬运 */
		bool OutStruct(const TCHAR* ParamName, const FStructProperty*& OutProp, const void*& OutAddr) const
		{
			if (!bInvoked)
			{
				return false;
			}
			const FStructProperty* Prop = CastField<FStructProperty>(FindParam(ParamName));
			if (!Prop)
			{
				return false;
			}
			OutProp = Prop;
			OutAddr = Prop->ContainerPtrToValuePtr<void>(const_cast<uint8*>(Buffer.GetData()));
			return true;
		}

	private:
		void Warn(const TCHAR* ParamName, const TCHAR* Kind) const
		{
			// 形参名拼错不会让 ProcessEvent 报错，只会静默传默认值 —— 必须自己喊出来
			UE_LOG(LogUALPieProbe, Warning, TEXT("%s: 没有 %s 形参 '%s'"),
				Function ? *Function->GetName() : TEXT("<no function>"), Kind, ParamName);
		}

		UObject* Target = nullptr;
		UFunction* Function = nullptr;
		TArray<uint8> Buffer;
		FString Error;
		bool bInvoked = false;
	};

	// ========================================================================
	// 场景解析
	// ========================================================================

	UWorld* FindPieWorld()
	{
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.World())
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	/** 探针跑一次要用到的全部现场对象。缺一个就没法往下走，所以一起解析、一起校验 */
	struct FProbeScene
	{
		UWorld* World = nullptr;
		APlayerController* PC = nullptr;
		APawn* Pawn = nullptr;
		ULocalPlayer* LocalPlayer = nullptr;
		UObject* EnhancedSubsystem = nullptr;   // UEnhancedInputLocalPlayerSubsystem
		UObject* PlayerInput = nullptr;         // UEnhancedPlayerInput（老工程上可能不是）
		UGameViewportClient* Viewport = nullptr;

		bool IsPlayable() const { return World && PC && Pawn; }
	};

	bool ResolveScene(FProbeScene& Out)
	{
		Out.World = FindPieWorld();
		if (!Out.World)
		{
			UE_LOG(LogUALPieProbe, Error, TEXT("PIE 没在跑 —— 先按 Play，再敲这条命令"));
			return false;
		}

		Out.PC = Out.World->GetFirstPlayerController();
		if (!Out.PC)
		{
			UE_LOG(LogUALPieProbe, Error, TEXT("PIE 世界里没有 PlayerController"));
			return false;
		}

		// 这一条本身就是设计里 §13.1 第 4 条的现场演示：PIE 刚起来时
		// PlayerController 已经在了、Pawn 还没 Possess，这时候注入什么都没反应
		Out.Pawn = Out.PC->GetPawn();
		if (!Out.Pawn)
		{
			UE_LOG(LogUALPieProbe, Error,
				TEXT("PlayerController 还没 Possess 任何 Pawn —— 等几秒再跑，或者这个工程没有可控角色"));
			return false;
		}

		Out.LocalPlayer = Out.PC->GetLocalPlayer();
		// .Get()：TObjectPtr<UPlayerInput> 到 UObject* 要过两次用户自定义转换，编译器不认
		Out.PlayerInput = Out.PC->PlayerInput.Get();
		Out.Viewport = Out.World->GetGameViewport();

		if (Out.LocalPlayer)
		{
			if (UClass* SubsystemClass = UALReflect::FindScriptClass(
					TEXT("/Script/EnhancedInput.EnhancedInputLocalPlayerSubsystem")))
			{
				Out.EnhancedSubsystem = Out.LocalPlayer->GetSubsystemBase(SubsystemClass);
			}
		}

		UE_LOG(LogUALPieProbe, Display,
			TEXT("[现场] 世界=%s｜PC=%s｜Pawn=%s (%s)｜EnhancedInput 子系统=%s｜视口=%s"),
			*Out.World->GetName(),
			*Out.PC->GetName(),
			*Out.Pawn->GetName(),
			*Out.Pawn->GetClass()->GetName(),
			Out.EnhancedSubsystem ? TEXT("有") : TEXT("没有（旧输入系统工程？）"),
			Out.Viewport ? TEXT("有") : TEXT("没有"));

		return true;
	}

	// ========================================================================
	// A 段：当前生效的键位表（顺带验证 §5.2 的反射读法）
	// ========================================================================

	struct FProbeMapping
	{
		const UObject* Action = nullptr;
		FKey Key;
	};

	/** 读 UEnhancedPlayerInput::EnhancedActionMappings（UPROPERTY(Transient) 数组，九版全有） */
	bool ReadActionMappings(UObject* PlayerInput, TArray<FProbeMapping>& Out)
	{
		if (!PlayerInput)
		{
			return false;
		}
		FArrayProperty* ArrayProp =
			FindFProperty<FArrayProperty>(PlayerInput->GetClass(), TEXT("EnhancedActionMappings"));
		if (!ArrayProp)
		{
			return false;
		}
		const FStructProperty* ElemProp = CastField<FStructProperty>(ArrayProp->Inner);
		if (!ElemProp || !ElemProp->Struct)
		{
			return false;
		}

		FObjectPropertyBase* ActionProp =
			FindFProperty<FObjectPropertyBase>(ElemProp->Struct, TEXT("Action"));
		FStructProperty* KeyProp = FindFProperty<FStructProperty>(ElemProp->Struct, TEXT("Key"));

		FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(PlayerInput));
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			const uint8* Elem = Helper.GetRawPtr(Index);
			FProbeMapping Mapping;
			if (ActionProp)
			{
				Mapping.Action = ActionProp->GetObjectPropertyValue_InContainer(Elem);
			}
			if (KeyProp)
			{
				Mapping.Key = *KeyProp->ContainerPtrToValuePtr<FKey>(Elem);
			}
			Out.Add(Mapping);
		}
		return true;
	}

	/**
	 * 读「此刻挂着哪些输入上下文」。
	 *
	 * ## 又一个「编得过、跑得动、永远返回空」
	 *
	 * 这个属性 5.6 起换了名字**和类型**：
	 *
	 *   · 5.1 – 5.5：`AppliedInputContexts`     `TMap<IMC*, int32>`                 ← 真数据
	 *   · 5.6 – 5.8：`AppliedInputContextData`  `TMap<IMC*, FAppliedInputContextData>` ← 真数据
	 *                `AppliedInputContexts` 还在（标了 UE_DEPRECATED(5.6)），但**是空的**
	 *
	 * 第一次真机跑（2026-09-03，5.8）就撞上了：反射读到老名字、拿回一张空表，
	 * 探针报「一个输入上下文都没挂」，而同一次运行的 EnhancedActionMappings 里
	 * 明明有 13 条映射 —— 自相矛盾才让人发现的。
	 *
	 * 要是这样上线，`ue_input_map` 会在**所有 5.6/5.7/5.8 工程**上说
	 * 「这个游戏没挂任何输入上下文」，而且不报错。
	 * 和 UAL_VersionCompat.h 里 LocateBoundObjects / GetUsedTextures 是同一类坑。
	 */
	void LogAppliedContexts(UObject* PlayerInput)
	{
		if (!PlayerInput)
		{
			return;
		}

		// 新名字优先。名字在不在是编译器/反射能直接回答的事实，比版本号可靠
		FMapProperty* MapProp =
			FindFProperty<FMapProperty>(PlayerInput->GetClass(), TEXT("AppliedInputContextData"));
		const TCHAR* SourceName = TEXT("AppliedInputContextData");
		if (!MapProp)
		{
			MapProp = FindFProperty<FMapProperty>(PlayerInput->GetClass(), TEXT("AppliedInputContexts"));
			SourceName = TEXT("AppliedInputContexts");
		}
		if (!MapProp)
		{
			UE_LOG(LogUALPieProbe, Warning, TEXT("[A 键位表] 两个名字都读不到"));
			return;
		}
		UE_LOG(LogUALPieProbe, Display, TEXT("[A 键位表] 上下文表读自 %s"), SourceName);

		FObjectPropertyBase* KeyProp = CastField<FObjectPropertyBase>(MapProp->KeyProp);
		// 5.6+ 的值是结构体（FAppliedInputContextData）不是 int，优先级藏在里面、名字还叫 Priority。
		// 只认 int 的话，5.6/5.7/5.8 上 ValueProp 恒为空，下面会给每一条都印「优先级 0」——
		// 而这个探针存在的理由恰恰是查「是不是被高优先级上下文盖住了」，
		// 编出来的 0 比不报更坏。和 UAL_InputCommands.cpp 的 ReadAppliedContexts 同一套判法
		FNumericProperty* ValueProp = CastField<FNumericProperty>(MapProp->ValueProp);
		FStructProperty* ValueStruct = CastField<FStructProperty>(MapProp->ValueProp);
		FNumericProperty* InnerPriority =
			ValueStruct ? FindFProperty<FNumericProperty>(ValueStruct->Struct, TEXT("Priority")) : nullptr;

		FScriptMapHelper Helper(MapProp, MapProp->ContainerPtrToValuePtr<void>(PlayerInput));
		int32 Count = 0;
		for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
		{
			if (!Helper.IsValidIndex(Index))
			{
				continue;
			}
			UObject* Context = KeyProp ? KeyProp->GetObjectPropertyValue(Helper.GetKeyPtr(Index)) : nullptr;
			// 取值器按 IsFloatingPoint() 分流：属性是按名字反射出来的，
			// 类型不由这里决定，而 GetSignedIntPropertyValue 里是
			// check(TIsIntegral<TCppType>::Value) —— 猜错了是 assert 崩编辑器，
			// 不是读到一个错的数（同 UAL_InputCommands.cpp 里那处）
			const auto ReadPriority = [](const FNumericProperty* Prop, const void* ValuePtr) -> int64
			{
				return Prop->IsFloatingPoint()
						   ? static_cast<int64>(Prop->GetFloatingPointPropertyValue(ValuePtr))
						   : Prop->GetSignedIntPropertyValue(ValuePtr);
			};

			bool bHasPriority = false;
			int64 Priority = 0;
			if (ValueProp)
			{
				Priority = ReadPriority(ValueProp, Helper.GetValuePtr(Index));
				bHasPriority = true;
			}
			else if (InnerPriority)
			{
				Priority = ReadPriority(
					InnerPriority, InnerPriority->ContainerPtrToValuePtr<void>(Helper.GetValuePtr(Index)));
				bHasPriority = true;
			}

			// 读不出来就说读不出来。印一个 0 会被当成「这条优先级最低」，
			// 而这一行正是用来判优先级冲突的
			if (bHasPriority)
			{
				UE_LOG(LogUALPieProbe, Display, TEXT("[A 键位表] 挂着的上下文：%s（优先级 %lld）"),
					Context ? *Context->GetPathName() : TEXT("<空>"), Priority);
			}
			else
			{
				UE_LOG(LogUALPieProbe, Display, TEXT("[A 键位表] 挂着的上下文：%s（优先级未知）"),
					Context ? *Context->GetPathName() : TEXT("<空>"));
			}
			++Count;
		}

		if (Count == 0)
		{
			UE_LOG(LogUALPieProbe, Warning,
				TEXT("[A 键位表] 一个输入上下文都没挂 —— 这时候注入动作不会有反应"));
		}
	}

	/** 读 UInputAction::ValueType（0=Boolean 1=Axis1D 2=Axis2D 3=Axis3D） */
	bool ReadActionValueType(const UObject* Action, uint8& OutType)
	{
		if (!Action)
		{
			return false;
		}
		FProperty* Prop = FindFProperty<FProperty>(Action->GetClass(), TEXT("ValueType"));
		if (FByteProperty* AsByte = CastField<FByteProperty>(Prop))
		{
			OutType = AsByte->GetPropertyValue_InContainer(Action);
			return true;
		}
		if (FEnumProperty* AsEnum = CastField<FEnumProperty>(Prop))
		{
			const void* Addr = AsEnum->ContainerPtrToValuePtr<void>(Action);
			OutType = static_cast<uint8>(AsEnum->GetUnderlyingProperty()->GetSignedIntPropertyValue(Addr));
			return true;
		}
		return false;
	}

	const TCHAR* ValueTypeName(uint8 Type)
	{
		switch (Type)
		{
		case 0: return TEXT("Boolean");
		case 1: return TEXT("Axis1D");
		case 2: return TEXT("Axis2D");
		case 3: return TEXT("Axis3D");
		default: return TEXT("未知");
		}
	}

	/**
	 * 打印键位表，并列出**全部** Axis2D 动作。
	 *
	 * ## 第一版「挑第一个 Axis2D」是错的
	 *
	 * 5.8 那个工程里映射的排列顺序把 `IA_MouseLook` 排在了 `IA_Move` 前面，
	 * 于是探针对着「鼠标视角」注入前进，角色当然一动不动，然后报
	 * 「0a 不成立，L1 走不通」—— 一个纯粹由排序造成的假否定，
	 * 差点让人推翻整份设计。
	 *
	 * 顺序在不同工程/不同版本上没有任何保证。所以：**不挑，全测**。
	 * 谁真的把角色推动了，谁就是移动动作 —— 这也正是正式工具该有的判据：
	 * 不猜名字，也不信顺序，只信「注进去之后发生了什么」。
	 */
	void LogMappingsAndCollectAxis2D(const FProbeScene& Scene, TArray<const UObject*>& OutAxis2D)
	{
		LogAppliedContexts(Scene.PlayerInput);

		TArray<FProbeMapping> Mappings;
		if (!ReadActionMappings(Scene.PlayerInput, Mappings))
		{
			UE_LOG(LogUALPieProbe, Warning,
				TEXT("[A 键位表] 读不到 EnhancedActionMappings —— 这个工程多半没用 Enhanced Input"));
			return;
		}

		TSet<const UObject*> Seen;
		for (const FProbeMapping& Mapping : Mappings)
		{
			if (!Mapping.Action)
			{
				continue;
			}
			uint8 ValueType = 0xFF;
			ReadActionValueType(Mapping.Action, ValueType);

			UE_LOG(LogUALPieProbe, Display, TEXT("[A 键位表] %s ← %s（值类型 %s）"),
				*Mapping.Action->GetName(), *Mapping.Key.ToString(), ValueTypeName(ValueType));

			if (ValueType == 2 && !Seen.Contains(Mapping.Action))
			{
				OutAxis2D.Add(Mapping.Action);
			}
			Seen.Add(Mapping.Action);
		}

		UE_LOG(LogUALPieProbe, Display, TEXT("[A 键位表] 共 %d 条映射 / %d 个动作 / %d 个 Axis2D 动作"),
			Mappings.Num(), Seen.Num(), OutAxis2D.Num());

		if (OutAxis2D.Num() == 0)
		{
			UE_LOG(LogUALPieProbe, Error,
				TEXT("[A 键位表] 没找到 Axis2D 动作，后面几段没法测移动"));
		}
	}

	// ========================================================================
	// 注入：动作层（L1）
	// ========================================================================

	/**
	 * 造一个 FInputActionValue。
	 *
	 * 这是整条链路上唯一「现有基础设施不够用」的地方，三步：
	 *   1. GetBoundActionValue 取一个**类型正确**的种子（九版全有）
	 *   2. 让引擎造值 —— MakeInputActionValueOfType（5.1+）或
	 *      MakeInputActionValue（5.0–5.5，要种子当 MatchValueType）
	 *   3. 把返回的结构体整块搬进 InjectInputForAction 的 RawValue
	 *
	 * 第 2 步没有一个函数横跨九版：OfType 是 5.1 才有的，
	 * 老名字 5.6 就删了。所以两个都试，谁在用谁。
	 */
	bool InjectAction(const FProbeScene& Scene, const UObject* Action, double X, double Y, double Z)
	{
		if (!Scene.EnhancedSubsystem || !Action)
		{
			return false;
		}

		UClass* LibClass = UALReflect::FindScriptClass(TEXT("/Script/EnhancedInput.EnhancedInputLibrary"));
		if (!LibClass)
		{
			UE_LOG(LogUALPieProbe, Error, TEXT("找不到 UEnhancedInputLibrary —— EnhancedInput 插件没启用？"));
			return false;
		}
		UObject* LibCDO = LibClass->GetDefaultObject();

		uint8 ValueType = 0;
		ReadActionValueType(Action, ValueType);

		// —— 第 2 步优先：OfType（5.1+），不需要种子
		{
			FProbeCall Make(LibCDO, TEXT("MakeInputActionValueOfType"));
			if (Make.IsValid())
			{
				Make.Num(TEXT("X"), X).Num(TEXT("Y"), Y).Num(TEXT("Z"), Z).Enum(TEXT("ValueType"), ValueType);
				if (Make.Invoke())
				{
					const FStructProperty* ValueProp = nullptr;
					const void* ValueAddr = nullptr;
					if (Make.OutStruct(TEXT("ReturnValue"), ValueProp, ValueAddr))
					{
						FProbeCall Inject(Scene.EnhancedSubsystem, TEXT("InjectInputForAction"));
						if (!Inject.IsValid())
						{
							UE_LOG(LogUALPieProbe, Error, TEXT("%s"), *Inject.GetError());
							return false;
						}
						Inject.Obj(TEXT("Action"), const_cast<UObject*>(Action));
						Inject.StructFrom(TEXT("RawValue"), ValueProp, ValueAddr);
						return Inject.Invoke();
					}
				}
			}
		}

		// —— 兜底：老形态，要先取种子（5.0–5.5）
		{
			FProbeCall Seed(LibCDO, TEXT("GetBoundActionValue"));
			if (!Seed.IsValid())
			{
				UE_LOG(LogUALPieProbe, Error, TEXT("两条造值路径都不可用：%s"), *Seed.GetError());
				return false;
			}
			Seed.Obj(TEXT("Actor"), Scene.Pawn).Obj(TEXT("Action"), const_cast<UObject*>(Action));
			if (!Seed.Invoke())
			{
				return false;
			}
			const FStructProperty* SeedProp = nullptr;
			const void* SeedAddr = nullptr;
			if (!Seed.OutStruct(TEXT("ReturnValue"), SeedProp, SeedAddr))
			{
				return false;
			}

			FProbeCall Make(LibCDO, TEXT("MakeInputActionValue"));
			if (!Make.IsValid())
			{
				UE_LOG(LogUALPieProbe, Error, TEXT("%s"), *Make.GetError());
				return false;
			}
			Make.Num(TEXT("X"), X).Num(TEXT("Y"), Y).Num(TEXT("Z"), Z);
			Make.StructFrom(TEXT("MatchValueType"), SeedProp, SeedAddr);
			if (!Make.Invoke())
			{
				return false;
			}

			const FStructProperty* ValueProp = nullptr;
			const void* ValueAddr = nullptr;
			if (!Make.OutStruct(TEXT("ReturnValue"), ValueProp, ValueAddr))
			{
				return false;
			}

			FProbeCall Inject(Scene.EnhancedSubsystem, TEXT("InjectInputForAction"));
			if (!Inject.IsValid())
			{
				return false;
			}
			Inject.Obj(TEXT("Action"), const_cast<UObject*>(Action));
			Inject.StructFrom(TEXT("RawValue"), ValueProp, ValueAddr);
			return Inject.Invoke();
		}
	}

	// ========================================================================
	// 注入：视口层（真人按键走的入口）
	// ========================================================================
	//
	// 为什么走视口而不是 UPlayerInput::InputKey：
	//
	//   1. 视口是真人按键的必经之路，**包括那道 SetIgnoreInput 闸**。
	//      0e 要比的正是「动作注入绕过闸 vs 按键撞上闸」，走视口才有对照。
	//   2. UPlayerInput::InputKey 收的 FInputKeyParams 在 5.6 被标了弃用，
	//      在 5.8 上用它会刷一片弃用警告。
	//
	// FInputKeyEventArgs 的构造形态两端不同（5.3 收 ControllerId，
	// 5.8 收 FInputDeviceId + 时间戳），用重载决议探测收口，不用版本号宏。

	namespace KeyArgs
	{
		// int 重载优先：5.4+ 的 (Viewport, InputDeviceId, Key, Event, Timestamp)
		template <typename T>
		auto Make(FViewport* Vp, const FKey& Key, EInputEvent Event, int)
			-> decltype(T(Vp, FInputDeviceId::CreateFromInternalId(0), Key, Event, uint64(0)))
		{
			return T(Vp, FInputDeviceId::CreateFromInternalId(0), Key, Event, uint64(0));
		}

		// long 重载兜底：老形态 (Viewport, ControllerId, Key, Event)
		template <typename T>
		auto Make(FViewport* Vp, const FKey& Key, EInputEvent Event, long)
			-> decltype(T(Vp, int32(0), Key, Event))
		{
			return T(Vp, 0, Key, Event);
		}
	}

	bool InjectViewportKey(const FProbeScene& Scene, const FKey& Key, EInputEvent Event)
	{
		if (!Scene.Viewport)
		{
			return false;
		}
		return Scene.Viewport->InputKey(
			KeyArgs::Make<FInputKeyEventArgs>(Scene.Viewport->Viewport, Key, Event, 0));
	}

	// ========================================================================
	// 采样：位移对照组
	// ========================================================================

	/**
	 * 一次「空转 N 帧（基线）→ 注入 N 帧」的对照测量。
	 *
	 * 只测注入那一段是不够的：重力下落、被别的东西推、root motion 动画
	 * 都会产生位移。有对照组才能说「这段位移是我们造成的」。
	 *
	 * ## 顺序是反过来改的：基线在前，注入在后
	 *
	 * 第一版把基线放在注入**之后**，结果每一次「空转段」都量到 55–85 的位移 ——
	 * 那不是噪音，是**角色的惯性**：松手之后 CharacterMovement 还要按
	 * BrakingDeceleration 滑一段。基线被自己刚造成的运动污染，
	 * 于是「注入段 94 / 空转段 59」这种明明是成功的样本被判成失败。
	 *
	 * 基线放在最前面就没有这个问题：站着不动的角色，那半秒里位移接近 0。
	 * **这条同样适用于正式工具** —— 停手不等于立刻静止，
	 * 任何「先动后测静止」的判据都会被惯性骗到。
	 *
	 * 只比**水平**位移：Z 轴上掉一下太常见，掺进来会把结论搞脏。
	 */
	struct FMoveSample
	{
		double MovedWhileIdle = 0.0;        // 基线：注入之前
		double MovedWhileInjecting = 0.0;   // 注入段

		/** 判定：注入段明显动了，且显著超过基线 */
		bool LooksDriven() const
		{
			return MovedWhileInjecting > 20.0
				&& MovedWhileInjecting > MovedWhileIdle * 2.0 + 10.0;
		}

		FString Describe() const
		{
			return FString::Printf(TEXT("基线位移 %.1f → 注入段位移 %.1f（%.1f 倍）｜判定：%s"),
				MovedWhileIdle, MovedWhileInjecting,
				MovedWhileIdle > 0.01 ? MovedWhileInjecting / MovedWhileIdle : 999.0,
				LooksDriven() ? TEXT("✅ 是我们驱动的") : TEXT("❌ 没被驱动"));
		}
	};

	/**
	 * 跨帧跑一次对照测量，结果通过回调交出去。
	 *
	 * 用 FTSTicker 而不是世界 Tick：探针只要「每帧来一次」，不要求
	 * 精确排在 ProcessInputStack 之前。**这一点是探针的局限，正式实现要重新考虑** ——
	 * 注入和消费的先后如果不稳定，长脚本会出现随机丢帧。
	 */
	void RunMoveSample(
		const FProbeScene& Scene,
		int32 FramesPerPhase,
		TFunction<void()> OnInjectPhaseBegin,
		TFunction<void()> InjectOneFrame,
		TFunction<void()> OnInjectPhaseEnd,
		TFunction<void(const FMoveSample&)> OnDone)
	{
		TSharedRef<FMoveSample> Result = MakeShared<FMoveSample>();
		TSharedRef<int32> Frame = MakeShared<int32>(0);
		TWeakObjectPtr<APawn> WeakPawn = Scene.Pawn;
		TSharedRef<FVector> Anchor = MakeShared<FVector>(Scene.Pawn->GetActorLocation());

		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda(
				[Result, Frame, WeakPawn, Anchor, FramesPerPhase,
				 OnInjectPhaseBegin, InjectOneFrame, OnInjectPhaseEnd, OnDone](float) -> bool
				{
					APawn* Pawn = WeakPawn.Get();
					if (!Pawn)
					{
						// PIE 中途停了。报「没测成」，不要拿半份数据下结论
						UE_LOG(LogUALPieProbe, Warning, TEXT("Pawn 没了（PIE 停了？）—— 这次测量作废"));
						return false;
					}

					const int32 Current = (*Frame)++;

					// 第一段：什么都不做，量基线
					if (Current < FramesPerPhase)
					{
						return true;
					}

					if (Current == FramesPerPhase)
					{
						const FVector Now = Pawn->GetActorLocation();
						Result->MovedWhileIdle = FVector::Dist2D(Now, *Anchor);
						*Anchor = Now;
						if (OnInjectPhaseBegin)
						{
							OnInjectPhaseBegin();  // 视口按键在这里按下
						}
						InjectOneFrame();
						return true;
					}

					// 第二段：注入
					if (Current < FramesPerPhase * 2)
					{
						InjectOneFrame();
						return true;
					}

					if (OnInjectPhaseEnd)
					{
						OnInjectPhaseEnd();  // 视口按键在这里抬起
					}
					Result->MovedWhileInjecting = FVector::Dist2D(Pawn->GetActorLocation(), *Anchor);
					OnDone(*Result);
					return false;
				}),
			0.0f);
	}

	// ========================================================================
	// 各段
	// ========================================================================


	/**
	 * 0a —— 动作注入。
	 *
	 * **逐个试全部 Axis2D 动作**，而不是挑第一个（第一版就栽在这里，见
	 * LogMappingsAndCollectAxis2D 的注释）。谁真的把角色推动了，谁就是移动动作，
	 * 后面几段都用它。
	 */
	void StageL1(
		const FProbeScene& Scene,
		const TArray<const UObject*>& Candidates,
		TFunction<void(const UObject* /*DrivingAction*/)> Next)
	{
		if (Candidates.Num() == 0)
		{
			UE_LOG(LogUALPieProbe, Error, TEXT("[B 动作注入] 跳过：没有 Axis2D 动作可试"));
			Next(nullptr);
			return;
		}

		UE_LOG(LogUALPieProbe, Display, TEXT("[B 动作注入] 开始（0a），逐个试 %d 个 Axis2D 动作"),
			Candidates.Num());

		// 递归跑：一个测完再测下一个。共享指针装索引和结果，跨帧活着
		TSharedRef<int32> Index = MakeShared<int32>(0);
		TSharedRef<TArray<const UObject*>> List = MakeShared<TArray<const UObject*>>(Candidates);
		TSharedRef<const UObject*> Best = MakeShared<const UObject*>(nullptr);
		TSharedRef<double> BestMove = MakeShared<double>(0.0);

		// TFunction 想递归调自己，得有一个共享的持有者 —— lambda 里引用不到自己
		TSharedRef<TFunction<void()>> Step = MakeShared<TFunction<void()>>();
		FProbeScene Captured = Scene;

		*Step = [Captured, Index, List, Best, BestMove, Step, Next]()
		{
			if (*Index >= List->Num())
			{
				if (*Best)
				{
					UE_LOG(LogUALPieProbe, Display,
						TEXT("[B 动作注入] ✅ 0a 成立 —— %s 能驱动角色（位移 %.1f），后面几段用它"),
						*(*Best)->GetName(), *BestMove);
				}
				else
				{
					UE_LOG(LogUALPieProbe, Error,
						TEXT("[B 动作注入] ❌ 0a 不成立 —— %d 个 Axis2D 动作全都推不动角色，")
						TEXT("L1 这条路走不通，整份设计要重新评估"), List->Num());
				}
				Next(*Best);
				return;
			}

			const UObject* Action = (*List)[*Index];
			++(*Index);

			RunMoveSample(
				Captured, 30,
				[]() {},
				[Captured, Action]() { InjectAction(Captured, Action, 0.0, 1.0, 0.0); },
				[]() {},  // 动作注入只活一帧，不用收手
				[Action, Best, BestMove, Step](const FMoveSample& Sample)
				{
					UE_LOG(LogUALPieProbe, Display, TEXT("[B 动作注入] %s：%s"),
						*Action->GetName(), *Sample.Describe());
					if (Sample.LooksDriven() && Sample.MovedWhileInjecting > *BestMove)
					{
						*Best = Action;
						*BestMove = Sample.MovedWhileInjecting;
					}
					(*Step)();
				});
		};

		(*Step)();
	}

	void StageViewportKey(const FProbeScene& Scene, TFunction<void()> Next)
	{
		UE_LOG(LogUALPieProbe, Display, TEXT("[C 视口按键] 开始（0c），发 W"));

		FProbeScene Captured = Scene;
		RunMoveSample(
			Scene, 30,
			[Captured]() { InjectViewportKey(Captured, EKeys::W, IE_Pressed); },
			[]() {},  // 按下是持续状态，不用每帧重发
			[Captured]() { InjectViewportKey(Captured, EKeys::W, IE_Released); },
			[Next](const FMoveSample& Sample)
			{
				UE_LOG(LogUALPieProbe, Display, TEXT("[C 视口按键] %s"), *Sample.Describe());
				Next();
			});
	}

	/**
	 * 0e —— 整个探针里最重要的一段。
	 *
	 * 把游戏切到 UIOnly（等价于「打开了一个全屏菜单」），然后两条路各来一次：
	 *   · 动作注入：设计文档 §12.8 推断**照样生效**
	 *   · 视口按键：应当被 SetIgnoreInput 那道闸挡住
	 *
	 * 只有动作那条动 = 推断坐实，「注入成功 ≠ 玩家按得动」必须写进每一条工具描述。
	 * 两条都不动 = 注入也被挡，那是好消息。
	 * 两条都动 = 这个工程的 UIOnly 没拦住按键，这一段的结论不成立。
	 */
	void StageUIOnly(const FProbeScene& Scene, const UObject* MoveAction, TFunction<void()> Next)
	{
		if (!MoveAction || !Scene.PC)
		{
			UE_LOG(LogUALPieProbe, Error, TEXT("[D UI 抢输入] 跳过：0a 没找到能驱动的动作"));
			Next();
			return;
		}

		UE_LOG(LogUALPieProbe, Display, TEXT("[D UI 抢输入] 切到 UIOnly（0e），用 %s"),
			*MoveAction->GetName());
		Scene.PC->SetInputMode(FInputModeUIOnly());

		FProbeScene Captured = Scene;
		const UObject* Action = MoveAction;

		RunMoveSample(
			Scene, 30,
			[]() {},
			[Captured, Action]() { InjectAction(Captured, Action, 0.0, 1.0, 0.0); },
			[]() {},
			[Captured, Next](const FMoveSample& InjectSample)
			{
				UE_LOG(LogUALPieProbe, Display, TEXT("[D UI 抢输入] 动作注入：%s"), *InjectSample.Describe());
				const bool bInjectionStillWorks = InjectSample.LooksDriven();

				RunMoveSample(
					Captured, 30,
					[Captured]() { InjectViewportKey(Captured, EKeys::W, IE_Pressed); },
					[]() {},
					[Captured]() { InjectViewportKey(Captured, EKeys::W, IE_Released); },
					[Captured, Next, bInjectionStillWorks](const FMoveSample& KeySample)
					{
						UE_LOG(LogUALPieProbe, Display, TEXT("[D UI 抢输入] 视口按键：%s"), *KeySample.Describe());

						if (bInjectionStillWorks && !KeySample.LooksDriven())
						{
							UE_LOG(LogUALPieProbe, Error,
								TEXT("[D UI 抢输入] ⚠️ 设计文档 §12.8 坐实：菜单开着时我们注入得进去，")
								TEXT("真人按键进不去。工具必须警告这种状态，否则会给出「移动正常」的假结论"));
						}
						else if (!bInjectionStillWorks && !KeySample.LooksDriven())
						{
							UE_LOG(LogUALPieProbe, Display,
								TEXT("[D UI 抢输入] ✅ 好消息：UIOnly 下注入也被挡住了，不存在那个假阳性"));
						}
						else
						{
							UE_LOG(LogUALPieProbe, Warning,
								TEXT("[D UI 抢输入] 视口按键也动了 —— 这个工程的 UIOnly 没拦住按键，")
								TEXT("这一段的结论不成立"));
						}

						if (Captured.PC)
						{
							Captured.PC->SetInputMode(FInputModeGameOnly());
						}
						Next();
					});
			});
	}

	/** 0b —— 暂停期间注入会不会被吃掉；「恢复 → 注入 → 再暂停」这条路走不走得通 */
	void StagePause(const FProbeScene& Scene, const UObject* MoveAction, TFunction<void()> Next)
	{
		if (!MoveAction || !GEditor)
		{
			UE_LOG(LogUALPieProbe, Error, TEXT("[E 暂停] 跳过：0a 没找到能驱动的动作"));
			Next();
			return;
		}

		UE_LOG(LogUALPieProbe, Display, TEXT("[E 暂停] 先暂停世界，再注入 30 帧（0b）"));
		GEditor->SetPIEWorldsPaused(true);

		FProbeScene Captured = Scene;
		const UObject* Action = MoveAction;

		RunMoveSample(
			Scene, 30,
			[]() {},
			[Captured, Action]() { InjectAction(Captured, Action, 0.0, 1.0, 0.0); },
			[]() {},
			[Captured, Action, Next](const FMoveSample& PausedSample)
			{
				UE_LOG(LogUALPieProbe, Display, TEXT("[E 暂停] 暂停中注入：%s"), *PausedSample.Describe());
				if (!PausedSample.LooksDriven())
				{
					UE_LOG(LogUALPieProbe, Display,
						TEXT("[E 暂停] ✅ 符合预期：bTriggerWhenPaused 默认为假，暂停时注入不会触发。")
						TEXT("交互模式必须走「恢复 → 注入并跑几帧 → 再暂停」"));
				}

				if (GEditor)
				{
					GEditor->SetPIEWorldsPaused(false);
				}

				// 恢复之后立刻再来一次：验证暂停期间攒下的注入不会「憋着」一次性爆发
				RunMoveSample(
					Captured, 30,
					[]() {},
					[Captured, Action]() { InjectAction(Captured, Action, 0.0, 1.0, 0.0); },
					[]() {},
					[Next](const FMoveSample& ResumedSample)
					{
						UE_LOG(LogUALPieProbe, Display, TEXT("[E 暂停] 恢复后注入：%s"), *ResumedSample.Describe());
						Next();
					});
			});
	}

	/** 0d —— 编辑器节流。数一段墙钟时间里跑了多少帧，前台/后台各跑一次做对比 */
	void StageThrottle(TFunction<void()> Next)
	{
		bool bThrottle = true;
		if (UClass* SettingsClass = UALReflect::FindScriptClass(TEXT("/Script/UnrealEd.EditorPerformanceSettings")))
		{
			if (const UObject* Settings = SettingsClass->GetDefaultObject())
			{
				UALReflect::GetBoolProp(Settings, TEXT("bThrottleCPUWhenNotForeground"), bThrottle);
			}
		}
		UE_LOG(LogUALPieProbe, Display,
			TEXT("[F 节流] bThrottleCPUWhenNotForeground = %s（0d）。开始数 3 秒内的帧数"),
			bThrottle ? TEXT("开") : TEXT("关"));

		TSharedRef<int32> Frames = MakeShared<int32>(0);
		TSharedRef<double> Start = MakeShared<double>(FPlatformTime::Seconds());

		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([Frames, Start, Next](float) -> bool
			{
				++(*Frames);
				const double Elapsed = FPlatformTime::Seconds() - *Start;
				if (Elapsed < 3.0)
				{
					return true;
				}
				UE_LOG(LogUALPieProbe, Display,
					TEXT("[F 节流] 3 秒 %d 帧，约 %.1f fps。")
					TEXT("请把编辑器窗口切到后台再跑一次 UAL.PieInputProbe throttle 对比"),
					*Frames, *Frames / Elapsed);
				Next();
				return false;
			}),
			0.0f);
	}

	/**
	 * 0f —— 旧输入系统。
	 *
	 * 两个测试工程都是 Enhanced Input 模板，DefaultInput.ini 里一条
	 * Action/Axis Mapping 都没有，「找个老工程」这个前提不成立。
	 *
	 * 替代做法：引擎自带的 ADefaultPawn。它的 bAddDefaultMovementBindings 默认为真，
	 * 构造时通过 UPlayerInput::AddEngineDefinedAxisMapping 注册一整套**纯旧输入系统**
	 * 的轴映射（W/S 是 DefaultPawn_MoveForward），5.3 与 5.8 这段代码逐行相同。
	 *
	 * ⚠️ 残留差距：引擎内建映射进的是 EngineDefinedAxisMappings，真实老工程的
	 * 映射来自 UInputSettings。求值路径相同，来源不同 —— 这一段通过只能说明
	 * 「视口按键能驱动旧输入的求值路径」，不能完全替代在真老工程上跑一次。
	 */
	void StageLegacyInput(const FProbeScene& Scene, TFunction<void()> Next)
	{
		if (!Scene.World || !Scene.PC)
		{
			Next();
			return;
		}

		UE_LOG(LogUALPieProbe, Display, TEXT("[G 旧输入] 生成 DefaultPawn 并接管（0f）"));

		APawn* OriginalPawn = Scene.Pawn;
		const FVector SpawnAt = OriginalPawn ? OriginalPawn->GetActorLocation() + FVector(0, 0, 200)
											 : FVector::ZeroVector;

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ADefaultPawn* Probe = Scene.World->SpawnActor<ADefaultPawn>(SpawnAt, FRotator::ZeroRotator, SpawnParams);
		if (!Probe)
		{
			UE_LOG(LogUALPieProbe, Error, TEXT("[G 旧输入] DefaultPawn 生成失败"));
			Next();
			return;
		}

		Scene.PC->Possess(Probe);

		FProbeScene Captured = Scene;
		Captured.Pawn = Probe;

		RunMoveSample(
			Captured, 30,
			[Captured]() { InjectViewportKey(Captured, EKeys::W, IE_Pressed); },
			[]() {},
			[Captured]() { InjectViewportKey(Captured, EKeys::W, IE_Released); },
			[Captured, Probe, OriginalPawn, Next](const FMoveSample& Sample)
			{
				UE_LOG(LogUALPieProbe, Display, TEXT("[G 旧输入] %s"), *Sample.Describe());

				// 收拾干净：把玩家还回原来的 pawn，探针 pawn 销毁。
				// 不还的话用户按停止之后场景里会多一个飞行 pawn，看起来像我们改坏了关卡
				if (Captured.PC && OriginalPawn)
				{
					Captured.PC->Possess(OriginalPawn);
				}
				if (Probe)
				{
					Probe->Destroy();
				}
				Next();
			});
	}

	// ========================================================================
	// 串起来
	// ========================================================================

	void RunAll()
	{
		FProbeScene Scene;
		if (!ResolveScene(Scene))
		{
			return;
		}

		UE_LOG(LogUALPieProbe, Display, TEXT("===== UAL PIE 输入探针开始 ====="));

		TArray<const UObject*> Axis2D;
		LogMappingsAndCollectAxis2D(Scene, Axis2D);

		// 0a 先跑，它顺带定出后面几段用哪个动作。
		// 0a 不过后面的结论都没意义，但还是全跑完 —— 一次真机运行要人配合，
		// 能多拿一段数据就多拿一段
		StageL1(Scene, Axis2D, [Scene](const UObject* Driving)
		{
			StageViewportKey(Scene, [Scene, Driving]()
			{
				StageUIOnly(Scene, Driving, [Scene, Driving]()
				{
					StagePause(Scene, Driving, [Scene]()
					{
						StageLegacyInput(Scene, []()
						{
							StageThrottle([]()
							{
								UE_LOG(LogUALPieProbe, Display, TEXT("===== 探针结束，请把以上整段贴回 ====="));
							});
						});
					});
				});
			});
		});
	}

	void RunStage(const TArray<FString>& Args)
	{
		if (Args.Num() == 0)
		{
			RunAll();
			return;
		}

		FProbeScene Scene;
		if (!ResolveScene(Scene))
		{
			return;
		}

		const FString& Stage = Args[0];
		auto Done = []() { UE_LOG(LogUALPieProbe, Display, TEXT("===== 该段结束 =====")); };

		TArray<const UObject*> Axis2D;
		if (Stage != TEXT("viewport") && Stage != TEXT("legacy") && Stage != TEXT("throttle"))
		{
			LogMappingsAndCollectAxis2D(Scene, Axis2D);
		}

		if (Stage == TEXT("map"))
		{
			// 上面已经打完了
		}
		else if (Stage == TEXT("l1"))
		{
			StageL1(Scene, Axis2D, [](const UObject*) {});
		}
		else if (Stage == TEXT("viewport"))
		{
			StageViewportKey(Scene, Done);
		}
		else if (Stage == TEXT("uionly"))
		{
			StageL1(Scene, Axis2D, [Scene, Done](const UObject* Driving)
			{
				StageUIOnly(Scene, Driving, Done);
			});
		}
		else if (Stage == TEXT("pause"))
		{
			StageL1(Scene, Axis2D, [Scene, Done](const UObject* Driving)
			{
				StagePause(Scene, Driving, Done);
			});
		}
		else if (Stage == TEXT("legacy"))
		{
			StageLegacyInput(Scene, Done);
		}
		else if (Stage == TEXT("throttle"))
		{
			StageThrottle(Done);
		}
		else
		{
			UE_LOG(LogUALPieProbe, Warning,
				TEXT("认不出的段名，可用：map / l1 / viewport / uionly / pause / legacy / throttle"));
		}
	}
}

static FAutoConsoleCommand GUALPieInputProbeCommand(
	TEXT("UAL.PieInputProbe"),
	TEXT("一次性探针：验证 PIE 里的输入注入是否可用（PIE 自主操作设计第 0 步）"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&RunStage));
