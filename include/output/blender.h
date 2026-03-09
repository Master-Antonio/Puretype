

#pragma once

#include <Windows.h>
#include <d2d1.h>
#include <cstdint>

namespace puretype {

struct RGBABitmap;  // from subpixel_filter.h

class Blender {
public:
    Blender() = default;
    ~Blender() = default;

    /// Composite a filtered glyph bitmap onto a GDI HDC.
    /// Uses sRGB transfer (color_math.h LUTs) for gamma-correct blending.
    /// The gamma parameter is a fine-tune exponent on top of sRGB (1.0 = pure sRGB).
    bool BlitToHDC(HDC hdc, int x, int y,
                   const RGBABitmap& bitmap,
                   COLORREF textColor,
                   float gamma);

    /// Composite a filtered glyph bitmap onto a D2D render target.
    bool BlitToD2DTarget(ID2D1RenderTarget* pRT, float x, float y,
                         const RGBABitmap& bitmap,
                         const D2D1_COLOR_F& textColor,
                         float gamma);

    static Blender& Instance();

    /// Premultiply alpha in a BGRA buffer (used for D2D path).
    void PremultiplyAlpha(uint8_t* bgra, int width, int height, int pitch);
};

}
