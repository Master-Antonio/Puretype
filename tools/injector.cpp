// =============================================================================
//  injector.cpp — PureType System-Wide Injector (System Tray Application)
//
//  Uses SetWindowsHookEx(WH_CBT) to globally inject PureType.dll into all
//  GUI processes. Runs as an invisible Win32 app with a system tray icon.
//
//  Architecture:
//  1. On startup: grant UWP AppContainer read+execute on the DLL
//  2. "Enable":   LoadLibrary the DLL, resolve PureTypeCBTProc, install
//                 a global WH_CBT hook → Windows auto-loads DLL into all
//                 processes that create windows
//  3. "Disable":  UnhookWindowsHookEx → DLL is unloaded from processes
//  4. "Exit":     Unhook + destroy tray icon + quit message loop
//
//  The tray menu also offers panel type selection (LG WRGB / Samsung QD-OLED)
//  which is persisted to puretype.ini so it survives restarts.
// =============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <Windows.h>
#include <shellapi.h>   // NOTIFYICONDATA, Shell_NotifyIconW
#include <sddl.h>       // ConvertStringSidToSidW
#include <aclapi.h>     // SetNamedSecurityInfoW, SetEntriesInAclW
#include <strsafe.h>
#include <cstdio>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <map>

// Resource ID for the embedded icon (defined in puretype.rc)
#define IDI_PURETYPE_ICON 1

// ── Constants ────────────────────────────────────────────────────────────────

static const wchar_t* const kWindowClass = L"PureTypeTrayWindow";
static const wchar_t* const kWindowTitle = L"PureType";
static const UINT WM_TRAYICON            = WM_APP + 1;

// Context menu item IDs
static const UINT IDM_ENABLE      = 1001;
static const UINT IDM_DISABLE     = 1002;
static const UINT IDM_PANEL_WRGB  = 1010;
static const UINT IDM_PANEL_QDOLED= 1011;
static const UINT IDM_EXIT        = 1099;

// ── Global state ─────────────────────────────────────────────────────────────

static HINSTANCE g_hInstance   = nullptr;
static HWND      g_hWnd       = nullptr;
static HHOOK     g_hCBTHook   = nullptr;
static HMODULE   g_hDll       = nullptr;
static bool      g_hookActive = false;
static NOTIFYICONDATAW g_nid  = {};

// Panel type: 0 = WRGB (LG), 1 = QD_OLED_TRIANGLE (Samsung)
static int g_panelType = 0;

// ── Forward declarations ─────────────────────────────────────────────────────

static std::wstring GetDllPath();
static std::wstring GetIniPath();
static bool GrantUWPPermissions(const std::wstring& filePath);
static bool EnableHook();
static void DisableHook();
static void CreateTrayIcon(HWND hWnd);
static void RemoveTrayIcon();
static void ShowTrayMenu(HWND hWnd);
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// ── INI Read/Write ───────────────────────────────────────────────────────────
// We read/write PanelType directly to puretype.ini using Win32 INI APIs.

static void LoadPanelTypeFromIni() {
    std::wstring iniPath = GetIniPath();
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(L"General", L"PanelType", L"WRGB",
                              buf, ARRAYSIZE(buf), iniPath.c_str());

    std::wstring val(buf);
    // Case-insensitive compare
    for (auto& c : val) c = towupper(c);
    if (val == L"QD_OLED_TRIANGLE") {
        g_panelType = 1;
    } else {
        g_panelType = 0;
    }
}

static void SavePanelTypeToIni(int panelType) {
    std::wstring iniPath = GetIniPath();
    const wchar_t* value = (panelType == 1) ? L"QD_OLED_TRIANGLE" : L"WRGB";
    WritePrivateProfileStringW(L"General", L"PanelType", value, iniPath.c_str());
    g_panelType = panelType;
}

// =============================================================================
//  UWP AppContainer ACL (Security Descriptor Modification)
// =============================================================================
//
//  Problem: UWP apps (Calculator, Settings, new Notepad on Win11) run inside
//  AppContainer sandboxes. When SetWindowsHookEx forces these processes to
//  load our DLL, Windows checks the DLL file's ACL. If the AppContainer SID
//  doesn't have Read+Execute, the load fails with ACCESS_DENIED and the
//  target app may crash.
//
//  Solution: Add an ACE (Access Control Entry) granting READ_CONTROL,
//  GENERIC_READ, and GENERIC_EXECUTE to the well-known SID
//  "S-1-15-2-1" (ALL APPLICATION PACKAGES).
//
//  We also grant to "S-1-15-2-2" (ALL RESTRICTED APPLICATION PACKAGES)
//  for broader Win11 compat.
//
//  This is equivalent to running:
//    icacls PureType.dll /grant "ALL APPLICATION PACKAGES:(RX)"
//    icacls PureType.dll /grant "ALL RESTRICTED APPLICATION PACKAGES:(RX)"
// =============================================================================

