# Build and run the disc image tests (runtime/disc.c, runtime/launch.c's disc choice, settings.c's
# storage of it). Synthetic images go to build\unit\disc\work; if the owner's disc image is in
# orig\disc\ and orig\SCUS_973.16 exists, the real image is checked as well (read only).
#   powershell -ExecutionPolicy Bypass -File tests\unit\disc\run.ps1
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {      # worktree: tools live in the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$clang = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
$rt = Join-Path $root 'runtime'
$out = Join-Path $root 'build\unit\disc'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'test_disc.exe'

& $clang -std=c11 -O1 -g -Wall -Wextra -Werror -I $rt (Join-Path $PSScriptRoot 'test_disc.c') `
    (Join-Path $rt 'disc.c') (Join-Path $rt 'launch.c') (Join-Path $rt 'settings.c') -luser32 -o $exe
if ($LASTEXITCODE) { Write-Output "build failed"; exit 1 }

$testArgs = @()
$iso = Get-ChildItem (Join-Path $main 'orig\disc') -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1
$elf = Join-Path $main 'orig\SCUS_973.16'
if ($iso -and (Test-Path $elf)) { $testArgs = @('--real', $iso.FullName, $elf) }
& $exe @testArgs
exit $LASTEXITCODE
