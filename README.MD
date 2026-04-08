<p align="center">
  <img src="puretype_logo.png" alt="PureType" width="96"/>
</p>

<h1 align="center">PureType</h1>

<p align="center">
  <strong>OLED-aware subpixel text rendering for Windows</strong><br/>
  <em>Sharper, cleaner text on LG WOLED &amp; Samsung QD-OLED displays.</em>
</p>

<p align="center">
  <a href="#installation">Installation</a> · <a href="#graphical-interface">UI Guide</a> · <a href="#manual-configuration">Manual INI</a> · <a href="https://paypal.me/masterantonio">Donate</a>
</p>

---

> ⚠️ **Notice regarding the Repository**: PureType has transitioned to a closed-source development model. This public repository now serves strictly as the official hub for downloading **Releases**, reading documentation, and tracking community **Issues**. 

---

## Why PureType?

ClearType was designed in 2000 for RGB-stripe LCD panels. Modern OLED displays, LG WOLED and Samsung QD-OLED, have completely different subpixel layouts:

- **WOLED** panels add a fourth **white** subpixel that ClearType does not model.
- **QD-OLED** panels arrange subpixels in a **triangular** pattern with a half-pixel vertical offset between rows.

Both layouts produce incorrect colour fringing and a luminance haze when rendered with standard ClearType. PureType intercepts GDI and DirectWrite text rendering calls and remaps per-channel coverage to the physical subpixel geometry of your panel.

Subpixel center positions are derived from **panel microscopy** — macro photographs with individual subpixels lit individually — rather than geometric assumptions.

Discord: https://discord.gg/c2U8QbHzv9

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

1. Download the latest release `.zip` from the **Releases** tab and extract it to a permanent directory (e.g. `C:\PureType\`).
2. Run **`puretype.exe`**. The application starts minimized in the **system tray**.
3. **Double-click** the tray icon to open the graphical configuration UI.
4. Select your OLED panel type and choose a **Quick Preset** (Balanced, Sharp, or Clean).
5. Click **Save & Apply**. Done — changes are applied instantly.

> **Note:** PureType automatically disables Windows ClearType when it activates and restores it on exit. No system-wide settings need to be changed manually.

---

## Graphical Interface

PureType includes a modern **WPF-based UI** (`PuretypeUI.exe`, .NET 9) with sidebar navigation and a live before/after preview. You can configure everything without editing `puretype.ini` manually.

### Tabs

| Tab             | What it does                                                                                                                                                                 |
|-----------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Overview** | Live rendered preview (standard AA vs. PureType), quick preset chips (Balanced / Sharp / Clean), active configuration summary.                                               |
| **Rendering** | Panel type, filter strength, WOLED crosstalk reduction, gamma mode & correction, OLED gamma output, luma contrast, subpixel hinting, fractional positioning, stem darkening. |
| **Display** | LOD glyph-size thresholds, high-DPI fade-out thresholds.                                                                                                                     |
| **System Font** | Install/restore the bundled **Inter** variable font as the system UI font. Adjust weight, optical size, and letter spacing axes with live preview.                           |
| **Profiles** | Create per-monitor or per-application override profiles.                                                                                                                     |
| **Settings** | Start with Windows, debug logging, glyph highlight overlay, process blacklist.                                                                                               |
| **Info** | Version, author, GitHub and donate links, license.                                                                                                                           |

---

## Manual Configuration & Profiles

All settings live in **`puretype.ini`** (same directory as the DLL). The UI reads and writes this file, but advanced users can edit it directly with any text editor. Changes take effect after clicking **Save & Apply** in the UI, or by right-clicking the tray icon → **Disable** then **Enable** PureType.

### Profiles: per-monitor and per-application overrides
Profiles let you override any setting for a specific monitor or application. The app resolves values with this priority:
**App profile → Monitor profile → Global (`[general]`)**

*To configure profiles easily, use the **Profiles** tab in the UI.*

---

## How it works

PureType hooks `ExtTextOutW` (GDI) and DirectWrite draw calls. For each text draw call:

1. The screen region is captured **before** rendering.
2. The renderer draws normally — layout, metrics, and clipping remain untouched.
3. The region is captured **after** rendering.
4. Per-channel subpixel coverage is extracted from the before/after delta.
5. Coverage values are **remapped** to the physical subpixel positions of the target OLED panel.
6. The corrected pixels are written back using per-pixel alpha — background pixels are never touched.

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

You can customize the injection blacklist from **Settings → Process Blacklist** or via the `puretype.ini` file.

---

## Reporting issues

Use the GitHub issue templates in this repository:

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

**Copyright (c) 2026-2027 Antonio (Toriga). All rights reserved.**

PureType is proprietary software for personal use. You may not reverse engineer, decompile, modify, or distribute this software commercially without permission. See the `LICENSE` file for full details.

---

<p align="center">
  <strong>Support the project:</strong> <a href="https://paypal.me/masterantonio">paypal.me/masterantonio</a><br/>
  <strong>Patreon:</strong> <a href="https://www.patreon.com/Puretype">https://www.patreon.com/Puretype</a><br/>
  <strong>Author:</strong> <a href="https://github.com/Master-Antonio">Toriga</a>
</p>