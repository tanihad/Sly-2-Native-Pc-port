# M9 exactness gate (notes/d3d11-renderer.md M9: "every optimisation bit-exact against the un-optimised hw output at
# S=1 and S=3, lockstep hw vs hw"): every recording of tests/gpu/corpus.txt is replayed in mixed mode (the game's GPU
# tiers) by the optimised gs_replay and by a baseline gs_replay built from the un-optimised runtime, each writing a hash
# of every sample of every target every -Every VBlanks (gs_replay --sample-hash) and, with -Game, a hash of every
# GPU picture (--game, game-pic-hash); the two must be identical.
#   powershell -ExecutionPolicy Bypass -File scripts\m9_exact.ps1 -Exe build\cur\gs_replay.exe -BaseExe build\basert\gs_replay.exe
#       [-Scales 1,3] [-Every 20] [-Only ep8_boss,gpu-C_ep1_hub] [-Out build\exact] [-Game] [-Extra "--x"] [-BaseExtra "--y"]
# Baseline hashes are kept in -Out\base_S<scale>_E<every>\ and reused (delete them after a baseline change). One line per
# recording and scale; exit 1 on any difference.
param([Parameter(Mandatory = $true)][string]$Exe, [Parameter(Mandatory = $true)][string]$BaseExe, [string]$Scales = "1,3",
      [int]$Every = 20, [string[]]$Only = @(), [string]$Out = "", [switch]$Game, [string]$Extra = "", [string]$BaseExtra = "",
      [string]$CorpusRoot = "")
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
. (Join-Path $PSScriptRoot 'project.ps1'); . (Join-Path $PSScriptRoot 'gpu_corpus.ps1')
$CorpusRoot = Resolve-GpuCorpus $CorpusRoot $MainRoot
if (-not $CorpusRoot) { exit 0 }
$root = Split-Path $PSScriptRoot -Parent
if (-not $Out) { $Out = Join-Path $root 'build\exact' }
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$Exe = (Resolve-Path $Exe).Path
$BaseExe = (Resolve-Path $BaseExe).Path
$runO = Join-Path $Out ("opt_{0}.exe" -f $PID); Copy-Item $Exe $runO -Force
$runB = Join-Path $Out ("base_{0}.exe" -f $PID); Copy-Item $BaseExe $runB -Force
$lines = Get-Content (Join-Path $root 'tests\gpu\corpus.txt') | Where-Object { $_ -and -not $_.StartsWith('#') }
$failed = 0
function Run($exe, $rec, $scale, $aspect, $hash, $log, $extra, $game) {
    $a = @($rec, '-', '0', '--backend', 'hw', '--adapter', 'hw')
    if ($game) { $a += @('--game') } else { $a += @('--mixed', '--sample-hash', $hash, '--sample-every', "$Every") }
    if ($scale -ne 1) { $a += @('--scale', "$scale") }
    if ($aspect) { $a += @('--aspect', $aspect) }
    $a += @($extra -split ' ' | Where-Object { $_ })
    $quoted = ($a | ForEach-Object { '"' + $_ + '"' }) -join ' '
    & cmd /c "`"`"$exe`" $quoted 2>`"$log`"`""
    return $LASTEXITCODE
}
foreach ($line in $lines) {
    $rel = ($line -split '\|')[0].Trim()
    $aspect = if ($line -match 'aspect=(\d+:\d+)') { $Matches[1] } else { '' }
    $name = ($rel -replace '\\build\\gsrec\\', '_' -replace '\\build\\', '_' -replace '\.gsrec$', '')
    if ($Only.Count -and -not ($Only | Where-Object { $name -like "*$_*" })) { continue }
    $rec = Join-Path $CorpusRoot $rel
    if (-not (Test-Path $rec)) { Write-Host "$name : MISSING"; $failed++; continue }
    foreach ($s in ($Scales -split ',')) {
        $s = [int]$s
        $bdir = Join-Path $Out ("base_S{0}_E{1}" -f $s, $Every); New-Item -ItemType Directory -Force $bdir | Out-Null
        $suffix = if ($Game) { 'game' } else { 'smp' }
        $bh = Join-Path $bdir "$name.$suffix.txt"
        $oh = Join-Path $Out "$name.S$s.$suffix.txt"
        if (-not (Test-Path $bh)) {
            $blog = Join-Path $bdir "$name.$suffix.log"
            $rc = Run $runB $rec $s $aspect $bh $blog $BaseExtra $Game
            if ($Game) { (Get-Content $blog | Where-Object { $_ -match '^game-pic-hash ' }) | Set-Content -Encoding ascii $bh }
            if ($rc -ne 0 -and -not $Game) { Write-Host "$name S$s : baseline rc $rc"; }
        }
        $olog = Join-Path $Out "$name.S$s.$suffix.log"
        $rc = Run $runO $rec $s $aspect $oh $olog $Extra $Game
        if ($Game) { (Get-Content $olog | Where-Object { $_ -match '^game-pic-hash ' }) | Set-Content -Encoding ascii $oh }
        $a = @(Get-Content $bh); $b = @(Get-Content $oh)
        $n = [Math]::Max($a.Count, $b.Count); $diff = 0; $first = ''
        for ($i = 0; $i -lt $n; $i++) { if ($a[$i] -ne $b[$i]) { $diff++; if (-not $first) { $first = "base '$($a[$i])' vs '$($b[$i])'" } } }
        if ($a.Count -eq 0) { $diff++; $first = 'no baseline lines' }
        if ($diff) { $failed++; Write-Host ("{0} S{1}: {2} of {3} lines differ; first: {4}" -f $name, $s, $diff, $n, $first) }
        else { Write-Host ("{0} S{1}: identical ({2} lines)" -f $name, $s, $n); Remove-Item $olog -ErrorAction SilentlyContinue }
    }
}
Remove-Item $runO, $runB -ErrorAction SilentlyContinue
if ($failed) { Write-Host "m9_exact: $failed FAILED"; exit 1 }
Write-Host "m9_exact: all identical"
