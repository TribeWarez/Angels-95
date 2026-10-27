#!/usr/bin/env bash
# build-data.sh - Package all GameData assets into .oz* containers (Linux).
# Mirror of build-data.ps1. Run from the repo root after `make ozpack`.
#
# The two scripts must stay in step: build-data.ps1 is the Windows path and
# this is the POSIX path; both drive OzPack over the same GameData -> System/Data
# mapping. A folder move that updates only one leaves a release that packages a
# different set of assets per platform.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OZPACK="$ROOT/OzPack"
GAMEDATA="$ROOT/GameData"
OUT="$ROOT/System/Data"
OUT_TEX="$OUT/Textures"
OUT_MUS="$OUT/Music"
OUT_SND="$OUT/Sound"
OUT_ZONES="$OUT/Zones"

run_ozpack() {
    local magic="$1" dir="$2" out="$3"
    echo "==> [$magic] $dir -> $out"
    if [ ! -d "$dir" ]; then
        echo "ERROR: Input directory not found: $dir" >&2
        exit 1
    fi
    mkdir -p "$(dirname "$out")"
    if [ ! -f "$OZPACK" ]; then
        echo "ERROR: OzPack not found at $OZPACK (run 'make ozpack' first)" >&2
        exit 1
    fi
    "$OZPACK" pack "$magic" "$dir" "$out"
}

# Non-empty check: true when the dir contains at least one regular file
# (recursively), used to skip empty weapon/game folders the way build-data.ps1 does.
has_files() {
    [ -n "$(find "$1" -type f -print -quit 2>/dev/null)" ]
}

# Ensure output directories
for d in "$OUT" "$OUT_TEX" "$OUT_MUS" "$OUT_SND" "$OUT_ZONES"; do
    mkdir -p "$d"
done

# --- Prune the packages this script owns ---
# OzPack has no notion of a stale output; PackageAssetLoader opens EVERY .oz*
# under System/Data, so a stale package keeps resolving paths that no longer
# exist. Delete what a previous run wrote first; imported_*.oz* are the
# editor's own import packages - keep them.
find "$OUT" -type f -name '*.oz*' ! -name 'imported_*' -delete

echo "=== Packaging Shared Assets ==="

# --- Audio ---
[ -d "$GAMEDATA/Audio/Sounds" ]   && run_ozpack "OZSD" "$GAMEDATA/Audio/Sounds"   "$OUT_SND/global.ozsnd"
[ -d "$GAMEDATA/Audio/Ambience" ] && run_ozpack "OZMX" "$GAMEDATA/Audio/Ambience" "$OUT_MUS/ambience.ozmux"
[ -d "$GAMEDATA/Audio/Music" ]    && run_ozpack "OZMX" "$GAMEDATA/Audio/Music"    "$OUT_MUS/library.ozmux"

