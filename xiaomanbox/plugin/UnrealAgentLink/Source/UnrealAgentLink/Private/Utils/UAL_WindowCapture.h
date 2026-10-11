#pragma once

#include "CoreMinimal.h"

namespace UAL_WindowCapture
{
    /** Both capture backends provide BGRA; PNG output must have opaque window pixels. */
    inline bool PreparePixels(TArray<FColor>& Pixels, const FIntPoint& Size)
    {
        if (Size.X <= 0 || Size.Y <= 0 || int64(Size.X) * Size.Y != Pixels.Num())
        {
            return false;
        }
        for (FColor& Pixel : Pixels)
        {
            Pixel.A = 255;
        }
        return true;
    }
}
