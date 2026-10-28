#Requires -Version 5.1
<#
.SYNOPSIS
    Removes the junctions installed by install.ps1. It never follows a junction,
    so the repository contents are untouched.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$extRoot = if ($env:VSCODE_EXTENSIONS_DIR) { $env:VSCODE_EXTENSIONS_DIR } else { Join-Path $env:USERPROFILE '.vscode\extensions' }
$prefixes = @('tribewarez.angels95-ozone', 'tribewarez.angels95-ozls')

if (-not (Test-Path $extRoot)) {
    Write-Host "No extensions directory at $extRoot"
    return
}

Get-ChildItem $extRoot -Force -Directory -ErrorAction SilentlyContinue |
    Where-Object {
        $name = $_.Name.ToLowerInvariant()
        ($prefixes | Where-Object { $name.StartsWith($_) }).Count -gt 0
    } |
    ForEach-Object {
        $isLink = (($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
        if ($isLink) {
            Write-Host "Removing junction $($_.FullName)"
            & cmd /c rmdir "`"$($_.FullName)`"" | Out-Null
        } else {
            Write-Warning "Skipping a real directory (not installed by this script): $($_.FullName)"
        }
    }

Write-Host 'Done. Reload VS Code.'
