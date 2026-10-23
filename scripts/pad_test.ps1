# Controller test: shows every controller the game sees (DualSense, DualShock 4, Xbox, other
# gamepads), live buttons / sticks / triggers / battery, the DualShock 2 state the game gets, and
# tests rumble (keys 1, 2, 3 in the window). Runs sly2.exe --pad-test (runtime/input_padtest.c).
#   powershell -ExecutionPolicy Bypass -File scripts\pad_test.ps1 [-Exe path\sly2.exe] [-Print] [-Seconds N]
# -Print: no window; prints the controller list and every change to this console (a development
#         build, which is a console program; the player's build logs to sly2.log instead).
param([string]$Exe = '', [switch]$Print, [double]$Seconds = 0)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Exe) {
    $candidates = if ($Print) { @('build\sly2\sly2.exe', 'build\rel\sly2.exe', 'dist\Sly2\sly2.exe') }
                  else { @('dist\Sly2\sly2.exe', 'build\rel\sly2.exe', 'build\sly2\sly2.exe') }
    foreach ($c in $candidates) { $p = Join-Path $root $c; if (Test-Path $p) { $Exe = $p; break } }
}
if (-not $Exe -or -not (Test-Path $Exe)) { Write-Output 'no sly2.exe found: build it first (scripts\package.py or scripts\build_game.py)'; exit 1 }
$a = @('--pad-test')
if ($Print) { $a += '--print' }
if ($Seconds -gt 0) { $a += @('--seconds', "$Seconds") }
Write-Output "$Exe $($a -join ' ')"
if ($Print) { & $Exe @a; exit $LASTEXITCODE }
$p = Start-Process -FilePath $Exe -ArgumentList $a -PassThru
$p.WaitForExit()
exit $p.ExitCode
