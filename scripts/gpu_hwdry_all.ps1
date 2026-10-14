# The GPU frontend dry run (gs_replay -Backend hwdry, notes/d3d11-renderer.md M1) on every corpus
# recording (tests/gpu/corpus.txt): the software renderer runs alongside, so the VRAM hashes after
# every VBlank must equal tests/gpu/vram_hashes.txt (the frontend changes nothing), and the run must
# have no PUNT. Writes build\m1\hwdry\<name>.json (per-VBlank counters) and table.md (summary).
#   powershell -ExecutionPolicy Bypass -File scripts\gpu_hwdry_all.ps1 [-Only ep2_hub,title] [-FrontOnly]
#       [-StraddleRov] [-Out build\m1\hwdry] [-CorpusRoot <dir>]
# -FrontOnly: without the software rasterizer (frontend timing; VRAM hashes not checked).
param([string[]]$Only = @(), [switch]$FrontOnly, [switch]$StraddleRov, [switch]$SelfCheck, [string]$Out = "", [string]$CorpusRoot = "")
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
if (-not $Out) { $Out = Join-Path $root 'build\m1\hwdry' }
New-Item -ItemType Directory -Force $Out | Out-Null
$expected = @{}
foreach ($l in Get-Content (Join-Path $root 'tests\gpu\vram_hashes.txt')) {
    if ($l -match '^(\S+) \| (\w+) \| (\d+)') { $expected[$matches[1]] = $matches[2] }
}
$recs = Get-Content (Join-Path $root 'tests\gpu\corpus.txt') | Where-Object { $_ -and -not $_.StartsWith('#') } |
    ForEach-Object { ($_ -split '\|')[0].Trim() }
