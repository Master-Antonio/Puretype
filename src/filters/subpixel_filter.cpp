

#include "filters/subpixel_filter.h"
#include "rasterizer/ft_rasterizer.h"
#include "color_math.h"
#include "stem_darkening.h"
#include "config.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace puretype {

std::unique_ptr<SubpixelFilter> SubpixelFilter::Create(int panelType) {
    if (panelType == static_cast<int>(PanelType::QD_OLED_TRIANGLE)) {
        return std::make_unique<TriangularFilter>();
    }
    return std::make_unique<WRGBFilter>();
}

RGBABitmap WRGBFilter::Apply(const GlyphBitmap& glyph,
                             const ConfigData& cfg) const {
    // FreeType LCD bitmaps store triplets (R/G/B) horizontally.
    const int pixelWidth = glyph.width / 3;
    const int height = glyph.height;

    RGBABitmap result;
    result.width = pixelWidth;
    result.height = height;
    result.pitch = pixelWidth * 4;
    result.data.resize(result.pitch * height, 0);

    if (pixelWidth <= 0 || height <= 0) return result;

    constexpr float kR[5] = {0.00f, 0.03f, 0.44f, 0.09f, 0.03f};
    constexpr float kG[5] = {0.00f, 0.18f, 0.64f, 0.18f, 0.00f};
    constexpr float kB[5] = {0.03f, 0.09f, 0.44f, 0.03f, 0.00f};

    constexpr float kRsum = 0.59f; // Normalize back to unit energy after filtering.
    constexpr float kBsum = 0.59f;

    constexpr float kWhiteBase = 0.22f; // Base WRGB white-subpixel luminance contribution.

    const float chromaStrength = std::clamp(cfg.filterStrength, 0.0f, 1.0f);

    const float emSize = static_cast<float>(height);
    const float darkenAmount = cfg.stemDarkeningEnabled
        ? computeDarkenAmount(emSize, cfg.stemDarkeningStrength)
        : 0.0f;

    std::vector<float> rawR(pixelWidth * height);
    std::vector<float> rawG(pixelWidth * height);
    std::vector<float> rawB(pixelWidth * height);

    for (int y = 0; y < height; ++y) {
        const uint8_t* row = glyph.data.data() + y * glyph.pitch;
        for (int px = 0; px < pixelWidth; ++px) {
            int sx = px * 3;
            int off = y * pixelWidth + px;

            rawR[off] = sRGBToLinear(row[sx + 0]);
            rawG[off] = sRGBToLinear(row[sx + 1]);
            rawB[off] = sRGBToLinear(row[sx + 2]);
        }
    }

    const float gammaPow = cfg.gamma;
    const float invGammaPow = (gammaPow > 0.01f) ? (1.0f / gammaPow) : 1.0f;

    for (int y = 0; y < height; ++y) {
        for (int px = 0; px < pixelWidth; ++px) {

            float filtR = 0.0f, filtG = 0.0f, filtB = 0.0f;

            for (int t = -2; t <= 2; ++t) {
                int nx = std::clamp(px + t, 0, pixelWidth - 1);
                int srcOff = y * pixelWidth + nx;
                int ki = t + 2;

                filtR += rawR[srcOff] * kR[ki];
                filtG += rawG[srcOff] * kG[ki];
                filtB += rawB[srcOff] * kB[ki];
            }

            filtR /= kRsum;
            filtB /= kBsum;

            int off = y * pixelWidth + px;
            int leftX = std::max(px - 1, 0);
            int rightX = std::min(px + 1, pixelWidth - 1);
            int leftOff = y * pixelWidth + leftX;
            int rightOff = y * pixelWidth + rightX;

            float yCenter = 0.2126f * rawR[off] + 0.7152f * rawG[off] + 0.0722f * rawB[off];
            float yLeft   = 0.2126f * rawR[leftOff] + 0.7152f * rawG[leftOff] + 0.0722f * rawB[leftOff];
            float yRight  = 0.2126f * rawR[rightOff] + 0.7152f * rawG[rightOff] + 0.0722f * rawB[rightOff];

            float gradient = std::abs(yLeft - yRight);
            float edgeFactor = 1.0f - std::clamp(gradient * 2.0f, 0.0f, 0.70f);

            float wEnergy = yCenter * kWhiteBase * edgeFactor;

            filtR = filtR * (1.0f - wEnergy * 0.5f) + wEnergy * 0.5f;
            filtG = filtG * (1.0f - wEnergy * 0.3f) + wEnergy * 0.3f;
            filtB = filtB * (1.0f - wEnergy * 0.5f) + wEnergy * 0.5f;

            float rawY = (filtR + filtG + filtB) / 3.0f;

            float finalR = rawY + (filtR - rawY) * chromaStrength;
            float finalG = rawY + (filtG - rawY) * chromaStrength;
            float finalB = rawY + (filtB - rawY) * chromaStrength;

            float yLeft2  = 0.2126f * rawR[leftOff] + 0.7152f * rawG[leftOff] + 0.0722f * rawB[leftOff];
            float yRight2 = 0.2126f * rawR[rightOff] + 0.7152f * rawG[rightOff] + 0.0722f * rawB[rightOff];
            float edgeY = yCenter - 0.5f * (yLeft2 + yRight2);
            float sharpenGain = 0.30f + 0.20f * (1.0f - chromaStrength);
            float ySharp = std::clamp(yCenter + edgeY * sharpenGain, 0.0f, 1.0f);

            float outY = (finalR + finalG + finalB) / 3.0f;
            if (outY > 1e-6f) {
                float scale = std::clamp(ySharp / outY, 0.0f, 2.5f);
                finalR *= scale;
                finalG *= scale;
                finalB *= scale;
            } else {
                finalR = ySharp;
                finalG = ySharp;
                finalB = ySharp;
            }

            finalR = applyStemDarkening(std::clamp(finalR, 0.0f, 1.0f), darkenAmount);
            finalG = applyStemDarkening(std::clamp(finalG, 0.0f, 1.0f), darkenAmount);
            finalB = applyStemDarkening(std::clamp(finalB, 0.0f, 1.0f), darkenAmount);

            if (std::abs(gammaPow - 1.0f) > 0.01f) {
                finalR = std::pow(finalR, invGammaPow);
                finalG = std::pow(finalG, invGammaPow);
                finalB = std::pow(finalB, invGammaPow);
            }

            finalR = std::clamp(finalR, 0.0f, 1.0f);
            finalG = std::clamp(finalG, 0.0f, 1.0f);
            finalB = std::clamp(finalB, 0.0f, 1.0f);
            float alpha = std::max({finalR, finalG, finalB});

            uint8_t* out = result.data.data() + y * result.pitch + px * 4;
            out[0] = linearToSRGB(finalB);
            out[1] = linearToSRGB(finalG);
            out[2] = linearToSRGB(finalR);
            out[3] = static_cast<uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }

    return result;
}

RGBABitmap TriangularFilter::Apply(const GlyphBitmap& glyph,
                                   const ConfigData& cfg) const {
    // FreeType LCD bitmaps store triplets (R/G/B) horizontally.
    const int pixelWidth = glyph.width / 3;
    const int height = glyph.height;

    RGBABitmap result;
    result.width = pixelWidth;
    result.height = height;
    result.pitch = pixelWidth * 4;
    result.data.resize(result.pitch * height, 0);

    if (pixelWidth <= 0 || height <= 0) return result;

    constexpr float kR[3][3] = {
        {0.01f, 0.06f, 0.01f},
        {0.06f, 0.60f, 0.06f},
        {0.01f, 0.06f, 0.01f},
    };
    constexpr float kG[3][3] = {
        {0.04f, 0.12f, 0.04f},
        {0.12f, 0.28f, 0.12f},
        {0.04f, 0.12f, 0.04f},
    };
    constexpr float kB[3][3] = {
        {0.01f, 0.06f, 0.01f},
        {0.06f, 0.60f, 0.06f},
        {0.01f, 0.06f, 0.01f},
    };

    constexpr float kRsum = 0.88f;
    constexpr float kBsum = 0.88f;

    const float chromaStrength = std::clamp(cfg.filterStrength * 0.90f, 0.0f, 1.0f);

    const float emSize = static_cast<float>(height);
    const float darkenAmount = cfg.stemDarkeningEnabled
        ? computeDarkenAmount(emSize, cfg.stemDarkeningStrength)
        : 0.0f;

    auto sampleLinear = [&](int sx, int sy) -> float {
        if (sx < 0 || sx >= glyph.width || sy < 0 || sy >= height) return 0.0f;
        return sRGBToLinear(glyph.data[sy * glyph.pitch + sx]);
    };

    auto sampleBilinearY = [&](int sx, float fy) -> float {
        int y0 = static_cast<int>(std::floor(fy));
        int y1 = y0 + 1;
        float t = fy - static_cast<float>(y0);
        float v0 = sampleLinear(sx, y0);
        float v1 = sampleLinear(sx, y1);
        return v0 * (1.0f - t) + v1 * t;
    };

    std::vector<float> rawR(pixelWidth * height);
    std::vector<float> rawG(pixelWidth * height);
    std::vector<float> rawB(pixelWidth * height);

    for (int y = 0; y < height; ++y) {
        for (int px = 0; px < pixelWidth; ++px) {
            int sx = px * 3;
            int off = y * pixelWidth + px;

            rawR[off] = sampleLinear(sx + 0, y);

            // QD-OLED green is vertically offset from red/blue; sample at y+0.33.
            rawG[off] = sampleBilinearY(sx + 1, static_cast<float>(y) + 0.33f);
            rawB[off] = sampleLinear(sx + 2, y);
        }
    }

    const float gammaPow = cfg.gamma;
    const float invGammaPow = (gammaPow > 0.01f) ? (1.0f / gammaPow) : 1.0f;

    for (int y = 0; y < height; ++y) {
        for (int px = 0; px < pixelWidth; ++px) {

            float filtR = 0.0f, filtG = 0.0f, filtB = 0.0f;

            for (int ky = -1; ky <= 1; ++ky) {
                for (int kx = -1; kx <= 1; ++kx) {
                    int ny = std::clamp(y + ky, 0, height - 1);
                    int nx = std::clamp(px + kx, 0, pixelWidth - 1);
                    int srcOff = ny * pixelWidth + nx;

                    filtR += rawR[srcOff] * kR[ky + 1][kx + 1];
                    filtG += rawG[srcOff] * kG[ky + 1][kx + 1];
                    filtB += rawB[srcOff] * kB[ky + 1][kx + 1];
                }
            }

            filtR /= kRsum;
            filtB /= kBsum;

            int off = y * pixelWidth + px;
            float rawY = 0.2126f * rawR[off] + 0.7152f * rawG[off] + 0.0722f * rawB[off];
            float filtY = (filtR + filtG + filtB) / 3.0f;

            float finalR = rawY + (filtR - filtY) * chromaStrength;
            float finalG = rawY + (filtG - filtY) * (chromaStrength * 0.85f);
            float finalB = rawY + (filtB - filtY) * chromaStrength;

            int leftX = std::max(px - 1, 0);
            int rightX = std::min(px + 1, pixelWidth - 1);
            int upY = std::max(y - 1, 0);
            int downY = std::min(y + 1, height - 1);

            auto lumAt = [&](int ax, int ay) -> float {
                int o = ay * pixelWidth + ax;
                return 0.2126f * rawR[o] + 0.7152f * rawG[o] + 0.0722f * rawB[o];
            };

            float yNeighbors = 0.25f * (lumAt(leftX, y) + lumAt(rightX, y)
                                       + lumAt(px, upY) + lumAt(px, downY));
            float edgeY = rawY - yNeighbors;
            float sharpenGain = 0.28f + 0.22f * (1.0f - chromaStrength);
            float ySharp = std::clamp(rawY + edgeY * sharpenGain, 0.0f, 1.0f);

            float outY = (finalR + finalG + finalB) / 3.0f;
            if (outY > 1e-6f) {
                float scale = std::clamp(ySharp / outY, 0.0f, 2.5f);
                finalR *= scale;
                finalG *= scale;
                finalB *= scale;
            } else {
                finalR = ySharp;
                finalG = ySharp;
                finalB = ySharp;
            }

            finalR = applyStemDarkening(std::clamp(finalR, 0.0f, 1.0f), darkenAmount);
            finalG = applyStemDarkening(std::clamp(finalG, 0.0f, 1.0f), darkenAmount);
            finalB = applyStemDarkening(std::clamp(finalB, 0.0f, 1.0f), darkenAmount);

            if (std::abs(gammaPow - 1.0f) > 0.01f) {
                finalR = std::pow(finalR, invGammaPow);
                finalG = std::pow(finalG, invGammaPow);
                finalB = std::pow(finalB, invGammaPow);
            }

            finalR = std::clamp(finalR, 0.0f, 1.0f);
            finalG = std::clamp(finalG, 0.0f, 1.0f);
            finalB = std::clamp(finalB, 0.0f, 1.0f);
            float alpha = std::max({finalR, finalG, finalB});

            uint8_t* out = result.data.data() + y * result.pitch + px * 4;
            out[0] = linearToSRGB(finalB);
            out[1] = linearToSRGB(finalG);
            out[2] = linearToSRGB(finalR);
            out[3] = static_cast<uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }

    return result;
}

}
