#pragma once

#include "rasterizer/ft_rasterizer.h"
#include "config.h"

namespace puretype
{
    class ToneMapper
    {
    public:
        
        
        static void Apply(RGBABitmap& bitmap, const ConfigData& cfg);
    };
} 
