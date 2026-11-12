# (shadows) Look = Reimagined's shadow-overlap check (notes/reimagined-lighting.md "Shadows rework"): what the owner saw as
# "two shadows collide and flick" / "Sly running under a tree's shadow is not smooth". Two game runs at once on the owner's
# settings (1440p, render scale 100 %, MSAA 2x, thin outlines, filters modern, motion blur off, 165 fps) on the ep2 trees
# path of reim_stability_gate (deterministic inside a run: START anchored, one X, PS2_FRAME_DT_FIXED, the dump window a pad
# token); in each, the old shadows (A: the rework's path off) and the rework (B) alternate every 6 pictures
# (PS2_GPU_AB_R8=4096, PS2_REIM_X=64): both see the same camera path. The pictures are debug views of the post pass:
#   view 8  - the world's shadows alone: the game's own shadow darkening x the key light's shadow (no textures, occlusion,
#             relight or grade; characters black). Lit = 128, one shadow ~87, two stacked ~55.
#   view 10 - the light the post pass gives the game-lit (characters) alone (everything else black).
# Measured with scripts/stability_check.c in Sly's region (the middle-bottom of the picture, x 0.33..0.67, y 0.40..0.95):
# reversals of 8 x 8 block luma over triples of consecutive pictures of one phase, per 1000 blocks - view 8 at a shadow-sized
# step (12 / 255) with the share of blocks darker than one shadow can make (< 70: stacked shadows), view 10 at 6 / 255 (the
# character's shading pulsing frame to frame).
# (shadows2) View 8 paints the game-lit (characters) in a pure-red marker: the world-shadow number leaves out the blocks at
# and next to them ("mask") - Sly's silhouette moving over the shadowed ground made most of the old number's reversals (one
# triple of his legs over a shadow: 40+ of a run's ~400). The old number (markers read as the flat grey) is printed too.
# The world number counts reversals at 6 / 255 with at most 5 a triple: a content pop (a caster the game draws for one
# frame) covers dozens of blocks at once and lands in an A or a B phase by chance - uncapped at 12 / 255 the same build gave
# 0.57..1.26 x over three runs, capped at 6 / 255 0.95..0.98 x.
# (shadows2) One pair of runs is not a verdict: the world-shadow ratio of single pairs spread 0.70..1.56 x on one build (the
# events come in bursts - a few frames of one shadow edge - that fall into A or B phases by chance). -Runs pairs (default 3)
# are made one after the other and the verdict is on the MEDIAN of the per-run numbers (the spread is printed): passes
# when the rework's median stacked share is at most half the old's, its median world-shadow ratio at most -Limit8 and its
# median character-light ratio at most -Limit.
#   powershell -ExecutionPolicy Bypass -File scripts\reim_shadow_gate.ps1 [-Exe build\rel\sly2.exe] [-Runs 3] [-Limit 0.85]
#              [-Limit8 0.8] [-Keep] [-Heat] [-Extra "K=V;K=V"] [-Tag t]
# -Heat writes each phase's reversal map (stability_check heat.pgm: one pixel per 8 x 8 block) into build\light\shadow_gate_heat.
param([string]$Exe = '', [double]$Limit = 0.85, [double]$Limit8 = 0.8, [int]$Runs = 3, [switch]$Keep, [switch]$Heat,
      [int]$Frames = 850, [string]$Extra = '', [string]$Tag = '')
$wt = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'project.ps1')
$chk = Join-Path $wt 'build\stability_check.exe'
if (-not (Test-Path $chk) -or (Get-Item $chk).LastWriteTime -lt (Get-Item (Join-Path $PSScriptRoot 'stability_check.c')).LastWriteTime) {
    & $ProjectClang -O2 -o $chk (Join-Path $PSScriptRoot 'stability_check.c')
}
if ($Exe) { $env:M9_GAME_EXE = (Resolve-Path $Exe).Path }
$w = Join-Path $wt 'build\models\x4plus.bin'
if (Test-Path $w) { $env:PS2_AI_WEIGHTS = $w }
if (-not $env:PS2_TEXCACHE_SEED) { $env:PS2_TEXCACHE_SEED = Join-Path $wt 'build\reimagine\texcache' }
$ini = 'graphics.look=reimagined;graphics.lighting=enhanced;graphics.textures=enhanced;graphics.render_scale=100%;graphics.msaa=2x;' +
       'graphics.outlines=thin;graphics.filters=modern;graphics.motion_blur=off;graphics.preset=custom'
