#include "filters/subpixel_filter.h"
#include "filters/tone_mapper.h"
#include "color_math.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace puretype
{
    void ToneMapper::Apply(RGBABitmap& bitmap, const ConfigData& cfg)
    {
        if (bitmap.data.empty() || bitmap.width <= 0 || bitmap.height <= 0) return;

        const bool qdPanel = (cfg.panelType == PanelType::QD_OLED_GEN1 ||
            cfg.panelType == PanelType::QD_OLED_GEN3 ||
            cfg.panelType == PanelType::QD_OLED_GEN4);
        const bool tinyText = (bitmap.height <= 18);
        const bool smallText = (bitmap.height <= 24);
        const float sizeBoost = std::clamp((24.0f - static_cast<float>(bitmap.height)) / 24.0f, 0.0f, 1.0f);

        const float contrastStrength = cfg.lumaContrastStrength;

        // --- CPU OPTIMIZATION: LUT for S-curve ---
        const float expBase = (qdPanel ? 1.01f : 1.03f) * (1.0f + (contrastStrength - 1.0f) * 0.5f);
        const float expSize = qdPanel ? 0.10f : 0.16f;
        const float gainBase = qdPanel ? 1.000f : 1.004f;
        const float gainSize = qdPanel ? 0.008f : 0.012f;

        const float finalExp = expBase + expSize * sizeBoost;
        const float finalGain = gainBase + gainSize * sizeBoost;

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
            readabilityLUT[i] = std::clamp(c, 0.0f, 1.0f);
        }

        auto applyReadabilityFast = [&](float c) -> float
        {
            const int idx = static_cast<int>(std::clamp(c, 0.0f, 1.0f) * (LUT_SIZE - 1) + 0.5f);
            return readabilityLUT[idx];
        };


        // chroma should be preserved on OLED panels.
        //
        // Root cause of the original bug: the values (0.18–0.35) were ported
        // from an LCD ClearType tone-mapper, where chroma suppression is
        // desirable — LCD subpixel rendering creates colour fringing that
        // looks like an artifact on LCD screens. On OLED the situation is
        // reversed: per-channel subpixel values ARE the rendering mechanism.
        // Suppressing chroma to 18–35% of its original value means discarding
        // 65–82% of the subpixel information computed by the filter — making
        // the output nearly greyscale and visually identical to plain GDI text.
        //
        // Correct approach for OLED:
        //   - Preserve as much chroma as possible (0.70–0.85).
        //   - Tiny text benefits from slightly less chroma because at ≤18 px
        //     the subpixel cells are close to pixel size and fringing reads as
        //     a colour defect rather than as sharpening — 0.70 is still high
        //     but gives a small margin against the worst offenders.
        //   - Large text (>32 px) should get MAXIMUM chroma preservation (0.85)
        //     because stems are wide enough that each subpixel column maps to
        //     a distinct color channel with zero overlap — full chroma is
        //     exactly correct here.
        //
        // The chromaKeep = 1.0 extreme would give raw subpixel output with no
        // luma anchoring at all; values 0.70–0.85 strike the balance between
        // colour accuracy and perceptual neutrality on mixed-colour text.
        float chromaKeep;
        if (tinyText)
        {
            // ≤18 px: slightly conservative to avoid colour defects on very
            // small glyphs where subpixel cells are near pixel-sized.
            chromaKeep = qdPanel ? 0.70f : 0.72f;
        }
        else if (smallText)
        {
            // 19–24 px: main body text range, full subpixel benefit.
            chromaKeep = qdPanel ? 0.75f : 0.77f;
        }
        else if (bitmap.height <= 32)
        {
            // 25–32 px: medium text, stems wide enough for clean subpixel.
            chromaKeep = qdPanel ? 0.80f : 0.82f;
        }
        else
        {
            // >32 px: large / heading text — preserve maximum subpixel colour.
            chromaKeep = qdPanel ? 0.83f : 0.85f;
        }

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

                // Use BT.709 luminance weights instead of the ad-hoc
                // 0.72*max + 0.28*avg formula. BT.709 correctly weights the
                // perceptual contribution of each channel for luma extraction,
                // which is critical for coloured text on OLED panels.
                float yCov = 0.2126f * covR + 0.7152f * covG + 0.0722f * covB;

                covR = yCov + (covR - yCov) * chromaKeep;
                covG = yCov + (covG - yCov) * chromaKeep;
                covB = yCov + (covB - yCov) * chromaKeep;

                covR = applyReadabilityFast(covR);
                covG = applyReadabilityFast(covG);
                covB = applyReadabilityFast(covB);

                // Alpha must be derived from the sRGB-encoded byte values,
                // not from linear-space floats. The downstream D2D compositing path
                // uses DXGI_FORMAT_B8G8R8A8_UNORM with D2D1_ALPHA_MODE_PREMULTIPLIED,
                // which requires color_byte <= alpha_byte for all channels.
                uint8_t byteB = linearToSRGB(covB);
                uint8_t byteG = linearToSRGB(covG);
                uint8_t byteR = linearToSRGB(covR);

                px[0] = byteB;
                px[1] = byteG;
                px[2] = byteR;
                px[3] = std::max({byteR, byteG, byteB});
            }
        }
    }
} // namespace puretype