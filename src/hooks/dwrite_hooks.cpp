#include "hooks/dwrite_hooks.h"

#include "config.h"
#include "color_math.h"
#include "filters/subpixel_filter.h"
#include "output/blender.h"
#include "puretype.h"
#include "rasterizer/ft_rasterizer.h"

#include <MinHook.h>
#include <Windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <dwrite_2.h>
#include <dwrite_3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

extern void PureTypeLog(const char* fmt, ...);

// ── Inter font detection helper ──────────────────────────────────────
// Checks whether the given font file path belongs to the Inter variable font.
// Case-insensitive substring match on the filename portion.
static bool IsInterFont(const std::string& fontPath)
{
    // Find the last path separator to isolate the filename.
    auto pos = fontPath.find_last_of("\\/");
    const std::string filename = (pos != std::string::npos)
                                     ? fontPath.substr(pos + 1)
                                     : fontPath;

    // Case-insensitive check for "inter" in the filename.
    std::string lower = filename;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return lower.find("inter") != std::string::npos &&
        lower.find("variable") != std::string::npos;
}


namespace puretype::hooks
{
    namespace
    {
        // IDWrite* vtable slots are ABI-sensitive; keep these aligned with the SDK interface layout.
        constexpr size_t kFactoryCreateTextLayoutVtableIndex = 18;
        constexpr size_t kFactoryCreateGdiCompatibleTextLayoutVtableIndex = 19;
        constexpr size_t kTextLayoutDrawVtableIndex = 58;

        using DWriteCreateFactory_t = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
        using CreateTextLayout_t = HRESULT(STDMETHODCALLTYPE*)(IDWriteFactory*,
                                                               WCHAR const*,
                                                               UINT32,
                                                               IDWriteTextFormat*,
                                                               FLOAT,
                                                               FLOAT,
                                                               IDWriteTextLayout**);
        using CreateGdiCompatibleTextLayout_t = HRESULT(STDMETHODCALLTYPE*)(IDWriteFactory*,
                                                                            WCHAR const*,
                                                                            UINT32,
                                                                            IDWriteTextFormat*,
                                                                            FLOAT,
                                                                            FLOAT,
                                                                            FLOAT,
                                                                            DWRITE_MATRIX const*,
                                                                            BOOL,
                                                                            IDWriteTextLayout**);
        using TextLayoutDraw_t = HRESULT(STDMETHODCALLTYPE*)(
            IDWriteTextLayout*, void*, IDWriteTextRenderer*, FLOAT, FLOAT);

        DWriteCreateFactory_t g_origDWriteCreateFactory = nullptr;
        CreateTextLayout_t g_origCreateTextLayout = nullptr;
        CreateGdiCompatibleTextLayout_t g_origCreateGdiCompatibleTextLayout = nullptr;
        TextLayoutDraw_t g_origTextLayoutDraw = nullptr;

        LPVOID g_targetDWriteCreateFactory = nullptr;
        LPVOID g_targetCreateTextLayout = nullptr;
        LPVOID g_targetCreateGdiCompatibleTextLayout = nullptr;
        LPVOID g_targetTextLayoutDraw = nullptr;

        std::mutex g_hookMutex;
        thread_local bool g_insideDWriteDrawGlyphRun = false;

        enum class DrawGlyphRunPreflightState
        {
            Pass,
            Fail,
            Uncertain
        };

        enum class FallbackReason
        {
            None,
            InvalidGlyphRun,
            MissingGlyphIndices,
            ReentrantCall,
            MissingRenderTarget,
            FilterDisabled,
            MissingFontPath,
            MissingFilter,
            MissingReliableTextColor,
            GlyphRasterizationFailed,
            FilterApplicationFailed,
            StagingBoundsFailure,
            StagingAllocationFailed,
            EmptyStagedRun,
            TransactionCommitFailed
        };

        struct DrawGlyphRunPreflightResult
        {
            DrawGlyphRunPreflightState state = DrawGlyphRunPreflightState::Uncertain;
            FallbackReason reason = FallbackReason::None;
        };

        struct DWriteGuardrailCounters
        {
            std::atomic<uint64_t> fallbackTotal{0};
            std::atomic<uint64_t> fallbackUncertain{0};
            std::atomic<uint64_t> fallbackUnsupported{0};
            std::atomic<uint64_t> fallbackAvoidable{0};

            std::atomic<uint64_t> sampleCalls{0};
            std::atomic<uint64_t> cacheHits{0};
            std::atomic<uint64_t> cacheMisses{0};
        };

        DWriteGuardrailCounters g_guardrailCounters;
        std::atomic<uint64_t> g_drawGlyphRunCallCounter{0};

        class SamplingLatencyWindow
        {
        public:
            void Observe(const uint64_t micros)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_samples.size() >= kMaxSamples)
                {
                    m_samples.pop_front();
                }
                m_samples.push_back(micros);
            }

