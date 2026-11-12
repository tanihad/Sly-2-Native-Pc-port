# (tex-opt) Prototype of a one-time "prepare the Reimagined textures of every level" pass (notes/reimagined.md "Ready before
# first sight", option b; measurements: build\texopt\fullbuild\summary.md): levels are warped into with scripts\warp_run.ps1
# (headless, Reimagined on, a big AI budget), the camera sweeps with scripted pad input while the net builds the AI images,
# and the cache (<UserDir>\textures: images + level lists) carries over from one run to the next.
#   .\scripts\ai_prepare_all.ps1 -Levels ep1,ep2,m1_brawl,w9 [-Seconds 90] [-PerRun 1] [-UserDir build\prepare\user]
#        [-Exe <sly2.exe>] [-FrameRate 30] [-AiMs 24] [-Size 1280x720] [-Weights <x4plus.bin>] [-Extra "K=V;K=V"]
#        [-Skip 2400] [-Settle 20] [-PadSweep "<pad tokens>"] [-Tag prep] [-WaitQuiet <minutes>] [-Quiet]
# A level is: ep<N> (the episode's hub: START, X at the safehouse), a job name from `warp.py list` (m3_boss, ...: started
# the way the game does it, in its own world), or w<world> (that world at its default start point; world ids 0..43: start
# world column of scripts\warp_tasks.txt). -Seconds: game seconds per level (60 VBlanks a second; a level's slot).
# -PerRun n: n levels per game process (the first one from the title, the others by level travel, PS2_TASK_SET j<task>:<world>
# / w<world>, runtime/warp.c: no new boot + title, ~35 s saved); 1 = a process per level (safest: a crashed job costs
# only itself). -FrameRate / -AiMs: the net's budget per picture is capped at a quarter of the frame interval unless
# PS2_REIM_FIX=0 (-Extra), so the default is the fastest drain measured: frame rate 30, PS2_REIM_FIX=0, 24 ms (a net
# share of ~50 % of the GPU). -Skip: pad frames of X presses before each level's camera sweep (briefings / intro cutscenes:
# ep1's holds Sly for ~4000 frames otherwise). -Settle: seconds to keep the last process running for the queue to drain.
# -UserDir: the folder to fill (its textures\ subfolder = the Reimagined cache with the level lists; every run works in its
# own build\warp\<name> folder, never a real user folder). Needs the weights (-Weights, PS2_AI_WEIGHTS or
# build\models\x4plus.bin). Timings are only clean without other sly2* processes (-WaitQuiet n: wait up to n minutes for
# them to end before a run; the Others column counts the ones seen at the start). -Exe: a sly2.exe (default build\rel).
# Prints per level: its world, the seconds from the process start to its first 3D frame, the textures listed for it, and
# per run the images in the cache before / after, the net's builds, TFLOP, GPU ms per GFLOP and the queue left; the
# timeline (seconds, built, queue, TFLOP) is kept in <UserDir>\timeline_<run>.csv. Nothing is committed or shipped: the
# generated images are derived from the game's data.
param([string[]]$Levels = @('ep1'), [int]$Seconds = 90, [int]$PerRun = 1, [string]$UserDir = '', [string]$Exe = '', [string]$Weights = '',
      [double]$AiMs = 24, [string]$FrameRate = '30', [string]$Size = '1280x720', [string]$Extra = 'PS2_REIM_FIX=0', [int]$Settle = 0,
      [string]$PadSweep = '', [string]$Tag = 'prep', [int]$WaitQuiet = 0, [int]$Skip = 2400, [switch]$Quiet)
