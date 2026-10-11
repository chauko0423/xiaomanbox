#include "UAL_InputCommands.h"

#include "UAL_CommandUtils.h"
#include "UAL_ReflectCall.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "Containers/Ticker.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "InputCoreTypes.h"
// FInputKeyEventArgs 的落脚点跨版本变过：5.0/5.1 在 UnrealClient.h 里，5.2 起
// 拆到独立的 InputKeyEventArgs.h。UnrealClient.h 在 5.2+ 会把新头转include 进来，
// 所以统一包它 —— 九个版本都能拿到定义，也不用版本号宏。
#include "UnrealClient.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "UObject/UnrealType.h"

/**
 * `FInputKeyEventArgs` 的构造形态跨版本不同，用重载决议探测收口，不用版本号宏：
 *   5.4+   (Viewport, FInputDeviceId, Key, Event, Timestamp)
 *   更早的 (Viewport, ControllerId, Key, Event)
 *
 * 为什么走视口而不是 UPlayerInput::InputKey：视口是真人按键的必经之路，
 * **包括那道 SetIgnoreInput 闸**；而 UPlayerInput::InputKey 收的 FInputKeyParams
 * 在 5.6 被标了弃用，5.8 上用它会刷一片弃用警告。
 */
namespace UALKeyArgs
{
	// int 重载优先：新形态
	template <typename T>
	auto Make(FViewport* Vp, const FKey& Key, EInputEvent Event, int)
		-> decltype(T(Vp, FInputDeviceId::CreateFromInternalId(0), Key, Event, uint64(0)))
	{
		return T(Vp, FInputDeviceId::CreateFromInternalId(0), Key, Event, uint64(0));
	}

	// long 重载兜底：老形态
	template <typename T>
	auto Make(FViewport* Vp, const FKey& Key, EInputEvent Event, long)
		-> decltype(T(Vp, int32(0), Key, Event))
	{
		return T(Vp, 0, Key, Event);
	}
}

namespace
{
	/** 一次调用最多加载多少个 IMC 资产。大工程里 IMC 可能上百个，全加载会卡住命令线程 */
	constexpr int32 UAL_MaxContextAssets = 40;

	const TCHAR* ValueTypeName(uint8 Type)
	{
		switch (Type)
		{
		case 0: return TEXT("Boolean");
		case 1: return TEXT("Axis1D");
		case 2: return TEXT("Axis2D");
		case 3: return TEXT("Axis3D");
		default: return TEXT("Unknown");
		}
	}

	/** 读 UInputAction::ValueType（枚举可能是 FByteProperty 也可能是 FEnumProperty） */
	bool ReadValueType(const UObject* Action, uint8& OutType)
	{
		if (!Action)
		{
			return false;
		}
		FProperty* Prop = FindFProperty<FProperty>(Action->GetClass(), TEXT("ValueType"));
		if (const FByteProperty* AsByte = CastField<FByteProperty>(Prop))
		{
			OutType = AsByte->GetPropertyValue_InContainer(Action);
			return true;
		}
		if (const FEnumProperty* AsEnum = CastField<FEnumProperty>(Prop))
		{
			const void* Addr = AsEnum->ContainerPtrToValuePtr<void>(Action);
			OutType = static_cast<uint8>(AsEnum->GetUnderlyingProperty()->GetSignedIntPropertyValue(Addr));
			return true;
		}
		return false;
	}

	/**
	 * 触发器/修饰器的可读名字。
	 *
	 * 类名是 `InputTriggerHold` / `InputModifierNegate` 这种，去掉前缀后
	 * 就是模型能直接用的词。**不做映射表** —— 用户自定义的触发器类名各种各样，
	 * 表里没有的就原样给出去，比报一个「未知」有用。
	 */
	FString FriendlyBehaviourName(const UObject* Object)
	{
		if (!Object)
		{
			return FString();
		}
		FString Name = Object->GetClass()->GetName();
		Name.RemoveFromStart(TEXT("InputTrigger"));
		Name.RemoveFromStart(TEXT("InputModifier"));
		return Name.IsEmpty() ? Object->GetClass()->GetName() : Name;
	}

	/**
	 * 把一个触发器/修饰器实例连同它的数值参数写成 JSON。
	 *
	 * **参数必须带上。** 一个挂着 `Hold 1.0s` 的交互键，注入一帧的 true
	 * 什么也不会发生 —— 只回一个 "Hold" 而不说门槛是多少，
	 * 调用方就会去查别的地方（设计文档 §5.3）。
	 *
	 * 只导出数值、布尔和枚举三类：这类类的可调参数几乎全在这里面，
	 * 而对象/数组/结构体属性导出来只会把返回撑大。
	 */
	TSharedPtr<FJsonObject> BehaviourToJson(const UObject* Object)
	{
		if (!Object)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("type"), FriendlyBehaviourName(Object));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		int32 ParamCount = 0;
		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			FProperty* Prop = *It;

