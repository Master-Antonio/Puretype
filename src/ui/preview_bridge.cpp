#include "rasterizer/ft_rasterizer.h"
#include "filters/subpixel_filter.h"
#include "color_math.h"
#include "config.h"
#include "output/tone_parity.h"

#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kPreviewParamsV2Version = 1;

    struct PreviewParamsV2
    {
        uint32_t size;
        uint32_t version;
        const char* text;
        const char* fontPath;
        uint32_t fontSize;
        float filterStrength;
        float gamma;
        int32_t gammaMode;
        float oledGammaOutput;
        float lumaContrastStrength;
        float woledCrossTalkReduction;
        uint32_t enableSubpixelHinting;
        uint32_t enableFractionalPositioning;
        uint32_t stemDarkeningEnabled;
        float stemDarkeningStrength;
        int32_t panelType;
        uint32_t useMeasuredContrast;
        int32_t width;
        int32_t height;
        uint8_t* pBuffer;

        // Optional extension fields for forward-compatible preview behavior parity.
        float lodThresholdSmall;
        float lodThresholdLarge;
        uint32_t toneParityV2Enabled;
    };

    constexpr uint32_t kPreviewParamsV2RequiredSize =
        static_cast<uint32_t>(offsetof(PreviewParamsV2, pBuffer) + sizeof(uint8_t*));

    bool GeneratePreviewInternal(const char* text,
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
                                 bool useMeasuredContrast,
                                 int width,
                                 int height,
                                 uint8_t* pBuffer,
                                 float lodThresholdSmall,
                                 float lodThresholdLarge,
                                 bool toneParityV2Enabled);
}

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
    bool useMeasuredContrast,
    int width,
    int height,
    uint8_t* pBuffer);

extern "C" __declspec(dllexport) bool GeneratePreviewV2(const PreviewParamsV2* params,
                                                        uint32_t paramsSize)
{
    if (!params) return false;

    const uint32_t incomingSize = std::min(paramsSize, params->size);
    if (incomingSize < kPreviewParamsV2RequiredSize) return false;
    if (params->version != kPreviewParamsV2Version) return false;

    PreviewParamsV2 parsed = {};
    std::memcpy(&parsed, params, std::min(incomingSize, static_cast<uint32_t>(sizeof(parsed))));

    constexpr uint32_t kLodSmallOffset = static_cast<uint32_t>(offsetof(PreviewParamsV2, lodThresholdSmall));
    constexpr uint32_t kLodLargeOffset = static_cast<uint32_t>(offsetof(PreviewParamsV2, lodThresholdLarge));
    constexpr uint32_t kToneParityOffset = static_cast<uint32_t>(offsetof(PreviewParamsV2, toneParityV2Enabled));

    const bool hasLodSmall = incomingSize >= (kLodSmallOffset + sizeof(float));
    const bool hasLodLarge = incomingSize >= (kLodLargeOffset + sizeof(float));
    const bool hasToneParity = incomingSize >= (kToneParityOffset + sizeof(uint32_t));

    const float lodSmall = hasLodSmall ? parsed.lodThresholdSmall : 12.0f;
    const float lodLarge = hasLodLarge ? parsed.lodThresholdLarge : 24.0f;
    const bool toneParityV2Enabled = hasToneParity && (parsed.toneParityV2Enabled != 0u);

    return GeneratePreviewInternal(parsed.text,
                                   parsed.fontPath,
                                   parsed.fontSize,
                                   parsed.filterStrength,
                                   parsed.gamma,
                                   parsed.gammaMode,
                                   parsed.oledGammaOutput,
                                   parsed.lumaContrastStrength,
                                   parsed.woledCrossTalkReduction,
                                   parsed.enableSubpixelHinting != 0u,
                                   parsed.enableFractionalPositioning != 0u,
                                   parsed.stemDarkeningEnabled != 0u,
                                   parsed.stemDarkeningStrength,
                                   parsed.panelType,
                                   parsed.useMeasuredContrast != 0u,
                                   parsed.width,
                                   parsed.height,
                                   parsed.pBuffer,
                                   lodSmall,
                                   lodLarge,
                                   toneParityV2Enabled);
}

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
    bool useMeasuredContrast,
    int width,
    int height,
    uint8_t* pBuffer)
{
    return GeneratePreviewInternal(text,
                                   fontPath,
                                   fontSize,
                                   filterStrength,
                                   gamma,
                                   gammaMode,
                                   oledGammaOutput,
                                   lumaContrastStrength,
                                   woledCrossTalkReduction,
                                   enableSubpixelHinting,
                                   enableFractionalPositioning,
                                   stemDarkeningEnabled,
                                   stemDarkeningStrength,
                                   panelType,
                                   useMeasuredContrast,
                                   width,
                                   height,
                                   pBuffer,
                                   12.0f,
                                   24.0f,
                                   false);
}

