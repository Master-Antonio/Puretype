---
name: Panel compatibility
about: New panel type, subpixel measurement, or calibration data for an existing panel
title: "[PANEL] "
labels: ''
assignees: ''

---

## Panel information

| Field | Value |
|---|---|
| Manufacturer | <!-- LG / Samsung / Other --> |
| Model | <!-- e.g. Dell AW3225QF --> |
| Panel technology | <!-- WOLED / QD-OLED / Other --> |
| Resolution | <!-- e.g. 3440×1440 --> |
| Size | <!-- e.g. 34" --> |
| Current `panelType` used | <!-- or "none / unknown" --> |

## What is the issue

- [ ] This panel is not listed — it needs a new `panelType` value
- [ ] The existing `panelType` produces visible fringing or wrong colours
- [ ] I have subpixel measurement data that could improve the interpolation weights
- [ ] Other: _______________

## Subpixel layout (if known)

<!-- 
If you have a macro photo of the panel with individual subpixels lit,
attach it here. Sharp, in-focus images are most useful for measuring
subpixel center positions.

If you know the layout already, describe it:
  e.g. RWBG stripe, W ≈ 40% width, R = B = G ≈ 20% each
-->

## Measured subpixel centers (if available)

<!-- 
Normalised positions within one pixel (0.0 = left edge, 1.0 = right edge).
Leave blank if unknown.
-->

| Subpixel | Measured center |
|---|---|
| R | |
| G (or W) | |
| B (or W) | |
| G (WOLED only) | |

## Current rendering quality

<!-- Describe what you observe: fringing direction/colour, text sharpness, etc. -->

## Additional context

<!-- Links to panel reviews with subpixel photos (e.g. pcmonitors.info, rtings.com), datasheets, etc. -->
