# perf_report.ps1 - summarise one FrameProfiler CSV.
#
#   .\tools\perf_report.ps1 build\perf\silentpines.csv
#   .\tools\perf_report.ps1 build\perf\silentpines.csv -Top 12
#
# The CSV is written by `Angels95.exe --bench ... --bench-csv <path>` or
# `AngelEd.exe ... --bench ...`. Metadata lines start with '#'; the first data
# line is the column header.
param(
    [Parameter(Mandatory = $true)][string]$Csv,
    [int]$Top = 12
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Csv)) { throw "CSV not found: $Csv" }

$lines = Get-Content $Csv
$meta = $lines | Where-Object { $_ -match '^#' }
$headerLine = $lines | Where-Object { $_ -match '^frame,ms' } | Select-Object -First 1
if (-not $headerLine) { throw "No 'frame,ms' header in $Csv" }

$headers = $headerLine.Split(',')
$rows = @()
foreach ($line in $lines) {
    if ($line -match '^#' -or $line -match '^frame,ms' -or [string]::IsNullOrWhiteSpace($line)) { continue }
    $parts = $line.Split(',')
    if ($parts.Count -ne $headers.Count) { continue }
    $row = @{}
    for ($i = 0; $i -lt $headers.Count; $i++) { $row[$headers[$i]] = $parts[$i] }
    $rows += [pscustomobject]$row
}
if ($rows.Count -eq 0) { throw "No data rows in $Csv" }

function Percentile([double[]]$values, [double]$p) {
    if ($values.Count -eq 0) { return 0 }
    $sorted = $values | Sort-Object
    $idx = [math]::Min($sorted.Count - 1, [math]::Round($p * ($sorted.Count - 1)))
    return $sorted[$idx]
}

$ms = $rows | ForEach-Object { [double]$_.ms }
$fps = $rows | ForEach-Object { [double]$_.fps }
$p50 = Percentile $ms 0.50
$p95 = Percentile $ms 0.95
$p99 = Percentile $ms 0.99

Write-Output "=== $Csv ==="
foreach ($m in $meta) { Write-Output $m }
Write-Output ("frames={0}  p50={1:N2}ms  p95={2:N2}ms  p99={3:N2}ms  avg={4:N1} fps  min={5:N1}  max={6:N1}" -f `
    $rows.Count, $p50, $p95, $p99, (($fps | Measure-Object -Average).Average), `
    (($fps | Measure-Object -Minimum).Minimum), (($fps | Measure-Object -Maximum).Maximum))

$scopeCols = $headers | Where-Object { $_ -like 'scope.*' }
$counterCols = $headers | Where-Object { $_ -like 'counter.*' }

Write-Output ""
Write-Output "-- slowest scopes (mean ms) --"
$scopeCols | ForEach-Object {
    $col = $_
    $v = $rows | ForEach-Object { [double]$_.$col }
    [pscustomobject]@{
        scope = $col.Substring(6)
        mean  = ($v | Measure-Object -Average).Average
        p95   = Percentile $v 0.95
        max   = ($v | Measure-Object -Maximum).Maximum
    }
} | Sort-Object mean -Descending | Select-Object -First $Top |
    Format-Table -AutoSize | Out-String | Write-Output

Write-Output "-- counters (mean per frame) --"
$counterCols | ForEach-Object {
    $col = $_
    $v = $rows | ForEach-Object { [double]$_.$col }
    [pscustomobject]@{
        counter = $col.Substring(8)
        mean    = ($v | Measure-Object -Average).Average
        max     = ($v | Measure-Object -Maximum).Maximum
    }
} | Sort-Object mean -Descending | Select-Object -First $Top |
    Format-Table -AutoSize | Out-String | Write-Output
