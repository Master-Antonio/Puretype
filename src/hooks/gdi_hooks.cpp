#include "hooks/gdi_hooks.h"
#include "config.h"
#include "puretype.h"

#include <MinHook.h>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>
#include <cstring>

#include "color_math.h"
#include "output/tone_parity.h"
#include "render_optimizer.h"
#include "stem_darkening.h"

extern void PureTypeLog(const char* fmt, ...);



static bool IsInterFontGDI(const wchar_t* faceName)
{
    if (!faceName) return false;
    
    return (_wcsnicmp(faceName, L"Inter", 5) == 0);
}


namespace puretype::hooks
{
    using ExtTextOutW_t = BOOL(WINAPI*)(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
    using DrawTextW_t = int(WINAPI*)(HDC, LPCWSTR, int, LPRECT, UINT);
    using DrawTextExW_t = int(WINAPI*)(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
    using PolyTextOutW_t = BOOL(WINAPI*)(HDC, const POLYTEXTW*, int);

    static ExtTextOutW_t g_OrigExtTextOutW = nullptr;
    static DrawTextW_t g_OrigDrawTextW = nullptr;
    static DrawTextExW_t g_OrigDrawTextExW = nullptr;
    static PolyTextOutW_t g_OrigPolyTextOutW = nullptr;

    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    

    
    
    
    struct PixelCapture
    {
        std::vector<uint8_t> data; 
        int x = 0, y = 0, width = 0, height = 0, pitch = 0;

        [[nodiscard]] bool IsValid() const
        {
            return !data.empty() && width > 0 && height > 0;
        }
    };

    static PixelCapture CaptureRegion(HDC hdc, int x, int y, int w, int h)
    {
        PixelCapture cap;
        if (w <= 0 || h <= 0) return cap;

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* dibBits = nullptr;
        HDC memDC = CreateCompatibleDC(hdc);
        if (!memDC) return cap;

        HBITMAP hBmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
        if (!hBmp || !dibBits)
        {
            DeleteDC(memDC);
            return cap;
        }

        HGDIOBJ old = SelectObject(memDC, hBmp);
        BitBlt(memDC, 0, 0, w, h, hdc, x, y, SRCCOPY);

        cap.x = x;
        cap.y = y;
        cap.width = w;
        cap.height = h;
        cap.pitch = w * 4;
        cap.data.resize(static_cast<size_t>(w * 4) * h);
        std::memcpy(cap.data.data(), dibBits, cap.data.size());

        SelectObject(memDC, old);
        DeleteObject(hBmp);
        DeleteDC(memDC);
        return cap;
    }

    
    
    
    
    
    
    static RECT ComputeCaptureBounds(HDC hdc,
                                     int x, int y,
                                     UINT options,
                                     const RECT* lprc,
                                     LPCWSTR lpString,
                                     UINT cbCount,
                                     const INT* lpDx)
    {
        constexpr int kMargin = 4;

        if (lprc && (options & (ETO_OPAQUE | ETO_CLIPPED)))
        {
            return {
                lprc->left - kMargin, lprc->top - kMargin,
                lprc->right + kMargin, lprc->bottom + kMargin
            };
        }

        TEXTMETRICW tm = {};
        GetTextMetricsW(hdc, &tm);

        int textW = 0;
        if (lpDx)
        {
            const bool hasPDY = (options & ETO_PDY) != 0;
            for (UINT i = 0; i < cbCount; ++i)
                textW += hasPDY ? lpDx[i * 2] : lpDx[i];
        }
        else
        {
            SIZE sz = {};
            if (options & ETO_GLYPH_INDEX)
                GetTextExtentExPointI(hdc,
                                      reinterpret_cast<LPWORD>(const_cast<LPWSTR>(lpString)),
                                      static_cast<int>(cbCount), 0, nullptr, nullptr, &sz);
            else
                GetTextExtentExPointW(hdc, lpString,
                                      static_cast<int>(cbCount), 0, nullptr, nullptr, &sz);
            textW = sz.cx;
        }

        const UINT align = GetTextAlign(hdc);
        int left = x;
        if ((align & TA_CENTER) == TA_CENTER) left = x - textW / 2;
        else if ((align & TA_RIGHT) == TA_RIGHT) left = x - textW;

        int top;
        if ((align & TA_BASELINE) == TA_BASELINE) top = y - tm.tmAscent;
        else if ((align & TA_BOTTOM) == TA_BOTTOM) top = y - tm.tmHeight;
        else top = y;

        return {
            left - kMargin, top - kMargin,
            left + textW + kMargin, top + tm.tmHeight + kMargin
        };
    }

    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    struct ForceSubpixelRender
    {
        HDC m_hdc = nullptr;
        HFONT m_origFont = nullptr;
        HFONT m_ctFont = nullptr;
        bool m_active = false;

        explicit ForceSubpixelRender(HDC hdc, const ConfigData* cfg = nullptr)
        {
            m_hdc = hdc;
            HFONT origFont = static_cast<HFONT>(GetCurrentObject(hdc, OBJ_FONT));
            if (!origFont) return;

            LOGFONTW lf = {};
            if (GetObjectW(origFont, sizeof(lf), &lf) == 0) return;

            
            const bool isInter = IsInterFontGDI(lf.lfFaceName);
            bool needsClone = false;

            if (isInter && cfg && cfg->interFontWeight > 0)
            {
                lf.lfWeight = static_cast<LONG>(cfg->interFontWeight);
                needsClone = true;
            }

            
            if (lf.lfQuality == CLEARTYPE_QUALITY ||
                lf.lfQuality == CLEARTYPE_NATURAL_QUALITY)
            {
                if (!needsClone)
                {
                    m_active = true; 
                    return;
                }
            }
            else
            {
                
                lf.lfQuality = CLEARTYPE_QUALITY;
                needsClone = true;
            }

            if (!needsClone)
            {
                m_active = true;
                return;
            }

            m_ctFont = CreateFontIndirectW(&lf);
            if (!m_ctFont) return;

            m_origFont = static_cast<HFONT>(SelectObject(hdc, m_ctFont));
            m_active = true;
        }

        ~ForceSubpixelRender()
        {
            if (m_origFont && m_hdc)
            {
                SelectObject(m_hdc, m_origFont);
            }
            if (m_ctFont)
            {
                DeleteObject(m_ctFont);
            }
        }

        
        [[nodiscard]] bool IsActive() const { return m_active; }

        ForceSubpixelRender(const ForceSubpixelRender&) = delete;
        ForceSubpixelRender& operator=(const ForceSubpixelRender&) = delete;
    };

    
    
    
    
    
    
    
    
    
    
    
    static void RemapToOLED(HDC hdc,
                            const PixelCapture& before,
                            const PixelCapture& after,
                            COLORREF textColor,
                            const ConfigData& cfg,
                            bool opaqueBackground, 
                            bool isComposited, 
                            float dpiScale = 1.0f, 
                            uint16_t fontWeight = 400, 
                            float emSizePx = 0.0f, 
                            COLORREF bkColor = CLR_INVALID) 
    {
        (void)isComposited;

        if (!before.IsValid() || !after.IsValid()) return;
        if (before.width != after.width || before.height != after.height) return;

        const int w = before.width;
        const int h = before.height;

        const float linTextR = sRGBToLinear(GetRValue(textColor));
        const float linTextG = sRGBToLinear(GetGValue(textColor));
        const float linTextB = sRGBToLinear(GetBValue(textColor));
        
        const float linTextLuma = 0.2126f * linTextR + 0.7152f * linTextG + 0.0722f * linTextB;
        
        const float textR_s_const = static_cast<float>(GetRValue(textColor));
        const float textG_s_const = static_cast<float>(GetGValue(textColor));
        const float textB_s_const = static_cast<float>(GetBValue(textColor));

        const bool qdGen1 = (cfg.panelType == PanelType::QD_OLED_GEN1);
        const bool qdGen3 = (cfg.panelType == PanelType::QD_OLED_GEN3);
        const bool qdGen4 = (cfg.panelType == PanelType::QD_OLED_GEN4);
        const bool qdPanel = qdGen1 || qdGen3 || qdGen4;
        const bool rgwbPanel = (cfg.panelType == PanelType::RGWB);
        const EdgeAdaptiveParams edgeParams = GetEdgeAdaptiveParams(qdPanel);

        
        
        const float emSize = (emSizePx > 0.0f) ? emSizePx : static_cast<float>(h);
        const float darkenAmount = cfg.stemDarkeningEnabled
                                       ? computeDarkenAmount(emSize, cfg.stemDarkeningStrength, fontWeight)
                                       : 0.0f;

        
        const bool tinyText = (h <= 18);
        const bool smallText = (h <= 24);
        const float sizeBoost = std::clamp((24.0f - static_cast<float>(h)) / 24.0f, 0.0f, 1.0f);
        const float expBase = (qdPanel ? 1.01f : 1.03f)
            * (1.0f + (cfg.lumaContrastStrength - 1.0f) * 0.5f);
        const float finalExp = expBase + (qdPanel ? 0.10f : 0.16f) * sizeBoost;
        const float finalGain = (qdPanel ? 1.000f : 1.004f)
            + (qdPanel ? 0.008f : 0.012f) * sizeBoost;

        constexpr int LUT_SIZE = 1024;
        float scurveLUT[LUT_SIZE];
        for (int i = 0; i < LUT_SIZE; ++i)
        {
            float c = static_cast<float>(i) / (LUT_SIZE - 1);
            c = 1.0f - std::pow(1.0f - c, finalExp);
            if (c > 0.20f) c = std::min(1.0f, c * finalGain);
            scurveLUT[i] = std::clamp(c, 0.0f, 1.0f);
        }
        auto scurve = [&](float v) -> float
        {
            return scurveLUT[static_cast<int>(
                std::clamp(v, 0.0f, 1.0f) * (LUT_SIZE - 1) + 0.5f)];
        };

        
        
        
        
        float chromaKeepBase;
        if (tinyText) chromaKeepBase = qdPanel ? 0.78f : 0.77f;
        else if (smallText) chromaKeepBase = qdPanel ? 0.82f : 0.80f;
        else if (h <= 32) chromaKeepBase = qdPanel ? 0.86f : 0.84f;
        else chromaKeepBase = qdPanel ? 0.88f : 0.87f;

        const float chromaFamilyScale = qdPanel ? cfg.chromaKeepScaleQD : cfg.chromaKeepScaleWOLED;
        chromaKeepBase *= std::clamp(chromaFamilyScale, 0.60f, 1.30f);

        
        if (fontWeight > 0)
        {
            const float weightNorm = std::clamp(static_cast<float>(fontWeight) / 400.0f, 0.5f, 2.0f);
            chromaKeepBase *= (0.85f + 0.15f * weightNorm);
        }

        
        
        if (dpiScale < 1.0f)
            chromaKeepBase *= 0.4f + 0.6f * dpiScale; 
        const float toneStrength = std::clamp(cfg.filterStrength * dpiScale, 0.0f, 5.0f);

        ConstrainedChromaFastPath fastPath;
        fastPath.maxEdgeRisk = tinyText ? 0.05f : 0.08f;
        fastPath.maxChannelSpread = tinyText ? 0.05f : 0.06f;
        fastPath.maxLumaDelta = tinyText ? 0.0025f : 0.0035f;

        
        
        
        
        
        
        
        
        
        
        std::vector<uint8_t> output(static_cast<size_t>(before.pitch) * h, 0);
        bool anyModified = false;

        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        std::vector<float> rowMaskR(w), rowMaskG(w), rowMaskB(w);
        std::vector<float> rowSmR(w), rowSmG(w), rowSmB(w);
        std::vector<float> rowBgR(w), rowBgG(w), rowBgB(w);
        std::vector<uint8_t> rowFlags(w); 

        constexpr int kOpaqueEdgeScoreMin = 24;
        const auto colorDeltaScore = [](const uint8_t* p0, const uint8_t* p1) -> int
        {
            return std::abs(static_cast<int>(p0[0]) - static_cast<int>(p1[0]))
                + std::abs(static_cast<int>(p0[1]) - static_cast<int>(p1[1]))
                + std::abs(static_cast<int>(p0[2]) - static_cast<int>(p1[2]));
        };

        
        
        
        
        
        std::vector<float> prevSmR(w, 0.0f), prevSmG(w, 0.0f), prevSmB(w, 0.0f);
        bool hasPrevRow = false;

        auto extractMask = [](float ct, float bg, float text) -> float
        {
            const float d = text - bg;
            
            
            if (std::abs(d) < 0.5f) return 0.0f;
            return std::clamp((ct - bg) / d, 0.0f, 1.0f);
        };

        
        
        
        
        const bool useExplicitBg = opaqueBackground && bkColor != CLR_INVALID;
        const float explBgR_s = useExplicitBg ? static_cast<float>(GetRValue(bkColor)) : 0.0f;
        const float explBgG_s = useExplicitBg ? static_cast<float>(GetGValue(bkColor)) : 0.0f;
        const float explBgB_s = useExplicitBg ? static_cast<float>(GetBValue(bkColor)) : 0.0f;
        const float explBgR_lin = useExplicitBg ? sRGBToLinear(GetRValue(bkColor)) : 0.0f;
        const float explBgG_lin = useExplicitBg ? sRGBToLinear(GetGValue(bkColor)) : 0.0f;
        const float explBgB_lin = useExplicitBg ? sRGBToLinear(GetBValue(bkColor)) : 0.0f;

        for (int row = 0; row < h; ++row)
        {
            const uint8_t* bRow = before.data.data() + row * before.pitch;
            const uint8_t* aRow = after.data.data() + row * after.pitch;
            const uint8_t* aRowPrev = row > 0 ? after.data.data() + (row - 1) * after.pitch : nullptr;
            const uint8_t* aRowNext = row + 1 < h ? after.data.data() + (row + 1) * after.pitch : nullptr;
            uint8_t* oRow = output.data() + row * before.pitch;

            
            
            
            for (int col = 0; col < w; ++col)
            {
                const uint8_t* bp = bRow + col * 4; 
                const uint8_t* ap = aRow + col * 4;

                
                if (ap[0] == bp[0] && ap[1] == bp[1] && ap[2] == bp[2])
                {
                    rowFlags[col] = 0;
                    rowMaskR[col] = rowMaskG[col] = rowMaskB[col] = 0.0f;
                    continue;
                }

                
                
                
                if (useExplicitBg)
                {
                    rowBgR[col] = explBgR_lin;
                    rowBgG[col] = explBgG_lin;
                    rowBgB[col] = explBgB_lin;
                }
                else
                {
                    rowBgB[col] = sRGBToLinear(bp[0]);
                    rowBgG[col] = sRGBToLinear(bp[1]);
                    rowBgR[col] = sRGBToLinear(bp[2]);
                }

                
                const float bgB_s = useExplicitBg ? explBgB_s : static_cast<float>(bp[0]);
                const float bgG_s = useExplicitBg ? explBgG_s : static_cast<float>(bp[1]);
                const float bgR_s = useExplicitBg ? explBgR_s : static_cast<float>(bp[2]);
                const float ctB_s = static_cast<float>(ap[0]);
                const float ctG_s = static_cast<float>(ap[1]);
                const float ctR_s = static_cast<float>(ap[2]);

                const float mR = extractMask(ctR_s, bgR_s, textR_s_const);
                const float mG = extractMask(ctG_s, bgG_s, textG_s_const);
                const float mB = extractMask(ctB_s, bgB_s, textB_s_const);

                const float totalMask = std::max({mR, mG, mB});
                if (totalMask < 0.02f)
                {
                    
                    rowFlags[col] = 1;
                    rowMaskR[col] = rowMaskG[col] = rowMaskB[col] = 0.0f;
                    continue;
                }

                if (opaqueBackground && !useExplicitBg)
                {
                    
                    
                    
                    int edgeScore = 0;
                    if (col > 0)
                    {
                        edgeScore = std::max(edgeScore, colorDeltaScore(ap, aRow + (col - 1) * 4));
                    }
                    if (col + 1 < w)
                    {
                        edgeScore = std::max(edgeScore, colorDeltaScore(ap, aRow + (col + 1) * 4));
                    }
                    if (aRowPrev)
                    {
                        edgeScore = std::max(edgeScore, colorDeltaScore(ap, aRowPrev + col * 4));
                    }
                    if (aRowNext)
                    {
                        edgeScore = std::max(edgeScore, colorDeltaScore(ap, aRowNext + col * 4));
                    }

                    if (edgeScore < kOpaqueEdgeScoreMin)
                    {
                        rowFlags[col] = 1;
                        rowMaskR[col] = rowMaskG[col] = rowMaskB[col] = 0.0f;
                        continue;
                    }
                }

                
                rowFlags[col] = 2;
                rowMaskR[col] = mR;
                rowMaskG[col] = mG;
                rowMaskB[col] = mB;
            }

            
            
            
            
            
            
            
            
            
            
            
            
            
            
            constexpr float kFirSide = 0.08f;
            constexpr float kBilateralSigma = 0.15f; 
            constexpr float kSigma2x2 = 2.0f * kBilateralSigma * kBilateralSigma;

            for (int col = 0; col < w; ++col)
            {
                const float mR = rowMaskR[col];
                const float mG = rowMaskG[col];
                const float mB = rowMaskB[col];

                
                float lwR = 0.0f, lwG = 0.0f, lwB = 0.0f;
                float lR = mR, lG = mG, lB = mB;
                if (col > 0)
                {
                    lR = rowMaskR[col - 1];
                    lG = rowMaskG[col - 1];
                    lB = rowMaskB[col - 1];
                    const float dR = mR - lR, dG = mG - lG, dB = mB - lB;
                    lwR = kFirSide * std::exp(-(dR * dR) / kSigma2x2);
                    lwG = kFirSide * std::exp(-(dG * dG) / kSigma2x2);
                    lwB = kFirSide * std::exp(-(dB * dB) / kSigma2x2);
                }

                
                float rwR = 0.0f, rwG = 0.0f, rwB = 0.0f;
                float rR = mR, rG = mG, rB = mB;
                if (col < w - 1)
                {
                    rR = rowMaskR[col + 1];
                    rG = rowMaskG[col + 1];
                    rB = rowMaskB[col + 1];
                    const float dR = mR - rR, dG = mG - rG, dB = mB - rB;
                    rwR = kFirSide * std::exp(-(dR * dR) / kSigma2x2);
                    rwG = kFirSide * std::exp(-(dG * dG) / kSigma2x2);
                    rwB = kFirSide * std::exp(-(dB * dB) / kSigma2x2);
                }

                
                rowSmR[col] = lR * lwR + mR * (1.0f - lwR - rwR) + rR * rwR;
                rowSmG[col] = lG * lwG + mG * (1.0f - lwG - rwG) + rG * rwG;
                rowSmB[col] = lB * lwB + mB * (1.0f - lwB - rwB) + rB * rwB;
            }

            
            
            
            
            
            
            
            
            
            
            
            
            
            
            
            
            
            if (cfg.enableFractionalPositioning)
            {
                
                float sumR = 0.0f, sumB = 0.0f;
                float centR = 0.0f, centB = 0.0f;
                int textCount = 0;
                for (int col = 0; col < w; ++col)
                {
                    if (rowFlags[col] != 2) continue;
                    textCount++;
                    const float fcol = static_cast<float>(col);
                    sumR += rowSmR[col];
                    centR += rowSmR[col] * fcol;
                    sumB += rowSmB[col];
                    centB += rowSmB[col] * fcol;
                }

                
                if (textCount >= 3 && sumR > 0.5f && sumB > 0.5f)
                {
                    centR /= sumR;
                    centB /= sumB;

                    
                    
                    
                    float expectedSep = -0.667f; 
                    if (qdGen1) expectedSep = cfg.qdExpectedSepGen1;
                    else if (qdGen3) expectedSep = cfg.qdExpectedSepGen3;
                    else if (qdGen4) expectedSep = cfg.qdExpectedSepGen4;

                    
                    const float actualSep = centR - centB;
                    const float phaseError = (actualSep - expectedSep) * 0.5f;
                    const float shift = std::clamp(phaseError, -0.33f, 0.33f);

                    if (std::abs(shift) > 0.02f) 
                    {
                        
                        
                        const float t = std::abs(shift);
                        const int dir = (shift > 0.0f) ? -1 : 1; 

                        
                        std::vector<float> tmpR(rowSmR), tmpG(rowSmG), tmpB(rowSmB);
                        for (int col = 0; col < w; ++col)
                        {
                            const int srcCol = std::clamp(col + dir, 0, w - 1);
                            rowSmR[col] = tmpR[col] * (1.0f - t) + tmpR[srcCol] * t;
                            rowSmG[col] = tmpG[col] * (1.0f - t) + tmpG[srcCol] * t;
                            rowSmB[col] = tmpB[col] * (1.0f - t) + tmpB[srcCol] * t;
                        }
                    }
                }
            }

            
            
            
            
            
            
            
            
            
            
            
            
            
            if (qdPanel && hasPrevRow)
            {
                const int shift = (row % 2 == 0) ? 1 : -1;
                
                const float qdBlendBase = std::clamp(cfg.qdVerticalBlend, 0.0f, 0.30f);
                const float kVertBlend = qdBlendBase * std::min(toneStrength, 1.0f);

                for (int col = 0; col < w; ++col)
                {
                    const int adjCol = std::clamp(col + shift, 0, w - 1);
                    rowSmR[col] = rowSmR[col] * (1.0f - kVertBlend)
                        + prevSmR[adjCol] * kVertBlend;
                    rowSmG[col] = rowSmG[col] * (1.0f - kVertBlend)
                        + prevSmG[adjCol] * kVertBlend;
                    rowSmB[col] = rowSmB[col] * (1.0f - kVertBlend)
                        + prevSmB[adjCol] * kVertBlend;
                }
            }

            
            
            
            
            for (int col = 0; col < w; ++col)
            {
                if (rowFlags[col] != 2) continue;

                
                float maskR = applyStemDarkening(rowSmR[col], darkenAmount);
                float maskG = applyStemDarkening(rowSmG[col], darkenAmount);
                float maskB = applyStemDarkening(rowSmB[col], darkenAmount);

                
                
                
                if (!qdPanel && cfg.woledCrossTalkReduction > 0.0f)
                {
                    const float wSignal = std::min({maskR, maskG, maskB}) * cfg.woledCrossTalkReduction;
                    maskR = std::max(0.0f, maskR - wSignal);
                    maskG = std::max(0.0f, maskG - wSignal);
                    maskB = std::max(0.0f, maskB - wSignal);
                }

                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                
                

                float finalCovR, finalCovG, finalCovB;

                
                
                
                
                
                const float oledBlend = std::min(toneStrength, 1.0f);

                if (qdGen1)
                {
                    const float oledR = maskR * 0.66f + maskG * 0.34f;
                    const float oledB = maskG * 0.34f + maskB * 0.66f;
                    finalCovR = maskR + (oledR - maskR) * oledBlend;
                    finalCovG = maskG;
                    finalCovB = maskB + (oledB - maskB) * oledBlend;
                }
                else if (qdGen3)
                {
                    const float oledR = maskR * 0.75f + maskG * 0.25f;
                    const float oledB = maskG * 0.25f + maskB * 0.75f;
                    finalCovR = maskR + (oledR - maskR) * oledBlend;
                    finalCovG = maskG;
                    finalCovB = maskB + (oledB - maskB) * oledBlend;
                }
                else if (qdGen4)
                {
                    const float oledR = maskR * 0.75f + maskG * 0.25f;
                    const float oledB = maskG * 0.25f + maskB * 0.75f;
                    finalCovR = maskR + (oledR - maskR) * oledBlend;
                    finalCovG = maskG;
                    finalCovB = maskB + (oledB - maskB) * oledBlend;
                }
                else if (rgwbPanel)
                {
                    const float alpha_R = maskR;
                    const float alpha_G = maskR * 0.60f + maskG * 0.40f;
                    const float alpha_W = (maskG * 0.70f + maskB * 0.30f)
                        * (1.0f - cfg.woledCrossTalkReduction);
                    const float alpha_B = maskB;
                    finalCovR = std::max(alpha_R, alpha_W);
                    finalCovG = std::max(alpha_G, alpha_W);
                    finalCovB = std::max(alpha_B, alpha_W);
                }
                else
                {
                    const float alpha_R = maskR;
                    const float alpha_W = (maskR * 0.30f + maskG * 0.70f)
                        * (1.0f - cfg.woledCrossTalkReduction);
                    const float alpha_B = maskG * 0.40f + maskB * 0.60f;
                    const float alpha_G = maskB;
                    finalCovR = std::max(alpha_R, alpha_W);
                    finalCovG = std::max(alpha_G, alpha_W);
                    finalCovB = std::max(alpha_B, alpha_W);
                }

                const float baseCovR = std::clamp(finalCovR, 0.0f, 1.0f);
                const float baseCovG = std::clamp(finalCovG, 0.0f, 1.0f);
                const float baseCovB = std::clamp(finalCovB, 0.0f, 1.0f);

                const float yCov = 0.2126f * finalCovR
                    + 0.7152f * finalCovG
                    + 0.0722f * finalCovB;

                
                
                
                const int leftCol = std::max(col - 1, 0);
                const int rightCol = std::min(col + 1, w - 1);
                const float yLeft = 0.2126f * rowSmR[leftCol]
                    + 0.7152f * rowSmG[leftCol]
                    + 0.0722f * rowSmB[leftCol];
                const float yRight = 0.2126f * rowSmR[rightCol]
                    + 0.7152f * rowSmG[rightCol]
                    + 0.0722f * rowSmB[rightCol];
                const float yUp = hasPrevRow
                                      ? (0.2126f * prevSmR[col] + 0.7152f * prevSmG[col] + 0.0722f * prevSmB[col])
                                      : yCov;

                const float gradX = std::abs(yRight - yLeft);
                const float gradY = std::abs(yCov - yUp);
                const float channelSpread = std::max({finalCovR, finalCovG, finalCovB})
                    - std::min({finalCovR, finalCovG, finalCovB});
                const float thinness = std::clamp(1.0f - yCov * 2.0f, 0.0f, 1.0f);
                const float edgeRisk = ComputeEdgeRisk(gradX, gradY, channelSpread, thinness, edgeParams);

                
                
                const float bgLuma = 0.2126f * rowBgR[col]
                    + 0.7152f * rowBgG[col]
                    + 0.0722f * rowBgB[col];
                const float localContrast = std::abs(linTextLuma - bgLuma);
                const float chromaKeep =
                    ComputeAdaptiveChromaKeep(chromaKeepBase, localContrast, edgeRisk, edgeParams);
                finalCovR = yCov + (finalCovR - yCov) * chromaKeep;
                finalCovG = yCov + (finalCovG - yCov) * chromaKeep;
                finalCovB = yCov + (finalCovB - yCov) * chromaKeep;

                
                finalCovR = scurve(finalCovR);
                finalCovG = scurve(finalCovG);
                finalCovB = scurve(finalCovB);
                float targetY = scurve(yCov);

                
                
                
                
                

                if (std::abs(toneStrength - 1.0f) > 0.001f)
                {
                    finalCovR = std::clamp(baseCovR + (finalCovR - baseCovR) * toneStrength, 0.0f, 1.0f);
                    finalCovG = std::clamp(baseCovG + (finalCovG - baseCovG) * toneStrength, 0.0f, 1.0f);
                    finalCovB = std::clamp(baseCovB + (finalCovB - baseCovB) * toneStrength, 0.0f, 1.0f);
                    targetY = std::clamp(yCov + (targetY - yCov) * toneStrength, 0.0f, 1.0f);
                }

                const std::array<float, 3> solved = ApplyConstrainedChromaOptimization(
                    {finalCovR, finalCovG, finalCovB},
                    targetY,
                    edgeRisk,
                    localContrast,
                    channelSpread,
                    edgeParams,
                    fastPath);
                finalCovR = solved[0];
                finalCovG = solved[1];
                finalCovB = solved[2];

                
                const float bgR_lin = rowBgR[col];
                const float bgG_lin = rowBgG[col];
                const float bgB_lin = rowBgB[col];

                float outR = bgR_lin * (1.0f - finalCovR) + linTextR * finalCovR;
                float outG = bgG_lin * (1.0f - finalCovG) + linTextG * finalCovG;
                float outB = bgB_lin * (1.0f - finalCovB) + linTextB * finalCovB;

                const float covMax = std::max({finalCovR, finalCovG, finalCovB});
                if (cfg.toneParityV2Enabled)
                {
                    const float postGamma = ComputeToneParityPostGamma(cfg.gamma, cfg.oledGammaOutput);
                    ApplyToneParityPostComposite(outR, outG, outB,
                                                 bgR_lin, bgG_lin, bgB_lin,
                                                 covMax,
                                                 postGamma);
                }
                else if (cfg.gamma > 1.001f && covMax > 0.01f)
                {
                    const float gammaCorr = cfg.gamma;
                    outR = bgR_lin + (outR - bgR_lin) * std::pow(covMax, gammaCorr - 1.0f);
                    outG = bgG_lin + (outG - bgG_lin) * std::pow(covMax, gammaCorr - 1.0f);
                    outB = bgB_lin + (outB - bgB_lin) * std::pow(covMax, gammaCorr - 1.0f);
                    outR = std::clamp(outR, 0.0f, 1.0f);
                    outG = std::clamp(outG, 0.0f, 1.0f);
                    outB = std::clamp(outB, 0.0f, 1.0f);
                }

                oRow[col * 4 + 0] = linearToSRGB(outB);
                oRow[col * 4 + 1] = linearToSRGB(outG);
                oRow[col * 4 + 2] = linearToSRGB(outR);
                oRow[col * 4 + 3] = 0xFF;
                anyModified = true;
            }

            
            if (qdPanel)
            {
                std::copy(rowSmR.begin(), rowSmR.end(), prevSmR.begin());
                std::copy(rowSmG.begin(), rowSmG.end(), prevSmG.begin());
                std::copy(rowSmB.begin(), rowSmB.end(), prevSmB.begin());
                hasPrevRow = true;
            }
        }

        if (!anyModified) return;

        
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* dibBits = nullptr;
        HDC memDC = CreateCompatibleDC(hdc);
        if (!memDC) return;

        HBITMAP hBitmap = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
        if (!hBitmap || !dibBits)
        {
            DeleteDC(memDC);
            return;
        }

        std::memcpy(dibBits, output.data(), output.size());

        HGDIOBJ old = SelectObject(memDC, hBitmap);

        
        
        
        BLENDFUNCTION blend = {};
        blend.BlendOp = AC_SRC_OVER;
        blend.BlendFlags = 0;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        AlphaBlend(hdc, before.x, before.y, w, h, memDC, 0, 0, w, h, blend);

        SelectObject(memDC, old);
        DeleteObject(hBitmap);
        DeleteDC(memDC);
    }

    
    
    
    static thread_local bool g_insideHook = false;

    struct HookRefGuard
    {
        HookRefGuard() { ++g_activeHookCount; }
        ~HookRefGuard() { --g_activeHookCount; }
        HookRefGuard(const HookRefGuard&) = delete;
        HookRefGuard& operator=(const HookRefGuard&) = delete;
    };

    static BOOL WINAPI Hooked_ExtTextOutW(
        HDC hdc, int x, int y, UINT options,
        const RECT* lprc, LPCWSTR lpString, UINT cbCount, const INT* lpDx)
    {
        
        if (g_insideHook)
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);

        
        if (!lpString || cbCount == 0)
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);

        
        std::string monitorName = "";
        if (HWND hwnd = WindowFromDC(hdc); hwnd)
        {
            if (HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST); hmon)
            {
                MONITORINFOEXA mi;
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoA(hmon, &mi))
                {
                    monitorName = mi.szDevice;
                }
            }
        }

        const auto cfg = Config::Instance().GetData(monitorName);
        if (cfg.filterStrength <= 0.0f)
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);
        initColorMathLUTs(cfg.gammaMode == GammaMode::OLED);

        
        
        static thread_local float s_cachedDpi = 0.0f;
        static thread_local int s_dpiCallCount = 0;
        if (s_cachedDpi == 0.0f || (++s_dpiCallCount & 0xFF) == 0) 
            s_cachedDpi = static_cast<float>(GetDeviceCaps(hdc, LOGPIXELSX));

        const float dpi = s_cachedDpi;

        
        if (dpi >= cfg.highDpiThresholdHigh)
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);

        
        float dpiScale = 1.0f;
        if (dpi > cfg.highDpiThresholdLow)
        {
            dpiScale = 1.0f - std::clamp(
                (dpi - cfg.highDpiThresholdLow) / (cfg.highDpiThresholdHigh - cfg.highDpiThresholdLow),
                0.0f, 1.0f);
        }

        HookRefGuard refGuard;
        g_insideHook = true;

        
        RECT bounds = ComputeCaptureBounds(hdc, x, y, options, lprc,
                                           lpString, cbCount, lpDx);

        
        RECT clipBox = {};
        if (GetClipBox(hdc, &clipBox) != ERROR)
        {
            bounds.left = std::max(bounds.left, clipBox.left);
            bounds.top = std::max(bounds.top, clipBox.top);
            bounds.right = std::min(bounds.right, clipBox.right);
            bounds.bottom = std::min(bounds.bottom, clipBox.bottom);
        }

        const int captureW = bounds.right - bounds.left;
        const int captureH = bounds.bottom - bounds.top;

        if (captureW <= 0 || captureH <= 0)
        {
            g_insideHook = false;
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);
        }

        
        PixelCapture before = CaptureRegion(hdc, bounds.left, bounds.top,
                                            captureW, captureH);
        if (!before.IsValid())
        {
            g_insideHook = false;
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);
        }

        
        
        
        
        {
            ForceSubpixelRender ctGuard(hdc, &cfg);

            BOOL result = g_OrigExtTextOutW(hdc, x, y, options,
                                            lprc, lpString, cbCount, lpDx);

            
            PixelCapture after = CaptureRegion(hdc, bounds.left, bounds.top,
                                               captureW, captureH);

            if (after.IsValid())
            {
                const COLORREF textColor = GetTextColor(hdc);

                
                
                
                
                
                
                
                
                
                
                bool isComposited = false;
                if (const HWND hwnd = WindowFromDC(hdc))
                {
                    const LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
                    isComposited = ((exStyle & WS_EX_COMPOSITED) || (exStyle & WS_EX_LAYERED));
                }

                const bool opaqueBackground = (options & ETO_OPAQUE) && (lprc != nullptr);

                
                uint16_t fontWeight = 400;
                float emSizePx = static_cast<float>(captureH);
                {
                    HFONT hFont = static_cast<HFONT>(GetCurrentObject(hdc, OBJ_FONT));
                    if (hFont)
                    {
                        LOGFONTW lf = {};
                        if (GetObjectW(hFont, sizeof(lf), &lf))
                        {
                            fontWeight = static_cast<uint16_t>(
                                std::clamp(static_cast<int>(lf.lfWeight), 100, 900));
                        }

                        TEXTMETRICW tm = {};
                        if (GetTextMetricsW(hdc, &tm) && tm.tmHeight > 0)
                        {
                            emSizePx = static_cast<float>(tm.tmHeight);
                        }
                    }
                }

                
                
                const COLORREF bkColor = opaqueBackground ? GetBkColor(hdc) : CLR_INVALID;

                RemapToOLED(hdc, before, after, textColor, cfg,
                            opaqueBackground, isComposited, dpiScale, fontWeight, emSizePx,
                            bkColor);
            }

            g_insideHook = false;
            return result;
        }
    }

    static BOOL WINAPI Hooked_PolyTextOutW(HDC hdc, const POLYTEXTW* ppt, int cStrings)
    {
        if (g_insideHook || !ppt || cStrings <= 0)
            return g_OrigPolyTextOutW(hdc, ppt, cStrings);

        BOOL success = TRUE;
        for (int i = 0; i < cStrings; i++)
        {
            if (!Hooked_ExtTextOutW(hdc, ppt[i].x, ppt[i].y, ppt[i].uiFlags,
                                    &ppt[i].rcl, ppt[i].lpstr, ppt[i].n, ppt[i].pdx))
                success = FALSE;
        }
        return success;
    }

    
    
    static int WINAPI Hooked_DrawTextW(HDC hdc, LPCWSTR text, int len,
                                       LPRECT lprc, UINT fmt)
    {
        return g_OrigDrawTextW(hdc, text, len, lprc, fmt);
    }

    static int WINAPI Hooked_DrawTextExW(HDC hdc, LPWSTR text, int len,
                                         LPRECT lprc, UINT fmt, LPDRAWTEXTPARAMS p)
    {
        return g_OrigDrawTextExW(hdc, text, len, lprc, fmt, p);
    }

    bool InstallGDIHooks()
    {
        bool success = true;

        HMODULE hGdi32 = GetModuleHandleW(L"gdi32.dll");
        if (!hGdi32) hGdi32 = LoadLibraryW(L"gdi32.dll");
        if (hGdi32)
        {
            if (auto p = reinterpret_cast<ExtTextOutW_t>(GetProcAddress(hGdi32, "ExtTextOutW")))
            {
                const MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(p),
                                                       reinterpret_cast<LPVOID>(&Hooked_ExtTextOutW),
                                                       reinterpret_cast<LPVOID*>(&g_OrigExtTextOutW));
                if (status != MH_OK)
                {
                    PureTypeLog("MH_CreateHook(ExtTextOutW) failed: %s", MH_StatusToString(status));
                    success = false;
                }
            }
            else
            {
                PureTypeLog("ExtTextOutW not found");
                success = false;
            }

            if (auto p = reinterpret_cast<PolyTextOutW_t>(GetProcAddress(hGdi32, "PolyTextOutW")))
            {
                const MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(p),
                                                       reinterpret_cast<LPVOID>(&Hooked_PolyTextOutW),
                                                       reinterpret_cast<LPVOID*>(&g_OrigPolyTextOutW));
                if (status != MH_OK)
                {
                    PureTypeLog("MH_CreateHook(PolyTextOutW) failed: %s", MH_StatusToString(status));
                    success = false;
                }
            }
            else
            {
                PureTypeLog("PolyTextOutW not found");
                success = false;
            }
        }
        else
        {
            PureTypeLog("gdi32.dll not available");
            success = false;
        }

        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (!hUser32) hUser32 = LoadLibraryW(L"user32.dll");
        if (hUser32)
        {
            if (auto p = reinterpret_cast<DrawTextW_t>(GetProcAddress(hUser32, "DrawTextW")))
            {
                const MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(p),
                                                       reinterpret_cast<LPVOID>(&Hooked_DrawTextW),
                                                       reinterpret_cast<LPVOID*>(&g_OrigDrawTextW));
                if (status != MH_OK)
                {
                    PureTypeLog("MH_CreateHook(DrawTextW) failed: %s", MH_StatusToString(status));
                    success = false;
                }
            }
            else
            {
                PureTypeLog("DrawTextW not found");
                success = false;
            }

            if (auto p = reinterpret_cast<DrawTextExW_t>(GetProcAddress(hUser32, "DrawTextExW")))
            {
                const MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(p),
                                                       reinterpret_cast<LPVOID>(&Hooked_DrawTextExW),
                                                       reinterpret_cast<LPVOID*>(&g_OrigDrawTextExW));
                if (status != MH_OK)
                {
                    PureTypeLog("MH_CreateHook(DrawTextExW) failed: %s", MH_StatusToString(status));
                    success = false;
                }
            }
            else
            {
                PureTypeLog("DrawTextExW not found");
                success = false;
            }
        }
        else
        {
            PureTypeLog("user32.dll not available");
            success = false;
        }

        return success;
    }

    void RemoveGDIHooks()
    {
        if (const HMODULE hGdi32 = GetModuleHandleW(L"gdi32.dll"))
        {
            if (g_OrigExtTextOutW)
            {
                if (auto p = reinterpret_cast<LPVOID>(GetProcAddress(hGdi32, "ExtTextOutW")))
                    MH_RemoveHook(p);
                g_OrigExtTextOutW = nullptr;
            }
            if (g_OrigPolyTextOutW)
            {
                if (auto p = reinterpret_cast<LPVOID>(GetProcAddress(hGdi32, "PolyTextOutW")))
                    MH_RemoveHook(p);
                g_OrigPolyTextOutW = nullptr;
            }
        }
        if (const HMODULE hUser32 = GetModuleHandleW(L"user32.dll"))
        {
            if (g_OrigDrawTextW)
            {
                if (auto p = reinterpret_cast<LPVOID>(GetProcAddress(hUser32, "DrawTextW")))
                    MH_RemoveHook(p);
                g_OrigDrawTextW = nullptr;
            }
            if (g_OrigDrawTextExW)
            {
                if (auto p = reinterpret_cast<LPVOID>(GetProcAddress(hUser32, "DrawTextExW")))
                    MH_RemoveHook(p);
                g_OrigDrawTextExW = nullptr;
            }
        }
    }
}
