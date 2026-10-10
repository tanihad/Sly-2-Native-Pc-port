# Build and run the window tests: unit checks, presenter checks (small test windows), then the
# interactive window test (opens a window for a few seconds) with the Direct3D 11 presenter and
# again with the GDI fallback.
#   powershell -ExecutionPolicy Bypass -File tests\unit\window\run.ps1 [-UnitOnly] [-NoFullscreen] [-Seconds 3]
param([switch]$UnitOnly, [switch]$NoFullscreen, [double]$Seconds = 3)
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
$out = Join-Path $root 'build\unit\window'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'test_window.exe'

& $clang -std=c11 -O1 -Wall -Wextra -Werror -I (Join-Path $root 'runtime') (Join-Path $PSScriptRoot 'test_window.c') `
    -o $exe -luser32 -lgdi32 -lwinmm
if ($LASTEXITCODE) { Write-Output "build failed"; exit 1 }

$testArgs = @('--seconds', "$Seconds")
if ($UnitOnly) { $testArgs += '--unit-only' }
if ($NoFullscreen) { $testArgs += '--no-fullscreen' }
Write-Output "== test_window (Direct3D 11 presenter) =="
Remove-Item Env:\PS2_WINDOW_GDI -ErrorAction SilentlyContinue
& $exe @testArgs
$rc = $LASTEXITCODE
if (-not $UnitOnly -and $rc -eq 0) {
    Write-Output "== closing the window ends the process with code 0 =="
    & $exe --close-test
    if ($LASTEXITCODE -ne 0) { Write-Output "FAILED: exit code $LASTEXITCODE"; exit 1 }
    Write-Output "PASSED"
    Write-Output "== test_window (GDI presenter) =="
    $env:PS2_WINDOW_GDI = '1'
    & $exe --seconds 2.5 --no-fullscreen
    $rc = $LASTEXITCODE
    Remove-Item Env:\PS2_WINDOW_GDI -ErrorAction SilentlyContinue
}
exit $rc
