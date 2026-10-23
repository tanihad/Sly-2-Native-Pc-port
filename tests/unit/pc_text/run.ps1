# Build and run the PC wording unit tests (runtime/pc_text.c without the game glue +
# tests/unit/pc_text/test_pc_text.c). Output goes to build\unit\pc_text\.
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$clang = Join-Path $root 'tools\llvm-mingw\bin\clang.exe'
if (-not (Test-Path $clang)) {                     # worktree: tools\ lives in the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
        $clang = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
    }
}
$out = Join-Path $root 'build\unit\pc_text'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'test_pc_text.exe'

& $clang -std=c11 -O1 -g -Wall -Wextra -Werror -DPC_TEXT_NO_GAME -I (Join-Path $root 'runtime') `
    (Join-Path $root 'runtime\pc_text.c') (Join-Path $PSScriptRoot 'test_pc_text.c') -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'build failed'; exit 1 }
& $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'pc_text unit tests FAILED'; exit 1 }
Write-Host 'pc_text unit tests passed'
exit 0
