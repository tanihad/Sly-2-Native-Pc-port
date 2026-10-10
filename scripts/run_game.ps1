# Run the recompiled game for a few seconds and summarize what it did.
#   .\scripts\run_game.ps1 [-Seconds 5] [-Skip 0] [-Out build\sly2] [-Dump <dir>] [-DumpEvery 60] [-Window]
#                          [-Wav <file>] [-Pad "<frame>:<button>[:<held>],..."]
# Headless by default (no window, audio rendered in real time into a null sink); -Window shows it
# and plays sound through the default device. -Wav also records everything the mixer renders.
# Works from a git worktree too.
param([int]$Seconds = 5, [int]$Skip = 0, [string]$Out = "", [string]$Dump = "", [int]$DumpEvery = 60,
      [switch]$Window, [string]$Wav = "", [string]$Pad = "")
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'orig\SCUS_973.16'))) {   # worktree: find the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
if (-not $Out) { $Out = Join-Path $root 'build\sly2' }
$exe = Join-Path $Out 'sly2.exe'
$log = Join-Path $Out 'run.log'

$env:PS2_WATCHDOG = "$Seconds"
if ($Window) {
    Remove-Item Env:\PS2_HEADLESS -ErrorAction SilentlyContinue
    Remove-Item Env:\PS2_AUDIO_NULL -ErrorAction SilentlyContinue
} else {
    $env:PS2_HEADLESS = "1"
    if (-not $env:PS2_AUDIO_NULL) { $env:PS2_AUDIO_NULL = "1" }    # headless: no sound device
}
if ($Wav) {
    $wavDir = Split-Path ([IO.Path]::GetFullPath($Wav)) -Parent
    New-Item -ItemType Directory -Force $wavDir | Out-Null
    $env:PS2_AUDIO_WAV = [IO.Path]::GetFullPath($Wav)
} else { Remove-Item Env:\PS2_AUDIO_WAV -ErrorAction SilentlyContinue }
if ($Pad) { $env:PS2_INPUT = $Pad }    # scripted pad presses (window.c)
if ($Dump) { New-Item -ItemType Directory -Force $Dump | Out-Null; $env:PS2_DUMP_FRAMES = $Dump; $env:PS2_DUMP_EVERY = "$DumpEvery" }
else { Remove-Item Env:\PS2_DUMP_FRAMES -ErrorAction SilentlyContinue }
$iso = Get-ChildItem (Join-Path $main 'orig\disc') -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1
if ($iso) { $env:PS2_ISO = $iso.FullName }

$p = Start-Process -FilePath $exe -ArgumentList (Join-Path $main 'orig\SCUS_973.16') `
    -RedirectStandardError $log -RedirectStandardOutput (Join-Path $Out 'run.out') -PassThru -NoNewWindow
if (-not $p.WaitForExit(($Seconds + 15) * 1000)) { Stop-Process -Id $p.Id -Force; "-- killed (watchdog didn't fire)" }
else { "-- exited, code $($p.ExitCode)$(if ($p.ExitCode -eq 3) { ' (watchdog)' })" }

$lines = Get-Content $log
"log lines: $($lines.Count)"
$lines | Select-Object -Skip $Skip
