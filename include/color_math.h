// =============================================================================
//  color_math.h — IEC 61966-2-1 sRGB Transfer Functions
//
//  Provides physically accurate sRGB ↔ Linear conversion via precomputed LUTs.
//  The sRGB standard has a linear toe segment near zero that prevents dark
//  stems from vanishing ("dark text chewing"), unlike a naive pow(x, 2.2).
//
//  sRGB → Linear:
//    if sRGB ≤ 0.04045:  linear = sRGB / 12.92
//    else:                linear = ((sRGB + 0.055) / 1.055)^2.4
//
//  Linear → sRGB:
//    if linear ≤ 0.0031308:  sRGB = 12.92 × linear
//    else:                    sRGB = 1.055 × linear^(1/2.4) − 0.055
// =============================================================================
#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

namespace puretype {

// ── LUT storage ──────────────────────────────────────────────────────────────
// Forward-LUT:  256 entries, uint8_t → float linear [0..1]
// Inverse-LUT:  4097 entries, fixed-point linear index → uint8_t sRGB
inline float    g_sRGBToLinearLUT[256]  = {};
inline uint8_t  g_linearToSRGBLUT[4097] = {};

// ── Scalar math (reference, used during LUT build) ───────────────────────────

inline float sRGBToLinearExact(float s) {
    if (s <= 0.04045f) return s / 12.92f;
    return std::pow((s + 0.055f) / 1.055f, 2.4f);
}

inline float linearToSRGBExact(float l) {
    if (l <= 0.0031308f) return 12.92f * l;
    return 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

// ── Fast LUT-backed conversions ──────────────────────────────────────────────

inline float sRGBToLinear(uint8_t v) {
    return g_sRGBToLinearLUT[v];
}

inline uint8_t linearToSRGB(float linear) {
    linear = std::clamp(linear, 0.0f, 1.0f);
    // Map [0,1] → [0, 4096] and look up in the inverse LUT
    int idx = static_cast<int>(linear * 4096.0f + 0.5f);
    idx = std::clamp(idx, 0, 4096);
    return g_linearToSRGBLUT[idx];
}

// ── One-time initialization (call from DllMain) ──────────────────────────────

inline void initColorMathLUTs() {
    // Forward LUT: sRGB byte → linear float
    for (int i = 0; i < 256; ++i) {
        float s = static_cast<float>(i) / 255.0f;
        g_sRGBToLinearLUT[i] = sRGBToLinearExact(s);
    }

    // Inverse LUT: fixed-point linear → sRGB byte
    for (int i = 0; i <= 4096; ++i) {
        float linear = static_cast<float>(i) / 4096.0f;
        float srgb = linearToSRGBExact(linear);
        g_linearToSRGBLUT[i] = static_cast<uint8_t>(
            std::clamp(srgb * 255.0f + 0.5f, 0.0f, 255.0f));
    }
}

} // namespace puretype
