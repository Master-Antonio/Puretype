using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Globalization;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using Microsoft.Win32;

namespace PuretypeUI
{
    public partial class MainWindow : Window
    {
        // ── Win32 interop — tray reload signal ───────────────────────────────
        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr FindWindow(string lpClassName, string lpWindowName);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

        // WM_APP+2 — must match WM_PURETYPE_RELOAD in injector.cpp
        private const uint WM_PURETYPE_RELOAD = 0x8002u;

        private static void SignalTrayReload()
        {
            IntPtr hTray = FindWindow("PureTypeTrayWindow", "PureType");
            if (hTray != IntPtr.Zero)
                PostMessage(hTray, WM_PURETYPE_RELOAD, IntPtr.Zero, IntPtr.Zero);
        }

        // ── DLL import ────────────────────────────────────────────────────────
        [DllImport("PureType.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        private static extern bool GeneratePreview(
            string text,
            string fontPath,
            uint   fontSize,
            float  filterStrength,
            float  gamma,
            int    gammaMode,
            float  oledGammaOutput,
            float  lumaContrastStrength,
            float  woledCrossTalkReduction,
            bool   enableSubpixelHinting,
            bool   enableFractionalPositioning,
            bool   stemDarkeningEnabled,
            float  stemDarkeningStrength,
            int    panelType,
            bool   useMeasuredContrast,
            int    width,
            int    height,
            IntPtr pBuffer);

        // ── Preview buffer ────────────────────────────────────────────────────
        private const int PreviewRenderWidth = 1200;
        private const int PreviewRenderHeight = 400;
        private IntPtr          _previewBuffer = IntPtr.Zero;
        private WriteableBitmap? _wb;
        private bool            _dllAvailable  = true;

        // ── INI ───────────────────────────────────────────────────────────────
        private string       _iniPath  = string.Empty;
        private List<string> _iniLines = new();

        // ── Context Switching ─────────────────────────────────────────────
        private enum ContextType
        {
            Global,
            Monitor,
            App
        }

        private ContextType _currentContext = ContextType.Global;
        private string _currentContextName = string.Empty;
        private string _currentSection = "general";
        private string _contextTitle = "EDITING GLOBAL DEFAULTS";
        private bool _isUpdatingContext = false;
        private bool _isUpdatingAppsCombo = false;
        private List<string> _allAppNames = new();

        [DllImport("user32.dll")]
        private static extern bool EnumDisplayDevices(string? lpDevice, uint iDevNum, ref DISPLAY_DEVICE lpDisplayDevice, uint dwFlags);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
        public struct DISPLAY_DEVICE
        {
            [MarshalAs(UnmanagedType.U4)]
            public int cb;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
            public string DeviceName;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string DeviceString;
            [MarshalAs(UnmanagedType.U4)]
            public int StateFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string DeviceID;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string DeviceKey;
        }

        private static readonly string FontPath =
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Fonts), "arial.ttf");

        // ═════════════════════════════════════════════════════════════════════
        public MainWindow()
        {
            try
            {
                InitializeComponent();
                Loaded += MainWindow_Loaded;
                StateChanged += (_, _) => UpdateMaxRestoreGlyph();
            }
            catch (Exception ex) { MessageBox.Show($"Init error: {ex.Message}", "PureType"); }
        }

        private void MainWindow_Loaded(object sender, RoutedEventArgs e)
        {
            try
            {
                _previewBuffer = Marshal.AllocHGlobal(PreviewRenderWidth * PreviewRenderHeight * 4);
                _wb = new WriteableBitmap(PreviewRenderWidth, PreviewRenderHeight, 96, 96, PixelFormats.Bgra32, null);
                PreviewImage.Source = _wb;
                LoadSettings();
                UpdatePreview();
                UpdateMaxRestoreGlyph();
            }
            catch (Exception ex) { MessageBox.Show($"Load error: {ex.Message}", "PureType"); }
        }

        // ═════════════════════════════════════════════════════════════════════
        //  INI helpers
        // ═════════════════════════════════════════════════════════════════════

        private static Dictionary<string, string> ParseIni(IEnumerable<string> lines)
        {
            var dict    = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            var section = string.Empty;
            foreach (var raw in lines)
            {
                var t = raw.Trim();
                if (string.IsNullOrEmpty(t) || t[0] == ';' || t[0] == '#') continue;
                if (t.StartsWith("[") && t.EndsWith("]"))
                {
                    section = t[1..^1].Trim().ToLowerInvariant();
                    continue;
                }
                var eq = t.IndexOf('=');
                if (eq < 0) continue;
                var key = t[..eq].Trim().ToLowerInvariant();
                var val = t[(eq + 1)..].Trim();
                var sc  = val.IndexOf(';');
                if (sc >= 0) val = val[..sc].Trim();
                dict[section + "." + key] = val;
            }
            return dict;
        }