			/*
			 * 值一律交给 `PropertyToJsonValueCompat` 取，**不要自己按类型挑取值器**。
			 *
			 * `FNumericProperty` 的两个取值器各自带一道断言（`UnrealType.h`，
			 * `TProperty_Numeric` 里）：`GetFloatingPointPropertyValue` 是
			 * `check(TIsFloatingPoint<TCppType>::Value)`，`GetSignedIntPropertyValue`
			 * 是 `check(TIsIntegral<TCppType>::Value)`。挑错了**不是返回一个不准的数，
			 * 是直接 assert 把编辑器崩掉**。这里原来一律调浮点版，于是工程里只要有一个
			 * 动作挂了 Pulse 或 Combo 触发器，`input.map` 就必崩 —— 那两个类上有
			 * `int32 TriggerLimit` 和 `int32 CurrentComboStepIndex`。
			 * 真机上崩过（2026-09-21 的用户报告）。
			 *
			 * 走这个共用封装而不是在这儿再写一遍分流：它包的
			 * `FJsonObjectConverter::UPropertyToJsonValue` 里就是同一套
			 * `IsFloatingPoint()` / `IsInteger()` 判断，而且本仓库另外七处属性转 JSON
			 * 都走它 —— 那个封装存在的理由就是把跨引擎版本的差异收在一个地方。
			 *
			 * 顺带把枚举也接上了：`FEnumProperty` 不是 `FNumericProperty` 的子类，
			 * 以前两个分支都接不住，于是 `SwizzleAxis` 的 `Order`、`DeadZone` 的
			 * `Type` 这些**唯一的可调参数**一个都不出现在返回里，
			 * 调用方看到的 ZYX 和默认的 YXZ 一模一样。转换器对枚举回的是名字字符串，
			 * 比裸数字有用得多，所以底层是枚举的 `FByteProperty` 也不再跳过。
			 */
			const bool bWanted = CastField<FNumericProperty>(Prop) || CastField<FBoolProperty>(Prop) ||
								 CastField<FEnumProperty>(Prop);
			if (!bWanted)
			{
				continue;
			}

			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Object);

			/*
			 * `uint64` 要在转换器之前拦下来。
			 *
			 * 整数那条路上引擎只用带符号的取值器（`JsonObjectConverter.cpp` 里就是
			 * `GetSignedIntPropertyValue`），而它对 `uint64` 是个直接重解释的
			 * `(int64)` 转换 —— 断言过得去，值悄悄绕成负数。
			 * 一个 `uint64 StateMask = 0x8000...` 会报成 -9223372036854775808。
			 */
			if (const FUInt64Property* AsUnsigned64 = CastField<FUInt64Property>(Prop))
			{
				Params->SetNumberField(Prop->GetName(),
					static_cast<double>(AsUnsigned64->GetPropertyValue(ValuePtr)));
				++ParamCount;
				continue;
			}

			if (const TSharedPtr<FJsonValue> Value = UAL_CommandUtils::PropertyToJsonValueCompat(Prop, ValuePtr))
			{
				Params->SetField(Prop->GetName(), Value);
				++ParamCount;
			}
		}
		if (ParamCount > 0)
		{
			Json->SetObjectField(TEXT("params"), Params);
		}
		return Json;
	}

	/** 读一个 UPROPERTY(Instanced) 的对象数组（Triggers / Modifiers），写成 JSON 数组 */
	TArray<TSharedPtr<FJsonValue>> ReadBehaviourArray(const void* Container, const UStruct* Owner, const TCHAR* PropName)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		if (!Container || !Owner)
		{
			return Out;
		}
		FArrayProperty* ArrayProp = FindFProperty<FArrayProperty>(Owner, PropName);
		if (!ArrayProp)
		{
			return Out;
		}
		const FObjectPropertyBase* ElemProp = CastField<FObjectPropertyBase>(ArrayProp->Inner);
		if (!ElemProp)
		{
			return Out;
		}

		FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Container));
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			if (UObject* Entry = ElemProp->GetObjectPropertyValue(Helper.GetRawPtr(Index)))
			{
				if (TSharedPtr<FJsonObject> Json = BehaviourToJson(Entry))
				{
					Out.Add(MakeShared<FJsonValueObject>(Json));
				}
			}
		}
		return Out;
	}

	/** 一个动作在返回里的累积形态：多条按键映射会合并到同一个动作下 */
	struct FActionEntry
	{
		const UObject* Action = nullptr;
		TArray<TSharedPtr<FJsonValue>> Keys;
	};

	/** 把一条按键映射（含它自己的 trigger/modifier）写成 JSON */
	TSharedPtr<FJsonObject> KeyMappingToJson(const FKey& Key, const void* MappingStruct, const UStruct* MappingType)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("key"), Key.ToString());

		// 按键级的 trigger/modifier 覆盖/叠加在动作级之上。为空就不写 ——
		// 绝大多数映射没有自己的，全写出来会把返回撑大好几倍
		const TArray<TSharedPtr<FJsonValue>> Triggers = ReadBehaviourArray(MappingStruct, MappingType, TEXT("Triggers"));
		if (Triggers.Num() > 0)
		{
			Json->SetArrayField(TEXT("triggers"), Triggers);
		}
		const TArray<TSharedPtr<FJsonValue>> Modifiers = ReadBehaviourArray(MappingStruct, MappingType, TEXT("Modifiers"));
		if (Modifiers.Num() > 0)
		{
			Json->SetArrayField(TEXT("modifiers"), Modifiers);
		}
		return Json;
	}

	/** 把累积好的动作表写成 JSON 数组 */
	TArray<TSharedPtr<FJsonValue>> ActionsToJson(const TArray<FActionEntry>& Entries)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FActionEntry& Entry : Entries)
		{
			if (!Entry.Action)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("name"), Entry.Action->GetName());
			Json->SetStringField(TEXT("path"), Entry.Action->GetPathName());

			uint8 ValueType = 0xFF;
			ReadValueType(Entry.Action, ValueType);
			// 值类型必须给：往 Boolean 动作注入一个 2D 向量不报错，静默无效
			Json->SetStringField(TEXT("value_type"), ValueTypeName(ValueType));

			Json->SetArrayField(TEXT("keys"), Entry.Keys);

			const TArray<TSharedPtr<FJsonValue>> Triggers =
				ReadBehaviourArray(Entry.Action, Entry.Action->GetClass(), TEXT("Triggers"));
			if (Triggers.Num() > 0)
			{
				Json->SetArrayField(TEXT("triggers"), Triggers);
			}
			const TArray<TSharedPtr<FJsonValue>> Modifiers =
				ReadBehaviourArray(Entry.Action, Entry.Action->GetClass(), TEXT("Modifiers"));
			if (Modifiers.Num() > 0)
			{
				Json->SetArrayField(TEXT("modifiers"), Modifiers);
			}

			Out.Add(MakeShared<FJsonValueObject>(Json));
		}
		return Out;
	}

	/** 往动作表里塞一条按键映射，同一个动作合并 */
	void AddMapping(TArray<FActionEntry>& Entries, const UObject* Action, const TSharedPtr<FJsonObject>& KeyJson)
	{
		if (!Action || !KeyJson.IsValid())
		{
			return;
		}
		FActionEntry* Found = Entries.FindByPredicate(
			[Action](const FActionEntry& E) { return E.Action == Action; });
		if (!Found)
		{
			Found = &Entries.AddDefaulted_GetRef();
			Found->Action = Action;
		}
		Found->Keys.Add(MakeShared<FJsonValueObject>(KeyJson));
	}

	// ------------------------------------------------------------------
	// 运行时：读正在跑的游戏
	// ------------------------------------------------------------------

	/**
	 * 读「此刻挂着哪些输入上下文」。
	 *
	 * ⚠️ 这个属性 5.6 起换了名字**和类型**：真数据搬到 `AppliedInputContextData`
	 * （值从 int32 变成结构体），老的 `AppliedInputContexts` 还在但**是空的**。
	 * 只认老名字的话，5.6/5.7/5.8 上会一律回答「没挂任何上下文」而且不报错 ——
	 * 2026-09-03 的探针就是这么撞上的。所以先找新名字，找不到再回落。
	 */
	void ReadAppliedContexts(const UObject* PlayerInput, TArray<TSharedPtr<FJsonValue>>& Out, FString& OutSourceProp)
	{
		if (!PlayerInput)
		{
			return;
		}

		FMapProperty* MapProp = FindFProperty<FMapProperty>(PlayerInput->GetClass(), TEXT("AppliedInputContextData"));
		OutSourceProp = TEXT("AppliedInputContextData");
		if (!MapProp)
		{
			MapProp = FindFProperty<FMapProperty>(PlayerInput->GetClass(), TEXT("AppliedInputContexts"));
			OutSourceProp = TEXT("AppliedInputContexts");
		}
		if (!MapProp)
		{
			OutSourceProp.Empty();
			return;
		}

		const FObjectPropertyBase* KeyProp = CastField<FObjectPropertyBase>(MapProp->KeyProp);
		// 5.6+ 的值是结构体，取不到优先级就只报上下文本身，不要因此整条不报
		const FNumericProperty* ValueProp = CastField<FNumericProperty>(MapProp->ValueProp);
		const FStructProperty* ValueStruct = CastField<FStructProperty>(MapProp->ValueProp);

		FScriptMapHelper Helper(MapProp, MapProp->ContainerPtrToValuePtr<void>(PlayerInput));
		for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
		{
			if (!Helper.IsValidIndex(Index))
			{
				continue;
			}
			const UObject* Context = KeyProp ? KeyProp->GetObjectPropertyValue(Helper.GetKeyPtr(Index)) : nullptr;
			if (!Context)
			{
				continue;
			}

			TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("path"), Context->GetPathName());
			Json->SetStringField(TEXT("name"), Context->GetName());

			/*
			 * 取值器按 `IsFloatingPoint()` 分流，别写死带符号整数那一版。
			 *
			 * 这两处拿到的属性都是**按名字**反射出来的（`ValueProp` 是 map 的值类型，
			 * `Inner` 是结构体里叫 Priority 的那个字段），代码并不知道它到底是什么类型 ——
			 * 而 `GetSignedIntPropertyValue` 里是 `check(TIsIntegral<TCppType>::Value)`，
			 * 类型对不上就是 assert 崩编辑器，和 `BehaviourToJson` 那次崩的是同一道断言。
			 * 今天九个引擎版本上 Priority 都是 int32，所以还没崩过；哪天它变成 float
			 * 就崩了，而这行代码没有任何办法察觉。一次三元表达式就把这条路堵死。
			 */
			const auto ReadNumeric = [](const FNumericProperty* Prop, const void* ValuePtr) -> double
			{
				return Prop->IsFloatingPoint() ? Prop->GetFloatingPointPropertyValue(ValuePtr)
											   : static_cast<double>(Prop->GetSignedIntPropertyValue(ValuePtr));
			};

			if (ValueProp)
			{
				Json->SetNumberField(TEXT("priority"), ReadNumeric(ValueProp, Helper.GetValuePtr(Index)));
			}
			else if (ValueStruct)
			{
				// 5.6+：优先级藏在结构体里，名字仍是 Priority
				if (const FNumericProperty* Inner =
						FindFProperty<FNumericProperty>(ValueStruct->Struct, TEXT("Priority")))
				{
					Json->SetNumberField(TEXT("priority"),
						ReadNumeric(Inner, Inner->ContainerPtrToValuePtr<void>(Helper.GetValuePtr(Index))));
				}
			}

			Out.Add(MakeShared<FJsonValueObject>(Json));
		}
	}

	/** 读运行时压平后的映射表（`EnhancedActionMappings`，九版全有） */
	bool ReadRuntimeMappings(const UObject* PlayerInput, TArray<FActionEntry>& Out)
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

		const FObjectPropertyBase* ActionProp =
			FindFProperty<FObjectPropertyBase>(ElemProp->Struct, TEXT("Action"));
		const FStructProperty* KeyProp = FindFProperty<FStructProperty>(ElemProp->Struct, TEXT("Key"));

		FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(PlayerInput));
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			const uint8* Elem = Helper.GetRawPtr(Index);
			const UObject* Action = ActionProp ? ActionProp->GetObjectPropertyValue_InContainer(Elem) : nullptr;
			const FKey Key = KeyProp ? *KeyProp->ContainerPtrToValuePtr<FKey>(Elem) : FKey();
			AddMapping(Out, Action, KeyMappingToJson(Key, Elem, ElemProp->Struct));
		}
		return true;
	}

	// ------------------------------------------------------------------
	// 资产：PIE 没跑时只能说「工程里定义了什么」
	// ------------------------------------------------------------------

	/**
	 * 从资产里读 IMC。
	 *
	 * **这跟运行时不是一回事**：资产里定义了不代表游戏里那一刻挂着。
	 * 调用方靠 `source: "asset"` 区分，返回里也会带一句说明。
	 */
	void ReadContextAssets(TArray<TSharedPtr<FJsonValue>>& OutContexts, TArray<FActionEntry>& OutActions, bool& bOutTruncated)
	{
		UClass* ContextClass = UALReflect::FindScriptClass(TEXT("/Script/EnhancedInput.InputMappingContext"));
		if (!ContextClass)
		{
			return; // EnhancedInput 插件没启用，这个工程走的是旧输入系统
		}

		const FAssetRegistryModule& Registry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

		TArray<FAssetData> Assets;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		Registry.Get().GetAssetsByClass(ContextClass->GetClassPathName(), Assets);
#else
		Registry.Get().GetAssetsByClass(ContextClass->GetFName(), Assets);
#endif

		int32 Loaded = 0;
		for (const FAssetData& Asset : Assets)
		{
			if (Loaded >= UAL_MaxContextAssets)
			{
				// 截断要说出来。默默少给几个上下文，模型会以为那些绑定不存在
				bOutTruncated = true;
				break;
			}
			UObject* Context = Asset.GetAsset();
			if (!Context)
			{
				continue;
			}
			++Loaded;

			TSharedPtr<FJsonObject> ContextJson = MakeShared<FJsonObject>();
			ContextJson->SetStringField(TEXT("path"), Context->GetPathName());
			ContextJson->SetStringField(TEXT("name"), Context->GetName());
			OutContexts.Add(MakeShared<FJsonValueObject>(ContextJson));

			FArrayProperty* MappingsProp =
				FindFProperty<FArrayProperty>(Context->GetClass(), TEXT("Mappings"));
			if (!MappingsProp)
			{
				continue;
			}
			const FStructProperty* ElemProp = CastField<FStructProperty>(MappingsProp->Inner);
			if (!ElemProp || !ElemProp->Struct)
			{
				continue;
			}
			const FObjectPropertyBase* ActionProp =
				FindFProperty<FObjectPropertyBase>(ElemProp->Struct, TEXT("Action"));
			const FStructProperty* KeyProp = FindFProperty<FStructProperty>(ElemProp->Struct, TEXT("Key"));

			FScriptArrayHelper Helper(MappingsProp, MappingsProp->ContainerPtrToValuePtr<void>(Context));
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				const uint8* Elem = Helper.GetRawPtr(Index);
				const UObject* Action = ActionProp ? ActionProp->GetObjectPropertyValue_InContainer(Elem) : nullptr;
				const FKey Key = KeyProp ? *KeyProp->ContainerPtrToValuePtr<FKey>(Elem) : FKey();
				AddMapping(OutActions, Action, KeyMappingToJson(Key, Elem, ElemProp->Struct));
			}
		}
	}

	/**
	 * 旧输入系统的 Action / Axis Mapping。
	 *
	 * 5.0–5.3 起步的工程大量还在用这一套，**它们没有 IMC，也没有「动作层」**。
	 * 不认这一套的话，`input.map` 会对着这类工程回一张空表，
	 * 而模型会说「这个工程没有配置任何输入」—— 把工具的盲区说成用户的 bug，
	 * 比不回答更糟。见设计文档画像 12.2。
	 */
	void ReadLegacyMappings(TSharedPtr<FJsonObject>& Result, bool& bOutHasLegacy)
	{
		const UInputSettings* Settings = GetDefault<UInputSettings>();
		if (!Settings)
		{
			return;
		}

		TMap<FName, TArray<TSharedPtr<FJsonValue>>> ActionKeys;
		for (const FInputActionKeyMapping& Mapping : Settings->GetActionMappings())
		{
			ActionKeys.FindOrAdd(Mapping.ActionName).Add(MakeShared<FJsonValueString>(Mapping.Key.ToString()));
		}

		TMap<FName, TArray<TSharedPtr<FJsonValue>>> AxisKeys;
		for (const FInputAxisKeyMapping& Mapping : Settings->GetAxisMappings())
		{
			TSharedPtr<FJsonObject> KeyJson = MakeShared<FJsonObject>();
			KeyJson->SetStringField(TEXT("key"), Mapping.Key.ToString());
			// scale 决定方向：同一个轴上 W 是 +1、S 是 -1，不给出来就没法用
			KeyJson->SetNumberField(TEXT("scale"), Mapping.Scale);
			AxisKeys.FindOrAdd(Mapping.AxisName).Add(MakeShared<FJsonValueObject>(KeyJson));
		}

		if (ActionKeys.Num() == 0 && AxisKeys.Num() == 0)
		{
			return;
		}
		bOutHasLegacy = true;

		TArray<TSharedPtr<FJsonValue>> ActionsJson;
		for (const TPair<FName, TArray<TSharedPtr<FJsonValue>>>& Pair : ActionKeys)
		{
			TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("name"), Pair.Key.ToString());
			Json->SetArrayField(TEXT("keys"), Pair.Value);
			ActionsJson.Add(MakeShared<FJsonValueObject>(Json));
		}

		TArray<TSharedPtr<FJsonValue>> AxesJson;
		for (const TPair<FName, TArray<TSharedPtr<FJsonValue>>>& Pair : AxisKeys)
		{
			TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("name"), Pair.Key.ToString());
			Json->SetArrayField(TEXT("keys"), Pair.Value);
			AxesJson.Add(MakeShared<FJsonValueObject>(Json));
		}

		Result->SetArrayField(TEXT("legacy_actions"), ActionsJson);
		Result->SetArrayField(TEXT("legacy_axes"), AxesJson);
	}
}

