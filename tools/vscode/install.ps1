#Requires -Version 5.1
<#
.SYNOPSIS
    Junction-links the two Angels95 VS Code extensions into the user's
    extensions directory. No npm or vsce needed; edits in the repo take effect
    after "Developer: Reload Window".

.PARAMETER Only
    Install just one extension.

.PARAMETER Force
    Replace a real directory that already occupies the target name. Only use
    this if you are sure it is not a hand-installed copy you want to keep.
#>
[CmdletBinding()]
param(
    [ValidateSet('all', 'ozone', 'ozls')]
    [string]$Only = 'all',

    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$extRoot = if ($env:VSCODE_EXTENSIONS_DIR) { $env:VSCODE_EXTENSIONS_DIR } else { Join-Path $env:USERPROFILE '.vscode\extensions' }

if (-not (Test-Path $extRoot)) {
    New-Item -ItemType Directory -Path $extRoot -Force | Out-Null
}

$extensions = @(
    @{ Key = 'ozone'; Source = (Join-Path $here 'angels95-ozone') },
    @{ Key = 'ozls';  Source = (Join-Path $here 'angels95-ozls') }
)

function Get-ExtensionId {
    param([string]$Source)
    $pkg = Get-Content (Join-Path $Source 'package.json') -Raw | ConvertFrom-Json
    $name = "$($pkg.publisher).$($pkg.name)".ToLowerInvariant()
    return "$name-$($pkg.version)"
}

function Test-ReparsePoint {
    param([System.IO.FileSystemInfo]$Item)
    return (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
}

foreach ($ext in $extensions) {
    if ($Only -ne 'all' -and $Only -ne $ext.Key) { continue }

    if (-not (Test-Path (Join-Path $ext.Source 'package.json'))) {
        throw "Extension source not found: $($ext.Source)"
    }

    $id = Get-ExtensionId -Source $ext.Source
    $prefix = ($id -replace '-\d+\.\d+\.\d+$', '')
    $target = Join-Path $extRoot $id

    # Remove a previous junction for this extension (junctions must be removed
    # with rmdir, never Remove-Item, or the repo contents are deleted).
    $blocked = $false
    foreach ($existing in Get-ChildItem $extRoot -Force -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name.ToLowerInvariant().StartsWith($prefix) }) {
        if (Test-ReparsePoint $existing) {
            Write-Host "Removing old junction $($existing.FullName)"
            & cmd /c rmdir "`"$($existing.FullName)`"" | Out-Null
        } elseif ($Force) {
            Write-Host "Removing real directory $($existing.FullName) (-Force)"
            Remove-Item $existing.FullName -Recurse -Force
        } else {
            Write-Warning "A real extension directory already exists: $($existing.FullName). Re-run with -Force to replace it."
            $blocked = $true
        }
    }
    if ($blocked) { continue }

    Write-Host "Linking $target -> $($ext.Source)"
    & cmd /c mklink /J "`"$target`"" "`"$($ext.Source)`"" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "mklink failed for $target"
    }
}

Write-Host ''
Write-Host 'Installed. In VS Code run "Developer: Reload Window".'
Write-Host "Extensions directory: $extRoot"
