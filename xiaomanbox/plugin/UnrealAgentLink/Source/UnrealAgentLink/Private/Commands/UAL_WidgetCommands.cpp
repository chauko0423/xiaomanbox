// Copyright Epic Games, Inc. All Rights Reserved.
// UnrealAgentLink - Widget Commands

#include "UAL_WidgetCommands.h"
#include "UAL_CommandUtils.h"

// UMG & Widget Blueprint
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "UAL_ScopedTransaction.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "Components/Border.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/ProgressBar.h"
#include "Components/Slider.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/SpinBox.h"
#include "Components/RichTextBlock.h"
// 字号和对齐（FontSize / Justification）。UTextLayoutWidget 是 TextBlock、
// RichTextBlock、EditableText(Box) 的共同基类，Justification 声明在它身上
#include "Components/TextWidgetTypes.h"
#include "Components/GridPanel.h"
#include "Components/GridSlot.h"
#include "Components/WrapBox.h"
#include "Components/UniformGridPanel.h"

// Editor utilities
#include "Editor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

// Rendering (for Preview)
#include "Slate/WidgetRenderer.h"
#include "RenderingThread.h"
#include "UObject/Package.h"
#include "Engine/TextureRenderTarget2D.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"

// 仅编辑器模式 (UMGEditor 模块)
#if WITH_EDITOR
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "FileHelpers.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUALWidget, Log, All);

// ============================================================================
// 辅助：生成唯一 Widget 名称（防止重名崩溃）
// ============================================================================

static FName MakeUniqueWidgetName(UWidgetTree* WidgetTree, const FString& DesiredName)
{
	if (DesiredName.IsEmpty())
	{
		return NAME_None; // 让 ConstructWidget 自动生成
	}

	FName TestName(*DesiredName);
	if (!WidgetTree->FindWidget(TestName))
	{
		return TestName;
	}

	// 名称冲突，追加后缀
	for (int32 i = 1; i < 1000; ++i)
	{
		FString SuffixedName = FString::Printf(TEXT("%s_%d"), *DesiredName, i);
		TestName = FName(*SuffixedName);
		if (!WidgetTree->FindWidget(TestName))
		{
			UE_LOG(LogUALWidget, Warning, TEXT("Widget name '%s' already exists, renamed to '%s'"),
				*DesiredName, *SuffixedName);
			return TestName;
		}
	}

	// 极端情况，回退到自动命名
	UE_LOG(LogUALWidget, Error, TEXT("Failed to generate unique name for '%s' after 999 attempts"), *DesiredName);
	return NAME_None;
}

// ============================================================================
// 命令注册
// ============================================================================

void FUAL_WidgetCommands::RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	// Phase 1: 只读能力
	CommandMap.Add(TEXT("widget.get_hierarchy"), &Handle_GetHierarchy);
	
	// Phase 2: 创建 + CanvasPanel 布局
	CommandMap.Add(TEXT("widget.create"), &Handle_Create);
	CommandMap.Add(TEXT("widget.set_canvas_slot"), &Handle_SetCanvasSlot);
	
	// Phase 3: 通用子控件添加。
	// add_control / add_to_vertical / add_to_horizontal 已删（2026-09-16）——
	// add_child 认所有容器（Canvas/VBox/HBox/Overlay…）且收同一套 slot 参数，
	// 那三条是它出现之前的分容器版本，盒子这边一条都没调过
	CommandMap.Add(TEXT("widget.add_child"), &Handle_AddChild);
	CommandMap.Add(TEXT("widget.set_vertical_slot"), &Handle_SetVerticalSlot);
	
	// Phase 4: 预览渲染
	CommandMap.Add(TEXT("widget.preview"), &Handle_Preview);
	
	// Phase 5: 事件绑定
	CommandMap.Add(TEXT("widget.make_variable"), &Handle_MakeVariable);
	CommandMap.Add(TEXT("widget.set_property"), &Handle_SetProperty);
	
	UE_LOG(LogUALWidget, Log, TEXT("FUAL_WidgetCommands: Registered %d widget commands"), 8);
}

// ============================================================================
// Phase 1: 只读能力 - widget.get_hierarchy
// ============================================================================

void FUAL_WidgetCommands::Handle_GetHierarchy(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}

	FString Error;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, Error);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 404, Error);
		return;
	}

	UWidgetTree* WidgetTree = WidgetBP->WidgetTree;
	if (!WidgetTree)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("WidgetBlueprint has no WidgetTree"));
		return;
	}

	UWidget* RootWidget = WidgetTree->RootWidget;
	if (!RootWidget)
	{
		// 空 Widget，返回空结构
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), Path);
		Result->SetField(TEXT("root"), MakeShared<FJsonValueNull>());
		Result->SetNumberField(TEXT("widget_count"), 0);
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	// 递归构建 Widget 层级
	TSharedPtr<FJsonObject> RootJson = BuildWidgetJson(RootWidget);
	
	// 统计控件数量
	int32 WidgetCount = 0;
	WidgetTree->ForEachWidget([&WidgetCount](UWidget* Widget)
	{
		WidgetCount++;
	});

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), Path);
	Result->SetStringField(TEXT("name"), WidgetBP->GetName());
	Result->SetObjectField(TEXT("root"), RootJson);
	Result->SetNumberField(TEXT("widget_count"), WidgetCount);
	
	UE_LOG(LogUALWidget, Log, TEXT("widget.get_hierarchy: path=%s, widget_count=%d"), *Path, WidgetCount);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.get_hierarchy is only available in editor mode"));
#endif
}

// ============================================================================
// Phase 2: 创建 + CanvasPanel 布局
// ============================================================================

void FUAL_WidgetCommands::Handle_Create(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	// 解析参数
	FString Name;
	if (!Payload->TryGetStringField(TEXT("name"), Name))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: name"));
		return;
	}
	
	FString Folder = TEXT("/Game/UI");
	Payload->TryGetStringField(TEXT("folder"), Folder);
	
	FString RootType = TEXT("CanvasPanel");
	Payload->TryGetStringField(TEXT("root_type"), RootType);
	
	// 确保文件夹路径格式正确
	if (!Folder.StartsWith(TEXT("/")))
	{
		Folder = TEXT("/") + Folder;
	}
	
	// 检查资产是否已存在（防止重名崩溃）
	FString AssetPath = Folder / Name;
	if (StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath))
	{
		UAL_CommandUtils::SendError(RequestId, 409,
			FString::Printf(TEXT("Widget Blueprint already exists: %s"), *AssetPath));
		return;
	}
	
	/**
	 * 根控件类型在建资产**之前**就认好，认不出来直接 400。
	 *
	 * 原来认不出来就悄悄换成 CanvasPanel，回执里却照抄请求的 root_type ——
	 * 调用方以为根是 VerticalBox，接着按竖排去加子控件，全叠在左上角。
	 * FindWidgetClass 除了常用表还会按 U 前缀动态找任意 UWidget 子类，
	 * 这样都找不到就是真写错了，退回默认只会把错藏起来。
	 * 放在 CreateAsset 前面，是为了拒绝时不留下一个半成品资产。
	 */
	UClass* RootClass = FindWidgetClass(RootType);
	if (!RootClass)
	{
		UAL_CommandUtils::SendError(RequestId, 400,
			FString::Printf(TEXT("Unknown root_type '%s'. Use a UWidget class name without the U prefix, e.g. CanvasPanel / VerticalBox / HorizontalBox / Overlay / ScrollBox / GridPanel / UniformGridPanel / WrapBox / SizeBox / Border."),
				*RootType));
		return;
	}

	// 事务包裹
	const FUAL_ScopedTransaction Transaction(NSLOCTEXT("UAL", "CreateWidget", "Agent Create Widget"));
	
	// 创建 Widget Blueprint
	UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
	Factory->ParentClass = UUserWidget::StaticClass();
	
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	UObject* NewAsset = AssetToolsModule.Get().CreateAsset(Name, Folder, UWidgetBlueprint::StaticClass(), Factory);
	
	if (!NewAsset)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create Widget Blueprint asset"));
		return;
	}
	
	UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(NewAsset);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Created asset is not a Widget Blueprint"));
		return;
	}
	
	// 标记蓝图被修改
	WidgetBP->Modify();
	
	// 设置根控件
	UWidgetTree* WidgetTree = WidgetBP->WidgetTree;
	if (WidgetTree)
	{
		// 创建根控件（RootClass 在建资产前已经校验过）
		UWidget* RootWidget = WidgetTree->ConstructWidget<UWidget>(RootClass);
		if (RootWidget)
		{
			RootWidget->SetDesignerFlags(EWidgetDesignFlags::Designing);
			WidgetTree->RootWidget = RootWidget;
		}
	}
	
	// 标记结构修改并保存
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	
	// 保存资产
	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(WidgetBP->GetOutermost());
	// 返回值原来被丢掉，回执里也就没有 saved —— 只读目录、源码管理没签出时
	// 资产只在内存里，关编辑器就没了，调用方却以为已经落盘
	const bool bSaved = FEditorFileUtils::PromptForCheckoutAndSave(PackagesToSave, false, false)
		== FEditorFileUtils::PR_Success;

	// 构建响应
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("name"), WidgetBP->GetName());
	Result->SetStringField(TEXT("path"), WidgetBP->GetPathName());
	// root_type 从树上读回来，不回显请求：报的是引擎里此刻真正的根
	UWidget* ActualRoot = WidgetTree ? WidgetTree->RootWidget : nullptr;
	const FString ActualRootType = ActualRoot ? ActualRoot->GetClass()->GetName() : FString();
	Result->SetStringField(TEXT("root_type"), ActualRootType);
	Result->SetStringField(TEXT("requested_root_type"), RootType);
	if (!ActualRoot)
	{
		Result->SetStringField(TEXT("warning"),
			TEXT("The Widget Blueprint was created but has no root widget - the root could not be constructed."));
	}
	else if (ActualRoot->GetClass() != RootClass)
	{
		// 按类比、不按名字比：别名（Text → TextBlock）类名不同但就是请求的那个，不该报警
		Result->SetStringField(TEXT("warning"),
			FString::Printf(TEXT("Requested root_type '%s' (%s) but the root widget is '%s'."),
				*RootType, *RootClass->GetName(), *ActualRootType));
	}
	Result->SetBoolField(TEXT("saved"), bSaved);
	
	UE_LOG(LogUALWidget, Log, TEXT("widget.create: name=%s, path=%s"), *Name, *WidgetBP->GetPathName());
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.create is only available in editor mode"));
#endif
}

