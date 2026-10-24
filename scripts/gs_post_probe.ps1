# Build scripts/gs_post_probe.c against this checkout's GS code and run it: replays a GS recording
# synchronously and, inside VBlank indices [First, Last], snapshots both 512x224 frame buffers at
# every FRAME change, printing which channels changed and their best exact source (channel shuffles).
#   powershell -ExecutionPolicy Bypass -File scripts\gs_post_probe.ps1 -Rec x.gsrec -First 5 -Last 6
param([Parameter(Mandatory = $true)][string]$Rec, [int]$First = 5, [int]$Last = 6, [string]$Out = "")
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$clang = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
$Runtime = Join-Path $root 'runtime'
if (-not $Out) { $Out = Join-Path $root 'build\census' }
New-Item -ItemType Directory -Force $Out | Out-Null
$names = @('gs.c', 'gs_draw.c', 'gs_draw_pixel.c', 'gs_draw_tex.c', 'gs_draw_swz.c', 'gs_setup.c', 'gs_thread.c', 'gs_record.c', 'movie_layer.c', 'debug.c')
$src = @($names | ForEach-Object { Join-Path $Runtime $_ } | Where-Object { Test-Path $_ })
$src += @(Get-ChildItem $Runtime -Filter 'gs_mem*.c' | ForEach-Object { $_.FullName })
$src += Join-Path $PSScriptRoot 'gs_post_probe.c'
$exe = Join-Path $Out 'gs_post_probe.exe'
& $clang -O2 -std=c11 -w -I $Runtime @src -lsynchronization -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'gs_post_probe build failed'; exit 1 }
& $exe $Rec $First $Last 2>$null
exit $LASTEXITCODE
