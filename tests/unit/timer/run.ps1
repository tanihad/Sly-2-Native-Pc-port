# Build and run the EE timer unit tests:  powershell -ExecutionPolicy Bypass -File tests\unit\timer\run.ps1
# Links runtime/timer.c with the fakes in test_timer.c only. Output: build\unit\timer\.
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
$out = Join-Path $root 'build\unit\timer'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'test_timer.exe'

& $clang -std=c11 -O1 -g -Wall -Wextra -Werror -I (Join-Path $root 'runtime') `
    (Join-Path $PSScriptRoot 'test_timer.c') (Join-Path $root 'runtime\timer.c') -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'timer tests: build failed'; exit 1 }

& $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'timer tests: FAILED'; exit 1 }
Write-Host 'timer tests: all passed'
exit 0
