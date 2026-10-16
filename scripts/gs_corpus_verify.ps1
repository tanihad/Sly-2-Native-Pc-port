# Check GS recordings made by scripts\gs_record_scene.ps1 against the frames the live run dumped:
# replays build\gsrec\<name>.gsrec with this checkout's GS code (scripts\gs_replay.ps1, software
# renderer) into build\replay\<name>\ and compares every frame both have (scripts\compare_frames.py,
# tolerance 0) with build\gsrec\<name>\rec_frames (or frames).
#   powershell -ExecutionPolicy Bypass -File scripts\gs_corpus_verify.ps1 -Name ep0_museum,ep2_photo
param([Parameter(Mandatory = $true)][string[]]$Name)
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {
    $gitdir = ((Get-Content (Join-Path $root '.git') -Raw) -replace '^gitdir:\s*', '').Trim()
    $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
}
$py = Join-Path $main '.venv\Scripts\python.exe'
foreach ($n in ($Name -split ',' | Where-Object { $_ })) {
    $rec = Join-Path $root "build\gsrec\$n.gsrec"
    $live = Join-Path $root "build\gsrec\$n\rec_frames"
    if (-not (Test-Path $live)) { $live = Join-Path $root "build\gsrec\$n\frames" }
    $out = Join-Path $root "build\replay\$n"
    Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
    $msg = & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'gs_replay.ps1') -Rec $rec -Out $out 2>&1 |
        Where-Object { "$_" -match '^replayed' }
    $cmp = & $py (Join-Path $PSScriptRoot 'compare_frames.py') $live $out
    "$n : $msg"
    $cmp | Where-Object { $_ -notmatch 'identical$' -or $_ -match 'common frames' } | Select-Object -Last 12 | ForEach-Object { "  $_" }
}
