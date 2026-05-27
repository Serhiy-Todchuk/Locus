# Locus -- build WS2 (Simple English Wikipedia, Kiwix-extracted) test workspace.
#
# Pre-S6.2 path: until native libzim support lands inside Locus itself,
# S5.N proves the non-code story on a pre-extracted ZIM. This script
# downloads a small Simple English Wikipedia ZIM and unpacks it into a
# flat folder of HTML articles that Locus's existing HtmlExtractor can
# index unchanged.
#
# Output:
#   D:\Projects\LocusTestWorkspaces\WS2_Wikipedia\
#     A\                    (article HTML files, basename = sanitised title)
#     LOCUS.md              (read-only / no-code policy hint)
#
# Extraction is delegated to build_ws2_extract.py (uses python-libzim,
# which has prebuilt wheels for Windows / Linux / macOS). The official
# zimdump tool from openzim/zim-tools only ships for Linux/macOS, so the
# python-libzim path is what makes this script cross-platform.
#
# Prerequisites: Python 3 + pip on PATH. The script will pip-install
# libzim into the active Python environment if it's missing.
#
# Usage:
#   pwsh ./scripts/build_ws2.ps1              # default workspace path + 5000-article cap
#   pwsh ./scripts/build_ws2.ps1 -Force       # re-download + re-extract
#   pwsh ./scripts/build_ws2.ps1 -ZimUrl <u>  # point at a different Kiwix snapshot
#   pwsh ./scripts/build_ws2.ps1 -MaxArticles 0   # keep all articles (slow!)
#
# Browse newer snapshots at https://download.kiwix.org/zim/wikipedia/ --
# look for files matching wikipedia_en_simple_all_nopic_YYYY-MM.zim.
#
# Default -MaxArticles is 5000 -- the full Simple English nopic ZIM has
# ~250K articles, which makes a great long-term corpus but a slow demo
# workspace. The first 5000 cover enough topical ground for the S5.N
# acceptance prompts and keep first-index + embedding wall time on the
# order of minutes, not hours.

[CmdletBinding()]
param(
    [string]$WorkspaceRoot = 'D:\Projects\LocusTestWorkspaces\WS2_Wikipedia',
    [string]$DownloadCache = 'D:\Projects\LocusTestWorkspaces\.cache',
    [string]$ZimUrl        = 'https://download.kiwix.org/zim/wikipedia/wikipedia_en_simple_all_nopic_2026-05.zim',
    [int]   $MaxArticles   = 5000,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

function Format-Size([long]$bytes) {
    if ($bytes -ge 1GB) { return "{0:N2} GB" -f ($bytes / 1GB) }
    if ($bytes -ge 1MB) { return "{0:N1} MB" -f ($bytes / 1MB) }
    return "{0:N0} KB" -f ($bytes / 1KB)
}

function Download-If-Missing($Url, $Dest, $MinBytes) {
    if ((Test-Path $Dest) -and -not $Force) {
        $size = (Get-Item $Dest).Length
        if ($size -ge $MinBytes) {
            Write-Host "[skip] $(Split-Path -Leaf $Dest) already present ($(Format-Size $size))"
            return
        }
        Write-Host "[warn] $(Split-Path -Leaf $Dest) exists but is too small ($(Format-Size $size)) - re-downloading"
        Remove-Item $Dest -Force
    }
    $tmp = "$Dest.partial"
    if (Test-Path $tmp) { Remove-Item $tmp -Force }

    Write-Host "[get ] $(Split-Path -Leaf $Dest)"
    Write-Host "       $Url"

    $oldPref = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'
    try {
        Invoke-WebRequest -Uri $Url -OutFile $tmp -UseBasicParsing
    } finally {
        $ProgressPreference = $oldPref
    }

    $size = (Get-Item $tmp).Length
    if ($size -lt $MinBytes) {
        Remove-Item $tmp -Force
        throw "Downloaded file is suspiciously small ($(Format-Size $size)) - server may have returned an error page. URL: $Url"
    }
    Move-Item -Force $tmp $Dest
    Write-Host "[ok  ] $(Split-Path -Leaf $Dest) ($(Format-Size $size))"
}

# 1. Set up directories.
New-Item -ItemType Directory -Force -Path $WorkspaceRoot | Out-Null
New-Item -ItemType Directory -Force -Path $DownloadCache | Out-Null

$ZimName = Split-Path -Leaf $ZimUrl
$ZimPath = Join-Path $DownloadCache $ZimName

Write-Host "Locus WS2 builder"
Write-Host "  Workspace : $WorkspaceRoot"
Write-Host "  Cache     : $DownloadCache"
Write-Host ""

# 2. Download the ZIM (~940 MB for Simple English nopic 2026-05).
Download-If-Missing $ZimUrl $ZimPath (400MB)

# 3. Ensure python + libzim are available. Install libzim if missing.
$python = (Get-Command python -ErrorAction SilentlyContinue)
if (-not $python) {
    throw "python is not on PATH. Install Python 3.10+ and re-run."
}
# Don't redirect python stderr with 2>&1 in PS 5.1 -- it wraps stderr lines
# as ErrorRecords and reports NativeCommandError even when the exit code is
# zero. Run the check cleanly and inspect $LASTEXITCODE only.
& python -c "from libzim.reader import Archive" *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "[pip ] python-libzim not installed - installing now ..."
    & python -m pip install --quiet libzim
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install python-libzim. Run 'python -m pip install libzim' manually and retry."
    }
    & python -c "from libzim.reader import Archive" *> $null
    if ($LASTEXITCODE -ne 0) {
        throw "python-libzim installed but still cannot be imported. Check your Python environment."
    }
}
Write-Host "[ok  ] python-libzim ready"