void FUAL_WidgetCommands::Handle_SetCanvasSlot(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	// 解析参数
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	
	FString WidgetName;
	if (!Payload->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: widget_name"));
		return;
	}
	
	// 加载 WidgetBlueprint
	FString Error;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, Error);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 404, Error);
		return;
	}
	
	// 查找控件
	UWidget* Widget = FindWidgetByName(WidgetBP->WidgetTree, WidgetName);
	if (!Widget)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Widget not found: %s"), *WidgetName));
		return;
	}
	
	// 获取 CanvasPanelSlot
	UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Widget->Slot);
	if (!Slot)
	{
		UAL_CommandUtils::SendError(RequestId, 400, 
			FString::Printf(TEXT("Widget '%s' is not in a CanvasPanel"), *WidgetName));
		return;
	}
	
	// 事务包裹
	const FUAL_ScopedTransaction Transaction(NSLOCTEXT("UAL", "SetCanvasSlot", "Agent Set Canvas Slot"));
	WidgetBP->Modify();
	
	// 更新 Slot 属性
	FString AnchorsStr;
	if (Payload->TryGetStringField(TEXT("anchors"), AnchorsStr))
	{
		Slot->SetAnchors(ParseAnchors(AnchorsStr));
	}
	
	const TSharedPtr<FJsonObject>* PosObj;
	if (Payload->TryGetObjectField(TEXT("position"), PosObj))
	{
		Slot->SetPosition(ParseVector2D(*PosObj));
	}
	
	const TSharedPtr<FJsonObject>* SizeObj;
	if (Payload->TryGetObjectField(TEXT("size"), SizeObj))
	{
		Slot->SetSize(ParseVector2D(*SizeObj));
	}
	
	const TSharedPtr<FJsonObject>* AlignObj;
	if (Payload->TryGetObjectField(TEXT("alignment"), AlignObj))
	{
		Slot->SetAlignment(ParseVector2D(*AlignObj));
	}
	
	int32 ZOrder;
	if (Payload->TryGetNumberField(TEXT("z_order"), ZOrder))
	{
		Slot->SetZOrder(ZOrder);
	}
	
	// 标记修改
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	
	// 构建响应
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("widget_name"), WidgetName);
	Result->SetObjectField(TEXT("slot_data"), BuildCanvasSlotJson(Slot));
	
	UE_LOG(LogUALWidget, Log, TEXT("widget.set_canvas_slot: widget=%s"), *WidgetName);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.set_canvas_slot is only available in editor mode"));
#endif
}

// ============================================================================
// Phase 3: 其他布局容器
// ============================================================================

void FUAL_WidgetCommands::Handle_SetVerticalSlot(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	
	FString WidgetName;
	if (!Payload->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: widget_name"));
		return;
	}
	
	FString Error;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, Error);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 404, Error);
		return;
	}
	
	UWidget* Widget = FindWidgetByName(WidgetBP->WidgetTree, WidgetName);
	if (!Widget)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Widget not found: %s"), *WidgetName));
		return;
	}
	
	UVerticalBoxSlot* Slot = Cast<UVerticalBoxSlot>(Widget->Slot);
	if (!Slot)
	{
		UAL_CommandUtils::SendError(RequestId, 400, 
			FString::Printf(TEXT("Widget '%s' is not in a VerticalBox"), *WidgetName));
		return;
	}
	
	const FUAL_ScopedTransaction Transaction(NSLOCTEXT("UAL", "SetVerticalSlot", "Agent Set Vertical Slot"));
	WidgetBP->Modify();
	
	FString SizeRule;
	if (Payload->TryGetStringField(TEXT("size_rule"), SizeRule))
	{
		if (SizeRule.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))
		{
			Slot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
		else
		{
			Slot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
		}
	}
	
	const TSharedPtr<FJsonObject>* PaddingObj;
	if (Payload->TryGetObjectField(TEXT("padding"), PaddingObj))
	{
		double Left = 0, Top = 0, Right = 0, Bottom = 0;
		(*PaddingObj)->TryGetNumberField(TEXT("left"), Left);
		(*PaddingObj)->TryGetNumberField(TEXT("top"), Top);
		(*PaddingObj)->TryGetNumberField(TEXT("right"), Right);
		(*PaddingObj)->TryGetNumberField(TEXT("bottom"), Bottom);
		Slot->SetPadding(FMargin(Left, Top, Right, Bottom));
	}
	
	FString HAlign;
	if (Payload->TryGetStringField(TEXT("h_align"), HAlign))
	{
		if (HAlign.Contains(TEXT("Left"))) Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Left);
		else if (HAlign.Contains(TEXT("Center"))) Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
		else if (HAlign.Contains(TEXT("Right"))) Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Right);
		else if (HAlign.Contains(TEXT("Fill"))) Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Fill);
	}
	
	FString VAlign;
	if (Payload->TryGetStringField(TEXT("v_align"), VAlign))
	{
		if (VAlign.Contains(TEXT("Top"))) Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Top);
		else if (VAlign.Contains(TEXT("Center"))) Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Center);
		else if (VAlign.Contains(TEXT("Bottom"))) Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Bottom);
		else if (VAlign.Contains(TEXT("Fill"))) Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Fill);
	}
	
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("widget_name"), WidgetName);
	Result->SetObjectField(TEXT("slot_data"), BuildVerticalSlotJson(Slot));
	
	UE_LOG(LogUALWidget, Log, TEXT("widget.set_vertical_slot: widget=%s"), *WidgetName);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.set_vertical_slot is only available in editor mode"));
#endif
}

// ============================================================================
// Phase 3: 通用子控件添加 - widget.add_child
// ============================================================================