void FUAL_InputCommands::RegisterCommands(TMap<FString, FHandlerFunc>& CommandMap)
{
	CommandMap.Add(TEXT("input.map"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetInputMap(Payload, RequestId);
	});

	CommandMap.Add(TEXT("input.inject_action"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_InjectAction(Payload, RequestId);
	});

	CommandMap.Add(TEXT("input.inject_key"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_InjectKey(Payload, RequestId);
	});
}

void FUAL_InputCommands::Handle_GetInputMap(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);

	int32 PlayerIndex = 0;
	if (Payload.IsValid())
	{
		double Raw = 0.0;
		if (Payload->TryGetNumberField(TEXT("player_index"), Raw))
		{
			PlayerIndex = FMath::Max(0, static_cast<int32>(Raw));
		}
	}

	TArray<TSharedPtr<FJsonValue>> Contexts;
	TArray<FActionEntry> Actions;
	bool bHasLegacy = false;
	bool bTruncated = false;

	// 旧输入系统两条路都要读：混合工程（迁移中）真实存在，
	// 只报一套会让另一半绑定凭空消失
	ReadLegacyMappings(Result, bHasLegacy);

	const bool bPlaying = UAL_CommandUtils::IsPlayInProgress();
	bool bReadRuntime = false;
	FString ContextPropName;

	if (bPlaying)
	{
		UWorld* World = UAL_CommandUtils::GetLiveWorld(PlayerIndex);
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		const UObject* PlayerInput = PC ? Cast<UObject>(PC->PlayerInput) : nullptr;

		if (!PC)
		{
			// 说清是哪一步没成，不要只回一句「读不到」
			Result->SetStringField(TEXT("runtime_error"),
				UAL_CommandUtils::LStr(TEXT("游戏在跑，但这个世界里还没有 PlayerController —— 可能刚起来，等一下再试"),
					TEXT("Play is running but this world has no PlayerController yet.")));
		}
		else if (!PlayerInput)
		{
			Result->SetStringField(TEXT("runtime_error"),
				UAL_CommandUtils::LStr(TEXT("PlayerController 上没有 PlayerInput"),
					TEXT("PlayerController has no PlayerInput.")));
		}
		else
		{
			ReadAppliedContexts(PlayerInput, Contexts, ContextPropName);
			bReadRuntime = ReadRuntimeMappings(PlayerInput, Actions);
		}
	}

	if (!bReadRuntime)
	{
		// PIE 没跑（或运行时读不到）就退回资产。**这两者的含义完全不同**，
		// 靠 source 字段区分，下面还会再写一句人话
		ReadContextAssets(Contexts, Actions, bTruncated);
	}

	Result->SetStringField(TEXT("source"), bReadRuntime ? TEXT("runtime") : TEXT("asset"));
	Result->SetArrayField(TEXT("contexts"), Contexts);
	Result->SetArrayField(TEXT("actions"), ActionsToJson(Actions));
	Result->SetNumberField(TEXT("action_count"), Actions.Num());

	if (!ContextPropName.IsEmpty())
	{
		// 读自哪个属性要说出来：5.6 改名那次就是靠这条线索发现的
		Result->SetStringField(TEXT("contexts_read_from"), ContextPropName);
	}
	if (bTruncated)
	{
		Result->SetBoolField(TEXT("contexts_truncated"), true);
		Result->SetNumberField(TEXT("contexts_limit"), UAL_MaxContextAssets);
	}

	// 这个工程用的是哪一套输入系统。模型据此决定能不能按「动作」注入
	const bool bHasEnhanced = Actions.Num() > 0;
	const TCHAR* System = bHasEnhanced && bHasLegacy ? TEXT("both")
		: bHasEnhanced ? TEXT("enhanced")
		: bHasLegacy ? TEXT("legacy")
		: TEXT("none");
	Result->SetStringField(TEXT("input_system"), System);

	Result->SetStringField(TEXT("source_note"), bReadRuntime
		? UAL_CommandUtils::LStr(
			TEXT("这是游戏此刻真实生效的映射。上下文由游戏逻辑随时增删，换个场景就可能不一样。"),
			TEXT("These are the mappings actually active right now. Contexts are added/removed by "
				 "game logic, so this can differ in another scene."))
		: UAL_CommandUtils::LStr(
			TEXT("游戏没在运行，这是**工程里定义**的映射，不代表游戏里那一刻挂着哪些上下文。"
				 "要知道此刻能按什么，先把游戏跑起来再查。"),
			TEXT("Play is not running, so these are the mappings defined in the project. They do not "
				 "tell you which contexts are actually applied at any given moment.")));

	UAL_CommandUtils::AddWorldInfo(Result);
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// 注入原语（设计文档 §4）
// ============================================================================

