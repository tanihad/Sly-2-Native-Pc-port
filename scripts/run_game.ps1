# Run the recompiled game for a few seconds and summarize what it did.
#   .\scripts\run_game.ps1 [-Seconds 5] [-Skip 0]
param([int]$Seconds = 5, [int]$Skip = 0)
$root = Split-Path $PSScriptRoot -Parent
$exe  = Join-Path $root 'build\sly2\sly2.exe'
$log  = Join-Path $root 'build\sly2\run.log'

$env:PS2_WATCHDOG = "$Seconds"
$iso = Get-ChildItem (Join-Path $root 'orig\disc') -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1
if ($iso) { $env:PS2_ISO = $iso.FullName }
$p = Start-Process -FilePath $exe -ArgumentList (Join-Path $root 'orig\SCUS_973.16') `
    -RedirectStandardError $log -RedirectStandardOutput (Join-Path $root 'build\sly2\run.out') -PassThru -NoNewWindow
if (-not $p.WaitForExit(($Seconds + 10) * 1000)) { Stop-Process -Id $p.Id -Force; "-- killed (watchdog didn't fire)" }
else { "-- exited, code $($p.ExitCode)$(if ($p.ExitCode -eq 3) { ' (watchdog)' })" }

$lines = Get-Content $log
"log lines: $($lines.Count)"
$lines | Select-Object -Skip $Skip