void FUAL_WidgetCommands::Handle_AddChild(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	
	FString ParentName;
	if (!Payload->TryGetStringField(TEXT("parent_name"), ParentName))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: parent_name"));
		return;
	}
	
	FString ControlType;
	if (!Payload->TryGetStringField(TEXT("control_type"), ControlType))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: control_type"));
		return;
	}
	
	FString WidgetName;
	Payload->TryGetStringField(TEXT("name"), WidgetName);
	
	FString Error;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, Error);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 404, Error);
		return;
	}
	
	UWidgetTree* WidgetTree = WidgetBP->WidgetTree;
	if (!WidgetTree)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("WidgetBlueprint has no WidgetTree"));
		return;
	}
	
	// 查找父容器
	UWidget* Parent = nullptr;
	FString ParentType;
	
	if (ParentName.Equals(TEXT("root"), ESearchCase::IgnoreCase))
	{
		Parent = WidgetTree->RootWidget;
		if (!Parent)
		{
			UAL_CommandUtils::SendError(RequestId, 404, TEXT("Widget has no root"));
			return;
		}
		ParentType = Parent->GetClass()->GetName();
	}
	else
	{
		Parent = FindWidgetByName(WidgetTree, ParentName);
		if (!Parent)
		{
			UAL_CommandUtils::SendError(RequestId, 404, 
				FString::Printf(TEXT("Parent not found: %s"), *ParentName));
			return;
		}
		ParentType = Parent->GetClass()->GetName();
	}
	
	// 创建新控件
	UClass* WidgetClass = FindWidgetClass(ControlType);
	if (!WidgetClass)
	{
		UAL_CommandUtils::SendError(RequestId, 400, 
			FString::Printf(TEXT("Unknown control type: %s"), *ControlType));
		return;
	}
	
	// 生成唯一名称（防止重名崩溃）
	FName UniqueName = NAME_None;
	if (!WidgetName.IsEmpty())
	{
		UniqueName = MakeUniqueWidgetName(WidgetTree, WidgetName);
	}
	
	UWidget* NewWidget = WidgetTree->ConstructWidget<UWidget>(WidgetClass, UniqueName);
	if (!NewWidget)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to construct widget"));
		return;
	}
	
	NewWidget->SetDesignerFlags(EWidgetDesignFlags::Designing);
	
	// 根据父容器类型选择添加方式
	FString SlotType;
	TSharedPtr<FJsonObject> SlotData = MakeShared<FJsonObject>();
	// 这个父容器的槽位真正认了哪些参数。没进这张表、调用方又传了的，
	// 最后进 ignored_fields —— 原来一律 ok:true，给 VBox 传 anchors 就静默丢掉
	TSet<FString> AppliedFields;
	auto MarkApplied = [&AppliedFields](std::initializer_list<const TCHAR*> Fields)
	{
		for (const TCHAR* Field : Fields)
		{
			AppliedFields.Add(Field);
		}
	};
	// 单内容容器（Button/Border/SizeBox）原有的子控件被顶掉时记下来
	TSharedPtr<FJsonObject> ReplacedChild;
	
	// CanvasPanel
	if (UCanvasPanel* Canvas = Cast<UCanvasPanel>(Parent))
	{
		UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Canvas->AddChild(NewWidget));
		if (!Slot)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to add widget to CanvasPanel"));
			return;
		}
		
		// 设置 Canvas Slot 属性
		FString Anchors;
		if (Payload->TryGetStringField(TEXT("anchors"), Anchors))
		{
			Slot->SetAnchors(ParseAnchors(Anchors));
		}
		
		const TSharedPtr<FJsonObject>* PosObj;
		if (Payload->TryGetObjectField(TEXT("position"), PosObj))
		{
			double X = 0, Y = 0;
			(*PosObj)->TryGetNumberField(TEXT("x"), X);
			(*PosObj)->TryGetNumberField(TEXT("y"), Y);
			Slot->SetPosition(FVector2D(X, Y));
		}
		
		const TSharedPtr<FJsonObject>* SizeObj;
		if (Payload->TryGetObjectField(TEXT("size"), SizeObj))
		{
			double W = 100, H = 40;
			(*SizeObj)->TryGetNumberField(TEXT("width"), W);
			(*SizeObj)->TryGetNumberField(TEXT("height"), H);
			Slot->SetSize(FVector2D(W, H));
		}
		
		MarkApplied({ TEXT("anchors"), TEXT("position"), TEXT("size") });
		SlotType = TEXT("CanvasPanelSlot");
		SlotData = BuildCanvasSlotJson(Slot);
	}
	// VerticalBox
	else if (UVerticalBox* VBox = Cast<UVerticalBox>(Parent))
	{
		UVerticalBoxSlot* Slot = Cast<UVerticalBoxSlot>(VBox->AddChild(NewWidget));
		if (!Slot)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to add widget to VerticalBox"));
			return;
		}
		
		// 设置 Slot 属性
		FString SizeRule;
		if (Payload->TryGetStringField(TEXT("size_rule"), SizeRule))
		{
			if (SizeRule.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))
			{
				Slot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			}
			else
			{
				Slot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
			}
		}
		
		// 设置对齐
		FString HAlign;
		if (Payload->TryGetStringField(TEXT("h_align"), HAlign))
		{
			if (HAlign.Equals(TEXT("Left"), ESearchCase::IgnoreCase))
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Left);
			else if (HAlign.Equals(TEXT("Center"), ESearchCase::IgnoreCase))
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
			else if (HAlign.Equals(TEXT("Right"), ESearchCase::IgnoreCase))
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Right);
			else
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Fill);
		}
		
		FString VAlign;
		if (Payload->TryGetStringField(TEXT("v_align"), VAlign))
		{
			if (VAlign.Equals(TEXT("Top"), ESearchCase::IgnoreCase))
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Top);
			else if (VAlign.Equals(TEXT("Center"), ESearchCase::IgnoreCase))
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Center);
			else if (VAlign.Equals(TEXT("Bottom"), ESearchCase::IgnoreCase))
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Bottom);
			else
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Fill);
		}
		
		// 设置内边距
		const TSharedPtr<FJsonObject>* PaddingObj;
		if (Payload->TryGetObjectField(TEXT("padding"), PaddingObj))
		{
			double Left = 0, Top = 0, Right = 0, Bottom = 0;
			(*PaddingObj)->TryGetNumberField(TEXT("left"), Left);
			(*PaddingObj)->TryGetNumberField(TEXT("top"), Top);
			(*PaddingObj)->TryGetNumberField(TEXT("right"), Right);
			(*PaddingObj)->TryGetNumberField(TEXT("bottom"), Bottom);
			Slot->SetPadding(FMargin(Left, Top, Right, Bottom));
		}
		
		MarkApplied({ TEXT("size_rule"), TEXT("h_align"), TEXT("v_align"), TEXT("padding") });
		SlotType = TEXT("VerticalBoxSlot");
		SlotData = BuildVerticalSlotJson(Slot);
	}
	// HorizontalBox
	else if (UHorizontalBox* HBox = Cast<UHorizontalBox>(Parent))
	{
		UHorizontalBoxSlot* Slot = Cast<UHorizontalBoxSlot>(HBox->AddChild(NewWidget));
		if (!Slot)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to add widget to HorizontalBox"));
			return;
		}
		
		FString SizeRule;
		if (Payload->TryGetStringField(TEXT("size_rule"), SizeRule))
		{
			if (SizeRule.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))
			{
				Slot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			}
			else
			{
				Slot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
			}
		}
		
		const TSharedPtr<FJsonObject>* PaddingObj;
		if (Payload->TryGetObjectField(TEXT("padding"), PaddingObj))
		{
			double Left = 0, Top = 0, Right = 0, Bottom = 0;
			(*PaddingObj)->TryGetNumberField(TEXT("left"), Left);
			(*PaddingObj)->TryGetNumberField(TEXT("top"), Top);
			(*PaddingObj)->TryGetNumberField(TEXT("right"), Right);
			(*PaddingObj)->TryGetNumberField(TEXT("bottom"), Bottom);
			Slot->SetPadding(FMargin(Left, Top, Right, Bottom));
		}
		
		// 对齐原来只在 VBox / Overlay 分支里设，HBox 收了 h_align/v_align 却不管 ——
		// 工具描述写着「VerticalBox/HorizontalBox 用 alignment」，传了就该生效
		FString HAlign;
		if (Payload->TryGetStringField(TEXT("h_align"), HAlign))
		{
			if (HAlign.Equals(TEXT("Left"), ESearchCase::IgnoreCase))
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Left);
			else if (HAlign.Equals(TEXT("Center"), ESearchCase::IgnoreCase))
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
			else if (HAlign.Equals(TEXT("Right"), ESearchCase::IgnoreCase))
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Right);
			else
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Fill);
		}

		FString VAlign;
		if (Payload->TryGetStringField(TEXT("v_align"), VAlign))
		{
			if (VAlign.Equals(TEXT("Top"), ESearchCase::IgnoreCase))
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Top);
			else if (VAlign.Equals(TEXT("Center"), ESearchCase::IgnoreCase))
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Center);
			else if (VAlign.Equals(TEXT("Bottom"), ESearchCase::IgnoreCase))
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Bottom);
			else
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Fill);
		}

		MarkApplied({ TEXT("size_rule"), TEXT("h_align"), TEXT("v_align"), TEXT("padding") });
		SlotType = TEXT("HorizontalBoxSlot");
		SlotData = BuildHorizontalSlotJson(Slot);
	}
	// Overlay
	else if (UOverlay* Overlay = Cast<UOverlay>(Parent))
	{
		UOverlaySlot* Slot = Cast<UOverlaySlot>(Overlay->AddChild(NewWidget));
		if (!Slot)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to add widget to Overlay"));
			return;
		}
		
		// 设置对齐
		FString HAlign;
		if (Payload->TryGetStringField(TEXT("h_align"), HAlign))
		{
			if (HAlign.Equals(TEXT("Left"), ESearchCase::IgnoreCase))
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Left);
			else if (HAlign.Equals(TEXT("Center"), ESearchCase::IgnoreCase))
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
			else if (HAlign.Equals(TEXT("Right"), ESearchCase::IgnoreCase))
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Right);
			else
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Fill);
		}
		
		FString VAlign;
		if (Payload->TryGetStringField(TEXT("v_align"), VAlign))
		{
			if (VAlign.Equals(TEXT("Top"), ESearchCase::IgnoreCase))
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Top);
			else if (VAlign.Equals(TEXT("Center"), ESearchCase::IgnoreCase))
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Center);
			else if (VAlign.Equals(TEXT("Bottom"), ESearchCase::IgnoreCase))
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Bottom);
			else
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Fill);
		}
		
		const TSharedPtr<FJsonObject>* PaddingObj;
		if (Payload->TryGetObjectField(TEXT("padding"), PaddingObj))
		{
			double Left = 0, Top = 0, Right = 0, Bottom = 0;
			(*PaddingObj)->TryGetNumberField(TEXT("left"), Left);
			(*PaddingObj)->TryGetNumberField(TEXT("top"), Top);
			(*PaddingObj)->TryGetNumberField(TEXT("right"), Right);
			(*PaddingObj)->TryGetNumberField(TEXT("bottom"), Bottom);
			Slot->SetPadding(FMargin(Left, Top, Right, Bottom));
		}
		
		MarkApplied({ TEXT("h_align"), TEXT("v_align"), TEXT("padding") });
		SlotType = TEXT("OverlaySlot");
	}
	// Button/Border/SizeBox 等单内容容器 (ContentWidget)
	else if (UContentWidget* ContentWidget = Cast<UContentWidget>(Parent))
	{
		// 单内容容器只能放一个子控件，已有内容就顶掉。顶掉照做（按钮换文字是常见操作），
		// 但必须进回执：原来静默清掉，调用方以为按钮里的图标还在
		if (ContentWidget->GetChildrenCount() > 0)
		{
			if (UWidget* OldContent = ContentWidget->GetContent())
			{
				ReplacedChild = MakeShared<FJsonObject>();
				ReplacedChild->SetStringField(TEXT("name"), OldContent->GetName());
				ReplacedChild->SetStringField(TEXT("class"), OldContent->GetClass()->GetName());
			}
			ContentWidget->ClearChildren();
		}
		
		UPanelSlot* Slot = ContentWidget->AddChild(NewWidget);
		if (!Slot)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to set content widget"));
			return;
		}
		
		SlotType = TEXT("ContentSlot");
	}
	else
	{
		// 尝试作为通用 PanelWidget 添加
		UPanelWidget* Panel = Cast<UPanelWidget>(Parent);
		if (!Panel)
		{
			UAL_CommandUtils::SendError(RequestId, 400, 
				FString::Printf(TEXT("Parent '%s' (%s) is not a container"), *ParentName, *ParentType));
			return;
		}
		
		UPanelSlot* Slot = Panel->AddChild(NewWidget);
		if (!Slot)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to add widget to panel"));
			return;
		}
		
		SlotType = TEXT("PanelSlot");
	}
	
	// 如果是 TextBlock，设置文本
	FString Text;
	UTextBlock* NewTextBlock = Cast<UTextBlock>(NewWidget);
	if (Payload->TryGetStringField(TEXT("text"), Text))
	{
		if (NewTextBlock)
		{
			NewTextBlock->SetText(FText::FromString(Text));
			AppliedFields.Add(TEXT("text"));
		}
	}

	// 调用方传了、这个父容器 / 控件类型却不认的参数
	TArray<TSharedPtr<FJsonValue>> IgnoredFields;
	TArray<FString> IgnoredSlotFields;
	static const TCHAR* OptionalSlotFields[] = {
		TEXT("anchors"), TEXT("position"), TEXT("size"), TEXT("size_rule"),
		TEXT("h_align"), TEXT("v_align"), TEXT("padding")
	};
	for (const TCHAR* Field : OptionalSlotFields)
	{
		if (Payload->HasField(Field) && !AppliedFields.Contains(Field))
		{
			IgnoredFields.Add(MakeShared<FJsonValueString>(Field));
			IgnoredSlotFields.Add(Field);
		}
	}
	const bool bTextIgnored = Payload->HasField(TEXT("text")) && !AppliedFields.Contains(TEXT("text"));
	if (bTextIgnored)
	{
		IgnoredFields.Add(MakeShared<FJsonValueString>(TEXT("text")));
	}
	
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("name"), NewWidget->GetName());
	Result->SetStringField(TEXT("class"), NewWidget->GetClass()->GetName());
	Result->SetStringField(TEXT("parent"), Parent->GetName());
	Result->SetStringField(TEXT("parent_type"), ParentType);
	Result->SetStringField(TEXT("slot_type"), SlotType);
	if (SlotData.IsValid() && SlotData->Values.Num() > 0)
	{
		Result->SetObjectField(TEXT("slot_data"), SlotData);
	}
	// 文本从控件上读回来，不回显请求
	if (NewTextBlock)
	{
		Result->SetStringField(TEXT("text"), NewTextBlock->GetText().ToString());
	}
	if (IgnoredFields.Num() > 0)
	{
		Result->SetArrayField(TEXT("ignored_fields"), IgnoredFields);
		// AGENTS.md §5 第 14 条：有没办成的，顶层带 failed_count
		Result->SetNumberField(TEXT("failed_count"), IgnoredFields.Num());
		TArray<FString> Notes;
		if (IgnoredSlotFields.Num() > 0)
		{
			Notes.Add(FString::Printf(TEXT("parent '%s' (%s) gives a %s, which does not take %s"),
				*Parent->GetName(), *ParentType, *SlotType, *FString::Join(IgnoredSlotFields, TEXT(", "))));
		}
		if (bTextIgnored)
		{
			Notes.Add(FString::Printf(TEXT("'text' only applies to TextBlock, this is a %s - add a TextBlock child to it"),
				*NewWidget->GetClass()->GetName()));
		}
		Result->SetStringField(TEXT("ignored_note"),
			FString::Printf(TEXT("Not applied: %s."), *FString::Join(Notes, TEXT("; "))));
	}
	if (ReplacedChild.IsValid())
	{
		Result->SetObjectField(TEXT("replaced_child"), ReplacedChild);
	}
	
	UE_LOG(LogUALWidget, Log, TEXT("widget.add_child: type=%s, name=%s, parent=%s (%s)"), 
		*ControlType, *NewWidget->GetName(), *Parent->GetName(), *ParentType);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.add_child is only available in editor mode"));
