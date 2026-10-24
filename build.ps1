# Angels95 Windows Build Script (PowerShell)
# For MSYS2/MINGW64 or similar MinGW-w64 environments.
# For native w64devkit builds, use build-native-win.ps1 instead.
# Requires: MinGW-w64 (g++), CMake, Git
# Run from the repo root.

param(
    [switch]$SkipData,
    [switch]$SkipClean,
    [switch]$Fast,      # incremental rebuild: keep build/ objects (implies -SkipClean)
    [switch]$Debug,     # MODE=debug: -O0 -g for fast compiles + symbols
    [switch]$ResetConfig # discard System/*.ini and restore the shipped templates
)

# -Fast is sugar for -SkipClean (iterate without recompiling the world).
if ($Fast) { $SkipClean = $true }

# Forward MODE to every make invocation.
$MakeMode = @()
if ($Debug) { $MakeMode += "MODE=debug" }

$RAYLIB_VERSION = "5.5"
$RAYLIB_DIR = "$env:USERPROFILE\raylib-$RAYLIB_VERSION"
$OUT_DIR       = "$PSScriptRoot\System"

function Write-Step { Write-Host "==> $args" -ForegroundColor Cyan }
function Fail { Write-Host "ERROR: $args" -ForegroundColor Red; exit 1 }

# --- Prerequisites ---
Write-Step "Checking prerequisites..."

$haveGpp = Get-Command g++ -ErrorAction SilentlyContinue
$haveWindres = Get-Command windres -ErrorAction SilentlyContinue
$haveMake = Get-Command mingw32-make -ErrorAction SilentlyContinue
$haveCmake = Get-Command cmake -ErrorAction SilentlyContinue
$haveGit = Get-Command git -ErrorAction SilentlyContinue

if (-not $haveGpp) {
    Fail "g++ not found in PATH. Install MinGW-w64 (https://winlibs.com) and add to PATH."
}
if (-not $haveWindres) {
    Fail "windres not found (required for .ico embedding). Install GNU Binutils."
}
if (-not $haveMake) {
    Fail "mingw32-make not found. Install MinGW-w64 with make."
}
if (-not $haveCmake) {
    Fail "cmake not found. Install from https://cmake.org/download/"
}
if (-not $haveGit) {
    Fail "git not found. Install from https://git-scm.com/downloads"
}

Write-Step "g++: $($haveGpp.Source)"
Write-Step "windres: $($haveWindres.Source)"
Write-Step "cmake: $($haveCmake.Source)"
Write-Step "git: $($haveGit.Source)"

# --- raylib ---
$raylibInstalled = (Test-Path "$RAYLIB_DIR\raylib\include\raylib.h") -or
                   (Test-Path "C:\raylib\raylib\include\raylib.h")

