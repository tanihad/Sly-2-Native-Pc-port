# Build and run the native IOP kernel tests:  powershell -ExecutionPolicy Bypass -File tests\unit\iop_kernel\run.ps1
# Links runtime/iop_kernel.c, iop_libs.c, iop_sif.c, iop_loader.c with the fakes in test_iop_kernel.c.
# Output: build\unit\iop_kernel\.
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {      # worktree: tools live in the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$clang = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
$out = Join-Path $root 'build\unit\iop_kernel'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'test_iop_kernel.exe'
$rt = Join-Path $root 'runtime'

& $clang -std=c11 -O1 -g -Wall -Wextra -Werror -I $rt (Join-Path $PSScriptRoot 'test_iop_kernel.c') `
    @('iop_kernel.c', 'iop_libs.c', 'iop_sif.c', 'iop_loader.c' | ForEach-Object { Join-Path $rt $_ }) -lwinmm -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'iop_kernel tests: build failed'; exit 1 }

& $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'iop_kernel tests: FAILED'; exit 1 }
Write-Host 'iop_kernel tests: all passed'
exit 0
