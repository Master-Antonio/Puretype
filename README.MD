![PureType icon](src/puretype.png)

# PureType

**OLED-aware subpixel text rendering for Windows.**

ClearType was designed in 2000 for RGB-stripe LCD panels. Modern OLED displays — LG WOLED, Samsung QD-OLED — have
completely different subpixel layouts. PureType intercepts GDI and DirectWrite calls and remaps the per-channel coverage
to the physical subpixel geometry of your panel, producing sharper and more accurate text rendering with no changes to
system settings.

Subpixel center positions are derived from panel microscopy — macro photographs with individual subpixels lit
individually — rather than geometric assumptions. This makes the remap as accurate as the physical hardware allows.

---

## Support

https://paypal.me/masterantonio

---

## Supported panels

| Panel type | Examples | `panelType` value |
|---|---|---|
| LG WOLED RWBG | LG 27GR95QE, 45GR95QE, C-series OLED TVs used as monitors | `rwbg` |
| LG WOLED RGWB | LG 32" OLED models (PG32UCDP, 32GS95UE…) | `rgwb` |
| Samsung QD-OLED Gen 1-2 | Dell AW3423DW / AW3423DWF, Odyssey G8 OLED 34" gen1, Odyssey Neo G9 OLED | `qd_oled_gen1` |
| Samsung QD-OLED Gen 3 | Odyssey G8 OLED 27" QHD, Dell AW2725DF, 32" 4K models | `qd_oled_gen3` |
| Samsung QD-OLED Gen 4 | MSI MPG 272URX, 27" 4K UHD models 2024-2025 | `qd_oled_gen4` |

**Not sure which generation?**
- Oval / teardrop shaped subpixels, R clearly larger than B → Gen 1-2
- Rectangular subpixels, R slightly wider than B → Gen 3
- Rectangular subpixels, R ≈ B equal size → Gen 4

---

## How it works

Windows renders text with ClearType, which assumes every pixel contains three horizontal subpixels in RGB order. WOLED
panels add a fourth **white** subpixel that ClearType does not model. QD-OLED panels arrange subpixels in a
**triangular** pattern with a half-pixel vertical offset between rows. Both layouts produce incorrect colour fringing
and a luminance haze when rendered with standard ClearType.

PureType hooks `ExtTextOutW` (GDI) and `IDWriteTextRenderer::DrawGlyphRun` (DirectWrite). For each text draw call:

1. The screen region is captured before GDI renders.
2. GDI renders normally — layout, metrics, and clipping are entirely GDI's responsibility.
3. The region is captured after rendering.
4. Per-channel subpixel coverage is extracted from the before/after difference, solved in sRGB space (the domain ClearType actually composites in).
5. Coverage values are remapped to the physical subpixel positions of the target panel using linear interpolation between ClearType sample points.
6. The corrected pixels are written back using `AlphaBlend` with per-pixel alpha — text pixels are replaced, background pixels are never touched, preserving compositor content on composited surfaces (desktop icon labels, DWM-managed windows).

GDI handles all glyph metrics. PureType touches only pixel values. This makes it compatible with any rendering
framework — Qt, MFC, WPF, DirectWrite, legacy Win32 — without requiring a process blacklist or per-application
configuration.

If the system AA setting is grayscale or off, PureType temporarily forces ClearType for the duration of each GDI call
(via a LOGFONT clone with `lfQuality = CLEARTYPE_QUALITY`) so that per-channel subpixel data is always available. The
original font is restored immediately after capture. Glyph metrics are unaffected by `lfQuality`.

---

## Installation

1. Download the latest release and extract to a permanent directory, e.g. `C:\PureType\`.
2. Run `puretype.exe`.
3. Puretype is running in background. Double click to the icon in the system tray to open UI.
4. Chose the panel type and click the "✦ Apply recommended" button to automatically load the tuned constants for your panel.

No system-wide ClearType settings need to be changed. PureType works whether ClearType is enabled or disabled.

---

## Configuration

All settings live in `puretype.ini` next to the DLL. Changes take effect after restarting the injection.

```ini
[general]
; Panel type — see table above for the correct value for your monitor.
; WOLED:   rwbg | rgwb
; QD-OLED: qd_oled_gen1 | qd_oled_gen3 | qd_oled_gen4
panelType                   = rwbg

