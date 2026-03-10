#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <list>
#include <mutex>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace puretype
{
    struct GlyphBitmap
    {
        uint64_t fontHash = 0;
        uint32_t glyphIndex = 0;
        uint32_t pixelSize = 0;
        uint16_t fontWeight = 400;
        uint8_t phaseX = 0;
        uint8_t phaseY = 0;

        int width;
        int height;
        int pitch;
        int bearingX;
        int bearingY;
        int advanceX;
        std::vector<uint8_t> data;
    };

    struct GlyphCacheKey
    {
        std::string fontPath;
        uint32_t glyphIndex;
        uint32_t pixelSize;
        uint16_t fontWeight;
        uint8_t phaseX;
        uint8_t phaseY;
        uint8_t panelType;
        bool subpixelHinting;

        bool operator==(const GlyphCacheKey& o) const
        {
            return fontPath == o.fontPath &&
                glyphIndex == o.glyphIndex &&
                pixelSize == o.pixelSize &&
                fontWeight == o.fontWeight &&
                phaseX == o.phaseX &&
                phaseY == o.phaseY &&
                panelType == o.panelType &&
                subpixelHinting == o.subpixelHinting;
        }
    };

    struct GlyphCacheKeyHash
    {
        size_t operator()(const GlyphCacheKey& k) const
        {
            size_t h = std::hash<std::string>{}(k.fontPath);
            h ^= std::hash<uint32_t>{}(k.glyphIndex) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(k.pixelSize) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint16_t>{}(k.fontWeight) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint8_t>{}(k.phaseX) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint8_t>{}(k.phaseY) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint8_t>{}(k.panelType) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<bool>{}(k.subpixelHinting) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    class GlyphCache
    {
    public:
        const GlyphBitmap* TryGet(const GlyphCacheKey& key);
        const GlyphBitmap* Put(const GlyphCacheKey& key, GlyphBitmap&& bitmap, size_t extraBytes);
        void Clear();

    private:
        struct CachedGlyph
        {
            GlyphCacheKey key;
            GlyphBitmap bitmap;
            size_t bytes = 0;
        };

        static constexpr size_t MAX_CACHE_BYTES = 10 * 1024 * 1024;
        using CacheList = std::list<CachedGlyph>;
        CacheList m_cacheList;
        std::unordered_map<GlyphCacheKey, CacheList::iterator, GlyphCacheKeyHash> m_cacheMap;
        size_t m_cacheBytes = 0;
        std::mutex m_mutex;

        void EvictOldest();
    };

    class FTRasterizer
    {
    public:
        FTRasterizer();
        ~FTRasterizer();

        bool Initialize();

        void Shutdown();

        const GlyphBitmap* RasterizeGlyph(const std::string& fontPath,
                                          uint32_t glyphIndex,
                                          uint32_t pixelSize,
                                          uint16_t fontWeight = 400,
                                          uint8_t phaseX = 0,
                                          uint8_t phaseY = 0);

        struct PositionedGlyph
        {
            const GlyphBitmap* bitmap;
            int offsetX;
            int offsetY;
        };

        std::vector<PositionedGlyph> RasterizeGlyphRun(
            const std::string& fontPath,
            const uint16_t* glyphIndices,
            uint32_t glyphCount,
            uint32_t pixelSize,
            const int* lpDx = nullptr,
            uint16_t fontWeight = 400,
            const uint8_t* fractionalPhaseX = nullptr,
            const uint8_t* fractionalPhaseY = nullptr);

        static FTRasterizer& Instance();

    private:
        FT_Library m_ftLibrary = nullptr;

        std::unordered_map<std::string, FT_Face> m_faceCache;
        GlyphCache m_cache;
        std::mutex m_mutex;

        FT_Face GetOrLoadFace(const std::string& fontPath);
    };
}