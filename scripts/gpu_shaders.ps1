# Build scripts\hlslc.c (build\tools\hlslc.exe) and compile the GPU backend's shaders
# (runtime\gpu\shaders\perms.txt) into build\gen\gpu_shaders.h. Used by gs_replay.ps1 and the
# gs-hw unit tests; scripts\build_game.py does the same in Python. Exit code 1 on any error.
#   powershell -ExecutionPolicy Bypass -File scripts\gpu_shaders.ps1 [-Root <checkout>] [-Gen <dir>]
param([string]$Root = "", [string]$Gen = "")
if (-not $Root) { $Root = Split-Path $PSScriptRoot -Parent }
$main = $Root
if (-not (Test-Path (Join-Path $Root 'tools\llvm-mingw'))) {            # worktree: tools live in the main checkout
    $gitfile = Join-Path $Root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$clang = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
$tools = Join-Path $Root 'build\tools'
if (-not $Gen) { $Gen = Join-Path $Root 'build\gen' }
New-Item -ItemType Directory -Force $tools, $Gen | Out-Null
$src = Join-Path $Root 'scripts\hlslc.c'
$exe = Join-Path $tools 'hlslc.exe'
if (-not (Test-Path $exe) -or (Get-Item $src).LastWriteTime -gt (Get-Item $exe).LastWriteTime) {
    & $clang -O1 -std=c11 -Wall -Wextra -o $exe $src
    if ($LASTEXITCODE -ne 0) { Write-Host 'hlslc build failed'; exit 1 }
}
& $exe (Join-Path $Root 'runtime\gpu\shaders\perms.txt') (Join-Path $Gen 'gpu_shaders.h')
exit $LASTEXITCODE