#endif
}

// ============================================================================
// Phase 4: 预览渲染
// ============================================================================

void FUAL_WidgetCommands::Handle_Preview(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	
	int32 Width = 1920;
	int32 Height = 1080;
	Payload->TryGetNumberField(TEXT("width"), Width);
	Payload->TryGetNumberField(TEXT("height"), Height);
	
	FString Error;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, Error);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 404, Error);
		return;
	}
	
	// 编译蓝图确保最新
	FKismetEditorUtilities::CompileBlueprint(WidgetBP);
	
	// 获取生成类
	UClass* WidgetClass = WidgetBP->GeneratedClass;
	if (!WidgetClass || !WidgetClass->IsChildOf(UUserWidget::StaticClass()))
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Widget Blueprint has no valid generated class"));
		return;
	}
	
	// 创建临时 Widget 实例。
	// Outer 必须是 TransientPackage，不能挂在编辑器关卡下：挂在关卡下的实例在 GC 前一直活着，
	// 之后蓝图改了 ubergraph 重编译时会被重实例化，UE 5.7 上会 ensure 帧 key 不匹配后崩溃。
	// 用设计器模式（同引擎自带的 Widget 缩略图渲染），不跑 Construct 里的游戏逻辑。
	UUserWidget* TempWidget = NewObject<UUserWidget>(GetTransientPackage(), WidgetClass, NAME_None, RF_Transient);
	if (!TempWidget)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create widget instance for preview"));
		return;
	}
	TempWidget->SetDesignerFlags(EWidgetDesignFlags::Designing | EWidgetDesignFlags::ExecutePreConstruct);
	TempWidget->Initialize();

	// 强制布局计算
	TempWidget->ForceLayoutPrepass();

	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
	RenderTarget->InitAutoFormat(Width, Height);
	RenderTarget->UpdateResourceImmediate();

	// 使用 FWidgetRenderer 渲染到 RenderTarget；渲染器和 Slate 控件限定在块内，画完即释放
	{
		FWidgetRenderer WidgetRenderer(true);
		TSharedRef<SWidget> SlateWidget = TempWidget->TakeWidget();
		WidgetRenderer.DrawWidget(RenderTarget, SlateWidget, FVector2D(Width, Height), 0.0f);
		FlushRenderingCommands();
	}

	// 保存到文件
	FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots/UAL"));
	IFileManager::Get().MakeDirectory(*OutputDir, true);
	
	FString Filename = FString::Printf(TEXT("widget_preview_%s_%lld.png"), 
		*FPaths::GetBaseFilename(Path), FDateTime::Now().GetTicks());
	FString OutputPath = FPaths::Combine(OutputDir, Filename);
	
	// 读取像素数据
	TArray<FColor> Bitmap;
	FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
	if (RTResource && RTResource->ReadPixels(Bitmap))
	{
		// 保存为 PNG
		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		
		if (ImageWrapper->SetRaw(Bitmap.GetData(), Bitmap.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
		{
			const TArray64<uint8>& PNGData = ImageWrapper->GetCompressed();
			if (PNGData.Num() > 0)
			{
				FFileHelper::SaveArrayToFile(PNGData, *OutputPath);
			}
		}
	}
	
	// 清理临时实例：释放 Slate 资源后交给 GC。
	// 不要手动 ConditionalBeginDestroy——那会把仍可达的对象改名为 None、拆掉 ubergraph 帧，
	// 留到下次编译重实例化时就是野指针。
	TempWidget->RemoveFromParent();
	TempWidget->ReleaseSlateResources(true);
	TempWidget->MarkAsGarbage();
	RenderTarget->ReleaseResource();
	RenderTarget->MarkAsGarbage();
	
	// 返回结果
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("path"), OutputPath);
	Result->SetNumberField(TEXT("width"), Width);
	Result->SetNumberField(TEXT("height"), Height);
	
	UE_LOG(LogUALWidget, Log, TEXT("widget.preview: path=%s, output=%s"), *Path, *OutputPath);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.preview is only available in editor mode"));
#endif
}

// ============================================================================
// Phase 5: 事件绑定
// ============================================================================

void FUAL_WidgetCommands::Handle_MakeVariable(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	
	FString WidgetName;
	if (!Payload->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: widget_name"));
		return;
	}
	
	FString VariableName;
	if (!Payload->TryGetStringField(TEXT("variable_name"), VariableName))
	{
		VariableName = WidgetName; // 默认使用控件名
	}
	
	FString Error;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, Error);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 404, Error);
		return;
	}
	
	UWidget* Widget = FindWidgetByName(WidgetBP->WidgetTree, WidgetName);
	if (!Widget)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Widget not found: %s"), *WidgetName));
		return;
	}
	
	const FUAL_ScopedTransaction Transaction(NSLOCTEXT("UAL", "MakeVariable", "Agent Make Variable"));
	WidgetBP->Modify();
	
	// 设置控件为变量
	Widget->bIsVariable = true;
	
	// 重命名控件（变量名）- 使用安全重命名防止重名崩溃
	if (VariableName != WidgetName)
	{
		FName SafeName = GenerateUniqueWidgetName(WidgetBP->WidgetTree, VariableName);
		Widget->Rename(*SafeName.ToString());
	}
	
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("widget_name"), Widget->GetName());
	Result->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
	
	UE_LOG(LogUALWidget, Log, TEXT("widget.make_variable: widget=%s"), *Widget->GetName());
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.make_variable is only available in editor mode"));
#endif
}

/** JSON 里这个值到底是什么类型 —— 报错要说事实，不能替调用方假设它传错了 */
static FString UAL_JsonTypeName(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid()) return TEXT("缺失");
	switch (Value->Type)
	{
	case EJson::None:    return TEXT("None");
	case EJson::Null:    return TEXT("null");
	case EJson::String:  return TEXT("字符串");
	case EJson::Number:  return TEXT("数字");
	case EJson::Boolean: return TEXT("布尔");
	case EJson::Array:   return TEXT("数组");
	case EJson::Object:  return TEXT("对象");
	default:             return TEXT("未知");
	}
}

/**
 * 读颜色值。**对象和「被序列化成字符串的对象」都收。**
 *
 * 2026-09-17 真机复测：调用方传的是 `{"r":0.5,"g":0.5,"b":0.5,"a":0.5}`，
 * 而这里的 `TryGetObjectField` 拿不到。盒子那侧发出去的确实还是嵌套对象
 * （单测验过），所以形状是在中间某一层变的 —— 最可能是被序列化成了字符串
 * （MCP 入口和某些模型的工具参数编码都会这么干，而盒子的 schema 允许 value
 * 是字符串，于是原样转发）。**这一条尚未证实**，下面那句报错会把实际类型
 * 说出来，下次真机跑一次就有定论。
 *
 * 不管是哪一层干的，收两种形状都是对的：调用方写对了、后端也支持，中间那层的
 * 编码差异不该变成用户面前的失败。插件是所有客户端（盒子 / CLI / MCP）唯一的
 * 汇合点，在这里兼容一次覆盖全部入口，比在每个客户端各补一次可靠。
 *
 * 拿不到就说**实际收到的是什么类型**，不再断言「你没传对象」：
 * 原来那句话在调用方明明传了对象的时候是假的，它会照着错方向改两轮。
 */
