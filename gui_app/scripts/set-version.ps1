# Single point of control for the FlashDEG GUI version. Updates every place the
# GUI version is declared so they never drift:
#   package.json           (npm)
#   package-lock.json      (npm lockfile: root + packages."")
#   src-tauri/Cargo.toml   (Rust crate)
#   src-tauri/Cargo.lock   (Rust lockfile: flashdeg-gui entry)
#   src-tauri/tauri.conf.json  (Tauri product version → exe resource / bundle)
#
# Does NOT touch the MSI / installer (installer/ is managed separately).
# The generated Windows resource (target/.../resource.rc) refreshes on rebuild.
#
# Usage:
#   .\scripts\set-version.ps1 1.0.1

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string] $Version
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($Version -notmatch '^\d+\.\d+\.\d+([-+].+)?$') {
    throw "Version must be semver (e.g. 1.0.1), got: '$Version'"
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$guiDir = Split-Path -Parent $scriptDir
$srcTauri = Join-Path $guiDir "src-tauri"

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# Apply a regex (optionally first-match-only) to a file, preserving its
# encoding/line endings, and assert it actually changed something.
function Update-File {
    param(
        [string] $Path,
        [string] $Pattern,
        [string] $Replacement,
        [int] $Count = -1,   # -1 = replace all
        [string] $Label
    )
    if (-not (Test-Path -LiteralPath $Path)) { throw "Not found: $Path" }
    $text = [System.IO.File]::ReadAllText($Path)
    $rx = [regex]::new($Pattern)
    $new = if ($Count -ge 0) { $rx.Replace($text, $Replacement, $Count) } else { $rx.Replace($text, $Replacement) }
    if ($new -eq $text) {
        throw "No version field matched in $Label ($Path) — pattern needs updating."
    }
    [System.IO.File]::WriteAllText($Path, $new, $utf8NoBom)
    Write-Host ("  updated {0}" -f $Label)
}

$pkgJson = Join-Path $guiDir "package.json"
$pkgLock = Join-Path $guiDir "package-lock.json"
$cargoToml = Join-Path $srcTauri "Cargo.toml"
$cargoLock = Join-Path $srcTauri "Cargo.lock"
$tauriConf = Join-Path $srcTauri "tauri.conf.json"

$verRepl = '${1}' + $Version + '${2}'

Write-Host "Setting FlashDEG GUI version to $Version"

# package.json — the root "version" is the first one in the file.
Update-File -Path $pkgJson -Label "package.json" -Count 1 `
    -Pattern '("version":\s*")[^"]*(")' -Replacement $verRepl

# tauri.conf.json — the product "version" is the first one in the file.
Update-File -Path $tauriConf -Label "tauri.conf.json" -Count 1 `
    -Pattern '("version":\s*")[^"]*(")' -Replacement $verRepl

# Cargo.toml — the [package] version (only line starting with `version = "`).
Update-File -Path $cargoToml -Label "Cargo.toml" `
    -Pattern '(?m)(^version = ")[^"]*(")' -Replacement $verRepl

# Cargo.lock — the flashdeg-gui package entry.
Update-File -Path $cargoLock -Label "Cargo.lock" `
    -Pattern '(name = "flashdeg-gui"\r?\nversion = ")[^"]*(")' -Replacement $verRepl

# package-lock.json — both flashdeg-gui occurrences (root + packages."").
Update-File -Path $pkgLock -Label "package-lock.json" `
    -Pattern '("name":\s*"flashdeg-gui",\s*"version":\s*")[^"]*(")' -Replacement $verRepl

# Validate the JSON files still parse. -AsHashTable so package-lock.json's
# empty-string package key ("") is accepted.
foreach ($j in @($pkgJson, $pkgLock, $tauriConf)) {
    try { Get-Content -Raw -LiteralPath $j | ConvertFrom-Json -AsHashTable | Out-Null }
    catch { throw "Invalid JSON after edit: $j — $_" }
}

Write-Host "Done. Lockfiles are patched; a later `npm install` / `cargo build` will confirm."
