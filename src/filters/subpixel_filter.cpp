// =============================================================================
//  subpixel_filter.cpp — HVS-Optimized Panel-Specific Convolution Kernels
//
//  This file implements scientifically accurate subpixel filtering for two
//  OLED panel geometries:
//
//  1. WRGBFilter  — LG WOLED [W-R-G-B] stripe layout
//     • Horizontal 1×5 FIR filter per channel, HVS-weighted
//     • Dynamic White subpixel injection for luminance boost
//     • Green channel (peak HVS sensitivity at 555nm) gets widest spread
//
//  2. TriangularFilter — Samsung QD-OLED triangular layout
//     • 2D 3×3 kernel with HVS-weighted channel separation
//     • Bilinear Y-offset sampling for physically offset Green subpixels
//     • Tighter R/B spread to prevent color fringing on triangle grid
//
//  Both filters operate entirely in linear light space (via sRGB LUT),
//  integrate stem darkening for typographic weight restoration, and output
//  BGRA with per-channel coverage encoded for the blender to composite.
// =============================================================================

#include "filters/subpixel_filter.h"
#include "rasterizer/ft_rasterizer.h"
#include "color_math.h"
#include "stem_darkening.h"
#include "config.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace puretype {

// ── Factory ──────────────────────────────────────────────────────────────────

std::unique_ptr<SubpixelFilter> SubpixelFilter::Create(int panelType) {
    if (panelType == static_cast<int>(PanelType::QD_OLED_TRIANGLE)) {
        return std::make_unique<TriangularFilter>();
    }
    return std::make_unique<WRGBFilter>();
}

// =============================================================================
//  WRGBFilter — LG WOLED Stripe Layout
// =============================================================================
//
//  Physical subpixel order per pixel: [White] [Red] [Green] [Blue]
//
//  The White subpixel is unique to LG: it contributes luminance but NOT
//  chrominance. We exploit this by injecting luminance energy via W at
//  flat regions (boosting brightness) and reducing W at edges (preserving
//  crispness).
//
//  Horizontal FIR kernel design (per-channel, 5-tap):
//  The Green channel (peak HVS luminance sensitivity at λ=555nm) receives
//  the WIDEST spatial spread — the eye resolves luminance at higher spatial
//  frequency than chrominance. Red and Blue are kept tight.
//
//  Kernel weights (each column sums to ≈1.0, energy-preserving):
//
//                      Red     Green    Blue
//    Pixel - 2:    [ 0.00,   0.00,   0.03 ]
//    Pixel - 1:    [ 0.03,   0.18,   0.09 ]
//    Pixel    :    [ 0.44,   0.64,   0.44 ]   ← center
//    Pixel + 1:    [ 0.09,   0.18,   0.03 ]
//    Pixel + 2:    [ 0.03,   0.00,   0.00 ]
//                  ─────   ─────   ─────
//    Column sum:    0.59    1.00    0.59    (R/B < 1 → tighter, less fringing)
//
//  The asymmetry (Pixel-1 vs Pixel+1 for R vs B) reflects the physical
//  subpixel ordering on the stripe.
//
//  White subpixel injection:
//    W_luminance = Rec.709(R, G, B)  = 0.2126R + 0.7152G + 0.0722B
//    W_energy    = W_luminance × edgeAdaptive(neighbors)
//    At edges (high gradient): W is reduced → preserves crispness
//    In flat areas:            W is boosted → increases peak luminance
// =============================================================================

