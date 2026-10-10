# Build and run the pad (padman HLE) unit tests: runtime/pad.c + test_pad.c with fake neighbours.
#   powershell -ExecutionPolicy Bypass -File tests\unit\pad\run.ps1
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = (Resolve-Path (Join-Path $here '..\..\..')).Path
$clang = Join-Path $root 'tools\llvm-mingw\bin\clang.exe'
if (-not (Test-Path $clang)) { $clang = 'C:\Users\tanih\Projects\sly2-decomp\tools\llvm-mingw\bin\clang.exe' }
$out = Join-Path $root 'build\unit\pad'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'test_pad.exe'

& $clang -std=c11 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter `
    -I (Join-Path $root 'runtime') `
    (Join-Path $root 'runtime\pad.c') (Join-Path $here 'test_pad.c') -o $exe
if ($LASTEXITCODE -ne 0) { Write-Error 'build failed'; exit 1 }
& $exe
exit $LASTEXITCODE
