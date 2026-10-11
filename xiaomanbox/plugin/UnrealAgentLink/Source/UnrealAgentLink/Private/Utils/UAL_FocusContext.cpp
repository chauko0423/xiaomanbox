#include "UAL_FocusContext.h"

#include "UAL_MaterialCommands.h"
#include "UAL_VersionCompat.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/IToolkit.h"

#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialInstance.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"

#include "BlueprintEditor.h"
#include "Kismet2/KismetEditorUtilities.h"

#include "IMaterialEditor.h"
#include "MaterialGraph/MaterialGraphNode.h"

#include "AssetRegistry/AssetData.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"

namespace
{
	/**
	 * 每类列表最多报多少条。
	 *
	 * 用户框选整关卡是很正常的操作，全量回传能到几千条 —— 这段上下文是给模型
	 * 每轮读的，撑爆的代价远大于「多看到第 51 个 Actor」的收益。
	 * 超出时另给 *_truncated 和总数，让模型知道自己看到的不是全部。
	 */
	constexpr int32 MAX_ITEMS = 50;

	/**
	 * 蓝图节点的 id 格式。
	 *
	 * 必须和 blueprint.get_graph 回的 node_id 逐字一致，
	 * 见 UAL_BlueprintCommands.cpp 的 UAL_GuidToString。
	 */
	FString NodeGuidToString(const FGuid& Guid)
	{
		return Guid.ToString(EGuidFormats::DigitsWithHyphens);
	}

	/** 资产类型标签。focusContext.ts 按 "blueprint" 这类值分支，别随手改字面量 */
	FString ClassifyAsset(const UObject* Asset)
	{
		if (!Asset) return TEXT("other");
		if (Asset->IsA(UBlueprint::StaticClass())) return TEXT("blueprint");
		if (Asset->IsA(UMaterial::StaticClass())) return TEXT("material");
		if (Asset->IsA(UMaterialInstance::StaticClass())) return TEXT("material_instance");
		if (Asset->IsA(UTexture::StaticClass())) return TEXT("texture");
		if (Asset->IsA(UStaticMesh::StaticClass()) || Asset->IsA(USkeletalMesh::StaticClass())) return TEXT("mesh");
		return TEXT("other");
	}

	TSharedPtr<FJsonObject> DescribeEditedAsset(UObject* Asset)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		const FString Type = ClassifyAsset(Asset);

		Info->SetStringField(TEXT("type"), Type);
		Info->SetStringField(TEXT("name"), Asset->GetName());
		Info->SetStringField(TEXT("path"), Asset->GetPathName());
		if (const UPackage* Package = Asset->GetOutermost())
		{
			Info->SetBoolField(TEXT("isModified"), Package->IsDirty());
		}

		if (const UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
		{
			if (Blueprint->ParentClass)
			{
				Info->SetStringField(TEXT("parentClass"), Blueprint->ParentClass->GetName());
			}
		}
		else if (const UMaterialInstance* Instance = Cast<UMaterialInstance>(Asset))
		{
			if (Instance->Parent)
			{
				Info->SetStringField(TEXT("parentMaterial"), Instance->Parent->GetPathName());
			}
		}

		return Info;
	}

	/** 节点标题。ListView 那版是不带换行的单行标题，适合塞进 JSON */
	FString NodeTitle(const UEdGraphNode* Node)
	{
		return Node ? Node->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString();
	}