namespace
{
	/** 一次注入最多按住多少帧。没有上限的话一个写错的参数能让角色一直往前走到会话结束 */
	constexpr int32 UAL_MaxInjectFrames = 600;

	/** 解析现场：PIE 世界 / PlayerController / 视口 / PlayerInput。缺哪一样都说清楚 */
	struct FInjectScene
	{
		UWorld* World = nullptr;
		APlayerController* PC = nullptr;
		UObject* PlayerInput = nullptr;
		UObject* Subsystem = nullptr;     // UEnhancedInputLocalPlayerSubsystem
		UGameViewportClient* Viewport = nullptr;
		FString Error;

		bool IsValid() const { return Error.IsEmpty(); }
	};

	FInjectScene ResolveInjectScene(int32 PlayerIndex)
	{
		FInjectScene Scene;

		if (!UAL_CommandUtils::IsPlayInProgress())
		{
			Scene.Error = UAL_CommandUtils::LStr(
				TEXT("游戏没在运行。注入输入只在 PIE 期间有意义 —— 先用 ue_playtest 把游戏跑起来。"),
				TEXT("Play is not running. Input injection only makes sense during PIE."));
			return Scene;
		}

		Scene.World = UAL_CommandUtils::GetLiveWorld(PlayerIndex);
		Scene.PC = Scene.World ? Scene.World->GetFirstPlayerController() : nullptr;
		if (!Scene.PC)
		{
			// 这一条是设计文档 §13.1 第 4 条的现场：PIE 刚起来时 PlayerController
			// 已经在了、Pawn 还没 Possess，这时候注入什么都没反应
			Scene.Error = UAL_CommandUtils::LStr(
				TEXT("这个世界里还没有 PlayerController —— 游戏可能刚起来，等一下再试。"),
				TEXT("No PlayerController in this world yet; play may still be starting up."));
			return Scene;
		}

		Scene.PlayerInput = Scene.PC->PlayerInput.Get();
		Scene.Viewport = Scene.World->GetGameViewport();

		if (ULocalPlayer* LocalPlayer = Scene.PC->GetLocalPlayer())
		{
			if (UClass* SubsystemClass = UALReflect::FindScriptClass(
					TEXT("/Script/EnhancedInput.EnhancedInputLocalPlayerSubsystem")))
			{
				Scene.Subsystem = LocalPlayer->GetSubsystemBase(SubsystemClass);
			}
		}
		return Scene;
	}

