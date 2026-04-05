#include "output/blender.h"
#include "output/tone_parity.h"
#include "filters/subpixel_filter.h"
#include "color_math.h"
#include "config.h"

#include <cmath>
#include <algorithm>
#include <vector>

namespace puretype
{
    Blender& Blender::Instance()
    {
        static Blender instance;
        return instance;
    }

    void Blender::PremultiplyAlpha(uint8_t* bgra, int width, int height, int pitch)
    {
        for (int y = 0; y < height; ++y)
        {
            uint8_t* row = bgra + y * pitch;
            for (int x = 0; x < width; ++x)
            {
                uint8_t* px = row + x * 4;
                uint8_t a = px[3];
                if (a == 0)
                {
                    px[0] = px[1] = px[2] = 0;
                }
                else if (a < 255)
                {
                    px[0] = static_cast<uint8_t>((px[0] * a + 127) / 255);
                    px[1] = static_cast<uint8_t>((px[1] * a + 127) / 255);
                    px[2] = static_cast<uint8_t>((px[2] * a + 127) / 255);
                }
            }
        }
    }

    bool Blender::BlitToHDC(HDC hdc, int x, int y,
                            const RGBABitmap& bitmap,
                            COLORREF textColor,
                            float gamma)
    {
        if (bitmap.data.empty() || bitmap.width <= 0 || bitmap.height <= 0) return false;

        const float clampedGamma = std::clamp(gamma, 0.5f, 3.0f);
        const float invCoverageGamma = 1.0f / clampedGamma;

        float linTextR = sRGBToLinear(GetRValue(textColor));
        float linTextG = sRGBToLinear(GetGValue(textColor));
        float linTextB = sRGBToLinear(GetBValue(textColor));

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = bitmap.width;
        bmi.bmiHeader.biHeight = -bitmap.height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* dibBits = nullptr;
        HDC memDC = CreateCompatibleDC(hdc);
        if (!memDC) return false;

        HBITMAP hBitmap = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
        if (!hBitmap || !dibBits)
        {
            DeleteDC(memDC);
            return false;
        }

        HGDIOBJ oldBitmap = SelectObject(memDC, hBitmap);
        if (!oldBitmap)
        {
            DeleteObject(hBitmap);
            DeleteDC(memDC);
            return false;
        }

        BitBlt(memDC, 0, 0, bitmap.width, bitmap.height, hdc, x, y, SRCCOPY);

        // Query real DIB pitch from the GDI bitmap object instead of assuming width*4.
        // For 32bpp top-down DIBs this is typically width*4, but relying on the BITMAP
        // structure eliminates a class of stride-mismatch bugs if the width is unusual.
        BITMAP dibInfo = {};
        GetObject(hBitmap, sizeof(dibInfo), &dibInfo);
        int dstPitch = dibInfo.bmWidthBytes; // True scanline stride from GDI

        uint8_t* dst = static_cast<uint8_t*>(dibBits);

        for (int row = 0; row < bitmap.height; ++row)
        {
            const uint8_t* srcRow = bitmap.data.data() + row * bitmap.pitch;
            uint8_t* dstRow = dst + row * dstPitch;

            for (int col = 0; col < bitmap.width; ++col)
            {
                const uint8_t* srcPx = srcRow + col * 4;
                uint8_t* dstPx = dstRow + col * 4;

                if (srcPx[3] == 0) continue;

                // Per-channel subpixel blending in linear light.
                // This is the CORRECT path: GDI gives us direct pixel access,
                // so we can do true per-channel alpha blending.
                float maskB = std::pow(std::clamp(sRGBToLinear(srcPx[0]), 0.0f, 1.0f), invCoverageGamma);
                float maskG = std::pow(std::clamp(sRGBToLinear(srcPx[1]), 0.0f, 1.0f), invCoverageGamma);
                float maskR = std::pow(std::clamp(sRGBToLinear(srcPx[2]), 0.0f, 1.0f), invCoverageGamma);

                float bgB = sRGBToLinear(dstPx[0]);
                float bgG = sRGBToLinear(dstPx[1]);
                float bgR = sRGBToLinear(dstPx[2]);

                float finalB = bgB * (1.0f - maskB) + linTextB * maskB;
                float finalG = bgG * (1.0f - maskG) + linTextG * maskG;
                float finalR = bgR * (1.0f - maskR) + linTextR * maskR;

                dstPx[0] = linearToSRGB(finalB);
                dstPx[1] = linearToSRGB(finalG);
                dstPx[2] = linearToSRGB(finalR);
                dstPx[3] = 0xFF; // Preserve opacity for DWM-composited surfaces
            }
        }

        BOOL blitResult = BitBlt(hdc, x, y, bitmap.width, bitmap.height, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(memDC);

        return blitResult != FALSE;
    }

    bool Blender::BlitToD2DTarget(ID2D1RenderTarget* pRT, float x, float y,
                                  const RGBABitmap& bitmap,
                                  const D2D1_COLOR_F& textColor,
                                  float gamma,
                                  float oledGammaOutput,
                                  bool toneParityV2Enabled,
                                  float pixelsPerDip)
    {
        if (!pRT || bitmap.data.empty()) return false;

        const float clampedGamma = std::clamp(gamma, 0.5f, 3.0f);
        const float invCoverageGamma = 1.0f / clampedGamma;
        const float postGamma = ComputeToneParityPostGamma(gamma, oledGammaOutput);

        float linR = sRGBToLinear(static_cast<uint8_t>(std::clamp(textColor.r, 0.0f, 1.0f) * 255.0f + 0.5f));
        float linG = sRGBToLinear(static_cast<uint8_t>(std::clamp(textColor.g, 0.0f, 1.0f) * 255.0f + 0.5f));
        float linB = sRGBToLinear(static_cast<uint8_t>(std::clamp(textColor.b, 0.0f, 1.0f) * 255.0f + 0.5f));
        const float textAlpha = std::clamp(textColor.a, 0.0f, 1.0f);

        std::vector<uint8_t> colorized(bitmap.data.size());

        for (int row = 0; row < bitmap.height; ++row)
        {
            for (int col = 0; col < bitmap.width; ++col)
            {
                int offset = row * bitmap.pitch + col * 4;
                const uint8_t* srcPx = bitmap.data.data() + offset;
                uint8_t* dstPx = colorized.data() + offset;

                if (srcPx[3] == 0)
                {
                    dstPx[0] = dstPx[1] = dstPx[2] = dstPx[3] = 0;
                    continue;
                }

                float maskB = std::clamp(sRGBToLinear(srcPx[0]), 0.0f, 1.0f);
                float maskG = std::clamp(sRGBToLinear(srcPx[1]), 0.0f, 1.0f);
                float maskR = std::clamp(sRGBToLinear(srcPx[2]), 0.0f, 1.0f);

                if (toneParityV2Enabled)
                {
                    // Match GDI parity semantics by scaling effective coverage.
                    // D2D composition does not expose destination background here,
                    // so we modulate mask coverage directly before premultiplied blit.
                    const float coverageMax = std::max({maskR, maskG, maskB});
                    float parityFactor = 1.0f;
                    if (coverageMax > 0.01f && postGamma > 1.001f)
                    {
                        parityFactor = std::pow(coverageMax, postGamma - 1.0f);
                    }

                    maskB = std::clamp(maskB * parityFactor, 0.0f, 1.0f);
                    maskG = std::clamp(maskG * parityFactor, 0.0f, 1.0f);
                    maskR = std::clamp(maskR * parityFactor, 0.0f, 1.0f);
                }
                else
                {
                    maskB = std::pow(maskB, invCoverageGamma);
                    maskG = std::pow(maskG, invCoverageGamma);
                    maskR = std::pow(maskR, invCoverageGamma);
                }

                float outB = linB * maskB;
                float outG = linG * maskG;
                float outR = linR * maskR;

                // Premultiplied alpha invariant for DXGI_FORMAT_B8G8R8A8_UNORM.
                // The raw byte values must satisfy: R_byte <= A_byte, G_byte <= A_byte, B_byte <= A_byte.
                const uint8_t baseB = linearToSRGB(outB);
                const uint8_t baseG = linearToSRGB(outG);
                const uint8_t baseR = linearToSRGB(outR);

                const uint8_t byteB = static_cast<uint8_t>(std::clamp(baseB * textAlpha + 0.5f, 0.0f, 255.0f));
                const uint8_t byteG = static_cast<uint8_t>(std::clamp(baseG * textAlpha + 0.5f, 0.0f, 255.0f));
                const uint8_t byteR = static_cast<uint8_t>(std::clamp(baseR * textAlpha + 0.5f, 0.0f, 255.0f));

                const float maskAlpha = std::max({maskR, maskG, maskB});
                const uint8_t coverageAlpha = static_cast<uint8_t>(
                    std::clamp(maskAlpha * textAlpha * 255.0f + 0.5f, 0.0f, 255.0f));

                dstPx[0] = byteB;
                dstPx[1] = byteG;
                dstPx[2] = byteR;
                dstPx[3] = std::max<uint8_t>(coverageAlpha, std::max({byteR, byteG, byteB}));
            }
        }

        D2D1_BITMAP_PROPERTIES bmpProps = {};
        bmpProps.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        bmpProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        pRT->GetDpi(&bmpProps.dpiX, &bmpProps.dpiY);

        ID2D1Bitmap* pBitmap = nullptr;
        D2D1_SIZE_U bmpSize = {static_cast<UINT32>(bitmap.width), static_cast<UINT32>(bitmap.height)};

        HRESULT hr = pRT->CreateBitmap(bmpSize, colorized.data(), static_cast<UINT32>(bitmap.pitch), &bmpProps,
                                       &pBitmap);
        if (FAILED(hr) || !pBitmap) return false;

        FLOAT rtDpiX = 96.0f, rtDpiY = 96.0f;
        pRT->GetDpi(&rtDpiX, &rtDpiY);

        // PIXEL SNAPPING FIX (Bug B): Flawless inverse-matrix mapping.
        // We find the exact hardware pixel coordinate where the origin (X, Y) lands.
        // We snap that physical coordinate to the integer grid, then dynamically
        // bounce it backward through an inverted matrix. This guarantees the final 
        // raster footprint hits 1:1 hardware pixels without bypassing D2D clipping bounds.
        D2D1_MATRIX_3X2_F transform;
        pRT->GetTransform(&transform);

        float worldX = (x * transform._11) + (y * transform._21) + transform._31;
        float worldY = (x * transform._12) + (y * transform._22) + transform._32;

        float physX = std::round(worldX * (rtDpiX / 96.0f));
        float physY = std::round(worldY * (rtDpiY / 96.0f));

        float snappedWorldX = physX / (rtDpiX / 96.0f);
        float snappedWorldY = physY / (rtDpiY / 96.0f);

        D2D1_MATRIX_3X2_F inv = transform;
        // D2D1InvertMatrix returns FALSE when the matrix is singular (det == 0),
        // which can happen during collapsed animations or degenerate transforms.
        // Using the uninverted garbage matrix places glyphs at arbitrary positions.
        //
        // Fix: when the transform is not invertible, draw at the original logical
        // coordinates (x, y) without pixel snapping. The glyph renders correctly,
        // just without sub-pixel position correction. We must NOT return false here
        // because that triggers the allGlyphsBlitted=false fallback which forwards
        // the entire glyph run to DWrite — re-drawing all glyphs already rendered
        // by PureType in this call with standard ClearType on top of them.
        if (!D2D1InvertMatrix(&inv))
        {
            D2D1_RECT_F destRect = {
                x,
                y,
                x + (static_cast<float>(bitmap.width) / pixelsPerDip),
                y + (static_cast<float>(bitmap.height) / pixelsPerDip)
            };
            pRT->DrawBitmap(pBitmap, destRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            pBitmap->Release();
            return true;
        }

        float logicalX = (snappedWorldX * inv._11) + (snappedWorldY * inv._21) + inv._31;
        float logicalY = (snappedWorldX * inv._12) + (snappedWorldY * inv._22) + inv._32;

        D2D1_RECT_F destRect = {
            logicalX,
            logicalY,
            logicalX + (static_cast<float>(bitmap.width) / pixelsPerDip),
            logicalY + (static_cast<float>(bitmap.height) / pixelsPerDip)
        };

        pRT->DrawBitmap(pBitmap, destRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);

        pBitmap->Release();

        return true;
    }
}
