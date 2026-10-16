# Build and run the GPU frontend unit tests (runtime/gpu/gpu_front.c, gpu_own.c, gpu_clut.c with the
# software GS and the real gs_mem). No GPU needed.
#   powershell -ExecutionPolicy Bypass -File tests\unit\gs-front\run.ps1
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
$out = Join-Path $root 'build\unit\gs-front'
New-Item -ItemType Directory -Force $out | Out-Null
$rt = Join-Path $root 'runtime'
$src = @('gs.c', 'gs_draw.c', 'gs_draw_pixel.c', 'gs_draw_tex.c', 'gs_draw_swz.c', 'gs_setup.c', 'gs_thread.c', 'gs_record.c', 'debug.c') | ForEach-Object { Join-Path $rt $_ }
$src += @(Get-ChildItem $rt -Filter 'gs_mem*.c' | ForEach-Object { $_.FullName })
$src += @('gpu_front.c', 'gpu_own.c', 'gpu_clut.c', 'gpu_fthread.c') | ForEach-Object { Join-Path (Join-Path $rt 'gpu') $_ }
$src += Join-Path $PSScriptRoot 'test_gs_front.c'
$exe = Join-Path $out 'test_gs_front.exe'
& $clang -O2 -std=c11 -Wall -Wno-comment -I $rt @src -lsynchronization -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'gs-front build failed'; exit 1 }
& $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'gs-front unit tests FAILED'; exit 1 }
Write-Host 'gs-front unit tests passed'
exit 0
