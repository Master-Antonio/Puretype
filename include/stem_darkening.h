// =============================================================================
//  stem_darkening.h — FreeType/Adobe-style Coverage-Domain Stem Darkening
//
//  Thin stems and strokes become visually underweight when ClearType RGB
//  subpixel rendering is replaced with grayscale-like OLED filtering.
//  Stem darkening compensates by boosting coverage at low values (thin
//  strokes) while leaving high coverage (solid fills) mostly untouched.
//
//  The darkening amount is inversely proportional to font em-size:
//  small fonts need more darkening, large display fonts need almost none.
//
//  Algorithm (operates in linear coverage space):
//    darkenAmount = lerp(maxDarken, minDarken, t)
//    where t = clamp((emSize - emMin) / (emMax - emMin), 0, 1)
//    boostedCoverage = 1 - (1 - coverage)^(1 + darkenAmount * strength)
// =============================================================================
#pragma once

#include <cmath>
#include <algorithm>

namespace puretype {

struct StemDarkeningParams {
    // Font em-size range over which darkening interpolates
    float emSizeMin   = 8.0f;    // At or below this size: maximum darkening
    float emSizeMax   = 40.0f;   // At or above this size: minimum darkening

    // Darkening amounts (power-curve exponent boost)
    float darkenMax   = 0.50f;   // For tiny fonts (≤ emSizeMin)
    float darkenMin   = 0.05f;   // For large fonts (≥ emSizeMax)
};

// Compute the power-curve exponent boost for a given em-size.
inline float computeDarkenAmount(float emSize, float strength,
                                  const StemDarkeningParams& params = {}) {
    if (strength <= 0.0f) return 0.0f;

    float t = std::clamp(
        (emSize - params.emSizeMin) / (params.emSizeMax - params.emSizeMin),
        0.0f, 1.0f);

    // Linear interpolation: small size → darkenMax, large size → darkenMin
    float amount = params.darkenMax + t * (params.darkenMin - params.darkenMax);

    return amount * strength;
}

// Apply stem darkening to a single linear coverage value.
// darkenAmount must be precomputed via computeDarkenAmount().
inline float applyStemDarkening(float coverage, float darkenAmount) {
    if (darkenAmount <= 0.0f || coverage <= 0.0f) return coverage;
    coverage = std::clamp(coverage, 0.0f, 1.0f);

    // Power curve: boost low coverage, preserve high coverage
    // boosted = 1 - (1 - c)^(1 + darkenAmount)
    float exponent = 1.0f + darkenAmount;
    return 1.0f - std::pow(1.0f - coverage, exponent);
}

} // namespace puretype
