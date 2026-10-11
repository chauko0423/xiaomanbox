#include "UAL_MessageLogCommands.h"
#include "UAL_CommandUtils.h"
#include "UAL_NetworkManager.h"

#include "MessageLogModule.h"
#include "IMessageLogListing.h"
#include "Logging/TokenizedMessage.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALMessageLog, Log, All);

// ========== 订阅管理 ==========
// 存储已订阅的类别及其委托句柄

void FUAL_MessageLogCommands::RegisterCommands(TMap<FString, FHandlerFunc>& CommandMap)
{
	CommandMap.Add(TEXT("messagelog.list"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_List(Payload, RequestId);
	});

	CommandMap.Add(TEXT("messagelog.get"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_Get(Payload, RequestId);
	});

	// messagelog.subscribe / unsubscribe 已删（2026-09-16）：它们推 messagelog.changed
	// 事件，而盒子这边从来没订阅过，也没有任何地方接那个事件。
	// 要看日志走 messagelog.list / messagelog.get 拉取
}

FString FUAL_MessageLogCommands::SeverityToString(int32 Severity)
{
	switch (Severity)
	{
	case EMessageSeverity::Error: return TEXT("Error");
	case EMessageSeverity::PerformanceWarning: return TEXT("PerformanceWarning");
	case EMessageSeverity::Warning: return TEXT("Warning");
	case EMessageSeverity::Info: return TEXT("Info");
	default: return TEXT("Unknown");
	}
}

TSharedPtr<FJsonObject> FUAL_MessageLogCommands::SerializeMessage(const TSharedRef<FTokenizedMessage>& Message)
{
	TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
	MsgObj->SetStringField(TEXT("severity"), SeverityToString(Message->GetSeverity()));
	MsgObj->SetStringField(TEXT("text"), Message->ToText().ToString());

	// 序列化 Tokens
	TArray<TSharedPtr<FJsonValue>> TokensArray;
	for (const TSharedRef<IMessageToken>& Token : Message->GetMessageTokens())
	{
		TSharedPtr<FJsonObject> TokenObj = MakeShared<FJsonObject>();
		TokenObj->SetStringField(TEXT("text"), Token->ToText().ToString());

		// Token 类型
		switch (Token->GetType())
		{
		case EMessageToken::Text: TokenObj->SetStringField(TEXT("type"), TEXT("Text")); break;
		case EMessageToken::AssetName: TokenObj->SetStringField(TEXT("type"), TEXT("AssetName")); break;
		case EMessageToken::Actor: TokenObj->SetStringField(TEXT("type"), TEXT("Actor")); break;
		case EMessageToken::URL: TokenObj->SetStringField(TEXT("type"), TEXT("URL")); break;
		case EMessageToken::Action: TokenObj->SetStringField(TEXT("type"), TEXT("Action")); break;
		default: TokenObj->SetStringField(TEXT("type"), TEXT("Other")); break;
		}

		TokensArray.Add(MakeShared<FJsonValueObject>(TokenObj));
	}
	MsgObj->SetArrayField(TEXT("tokens"), TokensArray);

	return MsgObj;
}

// ========== messagelog.list ==========
void FUAL_MessageLogCommands::Handle_List(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FMessageLogModule& MessageLogModule = FModuleManager::LoadModuleChecked<FMessageLogModule>("MessageLog");
	
	TArray<TSharedPtr<FJsonValue>> CategoriesArray;
	
	// 常见的内置日志类别（尽可能覆盖更多）
	TArray<FName> KnownCategories = {
		// 蓝图相关
		FName("BlueprintLog"),
		FName("PIE"),
		// 地图/关卡
		FName("MapCheck"),
		FName("LightingResults"),
		FName("HLODResults"),
		// 资产
		FName("AssetCheck"),
		FName("AssetTools"),
		FName("LoadErrors"),
		// 其他
		FName("SlateStyleLog"),
		FName("SourceControl"),
		FName("PackagingResults"),
		FName("AutomationTestingLog"),
		FName("LocalizationService"),
		FName("UDNParser"),
		FName("TranslationEditor"),
		FName("AnimBlueprintLog")
	};
	
	for (const FName& CategoryName : KnownCategories)
	{
		if (MessageLogModule.IsRegisteredLogListing(CategoryName))
		{
			TSharedRef<IMessageLogListing> Listing = MessageLogModule.GetLogListing(CategoryName);
			
			TSharedPtr<FJsonObject> CategoryObj = MakeShared<FJsonObject>();
			CategoryObj->SetStringField(TEXT("name"), CategoryName.ToString());
			CategoryObj->SetStringField(TEXT("label"), Listing->GetLabel().ToString());
			
			CategoriesArray.Add(MakeShared<FJsonValueObject>(CategoryObj));
		}
	}
	
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("categories"), CategoriesArray);
	
	UE_LOG(LogUALMessageLog, Log, TEXT("messagelog.list: found %d categories"), CategoriesArray.Num());
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ========== messagelog.get ==========
void FUAL_MessageLogCommands::Handle_Get(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString CategoryStr;
	if (!Payload->TryGetStringField(TEXT("category"), CategoryStr))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: category"));
		return;
	}

	int32 Limit = 100;
	Payload->TryGetNumberField(TEXT("limit"), Limit);

	FMessageLogModule& MessageLogModule = FModuleManager::LoadModuleChecked<FMessageLogModule>("MessageLog");
	FName CategoryName(*CategoryStr);

	// 直接尝试获取（GetLogListing 会自动创建不存在的类别）
	TSharedRef<IMessageLogListing> Listing = MessageLogModule.GetLogListing(CategoryName);
	const TArray<TSharedRef<FTokenizedMessage>>& Messages = Listing->GetFilteredMessages();

	TArray<TSharedPtr<FJsonValue>> MessagesArray;
	int32 Count = FMath::Min(Messages.Num(), Limit);
	for (int32 i = 0; i < Count; ++i)
	{
		MessagesArray.Add(MakeShared<FJsonValueObject>(SerializeMessage(Messages[i])));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("category"), CategoryStr);
	Result->SetNumberField(TEXT("count"), MessagesArray.Num());
	Result->SetNumberField(TEXT("total"), Messages.Num());
	Result->SetArrayField(TEXT("messages"), MessagesArray);

	UE_LOG(LogUALMessageLog, Log, TEXT("messagelog.get: %s returned %d/%d messages"), *CategoryStr, MessagesArray.Num(), Messages.Num());
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}