static bool UAL_TryReadColorValue(const TSharedPtr<FJsonObject>& Payload, FLinearColor& OutColor, FString& OutError)
{
	const TSharedPtr<FJsonValue> RawValue = Payload->TryGetField(TEXT("value"));

	TSharedPtr<FJsonObject> ColorObj;
	const TSharedPtr<FJsonObject>* ObjectField = nullptr;
	if (RawValue.IsValid() && RawValue->TryGetObject(ObjectField) && ObjectField && ObjectField->IsValid())
	{
		ColorObj = *ObjectField;
	}
	else
	{
		// 字符串形态：可能是 "{\"r\":0.5,...}" 这样被序列化过的对象
		FString AsString;
		if (RawValue.IsValid() && RawValue->TryGetString(AsString))
		{
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(AsString);
			TSharedPtr<FJsonObject> Parsed;
			if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
			{
				ColorObj = Parsed;
			}
		}
	}

	if (!ColorObj.IsValid())
	{
		OutError = FString::Printf(
			TEXT("颜色要一个对象 {\"r\":0.8,\"g\":0.1,\"b\":0.1,\"a\":1}（分量 0..1），")
			TEXT("这次收到的 value 是%s。"),
			*UAL_JsonTypeName(RawValue));
		return false;
	}

	// r/g/b 一个都没有：多半传的是别的结构（比如 {"color":{...}} 或 [r,g,b]）
	if (!ColorObj->HasField(TEXT("r")) && !ColorObj->HasField(TEXT("g")) && !ColorObj->HasField(TEXT("b")))
	{
		OutError = TEXT("颜色对象里没有 r / g / b 字段。形如 {\"r\":0.8,\"g\":0.1,\"b\":0.1,\"a\":1}，分量 0..1。");
		return false;
	}

	OutColor = FLinearColor(
		ColorObj->HasField(TEXT("r")) ? (float)ColorObj->GetNumberField(TEXT("r")) : 0.0f,
		ColorObj->HasField(TEXT("g")) ? (float)ColorObj->GetNumberField(TEXT("g")) : 0.0f,
		ColorObj->HasField(TEXT("b")) ? (float)ColorObj->GetNumberField(TEXT("b")) : 0.0f,
		ColorObj->HasField(TEXT("a")) ? (float)ColorObj->GetNumberField(TEXT("a")) : 1.0f);
	return true;
}

/**
 * 这个控件**实际**能设哪几个属性。
 *
 * 失败时列一张全属性表是自相矛盾的：给 Border 设 Text 报「属性名 'Text' 不认识」，
 * 紧接着的清单里第一个就是 Text —— 调用方会认定清单不可信，然后照着乱试。
 * 真相是「Border 这个类没有 Text」，那就按类说。
 *
 * 分支和下面 `Handle_SetProperty` 的 Cast 一一对应，改那边记得改这里。
 */
static FString UAL_SupportedPropertiesFor(UWidget* Widget)
{
	TArray<FString> Names = { TEXT("Visibility"), TEXT("IsEnabled"), TEXT("ToolTipText") };

	if (Widget)
	{
		if (Widget->IsA<UTextBlock>())
		{
			Names.Append({ TEXT("Text"), TEXT("FontSize"), TEXT("Justification"), TEXT("ColorAndOpacity") });
		}
		else if (Widget->IsA<URichTextBlock>())
		{
			Names.Append({ TEXT("FontSize"), TEXT("Justification") });
		}
		else if (Widget->IsA<UTextLayoutWidget>())
		{
			Names.Add(TEXT("Justification"));
		}
		else if (Widget->IsA<UProgressBar>())
		{
			Names.Append({ TEXT("Percent"), TEXT("FillColorAndOpacity") });
		}
		else if (Widget->IsA<UImage>())
		{
			Names.Add(TEXT("ColorAndOpacity"));
		}
		else if (Widget->IsA<UBorder>())
		{
			Names.Add(TEXT("BrushColor"));
		}
	}

	return FString::Join(Names, TEXT(" / "));
}