$wt = Split-Path $PSScriptRoot -Parent
function Full($p) { if ([IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $wt $p } }   # (never the process cwd)
$main = $wt
if (-not (Test-Path (Join-Path $wt 'orig\SCUS_973.16'))) {   # worktree: the main checkout has the tools
    $gd = ((Get-Content (Join-Path $wt '.git') -Raw) -replace '^gitdir:\s*', '').Trim()
    $main = Split-Path (Split-Path (Split-Path $gd -Parent) -Parent) -Parent
}
$py = Join-Path $main '.venv\Scripts\python.exe'
if (-not $UserDir) { $UserDir = 'build\prepare\user' }
$UserDir = Full $UserDir
$seed = Join-Path $UserDir 'textures'
New-Item -ItemType Directory -Force $seed | Out-Null
if (-not $Weights) { $Weights = if ($env:PS2_AI_WEIGHTS) { $env:PS2_AI_WEIGHTS } else { Join-Path $wt 'build\models\x4plus.bin' } }
if (-not (Test-Path $Weights)) { "no weights at $Weights"; exit 2 }
$env:PS2_AI_WEIGHTS = $Weights
if ($Exe) { $env:M9_GAME_EXE = Full $Exe }
$free = [math]::Round((Get-PSDrive C).Free / 1GB, 1)
if ($free -lt 40) { "C: has $free GB free (< 40): not starting"; exit 2 }

$hubWorld = @(0, 2, 8, 12, 14, 17, 27, 32, 38)           # episode -> its hub's world (notes/warp.md)
$vbps = 60                                              # VBlanks per second (59.94 Hz at every frame rate setting: measured, 30 fps gives a game frame every 2nd VBlank)
$slot = $Seconds * $vbps
# the sweep: walk + camera turns (the right stick turns the camera); one cycle ~5800 pad frames
$cycle = @('+300:lup:400', '+0:rright:300', '+400:lleft:200', '+200:rleft:500', '+200:lup:600', '+600:ldown:600', '+0:rright:400', '+600:lright:300',
           '+300:lup:900', '+300:rleft:600', '+600:ldown:900', '+900:lleft:600', '+0:rright:600', '+600:lup:900')
# the pad tokens of one level's slot: X presses, then the sweep, then a filler so that the slot's tokens add up to $frames
function SlotPad([int]$frames) {
    $tok = @(); $sum = 0
    $n = [int][math]::Floor([math]::Min($Skip, $frames / 3) / 60)
    for ($i = 0; $i -lt $n; $i++) { $tok += '+60:cross:6'; $sum += 60 }
    if ($PadSweep) { $tok += $PadSweep; return ($tok -join ',') }
    :cyc while ($true) { foreach ($c in $cycle) { $d = [int]($c -split ':')[0].TrimStart('+'); if ($sum + $d -ge $frames) { break cyc }; $tok += $c; $sum += $d } }
    if ($frames - $sum -gt 0) { $tok += "+$($frames - $sum):none" }
    return ($tok -join ',')
}
function CacheCount { @(Get-ChildItem -Path $seed -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in '.jpg', '.png' }).Count }
function Num($s, $re) { if ($s -match $re) { [double]$Matches[1] } else { $null } }
function EpisodeOf($lv) { if ($lv -match '^ep(\d)$') { [int]$Matches[1] } elseif ($lv -match '^[mt](\d)_') { [int]$Matches[1] } elseif ($lv -match '^w\d+:(\d)$') { [int]$Matches[1] } else { 1 } }
# a level's PS2_TASK_SET item: w<world> (a hub, a world) or j<task oid>:<world> (a job: warp.py prints the oid it starts)
function TaskItem($lv, $scratch) {
    if ($lv -match '^ep(\d)$') { return "w$($hubWorld[[int]$Matches[1]])" }
    if ($lv -match '^w(\d+)') { return "w$($Matches[1])" }
    $o = & $py (Join-Path $wt 'scripts\warp.py') make --job $lv --direct --card (Join-Path $scratch "card_$lv") 2>&1 | Out-String
    if ($o -match 'PS2_WARP_TASK=(\d+):(\d+)') { return "j$($Matches[1]):$($Matches[2])" }
    if ($o -match 'PS2_WARP_TASK=(\d+)') { return "j$($Matches[1]):$($hubWorld[(EpisodeOf $lv)])" }
    return $null
}

