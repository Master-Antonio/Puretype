#include "rasterizer/ft_rasterizer.h"
#include "config.h"
#include "puretype.h"
#include <cstring>
#include <algorithm>
#include FT_LCD_FILTER_H

extern void PureTypeLog(const char* fmt, ...);

namespace puretype
{
    // Definition of the global hook reference counter (declared in puretype.h).
    std::atomic<int> g_activeHookCount{0};

    // -------------------------------------------------------------------------
    // Phase normalization.
    //
    // The subpixel grid has EXACTLY 3 subpixels per physical pixel.
    // Correct phase range is [0, 2] with a step of 64/3 ≈ 21 FT fractional units.
    //
    // Using 4 phases (& 0x03) produces steps of 16 FT units → 0, 16, 32, 48.
    // These do NOT align with subpixel centers (0, 21.3, 42.6), causing every
    // glyph to be rasterized at a suboptimally-shifted position.
    //
    // Using 3 phases (% 3) with step 21 → 0, 21, 42 → aligns exactly to the
    // three subpixel positions within each physical pixel.
    // -------------------------------------------------------------------------
    static constexpr uint8_t kPhaseCount = 3;
    static constexpr FT_Pos kPhaseStep = 21; // 64 FT units / 3 subpixels ≈ 21

    static uint8_t NormalizePhase(uint8_t raw)
    {
        return static_cast<uint8_t>(raw % kPhaseCount);
    }

    FTRasterizer& FTRasterizer::Instance()
    {
        static FTRasterizer instance;
        return instance;
    }

    FTRasterizer::FTRasterizer() = default;

    FTRasterizer::~FTRasterizer()
    {
        Shutdown();
    }

