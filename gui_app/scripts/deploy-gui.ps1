# Deploy (copy only) the already-built FlashDEG GUI into a distribution folder.
# Does NOT build — run `npm run build:win` (or `tauri build -- --no-bundle`)
# first. Updates ONLY the GUI files: `flashdeg-gui.exe` and `gene_maps/`.
# Anything else you placed in the folder by hand (the flashdeg engine,
# openblas.dll, LICENSE(S), README.url, …) is LEFT UNTOUCHED. The folder is
# created if missing. Existing GUI files are overwritten only after you confirm
# (or pass -Force).
#
# Output layout (default <repo>/dist/FlashDEG) — managed entries marked *:
#   FlashDEG/
#     flashdeg-gui.exe          *
#     gene_maps/                *
#       gene_symbols_human.tsv.gz
#       gene_symbols_fly.tsv.gz
#     (flashdeg.exe, openblas.dll, LICENSE(S), README.url, … added by you — kept)
#
# Usage:
#   npm run deploy:win                      # asks before overwriting
#   npm run deploy:win -- -Force            # overwrite without asking
#   .\scripts\deploy-gui.ps1 -OutDir D:\somewhere\FlashDEG

[CmdletBinding()]
param(
    # Where to place the GUI files. Defaults to <repo>/dist/FlashDEG.
    [string]$OutDir = "",
    # Overwrite existing GUI files without prompting.
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$guiAppDir = Split-Path -Parent $scriptDir
$repoRoot = Split-Path -Parent $guiAppDir

if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $repoRoot "dist\FlashDEG"
}

$exePath = Join-Path $guiAppDir "src-tauri\target\release\flashdeg-gui.exe"
$geneMapsDir = Join-Path $guiAppDir "gene_maps"

# Require an existing build — this script never builds.
if (-not (Test-Path $exePath)) {
    throw "GUI executable not found: $exePath`nBuild first: npm run build:win (or 'npm run tauri:build -- --no-bundle')."
}
if (-not (Test-Path $geneMapsDir)) {
    throw "gene_maps folder not found: $geneMapsDir"
}

# Update only the GUI files in place. Never wipe the folder.
Write-Host "==> Updating GUI files in $OutDir" -ForegroundColor Cyan

$outGeneMaps = Join-Path $OutDir "gene_maps"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
New-Item -ItemType Directory -Force -Path $outGeneMaps | Out-Null

# Plan the copies: source -> destination for each managed file.
$copies = @(
    [pscustomobject]@{ Src = $exePath; Dst = (Join-Path $OutDir "flashdeg-gui.exe") }
)
foreach ($gm in (Get-ChildItem -LiteralPath $geneMapsDir -Filter *.tsv.gz -File)) {
    $copies += [pscustomobject]@{ Src = $gm.FullName; Dst = (Join-Path $outGeneMaps $gm.Name) }
}

# Files that already exist would be OVERWRITTEN — ask first (unless -Force).
$existing = @($copies | Where-Object { Test-Path -LiteralPath $_.Dst })
if ($existing.Count -gt 0 -and -not $Force) {
    Write-Host "These existing files will be OVERWRITTEN:" -ForegroundColor Yellow
    $existing | ForEach-Object { Write-Host ("  " + $_.Dst.Substring($OutDir.Length + 1)) }
    $answer = Read-Host "Overwrite them? [y/N]"
    if ($answer -notmatch '^(y|yes)$') {
        Write-Host "Aborted — no files changed." -ForegroundColor Yellow
        exit 0
    }
}

foreach ($c in $copies) {
    Copy-Item -LiteralPath $c.Src -Destination $c.Dst -Force
}

# Report.
Write-Host ""
Write-Host "Deployed GUI files:" -ForegroundColor Green
$copies | ForEach-Object { Write-Host ("  " + $_.Dst.Substring($OutDir.Length + 1)) }
Write-Host ""
Write-Host "Other files in the folder were left untouched." -ForegroundColor DarkGray
Write-Host "Reminder: the flashdeg engine (flashdeg.exe [+ openblas.dll]) is NOT bundled — add it yourself." -ForegroundColor Yellow