	/**
	 * 把「现在这个游戏能不能接受真人输入」写进返回。
	 *
	 * ## 这是整套注入里最要紧的一段
	 *
	 * 2026-09-03 在 5.8 与 5.3 上各自实测坐实：游戏切到 UIOnly（等价于打开全屏菜单）后，
	 * **动作注入照常把角色推走了（142.8），而真人按键归零（0.0）**。
	 * 引擎是在**视口那一层**拦的（`SetIgnoreInput(true)`），而动作注入根本不经过视口。
	 *
	 * 也就是说：菜单开着的时候，L1 会给出一份「移动正常」的报告，
	 * 而玩家自己按 W 一动不动。不把这件事喊出来，这套工具就会持续输出
	 * **让人相信的错误结论** —— 那比没有工具更糟。
	 */
	void AddInputModeWarnings(const FInjectScene& Scene, const TSharedPtr<FJsonObject>& Result)
	{
		TArray<TSharedPtr<FJsonValue>> Warnings;

		if (Scene.Viewport && Scene.Viewport->IgnoreInput())
		{
			Result->SetBoolField(TEXT("viewport_ignores_input"), true);
			Warnings.Add(MakeShared<FJsonValueString>(UAL_CommandUtils::LStr(
				TEXT("⚠️ 游戏当前不接受视口输入（多半是打开了菜单一类的 UI，处于 UIOnly 模式）。")
				TEXT("真人此刻按键是没反应的；按动作注入却仍然生效。")
				TEXT("**不要据此判断「操作正常」** —— 先让游戏回到正常操作状态再验。"),
				TEXT("The game is ignoring viewport input right now (likely a UI/menu in UIOnly mode). "
					 "A real player's key presses do nothing here, while action injection still works. "
					 "Do NOT conclude that input works from this run."))));
		}

		if (Scene.PC && Scene.PC->IsMoveInputIgnored())
		{
			// 反方向的同一个坑：动作会触发、PrintString 会打，但 AddMovementInput 空转。
			// 那是过场/剧情的正确行为，不是 bug —— 不说清楚就会被当成失败去查
			Result->SetBoolField(TEXT("move_input_ignored"), true);
			Warnings.Add(MakeShared<FJsonValueString>(UAL_CommandUtils::LStr(
				TEXT("⚠️ 这个 PlayerController 正忽略移动输入（SetIgnoreMoveInput，过场/剧情常用）。")
				TEXT("动作会触发，但角色不会动 —— 这是正确行为，不是失败。"),
				TEXT("This PlayerController is ignoring move input (cinematic mode). Actions still fire "
					 "but the pawn will not move; that is correct behaviour, not a failure."))));
		}

		if (Warnings.Num() > 0)
		{
			Result->SetArrayField(TEXT("warnings"), Warnings);
		}
	}

