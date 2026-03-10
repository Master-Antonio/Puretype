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

    inline float computeDarkenAmount(float emSize, float strength,
                                     const StemDarkeningParams& params = {})
    {
        if (strength <= 0.0f) return 0.0f;

        float t = std::clamp(
            (emSize - params.emSizeMin) / (params.emSizeMax - params.emSizeMin),
            0.0f, 1.0f);

        float amount = params.darkenMax + t * (params.darkenMin - params.darkenMax);

        return amount * strength;
    }

    inline float applyStemDarkening(float coverage, float darkenAmount)
    {
        if (darkenAmount <= 0.0f || coverage <= 0.0f) return coverage;
        coverage = std::clamp(coverage, 0.0f, 1.0f);

        // Boost low coverages more than high coverages to preserve filled areas.
        float exponent = 1.0f + darkenAmount;
        return 1.0f - std::pow(1.0f - coverage, exponent);
    }
}