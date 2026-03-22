#include "hooks/dwrite_hooks.h"

#include "config.h"
#include "filters/subpixel_filter.h"
#include "output/blender.h"
#include "puretype.h"
#include "rasterizer/ft_rasterizer.h"

#include <MinHook.h>
#include <Windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <dwrite_2.h>
#include <dwrite_3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <mutex>
#include <new>
#include <string>
#include <vector>

extern void PureTypeLog(const char* fmt, ...);


namespace puretype::hooks
{
    namespace
    {
        // IDWrite* vtable slots are ABI-sensitive; keep these aligned with the SDK interface layout.
        constexpr size_t kFactoryCreateTextLayoutVtableIndex = 18;
        constexpr size_t kFactoryCreateGdiCompatibleTextLayoutVtableIndex = 19;
        constexpr size_t kTextLayoutDrawVtableIndex = 58;

        using DWriteCreateFactory_t = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
        using CreateTextLayout_t = HRESULT(STDMETHODCALLTYPE*)(IDWriteFactory*,
                                                               WCHAR const*,
                                                               UINT32,
                                                               IDWriteTextFormat*,
                                                               FLOAT,
                                                               FLOAT,
                                                               IDWriteTextLayout**);
        using CreateGdiCompatibleTextLayout_t = HRESULT(STDMETHODCALLTYPE*)(IDWriteFactory*,
                                                                            WCHAR const*,
                                                                            UINT32,
                                                                            IDWriteTextFormat*,
                                                                            FLOAT,
                                                                            FLOAT,
                                                                            FLOAT,
                                                                            DWRITE_MATRIX const*,
                                                                            BOOL,
                                                                            IDWriteTextLayout**);
        using TextLayoutDraw_t = HRESULT(STDMETHODCALLTYPE*)(
            IDWriteTextLayout*, void*, IDWriteTextRenderer*, FLOAT, FLOAT);

        DWriteCreateFactory_t g_origDWriteCreateFactory = nullptr;
        CreateTextLayout_t g_origCreateTextLayout = nullptr;
        CreateGdiCompatibleTextLayout_t g_origCreateGdiCompatibleTextLayout = nullptr;
        TextLayoutDraw_t g_origTextLayoutDraw = nullptr;

        LPVOID g_targetDWriteCreateFactory = nullptr;
        LPVOID g_targetCreateTextLayout = nullptr;
        LPVOID g_targetCreateGdiCompatibleTextLayout = nullptr;
        LPVOID g_targetTextLayoutDraw = nullptr;

        std::mutex g_hookMutex;
        thread_local bool g_insideDWriteDrawGlyphRun = false;

        // RAII guard for hook reference counting (DWrite path).
        struct DWriteHookRefGuard
        {
            DWriteHookRefGuard() { ++g_activeHookCount; }
            ~DWriteHookRefGuard() { --g_activeHookCount; }
            DWriteHookRefGuard(const DWriteHookRefGuard&) = delete;
            DWriteHookRefGuard& operator=(const DWriteHookRefGuard&) = delete;
        };

        template <typename T>
        T GetVtableMethod(void* object, const size_t index)
        {
            auto** vtable = *static_cast<void***>(object);
            if (!vtable) return nullptr;
            return reinterpret_cast<T>(vtable[index]);
        }

        bool IsReadablePointer(void const* p)
        {
            if (!p) return false;
            MEMORY_BASIC_INFORMATION mbi = {};
            if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
            if (mbi.State != MEM_COMMIT) return false;
            if ((mbi.Protect & PAGE_NOACCESS) != 0) return false;
            if ((mbi.Protect & PAGE_GUARD) != 0) return false;
            return true;
        }

        bool IsFromD2DModule(void const* functionAddress)
        {
            if (!functionAddress) return false;

            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    static_cast<LPCWSTR>(functionAddress), &module))
            {
                return false;
            }

            wchar_t modulePath[MAX_PATH] = {};
            if (GetModuleFileNameW(module, modulePath, MAX_PATH) == 0) return false;

