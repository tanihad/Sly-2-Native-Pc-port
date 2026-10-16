# Equivalence of the frontend thread (runtime/gpu/gpu_fthread.c, notes/d3d11-renderer.md M2): every
# corpus recording (tests/gpu/corpus.txt) replayed with the frontend alone (gs_replay -Backend hwdry
# -FrontOnly -StreamHash) on the decoding thread and on its own thread; the hashes of every frame's
# command stream and data must be identical (and 0 PUNT).
#   powershell -ExecutionPolicy Bypass -File scripts\gpu_fthread_check.ps1 [-Only ep8_boss,gpu-C_thiefnet] [-CorpusRoot <dir>] [-Mixed]
param([string[]]$Only = @(), [string]$CorpusRoot = "", [switch]$Mixed)
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
if (-not $CorpusRoot) { $CorpusRoot = Join-Path (Split-Path $main -Parent) 'sly2-worktrees' }
$out = Join-Path $root 'build\m2\fthread_check'
New-Item -ItemType Directory -Force $out | Out-Null
$recs = Get-Content (Join-Path $root 'tests\gpu\corpus.txt') | Where-Object { $_ -and -not $_.StartsWith('#') } |
    ForEach-Object { ($_ -split '\|')[0].Trim() }
$failed = 0
$n = 0
foreach ($rel in $recs) {
    $name = ($rel -replace '\\build\\gsrec\\', '_' -replace '\\build\\', '_' -replace '\.gsrec$', '')
    if ($Only.Count -and -not ($Only | Where-Object { $name -eq $_ })) { continue }
    $rec = Join-Path $CorpusRoot $rel
    if (-not (Test-Path $rec)) { Write-Host "$name : MISSING $rec"; $failed++; continue }
    $h = @{}
    foreach ($t in 'same', 'thread') {
        $log = Join-Path $out "$name.$t.log"
        $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'gs_replay.ps1'), '-Rec', $rec,
               '-Out', (Join-Path $out 'bin'), '-Bench', '-Backend', 'hwdry', '-FrontOnly', '-StreamHash', '-Log', $log)
        if ($t -eq 'thread') { $a += '-FrontThread' }
        if ($Mixed) { $a += '-Mixed' }        # (M5: the mixed-frame stream: FF records, escalation)
        & powershell @a | Out-Null
        $h[$t] = ((Get-Content $log | Where-Object { $_ -like 'stream-hash*' }) -join '') -replace 'stream-hash ', ''
        $h["$t.rc"] = $LASTEXITCODE
    }
    $n++
    $ok = $h['same'] -and $h['same'] -eq $h['thread'] -and $h['same.rc'] -eq 0 -and $h['thread.rc'] -eq 0
    if (-not $ok) { $failed++ }
    Write-Host ("{0,-28} {1,-4} same {2} thread {3} (exit {4}/{5})" -f $name, $(if ($ok) { 'ok' } else { 'FAIL' }), $h['same'], $h['thread'], $h['same.rc'], $h['thread.rc'])
}
if ($failed) { Write-Host "gpu_fthread_check: $failed of $n FAILED"; exit 1 }
Write-Host "gpu_fthread_check: $n recording(s) identical"
exit 0