$pad = "+2000:cross:6,+1000:lup:400,+0:lleft:200,+50:dump:$Frames,+350:lup:600"   # (reim_stability_gate's trees: a torch, then the trees' shadows)
$views = @(8, 10)
$reg = '0.33,0.40,0.67,0.95'
$hd = Join-Path $wt 'build\light\shadow_gate_heat'
if ($Heat) { New-Item -ItemType Directory -Force $hd | Out-Null }
$res = @()
for ($run = 1; $run -le $Runs; $run++) {
    $free = (Get-PSDrive C).Free / 1GB
    if ($free -lt 60) { "C: has $([math]::Round($free)) GB free (< 60): not starting run $run"; exit 2 }
    $ps = @()
    foreach ($v in $views) {
        $ex = "PS2_LIGHT=;PS2_LIGHT_LOG=;PS2_DUMP_BMP=1;PS2_DUMP_WAIT=1;PS2_GPU_AB_R8=4096;PS2_REIM_X=64;PS2_GPU_AB_LOG=1;PS2_REIM_DBG=$v;" +
              "PS2_DUMP_FROM=script;PS2_DUMP_TO=0;PS2_FRAME_DT_FIXED=1;PS2_REIM_LOG=1"
        if ($Extra) { $ex += ";$Extra" }
        $a = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'light_shots.ps1'), '-Scene', 'ep2_hub', '-At', '1', '-Frames', '1',
               '-Every', '1', '-Phase', '6', '-Fps', '165', '-Seconds', "$([int](240 + $Frames * 0.1))", '-Tag', "shadow_gate$Tag$(if ($Keep) { "r$run" })$v", '-Ini', $ini,
               '-Extra', "`"$ex`"", '-PadAll', "`"$pad`"")
        $ps += Start-Process powershell -ArgumentList $a -PassThru -WindowStyle Hidden
    }
    $ps | Wait-Process -Timeout 1200
    $m = @{}
    foreach ($v in $views) {
        $d = Join-Path $wt "build\light\shots\ep2_hub__shadow_gate$Tag$(if ($Keep) { "r$run" })$v"
        $ph = @(Select-String -Path (Join-Path $d 'run.log') -Pattern 'ab: phase ([AB]) from v(\d+)' | ForEach-Object { @{ p = $_.Matches[0].Groups[1].Value; v = [int]$_.Matches[0].Groups[2].Value } })
        foreach ($s in 'A', 'B') { New-Item -ItemType Directory -Force (Join-Path $d $s) | Out-Null }
        foreach ($f in Get-ChildItem $d -Filter 'frame_*.bmp') {
            $n = [int]($f.BaseName -replace 'frame_', '')
            $cur = $null; $start = 0
            foreach ($x in $ph) { if ($x.v -le $n) { $cur = $x.p; $start = $x.v } }
            if (-not $cur -or $n - $start -lt 1) { continue }   # (the first picture after a switch: the other mode's history)
            Move-Item $f.FullName (Join-Path $d "$cur\$($f.Name)")
        }
        foreach ($s in 'A', 'B') {
            $sd = Join-Path $d $s
            $n = @(Get-ChildItem $sd -Filter 'frame_*.bmp').Count
            if ($n -lt 100) { "FAIL: run $run view $v phase $s has $n pictures"; exit 1 }
            $k = "$v$s"
            $hp = if ($Heat) { Join-Path $hd "r$run`_v$v`_$s.pgm" } else { '-' }
            if ($v -eq 8) {
                $r1 = & $chk $sd 12 70 '-' 0.12 $reg marks
                $x = [regex]::Match("$r1", 'flicker ([\d.]+) per 1000 \(all\), [\d.]+ per 1000 \(dark < 70, ([\d.]+)% .*motion ([\d.]+)')
                if (-not $x.Success) { "FAIL: no measurement (run $run, view $v, $s): $r1"; exit 1 }
                $r2 = & $chk $sd 6 70 $hp 0.12 $reg mask 8 5   # (the world alone: blocks at / around the characters left out, 6 / 255, <= 5 a triple)
                $x2 = [regex]::Match("$r2", 'capped \(5 a triple\): flicker ([\d.]+) per 1000')
                if (-not $x2.Success) { "FAIL: no masked measurement (run $run, view $v, $s): $r2"; exit 1 }
                $r3 = & $chk $sd 12 70 '-' 0.12 $reg mask   # (the same at 12 / 255, uncapped: printed)
                $x3 = [regex]::Match("$r3", 'flicker ([\d.]+) per 1000 \(all\)')
                $m[$k] = @{ fl = [double]$x2.Groups[1].Value; f12 = [double]$x3.Groups[1].Value; fls = [double]$x.Groups[1].Value; stacked = [double]$x.Groups[2].Value; mo = [double]$x.Groups[3].Value; n = $n }
            } else {
                $r1 = & $chk $sd 6 64 $hp 0.12 $reg
                $x = [regex]::Match("$r1", 'flicker ([\d.]+) per 1000 \(all\).*motion ([\d.]+)')
                if (-not $x.Success) { "FAIL: no measurement (run $run, view $v, $s): $r1"; exit 1 }
                $m[$k] = @{ fl = [double]$x.Groups[1].Value; mo = [double]$x.Groups[2].Value; n = $n }
            }
        }
        if (-not $Keep) { Remove-Item -Recurse -Force (Join-Path $d 'A'), (Join-Path $d 'B') -ErrorAction SilentlyContinue; Get-ChildItem $d -Filter 'frame_*' | Remove-Item -Force }
    }
    $r = [pscustomobject]@{ run = $run; stA = $m['8A'].stacked; stB = $m['8B'].stacked; w8A = $m['8A'].fl; w8B = $m['8B'].fl;
                            r8 = $m['8B'].fl / [math]::Max($m['8A'].fl, 1e-6); s8A = $m['8A'].fls; s8B = $m['8B'].fls;
                            r8s = $m['8B'].fls / [math]::Max($m['8A'].fls, 1e-6); r12 = $m['8B'].f12 / [math]::Max($m['8A'].f12, 1e-6); c10A = $m['10A'].fl; c10B = $m['10B'].fl;
                            r10 = $m['10B'].fl / [math]::Max($m['10A'].fl, 1e-6); moA = $m['8A'].mo; moB = $m['8B'].mo }
    $res += $r
    "run $run`: stacked $($r.stA)% -> $($r.stB)%, world-shadow flicker $($r.w8A) -> $($r.w8B) per 1000 ($([math]::Round($r.r8, 3))x; with the " +
    "12 / 255 uncapped $([math]::Round($r.r12, 3))x, with the characters' silhouettes $($r.s8A) -> $($r.s8B), $([math]::Round($r.r8s, 3))x), " +
    "character-light flicker $($r.c10A) -> $($r.c10B) ($([math]::Round($r.r10, 3))x), motion $($r.moA) / $($r.moB)"
}
Remove-Item Env:\M9_GAME_EXE -ErrorAction SilentlyContinue
function med($a) { $s = @($a | Sort-Object); if ($s.Count % 2) { $s[($s.Count - 1) / 2] } else { ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2 } }
function rng($a) { $s = @($a | Sort-Object); "$([math]::Round($s[0], 3))..$([math]::Round($s[-1], 3))" }
$mStA = med $res.stA; $mStB = med $res.stB; $m8 = med $res.r8; $m8s = med $res.r8s; $m12 = med $res.r12; $m10 = med $res.r10
$ok = $true
if ($mStA -gt 0 -and $mStB -gt 0.5 * $mStA) { "FAIL: median stacked shadows $mStB% vs the old $mStA%"; $ok = $false }
if ($m8 -gt $Limit8) { "FAIL: median world-shadow flicker $([math]::Round($m8, 3)) x the old (> $Limit8)"; $ok = $false }
if ($m10 -gt $Limit) { "FAIL: median character-light flicker $([math]::Round($m10, 3)) x the old (> $Limit)"; $ok = $false }
$sum = "$Runs runs, medians (range): stacked shadows $mStA% ($(rng $res.stA)) -> $mStB% ($(rng $res.stB)), " +
       "world-shadow flicker $([math]::Round($m8, 3))x ($(rng $res.r8); 12 / 255 uncapped $([math]::Round($m12, 3))x ($(rng $res.r12)); old metric with the silhouettes $([math]::Round($m8s, 3))x ($(rng $res.r8s))), " +
       "character-light flicker $([math]::Round($m10, 3))x ($(rng $res.r10))"
if ($ok) { "shadow gate: PASS ($sum)"; exit 0 } else { "shadow gate: FAIL ($sum)"; exit 1 }
