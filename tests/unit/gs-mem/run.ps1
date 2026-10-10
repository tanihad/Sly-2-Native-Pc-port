# Build and run the GS local memory unit tests (runtime/gs_mem*.c).
#   powershell -ExecutionPolicy Bypass -File tests\unit\gs-mem\run.ps1
# Output: build\unit\gs-mem\test_gs_mem.exe. Works from a git worktree (tools come from the main checkout).
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$clang = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
$out = Join-Path $root 'build\unit\gs-mem'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'test_gs_mem.exe'
$rt = Join-Path $root 'runtime'
$srcs = @((Join-Path $PSScriptRoot 'test_gs_mem.c'), (Join-Path $rt 'gs_mem.c'), (Join-Path $rt 'gs_mem_xfer.c'), (Join-Path $rt 'gs_mem_tex.c'))

& $clang -std=c11 -O2 -Wall -Wextra -Wno-unused-function -Wno-comment -I $rt @srcs -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host "build failed"; exit 1 }
& $exe
$code = $LASTEXITCODE
if ($code -ne 0) { Write-Host "gs-mem unit tests FAILED"; exit $code }
Write-Host "gs-mem unit tests passed"
exit 0
