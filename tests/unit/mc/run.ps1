# Build and run the memory card service unit tests (runtime/mc.c + tests/unit/mc/test_mc.c).
# Output and the test card directory go to build\unit\mc\.
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
$out = Join-Path $root 'build\unit\mc'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'test_mc.exe'

& $clang -std=c11 -O1 -g -Wall -Wextra -Werror -I (Join-Path $root 'runtime') `
    (Join-Path $root 'runtime\mc.c') (Join-Path $PSScriptRoot 'test_mc.c') -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'build failed'; exit 1 }

& $exe
$first = $LASTEXITCODE
& $exe persist                                      # a second process: the card must still be there
$second = $LASTEXITCODE
if ($first -ne 0 -or $second -ne 0) { Write-Host 'mc unit tests FAILED'; exit 1 }
Write-Host 'mc unit tests passed'
exit 0
