[CmdletBinding()]
param([string]$OutputDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'assets'))

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
if (-not ('RockmanLauncherIconApi' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class RockmanLauncherIconApi
{
    [DllImport("user32.dll", EntryPoint = "LoadImageW", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr LoadImage(IntPtr instance, string path, uint type, int width, int height, uint flags);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool DestroyIcon(IntPtr icon);
}
'@
}

function New-Brush([string]$Color) {
    return [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml($Color))
}

function Draw-Polygon($Graphics, [string]$Color, [float[]]$Coordinates) {
    $points = [Drawing.PointF[]]::new($Coordinates.Length / 2)
    for ($index = 0; $index -lt $points.Length; $index++) {
        $points[$index] = [Drawing.PointF]::new($Coordinates[$index * 2], $Coordinates[$index * 2 + 1])
    }
    $brush = New-Brush $Color
    try { $Graphics.FillPolygon($brush, $points) }
    finally { $brush.Dispose() }
}

function Draw-RoundedRectangle($Graphics, [string]$Color, [float]$Left, [float]$Top,
    [float]$Width, [float]$Height, [float]$Radius, $Outline = $null) {
    $shape = [Drawing.Drawing2D.GraphicsPath]::new()
    $brush = New-Brush $Color
    $diameter = $Radius * 2
    try {
        $shape.AddArc($Left, $Top, $diameter, $diameter, 180, 90)
        $shape.AddArc($Left + $Width - $diameter, $Top, $diameter, $diameter, 270, 90)
        $shape.AddArc($Left + $Width - $diameter, $Top + $Height - $diameter, $diameter, $diameter, 0, 90)
        $shape.AddArc($Left, $Top + $Height - $diameter, $diameter, $diameter, 90, 90)
        $shape.CloseFigure()
        $Graphics.FillPath($brush, $shape)
        if ($null -ne $Outline) { $Graphics.DrawPath($Outline, $shape) }
    }
    finally { $brush.Dispose(); $shape.Dispose() }
}

function New-IconFrame($Master, [int]$Size) {
    $frame = [Drawing.Bitmap]::new($Size, $Size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [Drawing.Graphics]::FromImage($frame)
    try {
        $graphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
        $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.DrawImage($Master, [Drawing.Rectangle]::new(0, 0, $Size, $Size))
    }
    finally { $graphics.Dispose() }
    return $frame
}

function Get-IconFrameBytes($Frame) {
    $stream = [IO.MemoryStream]::new()
    try {
        if ($Frame.Width -eq 256) {
            $Frame.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
        }
        else {
            $size = $Frame.Width
            $pixels = [byte[]]::new($size * $size * 4)
            $locked = $Frame.LockBits([Drawing.Rectangle]::new(0, 0, $size, $size),
                [Drawing.Imaging.ImageLockMode]::ReadOnly, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
            try {
                for ($row = 0; $row -lt $size; $row++) {
                    [Runtime.InteropServices.Marshal]::Copy(
                        [IntPtr]::Add($locked.Scan0, $row * $locked.Stride),
                        $pixels, ($size - 1 - $row) * $size * 4, $size * 4)
                }
            }
            finally { $Frame.UnlockBits($locked) }
            $maskStride = [int][Math]::Ceiling($size / 32.0) * 4
            $mask = [byte[]]::new($maskStride * $size)
            for ($row = 0; $row -lt $size; $row++) {
                for ($column = 0; $column -lt $size; $column++) {
                    if ($pixels[($row * $size + $column) * 4 + 3] -eq 0) {
                        $offset = $row * $maskStride + [int][Math]::Floor($column / 8.0)
                        $mask[$offset] = $mask[$offset] -bor (128 -shr ($column % 8))
                    }
                }
            }
            $writer = [IO.BinaryWriter]::new($stream, [Text.Encoding]::UTF8, $true)
            try {
                $writer.Write([uint32]40)
                $writer.Write([int32]$size)
                $writer.Write([int32]($size * 2))
                $writer.Write([uint16]1)
                $writer.Write([uint16]32)
                $writer.Write([uint32]0)
                $writer.Write([uint32]$pixels.Length)
                $writer.Write([byte[]]::new(16))
                $writer.Write($pixels)
                $writer.Write($mask)
            }
            finally { $writer.Dispose() }
        }
        return ,$stream.ToArray()
    }
    finally { $stream.Dispose() }
}

[void][IO.Directory]::CreateDirectory($OutputDirectory)
$iconPath = Join-Path $OutputDirectory 'RockmanDash2-Enhanced.ico'
$pngPath = Join-Path $OutputDirectory 'RockmanDash2-Enhanced.png'
$previewDirectory = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\icon'
[void][IO.Directory]::CreateDirectory($previewDirectory)
$previewPath = Join-Path $previewDirectory 'preview.png'
$master = [Drawing.Bitmap]::new(1024, 1024, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [Drawing.Graphics]::FromImage($master)
$outline = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#16354D'), 7)
$outline.LineJoin = [Drawing.Drawing2D.LineJoin]::Round
$shell = [Drawing.Drawing2D.GraphicsPath]::new()
$face = [Drawing.Drawing2D.GraphicsPath]::new()
$blue = [Drawing.Drawing2D.LinearGradientBrush]::new(
    [Drawing.Point]::new(55, 30), [Drawing.Point]::new(205, 200),
    [Drawing.ColorTranslator]::FromHtml('#49C7EF'), [Drawing.ColorTranslator]::FromHtml('#1765BE'))
$skin = New-Brush '#FFE8CB'
try {
    $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.ScaleTransform(4, 4)

    Draw-RoundedRectangle $graphics '#1266AE' 19 95 37 72 11 $outline
    Draw-RoundedRectangle $graphics '#1266AE' 198 95 37 72 11 $outline
    Draw-RoundedRectangle $graphics '#FFD16B' 25 111 13 36 4
    Draw-RoundedRectangle $graphics '#FFD16B' 216 111 13 36 4

    $shell.AddBezier(41, 109, 37, 56, 77, 22, 127, 22)
    $shell.AddBezier(127, 22, 179, 22, 220, 57, 215, 111)
    $shell.AddLine(215, 111, 207, 164)
    $shell.AddBezier(207, 164, 201, 200, 168, 220, 127, 220)
    $shell.AddBezier(127, 220, 85, 220, 54, 198, 48, 165)
    $shell.CloseFigure()
    $graphics.FillPath($blue, $shell)
    $graphics.DrawPath($outline, $shell)
    Draw-Polygon $graphics '#15559A' @(46, 107, 66, 94, 72, 166, 95, 200, 71, 188, 53, 161)
    Draw-Polygon $graphics '#124983' @(210, 108, 191, 95, 183, 169, 160, 205, 187, 190, 203, 162)
    Draw-Polygon $graphics '#8EE7F9' @(57, 81, 73, 58, 95, 44, 107, 42, 103, 53, 84, 63, 70, 82)
    Draw-Polygon $graphics '#16354D' @(111, 27, 143, 27, 149, 63, 127, 83, 105, 63)
    Draw-Polygon $graphics '#FF6370' @(126, 35, 139, 52, 127, 68, 114, 52)
    Draw-Polygon $graphics '#D72C52' @(139, 52, 127, 68, 127, 51)

    $face.AddBezier(65, 104, 81, 93, 95, 94, 111, 104)
    $face.AddLine(111, 104, 127, 114)
    $face.AddLine(127, 114, 144, 104)
    $face.AddBezier(144, 104, 159, 94, 175, 93, 191, 104)
    $face.AddLine(191, 104, 187, 155)
    $face.AddBezier(187, 155, 184, 184, 158, 202, 127, 204)
    $face.AddBezier(127, 204, 97, 202, 71, 184, 68, 155)
    $face.CloseFigure()
    $graphics.FillPath($skin, $face)
    $graphics.DrawPath($outline, $face)
    Draw-RoundedRectangle $graphics '#FFFFFF' 79 119 31 42 10
    Draw-RoundedRectangle $graphics '#FFFFFF' 145 119 31 42 10
    Draw-RoundedRectangle $graphics '#16354D' 94 123 14 34 6
    Draw-RoundedRectangle $graphics '#16354D' 147 123 14 34 6
    Draw-RoundedRectangle $graphics '#56C6CE' 97 143 8 10 3
    Draw-RoundedRectangle $graphics '#56C6CE' 150 143 8 10 3
    Draw-RoundedRectangle $graphics '#FFFFFF' 96 126 5 7 2
    Draw-RoundedRectangle $graphics '#FFFFFF' 149 126 5 7 2
    Draw-Polygon $graphics '#BF826C' @(116, 179, 126, 181, 138, 177, 136, 182, 126, 185, 117, 183)

    Draw-RoundedRectangle $graphics '#FFAE36' 170 167 71 72 12 $outline
    Draw-Polygon $graphics '#FFCF70' @(182, 172, 228, 172, 235, 179, 177, 179)
    Draw-Polygon $graphics '#16354D' @(186, 182, 219, 182, 226, 189, 226, 201,
        201, 219, 226, 219, 226, 230, 184, 230, 184, 216, 213, 196, 213, 193, 186, 193)

    $sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
    $frames = foreach ($size in $sizes) {
        $frame = New-IconFrame $master $size
        try {
            if ($size -eq 256) { $frame.Save($pngPath, [Drawing.Imaging.ImageFormat]::Png) }
            [pscustomobject]@{ Size = $size; Bytes = (Get-IconFrameBytes $frame) }
        }
        finally { $frame.Dispose() }
    }
    $stream = [IO.File]::Create($iconPath)
    $writer = [IO.BinaryWriter]::new($stream)
    try {
        $writer.Write([uint16]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]$frames.Count)
        $offset = 6 + 16 * $frames.Count
        foreach ($frame in $frames) {
            $dimension = if ($frame.Size -eq 256) { 0 } else { $frame.Size }
            $writer.Write([byte]$dimension)
            $writer.Write([byte]$dimension)
            $writer.Write([uint16]0)
            $writer.Write([uint16]1)
            $writer.Write([uint16]32)
            $writer.Write([uint32]$frame.Bytes.Length)
            $writer.Write([uint32]$offset)
            $offset += $frame.Bytes.Length
        }
        foreach ($frame in $frames) { $writer.Write([byte[]]$frame.Bytes) }
    }
    finally { $writer.Dispose() }

    foreach ($size in $sizes) {
        $handle = [RockmanLauncherIconApi]::LoadImage([IntPtr]::Zero, $iconPath, 1, $size, $size, 0x10)
        if ($handle -eq [IntPtr]::Zero) { throw "Windows failed to load the ${size}x${size} icon." }
        $icon = [Drawing.Icon]::FromHandle($handle)
        $decoded = $icon.ToBitmap()
        try {
            if ($decoded.Width -ne $size -or $decoded.Height -ne $size -or
                $decoded.GetPixel(0, 0).A -ne 0 -or
                $decoded.GetPixel([int]($size / 2), [int]($size / 2)).A -eq 0) {
                throw "Windows icon decoding failed for ${size}x${size}."
            }
        }
        finally { $decoded.Dispose(); $icon.Dispose(); [void][RockmanLauncherIconApi]::DestroyIcon($handle) }
    }

    $preview = [Drawing.Bitmap]::new(560, 336)
    $previewGraphics = [Drawing.Graphics]::FromImage($preview)
    $dark = New-Brush '#222931'
    try {
        $previewGraphics.Clear([Drawing.ColorTranslator]::FromHtml('#F0F3F6'))
        $previewGraphics.FillRectangle($dark, 280, 0, 280, 336)
        $previewGraphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        foreach ($left in @(0, 280)) {
            $previewGraphics.DrawImage($master, [Drawing.Rectangle]::new($left + 12, 5, 256, 256))
            $position = $left + 34
            foreach ($size in @(16, 24, 32, 48)) {
                $frame = New-IconFrame $master $size
                try { $previewGraphics.DrawImageUnscaled($frame, $position, 284 + [int]((48 - $size) / 2)) }
                finally { $frame.Dispose() }
                $position += $size + 19
            }
        }
        $preview.Save($previewPath, [Drawing.Imaging.ImageFormat]::Png)
    }
    finally { $dark.Dispose(); $previewGraphics.Dispose(); $preview.Dispose() }
    Write-Host "PASS: Windows decoded all $($sizes.Count) icon sizes ($($sizes -join ', ')); transparency verified."
    Write-Host "Icon: $iconPath"
    Write-Host "PNG: $pngPath"
    Write-Host "Preview: $previewPath"
}
finally {
    $skin.Dispose()
    $blue.Dispose()
    $face.Dispose()
    $shell.Dispose()
    $outline.Dispose()
    $graphics.Dispose()
    $master.Dispose()
}