	/**
	 * 蓝图：当前聚焦的图 + 图里选中的节点。
	 *
	 * 只有 IBlueprintEditor 接口拿不到这两样 —— GetFocusedGraph 是 5.2 才提到接口上的，
	 * 5.0/5.1 上只在 FBlueprintEditor 这个具体类上有。所以统一往具体类上转：
	 * 全引擎里 FBlueprintEditor 是 IBlueprintEditor 唯一的实现，且是它的第一个基类。
	 */
	void AddBlueprintFocus(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Out)
	{
		TSharedPtr<IBlueprintEditor> Editor = FKismetEditorUtilities::GetIBlueprintEditorForObject(Blueprint, /*bOpenEditor=*/false);
		if (!Editor.IsValid())
		{
			return;
		}
		FBlueprintEditor* Concrete = static_cast<FBlueprintEditor*>(Editor.Get());

		if (UEdGraph* Graph = Concrete->GetFocusedGraph())
		{
			TSharedPtr<FJsonObject> GraphJson = MakeShared<FJsonObject>();
			GraphJson->SetStringField(TEXT("name"), Graph->GetName());
			GraphJson->SetStringField(TEXT("path"), Graph->GetPathName());
			GraphJson->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
			Out->SetObjectField(TEXT("focusedGraph"), GraphJson);
		}

		const FGraphPanelSelectionSet Selected = Concrete->GetSelectedNodes();
		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (UObject* Object : Selected)
		{
			UEdGraphNode* Node = Cast<UEdGraphNode>(Object);
			if (!Node) continue;
			if (Nodes.Num() >= MAX_ITEMS) break;

			TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
			NodeJson->SetStringField(TEXT("node_id"), NodeGuidToString(Node->NodeGuid));
			NodeJson->SetStringField(TEXT("class"), Node->GetClass()->GetName());
			NodeJson->SetStringField(TEXT("title"), NodeTitle(Node));
			NodeJson->SetNumberField(TEXT("pos_x"), Node->NodePosX);
			NodeJson->SetNumberField(TEXT("pos_y"), Node->NodePosY);
			Nodes.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
		Out->SetArrayField(TEXT("selectedNodes"), Nodes);
		Out->SetNumberField(TEXT("selectedNodeCount"), Selected.Num());
		Out->SetBoolField(TEXT("selectedNodesTruncated"), Selected.Num() > Nodes.Num());
	}

	/**
	 * 材质：材质图里选中的节点。
	 *
	 * 材质编辑器只有一张图，没有「聚焦哪张图」的概念，所以不报 focusedGraph。
	 *
	 * 转换前先比 GetEditorName()：材质实例编辑器（FMaterialInstanceEditor）也开
	 * 材质资产，但它不是 IMaterialEditor，认名字才不会转错。
	 */
	void AddMaterialFocus(UMaterial* Material, IAssetEditorInstance* Instance, const TSharedPtr<FJsonObject>& Out)
	{
		if (!Instance || Instance->GetEditorName() != FName(TEXT("MaterialEditor")))
		{
			return;
		}
		IMaterialEditor* MaterialEditor = static_cast<IMaterialEditor*>(Instance);

		TMap<UMaterialExpression*, FString> ExpressionIds;
		FUAL_MaterialCommands::BuildExpressionIds(Material, ExpressionIds);

		const TSet<UObject*> Selected = MaterialEditor->GetSelectedNodes();
		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (UObject* Object : Selected)
		{
			UEdGraphNode* Node = Cast<UEdGraphNode>(Object);
			if (!Node) continue;
			if (Nodes.Num() >= MAX_ITEMS) break;

			TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
			NodeJson->SetStringField(TEXT("class"), Node->GetClass()->GetName());
			NodeJson->SetStringField(TEXT("title"), NodeTitle(Node));
			NodeJson->SetNumberField(TEXT("pos_x"), Node->NodePosX);
			NodeJson->SetNumberField(TEXT("pos_y"), Node->NodePosY);

			// 注释框和根节点不是表达式，没有 node_id —— 少一个字段，不是错误
			if (const UMaterialGraphNode* GraphNode = Cast<UMaterialGraphNode>(Node))
			{
				if (const FString* Id = ExpressionIds.Find(GraphNode->MaterialExpression))
				{
					NodeJson->SetStringField(TEXT("node_id"), *Id);
				}

				/*
				 * GUID 也报一份 —— 隔了一段时间才用的调用方只能认它。
				 *
				 * 上面那个 node_id 是**位置编号**（`类名_数组下标`，见
				 * BuildExpressionIds）。删掉前面一个节点，后面全体位移，旧编号就
				 * 指向另一个同类节点了，**而且不报错**：连线报成功，回读却显示接在
				 * 别人身上。material.get_graph 早就同时报 guid，节点解析也把 GUID
				 * 排在最前面优先匹配 —— 只有这里还差一份。
				 *
				 * 差在这里的后果最重：焦点上下文是给「闪存」用的，那份快照钉在用户
				 * 发消息那一刻，模型可能几十秒后才拿它去改图。这中间用户删一个节点，
				 * 位置编号就已经指向别人了。事后再查 GUID 也认不回来 —— 那时只知道
				 * 「现在有哪些节点」，答不出「当时选的是哪个」。
				 */
				if (GraphNode->MaterialExpression)
				{
					NodeJson->SetStringField(
						TEXT("guid"),
						GraphNode->MaterialExpression->GetMaterialExpressionId().ToString());
				}
			}
			Nodes.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
		Out->SetArrayField(TEXT("selectedNodes"), Nodes);
		Out->SetNumberField(TEXT("selectedNodeCount"), Selected.Num());
		Out->SetBoolField(TEXT("selectedNodesTruncated"), Selected.Num() > Nodes.Num());
	}

	/** 关卡里选中的 Actor */
	void AddLevelSelection(const TSharedPtr<FJsonObject>& Out)
	{
		USelection* Selection = GEditor ? GEditor->GetSelectedActors() : nullptr;
		if (!Selection)
		{
			return;
		}

		TArray<AActor*> Actors;
		Selection->GetSelectedObjects<AActor>(Actors);

		TArray<TSharedPtr<FJsonValue>> Items;
		for (AActor* Actor : Actors)
		{
			if (!Actor) continue;
			if (Items.Num() >= MAX_ITEMS) break;

			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Actor->GetName());
			Item->SetStringField(TEXT("label"), Actor->GetActorLabel());
			Item->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
			Item->SetStringField(TEXT("path"), Actor->GetPathName());
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}

		Out->SetArrayField(TEXT("selectedActors"), Items);
		Out->SetNumberField(TEXT("selectedActorCount"), Actors.Num());
		Out->SetBoolField(TEXT("selectedActorsTruncated"), Actors.Num() > Items.Num());
	}

	/**
	 * 内容浏览器里选中的资产和当前文件夹。
	 *
	 * 用 GetModulePtr 而不是 LoadModuleChecked —— 这只是上下文里的一段，
	 * 内容浏览器模块没起来（命令行编辑器）时该少一个字段，不该整条命令崩掉。
	 */
	void AddContentBrowserSelection(const TSharedPtr<FJsonObject>& Out)
	{
		FContentBrowserModule* Module = FModuleManager::GetModulePtr<FContentBrowserModule>(TEXT("ContentBrowser"));
		if (!Module)
		{
			return;
		}
		IContentBrowserSingleton& ContentBrowser = Module->Get();

		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();

		TArray<FAssetData> SelectedAssets;
		ContentBrowser.GetSelectedAssets(SelectedAssets);

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FAssetData& Asset : SelectedAssets)
		{
			if (Items.Num() >= MAX_ITEMS) break;
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
			// GetSoftObjectPath() 是 5.1 才有的；5.0 上只有 FName 的 ObjectPath
			Item->SetStringField(TEXT("path"), UALCompat::GetObjectPathString(Asset));
			if (const UClass* Class = Asset.GetClass())
			{
				Item->SetStringField(TEXT("class"), Class->GetName());
			}
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		Info->SetArrayField(TEXT("selectedAssets"), Items);
		Info->SetNumberField(TEXT("selectedAssetCount"), SelectedAssets.Num());
		Info->SetBoolField(TEXT("selectedAssetsTruncated"), SelectedAssets.Num() > Items.Num());

		TArray<FString> Folders;
		ContentBrowser.GetSelectedPathViewFolders(Folders);
		TArray<TSharedPtr<FJsonValue>> FolderItems;
		for (const FString& Folder : Folders)
		{
			FolderItems.Add(MakeShared<FJsonValueString>(Folder));
		}
		Info->SetArrayField(TEXT("selectedFolders"), FolderItems);

		Out->SetObjectField(TEXT("contentBrowser"), Info);
	}
}

TSharedPtr<FJsonObject> FUAL_FocusContext::Build()
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> OpenEditors;
	TSharedPtr<FJsonObject> FocusedEditor;

