![PureType icon](src/puretype.png)


## Support me:
https://paypal.me/masterantonio

# PureType — Technical Documentation

PureType is a Windows DLL that injects into processes and corrects text rendering
for OLED displays. Windows ships ClearType, a subpixel rendering algorithm designed
in 2000 for RGB-stripe LCD panels. Modern OLED displays have fundamentally different
subpixel geometries — ClearType's output lands on the wrong physical emitters,
producing washed-out text and colour fringing. PureType intercepts GDI and
DirectWrite calls, extracts the per-channel coverage that ClearType encodes, and
re-blends using the actual subpixel layout of the target panel.

---

## The problem: why ClearType is wrong on OLED

### ClearType's model

ClearType assumes every physical pixel is composed of three horizontal subpixels in
RGB order (left to right). It oversamples glyph outlines at 3× horizontal resolution
and assigns the result to the R, G, and B channels independently. For a black glyph
on a white background:

```
pixel n:   R = coverage at subpixel n·3+0
           G = coverage at subpixel n·3+1
           B = coverage at subpixel n·3+2
```

This is correct on RGB-stripe LCDs. On OLED it is wrong.

### OLED subpixel layouts

**LG WOLED (RWBG and RGWB)**

LG WOLED panels add a fourth WHITE subpixel per pixel. The TCON drives the white
subpixel by simultaneously firing R, G, and B emitters at a shared luminance level.
The physical order is:

```
RWBG:  [ R ][ W ][ B ][ G ]   (4 subpixels per physical pixel)
RGWB:  [ R ][ G ][ W ][ B ]
```

ClearType assigns values to three channels assuming the pixel spans `[R][G][B]`.
The W subpixel is invisible to ClearType. When rendered naively, the W emitter fires
at the wrong intensity, producing a grey luminance haze on dark backgrounds.

**Samsung QD-OLED (triangular)**

Samsung QD-OLED panels arrange three subpixels per pixel in a triangular pattern.
Even and odd rows are horizontally offset by +0.5 physical subpixels:

```
Even row:  [ R ]    [ G ]    [ B ]
Odd  row:    [ G ]    [ B ]    [ R ]
             ↑ offset +0.5 subpixel relative to even row
```

ClearType's horizontal-stripe hinting pushes stem edges to positions optimised for
a flat row of R-G-B. On the triangular layout this creates visible misalignment on
diagonal strokes and produces the characteristic QD-OLED colour fringing.

---

## Architecture

### Hook strategy: post-processing

PureType intercepts `ExtTextOutW` (GDI) and `IDWriteTextRenderer::DrawGlyphRun`
(DirectWrite). The GDI path uses a post-processing approach: GDI renders normally
with all its own metrics intact, and PureType modifies only the resulting pixel
values. The DirectWrite path retains the FreeType rasterizer for panels where
FreeType's triangular-aware hinting is beneficial.

```
ExtTextOutW called by app
│
├─ capture HDC region before (background pixels)
├─ ForceSubpixelRender: clone LOGFONT with lfQuality=CLEARTYPE_QUALITY, SelectObject
├─ call original ExtTextOutW (GDI renders with ClearType — layout unmodified)
├─ ForceSubpixelRender destructor: restore original font, delete clone
├─ capture HDC region after (ClearType output, R≠G≠B always present)
├─ RemapToOLED(before, after, textColor, cfg)
└─ write remapped pixels back to HDC
```

**Why post-processing is necessary for universal compatibility**

Any approach that replaces GDI rendering (e.g. rasterizing with FreeType and
blitting the result) faces a fundamental conflict: frameworks like Qt, MFC, and
WPF compute glyph bounding boxes and advance widths from GDI metrics *before*
`ExtTextOutW` is called. If PureType substitutes a bitmap computed with different
metrics, it lands outside the reserved bounding box and is clipped. Post-processing
avoids this entirely — GDI handles all layout, PureType handles only pixel values.

### ForceSubpixelRender