$rows = @()
$failed = 0
foreach ($rel in $recs) {
    $name = ($rel -replace '\\build\\gsrec\\', '_' -replace '\\build\\', '_' -replace '\.gsrec$', '')
    if ($Only.Count -and -not ($Only | Where-Object { $name -like "*$_*" })) { continue }
    $rec = Join-Path $CorpusRoot $rel
    if (-not (Test-Path $rec)) { Write-Host "$name : MISSING $rec"; $failed++; continue }
    $json = Join-Path $Out "$name.json"
    $log = Join-Path $Out "$name.log"
    $hash = Join-Path $Out "$name.vram.txt"
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'gs_replay.ps1'), '-Rec', $rec,
           '-Out', (Join-Path $Out 'bin'), '-Bench', '-Workers', '0', '-Backend', 'hwdry', '-Metrics', $json, '-Log', $log)
    if ($FrontOnly) { $a += '-FrontOnly' } else { $a += @('-VramHash', $hash) }
    if ($StraddleRov) { $a += '-StraddleRov' }
    if ($SelfCheck) { $a += '-SelfCheck' }
    $t0 = Get-Date
    & powershell @a | Out-Null
    $code = $LASTEXITCODE
    $secs = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    $why = @()
    if (-not (Test-Path $json)) { $why += "no metrics (exit $code)" }
    if ($code -eq 5) { $why += 'PUNT' } elseif ($code -ne 0) { $why += "exit $code" }
    if (-not $FrontOnly) {
        $line = (Get-Content $log -ErrorAction SilentlyContinue | Where-Object { $_ -like 'vram-hash*' }) -join ''
        if ($line -notmatch 'vram-hash (\w+)') { $why += 'no vram hash' }
        elseif ($expected[$name] -and $matches[1] -ne $expected[$name]) { $why += "VRAM hash $($matches[1]) != $($expected[$name])" }
    }
    if ($why.Count) { $failed++ }
    if (Test-Path $json) {
        $m = Get-Content $json -Raw | ConvertFrom-Json
        $cpu = 0
        $cl = (Get-Content $log | Where-Object { $_ -like 'replay thread CPU*' }) -join ''
        if ($cl -match '\(([\d.]+) ms per VBlank\)') { $cpu = [double]$matches[1] }
        if ($m.sum.selfcheck_bad -gt 0) { $why += "selfcheck $($m.sum.selfcheck_bad)"; $failed++ }
        $s = $m.sum; $x = $m.max; $n = [math]::Max(1, $m.frames.Count)
        $zr = @()
        $zn = @('Z32', 'Z24', 'Z16', 'Z16S')
        for ($i = 0; $i -lt 4; $i++) {
            if ($s.zprims[$i] -gt 0) {
                $lo = ($m.frames | Where-Object { $_.zprims[$i] -gt 0 } | ForEach-Object { $_.zmin[$i] } | Measure-Object -Minimum).Minimum
                $zr += ('{0} {1:X}..{2:X}' -f $zn[$i], [uint32]$lo, [uint32]$x.zmax[$i])
            }
        }
        $rows += [pscustomobject][ordered]@{
            name = $name; vblanks = $m.frames.Count; punt = $s.tier[3]
            draws_max = $x.d3d_draws; draws_mean = [math]::Round($s.d3d_draws / $n, 1); commute_max = $x.d3d_draws_commute
            ff_max = $x.ff_draws; rov_draws_max = $x.rov_draws; split_max = $x.split_extra; rov_runs_max = $x.rov_runs
            zresolves_max = $x.zresolves; states_max = $x.states; state_changes_max = $x.state_changes
            records_max = ($m.frames | ForEach-Object { $_.records_z + $_.records_full + $_.records_sprite + $_.records_linepix } | Measure-Object -Maximum).Maximum
            vertices_max = $x.vertices; kicks_max = $x.kicks
            alias_sprites_max = $x.alias16_sprites; alias_disp_max = $x.alias16_dispatches
            rov_prims = $s.tier[1]; ff_prims = $s.tier[0]
            straddle_pct = $(if ($s.atst_kicks) { [math]::Round(100.0 * $s.atst_straddle / $s.atst_kicks, 2) } else { 0 })
            as128_pct = $(if ($s.cd_as_kicks) { [math]::Round(100.0 * $s.as_over128 / $s.cd_as_kicks, 2) } else { 0 })
            z = ($zr -join ', '); z32_over = $s.z32_over
            clears_z = $s.clears_z; clears_c = $s.clears_c; loads = $s.loads; load_blocks = $s.load_blocks
            downloads = $s.downloads; shadow = $s.shadow_copies; selfcopy = $s.alias16_selfcopy
            xfers = $s.xfers; patches_max = $x.patches; clut_new = $s.clut_new; region_clamp = $s.region_clamp; tbw_npot = $s.tbw_npot
            views_c32 = $s.tex_view_c32; views_alpha = $s.tex_view_alpha; targets_max = $x.targets_live
            rov_why = ($s.rov_why -join '/'); front_ms_mean = [math]::Round($m.front_ms_mean, 2); front_ms_max = [math]::Round($m.front_ms_max, 2); cpu_ms = $cpu; clut_gpu = $s.clut_gpu_reads; overlaps = $s.target_overlaps
            status = $(if ($why.Count) { $why -join '; ' } else { 'ok' }); seconds = $secs
        }
    }
    $r = $rows[-1]
    Write-Host ("{0,-24} {1,-6} draws max {2,5} (commute {3,5}) ROV runs {4,3} states {5,4} records {6,6} alias16 {7}/{8} punt {9} front {10} ms {11} s" -f `
        $name, $r.status, $r.draws_max, $r.commute_max, $r.rov_runs_max, $r.states_max, $r.records_max, $r.alias_sprites_max,
        $r.alias_disp_max, $r.punt, $r.front_ms_mean, $secs)
}
$rows | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $Out 'summary.json')
$md = @('| recording | VBlanks | PUNT | D3D draws max / mean | commute max | ROV draws max | ROV runs max | zresolves max | states max | state changes max | records max | ALIAS16 sprites / dispatches max | pass-A straddle % | As>128 % | Z range | front ms mean / max |',
        '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
foreach ($r in $rows) {
    $md += "| $($r.name) | $($r.vblanks) | $($r.punt) | $($r.draws_max) / $($r.draws_mean) | $($r.commute_max) | $($r.rov_draws_max) | $($r.rov_runs_max) | $($r.zresolves_max) | $($r.states_max) | $($r.state_changes_max) | $($r.records_max) | $($r.alias_sprites_max) / $($r.alias_disp_max) | $($r.straddle_pct) | $($r.as128_pct) | $($r.z) | $($r.front_ms_mean) / $($r.front_ms_max) |"
}
$md += ''
$md += '| recording | clears Z / colour | loads (blocks) | downloads | shadow copies | ALIAS16 self-copies | uploads -> max patches/VBlank | new CLUT slots | CLUT loads over GPU words | REGION_CLAMP prims | TBW not 2^n prims | CT32 / alpha views | max targets | ROV why (ref/colclamp/fbmsk/date/blend/C>128/pabe-dthe/z32/straddle) |'
$md += '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|'
foreach ($r in $rows) {
    $md += "| $($r.name) | $($r.clears_z) / $($r.clears_c) | $($r.loads) ($($r.load_blocks)) | $($r.downloads) | $($r.shadow) | $($r.selfcopy) | $($r.xfers) -> $($r.patches_max) | $($r.clut_new) | $($r.clut_gpu) | $($r.region_clamp) | $($r.tbw_npot) | $($r.views_c32) / $($r.views_alpha) | $($r.targets_max) | $($r.rov_why) |"
}
Set-Content (Join-Path $Out 'table.md') $md
if ($failed) { Write-Host "gpu_hwdry_all: $failed problem(s)"; exit 1 }
Write-Host "gpu_hwdry_all: $($rows.Count) recording(s) ok (0 PUNT, VRAM unchanged)"
exit 0
