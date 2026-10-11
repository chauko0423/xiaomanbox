#include "UAL_PropertyPath.h"

#include "UAL_CommandUtils.h"

#include "Misc/OutputDeviceNull.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALPropPath, Log, All);

namespace UALPropertyPath
{
	namespace
	{
		/** 路径的一段：字段名 + 可选的数组下标 */
		struct FSegment
		{
			FString Name;
			int32 Index = INDEX_NONE;
		};

		bool ParsePath(const FString& Path, TArray<FSegment>& OutSegments, FString& OutError)
		{
			OutSegments.Reset();

			TArray<FString> Parts;
			Path.ParseIntoArray(Parts, TEXT("."), /*InCullEmpty=*/true);
			if (Parts.Num() == 0)
			{
				OutError = TEXT("empty property path");
				return false;
			}

			for (const FString& Part : Parts)
			{
				FSegment Segment;

				int32 Open = INDEX_NONE;
				if (Part.FindChar(TEXT('['), Open))
				{
					int32 Close = INDEX_NONE;
					if (!Part.FindChar(TEXT(']'), Close) || Close < Open)
					{
						OutError = FString::Printf(TEXT("malformed array index in '%s'"), *Part);
						return false;
					}
					Segment.Name = Part.Left(Open);
					const FString IndexText = Part.Mid(Open + 1, Close - Open - 1);
					if (!IndexText.IsNumeric())
					{
						OutError = FString::Printf(TEXT("array index '%s' is not a number"), *IndexText);
						return false;
					}
					Segment.Index = FCString::Atoi(*IndexText);
					if (Segment.Index < 0)
					{
						OutError = FString::Printf(TEXT("array index %d is negative"), Segment.Index);
						return false;
					}
				}
				else
				{
					Segment.Name = Part;
				}

				if (Segment.Name.IsEmpty())
				{
					OutError = FString::Printf(TEXT("empty field name in '%s'"), *Part);
					return false;
				}
				OutSegments.Add(MoveTemp(Segment));
			}
			return true;
		}

		/** 把 JSON 值转成 UE 的属性文本字面量 */
		FString ToPropertyText(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid())
			{
				return FString();
			}

			switch (Value->Type)
			{
			case EJson::Boolean:
				return Value->AsBool() ? TEXT("true") : TEXT("false");

			case EJson::Number:
			{
				const double Number = Value->AsNumber();
				// 整数别写成 "5.000000" —— 枚举和整型属性的 ImportText 认不了小数点
				if (FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number)) &&
					FMath::Abs(Number) < 9.0e15)
				{
					return FString::Printf(TEXT("%lld"), static_cast<int64>(FMath::RoundToDouble(Number)));
				}
				return FString::SanitizeFloat(Number);
			}

			case EJson::String:
				return Value->AsString();

