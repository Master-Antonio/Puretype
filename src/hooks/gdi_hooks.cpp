#include "hooks/gdi_hooks.h"
#include "config.h"
#include "puretype.h"

#include <MinHook.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <cstring>

#include "color_math.h"

extern void PureTypeLog(const char* fmt, ...);


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

    // =========================================================================
    // ARCHITECTURE: post-processing (universal compatibility)
    // =========================================================================
    //
    // The previous "replace rendering" approach rasterized glyphs with FreeType
    // and blitted the result, bypassing GDI entirely. This broke any framework
    // that pre-computed its own layout metrics (Qt, MFC, custom paint engines)
    // because those frameworks reserved bounding boxes based on GDI metrics —
    // our FreeType metrics differed, and the result was clipped or mis-placed.
    //
    // The new approach leaves GDI's rendering pipeline COMPLETELY INTACT:
    //
    //   1. Capture the HDC region before calling GDI.
    //   2. Let GDI render with ClearType (unmodified call).
    //   3. Capture the same region after.
    //   4. For every pixel that changed: extract the per-channel subpixel
    //      coverage ClearType encoded, then re-blend using OLED-correct mapping.
    //   5. Write the result back to the HDC.
    //
    // This is universally compatible because:
    //   - GDI handles ALL metrics, positioning, clipping, font loading.
    //   - We only change pixel VALUES, never layout or advance widths.
    //   - Works identically for Qt5/Qt6, MFC, WinForms, Win32, WPF-GDI,
    //     EqualizerAPO, VoiceMeeter, legacy apps, CJK IMEs — everything.
    //
    // What we gain over raw ClearType on OLED panels:
    //
    //   WOLED (RWBG/RGWB — LG panels):
    //     ClearType treats pixels as an RGB stripe (R=left, G=centre, B=right).
    //     WOLED has a WHITE subpixel (R, W, B, G — 4 per pixel). The W subpixel
    //     drives R+G+B simultaneously via TCON hardware. ClearType does not model
    //     this; we reconstruct the correct W contribution from the per-channel
    //     coverages, eliminating the grey haze on dark backgrounds.
    //
    //   QD-OLED triangular (Samsung — Dell AW3423DW, Odyssey G8, etc.):
    //     ClearType's filter is optimised for horizontal RGB stripe panels.
    //     We re-apply the recovered coverages with per-channel blending without
    //     the stripe-specific fringing correction, which better matches the
    //     triangular layout where all three subpixels share vertical space.
    //
    //   Both panels benefit from the BT.709 luma anchoring, chromaKeep
    //   calibration, and the S-curve readability boost.
    // =========================================================================

    // -------------------------------------------------------------------------
    // Pixel region capture
    // -------------------------------------------------------------------------
    struct PixelCapture
    {
        std::vector<uint8_t> data; // BGRA, row-major, top-down
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

    // -------------------------------------------------------------------------
    // Compute the bounding rect to capture.
    //
    // Conservative: a few extra pixels are fine (only changed pixels get
    // remapped). Missing pixels means missing OLED correction. +4 px margin.
    // -------------------------------------------------------------------------
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

    // -------------------------------------------------------------------------
    // ForceSubpixelRender: RAII guard that temporarily replaces the font on
    // the DC with a ClearType-quality copy of itself.
    //
    // Problem:
    //   PureType's OLED remapping extracts per-channel R/G/B subpixel coverage
    //   from the GDI output. This only works when GDI writes different values to
    //   R, G, and B — i.e. when ClearType is active. With grayscale AA or no AA,
    //   GDI writes R == G == B per pixel, giving us zero subpixel information.
    //
    // Solution:
    //   Before calling GDI, clone the current LOGFONT with lfQuality forced to
    //   CLEARTYPE_QUALITY and select the clone into the DC. GDI then renders with
    //   full ClearType subpixel output regardless of the system AA setting. After
    //   GDI returns and we have captured the pixels, the destructor restores the
    //   original font and deletes the clone.
    //
    // Why this is safe:
    //   - GDI still computes ALL layout (advance widths, bearings, clipping).
    //     The LOGFONT face name, size, weight and style are identical — only
    //     lfQuality changes. GDI uses the same glyph outlines and hinting; only
    //     the AA compositing stage is different.
    //   - Metrics (tmHeight, tmAscent, tmDescent, GetTextExtentPoint32) are
    //     unaffected by lfQuality — they are the same for CT and grayscale fonts
    //     of the same face/size. Qt, MFC, and other frameworks compute their
    //     bounding boxes before our hook fires, so they never see the clone.
    //   - The clone is scoped to one ExtTextOutW call. It is deleted immediately
    //     after capture regardless of any error path.
    //   - If CreateFontIndirectW fails (unlikely, but defensive), the guard no-ops
    //     and leaves the original font in place — GDI renders normally without
    //     OLED correction for that call.
    // -------------------------------------------------------------------------
    struct ForceSubpixelRender
    {
        HDC m_hdc = nullptr;
        HFONT m_origFont = nullptr;
        HFONT m_ctFont = nullptr;
        bool m_active = false;

        explicit ForceSubpixelRender(HDC hdc)
        {
            m_hdc = hdc;
            HFONT origFont = static_cast<HFONT>(GetCurrentObject(hdc, OBJ_FONT));
            if (!origFont) return;

            LOGFONTW lf = {};
            if (GetObjectW(origFont, sizeof(lf), &lf) == 0) return;

            // Already ClearType — no work needed.
            if (lf.lfQuality == CLEARTYPE_QUALITY ||
                lf.lfQuality == CLEARTYPE_NATURAL_QUALITY)
            {
                m_active = true; // mark active so destructor is a no-op
                return;
            }

            // Clone with ClearType quality.
            lf.lfQuality = CLEARTYPE_QUALITY;
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

        // Returns true if GDI will produce ClearType output for this call.
        [[nodiscard]] bool IsActive() const { return m_active; }

        ForceSubpixelRender(const ForceSubpixelRender&) = delete;
        ForceSubpixelRender& operator=(const ForceSubpixelRender&) = delete;
    };

    // -------------------------------------------------------------------------
    // RemapToOLED
    //
    // Extracts per-channel ClearType subpixel coverage from the R/G/B delta
    // between before (background) and after (GDI ClearType output), then
    // re-blends using the target OLED panel's physical subpixel layout.
    //
    // Input contract: after was produced by GDI with ClearType active (either
    // naturally or forced by ForceSubpixelRender above). R, G, B channels
    // carry independent per-subpixel coverage values.
    // -------------------------------------------------------------------------
    static void RemapToOLED(HDC hdc,
                            const PixelCapture& before,
                            const PixelCapture& after,
                            COLORREF textColor,
                            const ConfigData& cfg)
    {
        if (!before.IsValid() || !after.IsValid()) return;
        if (before.width != after.width || before.height != after.height) return;

        const int w = before.width;
        const int h = before.height;

        const float linTextR = sRGBToLinear(GetRValue(textColor));
        const float linTextG = sRGBToLinear(GetGValue(textColor));
        const float linTextB = sRGBToLinear(GetBValue(textColor));
        // sRGB text bytes used for mask extraction in sRGB space.
        const float textR_s_const = static_cast<float>(GetRValue(textColor));
        const float textG_s_const = static_cast<float>(GetGValue(textColor));
        const float textB_s_const = static_cast<float>(GetBValue(textColor));

        const bool qdGen1 = (cfg.panelType == PanelType::QD_OLED_GEN1);
        const bool qdGen3 = (cfg.panelType == PanelType::QD_OLED_GEN3);
        const bool qdGen4 = (cfg.panelType == PanelType::QD_OLED_GEN4);
        const bool qdPanel = qdGen1 || qdGen3 || qdGen4;
        const bool rgwbPanel = (cfg.panelType == PanelType::RGWB);

        // ToneMapper S-curve LUT -----------------------------------------------
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

        // chromaKeep -----------------------------------------------------------
        float chromaKeep;
        if (tinyText) chromaKeep = qdPanel ? 0.70f : 0.72f;
        else if (smallText) chromaKeep = qdPanel ? 0.75f : 0.77f;
        else if (h <= 32) chromaKeep = qdPanel ? 0.80f : 0.82f;
        else chromaKeep = qdPanel ? 0.83f : 0.85f;

        // Output buffer — zero-initialised: alpha=0 for all pixels by default.
        // Text pixels receive alpha=255; background pixels stay at alpha=0.
        // AlphaBlend(AC_SRC_ALPHA) below skips alpha=0 pixels entirely,
        // leaving compositor content intact on composited DCs.
        std::vector<uint8_t> output(static_cast<size_t>(before.pitch) * h, 0);
        bool anyModified = false;

        for (int row = 0; row < h; ++row)
        {
            const uint8_t* bRow = before.data.data() + row * before.pitch;
            const uint8_t* aRow = after.data.data() + row * after.pitch;
            uint8_t* oRow = output.data() + row * before.pitch;

            for (int col = 0; col < w; ++col)
            {
                const uint8_t* bp = bRow + col * 4; // BGRA
                const uint8_t* ap = aRow + col * 4;

                // Background pixel — stays at alpha=0, not written back.
                if (ap[0] == bp[0] && ap[1] == bp[1] && ap[2] == bp[2]) continue;
                anyModified = true;

                // --- Step 1: background and ClearType output values -----------
                // Keep sRGB bytes for mask extraction (Step 2).
                // Linearise for the final blend (Step 6).
                const float bgB_lin = sRGBToLinear(bp[0]);
                const float bgG_lin = sRGBToLinear(bp[1]);
                const float bgR_lin = sRGBToLinear(bp[2]);

                // --- Step 2: extract per-channel subpixel coverage masks -------
                //
                // GDI ClearType composites in sRGB-encoded (gamma-compressed) space:
                //
                //   ct_byte = bg_byte × (1 − m) + text_byte × m
                //   →  m = (ct_byte − bg_byte) / (text_byte − bg_byte)
                //
                // Solving in LINEAR light is wrong because the blending was not
                // performed in linear light. For mid-coverage values where the sRGB
                // gamma curve deviates most from linear, linearising before extraction
                // over- or under-estimates m, producing systematic errors in the
                // per-channel coverage that then propagate through the OLED remap.
                //
                // Correct approach: solve in sRGB integer space, then use the mask
                // (which is a linear coverage fraction 0..1) for the final linear-light
                // blend in Step 6. The mask itself is dimensionless — it does not carry
                // a gamma encoding.
                const float bgB_s = static_cast<float>(bp[0]);
                const float bgG_s = static_cast<float>(bp[1]);
                const float bgR_s = static_cast<float>(bp[2]);
                const float ctB_s = static_cast<float>(ap[0]);
                const float ctG_s = static_cast<float>(ap[1]);
                const float ctR_s = static_cast<float>(ap[2]);
                // sRGB text colour bytes (pre-computed once per call site below).
                const float textB_s = textB_s_const;
                const float textG_s = textG_s_const;
                const float textR_s = textR_s_const;

                auto extractMask = [](float ct, float bg, float text) -> float
                {
                    const float d = text - bg;
                    // Use 0.5 as minimum delta to avoid division by near-zero
                    // in sRGB integer space (values 0..255).
                    if (std::abs(d) < 0.5f) return 0.0f;
                    return std::clamp((ct - bg) / d, 0.0f, 1.0f);
                };

                const float maskR = extractMask(ctR_s, bgR_s, textR_s);
                const float maskG = extractMask(ctG_s, bgG_s, textG_s);
                const float maskB = extractMask(ctB_s, bgB_s, textB_s);

                // --- Step 3: OLED channel reconstruction -----------------------
                //
                // ClearType subpixel centers (RGB stripe, 3 per pixel):
                //   CT_R = 0.1667    CT_G = 0.5000    CT_B = 0.8333
                //   CT span between adjacent samples = 0.3333
                //
                // Physical subpixel centers — MEASURED FROM PANEL MICROSCOPY:
                //
                // ── QD-OLED Gen 1-2 ────────────────────────────────────────────
                //   Oval subpixels. R noticeably larger than B (luminance efficiency).
                //   Asymmetric centers: R ≈ 0.280, G = 0.500, B ≈ 0.720
                //   (AW3423DW, AW3423DWF, Odyssey G8 OLED 34" gen1)
                //
                //   R at 0.280 → lerp(CT_R=0.167, CT_G=0.500):
                //     wR = (0.500−0.280)/0.333 = 0.661 ≈ 0.66
                //     wG = (0.280−0.167)/0.333 = 0.339 ≈ 0.34
                //   G at 0.500 → exact CT_G. Use maskG.
                //   B at 0.720 → lerp(CT_G=0.500, CT_B=0.833):
                //     wG = (0.833−0.720)/0.333 = 0.339 ≈ 0.34
                //     wB = (0.720−0.500)/0.333 = 0.661 ≈ 0.66
                //
                // ── QD-OLED Gen 3 ──────────────────────────────────────────────
                //   Rectangular subpixels. R slightly wider than B but nearly equal.
                //   Symmetric centers: R ≈ 0.250, G = 0.500, B ≈ 0.750
                //   (Odyssey G8 OLED 27" QHD, Dell AW2725DF, 32" 4K models)
                //
                //   R at 0.250 → lerp(CT_R, CT_G): wR=0.750, wG=0.250
                //   G at 0.500 → exact CT_G. Use maskG.
                //   B at 0.750 → lerp(CT_G, CT_B): wG=0.250, wB=0.750
                //
                // ── QD-OLED Gen 4 ──────────────────────────────────────────────
                //   Rectangular subpixels. R ≈ B near-equal width.
                //   Symmetric centers: R ≈ 0.250, G = 0.500, B ≈ 0.750
                //   (MSI MPG 272URX, 27" 4K UHD models 2024-2025)
                //   Same geometry as Gen 3 — weights identical; kept separate
                //   for future per-generation tuning if needed.
                //
                // ── WOLED RWBG ─────────────────────────────────────────────────
                //   W ≈ 40% pixel width, R=B=G ≈ 20% each (measured from image).
                //   Centers: R=0.100, W=0.400, B=0.700, G=0.900
                //
                //   R at 0.100 → CT_R nearest. maskR. Δ=0.067
                //   W at 0.400 → lerp(CT_R, CT_G): wR=0.300, wG=0.700
                //   B at 0.700 → lerp(CT_G, CT_B): wG=0.400, wB=0.600
                //   G at 0.900 → CT_B nearest. maskB. Δ=0.067
                //
                // ── WOLED RGWB ─────────────────────────────────────────────────
                //   Same physical panel as RWBG, different order.
                //   Centers: R=0.100, G=0.300, W=0.600, B=0.900
                //
                //   R at 0.100 → maskR. Δ=0.067
                //   G at 0.300 → lerp(CT_R, CT_G): wR=0.600, wG=0.400
                //   W at 0.600 → lerp(CT_G, CT_B): wG=0.700, wB=0.300
                //   B at 0.900 → maskB. Δ=0.067

                float finalCovR, finalCovG, finalCovB;

                if (qdGen1)
                {
                    // Gen 1-2: asymmetric R/B (R larger, shifted left; B smaller, shifted right)
                    // R at 0.280: wR=0.66, wG=0.34
                    // G at 0.500: exact
                    // B at 0.720: wG=0.34, wB=0.66
                    finalCovR = maskR * 0.66f + maskG * 0.34f;
                    finalCovG = maskG;
                    finalCovB = maskG * 0.34f + maskB * 0.66f;
                }
                else if (qdGen3)
                {
                    // Gen 3: rectangular, nearly symmetric
                    // R at 0.250: wR=0.75, wG=0.25
                    // G at 0.500: exact
                    // B at 0.750: wG=0.25, wB=0.75
                    finalCovR = maskR * 0.75f + maskG * 0.25f;
                    finalCovG = maskG;
                    finalCovB = maskG * 0.25f + maskB * 0.75f;
                }
                else if (qdGen4)
                {
                    // Gen 4: rectangular, R ≈ B equal — same center positions as gen3
                    // R at 0.250: wR=0.75, wG=0.25
                    // G at 0.500: exact
                    // B at 0.750: wG=0.25, wB=0.75
                    finalCovR = maskR * 0.75f + maskG * 0.25f;
                    finalCovG = maskG;
                    finalCovB = maskG * 0.25f + maskB * 0.75f;
                }
                else if (rgwbPanel)
                {
                    // RGWB: measured centers R=0.100, G=0.300, W=0.600, B=0.900
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
                    // RWBG (default): measured centers R=0.100, W=0.400, B=0.700, G=0.900
                    const float alpha_R = maskR;
                    const float alpha_W = (maskR * 0.30f + maskG * 0.70f)
                        * (1.0f - cfg.woledCrossTalkReduction);
                    const float alpha_B = maskG * 0.40f + maskB * 0.60f;
                    const float alpha_G = maskB;
                    finalCovR = std::max(alpha_R, alpha_W);
                    finalCovG = std::max(alpha_G, alpha_W);
                    finalCovB = std::max(alpha_B, alpha_W);
                }

                // --- Step 4: chromaKeep (BT.709 luma anchor) ------------------
                const float yCov = 0.2126f * finalCovR
                    + 0.7152f * finalCovG
                    + 0.0722f * finalCovB;
                finalCovR = yCov + (finalCovR - yCov) * chromaKeep;
                finalCovG = yCov + (finalCovG - yCov) * chromaKeep;
                finalCovB = yCov + (finalCovB - yCov) * chromaKeep;

                // --- Step 5: S-curve readability boost ------------------------
                finalCovR = scurve(finalCovR);
                finalCovG = scurve(finalCovG);
                finalCovB = scurve(finalCovB);

                // --- Step 6: final per-channel blend  bg → textColor ----------
                // Background is in linear light (bgR_lin etc.).
                // Coverage masks are dimensionless [0,1] — no gamma.
                // Text colour is in linear light (linTextR etc.).
                const float outR = bgR_lin * (1.0f - finalCovR) + linTextR * finalCovR;
                const float outG = bgG_lin * (1.0f - finalCovG) + linTextG * finalCovG;
                const float outB = bgB_lin * (1.0f - finalCovB) + linTextB * finalCovB;

                oRow[col * 4 + 0] = linearToSRGB(outB);
                oRow[col * 4 + 1] = linearToSRGB(outG);
                oRow[col * 4 + 2] = linearToSRGB(outR);
                oRow[col * 4 + 3] = 0xFF;
            }
        }

        if (!anyModified) return;

        // Write remapped pixels back to the HDC.
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

        // AlphaBlend(AC_SRC_ALPHA): alpha=0 pixels leave destination untouched,
        // alpha=255 pixels are replaced with our OLED-correct values.
        // This prevents writing opaque black over compositor content on composited DCs.
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

    // -------------------------------------------------------------------------
    // Hook
    // -------------------------------------------------------------------------
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
        // Re-entrancy guard: prevents processing our own BitBlt calls.
        if (g_insideHook)
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);

        // Skip empty calls (opaque background fill with no text).
        if (!lpString || cbCount == 0)
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);

        const auto& cfg = Config::Instance().Data();
        if (cfg.filterStrength <= 0.0f)
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);

        HookRefGuard refGuard;
        g_insideHook = true;

        // Step 1 — compute capture region.
        RECT bounds = ComputeCaptureBounds(hdc, x, y, options, lprc,
                                           lpString, cbCount, lpDx);

        // Clamp to DC clip box.
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

        // Step 2 — capture BEFORE.
        PixelCapture before = CaptureRegion(hdc, bounds.left, bounds.top,
                                            captureW, captureH);
        if (!before.IsValid())
        {
            g_insideHook = false;
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);
        }

        // Step 3 — force ClearType, let GDI render with original options.
        // ETO_OPAQUE is passed through unchanged — GDI handles the background fill.
        // The AlphaBlend writeback (alpha=0 for background pixels) means we never
        // overwrite compositor content regardless of what GDI puts in those pixels.
        {
            ForceSubpixelRender ctGuard(hdc);

            BOOL result = g_OrigExtTextOutW(hdc, x, y, options,
                                            lprc, lpString, cbCount, lpDx);

            // Step 4 — capture AFTER (ClearType subpixel data now present).
            PixelCapture after = CaptureRegion(hdc, bounds.left, bounds.top,
                                               captureW, captureH);

            if (after.IsValid())
            {
                const COLORREF textColor = GetTextColor(hdc);
                RemapToOLED(hdc, before, after, textColor, cfg);
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

    // DrawTextW / DrawTextExW delegate to GDI which calls ExtTextOutW internally.
    // Intercepting both layers would cause double-processing — pass through.
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
                if (MH_CreateHook(reinterpret_cast<LPVOID>(p),
                                  reinterpret_cast<LPVOID>(&Hooked_ExtTextOutW),
                                  reinterpret_cast<LPVOID*>(&g_OrigExtTextOutW)) != MH_OK)
                {
                    PureTypeLog("MH_CreateHook(ExtTextOutW) failed");
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
                MH_CreateHook(reinterpret_cast<LPVOID>(p),
                              reinterpret_cast<LPVOID>(&Hooked_PolyTextOutW),
                              reinterpret_cast<LPVOID*>(&g_OrigPolyTextOutW));
            }
        }

        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (!hUser32) hUser32 = LoadLibraryW(L"user32.dll");
        if (hUser32)
        {
            if (auto p = reinterpret_cast<DrawTextW_t>(GetProcAddress(hUser32, "DrawTextW")))
                MH_CreateHook(reinterpret_cast<LPVOID>(p),
                              reinterpret_cast<LPVOID>(&Hooked_DrawTextW),
                              reinterpret_cast<LPVOID*>(&g_OrigDrawTextW));

            if (auto p = reinterpret_cast<DrawTextExW_t>(GetProcAddress(hUser32, "DrawTextExW")))
                MH_CreateHook(reinterpret_cast<LPVOID>(p),
                              reinterpret_cast<LPVOID>(&Hooked_DrawTextExW),
                              reinterpret_cast<LPVOID*>(&g_OrigDrawTextExW));
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