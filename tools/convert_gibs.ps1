# convert_gibs.ps1 - convert the gore decal art in GameData/Tex/Global/Gibs
# from JPG to PNG.
#
# raylib's stb_image decoder rejects the shipped Gibs JPEGs (LoadImage returns
# 0x0) while the same pixels load fine as PNG. This script re-encodes each
# *.jpg beside it as a lossless *.png so CombatFX can load them. Run it after
# dropping new JPG gore art in that folder.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools/convert_gibs.ps1

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root 'GameData\Tex\Global\Gibs'

if (-not (Test-Path $dir)) {
    Write-Host "No Gibs folder at $dir - nothing to do."
    exit 0
}

$count = 0
Get-ChildItem -Path $dir -Filter '*.jpg' | ForEach-Object {
    $out = [System.IO.Path]::ChangeExtension($_.FullName, '.png')
    if (Test-Path $out) { Write-Host "  (exists) $(Split-Path $out -Leaf)"; return }
    $img = [System.Drawing.Image]::FromFile($_.FullName)
    $img.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
    $img.Dispose()
    Write-Host "  $($_.Name) -> $(Split-Path $out -Leaf)"
    $count++
}
Write-Host "Converted $count JPG(s) to PNG in $dir"
