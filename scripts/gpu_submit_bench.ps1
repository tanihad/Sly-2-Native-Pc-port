# M2 submission go/no-go benchmark (notes/d3d11-renderer.md 11 M2): replays recordings with
# gs_replay -Backend hwsubmit (the GPU frontend's frames executed on the render thread with real D3D11
# objects and trivial shaders, runtime/gpu/gpu_exec.c) and reports per recording the median over -Runs
# runs of: the render thread's worst frame (all game frames, and steady state = without replay-start
# frames), median frame, CPU time, us per D3D draw, GPU ms, the D3D driver threads' CPU, and the
# frontend's own time. -Pace (default on) replays VBlanks at 59.94 Hz as the game does, so a GPU
# backlog does not inflate the render thread's wall time. Game frame = VBlank with >= 1000 primitives.
# The frontend runs on its own thread (gpu_fthread.c) unless -SameThread; the GS-side columns are the
# per-game-frame CPU of the decoding (GS) thread and of the frontend thread (median / worst steady).
#   powershell -ExecutionPolicy Bypass -File scripts\gpu_submit_bench.ps1 [-Only ep8_boss,ep3_hub] [-Runs 5] [-NoPace] [-Scale 1]
param([string[]]$Only = @('ep8_boss', 'ep3_hub', 'ep5_hub_street', 'gpu-C_ep2_hub', 'gpu-C_thiefnet'), [int]$Runs = 5,
      [switch]$NoPace, [switch]$SameThread, [int]$Scale = 1, [string]$CorpusRoot = "", [string]$Out = "")
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
. (Join-Path $PSScriptRoot 'gpu_corpus.ps1'); $CorpusRoot = Resolve-GpuCorpus $CorpusRoot $main
if (-not $CorpusRoot) { exit 0 }
if (-not $Out) { $Out = Join-Path $root 'build\m2\submit' }
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$recs = Get-Content (Join-Path $root 'tests\gpu\corpus.txt') | Where-Object { $_ -and -not $_.StartsWith('#') } |
    ForEach-Object { ($_ -split '\|')[0].Trim() }
function Median($v) { $s = @($v | Sort-Object); if (-not $s.Count) { return 0 }; if ($s.Count % 2) { return $s[($s.Count - 1) / 2] }; return ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2 }
$load = (Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average
$others = @(Get-Process | Where-Object { $_.ProcessName -like 'sly2*' -or $_.ProcessName -like 'gs_replay*' } | ForEach-Object { $_.ProcessName })
Write-Host ("machine: CPU load {0}% before the runs; other game/replay processes: {1}" -f $load, $(if ($others.Count) { $others -join ', ' } else { 'none' }))
$bin = Join-Path $Out 'bin'
$built = $false
$rows = @()
foreach ($rel in $recs) {
    $name = ($rel -replace '\\build\\gsrec\\', '_' -replace '\\build\\', '_' -replace '\.gsrec$', '')
    if (-not ($Only | Where-Object { $name -eq $_ })) { continue }
    $rec = Join-Path $CorpusRoot $rel
    if (-not (Test-Path $rec)) { Write-Host "$name : missing $rec"; continue }
    $runsData = @()
    for ($i = 0; $i -lt $Runs; $i++) {
        $log = Join-Path $Out "$name.run$i.log"
        $runLoad = [math]::Round(((Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 1).CounterSamples | Measure-Object CookedValue -Average).Average)
        $runArgs = @($rec, '-', '0', '--backend', 'hwsubmit', '--scale', $Scale)
        if (-not $SameThread) { $runArgs += '--front-thread' }
        if (-not $NoPace) { $runArgs += '--pace' }
        if (-not $built) {
            $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'gs_replay.ps1'), '-Rec', $rec,
                   '-Out', $bin, '-Bench', '-Workers', '0', '-Backend', 'hwsubmit', '-Scale', $Scale, '-Log', $log)
            if (-not $NoPace) { $a += '-Pace' }
            if (-not $SameThread) { $a += '-FrontThread' }
            & powershell @a | Out-Null
            $built = $true
        } else {
            $exe = Join-Path $bin ("gs_replay_bench_{0}.exe" -f $PID)
            Copy-Item (Join-Path $bin 'gs_replay.exe') $exe -Force
            $quoted = ($runArgs | ForEach-Object { '"' + $_ + '"' }) -join ' '
            & cmd /c "`"`"$exe`" $quoted 2>`"$log`"`""
            Remove-Item $exe -ErrorAction SilentlyContinue
        }
        $line = (Get-Content $log | Where-Object { $_ -like 'submit-bench *' }) -join ''
        if (-not $line) { Write-Host "$name run $i : no result (see $log)"; continue }
        $kv = @{}
        foreach ($t in ($line -replace '^submit-bench ', '') -split ' ') { $p = $t -split '='; if ($p.Count -eq 2) { $kv[$p[0]] = [double]$p[1] } }
        $tb = (Get-Content $log | Where-Object { $_ -like 'thread-bench *' }) -join ''
        foreach ($t in ($tb -replace '^thread-bench ', '') -split ' ') { $p = $t -split '='; if ($p.Count -eq 2 -and $p[0] -ne 'game') { $kv['t_' + $p[0]] = [double]$p[1] } }
        $cl = (Get-Content $log | Where-Object { $_ -like 'replay thread CPU*' }) -join ''
        if ($cl -match 'CPU ([\d.]+) ms') { $kv['replay_cpu_ms'] = [double]$matches[1] }
        $kv['load'] = $runLoad
        $runsData += $kv
    }
    if (-not $runsData.Count) { continue }
    $med = { param($k) [math]::Round((Median @($runsData | ForEach-Object { $_[$k] })), 3) }
    $row = [ordered]@{ name = $name; runs = $runsData.Count; game_frames = & $med 'game'
        draws_max = & $med 'draws_max'
        render_worst_ms = & $med 'render_max'; render_worst_steady_ms = & $med 'render_max_steady'
        render_p95_ms = & $med 'render_p95'; render_median_ms = & $med 'render_med'
        render_cpu_worst_steady_ms = & $med 'cpu_max_steady'; render_cpu_median_ms = & $med 'cpu_med'
        driver_threads_cpu_per_game_frame_ms = & $med 'other_game_ms'
        upload_median_ms = & $med 'upload_med'; us_per_draw = & $med 'us_draw'
        gpu_median_ms = & $med 'gpu_med'; gpu_worst_steady_ms = & $med 'gpu_max_steady'
        front_median_ms = & $med 'front_med'; front_worst_ms = & $med 'front_max'
        gs_thread_median_ms = & $med 't_gs_med'; gs_thread_worst_steady_ms = & $med 't_gs_max_steady'
        frontend_thread_median_ms = & $med 't_ft_med'; frontend_thread_worst_steady_ms = & $med 't_ft_max_steady'
        replay_thread_cpu_per_game_frame_ms = [math]::Round((Median @($runsData | ForEach-Object { $_['replay_cpu_ms'] / [math]::Max(1, $_['game']) })), 3)
        machine_load_per_run = (@($runsData | ForEach-Object { $_['load'] }) -join ' ')
        worst_steady_runs = (@($runsData | ForEach-Object { [math]::Round($_['render_max_steady'], 2) }) -join ' ') }
    $rows += [pscustomobject]$row
    Write-Host ($rows[-1] | Format-List | Out-String).Trim()
    Write-Host ''
}
$rows | ConvertTo-Json | Set-Content (Join-Path $Out ("bench_s{0}{1}{2}.json" -f $Scale, $(if ($NoPace) { '_nopace' } else { '' }), $(if ($SameThread) { '_same' } else { '' })))