	/** 按名字或路径在运行时映射表里找动作。找不到时把候选列出来，别只回一句「没找到」 */
	const UObject* FindRuntimeAction(const UObject* PlayerInput, const FString& Wanted, TArray<FString>& OutAvailable)
	{
		TArray<FActionEntry> Entries;
		ReadRuntimeMappings(PlayerInput, Entries);

		const UObject* Found = nullptr;
		for (const FActionEntry& Entry : Entries)
		{
			if (!Entry.Action)
			{
				continue;
			}
			OutAvailable.AddUnique(Entry.Action->GetName());
			if (!Found && (Entry.Action->GetName() == Wanted || Entry.Action->GetPathName() == Wanted))
			{
				Found = Entry.Action;
			}
		}
		return Found;
	}

	/**
	 * 值类型校验。
	 *
	 * 往 Boolean 动作塞一个 2D 向量**不会报错，只会静默无效** ——
	 * 调用方会对着一份「注入成功但什么都没发生」的报告去查别的地方。
	 * 所以这里当场拒绝，并说清该给什么。
	 */
	bool ValidateValueForType(uint8 ValueType, double X, double Y, double Z, FString& OutError)
	{
		auto Nonzero = [](double V) { return FMath::Abs(V) > KINDA_SMALL_NUMBER; };

		switch (ValueType)
		{
		case 0: // Boolean
			if (Nonzero(Y) || Nonzero(Z))
			{
				OutError = UAL_CommandUtils::LStr(
					TEXT("这是 Boolean 动作，只认 x（非 0 即按下）。y/z 传了值会被静默丢掉。"),
					TEXT("This is a Boolean action; only x is used. Passing y/z would be silently dropped."));
				return false;
			}
			return true;
		case 1: // Axis1D
			if (Nonzero(Y) || Nonzero(Z))
			{
				OutError = UAL_CommandUtils::LStr(
					TEXT("这是 Axis1D 动作，只认 x。y/z 传了值会被静默丢掉。"),
					TEXT("This is an Axis1D action; only x is used."));
				return false;
			}
			return true;
		case 2: // Axis2D
			if (Nonzero(Z))
			{
				OutError = UAL_CommandUtils::LStr(
					TEXT("这是 Axis2D 动作，只认 x/y。z 传了值会被静默丢掉。"),
					TEXT("This is an Axis2D action; only x/y are used."));
				return false;
			}
			return true;
		case 3: // Axis3D
			return true;
		default:
			// 读不出类型时不拦 —— 拦一个我们没看懂的东西，比放过去更容易挡掉正常用法
			return true;
		}
	}

