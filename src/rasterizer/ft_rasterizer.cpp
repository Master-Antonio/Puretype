

#include "rasterizer/ft_rasterizer.h"
#include <cstring>
#include <algorithm>

extern void PureTypeLog(const char* fmt, ...);

namespace puretype {

FTRasterizer& FTRasterizer::Instance() {
    static FTRasterizer instance;
    return instance;
}

FTRasterizer::FTRasterizer() = default;

FTRasterizer::~FTRasterizer() {
    Shutdown();
}

#include FT_LCD_FILTER_H

bool FTRasterizer::Initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_ftLibrary) return true; 

    FT_Error error = FT_Init_FreeType(&m_ftLibrary);
    if (error) {
        PureTypeLog("FT_Init_FreeType failed with error %d", error);
        m_ftLibrary = nullptr;
        return false;
    }

    
    
    
    
    FT_Library_SetLcdFilter(m_ftLibrary, FT_LCD_FILTER_NONE);

    PureTypeLog("FreeType library initialized (version %d.%d.%d)",
                  FREETYPE_MAJOR, FREETYPE_MINOR, FREETYPE_PATCH);
    return true;
}

void FTRasterizer::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);

    
    m_cacheList.clear();
    m_cacheMap.clear();

    
    for (auto& [path, face] : m_faceCache) {
        if (face) FT_Done_Face(face);
    }
    m_faceCache.clear();

    
    if (m_ftLibrary) {
        FT_Done_FreeType(m_ftLibrary);
        m_ftLibrary = nullptr;
    }
}

FT_Face FTRasterizer::GetOrLoadFace(const std::string& fontPath) {
    
    auto it = m_faceCache.find(fontPath);
    if (it != m_faceCache.end()) return it->second;

    
    FT_Face face = nullptr;
    FT_Error error = FT_New_Face(m_ftLibrary, fontPath.c_str(), 0, &face);
    if (error) {
        PureTypeLog("FT_New_Face failed for '%s' (error %d)", fontPath.c_str(), error);
        return nullptr;
    }

    PureTypeLog("Loaded font face: %s (%s %s)",
                  fontPath.c_str(),
                  face->family_name ? face->family_name : "?",
                  face->style_name ? face->style_name : "?");

    m_faceCache[fontPath] = face;
    return face;
}

void FTRasterizer::EvictOldest() {
    if (m_cacheList.empty()) return;
    auto& oldest = m_cacheList.back();
    m_cacheMap.erase(oldest.first);
    m_cacheList.pop_back();
}

const GlyphBitmap* FTRasterizer::RasterizeGlyph(
    const std::string& fontPath,
    uint32_t glyphIndex,
    uint32_t pixelSize)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_ftLibrary) return nullptr;

    
    GlyphCacheKey key{ fontPath, glyphIndex, pixelSize };
    auto mapIt = m_cacheMap.find(key);
    if (mapIt != m_cacheMap.end()) {
        
        m_cacheList.splice(m_cacheList.begin(), m_cacheList, mapIt->second);
        return &mapIt->second->second;
    }

    
    FT_Face face = GetOrLoadFace(fontPath);
    if (!face) return nullptr;

    
    
    
    FT_Error error = FT_Set_Pixel_Sizes(face, 0, pixelSize);
    if (error) {
        PureTypeLog("FT_Set_Pixel_Sizes(%u) failed: %d", pixelSize, error);
        return nullptr;
    }

    
    error = FT_Load_Glyph(face, glyphIndex,
                           FT_LOAD_DEFAULT | FT_LOAD_TARGET_LCD);
    if (error) {
        PureTypeLog("FT_Load_Glyph(%u) failed: %d", glyphIndex, error);
        return nullptr;
    }

    
    error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD);
    if (error) {
        PureTypeLog("FT_Render_Glyph(LCD) failed: %d", error);
        return nullptr;
    }

    FT_Bitmap& ftBmp = face->glyph->bitmap;

    
    
    
    GlyphBitmap bmp;
    bmp.width    = static_cast<int>(ftBmp.width);   
    bmp.height   = static_cast<int>(ftBmp.rows);
    bmp.pitch    = ftBmp.pitch;
    bmp.bearingX = face->glyph->bitmap_left * 3;    
    bmp.bearingY = face->glyph->bitmap_top;
    bmp.advanceX = static_cast<int>(face->glyph->advance.x >> 6) * 3; 

    
    int dataSize = bmp.height * std::abs(bmp.pitch);
    bmp.data.resize(dataSize);
    if (ftBmp.pitch > 0) {
        std::memcpy(bmp.data.data(), ftBmp.buffer, dataSize);
    } else {
        
        int absPitch = std::abs(ftBmp.pitch);
        for (int row = 0; row < bmp.height; ++row) {
            const uint8_t* src = ftBmp.buffer + (bmp.height - 1 - row) * absPitch;
            std::memcpy(bmp.data.data() + row * absPitch, src, absPitch);
        }
        bmp.pitch = absPitch;
    }

    
    while (m_cacheList.size() >= MAX_CACHE_SIZE) {
        EvictOldest();
    }

    
    m_cacheList.emplace_front(key, std::move(bmp));
    m_cacheMap[key] = m_cacheList.begin();

    return &m_cacheList.front().second;
}

std::vector<FTRasterizer::PositionedGlyph> FTRasterizer::RasterizeGlyphRun(
    const std::string& fontPath,
    const uint16_t* glyphIndices,
    uint32_t glyphCount,
    uint32_t pixelSize,
    const int* lpDx)
{
    std::vector<PositionedGlyph> result;
    result.reserve(glyphCount);

    int currentX = 0; 

    for (uint32_t i = 0; i < glyphCount; ++i) {
        const GlyphBitmap* bmp = RasterizeGlyph(fontPath, glyphIndices[i], pixelSize);
        if (!bmp) continue;

        PositionedGlyph pg;
        pg.bitmap  = bmp;
        pg.offsetX = currentX + bmp->bearingX;
        pg.offsetY = 0; 
        result.push_back(pg);

        
        if (lpDx && i < glyphCount) {
            currentX += lpDx[i] * 3; 
        } else {
            currentX += bmp->advanceX;
        }
    }

    return result;
}

}
