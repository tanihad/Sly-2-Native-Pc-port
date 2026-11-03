# (r2-m2) The modern renderer's sweep (notes/modern-renderer.md 5, levels 2-3) on every episode hub and a few jobs,
# -Parallel runs at a time, one line per scene and check:
#   ab  scripts\r2_ab.ps1: a bracket recording of one frame replayed exact and hybrid, compared against tests\r2\gates.json
#       (entries bracketed, the verdict, the S = 1 numbers of the main frame buffer)
#   hc  (r2-m5) scripts\r2_hcheck.ps1: the game in hybrid mode with hybrid and all-exact frames alternating; what only one
#       mode draws alternates between consecutive pictures. Catches what the A/B cannot (it replays one GS stream): the
#       VU1 / VIF / GS state dropped mesh DMA leaves for later exact draws, stale mesh decodes, hint / slot races.
#   shots (r2-m8) the scene's modern frames at 2560 x 1440 with MSAA 4x: one picture (build\r2hc\shot_<scene>\shot.png)
#   tf  (r2-flicker) scripts\flicker_run.ps1: the game in modern mode, every picture of 180 VBlanks dumped; temporal
#       outliers (one-picture blinks, a blinking line / row band, blobs, blinking seams) against tests\r2\gates.json
#       level3_temporal - what the one-frame A/B and hc's averaged alternation can miss.
#   mf  (mft-pop) scripts\mft_pops.ps1: the game with R8's modern frame targets at 1440p MSAA 4x, 120 fps, no dumps;
#       PS2_R2_STATS counts "pops" (1..8 grid frames between MFT frames: a one-frame resolution / AA switch); must be 0.
#   .\scripts\r2_sweep.ps1 [-Scenes ep1,ep3] [-Checks ab,hc,tf,mf,shots] [-Parallel 3] [-Keep]
# Output: build\r2ab\<scene>\, build\r2hc\sw_<scene>\worst.png (deleted unless -Keep / on failure).
param([string[]]$Scenes = @(), [string[]]$Checks = @('ab', 'hc', 'tf', 'mf'), [int]$Parallel = 3, [switch]$Keep)
$Scenes = @($Scenes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Checks = @($Checks | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path $PSScriptRoot -Parent
$list = @(
    @{ n = 'ep1'; a = @('-Episode', '1', '-Job', 'm1_brawl', '-Play', '-At', '4800') },
    @{ n = 'ep2'; a = @('-Episode', '2', '-Play', '-At', '4800') },
    @{ n = 'ep3'; a = @('-Episode', '3', '-Play', '-At', '5000') },
    @{ n = 'ep4'; a = @('-Episode', '4', '-Play', '-At', '4800') },
    @{ n = 'ep5'; a = @('-Episode', '5', '-Play', '-At', '4800') },
    @{ n = 'ep6'; a = @('-Episode', '6', '-Play', '-At', '4800') },
    @{ n = 'ep7'; a = @('-Episode', '7', '-Play', '-At', '4800') },
    @{ n = 'ep8'; a = @('-Episode', '8', '-Play', '-At', '4800') },
    @{ n = 'm2_boardroom'; a = @('-Episode', '2', '-Job', 'm2_boardroom_brawl', '-Direct', '-At', '5400') },
    @{ n = 'm3_boss'; a = @('-Episode', '3', '-Job', 'm3_boss', '-Direct', '-At', '6600') },
    @{ n = 'm8_boss'; a = @('-Episode', '8', '-Job', 'm8_boss', '-Direct', '-At', '5200', '-Pad', '+2600:lup:90') }
)
if ($Scenes.Count) { $list = @($list | Where-Object { $Scenes -contains $_.n }) }
$free = [math]::Round((Get-PSDrive C).Free / 1GB, 1)
if ($free -lt 50) { "C: has $free GB free (< 50): not starting"; exit 1 }
$logs = Join-Path $root 'build\r2ab\sweep'
New-Item -ItemType Directory -Force $logs | Out-Null
$queue = [System.Collections.ArrayList]@()
foreach ($s in $list) { foreach ($c in $Checks) { [void]$queue.Add(@{ s = $s; c = $c }) } }
$total = $queue.Count
$running = @()
$fail = 0
while ($queue.Count -or $running.Count) {
    while ($queue.Count -and $running.Count -lt $Parallel) {
        $t = $queue[0]; $queue.RemoveAt(0)
        $s = $t.s
        $log = Join-Path $logs "$($s.n)_$($t.c).txt"
        if ($t.c -eq 'ab') {
            $args1 = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'r2_ab.ps1'), '-Name', "sw_$($s.n)", '-Levels', 'all') + $s.a
        } elseif ($t.c -eq 'mf') {
            $args1 = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'mft_pops.ps1'), '-Name', "sw_$($s.n)") + $s.a
        } elseif ($t.c -eq 'tf') {
            $args1 = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'flicker_run.ps1'), '-Name', "sw_$($s.n)", '-Frames', '180') + $s.a
        } else {
            $args1 = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'r2_hcheck.ps1'), '-Name', "sw_$($s.n)") + $s.a
            if ($t.c -eq 'shots') { $args1[5] = "shot_$($s.n)"; $args1 += @('-Shots', '-Frames', '4') }   # (r2-m8: modern frames at 1440p MSAA 4x)
        }
        $p = Start-Process -FilePath 'powershell' -ArgumentList $args1 -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru -NoNewWindow
        $running += [pscustomobject]@{ p = $p; s = $s; c = $t.c; log = $log }
    }
    Start-Sleep -Seconds 5
    $still = @()
    foreach ($r in $running) {
        if (-not $r.p.HasExited) { $still += $r; continue }
        $t = Get-Content $r.log -ErrorAction SilentlyContinue
        if ($r.c -eq 'mf') {               # (mft-pop) no MFT grid fallback in gameplay
            $g = ($t | Select-String '^mftpops:' | Select-Object -Last 1)
            $gl = if ($g) { $g.Line } else { 'mftpops: (no result)' }
            if ($gl -notmatch 'PASS') { $fail++ }
            "{0,-13} mf | {1}" -f $r.s.n, $gl
            continue
        }
        if ($r.c -eq 'tf') {               # (r2-flicker) temporal outliers
            $g = ($t | Select-String '^flicker:' | Select-Object -Last 1)
            $gl = if ($g) { $g.Line } else { 'flicker: (no result)' }
            if ($gl -notmatch 'PASS') { $fail++ }
            elseif (-not $Keep) { Remove-Item -Recurse -Force (Join-Path $root "build\flicker\sw_$($r.s.n)") -ErrorAction SilentlyContinue }
            "{0,-13} tf | {1}" -f $r.s.n, $gl
            Remove-Item -Recurse -Force (Join-Path $root "build\warp\flk_sw_$($r.s.n)\frames") -ErrorAction SilentlyContinue
            continue
        }
        if ($r.c -eq 'shots') {            # (r2-m8) the picture to look at: build\r2hc\shot_<scene>\shot.png
            $sp = Join-Path $root "build\r2hc\shot_$($r.s.n)\shot.png"
            if (-not (Test-Path $sp)) { $fail++ }
            "{0,-13} shots | {1}" -f $r.s.n, $(if (Test-Path $sp) { $sp } else { '(no picture)' })
            Remove-Item -Recurse -Force (Join-Path $root "build\warp\r2hc_shot_$($r.s.n)\frames") -ErrorAction SilentlyContinue
            continue
        }
        if ($r.c -eq 'hc') {
            $g = ($t | Select-String '^hcheck:' | Select-Object -Last 1)
            $gl = if ($g) { $g.Line } else { 'hcheck: (no result)' }
            if ($gl -notmatch 'PASS') { $fail++ }
            elseif (-not $Keep) { Remove-Item -Recurse -Force (Join-Path $root "build\r2hc\sw_$($r.s.n)") -ErrorAction SilentlyContinue }
            "{0,-13} hc | {1}" -f $r.s.n, $gl
            Remove-Item -Recurse -Force (Join-Path $root "build\warp\r2hc_sw_$($r.s.n)\frames") -ErrorAction SilentlyContinue
            continue
        }
        $g = ($t | Select-String '^gates:' | Select-Object -Last 1)
        $f = ($t | Select-String 'frame 0x38: ' | Select-Object -Last 1)
        $z = ($t | Select-String ' Z 0x70: ' | Select-Object -Last 1)
        $tr = Get-ChildItem (Join-Path $root "build\r2trace\ab_sw_$($r.s.n)") -Filter 'frame_0.txt' -ErrorAction SilentlyContinue
        $ent = if ($tr) { (Get-Content $tr.FullName -TotalCount 1) -replace '^.*entries (\d+).*$', '$1' } else { '?' }
        $gl = if ($g) { $g.Line } else { 'gates: (no result)' }
        if ($gl -notmatch 'PASS') { $fail++ }
        "{0,-13} ab entries {1,4} | {2} | {3} | {4}" -f $r.s.n, $ent, $gl, $(if ($f) { $f.Line -replace '^v\d+ frame 0x38: ', '' -replace ';.*alpha bit', '; alpha bit' } else { '' }),
            $(if ($z) { $z.Line -replace '^v\d+ ', '' } else { '' })
        if (-not $Keep) {
            Remove-Item -Recurse -Force (Join-Path $root "build\r2ab\sw_$($r.s.n)"), (Join-Path $root "build\r2trace\ab_sw_$($r.s.n)") -ErrorAction SilentlyContinue
            Remove-Item -Recurse -Force (Join-Path $root "build\warp\r2_ab_sw_$($r.s.n)\frames") -ErrorAction SilentlyContinue
        }
    }
    $running = $still
}
"sweep: $($total - $fail) of $total checks pass"
exit $(if ($fail) { 1 } else { 0 })
