#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <Windows.h>
#include <shellapi.h>
#include <sddl.h>
#include <aclapi.h>
#include <strsafe.h>
#include <cstdio>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <map>

#define IDI_PURETYPE_ICON 1

static const wchar_t* const kWindowClass = L"PureTypeTrayWindow";
static const wchar_t* const kWindowTitle = L"PureType";
static const UINT WM_TRAYICON = WM_APP + 1;

// Sent by PuretypeUI after saving the INI — tray reloads the hook immediately.
// PuretypeUI finds the tray window via FindWindow(kWindowClass, kWindowTitle).
static const UINT WM_PURETYPE_RELOAD = WM_APP + 2;

static const UINT IDM_ENABLE = 1001;
static const UINT IDM_DISABLE = 1002;
static const UINT IDM_PANEL_QDOLED_GEN1 = 1010;
static const UINT IDM_PANEL_RWBG = 1011;
static const UINT IDM_PANEL_RGWB = 1012;
static const UINT IDM_PANEL_QDOLED_GEN3 = 1013;
static const UINT IDM_PANEL_QDOLED_GEN4 = 1014;
static const UINT IDM_SETTINGS = 1005;
static const UINT IDM_EXIT = 1099;

static HINSTANCE g_hInstance = nullptr;
static HWND g_hWnd = nullptr;
static HHOOK g_hCBTHook = nullptr;
static HMODULE g_hDll = nullptr;
static bool g_hookActive = false;
static NOTIFYICONDATAW g_nid = {};

static int g_panelType = 0;
static bool g_clearTypeWasEnabled = false;

static bool SetClearTypeState(bool enable);
static bool GetClearTypeState();

static std::wstring GetDllPath();
static std::wstring GetIniPath();
static bool GrantUWPPermissions(const std::wstring& filePath);
static bool EnableHook();
static void DisableHook();
static void CreateTrayIcon(HWND hWnd);
static void RemoveTrayIcon();
static void ShowTrayMenu(HWND hWnd);
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

static void LoadPanelTypeFromIni()
{
    std::wstring iniPath = GetIniPath();
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(L"General", L"PanelType", L"RWBG",
                             buf, ARRAYSIZE(buf), iniPath.c_str());

    std::wstring val(buf);

    for (auto& c : val) c = towupper(c);
    if (val == L"QD_OLED_TRIANGLE" || val == L"QD_OLED_GEN1")
    {
        g_panelType = 2;
    }
    else if (val == L"QD_OLED_GEN3")
    {
        g_panelType = 3;
    }
    else if (val == L"QD_OLED_GEN4")
    {
        g_panelType = 4;
    }
    else if (val == L"RGWB")
    {
        g_panelType = 1;
    }
    else
    {
        g_panelType = 0;
    }
}

static bool GetClearTypeState()
{
    BOOL isSmoothingEnabled = FALSE;
    BOOL isClearTypeEnabled = FALSE;

    SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0, &isSmoothingEnabled, 0);
    if (isSmoothingEnabled)
    {
        SystemParametersInfoW(SPI_GETFONTSMOOTHINGTYPE, 0, &isClearTypeEnabled, 0);
        return isClearTypeEnabled == FE_FONTSMOOTHINGCLEARTYPE;
    }
    return false;
}

