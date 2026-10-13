# build-native-win.ps1 — Native Windows build using C:\raylib w64devkit + raylib
# Run from repo root: powershell -File build-native-win.ps1

param(
    [switch]$SkipData,
    [switch]$SkipClean,
    [switch]$Fast,      # incremental rebuild: keep build/ objects (implies -SkipClean)
    [switch]$Debug      # MODE=debug: -O0 -g for fast compiles + symbols
)

# -Fast is sugar for -SkipClean (iterate without recompiling the world).
if ($Fast) { $SkipClean = $true }

# Forward MODE to every make invocation.
$MakeMode = @()
if ($Debug) { $MakeMode += "MODE=debug" }

$W64DEVKIT = "C:\raylib\w64devkit"
$RAYLIB_SRC = "C:\raylib\raylib"
$OUT_DIR    = "$PSScriptRoot\System"

function Write-Step  { Write-Host "==> $args" -ForegroundColor Cyan }
function Fail { Write-Host "ERROR: $args" -ForegroundColor Red; exit 1 }

# --- PATH ---
$env:Path = "$W64DEVKIT\bin;$env:Path"

# --- Prerequisites ---
Write-Step "Checking prerequisites..."
if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    Fail "g++ not found. Expected in $W64DEVKIT\bin"
}
if (-not (Get-Command windres -ErrorAction SilentlyContinue)) {
    Fail "windres not found. Expected in $W64DEVKIT\bin"
}
if (-not (Get-Command mingw32-make -ErrorAction SilentlyContinue)) {
    Fail "mingw32-make not found. Expected in $W64DEVKIT\bin"
}
Write-Step "g++ $(& g++ --version | Select-Object -First 1)"
Write-Step "windres $(& windres --version | Select-Object -First 1)"

# --- raylib headers ---
Write-Step "Ensuring raylib headers are available..."
if (-not (Test-Path "$W64DEVKIT\include\raymath.h")) {
    if (Test-Path "$RAYLIB_SRC\src\raymath.h") {
        Copy-Item "$RAYLIB_SRC\src\raymath.h" "$W64DEVKIT\include\raymath.h"
        Copy-Item "$RAYLIB_SRC\src\rcamera.h"  "$W64DEVKIT\include\rcamera.h" -ErrorAction SilentlyContinue
        Copy-Item "$RAYLIB_SRC\src\rgestures.h" "$W64DEVKIT\include\rgestures.h" -ErrorAction SilentlyContinue
        Write-Step "Copied missing raylib headers to $W64DEVKIT\include"
    } else {
        Fail "raylib source not found at $RAYLIB_SRC\src\raylib.h"
    }
}

# --- Clean previous artifacts ---
if (-not $SkipClean) {
    Write-Step "Cleaning previous build..."
    & mingw32-make clean 2>&1 | Out-Null          # main build/
    Push-Location "$PSScriptRoot\AngelEd"
    & mingw32-make clean 2>&1 | Out-Null          # editor build-ed/
    Pop-Location
}

Write-Step "Build mode: $(if ($Debug) { 'debug (-O0 -g)' } else { 'release (-O3)' })$(if ($SkipClean) { ', incremental' } else { ', clean' })"

# --- 1. Build OTENGINE (game client) ---
Write-Step "Building Angels95 (OTENGINE)..."
Push-Location $PSScriptRoot
& mingw32-make -j $env:NUMBER_OF_PROCESSORS @MakeMode OTENGINE 2>&1
if ($LASTEXITCODE -ne 0) { Fail "OTENGINE build failed" }
Write-Step "Angels95.exe built."

# --- 2. Build AngelServ (dedicated server) ---
Write-Step "Building AngelServ..."
& mingw32-make @MakeMode AngelServ 2>&1
if ($LASTEXITCODE -ne 0) { Fail "AngelServ build failed" }
Write-Step "AngelServ.exe built."

# --- 3. Build OzPack (asset packer) ---
Write-Step "Building OzPack..."
& mingw32-make @MakeMode ozpack 2>&1
if ($LASTEXITCODE -ne 0) { Fail "OzPack build failed" }
Write-Step "OzPack.exe built."

# --- 3b. Build AngelMaster (master server) ---
Write-Step "Building AngelMaster..."
& mingw32-make @MakeMode AngelMaster 2>&1
if ($LASTEXITCODE -ne 0) { Fail "AngelMaster build failed" }
Write-Step "AngelMaster.exe built."
Pop-Location

