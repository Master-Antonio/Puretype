![PureType icon](src/puretype.png)

# PureType

**OLED-aware subpixel text rendering for Windows.**

ClearType was designed in 2000 for RGB-stripe LCD panels. Modern OLED displays — LG WOLED, Samsung QD-OLED — have
completely different subpixel layouts. PureType intercepts GDI and DirectWrite calls and remaps the per-channel coverage
to the physical subpixel geometry of your panel, producing sharper and more accurate text rendering with no changes to
system settings.

---

# Support me:

https://paypal.me/masterantonio

---

## Supported panels

| Panel type                 | Examples                                                              | `panelType` value  |
|----------------------------|-----------------------------------------------------------------------|--------------------|
| LG WOLED RWBG              | LG 27GR95QE, 45GR95QE, C-series OLED TVs used as monitors             | `rwbg`             |
| LG WOLED RGWB              | Some LG 32" OLED models                                               | `rgwb`             |
| Samsung QD-OLED triangular | Dell AW3423DW / AW3423DWF, Samsung Odyssey G8 OLED, Sony InZone M9 II | `qd_oled_triangle` |

---

## How it works

Windows renders text with ClearType, which assumes every pixel contains three horizontal subpixels in RGB order. WOLED
panels add a fourth **white** subpixel that ClearType does not model. QD-OLED panels arrange subpixels in a **triangular
** pattern with a half-pixel vertical offset between rows. Both layouts produce incorrect colour fringing and a
luminance haze when rendered with standard ClearType.

PureType hooks `ExtTextOutW` (GDI) and `IDWriteTextRenderer::DrawGlyphRun` (DirectWrite). For each text draw call:

1. The screen region is captured before GDI renders.
2. GDI renders normally — layout, metrics, and clipping are entirely GDI's responsibility.
3. The region is captured after rendering.
4. Per-channel subpixel coverage is extracted from the before/after difference.
5. Coverage values are remapped to the physical subpixel positions of the target panel.
6. The corrected pixels are written back to the HDC.

GDI handles all glyph metrics. PureType touches only pixel values. This makes it compatible with any rendering
framework — Qt, MFC, WPF, DirectWrite, legacy Win32 — without requiring a process blacklist or per-application
configuration.

If the system AA setting is grayscale or off, PureType temporarily forces ClearType for the duration of each GDI call (
via a LOGFONT clone with `lfQuality = CLEARTYPE_QUALITY`) so that per-channel subpixel data is always available. The
original font is restored immediately after capture. Glyph metrics are unaffected by `lfQuality`.

---

## Installation

1. Download the latest release and extract to a permanent directory, e.g. `C:\PureType\`.
2. Open `puretype.ini` and set `panelType` to match your display (see table above).
3. Run `inject.exe`. To start automatically at login, add it to Task Scheduler or a registry run key.

No system-wide ClearType settings need to be changed. PureType works whether ClearType is enabled or disabled.

---

## Configuration

All settings live in `puretype.ini` next to the DLL. Changes take effect after restarting the injection.

```ini
[general]
panelType = rwbg        ; rwbg | rgwb | qd_oled_triangle
filterStrength = 0.75        ; 0.0 (off) – 5.0
gamma = 1.0         ; 0.5 – 3.0
enableSubpixelHinting = true
enableFractionalPositioning = true
lodThresholdSmall = 10.0        ; px, small-text path threshold
lodThresholdLarge = 22.0        ; px, large-text path threshold
woledCrossTalkReduction = 0.08        ; 0.0 – 1.0, WOLED only
lumaContrastStrength = 1.15        ; 1.0 – 3.0
stemDarkeningEnabled = true
stemDarkeningStrength = 0.25

[debug]
enabled = false
logFile = PURETYPE.log
highlightRenderedGlyphs = false
```

`woledCrossTalkReduction` attenuates the W subpixel contribution before the TCON `max()` merge, eliminating the grey
haze that appears on dark backgrounds when the white subpixel fires at full intensity.

---

## Compatibility

| Framework / renderer                          | Status |
|-----------------------------------------------|--------|
| Win32 / MFC / WinForms                        | ✅      |
| Qt5 / Qt6 (EqualizerAPO, Peace, VoiceMeeter…) | ✅      |
| WPF with GDI fallback                         | ✅      |
| CJK IME (Microsoft IME, ATOK…)                | ✅      |
| Notepad / Notepad++                           | ✅      |
| Legacy 32-bit apps                            | ✅      |
| DirectWrite + Direct2D                        | ✅      |
| System ClearType off                          | ✅      |

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

| Document                                        | Description                                             |
|-------------------------------------------------|---------------------------------------------------------|
| [TECHNICAL.md](docs/TECHNICAL.md)               | Architecture, mathematics, and implementation reference |
| [Wiki: Before / After](../../wiki/Before-After) | Visual comparison of v0.0.3 → v0.0.4 changes            |

---

## License

GPL-3.0 - [LICENSE](LICENSE) 