static bool SetClearTypeState(bool enable)
{
    BOOL result = FALSE;
    if (enable)
    {
        // Enable font smoothing and set it to ClearType
        SystemParametersInfoW(SPI_SETFONTSMOOTHING, TRUE, 0, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
        result = SystemParametersInfoW(SPI_SETFONTSMOOTHINGTYPE, 0, (PVOID)FE_FONTSMOOTHINGCLEARTYPE,
                                       SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    }
    else
    {
        // Set font smoothing to Standard (Grayscale) or disable smoothing completely
        // Here we just switch from ClearType to Standard anti-aliasing to preserve basic smoothing
        SystemParametersInfoW(SPI_SETFONTSMOOTHING, TRUE, 0, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
        result = SystemParametersInfoW(SPI_SETFONTSMOOTHINGTYPE, 0, (PVOID)FE_FONTSMOOTHINGSTANDARD,
                                       SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    }
    return result != FALSE;
}

static void SavePanelTypeToIni(int panelType)
{
    std::wstring iniPath = GetIniPath();
    const wchar_t* value = L"RWBG";
    if (panelType == 1) value = L"RGWB";
    else if (panelType == 2) value = L"QD_OLED_GEN1";
    else if (panelType == 3) value = L"QD_OLED_GEN3";
    else if (panelType == 4) value = L"QD_OLED_GEN4";

    WritePrivateProfileStringW(L"General", L"PanelType", value, iniPath.c_str());
    g_panelType = panelType;
}

static bool GrantACLForSid(const std::wstring& filePath, const wchar_t* sidString)
{
    PSID pSid = nullptr;
    if (!ConvertStringSidToSidW(sidString, &pSid))
    {
        return false;
    }

    EXPLICIT_ACCESSW ea = {};
    // AppContainer processes need RX on DLL/INI for global hook injection to work.
    ea.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(pSid);

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

    if (result != ERROR_SUCCESS)
    {
        LocalFree(pSid);
        return false;
    }

    PACL pNewDacl = nullptr;
    result = SetEntriesInAclW(1, &ea, pOldDacl, &pNewDacl);
    if (result != ERROR_SUCCESS)
    {
        LocalFree(pSD);
        LocalFree(pSid);
        return false;
    }

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

static bool GrantUWPPermissions(const std::wstring& filePath)
{
    bool ok = true;

    // ALL APPLICATION PACKAGES
    if (!GrantACLForSid(filePath, L"S-1-15-2-1"))
    {
        ok = false;
    }

    // ALL RESTRICTED APPLICATION PACKAGES
    GrantACLForSid(filePath, L"S-1-15-2-2");

    std::wstring iniPath = GetIniPath();
    GrantACLForSid(iniPath, L"S-1-15-2-1");
    GrantACLForSid(iniPath, L"S-1-15-2-2");

    return ok;
}

static std::wstring GetExeDir()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path(exePath);
    auto pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
    {
        path = path.substr(0, pos + 1);
    }
    return path;
}

static std::wstring GetDllPath()
{
    return GetExeDir() + L"PureType.dll";
}

static std::wstring GetIniPath()
{
    return GetExeDir() + L"puretype.ini";
}

static bool EnableHook()
{
    if (g_hookActive) return true;

    std::wstring dllPath = GetDllPath();

    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        MessageBoxW(nullptr,
                    L"PureType.dll not found next to puretype.exe.",
                    L"PureType Error", MB_OK | MB_ICONERROR);
        return false;
    }

    GrantUWPPermissions(dllPath);

    g_hDll = LoadLibraryW(dllPath.c_str());
    if (!g_hDll)
    {
        MessageBoxW(nullptr,
                    L"Failed to load PureType.dll.\n"
                    L"Check that all dependencies are present.",
                    L"PureType Error", MB_OK | MB_ICONERROR);
        return false;
    }

    auto hookProc = reinterpret_cast<HOOKPROC>(
        GetProcAddress(g_hDll, "PureTypeCBTProc"));
    if (!hookProc)
    {
        MessageBoxW(nullptr,
                    L"PureTypeCBTProc export not found in PureType.dll.\n"
                    L"The DLL may be an older version.",
                    L"PureType Error", MB_OK | MB_ICONERROR);
        FreeLibrary(g_hDll);
        g_hDll = nullptr;
        return false;
    }

    // threadId=0 installs a desktop-wide hook.
    g_hCBTHook = SetWindowsHookExW(WH_CBT, hookProc, g_hDll, 0);
    if (!g_hCBTHook)
    {
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

    // Disable Windows ClearType
    g_clearTypeWasEnabled = GetClearTypeState();
    if (g_clearTypeWasEnabled)
    {
        SetClearTypeState(false);
    }

    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"PureType - Active");
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);

    return true;
}

static void DisableHook()
{
    if (!g_hookActive) return;

    if (g_hCBTHook)
    {
        UnhookWindowsHookEx(g_hCBTHook);
        g_hCBTHook = nullptr;
    }

    if (g_hDll)
    {
        FreeLibrary(g_hDll);
        g_hDll = nullptr;
    }

    g_hookActive = false;

    // Restore Windows ClearType if it was enabled before
    if (g_clearTypeWasEnabled)
    {
        SetClearTypeState(true);
    }

    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"PureType - Disabled");
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void CreateTrayIcon(HWND hWnd)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;

    g_nid.hIcon = LoadIconW(g_hInstance,
                            MAKEINTRESOURCEW(IDI_PURETYPE_ICON));
    if (!g_nid.hIcon)
    {
        g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }

    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"PureType - Disabled");

    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void ShowTrayMenu(HWND hWnd)
{
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    if (g_hookActive)
    {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, IDM_ENABLE, L"PureType Enabled");
        AppendMenuW(hMenu, MF_STRING, IDM_DISABLE, L"Disable PureType");
    }
    else
    {
        AppendMenuW(hMenu, MF_STRING, IDM_ENABLE, L"Enable PureType");
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, IDM_DISABLE, L"PureType Disabled");
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    HMENU hPanelMenu = CreatePopupMenu();
    AppendMenuW(hPanelMenu, MF_STRING | (g_panelType == 0 ? MF_CHECKED : 0),
                IDM_PANEL_RWBG, L"LG WOLED (RWBG)");
    AppendMenuW(hPanelMenu, MF_STRING | (g_panelType == 1 ? MF_CHECKED : 0),
                IDM_PANEL_RGWB, L"LG WOLED (RGWB)");
    AppendMenuW(hPanelMenu, MF_STRING | (g_panelType == 2 ? MF_CHECKED : 0),
                IDM_PANEL_QDOLED_GEN1, L"Samsung QD-OLED Gen 1-2");
    AppendMenuW(hPanelMenu, MF_STRING | (g_panelType == 3 ? MF_CHECKED : 0),
                IDM_PANEL_QDOLED_GEN3, L"Samsung QD-OLED Gen 3");
    AppendMenuW(hPanelMenu, MF_STRING | (g_panelType == 4 ? MF_CHECKED : 0),
                IDM_PANEL_QDOLED_GEN4, L"Samsung QD-OLED Gen 4");

    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hPanelMenu),
                L"Panel Type");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_SETTINGS, L"Settings / UI");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");

    SetForegroundWindow(hWnd);

    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);

    // Standard tray-menu quirk: force menu dismissal on outside click.
    PostMessage(hWnd, WM_NULL, 0, 0);

    DestroyMenu(hMenu);
}

