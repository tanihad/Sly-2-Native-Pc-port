# Launch the native Sly 2 build in a window.
#   powershell -ExecutionPolicy Bypass -File scripts\play.ps1
# Needs a built game (scripts\build_game.py) and your disc image in orig\disc\.
# Controls: notes\controls.md (keyboard or an Xbox-style controller). Alt+Enter / F11: fullscreen.
$root = Split-Path $PSScriptRoot -Parent
$exe  = Join-Path $root 'build\sly2\sly2.exe'
$elf  = Join-Path $root 'orig\SCUS_973.16'
$iso  = Get-ChildItem (Join-Path $root 'orig\disc') -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not (Test-Path $exe)) { Write-Host "Game not built yet: run scripts\build_game.py first."; exit 1 }
if (-not $iso) { Write-Host "No disc image found in orig\disc\."; exit 1 }
$env:PS2_ISO = $iso.FullName
Remove-Item Env:\PS2_HEADLESS, Env:\PS2_WATCHDOG, Env:\PS2_DUMP_FRAMES, Env:\PS2_INPUT -ErrorAction SilentlyContinue
Start-Process -FilePath $exe -ArgumentList "`"$elf`"" -WorkingDirectory (Split-Path $exe)