if (-not $raylibInstalled) {
    Write-Step "raylib not found. Cloning and building v$RAYLIB_VERSION..."
    Push-Location $env:TEMP
    git clone --depth 1 --branch "$RAYLIB_VERSION" https://github.com/raysan5/raylib.git raylib-src
    New-Item -ItemType Directory -Force -Path raylib-src\build | Out-Null
    Set-Location raylib-src\build
    cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF `
             -DCMAKE_INSTALL_PREFIX="$RAYLIB_DIR"
    if ($LASTEXITCODE -ne 0) { Fail "CMake configuration failed" }
    cmake --build . --parallel
    if ($LASTEXITCODE -ne 0) { Fail "Build failed" }
    cmake --install .
    Pop-Location
    Remove-Item "$env:TEMP\raylib-src" -Recurse -Force -ErrorAction SilentlyContinue
    Write-Step "raylib installed to $RAYLIB_DIR"
} else {
    Write-Step "raylib found."
}

# Ensure raylib DLL is on PATH for runtime
$raylibBin = "$RAYLIB_DIR\bin"
if (Test-Path $raylibBin) {
    $currentPath = [Environment]::GetEnvironmentVariable("PATH", "User")
    if ($currentPath -notlike "*$raylibBin*") {
        [Environment]::SetEnvironmentVariable("PATH", "$currentPath;$raylibBin", "User")
        Write-Step "Added $raylibBin to user PATH (reopen shell to take effect)"
    }
}

# --- Clean previous build artifacts from root ---
if (-not $SkipClean) {
    Write-Step "Cleaning previous build..."
    Push-Location $PSScriptRoot
    & mingw32-make clean 2>&1 | Out-Null
    Pop-Location
}

Write-Step "Build mode: $(if ($Debug) { 'debug (-O0 -g)' } else { 'release (-O3)' })$(if ($SkipClean) { ', incremental' } else { ', clean' })"

# --- 1. Build OTENGINE (game client) ---
Write-Step "Building Angels95 (OTENGINE)..."
Push-Location $PSScriptRoot
$env:Path += ";$RAYLIB_DIR\lib"
& mingw32-make -j $env:NUMBER_OF_PROCESSORS @MakeMode OTENGINE
if ($LASTEXITCODE -ne 0) { Fail "Game build failed" }

# --- 2. Build AngelServ (dedicated server) ---
Write-Step "Building AngelServ..."
& mingw32-make -j $env:NUMBER_OF_PROCESSORS @MakeMode AngelServ
if ($LASTEXITCODE -ne 0) { Fail "Server build failed" }

# --- 3. Build OzPack (asset packer) ---
Write-Step "Building OzPack..."
& mingw32-make @MakeMode ozpack
if ($LASTEXITCODE -ne 0) { Fail "OzPack build failed" }

# --- 3b. Build AngelMaster (master server) ---
Write-Step "Building AngelMaster..."
& mingw32-make @MakeMode AngelMaster
if ($LASTEXITCODE -ne 0) { Fail "AngelMaster build failed" }
Pop-Location

# --- 4. Build AngelEd (level editor) via Makefile ---
Write-Step "Building AngelEd..."
Push-Location "$PSScriptRoot\AngelEd"
& mingw32-make -j $env:NUMBER_OF_PROCESSORS @MakeMode AngelEd 2>&1
if ($LASTEXITCODE -ne 0) { Fail "AngelEd build failed" }
Pop-Location
Write-Step "AngelEd.exe built."

# --- 5. Assemble System/ directory ---
Write-Step "Assembling System/ release..."
if (-not (Test-Path $OUT_DIR)) { New-Item -ItemType Directory -Force -Path $OUT_DIR | Out-Null }
if (-not (Test-Path "$OUT_DIR\Data")) { New-Item -ItemType Directory -Force -Path "$OUT_DIR\Data" | Out-Null }

# Move EXEs (fail if any missing)
if (-not (Test-Path "$PSScriptRoot\Angels95.exe"))  { Fail "Angels95.exe not found" }
if (-not (Test-Path "$PSScriptRoot\AngelServ.exe")) { Fail "AngelServ.exe not found" }
if (-not (Test-Path "$PSScriptRoot\OzPack.exe"))    { Fail "OzPack.exe not found" }
if (-not (Test-Path "$PSScriptRoot\AngelMaster.exe")) { Fail "AngelMaster.exe not found" }
if (-not (Test-Path "$PSScriptRoot\AngelEd\AngelEd.exe")) { Fail "AngelEd.exe not found" }
Move-Item -Force "$PSScriptRoot\Angels95.exe"  "$OUT_DIR\Angels95.exe"
Move-Item -Force "$PSScriptRoot\AngelServ.exe" "$OUT_DIR\AngelServ.exe"
Move-Item -Force "$PSScriptRoot\OzPack.exe"    "$OUT_DIR\OzPack.exe"
Move-Item -Force "$PSScriptRoot\AngelMaster.exe" "$OUT_DIR\AngelMaster.exe"
Move-Item -Force "$PSScriptRoot\AngelEd\AngelEd.exe" "$OUT_DIR\AngelEd.exe"

# Copy/create INI files.
# These are USER data - player settings, the editor's window layout and the
# server's auth tokens - not build output, so a template is written only when
# the file is missing. -ResetConfig deletes them first to restore defaults.
if ($ResetConfig) {
    foreach ($cfg in @('Angels95.ini', 'AngelEd.ini', 'OzServer.ini')) {
        Remove-Item "$OUT_DIR\$cfg" -Force -ErrorAction SilentlyContinue
    }
}
if (-not (Test-Path "$OUT_DIR\Angels95.ini")) {
    @"
; Angels95 client settings - grouped by concern. The client fills in any key it
; does not find here and rewrites this file on exit.
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

[MasterServers]
Master=http://127.0.0.1:27950
"@ | Set-Content "$OUT_DIR\Angels95.ini" -Encoding UTF8
}

if (-not (Test-Path "$OUT_DIR\AngelEd.ini")) {
    @"
[Editor]
GridSize=1.0
SnapToGrid=1
ShowGrid=1

[Video]
Width=1600
Height=900
"@ | Set-Content "$OUT_DIR\AngelEd.ini" -Encoding UTF8
}

 # Create OzServer.ini (AngelServ reads [Server]/[Auth]/[MasterServers])
if (-not (Test-Path "$OUT_DIR\OzServer.ini")) {
    @"
; AngelServ config. CLI flags and env vars override these values.
[Server]
port=27015
http-port=8080
dir=GameData
; bind an interface/address (0.0.0.0 or blank = all interfaces)
bind=0.0.0.0
server-name=Angels95 Server

[Auth]
; HTTP API Bearer token (also env OZ_AUTH_TOKEN). Blank = open (dev only)
auth-token=
; Enables admin COMMANDs list/say/kick (also env OZ_ADMIN_TOKEN)
admin-token=
; Public IP to announce when behind NAT (masters can override)
public-ip=

[MasterServers]
; UDP heartbeat targets (host[:port], default 27900) and/or http:// URLs
; Master=your-master.example.com:27900
"@ | Set-Content "$OUT_DIR\OzServer.ini" -Encoding UTF8
}

# Create run scripts
@"
@echo off
cd /d "%~dp0.."
start "" "%~dp0Angels95.exe"
"@ | Set-Content "$OUT_DIR\run.bat" -Encoding ASCII

@"
Set-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
& "$PSScriptRoot\Angels95.exe"
"@ | Set-Content "$OUT_DIR\run.ps1" -Encoding UTF8

# VPS systemd unit. The docs (and the release notes) point at
# System\angels95-serv.service, so the source template in tools/ is copied next
# to the executables it references.
Copy-Item "$PSScriptRoot\tools\angels95-serv.service" "$OUT_DIR\angels95-serv.service" -Force -ErrorAction SilentlyContinue

# --- 6. Package assets (optional) ---
if (-not $SkipData) {
    Write-Step "Packaging assets..."
    Push-Location $PSScriptRoot
    if (Test-Path ".\build-data.ps1") {
        & .\build-data.ps1 2>&1
        if ($LASTEXITCODE -ne 0) { Write-Host "WARNING: Asset packaging failed" -ForegroundColor Yellow }
    } else {
        Write-Host "WARNING: build-data.ps1 not found, skipping asset packaging" -ForegroundColor Yellow
    }
    Pop-Location
} else {
    Write-Step "Skipping asset packaging (-SkipData)"
}

# --- 7. Verify outputs ---
Write-Step "Verifying outputs..."
$missing = @()
foreach ($exe in @("Angels95.exe", "AngelServ.exe", "AngelEd.exe", "OzPack.exe", "AngelMaster.exe")) {
    if (-not (Test-Path "$OUT_DIR\$exe")) { $missing += $exe }
}
if ($missing.Count -gt 0) {
    Fail "Missing EXEs: $($missing -join ', ')"
}

$dataFiles = Get-ChildItem "$OUT_DIR\Data" -Filter "*.oz*" -ErrorAction SilentlyContinue
if ($dataFiles.Count -eq 0 -and -not $SkipData) {
    Write-Host "WARNING: No .oz* packages found in System/Data" -ForegroundColor Yellow
}

Write-Step "Build complete."
Write-Host "System/ release in $OUT_DIR" -ForegroundColor Green
Write-Host "  Angels95.exe  - Game client" -ForegroundColor Green
Write-Host "  AngelServ.exe - Dedicated server" -ForegroundColor Green
Write-Host "  AngelMaster.exe - Master server (internet discovery)" -ForegroundColor Green
Write-Host "  AngelEd.exe - Level editor" -ForegroundColor Green
Write-Host "  OzPack.exe    - Asset packer" -ForegroundColor Green
Write-Host "  Data/*.oz*    - Packaged assets ($($dataFiles.Count) files)" -ForegroundColor Green
