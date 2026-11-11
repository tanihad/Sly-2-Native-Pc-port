# (tex-prebuild) Reimagined pop-in measurement (notes/reimagined.md "Ready before first sight"): the owner's settings
# (High: 1440p, 100 %, MSAA 8x, thin outlines; Look Reimagined; 120 fps), deterministic (PS2_FRAME_DT_FIXED: the game
# clock stepped 1/120 s a frame), START at the title, one X at the safehouse -> the hub loads; Sly then walks while the
# camera turns (new textures keep coming). PS2_AI_POPLOG=1 counts, from the first 3D frame after the load on: AI images
# that replaced an Enhanced image already shown (swaps; visible = a draw used it in the last 2 frames) and each texture's
# first image (AI or Enhanced) with its delay. Also the load (X -> first 3D frame, wall time) and per-picture GPU /
# render-thread times after the first 3D frame (p99 / max), and video memory.
#   scripts\tex_pop_gate.ps1 [-Cold] [-Scene ep2_hub] [-Seed <cache dir>] [-Extra "K=V;.."] [-Tag t] [-Video <frames>]
# -Cold: an empty Reimagined cache (else -Seed, default build\prebuild\texcache_warm; new images are copied back).
# -Video n: also dumps n pictures (every 2nd, 1920 x 1080) from the X on into build\prebuild\frames\<tag> (no timing then).
# Passes (exit 0) when no visible swap happened after the first 3D frame. One game run; nothing is left but a summary
# (and the frames with -Video).
param([switch]$Cold, [string]$Scene = 'ep2_hub', [string]$Seed = '', [string]$Extra = '', [int]$Seconds = 260, [string]$Tag = 'pop',
      [int]$Video = 0, [switch]$KeepLog, [string]$Size = '2560x1440')
$wt = Split-Path $PSScriptRoot -Parent
$w = Join-Path $wt 'build\models\x4plus.bin'
if (-not (Test-Path $w)) { $w = Join-Path (Split-Path $wt -Parent) 'light\build\models\x4plus.bin' }
$env:PS2_AI_WEIGHTS = $w
if ($Cold) {
    $Seed = Join-Path $wt "build\prebuild\texcache_cold_$Tag"
    Remove-Item -Recurse -Force $Seed -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $Seed | Out-Null
} elseif (-not $Seed) { $Seed = Join-Path $wt 'build\prebuild\texcache_warm' }
$env:PS2_TEXCACHE_SEED = $Seed
$ini = 'graphics.look=reimagined;graphics.render_scale=100%;graphics.msaa=8x;graphics.outlines=thin;graphics.filters=modern;graphics.preset=high'
# (X at the safehouse -> the hub's load; then walk + camera turns, as gfx_stress_gate's walk with the right stick)
$walk = '+1000:lup:400,+0:rright:300,+400:lleft:200,+200:rleft:500,+200:lup:600,+600:ldown:600,+0:rright:400,+600:lright:300,' +
        '+300:lup:900,+300:rleft:600,+600:ldown:900,+900:lleft:600,+0:rright:600,+600:lup:900'
