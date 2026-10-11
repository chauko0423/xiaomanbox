#include "UAL_ComponentInspectCommands.h"

#include "UAL_CommandUtils.h"
#include "UAL_PropertyPath.h"

#include "Animation/AnimInstance.h"
#include "Components/ActorComponent.h"
#include "Components/MeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"

/**
 * `actor.inspect_components` —— 组件级只读回读。
 *
 * ## 为什么要有
 *
 * 2026-09-24 买量定序器反馈（缺口 4）：只读评审子任务要查「新显示角色身上有没有描边和覆层」
 * 「Boss 的溶解在哪几个材质槽」「小怪 CDO 的攻击动画映射」，手上的只读工具都够不到 ——
 * `ue_get_actor(properties)` 只收顶层名字，同名属性取第一个组件的，而且读不到每个槽实际在用的材质；
 * `blueprint_describe` 给组件树不给组件上的值。子任务只能回一句「需主流程用 Python 补读」，
 * 主流程再用高权限 Python 一项项补。放开只读子任务的 Python 不是答案（那就不是只读了），
 * 缺的是一个够细的只读入口。
 *
 * ## 读什么
 *
 * - 对象：场景 Actor（`targets`，和 ue_get_actor 同一套寻址；PIE 在跑就读游戏里那份），
 *   或蓝图（`blueprint_path`：CDO + 本类及父类的组件模板）
 * - 组件：按名字或类名挑，不给就是全部
 * - 渲染状态（默认开）：每个槽**实际在用**的材质（覆盖过的标出来）、槽名、覆层材质、
 *   Custom Depth 与 Stencil、显隐、碰撞预设、网格资产、动画蓝图 / 动画模式
 * - 属性：点路径（`A.B[2].C`），走 UALPropertyPath::GetByPath，结构体、数组、对象引用都能读
 *
 * 版本差异用反射探测：OverlayMaterial 5.1 才有，SkeletalMesh 5.1 起改名 SkeletalMeshAsset。
 */
namespace UALComponentInspect
{
	using FJson = TSharedPtr<FJsonObject>;
	using FValues = TArray<TSharedPtr<FJsonValue>>;

	constexpr int32 MaxActors = 20;
	constexpr int32 MaxComponentsPerTarget = 60;
	constexpr int32 MaxSlots = 64;

	/** 按名字读一个对象引用属性。属性不存在（老版本引擎）返回 false，与「存在但为空」区分开 */
	bool ReflectObject(const UObject* Obj, const TCHAR* Name, UObject*& Out)
	{
		Out = nullptr;
		const FObjectPropertyBase* Prop = Obj ? FindFProperty<FObjectPropertyBase>(Obj->GetClass(), Name) : nullptr;
		if (!Prop) { return false; }
		Out = Prop->GetObjectPropertyValue_InContainer(Obj);
		return true;
	}

