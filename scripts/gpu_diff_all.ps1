# Replay every corpus recording with -Backend both (software renderer vs GPU backend, per VBlank)
# and gate the results against tests/gpu/baseline.json (notes/d3d11-renderer.md 10.1-10.2).
#   powershell -ExecutionPolicy Bypass -File scripts\gpu_diff_all.ps1 [-Mode mixed|ref|fallback] [-Scale N]
#       [-Adapter hw|warp] [-Workers 4] [-Only name,name] [-Update] [-CorpusRoot <dir>] [-AllowMissing] [-Exe <gs_replay.exe>]
# -Mode (M8): mixed (default; the game's mode: FF + ROV tiers in one frame, gs_replay --mixed) / ref (the exact REF
# tier, --ref) / fallback (the M0 all-fallback backend: sw underneath, -Workers pixel workers, S = 1 only).
# -Scale N (mixed / ref): the GPU draws N x N samples per native pixel (M7 sample grid); pictures are composed from
# sample 0, so the S = 1 gates apply at every scale.
# Per recording the gate checks: the number of pictures, the sw picture hash (the software renderer is the oracle and
# must not change unnoticed), PUNT = 0 (mixed / ref), and the picture gate: mixed - no picture beyond 10.2's whole-
# picture gate (PSNR >= 40 dB and <= 0.2% pixels above 16); ref - no differing picture; fallback - differing pixels
# <= max_diff_px and min PSNR >= min_psnr. Results: build\gpu_diff[_<mode>_s<N>]\<name>\metrics.json (+ PNGs of
# differing frames) and summary.json. -Update rewrites frames/sw_hash in the baseline from this run (the tolerances
# stay as they are). Recordings are read in place and never modified.
param([ValidateSet('mixed', 'ref', 'fallback')][string]$Mode = 'mixed', [int]$Scale = 1,
      [ValidateSet('hw', 'warp')][string]$Adapter = 'hw', [int]$Workers = 4, [string[]]$Only = @(), [switch]$Update,
      [string]$CorpusRoot = "", [switch]$AllowMissing, [string]$Exe = "")
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })   # -File passes "a,b" as one string
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
. (Join-Path $PSScriptRoot 'gpu_corpus.ps1'); $CorpusRoot = Resolve-GpuCorpus $CorpusRoot $main
if (-not $CorpusRoot) { exit 0 }
if ($Mode -eq 'fallback' -and $Scale -gt 1) { Write-Host '-Mode fallback draws with the software renderer: S = 1 only'; exit 1 }
$baselinePath = Join-Path $root 'tests\gpu\baseline.json'
$baseline = Get-Content $baselinePath -Raw | ConvertFrom-Json
$outRoot = Join-Path $root $(if ($Mode -eq 'fallback') { 'build\gpu_diff' } else { "build\gpu_diff_${Mode}_s$Scale" })
New-Item -ItemType Directory -Force $outRoot | Out-Null

