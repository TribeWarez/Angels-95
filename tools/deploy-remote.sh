#!/usr/bin/env bash
# deploy-remote.sh - finish an AngelServ/AngelMaster rollout on the VPS.
#
# Runs ON THE SERVER, as root, AFTER the workflow has rsynced the bundle into the
# install root. It validates the binaries, renders and installs the unit files
# present in System/ (so a host can ship either or both), restarts them and
# fails loudly (with journal output) if one does not come up.
#
# Usage (from .github/workflows/ci.yml):
#   sudo -n bash <install-root>/System/deploy-remote.sh <install-root>
#
# Sudoers on the VPS can be kept to the one fixed command
# (`.../System/deploy-remote.sh`), but note the deploy user can write that path,
# and the units run its contents as root. Treat deploy credentials accordingly.
set -euo pipefail

root="${1:?usage: deploy-remote.sh <install-root>}"
sys="$root/System"
# Overridable so the deploy script can be exercised without touching the real
# systemd tree (CI/local dry-runs); production uses the default.
unit_dir="${SYSTEMD_UNIT_DIR:-/etc/systemd/system}"

if [ ! -f "$sys/deploy-remote.sh" ]; then
    echo "ERROR: $sys/deploy-remote.sh is missing - did the rsync step run?" >&2
    exit 1
fi
chmod +x "$sys/deploy-remote.sh"

# Smoke-test a new binary before touching the running services. --version prints
# and exits, so a glibc/toolchain breakage fails here instead of taking the fleet
# down on restart (the b99 failure mode).
check_bin() {
    local name="$1"
    if [ ! -f "$sys/$name" ]; then
        echo "ERROR: $sys/$name is missing" >&2
        exit 1
    fi
    chmod +x "$sys/$name"
    "$sys/$name" --version
}

# A host may run only the game server (the public listing lives in
# TribeWarez/angels95-master-server) - ship/serve whichever unit files are
# present rather than assuming both.
units=""
if [ -f "$sys/angels95-serv.service" ]; then
    echo "--- binary check ---"
    check_bin AngelServ
    units="$units angels95-serv.service"
fi
if [ -f "$sys/angels95-master.service" ]; then
    check_bin AngelMaster
    units="$units angels95-master.service"
fi
[ -n "$units" ] || { echo "ERROR: no unit files in $sys" >&2; exit 1; }
echo "--------------------"

for unit in $units; do
    # The checked-in units carry the documented /srv/angels95 default; render
    # them to wherever this install actually lives.
    sed "s|/srv/angels95|$root|g" "$sys/$unit" > "$unit_dir/$unit"
done

# Saves/ is excluded from every deploy so player/world state survives, which
# also means a FRESH install has no such directory. GameState::save_world_state
# mkdir()s GameData/Saves/World<Name> (not recursive), so the parent has to
# exist before the first autosave or 60 s of play is lost. The master does not
# touch GameData, so only create it when the game server is part of the bundle.
if [ -f "$sys/angels95-serv.service" ]; then
    mkdir -p "$root/GameData/Saves"
fi

systemctl daemon-reload
systemctl enable $units >/dev/null 2>&1 || true
systemctl restart $units

# Restart=on-failure loops, so "activating" is not success. Give each unit a
# short window, then dump why it failed.
for unit in $units; do
    ok=0
    for _ in $(seq 1 20); do
        if systemctl is-active --quiet "$unit"; then ok=1; break; fi
        sleep 0.5
    done
    if [ "$ok" -ne 1 ]; then
        echo "ERROR: $unit did not become active" >&2
        systemctl --no-pager --lines=0 status "$unit" >&2 || true
        journalctl -u "$unit" -n 60 --no-pager >&2 || true
        exit 1
    fi
done

echo "Deployed build: $(cat "$sys/BUILD_VERSION" 2>/dev/null || echo unknown)"
systemctl --no-pager --lines=0 status $units || true