When the system AA setting is grayscale or off, GDI writes `R == G == B` per pixel.
No per-channel subpixel coverage can be extracted. PureType solves this by
temporarily forcing ClearType for the duration of the GDI call:

```cpp
LOGFONTW lf;
GetObjectW(currentFont, sizeof(lf), &lf);
lf.lfQuality = CLEARTYPE_QUALITY;
HFONT ctFont = CreateFontIndirectW(&lf);
SelectObject(hdc, ctFont);         // GDI now renders ClearType
// ... call original ExtTextOutW ...
SelectObject(hdc, originalFont);   // restore
DeleteObject(ctFont);              // delete clone
```

`lfQuality` does not affect glyph metrics. `tmHeight`, `tmAscent`, advance widths,
and `GetTextExtentPoint32` return identical values for CLEARTYPE_QUALITY and
ANTIALIASED_QUALITY fonts of the same face and size. Framework bounding boxes
computed before the hook remain valid.

`CreateFontIndirectW` with `CLEARTYPE_QUALITY` respects the system ClearType
registry configuration (`HKCU\Software\Microsoft\Avalon.Graphics`: gamma, pixel
geometry, contrast). The rendering output is equivalent whether ClearType was
already active or had to be forced.

---

## Mathematics

### Step 1 — Coverage extraction

ClearType composites text over a background using per-channel alpha blending in
sRGB space:

```
rendered_ch = bg_ch × (1 − mask_ch) + text_ch × mask_ch
```

Solving for `mask_ch`:

```
mask_ch = (rendered_ch − bg_ch) / (text_ch − bg_ch)
```

All values are linearised before extraction (sRGB → linear):

```
maskR = (linCT_R − linBG_R) / (linText_R − linBG_R)
maskG = (linCT_G − linBG_G) / (linText_G − linBG_G)
maskB = (linCT_B − linBG_B) / (linText_B − linBG_B)
```

Each mask is clamped to [0, 1]. When `|text_ch − bg_ch| < 0.001` (text and
background are identical in that channel), the mask for that channel is 0.

These three values represent the fractional coverage of the left, centre, and right
subpixels of a ClearType RGB-stripe pixel at positions 1/6, 3/6, 5/6 within the
pixel width.

### Step 2 — OLED channel reconstruction

#### WOLED RWBG

RWBG subpixel centres: R=1/8, W=3/8, B=5/8, G=7/8.
ClearType subpixel centres: R=1/6, G=3/6, B=5/6.

Nearest-match assignment:

| WOLED subpixel | Physical centre | ClearType source | Mapping |
|---|---|---|---|
| R | 1/8 = 0.125 | CT R at 1/6 = 0.167 | `alpha_R = maskR` |
| W | 3/8 = 0.375 | CT G at 3/6 = 0.500 | `alpha_W = maskG × (1 − crossTalk)` |
| B | 5/8 = 0.625 | between CT G and CT B | `alpha_B = (maskG + maskB) / 2` |
| G | 7/8 = 0.875 | CT B at 5/6 = 0.833 | `alpha_G = maskB` |

The W subpixel drives R, G, B simultaneously via TCON hardware. Final channel
coverage uses `max()` to model the TCON `OR` behaviour:

```
finalR = max(alpha_R, alpha_W)
finalG = max(alpha_G, alpha_W)
finalB = max(alpha_B, alpha_W)
```

`crossTalk` (default 0.08) attenuates the W contribution before the merge,
compensating for the physical leakage between the W emitter and adjacent colour
emitters on the WOLED panel.

#### WOLED RGWB

RGWB subpixel centres: R=1/8, G=3/8, W=5/8, B=7/8.

| WOLED subpixel | ClearType source |
|---|---|
| R | `maskR` |
| G | `maskG` |
| W | `maskB × (1 − crossTalk)` |
| B | `maskB` |

```
finalR = max(maskR, alpha_W)
finalG = max(maskG, alpha_W)
finalB = max(maskB, alpha_W)
```

#### QD-OLED triangular

