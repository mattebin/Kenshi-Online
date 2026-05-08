param(
    [string]$Version = "alpha-2026-05-08",
    [string]$BuildDir = "",
    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $RepoRoot "build-codex\bin\Release"
}
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $RepoRoot "release"
}

$BuildDir = (Resolve-Path $BuildDir).Path
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$PackageName = "KenshiMP-$Version"
$StageDir = Join-Path $OutputDir $PackageName
$ZipPath = Join-Path $OutputDir "$PackageName-installer.zip"

if (Test-Path -LiteralPath $StageDir) {
    Remove-Item -LiteralPath $StageDir -Recurse -Force
}
if (Test-Path -LiteralPath $ZipPath) {
    Remove-Item -LiteralPath $ZipPath -Force
}
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null

function Copy-Required {
    param(
        [string]$Source,
        [string]$DestinationName = ""
    )
    if (-not (Test-Path -LiteralPath $Source)) {
        throw "Required release file missing: $Source"
    }
    if ([string]::IsNullOrWhiteSpace($DestinationName)) {
        $DestinationName = Split-Path $Source -Leaf
    }
    Copy-Item -LiteralPath $Source -Destination (Join-Path $StageDir $DestinationName) -Force
}

function Copy-Optional {
    param(
        [string]$Source,
        [string]$DestinationName = ""
    )
    if (Test-Path -LiteralPath $Source) {
        if ([string]::IsNullOrWhiteSpace($DestinationName)) {
            $DestinationName = Split-Path $Source -Leaf
        }
        Copy-Item -LiteralPath $Source -Destination (Join-Path $StageDir $DestinationName) -Force
    }
}

$requiredBinaries = @(
    "KenshiMP.Core.dll",
    "KenshiMP.Server.exe",
    "KenshiMP.TestClient.exe"
)

$optionalBinaries = @(
    "KenshiMP.SafeAddon.dll",
    "KenshiMP.Dashboard.exe",
    "KenshiMP.MasterServer.exe",
    "KenshiMP.Injector.exe",
    "KenshiMP.LogTail.exe",
    "KenshiMP.Cartographer.exe",
    "KenshiMP.CrashWatchdog.exe",
    "KenshiMP.Probe.exe"
)

foreach ($file in $requiredBinaries) {
    Copy-Required (Join-Path $BuildDir $file)
}
foreach ($file in $optionalBinaries) {
    Copy-Optional (Join-Path $BuildDir $file)
}

$distFiles = @(
    "install.bat",
    "uninstall.bat",
    "KenshiMP.Restore.bat",
    "KenshiMP.SwitchAddon.bat",
    "README.md",
    "JOINING.md",
    "KNOWN_ISSUES.md",
    "RELEASE_NOTES.md",
    "server.json",
    "kenshi-online.mod",
    "Kenshi_MainMenu.layout",
    "Kenshi_MultiplayerPanel.layout",
    "Kenshi_MultiplayerHUD.layout"
)

foreach ($file in $distFiles) {
    Copy-Required (Join-Path $RepoRoot "dist\$file")
}

$branch = ""
$commit = ""
$dirty = ""
try { $branch = (& git -C $RepoRoot rev-parse --abbrev-ref HEAD).Trim() } catch {}
try { $commit = (& git -C $RepoRoot rev-parse --short HEAD).Trim() } catch {}
try {
    $status = (& git -C $RepoRoot status --short)
    $dirty = if ($status) { "yes" } else { "no" }
} catch {}

$manifest = @"
KenshiMP release package
Version: $Version
Built: $(Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz")
Source branch: $branch
Source commit: $commit
Working tree dirty when packaged: $dirty

Install:
  1. Extract this zip.
  2. Run install.bat.
  3. Read README.md and JOINING.md.
"@

Set-Content -LiteralPath (Join-Path $StageDir "VERSION.txt") -Value $manifest -Encoding ASCII

Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ZipPath -CompressionLevel Optimal

Write-Host "Created release package:"
Write-Host "  $ZipPath"
Write-Host ""
Write-Host "Package contents:"
Get-ChildItem -LiteralPath $StageDir | Sort-Object Name | Select-Object Name,Length | Format-Table -AutoSize
