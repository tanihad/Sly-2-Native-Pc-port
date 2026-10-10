# Launch Sly 2 (native PC build) in a window, with sound.
#   powershell -ExecutionPolicy Bypass -File scripts\play.ps1 [-Rebuild]
# Builds the release version on first use (about a minute). Needs your disc image in orig\disc\.
# Controls: notes\controls.md (keyboard, or an Xbox-style controller). Alt+Enter / F11: fullscreen.
# Saves go to build\rel\memcard\slot0 (a PS2 memory card as plain files).
param([switch]$Rebuild)
$root = Split-Path $PSScriptRoot -Parent
$out  = Join-Path $root 'build\rel'
$exe  = Join-Path $out 'sly2.exe'
$elf  = Join-Path $root 'orig\SCUS_973.16'
$py   = Join-Path $root '.venv\Scripts\python.exe'
$iso  = Get-ChildItem (Join-Path $root 'orig\disc') -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $iso) { Write-Host "No disc image found in orig\disc\."; exit 1 }
if ($Rebuild -or -not (Test-Path $exe)) {
    Write-Host "Building the release version..."
    & $py (Join-Path $root 'scripts\build_game.py') --release --out $out
    if ($LASTEXITCODE -ne 0) { Write-Host "Build failed."; exit 1 }
}
$env:PS2_ISO = $iso.FullName
foreach ($v in 'PS2_HEADLESS', 'PS2_WATCHDOG', 'PS2_DUMP_FRAMES', 'PS2_INPUT', 'PS2_AUDIO_NULL', 'PS2_AUDIO_WAV') {
    Remove-Item "Env:\$v" -ErrorAction SilentlyContinue
}
Start-Process -FilePath $exe -ArgumentList "`"$elf`"" -WorkingDirectory $out
