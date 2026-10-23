# Build and run the VIF unit tests (runtime/vif.c + fakes) into build/unit/vif/.
#   powershell -ExecutionPolicy Bypass -File tests\unit\vif\run.ps1
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$clang = Join-Path $root 'tools\llvm-mingw\bin\clang.exe'
if (-not (Test-Path $clang)) { . (Join-Path $root 'scripts\project.ps1'); $clang = $ProjectClang }   # worktree: the main checkout's
$out = Join-Path $root 'build\unit\vif'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'test_vif.exe'

& $clang -std=c11 -O2 -g -Wall -Wextra -Wno-unused-function -Werror `
    -I (Join-Path $root 'runtime') -I $PSScriptRoot `
    (Join-Path $root 'runtime\vif.c') (Join-Path $PSScriptRoot 'fakes.c') (Join-Path $PSScriptRoot 'test_vif.c') `
    -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host "build failed"; exit 1 }

& $exe
if ($LASTEXITCODE -ne 0) { Write-Host "VIF unit tests FAILED"; exit 1 }
Write-Host "VIF unit tests passed"
