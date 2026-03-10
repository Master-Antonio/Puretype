#include "filters/subpixel_filter.h"
#include "filters/tone_mapper.h"
#include "rasterizer/ft_rasterizer.h"
#include "color_math.h"
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
        inline float Clamp01(float v)
        {
            return std::clamp(v, 0.0f, 1.0f);
        }

        inline float PhaseFromQuant(uint8_t q, bool enabled)
        {
            if (!enabled) return 0.0f;
            return 0.25f * static_cast<float>(q & 0x03u);
        }

        inline float ComputeLodRelax(float emSize, float thresholdSmall, float thresholdLarge)
        {
            const float span = std::max(1.0f, thresholdLarge - thresholdSmall);
            const float t = std::clamp((emSize - thresholdSmall) / span, 0.0f, 1.0f);
            return 1.0f - std::exp(-3.0f * t);
        }

        inline uint16_t QuantizeFloat(float v, float scale)
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
                    crossTalkQ == o.crossTalkQ;
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

        // Computes a unique cache key for a filtered glyph based on rasterization and filter parameters.
        FilterCacheKey BuildFilterKey(const GlyphBitmap& glyph, const ConfigData& cfg)
        {
            FilterCacheKey key;
            key.fontHash = glyph.fontHash;
            key.glyphIndex = glyph.glyphIndex;
            key.pixelSize = static_cast<uint16_t>(std::min<uint32_t>(glyph.pixelSize, 65535));
            key.fontWeight = glyph.fontWeight;
            key.phaseX = glyph.phaseX & 0x03u;
            key.phaseY = glyph.phaseY & 0x03u;
            key.panelType = static_cast<uint8_t>(cfg.panelType);
            key.flags = (cfg.stemDarkeningEnabled ? 1u : 0u) | (cfg.enableFractionalPositioning ? 2u : 0u);
            key.filterStrengthQ = QuantizeFloat(cfg.filterStrength, 1024.0f);
            key.gammaQ = QuantizeFloat(cfg.gamma, 1024.0f);
            key.stemQ = QuantizeFloat(cfg.stemDarkeningStrength, 1024.0f);
            key.lodSmallQ = QuantizeFloat(cfg.lodThresholdSmall, 64.0f);
            key.lodLargeQ = QuantizeFloat(cfg.lodThresholdLarge, 64.0f);
            key.crossTalkQ = QuantizeFloat(cfg.woledCrossTalkReduction, 4096.0f);
            return key;
        }
    } // namespace

    std::unique_ptr<SubpixelFilter> SubpixelFilter::Create(int panelType)
    {
        if (panelType == static_cast<int>(PanelType::QD_OLED_TRIANGLE))
        {
            return std::make_unique<TriangularFilter>();
        }
        return std::make_unique<WOLEDFilter>();
    }

    inline float SampleContinuousX(const uint8_t* row, float fx, int width3x)
    {
        int x0 = std::clamp(static_cast<int>(fx), 0, width3x - 1);
        int x1 = std::clamp(x0 + 1, 0, width3x - 1);
        float t = fx - static_cast<float>(x0);
        return sRGBToLinear(row[x0]) * (1.0f - t) + sRGBToLinear(row[x1]) * t;
    }

    RGBABitmap WOLEDFilter::Apply(const GlyphBitmap& glyph,
                                  const ConfigData& cfg) const
    {
        const FilterCacheKey cacheKey = BuildFilterKey(glyph, cfg);
        RGBABitmap cached;
        if (GetFilteredGlyphCache().TryGet(cacheKey, cached))
        {
            return cached;
        }

        const int pixelWidth = glyph.width / 3;
        const int height = glyph.height;

        RGBABitmap result;
        result.width = pixelWidth;
        result.height = height;
        result.pitch = pixelWidth * 4;

        if (pixelWidth <= 0 || height <= 0) return result;

        result.data.resize(result.pitch * height, 0);

        const float emSize = static_cast<float>(height);
        const float darkenAmount = cfg.stemDarkeningEnabled
                                       ? computeDarkenAmount(emSize, cfg.stemDarkeningStrength)
                                       : 0.0f;

        const float FILTER_WEIGHTS[7] = {
            1.0f / 16.0f, 2.0f / 16.0f, 3.0f / 16.0f, 4.0f / 16.0f,
            3.0f / 16.0f, 2.0f / 16.0f, 1.0f / 16.0f
        };

        for (int y = 0; y < height; ++y)
        {
            const uint8_t* row0 = glyph.data.data() + y * glyph.pitch;
            std::vector<float> cov4X(pixelWidth * 4);

            for (int px = 0; px < pixelWidth; ++px)
            {
                float center_x = px * 3.0f + 1.0f;
                cov4X[px * 4 + 0] = SampleContinuousX(row0, center_x - 1.125f, pixelWidth * 3);
                cov4X[px * 4 + 1] = SampleContinuousX(row0, center_x - 0.375f, pixelWidth * 3);
                cov4X[px * 4 + 2] = SampleContinuousX(row0, center_x + 0.375f, pixelWidth * 3);
                cov4X[px * 4 + 3] = SampleContinuousX(row0, center_x + 1.125f, pixelWidth * 3);
            }

            for (int px = 0; px < pixelWidth; ++px)
            {
                int p = px * 4;
                float a[4];
                for (int slot = 0; slot < 4; ++slot)
                {
                    float sum = 0.0f;
                    for (int i = 0; i < 7; ++i)
                    {
                        int srcP = std::clamp(p + slot + i - 3, 0, (pixelWidth * 4) - 1);
                        sum += cov4X[srcP] * FILTER_WEIGHTS[i];
                    }
                    a[slot] = sum;
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

                // WOLED (TCON Inversion Handling)
                // Modern WOLED panels use an internal Timing Controller (TCON) to distribute luminance.
                // By ensuring the target color is at least alpha_w, we guarantee the TCON can 
                // extract the white component without crushing the chroma subpixels.
                float final_r = std::max(alpha_r, alpha_w);
                float final_g = std::max(alpha_g, alpha_w);
                float final_b = std::max(alpha_b, alpha_w);

                final_r = applyStemDarkening(final_r, darkenAmount);
                final_g = applyStemDarkening(final_g, darkenAmount);
                final_b = applyStemDarkening(final_b, darkenAmount);

                final_r = std::clamp(final_r, 0.0f, 1.0f);
                final_g = std::clamp(final_g, 0.0f, 1.0f);
                final_b = std::clamp(final_b, 0.0f, 1.0f);

                float alpha = std::max({final_r, final_g, final_b});

                uint8_t* out = result.data.data() + y * result.pitch + px * 4;
                out[0] = linearToSRGB(final_b);
                out[1] = linearToSRGB(final_g);
                out[2] = linearToSRGB(final_r);
                out[3] = static_cast<uint8_t>(alpha * 255.0f + 0.5f);
            }
        }
        ToneMapper::Apply(result, cfg);
        GetFilteredGlyphCache().Put(cacheKey, result);
        return result;
    }

    RGBABitmap TriangularFilter::Apply(const GlyphBitmap& glyph,
                                       const ConfigData& cfg) const
    {
        const FilterCacheKey cacheKey = BuildFilterKey(glyph, cfg);
        RGBABitmap cached;
        if (GetFilteredGlyphCache().TryGet(cacheKey, cached))
        {
            return cached;
        }

        const int pixelWidth = glyph.width / 3;
        const int height = glyph.height;

        RGBABitmap result;
        result.width = pixelWidth;
        result.height = height;
        result.pitch = pixelWidth * 4;

        if (pixelWidth <= 0 || height <= 0) return result;
        result.data.resize(result.pitch * height, 0);

        const float emSize = static_cast<float>(glyph.pixelSize > 0 ? glyph.pixelSize : height);
        const float darkenAmount = cfg.stemDarkeningEnabled
                                       ? computeDarkenAmount(emSize, cfg.stemDarkeningStrength)
                                       : 0.0f;

        const float FILTER_WEIGHTS[5] = {1.0f / 9.0f, 2.0f / 9.0f, 3.0f / 9.0f, 2.0f / 9.0f, 1.0f / 9.0f};


        for (int y = 0; y < height; ++y)
        {
            const uint8_t* row0 = glyph.data.data() + y * glyph.pitch;
            std::vector<float> cov3X(pixelWidth * 3);

            for (int px = 0; px < pixelWidth; ++px)
            {
                float cxR = px * 3.0f + 0.5f;
                float cxG = px * 3.0f + 1.0f;
                float cxB = px * 3.0f + 1.5f;

                cov3X[px * 3 + 0] = SampleContinuousX(row0, cxR, pixelWidth * 3);
                cov3X[px * 3 + 1] = SampleContinuousX(row0, cxG, pixelWidth * 3);
                cov3X[px * 3 + 2] = SampleContinuousX(row0, cxB, pixelWidth * 3);
            }

            for (int px = 0; px < pixelWidth; ++px)
            {
                int p = px * 3;
                float filt[3];
                for (int slot = 0; slot < 3; ++slot)
                {
                    float sum = 0.0f;
                    for (int i = 0; i < 5; ++i)
                    {
                        int srcP = std::clamp(p + slot + i - 2, 0, (pixelWidth * 3) - 1);
                        sum += cov3X[srcP] * FILTER_WEIGHTS[i];
                    }
                    filt[slot] = sum;
                }

                float final_r = std::clamp(filt[0], 0.0f, 1.0f);
                float final_g = std::clamp(filt[1], 0.0f, 1.0f);
                float final_b = std::clamp(filt[2], 0.0f, 1.0f);

                final_r = applyStemDarkening(final_r, darkenAmount);
                final_g = applyStemDarkening(final_g, darkenAmount);
                final_b = applyStemDarkening(final_b, darkenAmount);

                final_r = std::clamp(final_r, 0.0f, 1.0f);
                final_g = std::clamp(final_g, 0.0f, 1.0f);
                final_b = std::clamp(final_b, 0.0f, 1.0f);

                float alpha = std::max({final_r, final_g, final_b});

                uint8_t* out = result.data.data() + y * result.pitch + px * 4;
                out[0] = linearToSRGB(final_b);
                out[1] = linearToSRGB(final_g);
                out[2] = linearToSRGB(final_r);
                out[3] = static_cast<uint8_t>(alpha * 255.0f + 0.5f);
            }
        }
        ToneMapper::Apply(result, cfg);
        GetFilteredGlyphCache().Put(cacheKey, result);
        return result;
    }
}