Three subpixels per pixel, approximately same horizontal centres as RGB stripe
(differences are in the vertical dimension and the inter-row offset). The ClearType
masks are used directly for horizontal coverage. The vertical geometry (triangular
offset between even/odd rows) is handled separately by the FreeType rasterizer in
the DWrite path via a 2D blending term.

```
finalR = maskR
finalG = maskG
finalB = maskB
```

### Step 3 — Chroma anchoring (chromaKeep)

To prevent subpixel coverage from introducing excessive colour fringing at the
perceptual level, the per-channel coverages are partially collapsed toward luma.
BT.709 luminance weights are used:

```
Y = 0.2126 × finalR + 0.7152 × finalG + 0.0722 × finalB

finalR = Y + (finalR − Y) × chromaKeep
finalG = Y + (finalG − Y) × chromaKeep
finalB = Y + (finalB − Y) × chromaKeep
```

`chromaKeep = 1.0` gives full subpixel colour output. `chromaKeep = 0.0` gives pure
luma — identical channels, no subpixel benefit. The operating range is 0.70–0.85,
depending on font size:

| Size | QD-OLED | RWBG/RGWB |
|---|---|---|
| ≤ 18 px | 0.70 | 0.72 |
| 19–24 px | 0.75 | 0.77 |
| 25–32 px | 0.80 | 0.82 |
| > 32 px | 0.83 | 0.85 |

Larger text receives more chroma because wider stems span multiple subpixels
cleanly, making full subpixel colouring perceptually correct rather than distracting.

### Step 4 — S-curve readability boost

An S-curve is applied to each coverage value to increase stem contrast and compensate
for OLED's high peak brightness, which makes mid-coverage text look lighter than on
LCD:

```
c = 1 − (1 − c)^exp     [toe expansion]
if c > 0.20:
    c = min(1.0, c × gain)   [shoulder clip]
```

`exp` and `gain` are derived from `lumaContrastStrength` and the font size:

```
expBase  = (qdPanel ? 1.01 : 1.03) × (1 + (contrastStrength − 1) × 0.5)
finalExp = expBase + (qdPanel ? 0.10 : 0.16) × sizeBoost
```

`sizeBoost = clamp((24 − height) / 24, 0, 1)` — small text gets a stronger
boost because small stems have lower absolute coverage values and benefit more
from toe expansion.

The LUT is pre-computed per call (1024 entries, float) to keep the per-pixel
inner loop cost to a single array lookup.

### Step 5 — Final linear-light blend

The corrected per-channel coverage values are used to blend from background to
text colour in linear light:

```
outR = bgR × (1 − finalR) + textR × finalR
outG = bgG × (1 − finalG) + textG × finalG
outB = bgB × (1 − finalB) + textB × finalB
```

The result is re-encoded to sRGB and written back to the HDC. This is the correct
compositing formula for emissive displays: OLED pixels emit light linearly, and
blending must be performed in the linear domain to avoid the systematic darkening
that occurs when blending in the sRGB gamma-compressed domain.

---

## FreeType rasterizer (DWrite path)

The DirectWrite hook retains a FreeType rasterizer for the DWrite rendering path.
Key parameters:

**Hinting target (Bug 3 fix)**

| Panel | FreeType load flag | Rationale |
|---|---|---|
| RWBG / RGWB | `FT_LOAD_TARGET_LCD` | Horizontal-stripe hinting; biases stem edges toward horizontal subpixel boundaries |
| QD-OLED triangular | `FT_LOAD_TARGET_NORMAL` | Isotropic hinting; no horizontal-stripe bias for triangular layout |

Both paths use `FT_RENDER_MODE_LCD` to produce the 3× horizontal oversampled
bitmap that the subpixel filter expects.

**Phase normalisation**

FreeType's fractional positioning grid has 64 units per pixel. The subpixel grid
has 3 positions per pixel, giving a step of 64/3 ≈ 21.3 units. Phase values are
normalised with `% 3` and mapped:

```
phaseVec.x = (normPhaseX × 21) + oledOffsetX
phaseVec.y = (normPhaseY × 21) + oledOffsetY
```

