# (reimagine) Stability A/B in ONE run (the same camera path for both): a fixed pad path on the owner's settings (1440p,
# 120 fps, thin outlines, render scale auto, filters modern); PS2_GPU_AB=<phase> PS2_GPU_AB_R8=<mask> alternates the
# feature every <phase> pictures; every picture is dumped (BMP), sorted into A and B by the logged phase switches (the
# first pictures after each switch left out: the other mode's history), and each set measured with
# build\stability_check.exe over triples of consecutive pictures only (flicker: reversals of 8 x 8 block luma per 1000
# blocks x pictures; jitter: |second difference| / |first differences|; motion: mean change).
#   -Shot orbit: ep1 hub, Sly runs with the camera orbiting; -Shot trees: ep2 hub, Sly walks into the trees' shadows
#   -Mask 1024: A = Faithful lighting, B = Reimagined; 2048: A = 0.1.1207's shadows, B = fixed; 4096: A / B = PS2_REIM_X off / on
#   -Dbg 1: the key-light term alone (PS2_REIM_DBG=1). Prints one line per set; deletes the dumps unless -Keep.
param([int]$Mask = 1024, [int]$Dbg = 0, [int]$Phase = 6, [int]$Frames = 600, [double]$Thr = 3, [string]$Tag = '', [switch]$Keep,
      [string]$Blur = 'preset', [ValidateSet('orbit', 'trees')][string]$Shot = 'orbit', [string]$Extra = '')
$wt = Split-Path $PSScriptRoot -Parent
$chk = Join-Path $wt 'build\stability_check.exe'
if (-not $Tag) { $Tag = "ab$Mask`_$Shot`_dbg$Dbg" }
$w = Join-Path $wt 'build\models\x4plus.bin'
if (Test-Path $w) { $env:PS2_AI_WEIGHTS = $w }
if (-not $env:PS2_TEXCACHE_SEED) { $env:PS2_TEXCACHE_SEED = Join-Path $wt 'build\reimagine\texcache' }
$ini = "graphics.outlines=thin;graphics.render_scale=auto;graphics.filters=modern;graphics.look=reimagined;graphics.motion_blur=$Blur"
if ($Shot -eq 'orbit') { $scene = 'ep1_hub'; $at = 6400; $pad = '+400:lup:1500,+0:rright:1500' }
else { $scene = 'ep2_hub'; $at = 6450; $pad = '+200:lup:400,+0:lleft:200,+400:lup:600' }   # (into the tree shadows ~v6500-6900)
$ex = "PS2_LIGHT=;PS2_LIGHT_LOG=;PS2_DUMP_BMP=1;PS2_DUMP_WAIT=1;PS2_GPU_AB_R8=$Mask;PS2_GPU_AB_LOG=1;PS2_REIM_DBG=$Dbg;PS2_TEX_SWAPLOG=1"
if ($Extra) { $ex += ";$Extra" }
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'light_shots.ps1') -Scene $scene -At $at -Frames $Frames -Every 1 -Phase $Phase -Fps 120 `
    -Seconds 420 -Tag $Tag -Ini $ini -Extra $ex -PadExtra $pad | Out-Null
$d = Join-Path $wt "build\light\shots\$($scene)__$Tag"
$ph = @(Select-String -Path (Join-Path $d 'run.log') -Pattern 'ab: phase ([AB]) from v(\d+)' | ForEach-Object { @{ p = $_.Matches[0].Groups[1].Value; v = [int]$_.Matches[0].Groups[2].Value } })
foreach ($s in 'A', 'B') { New-Item -ItemType Directory -Force (Join-Path $d $s) | Out-Null }
$skip = if ($Phase -le 1) { 0 } elseif ($Phase -le 8) { 1 } else { 4 }   # (the first pictures after a switch: the other mode's history)
foreach ($f in Get-ChildItem $d -Filter 'frame_*.bmp') {
    $v = [int]($f.BaseName -replace 'frame_', '')
    $cur = $null; $start = 0
    foreach ($x in $ph) { if ($x.v -le $v) { $cur = $x.p; $start = $x.v } }
    if (-not $cur -or $v - $start -lt $skip) { continue }
    Move-Item $f.FullName (Join-Path $d "$cur\$($f.Name)")
}
foreach ($s in 'A', 'B') {
    $r = & $chk (Join-Path $d $s) $Thr 64
    "$Tag $s (mask $Mask, dbg $Dbg, threshold $Thr): $r"
}
$sw = Select-String -Path (Join-Path $d 'run.log') -Pattern 'tex-swap: frame' | Select-Object -Last 1
if ($sw) { "$Tag swaps: $($sw.Line.Trim())" }
if (-not $Keep) { Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue }
