# Chaos benchmark (perf-p3, notes/perf-plan.md "Chaos benchmark"): the most chaotic scenes we can reach (boss fights,
# brawls, explosions, particle effects, the heaviest hub views; tests/gpu/chaos.txt), each played by time-based scripted
# input in the release build, headless, frame_rate unlimited, 3x 16:9 (the default aspect), with PS2_FPS_TRACE (a line per
# VBlank), PS2_PERF and PS2_GS_HW_STATS; scripts\chaos_sum.py then reports, inside the scene's window, the median fps, the
# 1% and 0.1% lows (99th / 99.9th percentile frame times) and the per-stage ms (worker, GS thread, frontend, render, GPU).
#
#   .\scripts\chaos_bench.ps1 -Tag base [-Exes "base=build\base\sly2_p3_base.exe,new=build\rel\sly2.exe"] [-Scenes a,b]
#                             [-Repeat 2] [-Scale 3] [-Rate unlimited] [-Extra "K=V;K=V"] [-Pin]
#   .\scripts\chaos_bench.ps1 -Scout [-Scenes a,b] [-Parallel 4]     # pictures every 4 s (frames\), to find the windows
#   .\scripts\chaos_bench.ps1 -Record [-Scenes a,b]                  # GS recordings of the scenes (build\gsrec\chaos_<name>.gsrec)
#
# Builds are interleaved per scene and repeat (A B A B ...), so both see the same machine; results are appended to
# build\chaos\<Tag>.txt (one line per run) and build\chaos\<Tag>.jsonl, and a table of medians over the repeats is printed.
# -Record plays the scene at frame_rate 60 (the corpus convention: one game frame per VBlank or two) and records the
# scene's "rec" VBlanks starting where chaos.txt puts the REC token (gs_record.c), e.g. for gs_replay --front-thread.
# Every run copies the exe to a unique name (sly2_p3_<scene>_<build>.exe); nothing is ever killed by name.
param([string]$Tag = "run", [string]$Exes = "", [string[]]$Scenes = @(), [int]$Repeat = 1, [int]$Scale = 3,
      [string]$Rate = "unlimited", [string]$Extra = "", [switch]$Scout, [int]$Parallel = 1, [switch]$Record,
      [int]$RecFrames = 0, [switch]$Keep, [switch]$Pin, [int]$Secs = 0)
$Scenes = @($Scenes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'orig\SCUS_973.16'))) {   # worktree: find the main checkout
    $gitdir = ((Get-Content (Join-Path $root '.git') -Raw) -replace '^gitdir:\s*', '').Trim()
    $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
}
$py = Join-Path $main '.venv\Scripts\python.exe'
$iso = Get-ChildItem (Join-Path $main 'orig\disc') -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1

