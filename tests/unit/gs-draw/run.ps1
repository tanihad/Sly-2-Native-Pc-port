# Build and run the gs-draw unit tests (gs.c, gs_draw*.c against a fake gs_mem).
#   powershell -ExecutionPolicy Bypass -File tests\unit\gs-draw\run.ps1
# Builds: the fake gs_mem with the documented VRAM layout (direct-access fast paths), the fake with
# a linear layout (-DFAKE_LINEAR: gs_draw must fall back to gs_mem_read/write), and, once
# runtime/gs_mem.c is no longer the stub, the real runtime/gs_mem*.c. PNGs of every scene go to
# build\unit\gs-draw\ (and ...\linear\, ...\real\).
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {            # worktree: tools live in the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$clang = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
$out = Join-Path $root 'build\unit\gs-draw'
New-Item -ItemType Directory -Force $out | Out-Null

$rt = Join-Path $root 'runtime'
$src = @('gs.c', 'gs_draw.c', 'gs_draw_pixel.c', 'gs_draw_tex.c', 'gs_draw_swz.c', 'gs_setup.c', 'gs_thread.c', 'gs_record.c', 'debug.c') | ForEach-Object { Join-Path $rt $_ }
$src += Join-Path $PSScriptRoot 'test_gs_draw.c'
$flags = @('-O2', '-std=c11', '-Wall', '-Wno-comment', '-I', $rt)
$libs = @('-lsynchronization')
$fake = @(Join-Path $PSScriptRoot 'fake_gs_mem.c')

# test_gs_draw_async: the same scenes through the GS thread with 3 pixel workers (gs_thread.c)
$variants = @(@{ name = 'test_gs_draw'; defs = @(); mem = $fake; dir = $out },
              @{ name = 'test_gs_draw_async'; defs = @('-DTEST_ASYNC'); mem = $fake; dir = (Join-Path $out 'async') },
              @{ name = 'test_gs_draw_linear'; defs = @('-DFAKE_LINEAR'); mem = $fake; dir = (Join-Path $out 'linear') })
$realmem = Join-Path $rt 'gs_mem.c'
if ((Test-Path $realmem) -and -not (Select-String -Path $realmem -Pattern 'STUB' -Quiet)) {
    $mem = @(Get-ChildItem $rt -Filter 'gs_mem*.c' | ForEach-Object { $_.FullName }) + (Join-Path $PSScriptRoot 'real_gs_mem_glue.c')
    $variants += @{ name = 'test_gs_draw_real'; defs = @(); mem = $mem; dir = (Join-Path $out 'real') }
}

$failed = $false
foreach ($v in $variants) {
    New-Item -ItemType Directory -Force $v.dir | Out-Null
    $exe = Join-Path $out ($v.name + '.exe')
    & $clang @flags @($v.defs) @src @($v.mem) @libs -o $exe
    if ($LASTEXITCODE -ne 0) { Write-Host "build of $($v.name) failed"; exit 1 }
    & $exe $v.dir
    if ($LASTEXITCODE -ne 0) { $failed = $true }
}
if ($failed) { Write-Host 'gs-draw unit tests FAILED'; exit 1 }
Write-Host 'gs-draw unit tests passed'
