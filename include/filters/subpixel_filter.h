#pragma once

#include <cstdint>
#include <vector>
#include <memory>

namespace puretype
{
    struct GlyphBitmap;
    struct ConfigData;

    struct RGBABitmap
    {
        int width = 0;
        int height = 0;
        int pitch = 0;
        uint16_t fontWeight = 400;
        std::vector<uint8_t> data;
    };

    class SubpixelFilter
    {
    public:
        virtual ~SubpixelFilter() = default;

        virtual RGBABitmap Apply(const GlyphBitmap& glyph,
                                 const ConfigData& cfg) const = 0;

        static std::unique_ptr<SubpixelFilter> Create(int panelType);
    };

    class WOLEDFilter : public SubpixelFilter
    {
    public:
        RGBABitmap Apply(const GlyphBitmap& glyph,
                         const ConfigData& cfg) const override;
    };

    class TriangularFilter : public SubpixelFilter
    {
    public:
        RGBABitmap Apply(const GlyphBitmap& glyph,
                         const ConfigData& cfg) const override;
    };
}