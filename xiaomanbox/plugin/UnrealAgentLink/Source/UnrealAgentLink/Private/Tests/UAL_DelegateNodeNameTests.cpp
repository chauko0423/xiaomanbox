#include "UAL_DelegateNodeName.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

using UAL_DelegateNodeName::EKind;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUALDelegateNodeNameTest,
    "UnrealAgentLink.Blueprint.DelegateNodeName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUALDelegateNodeNameTest::RunTest(const FString& Parameters)
{
    FString Name;

    // class 直接点名
    TestTrue(TEXT("CallDispatcher"), UAL_DelegateNodeName::Parse(TEXT("calldispatcher"), TEXT("OnChestOpened"), Name) == EKind::Call);
    TestEqual(TEXT("CallDispatcher keeps the name"), Name, FString(TEXT("OnChestOpened")));
    TestTrue(TEXT("BindEvent"), UAL_DelegateNodeName::Parse(TEXT("bindevent"), TEXT("OnChestOpened"), Name) == EKind::Add);
    TestTrue(TEXT("UnbindEvent"), UAL_DelegateNodeName::Parse(TEXT("unbindevent"), TEXT("OnChestOpened"), Name) == EKind::Remove);
    TestTrue(TEXT("UnbindAllEvents"), UAL_DelegateNodeName::Parse(TEXT("unbindallevents"), TEXT("OnChestOpened"), Name) == EKind::Clear);

    // 引擎类名也认
    TestTrue(TEXT("AddDelegate"), UAL_DelegateNodeName::Parse(TEXT("adddelegate"), TEXT("OnChestOpened"), Name) == EKind::Add);

    // 编辑器标题：这正是此前六种写法全落空的那条路
    TestTrue(TEXT("Call <Name>"), UAL_DelegateNodeName::Parse(TEXT("function"), TEXT("Call OnChestOpened"), Name) == EKind::Call);
    TestEqual(TEXT("Call strips its prefix"), Name, FString(TEXT("OnChestOpened")));

    TestTrue(TEXT("Bind Event to <Name>"), UAL_DelegateNodeName::Parse(TEXT("function"), TEXT("Bind Event to OnChestOpened"), Name) == EKind::Add);
    TestEqual(TEXT("Bind strips its prefix"), Name, FString(TEXT("OnChestOpened")));

    TestTrue(TEXT("Unbind Event from <Name>"), UAL_DelegateNodeName::Parse(TEXT("callfunction"), TEXT("Unbind Event from OnChestOpened"), Name) == EKind::Remove);
    TestEqual(TEXT("Unbind strips its prefix"), Name, FString(TEXT("OnChestOpened")));

    // 「Unbind all」要先于「Unbind」命中，否则解除所有会被当成解除一个 ——
    // 两者建出来的是不同的节点类，错了不会报错，只会少解绑
    TestTrue(TEXT("Unbind all Events from <Name>"), UAL_DelegateNodeName::Parse(TEXT("function"), TEXT("Unbind all Events from OnChestOpened"), Name) == EKind::Clear);
    TestEqual(TEXT("Unbind all strips its prefix"), Name, FString(TEXT("OnChestOpened")));

    // 屏幕上抄下来的标题带空格（属性名被 NameToDisplayString 拆开了）
    TestTrue(TEXT("Display title"), UAL_DelegateNodeName::Parse(TEXT("function"), TEXT("Call On Chest Opened"), Name) == EKind::Call);
    TestEqual(TEXT("Display title keeps the spaces for Compact() to handle"), Name, FString(TEXT("On Chest Opened")));
    TestEqual(TEXT("Compact matches the property name"),
        UAL_DelegateNodeName::Compact(TEXT("On Chest Opened")),
        UAL_DelegateNodeName::Compact(TEXT("OnChestOpened")));

    // 别人身上的分发器
    TestTrue(TEXT("Qualified name"), UAL_DelegateNodeName::Parse(TEXT("function"), TEXT("Bind Event to BP_Chest.OnChestOpened"), Name) == EKind::Add);
    TestEqual(TEXT("Qualified name survives"), Name, FString(TEXT("BP_Chest.OnChestOpened")));

    // 普通函数不许被劫走。函数名里没有空格，所以「Call」后面那个空格就是分界
    TestTrue(TEXT("Plain function"), UAL_DelegateNodeName::Parse(TEXT("function"), TEXT("KismetSystemLibrary.PrintString"), Name) == EKind::None);
    TestTrue(TEXT("Function whose name starts with Call"), UAL_DelegateNodeName::Parse(TEXT("function"), TEXT("MyLib.CallSomething"), Name) == EKind::None);
    TestTrue(TEXT("Non-function class"), UAL_DelegateNodeName::Parse(TEXT("customevent"), TEXT("Call OnChestOpened"), Name) == EKind::None);

    return true;
}
#endif
