# Builds Packaging\AppIcon.ico from the shared 1024px artwork.
#
# Written out by hand rather than through System.Drawing's Icon.Save, which only
# emits a single low-resolution frame. Every size from Vista onwards may be stored
# as a PNG inside the icon, so each frame is simply the resized bitmap encoded as
# PNG and pointed at from the directory.

param(
    [string]$Source = (Join-Path $PSScriptRoot "..\..\assets\images\icon_1024.png"),
    [string]$Output = (Join-Path $PSScriptRoot "AppIcon.ico")
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

# 256 is the largest an icon directory can describe; it is stored as a zero.
$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)

$artwork = [System.Drawing.Image]::FromFile((Resolve-Path $Source))
$frames = @()
foreach ($size in $sizes) {
    $bitmap = New-Object System.Drawing.Bitmap($size, $size)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.InterpolationMode = "HighQualityBicubic"
    $graphics.PixelOffsetMode = "HighQuality"
    $graphics.SmoothingMode = "HighQuality"
    $graphics.DrawImage($artwork, 0, 0, $size, $size)
    $graphics.Dispose()
    $memory = New-Object System.IO.MemoryStream
    $bitmap.Save($memory, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    $frames += ,@($size, $memory.ToArray())
    $memory.Dispose()
}
$artwork.Dispose()

$stream = [System.IO.File]::Create($Output)
$writer = New-Object System.IO.BinaryWriter($stream)
try {
    $writer.Write([uint16]0)                  # reserved
    $writer.Write([uint16]1)                  # type: icon
    $writer.Write([uint16]$frames.Count)

    $offset = 6 + 16 * $frames.Count
    foreach ($frame in $frames) {
        $size = $frame[0]
        $bytes = $frame[1]
        $writer.Write([byte]($(if ($size -ge 256) { 0 } else { $size })))   # width
        $writer.Write([byte]($(if ($size -ge 256) { 0 } else { $size })))   # height
        $writer.Write([byte]0)                # palette entries
        $writer.Write([byte]0)                # reserved
        $writer.Write([uint16]1)              # colour planes
        $writer.Write([uint16]32)             # bits per pixel
        $writer.Write([uint32]$bytes.Length)
        $writer.Write([uint32]$offset)
        $offset += $bytes.Length
    }
    foreach ($frame in $frames) { $writer.Write($frame[1]) }
} finally {
    $writer.Dispose()
    $stream.Dispose()
}

"wrote $Output ($([int]((Get-Item $Output).Length / 1024)) kB, $($frames.Count) sizes)"
