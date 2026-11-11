# (reimagine-crash) Graphics-switch stress gate: the owner's settings (High: 1440p, 100 %, MSAA 8x, thin outlines; Look
# Reimagined; 120 fps), the ep2 hub with Sly walking (textures building: -Cold starts from an empty Reimagined cache),
# and every <Every> pad frames one change as the Options menu's Apply makes it (window.c PS2_GFX_STRESS: Look Faithful <->
# Reimagined, Retro <-> Reimagined, Quality Low..Ultra, render scale 100 <-> 75 %, MSAA 8x <-> 4x, in turn). One game run
# at a time (one process per -Runs); passes when no run crashed (no "[ps2] crash", the process ran to its end) and the
# runs together made at least -Min changes. No frame dumps. Log lines per run, then "gfx stress: PASS|FAIL (...)".
#   scripts\gfx_stress_gate.ps1 [-Cold] [-Actions lrqsm] [-Every 30] [-Seconds 300] [-Runs 2] [-Min 500] [-Extra "K=V;.."]
param([switch]$Cold, [string]$Actions = 'lrqsm', [int]$Every = 30, [int]$Seconds = 300, [int]$Runs = 2, [int]$Min = 500,
      [string]$Extra = '', [string]$Tag = 'gfxstress', [switch]$KeepLog)
$wt = Split-Path $PSScriptRoot -Parent
$env:PS2_AI_WEIGHTS = Join-Path $wt 'build\models\x4plus.bin'
$seedWarm = Join-Path $wt 'build\reimagine\texcache'
$ini = 'graphics.look=reimagined;graphics.render_scale=100%;graphics.msaa=8x;graphics.outlines=thin;graphics.filters=modern;graphics.preset=high'
# (START at the title, one X at the safehouse; Sly then walks back and forth: new textures keep coming)
$walk = '+1000:lup:400,+0:lleft:200,+400:lup:600,+600:ldown:600,+600:lright:300,+300:lup:900,+900:ldown:900,+900:lleft:600,+600:lup:900'
$total = 0; $crashes = 0; $nets = 0; $short = 0
for ($r = 1; $r -le $Runs; $r++) {
    if ($Cold) {
        $seed = Join-Path $wt "build\reimagine\texcache_stress_cold"
        Remove-Item -Recurse -Force $seed -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force $seed | Out-Null
        $env:PS2_TEXCACHE_SEED = $seed
    } else { $env:PS2_TEXCACHE_SEED = $seedWarm }
    $ex = "PS2_LIGHT=;PS2_LIGHT_LOG=;PS2_DUMP_FROM=script;PS2_DUMP_TO=0;PS2_GFX_STRESS=$Every,$Actions,5100"
    if ($Extra) { $ex += ";$Extra" }
    $name = "$Tag$(if ($Cold) { '_cold' })_$r"
    powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'light_shots.ps1') -Scene ep2_hub -At 1 -Frames 1 -Every 100000 -NoAB -Fps 120 `
        -Seconds $Seconds -Tag $name -Ini $ini -Extra $ex -PadAll "+2000:cross:6,$walk" | Out-Null
    $d = Join-Path $wt "build\light\shots\ep2_hub__$name"
    $log = Join-Path $d 'run.log'
    $n = @(Select-String -Path $log -Pattern '\[window\] stress \d+:' -ErrorAction SilentlyContinue).Count
    $cr = @(Select-String -Path $log -Pattern '\[ps2\] crash|device lost|removed' -ErrorAction SilentlyContinue)
    $nr = @(Select-String -Path $log -Pattern 'ai: x4plus net ready' -ErrorAction SilentlyContinue).Count
    $last = Select-String -Path $log -Pattern '\[window\] stress (\d+):' -ErrorAction SilentlyContinue | Select-Object -Last 1
    "run $r ($name): $n changes, $nr nets created, $($cr.Count) crash lines$(if ($cr.Count) { ': ' + $cr[0].Line.Trim() })"
    if ($cr.Count) { $crashes++; Copy-Item $log (Join-Path $wt "build\reimagine\$name.crash.log") -Force }
    if ($n -lt 10) { $short++ }
    $total += $n; $nets += $nr
    if ($KeepLog) { Copy-Item $log (Join-Path $wt "build\reimagine\$name.log") -Force }
    Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue
}
Remove-Item Env:\PS2_TEXCACHE_SEED -ErrorAction SilentlyContinue
$ok = $crashes -eq 0 -and $total -ge $Min -and $short -eq 0
"gfx stress: $(if ($ok) { 'PASS' } else { 'FAIL' }) ($total changes in $Runs runs, $crashes runs crashed, $nets nets created$(if ($short) { ", $short runs stopped early" }))"
if ($ok) { exit 0 } else { exit 1 }
