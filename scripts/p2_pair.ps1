# (perf-p2) paired A/B GPU benchmark for a GPU shared with other programs: for every round and recording, build A and
# build B replay back to back (alternating which goes first), so both see about the same load; reports per recording
# the median of the per-pair ratios B/A, and both builds' medians and minima of the per-run GPU medians (ms per game
# frame, gs_replay --mixed --bench-skip 10, 16:9), p90 and worst.
#   powershell -File scripts\p2_pair.ps1 -A build\gsr_base\gs_replay.exe -B build\gsr_final\gs_replay.exe [-Rounds 7] [-Scale 3]
#       [-Only ep1_hub,ep7_boss]
param([Parameter(Mandatory = $true)][string]$A, [Parameter(Mandatory = $true)][string]$B, [int]$Rounds = 7, [int]$Scale = 3,
      [string[]]$Only = @(), [string]$Aspect = "16:9")
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$corpus = 'C:\Users\tanih\Projects\sly2-worktrees'
$recs = [ordered]@{ 'ep1_hub' = 'gpu-C\build\gsrec\ep1_hub.gsrec'; 'ep3_hub' = 'ep3\build\gsrec\hub.gsrec'; 'ep5_street' = 'ep5\build\gsrec\hub_street.gsrec';
    'ep8_boss' = 'ep8\build\gsrec\boss.gsrec'; 'ep7_boss' = 'ep7\build\boss.gsrec'; 'ep2_hub' = 'gpu-C\build\gsrec\ep2_hub.gsrec';
    'm5_battle' = 'gpu-M8\build\gsrec\m5_battlezone.gsrec'; 'm1_brawl' = 'gpu-C\build\gsrec\m1_brawl.gsrec'; 'ep6_rc' = 'ep6\build\gsrec\rc_battle.gsrec';
    'load_m3' = 'gpu-C\build\gsrec\load_m3.gsrec'; 'ws_ep3_hud' = 'gpu-M10\build\gsrec\ws_ep3_hud.gsrec' }
$root = Split-Path $PSScriptRoot -Parent
$ea = Join-Path $root ("build\pair_a_{0}.exe" -f $PID); Copy-Item (Resolve-Path $A).Path $ea -Force
$eb = Join-Path $root ("build\pair_b_{0}.exe" -f $PID); Copy-Item (Resolve-Path $B).Path $eb -Force
$log = Join-Path $root ("build\pair_{0}.log" -f $PID)
function Run($exe, $rec) {
    $a = @($rec, '-', '0', '--backend', 'hw', '--adapter', 'hw', '--mixed', '--bench-skip', '10', '--scale', "$Scale", '--aspect', $Aspect)
    $q = ($a | ForEach-Object { '"' + $_ + '"' }) -join ' '
    & cmd /c "`"`"$exe`" $q 2>`"$log`"`""
    $t = (Get-Content $log | Where-Object { $_ -match '^ref-bench' }) -join ' '
    $o = @{}
    if ($t -match 'gpu_med_game=([\d.]+)') { $o.med = [double]$Matches[1] }
    if ($t -match 'gpu_max_game=([\d.]+)') { $o.max = [double]$Matches[1] }
    if ($t -match 'gpu_p90_game=([\d.]+)') { $o.p90 = [double]$Matches[1] }
    return [pscustomobject]$o
}
$res = @{}
foreach ($r in 1..$Rounds) {
    foreach ($k in $recs.Keys) {
        if ($Only.Count -and -not ($Only -contains $k)) { continue }
        $rec = Join-Path $corpus $recs[$k]
        if ($r % 2) { $x = Run $ea $rec; $y = Run $eb $rec } else { $y = Run $eb $rec; $x = Run $ea $rec }
        if (-not $res[$k]) { $res[$k] = New-Object System.Collections.ArrayList }
        [void]$res[$k].Add([pscustomobject]@{ a = $x; b = $y })
    }
}
Remove-Item $ea, $eb, $log -ErrorAction SilentlyContinue
function Med($xs) { $s = @($xs | Where-Object { $_ -ne $null } | Sort-Object); if (-not $s.Count) { return [double]::NaN }; if ($s.Count % 2) { $s[[int](($s.Count - 1) / 2)] } else { ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2 } }
function Mn($xs) { ($xs | Where-Object { $_ -ne $null } | Measure-Object -Minimum).Minimum }
"S=$Scale $Aspect, $Rounds paired rounds; GPU ms per game frame: A median [min] p90 worst | B median [min] p90 worst | B/A (median of pair ratios)"
foreach ($k in $recs.Keys) {
    $l = $res[$k]; if (-not $l) { continue }
    $ratios = @($l | ForEach-Object { if ($_.a.med -gt 0) { $_.b.med / $_.a.med } })
    "{0,-11} | A {1,6:N2} [{2,5:N2}] {3,6:N2} {4,6:N2} | B {5,6:N2} [{6,5:N2}] {7,6:N2} {8,6:N2} | {9:N3}" -f $k,
        (Med ($l | % { $_.a.med })), (Mn ($l | % { $_.a.med })), (Med ($l | % { $_.a.p90 })), (Med ($l | % { $_.a.max })),
        (Med ($l | % { $_.b.med })), (Mn ($l | % { $_.b.med })), (Med ($l | % { $_.b.p90 })), (Med ($l | % { $_.b.max })), (Med $ratios)
}
