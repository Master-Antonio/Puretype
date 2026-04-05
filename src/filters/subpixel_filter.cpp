#include "filters/subpixel_filter.h"
#include "filters/tone_mapper.h"
#include "rasterizer/ft_rasterizer.h"
#include "color_math.h"
#include "output/tone_parity.h"
#include "stem_darkening.h"
#include "config.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <list>
#include <unordered_map>
#include <mutex>

namespace puretype
{
    namespace
    {
        float Clamp01(const float v)
        {
            return std::clamp(v, 0.0f, 1.0f);
        }

        uint16_t QuantizeFloat(const float v, const float scale)
        {
            const float q = std::clamp(v * scale, 0.0f, 65535.0f);
            return static_cast<uint16_t>(q + 0.5f);
        }

        struct FilterCacheKey
        {
            uint64_t fontHash = 0;
            uint32_t glyphIndex = 0;
            uint16_t pixelSize = 0;
            uint16_t fontWeight = 400;
            uint8_t phaseX = 0;
            uint8_t phaseY = 0;
            uint8_t panelType = 0;
            uint8_t flags = 0;
            uint16_t filterStrengthQ = 0;
            uint16_t gammaQ = 0;
            uint16_t stemQ = 0;
            uint16_t lodSmallQ = 0;
            uint16_t lodLargeQ = 0;
            uint16_t crossTalkQ = 0;
            uint16_t lumaContrastQ = 0;
            uint16_t oledGammaOutputQ = 0;
            uint16_t textContrastHintQ = 0;
            uint16_t dpiScaleHintQ = 0;

            bool operator==(const FilterCacheKey& o) const
            {
                return fontHash == o.fontHash &&
                    glyphIndex == o.glyphIndex &&
                    pixelSize == o.pixelSize &&
                    fontWeight == o.fontWeight &&
                    phaseX == o.phaseX &&
                    phaseY == o.phaseY &&
                    panelType == o.panelType &&
                    flags == o.flags &&
                    filterStrengthQ == o.filterStrengthQ &&
                    gammaQ == o.gammaQ &&
                    stemQ == o.stemQ &&
                    lodSmallQ == o.lodSmallQ &&
                    lodLargeQ == o.lodLargeQ &&
                    crossTalkQ == o.crossTalkQ &&
                    lumaContrastQ == o.lumaContrastQ &&
                    oledGammaOutputQ == o.oledGammaOutputQ &&
                    textContrastHintQ == o.textContrastHintQ &&
                    dpiScaleHintQ == o.dpiScaleHintQ;
            }
        };

        struct FilterCacheKeyHash
        {
            size_t operator()(const FilterCacheKey& k) const
            {
                size_t h = std::hash<uint64_t>{}(k.fontHash);
                h ^= std::hash<uint32_t>{}(k.glyphIndex) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uint16_t>{}(k.pixelSize) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uint16_t>{}(k.fontWeight) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uint8_t>{}(k.phaseX) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uint8_t>{}(k.phaseY) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uint8_t>{}(k.panelType) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uint8_t>{}(k.flags) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uint16_t>{}(k.filterStrengthQ) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uint16_t>{}(k.gammaQ) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uint16_t>{}(k.stemQ) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uint16_t>{}(k.lodSmallQ) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uint16_t>{}(k.lodLargeQ) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uint16_t>{}(k.crossTalkQ) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uint16_t>{}(k.lumaContrastQ) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uint16_t>{}(k.oledGammaOutputQ) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uint16_t>{}(k.textContrastHintQ) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uint16_t>{}(k.dpiScaleHintQ) + 0x9e3779b9 + (h << 6) + (h >> 2);
                return h;
            }
        };

        class FilteredGlyphCache
        {
        public:
            bool TryGet(const FilterCacheKey& key, RGBABitmap& out)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_map.find(key);
                if (it == m_map.end()) return false;
                m_lru.splice(m_lru.begin(), m_lru, it->second);
                out = it->second->bitmap;
                return true;
            }

