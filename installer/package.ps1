# Locus release packaging.
#
# Stages a portable payload from the Release build, produces:
#   dist\Locus-<version>-win-x64\           (staged tree)
#   dist\Locus-<version>-win-x64.zip        (portable bundle)
#   dist\Locus-Setup-<version>-win-x64.exe  (Inno Setup installer)
#
# The GGUF models are NOT bundled - only the download scripts. Users pick a
# profile in the installer (or run a script from the zip).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File installer\package.ps1
#   powershell ... -Version 0.1.0-beta -SkipInstaller
#
[CmdletBinding()]
param(
    [string]$Version = '0.1.0-beta',
    [string]$Config  = 'Release',
    [switch]$SkipZip,
    [switch]$SkipInstaller
)

$ErrorActionPreference = 'Stop'

# Repo root = parent of this script's dir.
$RepoRoot   = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$BuildDir   = Join-Path $RepoRoot "build\release\$Config"
$DistRoot   = Join-Path $RepoRoot 'dist'
$StageName  = "Locus-$Version-win-x64"
$StageDir   = Join-Path $DistRoot $StageName

function Need($path, $what) {
    if (-not (Test-Path $path)) { throw "Missing $what : $path. Build Release first." }
}

Write-Host "Locus packaging - version $Version, config $Config"
Write-Host "  repo : $RepoRoot"
Write-Host "  build: $BuildDir"
Write-Host ""

# --- Verify build outputs ----------------------------------------------------
$GuiExe = Join-Path $BuildDir 'locus_gui.exe'
$CliExe = Join-Path $BuildDir 'locus.exe'
$Pdfium = Join-Path $BuildDir 'pdfium.dll'
$Prism  = Join-Path $BuildDir 'resources\prism'
Need $GuiExe 'locus_gui.exe'
Need $Pdfium 'pdfium.dll'
Need $Prism  'resources\prism'

# --- Stage the portable tree -------------------------------------------------
if (Test-Path $StageDir) { Remove-Item $StageDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir 'resources') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir 'models')    | Out-Null

Copy-Item $GuiExe $StageDir
if (Test-Path $CliExe) { Copy-Item $CliExe $StageDir }  # CLI is optional but nice to ship
Copy-Item $Pdfium $StageDir
Copy-Item $Prism (Join-Path $StageDir 'resources') -Recurse

# Model download scripts (ps1 + sh) + README. NEVER the GGUFs.
foreach ($f in @('download.ps1','download-small.ps1','download.sh','download-small.sh')) {
    $src = Join-Path $RepoRoot "models\$f"
    if (Test-Path $src) { Copy-Item $src (Join-Path $StageDir 'models') }
}
Copy-Item (Join-Path $RepoRoot 'installer\models-README.txt') (Join-Path $StageDir 'models\README.txt')

# Top-level docs.
Copy-Item (Join-Path $RepoRoot 'LICENSE')    $StageDir
Copy-Item (Join-Path $RepoRoot 'README.md')  $StageDir
if (Test-Path (Join-Path $RepoRoot 'QUICKSTART.md')) {
    Copy-Item (Join-Path $RepoRoot 'QUICKSTART.md') $StageDir
}

Write-Host "[stage] $StageDir"
Get-ChildItem $StageDir -Recurse -File | ForEach-Object {
    Write-Host ("        {0,10:N0}  {1}" -f $_.Length, $_.FullName.Substring($StageDir.Length + 1))
}
Write-Host ""

# --- Zip ---------------------------------------------------------------------
if (-not $SkipZip) {
    $Zip = Join-Path $DistRoot "$StageName.zip"
    if (Test-Path $Zip) { Remove-Item $Zip -Force }
    Compress-Archive -Path "$StageDir\*" -DestinationPath $Zip -CompressionLevel Optimal
    $zipSize = (Get-Item $Zip).Length
    Write-Host ("[zip  ] {0}  ({1:N1} MB)" -f $Zip, ($zipSize / 1MB))
}

# --- Installer ---------------------------------------------------------------
if (-not $SkipInstaller) {
    $Iscc = @(
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1

    if (-not $Iscc) {
        Write-Warning "ISCC.exe not found - skipping installer. Install Inno Setup 6 (winget install JRSoftware.InnoSetup)."
    } else {
        $Iss = Join-Path $RepoRoot 'installer\locus.iss'
        & $Iscc "/DAppVersion=$Version" "/DPayloadDir=$StageDir" $Iss
        if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }
        $Setup = Join-Path $DistRoot "Locus-Setup-$Version-win-x64.exe"
        if (Test-Path $Setup) {
            $setupSize = (Get-Item $Setup).Length
            Write-Host ("[setup] {0}  ({1:N1} MB)" -f $Setup, ($setupSize / 1MB))
        }
    }
}

Write-Host ""
Write-Host "Done. Artifacts in $DistRoot"
