using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Microsoft.Win32;
using PuretypeUI.Services;

namespace PuretypeUI
{
    public partial class MainWindow : Window
    {
        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr FindWindow(string lpClassName, string lpWindowName);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

        [DllImport("PureType.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        private static extern bool GeneratePreview(
            string text,
            string fontPath,
            uint fontSize,
            float filterStrength,
            float gamma,
            int gammaMode,
            float oledGammaOutput,
            float lumaContrastStrength,
            float woledCrossTalkReduction,
            bool enableSubpixelHinting,
            bool enableFractionalPositioning,
            bool stemDarkeningEnabled,
            float stemDarkeningStrength,
            int panelType,
            bool useMeasuredContrast,
            int width,
            int height,
            IntPtr pBuffer);

        [StructLayout(LayoutKind.Sequential)]
        private struct PreviewParamsV2
        {
            public uint size;
            public uint version;
            public IntPtr text;
            public IntPtr fontPath;
            public uint fontSize;
            public float filterStrength;
            public float gamma;
            public int gammaMode;
            public float oledGammaOutput;
            public float lumaContrastStrength;
            public float woledCrossTalkReduction;
            public uint enableSubpixelHinting;
            public uint enableFractionalPositioning;
            public uint stemDarkeningEnabled;
            public float stemDarkeningStrength;
            public int panelType;
            public uint useMeasuredContrast;
            public int width;
            public int height;
            public IntPtr pBuffer;
            public float lodThresholdSmall;
            public float lodThresholdLarge;
            public uint toneParityV2Enabled;
        }

        [DllImport("PureType.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi, EntryPoint = "GeneratePreviewV2")]
        private static extern bool GeneratePreviewV2(ref PreviewParamsV2 parameters, uint paramsSize);

        [DllImport("user32.dll")]
        private static extern bool EnumDisplayDevices(string? lpDevice, uint iDevNum, ref DISPLAY_DEVICE lpDisplayDevice, uint dwFlags);

        private const uint WM_PURETYPE_RELOAD = 0x8002u;
        private const uint PreviewParamsV2Version = 1u;
        private const int PreviewRenderWidth = 600;
        private const int PreviewRenderHeight = 200;

        private IntPtr _previewBuffer = IntPtr.Zero;
        private WriteableBitmap? _writeableBitmap;
        private bool _dllAvailable = true;
        private string _iniPath = "puretype.ini";
        private List<string> _iniLines = new();
        private bool? _previewV2Available;
        private bool _toneParityV2Enabled;
        private bool _isUpdatingContext;
        private readonly List<string> _allAppNames = new();
        private StackPanel[] _pages = Array.Empty<StackPanel>();
        private string _editingProfileSection = string.Empty;
        private string? _activeQuickPreset;

        private readonly GitHubUpdateService _updateService = new();
        private UpdateInfo? _latestUpdate;
        private CancellationTokenSource? _updateCts;
        private string? _updateStagingDir;

        private static readonly string FontPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.Fonts),
            "arial.ttf");

        private static readonly string InterFontDir = ResolveInterFontDir();
        private static readonly string InterRegularPath = Path.Combine(InterFontDir, "Inter-VariableFont_opsz,wght.ttf");
        private static readonly string InterItalicPath = Path.Combine(InterFontDir, "Inter-Italic-VariableFont_opsz,wght.ttf");

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
        public struct DISPLAY_DEVICE
        {
            [MarshalAs(UnmanagedType.U4)] public int cb;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
            [MarshalAs(UnmanagedType.U4)] public int StateFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
        }

        public MainWindow()
        {
            try
            {
                InitializeComponent();
                Loaded += MainWindow_Loaded;
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Init error: {ex.Message}", "PureType", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private static void SignalTrayReload()
        {
            IntPtr hTray = FindWindow("PureTypeTrayWindow", "PureType");
            if (hTray != IntPtr.Zero)
            {
                PostMessage(hTray, WM_PURETYPE_RELOAD, IntPtr.Zero, IntPtr.Zero);
            }
        }

        private static string ResolveInterFontDir()
        {
            string baseDir = AppDomain.CurrentDomain.BaseDirectory;
            string[] candidates =
            {
                Path.Combine(baseDir, "font"),
                Path.Combine(baseDir, "..", "font"),
                Path.Combine(baseDir, "..", "..", "font"),
                Path.Combine(baseDir, "..", "..", "..", "font")
            };

            foreach (string candidate in candidates)
            {
                string full = Path.GetFullPath(candidate);
                if (Directory.Exists(full) && File.Exists(Path.Combine(full, "Inter-VariableFont_opsz,wght.ttf")))
                {
                    return full;
                }
            }

            return Path.Combine(baseDir, "font");
        }

        private void MainWindow_Loaded(object sender, RoutedEventArgs e)
        {
            try
            {
                _pages = new[]
                {
                    PageOverview,
                    PageRendering,
                    PageDisplay,
                    PageSystemFont,
                    PageProfiles,
                    PageAdvanced,
                    PageSettings,
                    PageInfo
                };

                _previewBuffer = Marshal.AllocHGlobal(PreviewRenderWidth * PreviewRenderHeight * 4);
                _writeableBitmap = new WriteableBitmap(PreviewRenderWidth, PreviewRenderHeight, 96, 96, PixelFormats.Bgra32, null);
                PreviewImage.Source = _writeableBitmap;

                SidebarVersionText.Text = AppVersion.FullDisplay;
                InfoVersionText.Text = AppVersion.FullDisplay;
                InfoVersionValue.Text = "v" + AppVersion.Current;

                LoadSettings();
                UpdateInterPreview();
                UpdateInterHints();
                UpdatePreview();

                // Auto-check for updates if enabled
                if (AutoCheckUpdatesCheck.IsChecked == true)
                {
                    _ = CheckForUpdatesInternalAsync(silent: true);
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Load error: {ex.Message}", "PureType", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void ShowPage(StackPanel activePage)
        {
            foreach (StackPanel page in _pages)
            {
                page.Visibility = ReferenceEquals(page, activePage) ? Visibility.Visible : Visibility.Collapsed;
            }
        }

        private void Tab_Checked(object sender, RoutedEventArgs e)
        {
            if (!IsLoaded || _pages.Length == 0)
            {
                return;
            }

            if (TabOverview.IsChecked == true) ShowPage(PageOverview);
            else if (TabRendering.IsChecked == true) ShowPage(PageRendering);
            else if (TabDisplay.IsChecked == true) ShowPage(PageDisplay);
            else if (TabSystemFont.IsChecked == true) ShowPage(PageSystemFont);
            else if (TabProfiles.IsChecked == true) ShowPage(PageProfiles);
            else if (TabAdvanced.IsChecked == true) ShowPage(PageAdvanced);
            else if (TabSettings.IsChecked == true) ShowPage(PageSettings);
            else if (TabInfo.IsChecked == true) ShowPage(PageInfo);
        }

        private static Dictionary<string, string> ParseIni(IEnumerable<string> lines)
        {
            var result = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            string section = string.Empty;

            foreach (string raw in lines)
            {
                string trimmed = raw.Trim();
                if (string.IsNullOrEmpty(trimmed) || trimmed[0] == ';' || trimmed[0] == '#')
                {
                    continue;
                }

                if (trimmed.StartsWith("[") && trimmed.EndsWith("]"))
                {
                    section = trimmed[1..^1].Trim().ToLowerInvariant();
                    continue;
                }

                int eq = trimmed.IndexOf('=');
                if (eq < 0)
                {
                    continue;
                }

                string key = trimmed[..eq].Trim().ToLowerInvariant();
                string value = trimmed[(eq + 1)..].Trim();
                int commentIndex = value.IndexOf(';');
                if (commentIndex >= 0)
                {
                    value = value[..commentIndex].Trim();
                }

                result[$"{section}.{key}"] = value;
            }

            return result;
        }

        private static string? GetSectionValue(IReadOnlyDictionary<string, string> map, string section, string key)
            => map.TryGetValue($"{section}.{key}", out string? value) ? value : null;

        private void SetIniValue(string section, string key, string value)
        {
            string header = $"[{section}]";
            bool inSection = false;

            for (int i = 0; i < _iniLines.Count; i++)
            {
                string trimmed = _iniLines[i].Trim();
                if (trimmed.StartsWith("["))
                {
                    if (inSection)
                    {
                        _iniLines.Insert(i, $"{key} = {value}");
                        return;
                    }

                    inSection = string.Equals(trimmed, header, StringComparison.OrdinalIgnoreCase);
                    continue;
                }

                if (!inSection || string.IsNullOrWhiteSpace(trimmed) || trimmed[0] == ';' || trimmed[0] == '#')
                {
                    continue;
                }

                int eq = trimmed.IndexOf('=');
                if (eq < 0)
                {
                    continue;
                }

                string currentKey = trimmed[..eq].Trim();
                if (string.Equals(currentKey, key, StringComparison.OrdinalIgnoreCase))
                {
                    _iniLines[i] = $"{key} = {value}";
                    return;
                }
            }

            for (int i = 0; i < _iniLines.Count; i++)
            {
                if (string.Equals(_iniLines[i].Trim(), header, StringComparison.OrdinalIgnoreCase))
                {
                    _iniLines.Insert(i + 1, $"{key} = {value}");
                    return;
                }
            }

            _iniLines.Add(string.Empty);
            _iniLines.Add(header);
            _iniLines.Add($"{key} = {value}");
        }

        private void RemoveIniKey(string section, string key)
        {
            string header = $"[{section}]";
            bool inSection = false;

            for (int i = 0; i < _iniLines.Count; i++)
            {
                string trimmed = _iniLines[i].Trim();
                if (trimmed.StartsWith("["))
                {
                    if (inSection)
                    {
                        return;
                    }

                    inSection = string.Equals(trimmed, header, StringComparison.OrdinalIgnoreCase);
                    continue;
                }

                if (!inSection || string.IsNullOrWhiteSpace(trimmed) || trimmed[0] == ';' || trimmed[0] == '#')
                {
                    continue;
                }

                int eq = trimmed.IndexOf('=');
                if (eq < 0)
                {
                    continue;
                }

                string currentKey = trimmed[..eq].Trim();
                if (string.Equals(currentKey, key, StringComparison.OrdinalIgnoreCase))
                {
                    _iniLines.RemoveAt(i);
                    return;
                }
            }
        }

        private void RemoveIniSection(string section)
        {
            string targetHeader = $"[{section}]";
            bool inTarget = false;
            var newLines = new List<string>(_iniLines.Count);

            foreach (string line in _iniLines)
            {
                string trimmed = line.Trim();
                if (trimmed.StartsWith("[") && trimmed.EndsWith("]"))
                {
                    inTarget = string.Equals(trimmed, targetHeader, StringComparison.OrdinalIgnoreCase);
                }

                if (!inTarget)
                {
                    newLines.Add(line);
                }
            }

            _iniLines = newLines;
        }

        private bool HasSection(string section)
            => _iniLines.Any(line => string.Equals(line.Trim(), $"[{section}]", StringComparison.OrdinalIgnoreCase));

        private float GetFloat(IReadOnlyDictionary<string, string> map, string key, float defaultValue)
        {
            if (map.TryGetValue($"general.{key}", out string? value) &&
                float.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out float parsed))
            {
                return parsed;
            }

            return defaultValue;
        }

        private bool GetBool(IReadOnlyDictionary<string, string> map, string key, bool defaultValue)
        {
            if (map.TryGetValue($"general.{key}", out string? value))
            {
                return ParseBoolValue(value, defaultValue);
            }

            return defaultValue;
        }

        private string GetString(IReadOnlyDictionary<string, string> map, string key, string defaultValue)
            => map.TryGetValue($"general.{key}", out string? value) ? value : defaultValue;

        private static bool ParseBoolValue(string? value, bool defaultValue = false)
        {
            if (string.IsNullOrWhiteSpace(value))
            {
                return defaultValue;
            }

            string normalized = value.Trim().ToLowerInvariant();
            return normalized is "true" or "1" or "yes";
        }

        private static string NormalizeAppName(string raw)
        {
            string name = raw.Trim().ToLowerInvariant();
            if (string.IsNullOrWhiteSpace(name))
            {
                return string.Empty;
            }

            return name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? name : $"{name}.exe";
        }

        private static string F2(double value) => value.ToString("F2", CultureInfo.InvariantCulture);
        private static string F1(double value) => value.ToString("F1", CultureInfo.InvariantCulture);
        private static string Bool(bool value) => value ? "true" : "false";

        private void LoadSettings()
        {
            _isUpdatingContext = true;

            _iniPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "puretype.ini");
            _iniLines = File.Exists(_iniPath)
                ? new List<string>(File.ReadAllLines(_iniPath))
                : new List<string>(DefaultIni.Split('\n'));

            var map = ParseIni(_iniLines);

            PopulateDisplaysCombo();
            RefreshAppsComboItems();

            SelectComboByTag(PanelTypeCombo, GetString(map, "paneltype", "qd_oled_gen3").ToLowerInvariant());

            string gammaMode = GetString(map, "gammamode", "oled").ToLowerInvariant();
            if (gammaMode == "standard") gammaMode = "srgb";
            SelectComboByTag(GammaModeCombo, gammaMode);

            FilterStrengthSlider.Value = Math.Clamp(GetFloat(map, "filterstrength", 1.0f), 0.0, 5.0);
            GammaSlider.Value = Math.Clamp(GetFloat(map, "gamma", 1.0f), 0.5, 3.0);
            OledGammaOutputSlider.Value = Math.Clamp(GetFloat(map, "oledgammaoutput", 1.0f), 1.0, 2.0);
            LumaContrastSlider.Value = Math.Clamp(GetFloat(map, "lumacontraststrength", 1.20f), 1.0, 3.0);
            WoledCrosstalkSlider.Value = Math.Clamp(GetFloat(map, "woledcrosstalkreduction", 0.08f), 0.0, 1.0);
            QdSepGen1Slider.Value = Math.Clamp(GetFloat(map, "qdexpectedsepgen1", -0.44f), -1.0, -0.1);
            QdSepGen3Slider.Value = Math.Clamp(GetFloat(map, "qdexpectedsepgen3", -0.50f), -1.0, -0.1);
            QdSepGen4Slider.Value = Math.Clamp(GetFloat(map, "qdexpectedsepgen4", -0.50f), -1.0, -0.1);
            QdVerticalBlendSlider.Value = Math.Clamp(GetFloat(map, "qdverticalblend", 0.15f), 0.0, 0.30);
            ChromaKeepQdSlider.Value = Math.Clamp(GetFloat(map, "chromakeepscaleqd", 1.00f), 0.60, 1.30);
            ChromaKeepWoledSlider.Value = Math.Clamp(GetFloat(map, "chromakeepscalewoled", 1.00f), 0.60, 1.30);
            SubpixelHintingCheck.IsChecked = GetBool(map, "enablesubpixelhinting", true);
            FractionalPositioningCheck.IsChecked = GetBool(map, "enablefractionalpositioning", false);
            StemDarkeningCheck.IsChecked = GetBool(map, "stemdarkeningenabled", true);
            StemStrengthSlider.Value = Math.Clamp(GetFloat(map, "stemdarkeningstrength", 0.45f), 0.0, 2.0);
            _toneParityV2Enabled = GetBool(map, "toneparityv2enabled", false);

            LodSmallSlider.Value = Math.Clamp(GetFloat(map, "lodthresholdsmall", 10.0f), 6.0, 96.0);
            LodLargeSlider.Minimum = LodSmallSlider.Value + 1.0;
            LodLargeSlider.Value = Math.Max(Math.Clamp(GetFloat(map, "lodthresholdlarge", 22.0f), 7.0, 160.0), LodLargeSlider.Minimum);

            HighDpiLowSlider.Value = Math.Clamp(GetFloat(map, "highdpithresholdlow", 144.0f), 96.0, 384.0);
            HighDpiHighSlider.Minimum = HighDpiLowSlider.Value + 1.0;
            HighDpiHighSlider.Value = Math.Max(Math.Clamp(GetFloat(map, "highdpithresholdhigh", 216.0f), 96.0, 600.0), HighDpiHighSlider.Minimum);

            DebugEnabledCheck.IsChecked = ParseBoolValue(map.GetValueOrDefault("debug.enabled"), false);
            LogFileText.Text = map.GetValueOrDefault("debug.logfile") ?? "PURETYPE.log";
            HighlightGlyphsCheck.IsChecked = ParseBoolValue(map.GetValueOrDefault("debug.highlightrenderedglyphs"), false);

            AutoCheckUpdatesCheck.IsChecked = GetBool(map, "autocheckupdates", true);

            using (RegistryKey? runKey = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run", false))
            {
                StartWithWindowsCheck.IsChecked = runKey?.GetValue("PureType") != null;
            }

            string blacklist = map.GetValueOrDefault("general.blacklist") ?? string.Empty;
            BlacklistText.Text = string.Join(
                Environment.NewLine,
                blacklist.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries));

            InterWeightSlider.Value = Math.Clamp(GetFloat(map, "interfontweight", 400.0f), 100.0, 900.0);
            InterOpszSlider.Value = Math.Clamp(GetFloat(map, "interopticalsize", 14.0f), 14.0, 32.0);
            InterTrackingSlider.Value = Math.Clamp(GetFloat(map, "interletterspacing", 0.0f), -2.0, 4.0);

            UpdateSliderLabels();
            UpdateInterAxisLabels();
            SyncWoledVisibility();
            SyncStemStrengthEnabled();
            SyncDebugOptions();
            SyncQuickPresetSelection();
            RefreshProfilesList();

            _isUpdatingContext = false;
        }

        private void PopulateDisplaysCombo()
        {
            if (DisplaysCombo.Items.Count > 0)
            {
                return;
            }

            DISPLAY_DEVICE device = new() { cb = Marshal.SizeOf<DISPLAY_DEVICE>() };
            for (uint index = 0; EnumDisplayDevices(null, index, ref device, 0); index++)
            {
                if ((device.StateFlags & 1) != 0)
                {
                    DisplaysCombo.Items.Add(new ComboBoxItem
                    {
                        Content = $"{device.DeviceString} ({device.DeviceName})",
                        Tag = device.DeviceName
                    });
                }

                device.cb = Marshal.SizeOf<DISPLAY_DEVICE>();
            }

            if (DisplaysCombo.Items.Count > 0)
            {
                DisplaysCombo.SelectedIndex = 0;
            }
        }

        private static void SelectComboByTag(ComboBox combo, string tag)
        {
            foreach (ComboBoxItem item in combo.Items)
            {
                if (string.Equals(item.Tag?.ToString(), tag, StringComparison.OrdinalIgnoreCase))
                {
                    combo.SelectedItem = item;
                    return;
                }
            }

            if (combo.Items.Count > 0)
            {
                combo.SelectedIndex = 0;
            }
        }

        private void UpdateSliderLabels()
        {
            FilterValText.Text = F2(FilterStrengthSlider.Value);
            GammaValText.Text = F2(GammaSlider.Value);
            OledGammaValText.Text = F2(OledGammaOutputSlider.Value);
            LumaValText.Text = F2(LumaContrastSlider.Value);
            WoledValText.Text = F2(WoledCrosstalkSlider.Value);
            StemStrValText.Text = F2(StemStrengthSlider.Value);
            QdSepGen1ValText.Text = F2(QdSepGen1Slider.Value);
            QdSepGen3ValText.Text = F2(QdSepGen3Slider.Value);
            QdSepGen4ValText.Text = F2(QdSepGen4Slider.Value);
            QdVerticalBlendValText.Text = F2(QdVerticalBlendSlider.Value);
            ChromaKeepQdValText.Text = F2(ChromaKeepQdSlider.Value);
            ChromaKeepWoledValText.Text = F2(ChromaKeepWoledSlider.Value);
            UpdateSummary();
        }

        private void GetPresetValues(string preset, string panelTag, out double filterStrength, out double oledGammaOutput, out double woledCrosstalk, out double lumaContrast, out double stemStrength)
        {
            bool isWoled = IsWoledPanel(panelTag);

            // All presets use oledGammaOutput=1.0 because gamma compensation
            // is now applied post-compositing via cfg.gamma, not on coverage masks.
            // Coverage masks are geometric coefficients and must not be gamma-encoded.
            switch (preset)
            {
                case "sharp":
                    // Prioritizes stem crispness. Stronger darkening + higher contrast.
                    filterStrength = 1.00;
                    oledGammaOutput = 1.00;
                    woledCrosstalk = isWoled ? 0.06 : 0.0;
                    lumaContrast = isWoled ? 1.35 : 1.30;
                    stemStrength = isWoled ? 0.55 : 0.55;
                    break;
                case "clean":
                    // Prioritizes color purity. Extra filtering, lighter stems.
                    filterStrength = isWoled ? 1.20 : 1.15;
                    oledGammaOutput = 1.00;
                    woledCrosstalk = isWoled ? 0.12 : 0.0;
                    lumaContrast = isWoled ? 1.10 : 1.10;
                    stemStrength = isWoled ? 0.35 : 0.35;
                    break;
                default:
                    // Balanced: scientifically correct defaults.
                    // filterStrength=1.0 (full OLED geometric correction).
                    // stemStrength=0.45 (FreeType/Apple consistent for OLED).
                    // lumaContrast=1.20 (exp≈1.11, 5-7% mid-tone boost per Legge & Foley 1980).
                    filterStrength = 1.00;
                    oledGammaOutput = 1.00;
                    woledCrosstalk = isWoled ? 0.08 : 0.0;
                    lumaContrast = 1.20;
                    stemStrength = 0.45;
                    break;
            }
        }

        private static bool NearlyEqual(double left, double right, double epsilon = 0.011)
            => Math.Abs(left - right) <= epsilon;

        private string? DetectMatchingQuickPreset()
        {
            string panelTag = GetCurrentPanelTag();

            if (!NearlyEqual(GammaSlider.Value, 1.0) ||
                SubpixelHintingCheck.IsChecked != true ||
                StemDarkeningCheck.IsChecked != true)
            {
                return null;
            }

            foreach (string preset in new[] { "balanced", "sharp", "clean" })
            {
                GetPresetValues(preset, panelTag, out double filterStrength, out double oledGammaOutput, out double woledCrosstalk, out double lumaContrast, out double stemStrength);

                bool matches = NearlyEqual(FilterStrengthSlider.Value, filterStrength)
                    && NearlyEqual(OledGammaOutputSlider.Value, oledGammaOutput)
                    && NearlyEqual(LumaContrastSlider.Value, lumaContrast)
                    && NearlyEqual(StemStrengthSlider.Value, stemStrength);

                if (IsWoledPanel(panelTag))
                {
                    matches = matches && NearlyEqual(WoledCrosstalkSlider.Value, woledCrosstalk);
                }

                if (matches)
                {
                    return preset;
                }
            }

            return null;
        }

        private void SetQuickPresetSelection(string? preset)
        {
            _activeQuickPreset = preset;
            PresetBalancedButton.IsChecked = string.Equals(preset, "balanced", StringComparison.OrdinalIgnoreCase);
            PresetSharpButton.IsChecked = string.Equals(preset, "sharp", StringComparison.OrdinalIgnoreCase);
            PresetCleanButton.IsChecked = string.Equals(preset, "clean", StringComparison.OrdinalIgnoreCase);
            PresetCustomButton.IsChecked = string.IsNullOrWhiteSpace(preset);
        }

        private void SyncQuickPresetSelection()
        {
            SetQuickPresetSelection(DetectMatchingQuickPreset());
        }

        private void UpdateSummary()
        {
            SummaryPanel.Text = (PanelTypeCombo.SelectedItem as ComboBoxItem)?.Content?.ToString() ?? "—";
            SummaryFilter.Text = F2(FilterStrengthSlider.Value);
            SummaryGamma.Text = $"{((GammaModeCombo.SelectedItem as ComboBoxItem)?.Content?.ToString() ?? "—")} · {F2(GammaSlider.Value)}";
            SummaryStem.Text = StemDarkeningCheck.IsChecked == true
                ? $"On · {F2(StemStrengthSlider.Value)}"
                : "Off";
        }

        private void SaveSettings()
        {
            SetIniValue("general", "panelType", (PanelTypeCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "qd_oled_gen3");
            SetIniValue("general", "gammaMode", (GammaModeCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "oled");
            SetIniValue("general", "filterStrength", F2(FilterStrengthSlider.Value));
            SetIniValue("general", "gamma", F2(GammaSlider.Value));
            SetIniValue("general", "oledGammaOutput", F2(OledGammaOutputSlider.Value));
            SetIniValue("general", "enableSubpixelHinting", Bool(SubpixelHintingCheck.IsChecked == true));
            SetIniValue("general", "enableFractionalPositioning", Bool(FractionalPositioningCheck.IsChecked == true));
            SetIniValue("general", "lumaContrastStrength", F2(LumaContrastSlider.Value));
            SetIniValue("general", "qdExpectedSepGen1", F2(QdSepGen1Slider.Value));
            SetIniValue("general", "qdExpectedSepGen3", F2(QdSepGen3Slider.Value));
            SetIniValue("general", "qdExpectedSepGen4", F2(QdSepGen4Slider.Value));
            SetIniValue("general", "qdVerticalBlend", F2(QdVerticalBlendSlider.Value));
            SetIniValue("general", "chromaKeepScaleQD", F2(ChromaKeepQdSlider.Value));
            SetIniValue("general", "chromaKeepScaleWOLED", F2(ChromaKeepWoledSlider.Value));
            SetIniValue("general", "stemDarkeningEnabled", Bool(StemDarkeningCheck.IsChecked == true));
            SetIniValue("general", "stemDarkeningStrength", F2(StemStrengthSlider.Value));
            SetIniValue("general", "woledCrossTalkReduction", F2(WoledCrosstalkSlider.Value));
            SetIniValue("general", "lodThresholdSmall", F2(LodSmallSlider.Value));
            SetIniValue("general", "lodThresholdLarge", F2(LodLargeSlider.Value));
            SetIniValue("general", "highDpiThresholdLow", F2(HighDpiLowSlider.Value));
            SetIniValue("general", "highDpiThresholdHigh", F2(HighDpiHighSlider.Value));
            SetIniValue("general", "interFontWeight", ((int)InterWeightSlider.Value).ToString(CultureInfo.InvariantCulture));
            SetIniValue("general", "interOpticalSize", ((int)InterOpszSlider.Value).ToString(CultureInfo.InvariantCulture));
            SetIniValue("general", "interLetterSpacing", F1(InterTrackingSlider.Value));
            SetIniValue("general", "blacklist", string.Join(", ", BlacklistText.Text.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)));
            SetIniValue("general", "autoCheckUpdates", Bool(AutoCheckUpdatesCheck.IsChecked == true));

            SetIniValue("debug", "enabled", Bool(DebugEnabledCheck.IsChecked == true));
            SetIniValue("debug", "logFile", string.IsNullOrWhiteSpace(LogFileText.Text) ? "PURETYPE.log" : LogFileText.Text.Trim());
            SetIniValue("debug", "highlightRenderedGlyphs", Bool(HighlightGlyphsCheck.IsChecked == true));

            using (RegistryKey? runKey = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run", true))
            {
                if (runKey != null)
                {
                    string exePath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "puretype.exe");
                    if (StartWithWindowsCheck.IsChecked == true)
                    {
                        runKey.SetValue("PureType", $"\"{exePath}\"");
                    }
                    else
                    {
                        runKey.DeleteValue("PureType", false);
                    }
                }
            }

            File.WriteAllLines(_iniPath, _iniLines);
        }

        private void UpdatePreview()
        {
            if (!_dllAvailable || _previewBuffer == IntPtr.Zero || _writeableBitmap is null)
            {
                return;
            }

            try
            {
                if (PanelTypeCombo.SelectedItem is not ComboBoxItem panelItem)
                {
                    return;
                }

                int panelType = panelItem.Tag?.ToString() switch
                {
                    "rwbg" => 0,
                    "rgwb" => 1,
                    "qd_oled_gen1" => 2,
                    "qd_oled_gen3" => 3,
                    "qd_oled_gen4" => 4,
                    _ => 3
                };

                int gammaMode = (GammaModeCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() == "oled" ? 1 : 0;
                const string sample = "The quick brown fox jumps over the lazy dog. Lorem ipsum dolor sit amet, consectetur adipiscing elit.";

                bool ok;
                IntPtr sampleAnsi = IntPtr.Zero;
                IntPtr fontPathAnsi = IntPtr.Zero;

                bool GeneratePreviewV1Fallback()
                {
                    return GeneratePreview(
                        sample,
                        FontPath,
                        24,
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
                        true,
                        PreviewRenderWidth,
                        PreviewRenderHeight,
                        _previewBuffer);
                }

                try
                {
                    if (_previewV2Available != false)
                    {
                        sampleAnsi = Marshal.StringToHGlobalAnsi(sample);
                        fontPathAnsi = Marshal.StringToHGlobalAnsi(FontPath);

                        PreviewParamsV2 previewParams = new()
                        {
                            size = (uint)Marshal.SizeOf<PreviewParamsV2>(),
                            version = PreviewParamsV2Version,
                            text = sampleAnsi,
                            fontPath = fontPathAnsi,
                            fontSize = 24,
                            filterStrength = (float)FilterStrengthSlider.Value,
                            gamma = (float)GammaSlider.Value,
                            gammaMode = gammaMode,
                            oledGammaOutput = (float)OledGammaOutputSlider.Value,
                            lumaContrastStrength = (float)LumaContrastSlider.Value,
                            woledCrossTalkReduction = (float)WoledCrosstalkSlider.Value,
                            enableSubpixelHinting = SubpixelHintingCheck.IsChecked == true ? 1u : 0u,
                            enableFractionalPositioning = FractionalPositioningCheck.IsChecked == true ? 1u : 0u,
                            stemDarkeningEnabled = StemDarkeningCheck.IsChecked == true ? 1u : 0u,
                            stemDarkeningStrength = (float)StemStrengthSlider.Value,
                            panelType = panelType,
                            useMeasuredContrast = 1u,
                            width = PreviewRenderWidth,
                            height = PreviewRenderHeight,
                            pBuffer = _previewBuffer,
                            lodThresholdSmall = (float)LodSmallSlider.Value,
                            lodThresholdLarge = (float)LodLargeSlider.Value,
                            toneParityV2Enabled = _toneParityV2Enabled ? 1u : 0u
                        };

                        try
                        {
                            bool v2Ok = GeneratePreviewV2(ref previewParams, previewParams.size);
                            if (v2Ok)
                            {
                                _previewV2Available = true;
                                ok = true;
                            }
                            else
                            {
                                _previewV2Available = false;
                                ok = GeneratePreviewV1Fallback();
                            }
                        }
                        catch (EntryPointNotFoundException)
                        {
                            _previewV2Available = false;
                            ok = GeneratePreviewV1Fallback();
                        }
                    }
                    else
                    {
                        ok = GeneratePreviewV1Fallback();
                    }
                }
                finally
                {
                    if (sampleAnsi != IntPtr.Zero)
                    {
                        Marshal.FreeHGlobal(sampleAnsi);
                    }

                    if (fontPathAnsi != IntPtr.Zero)
                    {
                        Marshal.FreeHGlobal(fontPathAnsi);
                    }
                }

                if (!ok)
                {
                    return;
                }

                _writeableBitmap.Lock();
                unsafe
                {
                    Buffer.MemoryCopy(
                        (void*)_previewBuffer,
                        (void*)_writeableBitmap.BackBuffer,
                        PreviewRenderWidth * PreviewRenderHeight * 4,
                        PreviewRenderWidth * PreviewRenderHeight * 4);
                }
                _writeableBitmap.AddDirtyRect(new Int32Rect(0, 0, PreviewRenderWidth, PreviewRenderHeight));
                _writeableBitmap.Unlock();

                PreviewImage.Visibility = Visibility.Visible;
                PreviewFallback.Visibility = Visibility.Collapsed;
            }
            catch (DllNotFoundException)
            {
                _dllAvailable = false;
                PreviewImage.Visibility = Visibility.Collapsed;
                PreviewFallback.Visibility = Visibility.Visible;
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"Preview error: {ex.Message}");
            }
        }

        private void SyncWoledVisibility()
        {
            string panelTag = (PanelTypeCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "qd_oled_gen3";
            WoledPanel.Visibility = panelTag is "rwbg" or "rgwb" ? Visibility.Visible : Visibility.Collapsed;
        }

        private void SyncStemStrengthEnabled()
        {
            bool enabled = StemDarkeningCheck.IsChecked == true;
            StemStrengthPanel.Opacity = enabled ? 1.0 : 0.4;
            StemStrengthSlider.IsEnabled = enabled;
        }

        private void SyncDebugOptions()
        {
            DebugOptionsPanel.Visibility = DebugEnabledCheck.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;
        }

        private void RefreshAppsComboItems()
        {
            _allAppNames.Clear();
            var appNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

            foreach (string line in _iniLines)
            {
                string trimmed = line.Trim();
                if (trimmed.StartsWith("[app_", StringComparison.OrdinalIgnoreCase) && trimmed.EndsWith("]"))
                {
                    string name = NormalizeAppName(trimmed[5..^1]);
                    if (!string.IsNullOrEmpty(name))
                    {
                        appNames.Add(name);
                    }
                }
            }

            foreach (Process process in Process.GetProcesses())
            {
                try
                {
                    string name = NormalizeAppName(process.ProcessName);
                    if (!string.IsNullOrEmpty(name))
                    {
                        appNames.Add(name);
                    }
                }
                catch
                {
                }
                finally
                {
                    process.Dispose();
                }
            }

            _allAppNames.AddRange(appNames.OrderBy(static v => v, StringComparer.OrdinalIgnoreCase));
            AppsCombo.ItemsSource = null;
            AppsCombo.ItemsSource = _allAppNames;
        }

        private void RefreshProfilesList()
        {
            ProfilesContainer.Children.Clear();
            var map = ParseIni(_iniLines);
            var sections = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

            foreach (string key in map.Keys)
            {
                int dot = key.LastIndexOf('.');
                if (dot > 0)
                {
                    sections.Add(key[..dot]);
                }
            }

            foreach (string line in _iniLines)
            {
                string trimmed = line.Trim();
                if (trimmed.StartsWith("[") && trimmed.EndsWith("]"))
                {
                    string section = trimmed[1..^1].Trim().ToLowerInvariant();
                    if (section.StartsWith("app_") || section.StartsWith("monitor_"))
                    {
                        sections.Add(section);
                    }
                }
            }

            foreach (string section in sections.OrderBy(static s => s, StringComparer.OrdinalIgnoreCase))
            {
                bool isMonitor = section.StartsWith("monitor_", StringComparison.OrdinalIgnoreCase);
                bool isApp = section.StartsWith("app_", StringComparison.OrdinalIgnoreCase);
                if (!isMonitor && !isApp)
                {
                    continue;
                }

                string name = isMonitor ? section[8..] : section[4..];
                int overrideCount = map.Keys.Count(key => key.StartsWith(section + ".", StringComparison.OrdinalIgnoreCase));
                string summary = overrideCount == 0
                    ? "header only"
                    : string.Join(", ", map.Keys
                        .Where(key => key.StartsWith(section + ".", StringComparison.OrdinalIgnoreCase))
                        .Select(key => key[(section.Length + 1)..])
                        .Take(3));

                var card = new Border
                {
                    Background = string.Equals(section, _editingProfileSection, StringComparison.OrdinalIgnoreCase)
                        ? new SolidColorBrush(Color.FromArgb(0x18, 0x00, 0x78, 0xD4))
                        : (Brush)Application.Current.Resources["BrushSurfaceVariant"],
                    BorderBrush = string.Equals(section, _editingProfileSection, StringComparison.OrdinalIgnoreCase)
                        ? (Brush)Application.Current.Resources["BrushAccent"]
                        : (Brush)Application.Current.Resources["BrushBorder"],
                    BorderThickness = new Thickness(1),
                    CornerRadius = new CornerRadius(6),
                    Padding = new Thickness(14, 10, 14, 10),
                    Margin = new Thickness(0, 0, 0, 6)
                };

                var grid = new Grid();
                grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

                var badge = new Border
                {
                    Background = isMonitor
                        ? (Brush)Application.Current.Resources["BrushAccent"]
                        : new SolidColorBrush(Color.FromRgb(0x2E, 0xA0, 0x43)),
                    CornerRadius = new CornerRadius(4),
                    Padding = new Thickness(8, 3, 8, 3),
                    Margin = new Thickness(0, 0, 12, 0),
                    VerticalAlignment = VerticalAlignment.Center,
                    Child = new TextBlock
                    {
                        Text = isMonitor ? "Monitor" : "App",
                        Foreground = Brushes.White,
                        FontSize = 10,
                        FontWeight = FontWeights.Bold
                    }
                };
                Grid.SetColumn(badge, 0);
                grid.Children.Add(badge);

                var namePanel = new StackPanel();
                namePanel.Children.Add(new TextBlock
                {
                    Text = name,
                    FontSize = 13,
                    FontWeight = FontWeights.SemiBold,
                    Foreground = (Brush)Application.Current.Resources["BrushTextPrimary"]
                });
                namePanel.Children.Add(new TextBlock
                {
                    Text = $"{overrideCount} override{(overrideCount == 1 ? string.Empty : "s")} — {summary}",
                    FontSize = 11,
                    Foreground = (Brush)Application.Current.Resources["BrushTextSecondary"]
                });
                Grid.SetColumn(namePanel, 1);
                grid.Children.Add(namePanel);

                Button editButton = CreateProfileActionButton("\uE70F", (Brush)Application.Current.Resources["BrushAccent"], section, ProfileEdit_Click);
                Grid.SetColumn(editButton, 2);
                grid.Children.Add(editButton);

                Button deleteButton = CreateProfileActionButton("\uE74D", new SolidColorBrush(Color.FromRgb(0xE8, 0x11, 0x23)), section, ProfileDelete_Click);
                Grid.SetColumn(deleteButton, 3);
                grid.Children.Add(deleteButton);

                card.Child = grid;
                ProfilesContainer.Children.Add(card);
            }

            ProfileCountText.Text = ProfilesContainer.Children.Count.ToString(CultureInfo.InvariantCulture);
            ProfileCountBadge.Visibility = ProfilesContainer.Children.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
            NoProfilesHint.Visibility = ProfilesContainer.Children.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        }

        private Button CreateProfileActionButton(string glyph, Brush foreground, string tag, RoutedEventHandler clickHandler)
        {
            var button = new Button
            {
                Style = (Style)FindResource("BtnGhost"),
                Padding = new Thickness(8, 4, 8, 4),
                Margin = new Thickness(4, 0, 0, 0),
                Tag = tag,
                VerticalAlignment = VerticalAlignment.Center,
                Content = new TextBlock
                {
                    Text = glyph,
                    FontFamily = new FontFamily("Segoe MDL2 Assets"),
                    FontSize = 13,
                    Foreground = foreground
                }
            };
            button.Click += clickHandler;
            return button;
        }

        private string GetSelectedMonitorSection()
        {
            if (DisplaysCombo.SelectedItem is not ComboBoxItem selected)
            {
                return string.Empty;
            }

            string deviceName = selected.Tag?.ToString() ?? string.Empty;
            string normalized = new string(deviceName
                .Where(c => c != '\\' && c != '.' && !char.IsWhiteSpace(c))
                .ToArray());
            return $"monitor_{normalized}".ToLowerInvariant();
        }

        private string GetCurrentPanelTag()
            => (PanelTypeCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "qd_oled_gen3";

        private static bool IsWoledPanel(string tag)
            => tag is "rwbg" or "rgwb";

        private void ProfileEdit_Click(object sender, RoutedEventArgs e)
        {
            if (sender is Button button && button.Tag is string section)
            {
                OpenProfileEditor(section);
            }
        }

        private void ProfileDelete_Click(object sender, RoutedEventArgs e)
        {
            if (sender is Button button && button.Tag is string section)
            {
                DeleteProfileSection(section);
            }
        }

        private void DeleteProfile_Click(object sender, RoutedEventArgs e)
        {
            if (!string.IsNullOrWhiteSpace(_editingProfileSection))
            {
                DeleteProfileSection(_editingProfileSection);
            }
        }

        private void DeleteProfileSection(string section)
        {
            bool isApp = section.StartsWith("app_", StringComparison.OrdinalIgnoreCase);
            string name = isApp ? section[4..] : section.StartsWith("monitor_", StringComparison.OrdinalIgnoreCase) ? section[8..] : section;

            MessageBoxResult result = MessageBox.Show(
                this,
                $"Delete profile \"{name}\"?",
                "PureType",
                MessageBoxButton.YesNo,
                MessageBoxImage.Question);

            if (result != MessageBoxResult.Yes)
            {
                return;
            }

            RemoveIniSection(section);
            File.WriteAllLines(_iniPath, _iniLines);
            SignalTrayReload();

            if (string.Equals(_editingProfileSection, section, StringComparison.OrdinalIgnoreCase))
            {
                _editingProfileSection = string.Empty;
                ProfileEditCard.Visibility = Visibility.Collapsed;
            }

            if (isApp)
            {
                RefreshAppsComboItems();
            }

            RefreshProfilesList();
            StatusConfigText.Text = $"Profile deleted: {name}";
        }

        private void OpenProfileEditor(string section)
        {
            _editingProfileSection = section;
            var map = ParseIni(_iniLines);
            bool isMonitor = section.StartsWith("monitor_", StringComparison.OrdinalIgnoreCase);
            string name = isMonitor ? section[8..] : section.StartsWith("app_", StringComparison.OrdinalIgnoreCase) ? section[4..] : section;

            _isUpdatingContext = true;
            ProfileEditTitle.Text = $"Editing {(isMonitor ? "Monitor" : "App")}: {name}";
            ProfileEditIcon.Text = isMonitor ? "\uE7F4" : "\uE71E";
            ProfileEditCard.Visibility = Visibility.Visible;

            SelectComboByTag(ProfPanelTypeCombo, GetSectionValue(map, section, "paneltype") ?? string.Empty);

            string? gammaMode = GetSectionValue(map, section, "gammamode");
            if (string.Equals(gammaMode, "standard", StringComparison.OrdinalIgnoreCase)) gammaMode = "srgb";
            SelectComboByTag(ProfGammaModeCombo, gammaMode ?? string.Empty);

            ProfFilterSlider.Value = ParseSectionFloat(map, section, "filterstrength", FilterStrengthSlider.Value, 0.0, 5.0);
            ProfGammaSlider.Value = ParseSectionFloat(map, section, "gamma", GammaSlider.Value, 0.5, 3.0);
            ProfStemSlider.Value = ParseSectionFloat(map, section, "stemdarkeningstrength", StemStrengthSlider.Value, 0.0, 2.0);
            ProfWoledSlider.Value = ParseSectionFloat(map, section, "woledcrosstalkreduction", WoledCrosstalkSlider.Value, 0.0, 1.0);
            ProfStemDarkeningCheck.IsChecked = GetSectionValue(map, section, "stemdarkeningenabled") is { } stemValue
                ? ParseBoolValue(stemValue, StemDarkeningCheck.IsChecked == true)
                : StemDarkeningCheck.IsChecked;

            ProfFilterValText.Text = F2(ProfFilterSlider.Value) + (GetSectionValue(map, section, "filterstrength") == null ? " (global)" : string.Empty);
            ProfGammaValText.Text = F2(ProfGammaSlider.Value) + (GetSectionValue(map, section, "gamma") == null ? " (global)" : string.Empty);
            ProfStemStrValText.Text = F2(ProfStemSlider.Value) + (GetSectionValue(map, section, "stemdarkeningstrength") == null ? " (global)" : string.Empty);
            ProfWoledValText.Text = F2(ProfWoledSlider.Value) + (GetSectionValue(map, section, "woledcrosstalkreduction") == null ? " (global)" : string.Empty);

            _isUpdatingContext = false;
            RefreshProfilesList();
            ProfileEditCard.BringIntoView();
        }

        private static double ParseSectionFloat(IReadOnlyDictionary<string, string> map, string section, string key, double fallback, double min, double max)
        {
            string? value = GetSectionValue(map, section, key);
            return value != null && double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out double parsed)
                ? Math.Clamp(parsed, min, max)
                : fallback;
        }

        private void CreateMonitorProfile_Click(object sender, RoutedEventArgs e)
        {
            string section = GetSelectedMonitorSection();
            if (string.IsNullOrWhiteSpace(section))
            {
                MessageBox.Show(this, "Select a display first.", "PureType", MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }

            if (!HasSection(section))
            {
                _iniLines.Add(string.Empty);
                _iniLines.Add($"[{section}]");
                _iniLines.Add("; Monitor-specific overrides");
                File.WriteAllLines(_iniPath, _iniLines);
            }

            RefreshProfilesList();
            OpenProfileEditor(section);
            StatusConfigText.Text = "Monitor profile created";
        }

        private void AddAppProfile_Click(object sender, RoutedEventArgs e)
        {
            string appName = NormalizeAppName(AppsCombo.Text);
            if (string.IsNullOrWhiteSpace(appName))
            {
                MessageBox.Show(this, "Enter a process name (for example: notepad.exe).", "PureType", MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }

            string section = $"app_{appName}";
            if (!HasSection(section))
            {
                _iniLines.Add(string.Empty);
                _iniLines.Add($"[{section}]");
                _iniLines.Add($"; Override settings for {appName}");
                File.WriteAllLines(_iniPath, _iniLines);
            }

            RefreshAppsComboItems();
            RefreshProfilesList();
            OpenProfileEditor(section);
            StatusConfigText.Text = $"App profile created: {appName}";
        }

        private void SaveProfile_Click(object sender, RoutedEventArgs e)
        {
            if (string.IsNullOrWhiteSpace(_editingProfileSection))
            {
                return;
            }

            try
            {
                string section = _editingProfileSection;

                string panelTag = (ProfPanelTypeCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? string.Empty;
                if (string.IsNullOrEmpty(panelTag)) RemoveIniKey(section, "panelType");
                else SetIniValue(section, "panelType", panelTag);

                string gammaTag = (ProfGammaModeCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? string.Empty;
                if (string.IsNullOrEmpty(gammaTag)) RemoveIniKey(section, "gammaMode");
                else SetIniValue(section, "gammaMode", gammaTag);

                SetIniValue(section, "filterStrength", F2(ProfFilterSlider.Value));
                SetIniValue(section, "gamma", F2(ProfGammaSlider.Value));
                SetIniValue(section, "stemDarkeningEnabled", Bool(ProfStemDarkeningCheck.IsChecked == true));
                SetIniValue(section, "stemDarkeningStrength", F2(ProfStemSlider.Value));
                SetIniValue(section, "woledCrossTalkReduction", F2(ProfWoledSlider.Value));

                File.WriteAllLines(_iniPath, _iniLines);
                SignalTrayReload();

                string name = section.StartsWith("app_", StringComparison.OrdinalIgnoreCase) ? section[4..] : section.StartsWith("monitor_", StringComparison.OrdinalIgnoreCase) ? section[8..] : section;
                _editingProfileSection = string.Empty;
                ProfileEditCard.Visibility = Visibility.Collapsed;
                RefreshProfilesList();
                StatusConfigText.Text = $"Profile saved: {name}";
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, $"Save error:\n{ex.Message}", "PureType", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void CloseProfileEdit_Click(object sender, RoutedEventArgs e)
        {
            _editingProfileSection = string.Empty;
            ProfileEditCard.Visibility = Visibility.Collapsed;
            RefreshProfilesList();
        }

        private void ProfSlider_Changed(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (!IsLoaded || _isUpdatingContext) return;
            ProfFilterValText.Text = F2(ProfFilterSlider.Value);
            ProfGammaValText.Text = F2(ProfGammaSlider.Value);
            ProfStemStrValText.Text = F2(ProfStemSlider.Value);
            ProfWoledValText.Text = F2(ProfWoledSlider.Value);
        }

        private void Setting_Changed_Check(object sender, RoutedEventArgs e)
        {
            if (!IsLoaded || _isUpdatingContext) return;
            SyncWoledVisibility();
            UpdateSliderLabels();
            SyncQuickPresetSelection();
            UpdatePreview();
        }

        private void Setting_Changed_Combo(object sender, SelectionChangedEventArgs e)
        {
            if (!IsLoaded || _isUpdatingContext) return;
            SyncWoledVisibility();
            UpdateSliderLabels();
            UpdateInterHints();
            SyncQuickPresetSelection();
            UpdatePreview();
        }

        private void Setting_Changed_Slider(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (!IsLoaded || _isUpdatingContext) return;
            UpdateSliderLabels();
            SyncQuickPresetSelection();
            UpdatePreview();
        }

        private void StemDarkening_Changed(object sender, RoutedEventArgs e)
        {
            if (!IsLoaded || _isUpdatingContext) return;
            SyncStemStrengthEnabled();
            UpdateSliderLabels();
            SyncQuickPresetSelection();
            UpdatePreview();
        }

        private void Debug_Changed(object sender, RoutedEventArgs e)
        {
            if (!IsLoaded || _isUpdatingContext) return;
            SyncDebugOptions();
        }

        private void LodSmall_Changed(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (!IsLoaded) return;
            LodLargeSlider.Minimum = LodSmallSlider.Value + 1.0;
            if (LodLargeSlider.Value < LodLargeSlider.Minimum)
            {
                LodLargeSlider.Value = LodLargeSlider.Minimum;
            }
        }

        private void DpiLow_Changed(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (!IsLoaded) return;
            HighDpiHighSlider.Minimum = HighDpiLowSlider.Value + 1.0;
            if (HighDpiHighSlider.Value < HighDpiHighSlider.Minimum)
            {
                HighDpiHighSlider.Value = HighDpiHighSlider.Minimum;
            }
        }

        private void ApplyPreset_Click(object sender, RoutedEventArgs e)
        {
            if (!IsLoaded || sender is not FrameworkElement element)
            {
                return;
            }

            string preset = element.Tag?.ToString()?.ToLowerInvariant() ?? "balanced";
            string panelTag = GetCurrentPanelTag();
            GetPresetValues(preset, panelTag, out double filterStrength, out double oledGammaOutput, out double woledCrosstalk, out double lumaContrast, out double stemStrength);

            GammaSlider.Value = 1.0;
            SubpixelHintingCheck.IsChecked = true;
            FractionalPositioningCheck.IsChecked = false;
            StemDarkeningCheck.IsChecked = true;
            FilterStrengthSlider.Value = filterStrength;
            OledGammaOutputSlider.Value = oledGammaOutput;
            WoledCrosstalkSlider.Value = woledCrosstalk;
            LumaContrastSlider.Value = lumaContrast;
            StemStrengthSlider.Value = stemStrength;

            SyncWoledVisibility();
            SyncStemStrengthEnabled();
            UpdateSliderLabels();
            SetQuickPresetSelection(preset);
            UpdatePreview();
        }

        private void ResetAdvancedDefaults_Click(object sender, RoutedEventArgs e)
        {
            if (!IsLoaded)
            {
                return;
            }

            _isUpdatingContext = true;
            QdSepGen1Slider.Value = -0.44;
            QdSepGen3Slider.Value = -0.50;
            QdSepGen4Slider.Value = -0.50;
            QdVerticalBlendSlider.Value = 0.15;
            ChromaKeepQdSlider.Value = 1.00;
            ChromaKeepWoledSlider.Value = 1.00;
            _isUpdatingContext = false;

            UpdateSliderLabels();
            SyncQuickPresetSelection();
            UpdatePreview();
            StatusConfigText.Text = "Advanced defaults restored";
        }

        private void Save_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                SaveSettings();
                SignalTrayReload();
                StatusConfigText.Text = "Config applied";

                if (sender is Button button)
                {
                    object? originalContent = button.Content;
                    button.Content = "✓ Saved";
                    button.IsEnabled = false;

                    var timer = new System.Windows.Threading.DispatcherTimer
                    {
                        Interval = TimeSpan.FromSeconds(1.5)
                    };
                    timer.Tick += (_, _) =>
                    {
                        timer.Stop();
                        button.Content = originalContent;
                        button.IsEnabled = true;
                    };
                    timer.Start();
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, $"Save error:\n{ex.Message}", "PureType", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void InstallInterFont_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                if (!File.Exists(InterRegularPath))
                {
                    MessageBox.Show(this, $"Inter font file not found:\n{InterRegularPath}", "PureType", MessageBoxButton.OK, MessageBoxImage.Warning);
                    return;
                }

                string fontsDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "Fonts");
                string destRegular = Path.Combine(fontsDir, Path.GetFileName(InterRegularPath));
                string destItalic = Path.Combine(fontsDir, Path.GetFileName(InterItalicPath));
                string registryPath = @"HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts";
                string metricsPath = @"HKCU:\Control Panel\Desktop\WindowMetrics";
                string scriptPath = Path.Combine(Path.GetTempPath(), "puretype_install_font.ps1");

                var sb = new StringBuilder();
                sb.AppendLine($"Copy-Item -Path '{InterRegularPath}' -Destination '{destRegular}' -Force");
                if (File.Exists(InterItalicPath))
                {
                    sb.AppendLine($"Copy-Item -Path '{InterItalicPath}' -Destination '{destItalic}' -Force");
                }
                sb.AppendLine($"Set-ItemProperty -Path '{registryPath}' -Name 'Inter Variable (TrueType)' -Value '{Path.GetFileName(InterRegularPath)}'");
                if (File.Exists(InterItalicPath))
                {
                    sb.AppendLine($"Set-ItemProperty -Path '{registryPath}' -Name 'Inter Variable Italic (TrueType)' -Value '{Path.GetFileName(InterItalicPath)}'");
                }
                sb.AppendLine("New-Item -Path 'HKCU:\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts' -Force | Out-Null");
                sb.AppendLine("Set-ItemProperty -Path 'HKCU:\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts' -Name 'Segoe UI (TrueType)' -Value '' -Force");
                sb.AppendLine("Set-ItemProperty -Path 'HKCU:\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts' -Name 'Segoe UI Bold (TrueType)' -Value '' -Force");
                sb.AppendLine("Set-ItemProperty -Path 'HKCU:\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts' -Name 'Segoe UI Semibold (TrueType)' -Value '' -Force");
                sb.AppendLine($"New-ItemProperty -Path '{metricsPath}' -Name 'FontFace' -Value 'Inter Variable' -PropertyType String -Force | Out-Null");
                File.WriteAllText(scriptPath, sb.ToString());

                using Process? process = Process.Start(new ProcessStartInfo
                {
                    FileName = "powershell.exe",
                    Arguments = $"-ExecutionPolicy Bypass -File \"{scriptPath}\"",
                    Verb = "runas",
                    UseShellExecute = true,
                    WindowStyle = ProcessWindowStyle.Hidden
                });

                process?.WaitForExit(15000);
                InterFontStatusText.Text = "✓ Inter installed as system font. Sign out and back in to apply.";
                InterFontStatusText.Foreground = (Brush)Application.Current.Resources["BrushSuccess"];
            }
            catch (System.ComponentModel.Win32Exception)
            {
                InterFontStatusText.Text = "Installation cancelled.";
                InterFontStatusText.Foreground = (Brush)Application.Current.Resources["BrushTextSecondary"];
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, $"Font install error:\n{ex.Message}", "PureType", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void UninstallInterFont_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                string metricsPath = @"HKCU:\Control Panel\Desktop\WindowMetrics";
                string scriptPath = Path.Combine(Path.GetTempPath(), "puretype_restore_font.ps1");
                var sb = new StringBuilder();
                sb.AppendLine("Remove-ItemProperty -Path 'HKCU:\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts' -Name 'Segoe UI (TrueType)' -ErrorAction SilentlyContinue");
                sb.AppendLine("Remove-ItemProperty -Path 'HKCU:\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts' -Name 'Segoe UI Bold (TrueType)' -ErrorAction SilentlyContinue");
                sb.AppendLine("Remove-ItemProperty -Path 'HKCU:\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts' -Name 'Segoe UI Semibold (TrueType)' -ErrorAction SilentlyContinue");
                sb.AppendLine($"Remove-ItemProperty -Path '{metricsPath}' -Name 'FontFace' -ErrorAction SilentlyContinue");
                File.WriteAllText(scriptPath, sb.ToString());

                using Process? process = Process.Start(new ProcessStartInfo
                {
                    FileName = "powershell.exe",
                    Arguments = $"-ExecutionPolicy Bypass -File \"{scriptPath}\"",
                    UseShellExecute = true,
                    WindowStyle = ProcessWindowStyle.Hidden
                });

                process?.WaitForExit(15000);
                InterFontStatusText.Text = "✓ System font restored. Sign out and back in to apply.";
                InterFontStatusText.Foreground = (Brush)Application.Current.Resources["BrushSuccess"];
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, $"Font restore error:\n{ex.Message}", "PureType", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void InterAxis_Changed(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (!IsLoaded || _isUpdatingContext) return;
            UpdateInterAxisLabels();
            UpdateInterPreview();
        }

        private void UpdateInterAxisLabels()
        {
            InterWeightValText.Text = ((int)InterWeightSlider.Value).ToString(CultureInfo.InvariantCulture);
            InterOpszValText.Text = ((int)InterOpszSlider.Value).ToString(CultureInfo.InvariantCulture);
            InterTrackingValText.Text = F1(InterTrackingSlider.Value);
        }

        private void ApplyRecommendedFontAxes_Click(object sender, RoutedEventArgs e)
        {
            string panelTag = GetCurrentPanelTag();
            _isUpdatingContext = true;

            if (IsWoledPanel(panelTag))
            {
                InterWeightSlider.Value = 420;
                InterOpszSlider.Value = 14;
                InterTrackingSlider.Value = 0.0;
            }
            else if (panelTag == "qd_oled_gen1")
            {
                InterWeightSlider.Value = 400;
                InterOpszSlider.Value = 20;
                InterTrackingSlider.Value = 0.4;
            }
            else if (panelTag == "qd_oled_gen4")
            {
                InterWeightSlider.Value = 400;
                InterOpszSlider.Value = 16;
                InterTrackingSlider.Value = 0.2;
            }
            else
            {
                InterWeightSlider.Value = 400;
                InterOpszSlider.Value = 18;
                InterTrackingSlider.Value = 0.3;
            }

            _isUpdatingContext = false;
            UpdateInterAxisLabels();
            UpdateInterHints();
            UpdateInterPreview();
        }

        private void UpdateInterHints()
        {
            string panelTag = GetCurrentPanelTag();
            if (IsWoledPanel(panelTag))
            {
                FontAxesHintText.Text = "WOLED benefits from slightly heavier text to offset white-subpixel thinning.";
                InterWeightHintText.Text = "Recommended: 410–420 weight.";
                InterOpszHintText.Text = "Standard optical size usually works well on WOLED.";
                InterTrackingHintText.Text = "Tracking can stay near zero on WOLED.";
            }
            else
            {
                string gen = panelTag switch
                {
                    "qd_oled_gen1" => "Gen 1/2",
                    "qd_oled_gen4" => "Gen 4",
                    _ => "Gen 3"
                };
                FontAxesHintText.Text = $"QD-OLED {gen} benefits from slightly increased optical size and extra spacing to reduce color fringing.";
                InterWeightHintText.Text = "Weight 400 is usually ideal on QD-OLED.";
                InterOpszHintText.Text = "Increase optical size for cleaner counters and joins.";
                InterTrackingHintText.Text = "Try +0.2 to +0.5 px to reduce adjacent glyph fringing.";
            }
        }

        private void UpdateInterPreview()
        {
            try
            {
                FontWeight weight = InterWeightSlider.Value switch
                {
                    <= 150 => FontWeights.Thin,
                    <= 250 => FontWeights.ExtraLight,
                    <= 325 => FontWeights.Light,
                    <= 450 => FontWeights.Normal,
                    <= 550 => FontWeights.Medium,
                    <= 650 => FontWeights.SemiBold,
                    <= 750 => FontWeights.Bold,
                    <= 850 => FontWeights.ExtraBold,
                    _ => FontWeights.Black
                };

                FontFamily family = File.Exists(InterRegularPath)
                    ? new FontFamily(new Uri(new Uri(InterFontDir + Path.DirectorySeparatorChar).AbsoluteUri), "./#Inter")
                    : new FontFamily("Inter, Segoe UI Variable Display, Segoe UI");

                InterPreviewLarge.FontFamily = family;
                InterPreviewLarge.FontWeight = weight;
                InterPreviewSmall.FontFamily = family;
                InterPreviewSmall.FontWeight = weight;
            }
            catch
            {
            }
        }

        // ── Update Methods ──────────────────────────────────────────────────────

        private async void CheckForUpdates_Click(object sender, RoutedEventArgs e)
        {
            await CheckForUpdatesInternalAsync(silent: false);
        }

        private async void DownloadUpdate_Click(object sender, RoutedEventArgs e)
        {
            if (_latestUpdate is null)
                return;

            DownloadUpdateBtn.IsEnabled = false;
            CheckUpdateBtn.IsEnabled = false;
            UpdateProgressPanel.Visibility = Visibility.Visible;
            UpdateProgressText.Text = "Downloading...";
            SetUpdateProgress(0);

            _updateCts?.Cancel();
            _updateCts = new CancellationTokenSource();

            try
            {
                var progress = new Progress<double>(percent =>
                {
                    Dispatcher.Invoke(() =>
                    {
                        SetUpdateProgress(percent);
                        UpdateProgressText.Text = $"Downloading... {percent:F0}%";
                    });
                });

                _updateStagingDir = await Task.Run(
                    () => _updateService.DownloadAndExtractAsync(
                        _latestUpdate.DownloadUrl,
                        _latestUpdate.ExpectedSha256,
                        progress,
                        _updateCts.Token),
                    _updateCts.Token);

                string verifyNote = _latestUpdate.ExpectedSha256 is not null
                    ? "Download complete — SHA-256 verified ✓"
                    : "Download complete.";
                UpdateProgressText.Text = verifyNote;
                SetUpdateProgress(100);

                MessageBoxResult result = MessageBox.Show(
                    this,
                    $"PureType v{_latestUpdate.Version} is ready to install.\n\n" +
                    "The application will close and restart automatically.\n" +
                    "All current settings will be preserved.\n\nProceed?",
                    "PureType Update",
                    MessageBoxButton.YesNo,
                    MessageBoxImage.Question);

                if (result == MessageBoxResult.Yes)
                {
                    try
                    {
                        SaveSettings();
                    }
                    catch
                    {
                        // Settings save failure should not block update
                    }

                    _updateService.InstallUpdate(_updateStagingDir);

                    // Give the PowerShell script a moment to start
                    await Task.Delay(500);
                    Application.Current.Shutdown();
                }
                else
                {
                    UpdateProgressPanel.Visibility = Visibility.Collapsed;
                    DownloadUpdateBtn.IsEnabled = true;
                    CheckUpdateBtn.IsEnabled = true;
                }
            }
            catch (OperationCanceledException)
            {
                UpdateProgressPanel.Visibility = Visibility.Collapsed;
                UpdateStatusText.Text = "Download cancelled.";
                DownloadUpdateBtn.IsEnabled = true;
                CheckUpdateBtn.IsEnabled = true;
            }
            catch (Exception ex)
            {
                UpdateProgressPanel.Visibility = Visibility.Collapsed;
                UpdateStatusText.Text = $"Download failed: {ex.Message}";
                DownloadUpdateBtn.IsEnabled = true;
                CheckUpdateBtn.IsEnabled = true;
            }
        }

        private void ViewReleasePage_Click(object sender, RoutedEventArgs e)
        {
            if (_latestUpdate is not null && !string.IsNullOrEmpty(_latestUpdate.HtmlUrl))
            {
                Process.Start(new ProcessStartInfo(_latestUpdate.HtmlUrl) { UseShellExecute = true });
            }
        }

        private async Task CheckForUpdatesInternalAsync(bool silent)
        {
            if (!silent)
            {
                CheckUpdateBtn.IsEnabled = false;
                UpdateStatusText.Text = "Checking for updates...";
                UpdateAvailableBanner.Visibility = Visibility.Collapsed;
                DownloadUpdateBtn.Visibility = Visibility.Collapsed;
                ViewReleaseBtn.Visibility = Visibility.Collapsed;
            }

            try
            {
                _latestUpdate = await Task.Run(() => _updateService.CheckForUpdateAsync());

                if (_latestUpdate is not null)
                {
                    UpdateStatusText.Text = "A new version is available!";
                    UpdateVersionText.Text = $"PureType v{_latestUpdate.Version} available";
                    UpdateSizeText.Text = $"Size: {GitHubUpdateService.FormatBytes(_latestUpdate.AssetSize)}";

                    string notes = _latestUpdate.ReleaseNotes;
                    if (notes.Length > 300)
                        notes = notes[..300] + "…";
                    UpdateNotesText.Text = string.IsNullOrWhiteSpace(notes) ? string.Empty : notes;

                    UpdateAvailableBanner.Visibility = Visibility.Visible;
                    DownloadUpdateBtn.Visibility = Visibility.Visible;
                    ViewReleaseBtn.Visibility = Visibility.Visible;
                }
                else
                {
                    if (!silent)
                    {
                        UpdateStatusText.Text = $"You're up to date! (v{AppVersion.Current})";
                    }
                }
            }
            catch (Exception ex)
            {
                if (!silent)
                {
                    UpdateStatusText.Text = $"Update check failed: {ex.Message}";
                }
            }
            finally
            {
                if (!silent)
                {
                    CheckUpdateBtn.IsEnabled = true;
                }
            }
        }

        private void SetUpdateProgress(double percent)
        {
            double maxWidth = UpdateProgressPanel.ActualWidth > 0 ? UpdateProgressPanel.ActualWidth : 400;
            UpdateProgressBar.Width = Math.Max(0, maxWidth * Math.Clamp(percent, 0, 100) / 100.0);
        }

        // ── Window chrome ────────────────────────────────────────────────────

        private void Title_Close(object sender, RoutedEventArgs e) => Close();
        private void Title_Minimize(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

        private void Hyperlink_RequestNavigate(object sender, System.Windows.Navigation.RequestNavigateEventArgs e)
        {
            Process.Start(new ProcessStartInfo(e.Uri.AbsoluteUri) { UseShellExecute = true });
            e.Handled = true;
        }

        protected override void OnClosed(EventArgs e)
        {
            if (_previewBuffer != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(_previewBuffer);
                _previewBuffer = IntPtr.Zero;
            }

            _updateCts?.Cancel();
            _updateCts?.Dispose();
            _updateService.Dispose();

            base.OnClosed(e);
        }

        private const string DefaultIni =
            "[general]\n" +
            "panelType = qd_oled_gen3\n" +
            "gammaMode = oled\n" +
            "filterStrength = 1.00\n" +
            "gamma = 1.00\n" +
            "oledGammaOutput = 1.00\n" +
            "enableSubpixelHinting = true\n" +
            "enableFractionalPositioning = false\n" +
            "lodThresholdSmall = 10.00\n" +
            "lodThresholdLarge = 22.00\n" +
            "woledCrossTalkReduction = 0.08\n" +
            "lumaContrastStrength = 1.20\n" +
            "toneParityV2Enabled = true\n" +
            "dwriteFallbackV2Enabled = true\n" +
            "contrastSamplingCacheEnabled = true\n" +
            "colorGlyphBypassEnabled = true\n" +
            "qdExpectedSepGen1 = -0.44\n" +
            "qdExpectedSepGen3 = -0.50\n" +
            "qdExpectedSepGen4 = -0.50\n" +
            "qdVerticalBlend = 0.15\n" +
            "chromaKeepScaleQD = 1.00\n" +
            "chromaKeepScaleWOLED = 1.00\n" +
            "stemDarkeningEnabled = true\n" +
            "stemDarkeningStrength = 0.45\n" +
            "interFontWeight = 400\n" +
            "interOpticalSize = 18\n" +
            "interLetterSpacing = 0.3\n" +
            "autoCheckUpdates = true\n" +
            "blacklist = vgc.exe, vgtray.exe, easyanticheat.exe, easyanticheat_eos.exe, beservice.exe, bedaisy.exe, gameguard.exe, nprotect.exe, pnkbstra.exe, pnkbstrb.exe, faceit.exe, faceit_ac.exe, csgo.exe, cs2.exe, valorant.exe, valorant-win64-shipping.exe, r5apex.exe, fortniteclient-win64-shipping.exe, eldenring.exe, gta5.exe, rdr2.exe, overwatchlauncher.exe, rainbowsix.exe, destiny2.exe, tarkov.exe\n\n" +
            "[debug]\n" +
            "enabled = false\n" +
            "logFile = PURETYPE.log\n" +
            "highlightRenderedGlyphs = false\n";
    }
}
