---
name: Bug report
about: Text rendering issue, visual artifact, crash or unexpected behaviour
title: "[BUG] "
labels: bug
assignees: ''

---

## Description

<!-- A clear description of what is happening and what you expected instead. -->

## Panel type

<!-- Check the one that applies. -->

- [ ] LG WOLED — RWBG (`panelType = rwbg`)
- [ ] LG WOLED — RGWB (`panelType = rgwb`)
- [ ] Samsung QD-OLED Gen 1-2 — AW3423DW, AW3423DWF, Odyssey G8 34" gen1 (`panelType = qd_oled_gen1`)
- [ ] Samsung QD-OLED Gen 3 — Odyssey G8 27" QHD, AW2725DF, 32" 4K models (`panelType = qd_oled_gen3`)
- [ ] Samsung QD-OLED Gen 4 — MSI MPG 272URX, 27" 4K UHD 2024-2025 (`panelType = qd_oled_gen4`)
- [ ] Other / Unknown: _______________

**Monitor model:** <!-- e.g. LG 27GR95QE-B -->

## Environment

| Field | Value |
|---|---|
| Windows version | <!-- e.g. Windows 11 23H2 --> |
| PureType version | <!-- e.g. v0.0.4 --> |
| Application where bug occurs | <!-- e.g. Notepad, EqualizerAPO, Chrome --> |
| Rendering framework (if known) | <!-- Win32 / Qt5 / Qt6 / WPF / DirectWrite / Unknown --> |
| ClearType system setting | <!-- On / Off / Unknown --> |
| DPI / scaling | <!-- e.g. 100%, 125%, 150% --> |

## puretype.ini

```ini
<!-- Paste the full contents of your puretype.ini here -->
```

## Steps to reproduce

1. 
2. 
3. 

## What you see

<!-- Screenshot or description. If text is invisible, white, or has wrong colour, note the background colour too. -->

## What you expected

<!-- What the text should look like. -->

## Debug log

<!-- 
Enable debug logging by setting in puretype.ini:
  [debug]
  enabled = true
  logFile = PURETYPE.log

Reproduce the issue, then paste the relevant portion of PURETYPE.log here.
-->

<details>
<summary>PURETYPE.log</summary>

```
<!-- paste log here -->
```

</details>

## Additional context

<!-- Any other information: multi-monitor setup, HDR, hardware-accelerated GPU scheduling, etc. -->
