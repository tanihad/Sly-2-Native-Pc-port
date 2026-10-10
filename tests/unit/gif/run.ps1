# Build and run the GIF unit tests: powershell -ExecutionPolicy Bypass -File tests\unit\gif\run.ps1
# Output in build\unit\gif\. Works from the main checkout or a git worktree.
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {   # worktree: tools live in the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$clang = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
$out = Join-Path $root 'build\unit\gif'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'test_gif.exe'

& $clang -std=c11 -O1 -g -Wall -Wextra -Werror `
    -I (Join-Path $root 'runtime') `
    (Join-Path $PSScriptRoot 'test_gif.c') (Join-Path $root 'runtime\gif.c') -o $exe
if ($LASTEXITCODE) { "build failed"; exit 1 }
& $exe
if ($LASTEXITCODE) { "tests FAILED"; exit 1 }
"tests passed"
