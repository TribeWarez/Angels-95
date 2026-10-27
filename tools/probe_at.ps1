# Probe: which brushes contain a world (x,y) point, in OZONE Z-up space?
#
# usage: probe_at.ps1 -World <world.ozone> -px <x> -py <y>
#
# NAME THE NUMBERS. A bare negative argument is parsed by PowerShell as a
# PARAMETER NAME, so `probe_at.ps1 world.ozone -60 -30` bound nothing, left px/py
# at 0, and happily printed the brushes containing (0,0) - a silently wrong
# answer that looks like a real one. `-px -60` binds correctly because the
# parameter is already waiting for a value, so the guard below refuses a call
# that lost its coordinates instead of guessing.
param([string]$World, [double]$px, [double]$py)

if (-not $PSBoundParameters.ContainsKey('px') -or -not $PSBoundParameters.ContainsKey('py')) {
    throw "probe_at: pass named coordinates (-px <x> -py <y>); a bare '-60' is parsed as a parameter name and would silently probe (0,0)"
}

$lines = Get-Content $World
$n = 0
foreach ($line in $lines) {
    $n++
    if ($line -match '^\s*add\s+(box|cyl|sph|pyr)\s+(.*)$') {
        $type = $Matches[1]
        $rest = ($Matches[2] -split '\s+') | Where-Object { $_ -match '^-?[\d.]+(\.[\d]+)?$' }
        if ($rest.Count -lt 6) { continue }
        $x = [double]$rest[0]; $y = [double]$rest[1]; $z = [double]$rest[2]
        $x0 = $x1 = $y0 = $y1 = $z0 = $z1 = 0.0
        switch ($type) {
            'box' { $w=[double]$rest[3]; $h=[double]$rest[4]; $d=[double]$rest[5]
                    $x0=$x-$w/2; $x1=$x+$w/2; $y0=$y-$d/2; $y1=$y+$d/2; $z0=$z-$h/2; $z1=$z+$h/2 }
            'cyl' { $r=[Math]::Max([double]$rest[3],[double]$rest[4]); $h=[double]$rest[5]
                    $x0=$x-$r; $x1=$x+$r; $y0=$y-$r; $y1=$y+$r; $z0=$z-$h; $z1=$z }
            'sph' { $r=[double]$rest[3]; $x0=$x-$r; $x1=$x+$r; $y0=$y-$r; $y1=$y+$r; $z0=$z-$r; $z1=$z+$r }
            'pyr' { $w=[double]$rest[3]; $d=[double]$rest[4]; $h=[double]$rest[5]
                    $x0=$x-$w/2; $x1=$x+$w/2; $y0=$y-$d/2; $y1=$y+$d/2; $z0=$z-$h; $z1=$z }
        }
        if ($px -ge $x0 -and $px -le $x1 -and $py -ge $y0 -and $py -le $y1) {
            "L{0,4} {1,-4} x[{2,8:N2},{3,8:N2}] y[{4,8:N2},{5,8:N2}] z[{6,7:N2},{7,7:N2}]  {8}" -f `
                $n, $type, $x0, $x1, $y0, $y1, $z0, $z1, ($line.Trim().Substring(0, [Math]::Min(80, $line.Trim().Length)))
        }
    }
}