            void Put(const FilterCacheKey& key, const RGBABitmap& value)
            {
                const size_t bytes = value.data.size() + sizeof(RGBABitmap) + sizeof(FilterCacheKey);
                std::lock_guard<std::mutex> lock(m_mutex);

                auto old = m_map.find(key);
                if (old != m_map.end())
                {
                    if (m_bytes >= old->second->bytes) m_bytes -= old->second->bytes;
                    m_lru.erase(old->second);
                    m_map.erase(old);
                }

                while (!m_lru.empty() && (m_bytes + bytes) > kMaxBytes)
                {
                    auto last = std::prev(m_lru.end());
                    if (m_bytes >= last->bytes) m_bytes -= last->bytes;
                    m_map.erase(last->key);
                    m_lru.pop_back();
                }

                m_lru.push_front(Entry{key, value, bytes});
                m_map[m_lru.front().key] = m_lru.begin();
                m_bytes += bytes;
            }

        private:
            struct Entry
            {
                FilterCacheKey key;
                RGBABitmap bitmap;
                size_t bytes = 0;
            };

            static constexpr size_t kMaxBytes = 10 * 1024 * 1024;
            std::list<Entry> m_lru;
            std::unordered_map<FilterCacheKey, std::list<Entry>::iterator, FilterCacheKeyHash> m_map;
            size_t m_bytes = 0;
            std::mutex m_mutex;
        };

        FilteredGlyphCache& GetFilteredGlyphCache()
        {
            static FilteredGlyphCache cache;
            return cache;
        }