void FUAL_WidgetCommands::Handle_SetProperty(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	
	FString WidgetName;
	if (!Payload->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: widget_name"));
		return;
	}
	
	FString PropertyName;
	if (!Payload->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: property_name"));
		return;
	}
	
	FString Error;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, Error);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 404, Error);
		return;
	}
	
	UWidget* Widget = FindWidgetByName(WidgetBP->WidgetTree, WidgetName);
	if (!Widget)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Widget not found: %s"), *WidgetName));
		return;
	}
	
	const FUAL_ScopedTransaction Transaction(NSLOCTEXT("UAL", "SetProperty", "Agent Set Property"));
	WidgetBP->Modify();
	
	bool bSuccess = false;
	FString ResultMessage;
	// 写完读回来的值。有的分支才填，填了就进回执的 value 字段
	TSharedPtr<FJsonValue> ReadBackValue;

	// 处理常见属性（类型安全）
	if (PropertyName.Equals(TEXT("Text"), ESearchCase::IgnoreCase))
	{
		// TextBlock.Text
		FString TextValue;
		if (Payload->TryGetStringField(TEXT("value"), TextValue))
		{
			if (UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
			{
				TextBlock->SetText(FText::FromString(TextValue));
				bSuccess = true;
				ResultMessage = FString::Printf(TEXT("Set Text to: %s"), *TextValue);
			}
		}
	}
	else if (PropertyName.Equals(TEXT("Visibility"), ESearchCase::IgnoreCase))
	{
		/**
		 * 精确匹配枚举名，认不出来就拒。
		 *
		 * 原来是 Contains 链：认不出来的值一律落成 Visible，而
		 * "SelfHitTestInvisible" 先撞上 Contains("HitTestInvisible")，
		 * 设成了 HitTestInvisible —— 子控件跟着点不动了。回执还照抄输入，
		 * 调用方看到的是自己要的那个值，永远发现不了。
		 */
		struct FUAL_VisibilityName
		{
			const TCHAR* Name;
			ESlateVisibility Value;
		};
		static const FUAL_VisibilityName VisibilityNames[] = {
			{ TEXT("Visible"), ESlateVisibility::Visible },
			{ TEXT("Collapsed"), ESlateVisibility::Collapsed },
			{ TEXT("Hidden"), ESlateVisibility::Hidden },
			{ TEXT("HitTestInvisible"), ESlateVisibility::HitTestInvisible },
			{ TEXT("SelfHitTestInvisible"), ESlateVisibility::SelfHitTestInvisible },
		};

		FString VisValue;
		if (Payload->TryGetStringField(TEXT("value"), VisValue))
		{
			VisValue.TrimStartAndEndInline();
			// 兼容带枚举前缀的写法（ESlateVisibility::Hidden）
			VisValue.RemoveFromStart(TEXT("ESlateVisibility::"));

			const FUAL_VisibilityName* Matched = nullptr;
			for (const FUAL_VisibilityName& Entry : VisibilityNames)
			{
				if (VisValue.Equals(Entry.Name, ESearchCase::IgnoreCase))
				{
					Matched = &Entry;
					break;
				}
			}

			if (Matched)
			{
				Widget->SetVisibility(Matched->Value);
				bSuccess = true;
				// 写完从控件上读回来，回执报引擎里的值
				const FString ActualVis = StaticEnum<ESlateVisibility>()
					? StaticEnum<ESlateVisibility>()->GetNameStringByValue((int64)Widget->GetVisibility())
					: FString(Matched->Name);
				ReadBackValue = MakeShared<FJsonValueString>(ActualVis);
				ResultMessage = FString::Printf(TEXT("Visibility is now: %s"), *ActualVis);
			}
			else
			{
				ResultMessage = FString::Printf(
					TEXT("Visibility 只认 Visible / Collapsed / Hidden / HitTestInvisible / SelfHitTestInvisible，收到的是 \"%s\"。"),
					*VisValue);
			}
		}
		else
		{
			ResultMessage = TEXT("Visibility 要传字符串：Visible / Collapsed / Hidden / HitTestInvisible / SelfHitTestInvisible。");
		}
	}
	else if (PropertyName.Equals(TEXT("IsEnabled"), ESearchCase::IgnoreCase))
	{
		bool bEnabled = true;
		if (Payload->TryGetBoolField(TEXT("value"), bEnabled))
		{
			Widget->SetIsEnabled(bEnabled);
			bSuccess = true;
			ResultMessage = FString::Printf(TEXT("Set IsEnabled to: %s"), bEnabled ? TEXT("true") : TEXT("false"));
		}
	}
	else if (PropertyName.Equals(TEXT("ToolTipText"), ESearchCase::IgnoreCase))
	{
		FString TooltipValue;
		if (Payload->TryGetStringField(TEXT("value"), TooltipValue))
		{
			Widget->SetToolTipText(FText::FromString(TooltipValue));
			bSuccess = true;
			ResultMessage = FString::Printf(TEXT("Set ToolTipText to: %s"), *TooltipValue);
		}
	}
	// 颜色。做 UI 绕不开的一项 —— 「做一个红色血条」原来直接办不到，
	// 因为这里只有 Text/Visibility/IsEnabled/ToolTipText/Percent 五个分支。
	// value 形如 { "r":0.8, "g":0.1, "b":0.1, "a":1 }（0..1）。
	else if (PropertyName.Equals(TEXT("FillColorAndOpacity"), ESearchCase::IgnoreCase)
		|| PropertyName.Equals(TEXT("ColorAndOpacity"), ESearchCase::IgnoreCase)
		|| PropertyName.Equals(TEXT("BrushColor"), ESearchCase::IgnoreCase))
	{
		FLinearColor NewColor = FLinearColor::White;
		FString ColorError;
		if (UAL_TryReadColorValue(Payload, NewColor, ColorError))
		{
			if (UProgressBar* Bar = Cast<UProgressBar>(Widget))
			{
				Bar->SetFillColorAndOpacity(NewColor);
				bSuccess = true;
			}
			else if (UTextBlock* TextWidget = Cast<UTextBlock>(Widget))
			{
				TextWidget->SetColorAndOpacity(FSlateColor(NewColor));
				bSuccess = true;
			}
			else if (UImage* ImageWidget = Cast<UImage>(Widget))
			{
				ImageWidget->SetColorAndOpacity(NewColor);
				bSuccess = true;
			}
			else if (UBorder* BorderWidget = Cast<UBorder>(Widget))
			{
				BorderWidget->SetBrushColor(NewColor);
				bSuccess = true;
			}
			else
			{
				// 说清楚是「这个控件不支持」而不是「参数写错了」
				ResultMessage = FString::Printf(
					TEXT("控件 %s（%s）不支持颜色设置，目前支持 ProgressBar / TextBlock / Image / Border。"),
					*WidgetName, *Widget->GetClass()->GetName());
			}

			if (bSuccess)
			{
				ResultMessage = FString::Printf(TEXT("Set %s to (%.2f, %.2f, %.2f, %.2f)"),
					*PropertyName, NewColor.R, NewColor.G, NewColor.B, NewColor.A);
			}
		}
		else
		{
			ResultMessage = ColorError;
		}
	}
	/**
	 * 字号。
	 *
	 * 以前这里没有入口，文档只写「设不了」，于是 2026-09-16 的反馈里，
	 * 调用方绕去 Python 试了三轮（`widget_tree` 是 protected、CDO 上找不到控件），
	 * 第四轮才摸到按对象路径 load 控件那条路。一个字号绕了四次往返，
	 * 而这里加一个分支就完了 —— 能用 C++ 做的能力不做成 Python 脚本。
	 *
	 * 只能改字号，不整个换字体：字体资产要用户自己选，而「大一点」是
	 * 调用方真正会提的需求。改的是字号这**一个字段**，字族、字重、轮廓都留着。
	 */
	else if (PropertyName.Equals(TEXT("FontSize"), ESearchCase::IgnoreCase))
	{
		double SizeValue = 0.0;
		if (Payload->TryGetNumberField(TEXT("value"), SizeValue) && SizeValue > 0.0)
		{
			if (UTextBlock* TextWidget = Cast<UTextBlock>(Widget))
			{
				// UTextBlock::GetFont() 是 5.1 才加的；5.0 上 Font 还是个公开成员。
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
				FSlateFontInfo Font = TextWidget->GetFont();
#else
				FSlateFontInfo Font = TextWidget->Font;
#endif
				Font.Size = static_cast<float>(SizeValue);
				TextWidget->SetFont(Font);
				bSuccess = true;
			}
			else if (URichTextBlock* RichWidget = Cast<URichTextBlock>(Widget))
			{
				// RichTextBlock 的字体长在 DefaultTextStyle 里，没有 GetFont
				FSlateFontInfo Font = RichWidget->GetDefaultTextStyle().Font;
				Font.Size = static_cast<float>(SizeValue);
				RichWidget->SetDefaultFont(Font);
				bSuccess = true;
			}
			else
			{
				ResultMessage = FString::Printf(
					TEXT("控件 %s（%s）没有字号可设，FontSize 只支持 TextBlock / RichTextBlock。"),
					*WidgetName, *Widget->GetClass()->GetName());
			}

			if (bSuccess)
			{
				ResultMessage = FString::Printf(TEXT("Set FontSize to: %.0f"), SizeValue);
			}
		}
		else
		{
			ResultMessage = TEXT("FontSize 要传一个大于 0 的数字，例如 34。");
		}
	}
	/**
	 * 水平对齐。声明在 `UTextLayoutWidget` 上，所以 TextBlock、RichTextBlock、
	 * EditableText、EditableTextBox、MultiLineEditableText 一次全覆盖 ——
	 * 逐个 Cast 的话每加一种控件都要回来补一次。
	 */
	else if (PropertyName.Equals(TEXT("Justification"), ESearchCase::IgnoreCase))
	{
		FString JustifyValue;
		if (Payload->TryGetStringField(TEXT("value"), JustifyValue))
		{
			if (UTextLayoutWidget* TextLayout = Cast<UTextLayoutWidget>(Widget))
			{
				ETextJustify::Type NewJustify = ETextJustify::Left;
				bool bKnown = true;
				if (JustifyValue.Equals(TEXT("Left"), ESearchCase::IgnoreCase)) NewJustify = ETextJustify::Left;
				else if (JustifyValue.Equals(TEXT("Center"), ESearchCase::IgnoreCase)) NewJustify = ETextJustify::Center;
				else if (JustifyValue.Equals(TEXT("Right"), ESearchCase::IgnoreCase)) NewJustify = ETextJustify::Right;
				else bKnown = false;

				if (bKnown)
				{
					TextLayout->SetJustification(NewJustify);
					bSuccess = true;
					ResultMessage = FString::Printf(TEXT("Set Justification to: %s"), *JustifyValue);
				}
				else
				{
					ResultMessage = FString::Printf(
						TEXT("Justification 只认 Left / Center / Right，收到的是 \"%s\"。"), *JustifyValue);
				}
			}
			else
			{
				ResultMessage = FString::Printf(
					TEXT("控件 %s（%s）不是文本类控件，Justification 支持 TextBlock / RichTextBlock / EditableText / EditableTextBox。"),
					*WidgetName, *Widget->GetClass()->GetName());
			}
		}
		else
		{
			ResultMessage = TEXT("Justification 要传字符串：Left / Center / Right。");
		}
	}
	else if (PropertyName.Equals(TEXT("Percent"), ESearchCase::IgnoreCase))
	{
		// ProgressBar.Percent
		double PercentValue = 0.0;
		if (Payload->TryGetNumberField(TEXT("value"), PercentValue))
		{
			if (UProgressBar* ProgressBar = Cast<UProgressBar>(Widget))
			{
				ProgressBar->SetPercent(PercentValue);
				bSuccess = true;
				ResultMessage = FString::Printf(TEXT("Set Percent to: %.2f"), PercentValue);
			}
		}
	}
	
	if (!bSuccess)
	{
		/**
		 * 上面每个分支失败时都拼了一句**具体**的话（这个控件是什么类、
		 * 支持哪几种、值该怎么传）。原来这里把它整个丢掉，只回一句
		 * 「Property may not be supported or value type is incorrect」——
		 * 三种完全不同的失败长成一个样，调用方只能换个参数再猜一轮。
		 * 2026-09-16 的反馈里，一次 BrushColor 失败回给模型的全部信息就是
		 * 「设置属性失败」六个字。拼好的诊断必须发出去。
		 */
		FString Detail = ResultMessage;
		if (Detail.IsEmpty())
		{
			/**
			 * 走到这里有两种情况：属性名根本不认识，或者认识但**这个控件类没有**
			 * （给 Border 设 Text 就是后者 —— Text 分支只 Cast UTextBlock）。
			 *
			 * 两种都按「这个类能设什么」来答。原来一律列全属性表，于是出现
			 * 「属性名 'Text' 不认识」后面跟着一张第一项就是 Text 的清单 ——
			 * 自相矛盾，调用方会判定整张清单不可信（2026-09-17 复测时点出来的）。
			 */
			Detail = FString::Printf(
				TEXT("%s 这个控件不支持 '%s'。它能设的是：%s。"),
				*Widget->GetClass()->GetName(), *PropertyName, *UAL_SupportedPropertiesFor(Widget));
		}

		UAL_CommandUtils::SendError(RequestId, 400,
			FString::Printf(TEXT("Failed to set property '%s' on widget '%s'（%s）: %s"),
				*PropertyName, *WidgetName, *Widget->GetClass()->GetName(), *Detail));
		return;
	}
	
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("widget_name"), WidgetName);
	Result->SetStringField(TEXT("property_name"), PropertyName);
	Result->SetStringField(TEXT("message"), ResultMessage);
	if (ReadBackValue.IsValid())
	{
		Result->SetField(TEXT("value"), ReadBackValue);
	}

	UE_LOG(LogUALWidget, Log, TEXT("widget.set_property: widget=%s, property=%s"), *WidgetName, *PropertyName);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.set_property is only available in editor mode"));
#endif
}

// ============================================================================
// 辅助函数
// ============================================================================

UWidgetBlueprint* FUAL_WidgetCommands::LoadWidgetBlueprint(const FString& Path, FString& OutError)
{
#if WITH_EDITOR
	// 尝试多种路径格式
	FString NormalizedPath = Path;
	if (!NormalizedPath.StartsWith(TEXT("/")))
	{
		NormalizedPath = TEXT("/") + NormalizedPath;
	}
	
	// 尝试直接加载
	UObject* LoadedObject = StaticLoadObject(UWidgetBlueprint::StaticClass(), nullptr, *NormalizedPath);
	if (LoadedObject)
	{
		return Cast<UWidgetBlueprint>(LoadedObject);
	}
	
	// 尝试添加 _C 后缀（BlueprintGeneratedClass）
	if (!NormalizedPath.EndsWith(TEXT("_C")))
	{
		FString ClassPath = NormalizedPath + TEXT("_C");
		LoadedObject = StaticLoadObject(UWidgetBlueprint::StaticClass(), nullptr, *ClassPath);
		if (LoadedObject)
		{
			return Cast<UWidgetBlueprint>(LoadedObject);
		}
	}
	
	// 尝试通过 Asset Registry 查找
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	FString PackagePath, AssetName;
	NormalizedPath.Split(TEXT("."), &PackagePath, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (AssetName.IsEmpty())
	{
		// 从路径获取资产名
		int32 LastSlash;
		if (NormalizedPath.FindLastChar(TEXT('/'), LastSlash))
		{
			AssetName = NormalizedPath.Mid(LastSlash + 1);
			PackagePath = NormalizedPath.Left(LastSlash);
		}
	}
	
	TArray<FAssetData> AssetDataList;
	FARFilter Filter;
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
	Filter.ClassPaths.Add(UWidgetBlueprint::StaticClass()->GetClassPathName());
#else
	Filter.ClassNames.Add(UWidgetBlueprint::StaticClass()->GetFName());
#endif
	Filter.PackagePaths.Add(FName(*PackagePath));
	AssetRegistry.GetAssets(Filter, AssetDataList);
	
	for (const FAssetData& AssetData : AssetDataList)
	{
		if (AssetData.AssetName.ToString() == AssetName || AssetData.GetFullName().Contains(AssetName))
		{
			UObject* Asset = AssetData.GetAsset();
			if (UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Asset))
			{
				return WidgetBP;
			}
		}
	}
	
	OutError = FString::Printf(TEXT("Widget Blueprint not found: %s"), *Path);
	return nullptr;
#else
	OutError = TEXT("LoadWidgetBlueprint is only available in editor mode");
	return nullptr;
#endif
}