filterStrength              = 1.00   ; 0.0 (off) – 5.0
gamma                       = 1.00   ; 0.5 – 3.0
gammaMode                   = oled   ; srgb | oled
enableSubpixelHinting       = true
enableFractionalPositioning = true
lodThresholdSmall           = 10.00  ; px, small-text path threshold
lodThresholdLarge           = 22.00  ; px, large-text path threshold
highDpiThresholdLow         = 144.00 ; DPI, filtering fade-out start
highDpiThresholdHigh        = 216.00 ; DPI, filtering disabled completely
woledCrossTalkReduction     = 0.10   ; 0.0 – 1.0, WOLED only
lumaContrastStrength        = 1.15   ; 1.0 – 3.0
stemDarkeningEnabled        = true
stemDarkeningStrength       = 0.25   ; font-weight aware darkening

[debug]
enabled                     = false
logFile                     = PURETYPE.log
highlightRenderedGlyphs     = false
```

`woledCrossTalkReduction` attenuates the W subpixel contribution before the TCON `max()` merge, eliminating the grey haze that appears on dark backgrounds when the white subpixel fires at full intensity. `stemDarkeningStrength` dynamically reduces the visual weight of thin fonts on high-color-volume panels (like QD-OLED) based on the font's logical weight, achieving parity with FreeType's darkening algorithms.

---

## Graphical Interface (PuretypeUI)

Starting with recent builds, PureType includes a modern WPF-based graphical interface (`PuretypeUI.exe`) that allows you to preview the subpixel rendering in real-time. 

You no longer need to edit `puretype.ini` manually: 
1. Open the UI to view the live rendering preview.
2. Select your OLED panel type from the drop-down.
3. Click the **"✦ Apply recommended"** button to automatically load the scientifically tuned constants (Crosstalk reduction, Stem darkening, Luma contrast) specifically designed for your panel's Color Volume and Spectral Power Distribution.

---

## Compatibility

| Framework / renderer | Status |
|---|---|
| Win32 / MFC / WinForms | ✅ |
| Qt5 / Qt6 (EqualizerAPO, Peace, VoiceMeeter…) | ✅ |
| WPF with GDI fallback | ✅ |
| CJK IME (Microsoft IME, ATOK…) | ✅ |
| Notepad / Notepad++ | ✅ |
| Legacy 32-bit apps | ✅ |
| DirectWrite + Direct2D | ✅ |
| System ClearType off | ✅ |
| Composited / layered windows (desktop icon labels) | ✅ |

PureType does not inject into system processes (`csrss.exe`, `dwm.exe`, `lsass.exe`…), anti-cheat services, or game
executables.

---

## Building

**Dependencies:** FreeType 2, MinHook, Windows SDK (Direct2D 1.1, DirectWrite 3).  
**Targets:** Windows 10 1903+. Both x64 and x86 configurations are required for injection into 32-bit processes.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Output: `puretype.dll` — place alongside `puretype.ini` in the injection directory.

---

## Documentation

| Document | Description |
|---|---|
| [TECHNICAL.md](docs/TECHNICAL.md) | Architecture, mathematics, and implementation reference |
| [Wiki: Before / After](../../wiki/Before-After) | Visual comparison of all changes |

---

## Reporting issues

Use the GitHub issue templates:

- **Bug report** — wrong colours, invisible text, ghost characters, crashes
- **Feature request** — new options, new rendering behaviour
- **Panel compatibility** — new panel type or subpixel measurement data

When reporting a bug, attach your `puretype.ini` and enable debug logging (`[debug] enabled = true`) to capture a `PURETYPE.log` during reproduction.

---

## License

GPL-3.0 — [LICENSE](LICENSE)