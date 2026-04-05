#include <Windows.h>
#include <string>
#include <mutex>
#include <atomic>
#include <cstdarg>
#include <cctype>
#include <cstring>

#include "puretype.h"
#include "config.h"
#include "color_math.h"
#include "hooks/dwrite_hooks.h"
#include "hooks/gdi_hooks.h"
#include "rasterizer/ft_rasterizer.h"

#include <MinHook.h>

void PureTypeLog(const char* fmt, ...);

namespace
{
    FILE* g_logFile = nullptr;
    HMODULE g_hSelfModule = nullptr;
    std::mutex g_logMutex; // Thread-safe logging
    std::once_flag g_runtimeInitOnce;
    std::atomic<bool> g_runtimeInitialized{false};
    std::atomic<bool> g_runtimeTeardownStarted{false};

    constexpr char kBlacklistMapName[] = "Local\\PureTypeBlacklistSnapshot_v1";
    constexpr SIZE_T kBlacklistMapBytes = 32 * 1024;

    bool TryGetExeNameLower(char* const outExeName, const size_t outExeNameLen)
    {
        if (!outExeName || outExeNameLen == 0)
        {
            return false;
        }

        char exePathA[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exePathA, MAX_PATH) == 0)
        {
            return false;
        }

        const char* exeName = exePathA;
        for (const char* p = exePathA; *p; ++p)
        {
            if (*p == '\\' || *p == '/') exeName = p + 1;
        }

