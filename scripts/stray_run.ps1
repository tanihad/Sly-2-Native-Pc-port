# (stray-tri) The owner's "wrong triangle for one frame" repro: the ep1 hub at night, Sly running over the rooftops with
# camera turns for ~80 s, Modern renderer with the owner's settings (outlines thin, filters modern, render scale auto,
# 1920x1080, MFT on), every picture of VBlanks At .. At + Frames - 1 read back and judged online by the runtime's blink
# detector (PS2_DUMP_SPIKE, runtime/ps2_runtime.c dump_spike): one "[spike]" line per picture with blinking 16 x 16 blobs,
# the first few events written as spike_<vblank>.bmp (the picture before, the blinking one, the one after). Nothing else
# is written, so a long run costs no disk.
#   .\scripts\stray_run.ps1 -Name base [-Env "PS2_R2_FANS=0;..."] [-Ini "graphics.outlines=ps2"] [-Frames 7000] [-Keep]
# Output: build\flicker\<Name>\run.log and the event count ("stray: N events").
param([Parameter(Mandatory = $true)][string]$Name, [string]$Env = "", [string]$Ini = "", [int]$At = 4800,
      [int]$Frames = 7000, [int]$Seconds = 170, [int]$Fps = 120, [string]$Size = '1920x1080', [switch]$Keep)
$p = @('+1800:lup:240')
$pat = @('+240:rright:50', '+50:lup:180', '+180:cross:6', '+15:lup:160', '+160:rleft:70', '+70:lup:220', '+220:circle:6',
         '+20:lup:200', '+200:rright:30', '+30:lleft:120', '+120:lup:200', '+200:cross:6', '+12:cross:6', '+30:lup:240',
         '+240:rleft:40', '+40:ldown:150', '+150:lright:120')
for ($i = 0; $i -lt 4; $i++) { $p += $pat }
$p += '+120:lup:200'
$ini = "display.frame_rate=$Fps;display.aspect=16:9;graphics.preset=custom;graphics.renderer_mode=modern;graphics.render_scale=auto;" +
       "graphics.outlines=thin;graphics.filters=modern"
foreach ($kv in ($Ini -split ';' | Where-Object { $_ })) { $ini += ";$kv" }   # (later keys win: flicker_run writes them in order)
$envs = "PS2_DUMP_SPIKE=1;PS2_DUMP_WAIT=$(if ($Fps -gt 60) { 1 } else { 0 });PS2_GS_PRESENT_SIZE=$Size;PS2_MFT_LOG=1"
if ($Env) { $envs += ";$Env" }
$fa = @{ Name = $Name; Episode = 1; Job = 'm1_brawl'; Play = $true; At = $At; Frames = $Frames; Seconds = $Seconds; Scale = 'auto'
         Keep = $true; Pad = ($p -join ','); Ini = $ini; Env = $envs; Check = '--quiet' }
& (Join-Path $PSScriptRoot 'flicker_run.ps1') @fa | Out-Null
$root = Split-Path $PSScriptRoot -Parent
$log = Join-Path $root "build\flicker\$Name\run.log"
$ev = @(Select-String -Path $log -Pattern '^\[spike\] v' -ErrorAction SilentlyContinue)
$last = Select-String -Path $log -Pattern '^\[spike\] \d+ pictures' -ErrorAction SilentlyContinue | Select-Object -Last 1
$mft = Select-String -Path $log -Pattern 'mft: \d+ modern' -ErrorAction SilentlyContinue | Select-Object -Last 1
"stray: $($ev.Count) events$(if ($last) { " ($($last.Line -replace '^\[spike\] ', ''))" })"
$ev | ForEach-Object { "  " + ($_.Line -replace '^\[spike\] ', '') }
if ($mft) { "  " + ($mft.Line -replace '^.*?(mft: \d+ modern frames, \d+ exact).*$', '$1') }
if (-not $Keep) { Remove-Item -Recurse -Force (Join-Path $root "build\warp\flk_$Name\frames") -ErrorAction SilentlyContinue }
