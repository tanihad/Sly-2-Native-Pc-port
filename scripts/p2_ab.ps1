# (perf-p2) interleaved A/B GPU benchmark of several gs_replay builds: rounds of m9_bench -Runs 1 per build, one CSV line
# per build, recording and round; prints the median of the rounds per build (GPU median, p90, worst per game frame).
#   powershell -ExecutionPolicy Bypass -File scripts\p2_ab.ps1 -Exes "base=build\gsr_base\gs_replay.exe,p2=build\gsr_p2\gs_replay.exe"
#       [-Rounds 3] [-Scale 3] [-Only ep1_hub,ep7_boss] [-EnvVars "A=1"] [-Csv build\ab.csv]
param([Parameter(Mandatory = $true)][string]$Exes, [int]$Rounds = 3, [int]$Scale = 3, [string[]]$Only = @(), [string]$Csv = "",
      [string]$Aspect = "16:9")
$root = Split-Path $PSScriptRoot -Parent
if (-not $Csv) { $Csv = Join-Path $root ("build\p2_ab_{0}.csv" -f $PID) }
Remove-Item $Csv -ErrorAction SilentlyContinue
$builds = @()
foreach ($e in ($Exes -split ',')) {
    $n, $p = $e -split '=', 2
    $envs = ''
    if ($p -match '^(.*?)\|(.*)$') { $p = $Matches[1]; $envs = $Matches[2] }   # name=exe|ENV=1;ENV2=0
    $builds += [pscustomobject]@{ name = $n; exe = (Resolve-Path (Join-Path $root $p)).Path; envs = ($envs -replace ';', ',') }
}
$onlyArg = if ($Only.Count) { @('-Only', ($Only -join ',')) } else { @() }
foreach ($r in 1..$Rounds) {
    foreach ($b in $builds) {
        $envArg = if ($b.envs) { @('-EnvVars', $b.envs) } else { @() }
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'm9_bench.ps1') -Exe $b.exe -Runs 1 -Scale $Scale -Aspect $Aspect `
            -Label $b.name -Csv $Csv @envArg @onlyArg | Out-Null
    }
}
function Med($xs) { $s = @($xs | Sort-Object); if (-not $s.Count) { return [double]::NaN }; if ($s.Count % 2) { $s[[int](($s.Count - 1) / 2)] } else { ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2 } }
$rows = Get-Content $Csv | ForEach-Object { $f = $_.Trim([char]0xFEFF) -split ','; [pscustomobject]@{ b = $f[0]; rec = $f[3]; gpu = [double]$f[4]; p90 = [double]$f[5]; worst = [double]$f[9] } }
"S=$Scale $Aspect, $Rounds interleaved rounds (GPU ms per game frame: median of the rounds' medians / p90 / worst; [min of the medians: least disturbed by other GPU users])"
$recs = $rows | ForEach-Object { $_.rec } | Select-Object -Unique
foreach ($rec in $recs) {
    $line = "{0,-11}" -f $rec
    foreach ($b in $builds) {
        $x = @($rows | Where-Object { $_.rec -eq $rec -and $_.b -eq $b.name })
        $line += (" | {0} {1:N2} / {2:N2} / {3:N2} [{4:N2}]" -f $b.name, (Med ($x | ForEach-Object { $_.gpu })), (Med ($x | ForEach-Object { $_.p90 })), (Med ($x | ForEach-Object { $_.worst })), (($x | ForEach-Object { $_.gpu } | Measure-Object -Minimum).Minimum))
    }
    $line
}
