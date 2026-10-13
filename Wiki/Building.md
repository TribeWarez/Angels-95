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
make test                # runs ALL test suites
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
```

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

`System/angels95-serv.service` ships **systemd units** for `AngelServ` and
`AngelMaster` — copy the unit into `/etc/systemd/system/`, then:

```bash
systemctl daemon-reload
systemctl enable --now angels95-serv
```

Run both daemons with `--bind 0.0.0.0` so they listen on the public interface.