// Mutex name must match App.xaml.cs in PuretypeUI
static constexpr wchar_t kUIMutex[] = L"PureTypeUI_Instance";

static void LaunchSettingsUI(HWND hWnd)
{
    // If already running, bring it to front instead of spawning a second instance.
    HANDLE hMutex = OpenMutexW(SYNCHRONIZE, FALSE, kUIMutex);
    if (hMutex)
    {
        CloseHandle(hMutex);
        HWND hUI = FindWindowW(nullptr, L"Puretype");
        if (!hUI) hUI = FindWindowW(nullptr, L"Puretype Configuration");
        if (hUI)
        {
            if (IsIconic(hUI)) ShowWindow(hUI, SW_RESTORE);
            SetForegroundWindow(hUI);
        }
        return;
    }

    std::wstring exeDir = GetExeDir();
    std::wstring uiPath = exeDir + L"PuretypeUI.exe";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(uiPath.c_str(), nullptr, nullptr, nullptr,
                        FALSE, 0, nullptr, exeDir.c_str(), &si, &pi))
    {
        wchar_t msg[256];
        StringCchPrintfW(msg, 256,
                         L"Failed to launch PuretypeUI.exe (Error %lu).\nPath: %s",
                         GetLastError(), uiPath.c_str());
        MessageBoxW(hWnd, msg, L"PureType Error", MB_OK | MB_ICONERROR);
        return;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PURETYPE_RELOAD:
        // Re-read INI and restart hook so new processes get the updated config.
        // Already-running processes need to be closed and reopened.
        if (g_hookActive)
        {
            DisableHook();
            EnableHook();
        }
        LoadPanelTypeFromIni();
        return 0;

    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)
        {
            ShowTrayMenu(hWnd);
        }
        else if (LOWORD(lParam) == WM_LBUTTONDBLCLK)
        {
            LaunchSettingsUI(hWnd);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDM_ENABLE:
            EnableHook();
            break;
        case IDM_DISABLE:
            DisableHook();
            break;
        case IDM_PANEL_RWBG:
            SavePanelTypeToIni(0);
            if (g_hookActive)
            {
                DisableHook();
                EnableHook();
            }
            break;
        case IDM_PANEL_RGWB:
            SavePanelTypeToIni(1);
            if (g_hookActive)
            {
                DisableHook();
                EnableHook();
            }
            break;
        case IDM_PANEL_QDOLED_GEN1:
            SavePanelTypeToIni(2);
            if (g_hookActive)
            {
                DisableHook();
                EnableHook();
            }
            break;
        case IDM_PANEL_QDOLED_GEN3:
            SavePanelTypeToIni(3);
            if (g_hookActive)
            {
                DisableHook();
                EnableHook();
            }
            break;
        case IDM_PANEL_QDOLED_GEN4:
            SavePanelTypeToIni(4);
            if (g_hookActive)
            {
                DisableHook();
                EnableHook();
            }
            break;
        case IDM_SETTINGS:
            LaunchSettingsUI(hWnd);
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

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    g_hInstance = hInstance;

    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"PureType_Executable_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(nullptr,
                    L"PureType is already running.",
                    L"PureType", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    LoadPanelTypeFromIni();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWindowClass;
    RegisterClassW(&wc);

    // Message-only window keeps the app off taskbar/Alt-Tab.
    g_hWnd = CreateWindowExW(
        0, kWindowClass, kWindowTitle,
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInstance, nullptr);

    if (!g_hWnd)
    {
        MessageBoxW(nullptr, L"Failed to create message window.",
                    L"PureType Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    CreateTrayIcon(g_hWnd);

    EnableHook();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hMutex)
    {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    return static_cast<int>(msg.wParam);
}