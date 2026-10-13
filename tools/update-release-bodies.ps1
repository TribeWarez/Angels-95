#Requires -Version 5.1
$ErrorActionPreference = 'Stop'

$Repo = 'odelyzid/angels-95-omega-tech'
$Token = $env:GITHUB_TOKEN
if (-not $Token) { throw 'GITHUB_TOKEN env var is empty' }

# (tag, previous tag) ordered oldest -> newest
$Tags = @(
  @{ T='b75'; Prev='b74' },
  @{ T='b76'; Prev='b75' },
  @{ T='b82'; Prev='b76' },
  @{ T='b83'; Prev='b82' },
  @{ T='b84'; Prev='b83' },
  @{ T='b85'; Prev='b84' },
  @{ T='b86'; Prev='b85' },
  @{ T='b88'; Prev='b87' }
)

$ApiHeaders = @{
  Authorization        = "Bearer $Token"
  'User-Agent'         = 'angels95-release-changelog-script'
  Accept               = 'application/vnd.github+json'
  'X-GitHub-Api-Version' = '2022-11-28'
}

function Get-CuratedCommits {
  param([string]$Prev, [string]$Tag)
  $lines = git log "$Prev..$Tag" --no-merges --pretty=format:'%H%x09%s'
  $keep = 0; $total = 0
  $items = New-Object System.Collections.Generic.List[object]
  foreach ($line in $lines) {
    $total++
    if ($line.Length -lt 41) { continue }
    $sha = $line.Substring(0, 40).Trim()
    $subj = $line.Substring(41)
    $subj = $subj.TrimStart([char]0xFEFF)  # strip BOM
    # Keep if subject matches a curated prefix OR is a b<N> summary
    if ($subj -match '^(feat|fix|refactor|docs)(\(|:)' -or $subj -match '^b\d+ ') {
      $items.Add([pscustomobject]@{ Sha = $sha; Subject = $subj })
      $keep++
    }
  }
  return [pscustomobject]@{ Kept = $keep; Total = $total; Items = $items.ToArray() }
}

function Get-TypeAndTitle {
  param([string]$Subject)
  if ($Subject -match '^b(\d+)\s+(feat|fix|refactor|docs)(?:\(|:)\s*(.*)$') {
    $bn   = $Matches[1]
    $ty   = $Matches[2]
    $rest = $Matches[3]
    $label = '[b' + $bn + '] ' + $ty + ': ' + $rest
    return @{ Type = $ty; Scope = $null; Title = $label }
  }
  if ($Subject -match '^b(\d+)\s+(.+)$') {
    return @{ Type = 'feat'; Scope = $null; Title = '[b' + $Matches[1] + '] ' + $Matches[2] }
  }
  if ($Subject -match '^(feat|fix|refactor|docs)\((.+?)\):\s*(.+)$') {
    return @{ Type = $Matches[1]; Scope = $Matches[2]; Title = $Matches[3] }
  }
  if ($Subject -match '^(feat|fix|refactor|docs):\s*(.+)$') {
    return @{ Type = $Matches[1]; Scope = $null; Title = $Matches[2] }
  }
  return @{ Type = 'chore'; Scope = $null; Title = $Subject }
}