            uint64_t Percentile(const double p) const
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_samples.empty()) return 0;

                std::vector<uint64_t> sorted(m_samples.begin(), m_samples.end());
                std::sort(sorted.begin(), sorted.end());

                const double clamped = std::clamp(p, 0.0, 1.0);
                const size_t index = static_cast<size_t>(std::round(
                    clamped * static_cast<double>(sorted.size() - 1)));
                return sorted[index];
            }

        private:
            static constexpr size_t kMaxSamples = 1024;
            mutable std::mutex m_mutex;
            std::deque<uint64_t> m_samples;
        };

        SamplingLatencyWindow g_samplingLatencyWindow;

        uint64_t GetSteadyMilliseconds()
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                .count());
        }

        struct SamplingCacheKey
        {
            uintptr_t hwnd = 0;
            int leftQ = 0;
            int topQ = 0;
            int rightQ = 0;
            int bottomQ = 0;

            bool operator==(const SamplingCacheKey& o) const
            {
                return hwnd == o.hwnd &&
                    leftQ == o.leftQ &&
                    topQ == o.topQ &&
                    rightQ == o.rightQ &&
                    bottomQ == o.bottomQ;
            }
        };

        struct SamplingCacheKeyHash
        {
            size_t operator()(const SamplingCacheKey& k) const
            {
                size_t h = std::hash<uintptr_t>{}(k.hwnd);
                h ^= std::hash<int>{}(k.leftQ) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<int>{}(k.topQ) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<int>{}(k.rightQ) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<int>{}(k.bottomQ) + 0x9e3779b9 + (h << 6) + (h >> 2);
                return h;
            }
        };

        struct SamplingCacheEntry
        {
            float luma = 0.0f;
            uint64_t sampledAtMs = 0;
            uint64_t lastAccessMs = 0;
        };

        class BackgroundSamplingCache
        {
        public:
            bool TryGet(const SamplingCacheKey& key, const uint64_t nowMs, float* outLuma)
            {
                if (!outLuma) return false;

                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_entries.find(key);
                if (it == m_entries.end())
                {
                    return false;
                }

                if ((nowMs - it->second.sampledAtMs) > kTtlMs)
                {
                    m_entries.erase(it);
                    return false;
                }

                it->second.lastAccessMs = nowMs;
                *outLuma = it->second.luma;
                return true;
            }

            void Put(const SamplingCacheKey& key, const float luma, const uint64_t nowMs)
            {
                std::lock_guard<std::mutex> lock(m_mutex);

                EvictExpired(nowMs);

                SamplingCacheEntry& entry = m_entries[key];
                entry.luma = std::clamp(luma, 0.0f, 1.0f);
                entry.sampledAtMs = nowMs;
                entry.lastAccessMs = nowMs;

                if (m_entries.size() <= kMaxEntries)
                {
                    return;
                }

                auto oldest = m_entries.begin();
                for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
                {
                    if (it->second.lastAccessMs < oldest->second.lastAccessMs)
                    {
                        oldest = it;
                    }
                }

                if (oldest != m_entries.end())
                {
                    m_entries.erase(oldest);
                }
            }

        private:
            static constexpr uint64_t kTtlMs = 90;
            static constexpr size_t kMaxEntries = 1024;

            void EvictExpired(const uint64_t nowMs)
            {
                for (auto it = m_entries.begin(); it != m_entries.end();)
                {
                    if ((nowMs - it->second.sampledAtMs) > kTtlMs)
                    {
                        it = m_entries.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }

            std::unordered_map<SamplingCacheKey, SamplingCacheEntry, SamplingCacheKeyHash> m_entries;
            std::mutex m_mutex;
        };

        BackgroundSamplingCache& GetBackgroundSamplingCache()
        {
            static BackgroundSamplingCache cache;
            return cache;
        }

        SamplingCacheKey BuildSamplingCacheKey(HWND hwnd, const RECT& sampleRect)
        {
            auto quantize = [](const int value) -> int
            {
                constexpr int kQuantStepPx = 8;
                if (value >= 0)
                {
                    return value / kQuantStepPx;
                }
                return -(((-value) + kQuantStepPx - 1) / kQuantStepPx);
            };

            SamplingCacheKey key;
            key.hwnd = reinterpret_cast<uintptr_t>(hwnd);
            key.leftQ = quantize(sampleRect.left);
            key.topQ = quantize(sampleRect.top);
            key.rightQ = quantize(sampleRect.right);
            key.bottomQ = quantize(sampleRect.bottom);
            return key;
        }

        struct FontPathCacheEntry
        {
            std::string path;
            uint64_t cachedAtMs = 0;
        };

        std::unordered_map<uintptr_t, FontPathCacheEntry> g_fontPathCache;
        std::mutex g_fontPathCacheMutex;

        void CacheFontPathForFace(IDWriteFontFace* fontFace, const std::string& path)
        {
            if (!fontFace || path.empty()) return;

            constexpr size_t kMaxEntries = 512;
            const uint64_t nowMs = GetSteadyMilliseconds();
            const uintptr_t key = reinterpret_cast<uintptr_t>(fontFace);

            std::lock_guard<std::mutex> lock(g_fontPathCacheMutex);
            g_fontPathCache[key] = FontPathCacheEntry{path, nowMs};

            if (g_fontPathCache.size() <= kMaxEntries)
            {
                return;
            }

            auto oldest = g_fontPathCache.begin();
            for (auto it = g_fontPathCache.begin(); it != g_fontPathCache.end(); ++it)
            {
                if (it->second.cachedAtMs < oldest->second.cachedAtMs)
                {
                    oldest = it;
                }
            }
            if (oldest != g_fontPathCache.end())
            {
                g_fontPathCache.erase(oldest);
            }
        }

        bool TryGetCachedFontPathForFace(IDWriteFontFace* fontFace, std::string* outPath)
        {
            if (!fontFace || !outPath) return false;

            constexpr uint64_t kMaxAgeMs = 5ull * 60ull * 1000ull;
            const uint64_t nowMs = GetSteadyMilliseconds();
            const uintptr_t key = reinterpret_cast<uintptr_t>(fontFace);

            std::lock_guard<std::mutex> lock(g_fontPathCacheMutex);
            auto it = g_fontPathCache.find(key);
            if (it == g_fontPathCache.end())
            {
                return false;
            }

            if ((nowMs - it->second.cachedAtMs) > kMaxAgeMs)
            {
                g_fontPathCache.erase(it);
                return false;
            }

            *outPath = it->second.path;
            return !outPath->empty();
        }

        struct RecentTextColorEntry
        {
            D2D1_COLOR_F color{};
            uint64_t cachedAtMs = 0;
        };

        std::unordered_map<uintptr_t, RecentTextColorEntry> g_recentTextColorCache;
        std::mutex g_recentTextColorMutex;

        void CacheRecentTextColor(ID2D1RenderTarget* renderTarget, const D2D1_COLOR_F& color)
        {
            if (!renderTarget) return;

            constexpr size_t kMaxEntries = 512;
            const uint64_t nowMs = GetSteadyMilliseconds();
            const uintptr_t key = reinterpret_cast<uintptr_t>(renderTarget);

            std::lock_guard<std::mutex> lock(g_recentTextColorMutex);
            g_recentTextColorCache[key] = RecentTextColorEntry{color, nowMs};

            if (g_recentTextColorCache.size() <= kMaxEntries)
            {
                return;
            }

            auto oldest = g_recentTextColorCache.begin();
            for (auto it = g_recentTextColorCache.begin(); it != g_recentTextColorCache.end(); ++it)
            {
                if (it->second.cachedAtMs < oldest->second.cachedAtMs)
                {
                    oldest = it;
                }
            }

            if (oldest != g_recentTextColorCache.end())
            {
                g_recentTextColorCache.erase(oldest);
            }
        }

        bool TryGetRecentTextColor(ID2D1RenderTarget* renderTarget, D2D1_COLOR_F* outColor)
        {
            if (!renderTarget || !outColor) return false;

            constexpr uint64_t kMaxAgeMs = 2000;
            const uint64_t nowMs = GetSteadyMilliseconds();
            const uintptr_t key = reinterpret_cast<uintptr_t>(renderTarget);

            std::lock_guard<std::mutex> lock(g_recentTextColorMutex);
            auto it = g_recentTextColorCache.find(key);
            if (it == g_recentTextColorCache.end())
            {
                return false;
            }

            if ((nowMs - it->second.cachedAtMs) > kMaxAgeMs)
            {
                g_recentTextColorCache.erase(it);
                return false;
            }

            *outColor = it->second.color;
            return true;
        }

        bool IsColorGlyphRun(const DWRITE_GLYPH_RUN* glyphRun)
        {
            if (!glyphRun || !glyphRun->fontFace) return false;

            IDWriteFontFace2* fontFace2 = nullptr;
            const HRESULT hr = glyphRun->fontFace->QueryInterface(
                __uuidof(IDWriteFontFace2), reinterpret_cast<void**>(&fontFace2));
            if (FAILED(hr) || !fontFace2)
            {
                return false;
            }

            const BOOL isColorFont = fontFace2->IsColorFont();
            fontFace2->Release();
            return isColorFont != FALSE;
        }

        bool IsUnsupportedFallbackReason(const FallbackReason reason)
        {
            return reason == FallbackReason::InvalidGlyphRun ||
                reason == FallbackReason::MissingGlyphIndices ||
                reason == FallbackReason::MissingRenderTarget ||
                reason == FallbackReason::MissingFilter ||
                reason == FallbackReason::MissingFontPath;
        }

        bool IsAvoidableFallbackReason(const FallbackReason reason)
        {
            return reason == FallbackReason::MissingReliableTextColor ||
                reason == FallbackReason::GlyphRasterizationFailed ||
                reason == FallbackReason::FilterApplicationFailed;
        }

        void RecordFallback(const DrawGlyphRunPreflightState state, const FallbackReason reason)
        {
            if (reason == FallbackReason::None) return;

            g_guardrailCounters.fallbackTotal.fetch_add(1, std::memory_order_relaxed);
            if (state == DrawGlyphRunPreflightState::Uncertain)
            {
                g_guardrailCounters.fallbackUncertain.fetch_add(1, std::memory_order_relaxed);
            }
            if (IsUnsupportedFallbackReason(reason))
            {
                g_guardrailCounters.fallbackUnsupported.fetch_add(1, std::memory_order_relaxed);
            }
            if (IsAvoidableFallbackReason(reason))
            {
                g_guardrailCounters.fallbackAvoidable.fetch_add(1, std::memory_order_relaxed);
            }
        }

        void RecordSamplingProbe(const uint64_t elapsedMicros, const bool cacheHit)
        {
            g_guardrailCounters.sampleCalls.fetch_add(1, std::memory_order_relaxed);
            if (cacheHit)
            {
                g_guardrailCounters.cacheHits.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                g_guardrailCounters.cacheMisses.fetch_add(1, std::memory_order_relaxed);
            }
            g_samplingLatencyWindow.Observe(elapsedMicros);
        }

        void MaybeLogGuardrailCounters(const ConfigData& cfg)
        {
            if (!cfg.debugEnabled) return;

            const uint64_t runCount = g_drawGlyphRunCallCounter.fetch_add(1, std::memory_order_relaxed) + 1;
            if (runCount % 128 != 0) return;

            const uint64_t p50 = g_samplingLatencyWindow.Percentile(0.50);
            const uint64_t p95 = g_samplingLatencyWindow.Percentile(0.95);

            PureTypeLog(
                "DWriteGuardrails fallback_total=%llu fallback_uncertain=%llu fallback_unsupported=%llu fallback_avoidable=%llu sample_calls=%llu cache_hits=%llu cache_misses=%llu sample_time_p50_us=%llu sample_time_p95_us=%llu",
                static_cast<unsigned long long>(g_guardrailCounters.fallbackTotal.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_guardrailCounters.fallbackUncertain.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_guardrailCounters.fallbackUnsupported.
                                                                    load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_guardrailCounters.fallbackAvoidable.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_guardrailCounters.sampleCalls.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_guardrailCounters.cacheHits.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_guardrailCounters.cacheMisses.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(p50),
                static_cast<unsigned long long>(p95));
        }

        DrawGlyphRunPreflightResult EvaluateDrawGlyphRunPreflight(
            DWRITE_GLYPH_RUN const* glyphRun,
            const bool insideHook)
        {
            if (!glyphRun || glyphRun->glyphCount == 0 || !glyphRun->fontFace || glyphRun->isSideways)
            {
                return {DrawGlyphRunPreflightState::Fail, FallbackReason::InvalidGlyphRun};
            }

            if (!glyphRun->glyphIndices)
            {
                return {DrawGlyphRunPreflightState::Fail, FallbackReason::MissingGlyphIndices};
            }

            if (insideHook)
            {
                return {DrawGlyphRunPreflightState::Uncertain, FallbackReason::ReentrantCall};
            }

            return {DrawGlyphRunPreflightState::Pass, FallbackReason::None};
        }

        // RAII guard for hook reference counting (DWrite path).
        struct DWriteHookRefGuard
        {
            DWriteHookRefGuard() { ++g_activeHookCount; }
            ~DWriteHookRefGuard() { --g_activeHookCount; }
            DWriteHookRefGuard(const DWriteHookRefGuard&) = delete;
            DWriteHookRefGuard& operator=(const DWriteHookRefGuard&) = delete;
        };

        template <typename T>
        T GetVtableMethod(void* object, const size_t index)
        {
            auto** vtable = *static_cast<void***>(object);
            if (!vtable) return nullptr;
            return reinterpret_cast<T>(vtable[index]);
        }

        bool IsReadablePointer(void const* p)
        {
            if (!p) return false;
            MEMORY_BASIC_INFORMATION mbi = {};
            if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
            if (mbi.State != MEM_COMMIT) return false;
            if ((mbi.Protect & PAGE_NOACCESS) != 0) return false;
            if ((mbi.Protect & PAGE_GUARD) != 0) return false;
            return true;
        }

        bool IsFromD2DModule(void const* functionAddress)
        {
            if (!functionAddress) return false;

            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    static_cast<LPCWSTR>(functionAddress), &module))
            {
                return false;
            }

            wchar_t modulePath[MAX_PATH] = {};
            if (GetModuleFileNameW(module, modulePath, MAX_PATH) == 0) return false;

            std::wstring lowerPath(modulePath);
            std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });

            return lowerPath.find(L"\\d2d1.dll") != std::wstring::npos;
        }

        bool LooksLikeD2DComObject(IUnknown* object)
        {
            if (!object || !IsReadablePointer(object)) return false;

            auto* vtable = *reinterpret_cast<void***>(object);
            if (!vtable || !IsReadablePointer(vtable)) return false;

            const void* queryInterface = vtable[0];
            if (!queryInterface || !IsReadablePointer(queryInterface)) return false;

            // Only call into objects whose COM vtable resolves to d2d1.dll.
            // This avoids AVs when clientDrawingContext is a non-COM payload.
            return IsFromD2DModule(queryInterface);
        }

        ID2D1RenderTarget* TryGetD2DRenderTarget(void* clientDrawingContext)
        {
            if (!clientDrawingContext) return nullptr;

            auto* pUnk = static_cast<IUnknown*>(clientDrawingContext);
            if (!LooksLikeD2DComObject(pUnk)) return nullptr;

            // Try ID2D1DeviceContext first (D2D1.1+, most modern apps).
            ID2D1DeviceContext* deviceContext = nullptr;
            if (SUCCEEDED(
                pUnk->QueryInterface(__uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(&deviceContext))))
            {
                return deviceContext;
            }

            // Fall back to base ID2D1RenderTarget.
            ID2D1RenderTarget* renderTarget = nullptr;
            if (SUCCEEDED(
                pUnk->QueryInterface(__uuidof(ID2D1RenderTarget), reinterpret_cast<void**>(&renderTarget))))
            {
                return renderTarget;
            }

            return nullptr;
        }

        bool ComputeGlyphRunSampleRectPx(const DWRITE_GLYPH_RUN& glyphRun,
                                         const FLOAT baselineOriginX,
                                         const FLOAT baselineOriginY,
                                         const D2D1_MATRIX_3X2_F& transform,
                                         const FLOAT dpiX,
                                         const FLOAT dpiY,
                                         RECT* outRect)
        {
            if (!outRect || glyphRun.glyphCount == 0 || !glyphRun.fontFace)
            {
                return false;
            }

            float runAdvance = 0.0f;
            float minOffsetX = 0.0f;
            float maxOffsetX = 0.0f;
            float minOffsetY = 0.0f;
            float maxOffsetY = 0.0f;
            for (UINT32 i = 0; i < glyphRun.glyphCount; ++i)
            {
                const FLOAT advance = glyphRun.glyphAdvances
                                          ? glyphRun.glyphAdvances[i]
                                          : glyphRun.fontEmSize * 0.5f;
                runAdvance += advance;

                if (glyphRun.glyphOffsets)
                {
                    const FLOAT offsetX = glyphRun.glyphOffsets[i].advanceOffset;
                    const FLOAT offsetY = glyphRun.glyphOffsets[i].ascenderOffset;
                    minOffsetX = std::min(minOffsetX, offsetX);
                    maxOffsetX = std::max(maxOffsetX, offsetX);
                    minOffsetY = std::min(minOffsetY, offsetY);
                    maxOffsetY = std::max(maxOffsetY, offsetY);
                }
            }

            if (runAdvance <= 0.0f)
            {
                runAdvance = std::max(1.0f, glyphRun.fontEmSize);
            }

            DWRITE_FONT_METRICS metrics = {};
            glyphRun.fontFace->GetMetrics(&metrics);
            const float designUnits = std::max(1.0f, static_cast<float>(metrics.designUnitsPerEm));

            float ascentDip = static_cast<float>(metrics.ascent) * glyphRun.fontEmSize / designUnits;
            float descentDip = static_cast<float>(metrics.descent) * glyphRun.fontEmSize / designUnits;
            if (ascentDip <= 0.0f || descentDip <= 0.0f)
            {
                ascentDip = glyphRun.fontEmSize * 0.80f;
                descentDip = glyphRun.fontEmSize * 0.20f;
            }

            const float marginX = std::max(1.0f, glyphRun.fontEmSize * 0.30f);
            const float marginY = std::max(1.0f, glyphRun.fontEmSize * 0.25f);

            const float leftDip = baselineOriginX + minOffsetX - marginX;
            const float rightDip = baselineOriginX + runAdvance + maxOffsetX + marginX;
            const float topDip = baselineOriginY - ascentDip - maxOffsetY - marginY;
            const float bottomDip = baselineOriginY + descentDip - minOffsetY + marginY;

            if (!(rightDip > leftDip) || !(bottomDip > topDip))
            {
                return false;
            }

            auto transformPoint = [&transform](const float x, const float y) -> std::array<float, 2>
            {
                return {
                    x * transform._11 + y * transform._21 + transform._31,
                    x * transform._12 + y * transform._22 + transform._32
                };
            };

            const auto p0 = transformPoint(leftDip, topDip);
            const auto p1 = transformPoint(rightDip, topDip);
            const auto p2 = transformPoint(leftDip, bottomDip);
            const auto p3 = transformPoint(rightDip, bottomDip);

            const float minDipX = std::min({p0[0], p1[0], p2[0], p3[0]});
            const float maxDipX = std::max({p0[0], p1[0], p2[0], p3[0]});
            const float minDipY = std::min({p0[1], p1[1], p2[1], p3[1]});
            const float maxDipY = std::max({p0[1], p1[1], p2[1], p3[1]});

            const float dipToPxX = std::max(0.01f, dpiX / 96.0f);
            const float dipToPxY = std::max(0.01f, dpiY / 96.0f);

            outRect->left = static_cast<LONG>(std::floor(minDipX * dipToPxX) - 1.0f);
            outRect->top = static_cast<LONG>(std::floor(minDipY * dipToPxY) - 1.0f);
            outRect->right = static_cast<LONG>(std::ceil(maxDipX * dipToPxX) + 1.0f);
            outRect->bottom = static_cast<LONG>(std::ceil(maxDipY * dipToPxY) + 1.0f);
            return outRect->right > outRect->left && outRect->bottom > outRect->top;
        }

        bool EstimateBackgroundLumaFromHwnd(HWND hwnd, RECT sampleRectPx, float* outLuma)
        {
            if (!hwnd || !outLuma) return false;

            RECT clientRect = {};
            if (!GetClientRect(hwnd, &clientRect))
            {
                return false;
            }

            RECT clampedRect = {};
            if (!IntersectRect(&clampedRect, &sampleRectPx, &clientRect))
            {
                return false;
            }

            const int width = clampedRect.right - clampedRect.left;
            const int height = clampedRect.bottom - clampedRect.top;
            if (width <= 0 || height <= 0)
            {
                return false;
            }

            HDC dc = GetDC(hwnd);
            if (!dc)
            {
                return false;
            }

            // Sparse grid sampling keeps overhead small while giving a robust luma estimate.
            constexpr int kSampleCols = 6;
            constexpr int kSampleRows = 4;
            float sumLuma = 0.0f;
            int validSamples = 0;

            for (int row = 0; row < kSampleRows; ++row)
            {
                const int y = clampedRect.top +
                    static_cast<int>(((row + 0.5f) * static_cast<float>(height)) / kSampleRows);
                for (int col = 0; col < kSampleCols; ++col)
                {
                    const int x = clampedRect.left +
                        static_cast<int>(((col + 0.5f) * static_cast<float>(width)) / kSampleCols);
                    const COLORREF srgb = GetPixel(dc, x, y);
                    if (srgb == CLR_INVALID)
                    {
                        continue;
                    }

                    const float r = sRGBToLinear(GetRValue(srgb));
                    const float g = sRGBToLinear(GetGValue(srgb));
                    const float b = sRGBToLinear(GetBValue(srgb));
                    sumLuma += 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    ++validSamples;
                }
            }

            ReleaseDC(hwnd, dc);

            if (validSamples < 4)
            {
                return false;
            }

            *outLuma = std::clamp(sumLuma / static_cast<float>(validSamples), 0.0f, 1.0f);
            return true;
        }

        bool EstimateBackgroundLumaFromHwndInstrumented(HWND hwnd,
                                                        RECT sampleRectPx,
                                                        float* outLuma,
                                                        const bool cacheEnabled)
        {
            const auto start = std::chrono::steady_clock::now();

            bool cacheHit = false;
            bool ok = false;
            const uint64_t nowMs = GetSteadyMilliseconds();
            const SamplingCacheKey cacheKey = BuildSamplingCacheKey(hwnd, sampleRectPx);

            if (cacheEnabled)
            {
                cacheHit = GetBackgroundSamplingCache().TryGet(cacheKey, nowMs, outLuma);
            }

            if (cacheHit)
            {
                ok = true;
            }
            else
            {
                ok = EstimateBackgroundLumaFromHwnd(hwnd, sampleRectPx, outLuma);
                if (ok && cacheEnabled)
                {
                    GetBackgroundSamplingCache().Put(cacheKey, *outLuma, nowMs);
                }
            }

            const auto end = std::chrono::steady_clock::now();
            const uint64_t elapsedUs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());

            RecordSamplingProbe(elapsedUs, cacheHit);
            return ok;
        }

        std::string WideToUtf8(std::wstring const& value)
        {
            if (value.empty()) return {};
            int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (len <= 1) return {};
            std::string result(static_cast<size_t>(len), '\0');
            WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), len, nullptr, nullptr);
            if (!result.empty() && result.back() == '\0')
            {
                result.pop_back();
            }
            return result;
        }

        std::string GetFontPathFromFace(IDWriteFontFace* fontFace)
        {
            if (!fontFace) return {};

            UINT32 fileCount = 0;
            HRESULT hr = fontFace->GetFiles(&fileCount, nullptr);
            if (FAILED(hr) || fileCount == 0) return {};

            std::vector<IDWriteFontFile*> files(fileCount, nullptr);
            hr = fontFace->GetFiles(&fileCount, files.data());
            if (FAILED(hr) || files.empty()) return {};

            auto releaseFiles = [&]()
            {
                for (auto* file : files)
                {
                    if (file) file->Release();
                }
            };

            IDWriteFontFileLoader* loader = nullptr;
            hr = files[0]->GetLoader(&loader);
            if (FAILED(hr) || !loader)
            {
                releaseFiles();
                return {};
            }

            void const* referenceKey = nullptr;
            UINT32 referenceKeySize = 0;
            hr = files[0]->GetReferenceKey(&referenceKey, &referenceKeySize);
            if (FAILED(hr) || !referenceKey || referenceKeySize == 0)
            {
                loader->Release();
                releaseFiles();
                return {};
            }

            std::string result;
            IDWriteLocalFontFileLoader* localLoader = nullptr;
            hr = loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),
                                        reinterpret_cast<void**>(&localLoader));
            if (SUCCEEDED(hr) && localLoader)
            {
                UINT32 pathLength = 0;
                hr = localLoader->GetFilePathLengthFromKey(referenceKey, referenceKeySize, &pathLength);
                if (SUCCEEDED(hr) && pathLength > 0)
                {
                    std::wstring path(pathLength + 1, L'\0');
                    hr = localLoader->GetFilePathFromKey(
                        referenceKey, referenceKeySize, path.data(), static_cast<UINT32>(path.size()));
                    if (SUCCEEDED(hr))
                    {
                        if (!path.empty() && path.back() == L'\0')
                        {
                            path.pop_back();
                        }
                        result = WideToUtf8(path);
                    }
                }
                localLoader->Release();
            }

            loader->Release();
            releaseFiles();
            return result;
        }

        class CustomTextRenderer final : public IDWriteTextRenderer1
        {
        public:
            explicit CustomTextRenderer(IDWriteTextRenderer* original)
            {
                m_original = original;
                if (m_original)
                {
                    m_original->AddRef();
                    m_original->QueryInterface(
                        __uuidof(IDWriteTextRenderer1), reinterpret_cast<void**>(&m_original1));
                }
            }

            virtual ~CustomTextRenderer()
            {
                if (m_original1) m_original1->Release();
                if (m_original) m_original->Release();
            }

            ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refCount; }

            ULONG STDMETHODCALLTYPE Release() override
            {
                ULONG ref = --m_refCount;
                if (ref == 0) delete this;
                return ref;
            }

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
            {
                if (!ppvObject) return E_POINTER;
                *ppvObject = nullptr;

                if (riid == __uuidof(IUnknown) ||
                    riid == __uuidof(IDWritePixelSnapping) ||
                    riid == __uuidof(IDWriteTextRenderer))
                {
                    *ppvObject = static_cast<IDWriteTextRenderer*>(this);
                    AddRef();
                    return S_OK;
                }
                else if (riid == __uuidof(IDWriteTextRenderer1) && m_original1)
                {
                    *ppvObject = static_cast<IDWriteTextRenderer1*>(this);
                    AddRef();
                    return S_OK;
                }

                if (m_original)
                {
                    return m_original->QueryInterface(riid, ppvObject);
                }

                return E_NOINTERFACE;
            }

            HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void* clientDrawingContext,
                                                              BOOL* isDisabled) override
            {
                if (m_original)
                {
                    return m_original->IsPixelSnappingDisabled(clientDrawingContext, isDisabled);
                }
                if (isDisabled) *isDisabled = FALSE;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetCurrentTransform(void* clientDrawingContext,
                                                          DWRITE_MATRIX* transform) override
            {
                if (m_original)
                {
                    return m_original->GetCurrentTransform(clientDrawingContext, transform);
                }
                if (!transform) return E_POINTER;
                *transform = DWRITE_MATRIX{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void* clientDrawingContext,
                                                      FLOAT* pixelsPerDip) override
            {
                if (m_original)
                {
                    return m_original->GetPixelsPerDip(clientDrawingContext, pixelsPerDip);
                }
                if (!pixelsPerDip) return E_POINTER;
                *pixelsPerDip = 1.0f;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE DrawGlyphRun(void* clientDrawingContext,
                                                   FLOAT baselineOriginX,
                                                   FLOAT baselineOriginY,
                                                   DWRITE_MEASURING_MODE measuringMode,
                                                   DWRITE_GLYPH_RUN const* glyphRun,
                                                   DWRITE_GLYPH_RUN_DESCRIPTION const* glyphRunDescription,
                                                   IUnknown* clientDrawingEffect) override
            {
                const DrawGlyphRunPreflightResult preflight =
                    EvaluateDrawGlyphRunPreflight(glyphRun, g_insideDWriteDrawGlyphRun);
                if (preflight.state != DrawGlyphRunPreflightState::Pass)
                {
                    RecordFallback(preflight.state, preflight.reason);
                    return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                               measuringMode, glyphRun, glyphRunDescription,
                                               clientDrawingEffect);
                }

                // RAII reference count — prevents rasterizer shutdown while rendering.
                DWriteHookRefGuard refGuard;

                ID2D1RenderTarget* renderTarget = TryGetD2DRenderTarget(clientDrawingContext);
                if (!renderTarget)
                {
                    RecordFallback(DrawGlyphRunPreflightState::Fail, FallbackReason::MissingRenderTarget);
                    return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                               measuringMode, glyphRun, glyphRunDescription,
                                               clientDrawingEffect);
                }

                // RAII safety logic to automatically release the target when the method exits.
                struct RenderTargetReleaser
                {
                    ID2D1RenderTarget* rt;
                    ~RenderTargetReleaser() { if (rt) rt->Release(); }
                } rtReleaser{renderTarget};

                std::string monitorName = "";
                HWND targetHwnd = nullptr;
                ID2D1HwndRenderTarget* hwndRT = nullptr;
                if (SUCCEEDED(
                        renderTarget->QueryInterface(__uuidof(ID2D1HwndRenderTarget),
                            reinterpret_cast<void**>(&hwndRT))) &&
                    hwndRT)
                {
                    if (HWND hwnd = hwndRT->GetHwnd())
                    {
                        targetHwnd = hwnd;
                        if (HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST))
                        {
                            MONITORINFOEXA mi;
                            mi.cbSize = sizeof(mi);
                            if (GetMonitorInfoA(hmon, &mi))
                            {
                                monitorName = mi.szDevice;
                            }
                        }
                    }
                    hwndRT->Release();
                }

                const ConfigData cfg = Config::Instance().GetData(monitorName);
                ConfigData renderCfg = cfg;

                auto forwardWithFallback = [&](const DrawGlyphRunPreflightState state,
                                               const FallbackReason reason) -> HRESULT
                {
                    RecordFallback(state, reason);
                    MaybeLogGuardrailCounters(renderCfg);
                    return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                               measuringMode, glyphRun, glyphRunDescription,
                                               clientDrawingEffect);
                };

                if (renderCfg.colorGlyphBypassEnabled && IsColorGlyphRun(glyphRun))
                {
                    if (renderCfg.debugEnabled)
                    {
                        PureTypeLog("DWrite DrawGlyphRun: color glyph run bypassed to original renderer");
                    }
                    MaybeLogGuardrailCounters(renderCfg);
                    return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                               measuringMode, glyphRun, glyphRunDescription,
                                               clientDrawingEffect);
                }

                if (renderCfg.filterStrength <= 0.0f)
                {
                    return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                               FallbackReason::FilterDisabled);
                }

                // Optional fallback-reduction heuristics are gated by dwriteFallbackV2Enabled only.
                // Atomic transactional safety remains always-on regardless of this toggle.
                const bool fallbackHeuristicsEnabled = renderCfg.dwriteFallbackV2Enabled;

                initColorMathLUTs(renderCfg.gammaMode == GammaMode::OLED);

                std::string fontPath = GetFontPathFromFace(glyphRun->fontFace);
                if (!fontPath.empty())
                {
                    if (fallbackHeuristicsEnabled)
                    {
                        CacheFontPathForFace(glyphRun->fontFace, fontPath);
                    }
                }
                else if (fallbackHeuristicsEnabled)
                {
                    std::string cachedPath;
                    if (TryGetCachedFontPathForFace(glyphRun->fontFace, &cachedPath))
                    {
                        fontPath = std::move(cachedPath);
                        if (renderCfg.debugEnabled)
                        {
                            PureTypeLog("DWrite DrawGlyphRun: using cached font path fallback");
                        }
                    }
                }

                if (fontPath.empty())
                {
                    return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                               FallbackReason::MissingFontPath);
                }

                // ── Inter font axis overrides ────────────────────────────────
                const bool isInter = IsInterFont(fontPath);
                VariableAxisOverrides axisOverrides;
                if (isInter)
                {
                    if (renderCfg.interFontWeight > 0)
                        axisOverrides.weight = renderCfg.interFontWeight;
                    if (renderCfg.interOpticalSize > 0.f)
                        axisOverrides.opticalSize = renderCfg.interOpticalSize;
                }
                FLOAT pixelsPerDip = 1.0f;

                // Hardware Scale Synchronization:
                // Extract the true scale from the render target's transform and DPI,
                // rather than relying on DWrite's reported pixelsPerDip which can be
                // inaccurate for Qt, WinUI, and modern layout engines.
                FLOAT rtDpiX = 96.0f, rtDpiY = 96.0f;
                renderTarget->GetDpi(&rtDpiX, &rtDpiY);

                D2D1_MATRIX_3X2_F rtTransform;
                renderTarget->GetTransform(&rtTransform);

                float scaleX = std::sqrt(rtTransform._11 * rtTransform._11 + rtTransform._12 * rtTransform._12);
                if (scaleX > 0.0001f)
                {
                    pixelsPerDip = (rtDpiX / 96.0f) * scaleX;
                }
                else
                {
                    (void)GetPixelsPerDip(clientDrawingContext, &pixelsPerDip);
                }

                // Clamp pixelsPerDip to a minimum of 0.25 to prevent zero-size
                // rasterization and division-by-zero in downstream positioning math.
                pixelsPerDip = std::max(0.25f, pixelsPerDip);

                // DPI-aware fade-out: match GDI policy to reduce chroma/fringing at high DPI.
                const float effectiveDpi = pixelsPerDip * 96.0f;
                float dpiScaleHint = 1.0f;
                if (effectiveDpi >= renderCfg.highDpiThresholdHigh)
                {
                    return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                               FallbackReason::FilterDisabled);
                }
                if (effectiveDpi > renderCfg.highDpiThresholdLow)
                {
                    dpiScaleHint = 1.0f - std::clamp(
                        (effectiveDpi - renderCfg.highDpiThresholdLow) /
                        (renderCfg.highDpiThresholdHigh - renderCfg.highDpiThresholdLow),
                        0.0f, 1.0f);
                    renderCfg.filterStrength *= dpiScaleHint;
                    if (renderCfg.filterStrength <= 0.0f)
                    {
                        return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                                   FallbackReason::FilterDisabled);
                    }
                }
                renderCfg.dpiScaleHint = dpiScaleHint;

                const auto emPixels = static_cast<float>(std::lround(glyphRun->fontEmSize * pixelsPerDip));
                const uint32_t pixelSize = static_cast<uint32_t>(std::max(1.0f, emPixels));

                const auto filter = SubpixelFilter::Create(static_cast<int>(renderCfg.panelType));
                if (!filter)
                {
                    return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                               FallbackReason::MissingFilter);
                }

                auto textColor = D2D1_COLOR_F{0.0f, 0.0f, 0.0f, 1.0f};
                bool hasReliableTextColor = false;
                if (clientDrawingEffect)
                {
                    ID2D1SolidColorBrush* brush = nullptr;
                    if (SUCCEEDED(clientDrawingEffect->QueryInterface(
                            __uuidof(ID2D1SolidColorBrush), reinterpret_cast<void**>(&brush))) &&
                        brush)
                    {
                        textColor = brush->GetColor();
                        hasReliableTextColor = true;
                        brush->Release();
                        if (renderCfg.debugEnabled)
                        {
                            PureTypeLog("DWrite DrawGlyphRun: color from brush R=%.2f G=%.2f B=%.2f A=%.2f",
                                        textColor.r, textColor.g, textColor.b, textColor.a);
                        }
                    }
                    else if (renderCfg.debugEnabled)
                    {
                        PureTypeLog("DWrite DrawGlyphRun: clientDrawingEffect present but NOT a SolidColorBrush");
                    }
                }
                else if (renderCfg.debugEnabled)
                {
                    PureTypeLog("DWrite DrawGlyphRun: clientDrawingEffect is NULL");
                }

                if (hasReliableTextColor)
                {
                    CacheRecentTextColor(renderTarget, textColor);
                }
                else if (fallbackHeuristicsEnabled && TryGetRecentTextColor(renderTarget, &textColor))
                {
                    hasReliableTextColor = true;
                    if (renderCfg.debugEnabled)
                    {
                        PureTypeLog("DWrite DrawGlyphRun: using cached text color fallback");
                    }
                }

                if (!hasReliableTextColor)
                {
                    return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                               FallbackReason::MissingReliableTextColor);
                }

                // Contrast hint used by ToneMapper in the DWrite path.
                // Prefer real background sampling from HWND render targets, and
                // fall back to the text-luma proxy when sampling is unavailable.
                const uint8_t textR8 = static_cast<uint8_t>(std::clamp(textColor.r, 0.0f, 1.0f) * 255.0f + 0.5f);
                const uint8_t textG8 = static_cast<uint8_t>(std::clamp(textColor.g, 0.0f, 1.0f) * 255.0f + 0.5f);
                const uint8_t textB8 = static_cast<uint8_t>(std::clamp(textColor.b, 0.0f, 1.0f) * 255.0f + 0.5f);
                const float linTextR = sRGBToLinear(textR8);
                const float linTextG = sRGBToLinear(textG8);
                const float linTextB = sRGBToLinear(textB8);
                const float linTextLuma = 0.2126f * linTextR + 0.7152f * linTextG + 0.0722f * linTextB;
                const float textAlpha = std::clamp(textColor.a, 0.0f, 1.0f);

                float contrastHint = std::max(linTextLuma, 1.0f - linTextLuma);
                float sampledBgLuma = 0.0f;
                bool usedMeasuredContrast = false;
                if (targetHwnd)
                {
                    RECT sampleRectPx = {};
                    if (ComputeGlyphRunSampleRectPx(*glyphRun, baselineOriginX, baselineOriginY,
                                                    rtTransform, rtDpiX, rtDpiY, &sampleRectPx) &&
                        EstimateBackgroundLumaFromHwndInstrumented(
                            targetHwnd,
                            sampleRectPx,
                            &sampledBgLuma,
                            renderCfg.contrastSamplingCacheEnabled))
                    {
                        contrastHint = std::abs(linTextLuma - sampledBgLuma);
                        usedMeasuredContrast = true;
                    }
                }

                renderCfg.textContrastHint = std::clamp(contrastHint * textAlpha, 0.0f, 1.0f);

                if (renderCfg.debugEnabled)
                {
                    PureTypeLog("  font='%s' emSize=%.1f pixelsPerDip=%.2f pixelSize=%u glyphCount=%u",
                                fontPath.c_str(), glyphRun->fontEmSize, pixelsPerDip, pixelSize,
                                glyphRun->glyphCount);
                    PureTypeLog("  contrastHint=%.3f source=%s textLuma=%.3f bgLuma=%.3f",
                                renderCfg.textContrastHint,
                                usedMeasuredContrast ? "measured" : "proxy",
                                linTextLuma,
                                usedMeasuredContrast ? sampledBgLuma : -1.0f);
                }

                uint16_t fontWeight = 400;
                IDWriteFontFace3* fontFace3 = nullptr;
                if (SUCCEEDED(glyphRun->fontFace->QueryInterface(__uuidof(IDWriteFontFace3),
                        reinterpret_cast<void**>(&fontFace3))) &&
                    fontFace3)
                {
                    fontWeight = static_cast<uint16_t>(std::clamp(
                        static_cast<int>(fontFace3->GetWeight()), 100, 900));
                    fontFace3->Release();
                }

                // Override fontWeight for Inter so stem darkening uses the
                // user-specified axis value consistently.
                if (isInter && axisOverrides.weight > 0)
                {
                    fontWeight = static_cast<uint16_t>(std::clamp(axisOverrides.weight, 100, 900));
                }

                auto computePhase = [](FLOAT logicalCoord, FLOAT ppd) -> uint8_t
                {
                    const float pixelCoord = logicalCoord * ppd;
                    const float frac = pixelCoord - std::floor(pixelCoord);
                    int phase = static_cast<int>(std::floor(frac * 3.0f + 0.5f));
                    phase %= 3;
                    if (phase < 0) phase += 3;
                    return static_cast<uint8_t>(phase);
                };

                struct PendingGlyph
                {
                    FLOAT x = 0.0f;
                    FLOAT y = 0.0f;
                    RGBABitmap bitmap;
                };
                std::vector<PendingGlyph> pending;
                try
                {
                    pending.reserve(glyphRun->glyphCount);
                }
                catch (const std::bad_alloc&)
                {
                    return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                               FallbackReason::StagingAllocationFailed);
                }
                catch (...)
                {
                    return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                               FallbackReason::StagingAllocationFailed);
                }

                FLOAT penX = baselineOriginX;
                for (UINT32 i = 0; i < glyphRun->glyphCount; ++i)
                {
                    const FLOAT offsetX = glyphRun->glyphOffsets ? glyphRun->glyphOffsets[i].advanceOffset : 0.0f;
                    const FLOAT offsetY = glyphRun->glyphOffsets ? glyphRun->glyphOffsets[i].ascenderOffset : 0.0f;
                    const uint8_t phaseX = computePhase(penX + offsetX, pixelsPerDip);
                    const uint8_t phaseY = computePhase(baselineOriginY - offsetY, pixelsPerDip);

                    const GlyphBitmap* glyph = FTRasterizer::Instance().RasterizeGlyph(
                        fontPath, glyphRun->glyphIndices[i], pixelSize, renderCfg, fontWeight, phaseX, phaseY,
                        axisOverrides);
                    if (!glyph)
                    {
                        return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                                   FallbackReason::GlyphRasterizationFailed);
                    }

                    // Glyph positioning with padding-aware bearing.
                    FLOAT bearingXPixels = static_cast<FLOAT>(glyph->bearingX - glyph->padLeft) / 3.0f;
                    FLOAT glyphX = penX + bearingXPixels / pixelsPerDip;
                    FLOAT glyphY = baselineOriginY
                        - static_cast<FLOAT>(glyph->bearingY + glyph->padTop) / pixelsPerDip;

                    if (glyphRun->glyphOffsets)
                    {
                        glyphX += offsetX;
                        glyphY -= offsetY;
                    }

                    FLOAT advance = glyphRun->glyphAdvances
                                        ? glyphRun->glyphAdvances[i]
                                        : (static_cast<FLOAT>(glyph->advanceX) / 3.0f) / pixelsPerDip;

                    // Apply Inter letter spacing: add extra spacing (in DIP) between glyphs.
                    if (isInter && renderCfg.interLetterSpacing != 0.f &&
                        i + 1 < glyphRun->glyphCount)
                    {
                        advance += renderCfg.interLetterSpacing / pixelsPerDip;
                    }

                    penX += advance;

                    if (glyph->width <= 0 || glyph->height <= 0 || glyph->data.empty())
                    {
                        continue;
                    }

                    RGBABitmap filtered;
                    try
                    {
                        filtered = filter->Apply(*glyph, renderCfg);
                    }
                    catch (const std::bad_alloc&)
                    {
                        return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                                   FallbackReason::StagingAllocationFailed);
                    }
                    catch (...)
                    {
                        return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                                   FallbackReason::StagingAllocationFailed);
                    }
                    if (filtered.data.empty())
                    {
                        return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                                   FallbackReason::FilterApplicationFailed);
                    }

                    if (renderCfg.highlightRenderedGlyphs)
                    {
                        for (size_t p = 0; p + 3 < filtered.data.size(); p += 4)
                        {
                            filtered.data[p + 0] =
                                static_cast<uint8_t>(filtered.data[p + 0] * 0.7f + 200.0f * 0.3f);
                            filtered.data[p + 1] =
                                static_cast<uint8_t>(filtered.data[p + 1] * 0.7f + 200.0f * 0.3f);
                        }
                    }

                    PendingGlyph pg;
                    pg.x = glyphX;
                    pg.y = glyphY;
                    pg.bitmap = std::move(filtered);
                    try
                    {
                        pending.push_back(std::move(pg));
                    }
                    catch (const std::bad_alloc&)
                    {
                        return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                                   FallbackReason::StagingAllocationFailed);
                    }
                    catch (...)
                    {
                        return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                                   FallbackReason::StagingAllocationFailed);
                    }
                }

                if (pending.empty())
                {
                    return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                               FallbackReason::EmptyStagedRun);
                }

                // Always-on transactional policy:
                // Stage full-run output first, then commit once to avoid segmented fallback.
                int minLeftPx = std::numeric_limits<int>::max();
                int minTopPx = std::numeric_limits<int>::max();
                int maxRightPx = std::numeric_limits<int>::min();
                int maxBottomPx = std::numeric_limits<int>::min();

                for (const auto& item : pending)
                {
                    const int glyphLeftPx = static_cast<int>(std::floor(item.x * pixelsPerDip));
                    const int glyphTopPx = static_cast<int>(std::floor(item.y * pixelsPerDip));
                    minLeftPx = std::min(minLeftPx, glyphLeftPx);
                    minTopPx = std::min(minTopPx, glyphTopPx);
                    maxRightPx = std::max(maxRightPx, glyphLeftPx + item.bitmap.width);
                    maxBottomPx = std::max(maxBottomPx, glyphTopPx + item.bitmap.height);
                }

                if (maxRightPx <= minLeftPx || maxBottomPx <= minTopPx)
                {
                    return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                               FallbackReason::EmptyStagedRun);
                }

                const int64_t stagedWidth64 = static_cast<int64_t>(maxRightPx) -
                    static_cast<int64_t>(minLeftPx);
                const int64_t stagedHeight64 = static_cast<int64_t>(maxBottomPx) -
                    static_cast<int64_t>(minTopPx);
                constexpr int64_t kMaxStagedRunDimensionPx = 16384;
                constexpr size_t kMaxStagedRunBytes = 256ull * 1024ull * 1024ull;

                if (stagedWidth64 <= 0 || stagedHeight64 <= 0 ||
                    stagedWidth64 > kMaxStagedRunDimensionPx ||
                    stagedHeight64 > kMaxStagedRunDimensionPx)
                {
                    return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                               FallbackReason::StagingBoundsFailure);
                }

                const int64_t stagedPitch64 = stagedWidth64 * 4;
                if (stagedPitch64 <= 0 ||
                    stagedPitch64 > static_cast<int64_t>(std::numeric_limits<int>::max()))
                {
                    return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                               FallbackReason::StagingBoundsFailure);
                }

                const size_t stagedPitchSize = static_cast<size_t>(stagedPitch64);
                const size_t stagedHeightSize = static_cast<size_t>(stagedHeight64);
                if (stagedPitchSize > std::numeric_limits<size_t>::max() / stagedHeightSize)
                {
                    return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                               FallbackReason::StagingBoundsFailure);
                }

                const size_t stagedBytes = stagedPitchSize * stagedHeightSize;
                if (stagedBytes == 0 || stagedBytes > kMaxStagedRunBytes)
                {
                    return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                               FallbackReason::StagingBoundsFailure);
                }

                RGBABitmap stagedRun;
                stagedRun.width = static_cast<int>(stagedWidth64);
                stagedRun.height = static_cast<int>(stagedHeight64);
                stagedRun.pitch = static_cast<int>(stagedPitch64);
                try
                {
                    stagedRun.data.assign(stagedBytes, 0);
                }
                catch (const std::bad_alloc&)
                {
                    return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                               FallbackReason::StagingAllocationFailed);
                }
                catch (...)
                {
                    return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                               FallbackReason::StagingAllocationFailed);
                }

                for (const auto& item : pending)
                {
                    if (item.bitmap.width <= 0 || item.bitmap.height <= 0 || item.bitmap.pitch <= 0)
                    {
                        return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                                   FallbackReason::StagingBoundsFailure);
                    }

                    const int64_t glyphRowBytes64 = static_cast<int64_t>(item.bitmap.width) * 4;
                    if (glyphRowBytes64 <= 0 || glyphRowBytes64 > item.bitmap.pitch)
                    {
                        return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                                   FallbackReason::StagingBoundsFailure);
                    }

                    const size_t glyphPitchSize = static_cast<size_t>(item.bitmap.pitch);
                    const size_t glyphHeightSize = static_cast<size_t>(item.bitmap.height);
                    if (glyphPitchSize > std::numeric_limits<size_t>::max() / glyphHeightSize)
                    {
                        return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                                   FallbackReason::StagingBoundsFailure);
                    }

                    const size_t glyphExpectedBytes = glyphPitchSize * glyphHeightSize;
                    if (glyphExpectedBytes > item.bitmap.data.size())
                    {
                        return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                                   FallbackReason::StagingBoundsFailure);
                    }

                    const int glyphLeftPx = static_cast<int>(std::floor(item.x * pixelsPerDip));
                    const int glyphTopPx = static_cast<int>(std::floor(item.y * pixelsPerDip));
                    const int dstX0 = glyphLeftPx - minLeftPx;
                    const int dstY0 = glyphTopPx - minTopPx;

                    for (int row = 0; row < item.bitmap.height; ++row)
                    {
                        const int dstRow = dstY0 + row;
                        if (dstRow < 0 || dstRow >= stagedRun.height) continue;

                        const size_t srcRowOffset = static_cast<size_t>(row) * glyphPitchSize;
                        const uint8_t* srcRow = item.bitmap.data.data() + srcRowOffset;
                        const size_t dstRowOffset =
                            static_cast<size_t>(dstRow) * static_cast<size_t>(stagedRun.pitch);
                        uint8_t* dstRowPtr = stagedRun.data.data() + dstRowOffset;

                        for (int col = 0; col < item.bitmap.width; ++col)
                        {
                            const int dstCol = dstX0 + col;
                            if (dstCol < 0 || dstCol >= stagedRun.width) continue;

                            const uint8_t* srcPx = srcRow + col * 4;
                            uint8_t* dstPx = dstRowPtr + dstCol * 4;

                            dstPx[0] = std::max(dstPx[0], srcPx[0]);
                            dstPx[1] = std::max(dstPx[1], srcPx[1]);
                            dstPx[2] = std::max(dstPx[2], srcPx[2]);
                            dstPx[3] = std::max(dstPx[3], srcPx[3]);
                        }
                    }
                }

                struct RunRenderTransaction
                {
                    bool preflightPassed = false;
                    bool staged = false;
                    bool committed = false;
                } transaction;

                transaction.preflightPassed = true;
                transaction.staged = true;

                // Preflight transparent brush before commit when feasible so post-commit behavior is deterministic.
                ID2D1SolidColorBrush* transparentBrush = nullptr;
                const HRESULT transparentBrushHr = renderTarget->CreateSolidColorBrush(
                    D2D1_COLOR_F{0.0f, 0.0f, 0.0f, 0.0f}, &transparentBrush);

                struct TransparentBrushReleaser
                {
                    ID2D1SolidColorBrush* brush;
                    ~TransparentBrushReleaser() { if (brush) brush->Release(); }
                } transparentBrushReleaser{transparentBrush};

                const FLOAT runOriginX = static_cast<FLOAT>(minLeftPx) / pixelsPerDip;
                const FLOAT runOriginY = static_cast<FLOAT>(minTopPx) / pixelsPerDip;

                struct ScopedDrawGlyphRunFlag
                {
                    explicit ScopedDrawGlyphRunFlag(bool& inDrawFlag) : flag(inDrawFlag) { flag = true; }
                    ~ScopedDrawGlyphRunFlag() { flag = false; }
                    bool& flag;
                } scopedFlag(g_insideDWriteDrawGlyphRun);

                const bool runBlitOk = Blender::Instance().BlitToD2DTarget(
                    renderTarget,
                    runOriginX,
                    runOriginY,
                    stagedRun,
                    textColor,
                    renderCfg.gamma,
                    renderCfg.oledGammaOutput,
                    renderCfg.toneParityV2Enabled,
                    pixelsPerDip);
                if (!runBlitOk)
                {
                    return forwardWithFallback(DrawGlyphRunPreflightState::Fail,
                                               FallbackReason::TransactionCommitFailed);
                }

                transaction.committed = true;

                // D2D/DComp shadow buffer fix:
                // Forward with a transparent brush to register dirty rects
                // without drawing stock ClearType text over committed output.
                if (SUCCEEDED(transparentBrushHr) && transparentBrush)
                {
                    ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                        measuringMode, glyphRun, glyphRunDescription,
                                        transparentBrush);
                }
                else
                {
                    PureTypeLog(
                        "DWrite DrawGlyphRun: committed run kept without original forward; transparent brush unavailable hr=0x%08lx",
                        static_cast<unsigned long>(transparentBrushHr));
                }

                MaybeLogGuardrailCounters(renderCfg);
                return S_OK;
            }

            // IDWriteTextRenderer1 extension for DComp and Modern UI caching
            HRESULT STDMETHODCALLTYPE DrawGlyphRun(
                void* clientDrawingContext,
                FLOAT baselineOriginX,
                FLOAT baselineOriginY,
                DWRITE_GLYPH_ORIENTATION_ANGLE orientationAngle,
                DWRITE_MEASURING_MODE measuringMode,
                DWRITE_GLYPH_RUN const* glyphRun,
                DWRITE_GLYPH_RUN_DESCRIPTION const* glyphRunDescription,
                IUnknown* clientDrawingEffect) override
            {
                if (orientationAngle != DWRITE_GLYPH_ORIENTATION_ANGLE_0_DEGREES)
                {
                    if (!m_original1) return E_NOTIMPL;
                    return m_original1->DrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                                     orientationAngle, measuringMode, glyphRun,
                                                     glyphRunDescription, clientDrawingEffect);
                }

                return DrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                    measuringMode, glyphRun, glyphRunDescription, clientDrawingEffect);
            }

            HRESULT STDMETHODCALLTYPE DrawUnderline(void* clientDrawingContext,
                                                    FLOAT baselineOriginX,
                                                    FLOAT baselineOriginY,
                                                    DWRITE_UNDERLINE const* underline,
                                                    IUnknown* clientDrawingEffect) override
            {
                if (!m_original) return E_NOTIMPL;
                return m_original->DrawUnderline(clientDrawingContext, baselineOriginX, baselineOriginY,
                                                 underline, clientDrawingEffect);
            }

            HRESULT STDMETHODCALLTYPE DrawStrikethrough(void* clientDrawingContext,
                                                        FLOAT baselineOriginX,
                                                        FLOAT baselineOriginY,
                                                        DWRITE_STRIKETHROUGH const* strikethrough,
                                                        IUnknown* clientDrawingEffect) override
            {
                if (!m_original) return E_NOTIMPL;
                return m_original->DrawStrikethrough(clientDrawingContext, baselineOriginX, baselineOriginY,
                                                     strikethrough, clientDrawingEffect);
            }

            HRESULT STDMETHODCALLTYPE DrawInlineObject(void* clientDrawingContext,
                                                       FLOAT originX,
                                                       FLOAT originY,
                                                       IDWriteInlineObject* inlineObject,
                                                       BOOL isSideways,
                                                       BOOL isRightToLeft,
                                                       IUnknown* clientDrawingEffect) override
            {
                if (!m_original) return E_NOTIMPL;
                return m_original->DrawInlineObject(clientDrawingContext, originX, originY, inlineObject,
                                                    isSideways, isRightToLeft, clientDrawingEffect);
            }

            // IDWriteTextRenderer1 overrides
            HRESULT STDMETHODCALLTYPE DrawUnderline(void* clientDrawingContext,
                                                    FLOAT baselineOriginX,
                                                    FLOAT baselineOriginY,
                                                    DWRITE_GLYPH_ORIENTATION_ANGLE orientationAngle,
                                                    DWRITE_UNDERLINE const* underline,
                                                    IUnknown* clientDrawingEffect) override
            {
                if (!m_original1) return E_NOTIMPL;
                return m_original1->DrawUnderline(clientDrawingContext, baselineOriginX, baselineOriginY,
                                                  orientationAngle, underline, clientDrawingEffect);
            }

            HRESULT STDMETHODCALLTYPE DrawStrikethrough(void* clientDrawingContext,
                                                        FLOAT baselineOriginX,
                                                        FLOAT baselineOriginY,
                                                        DWRITE_GLYPH_ORIENTATION_ANGLE orientationAngle,
                                                        DWRITE_STRIKETHROUGH const* strikethrough,
                                                        IUnknown* clientDrawingEffect) override
            {
                if (!m_original1) return E_NOTIMPL;
                return m_original1->DrawStrikethrough(clientDrawingContext, baselineOriginX, baselineOriginY,
                                                      orientationAngle, strikethrough, clientDrawingEffect);
            }

            HRESULT STDMETHODCALLTYPE DrawInlineObject(void* clientDrawingContext,
                                                       FLOAT originX,
                                                       FLOAT originY,
                                                       DWRITE_GLYPH_ORIENTATION_ANGLE orientationAngle,
                                                       IDWriteInlineObject* inlineObject,
                                                       BOOL isSideways,
                                                       BOOL isRightToLeft,
                                                       IUnknown* clientDrawingEffect) override
            {
                if (!m_original1) return E_NOTIMPL;
                return m_original1->DrawInlineObject(clientDrawingContext, originX, originY,
                                                     orientationAngle, inlineObject,
                                                     isSideways, isRightToLeft, clientDrawingEffect);
            }

        private:
            HRESULT ForwardDrawGlyphRun(void* clientDrawingContext,
                                        FLOAT baselineOriginX,
                                        FLOAT baselineOriginY,
                                        DWRITE_MEASURING_MODE measuringMode,
                                        DWRITE_GLYPH_RUN const* glyphRun,
                                        DWRITE_GLYPH_RUN_DESCRIPTION const* glyphRunDescription,
                                        IUnknown* clientDrawingEffect)
            {
                if (!m_original) return E_NOTIMPL;
                return m_original->DrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                                measuringMode, glyphRun, glyphRunDescription,
                                                clientDrawingEffect);
            }

            ULONG m_refCount = 1;
            IDWriteTextRenderer* m_original = nullptr;
            IDWriteTextRenderer1* m_original1 = nullptr;
        };

        HRESULT STDMETHODCALLTYPE HookedTextLayoutDraw(IDWriteTextLayout* textLayout,
                                                       void* clientDrawingContext,
                                                       IDWriteTextRenderer* renderer,
                                                       FLOAT originX,
                                                       FLOAT originY);

        bool InstallTextLayoutDrawHook(IDWriteTextLayout* textLayout)
        {
            if (!textLayout) return false;
            std::lock_guard<std::mutex> lock(g_hookMutex);

            if (g_origTextLayoutDraw && g_targetTextLayoutDraw)
            {
                return true;
            }

            g_targetTextLayoutDraw = reinterpret_cast<LPVOID>(
                GetVtableMethod<TextLayoutDraw_t>(textLayout, kTextLayoutDrawVtableIndex));
            if (!g_targetTextLayoutDraw)
            {
                PureTypeLog("InstallTextLayoutDrawHook: vtable slot lookup failed");
                return false;
            }

            MH_STATUS status = MH_CreateHook(g_targetTextLayoutDraw,
                                             reinterpret_cast<LPVOID>(&HookedTextLayoutDraw),
                                             reinterpret_cast<LPVOID*>(&g_origTextLayoutDraw));
            if (status != MH_OK)
            {
                PureTypeLog("MH_CreateHook(IDWriteTextLayout::Draw) failed: %s", MH_StatusToString(status));
                return false;
            }

            status = MH_EnableHook(g_targetTextLayoutDraw);
            if (status != MH_OK && status != MH_ERROR_ENABLED)
            {
                PureTypeLog("MH_EnableHook(IDWriteTextLayout::Draw) failed: %s", MH_StatusToString(status));
                return false;
            }

            PureTypeLog("Hooked IDWriteTextLayout::Draw at %p", g_targetTextLayoutDraw);
            return true;
        }

        HRESULT STDMETHODCALLTYPE HookedCreateTextLayout(IDWriteFactory* factory,
                                                         WCHAR const* string,
                                                         UINT32 stringLength,
                                                         IDWriteTextFormat* textFormat,
                                                         FLOAT maxWidth,
                                                         FLOAT maxHeight,
                                                         IDWriteTextLayout** textLayout)
        {
            if (!g_origCreateTextLayout) return E_FAIL;

            HRESULT hr = g_origCreateTextLayout(factory, string, stringLength, textFormat, maxWidth,
                                                maxHeight, textLayout);
            if (SUCCEEDED(hr) && textLayout && *textLayout)
            {
                InstallTextLayoutDrawHook(*textLayout);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE HookedCreateGdiCompatibleTextLayout(IDWriteFactory* factory,
                                                                      WCHAR const* string,
                                                                      UINT32 stringLength,
                                                                      IDWriteTextFormat* textFormat,
                                                                      FLOAT layoutWidth,
                                                                      FLOAT layoutHeight,
                                                                      FLOAT pixelsPerDip,
                                                                      DWRITE_MATRIX const* transform,
                                                                      BOOL useGdiNatural,
                                                                      IDWriteTextLayout** textLayout)
        {
            if (!g_origCreateGdiCompatibleTextLayout) return E_FAIL;

            HRESULT hr = g_origCreateGdiCompatibleTextLayout(factory, string, stringLength, textFormat,
                                                             layoutWidth, layoutHeight, pixelsPerDip,
                                                             transform, useGdiNatural, textLayout);
            if (SUCCEEDED(hr) && textLayout && *textLayout)
            {
                InstallTextLayoutDrawHook(*textLayout);
            }
            return hr;
        }

        bool InstallFactoryMethodHooks(IDWriteFactory* factory)
        {
            if (!factory) return false;

            std::lock_guard<std::mutex> lock(g_hookMutex);
            auto* createTextLayoutTarget = reinterpret_cast<LPVOID>(
                GetVtableMethod<CreateTextLayout_t>(factory, kFactoryCreateTextLayoutVtableIndex));
            auto* createGdiTextLayoutTarget = reinterpret_cast<LPVOID>(GetVtableMethod<
                CreateGdiCompatibleTextLayout_t>(
                factory, kFactoryCreateGdiCompatibleTextLayoutVtableIndex));

            if (!createTextLayoutTarget || !createGdiTextLayoutTarget)
            {
                PureTypeLog("InstallFactoryMethodHooks: factory vtable lookup failed");
                return false;
            }

            if (!g_origCreateTextLayout)
            {
                g_targetCreateTextLayout = createTextLayoutTarget;
                MH_STATUS status = MH_CreateHook(g_targetCreateTextLayout,
                                                 reinterpret_cast<LPVOID>(&HookedCreateTextLayout),
                                                 reinterpret_cast<LPVOID*>(&g_origCreateTextLayout));
                if (status != MH_OK)
                {
                    PureTypeLog("MH_CreateHook(IDWriteFactory::CreateTextLayout) failed: %s",
                                MH_StatusToString(status));
                    return false;
                }
                status = MH_EnableHook(g_targetCreateTextLayout);
                if (status != MH_OK && status != MH_ERROR_ENABLED)
                {
                    PureTypeLog("MH_EnableHook(IDWriteFactory::CreateTextLayout) failed: %s",
                                MH_StatusToString(status));
                    return false;
                }
            }

            if (!g_origCreateGdiCompatibleTextLayout)
            {
                g_targetCreateGdiCompatibleTextLayout = createGdiTextLayoutTarget;
                MH_STATUS status = MH_CreateHook(g_targetCreateGdiCompatibleTextLayout,
                                                 reinterpret_cast<LPVOID>(&HookedCreateGdiCompatibleTextLayout),
                                                 reinterpret_cast<LPVOID*>(&g_origCreateGdiCompatibleTextLayout));
                if (status != MH_OK)
                {
                    PureTypeLog("MH_CreateHook(IDWriteFactory::CreateGdiCompatibleTextLayout) failed: %s",
                                MH_StatusToString(status));
                    return false;
                }
                status = MH_EnableHook(g_targetCreateGdiCompatibleTextLayout);
                if (status != MH_OK && status != MH_ERROR_ENABLED)
                {
                    PureTypeLog("MH_EnableHook(IDWriteFactory::CreateGdiCompatibleTextLayout) failed: %s",
                                MH_StatusToString(status));
                    return false;
                }
            }

            return true;
        }

        bool PrimeExistingDWriteObjects()
        {
            HMODULE hDwrite = GetModuleHandleW(L"dwrite.dll");
            if (!hDwrite) return false;

            auto createFactory =
                reinterpret_cast<DWriteCreateFactory_t>(GetProcAddress(hDwrite, "DWriteCreateFactory"));
            if (!createFactory) return false;

            IDWriteFactory* factory = nullptr;
            HRESULT hr = createFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&factory));
            if (FAILED(hr) || !factory)
            {
                PureTypeLog("PrimeExistingDWriteObjects: temporary factory creation failed (0x%08X)",
                            hr);
                return false;
            }

            bool ok = InstallFactoryMethodHooks(factory);

            IDWriteTextFormat* textFormat = nullptr;
            hr = factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f,
                                           L"en-us", &textFormat);
            if (SUCCEEDED(hr) && textFormat)
            {
                IDWriteTextLayout* textLayout = nullptr;
                hr = factory->CreateTextLayout(L"x", 1, textFormat, 32.0f, 32.0f, &textLayout);
                if (SUCCEEDED(hr) && textLayout)
                {
                    ok = InstallTextLayoutDrawHook(textLayout) && ok;
                    textLayout->Release();
                }
                else
                {
                    PureTypeLog("PrimeExistingDWriteObjects: temporary text layout creation failed (0x%08X)",
                                hr);
                    ok = false;
                }
                textFormat->Release();
            }
            else
            {
                PureTypeLog("PrimeExistingDWriteObjects: temporary text format creation failed (0x%08X)",
                            hr);
                ok = false;
            }

            factory->Release();
            return ok;
        }

        HRESULT WINAPI HookedDWriteCreateFactory(DWRITE_FACTORY_TYPE factoryType,
                                                 REFIID iid,
                                                 IUnknown** factory)
        {
            (void)iid;

            if (!g_origDWriteCreateFactory) return E_FAIL;

            HRESULT hr = g_origDWriteCreateFactory(factoryType, iid, factory);
            if (FAILED(hr) || !factory || !*factory) return hr;

            IDWriteFactory* baseFactory = nullptr;
            HRESULT qi = (*factory)->QueryInterface(__uuidof(IDWriteFactory),
                                                    reinterpret_cast<void**>(&baseFactory));
            if (SUCCEEDED(qi) && baseFactory)
            {
                InstallFactoryMethodHooks(baseFactory);
                baseFactory->Release();
            }

            return hr;
        }

        HRESULT STDMETHODCALLTYPE HookedTextLayoutDraw(IDWriteTextLayout* textLayout,
                                                       void* clientDrawingContext,
                                                       IDWriteTextRenderer* renderer,
                                                       FLOAT originX,
                                                       FLOAT originY)
        {
            if (!g_origTextLayoutDraw || !renderer)
            {
                return g_origTextLayoutDraw
                           ? g_origTextLayoutDraw(textLayout, clientDrawingContext, renderer, originX, originY)
                           : E_FAIL;
            }

            auto* wrappedRenderer = new(std::nothrow) CustomTextRenderer(renderer);
            if (!wrappedRenderer)
            {
                return g_origTextLayoutDraw(textLayout, clientDrawingContext, renderer, originX, originY);
            }

            HRESULT hr =
                g_origTextLayoutDraw(textLayout, clientDrawingContext, wrappedRenderer, originX, originY);
            wrappedRenderer->Release();
            return hr;
        }
    }

    bool InstallDWriteHooks(const bool primeExistingObjects)
    {
        HMODULE hDwrite = GetModuleHandleW(L"dwrite.dll");
        if (!hDwrite)
        {
            hDwrite = LoadLibraryW(L"dwrite.dll");
        }
        if (!hDwrite)
        {
            PureTypeLog("InstallDWriteHooks: dwrite.dll not available");
            return false;
        }

        g_targetDWriteCreateFactory = reinterpret_cast<LPVOID>(GetProcAddress(hDwrite, "DWriteCreateFactory"));
        if (!g_targetDWriteCreateFactory)
        {
            PureTypeLog("InstallDWriteHooks: DWriteCreateFactory not found");
            return false;
        }

        MH_STATUS status = MH_CreateHook(g_targetDWriteCreateFactory,
                                         reinterpret_cast<LPVOID>(&HookedDWriteCreateFactory),
                                         reinterpret_cast<LPVOID*>(&g_origDWriteCreateFactory));
        if (status != MH_OK)
        {
            PureTypeLog("MH_CreateHook(DWriteCreateFactory) failed: %s", MH_StatusToString(status));
            return false;
        }

        if (primeExistingObjects && !PrimeExistingDWriteObjects())
        {
            PureTypeLog("InstallDWriteHooks: initial DWrite object priming incomplete");
        }

        return true;
    }

    void RemoveDWriteHooks()
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);

        if (g_targetTextLayoutDraw)
        {
            MH_RemoveHook(g_targetTextLayoutDraw);
            g_targetTextLayoutDraw = nullptr;
            g_origTextLayoutDraw = nullptr;
        }

        if (g_targetCreateGdiCompatibleTextLayout)
        {
            MH_RemoveHook(g_targetCreateGdiCompatibleTextLayout);
            g_targetCreateGdiCompatibleTextLayout = nullptr;
            g_origCreateGdiCompatibleTextLayout = nullptr;
        }

        if (g_targetCreateTextLayout)
        {
            MH_RemoveHook(g_targetCreateTextLayout);
            g_targetCreateTextLayout = nullptr;
            g_origCreateTextLayout = nullptr;
        }

        if (g_targetDWriteCreateFactory)
        {
            MH_RemoveHook(g_targetDWriteCreateFactory);
            g_targetDWriteCreateFactory = nullptr;
            g_origDWriteCreateFactory = nullptr;
        }
    }
}
