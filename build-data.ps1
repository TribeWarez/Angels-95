# build-data.ps1 — Package all GameData assets into .oz* containers
# Run from repo root after building OzPack.
# Requires: System\OzPack.exe (built via `mingw32-make ozpack`)

$OZPACK   = "$PSScriptRoot\System\OzPack.exe"
$GAMEDATA = "$PSScriptRoot\GameData"
$OUT      = "$PSScriptRoot\System\Data"
$OUT_TEX  = "$OUT\Textures"
$OUT_MUS  = "$OUT\Music"
$OUT_SND  = "$OUT\Sound"
$OUT_ZONES= "$OUT\Zones"

function Write-Step { Write-Host "==> $args" -ForegroundColor Cyan }
function Run-OzPack { param($magic, $dir, $out)
    $dir = $dir.Replace("/", "\")
    $out = $out.Replace("/", "\")
    Write-Step "[$magic] $dir -> $out"

    # Validate input directory
    if (-not (Test-Path $dir -PathType Container)) {
        Write-Host "ERROR: Input directory not found: $dir" -ForegroundColor Red
        exit 1
    }

    # Ensure output directory exists
    $outDir = Split-Path $out -Parent
    if (-not (Test-Path $outDir -PathType Container)) {
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    }

    # Verify OzPack exists
    if (-not (Test-Path $OZPACK -PathType Leaf)) {
        Write-Host "ERROR: OzPack not found at $OZPACK" -ForegroundColor Red
        exit 1
    }

    # Run OzPack and capture output robustly
    $stdout = [System.Collections.Generic.List[string]]::new()
    $stderr = [System.Collections.Generic.List[string]]::new()
    $global:LastExitCode = -1

    try {
        $process = Start-Process -FilePath $OZPACK -ArgumentList @("pack", $magic, $dir, $out) -NoNewWindow -RedirectStandardOutput "stdout.tmp" -RedirectStandardError "stderr.tmp" -Wait -PassThru
        $global:LastExitCode = $process.ExitCode
        if (Test-Path "stdout.tmp") {
            $stdout.AddRange([System.IO.File]::ReadAllLines("stdout.tmp"))
            Remove-Item "stdout.tmp" -Force -ErrorAction SilentlyContinue
        }
        if (Test-Path "stderr.tmp") {
            $stderr.AddRange([System.IO.File]::ReadAllLines("stderr.tmp"))
            Remove-Item "stderr.tmp" -Force -ErrorAction SilentlyContinue
        }
    } catch {
        Write-Host "ERROR: Failed to execute OzPack: $_" -ForegroundColor Red
        exit 1
    }

    $allOutput = @($stdout) + @($stderr)
    if ($allOutput.Count -gt 0) {
        Write-Host "  $($allOutput -join "`n  ")"
    }

    if ($global:LastExitCode -ne 0) {
        Write-Host "ERROR: OzPack failed with exit code $global:LastExitCode" -ForegroundColor Red
        if ($stderr.Count -gt 0) {
            Write-Host "Stderr output:" -ForegroundColor Yellow
            $stderr | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
        }
        if ($stdout.Count -eq 0 -and $stderr.Count -eq 0) {
            Write-Host "No output was produced. Possible crash or missing dependency." -ForegroundColor Yellow
        }
        exit 1
    }
}

# Ensure output directories
foreach ($d in @($OUT, $OUT_TEX, $OUT_MUS, $OUT_SND, $OUT_ZONES)) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
}

Write-Step "=== Packaging Global Assets ==="

# --- Guns / Weapons ---
$gunDir = "$GAMEDATA\Tex\Global\Gun"
if (Test-Path $gunDir) {
    Get-ChildItem $gunDir -Directory | ForEach-Object {
        $name = $_.Name
        # Skip empty directories
        if ((Get-ChildItem $_.FullName -File -Recurse | Measure-Object).Count -gt 0) {
            Run-OzPack "OZPK" "$gunDir\$name" "$OUT\gun_$name.ozpak"
        } else {
            Write-Host "  (skipping empty: $name)" -ForegroundColor DarkGray
        }
    }
}

# --- Objects / Props ---
$objDir = "$GAMEDATA\Tex\Global\Models"
if (Test-Path $objDir) {
    Get-ChildItem $objDir -Directory | ForEach-Object {
        $name = $_.Name
        if ((Get-ChildItem $_.FullName -File -Recurse | Measure-Object).Count -gt 0) {
            Run-OzPack "OZPK" "$objDir\$name" "$OUT\object_$name.ozpak"
        } else {
            Write-Host "  (skipping empty: $name)" -ForegroundColor DarkGray
        }
    }
}

# --- Items (textures) ---
$itemsDir = "$GAMEDATA\Tex\Global\Items"
if (Test-Path $itemsDir) {
    Run-OzPack "OZTX" $itemsDir "$OUT_TEX\items.oztex"
}

# --- Sounds ---
$sndDir = "$GAMEDATA\Audio\Sounds"
if (Test-Path $sndDir) {
    Run-OzPack "OZSD" "$sndDir" "$OUT_SND\global.ozsnd"
}

# --- Music (ambience) ---
$ambDir = "$GAMEDATA\Audio\Ambience"
if (Test-Path $ambDir) {
    Run-OzPack "OZMX" $ambDir "$OUT_MUS\ambience.ozmux"
}

# --- Shaders ---
$shaderDir = "$GAMEDATA\Shaders"
if (Test-Path $shaderDir) {
    Run-OzPack "OZPK" $shaderDir "$OUT\shaders.ozpak"
}

# --- Editor icons (entity gizmos + editor app icon) ---
$engineDir = "$GAMEDATA\Tex\Editor"
if (Test-Path $engineDir) {
    Run-OzPack "OZTX" $engineDir "$OUT_TEX\editor_icons.oztex"
}

# --- Skybox library (Tex/Global/Sky: *.png panoramas used by levelinfo skybox paths) ---
$skyDir = "$GAMEDATA\Tex\Global\Sky"
if (Test-Path $skyDir) {
    Run-OzPack "OZTX" $skyDir "$OUT_TEX\sky.oztex"
}

# --- Pawn sprites/screams (Global/Pawn: <Name>.png / .mp3) ---
$pawnDir = "$GAMEDATA\Global\Pawn"
if (Test-Path $pawnDir) {
    Run-OzPack "OZTX" $pawnDir "$OUT_TEX\pawn.oztex"
}

# --- Player character model (Global/Player: *.glb + textures) ---
$playerDir = "$GAMEDATA\Global\Player"
if (Test-Path $playerDir) {
    Run-OzPack "OZPK" $playerDir "$OUT\player.ozpak"
}

# --- Title/menu art ---
$titleDir = "$GAMEDATA\Tex\Title"
if (Test-Path $titleDir) {
    Run-OzPack "OZTX" $titleDir "$OUT_TEX\title.oztex"
}

# --- Vertex-keyframe animation clips (Global/Anims: *.ozanim) ---
$animsDir = "$GAMEDATA\Global\Anims"
if (Test-Path $animsDir) {
    Run-OzPack "OZPK" $animsDir "$OUT\anims.ozpak"
}

# --- Ethereal / angelic skill tree nodes (Global/Skills: *.ozls) ---
$skillsDir = "$GAMEDATA\Global\Skills"
if (Test-Path $skillsDir) {
    Run-OzPack "OZPK" $skillsDir "$OUT\skills.ozpak"
}

# --- Spells (Global/Spells: *.ozls). The registry also reads these loose from
# GameData/, but the distributed System/ bundle carries only System\Data\*.oz*,
# so without this block spell defs would be missing from a release build. ---
$spellsDir = "$GAMEDATA\Global\Spells"
if (Test-Path $spellsDir) {
    Run-OzPack "OZPK" $spellsDir "$OUT\spells.ozpak"
}

Write-Step "=== Packaging Worlds ==="

# --- Worlds (oztex and ozone) ---
Get-ChildItem "$GAMEDATA\Worlds" -Directory | ForEach-Object {
    $worldName = $_.Name

    # Package tileset textures as .oztex
    $tilesetDir = "$GAMEDATA\Worlds\$worldName\oztex"
    if (Test-Path $tilesetDir) {
        Run-OzPack "OZTX" $tilesetDir "$OUT_TEX\world_${worldName}_tex.oztex"
    }

    # Package world file(s) as .ozone (or .ozwn)
    $worldFile = "$GAMEDATA\Worlds\$worldName\World.ozone"
    if (Test-Path $worldFile) {
        Run-OzPack "OZWN" "$GAMEDATA\Worlds\$worldName" "$OUT_ZONES\world_${worldName}.ozone"
    }
}

Write-Step "=== Verifying Output ==="
Get-ChildItem $OUT -Recurse -Filter "*.oz*" | Select-Object Name, Length | Format-Table -AutoSize

Write-Step "Done. Packages in $OUT" -ForegroundColor Green
