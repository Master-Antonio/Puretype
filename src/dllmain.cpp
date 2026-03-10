#include <Windows.h>
#include <string>
#include <algorithm>

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

    void LogInit(const std::string& path)
    {
        if (!puretype::Config::Instance().Data().debugEnabled) return;
        fopen_s(&g_logFile, path.c_str(), "w");
    }

    void LogShutdown()
    {
        if (g_logFile)
        {
            fclose(g_logFile);
            g_logFile = nullptr;
        }
    }

    static const wchar_t* const kBlacklist[] = {

        L"csrss.exe",
        L"smss.exe",
        L"lsass.exe",
        L"winlogon.exe",
        L"services.exe",
        L"svchost.exe",
        L"dwm.exe",
        L"wininit.exe",
        L"fontdrvhost.exe",
        L"sihost.exe",
        L"RuntimeBroker.exe",
        L"SearchHost.exe",
        L"StartMenuExperienceHost.exe",
        L"ShellExperienceHost.exe",
        L"conhost.exe",
        L"taskhostw.exe",
        L"dasHost.exe",
        L"WmiPrvSE.exe",
        L"dllhost.exe",
        L"audiodg.exe",
        L"spoolsv.exe",
        L"SecurityHealthService.exe",
        L"MsMpEng.exe",

        L"vgc.exe",
        L"vgtray.exe",
        L"EasyAntiCheat.exe",
        L"EasyAntiCheat_EOS.exe",
        L"BEService.exe",
        L"BEDaisy.exe",
        L"GameGuard.exe",
        L"nProtect.exe",
        L"PnkBstrA.exe",
        L"PnkBstrB.exe",
        L"faceit.exe",
        L"FACEIT_AC.exe",

        L"csgo.exe",
        L"cs2.exe",
        L"valorant.exe",
        L"VALORANT-Win64-Shipping.exe",
        L"r5apex.exe",
        L"FortniteClient-Win64-Shipping.exe",
        L"eldenring.exe",
        L"GTA5.exe",
        L"RDR2.exe",
        L"OverwatchLauncher.exe",
        L"RainbowSix.exe",
        L"destiny2.exe",
        L"Tarkov.exe",

        L"chrome.exe",
        L"msedge.exe",
        L"firefox.exe",
        L"opera.exe",
        L"brave.exe",
        L"vivaldi.exe",

        nullptr
    };

    static bool IsProcessBlacklisted()
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        const wchar_t* exeName = exePath;
        for (const wchar_t* p = exePath; *p; ++p)
        {
            if (*p == L'\\' || *p == L'/') exeName = p + 1;
        }

        for (int i = 0; kBlacklist[i] != nullptr; ++i)
        {
            if (_wcsicmp(exeName, kBlacklist[i]) == 0)
            {
                return true;
            }
        }
        return false;
    }
}

void PureTypeLog(const char* fmt, ...)
{
    if (!g_logFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    fprintf(g_logFile, "\n");
    fflush(g_logFile);
    va_end(args);
}

static std::string GetDllDirectory(HMODULE hModule)
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(hModule, path, MAX_PATH);
    std::string dir(path);
    auto pos = dir.find_last_of("\\/");
    if (pos != std::string::npos) dir = dir.substr(0, pos + 1);
    return dir;
}

extern "C" __declspec(dllexport)
LRESULT CALLBACK PureTypeCBTProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    // Hook proc exists only to force DLL load in target processes.
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        {
            if (IsProcessBlacklisted())
            {
                // Returning FALSE from process attach aborts injection for this process.
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
                std::string logPath = dllDir +
                    puretype::Config::Instance().Data().logFile;
                LogInit(logPath);

                wchar_t hostExe[MAX_PATH] = {};
                GetModuleFileNameW(nullptr, hostExe, MAX_PATH);
                PureTypeLog("=== PureType v%d.%d.%d initializing ===",
                            PURETYPE_VERSION_MAJOR,
                            PURETYPE_VERSION_MINOR,
                            PURETYPE_VERSION_PATCH);
                PureTypeLog("Host process: %ls", hostExe);
                const char* panelName = "RWBG";
                if (puretype::Config::Instance().Data().panelType == puretype::PanelType::QD_OLED_TRIANGLE)
                    panelName =
                        "QD_OLED_TRIANGLE";
                else if (puretype::Config::Instance().Data().panelType == puretype::PanelType::RGWB) panelName = "RGWB";
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

            mhStatus = MH_EnableHook(MH_ALL_HOOKS);
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

            MH_DisableHook(MH_ALL_HOOKS);
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