# --- Weapons (one package per weapon) ---
if [ -d "$GAMEDATA/Tex/Global/Gun" ]; then
    for sub in "$GAMEDATA"/Tex/Global/Gun/*/; do
        [ -d "$sub" ] || continue
        name="$(basename "$sub")"
        if has_files "$sub"; then
            run_ozpack "OZPK" "$sub" "$OUT/gun_$name.ozpak"
        else
            echo "  (skipping empty: $name)"
        fi
    done
fi

# --- Gear / objects ---
[ -d "$GAMEDATA/Tex/Global/Models" ] && run_ozpack "OZPK" "$GAMEDATA/Tex/Global/Models" "$OUT/models.ozpak"
# --- Items ---
[ -d "$GAMEDATA/Tex/Global/Items" ]  && run_ozpack "OZTX" "$GAMEDATA/Tex/Global/Items"  "$OUT_TEX/items.oztex"
# --- Skybox library ---
[ -d "$GAMEDATA/Tex/Global/Sky" ]    && run_ozpack "OZTX" "$GAMEDATA/Tex/Global/Sky"    "$OUT_TEX/sky.oztex"
# --- Portal/effect textures ---
[ -d "$GAMEDATA/Tex/Global/EFX" ]    && run_ozpack "OZTX" "$GAMEDATA/Tex/Global/EFX"    "$OUT_TEX/efx.oztex"
# --- Gore ---
[ -d "$GAMEDATA/Tex/Global/Gibs" ]   && run_ozpack "OZTX" "$GAMEDATA/Tex/Global/Gibs"   "$OUT_TEX/gibs.oztex"
# --- Title art ---
[ -d "$GAMEDATA/Tex/Title" ]         && run_ozpack "OZTX" "$GAMEDATA/Tex/Title"         "$OUT_TEX/title.oztex"
# --- Menu widget skin ---
[ -d "$GAMEDATA/Tex/Menu" ]          && run_ozpack "OZTX" "$GAMEDATA/Tex/Menu"          "$OUT_TEX/menu.oztex"
# --- Editor icons ---
[ -d "$GAMEDATA/Tex/Editor" ]        && run_ozpack "OZTX" "$GAMEDATA/Tex/Editor"        "$OUT_TEX/editor_icons.oztex"
# --- Engine art (HUD atlas, bar font, text bar, GameUI.ozls, app icons) ---
[ -d "$GAMEDATA/Tex/Engine" ]        && run_ozpack "OZPK" "$GAMEDATA/Tex/Engine"        "$OUT/engine.ozpak"

# --- Materials ---
[ -d "$GAMEDATA/Materials" ]         && run_ozpack "OZPK" "$GAMEDATA/Materials"         "$OUT/materials.ozpak"

# --- Game hierarchy (one package per game) ---
if [ -d "$GAMEDATA/Game" ]; then
    for sub in "$GAMEDATA"/Game/*/; do
        [ -d "$sub" ] || continue
        name="$(basename "$sub")"
        if has_files "$sub"; then
            run_ozpack "OZPK" "$sub" "$OUT/game_$name.ozpak"
        else
            echo "  (skipping empty: $name)"
        fi
    done
    # Defs sitting loose in GameData/Game (not under a game folder) have no home
    # in a per-game layout. Warn instead of silently dropping them.
    for f in "$GAMEDATA"/Game/*.ozls; do
        [ -e "$f" ] || continue
        echo "WARNING: .ozls file sits outside a game folder and will NOT be packaged: $f" >&2
    done
fi

# --- Shared tileset library ---
[ -d "$GAMEDATA/Tex/Tileset" ]       && run_ozpack "OZTX" "$GAMEDATA/Tex/Tileset"       "$OUT_TEX/tileset.oztex"

# --- Pawn models + sprites ---
[ -d "$GAMEDATA/Pawn/Pawns" ]        && run_ozpack "OZPK" "$GAMEDATA/Pawn/Pawns"        "$OUT/pawns.ozpak"
# --- Pawn definitions ---
[ -d "$GAMEDATA/Pawn/PawnDefs" ]     && run_ozpack "OZPK" "$GAMEDATA/Pawn/PawnDefs"     "$OUT/pawns_defs.ozpak"
# --- Player character model ---
[ -d "$GAMEDATA/Pawn/Player" ]       && run_ozpack "OZPK" "$GAMEDATA/Pawn/Player"       "$OUT/player.ozpak"
# --- Skill tree + spell defs ---
[ -d "$GAMEDATA/Pawn/Skills" ]       && run_ozpack "OZPK" "$GAMEDATA/Pawn/Skills"       "$OUT/skills.ozpak"
# --- Light defaults ---
[ -d "$GAMEDATA/Pawn/Lights" ]       && run_ozpack "OZPK" "$GAMEDATA/Pawn/Lights"       "$OUT/lights.ozpak"

# --- Environment meshes + vertex-keyframe clips ---
[ -d "$GAMEDATA/Environment" ]       && run_ozpack "OZPK" "$GAMEDATA/Environment"       "$OUT/environment.ozpak"

# --- Shaders ---
[ -d "$GAMEDATA/Shaders" ]           && run_ozpack "OZPK" "$GAMEDATA/Shaders"           "$OUT/shaders.ozpak"

echo "=== Packaging Worlds ==="
for world_dir in "$GAMEDATA"/Worlds/*/; do
    [ -d "$world_dir" ] || continue
    world_name="$(basename "$world_dir")"

    # Package tileset textures as .oztex
    if [ -d "$world_dir/oztex" ]; then
        run_ozpack "OZTX" "$world_dir/oztex" "$OUT_TEX/world_${world_name}_tex.oztex"
    fi

    # Package the whole world directory as .ozone (World.ozone + Models + Music + ...)
    if [ -f "$world_dir/World.ozone" ]; then
        run_ozpack "OZWN" "$world_dir" "$OUT_ZONES/world_${world_name}.ozone"
    fi
done

echo "=== Verifying Output ==="
find "$OUT" -type f -name '*.oz*' -printf '%p %s bytes\n' | sort
echo "Done. Packages in $OUT"