static bool GrantACLForSid(const std::wstring& filePath, const wchar_t* sidString) {
    PSID pSid = nullptr;
    if (!ConvertStringSidToSidW(sidString, &pSid)) {
        return false;
    }

    // Build an explicit access entry: Read + Execute for this SID
    EXPLICIT_ACCESSW ea = {};
    ea.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    ea.grfAccessMode        = SET_ACCESS;
    ea.grfInheritance       = NO_INHERITANCE;
    ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType  = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName    = reinterpret_cast<LPWSTR>(pSid);

    // Read the existing DACL from the file
    PACL pOldDacl = nullptr;
    PSECURITY_DESCRIPTOR pSD = nullptr;
    DWORD result = GetNamedSecurityInfoW(
        filePath.c_str(),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr, nullptr,
        &pOldDacl,
        nullptr,
        &pSD);

    if (result != ERROR_SUCCESS) {
        LocalFree(pSid);
        return false;
    }

    // Merge our new ACE into the existing DACL
    PACL pNewDacl = nullptr;
    result = SetEntriesInAclW(1, &ea, pOldDacl, &pNewDacl);
    if (result != ERROR_SUCCESS) {
        LocalFree(pSD);
        LocalFree(pSid);
        return false;
    }

    // Apply the merged DACL back to the file
    result = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(filePath.c_str()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr, nullptr,
        pNewDacl,
        nullptr);

    LocalFree(pNewDacl);
    LocalFree(pSD);
    LocalFree(pSid);

    return (result == ERROR_SUCCESS);
}

static bool GrantUWPPermissions(const std::wstring& filePath) {
    bool ok = true;

    // S-1-15-2-1 = ALL APPLICATION PACKAGES
    if (!GrantACLForSid(filePath, L"S-1-15-2-1")) {
        ok = false;
    }

    // S-1-15-2-2 = ALL RESTRICTED APPLICATION PACKAGES (Win10 1607+)
    GrantACLForSid(filePath, L"S-1-15-2-2");

    // Also grant to the INI file so the DLL can read config in AppContainers
    std::wstring iniPath = GetIniPath();
    GrantACLForSid(iniPath, L"S-1-15-2-1");
    GrantACLForSid(iniPath, L"S-1-15-2-2");

    return ok;
}

// =============================================================================
//  Path Resolution
// =============================================================================

static std::wstring GetExeDir() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path(exePath);
    auto pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        path = path.substr(0, pos + 1);
    }
    return path;
}

static std::wstring GetDllPath() {
    return GetExeDir() + L"PureType.dll";
}

static std::wstring GetIniPath() {
    return GetExeDir() + L"puretype.ini";
}

// =============================================================================
//  Hook Management
// =============================================================================

static bool EnableHook() {
    if (g_hookActive) return true;

    std::wstring dllPath = GetDllPath();

    // Verify DLL exists
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(nullptr,
            L"PureType.dll not found next to Injector.exe.",
            L"PureType Error", MB_OK | MB_ICONERROR);
        return false;
    }

    // Grant UWP permissions before loading
    GrantUWPPermissions(dllPath);

    // Load the DLL into our own process to get the hook proc address
    g_hDll = LoadLibraryW(dllPath.c_str());
    if (!g_hDll) {
        MessageBoxW(nullptr,
            L"Failed to load PureType.dll.\n"
            L"Check that all dependencies are present.",
            L"PureType Error", MB_OK | MB_ICONERROR);
        return false;
    }

    // Resolve the exported CBT hook callback
    auto hookProc = reinterpret_cast<HOOKPROC>(
        GetProcAddress(g_hDll, "PureTypeCBTProc"));
    if (!hookProc) {
        MessageBoxW(nullptr,
            L"PureTypeCBTProc export not found in PureType.dll.\n"
            L"The DLL may be an older version.",
            L"PureType Error", MB_OK | MB_ICONERROR);
        FreeLibrary(g_hDll);
        g_hDll = nullptr;
        return false;
    }

    // Install the global CBT hook
    // dwThreadId = 0 → system-wide (all threads in all processes)
    g_hCBTHook = SetWindowsHookExW(WH_CBT, hookProc, g_hDll, 0);
    if (!g_hCBTHook) {
        DWORD err = GetLastError();
        wchar_t msg[256];
        StringCchPrintfW(msg, 256,
            L"SetWindowsHookEx failed (error %lu).\n"
            L"Try running as Administrator.", err);
        MessageBoxW(nullptr, msg, L"PureType Error", MB_OK | MB_ICONERROR);
        FreeLibrary(g_hDll);
        g_hDll = nullptr;
        return false;
    }

    g_hookActive = true;

    // Update tray icon tooltip
    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"PureType — Active");
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);

    return true;
}

