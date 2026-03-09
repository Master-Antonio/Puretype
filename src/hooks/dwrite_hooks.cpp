

#include "hooks/dwrite_hooks.h"

#include "config.h"
#include "filters/subpixel_filter.h"
#include "output/blender.h"
#include "rasterizer/ft_rasterizer.h"

#include <MinHook.h>
#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <mutex>
#include <new>
#include <string>
#include <vector>

extern void PureTypeLog(const char* fmt, ...);

namespace puretype {
namespace hooks {

namespace {

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

static DWriteCreateFactory_t g_origDWriteCreateFactory = nullptr;
static CreateTextLayout_t g_origCreateTextLayout = nullptr;
static CreateGdiCompatibleTextLayout_t g_origCreateGdiCompatibleTextLayout = nullptr;
static TextLayoutDraw_t g_origTextLayoutDraw = nullptr;

static LPVOID g_targetDWriteCreateFactory = nullptr;
static LPVOID g_targetCreateTextLayout = nullptr;
static LPVOID g_targetCreateGdiCompatibleTextLayout = nullptr;
static LPVOID g_targetTextLayoutDraw = nullptr;

static std::mutex g_hookMutex;
static thread_local bool g_insideDWriteDrawGlyphRun = false;

template <typename T>
T GetVtableMethod(void* object, size_t index) {
    if (!object) return nullptr;
    auto** vtable = *reinterpret_cast<void***>(object);
    if (!vtable) return nullptr;
    return reinterpret_cast<T>(vtable[index]);
}

bool IsReadablePointer(void const* p) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & PAGE_NOACCESS) != 0) return false;
    if ((mbi.Protect & PAGE_GUARD) != 0) return false;
    return true;
}

bool IsFromD2DModule(void const* functionAddress) {
    if (!functionAddress) return false;

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(functionAddress), &module)) {
        return false;
    }

    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(module, modulePath, MAX_PATH) == 0) return false;

    std::wstring lowerPath(modulePath);
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return lowerPath.find(L"\\d2d1.dll") != std::wstring::npos;
}

ID2D1RenderTarget* TryGetD2DRenderTarget(void* clientDrawingContext) {
    if (!clientDrawingContext) return nullptr;
    if (!IsReadablePointer(clientDrawingContext)) return nullptr;

    if (!IsReadablePointer(*reinterpret_cast<void**>(clientDrawingContext))) return nullptr;
    auto** vtable = *reinterpret_cast<void***>(clientDrawingContext);
    if (!vtable || !vtable[0]) return nullptr;
    // Only treat context as render target when its vtable resolves into d2d1.dll.
    if (!IsFromD2DModule(vtable[0])) return nullptr;

    return reinterpret_cast<ID2D1RenderTarget*>(clientDrawingContext);
}

std::string WideToUtf8(std::wstring const& value) {
    if (value.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string result(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), len, nullptr, nullptr);
    if (!result.empty() && result.back() == '\0') {
        result.pop_back();
    }
    return result;
}