$pad = "+2000:cross:6,$walk"
$ex = 'PS2_LIGHT=;PS2_LIGHT_LOG=;PS2_DUMP_FROM=script;PS2_DUMP_TO=0;PS2_FRAME_DT_FIXED=1;PS2_AI_POPLOG=1;PS2_AI_LOG=1;PS2_TEX_SWAPLOG=1'
$every = 100000
if ($Video -gt 0) { $pad = "+2000:cross:6,+0:dump:$Video,$walk" -replace ',\+1000:lup', ',+1000:lup'; $every = 2; $Size = '1920x1080'; $ex += ';PS2_DUMP_BMP=1' }
else { $ex += ';PS2_GPU_FT=2;PS2_GPU_FT_SKIP=0' }
if ($Extra) { $ex += ";$Extra" }
$name = "$Tag$(if ($Cold) { '_cold' })"
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'light_shots.ps1') -Scene $Scene -At 1 -Frames 1 -Every $every -NoAB -Fps 120 `
    -Seconds $Seconds -Tag $name -Ini $ini -Extra $ex -PadAll $pad -Size $Size | Out-Null
Remove-Item Env:\PS2_TEXCACHE_SEED -ErrorAction SilentlyContinue
$d = Join-Path $wt "build\light\shots\$($Scene)__$name"
$log = Join-Path $d 'run.log'
if (-not (Test-Path $log)) { "pop $name`: no run log"; exit 2 }
$L = Get-Content $log
# the hub: the last level change before the last first-3D-frame line
$f3 = $L | Where-Object { $_ -match 'ai-pop: frame (\d+) v(\d+) t (\d+) ms world (\d+): first 3D frame' } | Select-Object -Last 1
$lc = $L | Where-Object { $_ -match 'ai-pop: frame (\d+) v(\d+) t (\d+) ms level' } | Select-Object -Last 1
$xv = $L | Where-Object { $_ -match 'pad frame|\[window\] script' } | Select-Object -First 3
$sum = $L | Where-Object { $_ -match 'ai-pop: .*swaps after it' } | Select-Object -Last 1
$crash = @($L | Where-Object { $_ -match '\[ps2\] crash|device lost' }).Count
$v3 = 0; $t3 = 0; $tl = 0
if ($f3 -match 'v(\d+) t (\d+) ms') { $v3 = [int]$Matches[1]; $t3 = [double]$Matches[2] }
if ($lc -match 't (\d+) ms') { $tl = [double]$Matches[1] }
"pop $name ($Scene, seed $Seed)"
if ($lc) { "  level change: $($lc.Trim())" }
if ($f3) { "  $($f3.Trim())   [load: $([math]::Round(($t3 - $tl) / 1000.0, 2)) s from the level change]" }
if ($sum) { "  $($sum.Trim())" } else { "  (no ai-pop summary)" }
$ai = $L | Where-Object { $_ -match 'ai: \d+ built by the net' } | Select-Object -Last 1
if ($ai) { "  $($ai.Trim())" }
$pre = $L | Where-Object { $_ -match 'ai-pre:' } | Select-Object -Last 4
foreach ($p in $pre) { "  $($p.Trim())" }
$vr = $L | Where-Object { $_ -match 'video memory' } | Select-Object -Last 1
if ($vr) { "  $($vr.Trim())" }
if ($Video -le 0 -and $v3) {
    $g = New-Object System.Collections.Generic.List[double]; $r = New-Object System.Collections.Generic.List[double]
    foreach ($l in $L) {
        if ($l -match '^.*ftp: v(\d+) ([\d.]+) ([\d.]+)') { if ([int]$Matches[1] -ge $v3) { $g.Add([double]$Matches[2]); $r.Add([double]$Matches[3]) } }
    }
    if ($g.Count) {
        $gs = $g.ToArray(); [Array]::Sort($gs); $rs = $r.ToArray(); [Array]::Sort($rs)
        $p = [int][math]::Floor($gs.Length * 0.99) - 1; if ($p -lt 0) { $p = 0 }
        "  frame times after the first 3D frame ($($gs.Length) pictures): GPU+present avg $([math]::Round(($gs | Measure-Object -Average).Average, 2)) p99 $($gs[$p]) max $($gs[-1]) ms | render thread avg $([math]::Round(($rs | Measure-Object -Average).Average, 2)) p99 $($rs[$p]) max $($rs[-1]) ms"
        $g0 = New-Object System.Collections.Generic.List[double]; $r0 = New-Object System.Collections.Generic.List[double]
        foreach ($l in $L) { if ($l -match '^.*ftp: v(\d+) ([\d.]+) ([\d.]+)') { $vv = [int]$Matches[1]; if ($vv -ge $v3 - 1200 -and $vv -lt $v3) { $g0.Add([double]$Matches[2]); $r0.Add([double]$Matches[3]) } } }
        if ($g0.Count) { $a = $g0.ToArray(); [Array]::Sort($a); $b = $r0.ToArray(); [Array]::Sort($b); "  during the load (1200 VBlanks before it, $($a.Length) pictures): GPU+present max $($a[-1]) ms, render thread max $($b[-1]) ms" }
    }
}
if ($crash) { "  CRASH lines: $crash" }
if ($KeepLog) { Copy-Item $log (Join-Path $wt "build\prebuild\$name.log") -Force }
if ($Video -gt 0) {
    $fd = Join-Path $wt "build\prebuild\frames\$name"
    Remove-Item -Recurse -Force $fd -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $fd | Out-Null
    Move-Item (Join-Path $d 'frame_*.*') $fd -ErrorAction SilentlyContinue
    "  frames: $fd ($(@(Get-ChildItem $fd).Count))"
}
Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue
$vis = if ($sum -match 'swaps after it \d+ \(visible (\d+)\)') { [int]$Matches[1] } else { -1 }
if ($vis -eq 0 -and -not $crash) { exit 0 } else { exit 1 }