# ref / mixed: one gs_replay build, run directly (synchronous: the GPU frontend replays on the replay thread)
$replay = $null
if ($Mode -ne 'fallback') {
    $replay = Join-Path $outRoot 'gs_replay.exe'
    if ($Exe) { Copy-Item $Exe $replay -Force }
    else {
        Remove-Item $replay -ErrorAction SilentlyContinue
        $first = Join-Path $CorpusRoot $baseline.recordings[0].path
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'gs_replay.ps1') -Rec $first -Backend hw -Ref -Bench `
            -Out $outRoot -Log (Join-Path $outRoot 'build.log') | Out-Null
        if (-not (Test-Path $replay)) { Write-Host 'gs_replay build failed'; exit 1 }
    }
}

$results = @()
$failed = 0
foreach ($r in $baseline.recordings) {
    if ($Only.Count -and -not ($Only | Where-Object { $r.name -like "*$_*" })) { continue }
    $rec = Join-Path $CorpusRoot $r.path
    $row = [ordered]@{ name = $r.name; status = 'ok'; frames = 0; diff_frames = 0; diff_px = 0; min_psnr = 0; sw_hash = '';
                       punt = 0; beyond = 0; render_ms = 0; gpu_ms = 0; seconds = 0; why = '' }
    if (-not (Test-Path $rec)) {
        $row.status = if ($AllowMissing) { 'missing' } else { 'FAIL' }
        $row.why = "recording not found: $rec"
        if (-not $AllowMissing) { $failed++ }
        $results += [pscustomobject]$row
        Write-Host ("{0,-22} {1}" -f $r.name, $row.why)
        continue
    }
    $out = Join-Path $outRoot ($r.name -replace '[\\/]', '_')
    if (Test-Path $out) { Get-ChildItem $out -Filter 'frame_*.png' | Remove-Item }
    New-Item -ItemType Directory -Force $out | Out-Null
    $metrics = Join-Path $out 'metrics.json'
    if (Test-Path $metrics) { Remove-Item $metrics }
    $t0 = Get-Date
    $logFile = Join-Path $out 'replay.log'
    if ($Mode -eq 'fallback') {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'gs_replay.ps1') -Rec $rec -Out $out `
            -Workers $Workers -Backend both -Adapter $Adapter -Metrics $metrics -Log $logFile | Out-Null
        $code = $LASTEXITCODE
    } else {
        $run = Join-Path $out ("gs_replay_{0}.exe" -f $PID)      # (a uniquely named copy)
        Copy-Item $replay $run -Force
        & $run $rec $out 0 --backend both --adapter $Adapter "--$Mode" --scale $Scale --metrics $metrics 2> $logFile | Out-Null
        $code = $LASTEXITCODE
        Remove-Item $run -ErrorAction SilentlyContinue
    }
    $row.seconds = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    if (-not (Test-Path $metrics)) {
        $row.status = 'FAIL'; $row.why = "replay failed (exit $code), see $logFile"; $failed++
        $results += [pscustomobject]$row
        Write-Host ("{0,-22} {1}" -f $r.name, $row.why)
        continue
    }
    $m = (Get-Content $metrics -Raw | ConvertFrom-Json).summary
    $row.frames = [int]$m.frames; $row.diff_frames = [int]$m.diff_frames; $row.diff_px = [long]$m.diff_px
    $row.min_psnr = [double]$m.min_psnr; $row.sw_hash = $m.sw_hash
    $row.render_ms = [double]$m.render_ms_median; $row.gpu_ms = [double]$m.gpu_ms_median
    if ($null -ne $m.punt) { $row.punt = [long]$m.punt }
    if ($null -ne $m.beyond_gate) { $row.beyond = [int]$m.beyond_gate }
    $why = @()
    if ($Update) {
        $r.frames = $row.frames
        $r.sw_hash = $row.sw_hash
    } else {
        if ($row.frames -ne [int]$r.frames) { $why += "frames $($row.frames) != baseline $($r.frames)" }
        if ($row.sw_hash -ne $r.sw_hash) { $why += "sw hash $($row.sw_hash) != baseline $($r.sw_hash)" }
    }
    switch ($Mode) {
        'fallback' {
            if ($row.diff_px -gt [long]$r.max_diff_px) { $why += "differing pixels $($row.diff_px) > $($r.max_diff_px)" }
            if ($row.min_psnr -lt [double]$r.min_psnr) { $why += "min PSNR $($row.min_psnr) < $($r.min_psnr)" }
            if ($code -ne 0 -and $code -ne 3) { $why += "replay exit code $code" }
        }
        'ref' {
            if ($row.diff_frames -ne 0) { $why += "$($row.diff_frames) pictures differ from sw ($($row.diff_px) px)" }
            if ($code -ne 0) { $why += "replay exit code $code" }
        }
        'mixed' {
            if ($row.beyond -ne 0) { $why += "$($row.beyond) pictures beyond the picture gate" }
            if ($code -ne 0) { $why += "replay exit code $code" }
        }
    }
    if ($Mode -ne 'fallback' -and $row.punt -ne 0) { $why += "PUNT $($row.punt)" }
    if ($why.Count) { $row.status = 'FAIL'; $row.why = $why -join '; '; $failed++ }
    $results += [pscustomobject]$row
    Write-Host ("{0,-22} {1,-4} {2,5} pictures, {3} differ ({4} px), {5} beyond, min PSNR {6}, PUNT {7}, sw {8}, render {9:N3} ms, GPU {10:N3} ms, {11} s {12}" -f `
        $r.name, $row.status, $row.frames, $row.diff_frames, $row.diff_px, $row.beyond, $row.min_psnr, $row.punt, $row.sw_hash,
        $row.render_ms, $row.gpu_ms, $row.seconds, $row.why)
}
$summary = [ordered]@{ mode = $Mode; scale = $Scale; adapter = $Adapter; workers = $Workers; failed = $failed; results = $results }
$summary | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $outRoot 'summary.json')
if ($Update) {
    $baseline | ConvertTo-Json -Depth 4 | Set-Content $baselinePath -Encoding ascii
    Write-Host "baseline updated: $baselinePath"
}
if ($failed) { Write-Host "gpu_diff_all ($Mode, S=$Scale): $failed recording(s) FAILED"; exit 1 }
if (-not $results.Count) { Write-Host "gpu_diff_all: no recording matched -Only"; exit 1 }
Write-Host "gpu_diff_all ($Mode, S=$Scale): all $($results.Count) passed"
exit 0