namespace
{
    bool GeneratePreviewInternal(
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
        bool useMeasuredContrast,
        int width,
        int height,
        uint8_t* pBuffer,
        float lodThresholdSmall,
        float lodThresholdLarge,
        bool toneParityV2Enabled)
    {
        if (!text || !fontPath || !pBuffer) return false;
        if (width <= 0 || height <= 0) return false;

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
        cfg.lodThresholdSmall = std::clamp(lodThresholdSmall, 6.0f, 96.0f);
        cfg.lodThresholdLarge = std::clamp(lodThresholdLarge, cfg.lodThresholdSmall + 1.0f, 160.0f);
        cfg.toneParityV2Enabled = toneParityV2Enabled;
        initColorMathLUTs(cfg.gammaMode == GammaMode::OLED);

        auto filter = SubpixelFilter::Create(panelType);
        if (!filter) return false;

        const int pitch = width * 4;
        const int splitX = width / 2;
        const int margin = std::max(16, width / 40);
        const int leftStartX = margin;
        const int leftEndX = std::max(leftStartX + 48, splitX - margin);
        const int rightStartX = std::min(width - margin - 48, splitX + margin);
        const int rightEndX = width - margin;

        const uint8_t textR = 236;
        const uint8_t textG = 242;
        const uint8_t textB = 250;
        const float linTextR = sRGBToLinear(textR);
        const float linTextG = sRGBToLinear(textG);
        const float linTextB = sRGBToLinear(textB);
        const float linTextLuma = 0.2126f * linTextR + 0.7152f * linTextG + 0.0722f * linTextB;
        cfg.textContrastHint = -1.0f;
        cfg.dpiScaleHint = 1.0f;

        auto blendMask = [&](uint8_t* dstPx,
                             float maskB,
                             float maskG,
                             float maskR,
                             const bool applyToneParity)
        {
            maskB = std::clamp(maskB, 0.0f, 1.0f);
            maskG = std::clamp(maskG, 0.0f, 1.0f);
            maskR = std::clamp(maskR, 0.0f, 1.0f);

            const float bgB = sRGBToLinear(dstPx[0]);
            const float bgG = sRGBToLinear(dstPx[1]);
            const float bgR = sRGBToLinear(dstPx[2]);

            float outB = bgB * (1.0f - maskB) + linTextB * maskB;
            float outG = bgG * (1.0f - maskG) + linTextG * maskG;
            float outR = bgR * (1.0f - maskR) + linTextR * maskR;

            if (applyToneParity && cfg.toneParityV2Enabled)
            {
                const float coverageMax = std::max({maskR, maskG, maskB});
                const float postGamma = ComputeToneParityPostGamma(cfg.gamma, cfg.oledGammaOutput);
                ApplyToneParityPostComposite(outR, outG, outB,
                                             bgR, bgG, bgB,
                                             coverageMax,
                                             postGamma);
            }

            dstPx[0] = linearToSRGB(outB);
            dstPx[1] = linearToSRGB(outG);
            dstPx[2] = linearToSRGB(outR);
            dstPx[3] = 0xFF;
        };

        // Opaque background for deterministic preview rendering.
        for (int y = 0; y < height; ++y)
        {
            const float t = static_cast<float>(y) / static_cast<float>(std::max(1, height - 1));
            for (int x = 0; x < width; ++x)
            {
                const bool right = x >= splitX;
                uint8_t* px = pBuffer + y * pitch + x * 4;

                const float baseR = right ? 7.0f : 11.0f;
                const float baseG = right ? 12.0f : 12.0f;
                const float baseB = right ? 19.0f : 16.0f;
                const float lift = 7.0f * t;

                px[2] = static_cast<uint8_t>(std::clamp(baseR + lift, 0.0f, 255.0f));
                px[1] = static_cast<uint8_t>(std::clamp(baseG + lift, 0.0f, 255.0f));
                px[0] = static_cast<uint8_t>(std::clamp(baseB + lift, 0.0f, 255.0f));
                px[3] = 0xFF;
            }
        }

        std::vector<uint8_t> backgroundSnapshot;
        if (useMeasuredContrast)
        {
            // Snapshot the clean preview background so contrast hints are measured
            // from the real background and not from previously drawn glyph pixels.
            backgroundSnapshot.resize(static_cast<size_t>(pitch) * height);
            std::memcpy(backgroundSnapshot.data(), pBuffer, backgroundSnapshot.size());
        }

        auto quantizeHint = [](const float v) -> float
        {
            const float c = std::clamp(v, 0.0f, 1.0f);
            return std::round(c * 64.0f) / 64.0f;
        };

        auto sampleBgLuma = [&](const int x0,
                                const int y0,
                                const int x1,
                                const int y1,
                                const int clipLeft,
                                const int clipRight,
                                float* outLuma) -> bool
        {
            if (!outLuma || !useMeasuredContrast || backgroundSnapshot.empty()) return false;

            const int left = std::max({x0, clipLeft, 0});
            const int top = std::max(y0, 0);
            const int right = std::min({x1, clipRight, width});
            const int bottom = std::min(y1, height);
            if (right <= left || bottom <= top) return false;

            constexpr int kSampleCols = 6;
            constexpr int kSampleRows = 4;
            float sum = 0.0f;
            int count = 0;

            for (int row = 0; row < kSampleRows; ++row)
            {
                const int y = top + static_cast<int>(((row + 0.5f) * static_cast<float>(bottom - top)) / kSampleRows);
                const int clampedY = std::clamp(y, top, bottom - 1);
                const uint8_t* rowPtr = backgroundSnapshot.data() + clampedY * pitch;

                for (int col = 0; col < kSampleCols; ++col)
                {
                    const int x = left + static_cast<int>(((col + 0.5f) * static_cast<float>(right - left)) /
                        kSampleCols);
                    const int clampedX = std::clamp(x, left, right - 1);
                    const uint8_t* px = rowPtr + clampedX * 4;
                    const float b = sRGBToLinear(px[0]);
                    const float g = sRGBToLinear(px[1]);
                    const float r = sRGBToLinear(px[2]);
                    sum += 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    ++count;
                }
            }

            if (count <= 0) return false;
            *outLuma = std::clamp(sum / static_cast<float>(count), 0.0f, 1.0f);
            return true;
        };

        const float proxyContrastHint = quantizeHint(std::max(linTextLuma, 1.0f - linTextLuma));
        float panelBgLuma = 0.0f;
        bool hasPanelBg = false;
        if (useMeasuredContrast)
        {
            hasPanelBg = sampleBgLuma(rightStartX, 0, rightEndX, height, rightStartX, rightEndX, &panelBgLuma);
        }
        const float defaultContrastHint = useMeasuredContrast
                                              ? quantizeHint(hasPanelBg
                                                                 ? std::abs(linTextLuma - panelBgLuma)
                                                                 : proxyContrastHint)
                                              : proxyContrastHint;
        cfg.textContrastHint = defaultContrastHint;

        auto renderParagraph = [&](int startX, int endX, bool pureType)
        {
            if (startX >= endX) return;

            const int topPadding = std::max(20, static_cast<int>(fontSize) / 2 + 12);
            const int bottomPadding = 18;
            const int lineHeight = std::max(12, static_cast<int>(std::round(static_cast<float>(fontSize) * 1.45f)));
            const int maxBaselineY = height - bottomPadding;
            const int spaceAdvance = std::max(1, static_cast<int>(fontSize / 3));

            int cursorX = startX;
            int cursorY = topPadding + static_cast<int>(fontSize);

            for (const char* p = text; *p; ++p)
            {
                const unsigned char ch = static_cast<unsigned char>(*p);
                if (ch == '\r') continue;

                if (ch == '\n')
                {
                    cursorX = startX;
                    cursorY += lineHeight;
                    if (cursorY > maxBaselineY) break;
                    continue;
                }

                const uint32_t charCode = static_cast<uint32_t>(ch);
                const uint32_t glyphIndex = rasterizer.GetGlyphIndex(fontPath, charCode);
                const GlyphBitmap* glyph = nullptr;
                if (glyphIndex) glyph = rasterizer.RasterizeGlyph(fontPath, glyphIndex, fontSize, cfg);

                int advance = 0;
                if (glyph) advance = std::max(1, glyph->advanceX / 3);
                else if (charCode == ' ') advance = spaceAdvance;
                else continue;

                if (cursorX + advance > endX)
                {
                    cursorX = startX;
                    cursorY += lineHeight;
                    if (cursorY > maxBaselineY) break;
                    if (charCode == ' ') continue;
                }

                if (glyph)
                {
                    const int gxOffset = cursorX + glyph->bearingX / 3 - glyph->padLeft;
                    const int gyOffset = cursorY - glyph->bearingY - glyph->padTop;

                    if (pureType)
                    {
                        if (useMeasuredContrast)
                        {
                            float localBgLuma = panelBgLuma;
                            if (sampleBgLuma(gxOffset - 1, gyOffset - 1,
                                             gxOffset + std::max(1, glyph->width) + 1,
                                             gyOffset + std::max(1, glyph->height) + 1,
                                             startX, endX, &localBgLuma))
                            {
                                cfg.textContrastHint = quantizeHint(std::abs(linTextLuma - localBgLuma));
                            }
                            else
                            {
                                cfg.textContrastHint = defaultContrastHint;
                            }
                        }
                        else
                        {
                            cfg.textContrastHint = proxyContrastHint;
                        }
                    }

                    RGBABitmap filtered = filter->Apply(*glyph, cfg);
                    if (!filtered.data.empty())
                    {
                        const int glyphW = filtered.width;
                        const int glyphH = std::min(filtered.height, glyph->height);

                        for (int gy = 0; gy < glyphH; ++gy)
                        {
                            const int bufY = gyOffset + gy;
                            if (bufY < 0 || bufY >= height) continue;

                            const uint8_t* rowFiltered = filtered.data.data() + gy * filtered.pitch;
                            const uint8_t* rowRaw = glyph->data.data() + gy * glyph->pitch;
                            uint8_t* rowDst = pBuffer + bufY * pitch;

                            for (int gx = 0; gx < glyphW; ++gx)
                            {
                                const int bufX = gxOffset + gx;
                                if (bufX < startX || bufX >= endX || bufX < 0 || bufX >= width) continue;

                                uint8_t* dstPx = rowDst + bufX * 4;

                                if (pureType)
                                {
                                    const uint8_t* srcPx = rowFiltered + gx * 4;
                                    const float maskB = sRGBToLinear(srcPx[0]);
                                    const float maskG = sRGBToLinear(srcPx[1]);
                                    const float maskR = sRGBToLinear(srcPx[2]);
                                    blendMask(dstPx, maskB, maskG, maskR, true);
                                }
                                else
                                {
                                    const int rawBase = gx * 3;
                                    if (rawBase + 2 >= glyph->pitch) continue;
                                    const int sum = rowRaw[rawBase + 0] + rowRaw[rawBase + 1] + rowRaw[rawBase + 2];
                                    const uint8_t gray = static_cast<uint8_t>(sum / 3);
                                    const float mask = sRGBToLinear(gray);
                                    blendMask(dstPx, mask, mask, mask, false);
                                }
                            }
                        }
                    }
                }

                cursorX += advance;
            }
        };

        // Left = baseline grayscale AA, right = configured PureType pipeline.
        renderParagraph(leftStartX, leftEndX, false);
        renderParagraph(rightStartX, rightEndX, true);

        for (int y = 0; y < height; ++y)
        {
            for (int dx = -1; dx <= 0; ++dx)
            {
                const int x = splitX + dx;
                if (x < 0 || x >= width) continue;
                uint8_t* pDst = pBuffer + y * pitch + x * 4;
                pDst[0] = 0;
                pDst[1] = 216;
                pDst[2] = 255;
                pDst[3] = 255;
            }
        }

        return true;
    }
} // namespace
