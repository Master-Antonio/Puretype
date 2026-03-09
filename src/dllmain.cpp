// =============================================================================
//  dllmain.cpp — PureType DLL Entry Point
//
//  Handles:
//  1. Process blacklist check (anti-cheat, system critical, browsers)
//  2. Exported CBT hook callback for SetWindowsHookEx system-wide injection
//  3. Full rendering pipeline initialization (FreeType, MinHook, hooks)
// =============================================================================

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

namespace {

FILE* g_logFile = nullptr;
HMODULE g_hSelfModule = nullptr;

void LogInit(const std::string& path) {
    if (!puretype::Config::Instance().Data().debugEnabled) return;
    fopen_s(&g_logFile, path.c_str(), "w");
}

void LogShutdown() {
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

// ── Process Blacklist ────────────────────────────────────────────────────────
// If the host process is in this list, we return FALSE from DllMain to abort
// loading. This prevents crashes in system-critical processes, anti-cheat
// conflicts, and browser sandbox violations.
//
// Categories:
//   SYSTEM   — Kernel/session managers, LSASS, DWM — hooking these can BSOD
//   ANTICHEAT — Vanguard, EAC, BattlEye — they detect injected DLLs
//   GAMES    — Major titles with integrity checks
//   BROWSERS — Sandboxed renderer processes reject unsigned DLLs
// ─────────────────────────────────────────────────────────────────────────────

static const wchar_t* const kBlacklist[] = {
    // ── System Critical ──
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

    // ── Anti-Cheat ──
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

    // ── Games (integrity-checked) ──
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

    // ── Browsers (sandboxed renderers) ──
    L"chrome.exe",
    L"msedge.exe",
    L"firefox.exe",
    L"opera.exe",
    L"brave.exe",
    L"vivaldi.exe",

    // ── Sentinel (must not be last due to null terminator) ──
    nullptr
};

static bool IsProcessBlacklisted() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // Extract just the filename
    const wchar_t* exeName = exePath;
    for (const wchar_t* p = exePath; *p; ++p) {
        if (*p == L'\\' || *p == L'/') exeName = p + 1;
    }

    for (int i = 0; kBlacklist[i] != nullptr; ++i) {
        if (_wcsicmp(exeName, kBlacklist[i]) == 0) {
            return true;
        }
    }
    return false;
}

} // anonymous namespace

// ── Logging (used by other TUs via extern declaration) ───────────────────────

void PureTypeLog(const char* fmt, ...) {
    if (!g_logFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    fprintf(g_logFile, "\n");
    fflush(g_logFile);
    va_end(args);
}

static std::string GetDllDirectory(HMODULE hModule) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(hModule, path, MAX_PATH);
    std::string dir(path);
    auto pos = dir.find_last_of("\\/");
    if (pos != std::string::npos) dir = dir.substr(0, pos + 1);
    return dir;
}

// =============================================================================
//  Exported CBT Hook Callback
//
//  This is the function that SetWindowsHookEx(WH_CBT, ...) points at.
//  When the injector installs a global WH_CBT hook referencing this proc,
//  Windows automatically loads PureType.dll into every process that
//  receives a CBT notification (i.e., every process creating a window).
//
//  The proc itself does nothing — it just forwards to the next hook.
//  All the real work happens in DLL_PROCESS_ATTACH above, which runs
//  when Windows loads the DLL into the target process.
// =============================================================================

extern "C" __declspec(dllexport)
LRESULT CALLBACK PureTypeCBTProc(int nCode, WPARAM wParam, LPARAM lParam) {
    // Always forward to the next hook in the chain.
    // We don't filter any CBT events — we only care that loading
    // this DLL triggered our DllMain initialization.
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// =============================================================================
//  DllMain — Process Attach / Detach
// =============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {

    case DLL_PROCESS_ATTACH: {
        // ── STEP 0: Blacklist check (MUST be first) ──────────────────────
        // If this process is blacklisted, abort loading immediately.
        // Returning FALSE tells Windows to unload the DLL.
        if (IsProcessBlacklisted()) {
            return FALSE;
        }

        DisableThreadLibraryCalls(hModule);
        g_hSelfModule = hModule;

        // ── STEP 1: Load config ──────────────────────────────────────────
        std::string dllDir = GetDllDirectory(hModule);
        std::string iniPath = dllDir + "puretype.ini";
        puretype::Config::Instance().LoadFromFile(iniPath);

        // ── STEP 2: Build sRGB LUTs ─────────────────────────────────────
        puretype::initColorMathLUTs();

        // ── STEP 3: Debug logging ────────────────────────────────────────
        if (puretype::Config::Instance().Data().debugEnabled) {
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
            PureTypeLog("Panel type: %s",
                puretype::Config::Instance().Data().panelType ==
                    puretype::PanelType::WRGB ? "WRGB" : "QD_OLED_TRIANGLE");
            PureTypeLog("Filter strength: %.2f",
                puretype::Config::Instance().Data().filterStrength);
            PureTypeLog("Gamma: %.2f",
                puretype::Config::Instance().Data().gamma);
            PureTypeLog("Stem darkening: %s (strength %.2f)",
                puretype::Config::Instance().Data().stemDarkeningEnabled ? "ON" : "OFF",
                puretype::Config::Instance().Data().stemDarkeningStrength);
        }

        // ── STEP 4: MinHook ──────────────────────────────────────────────
        MH_STATUS mhStatus = MH_Initialize();
        if (mhStatus != MH_OK) {
            PureTypeLog("ERROR: MH_Initialize failed: %s",
                          MH_StatusToString(mhStatus));
            return FALSE;
        }
        PureTypeLog("MinHook initialized successfully");

        // ── STEP 5: FreeType ─────────────────────────────────────────────
        if (!puretype::FTRasterizer::Instance().Initialize()) {
            PureTypeLog("ERROR: FreeType initialization failed");
            MH_Uninitialize();
            return FALSE;
        }
        PureTypeLog("FreeType rasterizer initialized");

        // ── STEP 6: Install rendering hooks ──────────────────────────────
        if (!puretype::hooks::InstallDWriteHooks()) {
            PureTypeLog("WARNING: Failed to install DirectWrite hooks");
        } else {
            PureTypeLog("DirectWrite hooks installed");
        }

        if (!puretype::hooks::InstallGDIHooks()) {
            PureTypeLog("WARNING: Failed to install GDI hooks");
        } else {
            PureTypeLog("GDI hooks installed");
        }

        // ── STEP 7: Enable all hooks ────────────────────────────────────
        mhStatus = MH_EnableHook(MH_ALL_HOOKS);
        if (mhStatus != MH_OK) {
            PureTypeLog("ERROR: MH_EnableHook(ALL) failed: %s",
                          MH_StatusToString(mhStatus));
        } else {
            PureTypeLog("All hooks enabled");
        }

        PureTypeLog("=== PureType initialization complete ===");
        break;
    }

    case DLL_PROCESS_DETACH: {
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