std::string GetFontPathFromFace(IDWriteFontFace* fontFace) {
    if (!fontFace) return {};

    UINT32 fileCount = 0;
    HRESULT hr = fontFace->GetFiles(&fileCount, nullptr);
    if (FAILED(hr) || fileCount == 0) return {};

    std::vector<IDWriteFontFile*> files(fileCount, nullptr);
    hr = fontFace->GetFiles(&fileCount, files.data());
    if (FAILED(hr) || files.empty()) return {};

    auto releaseFiles = [&]() {
        for (auto* file : files) {
            if (file) file->Release();
        }
    };

    IDWriteFontFileLoader* loader = nullptr;
    hr = files[0]->GetLoader(&loader);
    if (FAILED(hr) || !loader) {
        releaseFiles();
        return {};
    }

    void const* referenceKey = nullptr;
    UINT32 referenceKeySize = 0;
    hr = files[0]->GetReferenceKey(&referenceKey, &referenceKeySize);
    if (FAILED(hr) || !referenceKey || referenceKeySize == 0) {
        loader->Release();
        releaseFiles();
        return {};
    }

    std::string result;
    IDWriteLocalFontFileLoader* localLoader = nullptr;
    hr = loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),
                                reinterpret_cast<void**>(&localLoader));
    if (SUCCEEDED(hr) && localLoader) {
        UINT32 pathLength = 0;
        hr = localLoader->GetFilePathLengthFromKey(referenceKey, referenceKeySize, &pathLength);
        if (SUCCEEDED(hr) && pathLength > 0) {
            std::wstring path(pathLength + 1, L'\0');
            hr = localLoader->GetFilePathFromKey(
                referenceKey, referenceKeySize, path.data(), static_cast<UINT32>(path.size()));
            if (SUCCEEDED(hr)) {
                if (!path.empty() && path.back() == L'\0') {
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

class CustomTextRenderer final : public IDWriteTextRenderer {
public:
    explicit CustomTextRenderer(IDWriteTextRenderer* original) : m_original(original) {
        if (m_original) m_original->AddRef();
    }

    ~CustomTextRenderer() {
        if (m_original) m_original->Release();
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refCount; }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ref = --m_refCount;
        if (ref == 0) delete this;
        return ref;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;

        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) ||
            riid == __uuidof(IDWriteTextRenderer)) {
            *ppvObject = this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void* clientDrawingContext,
                                                      BOOL* isDisabled) override {
        if (m_original) {
            return m_original->IsPixelSnappingDisabled(clientDrawingContext, isDisabled);
        }
        if (isDisabled) *isDisabled = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void* clientDrawingContext,
                                                  DWRITE_MATRIX* transform) override {
        if (m_original) {
            return m_original->GetCurrentTransform(clientDrawingContext, transform);
        }
        if (!transform) return E_POINTER;
        *transform = DWRITE_MATRIX{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void* clientDrawingContext,
                                              FLOAT* pixelsPerDip) override {
        if (m_original) {
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
                                           IUnknown* clientDrawingEffect) override {
        if (!glyphRun || glyphRun->glyphCount == 0 || !glyphRun->fontFace || glyphRun->isSideways) {
            return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                       measuringMode, glyphRun, glyphRunDescription,
                                       clientDrawingEffect);
        }

        if (g_insideDWriteDrawGlyphRun) {
            // Prevent recursive re-entry when our own drawing calls trigger DirectWrite again.
            return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                       measuringMode, glyphRun, glyphRunDescription,
                                       clientDrawingEffect);
        }

        const auto& cfg = Config::Instance().Data();
        if (cfg.filterStrength <= 0.0f) {
            return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                       measuringMode, glyphRun, glyphRunDescription,
                                       clientDrawingEffect);
        }

        ID2D1RenderTarget* renderTarget = TryGetD2DRenderTarget(clientDrawingContext);
        if (!renderTarget) {
            return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                       measuringMode, glyphRun, glyphRunDescription,
                                       clientDrawingEffect);
        }

        std::string fontPath = GetFontPathFromFace(glyphRun->fontFace);
        if (fontPath.empty()) {
            wchar_t windowsDir[MAX_PATH] = {};
            GetWindowsDirectoryW(windowsDir, MAX_PATH);
            std::wstring fallback = std::wstring(windowsDir) + L"\\Fonts\\segoeui.ttf";
            if (GetFileAttributesW(fallback.c_str()) != INVALID_FILE_ATTRIBUTES) {
                fontPath = WideToUtf8(fallback);
            }
            if (fontPath.empty()) {
                return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                           measuringMode, glyphRun, glyphRunDescription,
                                           clientDrawingEffect);
            }
        }

        FLOAT pixelsPerDip = 1.0f;
        (void)GetPixelsPerDip(clientDrawingContext, &pixelsPerDip);
        float emPixels = static_cast<float>(std::lround(glyphRun->fontEmSize * pixelsPerDip));
        uint32_t pixelSize = static_cast<uint32_t>(std::max(1.0f, emPixels));

        auto filter = SubpixelFilter::Create(static_cast<int>(cfg.panelType));
        if (!filter) {
            return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                       measuringMode, glyphRun, glyphRunDescription,
                                       clientDrawingEffect);
        }

        D2D1_COLOR_F textColor = D2D1_COLOR_F{0.0f, 0.0f, 0.0f, 1.0f};
        if (clientDrawingEffect) {
            ID2D1SolidColorBrush* brush = nullptr;
            if (SUCCEEDED(clientDrawingEffect->QueryInterface(
                    __uuidof(ID2D1SolidColorBrush), reinterpret_cast<void**>(&brush))) &&
                brush) {
                textColor = brush->GetColor();
                brush->Release();
            }
        }

        struct PendingGlyph {
            FLOAT x = 0.0f;
            FLOAT y = 0.0f;
            RGBABitmap bitmap;
        };
        std::vector<PendingGlyph> pending;
        pending.reserve(glyphRun->glyphCount);

        FLOAT penX = baselineOriginX;
        for (UINT32 i = 0; i < glyphRun->glyphCount; ++i) {
            const GlyphBitmap* glyph = FTRasterizer::Instance().RasterizeGlyph(
                fontPath, glyphRun->glyphIndices[i], pixelSize);
            if (!glyph) {
                return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                           measuringMode, glyphRun, glyphRunDescription,
                                           clientDrawingEffect);
            }

            FLOAT glyphX = penX + static_cast<FLOAT>(glyph->bearingX) / 3.0f;
            FLOAT glyphY = baselineOriginY - static_cast<FLOAT>(glyph->bearingY);
            if (glyphRun->glyphOffsets) {
                glyphX += glyphRun->glyphOffsets[i].advanceOffset;
                glyphY -= glyphRun->glyphOffsets[i].ascenderOffset;
            }

            const FLOAT advance = glyphRun->glyphAdvances
                                      ? glyphRun->glyphAdvances[i]
                                      : (static_cast<FLOAT>(glyph->advanceX) / 3.0f);
            penX += advance;

            if (glyph->width <= 0 || glyph->height <= 0 || glyph->data.empty()) {
                continue;
            }

            RGBABitmap filtered = filter->Apply(*glyph, cfg);
            if (filtered.data.empty()) {
                return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                           measuringMode, glyphRun, glyphRunDescription,
                                           clientDrawingEffect);
            }

            if (cfg.highlightRenderedGlyphs) {
                for (size_t p = 0; p + 3 < filtered.data.size(); p += 4) {
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

        for (const auto& item : pending) {
            bool blitOk = Blender::Instance().BlitToD2DTarget(
                renderTarget, item.x, item.y, item.bitmap, textColor, cfg.gamma);
            if (!blitOk) {
                allGlyphsBlitted = false;
                break;
            }
        }

        g_insideDWriteDrawGlyphRun = false;

        if (allGlyphsBlitted) {
            return S_OK;
        }

        return ForwardDrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                   measuringMode, glyphRun, glyphRunDescription,
                                   clientDrawingEffect);
    }

    HRESULT STDMETHODCALLTYPE DrawUnderline(void* clientDrawingContext,
                                            FLOAT baselineOriginX,
                                            FLOAT baselineOriginY,
                                            DWRITE_UNDERLINE const* underline,
                                            IUnknown* clientDrawingEffect) override {
        if (!m_original) return E_NOTIMPL;
        return m_original->DrawUnderline(clientDrawingContext, baselineOriginX, baselineOriginY,
                                         underline, clientDrawingEffect);
    }

    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void* clientDrawingContext,
                                                FLOAT baselineOriginX,
                                                FLOAT baselineOriginY,
                                                DWRITE_STRIKETHROUGH const* strikethrough,
                                                IUnknown* clientDrawingEffect) override {
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
                                               IUnknown* clientDrawingEffect) override {
        if (!m_original) return E_NOTIMPL;
        return m_original->DrawInlineObject(clientDrawingContext, originX, originY, inlineObject,
                                            isSideways, isRightToLeft, clientDrawingEffect);
    }

