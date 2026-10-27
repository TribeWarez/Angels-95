# make_weapon_icon.ps1 - turn a white-on-transparent weapon silhouette into the
# dark icon the HUD and the world pickup billboard actually need.
#
# Every RetroWeaponsPack gun ships <Name>_silhouette.png as a PURE WHITE (255)
# alpha mask. Used as `icon =` it renders as a white billboard in every map and a
# white blob in the slotbar - the exact artefact World-Design-Pass item 4 fixed
# for Pistol_01/Rifle_01 (their icons were re-drawn by hand as 3-tone silhouettes
# in the 41..155 value range, mean ~75-80, 128x128).
#
# This is the mechanical equivalent for a NEW gun: keep the authored alpha,
# letterbox it into a square canvas at the shipped icon size, and tint it to the
# shipped value/colour. It is deliberately flat - the hand-drawn 3-tone icons are
# a shading pass, not something to fake from an alpha mask - so treat a new
# weapon's icon as "dark and readable" rather than final art.
#
# Usage (from the repo root):
#   powershell -ExecutionPolicy Bypass -File tools/make_weapon_icon.ps1 `
#       -Source "GameData/Tex/Global/Gun/SMG_01/Textures/SMG_01_silhouette.png"
#
# Without -Out the source file is overwritten (the icon is derived from it).
# Re-runnable: tinting an already-tinted icon is idempotent only through -Force;
# the script refuses a source whose opaque pixels are already dark, so a second
# run cannot multiply the value down to black.
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [string]$Out = "",
    [int]$Size = 128,
    [double]$TintR = 0.30,   # 0.30/0.31/0.333 * 255 = (76, 79, 85), the mean of
    [double]$TintG = 0.31,   # the shipped Pistol_01/Rifle_01 icons
    [double]$TintB = 0.333,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

if (-not $Out) { $Out = $Source }
if (-not (Test-Path $Source)) { throw "source not found: $Source" }

$src = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Source).Path)
try {
    # Refuse a source that is already tinted: re-running would crush it to black.
    $sum = 0; $n = 0
    for ($y = 0; $y -lt $src.Height; $y += 2) {
        for ($x = 0; $x -lt $src.Width; $x += 2) {
            $c = $src.GetPixel($x, $y)
            if ($c.A -gt 200) { $sum += [int](($c.R + $c.G + $c.B) / 3); $n++ }
        }
    }
    if ($n -eq 0) { throw "$Source has no opaque pixels - it is not a silhouette" }
    $mean = [int]($sum / $n)
    if ($mean -lt 200 -and -not $Force) {
        throw ("$Source already looks tinted (mean={0}); pass -Force to reformat it anyway" -f $mean)
    }

    # Letterbox: fit the LONGEST axis to $Size, centre the other one. The pack art
    # is 2:1 (512x256), so this lands the gun in the middle band of a square icon
    # the way the hand-drawn icons sit.
    $scale = $Size / [double]([Math]::Max($src.Width, $src.Height))
    $w = [int][Math]::Round($src.Width * $scale)
    $h = [int][Math]::Round($src.Height * $scale)
    $ox = [int](($Size - $w) / 2)
    $oy = [int](($Size - $h) / 2)

    $dst = New-Object System.Drawing.Bitmap($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($dst)
    try {
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceOver

        # Tint RGB, leave A untouched (row 4 of the matrix is the identity alpha).
        $m = New-Object System.Drawing.Imaging.ColorMatrix
        $m.Matrix00 = [float]$TintR
        $m.Matrix11 = [float]$TintG
        $m.Matrix22 = [float]$TintB
        $m.Matrix33 = 1.0
        $m.Matrix44 = 1.0
        $ia = New-Object System.Drawing.Imaging.ImageAttributes
        $ia.SetColorMatrix($m)

        $rect = New-Object System.Drawing.Rectangle($ox, $oy, $w, $h)
        $g.DrawImage($src, $rect, 0, 0, $src.Width, $src.Height,
                     [System.Drawing.GraphicsUnit]::Pixel, $ia)
    } finally {
        $g.Dispose()
    }
} finally {
    $src.Dispose()
}

try {
    $dst.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Host ("{0}: {1}x{2} icon, tint=({3},{4},{5}) -> {6}" -f `
        (Split-Path $Source -Leaf), $Size, $Size,
        [int][Math]::Round($TintR * 255), [int][Math]::Round($TintG * 255),
        [int][Math]::Round($TintB * 255), $Out)
} finally {
    $dst.Dispose()
}
