# gen_placeholders.ps1 - generate the placeholder assets for content that was
# referenced but never authored (the 57 dangling references worldcheck
# --asset-refs used to carry in tools/assetrefs-allow.txt).
#
# These are deliberately obvious placeholders, not art: a bevel-free box mesh,
# a flat two-tone texture, an initial-letter icon, and a short synthesised
# sound. They exist so every reference resolves and every wired feature is
# visible/audible (the gear defs carry item_id + equip_slot; the spells are the
# active cast system; the screams are the aggro stinger). Replace them with
# real art by overwriting the file - nothing in the defs needs to change.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools/gen_placeholders.ps1

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
function Full([string]$rel) { Join-Path $root $rel }
function Ensure([string]$rel) { $p = Full $rel; if (-not (Test-Path $p)) { New-Item -ItemType Directory -Force -Path $p | Out-Null }; $p }
# Never overwrite an existing asset: re-running the generator must not clobber
# real art (Object1Icon.png already existed when this script was first run).


# ---------------------------------------------------------------- PNG helpers
function Save-Plate([string]$path, [int]$size, [System.Drawing.Color]$fill,
                    [System.Drawing.Color]$edge, [string]$label,
                    [System.Drawing.Color]$textColor, [bool]$transparent) {
    if (Test-Path $path) { Write-Host "  (exists) $path"; return }
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    if ($transparent) { $g.Clear([System.Drawing.Color]::Transparent) }
    else { $g.Clear($fill) }
    $inset = [Math]::Max(1, [int]($size * 0.06))
    $inner = $size - 2 * $inset
    $rect = New-Object System.Drawing.Rectangle($inset, $inset, $inner, $inner)
    $brush = New-Object System.Drawing.SolidBrush($fill)
    $penWidth = [Math]::Max(1, [int]($size * 0.06))
    $pen = New-Object System.Drawing.Pen($edge, $penWidth)
    $g.FillRectangle($brush, $rect)
    $g.DrawRectangle($pen, $rect)
    if ($label) {
        $scale = 0.55
        if ($label.Length -gt 1) { $scale = 0.34 }
        $fontSize = [Math]::Max(6, [int]($size * $scale))
        $font = New-Object System.Drawing.Font('Consolas', $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
        $tb = New-Object System.Drawing.SolidBrush($textColor)
        $sf = New-Object System.Drawing.StringFormat
        $sf.Alignment = [System.Drawing.StringAlignment]::Center
        $sf.LineAlignment = [System.Drawing.StringAlignment]::Center
        $g.DrawString($label, $font, $tb, (New-Object System.Drawing.RectangleF(0, 0, $size, $size)), $sf)
        $tb.Dispose(); $font.Dispose()
    }
    $brush.Dispose(); $pen.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

# Two-tone diffuse texture: base fill + a diagonal band, so the surface reads as
# material rather than a flat colour.
function Save-Material([string]$path, [int]$size, [System.Drawing.Color]$base, [System.Drawing.Color]$accent) {
    if (Test-Path $path) { Write-Host "  (exists) $path"; return }
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear($base)
    $b = New-Object System.Drawing.SolidBrush($accent)
    $y1 = [int]($size * 0.35)
    $y2 = [int]($size * 0.55)
    $pts = @(
        (New-Object System.Drawing.Point(0, 0)),
        (New-Object System.Drawing.Point($size, 0)),
        (New-Object System.Drawing.Point($size, $y1)),
        (New-Object System.Drawing.Point(0, $y2))
    )
    $g.FillPolygon($b, $pts)
    $b.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

# ------------------------------------------------------------------ OBJ helper
# A six-quad box, base at y=0, in the engine's Y-up space, with UVs.
function Save-BoxObj([string]$path, [double]$w, [double]$h, [double]$d) {
    if (Test-Path $path) { Write-Host "  (exists) $path"; return }
    $x = $w / 2.0; $z = $d / 2.0
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("# Placeholder box mesh (tools/gen_placeholders.ps1). Replace with real art.")
    [void]$sb.AppendLine("v -$x 0 -$z"); [void]$sb.AppendLine("v $x 0 -$z")
    [void]$sb.AppendLine("v $x 0 $z");    [void]$sb.AppendLine("v -$x 0 $z")
    [void]$sb.AppendLine("v -$x $h -$z"); [void]$sb.AppendLine("v $x $h -$z")
    [void]$sb.AppendLine("v $x $h $z");   [void]$sb.AppendLine("v -$x $h $z")
    foreach ($uv in @("0 0", "1 0", "1 1", "0 1")) { [void]$sb.AppendLine("vt $uv") }
    foreach ($n in @("0 1 0", "0 -1 0", "0 0 1", "0 0 -1", "1 0 0", "-1 0 0")) { [void]$sb.AppendLine("vn $n") }
    # bottom, top, front, back, right, left (CCW seen from outside)
    $faces = @(
        @(1, 4, 3, 2), @(5, 6, 7, 8), @(2, 3, 7, 6), @(4, 1, 5, 8),
        @(3, 4, 8, 7), @(1, 2, 6, 5)
    )
    $n = 1
    foreach ($f in $faces) {
        $uvn = @($n, 1, 2, 3)[0..3]
        [void]$sb.AppendLine("f " + [string]::Join(" ", ($f | ForEach-Object { "$_/$(1)/$(1)" })))
        $n++
    }
    [System.IO.File]::WriteAllText($path, $sb.ToString())
}

# ------------------------------------------------------------------ WAV helper
function Save-Wav([string]$path, [double]$seconds, [double]$freqStart, [double]$freqEnd,
                  [double]$noise, [double]$decay, [int]$sampleRate = 22050) {
    if (Test-Path $path) { Write-Host "  (exists) $path"; return }
    $count = [int]($seconds * $sampleRate)
    $data = New-Object byte[] ($count * 2)
    $rng = New-Object System.Random 1337
    $phase = 0.0
    for ($i = 0; $i -lt $count; $i++) {
        $t = $i / [double]$sampleRate
        $freq = $freqStart + ($freqEnd - $freqStart) * ($t / $seconds)
        $phase += 2.0 * [Math]::PI * $freq / $sampleRate
        $env = [Math]::Exp(- $decay * $t)
        $s = [Math]::Sin($phase) * 0.6 + ([Math]::Sign([Math]::Sin($phase * 0.5)) * 0.25)
        $s += $noise * ($rng.NextDouble() * 2.0 - 1.0)
        $v = [int]([Math]::Max(-1.0, [Math]::Min(1.0, $s * $env)) * 32000)
        $data[2 * $i] = [byte]($v -band 0xFF)
        $data[2 * $i + 1] = [byte](($v -shr 8) -band 0xFF)
    }
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write([char[]]'RIFF'); $bw.Write([int](36 + $data.Length)); $bw.Write([char[]]'WAVE')
    $bw.Write([char[]]'fmt '); $bw.Write([int]16); $bw.Write([int16]1); $bw.Write([int16]1)
    $bw.Write([int]$sampleRate); $bw.Write([int]($sampleRate * 2)); $bw.Write([int16]2); $bw.Write([int16]16)
    $bw.Write([char[]]'data'); $bw.Write([int]$data.Length); $bw.Write($data)
    [System.IO.File]::WriteAllBytes($path, $ms.ToArray())
    $bw.Dispose(); $ms.Dispose()
}

# ------------------------------------------------------------------- gear/items
# One box mesh + two-tone texture + initial icon per object def. Dimensions are
# a plausible silhouette per item type, nothing more.
$models = Ensure 'GameData\Tex\Global\Models'
$gear = @(
    @{ n = 'Accessory1'; w = 0.30; h = 0.16; d = 0.22; label = 'A1'; base = 190, 160, 90;  accent = 235, 210, 140 },
    @{ n = 'Accessory2'; w = 0.26; h = 0.20; d = 0.18; label = 'A2'; base = 120, 150, 190; accent = 175, 205, 235 },
    @{ n = 'Armory1';    w = 0.62; h = 0.46; d = 0.30; label = 'AR'; base = 150, 140, 130; accent = 200, 190, 180 },
    @{ n = 'Armory2';    w = 0.58; h = 0.40; d = 0.34; label = 'AR'; base = 135, 145, 150; accent = 185, 195, 200 },
    @{ n = 'Boots';      w = 0.42; h = 0.24; d = 0.66; label = 'BO'; base = 120, 95,  70;  accent = 170, 140, 105 },
    @{ n = 'Helmet';     w = 0.40; h = 0.34; d = 0.44; label = 'HE'; base = 155, 160, 170; accent = 205, 210, 220 },
    @{ n = 'Jewelry1';   w = 0.16; h = 0.14; d = 0.06; label = 'J1'; base = 215, 180, 80;  accent = 255, 230, 150 },
    @{ n = 'Jewelry2';   w = 0.16; h = 0.14; d = 0.06; label = 'J2'; base = 180, 110, 190; accent = 230, 170, 235 },
    @{ n = 'Legs';       w = 0.38; h = 0.55; d = 0.28; label = 'LE'; base = 130, 130, 140; accent = 180, 180, 190 },
    @{ n = 'Object1';    w = 0.14; h = 0.14; d = 0.62; label = 'O1'; base = 110, 120, 130; accent = 160, 170, 180 },
    @{ n = 'Object2';    w = 0.16; h = 0.16; d = 0.58; label = 'O2'; base = 130, 110, 100; accent = 185, 160, 145 },
    @{ n = 'Object3';    w = 0.12; h = 0.20; d = 0.50; label = 'O3'; base = 100, 130, 115; accent = 150, 185, 165 },
    @{ n = 'Object4';    w = 0.18; h = 0.12; d = 0.44; label = 'O4'; base = 140, 120, 90;  accent = 195, 170, 135 },
    @{ n = 'Object5';    w = 0.20; h = 0.18; d = 0.36; label = 'O5'; base = 120, 100, 140; accent = 170, 145, 195 }
)
foreach ($g in $gear) {
    Save-BoxObj  (Join-Path $models "$($g.n).obj")            $g.w $g.h $g.d
    Save-Material (Join-Path $models "$($g.n)Texture.png")    64 `
        ([System.Drawing.Color]::FromArgb($g.base[0], $g.base[1], $g.base[2])) `
        ([System.Drawing.Color]::FromArgb($g.accent[0], $g.accent[1], $g.accent[2]))
    Save-Plate   (Join-Path $models "$($g.n)Icon.png")        32 `
        ([System.Drawing.Color]::FromArgb(30, 30, 38)) `
        ([System.Drawing.Color]::FromArgb($g.accent[0], $g.accent[1], $g.accent[2])) `
        $g.label ([System.Drawing.Color]::FromArgb(235, 235, 240)) $false
    Write-Host "  gear $($g.n)"
}

# ------------------------------------------------------------------ selenite icon
# The weapon is shipped; only its hotbar icon was missing. A blade silhouette.
$selDir = Ensure 'GameData\Tex\Global\Gun\selenite_blade'
$selIcon = Join-Path $selDir 'selenite_blade_icon.png'
if (Test-Path $selIcon) { Write-Host "  (exists) $selIcon" } else {
$bmp = New-Object System.Drawing.Bitmap(32, 32)
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$gfx.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$gfx.Clear([System.Drawing.Color]::Transparent)
$blade = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(215, 225, 235), 4)
$blade.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$blade.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$gfx.DrawLine($blade, 7, 27, 25, 5)
$grip = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(150, 110, 70), 4)
$gfx.DrawLine($grip, 5, 29, 9, 25)
$gfx.DrawLine($grip, 11, 21, 15, 17)
$bmp.Save($selIcon, [System.Drawing.Imaging.ImageFormat]::Png)
$gfx.Dispose(); $bmp.Dispose()
Write-Host '  selenite_blade_icon.png'
}

# ------------------------------------------------------------------- spell icons
$skills = Ensure 'GameData\Tex\Global\Skills'
$spells = @(
    @{ n = 'arc_bolt';      label = 'A'; c = 120, 190, 255 },
    @{ n = 'frost_lance';   label = 'F'; c = 170, 240, 255 },
    @{ n = 'mend';          label = 'M'; c = 150, 235, 160 },
    @{ n = 'psychic_surge'; label = 'P'; c = 205, 150, 245 },
    @{ n = 'void_nova';     label = 'V'; c = 240, 130, 190 }
)
foreach ($s in $spells) {
    Save-Plate (Join-Path $skills "$($s.n).png") 32 `
        ([System.Drawing.Color]::FromArgb(26, 26, 34)) `
        ([System.Drawing.Color]::FromArgb($s.c[0], $s.c[1], $s.c[2])) `
        $s.label ([System.Drawing.Color]::FromArgb($s.c[0], $s.c[1], $s.c[2])) $false
    Write-Host "  spell icon $($s.n)"
}

# ------------------------------------------------------------------ spell sounds
$sndDir = Ensure 'GameData\Audio\Sounds\Skills'
Save-Wav (Join-Path $sndDir 'arc_bolt.wav')      0.30 1400  420 0.18 9.0
Save-Wav (Join-Path $sndDir 'frost_lance.wav')   0.45  900  240 0.35 5.0
Save-Wav (Join-Path $sndDir 'mend.wav')          0.55  520  700 0.05 3.0
Save-Wav (Join-Path $sndDir 'psychic_surge.wav') 0.40  300  980 0.20 6.0
Save-Wav (Join-Path $sndDir 'void_nova.wav')     0.60  220   70 0.40 3.5
Save-Wav (Join-Path $sndDir 'void_bolt.wav')     0.28  260  120 0.25 9.0
Write-Host '  spell sounds (6)'

# ----------------------------------------------------------------- pawn screams
$pawnDir = Ensure 'GameData\Pawn\Pawns'
Save-Wav (Join-Path $pawnDir 'Walker.wav') 0.35 420 260 0.45 7.0
Save-Wav (Join-Path $pawnDir 'Skaarj.wav') 0.40 520 220 0.40 6.0
Save-Wav (Join-Path $pawnDir 'Brute.wav')  0.55 180 110 0.35 3.5
Save-Wav (Join-Path $pawnDir 'Floater.wav') 0.30 700 380 0.30 9.0
Write-Host '  pawn screams (4)'

# --------------------------------------------------------------- liquids/effects
# Pool surfaces for the EngineTest outdoor extension. Flat pattern textures that
# read as liquid once SURF_TRANSLUCENT / SURF_GLOW and panU/panV animate them;
# the engine has no water/lava/acid texture of its own (shipped pools are
# zone-only and invisible).
$efxDir = Ensure 'GameData\Tex\Global\EFX'
function Save-Liquid([string]$name, [int]$size, [byte]$r0, [byte]$g0, [byte]$b0,
                     [byte]$r1, [byte]$g1, [byte]$b1, [double]$freq, [double]$wob) {
    $path = Join-Path $efxDir $name
    if (Test-Path $path) { Write-Host "  (exists) $path"; return }
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    for ($y = 0; $y -lt $size; $y++) {
        for ($x = 0; $x -lt $size; $x++) {
            $u = $x / [double]$size; $v = $y / [double]$size
            $w = 0.5 + 0.5 * [Math]::Sin(2 * [Math]::PI * ($u * $freq + [Math]::Sin(2 * [Math]::PI * $v * $wob)))
            $w2 = 0.5 + 0.5 * [Math]::Sin(2 * [Math]::PI * ($v * $freq * 0.7 + [Math]::Cos(2 * [Math]::PI * $u * $wob * 1.3)))
            $m = 0.5 * ($w + $w2)
            $r = [byte]($r0 + ($r1 - $r0) * $m)
            $g = [byte]($g0 + ($g1 - $g0) * $m)
            $b = [byte]($b0 + ($b1 - $b0) * $m)
            $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(255, $r, $g, $b))
        }
    }
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "  liquid $name"
}
Save-Liquid 'liquid_water.png' 64 30 70 140   110 190 235  4.0 3.0
Save-Liquid 'liquid_lava.png'  64 90 20 10    255 190 60   3.0 2.0
Save-Liquid 'liquid_acid.png'  64 20 80 30    160 230 80   5.0 4.0
Write-Host 'Placeholders written. Overwrite any file with real art; the defs stay valid.'
