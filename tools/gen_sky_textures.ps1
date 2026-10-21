# tools/gen_sky_textures.ps1
#
# Generates the sky pairs used by the outdoor maps:
#
#   GameData/Tex/Global/Sky/sky_grad_day.png    256x256  RGB
#   GameData/Tex/Global/Sky/sky_cap_day.png      64x64   RGB
#   GameData/Tex/Global/Sky/sky_grad_dusk.png   256x256  RGB
#   GameData/Tex/Global/Sky/sky_cap_dusk.png     64x64   RGB
#
# WHY THESE EXIST
# ---------------
# oz::Skybox draws SIX separate quads (Source/Renderer/Skybox.hpp). Each wall quad
# stretches the ENTIRE image across itself, so at a cube corner one wall's right
# edge meets the next wall's left edge. The shipped panoramas have unrelated
# content at their two edges, which is the hard diagonal band visible in every
# outdoor screenshot. The top face additionally uses a SEPARATE cap texture on a
# horizontal quad, so the wall tops meet an unrelated image - the bright rectangle
# across the upper sky.
#
# THE TWO CONSTRAINTS THAT FIX IT WITHOUT TOUCHING C++
# ----------------------------------------------------
# 1. Horizontal periodicity. Every horizontal variation here is a sum of
#    sin(2*pi*k*u) with INTEGER k, and the last column is then overwritten with
#    column 0. That makes the left and right edges byte-identical, so adjacent
#    walls join seamlessly. (Faces 2 and 5 also mirror U; a symmetric field makes
#    the mirror invisible, which is why the clouds are not given a direction.)
# 2. The cap edge must equal the gradient's ZENITH, because the cap is a
#    horizontal plane meeting the wall tops at 90 degrees.
#
# HORIZON AT MID-HEIGHT, NOT AT THE BOTTOM
# ----------------------------------------
# The cube is centred on the camera and each wall is 2000 units tall, so the
# image's vertical midpoint (V = 0.5) lands exactly on the eye line. The gradient
# therefore reaches its horizon colour at row h/2 and continues into a dimmer
# below-horizon band for the lower half, which terrain and fog mostly cover.
#
# Deterministic: fixed seeds, so re-running rewrites identical bytes.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools/gen_sky_textures.ps1

