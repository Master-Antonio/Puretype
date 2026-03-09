

#include "output/blender.h"
#include "filters/subpixel_filter.h"
#include "color_math.h"
#include "config.h"

#include <cmath>
#include <algorithm>
#include <cstring>
#include <vector>

extern void PureTypeLog(const char* fmt, ...);

namespace puretype {

Blender& Blender::Instance() {
    static Blender instance;
    return instance;
}

void Blender::PremultiplyAlpha(uint8_t* bgra, int width, int height, int pitch) {
    for (int y = 0; y < height; ++y) {
        uint8_t* row = bgra + y * pitch;
        for (int x = 0; x < width; ++x) {
            uint8_t* px = row + x * 4;
            uint8_t a = px[3];
            if (a == 0) {
                px[0] = px[1] = px[2] = 0;
            } else if (a < 255) {
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
    (void)gamma; // Gamma shaping is already applied in SubpixelFilter::Apply.
    if (bitmap.data.empty() || bitmap.width <= 0 || bitmap.height <= 0) return false;

    uint8_t textR8 = GetRValue(textColor);
    uint8_t textG8 = GetGValue(textColor);
    uint8_t textB8 = GetBValue(textColor);
    float linTextR = sRGBToLinear(textR8);
    float linTextG = sRGBToLinear(textG8);
    float linTextB = sRGBToLinear(textB8);

    const auto& cfg = Config::Instance().Data();
    const bool qdPanel = (cfg.panelType == PanelType::QD_OLED_TRIANGLE);
    const bool nearBlackText = (std::max({textR8, textG8, textB8}) <= 96);
    const bool grayscaleText = (textR8 == textG8 && textG8 == textB8);
    const bool tinyText = (bitmap.height <= 18);
    const bool smallText = (bitmap.height <= 24);
    const float sizeBoost =
        std::clamp((24.0f - static_cast<float>(bitmap.height)) / 24.0f, 0.0f, 1.0f);

    if (nearBlackText) {
        linTextR = 0.0f;
        linTextG = 0.0f;
        linTextB = 0.0f;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize          = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth         = bitmap.width;
    bmi.bmiHeader.biHeight        = -bitmap.height; // Top-down DIB, matches our row order.
    bmi.bmiHeader.biPlanes        = 1;
    bmi.bmiHeader.biBitCount      = 32;
    bmi.bmiHeader.biCompression   = BI_RGB;

    void* dibBits = nullptr;
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP hBitmap = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (!hBitmap || !dibBits) {
        DeleteDC(memDC);
        return false;
    }

    HGDIOBJ oldBitmap = SelectObject(memDC, hBitmap);

    BitBlt(memDC, 0, 0, bitmap.width, bitmap.height, hdc, x, y, SRCCOPY);

    uint8_t* dst = static_cast<uint8_t*>(dibBits);
    int dstPitch = bitmap.width * 4;

    for (int row = 0; row < bitmap.height; ++row) {
        const uint8_t* src = bitmap.data.data() + row * bitmap.pitch;
        uint8_t* dstRow = dst + row * dstPitch;

        for (int col = 0; col < bitmap.width; ++col) {
            const uint8_t* srcPx = src + col * 4;
            uint8_t* dstPx = dstRow + col * 4;

            uint8_t alpha = srcPx[3];
            if (alpha == 0) continue;

            float covB = sRGBToLinear(srcPx[0]);
            float covG = sRGBToLinear(srcPx[1]);
            float covR = sRGBToLinear(srcPx[2]);

            float maxCov = std::max({covR, covG, covB});
            float avgCov = (covR + covG + covB) / 3.0f;
            // Weighted luminance estimate reduces color fringing from per-channel coverage.
            float yCov = 0.72f * maxCov + 0.28f * avgCov;

            float chromaKeep = 0.24f;
            if (tinyText) {
                chromaKeep = 0.0f;
            } else if (smallText) {
                chromaKeep = qdPanel ? 0.05f : 0.08f;
            } else if (bitmap.height <= 32) {
                chromaKeep = qdPanel ? 0.10f : 0.14f;
            } else {
                chromaKeep = qdPanel ? 0.18f : 0.24f;
            }
            if (grayscaleText) {
                chromaKeep = std::min(chromaKeep, 0.03f);
            }

            covR = yCov + (covR - yCov) * chromaKeep;
            covG = yCov + (covG - yCov) * chromaKeep;
            covB = yCov + (covB - yCov) * chromaKeep;

            auto readabilityTone = [&](float c) -> float {
                c = std::clamp(c, 0.0f, 1.0f);
                float expBase = qdPanel ? 1.01f : 1.03f;
                float expSize = qdPanel ? 0.10f : 0.16f;
                float gainBase = qdPanel ? 1.000f : 1.004f;
                float gainSize = qdPanel ? 0.008f : 0.012f;
                c = 1.0f - std::pow(1.0f - c, expBase + expSize * sizeBoost);
                if (c > 0.20f) {
                    c = std::min(1.0f, c * (gainBase + gainSize * sizeBoost));
                }
                return std::clamp(c, 0.0f, 1.0f);
            };
            covR = readabilityTone(covR);
            covG = readabilityTone(covG);
            covB = readabilityTone(covB);

            if (nearBlackText) {
                float stemBoost =
                    std::clamp((20.0f - static_cast<float>(bitmap.height)) / 20.0f, 0.0f, 1.0f);
                float solid = (qdPanel ? 0.94f : 0.96f) * std::max({covR, covG, covB}) +
                              (qdPanel ? 0.06f : 0.04f) * ((covR + covG + covB) / 3.0f);

                float darkExp = (tinyText ? (qdPanel ? 1.58f : 1.70f)
                                          : (qdPanel ? 1.38f : 1.50f)) +
                                (qdPanel ? 0.30f : 0.40f) * stemBoost;
                solid = 1.0f - std::pow(1.0f - std::clamp(solid, 0.0f, 1.0f), darkExp);
                if (solid > 0.30f) {
                    float slope = tinyText ? (qdPanel ? 1.10f : 1.14f)
                                           : (qdPanel ? 1.06f : 1.08f);
                    float lift = tinyText ? 0.015f : 0.008f;
                    solid = std::min(1.0f, solid * slope + lift);
                }
                if (tinyText) {
                    solid = std::max(solid, std::min(1.0f, yCov * 1.18f));
                }

                covR = solid;
                covG = solid;
                covB = solid;
            }

            float bgLinB = sRGBToLinear(dstPx[0]);
            float bgLinG = sRGBToLinear(dstPx[1]);
            float bgLinR = sRGBToLinear(dstPx[2]);

            float finalB = bgLinB * (1.0f - covB) + linTextB * covB;
            float finalG = bgLinG * (1.0f - covG) + linTextG * covG;
            float finalR = bgLinR * (1.0f - covR) + linTextR * covR;

            dstPx[0] = linearToSRGB(finalB);
            dstPx[1] = linearToSRGB(finalG);
            dstPx[2] = linearToSRGB(finalR);
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
                               float gamma)
{
    (void)gamma; // Gamma shaping is already applied in SubpixelFilter::Apply.
    if (!pRT || bitmap.data.empty()) return false;

    uint8_t textR8 = static_cast<uint8_t>(std::clamp(textColor.r, 0.0f, 1.0f) * 255.0f + 0.5f);
    uint8_t textG8 = static_cast<uint8_t>(std::clamp(textColor.g, 0.0f, 1.0f) * 255.0f + 0.5f);
    uint8_t textB8 = static_cast<uint8_t>(std::clamp(textColor.b, 0.0f, 1.0f) * 255.0f + 0.5f);

    float linR = sRGBToLinear(textR8);
    float linG = sRGBToLinear(textG8);
    float linB = sRGBToLinear(textB8);

    const auto& cfg = Config::Instance().Data();
    const bool qdPanel = (cfg.panelType == PanelType::QD_OLED_TRIANGLE);
    const bool nearBlackText = (std::max({textR8, textG8, textB8}) <= 96);
    const bool grayscaleText = (textR8 == textG8 && textG8 == textB8);
    const bool tinyText = (bitmap.height <= 18);
    const bool smallText = (bitmap.height <= 24);
    const float sizeBoost =
        std::clamp((24.0f - static_cast<float>(bitmap.height)) / 24.0f, 0.0f, 1.0f);

    if (nearBlackText) {
        linR = 0.0f;
        linG = 0.0f;
        linB = 0.0f;
    }

    std::vector<uint8_t> colorized(bitmap.data.size());
    for (int row = 0; row < bitmap.height; ++row) {
        for (int col = 0; col < bitmap.width; ++col) {
            int offset = row * bitmap.pitch + col * 4;
            const uint8_t* src = bitmap.data.data() + offset;
            uint8_t* dst = colorized.data() + offset;

            float covB = sRGBToLinear(src[0]);
            float covG = sRGBToLinear(src[1]);
            float covR = sRGBToLinear(src[2]);

            float maxCov = std::max({covR, covG, covB});
            float avgCov = (covR + covG + covB) / 3.0f;
            float yCov = 0.72f * maxCov + 0.28f * avgCov;

            float chromaKeep = 0.24f;
            if (tinyText) {
                chromaKeep = 0.0f;
            } else if (smallText) {
                chromaKeep = qdPanel ? 0.05f : 0.08f;
            } else if (bitmap.height <= 32) {
                chromaKeep = qdPanel ? 0.10f : 0.14f;
            } else {
                chromaKeep = qdPanel ? 0.18f : 0.24f;
            }
            if (grayscaleText) {
                chromaKeep = std::min(chromaKeep, 0.03f);
            }

            covR = yCov + (covR - yCov) * chromaKeep;
            covG = yCov + (covG - yCov) * chromaKeep;
            covB = yCov + (covB - yCov) * chromaKeep;

            auto readabilityTone = [&](float c) -> float {
                c = std::clamp(c, 0.0f, 1.0f);
                float expBase = qdPanel ? 1.01f : 1.03f;
                float expSize = qdPanel ? 0.10f : 0.16f;
                float gainBase = qdPanel ? 1.000f : 1.004f;
                float gainSize = qdPanel ? 0.008f : 0.012f;
                c = 1.0f - std::pow(1.0f - c, expBase + expSize * sizeBoost);
                if (c > 0.20f) {
                    c = std::min(1.0f, c * (gainBase + gainSize * sizeBoost));
                }
                return std::clamp(c, 0.0f, 1.0f);
            };
            covR = readabilityTone(covR);
            covG = readabilityTone(covG);
            covB = readabilityTone(covB);

            if (nearBlackText) {
                float stemBoost =
                    std::clamp((20.0f - static_cast<float>(bitmap.height)) / 20.0f, 0.0f, 1.0f);
                float solid = (qdPanel ? 0.94f : 0.96f) * std::max({covR, covG, covB}) +
                              (qdPanel ? 0.06f : 0.04f) * ((covR + covG + covB) / 3.0f);

                float darkExp = (tinyText ? (qdPanel ? 1.58f : 1.70f)
                                          : (qdPanel ? 1.38f : 1.50f)) +
                                (qdPanel ? 0.30f : 0.40f) * stemBoost;
                solid = 1.0f - std::pow(1.0f - std::clamp(solid, 0.0f, 1.0f), darkExp);
                if (solid > 0.30f) {
                    float slope = tinyText ? (qdPanel ? 1.10f : 1.14f)
                                           : (qdPanel ? 1.06f : 1.08f);
                    float lift = tinyText ? 0.015f : 0.008f;
                    solid = std::min(1.0f, solid * slope + lift);
                }
                if (tinyText) {
                    solid = std::max(solid, std::min(1.0f, yCov * 1.18f));
                }

                covR = solid;
                covG = solid;
                covB = solid;
            }

            float alphaCov = std::max({covR, covG, covB});
            if (alphaCov <= 0.0f) {
                dst[0] = dst[1] = dst[2] = 0;
                dst[3] = 0;
                continue;
            }

            float finalR = covR * linR;
            float finalG = covG * linG;
            float finalB = covB * linB;

            dst[0] = linearToSRGB(finalB);
            dst[1] = linearToSRGB(finalG);
            dst[2] = linearToSRGB(finalR);
            dst[3] = static_cast<uint8_t>(std::clamp(alphaCov, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }

    D2D1_BITMAP_PROPERTIES bmpProps = {};
    bmpProps.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    // D2D bitmap draw expects premultiplied BGRA.
    bmpProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    pRT->GetDpi(&bmpProps.dpiX, &bmpProps.dpiY);

    ID2D1Bitmap* pBitmap = nullptr;
    D2D1_SIZE_U bmpSize = { static_cast<UINT32>(bitmap.width),
                             static_cast<UINT32>(bitmap.height) };
    HRESULT hr = pRT->CreateBitmap(
        bmpSize,
        colorized.data(),
        static_cast<UINT32>(bitmap.pitch),
        bmpProps,
        &pBitmap
    );

    if (FAILED(hr) || !pBitmap) {
        PureTypeLog("CreateBitmap failed: 0x%08X", hr);
        return false;
    }

    D2D1_RECT_F destRect = {
        x,
        y,
        x + static_cast<float>(bitmap.width),
        y + static_cast<float>(bitmap.height)
    };

    pRT->DrawBitmap(pBitmap, destRect, 1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);

    pBitmap->Release();
    return true;
}

}
