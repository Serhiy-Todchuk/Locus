# Locus - download SMALL English-only embedding + reranker models.
#
#   bge-small-en-v1.5         Q8_0     ~37 MB    384-dim, 512 ctx, English
#   bge-reranker-v2-m3        Q4_K_M   ~438 MB   cross-encoder reranker (still
#                                                multilingual - there is no
#                                                widely-distributed smaller
#                                                GGUF reranker as of 2026)
#
# Total: ~475 MB. Use this profile when disk/RAM is tight or your workspace
# is English-only. Recall on non-English text and long chunks will be lower
# than the recommended bge-m3 set (see download.ps1).
#
# Usage:
#   pwsh ./models/download-small.ps1                # download both
#   pwsh ./models/download-small.ps1 -Embedder      # embedder only
#   pwsh ./models/download-small.ps1 -NoReranker    # skip reranker
#   pwsh ./models/download-small.ps1 -Force         # re-download

[CmdletBinding()]
param(
    [switch]$Embedder,
    [switch]$NoReranker,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

if (-not $Embedder) { $Embedder = $true }
$Reranker = -not $NoReranker

$ModelsDir = Split-Path -Parent $MyInvocation.MyCommand.Path

$Files = @(
    @{
        Name     = 'bge-small-en-v1.5-Q8_0.gguf'
        Url      = 'https://huggingface.co/ggml-org/bge-small-en-v1.5-Q8_0-GGUF/resolve/main/bge-small-en-v1.5-q8_0.gguf?download=true'
        MinBytes = 30MB
        Want     = $Embedder
    },
    @{
        Name     = 'bge-reranker-v2-m3-Q4_K_M.gguf'
        Url      = 'https://huggingface.co/gpustack/bge-reranker-v2-m3-GGUF/resolve/main/bge-reranker-v2-m3-Q4_K_M.gguf?download=true'
        MinBytes = 400MB
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

Write-Host "Locus model downloader (small English profile) - destination: $ModelsDir"
Write-Host ""

foreach ($f in $Files) {
    if ($f.Want) { Download-File $f }
}

Write-Host ""
Write-Host "Done. Models are in $ModelsDir"