	UObject* FocusedAsset = nullptr;
	IAssetEditorInstance* FocusedInstance = nullptr;

	UAssetEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (Subsystem)
	{
		// 焦点 = 最后一次被激活的那个编辑器。
		//
		// 以前这里取的是 GetAllEditedAssets() 的第一个，注释还写着「通常是最近打开的」——
		// 那个数组的顺序是内部容器序，跟用户看哪个窗口没关系。用户开着五个编辑器时
		// 「当前这个蓝图」基本是错的。GetLastActivationTime 是 IAssetEditorInstance
		// 的正式接口，5.0 起就有。
		double BestActivation = -1.0;

		for (UObject* Asset : Subsystem->GetAllEditedAssets())
		{
			if (!Asset) continue;

			IAssetEditorInstance* Instance = Subsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false);
			const double Activation = Instance ? Instance->GetLastActivationTime() : -1.0;

			TSharedPtr<FJsonObject> Info = DescribeEditedAsset(Asset);
			Info->SetNumberField(TEXT("lastActivationTime"), Activation);
			OpenEditors.Add(MakeShared<FJsonValueObject>(Info));

			if (Activation > BestActivation || !FocusedEditor.IsValid())
			{
				BestActivation = Activation;
				FocusedEditor = Info;
				FocusedAsset = Asset;
				FocusedInstance = Instance;
			}
		}
	}

	// 没开任何资产编辑器时，焦点就是关卡本身
	if (!FocusedEditor.IsValid() && GEditor && GEditor->GetWorld())
	{
		UWorld* World = GEditor->GetWorld();
		FocusedEditor = MakeShared<FJsonObject>();
		FocusedEditor->SetStringField(TEXT("type"), TEXT("level"));
		FocusedEditor->SetStringField(TEXT("name"), World->GetMapName());
		FocusedEditor->SetStringField(TEXT("path"), World->GetOutermost()->GetName());
		FocusedEditor->SetBoolField(TEXT("isModified"), World->GetOutermost()->IsDirty());
	}

	if (FocusedEditor.IsValid())
	{
		Result->SetObjectField(TEXT("focusedEditor"), FocusedEditor);
	}
	Result->SetArrayField(TEXT("openEditors"), OpenEditors);
	Result->SetBoolField(TEXT("hasOpenEditors"), OpenEditors.Num() > 0);

	// 图和节点只对**焦点那个**编辑器取。别的编辑器里选着什么跟这句话无关
	if (UBlueprint* Blueprint = Cast<UBlueprint>(FocusedAsset))
	{
		AddBlueprintFocus(Blueprint, Result);
	}
	else if (UMaterial* Material = Cast<UMaterial>(FocusedAsset))
	{
		AddMaterialFocus(Material, FocusedInstance, Result);
	}

	AddLevelSelection(Result);
	AddContentBrowserSelection(Result);

	return Result;
}
