# perf_compare.ps1 - compare two FrameProfiler CSVs.
#
#   .\tools\perf_compare.ps1 -Baseline build\perf\before.csv -Candidate build\perf\after.csv
#   .\tools\perf_compare.ps1 ... -Threshold 10
#
# Exits 1 when p95 frame time regresses by more than -Threshold percent (the
# default 10). Use -AllowRegression to report without failing.
param(
    [Parameter(Mandatory = $true)][string]$Baseline,
    [Parameter(Mandatory = $true)][string]$Candidate,
    [double]$Threshold = 10.0,
    [switch]$AllowRegression
)

$ErrorActionPreference = 'Stop'

function Read-PerfCsv([string]$path) {
    if (-not (Test-Path $path)) { throw "CSV not found: $path" }
    $lines = Get-Content $path
    $meta = @($lines | Where-Object { $_ -match '^#' })
    $headerLine = $lines | Where-Object { $_ -match '^frame,ms' } | Select-Object -First 1
    if (-not $headerLine) { throw "No 'frame,ms' header in $path" }
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
    if ($rows.Count -eq 0) { throw "No data rows in $path" }
    return [pscustomobject]@{ Path = $path; Meta = $meta; Headers = $headers; Rows = $rows }
}

function Percentile([double[]]$values, [double]$p) {
    if ($values.Count -eq 0) { return 0 }
    $sorted = $values | Sort-Object
    $idx = [math]::Min($sorted.Count - 1, [math]::Round($p * ($sorted.Count - 1)))
    return $sorted[$idx]
}

function FrameStats($data) {
    $ms = @($data.Rows | ForEach-Object { [double]$_.ms })
    return [pscustomobject]@{
        N   = $ms.Count
        P50 = (Percentile $ms 0.50)
        P95 = (Percentile $ms 0.95)
        P99 = (Percentile $ms 0.99)
        Avg = (($ms | Measure-Object -Average).Average)
    }
}

function CounterMean($data, [string]$name) {
    $col = "counter.$name"
    if ($data.Headers -notcontains $col) { return $null }
    $v = @($data.Rows | ForEach-Object { [double]$_.$col })
    return (($v | Measure-Object -Average).Average)
}

$base = Read-PerfCsv $Baseline
$cand = Read-PerfCsv $Candidate
$b = FrameStats $base
$c = FrameStats $cand

Write-Output "=== perf compare ==="
Write-Output ("baseline : {0} ({1} frames)" -f $Baseline, $b.N)
Write-Output ("candidate: {0} ({1} frames)" -f $Candidate, $c.N)
foreach ($m in $base.Meta) { Write-Output "  base $m" }
foreach ($m in $cand.Meta) { Write-Output "  cand $m" }

function Line([string]$label, [double]$from, [double]$to, [string]$unit) {
    $delta = if ($from -gt 0) { (($to - $from) / $from) * 100.0 } else { 0.0 }
    $sign = if ($delta -ge 0) { '+' } else { '' }
    Write-Output ("{0,-12} {1,9:N2}{4} -> {2,9:N2}{4}   {3}{5:N1}%" -f $label, $from, $to, $sign, $unit, $delta)
    return $delta
}

[void](Line "p50" $b.P50 $c.P50 "ms")
[void](Line "p95" $b.P95 $c.P95 "ms")
[void](Line "p99" $b.P99 $c.P99 "ms")
[void](Line "avg" $b.Avg $c.Avg "ms")

$counterNames = @(
    'brush.total', 'brush.fast', 'brush.perface', 'draw.calls', 'draw.faces',
    'tex.stat', 'uniform.upload', 'light.submitted', 'mesh.bind',
    'particle.drawn', 'props.drawn', 'props.culled'
)
Write-Output ""
foreach ($name in $counterNames) {
    $from = CounterMean $base $name
    $to = CounterMean $cand $name
    if ($null -eq $from -and $null -eq $to) { continue }
    if ($null -eq $from) { $from = 0 }
    if ($null -eq $to) { $to = 0 }
    [void](Line $name $from $to "")
}

$p95Delta = if ($b.P95 -gt 0) { (($c.P95 - $b.P95) / $b.P95) * 100.0 } else { 0.0 }
if ($p95Delta -gt $Threshold -and -not $AllowRegression) {
    Write-Output ""
    Write-Output ("REGRESSION: p95 +{0:N1}% (threshold {1:N1}%)" -f $p95Delta, $Threshold)
    exit 1
}
Write-Output ""
Write-Output "OK"
exit 0
