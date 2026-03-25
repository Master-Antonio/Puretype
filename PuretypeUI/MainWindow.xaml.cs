using System;
using System.Collections.Generic;
using System.IO;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Controls;
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
            int    width,
            int    height,
            IntPtr pBuffer);

        // ── Preview buffer ────────────────────────────────────────────────────
        private const int PreviewRenderWidth = 1200;
        private const int PreviewRenderHeight = 400;
        private IntPtr          _previewBuffer = IntPtr.Zero;
        private WriteableBitmap _wb;
        private bool            _dllAvailable  = true;

        // ── INI ───────────────────────────────────────────────────────────────
        private string       _iniPath  = string.Empty;
        private List<string> _iniLines = new();

        private static readonly string FontPath =
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Fonts), "arial.ttf");

        // ═════════════════════════════════════════════════════════════════════
        public MainWindow()
        {
            try { InitializeComponent(); Loaded += MainWindow_Loaded; }
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

        private static float ParseFloat(Dictionary<string, string> d, string fk, float def) =>
            d.TryGetValue(fk, out var s) && float.TryParse(s, NumberStyles.Float,
            CultureInfo.InvariantCulture, out float v) ? v : def;

        private static bool ParseBool(Dictionary<string, string> d, string fk, bool def)
        {
            if (!d.TryGetValue(fk, out var s)) return def;
            s = s.ToLowerInvariant();
            return s == "true" || s == "1" || s == "yes";
        }

        private static string F2(double v)   => v.ToString("F2", CultureInfo.InvariantCulture);
        private static string Bool(bool b)    => b ? "true" : "false";

        // ═════════════════════════════════════════════════════════════════════
        //  Load
        // ═════════════════════════════════════════════════════════════════════

        private void LoadSettings()
        {
            _iniPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "puretype.ini");
            _iniLines = File.Exists(_iniPath)
                ? new List<string>(File.ReadAllLines(_iniPath))
                : new List<string>(DefaultIni.Split('\n'));

            var d = ParseIni(_iniLines);

            // Panel type
            var panel = d.GetValueOrDefault("general.paneltype", "rwbg").ToLowerInvariant();
            foreach (ComboBoxItem item in PanelTypeCombo.Items)
                if (string.Equals(item.Tag?.ToString(), panel, StringComparison.OrdinalIgnoreCase))
                    { item.IsSelected = true; break; }

            // Gamma mode
            var gammaStr = d.GetValueOrDefault("general.gammamode", "oled").ToLowerInvariant();
            foreach (ComboBoxItem item in GammaModeCombo.Items)
                if (string.Equals(item.Tag?.ToString(), gammaStr, StringComparison.OrdinalIgnoreCase))
                    { item.IsSelected = true; break; }

            FilterStrengthSlider.Value       = Math.Clamp(ParseFloat(d, "general.filterstrength",         1.0f),  0.0, 5.0);
            GammaSlider.Value                = Math.Clamp(ParseFloat(d, "general.gamma",                  1.0f),  0.5, 3.0);
            OledGammaOutputSlider.Value      = Math.Clamp(ParseFloat(d, "general.oledgammaoutput",        1.0f),  1.0, 2.0);
            SubpixelHintingCheck.IsChecked   = ParseBool (d, "general.enablesubpixelhinting",             true);
            FractionalPositioningCheck.IsChecked = ParseBool(d, "general.enablefractionalpositioning",    true);
            LumaContrastSlider.Value         = Math.Clamp(ParseFloat(d, "general.lumacontraststrength",   1.15f), 1.0, 3.0);
            StemDarkeningCheck.IsChecked     = ParseBool (d, "general.stemdarkeningenabled",              true);
            StemStrengthSlider.Value         = Math.Clamp(ParseFloat(d, "general.stemdarkeningstrength",  0.25f), 0.0, 1.0);
            WoledCrosstalkSlider.Value       = Math.Clamp(ParseFloat(d, "general.woledcrosstalkreduction",0.08f), 0.0, 1.0);

            double lodSmall = Math.Clamp(ParseFloat(d, "general.lodthresholdsmall", 10.0f), 6.0, 96.0);
            double lodLarge = Math.Clamp(ParseFloat(d, "general.lodthresholdlarge", 22.0f), 7.0, 160.0);
            LodSmallSlider.Value   = lodSmall;
            LodLargeSlider.Minimum = lodSmall + 1;
            LodLargeSlider.Value   = Math.Max(lodLarge, lodSmall + 1);

            double dpiLow = Math.Clamp(ParseFloat(d, "general.highdpithresholdlow", 144.0f), 96.0, 192.0);
            double dpiHigh = Math.Clamp(ParseFloat(d, "general.highdpithresholdhigh", 216.0f), 144.0, 300.0);
            HighDpiLowSlider.Value = dpiLow;
            HighDpiHighSlider.Minimum = dpiLow + 1;
            HighDpiHighSlider.Value = Math.Max(dpiHigh, dpiLow + 1);

            DebugEnabledCheck.IsChecked      = ParseBool(d, "debug.enabled",                  false);
            LogFileText.Text                 = d.GetValueOrDefault("debug.logfile",           "PURETYPE.log");
            HighlightGlyphsCheck.IsChecked   = ParseBool(d, "debug.highlightrenderedglyphs",  false);

            using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run", false);
            StartWithWindowsCheck.IsChecked = key?.GetValue("PureType") != null;

            string blacklistStr = d.GetValueOrDefault("general.blacklist", "");
            if (string.IsNullOrWhiteSpace(blacklistStr))
            {
                blacklistStr = "vgc.exe, vgtray.exe, easyanticheat.exe, easyanticheat_eos.exe, beservice.exe, bedaisy.exe, gameguard.exe, nprotect.exe, pnkbstra.exe, pnkbstrb.exe, faceit.exe, faceit_ac.exe, csgo.exe, cs2.exe, valorant.exe, valorant-win64-shipping.exe, r5apex.exe, fortniteclient-win64-shipping.exe, eldenring.exe, gta5.exe, rdr2.exe, overwatchlauncher.exe, rainbowsix.exe, destiny2.exe, tarkov.exe";
            }
            BlacklistText.Text = string.Join("\n", blacklistStr.Split(new[] { ',' }, StringSplitOptions.RemoveEmptyEntries));

            SyncWoledVisibility();
            SyncStemStrengthEnabled();
            SyncDebugOptions();
        }

        // ═════════════════════════════════════════════════════════════════════
        //  Save
        // ═════════════════════════════════════════════════════════════════════

        private void SaveSettings()
        {
            var panelTag = (PanelTypeCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "rwbg";
            SetIniValue("general", "panelType",                  panelTag);
            var gammaTag = (GammaModeCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "oled";
            SetIniValue("general", "gammaMode",                  gammaTag);
            SetIniValue("general", "filterStrength",             F2(FilterStrengthSlider.Value));
            SetIniValue("general", "gamma",                      F2(GammaSlider.Value));
            SetIniValue("general", "oledGammaOutput",            F2(OledGammaOutputSlider.Value));
            SetIniValue("general", "enableSubpixelHinting",      Bool(SubpixelHintingCheck.IsChecked     == true));
            SetIniValue("general", "enableFractionalPositioning", Bool(FractionalPositioningCheck.IsChecked == true));
            SetIniValue("general", "lumaContrastStrength",       F2(LumaContrastSlider.Value));
            SetIniValue("general", "stemDarkeningEnabled",       Bool(StemDarkeningCheck.IsChecked   == true));
            SetIniValue("general", "stemDarkeningStrength",      F2(StemStrengthSlider.Value));
            SetIniValue("general", "woledCrossTalkReduction",    F2(WoledCrosstalkSlider.Value));
            SetIniValue("general", "lodThresholdSmall",          F2(LodSmallSlider.Value));
            SetIniValue("general", "lodThresholdLarge",          F2(LodLargeSlider.Value));
            SetIniValue("general", "highDpiThresholdLow",        F2(HighDpiLowSlider.Value));
            SetIniValue("general", "highDpiThresholdHigh",       F2(HighDpiHighSlider.Value));

            var bl = string.Join(", ", BlacklistText.Text.Split(new[] { '\n', '\r' }, StringSplitOptions.RemoveEmptyEntries).Select(s => s.Trim()));
            SetIniValue("general", "blacklist",                  bl);

            SetIniValue("debug",   "enabled",                    Bool(DebugEnabledCheck.IsChecked    == true));
            SetIniValue("debug",   "logFile",                    LogFileText.Text.Trim());
            SetIniValue("debug",   "highlightRenderedGlyphs",    Bool(HighlightGlyphsCheck.IsChecked == true));

            File.WriteAllLines(_iniPath, _iniLines);

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

        // ═════════════════════════════════════════════════════════════════════
        //  Preview
        // ═════════════════════════════════════════════════════════════════════

        private void UpdatePreview()
        {
            if (!_dllAvailable || _previewBuffer == IntPtr.Zero) return;
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

                string sample = "PureType brings true OLED subpixel rendering to Windows.\n" +
                                "Text is crisp, smooth, and color-fringe free, just like a smartphone.\n" +
                                "The quick brown fox jumps over the lazy dog.\n" +
                                "Sphinx of black quartz, judge my vow.";

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

        private void ApplyRecommended_Click(object sender, RoutedEventArgs e)
        {
            if (!IsLoaded) return;
            var tag = (PanelTypeCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "rwbg";

            // Reset base parameters
            FilterStrengthSlider.Value = 1.0;
            GammaSlider.Value = 1.0;
            OledGammaOutputSlider.Value = 1.0;
            
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

            // Apply panel-specific magic numbers based on Oled.MD guidelines
            switch(tag)
            {
                case "rwbg":
                case "rgwb":
                    OledGammaOutputSlider.Value = 1.20;
                    WoledCrosstalkSlider.Value = 0.10;
                    LumaContrastSlider.Value = 1.25;
                    StemDarkeningCheck.IsChecked = true;
                    StemStrengthSlider.Value = 0.20;
                    break;
                case "qd_oled_gen1":
                    OledGammaOutputSlider.Value = 1.15;
                    WoledCrosstalkSlider.Value = 0.00;
                    LumaContrastSlider.Value = 1.15;
                    FilterStrengthSlider.Value = 1.10; // Extra smooth for triangle matrix
                    StemDarkeningCheck.IsChecked = true;
                    StemStrengthSlider.Value = 0.30; // Very narrow SPD, stronger darkening
                    break;
                case "qd_oled_gen3":
                case "qd_oled_gen4":
                    OledGammaOutputSlider.Value = 1.15;
                    WoledCrosstalkSlider.Value = 0.00;
                    LumaContrastSlider.Value = 1.15;
                    StemDarkeningCheck.IsChecked = true;
                    StemStrengthSlider.Value = 0.25;
                    break;
            }
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

        private void Exit_Click(object sender, RoutedEventArgs e) => Close();

        private void Window_MouseLeftButtonDown(object sender, System.Windows.Input.MouseButtonEventArgs e)
        {
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
