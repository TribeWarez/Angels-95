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

# --- Prune the packages this script owns ---
# OzPack has no notion of a stale output: a renamed source folder used to leave
# its old .oz* behind, and PackageAssetLoader opens EVERY .oz* under System/Data,
# so a stale package keeps resolving paths that no longer exist (and can win a
# basename tie against the current one). Delete what a previous run wrote first.
# imported_*.oz* are the editor's own import packages - not ours, keep them.
Get-ChildItem -Path $OUT -Recurse -File |
    Where-Object { $_.Name -like "*.oz*" -and $_.Name -notlike "imported_*" } |
    ForEach-Object { Remove-Item $_.FullName -Force }

Write-Step "=== Packaging Shared Assets ==="

# --- Audio ---
$sndDir = "$GAMEDATA\Audio\Sounds"
if (Test-Path $sndDir) {
    Run-OzPack "OZSD" "$sndDir" "$OUT_SND\global.ozsnd"
}

$ambDir = "$GAMEDATA\Audio\Ambience"
if (Test-Path $ambDir) {
    Run-OzPack "OZMX" $ambDir "$OUT_MUS\ambience.ozmux"
}

$musicDir = "$GAMEDATA\Audio\Music"
if (Test-Path $musicDir) {
    Run-OzPack "OZMX" $musicDir "$OUT_MUS\library.ozmux"
}

# --- Weapons (one package per weapon, so a weapon's def, model, textures and
#     source art stay together and its def-relative stats keep resolving) ---
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

# --- Gear / objects (defs at the root next to the meshes and the plant folder;
#     the registry reads packaged defs, so the whole folder is one package) ---
$objDir = "$GAMEDATA\Tex\Global\Models"
if (Test-Path $objDir) {
    Run-OzPack "OZPK" $objDir "$OUT\models.ozpak"
}

# --- Items (item icons + MedMeshes) ---
$itemsDir = "$GAMEDATA\Tex\Global\Items"
if (Test-Path $itemsDir) {
    Run-OzPack "OZTX" $itemsDir "$OUT_TEX\items.oztex"
}

# --- Skybox library (*.png panoramas used by levelinfo skybox paths) ---
$skyDir = "$GAMEDATA\Tex\Global\Sky"
if (Test-Path $skyDir) {
    Run-OzPack "OZTX" $skyDir "$OUT_TEX\sky.oztex"
}

# --- Portal/effect textures ---
$efxDir = "$GAMEDATA\Tex\Global\EFX"
if (Test-Path $efxDir) {
    Run-OzPack "OZTX" $efxDir "$OUT_TEX\efx.oztex"
}

# --- Gore: blood splat decals (Tex/Global/Gibs/*.jpg) ---
$gibsDir = "$GAMEDATA\Tex\Global\Gibs"
if (Test-Path $gibsDir) {
    Run-OzPack "OZTX" $gibsDir "$OUT_TEX\gibs.oztex"
}

# --- Title art ---
$titleDir = "$GAMEDATA\Tex\Title"
if (Test-Path $titleDir) {
    Run-OzPack "OZTX" $titleDir "$OUT_TEX\title.oztex"
}

# --- Menu widget skin ---
$menuDir = "$GAMEDATA\Tex\Menu"
if (Test-Path $menuDir) {
    Run-OzPack "OZTX" $menuDir "$OUT_TEX\menu.oztex"
}

# --- Editor icons (entity gizmos + editor app icon) ---
$editorDir = "$GAMEDATA\Tex\Editor"
if (Test-Path $editorDir) {
    Run-OzPack "OZTX" $editorDir "$OUT_TEX\editor_icons.oztex"
}

# --- Engine art (HUD atlas, bar font, text bar, GameUI.ozls, app icons) ---
$engineArtDir = "$GAMEDATA\Tex\Engine"
if (Test-Path $engineArtDir) {
    Run-OzPack "OZPK" $engineArtDir "$OUT\engine.ozpak"
}

# --- Game hierarchy (GameData/Game: : game / : campaign / : sector / : mission
#     / : objective / : trigger / : event defs). One package for the whole
#     Angels95 game layer so a packaged install keeps the campaign graph. ---
$gameDir = "$GAMEDATA\Game"
if (Test-Path $gameDir) {
    Run-OzPack "OZPK" $gameDir "$OUT\game.ozpak"
}

# --- Shared tileset library ---
$tilesetDir = "$GAMEDATA\Tex\Tileset"
if (Test-Path $tilesetDir) {
    Run-OzPack "OZTX" $tilesetDir "$OUT_TEX\tileset.oztex"
}

# --- Pawn models + sprites (Pawn/Pawns: *.ozls, *.glb/.gltf, <Name>.png) ---
$pawnDir = "$GAMEDATA\Pawn\Pawns"
if (Test-Path $pawnDir) {
    Run-OzPack "OZPK" $pawnDir "$OUT\pawns.ozpak"
}

# --- Pawn definitions (*.cfg). The client and the editor read these from the
#     package too, so a packaged-only install keeps the authored defs. ---
$pawnDefsDir = "$GAMEDATA\Pawn\PawnDefs"
if (Test-Path $pawnDefsDir) {
    Run-OzPack "OZPK" $pawnDefsDir "$OUT\pawns_defs.ozpak"
}

# --- Player character model (Pawn/Player: *.glb + textures) ---
$playerDir = "$GAMEDATA\Pawn\Player"
if (Test-Path $playerDir) {
    Run-OzPack "OZPK" $playerDir "$OUT\player.ozpak"
}

# --- Skill tree + spell defs (Pawn/Skills: *.ozls) ---
$skillsDir = "$GAMEDATA\Pawn\Skills"
if (Test-Path $skillsDir) {
    Run-OzPack "OZPK" $skillsDir "$OUT\skills.ozpak"
}

# --- Light defaults (Pawn/Lights: *.ozls) ---
$lightsDir = "$GAMEDATA\Pawn\Lights"
if (Test-Path $lightsDir) {
    Run-OzPack "OZPK" $lightsDir "$OUT\lights.ozpak"
}

# --- Environment meshes + vertex-keyframe clips (Environment/**: *.glb/.obj/.ozanim) ---
$envDir = "$GAMEDATA\Environment"
if (Test-Path $envDir) {
    Run-OzPack "OZPK" $envDir "$OUT\environment.ozpak"
}

# --- Shaders ---
$shaderDir = "$GAMEDATA\Shaders"
if (Test-Path $shaderDir) {
    Run-OzPack "OZPK" $shaderDir "$OUT\shaders.ozpak"
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
