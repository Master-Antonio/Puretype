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
#include "render_optimizer.h"
#include "stem_darkening.h"

extern void PureTypeLog(const char* fmt, ...);

// ── Inter font detection helper (GDI path) ──────────────────────────
// Checks whether the LOGFONT face name is "Inter" or "Inter Variable".
static bool IsInterFontGDI(const wchar_t* faceName)
{
    if (!faceName) return false;
    // Case-insensitive prefix match: "Inter" covers "Inter", "Inter Variable", etc.
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

        explicit ForceSubpixelRender(HDC hdc, const ConfigData* cfg = nullptr)
        {
            m_hdc = hdc;
            HFONT origFont = static_cast<HFONT>(GetCurrentObject(hdc, OBJ_FONT));
            if (!origFont) return;

            LOGFONTW lf = {};
            if (GetObjectW(origFont, sizeof(lf), &lf) == 0) return;

            // Override weight for Inter font when configured.
            const bool isInter = IsInterFontGDI(lf.lfFaceName);
            bool needsClone = false;

            if (isInter && cfg && cfg->interFontWeight > 0)
            {
                lf.lfWeight = static_cast<LONG>(cfg->interFontWeight);
                needsClone = true;
            }

            // Already ClearType — only clone if we need to override weight.
            if (lf.lfQuality == CLEARTYPE_QUALITY ||
                lf.lfQuality == CLEARTYPE_NATURAL_QUALITY)
            {
                if (!needsClone)
                {
                    m_active = true; // mark active so destructor is a no-op
                    return;
                }
            }
            else
            {
                // Clone with ClearType quality.
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
                            const ConfigData& cfg,
                            bool opaqueBackground, // ETO_OPAQUE was set (GDI filled background)
                            bool isComposited, // DC belongs to a composited/layered window
                            float dpiScale = 1.0f, // 1.0 = full effect, 0.0 = skip (DPI fade)
                            uint16_t fontWeight = 400) // LOGFONT::lfWeight
    {
        if (!before.IsValid() || !after.IsValid()) return;
        if (before.width != after.width || before.height != after.height) return;

        const int w = before.width;
        const int h = before.height;

        const float linTextR = sRGBToLinear(GetRValue(textColor));
        const float linTextG = sRGBToLinear(GetGValue(textColor));
        const float linTextB = sRGBToLinear(GetBValue(textColor));
        // BT.709 text luminance — used for adaptive chromaKeep (#4).
        const float linTextLuma = 0.2126f * linTextR + 0.7152f * linTextG + 0.0722f * linTextB;
        // sRGB text bytes used for mask extraction in sRGB space.
        const float textR_s_const = static_cast<float>(GetRValue(textColor));
        const float textG_s_const = static_cast<float>(GetGValue(textColor));
        const float textB_s_const = static_cast<float>(GetBValue(textColor));

        const bool qdGen1 = (cfg.panelType == PanelType::QD_OLED_GEN1);
        const bool qdGen3 = (cfg.panelType == PanelType::QD_OLED_GEN3);
        const bool qdGen4 = (cfg.panelType == PanelType::QD_OLED_GEN4);
        const bool qdPanel = qdGen1 || qdGen3 || qdGen4;
        const bool rgwbPanel = (cfg.panelType == PanelType::RGWB);
        const EdgeAdaptiveParams edgeParams = GetEdgeAdaptiveParams(qdPanel);

        // Stem darkening — matches FreeType path (subpixel_filter.cpp)
        const float emSize = static_cast<float>(h);
        const float darkenAmount = cfg.stemDarkeningEnabled
                                       ? computeDarkenAmount(emSize, cfg.stemDarkeningStrength, fontWeight)
                                       : 0.0f;

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
        // Base value from glyph height; per-pixel adaptive modulation in Pass 3.
        // Higher values preserve more per-channel subpixel detail, improving
        // perceived sharpness at the cost of slightly more color fringing.
        float chromaKeepBase;
        if (tinyText) chromaKeepBase = qdPanel ? 0.78f : 0.77f;
        else if (smallText) chromaKeepBase = qdPanel ? 0.82f : 0.80f;
        else if (h <= 32) chromaKeepBase = qdPanel ? 0.86f : 0.84f;
        else chromaKeepBase = qdPanel ? 0.88f : 0.87f;

        // Font-weight aware adjustment: thin fonts are more sensitive to fringing.
        if (fontWeight > 0)
        {
            const float weightNorm = std::clamp(static_cast<float>(fontWeight) / 400.0f, 0.5f, 2.0f);
            chromaKeepBase *= (0.85f + 0.15f * weightNorm);
        }

        // DPI-aware chromaKeep reduction (#7).
        // At high DPI, reduce chroma to minimize fringing before reducing filter strength.
        if (dpiScale < 1.0f)
            chromaKeepBase *= 0.4f + 0.6f * dpiScale; // ramps from base to 40% of base
        const float toneStrength = std::clamp(cfg.filterStrength * dpiScale, 0.0f, 5.0f);

        ConstrainedChromaFastPath fastPath;
        fastPath.maxEdgeRisk = tinyText ? 0.05f : 0.08f;
        fastPath.maxChannelSpread = tinyText ? 0.05f : 0.06f;
        fastPath.maxLumaDelta = tinyText ? 0.0025f : 0.0035f;

        // Output buffer — zero-initialised: alpha=0 for all pixels by default.
        //
        // Per-pixel alpha strategy:
        //   alpha = 0   → AlphaBlend skips this pixel (compositor content intact)
        //   alpha = 255 → pixel is replaced with our value
        //
        // Three categories of changed pixels:
        //   A) before == after: nothing changed → keep alpha=0, skip
        //      Exception: ETO_OPAQUE background fill on non-composited DC → write ap, alpha=255
        //
        //   B) before != after, masks all ≈ 0: mask extraction failed.
        //      This happens when:
        //        - GetTextColor() returns wrong color (e.g., some dialog controls)
        //        - ETO_OPAQUE fill went in the opposite direction from textColor
        //          (old text area overwritten with background fill — "ghost chars" in Notepad)
        //      Fix: write GDI output (ap) directly on non-composited DCs.
        //      On composited DCs keep alpha=0 — writing ap could overwrite compositor content.
        //
        //   C) before != after, masks > threshold: normal text pixel → OLED remap, alpha=255
        std::vector<uint8_t> output(static_cast<size_t>(before.pitch) * h, 0);
        bool anyModified = false;

        // Per-row buffers for multi-pass processing.
        //
        // The FreeType path applies a 3-tap FIR (0.25/0.50/0.25) to its coverage
        // masks before the OLED remap. The old GDI path processed each pixel in
        // isolation, so ClearType quantization artifacts (abrupt mask changes
        // between adjacent pixels on the same glyph) passed through unsmoothed.
        //
        // New architecture: 3 passes per row.
        //   Pass 1: extract masks + linearised background for every pixel, categorise.
        //   Pass 2: 3-tap FIR horizontal smoothing on extracted masks.
        //   Pass 3: OLED remap + chromaKeep + S-curve + blend using smoothed masks.
        //
        // Pixel category flags stored in rowFlags[]:
        //   0 = Category A (unchanged) — already handled in Pass 1.
        //   1 = Category B (mask ≈ 0, changed) — GDI passthrough, handled in Pass 1.
        //   2 = Category C (valid text pixel) — processed in Pass 3.
        std::vector<float> rowMaskR(w), rowMaskG(w), rowMaskB(w);
        std::vector<float> rowSmR(w), rowSmG(w), rowSmB(w);
        std::vector<float> rowBgR(w), rowBgG(w), rowBgB(w);
        std::vector<uint8_t> rowFlags(w); // 0=skip, 1=passthrough, 2=text

        // Previous-row smoothed masks for QD-OLED vertical blending (#6).
        // QD-OLED triangular layouts have even/odd rows offset by ~1.5 subpixels.
        // We approximate this with 25% blending from the adjacent row, shifted ±1 pixel.
        // In the GDI path we only have the composited output, so we use the previous
        // row's masks as a one-row lookahead approximation.
        std::vector<float> prevSmR(w, 0.0f), prevSmG(w, 0.0f), prevSmB(w, 0.0f);
        bool hasPrevRow = false;

        auto extractMask = [](float ct, float bg, float text) -> float
        {
            const float d = text - bg;
            // Use 0.5 as minimum delta to avoid division by near-zero
            // in sRGB integer space (values 0..255).
            if (std::abs(d) < 0.5f) return 0.0f;
            return std::clamp((ct - bg) / d, 0.0f, 1.0f);
        };

        for (int row = 0; row < h; ++row)
        {
            const uint8_t* bRow = before.data.data() + row * before.pitch;
            const uint8_t* aRow = after.data.data() + row * after.pitch;
            uint8_t* oRow = output.data() + row * before.pitch;

            // -----------------------------------------------------------------
            // Pass 1: extract masks + categorise every pixel in this row
            // -----------------------------------------------------------------
            for (int col = 0; col < w; ++col)
            {
                const uint8_t* bp = bRow + col * 4; // BGRA
                const uint8_t* ap = aRow + col * 4;

                // Category A: pixel unchanged between before and after.
                if (ap[0] == bp[0] && ap[1] == bp[1] && ap[2] == bp[2])
                {
                    rowFlags[col] = 0;
                    rowMaskR[col] = rowMaskG[col] = rowMaskB[col] = 0.0f;

                    if (opaqueBackground && !isComposited)
                    {
                        oRow[col * 4 + 0] = ap[0];
                        oRow[col * 4 + 1] = ap[1];
                        oRow[col * 4 + 2] = ap[2];
                        oRow[col * 4 + 3] = 0xFF;
                        anyModified = true;
                    }
                    continue;
                }

                // Pixel changed.
                anyModified = true;

                // Linearise background for the final blend (Pass 3).
                rowBgB[col] = sRGBToLinear(bp[0]);
                rowBgG[col] = sRGBToLinear(bp[1]);
                rowBgR[col] = sRGBToLinear(bp[2]);

                // Extract per-channel subpixel coverage masks in sRGB space.
                const float bgB_s = static_cast<float>(bp[0]);
                const float bgG_s = static_cast<float>(bp[1]);
                const float bgR_s = static_cast<float>(bp[2]);
                const float ctB_s = static_cast<float>(ap[0]);
                const float ctG_s = static_cast<float>(ap[1]);
                const float ctR_s = static_cast<float>(ap[2]);

                const float mR = extractMask(ctR_s, bgR_s, textR_s_const);
                const float mG = extractMask(ctG_s, bgG_s, textG_s_const);
                const float mB = extractMask(ctB_s, bgB_s, textB_s_const);

                const float totalMask = std::max({mR, mG, mB});
                if (totalMask < 0.02f)
                {
                    // Category B: mask extraction failed.
                    rowFlags[col] = 1;
                    rowMaskR[col] = rowMaskG[col] = rowMaskB[col] = 0.0f;
                    if (!isComposited)
                    {
                        oRow[col * 4 + 0] = ap[0];
                        oRow[col * 4 + 1] = ap[1];
                        oRow[col * 4 + 2] = ap[2];
                        oRow[col * 4 + 3] = 0xFF;
                    }
                    continue;
                }

                // Category C: valid text pixel.
                rowFlags[col] = 2;
                rowMaskR[col] = mR;
                rowMaskG[col] = mG;
                rowMaskB[col] = mB;
            }

            // -----------------------------------------------------------------
            // Pass 2: bilateral edge-preserving horizontal smoothing
            //
            // Unlike a simple FIR which blurs everything equally, the bilateral
            // filter modulates each neighbor's weight by mask-value similarity.
            // Neighbors with similar coverage (stem interiors) get smoothed;
            // neighbors with very different coverage (glyph edges) are preserved.
            //
            // This achieves: smooth stems + sharp edges simultaneously.
            //
            // Base kernel: 0.08 / 0.84 / 0.08 — tuned for pixel resolution.
            // The 0.08 side weight is further scaled by exp(-diff²/2σ²) where
            // diff is the mask difference and σ = kBilateralSigma.
            // -----------------------------------------------------------------
            constexpr float kFirSide = 0.08f;
            constexpr float kBilateralSigma = 0.15f; // mask-space similarity threshold
            constexpr float kSigma2x2 = 2.0f * kBilateralSigma * kBilateralSigma;

            for (int col = 0; col < w; ++col)
            {
                const float mR = rowMaskR[col];
                const float mG = rowMaskG[col];
                const float mB = rowMaskB[col];

                // Left neighbor
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

                // Right neighbor
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

                // Normalize: center weight absorbs unused side weight
                rowSmR[col] = lR * lwR + mR * (1.0f - lwR - rwR) + rR * rwR;
                rowSmG[col] = lG * lwG + mG * (1.0f - lwG - rwG) + rG * rwG;
                rowSmB[col] = lB * lwB + mB * (1.0f - lwB - rwB) + rB * rwB;
            }

            // -----------------------------------------------------------------
            // Pass 2.25: Fractional subpixel positioning (#3)
            //
            // ClearType quantizes glyph positions to integer pixel boundaries.
            // The ideal position may lie at a 1/3 or 2/3 pixel subpixel offset.
            //
            // We estimate the subpixel phase error by comparing the R and B
            // mask centroids: if R and B coverage are perfectly aligned, the
            // glyph is centered. If they differ, we need a fractional shift.
            //
            // The shift is computed as: (centroidR - centroidB) deviation from
            // the expected RGB-stripe separation (0.667 pixel). A positive
            // deviation means the glyph should shift right; negative = left.
            //
            // Clamped to ±0.33 pixel (one subpixel).  Applied via linear
            // interpolation between adjacent smoothed mask values.
            // -----------------------------------------------------------------
            if (cfg.enableFractionalPositioning)
            {
                // Compute weighted centroids of R and B masks
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

                // Only apply when enough data for reliable estimate
                if (textCount >= 3 && sumR > 0.5f && sumB > 0.5f)
                {
                    centR /= sumR;
                    centB /= sumB;

                    // Expected R-B separation depends on physical panel topology.
                    // Accurate subpixel positioning requires centering the masks
                    // around the actual physical subpixel barycenters.
                    float expectedSep = -0.667f; // Default RGB stripe
                    if (qdGen3 || qdGen4) expectedSep = -0.500f; // R=0.25, B=0.75
                    else if (qdGen1) expectedSep = -0.440f; // R=0.28, B=0.72

                    // Actual separation tells us the phase error.
                    const float actualSep = centR - centB;
                    const float phaseError = (actualSep - expectedSep) * 0.5f;
                    const float shift = std::clamp(phaseError, -0.33f, 0.33f);

                    if (std::abs(shift) > 0.02f) // skip trivial shifts
                    {
                        // Apply fractional shift via linear interpolation.
                        // Positive shift = move right = sample from left neighbor.
                        const float t = std::abs(shift);
                        const int dir = (shift > 0.0f) ? -1 : 1; // sample direction

                        // Work on a copy to avoid read-after-write issues
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

            // -----------------------------------------------------------------
            // Pass 2.5: Vertical QD-OLED blending (#6)
            //
            // QD-OLED triangular layout: even/odd rows offset by ~1.5 subpixels.
            // The FreeType path uses SampleContinuousX with ±1.5 subpxl offset
            // from the adjacent row. In the GDI path we approximate this with
            // 25% blending from the PREVIOUS row's smoothed masks, shifted ±1
            // pixel (the closest integer approximation of 1.5 subpixels at the
            // pixel level of the composited ClearType output).
            //
            // Even rows: adjacent row is shifted right → blend from col+1.
            // Odd  rows: adjacent row is shifted left  → blend from col-1.
            // -----------------------------------------------------------------
            if (qdPanel && hasPrevRow)
            {
                const int shift = (row % 2 == 0) ? 1 : -1;
                // Vertical blend modulated by filterStrength — matches DWrite path.
                const float kVertBlend = 0.10f * std::min(toneStrength, 1.0f);

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

            // -----------------------------------------------------------------
            // Pass 3: OLED remap + chromaKeep + S-curve + blend
            // Uses smoothed masks from Pass 2 (+ vertical blend) for Category C pixels.
            // -----------------------------------------------------------------
            for (int col = 0; col < w; ++col)
            {
                if (rowFlags[col] != 2) continue;

                // Apply stem darkening before OLED remap — darkens thin stems.
                float maskR = applyStemDarkening(rowSmR[col], darkenAmount);
                float maskG = applyStemDarkening(rowSmG[col], darkenAmount);
                float maskB = applyStemDarkening(rowSmB[col], darkenAmount);

                // --- WOLED Cross-Talk Reduction (RWBG / RGWB) ----------
                // The physical white subpixel steals energy from RGB. This simulates
                // the hardware energy redirection to suppress color fringing.
                if (!qdPanel && cfg.woledCrossTalkReduction > 0.0f)
                {
                    const float wSignal = std::min({maskR, maskG, maskB}) * cfg.woledCrossTalkReduction;
                    maskR = std::max(0.0f, maskR - wSignal);
                    maskG = std::max(0.0f, maskG - wSignal);
                    maskB = std::max(0.0f, maskB - wSignal);
                }

                // --- OLED channel reconstruction -----------------------
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

                // filterStrength modulates how much OLED correction is applied.
                // At 1.0: full physical-subpixel remapping.
                // At 0.75 (default): 75% OLED correction + 25% stock ClearType.
                // This preserves more of the original sharpness/weight at
                // the cost of slightly more color fringing.
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

                // --- edge/fringing risk model -------------------------
                // Estimate risk from local luminance gradients (vertical edges are
                // more sensitive for subpixel fringing) and channel spread.
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

                // --- chromaKeep (BT.709 luma anchor) ------------------
                // Contrast/size-aware base + edge-adaptive limiter.
                const float bgLuma = 0.2126f * rowBgR[col]
                    + 0.7152f * rowBgG[col]
                    + 0.0722f * rowBgB[col];
                const float localContrast = std::abs(linTextLuma - bgLuma);
                const float chromaKeep =
                    ComputeAdaptiveChromaKeep(chromaKeepBase, localContrast, edgeRisk, edgeParams);
                finalCovR = yCov + (finalCovR - yCov) * chromaKeep;
                finalCovG = yCov + (finalCovG - yCov) * chromaKeep;
                finalCovB = yCov + (finalCovB - yCov) * chromaKeep;

                // --- S-curve readability boost ------------------------
                finalCovR = scurve(finalCovR);
                finalCovG = scurve(finalCovG);
                finalCovB = scurve(finalCovB);
                float targetY = scurve(yCov);

                // NOTE: oledGammaOutput is applied POST-COMPOSITING (below),
                // not here on coverage masks. Coverage masks are geometric
                // coefficients (α), not luminance values. Applying pow(α, γ)
                // with γ>1 crushes edge coverage and destroys sharpness.
                // See: Microsoft Research "Gamma-Correct Rendering" (2003).

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

                // --- final per-channel blend  bg → textColor ----------
                const float bgR_lin = rowBgR[col];
                const float bgG_lin = rowBgG[col];
                const float bgB_lin = rowBgB[col];

                float outR = bgR_lin * (1.0f - finalCovR) + linTextR * finalCovR;
                float outG = bgG_lin * (1.0f - finalCovG) + linTextG * finalCovG;
                float outB = bgB_lin * (1.0f - finalCovB) + linTextB * finalCovB;

                // --- OLED display gamma compensation (post-compositing) ---
                // Applied to the composited pixel, not the coverage mask.
                // Compensates for OLED's electroluminescent response: on
                // self-emissive displays, mid-tone text can appear lighter
                // than on LCDs (no backlight bleed to anchor black). This
                // correction operates on the actual pixel intensity that
                // the display will emit, which is the physically correct
                // domain for gamma compensation.
                //
                // cfg.gamma > 1.0 darkens the composited text pixel to
                // compensate for OLED's brighter mid-tone perception.
                // Formula: apply pow(pixel, gamma/sRGB_gamma) to the text
                // contribution only, preserving the background.
                if (cfg.gamma > 1.001f)
                {
                    const float gammaCorr = cfg.gamma;
                    // Isolate text contribution and apply gamma.
                    // Guard: only modify pixels where coverage is non-trivial.
                    const float covMax = std::max({finalCovR, finalCovG, finalCovB});
                    if (covMax > 0.01f)
                    {
                        outR = bgR_lin + (outR - bgR_lin) * std::pow(covMax, gammaCorr - 1.0f);
                        outG = bgG_lin + (outG - bgG_lin) * std::pow(covMax, gammaCorr - 1.0f);
                        outB = bgB_lin + (outB - bgB_lin) * std::pow(covMax, gammaCorr - 1.0f);
                        outR = std::clamp(outR, 0.0f, 1.0f);
                        outG = std::clamp(outG, 0.0f, 1.0f);
                        outB = std::clamp(outB, 0.0f, 1.0f);
                    }
                }

                oRow[col * 4 + 0] = linearToSRGB(outB);
                oRow[col * 4 + 1] = linearToSRGB(outG);
                oRow[col * 4 + 2] = linearToSRGB(outR);
                oRow[col * 4 + 3] = 0xFF;
            }

            // Save smoothed masks for vertical QD-OLED blending (#6) in next row.
            if (qdPanel)
            {
                std::copy(rowSmR.begin(), rowSmR.end(), prevSmR.begin());
                std::copy(rowSmG.begin(), rowSmG.end(), prevSmG.begin());
                std::copy(rowSmB.begin(), rowSmB.end(), prevSmB.begin());
                hasPrevRow = true;
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

        // Resolve target Monitor for OLED profile overrides
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

        const auto& cfg = Config::Instance().GetData(monitorName);
        if (cfg.filterStrength <= 0.0f)
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);
        initColorMathLUTs(cfg.gammaMode == GammaMode::OLED);

        // DPI-aware graduated fade-out (improvement #7).
        // Cached per-thread to avoid calling GetDeviceCaps on every ExtTextOutW.
        static thread_local float s_cachedDpi = 0.0f;
        static thread_local int s_dpiCallCount = 0;
        if (s_cachedDpi == 0.0f || (++s_dpiCallCount & 0xFF) == 0) // re-query every 256 calls
            s_cachedDpi = static_cast<float>(GetDeviceCaps(hdc, LOGPIXELSX));

        const float dpi = s_cachedDpi;

        // Above high threshold: skip OLED processing entirely (passthrough).
        if (dpi >= cfg.highDpiThresholdHigh)
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);

        // Compute DPI scale factor: 1.0 at/below dpiLow, ramps to 0.0 at dpiHigh.
        float dpiScale = 1.0f;
        if (dpi > cfg.highDpiThresholdLow)
        {
            dpiScale = 1.0f - std::clamp(
                (dpi - cfg.highDpiThresholdLow) / (cfg.highDpiThresholdHigh - cfg.highDpiThresholdLow),
                0.0f, 1.0f);
        }

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
            ForceSubpixelRender ctGuard(hdc, &cfg);

            BOOL result = g_OrigExtTextOutW(hdc, x, y, options,
                                            lprc, lpString, cbCount, lpDx);

            // Step 4 — capture AFTER (ClearType subpixel data now present).
            PixelCapture after = CaptureRegion(hdc, bounds.left, bounds.top,
                                               captureW, captureH);

            if (after.IsValid())
            {
                const COLORREF textColor = GetTextColor(hdc);

                // Detect composited/layered window — determines how background pixels are handled.
                //
                // Composited DCs (desktop icon labels, WS_EX_COMPOSITED/LAYERED windows):
                //   BitBlt reads zeros for transparent compositor areas → "before" is artificially
                //   black. We must NOT write unchanged pixels or mask-zero pixels back, otherwise
                //   we overwrite the compositor content with opaque fills (black box bug).
                //
                // Non-composited DCs (Notepad, dialogs, normal Win32 windows):
                //   "before" is the real previous content. Writing back GDI output for fill
                //   pixels (ETO_OPAQUE) and mask-zero pixels is correct and necessary.
                bool isComposited = false;
                if (const HWND hwnd = WindowFromDC(hdc))
                {
                    const LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
                    isComposited = ((exStyle & WS_EX_COMPOSITED) || (exStyle & WS_EX_LAYERED));
                }

                const bool opaqueBackground = (options & ETO_OPAQUE) && (lprc != nullptr);

                // Extract font weight for stem darkening adaptation.
                uint16_t fontWeight = 400;
                {
                    HFONT hFont = static_cast<HFONT>(GetCurrentObject(hdc, OBJ_FONT));
                    if (hFont)
                    {
                        LOGFONTW lf = {};
                        if (GetObjectW(hFont, sizeof(lf), &lf))
                            fontWeight = static_cast<uint16_t>(
                                std::clamp(static_cast<int>(lf.lfWeight), 100, 900));
                    }
                }

                RemapToOLED(hdc, before, after, textColor, cfg,
                            opaqueBackground, isComposited, dpiScale, fontWeight);
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
