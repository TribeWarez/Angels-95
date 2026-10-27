#!/usr/bin/env bash
# build-linux-bundle.sh - assemble the self-contained Linux release bundle.
#
# Packages GameData -> System/Data (via build-data.sh), then lays the Linux
# binaries + INI templates + run script + systemd units under System/ and tars
# it, mirroring what the Windows job assembles into System-<tag>.zip. A Linux
# user extracts the tarball and runs System/run.sh; the client then finds its
# packaged data in System/Data (RelocateToInstallRoot / PackageAssetLoader).
#
# Usage: ./build-linux-bundle.sh <tag>     (run from the repo root)

set -euo pipefail

TAG="${1:?usage: $0 <tag>}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STAGE="$ROOT/System"
OUTDIR="$ROOT/release"

# 1. Package the game data (build-data.sh writes $ROOT/System/Data).
"$ROOT/build-data.sh"

# 2. Lay out the bundle. The binaries must already be built (`make`).
mkdir -p "$STAGE"
for b in Angels95 AngelServ AngelMaster OzPack; do
    if [ ! -f "$ROOT/$b" ]; then
        echo "ERROR: $b not built at $ROOT/$b (run 'make' first)" >&2
        exit 1
    fi
    cp "$ROOT/$b" "$STAGE/$b"
    chmod +x "$STAGE/$b"
done

# Client settings template - same content as the Windows bundle's Angels95.ini.
# The client fills in any key it does not find here and rewrites this on exit.
cat > "$STAGE/Angels95.ini" <<'EOF'
; Angels95 client settings - grouped by concern. The client fills in any key
; it does not find here and rewrites this file on exit.
[AngelTech.Window]
window_width=1280
window_height=720
borderless=1

[AngelTech.Render]
vsync=1
msaa=0

[AngelTech.Audio]
audio_volume=100.000000
music_volume=100.000000
sfx_volume=100.000000
mute=False
EOF

# Server template (AngelServ is CLI-driven; this is a template only).
cat > "$STAGE/OzServer.ini" <<'EOF'
[Server]
Port=27015
HttpPort=8080
MaxPlayers=16
WorldDir=GameData

[Game]
ServerName=Angels95 Server
EOF

# Run script: cd to the install root (parent of System/) so the client finds
# System/Data and System/Angels95.ini, and pre-create the writable saves dir.
cat > "$STAGE/run.sh" <<'EOF'
#!/usr/bin/env bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$DIR/../GameData/Saves"
cd "$DIR/.."
exec "$DIR/Angels95" "$@"
EOF
chmod +x "$STAGE/run.sh"

# VPS/systemd units - shipped for parity with the Windows zip; used by the
# separate deploy job and the documented manual VPS deployment.
for f in angels95-serv.service angels95-master.service deploy-remote.sh; do
    if [ -f "$ROOT/tools/$f" ]; then
        cp "$ROOT/tools/$f" "$STAGE/$f"
    fi
done
chmod +x "$STAGE/deploy-remote.sh" 2>/dev/null || true

# 3. Tar it (top-level entry is System/, exactly like the Windows zip).
mkdir -p "$OUTDIR"
OUT="$OUTDIR/System-${TAG}-linux-x86_64.tar.gz"
tar -czf "$OUT" -C "$ROOT" System/
echo "==> Wrote $OUT"
ls -la "$OUT"