        FilterCacheKey BuildFilterKey(const GlyphBitmap& glyph, const ConfigData& cfg)
        {
            FilterCacheKey key;
            key.fontHash = glyph.fontHash;
            key.glyphIndex = glyph.glyphIndex;
            key.pixelSize = static_cast<uint16_t>(std::min<uint32_t>(glyph.pixelSize, 65535));
            key.fontWeight = glyph.fontWeight;
            key.phaseX = static_cast<uint8_t>(glyph.phaseX % 3u);
            key.phaseY = static_cast<uint8_t>(glyph.phaseY % 3u);
            key.panelType = static_cast<uint8_t>(cfg.panelType);
            key.flags = (cfg.stemDarkeningEnabled ? 1u : 0u)
                | (cfg.enableFractionalPositioning ? 2u : 0u)
                | (cfg.textContrastHint >= 0.0f ? 4u : 0u);
            key.filterStrengthQ = QuantizeFloat(cfg.filterStrength, 1024.0f);
            key.gammaQ = QuantizeFloat(cfg.gamma, 1024.0f);
            key.stemQ = QuantizeFloat(cfg.stemDarkeningStrength, 1024.0f);
            key.lodSmallQ = QuantizeFloat(cfg.lodThresholdSmall, 64.0f);
            key.lodLargeQ = QuantizeFloat(cfg.lodThresholdLarge, 64.0f);
            key.crossTalkQ = QuantizeFloat(cfg.woledCrossTalkReduction, 4096.0f);
            key.lumaContrastQ = QuantizeFloat(cfg.lumaContrastStrength, 1024.0f);
            key.oledGammaOutputQ = QuantizeFloat(cfg.oledGammaOutput, 1024.0f);
            key.textContrastHintQ = QuantizeFloat(std::max(0.0f, cfg.textContrastHint), 1024.0f);
            key.dpiScaleHintQ = QuantizeFloat(cfg.dpiScaleHint, 1024.0f);
            return key;
        }
    } // namespace

    std::unique_ptr<SubpixelFilter> SubpixelFilter::Create(int panelType)
    {
        // All three QD-OLED generations use the TriangularFilter.
        // The per-generation subpixel center differences (R/B asymmetry in gen1,
        // symmetric in gen3-4) are handled in RemapToOLED in gdi_hooks.cpp,
        // which operates on the final pixel output after filtering.
        // The filter itself only needs to know it's a triangular layout.
        if (panelType == static_cast<int>(PanelType::QD_OLED_GEN1) ||
            panelType == static_cast<int>(PanelType::QD_OLED_GEN3) ||
            panelType == static_cast<int>(PanelType::QD_OLED_GEN4))
            return std::make_unique<TriangularFilter>();
        return std::make_unique<WOLEDFilter>();
    }

    inline float SampleContinuousX(const uint8_t* row, float fx, int width3x)
    {
        const int x0 = std::clamp(static_cast<int>(fx), 0, width3x - 1);
        const int x1 = std::clamp(x0 + 1, 0, width3x - 1);
        const float t = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
        return sRGBToLinear(row[x0]) * (1.0f - t) + sRGBToLinear(row[x1]) * t;
    }

    RGBABitmap WOLEDFilter::Apply(const GlyphBitmap& glyph, const ConfigData& cfg) const
    {
        const FilterCacheKey cacheKey = BuildFilterKey(glyph, cfg);
        if (RGBABitmap cached; GetFilteredGlyphCache().TryGet(cacheKey, cached)) return cached;

        const int pixelWidth = (glyph.width + 2) / 3;
        const int height = glyph.height;

        RGBABitmap result;
        result.width = pixelWidth;
        result.height = height;
        result.pitch = pixelWidth * 4; // 32bpp packed; always 4-byte aligned
        result.fontWeight = glyph.fontWeight;

        if (pixelWidth <= 0 || height <= 0) return result;

        result.data.resize(result.pitch * height, 0);

        const float emSize = static_cast<float>(glyph.pixelSize > 0 ? glyph.pixelSize : height);
        const float darkenAmount = cfg.stemDarkeningEnabled
                                       ? computeDarkenAmount(emSize, cfg.stemDarkeningStrength, glyph.fontWeight)
                                       : 0.0f;

        const float lodFactor = ComputeLodTransitionFactor(emSize,
                                                           cfg.lodThresholdSmall,
                                                           cfg.lodThresholdLarge);
        const float lodFilterScale = 0.80f + 0.20f * lodFactor;
        const float strength = std::clamp(cfg.filterStrength * lodFilterScale, 0.0f, 2.0f);

        // The original 7-tap filter (1+2+3+4+3+2+1 / 16) spans 7/4 = 1.75 physical
        // pixels for WOLED's 4-slot layout. This creates visible horizontal color
        // fringing on thin strokes because energy from one pixel bleeds into the
        // next. A 3-tap (0.25 / 0.50 / 0.25) spans exactly 3/4 = 0.75 pixels —
        // enough to smooth quantization without inter-pixel crosstalk.
        constexpr float FILTER_WEIGHTS[3] = {0.25f, 0.50f, 0.25f};
        constexpr int FILTER_TAPS = 3;
        constexpr int FILTER_CENTER = 1; // offset for i - center

        // Pre-allocate the per-row coverage buffer outside the row loop.
        std::vector<float> cov4X(pixelWidth * 4);

        for (int y = 0; y < height; ++y)
        {
            const uint8_t* row0 = glyph.data.data() + y * glyph.pitch;

            for (int px = 0; px < pixelWidth; ++px)
            {
                const float center_x = px * 3.0f + 1.5f;
                cov4X[px * 4 + 0] = SampleContinuousX(row0, center_x - 1.125f, glyph.width);
                cov4X[px * 4 + 1] = SampleContinuousX(row0, center_x - 0.375f, glyph.width);
                cov4X[px * 4 + 2] = SampleContinuousX(row0, center_x + 0.375f, glyph.width);
                cov4X[px * 4 + 3] = SampleContinuousX(row0, center_x + 1.125f, glyph.width);
            }

            for (int px = 0; px < pixelWidth; ++px)
            {
                const int p = px * 4;
                float a[4];
                for (int slot = 0; slot < 4; ++slot)
                {
                    float sum = 0.0f;
                    for (int i = 0; i < FILTER_TAPS; ++i)
                    {
                        const int srcP = std::clamp(p + slot + i - FILTER_CENTER,
                                                    0, pixelWidth * 4 - 1);
                        sum += cov4X[srcP] * FILTER_WEIGHTS[i];
                    }
                    // Blend raw (sharp) ↔ filtered (smooth) by filterStrength.
                    const float raw = cov4X[p + slot];
                    a[slot] = raw + (sum - raw) * std::min(strength, 1.0f);
                }

                float alpha_r, alpha_w, alpha_b, alpha_g;
                if (cfg.panelType == PanelType::RWBG)
                {
                    alpha_r = a[0];
                    alpha_w = a[1];
                    alpha_b = a[2];
                    alpha_g = a[3];
                }
                else
                {
                    // RGWB
                    alpha_r = a[0];
                    alpha_g = a[1];
                    alpha_w = a[2];
                    alpha_b = a[3];
                }


                // The WOLED white subpixel physically drives R+G+B simultaneously
                // via the TCON. When W fires, it "spills" luminance into the
                // adjacent coloured subpixels even if those channels are dark —
                // this is cross-talk. The reduction factor attenuates the white
                // channel before the max() merge so that the colour channels
                // dominate edge contrast.
                //
                // Formula: instead of std::max(alpha_X, alpha_w), we compute
                //   std::max(alpha_X, alpha_w * (1 - crossTalk))
                // At crossTalk=0.0 the behaviour is unchanged.
                // At crossTalk=0.08 (default) W contributes 92% of its original
                // intensity to each channel — enough to eliminate the grey haze on
                // dark backgrounds without losing brightness on white text.
                const float w_reduced = alpha_w * (1.0f - cfg.woledCrossTalkReduction);

                float final_r = std::max(alpha_r, w_reduced);
                float final_g = std::max(alpha_g, w_reduced);
                float final_b = std::max(alpha_b, w_reduced);

                final_r = applyStemDarkening(final_r, darkenAmount);
                final_g = applyStemDarkening(final_g, darkenAmount);
                final_b = applyStemDarkening(final_b, darkenAmount);

                final_r = Clamp01(final_r);
                final_g = Clamp01(final_g);
                final_b = Clamp01(final_b);

                uint8_t* out = result.data.data() + y * result.pitch + px * 4;
                const uint8_t byteB = linearToSRGB(final_b);
                const uint8_t byteG = linearToSRGB(final_g);
                const uint8_t byteR = linearToSRGB(final_r);
                out[0] = byteB;
                out[1] = byteG;
                out[2] = byteR;
                out[3] = std::max({byteR, byteG, byteB});
            }
        }

        ToneMapper::Apply(result, cfg);
        GetFilteredGlyphCache().Put(cacheKey, result);
        return result;
    }

    RGBABitmap TriangularFilter::Apply(const GlyphBitmap& glyph, const ConfigData& cfg) const
    {
        const FilterCacheKey cacheKey = BuildFilterKey(glyph, cfg);
        RGBABitmap cached;
        if (GetFilteredGlyphCache().TryGet(cacheKey, cached)) return cached;

        const int pixelWidth = (glyph.width + 2) / 3;
        const int height = glyph.height;

        RGBABitmap result;
        result.width = pixelWidth;
        result.height = glyph.height;
        result.pitch = pixelWidth * 4;
        result.fontWeight = glyph.fontWeight;

        if (pixelWidth <= 0 || height <= 0) return result;
        result.data.resize(result.pitch * result.height, 0);

        const float emSize = static_cast<float>(glyph.pixelSize > 0 ? glyph.pixelSize : height);
        const float darkenAmount = cfg.stemDarkeningEnabled
                                       ? computeDarkenAmount(emSize, cfg.stemDarkeningStrength, glyph.fontWeight)
                                       : 0.0f;

        const float lodFactor = ComputeLodTransitionFactor(emSize,
                                                           cfg.lodThresholdSmall,
                                                           cfg.lodThresholdLarge);

        // filterStrength modulates how aggressively the OLED correction is applied.
        // At 1.0: full triangular correction (max color-fringe reduction).
        // At 0.5: 50% correction blended with sharp unfiltered coverage.
        // At 0.0: exits early in the hook (never reaches here).
        const float lodFilterScale = 0.80f + 0.20f * lodFactor;
        const float strength = std::clamp(cfg.filterStrength * lodFilterScale, 0.0f, 2.0f);

        // 3-tap FIR, symmetric — replaces the previous 5-tap which was too wide
        // and caused visible loss of sharpness. The 3-tap spans ±1 subpixel
        // (~1 physical pixel) which is sufficient for triangular geometry correction
        // while preserving stem crispness.
        constexpr float FILTER_WEIGHTS[3] = {0.25f, 0.50f, 0.25f};
        constexpr int FILTER_TAPS = 3;
        constexpr int FILTER_CENTER = 1;

        // Pre-allocate coverage buffer outside the row loop (same as WOLEDFilter).
        std::vector<float> cov3X(pixelWidth * 3);

        for (int y = 0; y < height; ++y)
        {
            const uint8_t* row0 = glyph.data.data() + y * glyph.pitch;
            //
            // In Samsung QD-OLED (AW3423DW, Odyssey G8 etc.) the RGB subpixels
            // are arranged in triangles. Even and odd physical rows are
            // horizontally offset by 1.5 subpixels relative to each other:
            //
            //   Even row:  [R . G . B . R . G . B]   x offsets 0, 1, 2 ...
            //   Odd  row:   [. G . B . R . G . B .]  x offsets +1.5 subpx
            //
            // A pure horizontal sampling pass treats all rows as stripe-RGB,
            // which is wrong for this geometry. The fix blends a fraction of the
            // adjacent row, shifted by ±1.5 subpixels, into the current row's
            // coverage samples. This approximates how the triangular layout
            // shares coverage between physical rows.
            //
            // Even rows look down (adjacent row is +1.5 subpx to the right).
            // Odd  rows look up   (adjacent row is -1.5 subpx to the left).
            const int yAdj = (y % 2 == 0)
                                 ? std::min(y + 1, height - 1) // even: look at row below
                                 : std::max(y - 1, 0); // odd:  look at row above
            const uint8_t* rowAdj = glyph.data.data() + yAdj * glyph.pitch;

            // Horizontal shift of the adjacent row in subpixel units.
            const float kAdjShift = (y % 2 == 0) ? +1.5f : -1.5f;

            // Vertical blend modulated by filterStrength.
            // Reduced from the original 25% to 15% max — the old value caused
            // visible vertical softening on small text. filterStrength further
            // scales this so users can minimize blur at the cost of slight
            // triangular geometry inaccuracy.
            const float qdBlendBase = std::clamp(cfg.qdVerticalBlend, 0.0f, 0.30f)
                * (0.85f + 0.15f * lodFactor);
            const float kVertBlend = qdBlendBase * std::min(strength, 1.0f);

            for (int px = 0; px < pixelWidth; ++px)
            {
                for (int slot = 0; slot < 3; ++slot)
                {
                    const float fxMain = px * 3.0f + slot + 0.5f;
                    const float fxAdj = fxMain + kAdjShift;

                    const float s0 = SampleContinuousX(row0, fxMain, glyph.width);
                    const float s1 = SampleContinuousX(rowAdj, fxAdj, glyph.width);

                    cov3X[px * 3 + slot] = s0 * (1.0f - kVertBlend) + s1 * kVertBlend;
                }
            }

            for (int px = 0; px < pixelWidth; ++px)
            {
                const int p = px * 3;
                float filt[3];
                for (int slot = 0; slot < 3; ++slot)
                {
                    float sum = 0.0f;
                    for (int i = 0; i < FILTER_TAPS; ++i)
                    {
                        const int srcP = std::clamp(p + slot + i - FILTER_CENTER,
                                                    0, pixelWidth * 3 - 1);
                        sum += cov3X[srcP] * FILTER_WEIGHTS[i];
                    }
                    // Blend between sharp (raw) and filtered coverage.
                    // filterStrength=1.0 → full FIR; 0.5 → half raw + half FIR.
                    const float raw = cov3X[p + slot];
                    filt[slot] = raw + (sum - raw) * std::min(strength, 1.0f);
                }

                float final_r = Clamp01(filt[0]);
                float final_g = Clamp01(filt[1]);
                float final_b = Clamp01(filt[2]);

                final_r = applyStemDarkening(final_r, darkenAmount);
                final_g = applyStemDarkening(final_g, darkenAmount);
                final_b = applyStemDarkening(final_b, darkenAmount);

                final_r = Clamp01(final_r);
                final_g = Clamp01(final_g);
                final_b = Clamp01(final_b);

                uint8_t* out = result.data.data() + y * result.pitch + px * 4;
                const uint8_t byteB = linearToSRGB(final_b);
                const uint8_t byteG = linearToSRGB(final_g);
                const uint8_t byteR = linearToSRGB(final_r);
                out[0] = byteB;
                out[1] = byteG;
                out[2] = byteR;
                out[3] = std::max({byteR, byteG, byteB});
            }
        }

        ToneMapper::Apply(result, cfg);
        GetFilteredGlyphCache().Put(cacheKey, result);
        return result;
    }
} // namespace puretype