        size_t i = 0;
        while (exeName[i] != '\0')
        {
            if (i + 1 >= outExeNameLen)
            {
                return false;
            }

            outExeName[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(exeName[i])));
            ++i;
        }

        outExeName[i] = '\0';
        return i > 0;
    }

    bool IsProcessBlacklistedEarly()
    {
        char exeNameLower[MAX_PATH] = {};
        if (!TryGetExeNameLower(exeNameLower, ARRAYSIZE(exeNameLower)))
        {
            // If we cannot identify the host process safely, fail closed.
            return true;
        }

        const HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, kBlacklistMapName);
        if (!hMap)
        {
            // Strict mode: snapshot unavailable means no injection.
            return true;
        }

        const auto* const view = static_cast<const char*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, kBlacklistMapBytes));
        if (!view)
        {
            CloseHandle(hMap);
            // Strict mode: unreadable snapshot means no injection.
            return true;
        }

        bool hasEntries = false;
        bool isBlacklisted = false;

        char token[MAX_PATH] = {};
        size_t tokenLen = 0;

        const auto flushToken = [&]() -> bool
        {
            if (tokenLen == 0)
            {
                return false;
            }

            token[tokenLen] = '\0';
            tokenLen = 0;
            hasEntries = true;
            return std::strcmp(token, exeNameLower) == 0;
        };

        for (SIZE_T i = 0; i < kBlacklistMapBytes; ++i)
        {
            const char ch = view[i];
            if (ch == '\0')
            {
                break;
            }

            if (ch == '\r' || ch == '\n')
            {
                if (flushToken())
                {
                    isBlacklisted = true;
                    break;
                }
                continue;
            }

            if (ch == ' ' || ch == '\t')
            {
                continue;
            }

            if (tokenLen + 1 >= ARRAYSIZE(token))
            {
                // Corrupt snapshot content: fail closed.
                isBlacklisted = true;
                break;
            }

            token[tokenLen++] = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }

        if (!isBlacklisted && flushToken())
        {
            isBlacklisted = true;
        }

        UnmapViewOfFile(view);
        CloseHandle(hMap);

        if (!isBlacklisted && !hasEntries)
        {
            // Empty/invalid snapshot should not silently disable strict gating.
            return true;
        }

        return isBlacklisted;
    }

    const char* PanelTypeToString(const puretype::PanelType panelType)
    {
        switch (panelType)
        {
        case puretype::PanelType::QD_OLED_GEN1:
            return "QD_OLED_GEN1";
        case puretype::PanelType::QD_OLED_GEN3:
            return "QD_OLED_GEN3";
        case puretype::PanelType::QD_OLED_GEN4:
            return "QD_OLED_GEN4";
        case puretype::PanelType::RGWB:
            return "RGWB";
        case puretype::PanelType::RWBG:
        default:
            return "RWBG";
        }
    }

    void LogInit(const std::string& path)
    {
        if (!puretype::Config::Instance().GetData("").debugEnabled) return;
        std::lock_guard lock(g_logMutex);
        fopen_s(&g_logFile, path.c_str(), "w");
    }

    void LogShutdown()
    {
        std::lock_guard lock(g_logMutex);
        if (g_logFile)
        {
            fclose(g_logFile);
            g_logFile = nullptr;
        }
    }


    std::string GetExeName()
    {
        char exePathA[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePathA, MAX_PATH);

        const char* exeName = exePathA;
        for (const char* p = exePathA; *p; ++p)
        {
            if (*p == '\\' || *p == '/') exeName = p + 1;
        }
        return std::string(exeName);
    }

    bool IsProcessBlacklistedFromConfig(const std::string& exeName)
    {
        for (const auto& item : puretype::Config::Instance().GetData("").blacklist)
        {
            if (_stricmp(exeName.c_str(), item.c_str()) == 0)
            {
                return true;
            }
        }
        return false;
    }

    void LogRuntimeConfiguration(const puretype::ConfigData& cfg)
    {
        wchar_t hostExe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, hostExe, MAX_PATH);
        PureTypeLog("=== PureType v%d.%d.%d initializing ===",
                    PURETYPE_VERSION_MAJOR,
                    PURETYPE_VERSION_MINOR,
                    PURETYPE_VERSION_PATCH);
        PureTypeLog("Host process: %ls", hostExe);
        PureTypeLog("Panel type: %s", PanelTypeToString(cfg.panelType));
        PureTypeLog("Filter strength: %.2f", cfg.filterStrength);
        PureTypeLog("Gamma: %.2f", cfg.gamma);
        PureTypeLog("Subpixel hinting: %s", cfg.enableSubpixelHinting ? "ON" : "OFF");
        PureTypeLog("Fractional positioning: %s", cfg.enableFractionalPositioning ? "ON" : "OFF");
        PureTypeLog("LOD thresholds: small %.2f / large %.2f",
                    cfg.lodThresholdSmall,
                    cfg.lodThresholdLarge);
        PureTypeLog("WOLED cross-talk reduction: %.3f", cfg.woledCrossTalkReduction);
        PureTypeLog("Luma contrast strength: %.2f", cfg.lumaContrastStrength);
        PureTypeLog("Toggle toneParityV2Enabled: %s", cfg.toneParityV2Enabled ? "ON" : "OFF");
        PureTypeLog("Toggle dwriteFallbackV2Enabled (heuristics only): %s",
                    cfg.dwriteFallbackV2Enabled ? "ON" : "OFF");
        PureTypeLog("Toggle contrastSamplingCacheEnabled: %s",
                    cfg.contrastSamplingCacheEnabled ? "ON" : "OFF");
        PureTypeLog("Toggle colorGlyphBypassEnabled: %s",
                    cfg.colorGlyphBypassEnabled ? "ON" : "OFF");
        PureTypeLog("DirectWrite atomic transactional DrawGlyphRun safety invariant: ALWAYS ON");
        PureTypeLog("Stem darkening: %s (strength %.2f)",
                    cfg.stemDarkeningEnabled ? "ON" : "OFF",
                    cfg.stemDarkeningStrength);
    }
}

//Thread-safe logging — all hook threads can call this concurrently.
void PureTypeLog(const char* fmt, ...)
{
    std::lock_guard lock(g_logMutex);
    if (!g_logFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    fprintf(g_logFile, "\n");
    fflush(g_logFile);
    va_end(args);
}

static std::string GetDllDirectory(const HMODULE hModule)
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(hModule, path, MAX_PATH);
    std::string dir(path);
    if (const auto pos = dir.find_last_of("\\/"); pos != std::string::npos) dir = dir.substr(0, pos + 1);
    return dir;
}