	/**
	 * 造一个 FInputActionValue 并注入一次。
	 *
	 * 三步链路（设计文档 §10.1.1）：
	 *   1. 让引擎造值 —— MakeInputActionValueOfType（5.1+）或 MakeInputActionValue（5.0–5.5，要种子）
	 *   2. OutStruct 拿到返回的结构体
	 *   3. StructFrom 整块搬进 InjectInputForAction 的 RawValue
	 *
	 * FInputActionValue 内部没有 UPROPERTY，逐成员赋值那条路在它身上不成立，
	 * 只能让引擎自己造好再搬字节。
	 */
	bool InjectActionOnce(const FInjectScene& Scene, const UObject* Action, uint8 ValueType,
		double X, double Y, double Z, FString& OutError)
	{
		if (!Scene.Subsystem || !Action)
		{
			OutError = TEXT("Enhanced Input subsystem unavailable");
			return false;
		}

		UClass* LibClass = UALReflect::FindScriptClass(TEXT("/Script/EnhancedInput.EnhancedInputLibrary"));
		if (!LibClass)
		{
			OutError = UAL_CommandUtils::LStr(
				TEXT("找不到 EnhancedInputLibrary —— 这个工程没启用 Enhanced Input 插件"),
				TEXT("EnhancedInputLibrary not found; the Enhanced Input plugin is not enabled."));
			return false;
		}
		UObject* LibCDO = LibClass->GetDefaultObject();

		// —— 5.1+：OfType，不需要种子
		{
			UALReflect::FCall Make(LibCDO, TEXT("MakeInputActionValueOfType"));
			if (Make.IsValid())
			{
				Make.Num(TEXT("X"), X).Num(TEXT("Y"), Y).Num(TEXT("Z"), Z).Enum(TEXT("ValueType"), ValueType);
				if (Make.Invoke())
				{
					const FStructProperty* ValueProp = nullptr;
					const void* ValueAddr = nullptr;
					if (Make.OutStruct(TEXT("ReturnValue"), ValueProp, ValueAddr))
					{
						UALReflect::FCall Inject(Scene.Subsystem, TEXT("InjectInputForAction"));
						if (!Inject.IsValid())
						{
							OutError = Inject.GetError();
							return false;
						}
						Inject.Obj(TEXT("Action"), const_cast<UObject*>(Action));
						Inject.StructFrom(TEXT("RawValue"), ValueProp, ValueAddr);
						return Inject.Invoke();
					}
				}
			}
		}

		// —— 5.0–5.5 的老形态：先取一个类型正确的种子当 MatchValueType
		UALReflect::FCall Seed(LibCDO, TEXT("GetBoundActionValue"));
		if (!Seed.IsValid())
		{
			OutError = Seed.GetError();
			return false;
		}
		Seed.Obj(TEXT("Actor"), Scene.PC ? Scene.PC->GetPawn() : nullptr)
			.Obj(TEXT("Action"), const_cast<UObject*>(Action));
		if (!Seed.Invoke())
		{
			OutError = TEXT("GetBoundActionValue failed");
			return false;
		}
		const FStructProperty* SeedProp = nullptr;
		const void* SeedAddr = nullptr;
		if (!Seed.OutStruct(TEXT("ReturnValue"), SeedProp, SeedAddr))
		{
			OutError = TEXT("GetBoundActionValue returned no struct");
			return false;
		}

		UALReflect::FCall Make(LibCDO, TEXT("MakeInputActionValue"));
		if (!Make.IsValid())
		{
			OutError = Make.GetError();
			return false;
		}
		Make.Num(TEXT("X"), X).Num(TEXT("Y"), Y).Num(TEXT("Z"), Z);
		Make.StructFrom(TEXT("MatchValueType"), SeedProp, SeedAddr);
		if (!Make.Invoke())
		{
			OutError = TEXT("MakeInputActionValue failed");
			return false;
		}

		const FStructProperty* ValueProp = nullptr;
		const void* ValueAddr = nullptr;
		if (!Make.OutStruct(TEXT("ReturnValue"), ValueProp, ValueAddr))
		{
			OutError = TEXT("MakeInputActionValue returned no struct");
			return false;
		}

		UALReflect::FCall Inject(Scene.Subsystem, TEXT("InjectInputForAction"));
		if (!Inject.IsValid())
		{
			OutError = Inject.GetError();
			return false;
		}
		Inject.Obj(TEXT("Action"), const_cast<UObject*>(Action));
		Inject.StructFrom(TEXT("RawValue"), ValueProp, ValueAddr);
		return Inject.Invoke();
	}

	/** 视口按键：真人路径的入口，会撞上 UI 那道闸 */
	bool InjectViewportKey(const FInjectScene& Scene, const FKey& Key, EInputEvent Event)
	{
		if (!Scene.Viewport || !Scene.Viewport->Viewport)
		{
			return false;
		}
		return Scene.Viewport->InputKey(
			UALKeyArgs::Make<FInputKeyEventArgs>(Scene.Viewport->Viewport, Key, Event, 0));
	}
}

void FUAL_InputCommands::Handle_InjectAction(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString ActionName;
	if (!Payload.IsValid() || !Payload->TryGetStringField(TEXT("action"), ActionName) || ActionName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, UAL_CommandUtils::LStr(
			TEXT("缺少 action —— 传动作名（如 IA_Move）或完整资产路径。先用 ue_input_map 查有哪些"),
			TEXT("Missing 'action'. Pass an action name or asset path; use ue_input_map to list them.")));
		return;
	}

	int32 PlayerIndex = 0;
	double RawIndex = 0.0;
	if (Payload->TryGetNumberField(TEXT("player_index"), RawIndex))
	{
		PlayerIndex = FMath::Max(0, static_cast<int32>(RawIndex));
	}

	const FInjectScene Scene = ResolveInjectScene(PlayerIndex);
	if (!Scene.IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 409, Scene.Error);
		return;
	}

	TArray<FString> Available;
	const UObject* Action = FindRuntimeAction(Scene.PlayerInput, ActionName, Available);
	if (!Action)
	{
		// 找不到时把当前可用的列出来。只回「没找到」的话，调用方会去猜别的名字，
		// 而真相多半是「那个动作所在的上下文此刻没挂上」
		Available.Sort();
		const FString AvailableText = Available.Num() > 0
			? FString::Join(Available, TEXT("、"))
			: UAL_CommandUtils::LStr(TEXT("（一个都没有）"), TEXT("(none)"));
		// FString::Printf 的格式串必须是字面量（UE 的编译期格式检查），
		// 双语文案是运行期才定的，所以这里拼接而不是 Printf
		UAL_CommandUtils::SendError(RequestId, 404,
			UAL_CommandUtils::LStr(TEXT("当前生效的映射里没有动作「"), TEXT("No action '"))
			+ ActionName
			+ UAL_CommandUtils::LStr(TEXT("」。此刻可用的是："), TEXT("' in the currently active mappings. Available now: "))
			+ AvailableText
			+ UAL_CommandUtils::LStr(
				TEXT("。注意这个动作是否属于一个还没挂上的输入上下文 —— 换个场景可能就有了。"),
				TEXT(". Note the action may belong to an input context that is not applied right now.")));
		return;
	}

	uint8 ValueType = 0xFF;
	ReadValueType(Action, ValueType);

	double X = 0.0, Y = 0.0, Z = 0.0;
	Payload->TryGetNumberField(TEXT("x"), X);
	Payload->TryGetNumberField(TEXT("y"), Y);
	Payload->TryGetNumberField(TEXT("z"), Z);
	bool bBoolValue = false;
	if (Payload->TryGetBoolField(TEXT("value"), bBoolValue))
	{
		// Boolean 动作允许直接给 true/false，比让调用方记住「x=1 就是按下」友好
		X = bBoolValue ? 1.0 : 0.0;
	}

	FString TypeError;
	if (!ValidateValueForType(ValueType, X, Y, Z, TypeError))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TypeError);
		return;
	}

	int32 Frames = 1;
	double RawFrames = 1.0;
	if (Payload->TryGetNumberField(TEXT("frames"), RawFrames))
	{
		Frames = FMath::Clamp(static_cast<int32>(RawFrames), 1, UAL_MaxInjectFrames);
	}

	FString InjectError;
	if (!InjectActionOnce(Scene, Action, ValueType, X, Y, Z, InjectError))
	{
		UAL_CommandUtils::SendError(RequestId, 500, InjectError.IsEmpty()
			? UAL_CommandUtils::LStr(TEXT("注入失败"), TEXT("Injection failed"))
			: InjectError);
		return;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("injected_at"), TEXT("action"));
	Result->SetStringField(TEXT("action"), Action->GetName());
	Result->SetStringField(TEXT("value_type"), ValueTypeName(ValueType));
	Result->SetNumberField(TEXT("frames"), Frames);

	// 这句必须每次都带（设计文档 §4）。L1 绕开了键位映射：一个根本没绑跳跃键的
	// 工程，这条路照样能让角色跳起来 —— 拿它当「键位正常」的证据是错的
	Result->SetStringField(TEXT("layer_note"), UAL_CommandUtils::LStr(
		TEXT("这次是按「动作」注入的，绕开了键位映射。它能验这段逻辑对不对，")
		TEXT("**不能证明键位绑对了** —— 要验键位用 input.inject_key 发真实按键。"),
		TEXT("Injected at the action layer, bypassing key mappings. This validates the logic but does "
			 "NOT prove the key bindings are correct; use input.inject_key for that.")));

	AddInputModeWarnings(Scene, Result);
	UAL_CommandUtils::AddWorldInfo(Result);

	if (Frames <= 1)
	{
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	// 注入只活一帧（引擎每帧 Reset 注入队列），按住 N 帧就要每帧补发一次。
	// 5.3+ 有连续注入 API，但那要多认一套版本差异，而每帧补发本来就是它内部做的事
	TSharedRef<int32> Remaining = MakeShared<int32>(Frames - 1);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Scene, Action, ValueType, X, Y, Z, Remaining, Result, RequestId](float) -> bool
		{
			// PIE 中途停了：世界没了就别再注入，如实收尾
			if (!UAL_CommandUtils::IsPlayInProgress())
			{
				Result->SetBoolField(TEXT("interrupted"), true);
				Result->SetNumberField(TEXT("frames_actually_injected"), 0);
				UAL_CommandUtils::SendResponse(RequestId, 200, Result);
				return false;
			}

			FString Ignored;
			InjectActionOnce(Scene, Action, ValueType, X, Y, Z, Ignored);

			if (--(*Remaining) > 0)
			{
				return true;
			}
			UAL_CommandUtils::SendResponse(RequestId, 200, Result);
			return false;
		}), 0.0f);
}