        private void SetIniValue(string section, string key, string value)
        {
            var sectionHeader = $"[{section}]";
            bool inSection    = false;
            bool found        = false;

            for (int i = 0; i < _iniLines.Count; i++)
            {
                var t = _iniLines[i].Trim();
                if (t.StartsWith("["))
                {
                    if (inSection && !found) break;
                    inSection = string.Equals(t, sectionHeader, StringComparison.OrdinalIgnoreCase);
                    continue;
                }
                if (!inSection || t.Length == 0 || t[0] == ';' || t[0] == '#') continue;
                var eq = t.IndexOf('=');
                if (eq < 0) continue;
                if (string.Equals(t[..eq].Trim(), key, StringComparison.OrdinalIgnoreCase))
                {
                    _iniLines[i] = $"{key} = {value}";
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                for (int i = 0; i < _iniLines.Count; i++)
                {
                    if (string.Equals(_iniLines[i].Trim(), sectionHeader, StringComparison.OrdinalIgnoreCase))
                    {
                        _iniLines.Insert(i + 1, $"{key} = {value}");
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    _iniLines.Add(string.Empty);
                    _iniLines.Add(sectionHeader);
                    _iniLines.Add($"{key} = {value}");
                }
            }
        }

        private float GetFloat(Dictionary<string, string> d, string fk, float def)
        {
            if (d.TryGetValue($"{_currentSection}.{fk}", out var s) || (_currentSection != "general" && d.TryGetValue($"general.{fk}", out s)))
            {
                if (float.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out float v)) return v;
            }
            return def;
        }

        private bool GetBool(Dictionary<string, string> d, string fk, bool def)
        {
            if (d.TryGetValue($"{_currentSection}.{fk}", out var s) || (_currentSection != "general" && d.TryGetValue($"general.{fk}", out s)))
            {
                s = s.ToLowerInvariant();
                return s == "true" || s == "1" || s == "yes";
            }
            return def;
        }

        private string GetString(Dictionary<string, string> d, string fk, string def)
        {
            if (d.TryGetValue($"{_currentSection}.{fk}", out var s) || (_currentSection != "general" && d.TryGetValue($"general.{fk}", out s)))
                return s;
            return def;
        }

        private static string F2(double v)   => v.ToString("F2", CultureInfo.InvariantCulture);
        private static string Bool(bool b)   => b ? "true" : "false";
        private static bool ParseBoolValue(string? value, bool defaultValue = false)
        {
            if (string.IsNullOrWhiteSpace(value)) return defaultValue;
            string v = value.Trim().ToLowerInvariant();
            return v == "true" || v == "1" || v == "yes";
        }

        private static string NormalizeAppName(string raw)
        {
            string name = raw.Trim().ToLowerInvariant();
            if (string.IsNullOrWhiteSpace(name)) return string.Empty;
            if (!name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
                name += ".exe";
            return name;
        }

        private static string NormalizeFilterToken(string raw)
        {
            if (string.IsNullOrWhiteSpace(raw)) return string.Empty;

            var sb = new StringBuilder(raw.Length);
            foreach (char c in raw)
            {
                if (char.IsLetterOrDigit(c))
                    sb.Append(char.ToLowerInvariant(c));
            }
            return sb.ToString();
        }

        private static bool IsSubsequenceMatch(string query, string candidate)
        {
            if (query.Length == 0) return true;
            int qi = 0;
            for (int i = 0; i < candidate.Length && qi < query.Length; i++)
            {
                if (candidate[i] == query[qi])
                    qi++;
            }
            return qi == query.Length;
        }

        private static bool MatchesAppFilter(string appName, string normalizedQuery)
        {
            if (string.IsNullOrEmpty(normalizedQuery)) return true;

            string normalizedCandidate = NormalizeFilterToken(appName);
            if (normalizedCandidate.Contains(normalizedQuery, StringComparison.Ordinal))
                return true;

            return IsSubsequenceMatch(normalizedQuery, normalizedCandidate);
        }

        private void ApplyAppsFilter(string rawQuery, bool openDropDownIfMatches = true)
        {
            string normalizedQuery = NormalizeFilterToken(rawQuery);

            var filtered = _allAppNames
                .Where(name => MatchesAppFilter(name, normalizedQuery))
                .OrderBy(name => name, StringComparer.OrdinalIgnoreCase)
                .ToList();

            _isUpdatingAppsCombo = true;
            try
            {
                AppsCombo.ItemsSource = filtered;
                AppsCombo.Text = rawQuery;

                if (AppsCombo.IsKeyboardFocusWithin &&
                    openDropDownIfMatches &&
                    !string.IsNullOrWhiteSpace(rawQuery) &&
                    filtered.Count > 0)
                {
                    AppsCombo.IsDropDownOpen = true;
                }
                else if (filtered.Count == 0 || string.IsNullOrWhiteSpace(rawQuery))
                {
                    AppsCombo.IsDropDownOpen = false;
                }
            }
            finally
            {
                _isUpdatingAppsCombo = false;
            }
        }

        private void SetContext(ContextType type, string rawName = "")
        {
            _currentContext = type;
            _currentContextName = rawName.Trim();

            switch (type)
            {
                case ContextType.Global:
                    _currentSection = "general";
                    _contextTitle = "EDITING GLOBAL DEFAULTS";
                    break;
                case ContextType.Monitor:
                    _currentSection = $"monitor_{_currentContextName}".ToLowerInvariant();
                    string monTitle = _currentContextName.StartsWith(@"\\.\", StringComparison.OrdinalIgnoreCase)
                        ? _currentContextName.Replace(@"\\.\", "")
                        : _currentContextName;
                    _contextTitle = $"EDITING MONITOR PROFILE: {monTitle.ToUpperInvariant()}";
                    break;
                case ContextType.App:
                    _currentContextName = NormalizeAppName(_currentContextName);
                    _currentSection = $"app_{_currentContextName}".ToLowerInvariant();
                    _contextTitle = $"EDITING APP PROFILE: {_currentContextName.ToUpperInvariant()}";
                    break;
                default:
                    _currentSection = "general";
                    _contextTitle = "EDITING GLOBAL DEFAULTS";
                    break;
            }

            ContextTitleText.Text = _contextTitle;
            SyncContextUiState();
        }

        private void SyncContextUiState()
        {
            bool isGlobal = _currentContext == ContextType.Global;

            LodSmallSlider.IsEnabled = isGlobal;
            LodLargeSlider.IsEnabled = isGlobal;
            HighDpiLowSlider.IsEnabled = isGlobal;
            HighDpiHighSlider.IsEnabled = isGlobal;
            BlacklistText.IsEnabled = isGlobal;
            StartWithWindowsCheck.IsEnabled = isGlobal;
            DebugEnabledCheck.IsEnabled = isGlobal;
            LogFileText.IsEnabled = isGlobal;
            HighlightGlyphsCheck.IsEnabled = isGlobal;
        }

        private void RefreshAppsComboItems()
        {
            string previousText = AppsCombo.Text.Trim();

            var appNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

            foreach (var line in _iniLines)
            {
                string t = line.Trim();
                if (!t.StartsWith("[app_", StringComparison.OrdinalIgnoreCase) || !t.EndsWith("]"))
                    continue;
                string extracted = t.Substring(5, t.Length - 6);
                string normalized = NormalizeAppName(extracted);
                if (!string.IsNullOrEmpty(normalized))
                    appNames.Add(normalized);
            }

            foreach (var proc in Process.GetProcesses())
            {
                try
                {
                    string normalized = NormalizeAppName(proc.ProcessName);
                    if (!string.IsNullOrEmpty(normalized))
                        appNames.Add(normalized);
                }
                catch
                {
                    // Some protected processes can throw on access; skip safely.
                }
                finally
                {
                    proc.Dispose();
                }
            }

            _allAppNames = appNames
                .OrderBy(v => v, StringComparer.OrdinalIgnoreCase)
                .ToList();

            ApplyAppsFilter(previousText, openDropDownIfMatches: false);
        }

        // ═════════════════════════════════════════════════════════════════════
        //  Load
        // ═════════════════════════════════════════════════════════════════════

        private void LoadSettings()
        {
            _isUpdatingContext = true;
            _iniPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "puretype.ini");
            _iniLines = File.Exists(_iniPath)
                ? new List<string>(File.ReadAllLines(_iniPath))
                : new List<string>(DefaultIni.Split('\n'));

            var d = ParseIni(_iniLines);

            // Populate DisplaysCombo securely (only runs once if empty)
            if (DisplaysCombo.Items.Count == 0)
            {
                DISPLAY_DEVICE dinfo = new DISPLAY_DEVICE();
                dinfo.cb = Marshal.SizeOf(dinfo);
                for (uint id = 0; EnumDisplayDevices(null, id, ref dinfo, 0); id++)
                {
                    if ((dinfo.StateFlags & 1) != 0) // DISPLAY_DEVICE_ATTACHED_TO_DESKTOP
                    {
                        var cbi = new ComboBoxItem { Content = $"{dinfo.DeviceString} ({dinfo.DeviceName})", Tag = dinfo.DeviceName };
                        DisplaysCombo.Items.Add(cbi);
                    }
                    dinfo.cb = Marshal.SizeOf(dinfo);
                }
                if (DisplaysCombo.Items.Count > 0) DisplaysCombo.SelectedIndex = 0;
            }

            // Populate App profiles + running processes.
            RefreshAppsComboItems();

            // Panel type
            var panel = GetString(d, "paneltype", "rwbg").ToLowerInvariant();
            foreach (ComboBoxItem item in PanelTypeCombo.Items)
                if (string.Equals(item.Tag?.ToString(), panel, StringComparison.OrdinalIgnoreCase))
                    { item.IsSelected = true; break; }

            // Gamma mode
            var gammaStr = GetString(d, "gammamode", "oled").ToLowerInvariant();
            foreach (ComboBoxItem item in GammaModeCombo.Items)
                if (string.Equals(item.Tag?.ToString(), gammaStr, StringComparison.OrdinalIgnoreCase))
                    { item.IsSelected = true; break; }

            FilterStrengthSlider.Value           = Math.Clamp(GetFloat(d, "filterstrength",         1.0f),  0.0, 5.0);
            GammaSlider.Value                    = Math.Clamp(GetFloat(d, "gamma",                  1.0f),  0.5, 3.0);
            OledGammaOutputSlider.Value          = Math.Clamp(GetFloat(d, "oledgammaoutput",        1.0f),  1.0, 2.0);
            SubpixelHintingCheck.IsChecked       = GetBool(d, "enablesubpixelhinting",             true);
            FractionalPositioningCheck.IsChecked = GetBool(d, "enablefractionalpositioning",    true);
            LumaContrastSlider.Value             = Math.Clamp(GetFloat(d, "lumacontraststrength",   1.15f), 1.0, 3.0);
            StemDarkeningCheck.IsChecked         = GetBool(d, "stemdarkeningenabled",              true);
            StemStrengthSlider.Value             = Math.Clamp(GetFloat(d, "stemdarkeningstrength",  0.25f), 0.0, 1.0);
            WoledCrosstalkSlider.Value           = Math.Clamp(GetFloat(d, "woledcrosstalkreduction",0.08f), 0.0, 1.0);

            // Context Title Update
            ContextTitleText.Text = _contextTitle;

            // Global-only settings
            if (_currentContext == ContextType.Global)
            {
                double lodSmall = Math.Clamp(GetFloat(d, "lodthresholdsmall", 10.0f), 6.0, 96.0);
                double lodLarge = Math.Clamp(GetFloat(d, "lodthresholdlarge", 22.0f), 7.0, 160.0);
                LodSmallSlider.Value   = lodSmall;
                LodLargeSlider.Minimum = lodSmall + 1;
                LodLargeSlider.Value   = Math.Max(lodLarge, lodSmall + 1);

                double dpiLow = Math.Clamp(GetFloat(d, "highdpithresholdlow", 144.0f), 96.0, 192.0);
                double dpiHigh = Math.Clamp(GetFloat(d, "highdpithresholdhigh", 216.0f), 144.0, 300.0);
                HighDpiLowSlider.Value = dpiLow;
                HighDpiHighSlider.Minimum = dpiLow + 1;
                HighDpiHighSlider.Value = Math.Max(dpiHigh, dpiLow + 1);

                DebugEnabledCheck.IsChecked      = ParseBoolValue(d.GetValueOrDefault("debug.enabled", "false"));
                LogFileText.Text                 = d.GetValueOrDefault("debug.logfile", "PURETYPE.log");
                HighlightGlyphsCheck.IsChecked   = ParseBoolValue(d.GetValueOrDefault("debug.highlightrenderedglyphs", "false"));

                using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run", false);
                StartWithWindowsCheck.IsChecked = key?.GetValue("PureType") != null;

                string blacklistStr = d.GetValueOrDefault("general.blacklist", "");
                if (string.IsNullOrWhiteSpace(blacklistStr))
                {
                    blacklistStr = "vgc.exe, vgtray.exe, easyanticheat.exe, easyanticheat_eos.exe, beservice.exe, bedaisy.exe, gameguard.exe, nprotect.exe, pnkbstra.exe, pnkbstrb.exe, faceit.exe, faceit_ac.exe, csgo.exe, cs2.exe, valorant.exe, valorant-win64-shipping.exe, r5apex.exe, fortniteclient-win64-shipping.exe, eldenring.exe, gta5.exe, rdr2.exe, overwatchlauncher.exe, rainbowsix.exe, destiny2.exe, tarkov.exe";
                }
                BlacklistText.Text = string.Join("\n", blacklistStr.Split(new[] { ',' }, StringSplitOptions.RemoveEmptyEntries));
            }

            SyncContextUiState();
            SyncWoledVisibility();
            SyncStemStrengthEnabled();
            SyncDebugOptions();
            _isUpdatingContext = false;
        }

        // ═════════════════════════════════════════════════════════════════════
        //  Save
        // ═════════════════════════════════════════════════════════════════════

        private void SaveSettings()
        {
            if (_isUpdatingContext) return;

            var panelTag = (PanelTypeCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "rwbg";
            SetIniValue(_currentSection, "panelType",                  panelTag);
            var gammaTag = (GammaModeCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "oled";
            SetIniValue(_currentSection, "gammaMode",                  gammaTag);
            SetIniValue(_currentSection, "filterStrength",             F2(FilterStrengthSlider.Value));
            SetIniValue(_currentSection, "gamma",                      F2(GammaSlider.Value));
            SetIniValue(_currentSection, "oledGammaOutput",            F2(OledGammaOutputSlider.Value));
            SetIniValue(_currentSection, "enableSubpixelHinting",      Bool(SubpixelHintingCheck.IsChecked     == true));
            SetIniValue(_currentSection, "enableFractionalPositioning", Bool(FractionalPositioningCheck.IsChecked == true));
            SetIniValue(_currentSection, "lumaContrastStrength",       F2(LumaContrastSlider.Value));
            SetIniValue(_currentSection, "stemDarkeningEnabled",       Bool(StemDarkeningCheck.IsChecked   == true));
            SetIniValue(_currentSection, "stemDarkeningStrength",      F2(StemStrengthSlider.Value));
            SetIniValue(_currentSection, "woledCrossTalkReduction",    F2(WoledCrosstalkSlider.Value));

            if (_currentContext == ContextType.Global)
            {
                SetIniValue("general", "lodThresholdSmall",          F2(LodSmallSlider.Value));
                SetIniValue("general", "lodThresholdLarge",          F2(LodLargeSlider.Value));
                SetIniValue("general", "highDpiThresholdLow",        F2(HighDpiLowSlider.Value));
                SetIniValue("general", "highDpiThresholdHigh",       F2(HighDpiHighSlider.Value));

                var bl = string.Join(", ", BlacklistText.Text.Split(new[] { '\n', '\r' }, StringSplitOptions.RemoveEmptyEntries).Select(s => s.Trim()));
                SetIniValue("general", "blacklist",                  bl);

                SetIniValue("debug",   "enabled",                    Bool(DebugEnabledCheck.IsChecked    == true));
                SetIniValue("debug",   "logFile",                    LogFileText.Text.Trim());
                SetIniValue("debug",   "highlightRenderedGlyphs",    Bool(HighlightGlyphsCheck.IsChecked == true));
                
                using var regKey = Registry.CurrentUser.OpenSubKey(
                    @"Software\Microsoft\Windows\CurrentVersion\Run", writable: true);
                if (regKey != null)
                {
                    var exePath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "puretype.exe");
                    if (StartWithWindowsCheck.IsChecked == true)
                        regKey.SetValue("PureType", $"\"{exePath}\"");
                    else
                        regKey.DeleteValue("PureType", throwOnMissingValue: false);
                }
            }

            File.WriteAllLines(_iniPath, _iniLines);
        }

        // ═════════════════════════════════════════════════════════════════════
        //  Preview
        // ═════════════════════════════════════════════════════════════════════

        private void UpdatePreview()
        {
            if (!_dllAvailable || _previewBuffer == IntPtr.Zero) return;
            if (_wb is null) return;
            try
            {
                var panelItem = PanelTypeCombo.SelectedItem as ComboBoxItem;
                if (panelItem == null) return;

                int panelType = panelItem.Tag?.ToString() switch
                {
                    "rwbg"             => 0,
                    "rgwb"             => 1,
                    "qd_oled_gen1"     => 2,
                    "qd_oled_gen3"     => 3,
                    "qd_oled_gen4"     => 4,
                    _                  => 0
                };

                var sizeItem = PreviewSizeCombo.SelectedItem as ComboBoxItem;
                uint fontSize = sizeItem != null && uint.TryParse(sizeItem.Tag?.ToString(), out uint fs) ? fs : 32u;

                var gammaItem = GammaModeCombo.SelectedItem as ComboBoxItem;
                int gammaMode = gammaItem?.Tag?.ToString() == "oled" ? 1 : 0;
                bool useMeasuredContrast =
                    (PreviewContrastCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() != "proxy";

                string sample =
                    "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor " +
                    "incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis " +
                    "nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat.";

                bool ok = GeneratePreview(
                    sample, FontPath, fontSize,
                    (float)FilterStrengthSlider.Value,
                    (float)GammaSlider.Value,
                    gammaMode,
                    (float)OledGammaOutputSlider.Value,
                    (float)LumaContrastSlider.Value,
                    (float)WoledCrosstalkSlider.Value,
                    SubpixelHintingCheck.IsChecked == true,
                    FractionalPositioningCheck.IsChecked == true,
                    StemDarkeningCheck.IsChecked == true,
                    (float)StemStrengthSlider.Value,
                    panelType,
                    useMeasuredContrast,
                    PreviewRenderWidth, PreviewRenderHeight,
                    _previewBuffer);

                if (!ok) return;

                _wb.Lock();
                unsafe
                {
                    Buffer.MemoryCopy(
                        (void*)_previewBuffer, (void*)_wb.BackBuffer,
                        PreviewRenderWidth * PreviewRenderHeight * 4,
                        PreviewRenderWidth * PreviewRenderHeight * 4);
                }
                _wb.AddDirtyRect(new Int32Rect(0, 0, PreviewRenderWidth, PreviewRenderHeight));
                _wb.Unlock();

                PreviewImage.Visibility    = Visibility.Visible;
                PreviewFallback.Visibility = Visibility.Collapsed;
            }
            catch (DllNotFoundException)
            {
                _dllAvailable = false;
                PreviewImage.Visibility    = Visibility.Collapsed;
                PreviewFallback.Visibility = Visibility.Visible;
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Preview: {ex.Message}");
            }
        }

        // ═════════════════════════════════════════════════════════════════════
        //  Sync helpers
        // ═════════════════════════════════════════════════════════════════════

        private void SyncWoledVisibility()
        {
            var tag = (PanelTypeCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "rwbg";
            bool isWoled = tag == "rwbg" || tag == "rgwb";
            WoledPanel.Visibility = isWoled ? Visibility.Visible : Visibility.Collapsed;
        }

        private void SyncStemStrengthEnabled()
        {
            bool on = StemDarkeningCheck.IsChecked == true;
            StemStrengthPanel.Opacity    = on ? 1.0 : 0.35;
            StemStrengthSlider.IsEnabled = on;
        }

        private void SyncDebugOptions()
        {
            bool on = DebugEnabledCheck.IsChecked == true;
            DebugOptionsPanel.Opacity   = on ? 1.0 : 0.35;
            DebugOptionsPanel.IsEnabled = on;
        }

        // ═════════════════════════════════════════════════════════════════════
        //  Event handlers
        // ═════════════════════════════════════════════════════════════════════

        private void Setting_Changed_Check(object sender, RoutedEventArgs e)
        {
            if (!IsLoaded) return;
            SyncWoledVisibility();
            UpdatePreview();
        }

        private void Setting_Changed_Combo(object sender, SelectionChangedEventArgs e)
        {
            if (!IsLoaded) return;
            SyncWoledVisibility();
            UpdatePreview();
        }

        private enum UserPreset
        {
            Balanced,
            Sharp,
            Clean
        }

        private void ApplyPreset_Click(object sender, RoutedEventArgs e)
        {
            if (!IsLoaded) return;
            if (sender is not FrameworkElement fe) return;

            UserPreset preset = fe.Tag?.ToString()?.ToLowerInvariant() switch
            {
                "sharp" => UserPreset.Sharp,
                "clean" => UserPreset.Clean,
                _ => UserPreset.Balanced
            };

            var panelTag = (PanelTypeCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "rwbg";

            // Base defaults shared by all presets.
            GammaSlider.Value = 1.0;
            SubpixelHintingCheck.IsChecked = true;
            FractionalPositioningCheck.IsChecked = true;
            LodSmallSlider.Value = 10;
            LodLargeSlider.Value = 22;
            HighDpiLowSlider.Value = 144;
            HighDpiHighSlider.Value = 216;

            foreach (ComboBoxItem item in GammaModeCombo.Items)
            {
                if (item.Tag?.ToString() == "oled") { item.IsSelected = true; break; }
            }

            double filterStrength;
            double oledGammaOutput;
            double woledCrosstalk;
            double lumaContrast;
            double stemStrength;

            switch (panelTag)
            {
                case "rwbg":
                case "rgwb":
                    switch (preset)
                    {
                        case UserPreset.Sharp:
                            filterStrength = 0.90;
                            oledGammaOutput = 1.18;
                            woledCrosstalk = 0.08;
                            lumaContrast = 1.35;
                            stemStrength = 0.30;
                            break;
                        case UserPreset.Clean:
                            filterStrength = 1.20;
                            oledGammaOutput = 1.22;
                            woledCrosstalk = 0.14;
                            lumaContrast = 1.10;
                            stemStrength = 0.14;
                            break;
                        default: // Bilanciato
                            filterStrength = 1.00;
                            oledGammaOutput = 1.20;
                            woledCrosstalk = 0.10;
                            lumaContrast = 1.25;
                            stemStrength = 0.20;
                            break;
                    }
                    break;
                case "qd_oled_gen1":
                    switch (preset)
                    {
                        case UserPreset.Sharp:
                            filterStrength = 0.95;
                            oledGammaOutput = 1.12;
                            woledCrosstalk = 0.00;
                            lumaContrast = 1.24;
                            stemStrength = 0.38;
                            break;
                        case UserPreset.Clean:
                            filterStrength = 1.25;
                            oledGammaOutput = 1.18;
                            woledCrosstalk = 0.00;
                            lumaContrast = 1.08;
                            stemStrength = 0.22;
                            break;
                        default: // Bilanciato
                            filterStrength = 1.10;
                            oledGammaOutput = 1.15;
                            woledCrosstalk = 0.00;
                            lumaContrast = 1.15;
                            stemStrength = 0.30;
                            break;
                    }
                    break;
                case "qd_oled_gen3":
                case "qd_oled_gen4":
                    switch (preset)
                    {
                        case UserPreset.Sharp:
                            filterStrength = 0.90;
                            oledGammaOutput = 1.12;
                            woledCrosstalk = 0.00;
                            lumaContrast = 1.22;
                            stemStrength = 0.33;
                            break;
                        case UserPreset.Clean:
                            filterStrength = 1.15;
                            oledGammaOutput = 1.18;
                            woledCrosstalk = 0.00;
                            lumaContrast = 1.08;
                            stemStrength = 0.18;
                            break;
                        default: // Bilanciato
                            filterStrength = 1.00;
                            oledGammaOutput = 1.15;
                            woledCrosstalk = 0.00;
                            lumaContrast = 1.15;
                            stemStrength = 0.25;
                            break;
                    }
                    break;
                default:
                    filterStrength = 1.00;
                    oledGammaOutput = 1.15;
                    woledCrosstalk = 0.00;
                    lumaContrast = 1.15;
                    stemStrength = 0.25;
                    break;
            }

            FilterStrengthSlider.Value = Math.Clamp(filterStrength, FilterStrengthSlider.Minimum, FilterStrengthSlider.Maximum);
            OledGammaOutputSlider.Value = Math.Clamp(oledGammaOutput, OledGammaOutputSlider.Minimum, OledGammaOutputSlider.Maximum);
            WoledCrosstalkSlider.Value = Math.Clamp(woledCrosstalk, WoledCrosstalkSlider.Minimum, WoledCrosstalkSlider.Maximum);
            LumaContrastSlider.Value = Math.Clamp(lumaContrast, LumaContrastSlider.Minimum, LumaContrastSlider.Maximum);
            StemDarkeningCheck.IsChecked = true;
            StemStrengthSlider.Value = Math.Clamp(stemStrength, StemStrengthSlider.Minimum, StemStrengthSlider.Maximum);

            SyncWoledVisibility();
            SyncStemStrengthEnabled();
            UpdatePreview();
        }

        private void StemDarkening_Changed(object sender, RoutedEventArgs e)
        {
            if (!IsLoaded) return;
            SyncStemStrengthEnabled();
            UpdatePreview();
        }

        private void Debug_Changed(object sender, RoutedEventArgs e)
        {
            if (!IsLoaded) return;
            SyncDebugOptions();
        }

        private void LodSmall_Changed(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (!IsLoaded) return;
            LodLargeSlider.Minimum = LodSmallSlider.Value + 1;
            if (LodLargeSlider.Value <= LodSmallSlider.Value)
                LodLargeSlider.Value = LodSmallSlider.Value + 1;
        }

        private void DpiLow_Changed(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (!IsLoaded) return;
            HighDpiHighSlider.Minimum = HighDpiLowSlider.Value + 1;
            if (HighDpiHighSlider.Value <= HighDpiLowSlider.Value)
                HighDpiHighSlider.Value = HighDpiLowSlider.Value + 1;
        }

        private void PreviewSize_Changed(object sender, SelectionChangedEventArgs e)
        {
            if (!IsLoaded) return;
            UpdatePreview();
        }

        private void PreviewContrast_Changed(object sender, SelectionChangedEventArgs e)
        {
            if (!IsLoaded) return;
            UpdatePreview();
        }

        private void ScrollViewer_MouseWheel(object sender, System.Windows.Input.MouseWheelEventArgs e)
        {
            if (sender is ScrollViewer sv)
            {
                sv.ScrollToVerticalOffset(sv.VerticalOffset - e.Delta * 0.4);
                e.Handled = true;
            }
        }

        private void SettingsScroll_ScrollChanged(object sender, ScrollChangedEventArgs e)
        {
            if (sender is not ScrollViewer sv) return;

            double extent  = sv.ScrollableHeight;
            double offset  = sv.VerticalOffset;
            double visible = sv.ViewportHeight;

            // Fade out the bottom gradient as user nears the end
            if (extent > 0)
            {
                double progress = offset / extent;
                ScrollFadeBottom.Opacity = Math.Max(0, 1.0 - progress * 1.5);
            }

            // Move the thumb along the track
            if (extent > 0 && ScrollIndicatorTrack.ActualHeight > 0)
            {
                double trackH  = ScrollIndicatorTrack.ActualHeight;
                double thumbH  = Math.Max(20, trackH * visible / (visible + extent));
                double maxTop  = trackH - thumbH;
                double topPos  = (offset / extent) * maxTop;
                ScrollIndicatorThumb.Height = thumbH;
                ScrollIndicatorThumb.Margin = new Thickness(0, topPos, 0, 0);
            }
        }

        // ── Save: NO close, NO dialog — solo feedback visivo sul bottone ──────
        private void Save_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                SaveSettings();
                SignalTrayReload();

                if (sender is Button btn)
                {
                    string original  = btn.Content?.ToString() ?? "SAVE  ·  APPLY";
                    btn.Content      = "✓  SAVED";
                    btn.IsEnabled    = false;

                    var timer = new System.Windows.Threading.DispatcherTimer
                        { Interval = TimeSpan.FromSeconds(2) };
                    timer.Tick += (s, _) =>
                    {
                        btn.Content   = original;
                        btn.IsEnabled = true;
                        ((System.Windows.Threading.DispatcherTimer)s!).Stop();
                    };
                    timer.Start();
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Errore salvataggio:\n{ex.Message}",
                    "PureType", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void Setting_Changed_Slider(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (!IsLoaded) return;
            UpdatePreview();
        }

        // ═════════════════════════════════════════════════════════════════════
        //  Context Switcher Event Handlers
        // ═════════════════════════════════════════════════════════════════════

        private void Tab_Checked(object sender, RoutedEventArgs e)
        {
            if (!IsLoaded || _isUpdatingContext) return;

            DisplaysPanel.Visibility = Visibility.Collapsed;
            AppsPanel.Visibility     = Visibility.Collapsed;

            if (TabGlobal.IsChecked == true)
            {
                SetContext(ContextType.Global);
                LoadSettings();
                UpdatePreview();
            }
            else if (TabDisplays.IsChecked == true)
            {
                DisplaysPanel.Visibility = Visibility.Visible;
                UpdateContextFromCombo(DisplaysCombo, "Monitor_");
            }
            else if (TabApps.IsChecked == true)
            {
                AppsPanel.Visibility = Visibility.Visible;
                UpdateContextFromCombo(AppsCombo, "App_");
            }
        }

        private void ContextCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (!IsLoaded || _isUpdatingContext) return;
            if (ReferenceEquals(sender, AppsCombo) && _isUpdatingAppsCombo) return;
            
            if (TabDisplays.IsChecked == true)
                UpdateContextFromCombo(DisplaysCombo, "Monitor_");
            else if (TabApps.IsChecked == true)
                UpdateContextFromCombo(AppsCombo, "App_");
        }

        private void UpdateContextFromCombo(ComboBox cb, string prefix)
        {
            _isUpdatingContext = true;
            string sel = "";
            if (cb.SelectedItem is ComboBoxItem cbi)
            {
                sel = cbi.Tag?.ToString() ?? "";
            }
            else if (cb.SelectedItem is string s)
            {
                sel = s.Trim();
            }
            else if (cb.IsEditable)
            {
                sel = cb.Text.Trim();
            }

            if (string.IsNullOrWhiteSpace(sel))
                sel = cb.Text.Trim();

            if (string.IsNullOrEmpty(sel))
            {
                SetContext(ContextType.Global);
                _contextTitle = "SELECT A PROFILE TO EDIT";
                ContextTitleText.Text = _contextTitle;
            }
            else
            {
                if (string.Equals(prefix, "Monitor_", StringComparison.OrdinalIgnoreCase))
                    SetContext(ContextType.Monitor, sel);
                else
                    SetContext(ContextType.App, sel);
            }
            
            _isUpdatingContext = false;
            LoadSettings();
            UpdatePreview();
        }

        private void AppsCombo_TextChanged(object sender, TextChangedEventArgs e)
        {
            if (!IsLoaded || _isUpdatingContext || _isUpdatingAppsCombo) return;

            string raw = AppsCombo.Text;
            ApplyAppsFilter(raw);

            string txt = NormalizeAppName(raw);
            if (string.IsNullOrEmpty(txt)) return;

            bool exists = _allAppNames.Any(name => string.Equals(name, txt, StringComparison.OrdinalIgnoreCase));
            if (!exists)
            {
                _contextTitle = $"NEW PROFILE (UNSAVED): {txt.ToUpperInvariant()}";
                ContextTitleText.Text = _contextTitle;
            }
        }

        private void AddAppBtn_Click(object sender, RoutedEventArgs e)
        {
            string txt = NormalizeAppName(AppsCombo.Text);
            if (string.IsNullOrEmpty(txt)) return;

            // Save the profile immediately by flushing existing slider state
            SetContext(ContextType.App, txt);
            SaveSettings();
            SignalTrayReload();

            RefreshAppsComboItems();
            ApplyAppsFilter(txt, openDropDownIfMatches: false);
            AppsCombo.SelectedItem = txt;
            AppsCombo.Text = txt;

            _contextTitle = $"EDITING APP PROFILE: {txt.ToUpperInvariant()}";
            ContextTitleText.Text = _contextTitle;

            MessageBox.Show($"Profile [App_{txt}] created and saved successfully.", "PureType", MessageBoxButton.OK, MessageBoxImage.Information);
        }

        private void RemoveAppBtn_Click(object sender, RoutedEventArgs e)
        {
            string txt = NormalizeAppName(AppsCombo.Text);
            if (string.IsNullOrEmpty(txt)) return;

            if (MessageBox.Show($"Are you sure you want to delete the profile for {txt}?", "PureType", MessageBoxButton.YesNo, MessageBoxImage.Warning) == MessageBoxResult.Yes)
            {
                string targetSection = $"[App_{txt}]".ToLowerInvariant();
                bool inSection = false;
                List<string> newLines = new();

                foreach (var line in _iniLines)
                {
                    string trimmed = line.Trim();
                    if (trimmed.StartsWith("["))
                    {
                        inSection = string.Equals(trimmed, targetSection, StringComparison.OrdinalIgnoreCase);
                    }

                    if (!inSection)
                    {
                        newLines.Add(line);
                    }
                }

                _iniLines = newLines;
                File.WriteAllLines(_iniPath, _iniLines);
                SignalTrayReload();

                AppsCombo.Text = "";
                RefreshAppsComboItems();
                UpdateContextFromCombo(AppsCombo, "App_");
                
                MessageBox.Show($"Profile for {txt} removed.", "PureType", MessageBoxButton.OK, MessageBoxImage.Information);
            }
        }

        private void Exit_Click(object sender, RoutedEventArgs e) => Close();

        private void Minimize_Click(object sender, RoutedEventArgs e)
        {
            WindowState = WindowState.Minimized;
        }

        private void ToggleMaximize_Click(object sender, RoutedEventArgs e)
        {
            ToggleWindowState();
        }

        private void ToggleWindowState()
        {
            WindowState = WindowState == WindowState.Maximized
                ? WindowState.Normal
                : WindowState.Maximized;
            UpdateMaxRestoreGlyph();
        }

        private void UpdateMaxRestoreGlyph()
        {
            if (MaxRestoreGlyph is null) return;
            MaxRestoreGlyph.Text = WindowState == WindowState.Maximized ? "[ ]" : "[]";
        }

        private void ResizeThumb_DragDelta(object sender, DragDeltaEventArgs e)
        {
            if (WindowState == WindowState.Maximized) return;
            if (sender is not Thumb thumb) return;
            if (!int.TryParse(thumb.Tag?.ToString(), out int direction)) return;

            double left = Left;
            double top = Top;
            double width = Width;
            double height = Height;

            switch (direction)
            {
                case 1: // left
                {
                    double delta = Math.Min(e.HorizontalChange, width - MinWidth);
                    left += delta;
                    width -= delta;
                    break;
                }
                case 2: // right
                    width = Math.Max(MinWidth, width + e.HorizontalChange);
                    break;
                case 3: // top
                {
                    double delta = Math.Min(e.VerticalChange, height - MinHeight);
                    top += delta;
                    height -= delta;
                    break;
                }
                case 6: // bottom
                    height = Math.Max(MinHeight, height + e.VerticalChange);
                    break;
                case 4: // top-left
                {
                    double deltaX = Math.Min(e.HorizontalChange, width - MinWidth);
                    double deltaY = Math.Min(e.VerticalChange, height - MinHeight);
                    left += deltaX;
                    top += deltaY;
                    width -= deltaX;
                    height -= deltaY;
                    break;
                }
                case 5: // top-right
                {
                    double deltaY = Math.Min(e.VerticalChange, height - MinHeight);
                    top += deltaY;
                    width = Math.Max(MinWidth, width + e.HorizontalChange);
                    height -= deltaY;
                    break;
                }
                case 7: // bottom-left
                {
                    double deltaX = Math.Min(e.HorizontalChange, width - MinWidth);
                    left += deltaX;
                    width -= deltaX;
                    height = Math.Max(MinHeight, height + e.VerticalChange);
                    break;
                }
                case 8: // bottom-right
                    width = Math.Max(MinWidth, width + e.HorizontalChange);
                    height = Math.Max(MinHeight, height + e.VerticalChange);
                    break;
            }

            Left = left;
            Top = top;
            Width = width;
            Height = height;
        }

        private void Window_MouseLeftButtonDown(object sender, System.Windows.Input.MouseButtonEventArgs e)
        {
            if (e.ClickCount == 2)
            {
                ToggleWindowState();
                return;
            }

            if (e.ButtonState == System.Windows.Input.MouseButtonState.Pressed)
                DragMove();
        }

        protected override void OnClosed(EventArgs e)
        {
            if (_previewBuffer != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(_previewBuffer);
                _previewBuffer = IntPtr.Zero;
            }
            base.OnClosed(e);
        }

        // ═════════════════════════════════════════════════════════════════════
        //  Default INI fallback
        // ═════════════════════════════════════════════════════════════════════

        private const string DefaultIni =
            "[general]\n" +
            "panelType = rwbg\n" +
            "filterStrength = 1.00\n" +
            "gamma = 1.00\n" +
            "enableSubpixelHinting = true\n" +
            "enableFractionalPositioning = true\n" +
            "lodThresholdSmall = 10.00\n" +
            "lodThresholdLarge = 22.00\n" +
            "woledCrossTalkReduction = 0.08\n" +
            "lumaContrastStrength = 1.15\n" +
            "stemDarkeningEnabled = true\n" +
            "stemDarkeningStrength = 0.25\n" +
            "blacklist = vgc.exe, vgtray.exe, easyanticheat.exe, easyanticheat_eos.exe, beservice.exe, bedaisy.exe, gameguard.exe, nprotect.exe, pnkbstra.exe, pnkbstrb.exe, faceit.exe, faceit_ac.exe, csgo.exe, cs2.exe, valorant.exe, valorant-win64-shipping.exe, r5apex.exe, fortniteclient-win64-shipping.exe, eldenring.exe, gta5.exe, rdr2.exe, overwatchlauncher.exe, rainbowsix.exe, destiny2.exe, tarkov.exe\n\n" +
            "[debug]\n" +
            "enabled = false\n" +
            "logFile = PURETYPE.log\n" +
            "highlightRenderedGlyphs = false\n";
    }
}
