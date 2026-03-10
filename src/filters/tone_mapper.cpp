#include "filters/subpixel_filter.h"
#include "filters/tone_mapper.h"
#include "color_math.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace puretype {

void ToneMapper::Apply(RGBABitmap& bitmap, const ConfigData& cfg) {
    if (bitmap.data.empty() || bitmap.width <= 0 || bitmap.height <= 0) return;

    const bool qdPanel = (cfg.panelType == PanelType::QD_OLED_TRIANGLE);
    const bool tinyText = (bitmap.height <= 18);
    const bool smallText = (bitmap.height <= 24);
    const float sizeBoost = std::clamp((24.0f - static_cast<float>(bitmap.height)) / 24.0f, 0.0f, 1.0f);
    
    // Scale contrast based on the configured perceptual luma contrast strength.
    const float contrastStrength = cfg.lumaContrastStrength;

    for (int row = 0; row < bitmap.height; ++row) {
        uint8_t* rowData = bitmap.data.data() + row * bitmap.pitch;

        for (int col = 0; col < bitmap.width; ++col) {
            uint8_t* px = rowData + col * 4;

            // Il filtro spaziale ci ha dato questi valori in spazio lineare (sRGBToLinear)
            float covB = sRGBToLinear(px[0]);
            float covG = sRGBToLinear(px[1]);
            float covR = sRGBToLinear(px[2]);

            if (covR <= 0.001f && covG <= 0.001f && covB <= 0.001f) continue;

            // --- 1. CHROMA CRUSHING (Uccide le sbavature di colore) ---
            float maxCov = std::max({covR, covG, covB});
            float avgCov = (covR + covG + covB) / 3.0f;
            float yCov = 0.72f * maxCov + 0.28f * avgCov;

            float chromaKeep = 0.24f;
            if (tinyText) {
                chromaKeep = qdPanel ? 0.30f : 0.35f;
            } else if (smallText) {
                chromaKeep = qdPanel ? 0.35f : 0.40f;
            } else if (bitmap.height <= 32) {
                chromaKeep = qdPanel ? 0.45f : 0.50f;
            } else {
                chromaKeep = qdPanel ? 0.18f : 0.24f;
            }

            covR = yCov + (covR - yCov) * chromaKeep;
            covG = yCov + (covG - yCov) * chromaKeep;
            covB = yCov + (covB - yCov) * chromaKeep;

            // --- 2. READABILITY TONE (Risolve l'effetto trasparente / pompa il testo) ---
            auto applyReadability = [&](float c) -> float {
                c = std::clamp(c, 0.0f, 1.0f);
                
                // Apply a power-law curve to boost low-coverage areas, scaled by contrast strength.
                float expBase = (qdPanel ? 1.01f : 1.03f) * (1.0f + (contrastStrength - 1.0f) * 0.5f);
                float expSize = qdPanel ? 0.10f : 0.16f;
                float gainBase = qdPanel ? 1.000f : 1.004f;
                float gainSize = qdPanel ? 0.008f : 0.012f;
                
                c = 1.0f - std::pow(1.0f - c, expBase + expSize * sizeBoost);
                if (c > 0.20f) {
                    c = std::min(1.0f, c * (gainBase + gainSize * sizeBoost));
                }
                return std::clamp(c, 0.0f, 1.0f);
            };

            covR = applyReadability(covR);
            covG = applyReadability(covG);
            covB = applyReadability(covB);

            // --- 3. FINALIZZAZIONE E ALPHA MASK ---
            float alphaMask = std::max({covR, covG, covB});

            px[0] = linearToSRGB(covB);
            px[1] = linearToSRGB(covG);
            px[2] = linearToSRGB(covR);
            px[3] = static_cast<uint8_t>(alphaMask * 255.0f + 0.5f);
        }
    }
}

} // namespace puretype