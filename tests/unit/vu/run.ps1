# Build and run the VU unit tests (runtime/vu.c + runtime/vu_exec.c + a fake GIF PATH1).
#   powershell -ExecutionPolicy Bypass -File tests\unit\vu\run.ps1 [-Opt O1] [-Elf <path to SCUS_973.16>]
# Output goes to build\unit\vu\. Also checks that the disassembler decodes all of the game's microcode.
param([string]$Opt = "O2", [string]$Elf = "")
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {          # worktree: find the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$clang = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
$python = Join-Path $main '.venv\Scripts\python.exe'
if (-not $Elf) { $Elf = Join-Path $main 'orig\SCUS_973.16' }
$out = Join-Path $root 'build\unit\vu'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'test_vu.exe'

& $clang "-$Opt" -std=c11 -Wall -Wextra -Werror -I (Join-Path $root 'runtime') -I $PSScriptRoot `
    (Join-Path $root 'runtime\vu.c') (Join-Path $root 'runtime\vu_exec.c') (Join-Path $PSScriptRoot 'test_vu.c') `
    -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host "build failed"; exit 1 }

& $exe $Elf
$rc = $LASTEXITCODE

if ((Test-Path $Elf) -and (Test-Path $python)) {
    $dis = & $python -I (Join-Path $PSScriptRoot 'vudis.py') --sly2 $Elf --stats
    if ($LASTEXITCODE -ne 0) { Write-Host "disassembler failed"; $rc = 1 }
    elseif ($dis -match 'INVALID') { Write-Host "disassembler: INVALID instructions in game microcode"; $rc = 1 }
    else { Write-Host "disassembler: $((($dis | Select-String '^== program').Count)) microprograms decoded, no invalid instructions" }
}
if ($rc -eq 0) { Write-Host "VU unit tests passed" } else { Write-Host "VU unit tests FAILED" }
exit $rc
