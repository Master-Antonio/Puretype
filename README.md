# PureType

Custom text rendering engine for OLED displays. Replaces Windows ClearType system-wide via DLL injection.

ClearType was designed for LCD panels with a fixed RGB stripe.
On OLEDs where subpixels are physically different shapes, sizes, and arrangements, it produces visible color fringing
and washed-out stems.
PureType fixes this by intercepting GDI and DirectWrite calls and re-rendering text with panel-aware subpixel filtering.

![PureType icon](src/puretype.png)

## What it does

- Hooks `ExtTextOutW` (GDI) and `IDWriteTextLayout::Draw` (DirectWrite) in target processes
- Rasterizes glyphs with FreeType in LCD mode (3× horizontal resolution)
- Applies HVS-optimized convolution kernels designed for the actual subpixel geometry of your panel
- Blends to screen in linear light using IEC 61966-2-1 sRGB transfer functions
- Applies stem darkening to restore font weight lost when ClearType's RGB sub-sampling is removed

## Supported panels

**LG WOLED (RWBG / RGWB)** — Handles the non-standard subpixel ordering and the addition of the White subpixel. Uses a
1x7 FIR filter in linear space. The algorithm applies "TCON Inversion Math": it injects the calculated white energy into
the RGB channels using a mathematical maximum before transmission, allowing the monitor's internal hardware to correctly
extract the White subpixel without energy clipping or color fringing.

**Samsung QD-OLED (Triangle/Diamond)** — Addresses the severe color fringing caused by the delta-grid arrangement of
QD-OLEDs. It applies specific fractional phase-shifting directly at the FreeType rasterization level, followed by a 1x5
FIR filter. It heavily utilizes the Perceptual Tone Mapper to apply Chroma Crushing (desaturating color artifacts on
text edges) and an aggressive S-Curve Readability Tone to counter the "washed-out" effect caused by the large black
gaps (fill-factor) in the QD-OLED matrix.

## How it works (The Pipeline)

1. **Injection:** `puretype.exe` forces Windows to load `PureType.dll` into target processes via
   `SetWindowsHookEx(WH_CBT)`.
2. **Hooking:** MinHook intercepts `ExtTextOutW` (GDI) and `IDWriteTextLayout::Draw` (DirectWrite), checking against a
   rigorous blacklist to avoid crashing sandboxed apps.
3. **Phase-Aware Rasterization:** FreeType renders the glyphs at 3× horizontal resolution. The rasterizer dynamically
   shifts the vector outlines (phase) based on the physical pixel grid of the target OLED panel.
4. **Subpixel Filtering:** FIR low-pass filters distribute the geometric coverage in strictly linear light, calculating
   exact light emission values for each subpixel.
5. **Perceptual Tone Mapping:**
    * *Chroma Crushing:* Detects subpixel imbalances on high-contrast edges and pushes them towards neutral grays,
      eliminating the purple/green OLED fringing.
    * *Readability Tone:* Applies a dynamic power exponent to inflate the text stems based on font size and the
      `LumaContrastStrength` setting.
6. **Compositing:** The final RGBA buffer is alpha-blended over the application's background using mathematically
   correct linear-space compositing via D2D or GDI `BitBlt`.

## Building

Requires Visual Studio 2022 Build Tools and CMake. FreeType and MinHook are fetched automatically.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output lands in `build/Release/`:

- `puretype.exe` — system tray loader
- `PureType.dll` — the rendering engine
- `puretype.ini` — configuration

## Usage

Put all three files in the same folder. Run `puretype.exe` — ideally as Administrator for full system-wide coverage. A
tray icon appears.

Right-click the tray icon to:

- **Enable / Disable** the hook (toggle injection on/off)
- **Panel Type** → pick your display (LG WRGB or Samsung QD-OLED)
- **Exit**

Panel choice is saved to `puretype.ini` and persists across restarts.

The DLL auto-skips processes where injection would cause problems: system services (`csrss`, `lsass`, `dwm`), anti-cheat
software (Vanguard, EAC, BattlEye), games with integrity checks, and browsers with strict sandboxes.

UWP apps (Calculator, Settings, the new Notepad) work out of the box — the injector automatically grants AppContainer
read permissions on the DLL before setting the hook.

## Configuration

Edit `puretype.ini`:

QD-OLED

```ini
[General]
PanelType = QD_OLED_TRIANGLE
FilterStrength = 1.0
Gamma = 1.0
EnableSubpixelHinting = true
EnableFractionalPositioning = true
LODThresholdSmall = 12.0
LODThresholdLarge = 24.0
WOLEDCrossTalkReduction = 0.0
LumaContrastStrength = 1.25
StemDarkeningEnabled = true
StemDarkeningStrength = 0.35
```

RWBG

```ini
[General]
PanelType = RWBG
FilterStrength = 1.0
Gamma = 1.0
EnableSubpixelHinting = true
EnableFractionalPositioning = true
LODThresholdSmall = 12.0
LODThresholdLarge = 24.0
WOLEDCrossTalkReduction = 0.12
LumaContrastStrength = 1.45
StemDarkeningEnabled = true
StemDarkeningStrength = 0.45
```

RGWB

```ini
[General]
PanelType = RGWB
FilterStrength = 1.0
Gamma = 1.0
EnableSubpixelHinting = true
EnableFractionalPositioning = true
LODThresholdSmall = 12.0
LODThresholdLarge = 24.0
WOLEDCrossTalkReduction = 0.10
LumaContrastStrength = 1.40
StemDarkeningEnabled = true
StemDarkeningStrength = 0.45
```

`Gamma` is **not** the display gamma — it's an optional exponent applied on top of the sRGB transfer curve. Leave it at
1.0 unless you have a specific reason to change it.

## How it works (briefly)

1. `puretype.exe` calls `SetWindowsHookEx(WH_CBT)` pointing at an exported callback in `PureType.dll`
2. Windows automatically loads `PureType.dll` into every process that creates a window
3. On load, the DLL checks the host process against a blacklist — if matched, it bails immediately
4. Otherwise it initializes FreeType, installs MinHook detours on GDI/DirectWrite text functions
5. When the app renders text, PureType intercepts the call, rasterizes with FreeType LCD mode, runs the subpixel filter,
   and composites to screen

The subpixel filter pipeline:

- Convert FreeType's LCD bitmap from sRGB to linear light (LUT, 256 entries)
- Apply per-channel convolution kernel (panel-specific, HVS-weighted)
- Stem darkening: `coverage = 1 - (1 - c)^(1 + boost)` — heavy at thin strokes, near-zero at solid fills
- Convert back to sRGB (LUT, 4097 entries with linear interpolation)
- Alpha-blend against the background in linear space

## Dependencies

- [FreeType](https://freetype.org/) — glyph rasterization (fetched by CMake)
- [MinHook](https://github.com/TsudaKageyu/minhook) — API hooking (fetched by CMake)
- Direct2D, DirectWrite, GDI32 — Windows platform APIs

## License

GNU GPLv3 
