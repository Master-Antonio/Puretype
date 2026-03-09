#include "rasterizer/ft_rasterizer.h"
#include "config.h"
#include "puretype.h"
#include <cstring>
#include <algorithm>
#include FT_LCD_FILTER_H
#include FT_MULTIPLE_MASTERS_H

extern void PureTypeLog(const char* fmt, ...);

namespace puretype
{
    
    std::atomic<int> g_activeHookCount{0};

    
    
    
    
    
    
    
    
    
    
    
    
    
    static constexpr uint8_t kPhaseCount = 3;
    static constexpr FT_Pos kPhaseStep = 21; 

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
            out = it->second->bitmap; 
            return true;
        }
        return false;
    }

    const GlyphBitmap* GlyphCache::Put(const GlyphCacheKey& key, GlyphBitmap&& bitmap, size_t extraBytes)
    {
        std::lock_guard lock(m_mutex);

        
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
            
            m_faceLRU.splice(m_faceLRU.begin(), m_faceLRU, it->second.lruIter);
            return it->second.face;
        }

        
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
        const ConfigData& cfg,
        uint16_t fontWeight,
        uint8_t phaseX,
        uint8_t phaseY,
        const VariableAxisOverrides& axisOverrides)
    {
        
        const uint8_t normPhaseX = NormalizePhase(phaseX);
        const uint8_t normPhaseY = NormalizePhase(phaseY);

        GlyphCacheKey key{
            fontPath, glyphIndex, pixelSize, fontWeight,
            normPhaseX,
            normPhaseY,
            static_cast<uint8_t>(cfg.panelType),
            cfg.enableSubpixelHinting,
            axisOverrides
        };

        
        
        
        
        
        
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
            
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_ftLibrary) return nullptr;

            FT_Face face = GetOrLoadFace(fontPath);
            if (!face) return nullptr;

            FT_Set_Pixel_Sizes(face, 0, pixelSize);

            
            
            if (axisOverrides.HasOverrides() && FT_HAS_MULTIPLE_MASTERS(face))
            {
                FT_MM_Var* mmVar = nullptr;
                if (FT_Get_MM_Var(face, &mmVar) == 0 && mmVar)
                {
                    
                    std::vector<FT_Fixed> coords(mmVar->num_axis);
                    for (FT_UInt a = 0; a < mmVar->num_axis; ++a)
                        coords[a] = mmVar->axis[a].def;

                    
                    for (FT_UInt a = 0; a < mmVar->num_axis; ++a)
                    {
                        const FT_ULong tag = mmVar->axis[a].tag;
                        if (tag == FT_MAKE_TAG('w', 'g', 'h', 't') && axisOverrides.weight != 0)
                        {
                            coords[a] = static_cast<FT_Fixed>(axisOverrides.weight) << 16;
                        }
                        else if (tag == FT_MAKE_TAG('o', 'p', 's', 'z') && axisOverrides.opticalSize != 0.f)
                        {
                            coords[a] = static_cast<FT_Fixed>(axisOverrides.opticalSize * 65536.f);
                        }
                    }

                    FT_Set_Var_Design_Coordinates(face, mmVar->num_axis, coords.data());
                    FT_Done_MM_Var(m_ftLibrary, mmVar);

                    
                    FT_Set_Pixel_Sizes(face, 0, pixelSize);
                }
            }

            
            
            
            
            
            
            
            
            
            
            
            
            
            
            FT_Int32 loadFlags = FT_LOAD_DEFAULT;
            const bool isQdPanel = (cfg.panelType == PanelType::QD_OLED_GEN1 ||
                cfg.panelType == PanelType::QD_OLED_GEN3 ||
                cfg.panelType == PanelType::QD_OLED_GEN4);
            if (isQdPanel)
            {
                
                loadFlags |= FT_LOAD_TARGET_NORMAL;
            }
            else
            {
                
                loadFlags |= FT_LOAD_TARGET_LCD;
            }

            if (cfg.enableSubpixelHinting)
                loadFlags |= FT_LOAD_FORCE_AUTOHINT;

            
            
            
            const FT_Pos oledPhaseX = isQdPanel ? 32 : 24;
            const FT_Pos oledPhaseY = isQdPanel ? 21 : 0;

            FT_Vector phaseVec;
            
            
            phaseVec.x = (normPhaseX * kPhaseStep) + oledPhaseX;
            phaseVec.y = (normPhaseY * kPhaseStep) + oledPhaseY;

            FT_Set_Transform(face, nullptr, &phaseVec);

            FT_Error error = FT_Load_Glyph(face, glyphIndex, loadFlags);
            if (error) return nullptr;

            
            
            error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD);
            if (error) return nullptr;

            FT_Bitmap& ftBmp = face->glyph->bitmap;

            if (ftBmp.pixel_mode != FT_PIXEL_MODE_LCD &&
                ftBmp.pixel_mode != FT_PIXEL_MODE_GRAY)
            {
                return nullptr;
            }

            const bool isGray = (ftBmp.pixel_mode == FT_PIXEL_MODE_GRAY);

            
            
            const int origWidth = isGray
                                      ? static_cast<int>(ftBmp.width) * 3
                                      : static_cast<int>(ftBmp.width);
            const int origHeight = static_cast<int>(ftBmp.rows);
            const int origPitch = isGray
                                      ? static_cast<int>(ftBmp.width) * 3
                                      : std::abs(ftBmp.pitch);

            
            bearingX = face->glyph->bitmap_left * 3; 
            bearingY = face->glyph->bitmap_top; 
            advanceX = static_cast<int>(face->glyph->advance.x >> 6) * 3;

            
            
            padLeftSubpixels = 3;
            padTopPixels = 1;

            bmpWidth = origWidth + padLeftSubpixels * 2;
            bmpHeight = origHeight + padTopPixels * 2;

            
            
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
        } 

        if (rawData.empty() && (bmpWidth > 0 || bmpHeight > 0)) return nullptr;

        
        GlyphBitmap bmp;
        bmp.fontHash = std::hash<std::string>{}(fontPath);
        bmp.glyphIndex = glyphIndex;
        bmp.pixelSize = pixelSize;
        bmp.fontWeight = fontWeight;
        bmp.phaseX = normPhaseX;
        bmp.phaseY = normPhaseY;
        bmp.width = bmpWidth;
        bmp.height = bmpHeight;
        bmp.pitch = bmpPitch; 
        bmp.bearingX = bearingX; 
        bmp.bearingY = bearingY; 
        bmp.advanceX = advanceX;
        bmp.padLeft = padLeftSubpixels; 
        bmp.padTop = padTopPixels; 
        bmp.data = std::move(rawData);

        return m_cache.Put(key, std::move(bmp), fontPath.size());
    }

    std::vector<FTRasterizer::PositionedGlyph> FTRasterizer::RasterizeGlyphRun(
        const std::string& fontPath,
        const uint16_t* glyphIndices,
        uint32_t glyphCount,
        uint32_t pixelSize,
        const ConfigData& cfg,
        const int* lpDx,
        uint16_t fontWeight,
        const uint8_t* fractionalPhaseX,
        const uint8_t* fractionalPhaseY,
        const VariableAxisOverrides& axisOverrides)
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
                fontPath, glyphIndices[i], pixelSize, cfg, fontWeight, phaseX, phaseY, axisOverrides);
            if (!bmp)
            {
                if (lpDx) currentX += lpDx[i] * 3;
                continue;
            }

            
            
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
} 
