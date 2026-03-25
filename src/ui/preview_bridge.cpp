#include "rasterizer/ft_rasterizer.h"
#include "filters/subpixel_filter.h"
#include "config.h"
#include <windows.h>
#include <memory>
#include <string>

extern "C" __declspec(dllexport) bool GeneratePreview(
    const char* text,
    const char* fontPath,
    uint32_t fontSize,
    float filterStrength,
    float gamma,
    int gammaMode,
    float oledGammaOutput,
    float lumaContrastStrength,
    float woledCrossTalkReduction,
    bool enableSubpixelHinting,
    bool enableFractionalPositioning,
    bool stemDarkeningEnabled,
    float stemDarkeningStrength,
    int panelType,
    int width,
    int height,
    uint8_t* pBuffer)
{
    if (!text || !fontPath || !pBuffer) return false;

    using namespace puretype;

    FTRasterizer& rasterizer = FTRasterizer::Instance();
    if (!rasterizer.Initialize()) return false;

    ConfigData cfg;
    cfg.panelType = static_cast<PanelType>(panelType);
    cfg.filterStrength = filterStrength;
    cfg.gamma = gamma;
    cfg.gammaMode = static_cast<GammaMode>(gammaMode);
    cfg.oledGammaOutput = oledGammaOutput;
    cfg.lumaContrastStrength = lumaContrastStrength;
    cfg.woledCrossTalkReduction = woledCrossTalkReduction;
    cfg.enableSubpixelHinting = enableSubpixelHinting;
    cfg.enableFractionalPositioning = enableFractionalPositioning;
    cfg.stemDarkeningEnabled = stemDarkeningEnabled;
    cfg.stemDarkeningStrength = stemDarkeningStrength;

    auto filter = SubpixelFilter::Create(panelType);
    if (!filter) return false;

    // Clear output buffer
    memset(pBuffer, 0, width * height * 4);

    const int splitX = width / 2;
    float invGamma = 1.0f / (gamma > 0.01f ? gamma : 1.0f);

    auto RenderParagraph = [&](int startX, bool isPureType)
    {
        int cursorX = startX;
        int cursorY = fontSize + 20; // Top padding

        for (const char* p = text; *p; ++p)
        {
            if (*p == '\n')
            {
                cursorX = startX;
                cursorY += static_cast<int>(fontSize * 1.5f);
                continue;
            }

            uint32_t charCode = static_cast<uint8_t>(*p);
            uint32_t glyphIndex = rasterizer.GetGlyphIndex(fontPath, charCode);
            if (!glyphIndex && charCode != ' ') continue;

            const GlyphBitmap* glyph = nullptr;
            if (glyphIndex)
            {
                glyph = rasterizer.RasterizeGlyph(fontPath, glyphIndex, fontSize);
            }

            if (glyph)
            {
                RGBABitmap filtered = filter->Apply(*glyph, cfg);
                if (!filtered.data.empty())
                {
                    int glyphW = filtered.width;
                    int glyphH = filtered.height;

                    int gxOffset = cursorX + glyph->bearingX / 3 - glyph->padLeft;
                    int gyOffset = cursorY - glyph->bearingY - glyph->padTop;

                    for (int gy = 0; gy < glyphH; ++gy)
                    {
                        int bufY = gyOffset + gy;
                        if (bufY < 0 || bufY >= height) continue;

                        const uint8_t* rowFiltered = filtered.data.data() + gy * filtered.pitch;
                        const uint8_t* rowRaw = glyph->data.data() + gy * glyph->pitch;
                        uint8_t* rowDst = pBuffer + bufY * (width * 4);

                        for (int gx = 0; gx < glyphW; ++gx)
                        {
                            int bufX = gxOffset + gx;
                            // Ensure we don't cross the splitX boundary or go out of bounds
                            if (bufX < 0 || bufX >= width) continue;
                            if (!isPureType && bufX >= splitX) continue;
                            if (isPureType && bufX < splitX) continue;

                            uint8_t* pDst = rowDst + bufX * 4;

                            if (!isPureType)
                            {
                                // LEFT: grayscale — simulates unfiltered ClearType
                                int sum = rowRaw[gx * 3 + 0] + rowRaw[gx * 3 + 1] + rowRaw[gx * 3 + 2];
                                uint8_t luma = static_cast<uint8_t>(sum / 3);
                                pDst[0] = std::max(pDst[0], luma);
                                pDst[1] = std::max(pDst[1], luma);
                                pDst[2] = std::max(pDst[2], luma);
                                pDst[3] = 255;
                            }
                            else
                            {
                                // RIGHT: PureType subpixel + gamma correction
                                const uint8_t* pSrc = rowFiltered + gx * 4;
                                for (int c = 0; c < 3; ++c)
                                {
                                    float val = pSrc[c] / 255.0f;
                                    uint8_t corrected = static_cast<uint8_t>(std::pow(val, invGamma) * 255.0f + 0.5f);
                                    pDst[c] = std::max(pDst[c], corrected);
                                }
                                pDst[3] = 255;
                            }
                        }
                    }
                }
            }

            if (glyph)
            {
                cursorX += glyph->advanceX / 3;
            }
            else if (charCode == ' ')
            {
                cursorX += fontSize / 3; // Approx space advance
            }
        }
    };

    // Render left (Inactive) and right (Active)
    RenderParagraph(30, false);
    RenderParagraph(splitX + 30, true);

    // Draw the cyan line over the whole height just to be sure it's visible even without text there
    for (int y = 0; y < height; ++y)
    {
        uint8_t* pDst = pBuffer + y * (width * 4) + splitX * 4;
        pDst[0] = 0; // B
        pDst[1] = 216; // G
        pDst[2] = 255; // R
        pDst[3] = 255;
    }

    return true;
}