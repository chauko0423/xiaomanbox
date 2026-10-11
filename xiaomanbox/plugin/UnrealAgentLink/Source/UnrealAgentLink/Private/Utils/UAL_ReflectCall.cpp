#include "UAL_ReflectCall.h"

#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALReflect, Log, All);

namespace UALReflect
{
	UClass* FindScriptClass(const TCHAR* PackageDotClass)
	{
		if (!PackageDotClass || *PackageDotClass == TEXT('\0'))
		{
			return nullptr;
		}
		// 显式全路径 + nullptr Outer 是 5.0–5.8 都成立的写法。
		// FindObject 而不是 LoadObject：/Script/ 包是模块加载时建的，
		// 插件没启用就该查不到，不该去尝试加载。
		return FindObject<UClass>(nullptr, PackageDotClass);
	}

	void FindDerivedClasses(UClass* Base, TArray<UClass*>& OutClasses)
	{
		OutClasses.Reset();
		if (!Base)
		{
			return;
		}

		TArray<UClass*> Derived;
		GetDerivedClasses(Base, Derived, /*bRecursive=*/true);

		OutClasses.Reserve(Derived.Num());
		for (UClass* Cls : Derived)
		{
			if (!Cls)
			{
				continue;
			}
			// 抽象类建不出节点；SKEL_/REINST_ 是蓝图重编译留下的临时类，
			// 列给模型看只会让它挑到一个建出来就崩的类型
			if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}
			const FString ClassName = Cls->GetName();
			if (ClassName.StartsWith(TEXT("SKEL_")) || ClassName.StartsWith(TEXT("REINST_")))
			{
				continue;
			}
			OutClasses.Add(Cls);
		}
	}

	// ------------------------------------------------------------------
	// UPROPERTY 读
	// ------------------------------------------------------------------

	namespace
	{
		/** 找属性并拿到它在 Obj 里的地址。找不到返回 nullptr。 */
		template <typename TPropType>
		const TPropType* FindPropAndAddr(const UObject* Obj, const TCHAR* PropName, const void*& OutAddr)
		{
			OutAddr = nullptr;
			if (!Obj || !PropName)
			{
				return nullptr;
			}
			FProperty* Prop = Obj->GetClass()->FindPropertyByName(FName(PropName));
			const TPropType* Typed = CastField<TPropType>(Prop);
			if (!Typed)
			{
				return nullptr;
			}
			OutAddr = Typed->template ContainerPtrToValuePtr<void>(Obj);
			return Typed;
		}
	}

	UObject* GetObjectProp(const UObject* Obj, const TCHAR* PropName)
	{
		const void* Addr = nullptr;
		const FObjectPropertyBase* Prop = FindPropAndAddr<FObjectPropertyBase>(Obj, PropName, Addr);
		if (!Prop || !Addr)
		{
			return nullptr;
		}
		return Prop->GetObjectPropertyValue(Addr);
	}

	bool GetObjectArrayProp(const UObject* Obj, const TCHAR* PropName, TArray<UObject*>& Out)
	{
		Out.Reset();

		const void* Addr = nullptr;
		const FArrayProperty* ArrayProp = FindPropAndAddr<FArrayProperty>(Obj, PropName, Addr);
		if (!ArrayProp || !Addr)
		{
			return false;
		}
		const FObjectPropertyBase* Inner = CastField<FObjectPropertyBase>(ArrayProp->Inner);
		if (!Inner)
		{
			return false;
		}

		FScriptArrayHelper Helper(ArrayProp, Addr);
		Out.Reserve(Helper.Num());
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			// 数组里混 null 是正常的（节点被删过），交给调用方跳过而不是在这里吞掉，
			// 否则下标和引擎里的顺序对不上
			Out.Add(Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index)));
		}
		return true;
	}

	bool GetIntProp(const UObject* Obj, const TCHAR* PropName, int32& Out)
	{
		const void* Addr = nullptr;
		const FNumericProperty* Prop = FindPropAndAddr<FNumericProperty>(Obj, PropName, Addr);
		if (!Prop || !Addr || !Prop->IsInteger())
		{
			return false;
		}
		Out = static_cast<int32>(Prop->GetSignedIntPropertyValue(Addr));
		return true;
	}

	bool GetBoolProp(const UObject* Obj, const TCHAR* PropName, bool& Out)
	{
		const void* Addr = nullptr;
		const FBoolProperty* Prop = FindPropAndAddr<FBoolProperty>(Obj, PropName, Addr);
		if (!Prop || !Addr)
		{
			return false;
		}
		Out = Prop->GetPropertyValue(Addr);
		return true;
	}

	bool GetNameProp(const UObject* Obj, const TCHAR* PropName, FName& Out)
	{
		const void* Addr = nullptr;
		const FNameProperty* Prop = FindPropAndAddr<FNameProperty>(Obj, PropName, Addr);
		if (!Prop || !Addr)
		{
			return false;
		}
		Out = Prop->GetPropertyValue(Addr);
		return true;
	}

	bool GetStringProp(const UObject* Obj, const TCHAR* PropName, FString& Out)
	{
		const void* Addr = nullptr;
		const FStrProperty* Prop = FindPropAndAddr<FStrProperty>(Obj, PropName, Addr);
		if (!Prop || !Addr)
		{
			return false;
		}
		Out = Prop->GetPropertyValue(Addr);
		return true;
	}

	bool GetNameInStructProp(const UObject* Obj, const TCHAR* StructPropName, const TCHAR* MemberName, FName& Out)
	{
		const void* Addr = nullptr;
		const FStructProperty* StructProp = FindPropAndAddr<FStructProperty>(Obj, StructPropName, Addr);
		if (!StructProp || !Addr || !StructProp->Struct)
		{
			return false;
		}
		FProperty* Member = StructProp->Struct->FindPropertyByName(FName(MemberName));
		const FNameProperty* NameMember = CastField<FNameProperty>(Member);
		if (!NameMember)
		{
			return false;
		}
		Out = NameMember->GetPropertyValue(NameMember->ContainerPtrToValuePtr<void>(Addr));
		return true;
	}

	bool GetBoolInStructProp(const UObject* Obj, const TCHAR* StructPropName, const TCHAR* MemberName, bool& Out)
	{
		const void* Addr = nullptr;
		const FStructProperty* StructProp = FindPropAndAddr<FStructProperty>(Obj, StructPropName, Addr);
		if (!StructProp || !Addr || !StructProp->Struct)
		{
			return false;
		}
		FProperty* Member = StructProp->Struct->FindPropertyByName(FName(MemberName));
		const FBoolProperty* BoolMember = CastField<FBoolProperty>(Member);
		if (!BoolMember)
		{
			return false;
		}
		Out = BoolMember->GetPropertyValue(BoolMember->ContainerPtrToValuePtr<void>(Addr));
		return true;
	}

	// ------------------------------------------------------------------
	// UPROPERTY 写
	// ------------------------------------------------------------------

	bool SetIntProp(UObject* Obj, const TCHAR* PropName, int32 Value)
	{
		if (!Obj || !PropName)
		{
			return false;
		}
		FProperty* Prop = Obj->GetClass()->FindPropertyByName(FName(PropName));
		FNumericProperty* Numeric = CastField<FNumericProperty>(Prop);
		if (!Numeric || !Numeric->IsInteger())
		{
			return false;
		}
		Numeric->SetIntPropertyValue(Numeric->ContainerPtrToValuePtr<void>(Obj), static_cast<int64>(Value));
		return true;
	}

	bool SetStringProp(UObject* Obj, const TCHAR* PropName, const FString& Value)
	{
		if (!Obj || !PropName)
		{
			return false;
		}
		FProperty* Prop = Obj->GetClass()->FindPropertyByName(FName(PropName));
		FStrProperty* StrProp = CastField<FStrProperty>(Prop);
		if (!StrProp)
		{
			return false;
		}
		StrProp->SetPropertyValue(StrProp->ContainerPtrToValuePtr<void>(Obj), Value);
		return true;
	}

	bool SetNameProp(UObject* Obj, const TCHAR* PropName, FName Value)
	{
		if (!Obj || !PropName)
		{
			return false;
		}
		FProperty* Prop = Obj->GetClass()->FindPropertyByName(FName(PropName));
		FNameProperty* NameProp = CastField<FNameProperty>(Prop);
		if (!NameProp)
		{
			return false;
		}
		NameProp->SetPropertyValue(NameProp->ContainerPtrToValuePtr<void>(Obj), Value);
		return true;
	}

	bool SetObjectProp(UObject* Obj, const TCHAR* PropName, UObject* Value)
	{
		if (!Obj || !PropName)
		{
			return false;
		}
		FProperty* Prop = Obj->GetClass()->FindPropertyByName(FName(PropName));
		FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop);
		if (!ObjProp)
		{
			return false;
		}
		// 类不对就不写：塞一个错类型的对象进去，引擎那头要到用的时候才崩
		if (Value && ObjProp->PropertyClass && !Value->IsA(ObjProp->PropertyClass))
		{
			return false;
		}
		ObjProp->SetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(Obj), Value);
		return true;
	}

	// ------------------------------------------------------------------
	// FCall
	// ------------------------------------------------------------------

	FCall::FCall(UObject* InTarget, const TCHAR* FuncName)
	{
		if (!InTarget)
		{
			Error = TEXT("target object is null");
			return;
		}
		if (!FuncName)
		{
			Error = TEXT("function name is null");
			return;
		}

		UFunction* Found = InTarget->FindFunction(FName(FuncName));
		if (!Found)
		{
			Error = FString::Printf(TEXT("%s has no reflected function '%s'"),
				*InTarget->GetClass()->GetName(), FuncName);
			return;
		}

		Target = InTarget;
		Function = Found;

		// 参数缓冲按 ParmsSize 开，再逐个 InitializeValue —— 光 Memzero 对
		// FString/TArray 这类有构造语义的形参不安全
		Buffer.SetNumUninitialized(Function->ParmsSize);
		FMemory::Memzero(Buffer.GetData(), Function->ParmsSize);
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->InitializeValue_InContainer(Buffer.GetData());
		}
	}

	FCall::~FCall()
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

	FProperty* FCall::FindParam(const TCHAR* ParamName) const
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

	FCall& FCall::Obj(const TCHAR* ParamName, UObject* Value)
	{
		if (FObjectPropertyBase* Prop = CastField<FObjectPropertyBase>(FindParam(ParamName)))
		{
			Prop->SetObjectPropertyValue(Prop->ContainerPtrToValuePtr<void>(Buffer.GetData()), Value);
		}
		else if (Function)
		{
			// 形参名拼错不会让 ProcessEvent 报错，只会静默传 null —— 必须自己喊出来
			UE_LOG(LogUALReflect, Warning, TEXT("%s: no object param '%s'"), *Function->GetName(), ParamName);
		}
		return *this;
	}

	FCall& FCall::Cls(const TCHAR* ParamName, UClass* Value)
	{
		if (FClassProperty* Prop = CastField<FClassProperty>(FindParam(ParamName)))
		{
			Prop->SetObjectPropertyValue(Prop->ContainerPtrToValuePtr<void>(Buffer.GetData()), Value);
		}
		else if (Function)
		{
			UE_LOG(LogUALReflect, Warning, TEXT("%s: no class param '%s'"), *Function->GetName(), ParamName);
		}
		return *this;
	}

	FCall& FCall::Name(const TCHAR* ParamName, FName Value)
	{
		if (FNameProperty* Prop = CastField<FNameProperty>(FindParam(ParamName)))
		{
			Prop->SetPropertyValue(Prop->ContainerPtrToValuePtr<void>(Buffer.GetData()), Value);
		}
		else if (Function)
		{
			UE_LOG(LogUALReflect, Warning, TEXT("%s: no name param '%s'"), *Function->GetName(), ParamName);
		}
		return *this;
	}

	FCall& FCall::Bool(const TCHAR* ParamName, bool Value)
	{
		if (FBoolProperty* Prop = CastField<FBoolProperty>(FindParam(ParamName)))
		{
			Prop->SetPropertyValue(Prop->ContainerPtrToValuePtr<void>(Buffer.GetData()), Value);
		}
		else if (Function)
		{
			UE_LOG(LogUALReflect, Warning, TEXT("%s: no bool param '%s'"), *Function->GetName(), ParamName);
		}
		return *this;
	}

	FCall& FCall::Num(const TCHAR* ParamName, double Value)
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
		else if (Function)
		{
			UE_LOG(LogUALReflect, Warning, TEXT("%s: no float/double param '%s'"), *Function->GetName(), ParamName);
		}
		return *this;
	}

	FCall& FCall::Enum(const TCHAR* ParamName, uint8 Value)
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
		else if (Function)
		{
			UE_LOG(LogUALReflect, Warning, TEXT("%s: no enum param '%s'"), *Function->GetName(), ParamName);
		}
		return *this;
	}

	FCall& FCall::StructFrom(const TCHAR* ParamName, const FStructProperty* SrcProp, const void* SrcValueAddr)
	{
		FStructProperty* Dst = CastField<FStructProperty>(FindParam(ParamName));
		if (!Dst || !SrcProp || !SrcValueAddr)
		{
			if (Function)
			{
				UE_LOG(LogUALReflect, Warning, TEXT("%s: no struct param '%s'"), *Function->GetName(), ParamName);
			}
			return *this;
		}
		if (Dst->Struct != SrcProp->Struct)
		{
			// 类型不同还硬搬的话不会报错，只会在运行时给出乱值 —— 那种错最难查
			UE_LOG(LogUALReflect, Warning,
				TEXT("param '%s' is %s but source is %s; refusing to copy"),
				ParamName,
				Dst->Struct ? *Dst->Struct->GetName() : TEXT("<null>"),
				SrcProp->Struct ? *SrcProp->Struct->GetName() : TEXT("<null>"));
			return *this;
		}
		Dst->Struct->CopyScriptStruct(Dst->ContainerPtrToValuePtr<void>(Buffer.GetData()), SrcValueAddr);
		return *this;
	}

	bool FCall::Invoke()
	{
		if (!IsValid())
		{
			return false;
		}
		Target->ProcessEvent(Function, Buffer.GetData());
		bInvoked = true;
		return true;
	}

	UObject* FCall::OutObject(const TCHAR* ParamName) const
	{
		if (!bInvoked)
		{
			return nullptr;
		}
		const FObjectPropertyBase* Prop = CastField<FObjectPropertyBase>(FindParam(ParamName));
		if (!Prop)
		{
			return nullptr;
		}
		return Prop->GetObjectPropertyValue(Prop->ContainerPtrToValuePtr<void>(Buffer.GetData()));
	}

	bool FCall::OutBool(const TCHAR* ParamName, bool DefaultValue) const
	{
		if (!bInvoked)
		{
			return DefaultValue;
		}
		const FBoolProperty* Prop = CastField<FBoolProperty>(FindParam(ParamName));
		if (!Prop)
		{
			return DefaultValue;
		}
		return Prop->GetPropertyValue(Prop->ContainerPtrToValuePtr<void>(Buffer.GetData()));
	}

	bool FCall::OutStruct(const TCHAR* ParamName, const FStructProperty*& OutProp, const void*& OutAddr) const
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
}
