#include "filters/subpixel_filter.h"
#include "filters/tone_mapper.h"
#include "color_math.h"
#include "output/tone_parity.h"
#include "render_optimizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace puretype
{
    void ToneMapper::Apply(RGBABitmap& bitmap, const ConfigData& cfg)
    {
        if (bitmap.data.empty() || bitmap.width <= 0 || bitmap.height <= 0) return;

        const bool qdPanel = (cfg.panelType == PanelType::QD_OLED_GEN1 ||
            cfg.panelType == PanelType::QD_OLED_GEN3 ||
            cfg.panelType == PanelType::QD_OLED_GEN4);
        const EdgeAdaptiveParams edgeParams = GetEdgeAdaptiveParams(qdPanel);

        const bool tinyText = (bitmap.height <= 18);
        const bool smallText = (bitmap.height <= 24);
        const float sizeBoost = std::clamp((24.0f - static_cast<float>(bitmap.height)) / 24.0f, 0.0f, 1.0f);
        const float lodFactor = ComputeLodTransitionFactor(static_cast<float>(bitmap.height),
                                                           cfg.lodThresholdSmall,
                                                           cfg.lodThresholdLarge);

        const float contrastStrength = cfg.lumaContrastStrength * (1.0f + (1.0f - lodFactor) * 0.08f);
        const float toneStrength = std::clamp(cfg.filterStrength * (0.85f + 0.15f * lodFactor), 0.0f, 5.0f);

        // S-curve parameters.
        float expBase = (qdPanel ? 1.01f : 1.03f) * (1.0f + (contrastStrength - 1.0f) * 0.5f);

        // Reduce S-curve aggression on thin fonts.
        if (cfg.stemDarkeningEnabled && bitmap.fontWeight < 400 && bitmap.fontWeight > 0)
        {
            const float weightFactor = static_cast<float>(bitmap.fontWeight) / 400.0f;
            expBase = 1.0f + (expBase - 1.0f) * weightFactor;
        }

        const float expSize = qdPanel ? 0.10f : 0.16f;
        const float gainBase = qdPanel ? 1.000f : 1.004f;
        const float gainSize = qdPanel ? 0.008f : 0.012f;

        const float lodReadabilityScale = 1.0f + (1.0f - lodFactor) * 0.10f;
        const float finalExp = (expBase + expSize * sizeBoost) * lodReadabilityScale;
        const float finalGain = (gainBase + gainSize * sizeBoost) * (1.0f + (1.0f - lodFactor) * 0.02f);

        constexpr int LUT_SIZE = 1024;
        float readabilityLUT[LUT_SIZE];
        for (int i = 0; i < LUT_SIZE; ++i)
        {
            float c = static_cast<float>(i) / (LUT_SIZE - 1);
            c = 1.0f - std::pow(1.0f - c, finalExp);
            if (c > 0.20f)
            {
                c = std::min(1.0f, c * finalGain);
            }
            readabilityLUT[i] = Clamp01(c);
        }

        auto applyReadabilityFast = [&](const float c) -> float
        {
            const int idx = static_cast<int>(Clamp01(c) * (LUT_SIZE - 1) + 0.5f);
            return readabilityLUT[idx];
        };

        // Base chroma policy by glyph size.
        // Higher values preserve more per-channel subpixel detail, improving
        // perceived sharpness at the cost of slightly more color fringing.
        // Previous values (0.70-0.83 for QD) were too aggressive and caused
        // visible loss of crispness compared to stock ClearType.
        float chromaKeepBase;
        if (tinyText)
        {
            chromaKeepBase = qdPanel ? 0.78f : 0.77f;
        }
        else if (smallText)
        {
            chromaKeepBase = qdPanel ? 0.82f : 0.80f;
        }
        else if (bitmap.height <= 32)
        {
            chromaKeepBase = qdPanel ? 0.86f : 0.84f;
        }
        else
        {
            chromaKeepBase = qdPanel ? 0.88f : 0.87f;
        }

        const float chromaFamilyScale = qdPanel ? cfg.chromaKeepScaleQD : cfg.chromaKeepScaleWOLED;
        chromaKeepBase *= std::clamp(chromaFamilyScale, 0.60f, 1.30f);
        chromaKeepBase *= 0.82f + 0.18f * lodFactor;

        // Font weight aware adjustment: thinner glyphs tolerate slightly less chroma.
        if (bitmap.fontWeight > 0)
        {
            const float weightNorm = std::clamp(static_cast<float>(bitmap.fontWeight) / 400.0f, 0.5f, 2.0f);
            chromaKeepBase *= (0.85f + 0.15f * weightNorm);
        }

        // High-DPI attenuation hint from hooks.
        const float dpiScaleHint = std::clamp(cfg.dpiScaleHint, 0.0f, 1.0f);
        if (dpiScaleHint < 1.0f)
        {
            chromaKeepBase *= 0.4f + 0.6f * dpiScaleHint;
        }

        // Contrast hint from hooks (for DWrite proxy and preview consistency).
        const float contrastHint = (cfg.textContrastHint >= 0.0f)
                                       ? std::clamp(cfg.textContrastHint, 0.0f, 1.0f)
                                       : 1.0f;
        chromaKeepBase *= 0.7f + 0.3f * contrastHint;
        chromaKeepBase = Clamp01(chromaKeepBase);

        ConstrainedChromaFastPath fastPath;
        fastPath.maxEdgeRisk = tinyText ? 0.05f : 0.08f;
        fastPath.maxChannelSpread = tinyText ? 0.05f : 0.06f;
        fastPath.maxLumaDelta = tinyText ? 0.0025f : 0.0035f;

        // Precompute luminance map from current per-channel masks.
        std::vector<float> yMap(static_cast<size_t>(bitmap.width) * bitmap.height, 0.0f);
        for (int row = 0; row < bitmap.height; ++row)
        {
            const uint8_t* rowData = bitmap.data.data() + row * bitmap.pitch;
            for (int col = 0; col < bitmap.width; ++col)
            {
                const uint8_t* px = rowData + col * 4;
                const float covB = sRGBToLinear(px[0]);
                const float covG = sRGBToLinear(px[1]);
                const float covR = sRGBToLinear(px[2]);
                yMap[static_cast<size_t>(row) * bitmap.width + col] =
                    0.2126f * covR + 0.7152f * covG + 0.0722f * covB;
            }
        }

        auto sampleY = [&](const int y, const int x) -> float
        {
            const int clampedY = std::clamp(y, 0, bitmap.height - 1);
            const int clampedX = std::clamp(x, 0, bitmap.width - 1);
            return yMap[static_cast<size_t>(clampedY) * bitmap.width + clampedX];
        };

        for (int row = 0; row < bitmap.height; ++row)
        {
            uint8_t* rowData = bitmap.data.data() + row * bitmap.pitch;

            for (int col = 0; col < bitmap.width; ++col)
            {
                uint8_t* px = rowData + col * 4;

                float covB = sRGBToLinear(px[0]);
                float covG = sRGBToLinear(px[1]);
                float covR = sRGBToLinear(px[2]);

                if (covR <= 0.001f && covG <= 0.001f && covB <= 0.001f) continue;

                const float baseCovR = covR;
                const float baseCovG = covG;
                const float baseCovB = covB;
                const float yCov = sampleY(row, col);

                // Edge/fringing risk model:
                // - vertical edges (high d/dx) are the most sensitive on stripe-based mapping,
                // - thin coverage areas are more vulnerable,
                // - large per-channel spread indicates chroma stress.
                const float yL = sampleY(row, col - 1);
                const float yR = sampleY(row, col + 1);
                const float yU = sampleY(row - 1, col);
                const float yD = sampleY(row + 1, col);

                const float gradX = std::abs(yR - yL);
                const float gradY = std::abs(yD - yU);
                const float channelSpread = std::max({covR, covG, covB}) - std::min({covR, covG, covB});
                const float thinness = Clamp01(1.0f - yCov * 2.0f);
                const float edgeRisk = ComputeEdgeRisk(gradX, gradY, channelSpread, thinness, edgeParams);

                // Edge-adaptive chroma limiting: preserve chroma except on risky edges.
                const float chromaKeep =
                    ComputeAdaptiveChromaKeep(chromaKeepBase, contrastHint, edgeRisk, edgeParams);
                covR = yCov + (covR - yCov) * chromaKeep;
                covG = yCov + (covG - yCov) * chromaKeep;
                covB = yCov + (covB - yCov) * chromaKeep;

                covR = applyReadabilityFast(covR);
                covG = applyReadabilityFast(covG);
                covB = applyReadabilityFast(covB);

                float targetY = applyReadabilityFast(yCov);

                // NOTE: oledGammaOutput is no longer applied here.
                // Coverage masks are geometric coefficients (α ∈ [0,1]),
                // not luminance values. pow(α, γ) with γ>1 crushes edge
                // coverage (0.7 → 0.49 at γ=2), destroying anti-aliasing
                // sharpness. Display gamma compensation is applied
                // post-compositing in the Blender instead.

                if (std::abs(toneStrength - 1.0f) > 0.001f)
                {
                    covR = Clamp01(baseCovR + (covR - baseCovR) * toneStrength);
                    covG = Clamp01(baseCovG + (covG - baseCovG) * toneStrength);
                    covB = Clamp01(baseCovB + (covB - baseCovB) * toneStrength);
                    targetY = Clamp01(yCov + (targetY - yCov) * toneStrength);
                }

                const std::array<float, 3> solved = ApplyConstrainedChromaOptimization(
                    {covR, covG, covB},
                    targetY,
                    edgeRisk,
                    contrastHint,
                    channelSpread,
                    edgeParams,
                    fastPath);

                const uint8_t byteR = linearToSRGB(solved[0]);
                const uint8_t byteG = linearToSRGB(solved[1]);
                const uint8_t byteB = linearToSRGB(solved[2]);

                px[0] = byteB;
                px[1] = byteG;
                px[2] = byteR;
                px[3] = std::max({byteR, byteG, byteB});
            }
        }
    }
} // namespace puretype