static bool InitializePureTypeRuntime()
{
    std::call_once(g_runtimeInitOnce, []
    {
        if (!g_hSelfModule)
        {
            return;
        }

        const std::string dllDir = GetDllDirectory(g_hSelfModule);
        const std::string iniPath = dllDir + "puretype.ini";
        const std::string exeName = GetExeName();

        puretype::Config::Instance().LoadFromFile(iniPath, exeName);

        if (IsProcessBlacklistedFromConfig(exeName))
        {
            return;
        }

        const puretype::ConfigData cfg = puretype::Config::Instance().GetData("");

        if (cfg.debugEnabled)
        {
            LogInit(dllDir + cfg.logFile);
            LogRuntimeConfiguration(cfg);
        }

        puretype::initColorMathLUTs(cfg.gammaMode == puretype::GammaMode::OLED);

        bool mhReady = false;
        bool rasterizerReady = false;
        bool dwriteHooksInstalled = false;
        bool gdiHooksInstalled = false;

        MH_STATUS mhStatus = MH_Initialize();
        if (mhStatus != MH_OK)
        {
            PureTypeLog("ERROR: MH_Initialize failed: %s", MH_StatusToString(mhStatus));
            return;
        }
        mhReady = true;

        if (!puretype::FTRasterizer::Instance().Initialize())
        {
            PureTypeLog("ERROR: FreeType initialization failed");
            MH_Uninitialize();
            return;
        }
        rasterizerReady = true;

        if (!puretype::hooks::InstallDWriteHooks(false))
        {
            PureTypeLog("ERROR: Failed to install DirectWrite hooks");
            puretype::FTRasterizer::Instance().Shutdown();
            MH_Uninitialize();
            return;
        }
        dwriteHooksInstalled = true;

        if (!puretype::hooks::InstallGDIHooks())
        {
            PureTypeLog("ERROR: Failed to install GDI hooks");
            puretype::hooks::RemoveDWriteHooks();
            puretype::FTRasterizer::Instance().Shutdown();
            MH_Uninitialize();
            return;
        }
        gdiHooksInstalled = true;

        mhStatus = MH_EnableHook(nullptr);
        if (mhStatus != MH_OK)
        {
            PureTypeLog("ERROR: MH_EnableHook(ALL) failed: %s", MH_StatusToString(mhStatus));
            if (gdiHooksInstalled) puretype::hooks::RemoveGDIHooks();
            if (dwriteHooksInstalled) puretype::hooks::RemoveDWriteHooks();
            if (rasterizerReady) puretype::FTRasterizer::Instance().Shutdown();
            if (mhReady) MH_Uninitialize();
            return;
        }

        g_runtimeInitialized.store(true, std::memory_order_release);
        PureTypeLog("=== PureType initialization complete ===");
    });

    return g_runtimeInitialized.load(std::memory_order_acquire);
}

static void ShutdownPureTypeRuntime()
{
    bool expected = false;
    if (!g_runtimeTeardownStarted.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        return;
    }

    if (!g_runtimeInitialized.load(std::memory_order_acquire))
    {
        LogShutdown();
        return;
    }

    PureTypeLog("=== PureType shutting down ===");

    MH_DisableHook(nullptr);
    PureTypeLog("Hooks disabled (no new entries)");

    constexpr int kMaxWaitMs = 100;
    int waited = 0;
    while (puretype::g_activeHookCount.load(std::memory_order_acquire) > 0
        && waited < kMaxWaitMs)
    {
        constexpr int kPollIntervalMs = 2;
        Sleep(kPollIntervalMs);
        waited += kPollIntervalMs;
    }

    if (waited >= kMaxWaitMs)
    {
        PureTypeLog("WARNING: Timed out waiting for %d active hooks to complete",
                    puretype::g_activeHookCount.load());
    }
    else
    {
        PureTypeLog("All active hooks completed (waited %dms)", waited);
    }

    const int remainingHooks = puretype::g_activeHookCount.load(std::memory_order_acquire);
    if (remainingHooks > 0)
    {
        PureTypeLog("WARNING: Skipping hook/resource teardown (%d hooks still active)",
                    remainingHooks);
        PureTypeLog("=== PureType shutdown complete (deferred teardown) ===");
        LogShutdown();
        return;
    }

    puretype::hooks::RemoveDWriteHooks();
    puretype::hooks::RemoveGDIHooks();
    PureTypeLog("Hooks removed");

    MH_Uninitialize();
    PureTypeLog("MinHook uninitialized");

    puretype::FTRasterizer::Instance().Shutdown();
    PureTypeLog("FreeType shutdown");

    g_runtimeInitialized.store(false, std::memory_order_release);
    PureTypeLog("=== PureType shutdown complete ===");
    LogShutdown();
}

extern "C" __declspec(dllexport)
LRESULT CALLBACK PureTypeCBTProc(const int nCode, const WPARAM wParam, const LPARAM lParam)
{
    InitializePureTypeRuntime();
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

BOOL APIENTRY DllMain(const HMODULE hModule, const DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        g_hSelfModule = hModule;
        if (IsProcessBlacklistedEarly())
        {
            return FALSE;
        }
        break;

    case DLL_PROCESS_DETACH:
        ShutdownPureTypeRuntime();
        break;

    default:
        break;
    }

    return TRUE;
}
