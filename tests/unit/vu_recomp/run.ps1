# Differential test of the VU recompiler: random microprograms (gen.py) compiled by recomp/vu_recomp.py
# must give exactly the interpreter's results, timing and XGKICKs (test_vu_recomp.c).
#   powershell -ExecutionPolicy Bypass -File tests\unit\vu_recomp\run.ps1 [-Count 160] [-Seed 1] [-States 24] [-Opt O2]
# Output goes to build\unit\vu_recomp\.
param([int]$Count = 160, [int]$Seed = 1, [int]$States = 24, [string]$Opt = "O2", [string]$Out = "")
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
$out = if ($Out) { [IO.Path]::GetFullPath((Join-Path $root $Out)) } else { Join-Path $root 'build\unit\vu_recomp' }
New-Item -ItemType Directory -Force $out | Out-Null

& $python (Join-Path $PSScriptRoot 'gen.py') $out --count $Count --seed $Seed
if ($LASTEXITCODE -ne 0) { Write-Host "generator failed"; exit 1 }

# compile the generated units in parallel (one clang per file), then link
$rt = Join-Path $root 'runtime'
$gen = Get-ChildItem (Join-Path $out 'gen') -Filter *.c
$jobs = @()
foreach ($c in $gen) {
    $o = Join-Path $out ($c.BaseName + '.o')
    $p = Start-Process -FilePath $clang -ArgumentList @("-$Opt", '-std=c11', '-w', '-I', "`"$rt`"", '-c', "`"$($c.FullName)`"", '-o', "`"$o`"") -NoNewWindow -PassThru
    $null = $p.Handle                    # keeps ExitCode available after the process ends
    $jobs += $p
}
foreach ($j in $jobs) { $j.WaitForExit(); if ($j.ExitCode -ne 0) { Write-Host "compiling generated code failed"; exit 1 } }
$objs = $gen | ForEach-Object { Join-Path $out ($_.BaseName + '.o') }
$exe = Join-Path $out 'test_vu_recomp.exe'
& $clang "-$Opt" -std=c11 -Wall -Wextra -Werror -I $rt (Join-Path $rt 'vu.c') (Join-Path $rt 'vu_exec.c') `
    (Join-Path $PSScriptRoot 'test_vu_recomp.c') @objs -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host "build failed"; exit 1 }

& $exe (Join-Path $out 'programs.bin') $States
$rc = $LASTEXITCODE
if ($rc -eq 0) { Write-Host "VU recompiler tests passed" } else { Write-Host "VU recompiler tests FAILED" }
exit $rc
