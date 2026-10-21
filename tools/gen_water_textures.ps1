# tools/gen_water_textures.ps1
#
# Generates the two shared water textures used by the outdoor maps:
#
#   GameData/Tex/Global/EFX/water_pool.png          64x64  deep-water ripple field
#   GameData/Tex/Global/EFX/waterfall_streaks.png   64x128 vertical streaks (alpha)
#
# Why these exist:
#   `liquid_water.png` (the only shipped water texture) is a high-contrast blue
#   tile pattern. Used on a large pool it reads as bathroom tiling, and used on a
#   cascade sheet it reads as a flat blue panel. Neither is a texture problem the
#   renderer can fix: the surface shader samples baseColor and applies uAlpha, so
#   the look has to come from the image.
#
#   water_pool.png is deliberately LOW contrast and dark. Pool slabs are drawn
#   with flags = UNLIT | PAN_U | TRANSLUCENT and surfAlpha ~0.8, so the stone bed
#   shows through - a dark low-contrast image reads as depth, a bright tiled one
#   reads as a swimming pool.
#
#   waterfall_streaks.png carries its own ALPHA. The cascade sheet is
#   TRANSLUCENT | TWO_SIDED | PAN_V, and the shader multiplies the texture's alpha
#   by uAlpha, so transparent gaps between streaks are what make a sheet look like
#   falling water instead of a painted panel. Draw it as vertical lines that run
#   the full height so the PAN_V scroll wraps without a visible seam.
#
# Deterministic: fixed RNG seeds, so re-running produces byte-identical files.
# Overwrites the two outputs; touches nothing else.

