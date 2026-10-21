#Requires -Version 5.1
<#
tools/sync-wiki.ps1

Publishes the tracked `Wiki/` folder to the GitHub wiki repository
(https://github.com/<owner>/<repo>.wiki.git), which is a SEPARATE git repo from
this one. The wiki is the only published documentation site: the repo has no
GitHub Pages site and no gh-pages branch, and angels95.tribewarez.com serves a
landing page rather than the docs.

WHAT IT DOES
------------
1. clones the wiki repo (fresh every run, so a stale temp clone cannot win),
2. deletes every page in it (the tracked folder is the single source of truth,
   so an orphaned page such as the old World-Format-WDL must not survive),
3. copies `Wiki/*.md` in,
4. rewrites what only works in a checkout:
     - `../GameData/...` image paths -> raw.githubusercontent.com URLs, because
       the wiki has no `GameData/` beside it,
     - wiki-internal `Page.md` / `Wiki/Page` links -> bare `Page`, which is what
       GitHub's wiki resolver expects,
5. regenerates `_Sidebar.md` from Home.md's Contents table (plus an
   "Other pages" section for anything not listed) and `_Footer.md`,
6. commits and pushes only when something actually changed.

USAGE
-----
  pwsh tools/sync-wiki.ps1                 # sync and push (needs push access)
  pwsh tools/sync-wiki.ps1 -DryRun         # build the commit, do not push
  pwsh tools/sync-wiki.ps1 -WikiUrl "https://x-access-token:$TOKEN@github.com/OWNER/REPO.wiki.git"

CI runs this from .github/workflows/wiki-sync.yml on any push that touches
`Wiki/**`. The wiki repo is not the workflow's own repository, so a
fine-grained PAT with Contents: read+write on this repository is the reliable
token; the workflow falls back to GITHUB_TOKEN, which works when the repository
grants it wiki write access.

