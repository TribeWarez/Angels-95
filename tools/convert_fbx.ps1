# convert_fbx.ps1 — convert FBX source art to binary glTF (.glb) for the engine,
# then strip skinning/animation so raylib can load it safely.
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
# then deleted (clips are not used by the shipped weapons).
#
# `-NoFlatten` keeps skins/animations for a manual experiment. Nothing in the
# build calls tools/merge_glb_anims.py, so `-NoFlatten` output is unverified
# against raylib; the view-model's reload uses the rigid magazine in
# `<mesh>_AdditionalMeshes.glb` plus procedural motion instead.
#
# Usage (from the repo root):
#   powershell -ExecutionPolicy Bypass -File tools/convert_fbx.ps1
#   powershell ... -File tools/convert_fbx.ps1 -Root "GameData/Tex/Global/Gun/Pistol_01"
#   powershell ... -File tools/convert_fbx.ps1 -NoFlatten
#   powershell ... -File tools/convert_fbx.ps1 -Animated
#
# -Animated is the DEFAULT: for every base model that has sibling
# Animations/<Name>_Fire.glb or reload clips (_Reload/_ReloadStart/_ReloadStep/
# _ReloadEnd), collapse the duplicate skins into one (merge_glb_skins.py) and
# merge the clips (merge_glb_anims.py) instead of flattening. That is how the
# shipped Pistol_01/Rifle_01/SMG_01/Shotgun_01 reload clips are produced; a
# reload authored as several takes is concatenated into one "Reload" clip.
# Pass -NoAnimated to force the old flatten-everything behaviour (it STRIPS the
# reload clips, so only use it for static-only art).

param(
    [string]$Root = "GameData/Tex/Global/Gun",
    [string]$Fbx2gltf = "",
    [switch]$NoFlatten,
    [switch]$Animated,     # accepted for older invocations; default is on
    [switch]$NoAnimated    # opt out: flatten even models with animation GLBs
)

# Animated models are the default: otherwise a plain re-run of this script would
# silently strip the weapon reload clips that -Animated produced.
$Animated = -not $NoAnimated

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

    if ($NoFlatten -and -not $Animated) { return }

    # --- animated models: collapse duplicate skins, then merge clips ----------
    # FBX2glTF writes ONE skin per mesh part even though every part is bound to
    # the same armature. raylib supports a single skin, so instead of stripping
    # the rig (flatten) an ANIMATED model gets its duplicate skin records
    # collapsed into one by tools/merge_glb_skins.py. The script refuses if the
    # skins are not equivalent. Clips are then merged by name (Fire/Reload) and
    # an Idle clip is synthesized from the bind pose.
    #
    # A reload is not always ONE take: the shotgun authors ReloadStep +
    # ReloadEnd (shell in, port closed), and a pump-action would add
    # ReloadStart. The engine plays a single "Reload" clip, so the segments are
    # concatenated in authored order with '+'. A model that already carries a
    # clip of that name keeps it (merge_glb_anims.py skips it), so re-running
    # this script cannot duplicate Fire/Reload.
    # NOTE: this must NOT be spelled $animated. PowerShell variable names are
    # case-insensitive, so it collides with the [switch]$Animated parameter and
    # assigning an array to a switch type aborts the whole animated pass - the
    # script converted every FBX and then threw before merging a single clip.
    $animatedModels = @()
    if ($Animated) {
        $bases = Get-ChildItem -Path $Root -Recurse -File -Filter *.glb -ErrorAction SilentlyContinue |
                 Where-Object { $_.DirectoryName -notlike "*Animations*" }
        foreach ($g in $bases) {
            $animDir = Join-Path $g.DirectoryName 'Animations'
            $fire    = Join-Path $animDir ($g.BaseName + '_Fire.glb')
            $segments = @()
            foreach ($seg in @('_ReloadStart', '_Reload', '_ReloadStep', '_ReloadEnd')) {
                $p = Join-Path $animDir ($g.BaseName + $seg + '.glb')
                if (Test-Path $p) { $segments += (Resolve-Path $p).Path }
            }
            if (-not (Test-Path $fire) -and $segments.Count -eq 0) { continue }
            Write-Host "Animating $($g.Name)"
            & python tools/merge_glb_skins.py $g.FullName
            if ($LASTEXITCODE -ne 0) { Write-Warning "  skin merge failed for $($g.Name)"; continue }
            $mergeArgs = @("tools/merge_glb_anims.py", $g.FullName)
            if (Test-Path $fire) { $mergeArgs += ("Fire=" + (Resolve-Path $fire).Path) }
            if ($segments.Count -gt 0) { $mergeArgs += ("Reload=" + ($segments -join '+')) }
            & python @mergeArgs
            if ($LASTEXITCODE -ne 0) { Write-Warning "  clip merge failed for $($g.Name)"; continue }
            $animatedModels += $g.FullName
        }
    }

    if (-not $NoFlatten) {
        # --- flatten everything that is NOT an animated model ------------------
        # Static props keep the old treatment: strip skins/animations so raylib
        # can never meet a multi-skin model (its handling corrupts memory).
        $glbs = Get-ChildItem -Path $Root -Recurse -File -Filter *.glb -ErrorAction SilentlyContinue |
                Where-Object { $animatedModels -notcontains $_.FullName }
        if ($glbs -and $glbs.Count -gt 0) {
            $paths = @($glbs | ForEach-Object { $_.FullName })
            & python tools/flatten_glb.py @paths
        }
    }

    # Remove animation-only GLBs: their clips now live in the model GLB.
    # The segment names matter: ReloadStep/ReloadEnd are animation-only files the
    # shotgun authors, and an earlier pattern that only listed _Fire/_Reload left
    # them in the package as clip-less look-alikes.
    Get-ChildItem -Path $Root -Recurse -File -Filter *.glb |
        Where-Object { $_.BaseName -match '_(Fire|Reload|ReloadStart|ReloadStep|ReloadEnd|BasePose|EmptyMagazine)$' } |
        Remove-Item -Force -ErrorAction SilentlyContinue
} finally { Pop-Location }