# time-based pads (runtime/window.c r<s> tokens: the same real-time input at any frame rate)
$macros = @{
    # fight: run, a three-hit combo, camera, sidestep, double jump, combo, back off, triangle, camera, circle
    'GPT' = 'r0.5:lup:2s,r2:square:0.1s,r0.35:square:0.1s,r0.35:square:0.1s,r0.4:rright:1s,r1:lleft:1.5s,r1.5:cross:0.1s,r0.3:cross:0.1s,r0.6:square:0.1s,r0.35:square:0.1s,r0.4:ldown:1.5s,r1.5:triangle:0.1s,r0.5:rleft:1s,r1:lright:1.5s,r1.5:circle:0.1s';
    # hub walk (fps2_scene's HPT without its wait)
    'HPT' = 'r1:lup:1.5s,r2:rright:1s,r1:lup:1s,r1:cross:0.1s,r1:lleft:1s,r3:lup:2s,r2:rleft:1.3s,r5:lup:3.3s,r3:square:0.1s,r5:lup:2.5s';
    # vehicle / turret: aim around and fire
    'TPT' = 'r0.5:rright:1.5s,r0.2:r1:2s,r2:square:0.1s,r0.3:cross:0.1s,r0.5:rleft:1.5s,r0.2:r1:2s,r2:lup:2s,r2:circle:0.1s,r0.3:square:0.1s'
}
function Expand-Pad([string]$pad, [string]$rec) {
    $out = @()
    foreach ($t in ($pad -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
        if ($t -match '^(GPT|HPT|TPT)(\*(\d+))?$') {
            $n = if ($Matches[3]) { [int]$Matches[3] } else { 1 }
            for ($i = 0; $i -lt $n; $i++) { $out += $macros[$Matches[1]] }
        } elseif ($t -eq 'REC') { $out += $rec }
        else { $out += $t }
    }
    return ($out -join ',')
}

$list = Join-Path $root 'tests\gpu\chaos.txt'
$all = @(Get-Content $list | Where-Object { $_.Trim() -and -not $_.Trim().StartsWith('#') } | ForEach-Object {
    $f = @($_ -split '\|' | ForEach-Object { $_.Trim() })
    $w = $f[6] -split '-'
    [pscustomobject]@{ name = $f[0]; ep = $f[1]; job = $f[2]; flags = $f[3]; secs = [int]$f[4]; pad = $f[5];
                       from = [double]$w[0]; to = [double]$w[1]; recf = $(if ($f.Count -gt 7 -and $f[7]) { [int]$f[7] } else { 240 }) }
})
if ($Scenes.Count) { $all = @($all | Where-Object { $Scenes -contains $_.name }) }
if (-not $all.Count) { "no scenes"; exit 1 }

$builds = @()
foreach ($e in ($Exes -split ',' | Where-Object { $_ })) {
    $n, $p = $e -split '=', 2
    $builds += [pscustomobject]@{ name = $n; exe = (Resolve-Path (Join-Path $root $p)).Path }
}
if (-not $builds.Count) { $builds = @([pscustomobject]@{ name = $Tag; exe = (Join-Path $root 'build\rel\sly2.exe') }) }

$free = [math]::Round((Get-PSDrive C).Free / 1GB, 1)
if ($free -lt 50) { "C: has $free GB free (< 50): not starting"; exit 1 }

# Start one run; returns @{ proc; dir; scene; build }
function Start-Run($s, $b, [string]$mode, [int]$rep) {
    $dir = Join-Path $root "build\chaos\$mode\$($s.name)_$($b.name)_$rep"
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    $card = Join-Path $dir 'card'
    New-Item -ItemType Directory -Force $dir, $card | Out-Null
    $mk = @('scripts\warp.py', 'make', '--episode', $s.ep, '--card', $card)
    $direct = $false; $play = $false; $envs = @{}
    if ($s.job) { $mk += @('--job', $s.job) }
    foreach ($fl in ($s.flags -split ' ' | Where-Object { $_ })) {
        if ($fl -eq 'direct') { $mk += '--direct'; $direct = $true }
        elseif ($fl -eq 'play') { $play = $true }
        elseif ($fl -like 'make=*') { $mk += (($fl.Substring(5) -replace '~', ' ') -split ' ' | Where-Object { $_ }) }
        elseif ($fl -like 'env=*') { $k, $v = $fl.Substring(4) -split '=', 2; $envs[$k] = $v }
    }
    Push-Location $root
    $made = & $py @mk
    Pop-Location
    $task = ''
    if ($direct) {
        $w = $made | Select-String 'PS2_WARP_TASK=(\S+)' | Select-Object -First 1
        if (-not $w) { Write-Host "$($s.name): warp.py printed no PS2_WARP_TASK"; return $null }
        $task = $w.Matches[0].Groups[1].Value
    }
    $recTok = if ($mode -eq 'record') { 'r0:rec' } else { 'r0:none' }
    $pad = Expand-Pad $s.pad $recTok
    $script = '@187D80/1DD96C+30:start:10'
    if ($play) { $pad = "r25:cross:0.2s,$pad" }    # X at the safehouse ("Press X to play as ...") 25 s after START
    if ($pad) { $script += ",$pad" }
    $exe = Join-Path $dir ("sly2_p3_{0}_{1}.exe" -f ($s.name -replace '[^\w]', '_'), $b.name)
    Copy-Item $b.exe $exe -Force
    $keep = @{}
    $set = @{
        PS2_WATCHDOG = "$(if ($Secs -gt 0) { $Secs } else { $s.secs })"; PS2_HEADLESS = '1'; PS2_AUDIO_NULL = '1'; PS2_INPUT = $script; PS2_MEMCARD_DIR = $card;
        PS2_WARP_TASK = $task; PS2_FPS_TRACE = (Join-Path $dir 'trace.txt'); PS2_PERF = '1'; PS2_GS_HW_STATS = '1';
        PS2_FRAME_RATE = $(if ($mode -eq 'record') { '60' } else { $Rate }); PS2_GS_SCALE = "$Scale"
    }
    if ($iso) { $set['PS2_ISO'] = $iso.FullName }
    if ($mode -eq 'scout') {
        New-Item -ItemType Directory -Force (Join-Path $dir 'frames') | Out-Null
        $set['PS2_DUMP_FRAMES'] = (Join-Path $dir 'frames'); $set['PS2_DUMP_SECS'] = '4'; $set['PS2_DUMP_EVERY'] = '100000'
    }
    if ($mode -eq 'record') {
        $rec = Join-Path $root "build\gsrec\chaos_$($s.name).gsrec"
        New-Item -ItemType Directory -Force (Split-Path $rec) | Out-Null
        Remove-Item $rec -ErrorAction SilentlyContinue
        $set['PS2_GS_RECORD'] = $rec; $set['PS2_GS_RECORD_FROM'] = '-1'
        $set['PS2_GS_RECORD_FRAMES'] = "$(if ($RecFrames -gt 0) { $RecFrames } else { $s.recf })"
        New-Item -ItemType Directory -Force (Join-Path $dir 'frames') | Out-Null
        $set['PS2_DUMP_FRAMES'] = (Join-Path $dir 'frames'); $set['PS2_DUMP_FROM'] = 'rec'; $set['PS2_DUMP_EVERY'] = '30'
    }
    foreach ($k in $envs.Keys) { $set[$k] = $envs[$k] }
    foreach ($kv in ($Extra -split ';' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; $set[$k] = $v }
    foreach ($k in $set.Keys) { $keep[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $set[$k]) }
    $p = Start-Process -FilePath $exe -ArgumentList (Join-Path $main 'orig\SCUS_973.16') -WorkingDirectory $dir `
        -RedirectStandardError (Join-Path $dir 'run.log') -RedirectStandardOutput (Join-Path $dir 'run.out') -PassThru -NoNewWindow
    foreach ($k in $keep.Keys) { [Environment]::SetEnvironmentVariable($k, $keep[$k]) }
    if ($Pin -and $p) { try { $p.ProcessorAffinity = [IntPtr]0xFFFF } catch {} }   # (the 8 P-cores' 16 threads)
    return [pscustomobject]@{ proc = $p; dir = $dir; scene = $s; build = $b; exe = $exe; rec = $(if ($mode -eq 'record') { $rec } else { '' }) }
}
function Wait-Run($r) {
    $t0 = Get-Date
    while (-not $r.proc.HasExited) {
        Start-Sleep -Seconds 1
        if ($r.rec -and (Select-String -Path (Join-Path $r.dir 'run.log') -Pattern 'GS recording complete' -Quiet)) {
            Start-Sleep -Seconds 3
            if (-not $r.proc.HasExited) { Stop-Process -Id $r.proc.Id -Force }
            break
        }
        if (((Get-Date) - $t0).TotalSeconds -gt $(if ($Secs -gt 0) { $Secs } else { $r.scene.secs }) + 30) { Stop-Process -Id $r.proc.Id -Force; Write-Host "$($r.dir): killed (watchdog)"; break }
    }
    if (-not $Keep) { Remove-Item $r.exe -ErrorAction SilentlyContinue }
}

if ($Scout -or $Record) {
    $mode = if ($Record) { 'record' } else { 'scout' }
    $queue = [System.Collections.ArrayList]@($all)
    $running = @()
    while ($queue.Count -or $running.Count) {
        while ($queue.Count -and $running.Count -lt $Parallel) {
            $s = $queue[0]; $queue.RemoveAt(0)
            $r = Start-Run $s $builds[0] $mode 0
            if ($r) { $running += $r; Write-Host "started $($s.name) -> $($r.dir)" }
        }
        Start-Sleep -Seconds 2
        $still = @()
        foreach ($r in $running) {
            $done = $r.proc.HasExited
            if (-not $done -and $r.rec -and (Select-String -Path (Join-Path $r.dir 'run.log') -Pattern 'GS recording complete' -Quiet)) { Wait-Run $r; $done = $true }
            if ($done) {
                Wait-Run $r
                if ($mode -eq 'scout') {
                    $fr = @(Get-ChildItem (Join-Path $r.dir 'frames') -Filter 'frame_*.png' | Sort-Object Name)
                    if ($fr.Count) { & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'chaos_sheet.ps1') -Dir $r.dir -Cols 8 -Width 240 | Out-Null }
                    & $py (Join-Path $PSScriptRoot 'chaos_sum.py') $r.dir --name "$($r.scene.name) (scout, whole run)"
                } else {
                    $sz = if (Test-Path $r.rec) { [int]((Get-Item $r.rec).Length / 1MB) } else { 0 }
                    $m = Select-String -Path (Join-Path $r.dir 'run.log') -Pattern 'recording the GS stream from VBlank (\d+) for (\d+)' | Select-Object -First 1
                    Write-Host "$($r.scene.name): $($r.rec) ($sz MB) $(if ($m) { $m.Line })"
                }
            } else { $still += $r }
        }
        $running = $still
    }
    exit 0
}

$outTxt = Join-Path $root "build\chaos\$Tag.txt"
$outJs = Join-Path $root "build\chaos\$Tag.jsonl"
New-Item -ItemType Directory -Force (Split-Path $outTxt) | Out-Null
for ($rep = 0; $rep -lt $Repeat; $rep++) {
    foreach ($s in $all) {
        $order = if ($rep % 2) { @($builds[($builds.Count - 1)..0]) } else { $builds }
        foreach ($b in $order) {
            if ([math]::Round((Get-PSDrive C).Free / 1GB, 1) -lt 50) { "C: below 50 GB free: stopping"; exit 1 }
            $r = Start-Run $s $b 'bench' $rep
            if (-not $r) { continue }
            Wait-Run $r
            $line = & $py (Join-Path $PSScriptRoot 'chaos_sum.py') $r.dir --from $s.from --to $s.to --name "$($s.name)@$($b.name)#$rep" --json $outJs
            $line | Add-Content $outTxt
            $line
        }
    }
}
# medians over the repeats, per scene and build
& $py -c @"
import json, statistics, sys, collections
rows = [json.loads(l) for l in open(r'$outJs')]
g = collections.defaultdict(list)
for r in rows:
    if 'fps_med' not in r: continue
    sc, rest = r['name'].split('@', 1); b = rest.split('#')[0]
    g[(sc, b)].append(r)
keys = ['fps_med', 'low1', 'low01', 'worker', 'gs', 'fr', 'rc', 'gpu']
print('scene | build | n | fps med | 1% low | 0.1% low | worker | GS | front | render | GPU')
for (sc, b), rs in sorted(g.items()):
    m = {k: statistics.median([r.get(k, 0) for r in rs]) for k in keys}
    print(f"{sc} | {b} | {len(rs)} | {m['fps_med']:.1f} | {m['low1']:.1f} | {m['low01']:.1f} | {m['worker']:.2f} | {m['gs']:.2f} | {m['fr']:.2f} | {m['rc']:.2f} | {m['gpu']:.2f}")
"@
