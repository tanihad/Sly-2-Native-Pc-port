# M9 replay benchmark (notes/d3d11-renderer.md M9): GPU / render-thread time per game frame of the GPU renderer's
# mixed frames (gs_replay --backend hw --mixed, no read-backs) on the target recordings, interleaved runs, medians.
#   powershell -ExecutionPolicy Bypass -File scripts\m9_bench.ps1 -Exe build\base\gs_replay.exe [-Runs 3] [-Scale 3]
#       [-Aspect 16:9] [-Only ep8_boss,ep3_hub] [-Extra "--thin"] [-Prof] [-Label base] [-Csv out.csv] [-EnvVars "A=1,B=2"]
# One line per recording: GPU ms per game frame (median of the runs' medians, and their p90), D3D draws, render-thread
# CPU ms; -Prof also prints PS2_GPU_PROF's split (GPU ms per VBlank per kind of work, last report of the last run).
# The GPU utilisation (nvidia-smi) before each run is printed: other programs on the GPU make timings meaningless.
param([Parameter(Mandatory = $true)][string]$Exe, [int]$Runs = 3, [int]$Scale = 3, [string]$Aspect = "16:9",
      [string[]]$Only = @(), [string[]]$Extra = @(), [switch]$Prof, [string]$Label = "", [string]$Csv = "",
      [string]$CorpusRoot = "C:\Users\tanih\Projects\sly2-worktrees", [int]$Skip = 10, [string]$EnvVars = "")
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Extra = @($Extra | ForEach-Object { $_ -split ' ' } | Where-Object { $_ })
$envs = @{}
foreach ($kv in ($EnvVars -split ',' | Where-Object { $_ })) { $q = $kv -split '=', 2; $envs[$q[0]] = $q[1] }
$recs = [ordered]@{
    'ep8_boss'   = 'ep8\build\gsrec\boss.gsrec'
    'ep3_hub'    = 'ep3\build\gsrec\hub.gsrec'
    'ep5_street' = 'ep5\build\gsrec\hub_street.gsrec'
    'ep1_hub'    = 'gpu-C\build\gsrec\ep1_hub.gsrec'
    'ep7_boss'   = 'ep7\build\boss.gsrec'
    'load_m3'    = 'gpu-C\build\gsrec\load_m3.gsrec'
    'ws_ep3_hud' = 'gpu-M10\build\gsrec\ws_ep3_hud.gsrec'
}
$Exe = (Resolve-Path $Exe).Path
$dir = Split-Path $Exe -Parent
$run = Join-Path $dir ("gs_replay_bench_{0}.exe" -f $PID)
Copy-Item $Exe $run -Force
$smi = 'C:\Windows\System32\nvidia-smi.exe'
$res = @{}
foreach ($k in $recs.Keys) { $res[$k] = New-Object System.Collections.ArrayList }
$profLine = @{}
foreach ($r in 1..$Runs) {
    foreach ($k in $recs.Keys) {
        if ($Only.Count -and -not ($Only -contains $k)) { continue }
        $rec = Join-Path $CorpusRoot $recs[$k]
        $log = Join-Path $dir ("bench_{0}_{1}.log" -f $k, $PID)
        $util = if (Test-Path $smi) { (& $smi --query-gpu=utilization.gpu --format=csv,noheader,nounits) -join '' } else { '?' }
        $a = @($rec, '-', '0', '--backend', 'hw', '--adapter', 'hw', '--mixed', '--bench-skip', "$Skip")
        if ($Scale -ne 1) { $a += @('--scale', "$Scale") }
        if ($Aspect) { $a += @('--aspect', $Aspect) }
        $a += $Extra
        $saved = @{}
        foreach ($e in $envs.Keys) { $saved[$e] = [Environment]::GetEnvironmentVariable($e); [Environment]::SetEnvironmentVariable($e, [string]$envs[$e]) }
        if ($Prof) { $env:PS2_GPU_PROF = '1' }
        $quoted = ($a | ForEach-Object { '"' + $_ + '"' }) -join ' '
        & cmd /c "`"`"$run`" $quoted 2>`"$log`"`""
        $rc = $LASTEXITCODE
        if ($Prof) { Remove-Item Env:PS2_GPU_PROF -ErrorAction SilentlyContinue }
        foreach ($e in $envs.Keys) { [Environment]::SetEnvironmentVariable($e, $saved[$e]) }
        $txt = Get-Content $log
        $rb = $txt | Where-Object { $_ -match '^ref-bench ' } | Select-Object -Last 1
        $rd = $txt | Where-Object { $_ -match '^ref-bench-dist ' } | Select-Object -Last 1
        $rn = $txt | Where-Object { $_ -match '^render-bench ' } | Select-Object -Last 1
        $pl = $txt | Where-Object { $_ -match 'prof: ' } | Select-Object -Last 1
        if ($pl) { $profLine[$k] = ($pl -replace '^.*prof: ', '') }
        $o = [ordered]@{ run = $r; util = $util; rc = $rc }
        if ($rb -match 'gpu_med_game=([\d.]+)') { $o.gpu = [double]$Matches[1] }
        if ($rd -match 'gpu_p90_game=([\d.]+)') { $o.p90 = [double]$Matches[1] }
        if ($rn -match 'draws_med=([\d.]+)') { $o.draws = [double]$Matches[1] }
        if ($rn -match 'render_cpu_med=([\d.]+)') { $o.cpu = [double]$Matches[1] }
        if ($rn -match 'render_wall_med=([\d.]+)') { $o.wall = [double]$Matches[1] }
        [void]$res[$k].Add([pscustomobject]$o)
        Write-Host ("  run {0} {1,-10} util {2,3}% rc {3}: GPU {4:N3} (p90 {5:N3}) draws {6} render CPU {7:N3}" -f $r, $k, $util, $rc, $o.gpu, $o.p90, $o.draws, $o.cpu)
        Remove-Item $log -ErrorAction SilentlyContinue
    }
}
Remove-Item $run -ErrorAction SilentlyContinue
function Med($xs) { $s = @($xs | Where-Object { $_ -ne $null } | Sort-Object); if (-not $s.Count) { return [double]::NaN }; if ($s.Count % 2) { return $s[[int](($s.Count - 1) / 2)] } else { return ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2 } }
Write-Host ("{0} S={1} aspect {2} {3}" -f $Label, $Scale, $Aspect, ($Extra -join ' '))
Write-Host ("{0,-11} {1,8} {2,8} {3,7} {4,8} {5,8}  runs" -f 'recording', 'GPU', 'GPU p90', 'draws', 'rCPU', 'rWall')
$lines = @()
foreach ($k in $recs.Keys) {
    $l = $res[$k]
    if (-not $l.Count) { continue }
    $g = Med ($l | ForEach-Object { $_.gpu }); $p = Med ($l | ForEach-Object { $_.p90 }); $d = Med ($l | ForEach-Object { $_.draws })
    $c = Med ($l | ForEach-Object { $_.cpu }); $w = Med ($l | ForEach-Object { $_.wall })
    $all = ($l | ForEach-Object { '{0:N2}' -f $_.gpu }) -join ' '
    Write-Host ("{0,-11} {1,8:N3} {2,8:N3} {3,7:N0} {4,8:N3} {5,8:N3}  {6}" -f $k, $g, $p, $d, $c, $w, $all)
    $lines += ("{0},{1},{2},{3},{4},{5},{6},{7},{8}" -f $Label, $Scale, $Aspect, $k, $g, $p, $d, $c, $w)
    if ($profLine[$k]) { Write-Host ("    prof {0}" -f $profLine[$k]) }
}
if ($Csv) { $lines | Add-Content -Path $Csv -Encoding utf8 }