UWidget* FUAL_WidgetCommands::FindWidgetByName(UWidgetTree* WidgetTree, const FString& WidgetName)
{
	if (!WidgetTree)
	{
		return nullptr;
	}
	
	// 如果名称为空或 "root"，返回根控件
	if (WidgetName.IsEmpty() || WidgetName.Equals(TEXT("root"), ESearchCase::IgnoreCase))
	{
		return WidgetTree->RootWidget;
	}
	
	UWidget* FoundWidget = nullptr;
	WidgetTree->ForEachWidget([&FoundWidget, &WidgetName](UWidget* Widget)
	{
		if (Widget && Widget->GetName().Equals(WidgetName, ESearchCase::IgnoreCase))
		{
			FoundWidget = Widget;
		}
	});
	
	return FoundWidget;
}

UClass* FUAL_WidgetCommands::FindWidgetClass(const FString& ClassName)
{
	// 常见控件类型映射
	static TMap<FString, UClass*> WidgetClassMap;
	if (WidgetClassMap.Num() == 0)
	{
		WidgetClassMap.Add(TEXT("Button"), UButton::StaticClass());
		WidgetClassMap.Add(TEXT("Text"), UTextBlock::StaticClass());
		WidgetClassMap.Add(TEXT("TextBlock"), UTextBlock::StaticClass());
		WidgetClassMap.Add(TEXT("Image"), UImage::StaticClass());
		WidgetClassMap.Add(TEXT("CanvasPanel"), UCanvasPanel::StaticClass());
		WidgetClassMap.Add(TEXT("VerticalBox"), UVerticalBox::StaticClass());
		WidgetClassMap.Add(TEXT("HorizontalBox"), UHorizontalBox::StaticClass());
		WidgetClassMap.Add(TEXT("Overlay"), UOverlay::StaticClass());
		WidgetClassMap.Add(TEXT("Border"), UBorder::StaticClass());
		WidgetClassMap.Add(TEXT("ScrollBox"), UScrollBox::StaticClass());
		WidgetClassMap.Add(TEXT("SizeBox"), USizeBox::StaticClass());
		WidgetClassMap.Add(TEXT("Spacer"), USpacer::StaticClass());
		WidgetClassMap.Add(TEXT("ProgressBar"), UProgressBar::StaticClass());
		WidgetClassMap.Add(TEXT("Slider"), USlider::StaticClass());
		WidgetClassMap.Add(TEXT("CheckBox"), UCheckBox::StaticClass());
		WidgetClassMap.Add(TEXT("ComboBox"), UComboBoxString::StaticClass());
		WidgetClassMap.Add(TEXT("ComboBoxString"), UComboBoxString::StaticClass());
		WidgetClassMap.Add(TEXT("EditableText"), UEditableText::StaticClass());
		WidgetClassMap.Add(TEXT("EditableTextBox"), UEditableTextBox::StaticClass());
		WidgetClassMap.Add(TEXT("SpinBox"), USpinBox::StaticClass());
		WidgetClassMap.Add(TEXT("RichTextBlock"), URichTextBlock::StaticClass());
		WidgetClassMap.Add(TEXT("GridPanel"), UGridPanel::StaticClass());
		WidgetClassMap.Add(TEXT("WrapBox"), UWrapBox::StaticClass());
		WidgetClassMap.Add(TEXT("UniformGridPanel"), UUniformGridPanel::StaticClass());
	}
	
	UClass** FoundClass = WidgetClassMap.Find(ClassName);
	if (FoundClass)
	{
		return *FoundClass;
	}
	
	// 尝试动态查找
	FString FullClassName = FString::Printf(TEXT("U%s"), *ClassName);
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
	UClass* DynamicClass = FindFirstObject<UClass>(*FullClassName, EFindFirstObjectOptions::None);
#else
	UClass* DynamicClass = FindObject<UClass>(ANY_PACKAGE, *FullClassName);
#endif
	if (DynamicClass && DynamicClass->IsChildOf(UWidget::StaticClass()))
	{
		return DynamicClass;
	}
	
	return nullptr;
}

TSharedPtr<FJsonObject> FUAL_WidgetCommands::BuildWidgetJson(UWidget* Widget)
{
	if (!Widget)
	{
		return nullptr;
	}
	
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	
	// 基本信息
	Obj->SetStringField(TEXT("name"), Widget->GetName());
	Obj->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
	Obj->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
	// 报**设计期的 Visibility 属性**，不是 IsVisible()。
	//
	// IsVisible() 问的是「此刻在屏幕上看得见吗」，而这里遍历的是
	// WidgetBlueprint 里的模板控件 —— 它们没有对应的 Slate 实例，
	// 所以**恒为 false**。调用方据此会以为整个界面都被隐藏了，
	// 然后去「修」一个本来就没问题的可见性。
	Obj->SetStringField(TEXT("visibility"),
		StaticEnum<ESlateVisibility>()
			? StaticEnum<ESlateVisibility>()->GetNameStringByValue((int64)Widget->GetVisibility())
			: TEXT("Unknown"));
	
	// Slot 信息
	if (UPanelSlot* Slot = Widget->Slot)
	{
		Obj->SetStringField(TEXT("slot_type"), Slot->GetClass()->GetName());
		
		// 根据 Slot 类型导出具体属性
		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
		{
			Obj->SetObjectField(TEXT("slot_data"), BuildCanvasSlotJson(CanvasSlot));
		}
		else if (UVerticalBoxSlot* VSlot = Cast<UVerticalBoxSlot>(Slot))
		{
			Obj->SetObjectField(TEXT("slot_data"), BuildVerticalSlotJson(VSlot));
		}
		else if (UHorizontalBoxSlot* HSlot = Cast<UHorizontalBoxSlot>(Slot))
		{
			Obj->SetObjectField(TEXT("slot_data"), BuildHorizontalSlotJson(HSlot));
		}
		else if (UOverlaySlot* OSlot = Cast<UOverlaySlot>(Slot))
		{
			// Overlay Slot
			TSharedPtr<FJsonObject> SlotData = MakeShared<FJsonObject>();
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
			SlotData->SetStringField(TEXT("h_align"), UEnum::GetValueAsString(OSlot->GetHorizontalAlignment()));
			SlotData->SetStringField(TEXT("v_align"), UEnum::GetValueAsString(OSlot->GetVerticalAlignment()));
#else
			SlotData->SetStringField(TEXT("h_align"), UEnum::GetValueAsString(TEXT("EHorizontalAlignment"), OSlot->HorizontalAlignment));
			SlotData->SetStringField(TEXT("v_align"), UEnum::GetValueAsString(TEXT("EVerticalAlignment"), OSlot->VerticalAlignment));
#endif
			Obj->SetObjectField(TEXT("slot_data"), SlotData);
		}
		else if (UGridSlot* GSlot = Cast<UGridSlot>(Slot))
		{
			// Grid Slot
			TSharedPtr<FJsonObject> SlotData = MakeShared<FJsonObject>();
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
			SlotData->SetNumberField(TEXT("row"), GSlot->GetRow());
			SlotData->SetNumberField(TEXT("column"), GSlot->GetColumn());
			SlotData->SetNumberField(TEXT("row_span"), GSlot->GetRowSpan());
			SlotData->SetNumberField(TEXT("column_span"), GSlot->GetColumnSpan());
#else
			SlotData->SetNumberField(TEXT("row"), GSlot->Row);
			SlotData->SetNumberField(TEXT("column"), GSlot->Column);
			SlotData->SetNumberField(TEXT("row_span"), GSlot->RowSpan);
			SlotData->SetNumberField(TEXT("column_span"), GSlot->ColumnSpan);
#endif
			Obj->SetObjectField(TEXT("slot_data"), SlotData);
		}
	}
	
	// 如果是容器控件，递归处理子控件
	if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
	{
		TArray<TSharedPtr<FJsonValue>> Children;
		for (int32 i = 0; i < Panel->GetChildrenCount(); i++)
		{
			UWidget* Child = Panel->GetChildAt(i);
			TSharedPtr<FJsonObject> ChildJson = BuildWidgetJson(Child);
			if (ChildJson.IsValid())
			{
				Children.Add(MakeShared<FJsonValueObject>(ChildJson));
			}
		}
		
		if (Children.Num() > 0)
		{
			Obj->SetArrayField(TEXT("children"), Children);
		}
	}
	
	return Obj;
}

