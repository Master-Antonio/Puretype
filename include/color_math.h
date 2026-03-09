#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <mutex>

namespace puretype
{
    
    inline float g_sRGBToLinearLUT[256] = {};
    inline uint8_t g_linearToSRGBLUT[4097] = {}; 

    
    
    
    
    
    
    inline float g_OLEDToLinearLUT[256] = {};
    inline uint8_t g_linearToOLEDLUT[4097] = {};

    
    
    
    inline thread_local float* g_activeToLinearLUT = g_sRGBToLinearLUT;
    inline thread_local uint8_t* g_activeToGammaLUT = g_linearToSRGBLUT;
    inline std::once_flag g_colorMathLutInitOnce;

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

    
    inline float OLEDToLinearExact(float s)
    {
        constexpr float kLow = 0.18f; 
        constexpr float kHigh = 0.25f; 

        float linOLED = std::pow(s, 2.0f); 
        float linSRGB = sRGBToLinearExact(s);

        if (s <= kLow) return linOLED;
        if (s >= kHigh) return linSRGB;

        
        float t = (s - kLow) / (kHigh - kLow);
        t = t * t * (3.0f - 2.0f * t); 
        return linOLED * (1.0f - t) + linSRGB * t;
    }

    inline float linearToOLEDExact(float l)
    {
        
        float lo = 0.0f, hi = 1.0f;
        for (int i = 0; i < 24; ++i) 
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

        
        int idx = static_cast<int>(linear * 4096.0f + 0.5f);
        idx = std::clamp(idx, 0, 4096);
        return g_activeToGammaLUT[idx];
    }

    inline void setColorMathGammaMode(bool useOLEDGamma)
    {
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

    inline void initColorMathLUTs(bool useOLEDGamma = false)
    {
        std::call_once(g_colorMathLutInitOnce, []()
        {
            
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
        });

        setColorMathGammaMode(useOLEDGamma);
    }
}