RGBABitmap WRGBFilter::Apply(const GlyphBitmap& glyph,
                             const ConfigData& cfg) const {
    const int pixelWidth = glyph.width / 3;
    const int height = glyph.height;

    RGBABitmap result;
    result.width = pixelWidth;
    result.height = height;
    result.pitch = pixelWidth * 4;
    result.data.resize(result.pitch * height, 0);

    if (pixelWidth <= 0 || height <= 0) return result;

    // ── HVS-weighted per-channel horizontal FIR kernels ──────────────────
    //          tap:   -2      -1     center   +1      +2
    constexpr float kR[5] = {0.00f, 0.03f, 0.44f, 0.09f, 0.03f};  // sum≈0.59
    constexpr float kG[5] = {0.00f, 0.18f, 0.64f, 0.18f, 0.00f};  // sum=1.00
    constexpr float kB[5] = {0.03f, 0.09f, 0.44f, 0.03f, 0.00f};  // sum≈0.59

    // Normalize R and B kernels to sum=1 for energy preservation
    constexpr float kRsum = 0.59f;
    constexpr float kBsum = 0.59f;

    // White subpixel baseline contribution
    constexpr float kWhiteBase = 0.22f;

    const float chromaStrength = std::clamp(cfg.filterStrength, 0.0f, 1.0f);

    // Font em-size estimate for stem darkening (glyph height is a proxy)
    const float emSize = static_cast<float>(height);
    const float darkenAmount = cfg.stemDarkeningEnabled
        ? computeDarkenAmount(emSize, cfg.stemDarkeningStrength)
        : 0.0f;

    // ── Step 1: Convert subpixel samples to linear and extract R/G/B ─────
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

    // ── Step 2: Apply per-channel HVS FIR + White injection + stem darkening
    const float gammaPow = cfg.gamma;
    const float invGammaPow = (gammaPow > 0.01f) ? (1.0f / gammaPow) : 1.0f;

    for (int y = 0; y < height; ++y) {
        for (int px = 0; px < pixelWidth; ++px) {
            // ── Per-channel horizontal FIR convolution ───────────────────
            float filtR = 0.0f, filtG = 0.0f, filtB = 0.0f;

            for (int t = -2; t <= 2; ++t) {
                int nx = std::clamp(px + t, 0, pixelWidth - 1);
                int srcOff = y * pixelWidth + nx;
                int ki = t + 2;

                filtR += rawR[srcOff] * kR[ki];
                filtG += rawG[srcOff] * kG[ki];
                filtB += rawB[srcOff] * kB[ki];
            }

            // Normalize R and B to preserve energy
            filtR /= kRsum;
            filtB /= kBsum;

            // ── White subpixel injection (LG WRGB) ──────────────────────
            // Sample local neighborhood for edge detection
            int off = y * pixelWidth + px;
            int leftX = std::max(px - 1, 0);
            int rightX = std::min(px + 1, pixelWidth - 1);
            int leftOff = y * pixelWidth + leftX;
            int rightOff = y * pixelWidth + rightX;

            float yCenter = 0.2126f * rawR[off] + 0.7152f * rawG[off] + 0.0722f * rawB[off];
            float yLeft   = 0.2126f * rawR[leftOff] + 0.7152f * rawG[leftOff] + 0.0722f * rawB[leftOff];
            float yRight  = 0.2126f * rawR[rightOff] + 0.7152f * rawG[rightOff] + 0.0722f * rawB[rightOff];

            // Gradient magnitude → edge factor (1.0 in flat areas, reduced at edges)
            float gradient = std::abs(yLeft - yRight);
            float edgeFactor = 1.0f - std::clamp(gradient * 2.0f, 0.0f, 0.70f);

            // White contribution: luminance-weighted, edge-adaptive
            float wEnergy = yCenter * kWhiteBase * edgeFactor;

            // Blend White into all channels (W adds equal luminance)
            filtR = filtR * (1.0f - wEnergy * 0.5f) + wEnergy * 0.5f;
            filtG = filtG * (1.0f - wEnergy * 0.3f) + wEnergy * 0.3f;
            filtB = filtB * (1.0f - wEnergy * 0.5f) + wEnergy * 0.5f;

            // ── Luminance-chroma separation (HVS optimization) ──────────
            float rawY = (filtR + filtG + filtB) / 3.0f;

            // Blend toward grayscale based on chroma strength
            float finalR = rawY + (filtR - rawY) * chromaStrength;
            float finalG = rawY + (filtG - rawY) * chromaStrength;
            float finalB = rawY + (filtB - rawY) * chromaStrength;

            // ── Luminance sharpening (unsharp mask on luminance only) ────
            float yLeft2  = 0.2126f * rawR[leftOff] + 0.7152f * rawG[leftOff] + 0.0722f * rawB[leftOff];
            float yRight2 = 0.2126f * rawR[rightOff] + 0.7152f * rawG[rightOff] + 0.0722f * rawB[rightOff];
            float edgeY = yCenter - 0.5f * (yLeft2 + yRight2);
            float sharpenGain = 0.30f + 0.20f * (1.0f - chromaStrength);
            float ySharp = std::clamp(yCenter + edgeY * sharpenGain, 0.0f, 1.0f);

            // Rescale channels to match sharpened luminance
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

            // ── Stem darkening (coverage-domain) ─────────────────────────
            finalR = applyStemDarkening(std::clamp(finalR, 0.0f, 1.0f), darkenAmount);
            finalG = applyStemDarkening(std::clamp(finalG, 0.0f, 1.0f), darkenAmount);
            finalB = applyStemDarkening(std::clamp(finalB, 0.0f, 1.0f), darkenAmount);

            // ── Optional fine-tune gamma exponent ────────────────────────
            if (std::abs(gammaPow - 1.0f) > 0.01f) {
                finalR = std::pow(finalR, invGammaPow);
                finalG = std::pow(finalG, invGammaPow);
                finalB = std::pow(finalB, invGammaPow);
            }

            finalR = std::clamp(finalR, 0.0f, 1.0f);
            finalG = std::clamp(finalG, 0.0f, 1.0f);
            finalB = std::clamp(finalB, 0.0f, 1.0f);
            float alpha = std::max({finalR, finalG, finalB});

            // ── Output: convert linear coverage to sRGB-encoded BGRA ─────
            uint8_t* out = result.data.data() + y * result.pitch + px * 4;
            out[0] = linearToSRGB(finalB);
            out[1] = linearToSRGB(finalG);
            out[2] = linearToSRGB(finalR);
            out[3] = static_cast<uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }

    return result;
}

// =============================================================================
//  TriangularFilter — Samsung QD-OLED Triangular Layout
// =============================================================================
//
//  Physical subpixel layout (triangle pattern):
//
//    Row 0:   R . B . R . B . R . B
//    Row 1:   . G . G . G . G . G .
//    Row 0:   R . B . R . B . R . B
//
//  The Green subpixel is offset vertically by ~0.33 pixels relative to R/B.
//
//  2D 3×3 HVS-weighted kernel (per-channel):
//
//                      Red     Green    Blue
//    (-1,-1):      [ 0.01,   0.04,   0.01 ]
//    ( 0,-1):      [ 0.06,   0.12,   0.06 ]
//    (+1,-1):      [ 0.01,   0.04,   0.01 ]
//    (-1, 0):      [ 0.06,   0.12,   0.06 ]
//    ( 0, 0):      [ 0.60,   0.28,   0.60 ]   ← center
//    (+1, 0):      [ 0.06,   0.12,   0.06 ]
//    (-1,+1):      [ 0.01,   0.04,   0.01 ]
//    ( 0,+1):      [ 0.06,   0.12,   0.06 ]
//    (+1,+1):      [ 0.01,   0.04,   0.01 ]
//                  ─────    ─────   ─────
//    Sum:           0.88     1.00    0.88
//
//  Green center weight is LOW (0.28) → widest spatial spread, matching
//  the HVS's high luminance spatial frequency resolution.
//  R/B center weight is HIGH (0.60) → tight, minimizing color fringing
//  on the triangular grid.
//
//  Green Y-offset: bilinear interpolation at y+0.33 to compensate for
//  the physical vertical offset of G subpixels in the triangle.
// =============================================================================

RGBABitmap TriangularFilter::Apply(const GlyphBitmap& glyph,
                                   const ConfigData& cfg) const {
    const int pixelWidth = glyph.width / 3;
    const int height = glyph.height;

    RGBABitmap result;
    result.width = pixelWidth;
    result.height = height;
    result.pitch = pixelWidth * 4;
    result.data.resize(result.pitch * height, 0);

    if (pixelWidth <= 0 || height <= 0) return result;

    // ── Per-channel 3×3 kernels ──────────────────────────────────────────
    // [ky+1][kx+1] indexing
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
    // kG sums to 1.00, no normalization needed

    const float chromaStrength = std::clamp(cfg.filterStrength * 0.90f, 0.0f, 1.0f);

    // Stem darkening
    const float emSize = static_cast<float>(height);
    const float darkenAmount = cfg.stemDarkeningEnabled
        ? computeDarkenAmount(emSize, cfg.stemDarkeningStrength)
        : 0.0f;

    // ── Helper: sample a subpixel in linear space ────────────────────────
    auto sampleLinear = [&](int sx, int sy) -> float {
        if (sx < 0 || sx >= glyph.width || sy < 0 || sy >= height) return 0.0f;
        return sRGBToLinear(glyph.data[sy * glyph.pitch + sx]);
    };

    // ── Helper: bilinear Y-interpolation for Green offset ────────────────
    auto sampleBilinearY = [&](int sx, float fy) -> float {
        int y0 = static_cast<int>(std::floor(fy));
        int y1 = y0 + 1;
        float t = fy - static_cast<float>(y0);
        float v0 = sampleLinear(sx, y0);
        float v1 = sampleLinear(sx, y1);
        return v0 * (1.0f - t) + v1 * t;
    };

    // ── Step 1: Build linear-space per-channel arrays ────────────────────
    std::vector<float> rawR(pixelWidth * height);
    std::vector<float> rawG(pixelWidth * height);
    std::vector<float> rawB(pixelWidth * height);

    for (int y = 0; y < height; ++y) {
        for (int px = 0; px < pixelWidth; ++px) {
            int sx = px * 3;
            int off = y * pixelWidth + px;

            rawR[off] = sampleLinear(sx + 0, y);
            // Green is physically offset ~0.33 pixels down in the triangle
            rawG[off] = sampleBilinearY(sx + 1, static_cast<float>(y) + 0.33f);
            rawB[off] = sampleLinear(sx + 2, y);
        }
    }

    // ── Step 2: Per-channel 2D convolution + stem darkening ──────────────
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

            // Normalize R and B
            filtR /= kRsum;
            filtB /= kBsum;

            // ── Luminance-chroma separation ──────────────────────────────
            int off = y * pixelWidth + px;
            float rawY = 0.2126f * rawR[off] + 0.7152f * rawG[off] + 0.0722f * rawB[off];
            float filtY = (filtR + filtG + filtB) / 3.0f;

            float finalR = rawY + (filtR - filtY) * chromaStrength;
            float finalG = rawY + (filtG - filtY) * (chromaStrength * 0.85f);
            float finalB = rawY + (filtB - filtY) * chromaStrength;

            // ── 2D luminance sharpening (unsharp mask) ───────────────────
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

            // Rescale to match sharpened luminance
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

            // ── Stem darkening ───────────────────────────────────────────
            finalR = applyStemDarkening(std::clamp(finalR, 0.0f, 1.0f), darkenAmount);
            finalG = applyStemDarkening(std::clamp(finalG, 0.0f, 1.0f), darkenAmount);
            finalB = applyStemDarkening(std::clamp(finalB, 0.0f, 1.0f), darkenAmount);

            // ── Optional fine-tune gamma ─────────────────────────────────
            if (std::abs(gammaPow - 1.0f) > 0.01f) {
                finalR = std::pow(finalR, invGammaPow);
                finalG = std::pow(finalG, invGammaPow);
                finalB = std::pow(finalB, invGammaPow);
            }

            finalR = std::clamp(finalR, 0.0f, 1.0f);
            finalG = std::clamp(finalG, 0.0f, 1.0f);
            finalB = std::clamp(finalB, 0.0f, 1.0f);
            float alpha = std::max({finalR, finalG, finalB});

            // ── Output BGRA ──────────────────────────────────────────────
            uint8_t* out = result.data.data() + y * result.pitch + px * 4;
            out[0] = linearToSRGB(finalB);
            out[1] = linearToSRGB(finalG);
            out[2] = linearToSRGB(finalR);
            out[3] = static_cast<uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }

    return result;
}

} // namespace puretype
