# Build and run the IOP recompiler tests:  powershell -ExecutionPolicy Bypass -File tests\unit\iop_recomp\run.ps1
# gen.py assembles two synthetic IRX modules (hand-encoded MIPS I), recompiles them with recomp/iop.py
# and writes the test cases; the harness links the generated C with the native IOP kernel.
# Output: build\unit\iop_recomp\.
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
$python = Join-Path $main '.venv\Scripts\python.exe'
$out = Join-Path $root 'build\unit\iop_recomp'
New-Item -ItemType Directory -Force $out | Out-Null

& $python (Join-Path $PSScriptRoot 'gen.py') $out
if ($LASTEXITCODE -ne 0) { Write-Host 'iop_recomp tests: generator failed'; exit 1 }

$rt = Join-Path $root 'runtime'
$gen = Join-Path $out 'gen'
$srcs = @((Join-Path $PSScriptRoot 'test_iop_recomp.c')) + (Get-ChildItem $gen -Filter *.c | ForEach-Object { $_.FullName }) +
        @('iop_kernel.c', 'iop_libs.c', 'iop_sif.c', 'iop_loader.c' | ForEach-Object { Join-Path $rt $_ })
$exe = Join-Path $out 'test_iop_recomp.exe'
& $clang -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-label -I $rt -I $gen -I $out @srcs -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'iop_recomp tests: build failed'; exit 1 }

& $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'iop_recomp tests: FAILED'; exit 1 }
Write-Host 'iop_recomp tests: all passed'
exit 0
