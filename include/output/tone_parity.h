#pragma once

#include <algorithm>
#include <cmath>

namespace puretype
{
    inline float ComputeLodTransitionFactor(const float pixelSize,
                                            const float lodThresholdSmall,
                                            const float lodThresholdLarge)
    {
        const float clampedSmallThreshold = std::clamp(lodThresholdSmall, 6.0f, 160.0f);
        const float clampedLargeThreshold = std::clamp(lodThresholdLarge, clampedSmallThreshold + 1.0f, 160.0f);
        const float size = std::max(0.0f, pixelSize);

        if (size <= clampedSmallThreshold) return 0.0f;
        if (size >= clampedLargeThreshold) return 1.0f;

        const float t = (size - clampedSmallThreshold) / (clampedLargeThreshold - clampedSmallThreshold);
        return t * t * (3.0f - 2.0f * t);
    }

    inline float ComputeToneParityPostGamma(const float gamma, const float oledGammaOutput)
    {
        const float clampedGamma = std::clamp(gamma, 0.5f, 3.0f);
        const float clampedOledGamma = std::clamp(oledGammaOutput, 1.0f, 2.0f);
        return std::clamp(clampedGamma * clampedOledGamma, 0.5f, 4.0f);
    }

    inline void ApplyToneParityPostComposite(float& outR,
                                             float& outG,
                                             float& outB,
                                             const float bgR,
                                             const float bgG,
                                             const float bgB,
                                             const float coverageMax,
                                             const float postGamma)
    {
        const float clampedCoverage = std::clamp(coverageMax, 0.0f, 1.0f);
        if (clampedCoverage <= 0.01f || postGamma <= 1.001f)
        {
            return;
        }

        const float factor = std::pow(clampedCoverage, postGamma - 1.0f);
        outR = std::clamp(bgR + (outR - bgR) * factor, 0.0f, 1.0f);
        outG = std::clamp(bgG + (outG - bgG) * factor, 0.0f, 1.0f);
        outB = std::clamp(bgB + (outB - bgB) * factor, 0.0f, 1.0f);
    }
} // namespace puretype