param(
    [string]$OutDir = "GameData/Tex/Global/EFX"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

# ---------------------------------------------------------------------------
# water_pool.png - 64x64, single sine field, dark blue-grey
# ---------------------------------------------------------------------------
$pw = 64; $ph = 64
$pool = New-Object System.Drawing.Bitmap($pw, $ph, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)

for ($y = 0; $y -lt $ph; $y++) {
    for ($x = 0; $x -lt $pw; $x++) {
        $v = [math]::Sin($x * 0.19 + $y * 0.11) * 9.0 `
           + [math]::Sin($y * 0.63 - $x * 0.07) * 6.0 `
           + [math]::Sin(($x + $y) * 0.35) * 3.5

        $r = [int](18 + $v * 0.55)
        $g = [int](40 + $v * 0.85)
        $b = [int](58 + $v * 1.15)

        if ($r -lt 0) { $r = 0 }; if ($r -gt 255) { $r = 255 }
        if ($g -lt 0) { $g = 0 }; if ($g -gt 255) { $g = 255 }
        if ($b -lt 0) { $b = 0 }; if ($b -gt 255) { $b = 255 }

        $pool.SetPixel($x, $y, [System.Drawing.Color]::FromArgb($r, $g, $b))
    }
}

$poolPath = Join-Path $OutDir "water_pool.png"
$pool.Save($poolPath, [System.Drawing.Imaging.ImageFormat]::Png)
$pool.Dispose()

# ---------------------------------------------------------------------------
# waterfall_streaks.png - 64x128, transparent ground + vertical streaks
# ---------------------------------------------------------------------------
$fw = 64; $fh = 128
$fall = New-Object System.Drawing.Bitmap($fw, $fh, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$gfx = [System.Drawing.Graphics]::FromImage($fall)
$gfx.Clear([System.Drawing.Color]::FromArgb(0, 255, 255, 255))
$gfx.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$gfx.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

$rng = New-Object System.Random(333)

# Long streaks: must span the full height so the vertical scroll wraps cleanly.
# Kept sparse (90 thin lines over 64 px) because coverage is what decides whether
# the sheet reads as water or as a painted panel - see the gap pass below.
for ($i = 0; $i -lt 90; $i++) {
    $x = $rng.Next(0, $fw)
    $thick = $rng.Next(1, 3)
    $alpha = $rng.Next(70, 200)
    $tint = $rng.Next(0, 35)

    $col = [System.Drawing.Color]::FromArgb($alpha, (245 - $tint), 250, 255)
    $pen = New-Object System.Drawing.Pen($col, $thick)

    $pts = New-Object 'System.Drawing.PointF[]' 7
    for ($k = 0; $k -lt 7; $k++) {
        $yy = [float]($fh * $k / 6.0)
        $xx = [float]($x + $rng.Next(-2, 3))
        $pts[$k] = [System.Drawing.PointF]::new($xx, $yy)
    }
    $gfx.DrawLines($pen, $pts)
    $pen.Dispose()
}

# Short foam highlights.
for ($i = 0; $i -lt 70; $i++) {
    $x = $rng.Next(0, $fw)
    $y = $rng.Next(0, $fh)
    $a = $rng.Next(120, 240)
    $col = [System.Drawing.Color]::FromArgb($a, 255, 255, 255)
    $pen = New-Object System.Drawing.Pen($col, 1)
    $gfx.DrawLine($pen, $x, $y, ($x + $rng.Next(-2, 3)), ($y + $rng.Next(2, 7)))
    $pen.Dispose()
}

# GAP PASS - punches real transparent holes so the sheet is perforated rather
# than near-solid. Without this, ~150 anti-aliased lines cover almost every column
# and the cascade renders as a pale painted panel (measured: 84% of pixels inked).
# SourceCopy on a fully transparent brush writes alpha 0 instead of blending.
$gfx.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
$clearBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(0, 0, 0, 0))
for ($i = 0; $i -lt 34; $i++) {
    $gx = $rng.Next(0, $fw)
    $gy = $rng.Next(0, $fh)
    $gw = $rng.Next(1, 5)
    $gh = $rng.Next(5, 22)
    $gfx.FillRectangle($clearBrush, $gx, $gy, $gw, $gh)
}
$clearBrush.Dispose()
$gfx.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceOver

$gfx.Dispose()
$fallPath = Join-Path $OutDir "waterfall_streaks.png"
$fall.Save($fallPath, [System.Drawing.Imaging.ImageFormat]::Png)
$fall.Dispose()

# ---------------------------------------------------------------------------
# foam.png - 64x64, wrap-safe bubble field (used for impact/shore foam)
#
# Drawn 9 times per bubble (offset by +/-64 in both axes) so the texture TILES:
# foam strips scroll with panU, and a non-tiling bubble field would pop a seam
# every 64 units along the strip.
# ---------------------------------------------------------------------------
$mw = 64; $mh = 64
$foam = New-Object System.Drawing.Bitmap($mw, $mh, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$gfx2 = [System.Drawing.Graphics]::FromImage($foam)
$gfx2.Clear([System.Drawing.Color]::FromArgb(0, 0, 0, 0))
$gfx2.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias

$rng2 = New-Object System.Random(777)

for ($i = 0; $i -lt 140; $i++) {
    $bx = $rng2.Next(0, $mw)
    $by = $rng2.Next(0, $mh)
    $br = $rng2.Next(1, 4)
    $ba = $rng2.Next(140, 246)
    $col = [System.Drawing.Color]::FromArgb($ba, 245, 250, 255)
    $brush = New-Object System.Drawing.SolidBrush($col)
    foreach ($ox in @(-$mw, 0, $mw)) {
        foreach ($oy in @(-$mh, 0, $mh)) {
            $gfx2.FillEllipse($brush, ($bx + $ox - $br), ($by + $oy - $br), (2 * $br), (2 * $br))
        }
    }
    $brush.Dispose()
}

# Bright cores on a few bubbles so the field has contrast rather than flat noise.
for ($i = 0; $i -lt 26; $i++) {
    $bx = $rng2.Next(0, $mw)
    $by = $rng2.Next(0, $mh)
    $col = [System.Drawing.Color]::FromArgb(240, 255, 255, 255)
    $brush = New-Object System.Drawing.SolidBrush($col)
    foreach ($ox in @(-$mw, 0, $mw)) {
        foreach ($oy in @(-$mh, 0, $mh)) {
            $gfx2.FillEllipse($brush, ($bx + $ox), ($by + $oy), 2, 2)
        }
    }
    $brush.Dispose()
}

$gfx2.Dispose()
$foamPath = Join-Path $OutDir "foam.png"
$foam.Save($foamPath, [System.Drawing.Imaging.ImageFormat]::Png)
$foam.Dispose()

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
foreach ($p in @($poolPath, $fallPath, $foamPath)) {
    $img = [System.Drawing.Image]::FromFile((Resolve-Path $p))
    $size = (Get-Item $p).Length
    Write-Host ("{0}  {1}x{2}  {3} bytes" -f $p, $img.Width, $img.Height, $size)
    $img.Dispose()
}