	TSharedPtr<FJsonValue> PathOrNull(const UObject* Obj)
	{
		return Obj ? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(Obj->GetPathName()))
			: StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>());
	}

	FJson RenderState(const UActorComponent* Component)
	{
		const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
		if (!Primitive) { return nullptr; }

		FJson Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("visible"), Primitive->IsVisible());
		Out->SetBoolField(TEXT("hidden_in_game"), Primitive->bHiddenInGame);
		Out->SetBoolField(TEXT("render_custom_depth"), Primitive->bRenderCustomDepth);
		Out->SetNumberField(TEXT("custom_depth_stencil"), Primitive->CustomDepthStencilValue);
		Out->SetStringField(TEXT("collision_profile"), Primitive->GetCollisionProfileName().ToString());

		// 每个槽**实际在用**的材质：GetMaterial 已经把覆盖和网格默认合在一起了。
		// 只读 OverrideMaterials 的话，没覆盖的槽全是空，看不出它现在渲的是什么
		const UMeshComponent* Mesh = Cast<UMeshComponent>(Primitive);
		TArray<FName> SlotNames;
		if (Mesh) { SlotNames = Mesh->GetMaterialSlotNames(); }
		FValues Slots;
		const int32 Count = Primitive->GetNumMaterials();
		for (int32 I = 0; I < Count && I < MaxSlots; ++I)
		{
			FJson Slot = MakeShared<FJsonObject>();
			Slot->SetNumberField(TEXT("slot"), I);
			if (SlotNames.IsValidIndex(I)) { Slot->SetStringField(TEXT("name"), SlotNames[I].ToString()); }
			Slot->SetField(TEXT("material"), PathOrNull(Primitive->GetMaterial(I)));
			if (Mesh && Mesh->OverrideMaterials.IsValidIndex(I) && Mesh->OverrideMaterials[I])
			{
				Slot->SetBoolField(TEXT("overridden"), true);
			}
			Slots.Add(MakeShared<FJsonValueObject>(Slot));
		}
		Out->SetArrayField(TEXT("materials"), Slots);
		if (Count > MaxSlots) { Out->SetNumberField(TEXT("material_slot_count"), Count); }

		// 覆层材质 5.1 起才有，反射探测；不存在就不写这个字段，而不是写个 null 冒充「没设」
		UObject* Overlay = nullptr;
		if (ReflectObject(Primitive, TEXT("OverlayMaterial"), Overlay))
		{
			Out->SetField(TEXT("overlay_material"), PathOrNull(Overlay));
		}

		if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Primitive))
		{
			Out->SetField(TEXT("mesh"), PathOrNull(Static->GetStaticMesh()));
		}
		if (const USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Primitive))
		{
			UObject* Asset = nullptr;
			if (ReflectObject(Skeletal, TEXT("SkeletalMeshAsset"), Asset) || ReflectObject(Skeletal, TEXT("SkeletalMesh"), Asset))
			{
				Out->SetField(TEXT("mesh"), PathOrNull(Asset));
			}
			Out->SetField(TEXT("anim_class"), PathOrNull(Skeletal->AnimClass.Get()));
			Out->SetStringField(TEXT("animation_mode"),
				Skeletal->GetAnimationMode() == EAnimationMode::AnimationBlueprint ? TEXT("anim_blueprint")
				: Skeletal->GetAnimationMode() == EAnimationMode::AnimationSingleNode ? TEXT("single_node")
				: TEXT("custom"));
			// 运行时实际挂上的动画实例（PIE 里才有）。和 anim_class 不一致就是被别处换掉了
			if (const UAnimInstance* Instance = Skeletal->GetAnimInstance())
			{
				Out->SetStringField(TEXT("anim_instance_class"), Instance->GetClass()->GetPathName());
			}
		}
		return Out;
	}

	void ReadPaths(const UObject* Root, const TArray<FString>& Paths, const FJson& Into)
	{
		if (Paths.Num() == 0) { return; }
		FJson Values = MakeShared<FJsonObject>();
		FJson Errors = MakeShared<FJsonObject>();
		for (const FString& Path : Paths)
		{
			FString Error;
			if (TSharedPtr<FJsonValue> Value = UALPropertyPath::GetByPath(Root, Path, Error)) { Values->SetField(Path, Value); }
			else { Errors->SetStringField(Path, Error); }
		}
		Into->SetObjectField(TEXT("properties"), Values);
		if (Errors->Values.Num() > 0) { Into->SetObjectField(TEXT("property_errors"), Errors); }
	}

	/** 按名字或类名挑组件；类名 `SkeletalMeshComponent` 和 `USkeletalMeshComponent` 都认 */
	bool Selected(const FString& Name, const UActorComponent* Component, const TArray<FString>& Selectors)
	{
		if (Selectors.Num() == 0) { return true; }
		for (const FString& S : Selectors)
		{
			if (S == TEXT("*") || Name.Equals(S, ESearchCase::IgnoreCase)) { return true; }
			for (const UClass* C = Component->GetClass(); C; C = C->GetSuperClass())
			{
				if (C->GetName().Equals(S, ESearchCase::IgnoreCase) || (S.StartsWith(TEXT("U")) && C->GetName().Equals(S.Mid(1), ESearchCase::IgnoreCase)))
				{
					return true;
				}
			}
		}
		return false;
	}

	struct FNamedComponent
	{
		FString Name;
		const UActorComponent* Component = nullptr;
		FString AttachParent;
	};

	FJson ComponentEntry(const FNamedComponent& Named, bool bRender, const TArray<FString>& Paths)
	{
		FJson Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Named.Name);
		Entry->SetStringField(TEXT("class"), Named.Component->GetClass()->GetName());
		if (!Named.AttachParent.IsEmpty()) { Entry->SetStringField(TEXT("attach_parent"), Named.AttachParent); }
		if (bRender)
		{
			if (FJson Render = RenderState(Named.Component)) { Entry->SetObjectField(TEXT("render"), Render); }
		}
		ReadPaths(Named.Component, Paths, Entry);
		return Entry;
	}

	/** 蓝图的组件：本类和父蓝图类的 SCS 模板，加上 CDO 上 C++ 声明的默认子对象 */
	TArray<FNamedComponent> BlueprintComponents(UBlueprintGeneratedClass* Class, const AActor* Cdo)
	{
		TArray<FNamedComponent> Out;
		TSet<const UActorComponent*> Seen;
		for (UClass* C = Class; C; C = C->GetSuperClass())
		{
			UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(C);
			if (!Generated || !Generated->SimpleConstructionScript) { continue; }
			for (USCS_Node* Node : Generated->SimpleConstructionScript->GetAllNodes())
			{
				if (!Node || !Node->ComponentTemplate || Seen.Contains(Node->ComponentTemplate)) { continue; }
				Seen.Add(Node->ComponentTemplate);
				FNamedComponent Named;
				Named.Name = Node->GetVariableName().ToString();
				Named.Component = Node->ComponentTemplate;
				Named.AttachParent = Node->ParentComponentOrVariableName.ToString();
				if (Named.AttachParent == TEXT("None")) { Named.AttachParent.Reset(); }
				Out.Add(Named);
			}
		}
		if (Cdo)
		{
			TArray<UActorComponent*> Native;
			Cdo->GetComponents(Native);
			for (const UActorComponent* Component : Native)
			{
				if (!Component || Seen.Contains(Component)) { continue; }
				Seen.Add(Component);
				FNamedComponent Named;
				Named.Name = Component->GetName();
				Named.Component = Component;
				Out.Add(Named);
			}
		}
		return Out;
	}

	TArray<FNamedComponent> ActorComponents(const AActor* Actor)
	{
		TArray<FNamedComponent> Out;
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (const UActorComponent* Component : Components)
		{
			if (!Component) { continue; }
			FNamedComponent Named;
			Named.Name = Component->GetName();
			Named.Component = Component;
			if (const USceneComponent* Scene = Cast<USceneComponent>(Component))
			{
				if (const USceneComponent* Parent = Scene->GetAttachParent()) { Named.AttachParent = Parent->GetName(); }
			}
			Out.Add(Named);
		}
		return Out;
	}

	TArray<FString> Strings(const TSharedPtr<FJsonObject>& Payload, const TCHAR* Field)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Payload->TryGetArrayField(Field, Array) && Array)
		{
			for (const TSharedPtr<FJsonValue>& V : *Array) { FString S; if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty()) Out.Add(S); }
		}
		return Out;
	}

	FJson TargetEntry(const TCHAR* Kind, const FString& Name, const UObject* Object, const TArray<FNamedComponent>& Components,
		const TArray<FString>& Selectors, bool bRender, const TArray<FString>& Paths, const TArray<FString>& ObjectPaths)
	{
		FJson Target = MakeShared<FJsonObject>();
		Target->SetStringField(TEXT("kind"), Kind);
		Target->SetStringField(TEXT("name"), Name);
		Target->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());
		ReadPaths(Object, ObjectPaths, Target);

		FValues Entries;
		TArray<FString> Available;
		for (const FNamedComponent& Named : Components)
		{
			Available.Add(Named.Name);
			if (!Selected(Named.Name, Named.Component, Selectors)) { continue; }
			if (Entries.Num() >= MaxComponentsPerTarget) { Target->SetBoolField(TEXT("truncated"), true); break; }
			Entries.Add(MakeShared<FJsonValueObject>(ComponentEntry(Named, bRender, Paths)));
		}
		Target->SetArrayField(TEXT("components"), Entries);
		// 挑的组件一个都没对上时，把名单给出去 —— 否则调用方只能换个名字再猜
		if (Entries.Num() == 0 && Selectors.Num() > 0)
		{
			FValues Names;
			for (const FString& N : Available) { Names.Add(MakeShared<FJsonValueString>(N)); }
			Target->SetArrayField(TEXT("available_components"), Names);
		}
		return Target;
	}

	/** 一个场景 Actor。命令和自动化测试共用（测试在临时世界里生成 Actor，走不了 targets 寻址） */
	FJson InspectActor(AActor* Actor, const TSharedPtr<FJsonObject>& Payload)
	{
		bool bRender = true;
		Payload->TryGetBoolField(TEXT("render_state"), bRender);
		return TargetEntry(TEXT("actor"), UAL_CommandUtils::GetActorFriendlyName(Actor), Actor, ActorComponents(Actor),
			Strings(Payload, TEXT("components")), bRender, Strings(Payload, TEXT("properties")), Strings(Payload, TEXT("object_properties")));
	}

	/** 纯函数：给命令和自动化测试共用 */
	FJson Inspect(const TSharedPtr<FJsonObject>& Payload, FString& Error)
	{
		const TArray<FString> Selectors = Strings(Payload, TEXT("components"));
		const TArray<FString> Paths = Strings(Payload, TEXT("properties"));
		const TArray<FString> ObjectPaths = Strings(Payload, TEXT("object_properties"));
		bool bRender = true;
		Payload->TryGetBoolField(TEXT("render_state"), bRender);

		FJson Data = MakeShared<FJsonObject>();
		FValues Targets;

		FString BlueprintPath;
		if (Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) && !BlueprintPath.IsEmpty())
		{
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
			UBlueprintGeneratedClass* Class = Blueprint ? Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass) : nullptr;
			if (!Class)
			{
				Error = FString::Printf(TEXT("找不到蓝图或它还没编译出类：%s"), *BlueprintPath);
				return nullptr;
			}
			UObject* Cdo = Class->GetDefaultObject();
			const AActor* ActorCdo = Cast<AActor>(Cdo);
			Targets.Add(MakeShared<FJsonValueObject>(TargetEntry(TEXT("blueprint_default"), Blueprint->GetName(), Cdo,
				BlueprintComponents(Class, ActorCdo), Selectors, bRender, Paths, ObjectPaths)));
			// 读的是类默认值和组件模板。构造脚本、BeginPlay 里再改的值不在这里 ——
			// 要看运行时实际值就对场景里的实例（或 PIE 里的）用 targets 读
			Data->SetStringField(TEXT("note"), TEXT("蓝图读的是类默认值和组件模板，不含构造脚本 / BeginPlay 之后的改动；要看实际值请对场景或 PIE 里的实例读。"));
		}
		else
		{
			const TSharedPtr<FJsonObject>* TargetsObj = nullptr;
			if (!Payload->TryGetObjectField(TEXT("targets"), TargetsObj) || !TargetsObj)
			{
				Error = TEXT("需要 targets（场景里的 Actor）或 blueprint_path（蓝图默认值）之一");
				return nullptr;
			}
			UWorld* World = UAL_CommandUtils::GetLiveWorld();
			TSet<AActor*> Actors;
			TArray<FString> Unmatched;
			if (!UAL_CommandUtils::ResolveTargetsToActors(*TargetsObj, World, Actors, Error, &Unmatched)) { return nullptr; }
			if (Actors.Num() == 0)
			{
				Error = FString::Printf(TEXT("没有匹配的 Actor（%s）。当前读的是%s世界"),
					*FString::Join(Unmatched, TEXT("、")), UAL_CommandUtils::WorldKindName(World));
				return nullptr;
			}
			int32 Count = 0;
			for (AActor* Actor : Actors)
			{
				if (++Count > MaxActors) { Data->SetBoolField(TEXT("truncated"), true); break; }
				Targets.Add(MakeShared<FJsonValueObject>(InspectActor(Actor, Payload)));
			}
			UAL_CommandUtils::AddWorldInfo(Data);
		}
		Data->SetArrayField(TEXT("targets"), Targets);
		return Data;
	}
}

void FUAL_ComponentInspectCommands::RegisterCommands(
	TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& Commands)
{
	Commands.Add(TEXT("actor.inspect_components"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		FString Error;
		const TSharedPtr<FJsonObject> Data = UALComponentInspect::Inspect(Payload, Error);
		if (Data.IsValid()) { UAL_CommandUtils::SendResponse(RequestId, 200, Data); }
		else { UAL_CommandUtils::SendError(RequestId, 400, Error); }
	});
}
