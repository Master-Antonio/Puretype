

#include "rasterizer/ft_rasterizer.h"
#include "config.h"
#include <cstring>
#include <algorithm>
#include FT_LCD_FILTER_H
#include FT_OUTLINE_H

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

const GlyphBitmap* GlyphCache::TryGet(const GlyphCacheKey& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cacheMap.find(key);
    if (it != m_cacheMap.end()) {
        m_cacheList.splice(m_cacheList.begin(), m_cacheList, it->second);
        return &it->second->bitmap;
    }
    return nullptr;
}

const GlyphBitmap* GlyphCache::Put(const GlyphCacheKey& key, GlyphBitmap&& bitmap, size_t extraBytes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Check if another thread inserted it while we were rasterizing
    auto it = m_cacheMap.find(key);
    if (it != m_cacheMap.end()) {
        m_cacheList.splice(m_cacheList.begin(), m_cacheList, it->second);
        return &it->second->bitmap;
    }
    
    const size_t entryBytes = bitmap.data.size() + sizeof(GlyphBitmap) + extraBytes;
    while (!m_cacheList.empty() && (m_cacheBytes + entryBytes) > MAX_CACHE_BYTES) {
        EvictOldest();
    }

    m_cacheList.push_front(CachedGlyph{key, std::move(bitmap), entryBytes});
    m_cacheBytes += entryBytes;
    m_cacheMap[key] = m_cacheList.begin();

    return &m_cacheList.front().bitmap;
}

void GlyphCache::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cacheList.clear();
    m_cacheMap.clear();
    m_cacheBytes = 0;
}

void GlyphCache::EvictOldest() {
    if (m_cacheList.empty()) return;
    auto& oldest = m_cacheList.back();
    if (m_cacheBytes >= oldest.bytes) {
        m_cacheBytes -= oldest.bytes;
    } else {
        m_cacheBytes = 0;
    }
    m_cacheMap.erase(oldest.key);
    m_cacheList.pop_back();
}

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
    // We apply panel-specific filtering ourselves after rasterization.

    PureTypeLog("FreeType library initialized (version %d.%d.%d)",
                  FREETYPE_MAJOR, FREETYPE_MINOR, FREETYPE_PATCH);
    return true;
}

void FTRasterizer::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);

    m_cache.Clear();

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

const GlyphBitmap* FTRasterizer::RasterizeGlyph(
    const std::string& fontPath,
    uint32_t glyphIndex,
    uint32_t pixelSize,
    uint16_t fontWeight,
    uint8_t phaseX,
    uint8_t phaseY)
{
    const auto& cfg = Config::Instance().Data();
    GlyphCacheKey key{
        fontPath, glyphIndex, pixelSize, fontWeight,
        static_cast<uint8_t>(phaseX & 0x03u),
        static_cast<uint8_t>(phaseY & 0x03u),
        static_cast<uint8_t>(cfg.panelType),
        cfg.enableSubpixelHinting
    };
    
    if (const GlyphBitmap* cached = m_cache.TryGet(key)) return cached;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ftLibrary) return nullptr;
    FT_Face face = GetOrLoadFace(fontPath);
    if (!face) return nullptr;

    FT_Set_Pixel_Sizes(face, 0, pixelSize);

    FT_Int32 loadFlags = FT_LOAD_DEFAULT | FT_LOAD_TARGET_LCD;
    if (cfg.enableSubpixelHinting) {
        loadFlags |= FT_LOAD_FORCE_AUTOHINT;
    }

    // Both WRGB and RWBG share the exact same physical luminance structure (White core).
    FT_Pos oledPhaseX = (cfg.panelType == PanelType::QD_OLED_TRIANGLE) ? 32 : 24;
    FT_Pos oledPhaseY = (cfg.panelType == PanelType::QD_OLED_TRIANGLE) ? 21 : 0;

    FT_Vector phaseVec;
    phaseVec.x = (phaseX * 16) + oledPhaseX; 
    phaseVec.y = (phaseY * 16) + oledPhaseY;

    // 2. Diciamo a FreeType esattamente dove si trova la griglia fisica OLED
    FT_Set_Transform(face, nullptr, &phaseVec);

    // 3. Ora FreeType carica e fa l'hinting tenendo conto della posizione esatta!
    FT_Error error = FT_Load_Glyph(face, glyphIndex, loadFlags);
    if (error) return nullptr;

    // 4. Renderizziamo sempre a risoluzione 3x (LCD) per estrarre i dati cromatici
    error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD);
    if (error) return nullptr;

    FT_Bitmap& ftBmp = face->glyph->bitmap;
    GlyphBitmap bmp;
    bmp.fontHash = std::hash<std::string>{}(fontPath);
    bmp.glyphIndex = glyphIndex;
    bmp.pixelSize = pixelSize;
    bmp.fontWeight = fontWeight;
    bmp.phaseX = static_cast<uint8_t>(phaseX & 0x03u);
    bmp.phaseY = static_cast<uint8_t>(phaseY & 0x03u);
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

    int currentX = 0;

    for (uint32_t i = 0; i < glyphCount; ++i) {
        const uint8_t phaseX = fractionalPhaseX ? static_cast<uint8_t>(fractionalPhaseX[i] & 0x03u) : 0;
        const uint8_t phaseY = fractionalPhaseY ? static_cast<uint8_t>(fractionalPhaseY[i] & 0x03u) : 0;
        const GlyphBitmap* bmp = RasterizeGlyph(fontPath, glyphIndices[i], pixelSize,
                                                fontWeight, phaseX, phaseY);
        if (!bmp) continue;

        PositionedGlyph pg;
        pg.bitmap  = bmp;
        pg.offsetX = currentX + bmp->bearingX;
        pg.offsetY = 0;
        result.push_back(pg);

        if (lpDx && i < glyphCount) {
            // GDI advances are pixel-based; internal coordinates are in LCD sub-samples.
            currentX += lpDx[i] * 3;
        } else {
            currentX += bmp->advanceX;
        }
    }

    return result;
}

}
