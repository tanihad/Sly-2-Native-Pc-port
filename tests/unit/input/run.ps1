# Build and run the PC input unit tests (runtime/input*.c; no controller needed, no window).
#   powershell -ExecutionPolicy Bypass -File tests\unit\input\run.ps1
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = (Resolve-Path (Join-Path $here '..\..\..')).Path
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {      # worktree: tools live in the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$clang = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
$out = Join-Path $root 'build\unit\input'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'test_input.exe'
$rt = Join-Path $root 'runtime'

& $clang -std=c11 -O1 -g -Wall -Wextra -Werror -I $rt `
    (Join-Path $rt 'input.c') (Join-Path $rt 'input_map.c') (Join-Path $rt 'input_ps.c') (Join-Path $rt 'settings.c') `
    (Join-Path $here 'test_input.c') -o $exe
if ($LASTEXITCODE -ne 0) { Write-Output 'build failed'; exit 1 }
& $exe
exit $LASTEXITCODE
