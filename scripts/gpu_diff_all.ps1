# Replay every corpus recording with -Backend both (software renderer vs GPU backend, per VBlank)
# and gate the results against tests/gpu/baseline.json (notes/d3d11-renderer.md 10.1-10.2).
#   powershell -ExecutionPolicy Bypass -File scripts\gpu_diff_all.ps1 [-Adapter hw|warp] [-Workers 4]
#       [-Only name,name] [-Update] [-CorpusRoot <dir>] [-AllowMissing]
# Per recording the gate checks: the number of pictures, the sw picture hash (the software renderer
# is the oracle and must not change unnoticed), differing pixels <= max_diff_px and the minimum
# PSNR >= min_psnr. Results: build\gpu_diff\<name>\metrics.json (+ PNGs of differing frames) and
# build\gpu_diff\summary.json. -Update rewrites frames/sw_hash in the baseline from this run (the
# tolerances stay as they are). Recordings are read in place and never modified.
param([ValidateSet('hw', 'warp')][string]$Adapter = 'hw', [int]$Workers = 4, [string[]]$Only = @(), [switch]$Update,
      [string]$CorpusRoot = "", [switch]$AllowMissing)
$ErrorActionPreference = 'Stop'
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
$baselinePath = Join-Path $root 'tests\gpu\baseline.json'
$baseline = Get-Content $baselinePath -Raw | ConvertFrom-Json
$outRoot = Join-Path $root 'build\gpu_diff'
New-Item -ItemType Directory -Force $outRoot | Out-Null

$results = @()
$failed = 0
foreach ($r in $baseline.recordings) {
    if ($Only.Count -and -not ($Only -contains $r.name)) { continue }
    $rec = Join-Path $CorpusRoot $r.path
    $row = [ordered]@{ name = $r.name; status = 'ok'; frames = 0; diff_frames = 0; diff_px = 0; min_psnr = 0; sw_hash = '';
                       render_ms = 0; gpu_ms = 0; seconds = 0; why = '' }
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
    $log = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'gs_replay.ps1') -Rec $rec -Out $out `
        -Workers $Workers -Backend both -Adapter $Adapter -Metrics $metrics 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $log | Set-Content (Join-Path $out 'replay.log')
    $row.seconds = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    if (-not (Test-Path $metrics)) {
        $row.status = 'FAIL'; $row.why = "replay failed (exit $code), see $out\replay.log"; $failed++
        $results += [pscustomobject]$row
        Write-Host ("{0,-22} {1}" -f $r.name, $row.why)
        continue
    }
    $m = (Get-Content $metrics -Raw | ConvertFrom-Json).summary
    $row.frames = [int]$m.frames; $row.diff_frames = [int]$m.diff_frames; $row.diff_px = [long]$m.diff_px
    $row.min_psnr = [double]$m.min_psnr; $row.sw_hash = $m.sw_hash
    $row.render_ms = [double]$m.render_ms_median; $row.gpu_ms = [double]$m.gpu_ms_median
    $why = @()
    if ($Update) {
        $r.frames = $row.frames
        $r.sw_hash = $row.sw_hash
    } else {
        if ($row.frames -ne [int]$r.frames) { $why += "frames $($row.frames) != baseline $($r.frames)" }
        if ($row.sw_hash -ne $r.sw_hash) { $why += "sw hash $($row.sw_hash) != baseline $($r.sw_hash)" }
    }
    if ($row.diff_px -gt [long]$r.max_diff_px) { $why += "differing pixels $($row.diff_px) > $($r.max_diff_px)" }
    if ($row.min_psnr -lt [double]$r.min_psnr) { $why += "min PSNR $($row.min_psnr) < $($r.min_psnr)" }
    if ($code -ne 0 -and $code -ne 3) { $why += "replay exit code $code" }
    if ($why.Count) { $row.status = 'FAIL'; $row.why = $why -join '; '; $failed++ }
    $results += [pscustomobject]$row
    Write-Host ("{0,-22} {1,-4} {2,5} pictures, {3} differ ({4} px), min PSNR {5}, sw {6}, render {7:N3} ms, GPU {8:N3} ms, {9} s {10}" -f `
        $r.name, $row.status, $row.frames, $row.diff_frames, $row.diff_px, $row.min_psnr, $row.sw_hash, $row.render_ms,
        $row.gpu_ms, $row.seconds, $row.why)
}
$summary = [ordered]@{ adapter = $Adapter; workers = $Workers; failed = $failed; results = $results }
$summary | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $outRoot 'summary.json')
if ($Update) {
    $baseline | ConvertTo-Json -Depth 4 | Set-Content $baselinePath -Encoding ascii
    Write-Host "baseline updated: $baselinePath"
}
if ($failed) { Write-Host "gpu_diff_all: $failed recording(s) FAILED"; exit 1 }
Write-Host "gpu_diff_all: all passed"
exit 0
