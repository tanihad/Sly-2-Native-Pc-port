# The game's GPU renderer path (runtime/gpu/gpu_hw.c, notes/d3d11-renderer.md M6) on every corpus recording
# (tests/gpu/corpus.txt): gs_replay --game - the frontend on its own thread, mixed FF/ROV frames, the PCRTC
# composed on the GPU and presented headless - with every picture read back and compared with the reference
# compose of the GPU's GS memory (must be identical: the GPU compose is exact). -Verify N: --game-verify N
# instead (the frontend on the replay thread next to the software rasterizer, gpu_hw's in-game memory
# verifier every Nth VBlank with the 10.2 tolerances, and every picture compared with the software
# renderer's under the picture gate). One line per recording; exit 1 on any failure.
#   powershell -ExecutionPolicy Bypass -File scripts\gpu_game_all.ps1 [-Only ep8_boss,gpu-C_thiefnet]
#       [-Verify N] [-Adapter hw|warp] [-Out build\game] [-CorpusRoot <dir>] [-Exe <gs_replay.exe>] [-Scale N]
# -Scale N > 1: pictures at scale, box-downsampled and gated (PSNR >= 32 dB, |mean signed error| < 0.5); recordings
# in $Waive keep a lower PSNR floor of their own (M8: fix-perf rv_prompt's full-width UI rules sit a third of a
# field row off the row centres, so supersampling them differs from the point-sampled native picture by design).
param([string[]]$Only = @(), [int]$Verify = 0, [ValidateSet('hw', 'warp')][string]$Adapter = 'hw', [string]$Out = "",
      [string]$CorpusRoot = "", [string]$Exe = "", [int]$Scale = 1, [string[]]$Extra = @(),
      [hashtable]$Waive = @{ 'fix-perf_rv_prompt' = 31.0 })
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
if (-not $Out) { $Out = Join-Path $root 'build\game' }
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$recs = Get-Content (Join-Path $root 'tests\gpu\corpus.txt') | Where-Object { $_ -and -not $_.StartsWith('#') } |
    ForEach-Object { ($_ -split '\|')[0].Trim() }
# (M10) widescreen recordings: "aspect=W:H" in their corpus line (shown at it; anamorphic display targets at scale)
$aspects = @{}
Get-Content (Join-Path $root 'tests\gpu\corpus.txt') | Where-Object { $_ -and -not $_.StartsWith('#') -and $_ -match 'aspect=(\d+:\d+)' } |
    ForEach-Object { $aspects[($_ -split '\|')[0].Trim()] = $Matches[1] }
$replay = Join-Path $Out 'gs_replay.exe'    # (PowerShell names are case-insensitive: not $exe, that is $Exe)
if ($Exe) { Copy-Item $Exe $replay -Force }
else {
    Remove-Item $replay -ErrorAction SilentlyContinue
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'gs_replay.ps1') -Rec (Join-Path $CorpusRoot $recs[0]) `
        -Backend hw -Game -Bench -Out $Out -Log (Join-Path $Out 'build.log') | Out-Null
    if (-not (Test-Path $replay)) { Write-Host 'gs_replay build failed'; exit 1 }
}
$mode = @('--game')
if ($Verify -gt 0) { $mode = @('--game-verify', "$Verify") }
if ($Scale -gt 1) { $mode += @('--scale', "$Scale") }                     # (M7: pictures at scale, box-downsample gate)
$Extra = @($Extra | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if ($Extra.Count) { $mode += $Extra }
$failed = 0
$pics = 0
foreach ($rel in $recs) {
    $name = ($rel -replace '\\build\\gsrec\\', '_' -replace '\\build\\', '_' -replace '\.gsrec$', '')
    if ($Only.Count -and -not ($Only | Where-Object { $name -like "*$_*" })) { continue }
    $rec = Join-Path $CorpusRoot $rel
    if (-not (Test-Path $rec)) { Write-Host "$name : MISSING $rec"; $failed++; continue }
    $o = Join-Path $Out $name
    New-Item -ItemType Directory -Force $o | Out-Null
    $run = Join-Path $o ("gs_replay_{0}.exe" -f $PID)      # (a uniquely named copy)
    Copy-Item $replay $run -Force
    $t0 = Get-Date
    $asp = if ($aspects.ContainsKey($rel)) { @('--aspect', $aspects[$rel]) } else { @() }
    & $run $rec $o 0 --backend hw --adapter $Adapter @mode @asp 2> (Join-Path $o 'log.txt') | Out-Null
    $rc = $LASTEXITCODE
    Remove-Item $run -ErrorAction SilentlyContinue
    $secs = [int]((Get-Date) - $t0).TotalSeconds
    $log = Join-Path $o 'log.txt'
    $g = (Select-String -Path $log -Pattern '^game: \d+ pictures' | Select-Object -First 1).Line
    $s = (Select-String -Path $log -Pattern '^game: vs sw' | Select-Object -First 1).Line
    $v = (Select-String -Path $log -Pattern 'verify v\d+: GPU memory differs beyond' | Measure-Object).Count
    $m = [regex]::Match("$g", 'game: (\d+) pictures checked \((\d+) display off\): (\d+) differ')
    if ($m.Success) { $pics += [int]$m.Groups[1].Value }
    $ok = $rc -eq 0 -and $m.Success -and $m.Groups[3].Value -eq '0' -and $v -eq 0
    if ($Verify -gt 0) { $ok = $ok -and "$s" -match ', 0 beyond the picture gate' }
    $q = (Select-String -Path $log -Pattern '^game: scale' | Select-Object -First 1).Line
    # (M8) box-gate waiver at scale: the recording's own PSNR floor (still a regression gate); everything else must pass
    $waived = ''
    if (-not $ok -and $Scale -gt 1 -and $Waive.ContainsKey($name) -and ($rc -eq 0 -or $rc -eq 3) -and $m.Success -and $v -eq 0 -and
        ($Verify -le 0 -or "$s" -match ', 0 beyond the picture gate')) {
        $wq = [regex]::Match("$q", 'min PSNR ([\d.]+) dB, max \|mean signed error\| ([\d.]+)')
        if ($wq.Success -and [double]$wq.Groups[1].Value -ge [double]$Waive[$name] -and [double]$wq.Groups[2].Value -lt 0.5) {
            $ok = $true; $waived = " | WAIVED (box-gate floor $($Waive[$name]) dB)"
        }
    }
    if (-not $ok) { $failed++ }
    $tail = if ($Verify -gt 0) { " | $s | verify beyond: $v" } else { '' }
    if ($q) { $tail += " | " + ($q -replace '^game: ', '') }
    $tail += $waived
    Write-Host ("{0,-24} {1,-6} {2,4} s  {3}{4}" -f $name, $(if ($ok) { 'ok' } else { 'FAILED' }), $secs, $g, $tail)
}
Write-Host "gpu_game_all: $pics pictures; $failed recording(s) failed"
if ($failed) { exit 1 }
exit 0
