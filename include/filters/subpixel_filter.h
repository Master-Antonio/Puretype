

#pragma once

#include <cstdint>
#include <vector>
#include <memory>

namespace puretype {

struct GlyphBitmap;
struct ConfigData;

struct RGBABitmap {
    int width;
    int height;
    int pitch;
    std::vector<uint8_t> data;
};

class SubpixelFilter {
public:
    virtual ~SubpixelFilter() = default;

    virtual RGBABitmap Apply(const GlyphBitmap& glyph,
                             const ConfigData& cfg) const = 0;

    static std::unique_ptr<SubpixelFilter> Create(int panelType);
};

class WRGBFilter : public SubpixelFilter {
public:
    RGBABitmap Apply(const GlyphBitmap& glyph,
                     const ConfigData& cfg) const override;
};

class TriangularFilter : public SubpixelFilter {
public:
    RGBABitmap Apply(const GlyphBitmap& glyph,
                     const ConfigData& cfg) const override;
};

}
