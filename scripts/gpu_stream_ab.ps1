# (perf-p3) Command-stream identity of a frontend change against a baseline frontend: every recording of the corpus
# (tests/gpu/corpus.txt) and of the chaos list (build\gsrec\chaos_*.gsrec, -Chaos) replayed with the frontend alone
# (gs_replay --backend hwdry --front-only --stream-hash) by the baseline exe (same thread) and by the new exe in each
# mode of -Modes; every hash must equal the baseline's (and no PUNT / error exit).
#   .\scripts\gpu_stream_ab.ps1 -Base build\p3\gsr_base\gs_replay.exe -New build\p3\gsr_new\gs_replay.exe
#                               [-Modes "same,thread,pool3"] [-Mixed] [-Chaos] [-Only ep8_boss,chaos_m5_boss]
# Modes: same (the new frontend on the decoding thread), thread (--front-thread), poolN (--front-thread --front-pool N).
param([Parameter(Mandatory = $true)][string]$Base, [Parameter(Mandatory = $true)][string]$New, [string]$Modes = "same,thread,pool3",
      [switch]$Mixed, [switch]$Chaos, [string[]]$Only = @(), [string]$CorpusRoot = "", [switch]$NoCorpus)
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
$recs = @()
if (-not $NoCorpus) {
    . (Join-Path $PSScriptRoot 'gpu_corpus.ps1'); $CorpusRoot = Resolve-GpuCorpus $CorpusRoot $main
    if ($CorpusRoot) {
        $recs += @(Get-Content (Join-Path $root 'tests\gpu\corpus.txt') | Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object {
            $rel = ($_ -split '\|')[0].Trim()
            [pscustomobject]@{ name = ($rel -replace '\\build\\gsrec\\', '_' -replace '\\build\\', '_' -replace '\.gsrec$', ''); path = (Join-Path $CorpusRoot $rel) } })
    }
}
if ($Chaos) {
    $recs += @(Get-ChildItem (Join-Path $root 'build\gsrec') -Filter 'chaos_*.gsrec' -ErrorAction SilentlyContinue | Sort-Object Name |
        ForEach-Object { [pscustomobject]@{ name = $_.BaseName; path = $_.FullName } })
}
if ($Only.Count) { $recs = @($recs | Where-Object { $Only -contains $_.name }) }
$out = Join-Path $root 'build\p3\stream_ab'
New-Item -ItemType Directory -Force $out | Out-Null
$bexe = Join-Path $out ("gsr_base_{0}.exe" -f $PID); Copy-Item (Resolve-Path $Base) $bexe -Force
$nexe = Join-Path $out ("gsr_new_{0}.exe" -f $PID); Copy-Item (Resolve-Path $New) $nexe -Force
function Run-Hash([string]$exe, [string]$rec, [string]$extra, [string]$log) {
    $a = "`"$rec`" - 0 --backend hwdry --adapter hw --front-only --stream-hash $extra"
    if ($Mixed) { $a += ' --mixed' }
    & cmd /c "`"`"$exe`" $a 2>`"$log`"`""
    $rc = $LASTEXITCODE
    $h = ((Get-Content $log | Where-Object { $_ -like 'stream-hash*' }) -join '') -replace 'stream-hash ', ''
    return @($h, $rc)
}
$failed = 0; $n = 0
foreach ($r in $recs) {
    if (-not (Test-Path $r.path)) { Write-Host "$($r.name): MISSING $($r.path)"; $failed++; continue }
    $b = Run-Hash $bexe $r.path '' (Join-Path $out "$($r.name).base.log")
    $line = "{0,-28} base {1} ({2})" -f $r.name, $b[0], $b[1]
    $ok = $b[0] -and $b[1] -eq 0
    foreach ($m in ($Modes -split ',' | Where-Object { $_ })) {
        $extra = if ($m -eq 'same') { '' } elseif ($m -eq 'thread') { '--front-thread' } elseif ($m -match '^pool(\d+)$') { "--front-thread --front-pool $($Matches[1])" } else { $m }
        $x = Run-Hash $nexe $r.path $extra (Join-Path $out "$($r.name).$m.log")
        $same = $x[0] -eq $b[0] -and $x[1] -eq 0
        if (-not $same) { $ok = $false }
        $line += " | $m $(if ($same) { 'ok' } else { "DIFF $($x[0]) ($($x[1]))" })"
    }
    $n++
    if (-not $ok) { $failed++ }
    Write-Host "$(if ($ok) { 'ok  ' } else { 'FAIL' }) $line"
}
Remove-Item $bexe, $nexe -ErrorAction SilentlyContinue
if ($failed) { Write-Host "gpu_stream_ab: $failed of $n FAILED"; exit 1 }
Write-Host "gpu_stream_ab: $n recording(s) identical in every mode"
exit 0
