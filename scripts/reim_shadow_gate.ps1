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
# character's shading pulsing frame to frame). Passes when the rework has at most half the old stacked share, no more
# view-8 flicker than the old, and at most -Limit x the old view-10 flicker.
#   powershell -ExecutionPolicy Bypass -File scripts\reim_shadow_gate.ps1 [-Exe build\rel\sly2.exe] [-Limit 0.85] [-Keep]
param([string]$Exe = '', [double]$Limit = 0.85, [switch]$Keep, [int]$Frames = 850, [string]$Extra = '')
$wt = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'project.ps1')
$chk = Join-Path $wt 'build\stability_check.exe'
if (-not (Test-Path $chk) -or (Get-Item $chk).LastWriteTime -lt (Get-Item (Join-Path $PSScriptRoot 'stability_check.c')).LastWriteTime) {
    & $ProjectClang -O2 -o $chk (Join-Path $PSScriptRoot 'stability_check.c')
}
$free = (Get-PSDrive C).Free / 1GB
if ($free -lt 60) { "C: has $([math]::Round($free)) GB free (< 60): not starting"; exit 2 }
if ($Exe) { $env:M9_GAME_EXE = (Resolve-Path $Exe).Path }
$w = Join-Path $wt 'build\models\x4plus.bin'
if (Test-Path $w) { $env:PS2_AI_WEIGHTS = $w }
if (-not $env:PS2_TEXCACHE_SEED) { $env:PS2_TEXCACHE_SEED = Join-Path $wt 'build\reimagine\texcache' }
$ini = 'graphics.look=reimagined;graphics.lighting=enhanced;graphics.textures=enhanced;graphics.render_scale=100%;graphics.msaa=2x;' +
       'graphics.outlines=thin;graphics.filters=modern;graphics.motion_blur=off;graphics.preset=custom'
$pad = "+2000:cross:6,+1000:lup:400,+0:lleft:200,+50:dump:$Frames,+350:lup:600"   # (reim_stability_gate's trees: a torch, then the trees' shadows)
$views = @(8, 10)
$ps = @()
foreach ($v in $views) {
    $ex = "PS2_LIGHT=;PS2_LIGHT_LOG=;PS2_DUMP_BMP=1;PS2_DUMP_WAIT=1;PS2_GPU_AB_R8=4096;PS2_REIM_X=64;PS2_GPU_AB_LOG=1;PS2_REIM_DBG=$v;" +
          "PS2_DUMP_FROM=script;PS2_DUMP_TO=0;PS2_FRAME_DT_FIXED=1;PS2_REIM_LOG=1"
    if ($Extra) { $ex += ";$Extra" }
    $a = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'light_shots.ps1'), '-Scene', 'ep2_hub', '-At', '1', '-Frames', '1',
           '-Every', '1', '-Phase', '6', '-Fps', '165', '-Seconds', "$([int](240 + $Frames * 0.1))", '-Tag', "shadow_gate$v", '-Ini', $ini,
           '-Extra', "`"$ex`"", '-PadAll', "`"$pad`"")
    $ps += Start-Process powershell -ArgumentList $a -PassThru -WindowStyle Hidden
}
$ps | Wait-Process -Timeout 1200
Remove-Item Env:\M9_GAME_EXE -ErrorAction SilentlyContinue
$reg = '0.33,0.40,0.67,0.95'
$m = @{}
foreach ($v in $views) {
    $d = Join-Path $wt "build\light\shots\ep2_hub__shadow_gate$v"
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
        if ($n -lt 100) { "FAIL: view $v phase $s has $n pictures"; exit 1 }
        $k = "$v$s"
        if ($v -eq 8) {
            $r1 = & $chk $sd 12 70 - 0.12 $reg
            $x = [regex]::Match("$r1", 'flicker ([\d.]+) per 1000 \(all\), [\d.]+ per 1000 \(dark < 70, ([\d.]+)% .*motion ([\d.]+)')
            if (-not $x.Success) { "FAIL: no measurement (view $v, $s): $r1"; exit 1 }
            $m[$k] = @{ fl = [double]$x.Groups[1].Value; stacked = [double]$x.Groups[2].Value; mo = [double]$x.Groups[3].Value; n = $n }
            "view 8  $s ($(if ($s -eq 'A') { 'old   ' } else { 'rework' }), $n pictures): world-shadow flicker $($m[$k].fl) per 1000, stacked shadows $($m[$k].stacked)% of blocks, motion $($m[$k].mo)"
        } else {
            $r1 = & $chk $sd 6 64 - 0.12 $reg
            $x = [regex]::Match("$r1", 'flicker ([\d.]+) per 1000 \(all\).*motion ([\d.]+)')
            if (-not $x.Success) { "FAIL: no measurement (view $v, $s): $r1"; exit 1 }
            $m[$k] = @{ fl = [double]$x.Groups[1].Value; mo = [double]$x.Groups[2].Value; n = $n }
            "view 10 $s ($(if ($s -eq 'A') { 'old   ' } else { 'rework' }), $n pictures): character-light flicker $($m[$k].fl) per 1000, motion $($m[$k].mo)"
        }
    }
    $sw = Select-String -Path (Join-Path $d 'run.log') -Pattern 'reim3-stats' | Select-Object -Last 1
    if ($sw -and $v -eq 8) { $sw.Line.Trim() }
    if (-not $Keep) { Remove-Item -Recurse -Force (Join-Path $d 'A'), (Join-Path $d 'B') -ErrorAction SilentlyContinue; Get-ChildItem $d -Filter 'frame_*' | Remove-Item -Force }
}
$ok = $true
$r8 = $m['8B'].fl / [math]::Max($m['8A'].fl, 1e-6); $r10 = $m['10B'].fl / [math]::Max($m['10A'].fl, 1e-6)
if ($m['8A'].stacked -gt 0 -and $m['8B'].stacked -gt 0.5 * $m['8A'].stacked) { "FAIL: stacked shadows $($m['8B'].stacked)% vs the old $($m['8A'].stacked)%"; $ok = $false }
if ($r8 -gt 1.0) { "FAIL: world-shadow flicker $([math]::Round($r8, 3)) x the old (> 1)"; $ok = $false }
if ($r10 -gt $Limit) { "FAIL: character-light flicker $([math]::Round($r10, 3)) x the old (> $Limit)"; $ok = $false }
$sum = "stacked shadows $($m['8A'].stacked)% -> $($m['8B'].stacked)%, world-shadow flicker $([math]::Round($r8, 3))x, character-light flicker $([math]::Round($r10, 3))x"
if ($ok) { "shadow gate: PASS ($sum)"; exit 0 } else { "shadow gate: FAIL ($sum)"; exit 1 }
