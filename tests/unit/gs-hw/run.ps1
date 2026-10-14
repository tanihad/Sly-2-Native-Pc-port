# Build and run the GPU backend (runtime/gpu) unit tests on the hardware adapter and on WARP:
# capabilities, the conformance kernel vs C, render thread + command rings, picture round trip,
# "both" mode on GS scenes, and the swap chain on a small window.
#   powershell -ExecutionPolicy Bypass -File tests\unit\gs-hw\run.ps1 [-Adapter hw|warp|both] [-NoWindow]
param([string]$Adapter = 'both', [switch]$NoWindow, [int]$Vectors = 131072)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {            # worktree: tools live in the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$clang = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
$out = Join-Path $root 'build\unit\gs-hw'
New-Item -ItemType Directory -Force $out | Out-Null
$gen = Join-Path $root 'build\gen'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\gpu_shaders.ps1') -Root $root -Gen $gen
if ($LASTEXITCODE -ne 0) { Write-Host 'shader build failed'; exit 1 }

$rt = Join-Path $root 'runtime'
$src = @('gs.c', 'gs_draw.c', 'gs_draw_pixel.c', 'gs_draw_tex.c', 'gs_draw_swz.c', 'gs_setup.c', 'gs_thread.c', 'gs_record.c', 'debug.c') | ForEach-Object { Join-Path $rt $_ }
$src += @(Get-ChildItem $rt -Filter 'gs_mem*.c' | ForEach-Object { $_.FullName })
$src += @(Get-ChildItem (Join-Path $rt 'gpu') -Filter '*.c' | ForEach-Object { $_.FullName })
$src += Join-Path $PSScriptRoot 'test_gs_hw.c'
$exe = Join-Path $out 'test_gs_hw.exe'
& $clang -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-comment -I $rt -I (Join-Path $rt 'gpu') -I $gen @src `
    -lsynchronization -luser32 -lgdi32 -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'gs-hw build failed'; exit 1 }
# a uniquely named copy: other agents may run test binaries of the same name
$run = Join-Path $out ("test_gs_hw_{0}.exe" -f $PID)
Copy-Item $exe $run -Force
$testArgs = @('--adapter', $Adapter, '--vectors', "$Vectors")
if ($NoWindow) { $testArgs += '--no-window' }
& $run @testArgs
$rc = $LASTEXITCODE
Remove-Item $run -ErrorAction SilentlyContinue
if ($rc -ne 0) { Write-Host 'gs-hw unit tests FAILED'; exit 1 }
Write-Host 'gs-hw unit tests passed'
exit 0