private:
    HRESULT ForwardDrawGlyphRun(void* clientDrawingContext,
                                FLOAT baselineOriginX,
                                FLOAT baselineOriginY,
                                DWRITE_MEASURING_MODE measuringMode,
                                DWRITE_GLYPH_RUN const* glyphRun,
                                DWRITE_GLYPH_RUN_DESCRIPTION const* glyphRunDescription,
                                IUnknown* clientDrawingEffect) {
        if (!m_original) return E_NOTIMPL;
        return m_original->DrawGlyphRun(clientDrawingContext, baselineOriginX, baselineOriginY,
                                        measuringMode, glyphRun, glyphRunDescription,
                                        clientDrawingEffect);
    }

    ULONG m_refCount = 1;
    IDWriteTextRenderer* m_original = nullptr;
};

HRESULT STDMETHODCALLTYPE HookedTextLayoutDraw(IDWriteTextLayout* textLayout,
                                               void* clientDrawingContext,
                                               IDWriteTextRenderer* renderer,
                                               FLOAT originX,
                                               FLOAT originY);

bool InstallTextLayoutDrawHook(IDWriteTextLayout* textLayout) {
    if (!textLayout) return false;
    std::lock_guard<std::mutex> lock(g_hookMutex);

    if (g_origTextLayoutDraw && g_targetTextLayoutDraw) {
        return true;
    }

    g_targetTextLayoutDraw = reinterpret_cast<LPVOID>(
        GetVtableMethod<TextLayoutDraw_t>(textLayout, kTextLayoutDrawVtableIndex));
    if (!g_targetTextLayoutDraw) {
        PureTypeLog("InstallTextLayoutDrawHook: vtable slot lookup failed");
        return false;
    }

    MH_STATUS status = MH_CreateHook(g_targetTextLayoutDraw,
                                     reinterpret_cast<LPVOID>(&HookedTextLayoutDraw),
                                     reinterpret_cast<LPVOID*>(&g_origTextLayoutDraw));
    if (status != MH_OK) {
        PureTypeLog("MH_CreateHook(IDWriteTextLayout::Draw) failed: %s", MH_StatusToString(status));
        return false;
    }

    status = MH_EnableHook(g_targetTextLayoutDraw);
    if (status != MH_OK && status != MH_ERROR_ENABLED) {
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
                                                 IDWriteTextLayout** textLayout) {
    if (!g_origCreateTextLayout) return E_FAIL;

    HRESULT hr = g_origCreateTextLayout(factory, string, stringLength, textFormat, maxWidth,
                                        maxHeight, textLayout);
    if (SUCCEEDED(hr) && textLayout && *textLayout) {
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
                                                              IDWriteTextLayout** textLayout) {
    if (!g_origCreateGdiCompatibleTextLayout) return E_FAIL;

    HRESULT hr = g_origCreateGdiCompatibleTextLayout(factory, string, stringLength, textFormat,
                                                     layoutWidth, layoutHeight, pixelsPerDip,
                                                     transform, useGdiNatural, textLayout);
    if (SUCCEEDED(hr) && textLayout && *textLayout) {
        InstallTextLayoutDrawHook(*textLayout);
    }
    return hr;
}

bool InstallFactoryMethodHooks(IDWriteFactory* factory) {
    if (!factory) return false;

    std::lock_guard<std::mutex> lock(g_hookMutex);
    auto* createTextLayoutTarget = reinterpret_cast<LPVOID>(
        GetVtableMethod<CreateTextLayout_t>(factory, kFactoryCreateTextLayoutVtableIndex));
    auto* createGdiTextLayoutTarget = reinterpret_cast<LPVOID>(GetVtableMethod<CreateGdiCompatibleTextLayout_t>(
        factory, kFactoryCreateGdiCompatibleTextLayoutVtableIndex));

    if (!createTextLayoutTarget || !createGdiTextLayoutTarget) {
        PureTypeLog("InstallFactoryMethodHooks: factory vtable lookup failed");
        return false;
    }

    if (!g_origCreateTextLayout) {
        g_targetCreateTextLayout = createTextLayoutTarget;
        MH_STATUS status = MH_CreateHook(g_targetCreateTextLayout,
                                         reinterpret_cast<LPVOID>(&HookedCreateTextLayout),
                                         reinterpret_cast<LPVOID*>(&g_origCreateTextLayout));
        if (status != MH_OK) {
            PureTypeLog("MH_CreateHook(IDWriteFactory::CreateTextLayout) failed: %s",
                          MH_StatusToString(status));
            return false;
        }
        status = MH_EnableHook(g_targetCreateTextLayout);
        if (status != MH_OK && status != MH_ERROR_ENABLED) {
            PureTypeLog("MH_EnableHook(IDWriteFactory::CreateTextLayout) failed: %s",
                          MH_StatusToString(status));
            return false;
        }
    }

    if (!g_origCreateGdiCompatibleTextLayout) {
        g_targetCreateGdiCompatibleTextLayout = createGdiTextLayoutTarget;
        MH_STATUS status = MH_CreateHook(g_targetCreateGdiCompatibleTextLayout,
                                         reinterpret_cast<LPVOID>(&HookedCreateGdiCompatibleTextLayout),
                                         reinterpret_cast<LPVOID*>(&g_origCreateGdiCompatibleTextLayout));
        if (status != MH_OK) {
            PureTypeLog("MH_CreateHook(IDWriteFactory::CreateGdiCompatibleTextLayout) failed: %s",
                          MH_StatusToString(status));
            return false;
        }
        status = MH_EnableHook(g_targetCreateGdiCompatibleTextLayout);
        if (status != MH_OK && status != MH_ERROR_ENABLED) {
            PureTypeLog("MH_EnableHook(IDWriteFactory::CreateGdiCompatibleTextLayout) failed: %s",
                          MH_StatusToString(status));
            return false;
        }
    }

    return true;
}

bool PrimeExistingDWriteObjects() {
    HMODULE hDwrite = GetModuleHandleW(L"dwrite.dll");
    if (!hDwrite) return false;

    auto createFactory =
        reinterpret_cast<DWriteCreateFactory_t>(GetProcAddress(hDwrite, "DWriteCreateFactory"));
    if (!createFactory) return false;

    IDWriteFactory* factory = nullptr;
    HRESULT hr = createFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                               reinterpret_cast<IUnknown**>(&factory));
    if (FAILED(hr) || !factory) {
        PureTypeLog("PrimeExistingDWriteObjects: temporary factory creation failed (0x%08X)",
                      hr);
        return false;
    }

    bool ok = InstallFactoryMethodHooks(factory);

    IDWriteTextFormat* textFormat = nullptr;
    hr = factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f,
                                   L"en-us", &textFormat);
    if (SUCCEEDED(hr) && textFormat) {
        IDWriteTextLayout* textLayout = nullptr;
        hr = factory->CreateTextLayout(L"x", 1, textFormat, 32.0f, 32.0f, &textLayout);
        if (SUCCEEDED(hr) && textLayout) {
            ok = InstallTextLayoutDrawHook(textLayout) && ok;
            textLayout->Release();
        } else {
            PureTypeLog("PrimeExistingDWriteObjects: temporary text layout creation failed (0x%08X)",
                          hr);
            ok = false;
        }
        textFormat->Release();
    } else {
        PureTypeLog("PrimeExistingDWriteObjects: temporary text format creation failed (0x%08X)",
                      hr);
        ok = false;
    }

    factory->Release();
    return ok;
}

