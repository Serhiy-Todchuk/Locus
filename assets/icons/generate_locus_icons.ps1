param(
    [string]$OutDir = (Join-Path $PSScriptRoot ".")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing

function Join-PathSafe([string]$Root, [string]$Child) {
    $path = Join-Path $Root $Child
    New-Item -ItemType Directory -Force -Path $path | Out-Null
    return $path
}

function New-RoundedRectPath([float]$X, [float]$Y, [float]$W, [float]$H, [float]$R) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $R * 2.0
    $path.AddArc($X, $Y, $d, $d, 180, 90)
    $path.AddArc($X + $W - $d, $Y, $d, $d, 270, 90)
    $path.AddArc($X + $W - $d, $Y + $H - $d, $d, $d, 0, 90)
    $path.AddArc($X, $Y + $H - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-Brush([string]$Hex) {
    return New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml($Hex))
}

function New-Pen([string]$Hex, [float]$Width) {
    $pen = New-Object System.Drawing.Pen ([System.Drawing.ColorTranslator]::FromHtml($Hex)), $Width
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    return $pen
}

function Pt([float]$X, [float]$Y) {
    return New-Object System.Drawing.PointF $X, $Y
}

function Fill-Poly($G, $Brush, [System.Drawing.PointF[]]$Pts) {
    $G.FillPolygon($Brush, $Pts)
}

function Draw-StateDot($G, [int]$Size, [string]$State) {
    if ($State -eq "none") { return }

    $colors = @{
        "idle"     = "#62C370"
        "active"   = "#20B7D8"
        "indexing" = "#F4B740"
        "error"    = "#E65757"
    }
    if (-not $colors.ContainsKey($State)) { return }

    $s = [float]$Size / 1024.0
    $outer = New-Brush "#111820"
    $inner = New-Brush $colors[$State]
    $cx = 787.0 * $s
    $cy = 787.0 * $s
    $ro = 122.0 * $s
    $ri = 82.0 * $s
    $G.FillEllipse($outer, $cx - $ro, $cy - $ro, $ro * 2, $ro * 2)
    $G.FillEllipse($inner, $cx - $ri, $cy - $ri, $ri * 2, $ri * 2)
    $outer.Dispose()
    $inner.Dispose()
}

function New-LocusBitmap([int]$Size, [string]$Purpose = "app", [string]$State = "none") {
    $bmp = New-Object System.Drawing.Bitmap $Size, $Size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.Clear([System.Drawing.Color]::Transparent)

    $s = [float]$Size / 1024.0
    $isSmall = $Size -le 32 -or $Purpose -eq "header" -or $Purpose -eq "tray"

    $bg = New-Brush "#111820"
    $bgPath = New-RoundedRectPath (64 * $s) (64 * $s) (896 * $s) (896 * $s) (190 * $s)
    $g.FillPath($bg, $bgPath)

    $cageColor = "#20B7D8"
    $nodeColor = "#EAF4F7"
    $stackColor = "#EAF4F7"
    $shadowColor = "#73DCEB"

    if ($isSmall) {
        $cagePen = New-Pen $cageColor (70 * $s)
        $hex = @(
            (Pt (512*$s) (178*$s)),
            (Pt (780*$s) (333*$s)),
            (Pt (780*$s) (691*$s)),
            (Pt (512*$s) (846*$s)),
            (Pt (244*$s) (691*$s)),
            (Pt (244*$s) (333*$s)),
            (Pt (512*$s) (178*$s))
        )
        $g.DrawLines($cagePen, [System.Drawing.PointF[]]$hex)
        $cagePen.Dispose()
    } else {
        $segPen = New-Pen $cageColor (50 * $s)
        $nodes = @(
            @(512,170), @(795,333), @(795,691), @(512,854), @(229,691), @(229,333)
        )
        $segments = @(
            @(590,215,725,293), @(795,420,795,604), @(725,731,590,809),
            @(434,809,299,731), @(229,604,229,420), @(299,293,434,215)
        )
        foreach ($seg in $segments) {
            $g.DrawLine($segPen, [float]$seg[0]*$s, [float]$seg[1]*$s, [float]$seg[2]*$s, [float]$seg[3]*$s)
        }
        $segPen.Dispose()

        $nodeBrush = New-Brush $nodeColor
        foreach ($n in $nodes) {
            $r = 58 * $s
            $g.FillEllipse($nodeBrush, ([float]$n[0]*$s)-$r, ([float]$n[1]*$s)-$r, $r*2, $r*2)
        }
        $nodeBrush.Dispose()
    }

    $stack = New-Brush $stackColor
    $shade = New-Brush $shadowColor

    if ($isSmall) {
        Fill-Poly $g $stack ([System.Drawing.PointF[]]@(
            (Pt (512*$s) (300*$s)), (Pt (710*$s) (407*$s)),
            (Pt (512*$s) (514*$s)), (Pt (314*$s) (407*$s))
        ))
        Fill-Poly $g $shade ([System.Drawing.PointF[]]@(
            (Pt (314*$s) (548*$s)), (Pt (512*$s) (655*$s)),
            (Pt (710*$s) (548*$s)), (Pt (710*$s) (620*$s)),
            (Pt (512*$s) (727*$s)), (Pt (314*$s) (620*$s))
        ))
    } else {
        Fill-Poly $g $stack ([System.Drawing.PointF[]]@(
            (Pt (512*$s) (318*$s)), (Pt (704*$s) (420*$s)),
            (Pt (512*$s) (522*$s)), (Pt (320*$s) (420*$s))
        ))
        Fill-Poly $g $shade ([System.Drawing.PointF[]]@(
            (Pt (320*$s) (535*$s)), (Pt (512*$s) (637*$s)),
            (Pt (704*$s) (535*$s)), (Pt (704*$s) (596*$s)),
            (Pt (512*$s) (698*$s)), (Pt (320*$s) (596*$s))
        ))
        Fill-Poly $g $stack ([System.Drawing.PointF[]]@(
            (Pt (320*$s) (620*$s)), (Pt (512*$s) (722*$s)),
            (Pt (704*$s) (620*$s)), (Pt (704*$s) (681*$s)),
            (Pt (512*$s) (783*$s)), (Pt (320*$s) (681*$s))
        ))
    }

    $stack.Dispose()
    $shade.Dispose()

    Draw-StateDot $g $Size $State

    $bgPath.Dispose()
    $bg.Dispose()
    $g.Dispose()
    return $bmp
}

function Save-Png([System.Drawing.Bitmap]$Bmp, [string]$Path) {
    $Bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
}

function Get-PngBytes([System.Drawing.Bitmap]$Bmp) {
    $ms = New-Object System.IO.MemoryStream
    $Bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $ms.ToArray()
    $ms.Dispose()
    return $bytes
}

function Write-U16LE($Writer, [int]$Value) {
    $Writer.Write([byte]($Value -band 0xff))
    $Writer.Write([byte](($Value -shr 8) -band 0xff))
}

function Write-U32LE($Writer, [int64]$Value) {
    $Writer.Write([byte]($Value -band 0xff))
    $Writer.Write([byte](($Value -shr 8) -band 0xff))
    $Writer.Write([byte](($Value -shr 16) -band 0xff))
    $Writer.Write([byte](($Value -shr 24) -band 0xff))
}

function Write-U32BE($Writer, [int64]$Value) {
    $Writer.Write([byte](($Value -shr 24) -band 0xff))
    $Writer.Write([byte](($Value -shr 16) -band 0xff))
    $Writer.Write([byte](($Value -shr 8) -band 0xff))
    $Writer.Write([byte]($Value -band 0xff))
}

function Save-Ico([string]$Path, [int[]]$Sizes, [string]$Purpose = "app", [string]$State = "none") {
    $entries = @()
    foreach ($size in $Sizes) {
        $bmp = New-LocusBitmap $size $Purpose $State
        $entries += [pscustomobject]@{ Size = $size; Bytes = (Get-PngBytes $bmp) }
        $bmp.Dispose()
    }

    $fs = [System.IO.File]::Create($Path)
    $bw = New-Object System.IO.BinaryWriter $fs
    Write-U16LE $bw 0
    Write-U16LE $bw 1
    Write-U16LE $bw $entries.Count

    $offset = 6 + (16 * $entries.Count)
    foreach ($entry in $entries) {
        $dim = if ($entry.Size -ge 256) { 0 } else { $entry.Size }
        $bw.Write([byte]$dim)
        $bw.Write([byte]$dim)
        $bw.Write([byte]0)
        $bw.Write([byte]0)
        Write-U16LE $bw 1
        Write-U16LE $bw 32
        Write-U32LE $bw $entry.Bytes.Length
        Write-U32LE $bw $offset
        $offset += $entry.Bytes.Length
    }
    foreach ($entry in $entries) {
        $bw.Write([byte[]]$entry.Bytes)
    }
    $bw.Dispose()
    $fs.Dispose()
}

function Save-Icns([string]$Path) {
    $chunks = @()
    $defs = @(
        @("icp4", 16), @("icp5", 32), @("icp6", 64),
        @("ic07", 128), @("ic08", 256), @("ic09", 512), @("ic10", 1024),
        @("ic11", 32), @("ic12", 64), @("ic13", 256), @("ic14", 512)
    )
    foreach ($def in $defs) {
        $bmp = New-LocusBitmap ([int]$def[1]) "app" "none"
        $chunks += [pscustomobject]@{ Type = [string]$def[0]; Bytes = (Get-PngBytes $bmp) }
        $bmp.Dispose()
    }

    $total = 8
    foreach ($chunk in $chunks) { $total += 8 + $chunk.Bytes.Length }

    $fs = [System.IO.File]::Create($Path)
    $bw = New-Object System.IO.BinaryWriter $fs
    $bw.Write([System.Text.Encoding]::ASCII.GetBytes("icns"))
    Write-U32BE $bw $total
    foreach ($chunk in $chunks) {
        $bw.Write([System.Text.Encoding]::ASCII.GetBytes($chunk.Type))
        Write-U32BE $bw (8 + $chunk.Bytes.Length)
        $bw.Write([byte[]]$chunk.Bytes)
    }
    $bw.Dispose()
    $fs.Dispose()
}

$OutDir = [System.IO.Path]::GetFullPath($OutDir)
$sourceDir = Join-PathSafe $OutDir "source"
$githubDir = Join-PathSafe $OutDir "github"
$windowsDir = Join-PathSafe $OutDir "windows"
$macosDir = Join-PathSafe $OutDir "macos"
$iconsetDir = Join-PathSafe $macosDir "Locus.iconset"
$trayDir = Join-PathSafe $OutDir "tray"
$aboutDir = Join-PathSafe $OutDir "about"
$headerDir = Join-PathSafe $OutDir "header"
$pngDir = Join-PathSafe $OutDir "png"

$svg = @'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" role="img" aria-label="Locus app icon">
  <rect x="64" y="64" width="896" height="896" rx="190" fill="#111820"/>
  <g fill="none" stroke="#20B7D8" stroke-width="50" stroke-linecap="round" stroke-linejoin="round">
    <path d="M590 215 L725 293"/>
    <path d="M795 420 L795 604"/>
    <path d="M725 731 L590 809"/>
    <path d="M434 809 L299 731"/>
    <path d="M229 604 L229 420"/>
    <path d="M299 293 L434 215"/>
  </g>
  <g fill="#EAF4F7">
    <circle cx="512" cy="170" r="58"/>
    <circle cx="795" cy="333" r="58"/>
    <circle cx="795" cy="691" r="58"/>
    <circle cx="512" cy="854" r="58"/>
    <circle cx="229" cy="691" r="58"/>
    <circle cx="229" cy="333" r="58"/>
    <polygon points="512,318 704,420 512,522 320,420"/>
    <polygon points="320,620 512,722 704,620 704,681 512,783 320,681"/>
  </g>
  <polygon points="320,535 512,637 704,535 704,596 512,698 320,596" fill="#73DCEB"/>
</svg>
'@
Set-Content -Path (Join-Path $sourceDir "locus-icon.svg") -Value $svg -NoNewline -Encoding UTF8

foreach ($size in @(16, 20, 24, 32, 40, 48, 64, 128, 256, 512, 1024)) {
    $bmp = New-LocusBitmap $size "app" "none"
    Save-Png $bmp (Join-Path $pngDir ("locus-icon-{0}.png" -f $size))
    $bmp.Dispose()
}

$github = New-LocusBitmap 1024 "app" "none"
Save-Png $github (Join-Path $githubDir "locus-github-front-1024.png")
$github.Dispose()

foreach ($size in @(256, 512)) {
    $bmp = New-LocusBitmap $size "app" "none"
    Save-Png $bmp (Join-Path $aboutDir ("locus-about-{0}.png" -f $size))
    $bmp.Dispose()
}

foreach ($size in @(16, 20, 24, 32)) {
    $bmp = New-LocusBitmap $size "header" "none"
    Save-Png $bmp (Join-Path $headerDir ("locus-header-{0}.png" -f $size))
    $bmp.Dispose()
}

foreach ($state in @("idle", "active", "indexing", "error")) {
    foreach ($size in @(16, 20, 24, 32)) {
        $bmp = New-LocusBitmap $size "tray" $state
        Save-Png $bmp (Join-Path $trayDir ("locus-tray-{0}-{1}.png" -f $state, $size))
        $bmp.Dispose()
    }
    Save-Ico (Join-Path $trayDir ("locus-tray-{0}.ico" -f $state)) @(16, 20, 24, 32) "tray" $state
}

Save-Ico (Join-Path $windowsDir "locus.ico") @(16, 20, 24, 32, 40, 48, 64, 128, 256) "app" "none"
Save-Icns (Join-Path $macosDir "Locus.icns")

$iconsetMap = @(
    @("icon_16x16.png", 16), @("icon_16x16@2x.png", 32),
    @("icon_32x32.png", 32), @("icon_32x32@2x.png", 64),
    @("icon_128x128.png", 128), @("icon_128x128@2x.png", 256),
    @("icon_256x256.png", 256), @("icon_256x256@2x.png", 512),
    @("icon_512x512.png", 512), @("icon_512x512@2x.png", 1024)
)
foreach ($entry in $iconsetMap) {
    $bmp = New-LocusBitmap ([int]$entry[1]) "app" "none"
    Save-Png $bmp (Join-Path $iconsetDir ([string]$entry[0]))
    $bmp.Dispose()
}

$manifest = @{
    name = "Locus icon asset set"
    source = "source/locus-icon.svg"
    design = "Data stack contained inside a hexagonal local-control boundary."
    windows = @{
        app = "windows/locus.ico"
        tray = @("tray/locus-tray-idle.ico", "tray/locus-tray-active.ico", "tray/locus-tray-indexing.ico", "tray/locus-tray-error.ico")
    }
    macos = @{
        app = "macos/Locus.icns"
        iconset = "macos/Locus.iconset"
    }
    usage = @{
        github_front_page = "github/locus-github-front-1024.png"
        explorer_finder_taskbar = @("windows/locus.ico", "macos/Locus.icns")
        tray = "tray/"
        about_window = "about/"
        window_header = "header/"
    }
} | ConvertTo-Json -Depth 5
Set-Content -Path (Join-Path $OutDir "manifest.json") -Value $manifest -Encoding UTF8

Write-Host "Generated Locus icons in $OutDir"
