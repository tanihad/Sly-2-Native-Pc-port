# Build and run the IPU unit tests (output in build\unit\ipu).
#   powershell -ExecutionPolicy Bypass -File tests\unit\ipu\run.ps1 [-Only <test>] [-Iso <path>] [-Opt -O0]
# test_movie decodes the intro movie from the disc image (orig\disc\*.iso of the main checkout) and
# writes PNG frames to build\unit\ipu; it is skipped if no image is found.
param([string]$Only = "", [string]$Iso = "", [string]$Opt = "-O2")
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {      # worktree: find the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$cc = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
$out = Join-Path $root 'build\unit\ipu'
New-Item -ItemType Directory -Force $out | Out-Null
if (-not $Iso) {
    $img = Get-ChildItem (Join-Path $main 'orig\disc') -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($img) { $Iso = $img.FullName }
}

$rt = Join-Path $root 'runtime'
$ipu = @('ipu.c', 'ipu_vlc.c', 'ipu_idct.c', 'ipu_csc.c') | ForEach-Object { Join-Path $rt $_ }
$common = @((Join-Path $PSScriptRoot 'fakes.c')) + $ipu
$tests = [ordered]@{
    'test_tables' = @()
    'test_idct'   = @()
    'test_regs'   = @()
    'test_bdec'   = @()
    'test_movie'  = @((Join-Path $rt 'debug.c'))
}

$failed = 0
foreach ($t in $tests.Keys) {
    if ($Only -and $t -ne $Only) { continue }
    $exe = Join-Path $out "$t.exe"
    $srcs = @((Join-Path $PSScriptRoot "$t.c")) + $common + $tests[$t]
    & $cc -std=c11 $Opt -Wall -Wextra -Wno-unused-function -I $rt -I $PSScriptRoot @srcs -o $exe
    if ($LASTEXITCODE) { "BUILD FAILED: $t"; $failed++; continue }
    $args = @()
    if ($t -eq 'test_movie') {
        if (-not $Iso) { "test_movie: skipped (no disc image)"; continue }
        $args = @($Iso, $out)
    }
    & $exe @args
    if ($LASTEXITCODE) { "FAILED: $t (exit $LASTEXITCODE)"; $failed++ }
}
if ($failed) { "IPU unit tests: $failed failed"; exit 1 }
"IPU unit tests: all passed"
