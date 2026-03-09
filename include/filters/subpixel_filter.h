

#pragma once

#include <cstdint>
#include <vector>
#include <memory>

namespace puretype {

struct GlyphBitmap;  // from ft_rasterizer.h
struct ConfigData;   // from config.h

struct RGBABitmap {
    int width;   // pixels
    int height;
    int pitch;   // bytes per row (width * 4)
    std::vector<uint8_t> data;  // BGRA
};

class SubpixelFilter {
public:
    virtual ~SubpixelFilter() = default;

    /// Apply panel-specific subpixel filtering with HVS optimization.
    /// glyph:  FreeType LCD-mode bitmap (3× horizontal width, 1 byte per sub-sample)
    /// cfg:    full config for gamma, stem darkening, filter strength, panel type
    /// returns: BGRA bitmap at pixel (not subpixel) width
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
