# Present-interval report of a run with PS2_PACE_LOG=1 (scripts\pace_run.ps1 -Extra "PS2_PACE_LOG=1"): how evenly the
# pictures were shown (the display's own record, DXGI frame statistics: "shown present" lines), or, offscreen, how evenly
# Present was called. Prints percentiles, the share within +-1 ms of the 16.68 ms VBlank period, a histogram, and the
# VBlank -> shown latency. -From/-To: VBlank range.
#   powershell -ExecutionPolicy Bypass -File scripts\pace_report.ps1 -Log build\pace\<name>\run.log [-From 600] [-To 99999]
param([Parameter(Mandatory = $true)][string]$Log, [int]$From = 0, [int]$To = 1000000000)
$P = 1000.0 * 1001.0 / 60000.0
$shown = @(Select-String -Path $Log -Pattern 'pace: shown present (\d+) \(v(\d+)\) at [\d.]+ ms: VBlank ([\d.]+), planned \+([-\d.]+), Present \+([-\d.]+), shown \+([-\d.]+), interval ([-\d.]+) ms' |
    ForEach-Object { $m = $_.Matches[0].Groups; [pscustomobject]@{ v = [int]$m[2].Value; plan = [double]$m[4].Value; pres = [double]$m[5].Value; shown = [double]$m[6].Value; iv = [double]$m[7].Value } } |
    Where-Object { $_.v -ge $From -and $_.v -le $To })
$kind = 'shown (DXGI frame statistics)'
if (-not $shown.Count) {
    $shown = @(Select-String -Path $Log -Pattern 'pace: offscreen present v(\d+): VBlank ([\d.]+), planned \+([-\d.]+), Present \+([-\d.]+), interval ([-\d.]+) ms' |
        ForEach-Object { $m = $_.Matches[0].Groups; [pscustomobject]@{ v = [int]$m[1].Value; plan = [double]$m[3].Value; pres = [double]$m[4].Value; shown = [double]$m[4].Value; iv = [double]$m[5].Value } } |
        Where-Object { $_.v -ge $From -and $_.v -le $To })
    $kind = 'Present calls (offscreen)'
}
if (-not $shown.Count) { "no pace lines in $Log"; exit 1 }
$iv = @($shown | Where-Object { $_.iv -gt 0 } | ForEach-Object { $_.iv } | Sort-Object)
function Q($a, $q) { $a[[Math]::Min($a.Count - 1, [int]($q * ($a.Count - 1) + 0.5))] }
"$kind, $($shown.Count) pictures (VBlank $(($shown | Select-Object -First 1).v)..$(($shown | Select-Object -Last 1).v)), $($iv.Count) intervals"
"  interval ms: p1 {0:N2}  p5 {1:N2}  median {2:N2}  p95 {3:N2}  p99 {4:N2}  max {5:N2}" -f (Q $iv 0.01), (Q $iv 0.05), (Q $iv 0.5), (Q $iv 0.95), (Q $iv 0.99), $iv[-1]
$within = @($iv | Where-Object { [Math]::Abs($_ - $P) -lt 1.0 }).Count
"  within +-1 ms of {0:N2} ms: {1:N1}%   standard deviation {2:N2} ms" -f $P, (100.0 * $within / [Math]::Max(1, $iv.Count)),
    [Math]::Sqrt((($iv | ForEach-Object { ($_ - $P) * ($_ - $P) }) | Measure-Object -Sum).Sum / [Math]::Max(1, $iv.Count))
$lat = @($shown | Where-Object { $_.shown -gt 0 } | ForEach-Object { $_.shown } | Sort-Object)
if ($lat.Count) { "  VBlank -> shown ms: median {0:N2}  p95 {1:N2}  max {2:N2}" -f (Q $lat 0.5), (Q $lat 0.95), $lat[-1] }
"  histogram (ms):"
$edges = @(0, 4, 8, 11, 13, 15, 16, 16.2, 16.4, 16.6, 16.8, 17, 17.2, 18, 19, 21, 25, 30, 35, 1000)
for ($i = 0; $i -lt $edges.Count - 1; $i++) {
    $n = @($iv | Where-Object { $_ -ge $edges[$i] -and $_ -lt $edges[$i + 1] }).Count
    if ($n) { "    {0,6:N1} - {1,6:N1}: {2,5} {3}" -f $edges[$i], $edges[$i + 1], $n, ('#' * [Math]::Min(60, [int](60.0 * $n / $iv.Count))) }
}
