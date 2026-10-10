# Render a GS recording (PS2_GS_RECORD, runtime/gs_record.c) with the GS code of a checkout.
#   powershell -ExecutionPolicy Bypass -File scripts\gs_replay.ps1 -Rec build\x.gsrec -Out build\replay_new [-Runtime <dir>] [-Workers 0]
#   ... -Bench: no pictures, just the time the GS code takes (a GS-only benchmark, no EE involved)
# -Runtime: the runtime/ directory whose GS code renders it (default: this checkout's), e.g. an
# older version exported with `git archive`, to compare two renderers on the same input:
#   scripts\compare_frames.py build\replay_old build\replay_new
# -Workers > 0 renders through the GS thread with that many pixel workers (deferred drawing).
param([Parameter(Mandatory = $true)][string]$Rec, [string]$Out = "", [string]$Runtime = "", [int]$Workers = 0,
      [switch]$Bench)
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {            # worktree: tools live in the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$clang = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
if (-not $Runtime) { $Runtime = Join-Path $root 'runtime' }
$Runtime = (Resolve-Path $Runtime).Path
if (-not $Out) { $Out = Join-Path $root 'build\gs_replay' }
New-Item -ItemType Directory -Force $Out | Out-Null
$names = @('gs.c', 'gs_draw.c', 'gs_draw_pixel.c', 'gs_draw_tex.c', 'gs_draw_swz.c', 'gs_thread.c', 'gs_record.c', 'debug.c')
$src = @($names | ForEach-Object { Join-Path $Runtime $_ } | Where-Object { Test-Path $_ })
$src += @(Get-ChildItem $Runtime -Filter 'gs_mem*.c' | ForEach-Object { $_.FullName })
$src += Join-Path $PSScriptRoot 'gs_replay.c'
$exe = Join-Path $Out 'gs_replay.exe'
& $clang -O2 -std=c11 -w -I $Runtime @src -lsynchronization -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'gs_replay build failed'; exit 1 }
& $exe $Rec $(if ($Bench) { '-' } else { $Out }) $Workers
exit $LASTEXITCODE
