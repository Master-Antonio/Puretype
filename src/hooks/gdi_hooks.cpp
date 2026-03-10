#include "hooks/gdi_hooks.h"
#include "config.h"
#include "rasterizer/ft_rasterizer.h"
#include "filters/subpixel_filter.h"
#include "output/blender.h"

#include <MinHook.h>
#include <Windows.h>
#include <dwrite.h>
#include <algorithm>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>

extern void PureTypeLog(const char* fmt, ...);

namespace puretype
{
    namespace hooks
    {
        using ExtTextOutW_t = BOOL(WINAPI*)(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
        static ExtTextOutW_t g_OrigExtTextOutW = nullptr;

        using DrawTextW_t = int(WINAPI*)(HDC, LPCWSTR, int, LPRECT, UINT);
        static DrawTextW_t g_OrigDrawTextW = nullptr;

        static std::string WideToUtf8(const std::wstring& ws)
        {
            if (ws.empty()) return {};
            int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (len <= 1) return {};
            std::string out(static_cast<size_t>(len), '\0');
            WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), len, nullptr, nullptr);
            if (!out.empty() && out.back() == '\0') out.pop_back();
            return out;
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
                {
                    if (file) file->Release();
                }
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
            HFONT hFont = reinterpret_cast<HFONT>(GetCurrentObject(hdc, OBJ_FONT));
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
                        {
                            fontPath = fontDirStr + fileName;
                        }
                        else
                        {
                            fontPath = fileName;
                        }
                        break;
                    }
                }
                RegCloseKey(hKey);
            }

            if (!fontPath.empty())
            {
                return WideToUtf8(fontPath);
            }

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
            {
                return WideToUtf8(segoePath);
            }

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
                // If the caller provided glyph indices directly, skip character mapping.

                for (UINT i = 0; i < count; ++i)
                {
                    indices[i] = static_cast<uint16_t>(text[i]);
                    if (indices[i] == 0xFFFF)
                    {
                        return {};
                    }
                }
            }
            else
            {
                DWORD result = GetGlyphIndicesW(hdc, text, static_cast<int>(count),
                                                indices.data(), GGI_MARK_NONEXISTING_GLYPHS);
                if (result == GDI_ERROR)
                {
                    return {};
                }

                for (UINT i = 0; i < count; ++i)
                {
                    if (indices[i] == 0xFFFF)
                    {
                        return {};
                    }
                }
            }

            return indices;
        }

        static thread_local bool g_insideHook = false;

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
            {
                // Prevent recursion when fallback/original path calls back into GDI text APIs.
                return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);
            }

            if (!lpString || cbCount == 0)
            {
                return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);
            }

            const auto& cfg = Config::Instance().Data();

            if (cfg.filterStrength <= 0.0f)
            {
                return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);
            }

            std::string fontPath = GetFontPathFromHDC(hdc);
            if (fontPath.empty())
            {
                return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);
            }

            uint32_t pixelSize = GetFontPixelSize(hdc);
            if (pixelSize == 0 || pixelSize > 200)
            {
                return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);
            }

            bool isGlyphIndex = (options & ETO_GLYPH_INDEX) != 0;
            auto glyphIndices = TextToGlyphIndices(hdc, lpString, cbCount, isGlyphIndex);
            if (glyphIndices.empty())
            {
                return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);
            }

            auto positionedGlyphs = FTRasterizer::Instance().RasterizeGlyphRun(
                fontPath, glyphIndices.data(),
                static_cast<uint32_t>(glyphIndices.size()), pixelSize, lpDx);

            if (positionedGlyphs.size() != glyphIndices.size())
            {
                return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);
            }

            for (const auto& pg : positionedGlyphs)
            {
                if (!pg.bitmap)
                {
                    return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);
                }
                if (pg.bitmap->width < 0 || pg.bitmap->height < 0)
                {
                    return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);
                }
            }

            g_insideHook = true;

            auto filter = SubpixelFilter::Create(static_cast<int>(cfg.panelType));
            COLORREF textColor = GetTextColor(hdc);

            std::vector<RGBABitmap> filteredBitmaps;
            filteredBitmaps.reserve(positionedGlyphs.size());

            for (auto& pg : positionedGlyphs)
            {
                if (!pg.bitmap)
                {
                    filteredBitmaps.push_back(RGBABitmap{});
                    continue;
                }

                if (pg.bitmap->width <= 0 || pg.bitmap->height <= 0 || pg.bitmap->data.empty())
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
                        filtered.data[i + 0] = static_cast<uint8_t>(
                            filtered.data[i + 0] * 0.7f + 200 * 0.3f);
                        filtered.data[i + 1] = static_cast<uint8_t>(
                            filtered.data[i + 1] * 0.7f + 200 * 0.3f);
                    }
                }

                filteredBitmaps.push_back(std::move(filtered));
            }

            UINT align = GetTextAlign(hdc);
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
            if (lpDx)
            {
                for (UINT i = 0; i < cbCount; ++i)
                {
                    totalWidth += lpDx[i];
                }
            }
            else
            {
                for (const auto& pg : positionedGlyphs)
                {
                    if (pg.bitmap)
                    {
                        totalWidth += pg.bitmap->advanceX / 3;
                    }
                }
            }
            if (totalWidth <= 0 && !positionedGlyphs.empty())
            {
                auto& last = positionedGlyphs.back();
                totalWidth = (last.offsetX / 3) + (last.bitmap ? last.bitmap->width : 0);
            }

            if ((align & TA_CENTER) == TA_CENTER)
            {
                startX -= totalWidth / 2;
            }
            else if ((align & TA_RIGHT) == TA_RIGHT)
            {
                startX -= totalWidth;
            }

            TEXTMETRICW tm;
            GetTextMetricsW(hdc, &tm);
            if ((align & TA_BOTTOM) == TA_BOTTOM)
            {
                startY -= tm.tmDescent;
            }
            else if (((align & TA_BASELINE) != TA_BASELINE) && ((align & TA_BOTTOM) != TA_BOTTOM))
            {
                // Default top-alignment adjustment.
                startY += tm.tmAscent;
            }

            if (lprc && (options & ETO_OPAQUE))
            {
                HBRUSH hBrush = CreateSolidBrush(GetBkColor(hdc));
                FillRect(hdc, lprc, hBrush);
                DeleteObject(hBrush);
            }
            else if (GetBkMode(hdc) == OPAQUE)
            {
                // Handle opaque background for non-clipped regions.
                RECT bgRect;
                bgRect.left = startX;
                bgRect.top = startY - tm.tmAscent;
                bgRect.right = startX + totalWidth;
                bgRect.bottom = startY + tm.tmDescent;

                HBRUSH hBrush = CreateSolidBrush(GetBkColor(hdc));
                FillRect(hdc, &bgRect, hBrush);
                DeleteObject(hBrush);
            }

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

            bool fallbackNeeded = false;
            for (size_t i = 0; i < positionedGlyphs.size(); ++i)
            {
                auto& pg = positionedGlyphs[i];
                auto& filtered = filteredBitmaps[i];

                if (!pg.bitmap)
                {
                    continue;
                }

                if (filtered.data.empty() || filtered.width <= 0 || filtered.height <= 0)
                {
                    continue;
                }

                int glyphX = startX + pg.offsetX / 3;
                int glyphY = startY - pg.bitmap->bearingY;

                if (lprc && (options & ETO_CLIPPED))
                {
                    if (glyphX >= lprc->right || glyphX + filtered.width <= lprc->left ||
                        glyphY >= lprc->bottom || glyphY + filtered.height <= lprc->top)
                    {
                        continue;
                    }
                }

                bool blitOk = Blender::Instance().BlitToHDC(
                    hdc, glyphX, glyphY, filtered, textColor, cfg.gamma);

                if (!blitOk)
                {
                    fallbackNeeded = true;
                    break;
                }
            }

            if (hClipRgn)
            {
                SelectClipRgn(hdc, hOldRgn);
                if (hOldRgn) DeleteObject(hOldRgn);
                DeleteObject(hClipRgn);
            }

            if (!fallbackNeeded && (align & TA_UPDATECP))
            {
                MoveToEx(hdc, startX + totalWidth, startY, nullptr);
            }

            g_insideHook = false;

            if (fallbackNeeded)
            {
                // Any partial failure falls back to stock GDI to avoid missing glyphs.
                return g_OrigExtTextOutW(hdc, x, y, options, lprc, lpString, cbCount, lpDx);
            }

            return TRUE;
        }

        static int WINAPI Hooked_DrawTextW(
            HDC hdc,
            LPCWSTR lpchText,
            int cchText,
            LPRECT lprc,
            UINT format)
        {
            if (format & DT_CALCRECT)
            {
                return g_OrigDrawTextW(hdc, lpchText, cchText, lprc, format);
            }

            return g_OrigDrawTextW(hdc, lpchText, cchText, lprc, format);
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
                        PureTypeLog("MH_CreateHook(ExtTextOutW) failed: %s",
                                    MH_StatusToString(status));
                        success = false;
                    }
                }
                else
                {
                    PureTypeLog("ExtTextOutW not found in gdi32.dll");
                    success = false;
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
                    if (status != MH_OK)
                    {
                        PureTypeLog("MH_CreateHook(DrawTextW) failed: %s",
                                    MH_StatusToString(status));
                        success = false;
                    }
                }
                else
                {
                    PureTypeLog("DrawTextW not found in user32.dll");
                    success = false;
                }
            }

            return success;
        }

        void RemoveGDIHooks()
        {
            HMODULE hGdi32 = GetModuleHandleW(L"gdi32.dll");
            if (hGdi32 && g_OrigExtTextOutW)
            {
                auto pOrig = GetProcAddress(hGdi32, "ExtTextOutW");
                if (pOrig) MH_RemoveHook(pOrig);
                g_OrigExtTextOutW = nullptr;
            }

            HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
            if (hUser32 && g_OrigDrawTextW)
            {
                auto pOrig = GetProcAddress(hUser32, "DrawTextW");
                if (pOrig) MH_RemoveHook(pOrig);
                g_OrigDrawTextW = nullptr;
            }
        }
    }
}