param(
    [string]$OutDir = "GameData/Tex/Global/Sky"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

function Clamp255([double]$v) {
    if ($v -lt 0) { return 0 }
    if ($v -gt 255) { return 255 }
    return [int][math]::Round($v)
}

# Smooth ease so the gradient has no visible banding step at the joins.
function Smooth([double]$t) {
    return $t * $t * (3.0 - 2.0 * $t)
}

function New-SkyGradient {
    param(
        [string]$Path,
        [int[]]$Zenith,
        [int[]]$Horizon,
        [int[]]$Below,
        [double]$CloudAmp,
        [int]$Seed
    )

    $w = 256
    $h = 256
    $bmp = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $rng = New-Object System.Random($Seed)

    # Phases for the 4 cloud harmonics (all integer periods => periodic in U).
    $phase = @()
    for ($k = 1; $k -le 4; $k++) { $phase += $rng.NextDouble() * 6.28318530718 }

    for ($y = 0; $y -lt $h; $y++) {
        # V IS INVERTED RELATIVE TO FILE ORDER. Measured in-client: the wall's TOP
        # edge samples the image's LAST row and the eye line samples the middle.
        # So row 0 must be the below-horizon colour and row h-1 the zenith, or the
        # player sees the below-horizon half above the eye line (which is exactly
        # what the first version of this script produced: a 50-level step where
        # the cap met the wall, with the wall top reading 'Below').
        $v = 1.0 - ($y / ($h - 1.0))

        # SYMMETRIC ABOUT THE EYE LINE, ON PURPOSE.
        #
        # Measured in-client: adjacent wall quads disagree by ~40 levels on a
        # one-way ramp, because two of the four side faces sample V in the opposite
        # direction from the other two (a Skybox.hpp face-orientation issue). A
        # texture that is symmetric about its vertical midpoint makes a V flip
        # invisible - and the mirrored lower half is hidden by terrain and by the
        # horizon fog that Sky.fs applies below ~6 degrees anyway.
        #
        # vv = 0 at the zenith (rows 0 AND h-1), 0.5 at the horizon (middle row).
        $vv = [math]::Min($v, 1.0 - $v)

        $t = Smooth($vv / 0.5)
        $r = $Zenith[0] + ($Horizon[0] - $Zenith[0]) * $t
        $g = $Zenith[1] + ($Horizon[1] - $Zenith[1]) * $t
        $b = $Zenith[2] + ($Horizon[2] - $Zenith[2]) * $t

        # Cloud envelope: upper sky only, fading to zero at both the zenith and the
        # horizon so the zenith row stays exactly the zenith colour the cap needs.
        $env = 0.0
        if ($vv -gt 0.06 -and $vv -lt 0.40) {
            $env = [math]::Sin([math]::PI * (($vv - 0.06) / 0.34))
        }

        for ($x = 0; $x -lt $w; $x++) {
            $u = $x / [double]$w

            $cloud = 0.0
            if ($env -gt 0.0 -and $CloudAmp -gt 0.0) {
                for ($k = 1; $k -le 4; $k++) {
                    $cloud += [math]::Sin(2.0 * [math]::PI * $k * $u + $phase[$k - 1]) * (1.0 / $k)
                }
                $cloud = $cloud * $CloudAmp * $env * 0.35
            }

            $rr = Clamp255 ($r + $cloud)
            $gg = Clamp255 ($g + $cloud * 0.95)
            $bb = Clamp255 ($b + $cloud * 0.8)

            $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb($rr, $gg, $bb))
        }
    }

    # Force the right edge to equal the left edge, byte for byte, so adjacent
    # wall quads join exactly regardless of sampling.
    for ($y = 0; $y -lt $h; $y++) {
        $bmp.SetPixel(($w - 1), $y, $bmp.GetPixel(0, $y))
    }

    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

function New-SkyCap {
    param(
        [string]$Path,
        [int[]]$Edge,
        [double]$Glow,
        [int]$Seed
    )

    $w = 64
    $h = 64
    $bmp = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $rng = New-Object System.Random($Seed)

    # Sun glow, offset slightly so the cap is not a dead bullseye.
    # RADIUS IS LOAD-BEARING: the wall quads reach 55 degrees of elevation, and the
    # cap quad is 2000 wide at 700 up, so the walls overdraw the cap out to
    # r = 700/tan(55) ~= 490 of the cap's 1000 half-width. Everything inside that
    # radius is hidden; only the region beyond it is visible. The glow is therefore
    # confined to r < 13/32 of the quad so the visible cap stays exactly the zenith
    # colour where it meets the wall tops.
    $cx = 24.0 + $rng.NextDouble() * 16.0
    $cy = 24.0 + $rng.NextDouble() * 16.0
    $maxR = 13.0

    for ($y = 0; $y -lt $h; $y++) {
        for ($x = 0; $x -lt $w; $x++) {
            $dx = $x - $cx
            $dy = $y - $cy
            $d = [math]::Sqrt($dx * $dx + $dy * $dy)
            $k = [math]::Max(0.0, 1.0 - ($d / $maxR))
            $k = $k * $k * $Glow

            $rr = Clamp255 ($Edge[0] + 255.0 * $k * 0.35)
            $gg = Clamp255 ($Edge[1] + 255.0 * $k * 0.32)
            $bb = Clamp255 ($Edge[2] + 255.0 * $k * 0.22)
            $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb($rr, $gg, $bb))
        }
    }

    # The outermost ring MUST be exactly the edge colour: it meets the wall tops.
    for ($x = 0; $x -lt $w; $x++) {
        $bmp.SetPixel($x, 0, [System.Drawing.Color]::FromArgb($Edge[0], $Edge[1], $Edge[2]))
        $bmp.SetPixel($x, ($h - 1), [System.Drawing.Color]::FromArgb($Edge[0], $Edge[1], $Edge[2]))
    }
    for ($y = 0; $y -lt $h; $y++) {
        $bmp.SetPixel(0, $y, [System.Drawing.Color]::FromArgb($Edge[0], $Edge[1], $Edge[2]))
        $bmp.SetPixel(($w - 1), $y, [System.Drawing.Color]::FromArgb($Edge[0], $Edge[1], $Edge[2]))
    }

    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

$dayZenith  = @(68, 108, 172)
$dayHorizon = @(200, 212, 220)
$dayBelow   = @(150, 158, 168)

$duskZenith  = @(48, 42, 86)
$duskHorizon = @(190, 150, 150)
$duskBelow   = @(90, 72, 88)

New-SkyGradient -Path (Join-Path $OutDir "sky_grad_day.png")  -Zenith $dayZenith  -Horizon $dayHorizon -Below $dayBelow -CloudAmp 14.0 -Seed 11
New-SkyGradient -Path (Join-Path $OutDir "sky_grad_dusk.png") -Zenith $duskZenith -Horizon $duskHorizon -Below $duskBelow -CloudAmp 9.0 -Seed 23

New-SkyCap -Path (Join-Path $OutDir "sky_cap_day.png")  -Edge $dayZenith  -Glow 0.7 -Seed 5
New-SkyCap -Path (Join-Path $OutDir "sky_cap_dusk.png") -Edge $duskZenith -Glow 0.45 -Seed 7

foreach ($n in @("sky_grad_day.png", "sky_cap_day.png", "sky_grad_dusk.png", "sky_cap_dusk.png")) {
    $p = Join-Path $OutDir $n
    $img = [System.Drawing.Image]::FromFile((Resolve-Path $p))
    Write-Host ("{0}  {1}x{2}  {3} bytes" -f $p, $img.Width, $img.Height, (Get-Item $p).Length)
    $img.Dispose()
}
