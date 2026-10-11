#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FUAL_AnimationCommands
{
public:
    static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& Commands);
};
