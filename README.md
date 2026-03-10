# PureType

Custom text rendering engine for OLED displays. Replaces Windows ClearType system-wide via DLL injection.

ClearType was designed for LCD panels with a fixed RGB stripe. 
On OLEDs where subpixels are physically different shapes, sizes, and arrangements, it produces visible color fringing and washed-out stems. 
PureType fixes this by intercepting GDI and DirectWrite calls and re-rendering text with panel-aware subpixel filtering.

![PureType icon](src/puretype.png)

## What it does

- Hooks `ExtTextOutW` (GDI) and `IDWriteTextLayout::Draw` (DirectWrite) in target processes
- Rasterizes glyphs with FreeType in LCD mode (3× horizontal resolution)
- Applies HVS-optimized convolution kernels designed for the actual subpixel geometry of your panel
- Blends to screen in linear light using IEC 61966-2-1 sRGB transfer functions
- Applies stem darkening to restore font weight lost when ClearType's RGB sub-sampling is removed

## Supported panels

**LG WRGB WOLED** — Horizontal 1×5 FIR filter with per-channel HVS weighting. Green gets the widest spatial spread (the eye resolves luminance detail via green). The White subpixel is used for adaptive luminance boost in flat regions while being suppressed at edges to preserve sharpness.

**Samsung QD-OLED** — 2D 3×3 kernel accounting for the triangular subpixel layout where Green is physically offset ~0.33px vertically from the Red/Blue row. Bilinear interpolation compensates for the offset. Red and Blue are kept tight to minimize color fringing on the triangle grid.

## Building

Requires Visual Studio 2022 Build Tools and CMake. FreeType and MinHook are fetched automatically.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output lands in `build/Release/`:
- `Injector.exe` — system tray loader
- `PureType.dll` — the rendering engine
- `puretype.ini` — configuration

## Usage

Put all three files in the same folder. Run `Injector.exe` — ideally as Administrator for full system-wide coverage. A tray icon appears.

Right-click the tray icon to:
- **Enable / Disable** the hook (toggle injection on/off)
- **Panel Type** → pick your display (LG WRGB or Samsung QD-OLED)
- **Exit**

Panel choice is saved to `puretype.ini` and persists across restarts.

The DLL auto-skips processes where injection would cause problems: system services (`csrss`, `lsass`, `dwm`), anti-cheat software (Vanguard, EAC, BattlEye), games with integrity checks, and browsers with strict sandboxes.

UWP apps (Calculator, Settings, the new Notepad) work out of the box — the injector automatically grants AppContainer read permissions on the DLL before setting the hook.

## Configuration

Edit `puretype.ini`:

QD-OLED
```ini
[General]
PanelType=QD_OLED_TRIANGLE
FilterStrength=1.0
Gamma=1.0
EnableSubpixelHinting=true
EnableFractionalPositioning=true
LODThresholdSmall=12.0
LODThresholdLarge=24.0
WOLEDCrossTalkReduction=0.0 
LumaContrastStrength=1.25
StemDarkeningEnabled=true
StemDarkeningStrength=0.35
```

RWBG
```ini
[General]
PanelType=RWBG 
FilterStrength=1.0
Gamma=1.0
EnableSubpixelHinting=true
EnableFractionalPositioning=true
LODThresholdSmall=12.0
LODThresholdLarge=24.0
WOLEDCrossTalkReduction=0.12 
LumaContrastStrength=1.45
StemDarkeningEnabled=true
StemDarkeningStrength=0.45
```

RGWB
```ini
[General]
PanelType=RGWB
FilterStrength=1.0
Gamma=1.0
EnableSubpixelHinting=true
EnableFractionalPositioning=true
LODThresholdSmall=12.0
LODThresholdLarge=24.0
WOLEDCrossTalkReduction=0.10 
LumaContrastStrength=1.40
StemDarkeningEnabled=true
StemDarkeningStrength=0.45
```

`Gamma` is **not** the display gamma — it's an optional exponent applied on top of the sRGB transfer curve. Leave it at 1.0 unless you have a specific reason to change it.

## How it works (briefly)

1. `Injector.exe` calls `SetWindowsHookEx(WH_CBT)` pointing at an exported callback in `PureType.dll`
2. Windows automatically loads `PureType.dll` into every process that creates a window
3. On load, the DLL checks the host process against a blacklist — if matched, it bails immediately
4. Otherwise it initializes FreeType, installs MinHook detours on GDI/DirectWrite text functions
5. When the app renders text, PureType intercepts the call, rasterizes with FreeType LCD mode, runs the subpixel filter, and composites to screen

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
