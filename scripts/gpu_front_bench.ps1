# GS-thread CPU cost of the GPU frontend (notes/d3d11-renderer.md M1 -> M2): replays recordings
# synchronously in three modes and reports the replay thread's CPU time (QueryThreadCycleTime, so a
# loaded machine inflates it less than wall time), median of -Runs runs:
#   decode   register decode + vertex queue only (what every backend pays)
#   front    decode + the frontend (gs_replay -Backend hwdry -FrontOnly)
#   sw       decode + the software rasterizer (synchronous, no pixel workers), for scale
# Per game frame = total / VBlanks with >= 1000 primitives.
#   powershell -ExecutionPolicy Bypass -File scripts\gpu_front_bench.ps1 [-Only ep8_boss,ep3_hub] [-Runs 3] [-NoSw]
param([string[]]$Only = @('ep8_boss', 'ep3_hub', 'ep5_hub_street', 'gpu-C_ep2_hub', 'gpu-C_thiefnet'), [int]$Runs = 3,
      [switch]$NoSw, [string]$CorpusRoot = "")
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
$out = Join-Path $root 'build\m1\bench'
New-Item -ItemType Directory -Force $out | Out-Null
$recs = Get-Content (Join-Path $root 'tests\gpu\corpus.txt') | Where-Object { $_ -and -not $_.StartsWith('#') } |
    ForEach-Object { ($_ -split '\|')[0].Trim() }
function Median($v) { $s = @($v | Sort-Object); if (-not $s.Count) { return 0 }; return $s[[int][math]::Floor(($s.Count - 1) / 2)] }
$rows = @()
foreach ($rel in $recs) {
    $name = ($rel -replace '\\build\\gsrec\\', '_' -replace '\\build\\', '_' -replace '\.gsrec$', '')
    if (-not ($Only | Where-Object { $name -eq $_ })) { continue }
    $rec = Join-Path $CorpusRoot $rel
    $modes = [ordered]@{ decode = @('-DecodeOnly'); front = @('-Backend', 'hwdry', '-FrontOnly') }
    if (-not $NoSw) { $modes['sw'] = @('-Backend', 'sw') }
    $res = @{}
    $json = Join-Path $out "$name.json"
    foreach ($k in $modes.Keys) {
        $v = @()
        for ($i = 0; $i -lt $Runs; $i++) {
            $log = Join-Path $out "$name.$k.log"
            $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'gs_replay.ps1'), '-Rec', $rec,
                   '-Out', (Join-Path $out 'bin'), '-Bench', '-Workers', '0', '-Log', $log) + $modes[$k]
            if ($k -eq 'front') { $a += @('-Metrics', $json) }
            & powershell @a | Out-Null
            $cl = (Get-Content $log | Where-Object { $_ -like 'replay thread CPU*' }) -join ''
            if ($cl -match 'CPU ([\d.]+) ms') { $v += [double]$matches[1] }
        }
        $res[$k] = Median $v
    }
    $m = Get-Content $json -Raw | ConvertFrom-Json
    $vbl = $m.frames.Count
    $game = @($m.frames | Where-Object { $_.kicks -ge 1000 }).Count
    if (-not $game) { $game = $vbl }
    $row = [ordered]@{ name = $name; vblanks = $vbl; game_frames = $game
        decode_ms_per_game_frame = [math]::Round($res['decode'] / $game, 2)
        front_ms_per_game_frame = [math]::Round(($res['front'] - $res['decode']) / $game, 2)
        decode_plus_front = [math]::Round($res['front'] / $game, 2) }
    if (-not $NoSw) { $row['sw_sync_ms_per_game_frame'] = [math]::Round(($res['sw']) / $game, 2) }
    $rows += [pscustomobject]$row
    Write-Host ($rows[-1] | Format-List | Out-String).Trim()
    Write-Host ''
}
$rows | ConvertTo-Json | Set-Content (Join-Path $out 'bench.json')
