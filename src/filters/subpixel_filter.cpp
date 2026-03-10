

#include "filters/subpixel_filter.h"
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
#include <intrin.h>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace puretype {

namespace {

inline float Clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

inline float PhaseFromQuant(uint8_t q, bool enabled) {
    if (!enabled) return 0.0f;
    return 0.25f * static_cast<float>(q & 0x03u);
}

inline float ComputeLodRelax(float emSize, float thresholdSmall, float thresholdLarge) {
    const float span = std::max(1.0f, thresholdLarge - thresholdSmall);
    const float t = std::clamp((emSize - thresholdSmall) / span, 0.0f, 1.0f);
    return 1.0f - std::exp(-3.0f * t);
}

inline uint16_t QuantizeFloat(float v, float scale) {
    const float q = std::clamp(v * scale, 0.0f, 65535.0f);
    return static_cast<uint16_t>(q + 0.5f);
}

struct FilterCacheKey {
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

    bool operator==(const FilterCacheKey& o) const {
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

struct FilterCacheKeyHash {
    size_t operator()(const FilterCacheKey& k) const {
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

class FilteredGlyphCache {
public:
    bool TryGet(const FilterCacheKey& key, RGBABitmap& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_map.find(key);
        if (it == m_map.end()) return false;
        m_lru.splice(m_lru.begin(), m_lru, it->second);
        out = it->second->bitmap;
        return true;
    }

    void Put(const FilterCacheKey& key, const RGBABitmap& value) {
        const size_t bytes = value.data.size() + sizeof(RGBABitmap) + sizeof(FilterCacheKey);
        std::lock_guard<std::mutex> lock(m_mutex);

        auto old = m_map.find(key);
        if (old != m_map.end()) {
            if (m_bytes >= old->second->bytes) m_bytes -= old->second->bytes;
            m_lru.erase(old->second);
            m_map.erase(old);
        }

        while (!m_lru.empty() && (m_bytes + bytes) > kMaxBytes) {
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
    struct Entry {
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

FilteredGlyphCache& GetFilteredGlyphCache() {
    static FilteredGlyphCache cache;
    return cache;
}

FilterCacheKey BuildFilterKey(const GlyphBitmap& glyph, const ConfigData& cfg) {
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

bool CpuSupportsAVX2() {
#if defined(_M_X64) || defined(_M_IX86)
    int regs[4] = {0, 0, 0, 0};
    __cpuid(regs, 1);
    const bool osxsave = (regs[2] & (1 << 27)) != 0;
    const bool avx = (regs[2] & (1 << 28)) != 0;
    if (!(osxsave && avx)) return false;

    const unsigned long long xcr0 = _xgetbv(0);
    if ((xcr0 & 0x6) != 0x6) return false;

    __cpuidex(regs, 7, 0);
    return (regs[1] & (1 << 5)) != 0;
#else
    return false;
#endif
}

inline bool HasAvx2Runtime() {
    static const bool kHasAvx2 = CpuSupportsAVX2();
    return kHasAvx2;
}

void Convolve5TapPhaseScalar(const float* src,
                             int width,
                             const float* kernel,
                             float phase,
                             float* dst) {
    for (int x = 0; x < width; ++x) {
        float acc = 0.0f;
        for (int t = -2; t <= 2; ++t) {
            const float fx = static_cast<float>(x + t) + phase;
            const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, width - 1);
            const int x1 = std::clamp(x0 + 1, 0, width - 1);
            const float frac = fx - static_cast<float>(x0);
            const float sample = src[x0] + (src[x1] - src[x0]) * frac;
            acc += sample * kernel[t + 2];
        }
        dst[x] = acc;
    }
}

#if defined(__AVX2__)
void Convolve5TapPhaseAVX2(const float* src,
                           int width,
                           const float* kernel,
                           float phase,
                           float* dst) {
    if (width < 16) {
        Convolve5TapPhaseScalar(src, width, kernel, phase, dst);
        return;
    }

    const __m256 vPhase = _mm256_set1_ps(phase);
    int x = 0;
    for (; x < 2 && x < width; ++x) {
        float acc = 0.0f;
        for (int t = -2; t <= 2; ++t) {
            const float fx = static_cast<float>(x + t) + phase;
            const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, width - 1);
            const int x1 = std::clamp(x0 + 1, 0, width - 1);
            const float frac = fx - static_cast<float>(x0);
            const float sample = src[x0] + (src[x1] - src[x0]) * frac;
            acc += sample * kernel[t + 2];
        }
        dst[x] = acc;
    }

    const int vecEnd = width - 10;
    for (; x <= vecEnd; x += 8) {
        __m256 sum = _mm256_setzero_ps();
        for (int t = -2; t <= 2; ++t) {
            const __m256 a = _mm256_loadu_ps(src + x + t);
            const __m256 b = _mm256_loadu_ps(src + x + t + 1);
            const __m256 interp = _mm256_add_ps(a, _mm256_mul_ps(vPhase, _mm256_sub_ps(b, a)));
            const __m256 w = _mm256_set1_ps(kernel[t + 2]);
            sum = _mm256_add_ps(sum, _mm256_mul_ps(interp, w));
        }
        _mm256_storeu_ps(dst + x, sum);
    }

    for (; x < width; ++x) {
        float acc = 0.0f;
        for (int t = -2; t <= 2; ++t) {
            const float fx = static_cast<float>(x + t) + phase;
            const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, width - 1);
            const int x1 = std::clamp(x0 + 1, 0, width - 1);
            const float frac = fx - static_cast<float>(x0);
            const float sample = src[x0] + (src[x1] - src[x0]) * frac;
            acc += sample * kernel[t + 2];
        }
        dst[x] = acc;
    }
}
#endif

} // namespace

std::unique_ptr<SubpixelFilter> SubpixelFilter::Create(int panelType) {
    if (panelType == static_cast<int>(PanelType::QD_OLED_TRIANGLE)) {
        return std::make_unique<TriangularFilter>();
    }
    return std::make_unique<WRGBFilter>();
}

RGBABitmap WRGBFilter::Apply(const GlyphBitmap& glyph,
                             const ConfigData& cfg) const {
    const FilterCacheKey cacheKey = BuildFilterKey(glyph, cfg);
    RGBABitmap cached;
    if (GetFilteredGlyphCache().TryGet(cacheKey, cached)) {
        return cached;
    }

    // FreeType LCD bitmaps store triplets (R/G/B) horizontally.
    const int pixelWidth = glyph.width / 3;
    const int height = glyph.height;

    RGBABitmap result;
    result.width = pixelWidth;
    result.height = height;
    result.pitch = pixelWidth * 4;
    result.data.resize(result.pitch * height, 0);

    if (pixelWidth <= 0 || height <= 0) return result;

    const float phaseX = PhaseFromQuant(glyph.phaseX, cfg.enableFractionalPositioning);
    const float phaseY = PhaseFromQuant(glyph.phaseY, cfg.enableFractionalPositioning);

    const float emSize = static_cast<float>(height);
    const float lodRelax = ComputeLodRelax(emSize, cfg.lodThresholdSmall, cfg.lodThresholdLarge);
    const float antiFringe = 1.0f - lodRelax;

    constexpr float kRBase[5] = {0.00f, 0.03f, 0.44f, 0.09f, 0.03f};
    constexpr float kGBase[5] = {0.00f, 0.18f, 0.64f, 0.18f, 0.00f};
    constexpr float kBBase[5] = {0.03f, 0.09f, 0.44f, 0.03f, 0.00f};

    float kR[5], kG[5], kB[5];
    for (int i = 0; i < 5; ++i) {
        kR[i] = kRBase[i] * antiFringe;
        kG[i] = kGBase[i] * antiFringe;
        kB[i] = kBBase[i] * antiFringe;
    }
    kR[2] += lodRelax;
    kG[2] += lodRelax;
    kB[2] += lodRelax;

    auto normalize5 = [](float* k) {
        float sum = 0.0f;
        for (int i = 0; i < 5; ++i) sum += k[i];
        if (sum <= 1e-6f) {
            k[0] = k[1] = k[3] = k[4] = 0.0f;
            k[2] = 1.0f;
            return;
        }
        const float inv = 1.0f / sum;
        for (int i = 0; i < 5; ++i) k[i] *= inv;
    };
    normalize5(kR);
    normalize5(kG);
    normalize5(kB);

    const float kWhiteBase = 0.22f * (0.60f + 0.40f * antiFringe);
    const float chromaStrength = std::clamp(cfg.filterStrength * (0.70f + 0.30f * lodRelax), 0.0f, 1.0f);

    const float darkenAmount = cfg.stemDarkeningEnabled
        ? computeDarkenAmount(emSize, cfg.stemDarkeningStrength)
        : 0.0f;

    std::vector<float> rawR(pixelWidth * height);
    std::vector<float> rawG(pixelWidth * height);
    std::vector<float> rawB(pixelWidth * height);

    for (int y = 0; y < height; ++y) {
        const uint8_t* row0 = glyph.data.data() + y * glyph.pitch;
        for (int px = 0; px < pixelWidth; ++px) {
            int sx = px * 3;
            int off = y * pixelWidth + px;

            const float r0 = sRGBToLinear(row0[sx + 0]);
            const float g0 = sRGBToLinear(row0[sx + 1]);
            const float b0 = sRGBToLinear(row0[sx + 2]);

            // Map logical RGB to physical subpixel layout
            if (cfg.panelType == PanelType::RWBG) {
                rawR[off] = r0;
                rawB[off] = g0;
                rawG[off] = b0;
            } else {
                rawR[off] = r0;
                rawG[off] = g0;
                rawB[off] = b0;
            }
        }
    }

    const float gammaPow = cfg.gamma;
    const float invGammaPow = (gammaPow > 0.01f) ? (1.0f / gammaPow) : 1.0f;

    std::vector<float> filtRBuf(pixelWidth);
    std::vector<float> filtGBuf(pixelWidth);
    std::vector<float> filtBBuf(pixelWidth);

    for (int y = 0; y < height; ++y) {
        float* rowR = rawR.data() + y * pixelWidth;
        float* rowG = rawG.data() + y * pixelWidth;
        float* rowB = rawB.data() + y * pixelWidth;

        // Fractional offsets are handled by FreeType's hinting geometry
        const float convPhaseX = 0.0f;

        if (HasAvx2Runtime()) {
#if defined(__AVX2__)
            Convolve5TapPhaseAVX2(rowR, pixelWidth, kR, convPhaseX, filtRBuf.data());
            Convolve5TapPhaseAVX2(rowG, pixelWidth, kG, convPhaseX, filtGBuf.data());
            Convolve5TapPhaseAVX2(rowB, pixelWidth, kB, convPhaseX, filtBBuf.data());
#else
            Convolve5TapPhaseScalar(rowR, pixelWidth, kR, convPhaseX, filtRBuf.data());
            Convolve5TapPhaseScalar(rowG, pixelWidth, kG, convPhaseX, filtGBuf.data());
            Convolve5TapPhaseScalar(rowB, pixelWidth, kB, convPhaseX, filtBBuf.data());
#endif
        } else {
            Convolve5TapPhaseScalar(rowR, pixelWidth, kR, convPhaseX, filtRBuf.data());
            Convolve5TapPhaseScalar(rowG, pixelWidth, kG, convPhaseX, filtGBuf.data());
            Convolve5TapPhaseScalar(rowB, pixelWidth, kB, convPhaseX, filtBBuf.data());
        }

        for (int px = 0; px < pixelWidth; ++px) {
            float filtR = filtRBuf[px];
            float filtG = filtGBuf[px];
            float filtB = filtBBuf[px];

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

            /* Evaluate Luma Sharpness scale multipliers BEFORE subtracting bleed */

            auto getLuma = [](float r, float g, float b) {
                return 0.2126f * r + 0.7152f * g + 0.0722f * b;
            };

            // Extract sharp luma mask from raw rendering vector
            float rawY = getLuma(rawR[off], rawG[off], rawB[off]);
            float filtY = getLuma(filtR, filtG, filtB);

            float finalR = rawY + (filtR - filtY) * chromaStrength;
            float finalG = rawY + (filtG - filtY) * chromaStrength;
            float finalB = rawY + (filtB - filtY) * chromaStrength;

            float yLeft2  = 0.2126f * rawR[leftOff] + 0.7152f * rawG[leftOff] + 0.0722f * rawB[leftOff];
            float yRight2 = 0.2126f * rawR[rightOff] + 0.7152f * rawG[rightOff] + 0.0722f * rawB[rightOff];
            float edgeY = yCenter - 0.5f * (yLeft2 + yRight2);
            float sharpenGain = 0.30f + 0.20f * (1.0f - chromaStrength);
            float ySharp = std::clamp(yCenter + edgeY * sharpenGain, 0.0f, 1.0f);

            // Inject sharpened luma without scaling structural chroma to prevent edge aberrations
            finalR = std::clamp(ySharp + (filtR - filtY) * chromaStrength, 0.0f, 1.0f);
            finalG = std::clamp(ySharp + (filtG - filtY) * chromaStrength, 0.0f, 1.0f);
            finalB = std::clamp(ySharp + (filtB - filtY) * chromaStrength, 0.0f, 1.0f);

            float wLuma = std::min({finalR, finalG, finalB});
            if (wLuma > 0.0f) {
                float wBleed = wLuma * cfg.woledCrossTalkReduction * edgeFactor;
                float currentLuma = getLuma(finalR, finalG, finalB);
                if (currentLuma > 0.0f) {
                    float scale = (currentLuma - wBleed) / currentLuma;
                    finalR *= scale;
                    finalG *= scale;
                    finalB *= scale;
                }
            }
            finalR = applyStemDarkening(std::clamp(finalR, 0.0f, 1.0f), darkenAmount);
            finalG = applyStemDarkening(std::clamp(finalG, 0.0f, 1.0f), darkenAmount);
            finalB = applyStemDarkening(std::clamp(finalB, 0.0f, 1.0f), darkenAmount);

            finalR = std::clamp(finalR, 0.0f, 1.0f);
            finalG = std::clamp(finalG, 0.0f, 1.0f);
            finalB = std::clamp(finalB, 0.0f, 1.0f);
            float alpha = std::max({finalR, finalG, finalB});

            uint8_t* out = result.data.data() + y * result.pitch + px * 4;
            if (cfg.panelType == PanelType::RWBG) {
                out[0] = linearToSRGB(finalG);
                out[1] = linearToSRGB(finalB);
                out[2] = linearToSRGB(finalR);
            } else {
                out[0] = linearToSRGB(finalB);
                out[1] = linearToSRGB(finalG);
                out[2] = linearToSRGB(finalR);
            }
            out[3] = static_cast<uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }

    return result;
}

inline float SampleContinuousX(const uint8_t* row, float fx, int width3x) {
    int x0 = std::clamp(static_cast<int>(fx), 0, width3x - 1);
    int x1 = std::clamp(x0 + 1, 0, width3x - 1);
    float t = fx - static_cast<float>(x0);
    return sRGBToLinear(row[x0]) * (1.0f - t) + sRGBToLinear(row[x1]) * t;
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

    // QD-OLED physical subpixel layout compensation masks
    constexpr float kR[3][3] = {
        {0.00f, 0.00f, 0.00f},
        {0.03f, 0.44f, 0.09f},
        {0.01f, 0.28f, 0.05f},
    };
    constexpr float kG[3][3] = {
        {0.03f, 0.28f, 0.03f},
        {0.06f, 0.44f, 0.06f},
        {0.00f, 0.00f, 0.00f},
    };
    constexpr float kB[3][3] = {
        {0.00f, 0.00f, 0.00f}, 
        {0.09f, 0.44f, 0.03f},
        {0.05f, 0.28f, 0.01f},
    };

    constexpr float kRsum = 0.90f;
    constexpr float kGsum = 0.90f;
    constexpr float kBsum = 0.90f;

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
        const uint8_t* row0 = glyph.data.data() + y * glyph.pitch;
        const uint8_t* row1 = glyph.data.data() + std::min(y + 1, height - 1) * glyph.pitch;
        
        for (int px = 0; px < pixelWidth; ++px) {
            int off = y * pixelWidth + px;

            // Sample for triangular subpixel arrangement
            float cxG = px * 3.0f + 1.0f;
            float cxR = px * 3.0f + 1.0f - 0.5f;
            float cxB = px * 3.0f + 1.0f + 0.5f;

            rawR[off] = SampleContinuousX(row0, cxR, pixelWidth * 3);
            rawB[off] = SampleContinuousX(row0, cxB, pixelWidth * 3);
            
            float green0 = SampleContinuousX(row0, cxG, pixelWidth * 3);
            float green1 = SampleContinuousX(row1, cxG, pixelWidth * 3);
            rawG[off] = green0 * 0.5f + green1 * 0.5f;
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
            filtG /= kGsum;
            filtB /= kBsum;

            int off = y * pixelWidth + px;
            
            auto getLuma = [](float r, float g, float b) {
                return 0.2126f * r + 0.7152f * g + 0.0722f * b;
            };

            float rawY = getLuma(rawR[off], rawG[off], rawB[off]);
            float filtY = getLuma(filtR, filtG, filtB);

            float finalR = rawY + (filtR - filtY) * chromaStrength;
            float finalG = rawY + (filtG - filtY) * chromaStrength;
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

            // Inject sharpened luma without scaling structural chroma to prevent edge aberrations
            finalR = std::clamp(ySharp + (filtR - filtY) * chromaStrength, 0.0f, 1.0f);
            finalG = std::clamp(ySharp + (filtG - filtY) * chromaStrength, 0.0f, 1.0f);
            finalB = std::clamp(ySharp + (filtB - filtY) * chromaStrength, 0.0f, 1.0f);

            finalR = applyStemDarkening(std::clamp(finalR, 0.0f, 1.0f), darkenAmount);
            finalG = applyStemDarkening(std::clamp(finalG, 0.0f, 1.0f), darkenAmount);
            finalB = applyStemDarkening(std::clamp(finalB, 0.0f, 1.0f), darkenAmount);

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
