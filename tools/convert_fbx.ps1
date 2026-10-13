# convert_fbx.ps1 — convert FBX source art to binary glTF (.glb) for the engine,
# then fold any sibling animation GLBs into the base model GLB with named clips.
#
# raylib can load .obj/.glb/.gltf/.iqm but NOT .fbx, so weapon/projectile art is
# authored in Blender and exported to FBX, then converted here.
#
# Uses the standalone Facebook FBX2glTF binary (no install). If it is not found
# in tools/ or on PATH it is downloaded from the v0.9.7 GitHub release.
# FBX2glTF exports each mesh part as a separate skin; raylib supports only one
# skin per model and its multi-skin handling is unreliable (heap corruption /
# crashes), so tools/flatten_glb.py strips the skins and animations from every
# produced .glb, yielding plain static models. Animation-only .glb files are
# then deleted (clips are not used).
#
# Usage (from the repo root):
#   powershell -ExecutionPolicy Bypass -File tools/convert_fbx.ps1
#   powershell ... -File tools/convert_fbx.ps1 -Root "GameData/Tex/Global/Gun/Pistol_01"
#   powershell ... -File tools/convert_fbx.ps1 -NoFlatten

param(
    [string]$Root = "GameData/Tex/Global/Gun",
    [string]$Fbx2gltf = "",
    [switch]$NoFlatten
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $repoRoot) { $repoRoot = (Get-Location).Path }
Push-Location $repoRoot
try {
    # --- locate (or fetch) FBX2glTF ---
    $exe = $null
    if ($Fbx2gltf -and (Test-Path $Fbx2gltf)) { $exe = (Resolve-Path $Fbx2gltf).Path }
    if (-not $exe) {
        foreach ($c in @("tools/FBX2glTF.exe", "FBX2glTF.exe")) {
            if (Test-Path $c) { $exe = (Resolve-Path $c).Path; break }
        }
    }
    if (-not $exe) {
        $cmd = Get-Command FBX2glTF -ErrorAction SilentlyContinue
        if ($cmd) { $exe = $cmd.Source }
    }
    if (-not $exe) {
        Write-Host "FBX2glTF not found; downloading v0.9.7 (windows-x64)..."
        $url = "https://github.com/facebookincubator/FBX2glTF/releases/download/v0.9.7/FBX2glTF-windows-x64.exe"
        New-Item -ItemType Directory -Force -Path "tools" | Out-Null
        Invoke-WebRequest -Uri $url -OutFile "tools/FBX2glTF.exe" -Headers @{ "User-Agent" = "opencode" }
        $exe = (Resolve-Path "tools/FBX2glTF.exe").Path
    }
    Write-Host "Using $exe"

    # --- convert every .fbx under $Root to .glb beside it ---
    $files = Get-ChildItem -Path $Root -Recurse -File -Filter *.fbx -ErrorAction SilentlyContinue
    if (-not $files -or $files.Count -eq 0) { Write-Host "No .fbx files under $Root"; return }

    $ok = 0; $fail = 0
    foreach ($f in $files) {
        $out = [System.IO.Path]::Combine($f.DirectoryName, [System.IO.Path]::GetFileNameWithoutExtension($f.Name))
        $rel = $f.FullName.Substring($repoRoot.Length).TrimStart('\', '/')
        Write-Host "Converting $rel"
        & $exe -b -i $f.FullName -o $out 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Warning "  FAILED: $($f.Name)"; $fail++ }
        else { Write-Host "  -> $out.glb"; $ok++ }
    }
    Write-Host ""
    Write-Host "Converted: $ok ok, $fail failed."

    if ($NoFlatten) { return }

    # --- flatten every .glb: strip skins + animations -------------------------
    # raylib's multi-skin glTF handling corrupts memory (one skin per mesh part
    # in the FBX exports), so the models are reduced to plain static meshes.
    $glbs = Get-ChildItem -Path $Root -Recurse -File -Filter *.glb -ErrorAction SilentlyContinue
    if ($glbs -and $glbs.Count -gt 0) {
        $paths = @($glbs | ForEach-Object { $_.FullName })
        & python tools/flatten_glb.py @paths
    }

    # Remove animation-only GLBs now that clips are unused.
    Get-ChildItem -Path $Root -Recurse -File -Filter *.glb |
        Where-Object { $_.BaseName -match '_(Fire|Reload|BasePose|EmptyMagazine)$' } |
        Remove-Item -Force -ErrorAction SilentlyContinue
} finally { Pop-Location }
