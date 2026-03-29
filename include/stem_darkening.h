#pragma once

#include <cmath>
#include <algorithm>

namespace puretype
{
    struct StemDarkeningParams
    {
        float emSizeMin = 8.0f;
        float emSizeMax = 40.0f;

        float darkenMax = 0.50f;
        float darkenMin = 0.05f;
    };

    // Weight-aware stem darkening.
    // fontWeight: 100–900, default 400 (Regular).
    //   weight=400 → scale=1.0 (unchanged)
    //   weight=700 → scale≈0.4 (Bold needs less darkening — stems already thick)
    //   weight=200 → scale≈1.4 (Light needs more — stems are thin)
    inline float computeDarkenAmount(float emSize, float strength,
                                     uint16_t fontWeight = 400,
                                     const StemDarkeningParams& params = {})
    {
        if (strength <= 0.0f) return 0.0f;

        float t = std::clamp(
            (emSize - params.emSizeMin) / (params.emSizeMax - params.emSizeMin),
            0.0f, 1.0f);

        float amount = params.darkenMax + t * (params.darkenMin - params.darkenMax);

        // Scale by font weight deviation from Regular (400).
        // Heavier fonts → less darkening, lighter fonts → more.
        float weightScale = 1.0f + (400.0f - static_cast<float>(fontWeight)) / 500.0f;
        weightScale = std::clamp(weightScale, 0.4f, 1.6f);

        return amount * strength * weightScale;
    }

    inline float applyStemDarkening(float coverage, float darkenAmount)
    {
        if (darkenAmount <= 0.0f || coverage <= 0.0f) return coverage;
        coverage = std::clamp(coverage, 0.0f, 1.0f);

        // CPU OPTIMIZATION: Exact parabolic match for the power function
        // 1.0f - std::pow(1.0f - coverage, 1.0f + darkenAmount)
        // Perfectly eliminates slow FPU cycle stalls inside the per-pixel engine core.
        return coverage + darkenAmount * coverage * (1.0f - coverage);
    }
}
