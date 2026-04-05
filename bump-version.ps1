<#
.SYNOPSIS
    Bumps the PureType version number.

.DESCRIPTION
    Increments the version (major, minor, or patch) in all relevant files:
      - PuretypeUI/AppVersion.cs
      - CMakeLists.txt
    - include/puretype.h

.PARAMETER Part
    Which part to increment: major, minor, or patch (default: patch).

.EXAMPLE
    .\bump-version.ps1              # 0.1.0 -> 0.1.1
    .\bump-version.ps1 -Part minor  # 0.1.0 -> 0.2.0
    .\bump-version.ps1 -Part major  # 0.1.0 -> 1.0.0
#>
param(
    [ValidateSet("major", "minor", "patch")]
    [string]$Part = "patch"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir = $scriptDir  # script lives at repo root

$versionFile = Join-Path $rootDir "PuretypeUI\AppVersion.cs"
$cmakeFile   = Join-Path $rootDir "CMakeLists.txt"
$headerFile  = Join-Path $rootDir "include\puretype.h"

# ── Read current version from AppVersion.cs ──────────────────────────────
$versionContent = Get-Content $versionFile -Raw
if ($versionContent -notmatch 'Current\s*=\s*"(\d+)\.(\d+)\.(\d+)"') {
    Write-Error "Cannot find version string in $versionFile"
    exit 1
}

[int]$major = $Matches[1]
[int]$minor = $Matches[2]
[int]$patch = $Matches[3]
$oldVersion = "$major.$minor.$patch"

# ── Bump ─────────────────────────────────────────────────────────────────
switch ($Part) {
    "major" { $major++; $minor = 0; $patch = 0 }
    "minor" { $minor++; $patch = 0 }
    "patch" { $patch++ }
}
$newVersion = "$major.$minor.$patch"

Write-Host "Bumping version: $oldVersion -> $newVersion ($Part)" -ForegroundColor Cyan

# ── Update AppVersion.cs ─────────────────────────────────────────────────
$versionContent = $versionContent -replace ('Current\s*=\s*"' + [regex]::Escape($oldVersion) + '"'), "Current = `"$newVersion`""
Set-Content -Path $versionFile -Value $versionContent -NoNewline -Encoding UTF8
Write-Host "  Updated: $versionFile" -ForegroundColor Green

# ── Update CMakeLists.txt ────────────────────────────────────────────────
if (Test-Path $cmakeFile) {
    $cmakeContent = Get-Content $cmakeFile -Raw
    # CMake uses project(PureType VERSION x.y.z ...)
    $updatedCmake = [regex]::Replace(
        $cmakeContent,
        'project\(PureType\s+VERSION\s+\d+\.\d+\.\d+',
        "project(PureType VERSION $newVersion",
        1)

    if ($updatedCmake -eq $cmakeContent) {
        Write-Error "Failed to update CMakeLists.txt project version"
        exit 1
    }

    Set-Content -Path $cmakeFile -Value $updatedCmake -NoNewline -Encoding UTF8
    Write-Host "  Updated: $cmakeFile" -ForegroundColor Green
}

# ── Update include/puretype.h version macros ─────────────────────────────
if (Test-Path $headerFile) {
    $headerContent = Get-Content $headerFile -Raw

    $updatedHeader = $headerContent
    $updatedHeader = [regex]::Replace($updatedHeader, 'PURETYPE_VERSION_MAJOR\s+\d+', "PURETYPE_VERSION_MAJOR $major", 1)
    $updatedHeader = [regex]::Replace($updatedHeader, 'PURETYPE_VERSION_MINOR\s+\d+', "PURETYPE_VERSION_MINOR $minor", 1)
    $updatedHeader = [regex]::Replace($updatedHeader, 'PURETYPE_VERSION_PATCH\s+\d+', "PURETYPE_VERSION_PATCH $patch", 1)

    if ($updatedHeader -eq $headerContent) {
        Write-Error "Failed to update include/puretype.h version macros"
        exit 1
    }

    Set-Content -Path $headerFile -Value $updatedHeader -NoNewline -Encoding UTF8
    Write-Host "  Updated: $headerFile" -ForegroundColor Green
}

Write-Host ""
Write-Host "Version bumped to $newVersion successfully!" -ForegroundColor Green
Write-Host "Rebuild PuretypeUI to see the change in the app." -ForegroundColor Yellow

