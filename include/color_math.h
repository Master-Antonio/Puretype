

#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

namespace puretype {

inline float    g_sRGBToLinearLUT[256]  = {};
inline uint8_t  g_linearToSRGBLUT[4097] = {}; // 12-bit domain [0..4096], includes exact 1.0 endpoint.

inline float sRGBToLinearExact(float s) {
    if (s <= 0.04045f) return s / 12.92f;
    return std::pow((s + 0.055f) / 1.055f, 2.4f);
}

inline float linearToSRGBExact(float l) {
    if (l <= 0.0031308f) return 12.92f * l;
    return 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

inline float sRGBToLinear(uint8_t v) {
    return g_sRGBToLinearLUT[v];
}

inline uint8_t linearToSRGB(float linear) {
    linear = std::clamp(linear, 0.0f, 1.0f);

    // Match LUT resolution used at init time.
    int idx = static_cast<int>(linear * 4096.0f + 0.5f);
    idx = std::clamp(idx, 0, 4096);
    return g_linearToSRGBLUT[idx];
}

inline void initColorMathLUTs() {

    for (int i = 0; i < 256; ++i) {
        float s = static_cast<float>(i) / 255.0f;
        g_sRGBToLinearLUT[i] = sRGBToLinearExact(s);
    }

    for (int i = 0; i <= 4096; ++i) {
        float linear = static_cast<float>(i) / 4096.0f;
        float srgb = linearToSRGBExact(linear);
        g_linearToSRGBLUT[i] = static_cast<uint8_t>(
            std::clamp(srgb * 255.0f + 0.5f, 0.0f, 255.0f));
    }
}

}
