# (r2-m1) The modern renderer's unit tests: the exact shader arithmetic (shaders/r2_vu.hlsli) through r2gpu's D3D11
# backend on the hardware adapter and on WARP, against the C reference (tests/unit/r2/test_r2.c).
#   powershell -ExecutionPolicy Bypass -File tests\unit\r2\run.ps1 [-Adapter hw|warp|both]
param([string]$Adapter = 'both')
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
$out = Join-Path $root 'build\unit\r2'
New-Item -ItemType Directory -Force $out | Out-Null
$gen = Join-Path $root 'build\gen'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\gpu_shaders.ps1') -Root $root -Gen $gen
if ($LASTEXITCODE -ne 0) { Write-Host 'shader build failed'; exit 1 }
$rt = Join-Path $root 'runtime'
$src = @((Join-Path $rt 'r2\r2_vertex.c'), (Join-Path $rt 'r2\r2gpu_d3d11.c'), (Join-Path $PSScriptRoot 'test_r2.c'))
$exe = Join-Path $out 'test_r2.exe'
& $clang -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -I $rt -I (Join-Path $rt 'gpu') -I $gen @src `
    -ld3d11 -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'r2 unit test build failed'; exit 1 }
$run = Join-Path $out ("test_r2_{0}.exe" -f $PID)                      # (a unique name: other agents may run tests)
Copy-Item $exe $run -Force
& $run --adapter $Adapter
$rc = $LASTEXITCODE
Remove-Item $run -ErrorAction SilentlyContinue
if ($rc -ne 0) { Write-Host 'r2 unit tests FAILED'; exit 1 }
exit 0
