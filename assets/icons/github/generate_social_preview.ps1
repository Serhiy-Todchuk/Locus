param(
    [string]$IconPath = (Join-Path $PSScriptRoot "locus-github-front-1024.png"),
    [string]$OutPath = (Join-Path $PSScriptRoot "locus-social-preview-1280x640.png")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing

function New-Brush([string]$Hex) {
    return New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml($Hex))
}

function New-Pen([string]$Hex, [float]$Width) {
    $pen = New-Object System.Drawing.Pen ([System.Drawing.ColorTranslator]::FromHtml($Hex)), $Width
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    return $pen
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

function Draw-Text($G, [string]$Text, [string]$FontName, [float]$Size, [int]$Style, [string]$Color, [float]$X, [float]$Y) {
    $font = New-Object System.Drawing.Font $FontName, $Size, ([System.Drawing.FontStyle]$Style), ([System.Drawing.GraphicsUnit]::Pixel)
    $brush = New-Brush $Color
    $G.DrawString($Text, $font, $brush, $X, $Y)
    $brush.Dispose()
    $font.Dispose()
}

$width = 1280
$height = 640
$bmp = New-Object System.Drawing.Bitmap $width, $height, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
$g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

$bg = New-Brush "#0F171D"
$g.FillRectangle($bg, 0, 0, $width, $height)
$bg.Dispose()

$panel = New-Brush "#121E26"
$panelPath = New-RoundedRectPath 40 40 1200 560 48
$g.FillPath($panel, $panelPath)
$panel.Dispose()
$panelPath.Dispose()

$accentPen = New-Pen "#20B7D8" 4
$g.DrawLine($accentPen, 560, 366, 920, 366)
$accentPen.Dispose()

$mutedPen = New-Pen "#20313B" 2
foreach ($x in @(78, 106, 1134, 1162)) {
    $g.DrawLine($mutedPen, $x, 92, $x, 548)
}
$mutedPen.Dispose()

$icon = [System.Drawing.Image]::FromFile((Resolve-Path $IconPath).Path)
$iconRect = New-Object System.Drawing.Rectangle 126, 130, 380, 380
$g.DrawImage($icon, $iconRect)
$icon.Dispose()

Draw-Text $g "Locus" "Segoe UI Semibold" 102 ([int][System.Drawing.FontStyle]::Regular) "#EAF4F7" 558 175
Draw-Text $g "Local AI for your workspace" "Segoe UI" 42 ([int][System.Drawing.FontStyle]::Regular) "#C7D6DC" 564 298
Draw-Text $g "Private. Efficient. Observable." "Segoe UI" 30 ([int][System.Drawing.FontStyle]::Regular) "#73DCEB" 564 395

$dotBrush = New-Brush "#62C370"
$g.FillEllipse($dotBrush, 1114, 466, 24, 24)
$dotBrush.Dispose()
Draw-Text $g "local-first" "Segoe UI" 26 ([int][System.Drawing.FontStyle]::Regular) "#91A5AE" 564 465

$bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()

Write-Host "Generated GitHub social preview: $OutPath"