# watches a run's log and writes <csv>: elapsed seconds, kind, values (ai: built, from cache, queue, TFLOP, GPU ms; first3d: world, frame; listed: world, n)
$watcher = {
    param($log, $csv)
    while (-not (Test-Path $log)) { Start-Sleep -Milliseconds 200 }
    $t0 = Get-Date; $seen = 0
    while ($true) {
        try {
            $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); $sr = New-Object IO.StreamReader($fs)
            $all = $sr.ReadToEnd(); $sr.Close(); $fs.Close()
            $ls = $all -split "`r?`n"
            for ($i = $seen; $i -lt $ls.Count - 1; $i++) {
                $l = $ls[$i]; $t = ((Get-Date) - $t0).TotalSeconds
                if ($l -match 'ai: (\d+) built by the net .*?(\d+) from the cache.*?(\d+) in the net.s queue.*?([\d.]+) TFLOP, GPU (\d+) ms') {
                    Add-Content $csv ('{0:N1},ai,{1},{2},{3},{4},{5}' -f $t, $Matches[1], $Matches[2], $Matches[3], $Matches[4], $Matches[5]) }
                elseif ($l -match 'ai-pop: frame (\d+) .* world (\d+): first 3D frame') { Add-Content $csv ('{0:N1},first3d,{1},{2}' -f $t, $Matches[2], $Matches[1]) }
                elseif ($l -match 'ai-pre: .*frame (\d+) level (\d+): (\d+) listed') { Add-Content $csv ('{0:N1},listed,{1},{2}' -f $t, $Matches[2], $Matches[3]) }
            }
            $seen = [math]::Max($seen, $ls.Count - 1)
        } catch {}
        Start-Sleep -Milliseconds 500
    }
}

