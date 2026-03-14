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
    // RemapToOLED
    //
    // Core post-processing kernel. Extracts per-channel ClearType coverage and
    // re-blends using the target panel's subpixel layout.
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

        const bool qdPanel = (cfg.panelType == PanelType::QD_OLED_TRIANGLE);
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

        // Output buffer --------------------------------------------------------
        std::vector<uint8_t> output(static_cast<size_t>(before.pitch) * h);
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

                // Default: keep whatever GDI wrote (background or opaque fill).
                oRow[col * 4 + 0] = ap[0];
                oRow[col * 4 + 1] = ap[1];
                oRow[col * 4 + 2] = ap[2];
                oRow[col * 4 + 3] = 0xFF;

                // Only process pixels where ClearType blended text.
                if (ap[0] == bp[0] && ap[1] == bp[1] && ap[2] == bp[2]) continue;
                anyModified = true;

                // --- Step 1: background and ClearType output in linear light ---
                const float bgB = sRGBToLinear(bp[0]);
                const float bgG = sRGBToLinear(bp[1]);
                const float bgR = sRGBToLinear(bp[2]);

                const float ctB = sRGBToLinear(ap[0]);
                const float ctG = sRGBToLinear(ap[1]);
                const float ctR = sRGBToLinear(ap[2]);

                // --- Step 2: extract per-channel subpixel coverage masks -------
                // ClearType blends: ct_ch = bg*(1-m) + text*m  →  m = (ct-bg)/(text-bg)
                auto extractMask = [](float ct, float bg, float text) -> float
                {
                    const float d = text - bg;
                    if (std::abs(d) < 0.001f) return 0.0f;
                    return std::clamp((ct - bg) / d, 0.0f, 1.0f);
                };

                const float maskR = extractMask(ctR, bgR, linTextR);
                const float maskG = extractMask(ctG, bgG, linTextG);
                const float maskB = extractMask(ctB, bgB, linTextB);

                // --- Step 3: OLED channel reconstruction -----------------------
                //
                // ClearType subpixel centers (3 per pixel, RGB stripe):
                //   R → 1/6  (0.167)   G → 3/6  (0.500)   B → 5/6  (0.833)
                //
                // WOLED RWBG subpixel centers (4 per pixel):
                //   R → 1/8  (0.125)   W → 3/8  (0.375)   B → 5/8  (0.625)   G → 7/8 (0.875)
                //
                // WOLED RGWB subpixel centers (4 per pixel):
                //   R → 1/8            G → 3/8              W → 5/8             B → 7/8
                //
                // QD-OLED (triangular, 3 per pixel):
                //   Approximately same as RGB stripe horizontally; use directly.

                float finalCovR, finalCovG, finalCovB;

                if (qdPanel)
                {
                    // QD-OLED: reuse ClearType masks directly for horizontal coverage.
                    finalCovR = maskR;
                    finalCovG = maskG;
                    finalCovB = maskB;
                }
                else if (rgwbPanel)
                {
                    // RGWB: order R(1/8), G(3/8), W(5/8), B(7/8)
                    //   WOLED R ← CT_R   (1/8 ≈ 1/6)
                    //   WOLED G ← CT_G   (3/8 ≈ 3/6, closest)
                    //   WOLED W ← CT_B   (5/8 ≈ 5/6, closest) * (1-crossTalk)
                    //   WOLED B ← CT_B   (7/8 ≈ 5/6, also closest)
                    //   W drives all channels via TCON max().
                    const float alpha_W = maskB * (1.0f - cfg.woledCrossTalkReduction);
                    finalCovR = std::max(maskR, alpha_W);
                    finalCovG = std::max(maskG, alpha_W);
                    finalCovB = std::max(maskB, alpha_W);
                }
                else
                {
                    // RWBG (default): order R(1/8), W(3/8), B(5/8), G(7/8)
                    //   WOLED R ← CT_R              (1/6 ≈ 1/8)
                    //   WOLED W ← CT_G * (1-xTalk)  (3/6 ≈ 3/8)
                    //   WOLED B ← avg(CT_G, CT_B)   (5/8 lies between CT_G and CT_B centers)
                    //   WOLED G ← CT_B              (5/6 ≈ 7/8)
                    //   W drives all channels via TCON max().
                    const float alpha_R = maskR;
                    const float alpha_W = maskG * (1.0f - cfg.woledCrossTalkReduction);
                    const float alpha_B = (maskG + maskB) * 0.5f;
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
                const float outR = bgR * (1.0f - finalCovR) + linTextR * finalCovR;
                const float outG = bgG * (1.0f - finalCovG) + linTextG * finalCovG;
                const float outB = bgB * (1.0f - finalCovB) + linTextB * finalCovB;

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
        BitBlt(hdc, before.x, before.y, w, h, memDC, 0, 0, SRCCOPY);
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

        // Step 3 — GDI renders with ClearType (completely unmodified).
        BOOL result = g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);

        // Step 4 — capture AFTER.
        PixelCapture after = CaptureRegion(hdc, bounds.left, bounds.top,
                                           captureW, captureH);
        if (!after.IsValid())
        {
            g_insideHook = false;
            return result;
        }

        // Step 5 — remap to OLED and write back.
        const COLORREF textColor = GetTextColor(hdc);
        RemapToOLED(hdc, before, after, textColor, cfg);

        g_insideHook = false;
        return result;
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
