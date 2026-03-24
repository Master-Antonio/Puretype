#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

namespace puretype
{
    // --- Standard sRGB gamma LUTs ---
    inline float g_sRGBToLinearLUT[256] = {};
    inline uint8_t g_linearToSRGBLUT[4097] = {}; // 12-bit domain [0..4096], includes exact 1.0 endpoint.

    // --- OLED gamma LUTs ---
    // OLED panels have a different electroluminescent response than LCDs.
    // Below ~20% brightness the standard sRGB curve (effective gamma ≈ 2.4)
    // underestimates perceived luminance, making dark text on dark backgrounds
    // too faint. The OLED curve uses gamma ≈ 2.0 in the shadows, blending
    // smoothly into standard sRGB above 25% where the curves converge.
    inline float g_OLEDToLinearLUT[256] = {};
    inline uint8_t g_linearToOLEDLUT[4097] = {};

    // Active LUT pointers — set by initColorMathLUTs() based on GammaMode.
    // All code should use sRGBToLinear() / linearToSRGB() which read from these.
    inline float* g_activeToLinearLUT = g_sRGBToLinearLUT;
    inline uint8_t* g_activeToGammaLUT = g_linearToSRGBLUT;

    inline float sRGBToLinearExact(float s)
    {
        if (s <= 0.04045f) return s / 12.92f;
        return std::pow((s + 0.055f) / 1.055f, 2.4f);
    }

    inline float linearToSRGBExact(float l)
    {
        if (l <= 0.0031308f) return 12.92f * l;
        return 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
    }

    // OLED gamma: softer in shadows (gamma ≈ 2.0), hermite blend to sRGB above.
    inline float OLEDToLinearExact(float s)
    {
        constexpr float kLow  = 0.18f; // pure OLED gamma below this
        constexpr float kHigh = 0.25f; // pure sRGB above this

        float linOLED = std::pow(s, 2.0f); // gamma 2.0 — softer shadows
        float linSRGB = sRGBToLinearExact(s);

        if (s <= kLow)  return linOLED;
        if (s >= kHigh)  return linSRGB;

        // Smooth hermite blend in transition zone
        float t = (s - kLow) / (kHigh - kLow);
        t = t * t * (3.0f - 2.0f * t); // smoothstep
        return linOLED * (1.0f - t) + linSRGB * t;
    }

    inline float linearToOLEDExact(float l)
    {
        // Inverse of OLEDToLinearExact via bisection — only used for LUT init.
        float lo = 0.0f, hi = 1.0f;
        for (int i = 0; i < 24; ++i) // 24 iterations → ~1e-7 precision
        {
            float mid = (lo + hi) * 0.5f;
            if (OLEDToLinearExact(mid) < l) lo = mid;
            else hi = mid;
        }
        return (lo + hi) * 0.5f;
    }

    inline float sRGBToLinear(uint8_t v)
    {
        return g_activeToLinearLUT[v];
    }

    inline uint8_t linearToSRGB(float linear)
    {
        linear = std::clamp(linear, 0.0f, 1.0f);

        // Match LUT resolution used at init time.
        int idx = static_cast<int>(linear * 4096.0f + 0.5f);
        idx = std::clamp(idx, 0, 4096);
        return g_activeToGammaLUT[idx];
    }

    inline void initColorMathLUTs(bool useOLEDGamma = false)
    {
        // Always build sRGB LUTs
        for (int i = 0; i < 256; ++i)
        {
            float s = static_cast<float>(i) / 255.0f;
            g_sRGBToLinearLUT[i] = sRGBToLinearExact(s);
        }

        for (int i = 0; i <= 4096; ++i)
        {
            float linear = static_cast<float>(i) / 4096.0f;
            float srgb = linearToSRGBExact(linear);
            g_linearToSRGBLUT[i] = static_cast<uint8_t>(
                std::clamp(srgb * 255.0f + 0.5f, 0.0f, 255.0f));
        }

        // Build OLED LUTs
        for (int i = 0; i < 256; ++i)
        {
            float s = static_cast<float>(i) / 255.0f;
            g_OLEDToLinearLUT[i] = OLEDToLinearExact(s);
        }

        for (int i = 0; i <= 4096; ++i)
        {
            float linear = static_cast<float>(i) / 4096.0f;
            float oled = linearToOLEDExact(linear);
            g_linearToOLEDLUT[i] = static_cast<uint8_t>(
                std::clamp(oled * 255.0f + 0.5f, 0.0f, 255.0f));
        }

        // Set active pointers
        if (useOLEDGamma)
        {
            g_activeToLinearLUT = g_OLEDToLinearLUT;
            g_activeToGammaLUT = g_linearToOLEDLUT;
        }
        else
        {
            g_activeToLinearLUT = g_sRGBToLinearLUT;
            g_activeToGammaLUT = g_linearToSRGBLUT;
        }
    }
}