$rows = @(); $runs = @()
for ($g = 0; $g -lt $Levels.Count; $g += $PerRun) {
    $grp = @($Levels[$g..([math]::Min($g + $PerRun, $Levels.Count) - 1)])
    $name = "$($Tag)_$($grp[0] -replace '[^A-Za-z0-9_]', '_')$(if ($grp.Count -gt 1) { "_x$($grp.Count)" })"
    $dir = Join-Path $wt "build\warp\$name"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $iniText = "[graphics]`r`nlook = reimagined`r`nrender_scale = 100%`r`n`r`n[display]`r`nframe_rate = $FrameRate`r`naspect = 16:9`r`n"
    [IO.File]::WriteAllText((Join-Path $dir 'sly2.ini'), $iniText)
    $wa = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'warp_run.ps1'), '-Release', '-Name', $name, '-DumpEvery', '1000000')
    $first = $grp[0]
    $ep = EpisodeOf $first
    $items = @(); $vb0 = 2000
    if ($first -match '^ep\d$') { $wa += @('-Episode', "$ep", '-Play'); $lead = '+2000:cross:6' }
    elseif ($first -match '^w(\d+)') { $wa += @('-Episode', "$ep"); $items += "w$($Matches[1])@2400"; $lead = '+2400:none'; $vb0 = 2400 }   # (after the Resume's hub has loaded)
    else { $wa += @('-Episode', "$ep", '-Job', $first, '-Direct'); $lead = '+1500:none' }
    $pad = @($lead, (SlotPad $slot))
    for ($i = 1; $i -lt $grp.Count; $i++) {
        $it = TaskItem $grp[$i] $dir
        if (-not $it) { "$($grp[$i]): no start for this level (warp.py): skipped"; continue }
        $items += "$it@$($vb0 + $i * $slot)"
        $pad += (SlotPad $slot)
    }
    $total = 50 + $grp.Count * ($Seconds + 25) + $Settle
    $wa += @('-Seconds', "$total", '-Pad', ($pad -join ','))
    for ($k = 0; $k -lt $WaitQuiet * 2 -and @(Get-Process -Name 'sly2*' -ErrorAction SilentlyContinue).Count; $k++) { Start-Sleep -Seconds 30 }
    $before = CacheCount
    $others = @(Get-Process -Name 'sly2*' -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne "sly2_warp_$name" }).Count
    $env:PS2_TEXCACHE_SEED = $seed
    $env:PS2_TEX_AI = '1'; $env:PS2_AI_MS = "$AiMs"; $env:PS2_AI_LOG = '1'; $env:PS2_AI_POPLOG = '1'
    $env:PS2_GS_PRESENT_SIZE = $Size; $env:PS2_USER_DIR = $dir
    if ($items.Count) { $env:PS2_TASK_SET = ($items -join ',') }
    $saved = @{}
    foreach ($e in ($Extra -split ';' | Where-Object { $_ })) { $a, $b = $e -split '=', 2; $saved[$a.Trim()] = [Environment]::GetEnvironmentVariable($a.Trim()); [Environment]::SetEnvironmentVariable($a.Trim(), $b) }
    $csv = Join-Path $UserDir "timeline_$($name).csv"
    Remove-Item $csv -ErrorAction SilentlyContinue
    $watch = Start-Job -ScriptBlock $watcher -ArgumentList (Join-Path $dir 'run.log'), $csv
    $t0 = Get-Date
    $out = & powershell @wa
    $secs = ((Get-Date) - $t0).TotalSeconds
    Start-Sleep -Seconds 1; Stop-Job $watch; Remove-Job $watch -Force
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    Remove-Item Env:\PS2_TASK_SET, Env:\PS2_TEXCACHE_SEED -ErrorAction SilentlyContinue
    $log = Join-Path $dir 'run.log'
    $L = if (Test-Path $log) { Get-Content $log } else { @() }
    $ai = @($L | Where-Object { $_ -match 'ai: \d+ built by the net' }) | Select-Object -Last 1
    $tm = if (Test-Path $csv) { @(Get-Content $csv | ForEach-Object { $f = $_ -split ','; [pscustomobject]@{ t = [double]$f[0]; kind = $f[1]; a = $f[2]; b = $f[3]; c = $f[4]; d = $f[5] } }) } else { @() }
    # per level change (first 3D frame of a world other than the title's): the textures listed for it until the next change
    $ev = @($tm | Where-Object { $_.kind -eq 'first3d' -and $_.a -ne '0' })
    for ($e = 0; $e -lt $ev.Count; $e++) {
        $tEnd = if ($e + 1 -lt $ev.Count) { $ev[$e + 1].t } else { 1e9 }
        $lst = @($tm | Where-Object { $_.kind -eq 'listed' -and $_.a -eq $ev[$e].a -and $_.t -ge $ev[$e].t -and $_.t -le $tEnd + 2 }) | Select-Object -Last 1
        $rows += [pscustomobject]@{ Run = $name; World = $ev[$e].a; First3D_s = [math]::Round($ev[$e].t); Listed = if ($lst) { $lst.b } else { '' } }
    }
    $built = Num $ai '(\d+) built by the net'; $tf = Num $ai '([\d.]+) TFLOP'; $gms = Num $ai 'GPU (\d+) ms in all'
    $after = CacheCount
    $r = [pscustomobject]@{ Run = $name; Levels = $grp.Count; Cache0 = $before; Cache1 = $after; Built = $built; TFLOP = $tf
                            msPerGFLOP = if ($tf) { [math]::Round($gms / ($tf * 1000), 3) } else { $null }
                            Queue_left = Num $ai '(\d+) in the net.s queue'; Run_s = [math]::Round($secs); Others = $others }
    $runs += $r
    if (-not $Quiet) { ($r | Format-List | Out-String).TrimEnd() | Write-Host }
    Remove-Item (Join-Path $dir 'sly2_warp_*.exe') -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force (Join-Path $dir 'frames'), (Join-Path $dir 'textures'), (Join-Path $dir 'card') -ErrorAction SilentlyContinue
}
($rows | Format-Table -AutoSize | Out-String -Width 200).TrimEnd()
($runs | Format-Table -AutoSize | Out-String -Width 250).TrimEnd()
"cache: $seed ($(CacheCount) images)"
