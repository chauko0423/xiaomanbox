#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace UALComponentInspect
{
	TSharedPtr<FJsonObject> InspectActor(AActor* Actor, const TSharedPtr<FJsonObject>& Payload);
}

namespace UALComponentInspectTest
{
	TSharedPtr<FJsonObject> Request(std::initializer_list<const TCHAR*> Components, std::initializer_list<const TCHAR*> Properties)
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> C, P;
		for (const TCHAR* S : Components) { C.Add(MakeShared<FJsonValueString>(S)); }
		for (const TCHAR* S : Properties) { P.Add(MakeShared<FJsonValueString>(S)); }
		Payload->SetArrayField(TEXT("components"), C);
		Payload->SetArrayField(TEXT("properties"), P);
		return Payload;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUALComponentInspectTest,
	"UnrealAgentLink.Actor.InspectComponents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUALComponentInspectTest::RunTest(const FString& Parameters)
{
	using namespace UALComponentInspectTest;

	// 反馈里那条：新显示角色的描边 Stencil / Custom Depth 在旧驱动上、新网格上没有 ——
	// 只读子任务要能按组件读出这两个值，而不是只拿到「第一个叫这个名字的属性」
	UWorld* World = UWorld::CreateWorld(EWorldType::None, false);
	AActor* Actor = World->SpawnActor<AActor>();
	UStaticMeshComponent* Body = NewObject<UStaticMeshComponent>(Actor, TEXT("Body"));
	Body->bRenderCustomDepth = true;
	Body->CustomDepthStencilValue = 3;
	UStaticMeshComponent* Weapon = NewObject<UStaticMeshComponent>(Actor, TEXT("Weapon"));
	Weapon->CustomDepthStencilValue = 7;

	// ── 按名字挑一个组件：读渲染状态 + 点路径属性（穿进结构体）────────────
	const TSharedPtr<FJsonObject> One = UALComponentInspect::InspectActor(Actor,
		Request({ TEXT("Body") }, { TEXT("CustomDepthStencilValue"), TEXT("BodyInstance.CollisionProfileName"), TEXT("NoSuchProp") }));
	const TArray<TSharedPtr<FJsonValue>> Components = One->GetArrayField(TEXT("components"));
	if (!TestEqual(TEXT("按名字只挑到一个"), Components.Num(), 1)) { World->DestroyWorld(false); return false; }
	const TSharedPtr<FJsonObject> Entry = Components[0]->AsObject();
	TestEqual(TEXT("挑到的是 Body"), Entry->GetStringField(TEXT("name")), FString(TEXT("Body")));
	const TSharedPtr<FJsonObject> Render = Entry->GetObjectField(TEXT("render"));
	TestTrue(TEXT("Custom Depth 开着"), Render->GetBoolField(TEXT("render_custom_depth")));
	TestEqual(TEXT("Stencil 是 Body 自己的 3，不是 Weapon 的 7"), static_cast<int32>(Render->GetNumberField(TEXT("custom_depth_stencil"))), 3);
	TestTrue(TEXT("材质槽数组在"), Render->HasTypedField<EJson::Array>(TEXT("materials")));
	const TSharedPtr<FJsonObject> Props = Entry->GetObjectField(TEXT("properties"));
	TestEqual(TEXT("点路径读顶层"), static_cast<int32>(Props->GetNumberField(TEXT("CustomDepthStencilValue"))), 3);
	TestTrue(TEXT("点路径穿进结构体"), Props->HasField(TEXT("BodyInstance.CollisionProfileName")));
	TestTrue(TEXT("读不到的属性单独报错，不拖垮其它"), Entry->GetObjectField(TEXT("property_errors"))->HasField(TEXT("NoSuchProp")));

	// ── 按类名挑：两个都要 ─────────────────────────────────────────────────
	const TSharedPtr<FJsonObject> ByClass = UALComponentInspect::InspectActor(Actor, Request({ TEXT("UStaticMeshComponent") }, {}));
	TestEqual(TEXT("按类名挑到两个"), ByClass->GetArrayField(TEXT("components")).Num(), 2);

	// ── 名字对不上：给名单，不让调用方接着猜 ─────────────────────────────
	const TSharedPtr<FJsonObject> Miss = UALComponentInspect::InspectActor(Actor, Request({ TEXT("Nope") }, {}));
	TestEqual(TEXT("没挑到"), Miss->GetArrayField(TEXT("components")).Num(), 0);
	TestTrue(TEXT("给出可选名单"), Miss->HasTypedField<EJson::Array>(TEXT("available_components")));

	World->DestroyWorld(false);
	return true;
}

#endif
