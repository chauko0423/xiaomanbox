#pragma once

#include "CoreMinimal.h"
#include "Misc/Paths.h"

namespace UAL_AssetSourcePath
{
    /** PackageName is an absolute /Game package name, not a native disk path. */
    inline FString DependencyFile(const FString& BaseDirectory, const FString& PackageName)
    {
        FString Filename = BaseDirectory + PackageName + TEXT(".uasset");
        FPaths::NormalizeFilename(Filename);
        return Filename;
    }
}
