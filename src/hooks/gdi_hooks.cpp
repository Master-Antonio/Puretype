#include "hooks/gdi_hooks.h"
#include "config.h"
#include "puretype.h"
#include "rasterizer/ft_rasterizer.h"
#include "filters/subpixel_filter.h"
#include "output/blender.h"

#include <MinHook.h>
#include <Windows.h>
#include <dwrite.h>
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
    static ExtTextOutW_t g_OrigExtTextOutW = nullptr;

    using DrawTextW_t = int(WINAPI*)(HDC, LPCWSTR, int, LPRECT, UINT);
    static DrawTextW_t g_OrigDrawTextW = nullptr;

    using DrawTextExW_t = int(WINAPI*)(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
    static DrawTextExW_t g_OrigDrawTextExW = nullptr;

    using PolyTextOutW_t = BOOL(WINAPI*)(HDC, const POLYTEXTW*, int);
    static PolyTextOutW_t g_OrigPolyTextOutW = nullptr;

    static std::string WideToUtf8(const std::wstring& ws)
    {
        if (ws.empty()) return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1) return {};
        std::string out(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), len, nullptr, nullptr);
        if (!out.empty() && out.back() == '\0') out.pop_back();
        return out;
    }

    // -----------------------------------------------------------------------
    // Qt window class detector.
    //
    // Qt computes glyph metrics (advance widths, bounding boxes) using its
    // own internal FreeType build before calling ExtTextOutW. By the time our
    // hook fires, Qt has already reserved exactly (original_width) pixels of
    // horizontal space for the glyph run. Our filter returns a bitmap that is
    // (original_width + 2) / 3 pixels wide — one third of what Qt expects.
    // Qt clips the blit to the bounding box it owns, cutting off roughly two
    // thirds of the glyph and making the text illegible.
    //
    // Detection strategy: Qt top-level windows and their children register
    // with class names that begin with "Qt5" or "Qt6". If the DC is associated
    // with a Qt window, we must pass through to the original GDI call
    // unchanged so Qt's own renderer handles everything.
    //
    // Edge cases handled:
    //   - WindowFromDC() can return NULL for memory DCs, printer DCs, and
    //     off-screen surfaces. In these cases we return false (allow hook)
    //     because there is no window layout engine to conflict with.
    //   - Qt windows occasionally render into child DCs whose HWND has a
    //     non-Qt class but whose root window is Qt. We walk up to the root
    //     with GetAncestor() and check there too.
    // -----------------------------------------------------------------------
    static bool IsQtWindow(HDC hdc)
    {
        if (!hdc) return false;

        HWND hwnd = WindowFromDC(hdc);
        if (!hwnd) return false; // memory DC or off-screen surface — allow hook

        // Check immediate window class.
        char className[128] = {};
        if (GetClassNameA(hwnd, className, sizeof(className)) > 0)
        {
            if (strncmp(className, "Qt5", 3) == 0 || strncmp(className, "Qt6", 3) == 0)
                return true;
        }

        // Check root ancestor class (handles child widgets rendered via parent DC).
        HWND root = GetAncestor(hwnd, GA_ROOT);
        if (root && root != hwnd)
        {
            char rootClass[128] = {};
            if (GetClassNameA(root, rootClass, sizeof(rootClass)) > 0)
            {
                if (strncmp(rootClass, "Qt5", 3) == 0 || strncmp(rootClass, "Qt6", 3) == 0)
                    return true;
            }
        }

        return false;
    }

    static std::string GetFontPathFromDWriteFace(IDWriteFontFace* fontFace)
    {
        if (!fontFace) return {};

        UINT32 fileCount = 0;
        HRESULT hr = fontFace->GetFiles(&fileCount, nullptr);
        if (FAILED(hr) || fileCount == 0) return {};

        std::vector<IDWriteFontFile*> files(fileCount, nullptr);
        hr = fontFace->GetFiles(&fileCount, files.data());
        if (FAILED(hr) || files.empty()) return {};

        auto releaseFiles = [&]()
        {
            for (auto* file : files)
                if (file) file->Release();
        };

        IDWriteFontFileLoader* loader = nullptr;
        hr = files[0]->GetLoader(&loader);
        if (FAILED(hr) || !loader)
        {
            releaseFiles();
            return {};
        }

        const void* key = nullptr;
        UINT32 keySize = 0;
        hr = files[0]->GetReferenceKey(&key, &keySize);
        if (FAILED(hr) || !key || keySize == 0)
        {
            loader->Release();
            releaseFiles();
            return {};
        }

        std::string pathUtf8;
        IDWriteLocalFontFileLoader* localLoader = nullptr;
        hr = loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),
                                    reinterpret_cast<void**>(&localLoader));
        if (SUCCEEDED(hr) && localLoader)
        {
            UINT32 pathLen = 0;
            hr = localLoader->GetFilePathLengthFromKey(key, keySize, &pathLen);
            if (SUCCEEDED(hr) && pathLen > 0)
            {
                std::wstring path(pathLen + 1, L'\0');
                hr = localLoader->GetFilePathFromKey(key, keySize, path.data(),
                                                     static_cast<UINT32>(path.size()));
                if (SUCCEEDED(hr))
                {
                    if (!path.empty() && path.back() == L'\0') path.pop_back();
                    pathUtf8 = WideToUtf8(path);
                }
            }
            localLoader->Release();
        }

        loader->Release();
        releaseFiles();
        return pathUtf8;
    }

    static std::string GetFontPathFromHDC(HDC hdc)
    {
        auto hFont = static_cast<HFONT>(GetCurrentObject(hdc, OBJ_FONT));
        if (!hFont) return "";

        LOGFONTW lf = {};
        if (GetObjectW(hFont, sizeof(lf), &lf) == 0) return "";

        wchar_t fontDir[MAX_PATH] = {};
        GetWindowsDirectoryW(fontDir, MAX_PATH);
        std::wstring fontDirStr(fontDir);
        fontDirStr += L"\\Fonts\\";

        std::wstring faceName(lf.lfFaceName);

        HKEY hKey = nullptr;
        std::wstring fontPath;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
                          0, KEY_READ, &hKey) == ERROR_SUCCESS)
        {
            DWORD index = 0;
            wchar_t valueName[256];
            DWORD valueNameSize;
            BYTE valueData[MAX_PATH * 2];
            DWORD valueDataSize;
            DWORD valueType;

            while (true)
            {
                valueNameSize = 256;
                valueDataSize = sizeof(valueData);
                LONG result = RegEnumValueW(hKey, index++, valueName, &valueNameSize,
                                            nullptr, &valueType, valueData, &valueDataSize);
                if (result != ERROR_SUCCESS) break;

                std::wstring entryName(valueName);
                if (entryName.find(faceName) != std::wstring::npos)
                {
                    std::wstring fileName(reinterpret_cast<wchar_t*>(valueData));
                    if (fileName.find(L'\\') == std::wstring::npos &&
                        fileName.find(L'/') == std::wstring::npos)
                        fontPath = fontDirStr + fileName;
                    else
                        fontPath = fileName;
                    break;
                }
            }
            RegCloseKey(hKey);
        }

        if (!fontPath.empty()) return WideToUtf8(fontPath);

        IDWriteFactory* factory = nullptr;
        HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(&factory));
        if (SUCCEEDED(hr) && factory)
        {
            IDWriteGdiInterop* interop = nullptr;
            hr = factory->GetGdiInterop(&interop);
            if (SUCCEEDED(hr) && interop)
            {
                IDWriteFontFace* face = nullptr;
                hr = interop->CreateFontFaceFromHdc(hdc, &face);
                if (SUCCEEDED(hr) && face)
                {
                    std::string exactPath = GetFontPathFromDWriteFace(face);
                    face->Release();
                    interop->Release();
                    factory->Release();
                    if (!exactPath.empty()) return exactPath;
                }
                else
                {
                    if (face) face->Release();
                    interop->Release();
                    factory->Release();
                }
            }
            else
            {
                if (interop) interop->Release();
                factory->Release();
            }
        }

        std::wstring segoePath = fontDirStr + L"segoeui.ttf";
        if (GetFileAttributesW(segoePath.c_str()) != INVALID_FILE_ATTRIBUTES)
            return WideToUtf8(segoePath);

        return {};
    }

    static uint32_t GetFontPixelSize(HDC hdc)
    {
        TEXTMETRICW tm = {};
        if (GetTextMetricsW(hdc, &tm))
        {
            int emHeight = tm.tmHeight - tm.tmInternalLeading;
            return static_cast<uint32_t>(emHeight > 0 ? emHeight : 16);
        }
        return 16;
    }

    static std::vector<uint16_t> TextToGlyphIndices(HDC hdc, LPCWSTR text, UINT count, bool isGlyphIndex)
    {
        std::vector<uint16_t> indices(count);
        if (isGlyphIndex)
        {
            for (UINT i = 0; i < count; ++i)
            {
                indices[i] = static_cast<uint16_t>(text[i]);
                if (indices[i] == 0xFFFF) return {};
            }
        }
        else
        {
            DWORD result = GetGlyphIndicesW(hdc, text, static_cast<int>(count),
                                            indices.data(), GGI_MARK_NONEXISTING_GLYPHS);
            if (result == GDI_ERROR) return {};
            for (UINT i = 0; i < count; ++i)
                if (indices[i] == 0xFFFF) return {};
        }
        return indices;
    }

    // -----------------------------------------------------------------------
    // Composited background blit helper.
    //
    // Instead of each glyph capturing its own slice of the HDC background
    // (which causes double-blending when glyphs overlap or the HDC already
    // contains a previous render of the same text), we capture the ENTIRE
    // background band ONCE before any glyphs are rendered, then each glyph
    // reads from that pre-captured copy.
    //
    // This fixes:
    //   - Notepad "blurry while typing" (TRANSPARENT-mode re-render over
    //     previously-rendered text doubles the coverage → blurry)
    //   - Any app that re-renders text in-place without clearing first
    // -----------------------------------------------------------------------
    struct BackgroundCapture
    {
        std::vector<uint8_t> data; // BGRA, row-major
        int x = 0; // top-left of captured region in HDC coordinates
        int y = 0;
        int width = 0;
        int height = 0;
        int pitch = 0; // bytes per row (= width * 4)

        [[nodiscard]] bool IsValid() const { return !data.empty() && width > 0 && height > 0; }
    };

    // Capture a region of the HDC into a CPU-side BGRA buffer.
    static BackgroundCapture CaptureBackground(HDC hdc, int x, int y, int w, int h)
    {
        BackgroundCapture cap;
        if (w <= 0 || h <= 0) return cap;

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // top-down
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
        cap.data.resize(cap.pitch * h);
        std::memcpy(cap.data.data(), dibBits, cap.data.size());

        SelectObject(memDC, old);
        DeleteObject(hBmp);
        DeleteDC(memDC);
        return cap;
    }

    // Blend a single glyph bitmap onto the HDC using a pre-captured background.
    // The background pixels are read from `bg` rather than from hdc, so glyphs
    // rendered earlier in the same pass do not pollute the background sample.
    static bool BlitGlyphWithCapturedBg(
        HDC hdc, int glyphX, int glyphY,
        const RGBABitmap& bitmap,
        COLORREF textColor,
        const BackgroundCapture& bg)
    {
        if (bitmap.data.empty() || bitmap.width <= 0 || bitmap.height <= 0) return false;
        if (!bg.IsValid()) return false;

        const float linTextR = sRGBToLinear(GetRValue(textColor));
        const float linTextG = sRGBToLinear(GetGValue(textColor));
        const float linTextB = sRGBToLinear(GetBValue(textColor));

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

        // read background from the PRE-CAPTURED copy, not from the live hdc.
        // This prevents double-blending: glyphs already written to hdc in this pass
        // will NOT pollute the background for subsequent glyphs.
        BITMAP dibInfo = {};
        GetObject(hBitmap, sizeof(dibInfo), &dibInfo);
        const int dstPitch = dibInfo.bmWidthBytes;

        auto* dst = static_cast<uint8_t*>(dibBits);

        for (int row = 0; row < bitmap.height; ++row)
        {
            const uint8_t* srcRow = bitmap.data.data() + row * bitmap.pitch;
            uint8_t* dstRow = dst + row * dstPitch;

            for (int col = 0; col < bitmap.width; ++col)
            {
                const uint8_t* srcPx = srcRow + col * 4;
                uint8_t* dstPx = dstRow + col * 4;

                // Map this pixel back to the pre-captured background.
                const int bgPixX = (glyphX + col) - bg.x;
                const int bgPixY = (glyphY + row) - bg.y;

                if (bgPixX < 0 || bgPixX >= bg.width ||
                    bgPixY < 0 || bgPixY >= bg.height)
                {
                    // Outside captured region — fall back to black background.
                    dstPx[0] = dstPx[1] = dstPx[2] = 0;
                }
                else
                {
                    const uint8_t* bgPx = bg.data.data() + bgPixY * bg.pitch + bgPixX * 4;
                    dstPx[0] = bgPx[0];
                    dstPx[1] = bgPx[1];
                    dstPx[2] = bgPx[2];
                }
                dstPx[3] = 0xFF; // Preserve opacity for DWM-composited surfaces

                if (srcPx[3] == 0) continue; // transparent pixel — keep bg

                // Per-channel subpixel blend in linear light.
                const float maskB = sRGBToLinear(srcPx[0]);
                const float maskG = sRGBToLinear(srcPx[1]);
                const float maskR = sRGBToLinear(srcPx[2]);

                const float bgB = sRGBToLinear(dstPx[0]);
                const float bgG = sRGBToLinear(dstPx[1]);
                const float bgR = sRGBToLinear(dstPx[2]);

                dstPx[0] = linearToSRGB(bgB * (1.0f - maskB) + linTextB * maskB);
                dstPx[1] = linearToSRGB(bgG * (1.0f - maskG) + linTextG * maskG);
                dstPx[2] = linearToSRGB(bgR * (1.0f - maskR) + linTextR * maskR);
                dstPx[3] = 0xFF;
            }
        }

        const BOOL blitResult = BitBlt(hdc, glyphX, glyphY,
                                       bitmap.width, bitmap.height,
                                       memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(memDC);
        return blitResult != FALSE;
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
        HDC hdc,
        int x,
        int y,
        UINT options,
        const RECT* lprc,
        LPCWSTR lpString,
        UINT cbCount,
        const INT* lpDx)
    {
        if (g_insideHook)
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);
        if (!lpString || cbCount == 0)
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);

        // Dynamic Qt window detection.
        //
        // Qt pre-computes layout metrics using its own FreeType build before
        // calling ExtTextOutW. The DC is associated with a Qt window whose
        // layout engine owns the glyph bounding boxes. If we replace the bitmap
        // with one at 1/3 the expected width, Qt clips it to the reserved
        // bounding box and the text becomes illegible.
        //
        // IsQtWindow() detects both Qt5 and Qt6 window classes, including
        // root-ancestor promotion for child widget DCs.
        if (IsQtWindow(hdc))
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);

        HookRefGuard refGuard;

        const auto& cfg = Config::Instance().Data();
        if (cfg.filterStrength <= 0.0f)
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);

        std::string fontPath = GetFontPathFromHDC(hdc);
        if (fontPath.empty())
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);

        uint32_t pixelSize = GetFontPixelSize(hdc);
        if (pixelSize == 0 || pixelSize > 200)
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);

        const bool isGlyphIndex = (options & ETO_GLYPH_INDEX) != 0;
        auto glyphIndices = TextToGlyphIndices(hdc, lpString, cbCount, isGlyphIndex);
        if (glyphIndices.empty())
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);

        // ETO_PDY: interleaved [dx0,dy0, dx1,dy1, ...] — extract X only.
        const bool hasPDY = (options & ETO_PDY) != 0;
        std::vector<int> cleanDx;
        const int* effectiveDx = lpDx;
        if (lpDx && hasPDY)
        {
            cleanDx.resize(cbCount);
            for (UINT i = 0; i < cbCount; ++i) cleanDx[i] = lpDx[i * 2];
            effectiveDx = cleanDx.data();
        }

        auto positionedGlyphs = FTRasterizer::Instance().RasterizeGlyphRun(
            fontPath, glyphIndices.data(),
            static_cast<uint32_t>(glyphIndices.size()), pixelSize, effectiveDx);

        if (positionedGlyphs.size() != glyphIndices.size())
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);

        for (const auto& pg : positionedGlyphs)
        {
            if (!pg.bitmap || pg.bitmap->width < 0 || pg.bitmap->height < 0)
                return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);
        }

        g_insideHook = true;

        auto filter = SubpixelFilter::Create(static_cast<int>(cfg.panelType));
        const COLORREF textColor = GetTextColor(hdc);

        std::vector<RGBABitmap> filteredBitmaps;
        filteredBitmaps.reserve(positionedGlyphs.size());

        for (auto& pg : positionedGlyphs)
        {
            if (!pg.bitmap || pg.bitmap->width <= 0 || pg.bitmap->height <= 0 ||
                pg.bitmap->data.empty())
            {
                filteredBitmaps.push_back(RGBABitmap{});
                continue;
            }

            RGBABitmap filtered = filter->Apply(*pg.bitmap, cfg);
            if (filtered.data.empty() || filtered.width <= 0 || filtered.height <= 0)
            {
                g_insideHook = false;
                return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);
            }

            if (cfg.highlightRenderedGlyphs)
            {
                for (size_t i = 0; i < filtered.data.size(); i += 4)
                {
                    filtered.data[i + 0] = static_cast<uint8_t>(filtered.data[i + 0] * 0.7f + 200 * 0.3f);
                    filtered.data[i + 1] = static_cast<uint8_t>(filtered.data[i + 1] * 0.7f + 200 * 0.3f);
                }
            }

            filteredBitmaps.push_back(std::move(filtered));
        }

        // ---------------------------------------------------------------
        // Text alignment & start position
        // ---------------------------------------------------------------
        const UINT align = GetTextAlign(hdc);
        int startX = x;
        int startY = y;

        if (align & TA_UPDATECP)
        {
            POINT pt;
            GetCurrentPositionEx(hdc, &pt);
            startX = pt.x;
            startY = pt.y;
        }

        int totalWidth = 0;
        if (effectiveDx)
        {
            for (UINT i = 0; i < cbCount; ++i) totalWidth += effectiveDx[i];
        }
        else
        {
            for (const auto& pg : positionedGlyphs)
                if (pg.bitmap) totalWidth += pg.bitmap->advanceX / 3;
        }

        if (totalWidth <= 0 && !positionedGlyphs.empty())
        {
            const auto& last = positionedGlyphs.back();
            if (last.bitmap)
                totalWidth = (last.offsetX + last.bitmap->advanceX) / 3;
        }

        if ((align & TA_CENTER) == TA_CENTER) startX -= totalWidth / 2;
        else if ((align & TA_RIGHT) == TA_RIGHT) startX -= totalWidth;

        TEXTMETRICW tm;
        GetTextMetricsW(hdc, &tm);
        if ((align & TA_BOTTOM) == TA_BOTTOM) startY -= tm.tmDescent;
        else if (((align & TA_BASELINE) != TA_BASELINE) && ((align & TA_BOTTOM) != TA_BOTTOM))
            startY += tm.tmAscent;

        // ---------------------------------------------------------------
        // Background fill (ETO_OPAQUE or OPAQUE mode)
        // ---------------------------------------------------------------
        if (lprc && (options & ETO_OPAQUE))
        {
            HBRUSH hBrush = CreateSolidBrush(GetBkColor(hdc));
            FillRect(hdc, lprc, hBrush);
            DeleteObject(hBrush);
        }
        else if (GetBkMode(hdc) == OPAQUE)
        {
            RECT bgRect = {
                startX, startY - tm.tmAscent,
                startX + totalWidth, startY + tm.tmDescent
            };
            HBRUSH hBrush = CreateSolidBrush(GetBkColor(hdc));
            FillRect(hdc, &bgRect, hBrush);
            DeleteObject(hBrush);
        }

        // ---------------------------------------------------------------
        // CRITICAL FIX — Pre-capture the background ONCE for the entire run.
        // ---------------------------------------------------------------

        int runLeft = INT_MAX, runRight = INT_MIN;
        int runTop = INT_MAX, runBottom = INT_MIN;

        for (size_t i = 0; i < positionedGlyphs.size(); ++i)
        {
            const auto& pg = positionedGlyphs[i];
            const auto& fb = filteredBitmaps[i];
            if (!pg.bitmap || fb.data.empty()) continue;

            const int padPx = pg.bitmap->padLeft / 3;
            const int gx = startX
                + static_cast<int>(std::floor(static_cast<float>(pg.offsetX) / 3.0f))
                - padPx;
            const int gy = startY - pg.bitmap->bearingY - pg.bitmap->padTop;

            runLeft = std::min(runLeft, gx);
            runRight = std::max(runRight, gx + fb.width);
            runTop = std::min(runTop, gy);
            runBottom = std::max(runBottom, gy + fb.height);
        }

        BackgroundCapture bgCapture;
        if (runLeft < runRight && runTop < runBottom)
        {
            bgCapture = CaptureBackground(hdc, runLeft, runTop,
                                          runRight - runLeft, runBottom - runTop);
        }

        // ---------------------------------------------------------------
        // Clip region for ETO_CLIPPED
        // ---------------------------------------------------------------
        HRGN hOldRgn = nullptr;
        HRGN hClipRgn = nullptr;
        if (lprc && (options & ETO_CLIPPED))
        {
            hClipRgn = CreateRectRgnIndirect(lprc);
            if (hClipRgn)
            {
                hOldRgn = CreateRectRgn(0, 0, 0, 0);
                if (GetClipRgn(hdc, hOldRgn) <= 0)
                {
                    DeleteObject(hOldRgn);
                    hOldRgn = nullptr;
                }
                ExtSelectClipRgn(hdc, hClipRgn, RGN_AND);
            }
        }

        // ---------------------------------------------------------------
        // Render glyphs using the frozen background capture
        // ---------------------------------------------------------------
        bool fallbackNeeded = false;
        for (size_t i = 0; i < positionedGlyphs.size(); ++i)
        {
            const auto& pg = positionedGlyphs[i];
            const auto& filtered = filteredBitmaps[i];

            if (!pg.bitmap || filtered.data.empty() ||
                filtered.width <= 0 || filtered.height <= 0)
                continue;

            const int padPx = pg.bitmap->padLeft / 3;
            const int glyphX = startX
                + static_cast<int>(std::floor(static_cast<float>(pg.offsetX) / 3.0f))
                - padPx;
            const int glyphY = startY - pg.bitmap->bearingY - pg.bitmap->padTop;

            if (lprc && (options & ETO_CLIPPED))
            {
                if (glyphX >= lprc->right || glyphX + filtered.width <= lprc->left ||
                    glyphY >= lprc->bottom || glyphY + filtered.height <= lprc->top)
                    continue;
            }

            bool blitOk;
            if (bgCapture.IsValid())
            {
                blitOk = BlitGlyphWithCapturedBg(hdc, glyphX, glyphY,
                                                 filtered, textColor, bgCapture);
            }
            else
            {
                blitOk = Blender::Instance().BlitToHDC(
                    hdc, glyphX, glyphY, filtered, textColor, cfg.gamma);
            }

            if (!blitOk)
            {
                fallbackNeeded = true;
                break;
            }
        }

        // Restore clip region (always).
        if (hClipRgn)
        {
            SelectClipRgn(hdc, hOldRgn);
            if (hOldRgn) DeleteObject(hOldRgn);
            DeleteObject(hClipRgn);
        }

        if (!fallbackNeeded && (align & TA_UPDATECP))
            MoveToEx(hdc, startX + totalWidth, startY, nullptr);

        g_insideHook = false;

        if (fallbackNeeded)
            return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);

        // GDI dirty-rect / DWM shadow buffer: run with empty clip so DWM
        // registers the bounding box without drawing stock ClearType.
        {
            const int savedDC = SaveDC(hdc);
            HRGN emptyRgn = CreateRectRgn(0, 0, 0, 0);
            ExtSelectClipRgn(hdc, emptyRgn, RGN_COPY);
            g_OrigExtTextOutW(hdc, x, y, options & ~ETO_OPAQUE, lprc, lpString, cbCount, lpDx);
            RestoreDC(hdc, savedDC);
            DeleteObject(emptyRgn);
        }

        return TRUE;
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

    static int WINAPI Hooked_DrawTextW(HDC hdc, LPCWSTR lpchText, int cchText,
                                       LPRECT lprc, UINT format)
    {
        return g_OrigDrawTextW(hdc, lpchText, cchText, lprc, format);
    }

    static int WINAPI Hooked_DrawTextExW(HDC hdc, LPWSTR lpchText, int cchText,
                                         LPRECT lprc, UINT format, LPDRAWTEXTPARAMS lpdtp)
    {
        return g_OrigDrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);
    }

    bool InstallGDIHooks()
    {
        bool success = true;

        HMODULE hGdi32 = GetModuleHandleW(L"gdi32.dll");
        if (!hGdi32) hGdi32 = LoadLibraryW(L"gdi32.dll");

        if (hGdi32)
        {
            auto pExtTextOutW = reinterpret_cast<ExtTextOutW_t>(
                GetProcAddress(hGdi32, "ExtTextOutW"));
            if (pExtTextOutW)
            {
                MH_STATUS status = MH_CreateHook(
                    reinterpret_cast<LPVOID>(pExtTextOutW),
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
                PureTypeLog("ExtTextOutW not found in gdi32.dll");
                success = false;
            }

            auto pPolyTextOutW = reinterpret_cast<PolyTextOutW_t>(
                GetProcAddress(hGdi32, "PolyTextOutW"));
            if (pPolyTextOutW)
            {
                MH_STATUS status = MH_CreateHook(
                    reinterpret_cast<LPVOID>(pPolyTextOutW),
                    reinterpret_cast<LPVOID>(&Hooked_PolyTextOutW),
                    reinterpret_cast<LPVOID*>(&g_OrigPolyTextOutW));
                if (status != MH_OK) success = false;
            }
        }

        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (!hUser32) hUser32 = LoadLibraryW(L"user32.dll");

        if (hUser32)
        {
            auto pDrawTextW = reinterpret_cast<DrawTextW_t>(
                GetProcAddress(hUser32, "DrawTextW"));
            if (pDrawTextW)
            {
                MH_STATUS status = MH_CreateHook(
                    reinterpret_cast<LPVOID>(pDrawTextW),
                    reinterpret_cast<LPVOID>(&Hooked_DrawTextW),
                    reinterpret_cast<LPVOID*>(&g_OrigDrawTextW));
                if (status != MH_OK) success = false;
            }

            auto pDrawTextExW = reinterpret_cast<DrawTextExW_t>(
                GetProcAddress(hUser32, "DrawTextExW"));
            if (pDrawTextExW)
            {
                MH_STATUS status = MH_CreateHook(
                    reinterpret_cast<LPVOID>(pDrawTextExW),
                    reinterpret_cast<LPVOID>(&Hooked_DrawTextExW),
                    reinterpret_cast<LPVOID*>(&g_OrigDrawTextExW));
                if (status != MH_OK) success = false;
            }
        }

        return success;
    }

    void RemoveGDIHooks()
    {
        if (const HMODULE hGdi32 = GetModuleHandleW(L"gdi32.dll"))
        {
            if (g_OrigExtTextOutW)
            {
                auto p = reinterpret_cast<LPVOID>(GetProcAddress(hGdi32, "ExtTextOutW"));
                if (p) MH_RemoveHook(p);
                g_OrigExtTextOutW = nullptr;
            }
            if (g_OrigPolyTextOutW)
            {
                auto p = reinterpret_cast<LPVOID>(GetProcAddress(hGdi32, "PolyTextOutW"));
                if (p) MH_RemoveHook(p);
                g_OrigPolyTextOutW = nullptr;
            }
        }

        if (const HMODULE hUser32 = GetModuleHandleW(L"user32.dll"))
        {
            if (g_OrigDrawTextW)
            {
                auto p = reinterpret_cast<LPVOID>(GetProcAddress(hUser32, "DrawTextW"));
                if (p) MH_RemoveHook(p);
                g_OrigDrawTextW = nullptr;
            }
            if (g_OrigDrawTextExW)
            {
                auto p = reinterpret_cast<LPVOID>(GetProcAddress(hUser32, "DrawTextExW"));
                if (p) MH_RemoveHook(p);
                g_OrigDrawTextExW = nullptr;
            }
        }
    }
}