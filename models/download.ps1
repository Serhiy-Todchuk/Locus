# Locus - download recommended embedding + reranker models (multilingual).
#
#   bge-m3              Q8_0   ~635 MB   1024-dim, 8K ctx, multilingual + code
#   bge-reranker-v2-m3  Q8_0   ~636 MB   cross-encoder reranker, multilingual
#
# Total: ~1.27 GB. Both files land next to this script in models/.
# Already-downloaded files are skipped (size-checked).
#
# Usage:
#   pwsh ./models/download.ps1                # download both
#   pwsh ./models/download.ps1 -Embedder      # embedder only
#   pwsh ./models/download.ps1 -Reranker      # reranker only
#   pwsh ./models/download.ps1 -Force         # re-download even if present

[CmdletBinding()]
param(
    [switch]$Embedder,
    [switch]$Reranker,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

# Default: both. If either flag is given, only download the requested ones.
if (-not $Embedder -and -not $Reranker) {
    $Embedder = $true
    $Reranker = $true
}

$ModelsDir = Split-Path -Parent $MyInvocation.MyCommand.Path

$Files = @(
    @{
        Name     = 'bge-m3-Q8_0.gguf'
        Url      = 'https://huggingface.co/ggml-org/bge-m3-Q8_0-GGUF/resolve/main/bge-m3-q8_0.gguf?download=true'
        MinBytes = 600MB
        Want     = $Embedder
    },
    @{
        Name     = 'bge-reranker-v2-m3-Q8_0.gguf'
        Url      = 'https://huggingface.co/gpustack/bge-reranker-v2-m3-GGUF/resolve/main/bge-reranker-v2-m3-Q8_0.gguf?download=true'
        MinBytes = 600MB
        Want     = $Reranker
    }
)

function Format-Size([long]$bytes) {
    if ($bytes -ge 1GB) { return "{0:N2} GB" -f ($bytes / 1GB) }
    if ($bytes -ge 1MB) { return "{0:N1} MB" -f ($bytes / 1MB) }
    return "{0:N0} KB" -f ($bytes / 1KB)
}

function Download-File($Spec) {
    $dest = Join-Path $ModelsDir $Spec.Name

    if ((Test-Path $dest) -and -not $Force) {
        $size = (Get-Item $dest).Length
        if ($size -ge $Spec.MinBytes) {
            Write-Host "[skip] $($Spec.Name) already present ($(Format-Size $size))"
            return
        }
        Write-Host "[warn] $($Spec.Name) exists but is too small ($(Format-Size $size)) - re-downloading"
        Remove-Item $dest -Force
    }

    $tmp = "$dest.partial"
    if (Test-Path $tmp) { Remove-Item $tmp -Force }

    Write-Host "[get ] $($Spec.Name)"
    Write-Host "       $($Spec.Url)"

    # Invoke-WebRequest's progress bar tanks throughput on PS 5.1; suppress it.
    $oldPref = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'
    try {
        Invoke-WebRequest -Uri $Spec.Url -OutFile $tmp -UseBasicParsing
    } finally {
        $ProgressPreference = $oldPref
    }

    $size = (Get-Item $tmp).Length
    if ($size -lt $Spec.MinBytes) {
        Remove-Item $tmp -Force
        throw "Downloaded file is suspiciously small ($(Format-Size $size)) - Hugging Face may have returned an HTML error page. URL: $($Spec.Url)"
    }

    Move-Item -Force $tmp $dest
    Write-Host "[ok  ] $($Spec.Name) ($(Format-Size $size))"
}

Write-Host "Locus model downloader - destination: $ModelsDir"
Write-Host ""

foreach ($f in $Files) {
    if ($f.Want) { Download-File $f }
}

Write-Host ""
Write-Host "Done. Models are in $ModelsDir"
