# Building

## Prerequisites

- **g++** with C++20 support (GCC 10+ or MinGW-w64)
- **make** (GNU Make)
- **raylib 5.5** — must be installed system-wide (Linux) or in `C:\raylib\w64devkit` (Windows)

### Linux Dependencies

```bash
sudo apt install g++ make cmake libgl1-mesa-dev \
  libx11-dev libxrandr-dev libxcursor-dev \
  libxi-dev libxinerama-dev libxext-dev \
  libasound2-dev libpulse-dev
```

### raylib 5.5 (Linux — build from source)

```bash
git clone --depth 1 --branch 5.5 https://github.com/raysan5/raylib.git /tmp/raylib
cmake -S /tmp/raylib -B /tmp/raylib/build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=OFF -DBUILD_GAMES=OFF
cmake --build /tmp/raylib/build --parallel && sudo cmake --install /tmp/raylib/build
```

`build.sh` handles both steps automatically on Linux.

### Windows — w64devkit (recommended)

1. Install [w64devkit](https://github.com/skeeto/w64devkit) to `C:\raylib\w64devkit` (GCC 15.2.0)
2. Place raylib 5.5 static lib at `C:\raylib\w64devkit\lib\libraylib.a`
3. Place raylib headers at `C:\raylib\w64devkit\include\raylib.h`

**Important**: Do NOT use WinGet GCC 16.1.0 — its C++ headers are broken with POSIX UCRT.

### Windows — MSYS2 / MINGW64

```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make mingw-w64-x86_64-raylib make
```

## Build Commands

### All Targets

```bash
make -j$(nproc)    # Linux
mingw32-make -j8  # Windows
```

Builds: `Angels95`, `AngelServ`, `OzPack`

### Individual Targets

| Target | Command | Dependencies |
|---|---|---|
| Game client | `make OTENGINE` | raylib 5.5 |
| Dedicated server | `make AngelServ` | None (standalone) |
| Asset packer | `make ozpack` | None (standalone) |
| Level editor | `make -C AngelEd` | raylib 5.5 + Win32 (**Windows only** — the Makefile errors out on Linux) |

### Windows Build Scripts

```powershell
.\build-native-win.ps1              # w64devkit — full System/ release
.\build-native-win.ps1 -SkipData    # skip asset packaging
.\build-native-win.ps1 -SkipClean   # skip make clean (incremental)
.\build-native-win.ps1 -Fast        # incremental; implies -SkipClean
.\build-native-win.ps1 -Debug       # MODE=debug (-O0 -g)

.\build.ps1                         # MSYS2 — full System/ release
.\build.ps1 -SkipData               # skip asset packaging
.\build.ps1 -Fast -Debug            # incremental debug build
```

Both scripts:
1. Build all 5 targets (Angels95, AngelServ, AngelMaster, OzPack, AngelEd)
2. Assemble `System/` with EXEs, INI files, run scripts
3. Package assets via `build-data.ps1`

### Build speed / iteration

- `MODE=debug` swaps `-O3` for `-O0 -g` (much faster compiles + symbols); default is `release`.
- Plain `make` is already incremental (objects in `build/`); the scripts run
  `make clean` unless you pass `-SkipClean`.
- **ccache** is auto-detected on PATH (prefixes `g++`/`gcc`); disable with
  `CCACHE=`, force with `CCACHE=<path>`.
- **LLD** is auto-enabled as linker (`-fuse-ld=lld`) when `ld.lld`/`lld` is on
  PATH; override with `LDEXTRA=` to disable.
- `make help` prints the resolved `MODE`/`OPTFLAGS`/ccache/linker.

### Asset Packaging

```powershell
.\build-data.ps1        # packages all GameData assets into System/Data/
```

Uses `OzPack.exe` to create `.oz*` containers from `GameData/` subdirectories.
Packaged `.dds`/image/audio assets decode **from memory** (b71) — the loader
passes the extension with its leading dot to
`LoadImageFromMemory`/`LoadWaveFromMemory`/`LoadMusicStreamFromMemory`/
`LoadFontFromMemory` — so packaged skyboxes/world textures no longer need the
`System/Cache` temp-file workaround.

## Testing

```bash
make test                # runs ALL test suites, then FAILS if any suite failed
make test_parser         # LightningScriptParser tests
make test_context        # LightningScriptContext tests
make test_registry       # LightningEntityRegistry tests
make test_entity_manager # LightningEntityManager lifecycle + ammo/reload/melee tests
make test_pawn_system    # OzPawnSystem CRUD + projectile collision tests
make test_ozone_parser   # OZONE parser tests
make test_join_uri       # angels95:// URI parsing tests
make test_master         # master heartbeat/list codec tests
make test_network        # Network packet serialization + find_free_port tests
make test_game_state     # GameState projectile simulation + AMMO pickup tests
make test_ozanim         # .ozanim vertex-keyframe format tests
make test_ozls_writer    # OzlsWriter patch-in-place tests
make test_autoconvex     # voxeliser / SAT collision tests
make test_csg            # CSG carve-budget tests
make test_brushmesh      # cylinder/pyramid winding + plane tests
make test_surface        # surface flags, fog curve parity, light uniform parity
make test_gametype       # GameTypeInfo ruleset tests
make test_editorbus      # editor event vocabulary tests
make test_assetscope     # editor asset-scope tree tests
make test_events         # EventBus / TriggerSystem / ObjectiveSystem tests
make test_game           # game-hierarchy def loading tests
make test_gizmomath      # editor gizmo math tests
make test_keybinds       # keybind table tests
make test_menu           # menu layout metrics tests
```

`make test` also runs `worldcheck` over every shipped world (with `--headroom-all`
and `--asset-refs`) and `tools/check_editor_objects.sh`, so a broken map or a
compiled-but-never-linked editor object fails the build. A suite that crashes
counts as a failure: the exit code is authoritative, not a log grep.

Tests are standalone `.test.cpp` files compiled directly into executables (no test framework). Most use no raylib dependency (`SERVER_CXX` compiler with `-DOMEGA_TEST_ENV`). `test_entity_manager` and `test_pawn_system` link raylib for Vector3/BoundingBox types.

## Outputs

| File | Location | Description |
|---|---|---|
| `Angels95.exe` | `System/` | Game client |
| `AngelServ.exe` | `System/` | Dedicated server |
| `AngelEd.exe` | `System/` | Level editor (Win32) |
| `OzPack.exe` | `System/` | Asset packer CLI |

## CI

GitHub Actions workflow (`.github/workflows/ci.yml`):

### Linux Job
1. Build raylib from source, install to `/usr/local`
2. `make AngelServ OTENGINE ozpack AngelMaster`
3. Smoke test: `timeout 3 ./AngelServ --dir GameData --port 27015 --http-port 8080`
4. Upload binaries as artifacts

### Windows Job (MSYS2)
1. Install `mingw-w64-x86_64-{gcc,make,raylib}`
2. Build all 5 targets (incl. `AngelEd`)
3. Assemble `System/` release
4. Run `build-data.ps1`
5. Upload artifact

Tags matching `b*` trigger a GitHub Release. The release contains:
- `System-<tag>.zip` — full Windows bundle (client, editor, servers, raylib/GLFW DLLs, the packaged assets in `System/Data`, INI templates, run scripts; no loose GameData tree - the client reads the packages)
- **Per-binary assets** (directly downloadable, no build needed):
  - `AngelServ-<tag>-{linux-x86_64,windows-x86_64.exe}` — dedicated server
  - `AngelMaster-<tag>-{linux-x86_64,windows-x86_64.exe}` — master server
  - `OzPack-<tag>-{linux-x86_64,windows-x86_64.exe}` — asset packer
  - `Angels95-<tag>-{linux-x86_64,windows-x86_64.exe}` — client (Windows copy needs the zip's DLLs)
  - `AngelEd-<tag>-windows-x86_64.exe` — editor (**Windows only**, Win32 native panels)

`AngelMaster`/`OzPack` are self-contained; `Angels95`/`AngelEd` need the `System-<tag>.zip` contents (raylib/GLFW DLLs + `System/Data` packages). `AngelServ` is self-contained but reads loose world files, so point `--dir` at a `GameData/` checkout.

## Running

### Client

```bash
.\System\Angels95.exe                 # from repo root
.\System\run.bat                      # via batch script (sets cwd)
Angels95.exe --world MyWorldName      # load a specific world
```

### Server

```bash
./AngelServ --port 27015 --http-port 8080 --dir GameData
```

| Flag | Default | Description |
|---|---|---|
| `--port` | 27015 | UDP game server port |
| `--http-port` | 8080 | HTTP map API port |
| `--dir` | GameData | Path to game data directory |
| `--bind` | all | Interface to bind — IPv4, hostname, or `0.0.0.0` on a VPS |
| `--master host[:port]` | – | Master UDP heartbeat target, repeatable |
| `--master-http URL` | – | Master HTTP(S) heartbeat URL, repeatable |
| `--public-ip` | – | Public IP to announce when behind NAT |
| `--auth-token` | – | HTTP Bearer gate (env `OZ_AUTH_TOKEN`) |
| `--admin-token` | – | Enables COMMAND list/say/kick (env `OZ_ADMIN_TOKEN`) |

Server behavior is controlled entirely by CLI flags (env overrides them);
`System/OzServer.ini` is a template written by the build scripts, never read at
runtime, except `AngelServ` now also accepts an optional
`System/OzServer.ini` (`[Server]`/`[Auth]`/`[MasterServers]`, CLI + env override).

### VPS deployment

`System/angels95-serv.service` and `System/angels95-master.service` ship
**systemd units** for `AngelServ` and `AngelMaster` — copy them into
`/etc/systemd/system/`, then:

```bash
systemctl daemon-reload
systemctl enable --now angels95-serv angels95-master
```

Run both daemons with `--bind 0.0.0.0` so they listen on the public interface.

### Automatic server deployment (GitHub Actions)

Pushing a `b*` tag deploys the servers, after the Linux CI job (`make test`,
worldchecks, the GLIBC 2.39 gate) passes. The `deploy` job in
`.github/workflows/ci.yml`:

1. downloads the `AngelServ`/`AngelMaster` binaries built by that run,
2. assembles a self-contained bundle (the binaries, the unit files,
   `deploy-remote.sh`, and the whole `GameData/` tree minus `Saves/` and
   `Screenshots/`, plus a `System/BUILD_VERSION` stamp),
3. smoke-tests the binaries on the runner,
4. rsyncs `GameData/` with `--delete` (a world removed upstream disappears from
   the server's world list) and `System/` without `--delete` (the host's
   `OzServer.ini`, logs and cache are never touched),
5. runs `System/deploy-remote.sh` over SSH, which validates the binaries,
   renders/installs the unit files present in `System/`, restarts them and fails
   with journal output if one does not become active.

Deploys are gated on the `production` GitHub environment, so the repository can
require a reviewer before a rollout. By default only `AngelServ` is shipped. The
in-repo `AngelMaster` is opt-in: when shipped it listens on **`27910/udp` +
`27960/tcp`** (see `tools/angels95-master.service`), so it can coexist with the
hosted listing on the protocol defaults `27900/27950`. Set the repository
variable `DEPLOY_MASTER` to `true` only on a host that should run the in-repo
master. A deploy that does not ship `angels95-master.service` automatically runs
`systemctl disable --now angels95-master` if that unit was installed by an
earlier deploy, so switching the flag off also stops a stale/crash-looping
master.

#### One-time VPS setup

```bash
# deploy user + install root
sudo useradd -m -s /bin/bash deploy
sudo mkdir -p /srv/angels95 && sudo chown deploy:deploy /srv/angels95

# the workflow needs rsync on both ends; curl is the HTTPS master uplink
sudo apt-get install -y rsync curl ca-certificates

# the remote script installs the units and restarts the services
echo 'deploy ALL=(root) NOPASSWD:ALL' | sudo tee /etc/sudoers.d/angels95-deploy
sudo chmod 440 /etc/sudoers.d/angels95-deploy
```

The deploy user can write the binaries the units execute as root, so a tighter
sudoers rule than `NOPASSWD:ALL` is not a security boundary. Use a dedicated,
key-only account and restrict it at the SSH/firewall level instead.

The binaries are built on `ubuntu-24.04` and CI verifies they require no symbol
newer than `GLIBC_2.39`, so the VPS must provide at least that (Ubuntu 24.04 or
a newer compatible distro). `deploy-remote.sh` runs `--version` on both binaries
before restarting, so a mismatched host fails the deploy instead of the fleet.

Create `System/OzServer.ini` once with the host's `[Server]`/`[Auth]` tokens —
the deploy never writes that file. Open the ports you actually use:
`27015/udp` + `8080/tcp` for `AngelServ`, `27910/udp` + `27960/tcp` for the
in-repo `AngelMaster` (put an HTTPS reverse proxy in front of it if it is
public). Advertise it explicitly — game servers via a second `[MasterServers]`
entry, clients via `System/Angels95.ini` — as the unit is not on the default
`27900/27950` ports.

#### Repository configuration

Secrets:

| Secret | Meaning |
|---|---|
| `DEPLOY_HOST` | VPS hostname or IP |
| `DEPLOY_USER` | SSH user that owns the install root (e.g. `deploy`) |
| `DEPLOY_SSH_KEY` | private key for that user, no passphrase |
| `DEPLOY_KNOWN_HOSTS` | optional pinned `known_hosts` line; when absent the job falls back to `ssh-keyscan` (TOFU) |

Variables:

| Variable | Default | Meaning |
|---|---|---|
| `DEPLOY_PORT` | `22` | SSH port |
| `DEPLOY_PATH` | `/srv/angels95` | install root; holds `System/` and `GameData/` |
| `DEPLOY_MASTER` | `false` | also ship the in-repo `AngelMaster` on `27910/27960`; opt-in, so it does not collide with the hosted listing on `27900/27950` |

#### Rollback and manual redeploy

Every deploy keeps the previous binaries as `System/AngelServ.prev` and (when
the master is shipped) `System/AngelMaster.prev`:

```bash
cd /srv/angels95/System
cp -f AngelServ.prev AngelServ
sudo systemctl restart angels95-serv
# only when DEPLOY_MASTER is true:
cp -f AngelMaster.prev AngelMaster && sudo systemctl restart angels95-master
```

To redeploy an existing tag without re-tagging, open that tag's **Build**
workflow run (or a failed run) and use **Re-run all jobs** — the `linux` and
`deploy` jobs run again and the deploy is idempotent.


## Wiki deployment

The published documentation is the **GitHub wiki** at
<https://github.com/TribeWarez/Angels-95/wiki>. It is a *separate* git
repository (`Angels-95.wiki.git`), not a GitHub Pages site — there is no
`gh-pages` branch and no `docs/` folder, so pushing `Wiki/` to `master` does not
publish anything by itself.

`Wiki/` is the single source of truth, and `tools/sync-wiki.ps1` republishes it:

```bash
pwsh tools/sync-wiki.ps1            # sync + push
pwsh tools/sync-wiki.ps1 -DryRun    # build the commit in a temp clone, no push
```

It clones the wiki, **replaces** every page (so a page deleted here disappears
there — that is how the dead `World-Format-WDL` was removed), rewrites
`../GameData/...` images to `raw.githubusercontent.com` URLs, turns `Page.md`
links into the bare `Page` form the wiki resolves, and regenerates `_Sidebar.md`
from this folder's `Home.md` Contents table plus `_Footer.md`.

**Automation:** `.github/workflows/wiki-sync.yml` runs it on any push to
`master` that touches `Wiki/**` (and on manual dispatch). Because the wiki is a
sibling repository, `GITHUB_TOKEN` may not have write access to it: add a
fine-grained PAT with **Contents: read and write** as the `WIKI_TOKEN` secret.
The workflow falls back to `GITHUB_TOKEN` when the repository grants it wiki
access, and fails loudly rather than leaving the docs silently stale.