void FUAL_InputCommands::Handle_InjectKey(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString KeyName;
	if (!Payload.IsValid() || !Payload->TryGetStringField(TEXT("key"), KeyName) || KeyName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, UAL_CommandUtils::LStr(
			TEXT("缺少 key —— 传按键名（如 W、SpaceBar、LeftMouseButton）"),
			TEXT("Missing 'key' (e.g. W, SpaceBar, LeftMouseButton).")));
		return;
	}

	const FKey Key(*KeyName);
	if (!Key.IsValid())
	{
		// 拼错的键名如果放过去，视口会静默吞掉，调用方看到的是「发了但没反应」
		UAL_CommandUtils::SendError(RequestId, 400,
			UAL_CommandUtils::LStr(TEXT("引擎不认识按键「"), TEXT("Unknown key '"))
			+ KeyName
			+ UAL_CommandUtils::LStr(TEXT("」。名字要和引擎一致，如 W / SpaceBar / E / LeftMouseButton"),
				TEXT("'. Use engine key names such as W / SpaceBar / E / LeftMouseButton.")));
		return;
	}

	int32 PlayerIndex = 0;
	double RawIndex = 0.0;
	if (Payload->TryGetNumberField(TEXT("player_index"), RawIndex))
	{
		PlayerIndex = FMath::Max(0, static_cast<int32>(RawIndex));
	}

	const FInjectScene Scene = ResolveInjectScene(PlayerIndex);
	if (!Scene.IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 409, Scene.Error);
		return;
	}

	FString Mode = TEXT("tap");
	Payload->TryGetStringField(TEXT("event"), Mode);

	int32 Frames = 1;
	double RawFrames = 1.0;
	if (Payload->TryGetNumberField(TEXT("frames"), RawFrames))
	{
		Frames = FMath::Clamp(static_cast<int32>(RawFrames), 1, UAL_MaxInjectFrames);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("injected_at"), TEXT("key"));
	Result->SetStringField(TEXT("key"), Key.ToString());
	Result->SetStringField(TEXT("event"), Mode);
	Result->SetStringField(TEXT("layer_note"), UAL_CommandUtils::LStr(
		TEXT("这次走的是真人按键那条路（视口 → 键位映射 → 动作）。")
		TEXT("它能验出键位绑没绑对，也会像真人一样被 UI 挡住。"),
		TEXT("Injected through the real player path (viewport -> key mapping -> action). "
			 "This validates key bindings and is blocked by UI just like a real player.")));

	AddInputModeWarnings(Scene, Result);
	UAL_CommandUtils::AddWorldInfo(Result);

	if (Mode == TEXT("release"))
	{
		Result->SetBoolField(TEXT("accepted"), InjectViewportKey(Scene, Key, IE_Released));
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	const bool bAccepted = InjectViewportKey(Scene, Key, IE_Pressed);
	Result->SetBoolField(TEXT("accepted"), bAccepted);

	if (Mode == TEXT("press"))
	{
		// 只按下不抬起：调用方要自己发 release。写进返回，免得忘了导致角色一直走
		Result->SetStringField(TEXT("held_note"), UAL_CommandUtils::LStr(
			TEXT("键还按着。记得用 event:\"release\" 抬起来，否则角色会一直保持这个输入。"),
			TEXT("The key is still held. Send event:\"release\" or the input stays active.")));
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	// tap：按住 N 帧后自动抬起
	Result->SetNumberField(TEXT("frames"), Frames);
	TSharedRef<int32> Remaining = MakeShared<int32>(Frames);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Scene, Key, Remaining, Result, RequestId](float) -> bool
		{
			if (!UAL_CommandUtils::IsPlayInProgress())
			{
				Result->SetBoolField(TEXT("interrupted"), true);
				UAL_CommandUtils::SendResponse(RequestId, 200, Result);
				return false;
			}
			if (--(*Remaining) > 0)
			{
				return true;
			}
			// 抬起必须发生 —— 漏了的话这个键会一直算按着，之后每一次试跑都是脏的
			InjectViewportKey(Scene, Key, IE_Released);
			UAL_CommandUtils::SendResponse(RequestId, 200, Result);
			return false;
		}), 0.0f);
}