TSharedPtr<FJsonObject> FUAL_WidgetCommands::BuildCanvasSlotJson(UCanvasPanelSlot* Slot)
{
	if (!Slot)
	{
		return nullptr;
	}
	
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	
	// Anchors
	FAnchors Anchors = Slot->GetAnchors();
	TSharedPtr<FJsonObject> AnchorsObj = MakeShared<FJsonObject>();
	AnchorsObj->SetNumberField(TEXT("min_x"), Anchors.Minimum.X);
	AnchorsObj->SetNumberField(TEXT("min_y"), Anchors.Minimum.Y);
	AnchorsObj->SetNumberField(TEXT("max_x"), Anchors.Maximum.X);
	AnchorsObj->SetNumberField(TEXT("max_y"), Anchors.Maximum.Y);
	Obj->SetObjectField(TEXT("anchors"), AnchorsObj);
	
	// Offsets (Position/Size)
	FMargin Offsets = Slot->GetOffsets();
	TSharedPtr<FJsonObject> OffsetsObj = MakeShared<FJsonObject>();
	OffsetsObj->SetNumberField(TEXT("left"), Offsets.Left);
	OffsetsObj->SetNumberField(TEXT("top"), Offsets.Top);
	OffsetsObj->SetNumberField(TEXT("right"), Offsets.Right);
	OffsetsObj->SetNumberField(TEXT("bottom"), Offsets.Bottom);
	Obj->SetObjectField(TEXT("offsets"), OffsetsObj);
	
	// Position
	FVector2D Position = Slot->GetPosition();
	TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
	PosObj->SetNumberField(TEXT("x"), Position.X);
	PosObj->SetNumberField(TEXT("y"), Position.Y);
	Obj->SetObjectField(TEXT("position"), PosObj);
	
	// Size
	FVector2D Size = Slot->GetSize();
	TSharedPtr<FJsonObject> SizeObj = MakeShared<FJsonObject>();
	SizeObj->SetNumberField(TEXT("width"), Size.X);
	SizeObj->SetNumberField(TEXT("height"), Size.Y);
	Obj->SetObjectField(TEXT("size"), SizeObj);
	
	// Alignment
	FVector2D Alignment = Slot->GetAlignment();
	TSharedPtr<FJsonObject> AlignObj = MakeShared<FJsonObject>();
	AlignObj->SetNumberField(TEXT("x"), Alignment.X);
	AlignObj->SetNumberField(TEXT("y"), Alignment.Y);
	Obj->SetObjectField(TEXT("alignment"), AlignObj);
	
	// Auto Size
	Obj->SetBoolField(TEXT("auto_size"), Slot->GetAutoSize());
	
	// Z Order
	Obj->SetNumberField(TEXT("z_order"), Slot->GetZOrder());
	
	return Obj;
}

TSharedPtr<FJsonObject> FUAL_WidgetCommands::BuildVerticalSlotJson(UVerticalBoxSlot* Slot)
{
	if (!Slot)
	{
		return nullptr;
	}
	
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	
	// Padding
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
	FMargin Padding = Slot->GetPadding();
#else
	FMargin Padding = Slot->Padding;
#endif
	TSharedPtr<FJsonObject> PaddingObj = MakeShared<FJsonObject>();
	PaddingObj->SetNumberField(TEXT("left"), Padding.Left);
	PaddingObj->SetNumberField(TEXT("top"), Padding.Top);
	PaddingObj->SetNumberField(TEXT("right"), Padding.Right);
	PaddingObj->SetNumberField(TEXT("bottom"), Padding.Bottom);
	Obj->SetObjectField(TEXT("padding"), PaddingObj);
	
	// Size
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
	FSlateChildSize Size = Slot->GetSize();
	Obj->SetStringField(TEXT("h_align"), UEnum::GetValueAsString(Slot->GetHorizontalAlignment()));
	Obj->SetStringField(TEXT("v_align"), UEnum::GetValueAsString(Slot->GetVerticalAlignment()));
#else
	FSlateChildSize Size = Slot->Size;
	Obj->SetStringField(TEXT("h_align"), UEnum::GetValueAsString(TEXT("EHorizontalAlignment"), Slot->HorizontalAlignment));
	Obj->SetStringField(TEXT("v_align"), UEnum::GetValueAsString(TEXT("EVerticalAlignment"), Slot->VerticalAlignment));
#endif
	Obj->SetStringField(TEXT("size_rule"), Size.SizeRule == ESlateSizeRule::Automatic ? TEXT("Auto") : TEXT("Fill"));
	Obj->SetNumberField(TEXT("size_value"), Size.Value);
	
	return Obj;
}

TSharedPtr<FJsonObject> FUAL_WidgetCommands::BuildHorizontalSlotJson(UHorizontalBoxSlot* Slot)
{
	if (!Slot)
	{
		return nullptr;
	}
	
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	
	// Padding
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
	FMargin Padding = Slot->GetPadding();
#else
	FMargin Padding = Slot->Padding;
#endif
	TSharedPtr<FJsonObject> PaddingObj = MakeShared<FJsonObject>();
	PaddingObj->SetNumberField(TEXT("left"), Padding.Left);
	PaddingObj->SetNumberField(TEXT("top"), Padding.Top);
	PaddingObj->SetNumberField(TEXT("right"), Padding.Right);
	PaddingObj->SetNumberField(TEXT("bottom"), Padding.Bottom);
	Obj->SetObjectField(TEXT("padding"), PaddingObj);
	
	// Size
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
	FSlateChildSize Size = Slot->GetSize();
	Obj->SetStringField(TEXT("h_align"), UEnum::GetValueAsString(Slot->GetHorizontalAlignment()));
	Obj->SetStringField(TEXT("v_align"), UEnum::GetValueAsString(Slot->GetVerticalAlignment()));
#else
	FSlateChildSize Size = Slot->Size;
	Obj->SetStringField(TEXT("h_align"), UEnum::GetValueAsString(TEXT("EHorizontalAlignment"), Slot->HorizontalAlignment));
	Obj->SetStringField(TEXT("v_align"), UEnum::GetValueAsString(TEXT("EVerticalAlignment"), Slot->VerticalAlignment));
#endif
	Obj->SetStringField(TEXT("size_rule"), Size.SizeRule == ESlateSizeRule::Automatic ? TEXT("Auto") : TEXT("Fill"));
	Obj->SetNumberField(TEXT("size_value"), Size.Value);
	
	return Obj;
}

FAnchors FUAL_WidgetCommands::ParseAnchors(const FString& AnchorsStr)
{
	// 预设锚点映射
	if (AnchorsStr.Equals(TEXT("TopLeft"), ESearchCase::IgnoreCase))
	{
		return FAnchors(0.0f, 0.0f, 0.0f, 0.0f);
	}
	else if (AnchorsStr.Equals(TEXT("TopCenter"), ESearchCase::IgnoreCase))
	{
		return FAnchors(0.5f, 0.0f, 0.5f, 0.0f);
	}
	else if (AnchorsStr.Equals(TEXT("TopRight"), ESearchCase::IgnoreCase))
	{
		return FAnchors(1.0f, 0.0f, 1.0f, 0.0f);
	}
	else if (AnchorsStr.Equals(TEXT("CenterLeft"), ESearchCase::IgnoreCase))
	{
		return FAnchors(0.0f, 0.5f, 0.0f, 0.5f);
	}
	else if (AnchorsStr.Equals(TEXT("Center"), ESearchCase::IgnoreCase))
	{
		return FAnchors(0.5f, 0.5f, 0.5f, 0.5f);
	}
	else if (AnchorsStr.Equals(TEXT("CenterRight"), ESearchCase::IgnoreCase))
	{
		return FAnchors(1.0f, 0.5f, 1.0f, 0.5f);
	}
	else if (AnchorsStr.Equals(TEXT("BottomLeft"), ESearchCase::IgnoreCase))
	{
		return FAnchors(0.0f, 1.0f, 0.0f, 1.0f);
	}
	else if (AnchorsStr.Equals(TEXT("BottomCenter"), ESearchCase::IgnoreCase))
	{
		return FAnchors(0.5f, 1.0f, 0.5f, 1.0f);
	}
	else if (AnchorsStr.Equals(TEXT("BottomRight"), ESearchCase::IgnoreCase))
	{
		return FAnchors(1.0f, 1.0f, 1.0f, 1.0f);
	}
	else if (AnchorsStr.Equals(TEXT("Stretch"), ESearchCase::IgnoreCase) || 
	         AnchorsStr.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))
	{
		return FAnchors(0.0f, 0.0f, 1.0f, 1.0f);
	}
	
	// 默认左上角
	return FAnchors(0.0f, 0.0f, 0.0f, 0.0f);
}

FVector2D FUAL_WidgetCommands::ParseVector2D(const TSharedPtr<FJsonObject>& Obj)
{
	if (!Obj.IsValid())
	{
		return FVector2D::ZeroVector;
	}
	
	double X = 0.0, Y = 0.0;
	
	// 支持 x/y 或 width/height
	if (!Obj->TryGetNumberField(TEXT("x"), X))
	{
		Obj->TryGetNumberField(TEXT("width"), X);
	}
	if (!Obj->TryGetNumberField(TEXT("y"), Y))
	{
		Obj->TryGetNumberField(TEXT("height"), Y);
	}
	
	return FVector2D(X, Y);
}

// ============================================================================
// 辅助函数: 唯一名称生成（防止重名崩溃）
// ============================================================================

FName FUAL_WidgetCommands::GenerateUniqueWidgetName(UWidgetTree* WidgetTree, const FString& DesiredName)
{
	if (!WidgetTree || DesiredName.IsEmpty())
	{
		return NAME_None;
	}
	
	// 先检查期望名称是否可用
	if (!WidgetTree->FindWidget(FName(*DesiredName)))
	{
		return FName(*DesiredName);
	}
	
	// 名称已存在，追加后缀递增
	UE_LOG(LogUALWidget, Warning, TEXT("Widget name '%s' already exists, generating unique name"), *DesiredName);
	
	for (int32 i = 1; i < 1000; i++)
	{
		FString Candidate = FString::Printf(TEXT("%s_%d"), *DesiredName, i);
		if (!WidgetTree->FindWidget(FName(*Candidate)))
		{
			UE_LOG(LogUALWidget, Log, TEXT("Using unique name: %s"), *Candidate);
			return FName(*Candidate);
		}
	}
	
	// 极端情况 fallback：使用 MakeUniqueObjectName
	return MakeUniqueObjectName(WidgetTree, UWidget::StaticClass(), FName(*DesiredName));
}