    const GlyphBitmap* GlyphCache::TryGet(const GlyphCacheKey& key)
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_cacheMap.find(key); it != m_cacheMap.end())
        {
            m_cacheList.splice(m_cacheList.begin(), m_cacheList, it->second);
            return &it->second->bitmap;
        }
        return nullptr;
    }

    bool GlyphCache::TryGetCopy(const GlyphCacheKey& key, GlyphBitmap& out)
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_cacheMap.find(key); it != m_cacheMap.end())
        {
            m_cacheList.splice(m_cacheList.begin(), m_cacheList, it->second);
            out = it->second->bitmap; // copy while mutex is held
            return true;
        }
        return false;
    }

    const GlyphBitmap* GlyphCache::Put(const GlyphCacheKey& key, GlyphBitmap&& bitmap, size_t extraBytes)
    {
        std::lock_guard lock(m_mutex);

        // Check if another thread inserted it while we were rasterizing.
        if (auto it = m_cacheMap.find(key); it != m_cacheMap.end())
        {
            m_cacheList.splice(m_cacheList.begin(), m_cacheList, it->second);
            return &it->second->bitmap;
        }

        const size_t entryBytes = bitmap.data.size() + sizeof(GlyphBitmap) + extraBytes;
        while (!m_cacheList.empty() && (m_cacheBytes + entryBytes) > MAX_CACHE_BYTES)
        {
            EvictOldest();
        }

        m_cacheList.push_front(CachedGlyph{key, std::move(bitmap), entryBytes});
        m_cacheBytes += entryBytes;
        m_cacheMap[key] = m_cacheList.begin();

        return &m_cacheList.front().bitmap;
    }

    void GlyphCache::Clear()
    {
        std::lock_guard lock(m_mutex);
        m_cacheList.clear();
        m_cacheMap.clear();
        m_cacheBytes = 0;
    }

    void GlyphCache::EvictOldest()
    {
        if (m_cacheList.empty()) return;
        const auto& oldest = m_cacheList.back();
        m_cacheBytes = (m_cacheBytes >= oldest.bytes) ? (m_cacheBytes - oldest.bytes) : 0;
        m_cacheMap.erase(oldest.key);
        m_cacheList.pop_back();
    }

    bool FTRasterizer::Initialize()
    {
        std::lock_guard lock(m_mutex);

        if (m_ftLibrary) return true;

        if (const FT_Error error = FT_Init_FreeType(&m_ftLibrary))
        {
            PureTypeLog("FT_Init_FreeType failed with error %d", error);
            m_ftLibrary = nullptr;
            return false;
        }

        // We apply panel-specific filtering ourselves after rasterization.
        FT_Library_SetLcdFilter(m_ftLibrary, FT_LCD_FILTER_NONE);

        PureTypeLog("FreeType library initialized (version %d.%d.%d)",
                    FREETYPE_MAJOR, FREETYPE_MINOR, FREETYPE_PATCH);
        return true;
    }

    void FTRasterizer::Shutdown()
    {
        std::lock_guard lock(m_mutex);

        m_cache.Clear();

        for (auto& [path, cached] : m_faceCache)
        {
            if (cached.face) FT_Done_Face(cached.face);
        }
        m_faceCache.clear();
        m_faceLRU.clear();

        if (m_ftLibrary)
        {
            FT_Done_FreeType(m_ftLibrary);
            m_ftLibrary = nullptr;
        }
    }

    void FTRasterizer::EvictOldestFace()
    {
        if (m_faceLRU.empty()) return;
        const std::string& oldestPath = m_faceLRU.back();
        if (const auto it = m_faceCache.find(oldestPath); it != m_faceCache.end())
        {
            if (it->second.face)
            {
                PureTypeLog("Face cache evicting: %s", oldestPath.c_str());
                FT_Done_Face(it->second.face);
            }
            m_faceCache.erase(it);
        }
        m_faceLRU.pop_back();
    }

    FT_Face FTRasterizer::GetOrLoadFace(const std::string& fontPath)
    {
        if (const auto it = m_faceCache.find(fontPath); it != m_faceCache.end())
        {
            // Cache hit — promote to MRU position.
            m_faceLRU.splice(m_faceLRU.begin(), m_faceLRU, it->second.lruIter);
            return it->second.face;
        }

        // Evict LRU face if at capacity.
        while (m_faceCache.size() >= MAX_FACE_CACHE_ENTRIES)
        {
            EvictOldestFace();
        }

        FT_Face face = nullptr;
        if (const FT_Error error = FT_New_Face(m_ftLibrary, fontPath.c_str(), 0, &face))
        {
            PureTypeLog("FT_New_Face failed for '%s' (error %d)", fontPath.c_str(), error);
            return nullptr;
        }

        PureTypeLog("Loaded font face: %s (%s %s)",
                    fontPath.c_str(),
                    face->family_name ? face->family_name : "?",
                    face->style_name ? face->style_name : "?");

        m_faceLRU.push_front(fontPath);
        CachedFace cached;
        cached.face = face;
        cached.lruIter = m_faceLRU.begin();
        m_faceCache[fontPath] = cached;

        return face;
    }

    uint32_t FTRasterizer::GetGlyphIndex(const std::string& fontPath, uint32_t charCode)
    {
        std::lock_guard lock(m_mutex);
        if (!m_ftLibrary) return 0;

        FT_Face face = GetOrLoadFace(fontPath);
        if (!face) return 0;

        return FT_Get_Char_Index(face, charCode);
    }

    const GlyphBitmap* FTRasterizer::RasterizeGlyph(
        const std::string& fontPath,
        uint32_t glyphIndex,
        uint32_t pixelSize,
        uint16_t fontWeight,
        uint8_t phaseX,
        uint8_t phaseY)
    {
        const auto& cfg = Config::Instance().Data();

        // Normalize phases to [0, kPhaseCount-1] = [0, 2].
        const uint8_t normPhaseX = NormalizePhase(phaseX);
        const uint8_t normPhaseY = NormalizePhase(phaseY);

        GlyphCacheKey key{
            fontPath, glyphIndex, pixelSize, fontWeight,
            normPhaseX,
            normPhaseY,
            static_cast<uint8_t>(cfg.panelType),
            cfg.enableSubpixelHinting
        };

        // 1. Thread-safe cache check — copy under lock to prevent use-after-free.
        //
        // TryGet returns a raw pointer that becomes dangling if a concurrent Put()
        // evicts the entry between the mutex release and the caller's dereference.
        // TryGetCopy copies the bitmap while holding the lock; the thread_local
        // staging slot avoids a heap allocation per lookup.
        {
            static thread_local GlyphBitmap tl_cachedBitmap;
            if (m_cache.TryGetCopy(key, tl_cachedBitmap))
                return &tl_cachedBitmap;
        }

        int bmpWidth = 0;
        int bmpHeight = 0;
        int bmpPitch = 0;
        int bearingX = 0;
        int bearingY = 0;
        int advanceX = 0;
        int padLeftSubpixels = 0;
        int padTopPixels = 0;
        std::vector<uint8_t> rawData;

        {
            // 2. Hold the FreeType lock only while calling FT_ APIs.
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_ftLibrary) return nullptr;

            FT_Face face = GetOrLoadFace(fontPath);
            if (!face) return nullptr;

            FT_Set_Pixel_Sizes(face, 0, pixelSize);


            // FT_LOAD_TARGET_LCD optimises stem positions for HORIZONTAL RGB stripe
            // layout — it biases vertical stem edges toward the nearest horizontal
            // subpixel boundary. This is correct for RWBG and RGWB panels where
            // subpixels run in a row left-to-right.
            //
            // For QD-OLED (triangular geometry) the subpixels are NOT in a stripe —
            // they form triangles with a ±0.5 px vertical offset between rows.
            // FT_LOAD_TARGET_LCD would push stem edges to wrong positions, creating
            // visible misalignment artifacts on diagonal and thin strokes.
            //
            // FT_LOAD_TARGET_NORMAL uses isotropic hinting (equal priority on both
            // axes), which is the correct baseline for triangular subpixel layouts.
            // We still render with FT_RENDER_MODE_LCD to obtain the 3× horizontal
            // oversampled bitmap that our TriangularFilter expects.
            FT_Int32 loadFlags = FT_LOAD_DEFAULT;
            const bool isQdPanel = (cfg.panelType == PanelType::QD_OLED_GEN1 ||
                cfg.panelType == PanelType::QD_OLED_GEN3 ||
                cfg.panelType == PanelType::QD_OLED_GEN4);
            if (isQdPanel)
            {
                // Isotropic hinting — no horizontal-stripe bias for triangular layout.
                loadFlags |= FT_LOAD_TARGET_NORMAL;
            }
            else
            {
                // RWBG / RGWB stripe panels — horizontal hinting is correct.
                loadFlags |= FT_LOAD_TARGET_LCD;
            }

            if (cfg.enableSubpixelHinting)
                loadFlags |= FT_LOAD_FORCE_AUTOHINT;

            // Panel-type base phase offsets, in FT 26.6 fractional units.
            // QD-OLED (all gens): 0.5px H + ~0.33px V for triangular subpixel geometry.
            // Standard RWBG/RGWB: 0.375px H, no V offset.
            const FT_Pos oledPhaseX = isQdPanel ? 32 : 24;
            const FT_Pos oledPhaseY = isQdPanel ? 21 : 0;

            FT_Vector phaseVec;
            // kPhaseStep = 21 FT units ≈ 64/3 aligns each phase to one subpixel center.
            // Old code used step=16 (4 phases), which is incoherent with the 3-subpixel grid.
            phaseVec.x = (normPhaseX * kPhaseStep) + oledPhaseX;
            phaseVec.y = (normPhaseY * kPhaseStep) + oledPhaseY;

            FT_Set_Transform(face, nullptr, &phaseVec);

            FT_Error error = FT_Load_Glyph(face, glyphIndex, loadFlags);
            if (error) return nullptr;

            // Always render in LCD mode to get the 3× horizontal oversampling
            // regardless of the load target / hinting mode chosen above.
            error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD);
            if (error) return nullptr;

            FT_Bitmap& ftBmp = face->glyph->bitmap;

            if (ftBmp.pixel_mode != FT_PIXEL_MODE_LCD &&
                ftBmp.pixel_mode != FT_PIXEL_MODE_GRAY)
            {
                return nullptr;
            }

            const bool isGray = (ftBmp.pixel_mode == FT_PIXEL_MODE_GRAY);

            // In LCD mode: ftBmp.width is already in bytes (3 bytes per logical pixel).
            // In gray mode: expand each sample to 3 identical bytes.
            const int origWidth = isGray
                                      ? static_cast<int>(ftBmp.width) * 3
                                      : static_cast<int>(ftBmp.width);
            const int origHeight = static_cast<int>(ftBmp.rows);
            const int origPitch = isGray
                                      ? static_cast<int>(ftBmp.width) * 3
                                      : std::abs(ftBmp.pitch);

            // Store the UNMODIFIED FreeType bearing — padding offset tracked separately.
            bearingX = face->glyph->bitmap_left * 3; // subpixel units
            bearingY = face->glyph->bitmap_top; // pixels above baseline
            advanceX = static_cast<int>(face->glyph->advance.x >> 6) * 3;

            // ADD PADDING: 1 physical pixel (3 subpixels) left/right, 1 pixel top/bottom.
            // This gives the FIR subpixel filter room to spread energy without edge-clipping.
            padLeftSubpixels = 3;
            padTopPixels = 1;

            bmpWidth = origWidth + padLeftSubpixels * 2;
            bmpHeight = origHeight + padTopPixels * 2;

            // 4-byte stride alignment required by GDI DIBs and our blitters.
            // INVARIANT: bmp.pitch is ALWAYS used as the row stride downstream — never bmp.width.
            bmpPitch = (bmpWidth + 3) & ~3;

            const int dataSize = bmpHeight * bmpPitch;
            if (origHeight > 0 && origPitch > 0 && ftBmp.buffer)
            {
                rawData.assign(dataSize, 0u);

                if (isGray)
                {
                    const int srcPitch = std::abs(ftBmp.pitch);
                    for (int row = 0; row < origHeight; ++row)
                    {
                        const uint8_t* srcRow = ftBmp.buffer +
                            (ftBmp.pitch > 0 ? row : (origHeight - 1 - row)) * srcPitch;
                        uint8_t* dstRow = rawData.data() +
                            (row + padTopPixels) * bmpPitch + padLeftSubpixels;

                        for (int col = 0; col < static_cast<int>(ftBmp.width); ++col)
                        {
                            const uint8_t val = srcRow[col];
                            dstRow[col * 3 + 0] = val;
                            dstRow[col * 3 + 1] = val;
                            dstRow[col * 3 + 2] = val;
                        }
                    }
                }
                else
                {
                    if (ftBmp.pitch > 0)
                    {
                        for (int row = 0; row < origHeight; ++row)
                        {
                            const uint8_t* srcRow = ftBmp.buffer + row * std::abs(ftBmp.pitch);
                            uint8_t* dstRow = rawData.data() +
                                (row + padTopPixels) * bmpPitch + padLeftSubpixels;
                            std::memcpy(dstRow, srcRow, origWidth);
                        }
                    }
                    else
                    {
                        const int absPitch = std::abs(ftBmp.pitch);
                        for (int row = 0; row < origHeight; ++row)
                        {
                            const uint8_t* srcRow = ftBmp.buffer + (origHeight - 1 - row) * absPitch;
                            uint8_t* dstRow = rawData.data() +
                                (row + padTopPixels) * bmpPitch + padLeftSubpixels;
                            std::memcpy(dstRow, srcRow, origWidth);
                        }
                    }
                }
            }
        } // release m_mutex

        if (rawData.empty() && (bmpWidth > 0 || bmpHeight > 0)) return nullptr;

        // 3. Build and cache the bitmap outside the FreeType bottleneck.
        GlyphBitmap bmp;
        bmp.fontHash = std::hash<std::string>{}(fontPath);
        bmp.glyphIndex = glyphIndex;
        bmp.pixelSize = pixelSize;
        bmp.fontWeight = fontWeight;
        bmp.phaseX = normPhaseX;
        bmp.phaseY = normPhaseY;
        bmp.width = bmpWidth;
        bmp.height = bmpHeight;
        bmp.pitch = bmpPitch; // ALWAYS use as row stride, never bmp.width
        bmp.bearingX = bearingX; // Unmodified FreeType bearing in subpixel units
        bmp.bearingY = bearingY; // Unmodified FreeType bearing in pixels
        bmp.advanceX = advanceX;
        bmp.padLeft = padLeftSubpixels; // Padding subpixels on the left (for positioning)
        bmp.padTop = padTopPixels; // Padding pixels on the top   (for positioning)
        bmp.data = std::move(rawData);

        return m_cache.Put(key, std::move(bmp), fontPath.size());
    }

    std::vector<FTRasterizer::PositionedGlyph> FTRasterizer::RasterizeGlyphRun(
        const std::string& fontPath,
        const uint16_t* glyphIndices,
        uint32_t glyphCount,
        uint32_t pixelSize,
        const int* lpDx,
        uint16_t fontWeight,
        const uint8_t* fractionalPhaseX,
        const uint8_t* fractionalPhaseY)
    {
        std::vector<PositionedGlyph> result;
        result.reserve(glyphCount);


        static thread_local std::vector<GlyphBitmap> tl_runStorage;
        tl_runStorage.clear();
        tl_runStorage.reserve(glyphCount);

        int currentX = 0;

        for (uint32_t i = 0; i < glyphCount; ++i)
        {
            const uint8_t phaseX = fractionalPhaseX
                                       ? NormalizePhase(fractionalPhaseX[i])
                                       : 0;
            const uint8_t phaseY = fractionalPhaseY
                                       ? NormalizePhase(fractionalPhaseY[i])
                                       : 0;

            const GlyphBitmap* bmp = RasterizeGlyph(
                fontPath, glyphIndices[i], pixelSize, fontWeight, phaseX, phaseY);
            if (!bmp)
            {
                if (lpDx) currentX += lpDx[i] * 3;
                continue;
            }

            // Copy the bitmap into per-run storage so each element has its own
            // stable address that won't be overwritten by subsequent loop iterations.
            tl_runStorage.push_back(*bmp);
            const GlyphBitmap* stableBmp = &tl_runStorage.back();

            PositionedGlyph pg{};
            pg.bitmap = stableBmp;
            pg.offsetX = currentX + stableBmp->bearingX;
            pg.offsetY = 0;
            result.push_back(pg);

            if (lpDx)
            {
                currentX += lpDx[i] * 3;
            }
            else
            {
                currentX += stableBmp->advanceX;
            }
        }

        return result;
    }
} // namespace puretype