# --- 4. Build AngelEd (level editor) via Makefile ---
Write-Step "Building AngelEd..."
Push-Location "$PSScriptRoot\AngelEd"
& mingw32-make -j $env:NUMBER_OF_PROCESSORS @MakeMode AngelEd 2>&1
if ($LASTEXITCODE -ne 0) { Fail "AngelEd build failed" }
Pop-Location
Write-Step "AngelEd.exe built."

# --- 5. Assemble System/ release ---
Write-Step "Assembling System/ release..."
# Ensure clean system directory structure
if (Test-Path $OUT_DIR) { Remove-Item -Recurse -Force "$OUT_DIR\*" -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path $OUT_DIR | Out-Null
New-Item -ItemType Directory -Force -Path "$OUT_DIR\Data" | Out-Null
New-Item -ItemType Directory -Force -Path "$OUT_DIR\Cache" | Out-Null

# Move EXEs (they build to repo root or AngelEd/ subdir)
if (Test-Path "$PSScriptRoot\Angels95.exe")  { Move-Item -Force "$PSScriptRoot\Angels95.exe"  "$OUT_DIR\Angels95.exe" }  else { Fail "Angels95.exe not found" }
if (Test-Path "$PSScriptRoot\AngelServ.exe") { Move-Item -Force "$PSScriptRoot\AngelServ.exe" "$OUT_DIR\AngelServ.exe" } else { Fail "AngelServ.exe not found" }
if (Test-Path "$PSScriptRoot\OzPack.exe")    { Move-Item -Force "$PSScriptRoot\OzPack.exe"    "$OUT_DIR\OzPack.exe" }    else { Fail "OzPack.exe not found" }
if (Test-Path "$PSScriptRoot\AngelMaster.exe") { Move-Item -Force "$PSScriptRoot\AngelMaster.exe" "$OUT_DIR\AngelMaster.exe" } else { Fail "AngelMaster.exe not found" }
if (Test-Path "$PSScriptRoot\AngelEd\AngelEd.exe") { Move-Item -Force "$PSScriptRoot\AngelEd\AngelEd.exe" "$OUT_DIR\AngelEd.exe" } else { Fail "AngelEd.exe not found" }

# Copy raylib DLL if present
$raylibDll = "$W64DEVKIT\bin\libraylib.dll"
if (Test-Path $raylibDll) {
    Copy-Item -Force $raylibDll "$OUT_DIR\libraylib.dll"
}

# Always recreate INI files
@"
[Video]
Width=1280
Height=720
Fullscreen=0
VSync=1

[Settings]
audio_volume=100.000000
mute=False

[Game]
ServerIP=127.0.0.1
ServerPort=27015

[MasterServers]
Master=http://127.0.0.1:27950
"@ | Set-Content "$OUT_DIR\Angels95.ini" -Encoding UTF8

@"
[Editor]
GridSize=1.0
SnapToGrid=1
ShowGrid=1

[Video]
Width=1600
Height=900
"@ | Set-Content "$OUT_DIR\AngelEd.ini" -Encoding UTF8

 # Create OzServer.ini (AngelServ reads [Server]/[Auth]/[MasterServers])
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
    & .\build-data.ps1 2>&1
    if ($LASTEXITCODE -ne 0) { Write-Host "WARNING: Asset packaging failed" -ForegroundColor Yellow }
    Pop-Location
} else {
    Write-Step "Skipping asset packaging (-SkipData)"
}

# After packaging, copy any loose assets needed at runtime
if (Test-Path "$PSScriptRoot\GameData\Shaders") {
    $shaderDest = "$OUT_DIR\Shaders"
    New-Item -ItemType Directory -Force -Path $shaderDest | Out-Null
    Copy-Item -Recurse -Force "$PSScriptRoot\GameData\Shaders\*" $shaderDest -ErrorAction SilentlyContinue
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
Write-Host "  libraylib.dll - Raylib runtime (if available)" -ForegroundColor Green
Write-Host "  Data/*.oz*    - Packaged assets ($($dataFiles.Count) files)" -ForegroundColor Green
Write-Host "  Cache/        - Temporary runtime cache" -ForegroundColor Green