function Format-ReleaseBody {
  param(
    [string]$Tag, [string]$Prev,
    [object]$Curated
  )
  $sb = New-Object System.Text.StringBuilder
  $diffUrl     = "https://github.com/$Repo/compare/$Prev...$Tag"
  $prevTagUrl  = "https://github.com/$Repo/releases/tag/$Prev"

  [void]$sb.AppendLine("## Angels95 $Tag")
  [void]$sb.AppendLine('')
  [void]$sb.AppendLine("Changes since [$Prev]($prevTagUrl) - [compare]($diffUrl)")
  [void]$sb.AppendLine('')
  if ($Curated.Items.Count -eq 0) {
    [void]$sb.AppendLine('_No curated commits in this range._')
    [void]$sb.AppendLine('')
  } else {
    $groups = $Curated.Items | ForEach-Object {
      $p = Get-TypeAndTitle $_.Subject
      [pscustomobject]@{
        Type  = $p.Type
        Scope = $p.Scope
        Title = $p.Title
        Sha   = $_.Sha
      }
    }
    # (display heading, plural form of the commit type)
    $headings = @{
      feat     = 'Features'
      fix      = 'Fixes'
      refactor = 'Refactors'
      docs     = 'Docs'
    }
    foreach ($key in 'feat','fix','refactor','docs') {
      $rows = @($groups | Where-Object { $_.Type -eq $key })
      if ($rows.Count -eq 0) { continue }
      [void]$sb.AppendLine('### ' + $headings[$key])
      [void]$sb.AppendLine('')
      foreach ($r in $rows) {
        $short = $r.Sha.Substring(0,7)
        $url   = "https://github.com/$Repo/commit/$($r.Sha)"
        $title = $r.Title -replace '`','``'
        if ($r.Scope) {
          $line = "- **$($key)($($r.Scope))**: $title ([$short]($url))"
        } else {
          $line = "- **$($key)**: $title ([$short]($url))"
        }
        [void]$sb.AppendLine($line)
      }
      [void]$sb.AppendLine('')
    }
  }

  [void]$sb.AppendLine('### Downloads')
  [void]$sb.AppendLine('')
  [void]$sb.AppendLine('**Standalone (run directly, no other files needed):**')
  [void]$sb.AppendLine("- `AngelServ-$Tag-linux-x86_64` / `AngelServ-$Tag-windows-x86_64.exe` - dedicated game server")
  [void]$sb.AppendLine("- `AngelMaster-$Tag-linux-x86_64` / `AngelMaster-$Tag-windows-x86_64.exe` - master/listing server")
  [void]$sb.AppendLine("- `OzPack-$Tag-linux-x86_64` / `OzPack-$Tag-windows-x86_64.exe` - asset packer CLI")
  [void]$sb.AppendLine('')
  [void]$sb.AppendLine("**Requires the full `System-$Tag.zip`** (raylib/GLFW DLLs, the packaged assets in `System/Data`, INI templates, run scripts, `angels95-serv.service`; the client needs no loose GameData tree):")
  [void]$sb.AppendLine("- `Angels95-$Tag-linux-x86_64` / `Angels95-$Tag-windows-x86_64.exe` - game client")
  [void]$sb.AppendLine("- `AngelEd-$Tag-windows-x86_64.exe` - editor (Windows only; Win32 native panels)")
  [void]$sb.AppendLine('')
  [void]$sb.AppendLine('Platform notes:')
  [void]$sb.AppendLine('- Linux binaries are dynamically linked against the distro glibc; run on a modern x86_64 Linux.')
  [void]$sb.AppendLine('- `AngelEd` is Windows-only (Win32 API); there is no Linux build.')
  [void]$sb.AppendLine('- `AngelServ` reads loose world files (`--dir <GameData>` with `GameData/Worlds/<Name>/World.ozone`), so it needs a game-data checkout; the zip''s packaged data serves the client and editor.')
  [void]$sb.AppendLine('- For VPS systemd units see `System/angels95-serv.service`; config lives in `System/OzServer.ini` (`--bind 0.0.0.0` to accept remote clients).')
  return $sb.ToString()
}

New-Item -ItemType Directory -Force -Path 'releases' | Out-Null

# 1. Token sanity check
$who = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo" -Headers $ApiHeaders
Write-Host "Auth OK as $($who.owner.login)/$($who.name) - visibility: $($who.visibility)"

# 2. Build & save drafts, fetch release IDs, PATCH
foreach ($pair in $Tags) {
  $tag  = $pair.T
  $prev = $pair.Prev
  $curated = Get-CuratedCommits -Prev $prev -Tag $tag
  Write-Host "$tag (prev=$prev): kept $($curated.Kept)/$($curated.Total) commits"

  $body = Format-ReleaseBody -Tag $tag -Prev $prev -Curated $curated
  $draftPath = "releases/$tag.md"
  Set-Content -LiteralPath $draftPath -Value $body -Encoding UTF8
  Write-Host "  draft -> $draftPath ($($body.Length) chars)"

  $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/tags/$tag" -Headers $ApiHeaders
  Write-Host "  release id=$($rel.id) name=`"$($rel.name)`""

  $payload = @{ body = $body }
  $json = $payload | ConvertTo-Json -Compress -Depth 5
  $uri  = "https://api.github.com/repos/$Repo/releases/$($rel.id)"
  $resp = Invoke-RestMethod -Uri $uri -Method Patch -Headers $ApiHeaders -Body $json -ContentType 'application/json'
  Write-Host "  PATCH ok -> https://github.com/$Repo/releases/tag/$tag (body length $($resp.body.Length))"
}

Write-Host ''
Write-Host 'Done. Drafts are in releases/*.md; live bodies are at https://github.com/odelyzid/angels-95-omega-tech/releases'