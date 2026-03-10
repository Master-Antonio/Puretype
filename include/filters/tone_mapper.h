#pragma once

#include "rasterizer/ft_rasterizer.h"
#include "config.h"

namespace puretype {

class ToneMapper {
public:
    // Applica le correzioni percettive umane (Chroma Crushing, Readability Tone, Sharpening)
    // Direttamente sulla bitmap generata dai filtri spaziali.
    static void Apply(RGBABitmap& bitmap, const ConfigData& cfg);
};

} // namespace puretype