# 4. Extract articles into A/ via the Python helper.
$ArticlesDst = Join-Path $WorkspaceRoot 'A'
$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ExtractPy   = Join-Path $ScriptDir 'build_ws2_extract.py'

if ((Test-Path $ArticlesDst) -and -not $Force) {
    $existing = @(Get-ChildItem -Path $ArticlesDst -File -ErrorAction SilentlyContinue).Count
    Write-Host "[skip ] $ArticlesDst already populated ($existing files) - pass -Force to re-extract"
} else {
    if (Test-Path $ArticlesDst) { Remove-Item -Recurse -Force $ArticlesDst }
    New-Item -ItemType Directory -Force -Path $ArticlesDst | Out-Null

    # Curated must-include articles: anchors for the S5.N retrieval-eval
    # gold queries (queries_ws2.json). libzim's iterate-by-id order isn't
    # alphabetical -- early ids tend to be short / digit-prefixed titles --
    # so without curation the first 5000 wouldn't include any of these.
    $Curated = @(
        'Australia', 'Canberra',
        'Albert_Einstein',
        'Ancient_Egypt',
        'Byzantine_Empire',
        'Ludwig_van_Beethoven',
        'Charles_Darwin',
        'DNA',
        'Earth',
        'French_Revolution',
        'Galileo_Galilei',
        'Heart',
        'Industrial_Revolution',
        'Japan',
        'Krakatoa',
        # Networking cluster -- anchors Test #2's "Compare TCP and UDP"
        # cross-document synthesis prompt. libzim iteration order skews
        # toward digit-prefix and short-title articles, so the alphabetic
        # T/U cluster would otherwise miss the 5000-article cap entirely.
        'Transmission_Control_Protocol',
        'User_Datagram_Protocol',
        'Internet_Protocol',
        'IP_address',
        'OSI_model',
        'Hypertext_Transfer_Protocol'   # HTTP + HTTPS both redirect here
    )
    $extractArgs = @($ZimPath, $ArticlesDst, '--max', $MaxArticles)
    foreach ($t in $Curated) { $extractArgs += @('--include', $t) }

    Write-Host "[zim  ] extracting articles (this can take a minute) ..."
    & python $ExtractPy @extractArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Article extraction failed with exit code $LASTEXITCODE"
    }
}

# 5. Write a LOCUS.md describing the workspace policy. The agent loads this
#    into every system prompt, so the read-only / no-code framing here is
#    what actually steers the model.
$LocusMd = @'
This workspace is an offline subset of Simple English Wikipedia, extracted
from a Kiwix .zim snapshot. All content is read-only -- do not attempt to
create, edit, or delete any file in this workspace.

Articles live under `A/` as HTML files. The basename of each file is the
article title with characters Windows disallows replaced by underscores
(e.g. `A/Byzantine_fault.html`).

This workspace has no code. Do not suggest code-related tools; in particular
the `code-aware search` capability bucket should stay off (Settings ->
Capabilities). Prefer `search_semantic` for topic questions and `search_text`
when the user names an article exactly.

If a question cannot be answered from the indexed articles, say so plainly
rather than fabricating coverage -- this is a small Simple English subset,
not the full English Wikipedia.
'@
$LocusMdPath = Join-Path $WorkspaceRoot 'LOCUS.md'
if (-not (Test-Path $LocusMdPath) -or $Force) {
    Set-Content -Path $LocusMdPath -Value $LocusMd -Encoding utf8
    Write-Host "[ok  ] LOCUS.md written"
} else {
    Write-Host "[skip ] LOCUS.md already present"
}

# 6. Summary.
$articleCount = 0
if (Test-Path $ArticlesDst) {
    $articleCount = @(Get-ChildItem -Path $ArticlesDst -File -ErrorAction SilentlyContinue).Count
}
Write-Host ""
Write-Host "Done. WS2 ready at: $WorkspaceRoot"
Write-Host "  Articles  : $articleCount HTML files under A/"
Write-Host "  ZIM cache : $(Format-Size (Get-Item $ZimPath).Length)"
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Launch locus_gui.exe `"$WorkspaceRoot`""
Write-Host "  2. Accept the Workspace Capabilities defaults (code_aware_search off, semantic_search on)"
Write-Host "  3. Wait for first index + embedding queue to drain (see Activity panel)"
Write-Host "  4. Try a prompt: \"What is the capital of Australia?\""
