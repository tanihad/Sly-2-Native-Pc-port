# (shadows) Look = Reimagined's shadow-overlap check (notes/reimagined-lighting.md "Shadows rework"): what the owner saw as
# "two shadows collide and flick" / "Sly running under a tree's shadow is not smooth". One game run on the owner's settings
# (1440p, render scale 100 %, MSAA 2x, thin outlines, filters modern, motion blur off, 165 fps), the ep2 trees path of
# reim_stability_gate (deterministic: START anchored, one X, PS2_FRAME_DT_FIXED, the dump window a pad token), the old
# shadows (A: PS2_REIM3's path off) and the rework (B) alternating every 6 pictures (PS2_GPU_AB_R8=4096, PS2_REIM_X=64), so
# both see the same camera path. The pictures show the post pass's light alone (PS2_REIM_DBG=8: occlusion x relight x
# shadow x exposure x local lights x the game's own shadow darkening - no textures, no grade: texture detail moving across
# blocks is not counted); measured in Sly's region (the middle-bottom of the picture, x 0.33..0.67, y 0.40..0.95) with
# scripts/stability_check.c: reversals of 8 x 8 block luma over triples of consecutive pictures of one phase at a shadow-sized
# step (12 / 255) in the shadowed blocks (luma < 100: lit is 128, one shadow ~87, two stacked ~52) per 1000 blocks, and the
# same over all of the region's blocks per its motion. Passes when the rework's numbers are at most -Limit x the old ones
# (default 0.85) and its stacked-shadow share (blocks darker than one shadow can make, < 70) is at most half the old one.
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
$ex = "PS2_LIGHT=;PS2_LIGHT_LOG=;PS2_DUMP_BMP=1;PS2_DUMP_WAIT=1;PS2_GPU_AB_R8=4096;PS2_REIM_X=64;PS2_GPU_AB_LOG=1;PS2_REIM_DBG=8;" +
      "PS2_DUMP_FROM=script;PS2_DUMP_TO=0;PS2_FRAME_DT_FIXED=1;PS2_REIM_LOG=1"
if ($Extra) { $ex += ";$Extra" }
$tag = 'shadow_gate'
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'light_shots.ps1') -Scene ep2_hub -At 1 -Frames 1 -Every 1 -Phase 6 -Fps 165 `
    -Seconds ([int](240 + $Frames * 0.1)) -Tag $tag -Ini $ini -Extra $ex -PadAll $pad | Out-Null
Remove-Item Env:\M9_GAME_EXE -ErrorAction SilentlyContinue
$d = Join-Path $wt "build\light\shots\ep2_hub__$tag"
$ph = @(Select-String -Path (Join-Path $d 'run.log') -Pattern 'ab: phase ([AB]) from v(\d+)' | ForEach-Object { @{ p = $_.Matches[0].Groups[1].Value; v = [int]$_.Matches[0].Groups[2].Value } })
foreach ($s in 'A', 'B') { New-Item -ItemType Directory -Force (Join-Path $d $s) | Out-Null }
foreach ($f in Get-ChildItem $d -Filter 'frame_*.bmp') {
    $v = [int]($f.BaseName -replace 'frame_', '')
    $cur = $null; $start = 0
    foreach ($x in $ph) { if ($x.v -le $v) { $cur = $x.p; $start = $x.v } }
    if (-not $cur -or $v - $start -lt 1) { continue }   # (the first picture after a switch: the other mode's history)
    Move-Item $f.FullName (Join-Path $d "$cur\$($f.Name)")
}
$reg = '0.33,0.40,0.67,0.95'
$m = @{}
foreach ($s in 'A', 'B') {
    $sd = Join-Path $d $s
    $n = @(Get-ChildItem $sd -Filter 'frame_*.bmp').Count
    $r1 = & $chk $sd 12 100 - 0.12 $reg      # (shadowed blocks: dark < 100)
    $r2 = & $chk $sd 70 70 - 0.12 $reg       # (only the share of blocks darker than one shadow: dark < 70)
    $x1 = [regex]::Match("$r1", 'flicker ([\d.]+) per 1000 \(all\), ([\d.]+) per 1000 \(dark < 100, ([\d.]+)% .*motion ([\d.]+)')
    $x2 = [regex]::Match("$r2", 'dark < 70, ([\d.]+)% of blocks')
    if (-not $x1.Success -or -not $x2.Success -or $n -lt 100) { "FAIL: no measurement for $s ($n pictures)"; exit 1 }
    $m[$s] = @{ all = [double]$x1.Groups[1].Value / [math]::Max([double]$x1.Groups[4].Value, 0.05); shad = [double]$x1.Groups[2].Value;
                share = [double]$x1.Groups[3].Value; stacked = [double]$x2.Groups[1].Value; n = $n }
    "$s ($(if ($s -eq 'A') { 'old' } else { 'rework' }), $n pictures): shadowed-block flicker $($m[$s].shad) per 1000 ($($m[$s].share)% of blocks shadowed), " +
        "region flicker per motion $([math]::Round($m[$s].all, 3)), stacked shadows $($m[$s].stacked)% of blocks"
}
$sw = Select-String -Path (Join-Path $d 'run.log') -Pattern 'reim3-stats' | Select-Object -Last 1
if ($sw) { $sw.Line.Trim() }
if (-not $Keep) { Remove-Item -Recurse -Force (Join-Path $d 'A'), (Join-Path $d 'B') -ErrorAction SilentlyContinue; Get-ChildItem $d -Filter 'frame_*' | Remove-Item -Force }
$ok = $true
$r1 = $m.B.shad / [math]::Max($m.A.shad, 1e-6); $r2 = $m.B.all / [math]::Max($m.A.all, 1e-6)
if ($r1 -gt $Limit) { "FAIL: shadowed-block flicker $([math]::Round($r1, 3)) x the old (> $Limit)"; $ok = $false }
if ($r2 -gt 1.0) { "FAIL: region flicker per motion $([math]::Round($r2, 3)) x the old (> 1)"; $ok = $false }
if ($m.B.stacked -gt [math]::Max(0.5 * $m.A.stacked, 0.0) -and $m.A.stacked -gt 0) { "FAIL: stacked shadows $($m.B.stacked)% vs $($m.A.stacked)%"; $ok = $false }
$sum = "shadowed flicker $([math]::Round($r1, 3))x, region flicker per motion $([math]::Round($r2, 3))x, stacked $($m.A.stacked)% -> $($m.B.stacked)%"
if ($ok) { "shadow gate: PASS ($sum)"; exit 0 } else { "shadow gate: FAIL ($sum)"; exit 1 }
