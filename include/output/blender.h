#pragma once

#include <Windows.h>
#include <d2d1.h>
#include <cstdint>

namespace puretype
{
    struct RGBABitmap;

    class Blender
    {
    public:
        Blender() = default;
        ~Blender() = default;

        bool BlitToHDC(HDC hdc, int x, int y,
                       const RGBABitmap& bitmap,
                       COLORREF textColor,
                       float gamma);

        bool BlitToD2DTarget(ID2D1RenderTarget* pRT, float x, float y,
                             const RGBABitmap& bitmap,
                             const D2D1_COLOR_F& textColor,
                             float gamma,
                             float pixelsPerDip = 1.0f);

        static Blender& Instance();

        void PremultiplyAlpha(uint8_t* bgra, int width, int height, int pitch);
    };
}