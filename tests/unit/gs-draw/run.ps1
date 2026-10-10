# Build and run the gs-draw unit tests (gs.c, gs_draw*.c against a fake gs_mem).
#   powershell -ExecutionPolicy Bypass -File tests\unit\gs-draw\run.ps1
# Two builds: the fake gs_mem with the documented VRAM layout (direct-access fast path) and with a
# linear layout (-DFAKE_LINEAR: gs_draw must fall back to gs_mem_read/write). PNGs of every scene go
# to build\unit\gs-draw\ (and ...\linear\).
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
New-Item -ItemType Directory -Force (Join-Path $out 'linear') | Out-Null

$rt = Join-Path $root 'runtime'
$src = @('gs.c', 'gs_draw.c', 'gs_draw_pixel.c', 'gs_draw_tex.c', 'gs_draw_swz.c', 'debug.c') | ForEach-Object { Join-Path $rt $_ }
$src += Join-Path $PSScriptRoot 'fake_gs_mem.c'
$src += Join-Path $PSScriptRoot 'test_gs_draw.c'
$flags = @('-O2', '-std=c11', '-Wall', '-Wno-comment', '-I', $rt)

$failed = $false
foreach ($variant in @(@{ name = 'test_gs_draw'; defs = @(); dir = $out },
                       @{ name = 'test_gs_draw_linear'; defs = @('-DFAKE_LINEAR'); dir = (Join-Path $out 'linear') })) {
    $exe = Join-Path $out ($variant.name + '.exe')
    & $clang @flags @($variant.defs) @src -o $exe
    if ($LASTEXITCODE -ne 0) { Write-Host "build of $($variant.name) failed"; exit 1 }
    & $exe $variant.dir
    if ($LASTEXITCODE -ne 0) { $failed = $true }
}
if ($failed) { Write-Host 'gs-draw unit tests FAILED'; exit 1 }
Write-Host 'gs-draw unit tests passed'
