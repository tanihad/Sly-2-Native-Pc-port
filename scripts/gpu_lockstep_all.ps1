# Lockstep of the GPU backend against the software renderer on every corpus recording
# (tests/gpu/corpus.txt; notes/d3d11-renderer.md 10.1, M4/M5 gates): GS memory compared (all 4 MB, sample 0)
# at every FRAME/ZBUF change and every VBlank, --no-resync (a difference is never repaired from sw
# memory), reference-composed pictures compared too. One line per recording; exit 1 on any divergence.
# A diverging recording's log (<Out>\<name>\log.txt) holds the bisection of its first divergence.
#   powershell -ExecutionPolicy Bypass -File scripts\gpu_lockstep_all.ps1 [-Only ep8_boss,gpu-C_thiefnet]
#       [-Adapter hw|warp] [-Scale N] [-Out build\lockstep] [-CorpusRoot <dir>]
#       [-Mixed [-Exact] [-StqExact] [-FfRovMask <bits>] [-StraddleRov] [-SplitFf]]
# Default: the REF tier (M4 gate, every difference diverges). -Mixed (M5): the FF and ROV tiers in one frame,
# compared with the tolerances of 10.2 (colour words within 16 tolerated and counted; alpha, Z and CPU data
# exact; pictures: PSNR >= 40 dB and <= 0.2% pixels above 16); -Exact: mixed mode compared exactly (with
# -StqExact and -FfRovMask 0x10 every FF class but the dual-source blend must be bit-exact).
param([string[]]$Only = @(), [ValidateSet('hw', 'warp')][string]$Adapter = 'hw', [int]$Scale = 1, [string]$Out = "",
      [string]$CorpusRoot = "", [switch]$Mixed, [switch]$Exact, [switch]$StqExact, [string]$FfRovMask = "", [switch]$StraddleRov, [switch]$SplitFf,
      [string]$Exe = "")
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {            # worktree: tools live in the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
if (-not $CorpusRoot) { $CorpusRoot = Join-Path (Split-Path $main -Parent) 'sly2-worktrees' }
if (-not $Out) { $Out = Join-Path $root 'build\lockstep' }
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
# build gs_replay once (a short benchmark replay of the first recording)
$recs = Get-Content (Join-Path $root 'tests\gpu\corpus.txt') | Where-Object { $_ -and -not $_.StartsWith('#') } |
    ForEach-Object { ($_ -split '\|')[0].Trim() }
if ($Exe) {                              # a prebuilt gs_replay (a series of runs with one build)
    New-Item -ItemType Directory -Force $Out | Out-Null
    Copy-Item $Exe (Join-Path $Out 'gs_replay.exe') -Force
    $exe = Join-Path $Out 'gs_replay.exe'
} else {
    $exe = Join-Path $Out 'gs_replay.exe'
    Remove-Item $exe -ErrorAction SilentlyContinue
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'gs_replay.ps1') -Rec (Join-Path $CorpusRoot $recs[0]) `
        -Backend hw -Ref -Bench -Out $Out -Log (Join-Path $Out 'build.log') | Out-Null
    if (-not (Test-Path $exe)) { Write-Host 'gs_replay build failed'; exit 1 }
}
$mode = @('--ref')
if ($Mixed) {
    $mode = @('--mixed')
    if ($Exact) { $mode += '--lockstep-exact' }
    if ($StqExact) { $mode += '--ff-stq-exact' }
    if ($FfRovMask) { $mode += @('--ff-rov-mask', $FfRovMask) }
    if ($StraddleRov) { $mode += '--straddle-rov' }
    if ($SplitFf) { $mode += '--split-ff' }
}
$failed = 0
$total = 0
foreach ($rel in $recs) {
    $name = ($rel -replace '\\build\\gsrec\\', '_' -replace '\\build\\', '_' -replace '\.gsrec$', '')
    if ($Only.Count -and -not ($Only | Where-Object { $name -like "*$_*" })) { continue }
    $rec = Join-Path $CorpusRoot $rel
    if (-not (Test-Path $rec)) { Write-Host "$name : MISSING $rec"; $failed++; continue }
    $o = Join-Path $Out $name
    New-Item -ItemType Directory -Force $o | Out-Null
    $run = Join-Path $o ("gs_replay_{0}.exe" -f $PID)      # (a uniquely named copy)
    Copy-Item $exe $run -Force
    $t0 = Get-Date
    & $run $rec $o 0 --backend both --adapter $Adapter @mode --lockstep --no-resync --scale $Scale 2> (Join-Path $o 'log.txt') | Out-Null
    $rc = $LASTEXITCODE
    Remove-Item $run -ErrorAction SilentlyContinue
    $secs = [int]((Get-Date) - $t0).TotalSeconds
    $log = Join-Path $o 'log.txt'
    $lk = (Select-String -Path $log -Pattern '^lockstep: \d+ comparisons' | Select-Object -First 1).Line
    $pic = (Select-String -Path $log -Pattern '^both ref:' | Select-Object -First 1).Line
    $m = [regex]::Match("$lk", 'lockstep: (\d+) comparisons, (\d+) differ')
    if ($m.Success) { $total += [int]$m.Groups[1].Value }
    if ($Mixed -and -not $Exact) {
        $gate = (Select-String -Path $log -Pattern '^both mixed:' | Select-Object -First 1).Line
        $tol = (Select-String -Path $log -Pattern '^lockstep tolerance:' | Select-Object -First 1).Line
        $ok = $rc -eq 0 -and $m.Success -and $m.Groups[2].Value -eq '0' -and "$gate" -match ': 0 pictures beyond'
        $pic = "$pic | $gate | $tol"
    } else {
        $ok = $rc -eq 0 -and $m.Success -and $m.Groups[2].Value -eq '0' -and "$pic" -match ', 0 differ'
    }
    if (-not $ok) { $failed++ }
    Write-Host ("{0,-24} {1,-8} {2,4} s  {3} | {4}" -f $name, $(if ($ok) { 'ok' } else { 'DIVERGED' }), $secs, $lk, $pic)
}
Write-Host "gpu_lockstep_all: $total comparisons; $failed recording(s) failed"
if ($failed) { exit 1 }
exit 0
