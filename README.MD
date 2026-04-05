<p align="center">
  <img src="PuretypeUI/Assets/puretype_logo.png" alt="PureType" width="96"/>
</p>

<h1 align="center">PureType</h1>

<p align="center">
  <strong>OLED-aware subpixel text rendering for Windows</strong><br/>
  <em>Sharper, cleaner text on LG WOLED &amp; Samsung QD-OLED displays.</em>
</p>

<p align="center">
  <a href="#installation">Installation</a> · <a href="#graphical-interface">UI Guide</a> · <a href="#manual-configuration">Manual INI</a> · <a href="#building-from-source">Build</a> · <a href="https://paypal.me/masterantonio">Donate</a>
</p>

---

## Why PureType?

ClearType was designed in 2000 for RGB-stripe LCD panels. Modern OLED displays — LG WOLED, Samsung QD-OLED — have
completely different subpixel layouts:

- **WOLED** panels add a fourth **white** subpixel that ClearType does not model.
- **QD-OLED** panels arrange subpixels in a **triangular** pattern with a half-pixel vertical offset between rows.

Both layouts produce incorrect colour fringing and a luminance haze when rendered with standard ClearType. PureType
intercepts GDI and DirectWrite text rendering calls and remaps per-channel coverage to the physical subpixel geometry of
your panel.

Subpixel center positions are derived from **panel microscopy** — macro photographs with individual subpixels lit
individually — rather than geometric assumptions.

---

## Supported panels

| Panel type              | Examples                                                                 | `panelType` value |
|-------------------------|--------------------------------------------------------------------------|-------------------|
| LG WOLED RWBG           | LG 27GR95QE, 45GR95QE, C-series OLED TVs used as monitors                | `rwbg`            |
| LG WOLED RGWB           | LG 32" OLED models (PG32UCDP, 32GS95UE…)                                 | `rgwb`            |
| Samsung QD-OLED Gen 1-2 | Dell AW3423DW / AW3423DWF, Odyssey G8 OLED 34" gen1, Odyssey Neo G9 OLED | `qd_oled_gen1`    |
| Samsung QD-OLED Gen 3   | Odyssey G8 OLED 27" QHD, Dell AW2725DF, 32" 4K models                    | `qd_oled_gen3`    |
| Samsung QD-OLED Gen 4   | MSI MPG 272URX, 27" 4K UHD models 2024-2025                              | `qd_oled_gen4`    |

> **Not sure which generation?**
> - Oval / teardrop shaped subpixels, R clearly larger than B → **Gen 1-2**
> - Rectangular subpixels, R slightly wider than B → **Gen 3**
> - Rectangular subpixels, R ≈ B equal size → **Gen 4**

---

## Installation

