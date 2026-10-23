# Build and run the settings-model and Options-menu tests (no window, no game).
#   powershell -ExecutionPolicy Bypass -File tests\unit\menu\run.ps1 [-UnitOnly] [-Shots]
# -Shots: also write every menu page at 2560x1440, 1280x720, 1024x768 and 800x600 as PNGs into
# build\screenshots\pc_menu_*.png and a contact sheet (scripts\menu_shots.py). -UnitOnly is accepted for the
# common test runner (this test never opens a window).
param([switch]$UnitOnly, [switch]$Shots)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {      # worktree: tools live in the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$clang = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
$out = Join-Path $root 'build\unit\menu'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'test_menu.exe'
& $clang -std=c11 -O1 -Wall -Wextra -Werror -I (Join-Path $root 'runtime') (Join-Path $PSScriptRoot 'test_menu.c') `
    -o $exe -luser32 -lgdi32
if ($LASTEXITCODE) { Write-Output "build failed"; exit 1 }
$env:PS2_USER_DIR = Join-Path $out 'userdata'
New-Item -ItemType Directory -Force $env:PS2_USER_DIR | Out-Null
$testArgs = @()
$raw = Join-Path $out 'shots'
if ($Shots) { New-Item -ItemType Directory -Force $raw | Out-Null; $testArgs += @('--shots', $raw) }
& $exe @testArgs
$rc = $LASTEXITCODE
Remove-Item Env:\PS2_USER_DIR
if ($rc -eq 0 -and $Shots) {
    $py = Join-Path $main '.venv\Scripts\python.exe'
    & $py (Join-Path $root 'scripts\menu_shots.py') $raw (Join-Path $root 'build\screenshots') (Join-Path $root 'notes\pc-menu')
    $rc = $LASTEXITCODE
    Remove-Item -Recurse -Force $raw                 # (raw pictures are big: only the PNGs stay)
}
exit $rc