NOTE: this file is deliberately ASCII-only. Windows PowerShell 5.1 reads a
BOM-less .ps1 as ANSI, so a literal em dash here would be mangled.
#>
[CmdletBinding()]
param(
    [string]$SourceDir = '',
    [string]$WikiUrl   = 'https://github.com/TribeWarez/Angels-95.wiki.git',
    [string]$RepoSlug  = 'TribeWarez/Angels-95',
    [string]$Message   = '',
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

function Invoke-Git {
    param([string[]]$GitArgs)
    & git @GitArgs
    if ($LASTEXITCODE -ne 0) {
        throw ("git {0} failed with exit code {1}" -f ($GitArgs -join ' '), $LASTEXITCODE)
    }
}

function Write-Utf8 {
    param([string]$Path, [string]$Text)
    [IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $SourceDir) { $SourceDir = Join-Path $repoRoot 'Wiki' }
if (-not (Test-Path $SourceDir)) { throw "source directory not found: $SourceDir" }

$pages = @(Get-ChildItem -Path $SourceDir -Filter *.md -File)
if ($pages.Count -eq 0) { throw "no .md pages found in $SourceDir" }

$work = Join-Path ([IO.Path]::GetTempPath()) 'angels95-wiki-sync'
if (Test-Path $work) { Remove-Item $work -Recurse -Force }

Write-Host ("Cloning {0}" -f $WikiUrl)
Invoke-Git @('clone', '--quiet', $WikiUrl, $work)

# 1. Wipe every page. Chrome is regenerated below, and a page that no longer
#    exists in Wiki/ must not linger (that is how World-Format-WDL survived).
Get-ChildItem -Path $work -Filter *.md -File | Remove-Item -Force

# 2. Copy the tracked pages.
foreach ($p in $pages) {
    Copy-Item -LiteralPath $p.FullName -Destination (Join-Path $work $p.Name) -Force
}

# 3. Rewrite checkout-only references for wiki rendering, and normalise the
#    line endings to LF. The wiki repo has no .gitattributes, so a Windows run
#    would otherwise commit CRLF and the next Linux CI run would rewrite all 17
#    pages back to LF - churn on every alternation.
$rawPrefix = "https://raw.githubusercontent.com/$RepoSlug/master/GameData/"
$imgRewrites = 0
$linkRewrites = 0
$eolRewrites = 0

foreach ($f in Get-ChildItem -Path $work -Filter *.md -File) {
    $text = [IO.File]::ReadAllText($f.FullName)
    $before = $text

    $imgRewrites += [regex]::Matches($text, '\.\./GameData/').Count
    $text = $text.Replace('](../GameData/', "]($rawPrefix")
    $text = $text.Replace('src="../GameData/', "src=`"$rawPrefix")

    # Wiki-internal links: `Wiki/Page.md`, `Wiki/Page`, `Page.md` -> `Page`.
    $text = [regex]::Replace($text, '\]\(Wiki/([A-Za-z0-9_-]+)\.md\)', ']($1)')
    $text = [regex]::Replace($text, '\]\(Wiki/([A-Za-z0-9_-]+)\)', ']($1)')
    $text = [regex]::Replace($text, '\]\(([A-Za-z0-9_-]+)\.md\)', ']($1)')
    if ($text -ne $before) { $linkRewrites++ }

    $lf = $text.Replace("`r`n", "`n").Replace("`r", "`n")
    if ($lf -ne $text) { $eolRewrites++ }

    Write-Utf8 -Path $f.FullName -Text $lf
}

Write-Host ("Rewrites: {0} image path(s), {1} page(s) with links, {2} page(s) CRLF->LF" -f `
    $imgRewrites, $linkRewrites, $eolRewrites)

# 4. Sidebar from Home.md's Contents table, then anything it did not list.
$sidebar = New-Object System.Collections.Generic.List[string]
[void]$sidebar.Add('### Angels95')
[void]$sidebar.Add('')
$listed = New-Object System.Collections.Generic.List[string]
[void]$sidebar.Add('- [Home](Home)')
[void]$listed.Add('Home')

$homePage = Join-Path $work 'Home.md'
if (Test-Path $homePage) {
    foreach ($line in [IO.File]::ReadAllLines($homePage)) {
        $m = [regex]::Match($line, '^\|\s*\[([^\]]+)\]\(([^)]+)\)\s*\|')
        if (-not $m.Success) { continue }
        $title = $m.Groups[1].Value
        $target = $m.Groups[2].Value
        if ($target -notmatch '^[A-Za-z0-9_-]+$') { continue }   # external or anchored
        [void]$sidebar.Add(("- [{0}]({1})" -f $title, $target))
        [void]$listed.Add($target)
    }
}

$others = New-Object System.Collections.Generic.List[string]
foreach ($f in Get-ChildItem -Path $work -Filter *.md -File) {
    $name = [IO.Path]::GetFileNameWithoutExtension($f.Name)
    if ($name -in @('Home', '_Sidebar', '_Footer')) { continue }
    if ($listed -contains $name) { continue }

    $title = ($name -replace '[-_]', ' ')
    # ReadAllLines with an explicit UTF-8 decoder: Windows PowerShell 5.1's
    # Get-Content defaults to ANSI, and these pages are UTF-8 without a BOM, so
    # an em dash in an H1 became mojibake in the sidebar. PS 7 defaults to UTF-8,
    # which is exactly the silent 5.1/7 disagreement worth pinning here.
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $head = [IO.File]::ReadAllLines($f.FullName, $utf8) | Select-Object -First 20
    $h1 = $head | Where-Object { $_ -match '^#\s+' } | Select-Object -First 1
    if ($h1) { $title = ($h1 -replace '^#\s+', '').Trim() }
    [void]$others.Add(("- [{0}]({1})" -f $title, $name))
}

if ($others.Count -gt 0) {
    [void]$sidebar.Add('')
    [void]$sidebar.Add('### Other pages')
    [void]$sidebar.Add('')
    foreach ($o in $others) { [void]$sidebar.Add($o) }
}

Write-Utf8 -Path (Join-Path $work '_Sidebar.md') -Text (($sidebar -join "`n") + "`n")

# 5. Footer. Version is the tag, so it follows releases without an edit.
$version = ''
try {
    $version = (& git -C $repoRoot describe --tags --always 2>$null)
    if ($LASTEXITCODE -ne 0) { $version = '' }
} catch { $version = '' }
if (-not $version) { $version = 'dev' }

$footer = ("Angels95 - AngelTech Engine | [repository](https://github.com/{0}) | {1}`n`nBuilt on [raylib](https://www.raylib.com/) 5.5.`n" -f $RepoSlug, $version)
Write-Utf8 -Path (Join-Path $work '_Footer.md') -Text $footer

# 6. Commit (only if the wiki actually differs) and push.
Invoke-Git @('-C', $work, 'add', '-A')

$status = & git -C $work status --porcelain
if (-not $status) {
    Write-Host 'Wiki is already up to date - nothing to push.'
    exit 0
}

# A headless runner has no git identity, and `git commit` then dies with
# "Author identity unknown" BEFORE the push - which looks like an auth failure if
# you only read the exit code. Scope the fallback to this temp clone so a
# developer's own identity is never touched.
$identEmail = (& git -C $work config user.email) 2>$null
if (-not $identEmail) {
    Write-Host 'No git identity configured - setting one on the temp clone only.'
    Invoke-Git @('-C', $work, 'config', 'user.name', 'angels95-wiki-sync')
    Invoke-Git @('-C', $work, 'config', 'user.email', 'wiki-sync@users.noreply.github.com')
}

if (-not $Message) { $Message = 'docs: sync wiki with the tracked Wiki/ folder' }
Invoke-Git @('-C', $work, 'commit', '-m', $Message)

$changed = (& git -C $work show --stat --oneline HEAD | Select-Object -First 1)
Write-Host ("Commit: {0}" -f $changed)

if ($DryRun) {
    Write-Host 'DryRun: commit built in the temp clone, not pushed.'
    Write-Host ("Clone kept at {0}" -f $work)
    exit 0
}

try {
    Invoke-Git @('-C', $work, 'push', 'origin', 'HEAD:master')
} catch {
    # The wiki is a sibling repository. GITHUB_TOKEN is scoped to THIS one and is
    # commonly refused here; say what actually fixes it instead of leaving a bare
    # "exit code 128" and a stale wiki.
    Write-Host ''
    Write-Host 'Wiki push failed. The wiki lives in its own repository'
    Write-Host '(<repo>.wiki.git), which GITHUB_TOKEN often cannot write to.'
    Write-Host 'Add a fine-grained PAT with "Contents: read and write" as the'
    Write-Host 'repository secret WIKI_TOKEN, then re-run the workflow.'
    throw
}
Write-Host 'Wiki published.'
