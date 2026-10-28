#Requires -Version 5.1
<#
.SYNOPSIS
    Builds a .vsix for each extension without npm / @vscode/vsce, then
    optionally installs it with the VS Code CLI.

.PARAMETER OutDir
    Where the .vsix files are written (default: tools/vscode/dist).

.PARAMETER Install
    Run `code --install-extension` on each built .vsix.

.PARAMETER ExtensionsDir
    Passed through as `--extensions-dir`; useful for a non-destructive test.
#>
[CmdletBinding()]
param(
    [string]$OutDir = '',
    [switch]$Install,
    [string]$ExtensionsDir
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.IO.Compression | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $OutDir) { $OutDir = Join-Path $here 'dist' }
$sources = @(
    (Join-Path $here 'angels95-ozone'),
    (Join-Path $here 'angels95-ozls')
)

if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

function Add-ZipFile {
    param(
        [System.IO.Compression.ZipArchive]$Archive,
        [string]$File,
        [string]$EntryName
    )
    $entry = $Archive.CreateEntry($EntryName, [System.IO.Compression.CompressionLevel]::Optimal)
    $out = $entry.Open()
    $in = [System.IO.File]::OpenRead($File)
    try {
        $in.CopyTo($out)
    } finally {
        $in.Dispose()
        $out.Dispose()
    }
}

function New-Vsix {
    param([string]$Source)

    $pkgPath = Join-Path $Source 'package.json'
    if (-not (Test-Path $pkgPath)) { throw "Missing $pkgPath" }
    $pkg = Get-Content $pkgPath -Raw | ConvertFrom-Json

    $stage = Join-Path $env:TEMP ('angels95-vsix-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage -Force | Out-Null

    try {
        $extDir = Join-Path $stage 'extension'
        Copy-Item -Path $Source -Destination $extDir -Recurse -Force

        $display = [System.Security.SecurityElement]::Escape([string]$pkg.displayName)
        $desc = [System.Security.SecurityElement]::Escape([string]$pkg.description)
        $engine = [System.Security.SecurityElement]::Escape([string]$pkg.engines.vscode)

        $manifest = @"
<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011">
  <Metadata>
    <Identity Language="en-US" Id="$($pkg.name)" Version="$($pkg.version)" Publisher="$($pkg.publisher)" />
    <DisplayName>$display</DisplayName>
    <Description xml:space="preserve">$desc</Description>
    <Tags>angels95,omegatech,ozone,ozls,lightningscript,syntax</Tags>
    <Categories>Programming Languages</Categories>
    <GalleryFlags>Public</GalleryFlags>
    <Properties>
      <Property Id="Microsoft.VisualStudio.Code.Engine" Value="$engine" />
      <Property Id="Microsoft.VisualStudio.Code.ExtensionDependencies" Value="" />
      <Property Id="Microsoft.VisualStudio.Code.ExtensionKind" Value="workspace" />
    </Properties>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code" />
  </Installation>
  <Dependencies />
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true" />
  </Assets>
</PackageManifest>
"@
        [System.IO.File]::WriteAllText((Join-Path $stage 'extension.vsixmanifest'), $manifest, (New-Object System.Text.UTF8Encoding($false)))

        $contentTypes = @"
<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="json" ContentType="application/json" />
  <Default Extension="js" ContentType="application/javascript" />
  <Default Extension="md" ContentType="text/markdown" />
  <Default Extension="txt" ContentType="text/plain" />
  <Default Extension="vsixmanifest" ContentType="text/xml" />
</Types>
"@
        [System.IO.File]::WriteAllText((Join-Path $stage '[Content_Types].xml'), $contentTypes, (New-Object System.Text.UTF8Encoding($false)))

        $vsix = Join-Path $OutDir ("{0}-{1}.vsix" -f $pkg.name, $pkg.version)
        if (Test-Path $vsix) { Remove-Item $vsix -Force }

        $archive = [System.IO.Compression.ZipFile]::Open($vsix, [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            Add-ZipFile -Archive $archive -File (Join-Path $stage 'extension.vsixmanifest') -EntryName 'extension.vsixmanifest'
            Add-ZipFile -Archive $archive -File (Join-Path $stage '[Content_Types].xml') -EntryName '[Content_Types].xml'

            $root = (Join-Path $stage 'extension')
            Get-ChildItem $root -Recurse -File | ForEach-Object {
                $rel = $_.FullName.Substring($root.Length + 1) -replace '\\', '/'
                Add-ZipFile -Archive $archive -File $_.FullName -EntryName ("extension/" + $rel)
            }
        } finally {
            $archive.Dispose()
        }

        Write-Host "Built $vsix"
        return $vsix
    } finally {
        if (Test-Path $stage) { Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

$built = @()
foreach ($source in $sources) {
    $built += New-Vsix -Source $source
}

if ($Install) {
    $code = Get-Command code -ErrorAction SilentlyContinue
    if (-not $code) {
        throw 'The `code` CLI is not on PATH. Open VS Code and run "Shell Command: Install ''code'' command in PATH".'
    }
    foreach ($vsix in $built) {
        $codeArgs = @()
        if ($ExtensionsDir) { $codeArgs += @('--extensions-dir', $ExtensionsDir) }
        $codeArgs += @('--install-extension', $vsix, '--force')
        Write-Host "code $($codeArgs -join ' ')"
        & code @codeArgs
        if ($LASTEXITCODE -ne 0) { throw "code --install-extension failed for $vsix" }
    }
}