QD-OLED uses `oledOffsetX = 32` (0.5 px) and `oledOffsetY = 21` (~0.33 px) to
centre the rasterization on the triangular subpixel geometry.

**TriangularFilter 2D blending (QD-OLED)**

QD-OLED even and odd rows are offset by ±1.5 subpixels. The filter blends 25% from
the adjacent row, shifted by the offset:

```
even row: sample = 0.75 × row[y][x] + 0.25 × row[y+1][x + 1.5]
odd  row: sample = 0.75 × row[y][x] + 0.25 × row[y−1][x − 1.5]
```

This approximates the effective coverage a triangular subpixel sees from both the
rasterized row above and below it.

---

## Configuration reference

All parameters live in `puretype.ini` next to the DLL.

| Key | Type | Range | Default | Description |
|---|---|---|---|---|
| `panelType` | enum | `rwbg`, `rgwb`, `qd_oled_triangle` | `rwbg` | Physical subpixel layout of the target display |
| `filterStrength` | float | 0.0 – 5.0 | 1.0 | Master filter intensity. 0 = passthrough |
| `gamma` | float | 0.5 – 3.0 | 1.0 | Rasterization gamma correction |
| `enableSubpixelHinting` | bool | — | true | FreeType `FT_LOAD_FORCE_AUTOHINT` |
| `enableFractionalPositioning` | bool | — | true | Sub-pixel X placement |
| `lodThresholdSmall` | float | 6 – 96 | 10.0 | px below which small-text path activates |
| `lodThresholdLarge` | float | lodSmall+1 – 160 | 22.0 | px above which large-text path activates |
| `woledCrossTalkReduction` | float | 0.0 – 1.0 | 0.08 | W subpixel attenuation factor (WOLED only) |
| `lumaContrastStrength` | float | 1.0 – 3.0 | 1.15 | S-curve contrast multiplier |
| `stemDarkeningEnabled` | bool | — | true | Darken thin vertical stems |
| `stemDarkeningStrength` | float | 0.0 – ∞ | 0.25 | Stem darkening intensity |
| `debug.enabled` | bool | — | false | Write log file |
| `debug.logFile` | string | — | PURETYPE.log | Log file path (relative to DLL) |
| `debug.highlightRenderedGlyphs` | bool | — | false | Tint PureType-rendered glyphs cyan |

---

## Injection mechanism

PureType exports a CBT hook procedure (`PureTypeCBTProc`) which Windows uses to
force the DLL into target processes via `SetWindowsHookEx(WH_CBT, ...)`. The
hook procedure itself does nothing — its only purpose is to trigger DLL load.

`DllMain(DLL_PROCESS_ATTACH)` runs the initialisation sequence:

1. Load `puretype.ini` from the DLL directory
2. Initialise sRGB ↔ linear LUTs
3. Initialise FreeType
4. Install GDI hooks (`ExtTextOutW`, `PolyTextOutW`, `DrawTextW`, `DrawTextExW`)
5. Install DWrite hooks (`DWriteCreateFactory`, `IDWriteFactory::CreateTextLayout`,
   `IDWriteTextLayout::Draw`)
6. Enable all MinHook hooks atomically

Shutdown (`DLL_PROCESS_DETACH`) disables all hooks, spin-waits up to 100 ms for
in-flight hook calls to complete (tracked via `g_activeHookCount`), then removes
hooks and shuts down FreeType.

A process blacklist prevents injection into system processes (`csrss.exe`,
`lsass.exe`, `dwm.exe`, etc.), anti-cheat services, and game executables where
DLL injection would cause detections or instability.

---

## Building

Dependencies: FreeType 2, MinHook, Windows SDK (Direct2D, DirectWrite).

The project targets Windows 10 1903+ (Direct2D 1.1, DWrite 3). Both x64 and x86
configurations are required for injection into 32-bit processes.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Output: `puretype.dll` (place alongside `puretype.ini` in the injection directory).