            std::wstring lowerPath(modulePath);
            std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });

            return lowerPath.find(L"\\d2d1.dll") != std::wstring::npos;
        }

        ID2D1RenderTarget* TryGetD2DRenderTarget(void* clientDrawingContext)
        {
            if (!clientDrawingContext) return nullptr;

            auto* pUnk = static_cast<IUnknown*>(clientDrawingContext);
            if (!IsReadablePointer(pUnk)) return nullptr;

            // Try ID2D1DeviceContext first (D2D1.1+, most modern apps).
            ID2D1DeviceContext* deviceContext = nullptr;
            if (SUCCEEDED(
                pUnk->QueryInterface(__uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(&deviceContext))))
            {
                return deviceContext;
            }

            // Fall back to base ID2D1RenderTarget.
            ID2D1RenderTarget* renderTarget = nullptr;
            if (SUCCEEDED(
                pUnk->QueryInterface(__uuidof(ID2D1RenderTarget), reinterpret_cast<void**>(&renderTarget))))
            {
                return renderTarget;
            }

            // Bug 7 fix: the original code had a third fallback that checked whether
            // vtable[0] came from d2d1.dll and then blindly reinterpret_cast'd the
            // context pointer to ID2D1RenderTarget*. This is unsafe: other D2D objects
            // (ID2D1Factory, ID2D1Geometry, etc.) also have their vtable in d2d1.dll.
            // Calling GetDpi() or CreateBitmap() on a misidentified object is UB.
            // The two QueryInterface calls above are the correct and sufficient way to
            // identify a render target — if both fail the context is not one.
            return nullptr;
        }

        std::string WideToUtf8(std::wstring const& value)
        {
            if (value.empty()) return {};
            int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (len <= 1) return {};
            std::string result(static_cast<size_t>(len), '\0');
            WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), len, nullptr, nullptr);
            if (!result.empty() && result.back() == '\0')
            {
                result.pop_back();
            }
            return result;
        }

        std::string GetFontPathFromFace(IDWriteFontFace* fontFace)
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

            void const* referenceKey = nullptr;
            UINT32 referenceKeySize = 0;
            hr = files[0]->GetReferenceKey(&referenceKey, &referenceKeySize);
            if (FAILED(hr) || !referenceKey || referenceKeySize == 0)
            {
                loader->Release();
                releaseFiles();
                return {};
            }

            std::string result;
            IDWriteLocalFontFileLoader* localLoader = nullptr;
            hr = loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),
                                        reinterpret_cast<void**>(&localLoader));
            if (SUCCEEDED(hr) && localLoader)
            {
                UINT32 pathLength = 0;
                hr = localLoader->GetFilePathLengthFromKey(referenceKey, referenceKeySize, &pathLength);
                if (SUCCEEDED(hr) && pathLength > 0)
                {
                    std::wstring path(pathLength + 1, L'\0');
                    hr = localLoader->GetFilePathFromKey(
                        referenceKey, referenceKeySize, path.data(), static_cast<UINT32>(path.size()));
                    if (SUCCEEDED(hr))
                    {
                        if (!path.empty() && path.back() == L'\0')
                        {
                            path.pop_back();
                        }
                        result = WideToUtf8(path);
                    }
                }
                localLoader->Release();
            }

            loader->Release();
            releaseFiles();
            return result;
        }

        class CustomTextRenderer final : public IDWriteTextRenderer1
        {
        public:
            explicit CustomTextRenderer(IDWriteTextRenderer* original)
            {
                m_original = original;
                if (m_original)
                {
                    m_original->AddRef();
                    m_original->QueryInterface(
                        __uuidof(IDWriteTextRenderer1), reinterpret_cast<void**>(&m_original1));
                }
            }

            virtual ~CustomTextRenderer()
            {
                if (m_original1) m_original1->Release();
                if (m_original) m_original->Release();
            }

            ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refCount; }

            ULONG STDMETHODCALLTYPE Release() override
            {
                ULONG ref = --m_refCount;
                if (ref == 0) delete this;
                return ref;
            }

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
            {
                if (!ppvObject) return E_POINTER;
                *ppvObject = nullptr;

                if (riid == __uuidof(IUnknown) ||
                    riid == __uuidof(IDWritePixelSnapping) ||
                    riid == __uuidof(IDWriteTextRenderer))
                {
                    *ppvObject = static_cast<IDWriteTextRenderer*>(this);
                    AddRef();
                    return S_OK;
                }
                else if (riid == __uuidof(IDWriteTextRenderer1) && m_original1)
                {
                    *ppvObject = static_cast<IDWriteTextRenderer1*>(this);
                    AddRef();
                    return S_OK;
                }

                if (m_original)
                {
                    return m_original->QueryInterface(riid, ppvObject);
                }

                return E_NOINTERFACE;
            }

            HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void* clientDrawingContext,
                                                              BOOL* isDisabled) override
            {
                if (m_original)
                {
                    return m_original->IsPixelSnappingDisabled(clientDrawingContext, isDisabled);
                }
                if (isDisabled) *isDisabled = FALSE;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetCurrentTransform(void* clientDrawingContext,
                                                          DWRITE_MATRIX* transform) override
            {
                if (m_original)
                {
                    return m_original->GetCurrentTransform(clientDrawingContext, transform);
                }
                if (!transform) return E_POINTER;
                *transform = DWRITE_MATRIX{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void* clientDrawingContext,
                                                      FLOAT* pixelsPerDip) override
            {
                if (m_original)
                {
                    return m_original->GetPixelsPerDip(clientDrawingContext, pixelsPerDip);
                }
                if (!pixelsPerDip) return E_POINTER;
                *pixelsPerDip = 1.0f;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE DrawGlyphRun(void* clientDrawingContext,
                                                   FLOAT baselineOriginX,
                                                   FLOAT baselineOriginY,
                                                   DWRITE_MEASURING_MODE measuringMode,
                                                   DWRITE_GLYPH_RUN const* glyphRun,
                                                   DWRITE_GLYPH_RUN_DESCRIPTION const* glyphRunDescription,
                                                   IUnknown* clientDrawingEffect) override
            {
                if (!glyphRun || glyphRun->glyphCount == 0 || !glyphRun->fontFace || glyphRun->isSideways)
                {
                    return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                               measuringMode, glyphRun, glyphRunDescription,
                                               clientDrawingEffect);
                }

                // Bug 2 fix: glyphCount > 0 does not guarantee glyphIndices is non-null.
                // A malformed glyph run (count set but pointer null) causes a null
                // dereference at glyphRun->glyphIndices[i] below.
                if (!glyphRun->glyphIndices)
                {
                    return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                               measuringMode, glyphRun, glyphRunDescription,
                                               clientDrawingEffect);
                }

                if (g_insideDWriteDrawGlyphRun)
                {
                    return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                               measuringMode, glyphRun, glyphRunDescription,
                                               clientDrawingEffect);
                }

                // RAII reference count — prevents rasterizer shutdown while rendering
                DWriteHookRefGuard refGuard;

                const auto& cfg = Config::Instance().Data();
                if (cfg.filterStrength <= 0.0f)
                {
                    return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                               measuringMode, glyphRun, glyphRunDescription,
                                               clientDrawingEffect);
                }

                ID2D1RenderTarget* renderTarget = TryGetD2DRenderTarget(clientDrawingContext);
                if (!renderTarget)
                {
                    return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                               measuringMode, glyphRun, glyphRunDescription,
                                               clientDrawingEffect);
                }

                // RAII safety logic to automatically release the target when the method exits
                struct RenderTargetReleaser
                {
                    ID2D1RenderTarget* rt;
                    ~RenderTargetReleaser() { if (rt) rt->Release(); }
                } rtReleaser{renderTarget};

                std::string fontPath = GetFontPathFromFace(glyphRun->fontFace);
                if (fontPath.empty())
                {
                    return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                               measuringMode, glyphRun, glyphRunDescription,
                                               clientDrawingEffect);
                }

                FLOAT pixelsPerDip = 1.0f;

                // Hardware Scale Synchronization:
                // Extract the true scale from the render target's transform and DPI,
                // rather than relying on DWrite's reported pixelsPerDip which can be
                // inaccurate for Qt, WinUI, and modern layout engines.
                FLOAT rtDpiX = 96.0f, rtDpiY = 96.0f;
                renderTarget->GetDpi(&rtDpiX, &rtDpiY);

                D2D1_MATRIX_3X2_F rtTransform;
                renderTarget->GetTransform(&rtTransform);

                float scaleX = std::sqrt(rtTransform._11 * rtTransform._11 + rtTransform._12 * rtTransform._12);
                if (scaleX > 0.0001f)
                {
                    pixelsPerDip = (rtDpiX / 96.0f) * scaleX;
                }
                else
                {
                    (void)GetPixelsPerDip(clientDrawingContext, &pixelsPerDip);
                }

                // Clamp pixelsPerDip to a minimum of 0.25 to prevent zero-size
                // rasterization and division-by-zero in downstream positioning math.
                pixelsPerDip = std::max(0.25f, pixelsPerDip);

                const auto emPixels = static_cast<float>(std::lround(glyphRun->fontEmSize * pixelsPerDip));
                const uint32_t pixelSize = static_cast<uint32_t>(std::max(1.0f, emPixels));

                const auto filter = SubpixelFilter::Create(static_cast<int>(cfg.panelType));
                if (!filter)
                {
                    return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                               measuringMode, glyphRun, glyphRunDescription,
                                               clientDrawingEffect);
                }

                auto textColor = D2D1_COLOR_F{0.0f, 0.0f, 0.0f, 1.0f};
                if (clientDrawingEffect)
                {
                    ID2D1SolidColorBrush* brush = nullptr;
                    if (SUCCEEDED(clientDrawingEffect->QueryInterface(
                            __uuidof(ID2D1SolidColorBrush), reinterpret_cast<void**>(&brush))) &&
                        brush)
                    {
                        textColor = brush->GetColor();
                        brush->Release();
                        // Bug 8 fix: PureTypeLog in the hot path acquires a mutex and
                        // builds a va_list on every call even when logging is disabled
                        // (it returns early inside, but the call overhead is still paid).
                        // At 60fps with many glyph runs this is thousands of lock/unlock
                        // per second with no benefit. Guard all hot-path log calls.
                        if (cfg.debugEnabled)
                        {
                            PureTypeLog("DWrite DrawGlyphRun: color from brush R=%.2f G=%.2f B=%.2f A=%.2f",
                                        textColor.r, textColor.g, textColor.b, textColor.a);
                        }
                    }
                    else
                    {
                        if (cfg.debugEnabled)
                        {
                            PureTypeLog("DWrite DrawGlyphRun: clientDrawingEffect present but NOT a SolidColorBrush");
                        }
                    }
                }
                else
                {
                    if (cfg.debugEnabled)
                    {
                        PureTypeLog("DWrite DrawGlyphRun: clientDrawingEffect is NULL — defaulting to black");
                    }
                }
                if (cfg.debugEnabled)
                {
                    PureTypeLog("  font='%s' emSize=%.1f pixelsPerDip=%.2f pixelSize=%u glyphCount=%u",
                                fontPath.c_str(), glyphRun->fontEmSize, pixelsPerDip, pixelSize,
                                glyphRun->glyphCount);
                }

                struct PendingGlyph
                {
                    FLOAT x = 0.0f;
                    FLOAT y = 0.0f;
                    RGBABitmap bitmap;
                };
                std::vector<PendingGlyph> pending;
                pending.reserve(glyphRun->glyphCount);

                FLOAT penX = baselineOriginX;
                for (UINT32 i = 0; i < glyphRun->glyphCount; ++i)
                {
                    const GlyphBitmap* glyph = FTRasterizer::Instance().RasterizeGlyph(
                        fontPath, glyphRun->glyphIndices[i], pixelSize);
                    if (!glyph)
                    {
                        return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                                   measuringMode, glyphRun, glyphRunDescription,
                                                   clientDrawingEffect);
                    }

                    // Glyph positioning with padding-aware bearing.
                    FLOAT bearingXPixels = static_cast<FLOAT>(glyph->bearingX - glyph->padLeft) / 3.0f;
                    FLOAT glyphX = penX + bearingXPixels / pixelsPerDip;
                    FLOAT glyphY = baselineOriginY
                        - static_cast<FLOAT>(glyph->bearingY + glyph->padTop) / pixelsPerDip;

                    if (glyphRun->glyphOffsets)
                    {
                        glyphX += glyphRun->glyphOffsets[i].advanceOffset;
                        glyphY -= glyphRun->glyphOffsets[i].ascenderOffset;
                    }

                    const FLOAT advance = glyphRun->glyphAdvances
                                              ? glyphRun->glyphAdvances[i]
                                              : (static_cast<FLOAT>(glyph->advanceX) / 3.0f) / pixelsPerDip;
                    penX += advance;

                    if (glyph->width <= 0 || glyph->height <= 0 || glyph->data.empty())
                    {
                        continue;
                    }

                    RGBABitmap filtered = filter->Apply(*glyph, cfg);
                    if (filtered.data.empty())
                    {
                        return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                                   measuringMode, glyphRun, glyphRunDescription,
                                                   clientDrawingEffect);
                    }

                    if (cfg.highlightRenderedGlyphs)
                    {
                        for (size_t p = 0; p + 3 < filtered.data.size(); p += 4)
                        {
                            filtered.data[p + 0] =
                                static_cast<uint8_t>(filtered.data[p + 0] * 0.7f + 200.0f * 0.3f);
                            filtered.data[p + 1] =
                                static_cast<uint8_t>(filtered.data[p + 1] * 0.7f + 200.0f * 0.3f);
                        }
                    }

                    PendingGlyph pg;
                    pg.x = glyphX;
                    pg.y = glyphY;
                    pg.bitmap = std::move(filtered);
                    pending.push_back(std::move(pg));
                }

                g_insideDWriteDrawGlyphRun = true;
                bool allGlyphsBlitted = true;

                for (const auto& item : pending)
                {
                    bool blitOk = Blender::Instance().BlitToD2DTarget(
                        renderTarget, item.x, item.y, item.bitmap, textColor, cfg.gamma, pixelsPerDip);
                    if (!blitOk)
                    {
                        allGlyphsBlitted = false;
                        break;
                    }
                }

                g_insideDWriteDrawGlyphRun = false;

                if (allGlyphsBlitted)
                {
                    // D2D/DComp Shadow Buffer Fix:
                    // Forward the call with a transparent brush to register bounding boxes
                    // for dirty-rect invalidation without drawing stock ClearType.
                    ID2D1SolidColorBrush* transparentBrush = nullptr;
                    HRESULT hr = renderTarget->CreateSolidColorBrush(
                        D2D1_COLOR_F{0.0f, 0.0f, 0.0f, 0.0f}, &transparentBrush);

                    if (SUCCEEDED(hr) && transparentBrush)
                    {
                        ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                            measuringMode, glyphRun, glyphRunDescription,
                                            transparentBrush);
                        transparentBrush->Release();
                    }
                    else
                    {
                        ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                            measuringMode, glyphRun, glyphRunDescription,
                                            clientDrawingEffect);
                    }

                    return S_OK;
                }

                return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                           measuringMode, glyphRun, glyphRunDescription,
                                           clientDrawingEffect);
            }

            // IDWriteTextRenderer1 extension for DComp and Modern UI caching
            HRESULT STDMETHODCALLTYPE DrawGlyphRun(
                void* clientDrawingContext,
                FLOAT baselineOriginX,
                FLOAT baselineOriginY,
                DWRITE_GLYPH_ORIENTATION_ANGLE orientationAngle,
                DWRITE_MEASURING_MODE measuringMode,
                DWRITE_GLYPH_RUN const* glyphRun,
                DWRITE_GLYPH_RUN_DESCRIPTION const* glyphRunDescription,
                IUnknown* clientDrawingEffect) override
            {
                if (orientationAngle != DWRITE_GLYPH_ORIENTATION_ANGLE_0_DEGREES)
                {
                    if (!m_original1) return E_NOTIMPL;
                    return m_original1->DrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                                     orientationAngle, measuringMode, glyphRun,
                                                     glyphRunDescription, clientDrawingEffect);
                }

                return DrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                    measuringMode, glyphRun, glyphRunDescription, clientDrawingEffect);
            }

            HRESULT STDMETHODCALLTYPE DrawUnderline(void* clientDrawingContext,
                                                    FLOAT baselineOriginX,
                                                    FLOAT baselineOriginY,
                                                    DWRITE_UNDERLINE const* underline,
                                                    IUnknown* clientDrawingEffect) override
            {
                if (!m_original) return E_NOTIMPL;
                return m_original->DrawUnderline(clientDrawingContext, baselineOriginX, baselineOriginY,
                                                 underline, clientDrawingEffect);
            }

            HRESULT STDMETHODCALLTYPE DrawStrikethrough(void* clientDrawingContext,
                                                        FLOAT baselineOriginX,
                                                        FLOAT baselineOriginY,
                                                        DWRITE_STRIKETHROUGH const* strikethrough,
                                                        IUnknown* clientDrawingEffect) override
            {
                if (!m_original) return E_NOTIMPL;
                return m_original->DrawStrikethrough(clientDrawingContext, baselineOriginX, baselineOriginY,
                                                     strikethrough, clientDrawingEffect);
            }

            HRESULT STDMETHODCALLTYPE DrawInlineObject(void* clientDrawingContext,
                                                       FLOAT originX,
                                                       FLOAT originY,
                                                       IDWriteInlineObject* inlineObject,
                                                       BOOL isSideways,
                                                       BOOL isRightToLeft,
                                                       IUnknown* clientDrawingEffect) override
            {
                if (!m_original) return E_NOTIMPL;
                return m_original->DrawInlineObject(clientDrawingContext, originX, originY, inlineObject,
                                                    isSideways, isRightToLeft, clientDrawingEffect);
            }

            // IDWriteTextRenderer1 overrides
            HRESULT STDMETHODCALLTYPE DrawUnderline(void* clientDrawingContext,
                                                    FLOAT baselineOriginX,
                                                    FLOAT baselineOriginY,
                                                    DWRITE_GLYPH_ORIENTATION_ANGLE orientationAngle,
                                                    DWRITE_UNDERLINE const* underline,
                                                    IUnknown* clientDrawingEffect) override
            {
                if (!m_original1) return E_NOTIMPL;
                return m_original1->DrawUnderline(clientDrawingContext, baselineOriginX, baselineOriginY,
                                                  orientationAngle, underline, clientDrawingEffect);
            }

            HRESULT STDMETHODCALLTYPE DrawStrikethrough(void* clientDrawingContext,
                                                        FLOAT baselineOriginX,
                                                        FLOAT baselineOriginY,
                                                        DWRITE_GLYPH_ORIENTATION_ANGLE orientationAngle,
                                                        DWRITE_STRIKETHROUGH const* strikethrough,
                                                        IUnknown* clientDrawingEffect) override
            {
                if (!m_original1) return E_NOTIMPL;
                return m_original1->DrawStrikethrough(clientDrawingContext, baselineOriginX, baselineOriginY,
                                                      orientationAngle, strikethrough, clientDrawingEffect);
            }

            HRESULT STDMETHODCALLTYPE DrawInlineObject(void* clientDrawingContext,
                                                       FLOAT originX,
                                                       FLOAT originY,
                                                       DWRITE_GLYPH_ORIENTATION_ANGLE orientationAngle,
                                                       IDWriteInlineObject* inlineObject,
                                                       BOOL isSideways,
                                                       BOOL isRightToLeft,
                                                       IUnknown* clientDrawingEffect) override
            {
                if (!m_original1) return E_NOTIMPL;
                return m_original1->DrawInlineObject(clientDrawingContext, originX, originY,
                                                     orientationAngle, inlineObject,
                                                     isSideways, isRightToLeft, clientDrawingEffect);
            }

        private:
            HRESULT ForwardDrawGlyphRun(void* clientDrawingContext,
                                        FLOAT baselineOriginX,
                                        FLOAT baselineOriginY,
                                        DWRITE_MEASURING_MODE measuringMode,
                                        DWRITE_GLYPH_RUN const* glyphRun,
                                        DWRITE_GLYPH_RUN_DESCRIPTION const* glyphRunDescription,
                                        IUnknown* clientDrawingEffect)
            {
                if (!m_original) return E_NOTIMPL;
                return m_original->DrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                                measuringMode, glyphRun, glyphRunDescription,
                                                clientDrawingEffect);
            }

            ULONG m_refCount = 1;
            IDWriteTextRenderer* m_original = nullptr;
            IDWriteTextRenderer1* m_original1 = nullptr;
        };

        HRESULT STDMETHODCALLTYPE HookedTextLayoutDraw(IDWriteTextLayout* textLayout,
                                                       void* clientDrawingContext,
                                                       IDWriteTextRenderer* renderer,
                                                       FLOAT originX,
                                                       FLOAT originY);

        bool InstallTextLayoutDrawHook(IDWriteTextLayout* textLayout)
        {
            if (!textLayout) return false;
            std::lock_guard<std::mutex> lock(g_hookMutex);

            if (g_origTextLayoutDraw && g_targetTextLayoutDraw)
            {
                return true;
            }

            g_targetTextLayoutDraw = reinterpret_cast<LPVOID>(
                GetVtableMethod<TextLayoutDraw_t>(textLayout, kTextLayoutDrawVtableIndex));
            if (!g_targetTextLayoutDraw)
            {
                PureTypeLog("InstallTextLayoutDrawHook: vtable slot lookup failed");
                return false;
            }

            MH_STATUS status = MH_CreateHook(g_targetTextLayoutDraw,
                                             reinterpret_cast<LPVOID>(&HookedTextLayoutDraw),
                                             reinterpret_cast<LPVOID*>(&g_origTextLayoutDraw));
            if (status != MH_OK)
            {
                PureTypeLog("MH_CreateHook(IDWriteTextLayout::Draw) failed: %s", MH_StatusToString(status));
                return false;
            }

            status = MH_EnableHook(g_targetTextLayoutDraw);
            if (status != MH_OK && status != MH_ERROR_ENABLED)
            {
                PureTypeLog("MH_EnableHook(IDWriteTextLayout::Draw) failed: %s", MH_StatusToString(status));
                return false;
            }

            PureTypeLog("Hooked IDWriteTextLayout::Draw at %p", g_targetTextLayoutDraw);
            return true;
        }

        HRESULT STDMETHODCALLTYPE HookedCreateTextLayout(IDWriteFactory* factory,
                                                         WCHAR const* string,
                                                         UINT32 stringLength,
                                                         IDWriteTextFormat* textFormat,
                                                         FLOAT maxWidth,
                                                         FLOAT maxHeight,
                                                         IDWriteTextLayout** textLayout)
        {
            if (!g_origCreateTextLayout) return E_FAIL;

            HRESULT hr = g_origCreateTextLayout(factory, string, stringLength, textFormat, maxWidth,
                                                maxHeight, textLayout);
            if (SUCCEEDED(hr) && textLayout && *textLayout)
            {
                InstallTextLayoutDrawHook(*textLayout);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE HookedCreateGdiCompatibleTextLayout(IDWriteFactory* factory,
                                                                      WCHAR const* string,
                                                                      UINT32 stringLength,
                                                                      IDWriteTextFormat* textFormat,
                                                                      FLOAT layoutWidth,
                                                                      FLOAT layoutHeight,
                                                                      FLOAT pixelsPerDip,
                                                                      DWRITE_MATRIX const* transform,
                                                                      BOOL useGdiNatural,
                                                                      IDWriteTextLayout** textLayout)
        {
            if (!g_origCreateGdiCompatibleTextLayout) return E_FAIL;

            HRESULT hr = g_origCreateGdiCompatibleTextLayout(factory, string, stringLength, textFormat,
                                                             layoutWidth, layoutHeight, pixelsPerDip,
                                                             transform, useGdiNatural, textLayout);
            if (SUCCEEDED(hr) && textLayout && *textLayout)
            {
                InstallTextLayoutDrawHook(*textLayout);
            }
            return hr;
        }

        bool InstallFactoryMethodHooks(IDWriteFactory* factory)
        {
            if (!factory) return false;

            std::lock_guard<std::mutex> lock(g_hookMutex);
            auto* createTextLayoutTarget = reinterpret_cast<LPVOID>(
                GetVtableMethod<CreateTextLayout_t>(factory, kFactoryCreateTextLayoutVtableIndex));
            auto* createGdiTextLayoutTarget = reinterpret_cast<LPVOID>(GetVtableMethod<
                CreateGdiCompatibleTextLayout_t>(
                factory, kFactoryCreateGdiCompatibleTextLayoutVtableIndex));

            if (!createTextLayoutTarget || !createGdiTextLayoutTarget)
            {
                PureTypeLog("InstallFactoryMethodHooks: factory vtable lookup failed");
                return false;
            }

            if (!g_origCreateTextLayout)
            {
                g_targetCreateTextLayout = createTextLayoutTarget;
                MH_STATUS status = MH_CreateHook(g_targetCreateTextLayout,
                                                 reinterpret_cast<LPVOID>(&HookedCreateTextLayout),
                                                 reinterpret_cast<LPVOID*>(&g_origCreateTextLayout));
                if (status != MH_OK)
                {
                    PureTypeLog("MH_CreateHook(IDWriteFactory::CreateTextLayout) failed: %s",
                                MH_StatusToString(status));
                    return false;
                }
                status = MH_EnableHook(g_targetCreateTextLayout);
                if (status != MH_OK && status != MH_ERROR_ENABLED)
                {
                    PureTypeLog("MH_EnableHook(IDWriteFactory::CreateTextLayout) failed: %s",
                                MH_StatusToString(status));
                    return false;
                }
            }

            if (!g_origCreateGdiCompatibleTextLayout)
            {
                g_targetCreateGdiCompatibleTextLayout = createGdiTextLayoutTarget;
                MH_STATUS status = MH_CreateHook(g_targetCreateGdiCompatibleTextLayout,
                                                 reinterpret_cast<LPVOID>(&HookedCreateGdiCompatibleTextLayout),
                                                 reinterpret_cast<LPVOID*>(&g_origCreateGdiCompatibleTextLayout));
                if (status != MH_OK)
                {
                    PureTypeLog("MH_CreateHook(IDWriteFactory::CreateGdiCompatibleTextLayout) failed: %s",
                                MH_StatusToString(status));
                    return false;
                }
                status = MH_EnableHook(g_targetCreateGdiCompatibleTextLayout);
                if (status != MH_OK && status != MH_ERROR_ENABLED)
                {
                    PureTypeLog("MH_EnableHook(IDWriteFactory::CreateGdiCompatibleTextLayout) failed: %s",
                                MH_StatusToString(status));
                    return false;
                }
            }

            return true;
        }

        bool PrimeExistingDWriteObjects()
        {
            HMODULE hDwrite = GetModuleHandleW(L"dwrite.dll");
            if (!hDwrite) return false;

            auto createFactory =
                reinterpret_cast<DWriteCreateFactory_t>(GetProcAddress(hDwrite, "DWriteCreateFactory"));
            if (!createFactory) return false;

            IDWriteFactory* factory = nullptr;
            HRESULT hr = createFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&factory));
            if (FAILED(hr) || !factory)
            {
                PureTypeLog("PrimeExistingDWriteObjects: temporary factory creation failed (0x%08X)",
                            hr);
                return false;
            }

            bool ok = InstallFactoryMethodHooks(factory);

            IDWriteTextFormat* textFormat = nullptr;
            hr = factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f,
                                           L"en-us", &textFormat);
            if (SUCCEEDED(hr) && textFormat)
            {
                IDWriteTextLayout* textLayout = nullptr;
                hr = factory->CreateTextLayout(L"x", 1, textFormat, 32.0f, 32.0f, &textLayout);
                if (SUCCEEDED(hr) && textLayout)
                {
                    ok = InstallTextLayoutDrawHook(textLayout) && ok;
                    textLayout->Release();
                }
                else
                {
                    PureTypeLog("PrimeExistingDWriteObjects: temporary text layout creation failed (0x%08X)",
                                hr);
                    ok = false;
                }
                textFormat->Release();
            }
            else
            {
                PureTypeLog("PrimeExistingDWriteObjects: temporary text format creation failed (0x%08X)",
                            hr);
                ok = false;
            }

            factory->Release();
            return ok;
        }

        HRESULT WINAPI HookedDWriteCreateFactory(DWRITE_FACTORY_TYPE factoryType,
                                                 REFIID iid,
                                                 IUnknown** factory)
        {
            (void)iid;

            if (!g_origDWriteCreateFactory) return E_FAIL;

            HRESULT hr = g_origDWriteCreateFactory(factoryType, iid, factory);
            if (FAILED(hr) || !factory || !*factory) return hr;

            IDWriteFactory* baseFactory = nullptr;
            HRESULT qi = (*factory)->QueryInterface(__uuidof(IDWriteFactory),
                                                    reinterpret_cast<void**>(&baseFactory));
            if (SUCCEEDED(qi) && baseFactory)
            {
                InstallFactoryMethodHooks(baseFactory);
                baseFactory->Release();
            }

            return hr;
        }

        HRESULT STDMETHODCALLTYPE HookedTextLayoutDraw(IDWriteTextLayout* textLayout,
                                                       void* clientDrawingContext,
                                                       IDWriteTextRenderer* renderer,
                                                       FLOAT originX,
                                                       FLOAT originY)
        {
            if (!g_origTextLayoutDraw || !renderer)
            {
                return g_origTextLayoutDraw
                           ? g_origTextLayoutDraw(textLayout, clientDrawingContext, renderer, originX, originY)
                           : E_FAIL;
            }

            auto* wrappedRenderer = new(std::nothrow) CustomTextRenderer(renderer);
            if (!wrappedRenderer)
            {
                return g_origTextLayoutDraw(textLayout, clientDrawingContext, renderer, originX, originY);
            }

            HRESULT hr =
                g_origTextLayoutDraw(textLayout, clientDrawingContext, wrappedRenderer, originX, originY);
            wrappedRenderer->Release();
            return hr;
        }
    }

    bool InstallDWriteHooks()
    {
        HMODULE hDwrite = GetModuleHandleW(L"dwrite.dll");
        if (!hDwrite)
        {
            hDwrite = LoadLibraryW(L"dwrite.dll");
        }
        if (!hDwrite)
        {
            PureTypeLog("InstallDWriteHooks: dwrite.dll not available");
            return false;
        }

        g_targetDWriteCreateFactory = reinterpret_cast<LPVOID>(GetProcAddress(hDwrite, "DWriteCreateFactory"));
        if (!g_targetDWriteCreateFactory)
        {
            PureTypeLog("InstallDWriteHooks: DWriteCreateFactory not found");
            return false;
        }

        MH_STATUS status = MH_CreateHook(g_targetDWriteCreateFactory,
                                         reinterpret_cast<LPVOID>(&HookedDWriteCreateFactory),
                                         reinterpret_cast<LPVOID*>(&g_origDWriteCreateFactory));
        if (status != MH_OK)
        {
            PureTypeLog("MH_CreateHook(DWriteCreateFactory) failed: %s", MH_StatusToString(status));
            return false;
        }

        if (!PrimeExistingDWriteObjects())
        {
            PureTypeLog("InstallDWriteHooks: initial DWrite object priming incomplete");
        }

        return true;
    }

    void RemoveDWriteHooks()
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);

        if (g_targetTextLayoutDraw)
        {
            MH_RemoveHook(g_targetTextLayoutDraw);
            g_targetTextLayoutDraw = nullptr;
            g_origTextLayoutDraw = nullptr;
        }

        if (g_targetCreateGdiCompatibleTextLayout)
        {
            MH_RemoveHook(g_targetCreateGdiCompatibleTextLayout);
            g_targetCreateGdiCompatibleTextLayout = nullptr;
            g_origCreateGdiCompatibleTextLayout = nullptr;
        }

        if (g_targetCreateTextLayout)
        {
            MH_RemoveHook(g_targetCreateTextLayout);
            g_targetCreateTextLayout = nullptr;
            g_origCreateTextLayout = nullptr;
        }

        if (g_targetDWriteCreateFactory)
        {
            MH_RemoveHook(g_targetDWriteCreateFactory);
            g_targetDWriteCreateFactory = nullptr;
            g_origDWriteCreateFactory = nullptr;
        }
    }
}