HRESULT WINAPI HookedDWriteCreateFactory(DWRITE_FACTORY_TYPE factoryType,
                                         REFIID iid,
                                         IUnknown** factory) {
    (void)iid;

    if (!g_origDWriteCreateFactory) return E_FAIL;

    HRESULT hr = g_origDWriteCreateFactory(factoryType, iid, factory);
    if (FAILED(hr) || !factory || !*factory) return hr;

    IDWriteFactory* baseFactory = nullptr;
    HRESULT qi = (*factory)->QueryInterface(__uuidof(IDWriteFactory),
                                            reinterpret_cast<void**>(&baseFactory));
    if (SUCCEEDED(qi) && baseFactory) {
        InstallFactoryMethodHooks(baseFactory);
        baseFactory->Release();
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE HookedTextLayoutDraw(IDWriteTextLayout* textLayout,
                                               void* clientDrawingContext,
                                               IDWriteTextRenderer* renderer,
                                               FLOAT originX,
                                               FLOAT originY) {
    if (!g_origTextLayoutDraw || !renderer) {
        return g_origTextLayoutDraw
                   ? g_origTextLayoutDraw(textLayout, clientDrawingContext, renderer, originX, originY)
                   : E_FAIL;
    }

    auto* wrappedRenderer = new (std::nothrow) CustomTextRenderer(renderer);
    if (!wrappedRenderer) {
        return g_origTextLayoutDraw(textLayout, clientDrawingContext, renderer, originX, originY);
    }

    HRESULT hr =
        g_origTextLayoutDraw(textLayout, clientDrawingContext, wrappedRenderer, originX, originY);
    wrappedRenderer->Release();
    return hr;
}

}

bool InstallDWriteHooks() {
    HMODULE hDwrite = GetModuleHandleW(L"dwrite.dll");
    if (!hDwrite) {
        hDwrite = LoadLibraryW(L"dwrite.dll");
    }
    if (!hDwrite) {
        PureTypeLog("InstallDWriteHooks: dwrite.dll not available");
        return false;
    }

    g_targetDWriteCreateFactory = reinterpret_cast<LPVOID>(GetProcAddress(hDwrite, "DWriteCreateFactory"));
    if (!g_targetDWriteCreateFactory) {
        PureTypeLog("InstallDWriteHooks: DWriteCreateFactory not found");
        return false;
    }

    MH_STATUS status = MH_CreateHook(g_targetDWriteCreateFactory,
                                     reinterpret_cast<LPVOID>(&HookedDWriteCreateFactory),
                                     reinterpret_cast<LPVOID*>(&g_origDWriteCreateFactory));
    if (status != MH_OK) {
        PureTypeLog("MH_CreateHook(DWriteCreateFactory) failed: %s", MH_StatusToString(status));
        return false;
    }

    if (!PrimeExistingDWriteObjects()) {
        PureTypeLog("InstallDWriteHooks: initial DWrite object priming incomplete");
    }

    return true;
}

void RemoveDWriteHooks() {
    std::lock_guard<std::mutex> lock(g_hookMutex);

    if (g_targetTextLayoutDraw) {
        MH_RemoveHook(g_targetTextLayoutDraw);
        g_targetTextLayoutDraw = nullptr;
        g_origTextLayoutDraw = nullptr;
    }

    if (g_targetCreateGdiCompatibleTextLayout) {
        MH_RemoveHook(g_targetCreateGdiCompatibleTextLayout);
        g_targetCreateGdiCompatibleTextLayout = nullptr;
        g_origCreateGdiCompatibleTextLayout = nullptr;
    }

    if (g_targetCreateTextLayout) {
        MH_RemoveHook(g_targetCreateTextLayout);
        g_targetCreateTextLayout = nullptr;
        g_origCreateTextLayout = nullptr;
    }

    if (g_targetDWriteCreateFactory) {
        MH_RemoveHook(g_targetDWriteCreateFactory);
        g_targetDWriteCreateFactory = nullptr;
        g_origDWriteCreateFactory = nullptr;
    }
}

}
}
