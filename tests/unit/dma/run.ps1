# Build and run the DMAC unit tests (runtime/dma.c + fake devices) into build/unit/dma/.
#   powershell -ExecutionPolicy Bypass -File tests\unit\dma\run.ps1
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {   # git worktree: tools live in the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$clang = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
$out = Join-Path $root 'build\unit\dma'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'test_dma.exe'

& $clang -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function `
    -I (Join-Path $root 'runtime') `
    (Join-Path $root 'runtime\dma.c') (Join-Path $PSScriptRoot 'test_dma.c') -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'build failed'; exit 1 }
& $exe
exit $LASTEXITCODE
