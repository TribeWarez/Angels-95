# perf_matrix.ps1 - run the client benchmark over every shipped world.
#
#   .\tools\perf_matrix.ps1                       # 180 frames/world, playerstart
#   .\tools\perf_matrix.ps1 -Frames 300 -Phase render -Out build\perf\matrix
#   .\tools\perf_matrix.ps1 -Worlds Sektor_01_SilentPines,World_endless_snow
#
# Each run writes <Out>\<World>.csv plus <Out>\<World>.log. The script prints a
# one-line p50/p95 summary per world and exits 1 if any run failed. It does NOT
# fail on slow worlds: this is a measurement tool, not a gate.
param(
    [string]$Exe = ".\Angels95.exe",
    [int]$Frames = 180,
    [int]$Warmup = 60,
    [ValidateSet('all', 'render')][string]$Phase = 'all',
    [string]$Out = "build\perf\matrix",
    [string[]]$Worlds = @(),
    [string]$Cam = ""
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Exe)) { throw "Executable not found: $Exe" }
New-Item -ItemType Directory -Force $Out | Out-Null

if ($Worlds.Count -eq 0) {
    $Worlds = Get-ChildItem "GameData\Worlds" -Directory | ForEach-Object { $_.Name } | Sort-Object
}

$failed = @()
foreach ($w in $Worlds) {
    $csv = Join-Path $Out "$w.csv"
    $log = Join-Path $Out "$w.log"
    $benchArgs = @("--world", $w, "--bench", "$Frames", "--bench-warmup", "$Warmup",
                   "--bench-phase", $Phase, "--bench-csv", $csv)
    if ($Cam -ne "") { $benchArgs += @("--bench-cam", $Cam) }

    Write-Output "=== $w ==="
    & $Exe @benchArgs *> $log
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $csv)) {
        Write-Output "  FAILED (exit $LASTEXITCODE) - see $log"
        $failed += $w
        continue
    }

    $ms = @(Get-Content $csv | Where-Object { $_ -match '^\d+,' } |
            ForEach-Object { [double]($_.Split(',')[1]) })
    if ($ms.Count -eq 0) {
        Write-Output "  FAILED (no data rows)"
        $failed += $w
        continue
    }
    $sorted = $ms | Sort-Object
    $p50 = $sorted[[math]::Min($sorted.Count - 1, [math]::Round(0.50 * ($sorted.Count - 1)))]
    $p95 = $sorted[[math]::Min($sorted.Count - 1, [math]::Round(0.95 * ($sorted.Count - 1)))]
    Write-Output ("  frames={0} p50={1:N2}ms p95={2:N2}ms" -f $ms.Count, $p50, $p95)
}

Write-Output ""
if ($failed.Count -gt 0) {
    Write-Output ("FAILED: " + ($failed -join ', '))
    exit 1
}
Write-Output "All worlds measured. CSVs in $Out."
exit 0
