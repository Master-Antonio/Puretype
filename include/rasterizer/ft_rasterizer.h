

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <list>
#include <mutex>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace puretype {

struct GlyphBitmap {
    int      width;       
    int      height;      
    int      pitch;       
    int      bearingX;    
    int      bearingY;    
    int      advanceX;    
    std::vector<uint8_t> data;  
};

struct GlyphCacheKey {
    std::string fontPath;
    uint32_t    glyphIndex;
    uint32_t    pixelSize;

    bool operator==(const GlyphCacheKey& o) const {
        return fontPath == o.fontPath &&
               glyphIndex == o.glyphIndex &&
               pixelSize == o.pixelSize;
    }
};

struct GlyphCacheKeyHash {
    size_t operator()(const GlyphCacheKey& k) const {
        size_t h = std::hash<std::string>{}(k.fontPath);
        h ^= std::hash<uint32_t>{}(k.glyphIndex) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(k.pixelSize)  + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

class FTRasterizer {
public:
    FTRasterizer();
    ~FTRasterizer();

    
    bool Initialize();

    
    void Shutdown();

    
    
    
    const GlyphBitmap* RasterizeGlyph(const std::string& fontPath,
                                       uint32_t glyphIndex,
                                       uint32_t pixelSize);

    
    
    struct PositionedGlyph {
        const GlyphBitmap* bitmap;
        int offsetX;  
        int offsetY;  
    };
    std::vector<PositionedGlyph> RasterizeGlyphRun(
        const std::string& fontPath,
        const uint16_t* glyphIndices,
        uint32_t glyphCount,
        uint32_t pixelSize,
        const int* lpDx = nullptr);

    
    static FTRasterizer& Instance();

private:
    FT_Library m_ftLibrary = nullptr;

    
    std::unordered_map<std::string, FT_Face> m_faceCache;

    
    static constexpr size_t MAX_CACHE_SIZE = 4096;
    using CacheList = std::list<std::pair<GlyphCacheKey, GlyphBitmap>>;
    CacheList m_cacheList;
    std::unordered_map<GlyphCacheKey, CacheList::iterator, GlyphCacheKeyHash> m_cacheMap;

    std::mutex m_mutex;

    FT_Face GetOrLoadFace(const std::string& fontPath);
    void EvictOldest();
};

}
