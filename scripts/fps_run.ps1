# Frame-rate test run (pc-fps, notes/frame-rate.md): a warp scenario (scripts\warp_run.ps1, release build, headless) at
# one [display] frame_rate, with the game clock and the player traced every VBlank (PS2_FPS_TRACE -> <dir>\trace.txt)
# for scripts\fps_trace.py.
#   .\scripts\fps_run.ps1 -Rate 120 -Episode 2 [-Job <job>] [-Direct] -Pad "r28:cross:0.1s,r15:none,r0:lup:5s" [-Seconds 90]
#                         [-Name <name>] [-DumpEvery 600 | -DumpSecs 2] [-Exe <path to sly2.exe>] [-Set ...] [-Make ...]
# -Pad tokens follow the title screen's START (warp_run.ps1); use the time tokens (t<s>, r<s>, holds "<s>s":
# runtime/window.c) so every rate gets the same real-time input.
param([Parameter(Mandatory = $true)][string]$Rate, [int]$Episode = 2, [string]$Job = "", [switch]$Direct,
      [string]$Pad = "", [int]$Seconds = 90, [string]$Name = "", [int]$DumpEvery = 600, [string]$Exe = "",
      [string]$Set = "", [string]$Make = "", [double]$DumpSecs = 0)
$root = Split-Path $PSScriptRoot -Parent
if (-not $Name) { $Name = "fps_ep$Episode$(if ($Job) { "_$Job" })_$Rate" }
$dir = Join-Path $root "build\warp\$Name"
New-Item -ItemType Directory -Force $dir | Out-Null
$env:PS2_FRAME_RATE = $Rate
$env:PS2_FPS_TRACE = Join-Path $dir 'trace.txt'
$env:PS2_PERF = "1"
if ($DumpSecs -gt 0) { $env:PS2_DUMP_SECS = "$DumpSecs" } else { Remove-Item Env:\PS2_DUMP_SECS -ErrorAction SilentlyContinue }   # (pictures every N wall s)
if ($Exe) { $env:M9_GAME_EXE = $Exe } else { Remove-Item Env:\M9_GAME_EXE -ErrorAction SilentlyContinue }
$a = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'warp_run.ps1'), '-Episode', "$Episode", '-Release',
       '-Seconds', "$Seconds", '-Name', $Name, '-DumpEvery', "$DumpEvery")
if ($Job) { $a += @('-Job', $Job) }
if ($Direct) { $a += '-Direct' }
if ($Pad) { $a += @('-Pad', $Pad) }
if ($Set) { $a += @('-Set', $Set) }
if ($Make) { $a += @('-Make', $Make) }
$out = & powershell @a                     # (not piped into Select-Object -First: that stops the run)
$out | Select-Object -First 3
Remove-Item Env:\PS2_FPS_TRACE, Env:\PS2_FRAME_RATE, Env:\PS2_PERF, Env:\PS2_DUMP_SECS -ErrorAction SilentlyContinue
Remove-Item (Join-Path $dir 'sly2_warp_*.exe') -ErrorAction SilentlyContinue      # (27 MB each: disk hygiene)
"log: $dir\run.log, trace: $dir\trace.txt"