static void DisableHook() {
    if (!g_hookActive) return;

    if (g_hCBTHook) {
        UnhookWindowsHookEx(g_hCBTHook);
        g_hCBTHook = nullptr;
    }

    if (g_hDll) {
        FreeLibrary(g_hDll);
        g_hDll = nullptr;
    }

    g_hookActive = false;

    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"PureType — Disabled");
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// =============================================================================
//  System Tray Icon
// =============================================================================

static void CreateTrayIcon(HWND hWnd) {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd             = hWnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;

    // Load the embedded icon from our resource (puretype.rc → puretype.ico)
    g_nid.hIcon = LoadIconW(g_hInstance,
                             MAKEINTRESOURCEW(IDI_PURETYPE_ICON));
    if (!g_nid.hIcon) {
        // Fallback to system icon if resource not found
        g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }

    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"PureType — Disabled");

    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void ShowTrayMenu(HWND hWnd) {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    // ── Enable / Disable ─────────────────────────────────────────────────
    if (g_hookActive) {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, IDM_ENABLE,  L"PureType Enabled");
        AppendMenuW(hMenu, MF_STRING,              IDM_DISABLE, L"Disable PureType");
    } else {
        AppendMenuW(hMenu, MF_STRING,              IDM_ENABLE,  L"Enable PureType");
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED,  IDM_DISABLE, L"✗ PureType Disabled");
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // ── Panel Type submenu ───────────────────────────────────────────────
    HMENU hPanelMenu = CreatePopupMenu();
    AppendMenuW(hPanelMenu, MF_STRING | (g_panelType == 0 ? MF_CHECKED : 0),
                IDM_PANEL_WRGB,   L"LG WRGB (WOLED)");
    AppendMenuW(hPanelMenu, MF_STRING | (g_panelType == 1 ? MF_CHECKED : 0),
                IDM_PANEL_QDOLED, L"Samsung QD-OLED (Triangular)");

    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hPanelMenu),
                L"Panel Type");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");

    // Required: make the menu disappear when clicking elsewhere
    SetForegroundWindow(hWnd);

    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);

    PostMessage(hWnd, WM_NULL, 0, 0);

    DestroyMenu(hMenu);
}

// =============================================================================
//  Window Procedure
// =============================================================================

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            ShowTrayMenu(hWnd);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_ENABLE:
            EnableHook();
            break;
        case IDM_DISABLE:
            DisableHook();
            break;
        case IDM_PANEL_WRGB:
            SavePanelTypeToIni(0);
            // If hook is active, re-hook to pick up the new panel setting.
            // The DLL re-reads the INI on next DLL_PROCESS_ATTACH.
            if (g_hookActive) {
                DisableHook();
                EnableHook();
            }
            break;
        case IDM_PANEL_QDOLED:
            SavePanelTypeToIni(1);
            if (g_hookActive) {
                DisableHook();
                EnableHook();
            }
            break;
        case IDM_EXIT:
            DisableHook();
            RemoveTrayIcon();
            PostQuitMessage(0);
            break;
        }
        return 0;

    case WM_DESTROY:
        DisableHook();
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

// =============================================================================
//  Entry Point (Win32 GUI — no console window)
// =============================================================================

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    g_hInstance = hInstance;

    // ── Prevent multiple instances ───────────────────────────────────────
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"PureType_Injector_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr,
            L"PureType is already running.",
            L"PureType", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // ── Load saved panel type from INI ───────────────────────────────────
    LoadPanelTypeFromIni();

    // ── Register invisible window class ──────────────────────────────────
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance;
    wc.lpszClassName  = kWindowClass;
    RegisterClassW(&wc);

    // Create a message-only window (invisible, no taskbar entry)
    g_hWnd = CreateWindowExW(
        0, kWindowClass, kWindowTitle,
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInstance, nullptr);

    if (!g_hWnd) {
        MessageBoxW(nullptr, L"Failed to create message window.",
                    L"PureType Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // ── Create system tray icon ──────────────────────────────────────────
    CreateTrayIcon(g_hWnd);

    // ── Auto-enable on startup ───────────────────────────────────────────
    EnableHook();

    // ── Message loop ─────────────────────────────────────────────────────
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Cleanup
    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    return static_cast<int>(msg.wParam);
}
