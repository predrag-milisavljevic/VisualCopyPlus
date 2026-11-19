# Regenerates res/app.ico (multi-size, PNG-compressed entries) using System.Drawing.
# Run with Windows PowerShell:  powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1
Add-Type -AssemblyName System.Drawing

$out   = Join-Path $PSScriptRoot '..\res\app.ico'
$sizes = 16, 20, 24, 32, 40, 48, 64, 256

function New-RoundRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function New-IconPng([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    $k = $s / 32.0
    $g.ScaleTransform($k, $k)

    # Background tile
    $bg = New-RoundRect 1.5 1.5 29 29 7
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, 0), (New-Object System.Drawing.PointF 32, 32), ([System.Drawing.Color]::FromArgb(255, 64, 150, 255)), ([System.Drawing.Color]::FromArgb(255, 22, 92, 214))
    $g.FillPath($grad, $bg)

    # Back sheet
    $back = New-RoundRect 7.5 6 12.5 15.5 2.2
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(140, 255, 255, 255))), $back)

    # Front sheet
    $front = New-RoundRect 12 10.5 12.5 15.5 2.2
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)), $front)

    # Plus mark on the front sheet
    $plus = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 22, 92, 214))
    $g.FillRectangle($plus, 17.1, 14.6, 2.3, 7.3)
    $g.FillRectangle($plus, 14.6, 17.1, 7.3, 2.3)

    # Spark / flash ticks (top-right)
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(230, 255, 255, 255)), 1.8
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
    if ($s -ge 24) {
        $g.DrawLine($pen, 24.5, 7.5, 26.8, 5.2)
        $g.DrawLine($pen, 25.8, 10.2, 28.2, 9.6)
        $g.DrawLine($pen, 21.8, 6.2, 22.4, 3.8)
    }
    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return ,$ms.ToArray()
}

$images = @()
foreach ($s in $sizes) { $images += ,(New-IconPng $s) }

$fs = [System.IO.File]::Create($out)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $images[$i].Length
    $dim = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$len); $bw.Write([UInt32]$offset)
    $offset += $len
}
foreach ($img in $images) { $bw.Write($img) }
$bw.Close()
Write-Host "Wrote $out"