			case EJson::Array:
			{
				// UE 的数组字面量是 (a,b,c)
				TArray<FString> Parts;
				for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
				{
					Parts.Add(ToPropertyText(Item));
				}
				return FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(",")));
			}

			case EJson::Object:
			{
				// UE 的结构体字面量是 (X=1,Y=2)
				TArray<FString> Parts;
				for (const auto& Pair : Value->AsObject()->Values)
				{
					Parts.Add(FString::Printf(TEXT("%s=%s"),
						*UAL_JsonKey(Pair.Key), *ToPropertyText(Pair.Value)));
				}
				return FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(",")));
			}

			default:
				return FString();
			}
		}

		/**
		 * 资产路径补全：`/Game/Trees/SM_Oak` → `/Game/Trees/SM_Oak.SM_Oak`。
		 *
		 * 对象和软对象属性的 ImportText 要完整的 `包名.对象名`，只给包名会静默解析成空。
		 * 而模型和用户手里拿到的几乎永远是不带后缀的那种写法。
		 */
		FString NormalizeObjectPath(const FString& Path)
		{
			if (Path.IsEmpty() || Path.Contains(TEXT(".")) || Path.Contains(TEXT("'")))
			{
				return Path;
			}
			if (!Path.StartsWith(TEXT("/")))
			{
				return Path;
			}
			FString AssetName;
			Path.Split(TEXT("/"), nullptr, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			return AssetName.IsEmpty() ? Path : FString::Printf(TEXT("%s.%s"), *Path, *AssetName);
		}

		/**
		 * 按属性类型转文本：对象引用所在的每一层（数组/集合元素、Map 键值、结构体成员）
		 * 都补全资产路径。以前只在最外层是对象或对象数组时补，
		 * `{"StaticMaterials":[{"MaterialInterface":"/Game/M_X"}]}` 这种嵌套一层的
		 * 照样在 ImportText 里失败。认不出的形状退回属性无关的 ToPropertyText。
		 */
		FString ToPropertyTextFor(const FProperty* Prop, const TSharedPtr<FJsonValue>& Value)
		{
			if (!Prop || !Value.IsValid())
			{
				return ToPropertyText(Value);
			}
			if (Prop->IsA<FObjectPropertyBase>() && Value->Type == EJson::String)
			{
				return NormalizeObjectPath(Value->AsString());
			}
			if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
			{
				if (Value->Type == EJson::Array)
				{
					TArray<FString> Parts;
					for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
					{
						Parts.Add(ToPropertyTextFor(ArrayProp->Inner, Item));
					}
					return FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(",")));
				}
			}
			else if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
			{
				if (Value->Type == EJson::Array)
				{
					TArray<FString> Parts;
					for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
					{
						Parts.Add(ToPropertyTextFor(SetProp->ElementProp, Item));
					}
					return FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(",")));
				}
			}
			else if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
			{
				if (Value->Type == EJson::Object)
				{
					// TMap 的字面量是 ((Key, Value),(Key, Value))
					TArray<FString> Parts;
					for (const auto& Pair : Value->AsObject()->Values)
					{
						const TSharedPtr<FJsonValue> KeyValue =
							MakeShared<FJsonValueString>(UAL_JsonKey(Pair.Key));
						Parts.Add(FString::Printf(TEXT("(%s, %s)"),
							*ToPropertyTextFor(MapProp->KeyProp, KeyValue),
							*ToPropertyTextFor(MapProp->ValueProp, Pair.Value)));
					}
					return FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(",")));
				}
			}
			else if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				if (Value->Type == EJson::Object && StructProp->Struct)
				{
					TArray<FString> Parts;
					for (const auto& Pair : Value->AsObject()->Values)
					{
						const FString MemberName = UAL_JsonKey(Pair.Key);
						const FProperty* Member = StructProp->Struct->FindPropertyByName(FName(*MemberName));
						Parts.Add(FString::Printf(TEXT("%s=%s"), *MemberName, *ToPropertyTextFor(Member, Pair.Value)));
					}
					return FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(",")));
				}
			}
			return ToPropertyText(Value);
		}

		/** ImportText 的跨版本包装：5.1 起改名 ImportText_Direct，5.0 上还叫 ImportText */
		bool ImportTextCompat(FProperty* Prop, const TCHAR* Text, void* ValueAddr, UObject* Owner)
		{
			FOutputDeviceNull Silent;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			return Prop->ImportText_Direct(Text, ValueAddr, Owner, PPF_None, &Silent) != nullptr;
#else
			return Prop->ImportText(Text, ValueAddr, PPF_None, Owner, &Silent) != nullptr;
#endif
		}

		/** ExportText 的跨版本包装。同样是 5.1 起加的 `_Direct` 后缀，参数表没变 */
		FString ExportTextCompat(const FProperty* Prop, const void* ValueAddr)
		{
			FString Text;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			Prop->ExportTextItem_Direct(Text, ValueAddr, /*DefaultValue=*/nullptr, nullptr, PPF_None);
#else
			Prop->ExportTextItem(Text, ValueAddr, /*DefaultValue=*/nullptr, nullptr, PPF_None);
#endif
			return Text;
		}

		/**
		 * 这个结构体有没有**自己的**文本格式（`WithImportTextItem`）。
		 *
		 * 有的话，UE 默认那套 `(成员=值,成员=值)` 字面量就不是它认的写法，
		 * 而是它的 `ImportTextItem` 返回 false 之后的**兜底**路径 —— 兜底只写
		 * 你点到名的成员，没点到的保持原值。对这类结构体来说那几乎总是错的：
		 *
		 *   `FPCGAttributePropertySelector` 的正经写法是 `PCGBegin(Distance)PCGEnd`，
		 *   传 `{"AttributeName":"Distance"}` 会写进名字、把 `Selection` 留在
		 *   `PointProperty` 上 —— 图跑起来 0 个实例，还不报错。
		 *   （实测里要靠 Python 才看得出来。）
		 */
		bool StructHasOwnTextFormat(const FStructProperty* StructProp)
		{
			if (!StructProp || !StructProp->Struct)
			{
				return false;
			}
			// 这里不能写 const：HasImportTextItem() 在 5.0 / 5.1 上还没标 const，
			// 拿 const 指针调它编不过。GetCppStructOps() 本身从 5.0 到 5.8 一直返回
			// 非 const 指针（核对过引擎 Class.h），所以去掉 const 全版本都成立，
			// 不用加版本分支。
			UScriptStruct::ICppStructOps* Ops = StructProp->Struct->GetCppStructOps();
			return Ops && Ops->HasImportTextItem();
		}

		/** 收集一个 UStruct 下所有可写字段名，用于拼建议 */
		void CollectFieldNames(const UStruct* Struct, TArray<FString>& Out)
		{
			Out.Reset();
			if (!Struct)
			{
				return;
			}
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
				{
					Out.Add(It->GetName());
				}
			}
		}

		FString UnknownFieldError(const FString& Name, const UStruct* Struct)
		{
			TArray<FString> Known;
			CollectFieldNames(Struct, Known);

			TArray<FString> Suggestions;
			UAL_CommandUtils::SuggestProperties(Name, Known, Suggestions, 5);

			FString Error = FString::Printf(TEXT("no field '%s' on %s."),
				*Name, Struct ? *Struct->GetName() : TEXT("(null)"));
			if (Suggestions.Num() > 0)
			{
				Error += FString::Printf(TEXT(" Did you mean: %s?"), *FString::Join(Suggestions, TEXT(", ")));
			}
			return Error;
		}

		/**
		 * 沿路径走到**最后一段之前**，返回最后一段所在的容器。
		 *
		 * @param bGrowArrays 写入时为 true，允许把数组撑到需要的长度；读取时为 false
		 */
		bool Walk(
			UObject* RootMutable,
			const UObject* RootConst,
			const TArray<FSegment>& Segments,
			bool bGrowArrays,
			const UStruct*& OutStruct,
			void*& OutContainer,
			UObject*& OutOwner,
			FString& OutError)
		{
			const UObject* Current = RootMutable ? RootMutable : RootConst;
			if (!Current)
			{
				OutError = TEXT("root object is null");
				return false;
			}

			OutStruct = Current->GetClass();
			// const_cast 只用于 bGrowArrays=false 的读路径，那条路上不会写回去
			OutContainer = const_cast<UObject*>(Current);
			OutOwner = RootMutable;

			for (int32 Index = 0; Index < Segments.Num() - 1; ++Index)
			{
				const FSegment& Segment = Segments[Index];

				FProperty* Prop = OutStruct->FindPropertyByName(FName(*Segment.Name));
				if (!Prop)
				{
					OutError = UnknownFieldError(Segment.Name, OutStruct);
					return false;
				}

				void* ValueAddr = Prop->ContainerPtrToValuePtr<void>(OutContainer);

				if (Segment.Index != INDEX_NONE)
				{
					FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
					if (!ArrayProp)
					{
						OutError = FString::Printf(TEXT("'%s' is not an array, cannot index it"), *Segment.Name);
						return false;
					}
					FScriptArrayHelper Helper(ArrayProp, ValueAddr);
					if (Segment.Index >= Helper.Num())
					{
						if (!bGrowArrays)
						{
							OutError = FString::Printf(TEXT("'%s' has %d entries, index %d is out of range"),
								*Segment.Name, Helper.Num(), Segment.Index);
							return false;
						}
						// 补到需要的长度。新元素是默认值，调用方接着往里写就行
						Helper.Resize(Segment.Index + 1);
					}
					Prop = ArrayProp->Inner;
					ValueAddr = Helper.GetRawPtr(Segment.Index);
				}

				if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
				{
					OutStruct = StructProp->Struct;
					OutContainer = ValueAddr;
					continue;
				}

				if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
				{
					UObject* Inner = ObjProp->GetObjectPropertyValue(ValueAddr);
					if (!Inner)
					{
						// Instanced 子对象为空时我们不替调用方 new 一个 —— 那需要知道具体子类，
						// 猜错了后面全错。说清楚是空的，让上层决定
						OutError = FString::Printf(
							TEXT("'%s' is null, cannot descend into it. Set it to an object first."),
							*Segment.Name);
						return false;
					}
					OutStruct = Inner->GetClass();
					OutContainer = Inner;
					OutOwner = Inner;
					continue;
				}

				OutError = FString::Printf(
					TEXT("'%s' is a %s, which has no sub-fields to descend into"),
					*Segment.Name, *Prop->GetCPPType());
				return false;
			}

			return true;
		}
	}

	bool SetByPath(UObject* Root, const FString& Path, const TSharedPtr<FJsonValue>& Value, FString& OutError)
	{
		if (!Root)
		{
			OutError = TEXT("root object is null");
			return false;
		}

		TArray<FSegment> Segments;
		if (!ParsePath(Path, Segments, OutError))
		{
			return false;
		}

		const UStruct* Struct = nullptr;
		void* Container = nullptr;
		UObject* Owner = nullptr;
		if (!Walk(Root, nullptr, Segments, /*bGrowArrays=*/true, Struct, Container, Owner, OutError))
		{
			return false;
		}

		const FSegment& Last = Segments.Last();
		FProperty* Prop = Struct->FindPropertyByName(FName(*Last.Name));
		if (!Prop)
		{
			OutError = UnknownFieldError(Last.Name, Struct);
			return false;
		}
		if (Prop->HasAnyPropertyFlags(CPF_EditConst))
		{
			OutError = FString::Printf(TEXT("'%s' is read-only"), *Last.Name);
			return false;
		}

		void* ValueAddr = Prop->ContainerPtrToValuePtr<void>(Container);

		if (Last.Index != INDEX_NONE)
		{
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
			if (!ArrayProp)
			{
				OutError = FString::Printf(TEXT("'%s' is not an array, cannot index it"), *Last.Name);
				return false;
			}
			FScriptArrayHelper Helper(ArrayProp, ValueAddr);
			if (Last.Index >= Helper.Num())
			{
				Helper.Resize(Last.Index + 1);
			}
			Prop = ArrayProp->Inner;
			ValueAddr = Helper.GetRawPtr(Last.Index);
		}

		/**
		 * 自带文本格式的结构体，不接受 `{成员: 值}` 这种写法 —— **宁可报错也不半写**。
		 *
		 * 走下去的话 `ToPropertyTextFor` 会拼出 `(AttributeName=Random)`，结构体的
		 * `ImportTextItem` 认不出来返回 false，UE 兜底用默认字面量解析器逐成员合并：
		 * 点到名的写了，没点到的留在原值。返回值是**成功**，回读也「对」（读的是
		 * 刚写进去的那个成员），错的那半永远不出现在任何返回里。
		 *
		 * 所以这里挡下来，并把当前值按它自己的格式导出来放进错误信息里 ——
		 * 调用方照着那个形状改一个字符串重发就行，不用去猜，也不用读引擎源码。
		 */
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			if (Value.IsValid() && Value->Type == EJson::Object && StructHasOwnTextFormat(StructProp))
			{
				OutError = FString::Printf(
					TEXT("'%s' is a %s, which has its own text format - writing it field-by-field would ")
					TEXT("silently leave the fields you did not name at their old values. ")
					TEXT("Pass a single string in that format instead. Its current value is: %s"),
					*Last.Name,
					*Prop->GetCPPType(),
					*ExportTextCompat(Prop, ValueAddr));
				return false;
			}
		}

		// 对象引用在哪一层都补全路径（数组/集合元素、结构体成员、Map 键值）。
		// 以前只在属性本身是对象时补，OverrideMaterials 这种 TArray<UMaterialInterface*>
		// 传 /Game/M_X 就在 ImportText 里失败 —— 真机上给金币蓝图的网格换材质就是这么倒下的
		FString Text = ToPropertyTextFor(Prop, Value);

		if (Text.IsEmpty() && Value.IsValid() && Value->Type == EJson::String)
		{
			// 空字符串是合法的「清空」意图，不该被当成转换失败
			Text = TEXT("");
		}

		Root->Modify();
		if (!ImportTextCompat(Prop, *Text, ValueAddr, Owner ? Owner : Root))
		{
			OutError = FString::Printf(
				TEXT("could not write '%s' into '%s' (%s). Check the value's type and format."),
				*Text, *Last.Name, *Prop->GetCPPType());
			return false;
		}

		OutError.Reset();
		return true;
	}

	TSharedPtr<FJsonValue> GetByPath(const UObject* Root, const FString& Path, FString& OutError)
	{
		if (!Root)
		{
			OutError = TEXT("root object is null");
			return nullptr;
		}

		TArray<FSegment> Segments;
		if (!ParsePath(Path, Segments, OutError))
		{
			return nullptr;
		}

		const UStruct* Struct = nullptr;
		void* Container = nullptr;
		UObject* Owner = nullptr;
		if (!Walk(nullptr, Root, Segments, /*bGrowArrays=*/false, Struct, Container, Owner, OutError))
		{
			return nullptr;
		}

		const FSegment& Last = Segments.Last();
		FProperty* Prop = Struct->FindPropertyByName(FName(*Last.Name));
		if (!Prop)
		{
			OutError = UnknownFieldError(Last.Name, Struct);
			return nullptr;
		}

		const void* ValueAddr = Prop->ContainerPtrToValuePtr<void>(Container);

		if (Last.Index != INDEX_NONE)
		{
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
			if (!ArrayProp)
			{
				OutError = FString::Printf(TEXT("'%s' is not an array"), *Last.Name);
				return nullptr;
			}
			FScriptArrayHelper Helper(ArrayProp, const_cast<void*>(ValueAddr));
			if (Last.Index >= Helper.Num())
			{
				OutError = FString::Printf(TEXT("'%s' has %d entries, index %d is out of range"),
					*Last.Name, Helper.Num(), Last.Index);
				return nullptr;
			}
			Prop = ArrayProp->Inner;
			ValueAddr = Helper.GetRawPtr(Last.Index);
		}

		OutError.Reset();
		return UAL_CommandUtils::PropertyToJsonValueCompat(Prop, ValueAddr);
	}

	namespace
	{
		/** FlattenProperties 的递归主体。Container 是 Struct 在内存里的那份数据 */
		void FlattenInto(
			const UStruct* Struct,
			const void* Container,
			const FString& Prefix,
			TMap<FString, TSharedPtr<FJsonValue>>& Out,
			int32 Depth,
			int32 MaxDepth,
			int32 MaxEntries)
		{
			if (!Struct || !Container || Depth > MaxDepth || Out.Num() >= MaxEntries)
			{
				return;
			}

			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				if (Out.Num() >= MaxEntries)
				{
					return;
				}

				FProperty* Prop = *It;
				// 只铺用户能在细节面板里看到的那些。内部状态铺出来只会淹掉有用的
				if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
				{
					continue;
				}
				if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
				{
					continue;
				}

				const FString Path = Prefix.IsEmpty() ? Prop->GetName() : Prefix + TEXT(".") + Prop->GetName();
				const void* ValueAddr = Prop->ContainerPtrToValuePtr<void>(Container);

				// 数组：逐个元素铺开，键带下标，正好是 SetByPath 认的写法
				if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
				{
					FScriptArrayHelper Helper(ArrayProp, ValueAddr);
					for (int32 Index = 0; Index < Helper.Num() && Out.Num() < MaxEntries; ++Index)
					{
						const FString ItemPath = FString::Printf(TEXT("%s[%d]"), *Path, Index);
						const void* ItemAddr = Helper.GetRawPtr(Index);

						if (const FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProp->Inner))
						{
							FlattenInto(InnerStruct->Struct, ItemAddr, ItemPath, Out, Depth + 1, MaxDepth, MaxEntries);
						}
						else
						{
							Out.Add(ItemPath, UAL_CommandUtils::PropertyToJsonValueCompat(ArrayProp->Inner, ItemAddr));
						}
					}
					continue;
				}

				/**
				 * 结构体：往里走一层。**走进去一条都没出来的话，得把它自己报出来。**
				 *
				 * 上面那道 `CPF_Edit | CPF_BlueprintVisible` 过滤是按「用户在细节面板里
				 * 看得见什么」设的，但有一类结构体的成员**故意**都不带这两个标志 ——
				 * 它们在面板上由自定义控件画，成员不直接暴露。典型就是 PCG 的属性选择器：
				 * `Selection` / `AttributeName` 都是裸 `UPROPERTY()`
				 * （引擎 PCGAttributePropertySelector.h）。
				 *
				 * 于是递归进去、一条都不产出，而父属性自己又被 `continue` 跳过了 ——
				 * 整个属性**凭空消失，且没有任何痕迹**。调用方看到「共 55 条属性」，
				 * 里面就是没有它要找的那两个，只能以为自己记错了名字。
				 * （实测：配错的选择器靠工具回读一辈子也看不出来。）
				 *
				 * 兜底用这个结构体自己的 `ExportText`：对选择器就是
				 * `PCGBegin(Distance)PCGEnd` —— 既看得懂，又正好是 SetByPath 认的写法。
				 */
				if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
				{
					const int32 Before = Out.Num();
					FlattenInto(StructProp->Struct, ValueAddr, Path, Out, Depth + 1, MaxDepth, MaxEntries);
					if (Out.Num() == Before && Out.Num() < MaxEntries)
					{
						Out.Add(Path, MakeShared<FJsonValueString>(ExportTextCompat(Prop, ValueAddr)));
					}
					continue;
				}

				// Instanced 子对象：往里走。非 Instanced 的对象引用只报路径，
				// 跟进去会顺着资产引用把半个工程铺出来
				if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
				{
					UObject* Inner = ObjProp->GetObjectPropertyValue(ValueAddr);
					if (Inner && Prop->HasAnyPropertyFlags(CPF_InstancedReference))
					{
						FlattenInto(Inner->GetClass(), Inner, Path, Out, Depth + 1, MaxDepth, MaxEntries);
						continue;
					}
					Out.Add(Path, MakeShared<FJsonValueString>(Inner ? Inner->GetPathName() : TEXT("None")));
					continue;
				}

				Out.Add(Path, UAL_CommandUtils::PropertyToJsonValueCompat(Prop, ValueAddr));
			}
		}
	}

	void FlattenProperties(
		const UObject* Root,
		TMap<FString, TSharedPtr<FJsonValue>>& OutValues,
		int32 MaxDepth,
		int32 MaxEntries)
	{
		OutValues.Reset();
		if (!Root)
		{
			return;
		}
		FlattenInto(Root->GetClass(), Root, FString(), OutValues, 0, MaxDepth, MaxEntries);
	}

	void ListWritableFields(const UObject* Root, const FString& Path, TArray<FString>& OutNames)
	{
		OutNames.Reset();
		if (!Root)
		{
			return;
		}

		if (Path.IsEmpty())
		{
			CollectFieldNames(Root->GetClass(), OutNames);
			return;
		}

		TArray<FSegment> Segments;
		FString Error;
		if (!ParsePath(Path, Segments, Error))
		{
			return;
		}
		// Walk 只走到倒数第二段，这里补一个空段让它把整条路径都走完
		Segments.Add(FSegment{ TEXT(""), INDEX_NONE });

		const UStruct* Struct = nullptr;
		void* Container = nullptr;
		UObject* Owner = nullptr;
		if (Walk(nullptr, Root, Segments, /*bGrowArrays=*/false, Struct, Container, Owner, Error))
		{
			CollectFieldNames(Struct, OutNames);
		}
	}
}
