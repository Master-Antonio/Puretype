#include <Windows.h>
#include <string>
#include <mutex>

#include "puretype.h"
#include "config.h"
#include "color_math.h"
#include "hooks/dwrite_hooks.h"
#include "hooks/gdi_hooks.h"
#include "rasterizer/ft_rasterizer.h"

#include <MinHook.h>

namespace
{
    FILE* g_logFile = nullptr;
    HMODULE g_hSelfModule = nullptr;
    std::mutex g_logMutex; // Thread-safe logging

    void LogInit(const std::string& path)
    {
        if (!puretype::Config::Instance().Data().debugEnabled) return;
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


    bool IsProcessBlacklisted()
    {
        char exePathA[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePathA, MAX_PATH);

        const char* exeName = exePathA;
        for (const char* p = exePathA; *p; ++p)
        {
            if (*p == '\\' || *p == '/') exeName = p + 1;
        }

        for (const auto& item : puretype::Config::Instance().Data().blacklist)
        {
            if (_stricmp(exeName, item.c_str()) == 0)
            {
                return true;
            }
        }
        return false;
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

extern "C" __declspec(dllexport)
LRESULT CALLBACK PureTypeCBTProc(const int nCode, const WPARAM wParam, const LPARAM lParam)
{
    // Hook proc exists only to force DLL load in target processes.
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

BOOL APIENTRY DllMain(const HMODULE hModule, const DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        {
            if (IsProcessBlacklisted())
            {
                return FALSE;
            }

            DisableThreadLibraryCalls(hModule);
            g_hSelfModule = hModule;

            std::string dllDir = GetDllDirectory(hModule);
            std::string iniPath = dllDir + "puretype.ini";
            puretype::Config::Instance().LoadFromFile(iniPath);

            puretype::initColorMathLUTs();

            if (puretype::Config::Instance().Data().debugEnabled)
            {
                const std::string logPath = dllDir +
                    puretype::Config::Instance().Data().logFile;
                LogInit(logPath);

                wchar_t hostExe[MAX_PATH] = {};
                GetModuleFileNameW(nullptr, hostExe, MAX_PATH);
                PureTypeLog("=== PureType v%d.%d.%d initializing ===",
                            PURETYPE_VERSION_MAJOR,
                            PURETYPE_VERSION_MINOR,
                            PURETYPE_VERSION_PATCH);
                PureTypeLog("Host process: %ls", hostExe);
                auto panelName = "RWBG";
                if (puretype::Config::Instance().Data().panelType == puretype::PanelType::QD_OLED_GEN1)
                    panelName = "QD_OLED_GEN1";
                else if (puretype::Config::Instance().Data().panelType == puretype::PanelType::QD_OLED_GEN3)
                    panelName = "QD_OLED_GEN3";
                else if (puretype::Config::Instance().Data().panelType == puretype::PanelType::QD_OLED_GEN4)
                    panelName = "QD_OLED_GEN4";
                else if (puretype::Config::Instance().Data().panelType == puretype::PanelType::RGWB)
                    panelName = "RGWB";
                PureTypeLog("Panel type: %s", panelName);
                PureTypeLog("Filter strength: %.2f",
                            puretype::Config::Instance().Data().filterStrength);
                PureTypeLog("Gamma: %.2f",
                            puretype::Config::Instance().Data().gamma);
                PureTypeLog("Subpixel hinting: %s",
                            puretype::Config::Instance().Data().enableSubpixelHinting ? "ON" : "OFF");
                PureTypeLog("Fractional positioning: %s",
                            puretype::Config::Instance().Data().enableFractionalPositioning ? "ON" : "OFF");
                PureTypeLog("LOD thresholds: small %.2f / large %.2f",
                            puretype::Config::Instance().Data().lodThresholdSmall,
                            puretype::Config::Instance().Data().lodThresholdLarge);
                PureTypeLog("WOLED cross-talk reduction: %.3f",
                            puretype::Config::Instance().Data().woledCrossTalkReduction);
                PureTypeLog("Luma contrast strength: %.2f",
                            puretype::Config::Instance().Data().lumaContrastStrength);
                PureTypeLog("Stem darkening: %s (strength %.2f)",
                            puretype::Config::Instance().Data().stemDarkeningEnabled ? "ON" : "OFF",
                            puretype::Config::Instance().Data().stemDarkeningStrength);
            }

            MH_STATUS mhStatus = MH_Initialize();
            if (mhStatus != MH_OK)
            {
                PureTypeLog("ERROR: MH_Initialize failed: %s",
                            MH_StatusToString(mhStatus));
                return FALSE;
            }
            PureTypeLog("MinHook initialized successfully");

            if (!puretype::FTRasterizer::Instance().Initialize())
            {
                PureTypeLog("ERROR: FreeType initialization failed");
                MH_Uninitialize();
                return FALSE;
            }
            PureTypeLog("FreeType rasterizer initialized");

            if (!puretype::hooks::InstallDWriteHooks())
            {
                PureTypeLog("WARNING: Failed to install DirectWrite hooks");
            }
            else
            {
                PureTypeLog("DirectWrite hooks installed");
            }

            if (!puretype::hooks::InstallGDIHooks())
            {
                PureTypeLog("WARNING: Failed to install GDI hooks");
            }
            else
            {
                PureTypeLog("GDI hooks installed");
            }

            mhStatus = MH_EnableHook(nullptr);
            if (mhStatus != MH_OK)
            {
                PureTypeLog("ERROR: MH_EnableHook(ALL) failed: %s",
                            MH_StatusToString(mhStatus));
            }
            else
            {
                PureTypeLog("All hooks enabled");
            }

            PureTypeLog("=== PureType initialization complete ===");
            break;
        }

    case DLL_PROCESS_DETACH:
        {
            PureTypeLog("=== PureType shutting down ===");

            // Safe shutdown with reference-counted spin-wait.
            //
            // Step 1: Disable all hooks to prevent NEW entries into our code.
            MH_DisableHook(nullptr);
            PureTypeLog("Hooks disabled (no new entries)");

            // Step 2: Spin-wait until all in-flight hook calls complete.
            // g_activeHookCount tracks how many threads are currently inside
            // our hook functions. We wait up to 100ms for them to finish.
            {
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
            }

            // Step 3: Now safe to remove hooks and destroy resources.
            puretype::hooks::RemoveDWriteHooks();
            puretype::hooks::RemoveGDIHooks();
            PureTypeLog("Hooks removed");

            MH_Uninitialize();
            PureTypeLog("MinHook uninitialized");

            puretype::FTRasterizer::Instance().Shutdown();
            PureTypeLog("FreeType shutdown");

            PureTypeLog("=== PureType shutdown complete ===");
            LogShutdown();
            break;
        }

    default:
        break;
    }

    return TRUE;
}