1. Download the latest release and extract to a permanent directory (e.g. `C:\PureType\`).
2. Run **`puretype.exe`**. The application starts minimized in the **system tray**.
3. **Double-click** the tray icon to open the graphical configuration UI.
4. Select your OLED panel type and choose a **Quick Preset** (Balanced, Sharp, or Clean).
5. Click **Save & Apply**. Done — changes are applied instantly.

> **Note:** PureType automatically disables Windows ClearType when it activates and restores it on exit. No system-wide
> settings need to be changed manually.

### Release folder structure

```
C:\PureType\
├── puretype.exe                  ← Tray launcher (run this)
├── PureType.dll                  ← Core rendering engine
├── PuretypeUI.exe                ← Graphical configuration UI
├── PuretypeUI.dll                ← .NET runtime assembly
├── PuretypeUI.runtimeconfig.json ← .NET runtime config
├── puretype.ini                  ← All settings (editable)
├── puretype.ico                  ← Tray icon
└── font/                         ← Bundled Inter variable font
    ├── Inter-VariableFont_opsz,wght.ttf
    └── Inter-Italic-VariableFont_opsz,wght.ttf
```

---

## Graphical Interface

PureType includes a modern **WPF-based UI** (`PuretypeUI.exe`, .NET 9) with sidebar navigation and a live before/after
preview. You can configure everything without editing `puretype.ini` manually.

### Tabs

| Tab             | What it does                                                                                                                                                                 |
|-----------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Overview**    | Live rendered preview (standard AA vs. PureType), quick preset chips (Balanced / Sharp / Clean), active configuration summary.                                               |
| **Rendering**   | Panel type, filter strength, WOLED crosstalk reduction, gamma mode & correction, OLED gamma output, luma contrast, subpixel hinting, fractional positioning, stem darkening. |
| **Display**     | LOD glyph-size thresholds, high-DPI fade-out thresholds.                                                                                                                     |
| **System Font** | Install/restore the bundled **Inter** variable font as the system UI font. Adjust weight, optical size, and letter spacing axes with live preview.                           |
| **Profiles**    | Create per-monitor or per-application override profiles.                                                                                                                     |
| **Settings**    | Start with Windows, debug logging, glyph highlight overlay, process blacklist.                                                                                               |
| **Info**        | Version, author, GitHub and donate links, license.                                                                                                                           |

### Quick Presets

Presets are **panel-aware** — values differ between WOLED and QD-OLED:

| Preset       | Description                                                                                      |
|--------------|--------------------------------------------------------------------------------------------------|
| **Balanced** | Scientifically correct defaults. `filterStrength=1.0`, `stemStrength=0.45`, `lumaContrast=1.20`. |
| **Sharp**    | Prioritizes stem crispness. Stronger darkening + higher contrast.                                |
| **Clean**    | Prioritizes colour purity. Stronger filtering, lighter stems.                                    |

When slider values no longer match any preset, a **Custom** indicator appears automatically.

---

## Manual Configuration

All settings live in **`puretype.ini`** (same directory as the DLL). The UI reads and writes this file, but advanced
users can edit it directly with any text editor. Changes take effect after:

- Clicking **Save & Apply** in the UI, or
- Right-clicking the tray icon → **Disable** then **Enable** PureType.

### Full annotated `puretype.ini`

```ini
; ============================================================
;  puretype.ini — PureType OLED subpixel renderer settings
; ============================================================

[general]

; ── Panel Type ──────────────────────────────────────────────
; Choose the subpixel layout of your OLED monitor.
; Values: rwbg | rgwb | qd_oled_gen1 | qd_oled_gen3 | qd_oled_gen4
panelType = qd_oled_gen3

; ── Gamma Mode ──────────────────────────────────────────────
; srgb  — Standard IEC 61966-2-1 sRGB curve (ideal for LCD)
; oled  — Softer gamma (~2.0) below 18% luminance, prevents
;         dark text from being crushed on OLED shadow response
gammaMode = oled

; ── Subpixel Filter ─────────────────────────────────────────
; Main filter intensity. 1.0 = full panel-aware correction.
; Range: 0.0 (passthrough) – 5.0    UI range: 0.0 – 2.0
filterStrength = 1.00

; ── Gamma Correction ────────────────────────────────────────
; Base gamma multiplier applied on top of gammaMode.
; Range: 0.5 – 3.0    UI range: 0.5 – 2.5
gamma = 1.0

; ── OLED Gamma Output ──────────────────────────────────────
; Post-processing gamma for OLED panels.
; Higher values brighten dark-on-light text.
; Range: 1.0 – 2.0
oledGammaOutput = 1.00

; ── Subpixel Hinting ───────────────────────────────────────
; Uses FreeType FT_LOAD_FORCE_AUTOHINT for crisper stems
; at small pixel sizes. Recommended for OLED.
; Values: true | false
enableSubpixelHinting = true

; ── Fractional Positioning ─────────────────────────────────
; Sub-pixel X placement of glyphs.
; false = recommended for GDI apps (avoids mask blur).
; DWrite path uses native FreeType phase tracking regardless.
; Values: true | false
enableFractionalPositioning = false

; ── LOD Thresholds ─────────────────────────────────────────
; Glyph-size thresholds (px) for level-of-detail filtering.
; Small: 6.0 – 96.0    Large: small+1 – 160.0
lodThresholdSmall = 10.0
lodThresholdLarge = 22.0

; ── High-DPI Fade Thresholds ───────────────────────────────
; At high PPI, subpixel filtering becomes unnecessary.
; Between Low and High, strength ramps down gradually.
; Above High, filtering is fully bypassed.
; Low: 96 – 384    High: low+1 – 600
highDpiThresholdLow = 144.0
highDpiThresholdHigh = 216.0

; ── WOLED Crosstalk Reduction ──────────────────────────────
; (WOLED panels only — rwbg / rgwb)
; Attenuates the white subpixel to reduce grey haze on dark
; backgrounds caused by the TCON max() merge.
; Range: 0.0 (off) – 1.0    UI range: 0.0 – 0.5
woledCrossTalkReduction = 0.08

; ── Luma Contrast ──────────────────────────────────────────
; S-curve boost: exp ≈ 1.0 + (value - 1.0) × 0.5
; 1.20 = optimal 5-7% mid-tone boost (Legge & Foley 1980).
; Range: 1.0 – 3.0    UI range: 0.5 – 2.0
lumaContrastStrength = 1.20

; ── Stem Darkening ─────────────────────────────────────────
; Darkens thin vertical strokes using font-weight-aware logic.
; Less darkening for Bold, more for Light.
; Values: true | false
stemDarkeningEnabled = true
; Range: 0.0 – 2.0    UI range: 0.0 – 1.0
stemDarkeningStrength = 0.45

; ── Inter Variable Font Axes ───────────────────────────────
; Used by the System Font tab when Inter is installed.
; interFontWeight:     100 – 900  (WOLED: try 410-420)
; interOpticalSize:    14 – 32    (QD-OLED: increase for clarity)
; interLetterSpacing:  -2.0 – 4.0 px  (QD-OLED: 0.2-0.5 reduces fringing)
interFontWeight = 400
interOpticalSize = 18
interLetterSpacing = 0.3

; ── Process Blacklist ──────────────────────────────────────
; Comma-separated list of executables to never inject into.
; If omitted, a built-in default list of games and anti-cheat
; processes is used automatically.
; blacklist = vgc.exe, csgo.exe, valorant.exe

; ============================================================
[debug]
; ============================================================

; Enable debug logging.
enabled = false

; Log file path (relative to DLL directory).
logFile = PURETYPE.log

; Tint every PureType-rendered glyph with a cyan overlay.
highlightRenderedGlyphs = false
```

---

### Profiles: per-monitor and per-application overrides

Profiles let you override any `[general]` setting for a specific monitor or application. The DLL resolves values with
this priority:

> **App profile → Monitor profile → Global (`[general]`)**

Any key you **don't** specify in a profile section inherits from `[general]` automatically.

#### Monitor profiles

The section name uses the format `[monitor_<devicename>]` where `<devicename>` is the Windows display device path with
backslashes, dots, and whitespace removed, all lowercase.

To find your device name, run in PowerShell:

```powershell
Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPClass -eq "Monitor" } | Select-Object Name
# Or check the Profiles tab in PuretypeUI — it lists all displays.
```

**Example** — override settings for `\\.\DISPLAY1`:

```ini
[monitor_display1]
; The device path \\.\DISPLAY1 becomes: display1 (remove \\ and .)
panelType = qd_oled_gen3
filterStrength = 1.10
gamma = 1.05
stemDarkeningStrength = 0.50
woledCrossTalkReduction = 0.00
```

**Example** — a second monitor with a different panel type:

```ini
[monitor_display2]
panelType = rwbg
filterStrength = 1.00
woledCrossTalkReduction = 0.10
gammaMode = oled
```

#### Application profiles

The section name uses the format `[app_<processname>]` where `<processname>` is the executable filename in lowercase (
including `.exe`).

**Example** — custom rendering for Notepad++:

```ini
[app_notepad++.exe]
filterStrength = 0.80
stemDarkeningStrength = 0.30
gammaMode = srgb
```

**Example** — disable filtering entirely for a specific app:

```ini
[app_firefox.exe]
filterStrength = 0.00
stemDarkeningEnabled = false
```

**Example** — stronger rendering for a terminal emulator:

```ini
[app_windowsterminal.exe]
filterStrength = 1.20
stemDarkeningStrength = 0.60
lumaContrastStrength = 1.35
```

> **Tip:** You can also create profiles from the UI (Profiles tab) — it writes the same INI sections. The UI is useful
> to discover your monitor device names automatically.

---

## How it works

PureType hooks `ExtTextOutW` (GDI) and DirectWrite draw calls via [MinHook](https://github.com/TsudaKageyu/minhook). For
each text draw call:

1. The screen region is captured **before** GDI renders.
2. GDI renders normally — layout, metrics, and clipping remain GDI's responsibility.
3. The region is captured **after** rendering.
4. Per-channel subpixel coverage is extracted from the before/after delta (sRGB space).
5. Coverage values are **remapped** to the physical subpixel positions of the target panel via linear interpolation.
6. The corrected pixels are written back using `AlphaBlend` with per-pixel alpha — background pixels are never touched.

GDI handles all glyph metrics. PureType touches **only pixel values**. This makes it compatible with any rendering
framework without per-application configuration.

If the system AA setting is grayscale or off, PureType temporarily forces ClearType (`lfQuality = CLEARTYPE_QUALITY`) so
that per-channel data is always available. The original font quality is restored immediately after capture.

---

## Architecture

```
puretype.exe              System tray launcher / global WH_CBT hook injector
PureType.dll              Core rendering engine (injected into all GUI processes)
PuretypeUI.exe            WPF configuration interface (.NET 9, x64)
puretype.ini              Configuration & profiles (INI format)
font/                     Bundled Inter variable font files
```

### Internal DLL modules

| Module          | Source                             | Role                                                    |
|-----------------|------------------------------------|---------------------------------------------------------|
| Config          | `src/config.cpp`                   | INI parser, per-process / per-monitor value resolution  |
| FT Rasterizer   | `src/rasterizer/ft_rasterizer.cpp` | FreeType 2 glyph rasterization with subpixel hinting    |
| Subpixel Filter | `src/filters/subpixel_filter.cpp`  | Panel-geometry-aware subpixel coverage remapping        |
| Tone Mapper     | `src/filters/tone_mapper.cpp`      | Gamma correction, luma contrast S-curve, stem darkening |
| Blender         | `src/output/blender.cpp`           | Per-pixel alpha compositing back to screen              |
| GDI Hooks       | `src/hooks/gdi_hooks.cpp`          | `ExtTextOutW` interception via MinHook                  |
| DWrite Hooks    | `src/hooks/dwrite_hooks.cpp`       | DirectWrite interception via MinHook                    |
| Preview Bridge  | `src/ui/preview_bridge.cpp`        | `GeneratePreview()` export for the UI live renderer     |

### Hook lifecycle

1. `puretype.exe` installs a desktop-wide `WH_CBT` hook referencing `PureType.dll`.
2. Windows loads `PureType.dll` into every new GUI process.
3. `DllMain(DLL_PROCESS_ATTACH)` performs only minimal setup plus an early strict blacklist gate (from injector snapshot).
  If that snapshot is missing or invalid, attach fails closed (`return FALSE`) so the DLL is not injected.
4. First `PureTypeCBTProc` callback performs runtime init outside loader lock: config load, FreeType init, GDI + DWrite hook install via MinHook.
5. Hooks intercept text rendering calls → subpixel filter pipeline runs.
6. `DLL_PROCESS_DETACH` → hooks disabled → spin-wait on ref-counted in-flight calls → resources destroyed.

---

## Compatibility

| Framework / Renderer                               | Status                                    |
|----------------------------------------------------|-------------------------------------------|
| Win32 / MFC / WinForms                             | ✅                                         |
| Qt5 / Qt6 (EqualizerAPO, Peace, VoiceMeeter…)      | ✅                                         |
| WPF with GDI fallback                              | ✅                                         |
| CJK IME (Microsoft IME, ATOK…)                     | ✅                                         |
| Notepad / Notepad++                                | ✅                                         |
| Legacy 32-bit apps                                 | ✅                                         |
| DirectWrite + Direct2D                             | ✅                                         |
| System ClearType off                               | ✅                                         |
| Composited / layered windows (desktop icon labels) | ✅                                         |
| UWP / AppContainer processes                       | ✅ (ACL permissions granted automatically) |

On puretype you can create your blacklist, customize it from **Settings → Process Blacklist** or the `blacklist` key in
`puretype.ini`.

---

## Building from source

### Requirements

- **CMake** 3.14+
- **Visual Studio** 2022 (or MSVC Build Tools) with C++17
- **Windows SDK** (Direct2D 1.1, DirectWrite 3)
- **.NET 9 SDK** (for PuretypeUI)

### Dependencies

Downloaded automatically via CMake FetchContent:

- [FreeType 2](https://freetype.org/) — VER-2-14-3
- [MinHook](https://github.com/TsudaKageyu/minhook) — v1.3.4

### Build commands

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Build outputs

| File                            | Description                                     |
|---------------------------------|-------------------------------------------------|
| `PureType.dll`                  | Core rendering library                          |
| `puretype.exe`                  | System tray launcher / injector                 |
| `PuretypeUI.exe`                | WPF configuration UI                            |
| `PuretypeUI.dll`                | .NET runtime assembly                           |
| `PuretypeUI.runtimeconfig.json` | .NET runtime config                             |
| `puretype.ini`                  | Default configuration (copied from source root) |
| `puretype.ico`                  | Application icon                                |
| `font/`                         | Inter variable font files                       |

---

## Reporting issues

Use the GitHub issue templates:

- **Bug report** — wrong colours, invisible text, ghost characters, crashes
- **Feature request** — new options, new rendering behaviour
- **Panel compatibility** — new panel type or subpixel measurement data

When reporting a bug:

1. Attach your `puretype.ini`
2. Enable debug logging: set `[debug] enabled = true`
3. Reproduce the issue
4. Attach the generated `PURETYPE.log`

---

## License

GPL-3.0 — see [LICENSE](LICENSE)

---

<p align="center">
  <strong>Support the project:</strong> <a href="https://paypal.me/masterantonio">paypal.me/masterantonio</a><br/>
  <strong>Author:</strong> <a href="https://github.com/Master-Antonio">Toriga</a>
</p>

