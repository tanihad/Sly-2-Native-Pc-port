# Build and run the GPU backend (runtime/gpu) unit tests on the hardware adapter and on WARP:
# capabilities, the conformance kernel vs C, render thread + command rings, picture round trip,
# "both" mode on GS scenes, the swap chain on a small window, and (M3) the gs-draw scenes with the REF
# tier alongside: GS memory of the GPU compared with the software renderer's before every read.
#   powershell -ExecutionPolicy Bypass -File tests\unit\gs-hw\run.ps1 [-Adapter hw|warp|both] [-NoWindow] [-RefOnly]
#       [-Modes ref,mixed,mixdef]
# -RefOnly: only the gs-draw scenes (REF tier, and since M5 the mixed FF/ROV frames: exact and default);
# -Modes: which of those variants (default all three).
param([string]$Adapter = 'both', [switch]$NoWindow, [int]$Vectors = 131072, [switch]$RefOnly, [string[]]$Modes = @())
$Modes = @($Modes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {            # worktree: tools live in the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$clang = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
$out = Join-Path $root 'build\unit\gs-hw'
New-Item -ItemType Directory -Force $out | Out-Null
$gen = Join-Path $root 'build\gen'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\gpu_shaders.ps1') -Root $root -Gen $gen
if ($LASTEXITCODE -ne 0) { Write-Host 'shader build failed'; exit 1 }

$rt = Join-Path $root 'runtime'
$common = @('gs.c', 'gs_draw.c', 'gs_draw_pixel.c', 'gs_draw_tex.c', 'gs_draw_swz.c', 'gs_setup.c', 'gs_thread.c', 'gs_record.c', 'debug.c') | ForEach-Object { Join-Path $rt $_ }
$common += @(Get-ChildItem $rt -Filter 'gs_mem*.c' | ForEach-Object { $_.FullName })
$common += @(Get-ChildItem (Join-Path $rt 'gpu') -Filter '*.c' | ForEach-Object { $_.FullName })
if (-not $RefOnly) {
    $src = $common + @(Join-Path $PSScriptRoot 'test_gs_hw.c')
    $exe = Join-Path $out 'test_gs_hw.exe'
    & $clang -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-comment -I $rt -I (Join-Path $rt 'gpu') -I $gen @src `
        -lsynchronization -luser32 -lgdi32 -o $exe
    if ($LASTEXITCODE -ne 0) { Write-Host 'gs-hw build failed'; exit 1 }
    # a uniquely named copy: other agents may run test binaries of the same name
    $run = Join-Path $out ("test_gs_hw_{0}.exe" -f $PID)
    Copy-Item $exe $run -Force
    $testArgs = @('--adapter', $Adapter, '--vectors', "$Vectors")
    if ($NoWindow) { $testArgs += '--no-window' }
    & $run @testArgs
    $rc = $LASTEXITCODE
    Remove-Item $run -ErrorAction SilentlyContinue
    if ($rc -ne 0) { Write-Host 'gs-hw unit tests FAILED'; exit 1 }
}

# M3: the gs-draw scenes (tests/unit/gs-draw/test_gs_draw.c built with TEST_HW_REF, real gs_mem) with
# the REF tier alongside, on each adapter
$gd = Join-Path $root 'tests\unit\gs-draw'
$refsrc = $common + @((Join-Path $gd 'test_gs_draw.c'), (Join-Path $gd 'real_gs_mem_glue.c'))
# M5: the same scenes in mixed frames (FF + ROV tiers): "mixed" exact (S/T/Q stepped exactly, dual-source
# blends on the ROV tier: every FF class bit-exact) and "mixdef" the default mode (hardware S/T/Q, dual-source
# blends +-1: the tolerances of notes/d3d11-renderer.md 10.2)
$variants = @(@('ref', @('-DTEST_HW_REF')), @('mixed', @('-DTEST_HW_REF', '-DTEST_HW_MIXED')),
              @('mixdef', @('-DTEST_HW_REF', '-DTEST_HW_MIXED', '-DTEST_HW_MIXED_DEFAULT')))
if ($Modes.Count) { $variants = @($variants | Where-Object { $Modes -contains $_[0] }) }
$adapters = if ($Adapter -eq 'both') { @('hw', 'warp') } else { @($Adapter) }
$refFailed = $false
foreach ($v in $variants) {
    $refexe = Join-Path $out ('test_gs_draw_hw' + $v[0] + '.exe')
    $defs = $v[1]
    & $clang -O2 -std=c11 -Wall -Wno-comment -Wno-unused-function @defs -I $rt -I (Join-Path $rt 'gpu') -I $gen @refsrc `
        -lsynchronization -luser32 -lgdi32 -o $refexe
    if ($LASTEXITCODE -ne 0) { Write-Host "gs-draw $($v[0]) build failed"; exit 1 }
    foreach ($a in $adapters) {
        $dir = Join-Path $out ($v[0] + '_' + $a)
        New-Item -ItemType Directory -Force $dir | Out-Null
        $rrun = Join-Path $out ("test_gs_draw_hw{0}_{1}_{2}.exe" -f $v[0], $a, $PID)
        Copy-Item $refexe $rrun -Force
        Write-Host "gs-draw scenes on $a ($($v[0]))"
        & $rrun $dir $a
        if ($LASTEXITCODE -ne 0) { $refFailed = $true; Write-Host "gs-hw $($v[0]) scenes on $a FAILED" }
        # (M11) the exact mixed scenes again without rasterizer-ordered views (PS2_GPU_NO_ROV: plain UAV draws of disjoint
        # primitives) - the same exact results
        if ($v[0] -eq 'mixed' -or $v[0] -eq 'ref') {
            $env:PS2_GPU_NO_ROV = '1'
            $ndir = Join-Path $out ($v[0] + '_norov_' + $a)
            New-Item -ItemType Directory -Force $ndir | Out-Null
            Write-Host "gs-draw scenes on $a ($($v[0]), no ROVs)"
            & $rrun $ndir $a
            if ($LASTEXITCODE -ne 0) { $refFailed = $true; Write-Host "gs-hw $($v[0]) scenes on $a without ROVs FAILED" }
            Remove-Item Env:\PS2_GPU_NO_ROV
        }
        Remove-Item $rrun -ErrorAction SilentlyContinue
    }
}
if ($refFailed) { Write-Host 'gs-hw REF / mixed scenes FAILED'; exit 1 }
Write-Host 'gs-hw unit tests passed'
exit 0
