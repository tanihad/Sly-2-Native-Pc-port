# High-frame-rate measurement of one scene (pc-fps2, notes/frame-rate.md "High frame rate: approaches compared"):
# the release build at a warp scenario (scripts\warp_run.ps1, headless) at one [display] frame_rate, with PS2_PERF (EE
# thread split per second) and PS2_GS_HW_STATS (GS thread / frontend / render / GPU per game frame), then a summary of
# the seconds from -From on: fps, EE busy split, per-thread ms per game frame.
#   .\scripts\fps2_scene.ps1 -Scene ep1_hub -Rate 120 [-Seconds 70] [-From 40] [-Scale 3] [-Exe <exe>] [-Extra "K=V;K=V"]
# Scenes: ep1_hub ep3_hub ep5_hub ep2_hub (hub after X, walking), m8_boss (direct job), intro (title path), or
# "<episode>|<job>|<flags>|<pad>" as in scripts\m8_scenes.txt. Run dir: build\warp\fps2_<scene>_<rate><Tag>.
param([Parameter(Mandatory = $true)][string]$Scene, [Parameter(Mandatory = $true)][string]$Rate, [int]$Seconds = 70,
      [int]$From = 40, [int]$Scale = 3, [string]$Exe = "", [string]$Extra = "", [string]$Tag = "", [switch]$Keep)
$root = Split-Path $PSScriptRoot -Parent
$HP = '+500:lup:90,+90:rright:60,+60:lup:60,+60:cross:6,+30:lleft:60,+200:lup:120,+120:rleft:80,+300:lup:200,+200:square:6,+300:lup:150'
$GP = '+2600:lup:90,+90:rright:60,+60:lup:60,+60:cross:6,+30:square:6,+30:lleft:60,+200:lup:120,+120:rleft:80,+100:cross:6,+300:lup:200,+200:circle:6,+300:lup:150,+150:square:6'
# time-based pads (runtime/window.c r<s> tokens) so every rate gets the same real-time input
$HPT = 'r12:none,r4:lup:1.5s,r2:rright:1s,r1:lup:1s,r1:cross:0.1s,r1:lleft:1s,r3:lup:2s,r2:rleft:1.3s,r5:lup:3.3s,r3:square:0.1s,r5:lup:2.5s'
$known = @{
    'ep1_hub' = '1|m1_brawl|play|HPT'; 'ep1_brief' = '1||play|HPT';'ep2_hub' = '2||play|HPT'; 'ep3_hub' = '3||play|HPT'; 'ep5_hub' = '5||play|HPT';
    'ep7_hub' = '7||play|HPT'; 'm8_boss' = '8|m8_boss|direct|GP'; 'm7_boss' = '7|m7_boss|direct|GP';
    'm2_rc' = '2|m2_rc_chopper|direct|GP'; 'm3_heist' = '3|m3_heist|direct|GP'
}
$spec = if ($known.ContainsKey($Scene)) { $known[$Scene] } else { $Scene }
$f = @($spec -split '\|')
$sn = $Scene -replace '[^\w]', '_'; if ($sn.Length -gt 32) { $sn = $sn.Substring(0, 32) }
$name = "fps2_$($sn)_$Rate$Tag"
$dir = Join-Path $root "build\warp\$name"
New-Item -ItemType Directory -Force $dir | Out-Null
$env:PS2_FRAME_RATE = $Rate
$env:PS2_PERF = '1'
$env:PS2_GS_HW_STATS = '1'
if ($Scale -gt 0) { $env:PS2_GS_SCALE = "$Scale" } else { Remove-Item Env:\PS2_GS_SCALE -ErrorAction SilentlyContinue }
if ($Exe) { $env:M9_GAME_EXE = $Exe } else { Remove-Item Env:\M9_GAME_EXE -ErrorAction SilentlyContinue }
foreach ($kv in ($Extra -split ';' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; Set-Item "Env:\$k" $v }
$wa = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'warp_run.ps1'), '-Episode', $f[0], '-Release',
        '-Seconds', "$Seconds", '-DumpEvery', '100000', '-Name', $name)
if ($f[1]) { $wa += @('-Job', $f[1]) }
$play = $false
foreach ($fl in ($f[2] -split ' ' | Where-Object { $_ })) { if ($fl -eq 'direct') { $wa += '-Direct' } elseif ($fl -eq 'play') { $play = $true } }
$pad = $f[3] -replace '^GP$', $GP -replace '^HP$', $HP -replace '^HPT$', $HPT
# (play: X at the safehouse 25 s after START in wall time - warp_run's -Play counts VBlanks, which come faster above 60)
if ($play) { $pad = "r25:cross:0.2s,$pad" }
if ($pad) { $wa += @('-Pad', $pad) }
& powershell @wa | Out-Null
Remove-Item Env:\PS2_FRAME_RATE, Env:\PS2_PERF, Env:\PS2_GS_HW_STATS, Env:\PS2_GS_SCALE, Env:\M9_GAME_EXE -ErrorAction SilentlyContinue
foreach ($kv in ($Extra -split ';' | Where-Object { $_ })) { $k = ($kv -split '=', 2)[0]; Remove-Item "Env:\$k" -ErrorAction SilentlyContinue }
if (-not $Keep) { Remove-Item (Join-Path $dir 'sly2_warp_*.exe'), (Join-Path $dir 'frames\*.png') -ErrorAction SilentlyContinue }
$log = Join-Path $dir 'run.log'
if (-not (Test-Path $log)) { "no run.log in $dir"; exit 1 }
$py = 'C:\Users\tanih\Projects\sly2-decomp\.venv\Scripts\python.exe'
& $py (Join-Path $PSScriptRoot 'fps2_sum.py') $log --from $From --name "$Scene@$Rate$Tag"
