# Build and run the frame pacing / GPU governor unit tests:  powershell -ExecutionPolicy Bypass -File tests\unit\pacing\run.ps1
# Links runtime/gpu/gpu_pace.c (pure decision logic: present delay, present flags, VRR detection, internal scale
# governor) with test_pacing.c. Output: build\unit\pacing\.
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
$out = Join-Path $root 'build\unit\pacing'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'test_pacing.exe'

& $clang -std=c11 -O1 -g -Wall -Wextra -Werror -I (Join-Path $root 'runtime') `
    (Join-Path $PSScriptRoot 'test_pacing.c') (Join-Path $root 'runtime\gpu\gpu_pace.c') -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'pacing tests: build failed'; exit 1 }

& $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'pacing tests: FAILED'; exit 1 }
# (pc-fps) the frame rate setting: runtime/frame_rate.c with the settings model
$exe2 = Join-Path $out 'test_frame_rate.exe'
& $clang -std=c11 -O1 -g -Wall -Wextra -Werror -I (Join-Path $root 'runtime') `
    (Join-Path $PSScriptRoot 'test_frame_rate.c') (Join-Path $root 'runtime\frame_rate.c') (Join-Path $root 'runtime\settings.c') `
    (Join-Path $root 'runtime\gpu\gpu_pace.c') -o $exe2 -luser32 -lshell32 -lole32
if ($LASTEXITCODE -ne 0) { Write-Host 'pacing tests: frame rate build failed'; exit 1 }
& $exe2
if ($LASTEXITCODE -ne 0) { Write-Host 'pacing tests: frame rate FAILED'; exit 1 }
Write-Host 'pacing tests: all passed'
exit 0
