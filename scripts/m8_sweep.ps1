# One scene of the GPU renderer's in-game sweep (notes/d3d11-renderer.md 10.5 / M8): starts the release build at an
# episode or job through scripts\warp_run.ps1 with the GPU renderer at internal scale -Scale (default 3, Modern
# preset), the in-game verifier (PS2_GS_VERIFY=-Verify: GPU memory vs the software renderer every N VBlanks; 0 = off,
# then the frontend runs on its own thread and the timings are the game's), PS2_GS_HW_STATS=1, and frame dumps every
# -DumpEvery VBlanks; with the verifier on, the comparisons at those VBlanks also write the software renderer's and
# the GPU memory's reference pictures (pair_<v>_sw/_hw.png, native) next to the GPU's own picture at scale
# (frame_<v>.png). Then summarizes run.log into <dir>\summary.txt (+ stats.csv: one row per PS2_GS_HW_STATS line).
#   powershell -ExecutionPolicy Bypass -File scripts\m8_sweep.ps1 -Name ep3_hub -Episode 3 -Play [-Seconds 150]
#   ... -Name m4_hack -Episode 4 -Job m4_hack -Direct [-Pad "+2500:lup:60"] [-Verify 0] [-Sw] [-Scale 1]
# -Sw: the software renderer instead (comparison runs). -Live: steer through <dir>\input.txt (PS2_INPUT_FILE).
# Run directory: build\warp\m8_<Name> (exe copy sly2_warp_m8_<Name>.exe). Delete the frames after looking at them.
param([Parameter(Mandatory = $true)][string]$Name, [Parameter(Mandatory = $true)][int]$Episode, [string]$Job = "",
      [switch]$Direct, [switch]$Play, [int]$Seconds = 150, [string]$Pad = "", [int]$Verify = 60, [int]$DumpEvery = 600,
      [int]$Scale = 3, [switch]$Sw, [string]$Set = "", [string]$Make = "", [int]$StatsSkip = 5, [switch]$Window,
      [string]$Extra = "", [switch]$Live, [int]$StatsFrom = 0)
$root = Split-Path $PSScriptRoot -Parent
$runName = "m8_$Name"
$dir = Join-Path $root "build\warp\$runName"
New-Item -ItemType Directory -Force $dir | Out-Null
if ($Live) { Set-Content (Join-Path $dir 'input.txt') '' -Encoding ascii; $env:PS2_INPUT_FILE = Join-Path $dir 'input.txt' }
else { Remove-Item Env:\PS2_INPUT_FILE -ErrorAction SilentlyContinue }
$env:PS2_GS_BACKEND = $(if ($Sw) { 'sw' } else { 'hw' })
$env:PS2_GS_SCALE = "$Scale"
$env:PS2_GS_HW_STATS = '1'
$env:PS2_DUMP_RAW = '1'              # (frame dumps unshaped: the picture as composed, so it lines up with the pairs)
if ($Verify -gt 0 -and -not $Sw) { $env:PS2_GS_VERIFY = "$Verify"; $env:PS2_GS_VERIFY_DUMP = "$DumpEvery" }
else { Remove-Item Env:\PS2_GS_VERIFY, Env:\PS2_GS_VERIFY_DUMP -ErrorAction SilentlyContinue }
# more environment (e.g. "PS2_GS_SAME_THREAD=1;PS2_PERF=1")
foreach ($kv in ($Extra -split ';' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; Set-Item "Env:\$k" $v }
$wa = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'warp_run.ps1'), '-Episode', "$Episode", '-Release',
        '-Seconds', "$Seconds", '-DumpEvery', "$DumpEvery", '-Name', $runName)
if ($Job) { $wa += @('-Job', $Job) }
if ($Direct) { $wa += '-Direct' }
if ($Play) { $wa += '-Play' }
if ($Pad) { $wa += @('-Pad', $Pad) }
if ($Set) { $wa += @('-Set', $Set) }
if ($Make) { $wa += @('-Make', $Make) }
if ($Window) { $wa += '-Window' }
$t0 = Get-Date
& powershell @wa | Out-Null
Remove-Item Env:\PS2_GS_BACKEND, Env:\PS2_GS_SCALE, Env:\PS2_GS_HW_STATS, Env:\PS2_GS_VERIFY, Env:\PS2_GS_VERIFY_DUMP, Env:\PS2_INPUT_FILE, Env:\PS2_DUMP_RAW -ErrorAction SilentlyContinue
foreach ($kv in ($Extra -split ';' | Where-Object { $_ })) { $k = ($kv -split '=', 2)[0]; Remove-Item "Env:\$k" -ErrorAction SilentlyContinue }
$log = Join-Path $dir 'run.log'
if (-not (Test-Path $log)) { "no run.log in $dir"; exit 1 }
$lines = Get-Content $log

function Med([double[]]$a) { if (-not $a.Count) { return 0 }; $s = $a | Sort-Object; return $s[[int][math]::Floor(($s.Count - 1) / 2)] }
$rx = 'stats v(\d+): ([\d.]+) flips/s \(([\d.]+) pictures/s, (\d+) dropped\) \| per game frame \((\d+), median / max ms\): GS thread ([\d.]+) \(mean\) \| frontend (?:thread|\(GS thread\)) ([\d.]+) / ([\d.]+) \| render CPU ([\d.]+) / ([\d.]+), wall ([\d.]+) / ([\d.]+), present ([\d.]+) \| GPU ([\d.]+) / ([\d.]+) \| latency ([\d.]+) / ([\d.]+) \| per VBlank: (\d+) prims, (\d+) draws \(FF (\d+), ROV (\d+) in ([\d.]+) runs\), PUNT (\d+), downloads ([\d.]+), loads ([\d.]+)'
$rows = @()
foreach ($l in $lines) {
    $m = [regex]::Match($l, $rx)
    if (-not $m.Success) { continue }
    $g = $m.Groups
    $rows += [pscustomobject]@{ vblank = [int]$g[1].Value; flips = [double]$g[2].Value; pics = [double]$g[3].Value; dropped = [int]$g[4].Value;
        frames = [int]$g[5].Value; gs = [double]$g[6].Value; front = [double]$g[7].Value; front_max = [double]$g[8].Value;
        render = [double]$g[9].Value; render_max = [double]$g[10].Value; gpu = [double]$g[14].Value; gpu_max = [double]$g[15].Value;
        latency = [double]$g[16].Value; prims = [int]$g[18].Value; draws = [int]$g[19].Value; punt = [long]$g[23].Value;
        downloads = [double]$g[24].Value; loads = [double]$g[25].Value }
}
$rows | Export-Csv (Join-Path $dir 'stats.csv') -NoTypeInformation
# (M8) per-second totals at the end of the stats line: target rescales, TRXDIR=1 reads
$resc = 0; $reads = 0
foreach ($l in $lines) {
    $m = [regex]::Match($l, 'this second: target rescales (\d+), TRXDIR=1 reads (\d+)')
    if ($m.Success) { $resc += [int]$m.Groups[1].Value; $reads += [int]$m.Groups[2].Value }
}
$use = @($rows | Select-Object -Skip $StatsSkip | Where-Object { $_.vblank -ge $StatsFrom -and $_.frames -gt 0 })
$verifyBad = @($lines | Select-String 'GPU memory differs beyond').Count
$verifyLast = ($lines | Select-String 'verify v\d+: \d+ comparisons' | Select-Object -Last 1).Line
$punts = @($lines | Select-String 'PUNT on the frontend thread').Count
$puntMax = if ($rows.Count) { ($rows | Measure-Object punt -Maximum).Maximum } else { 0 }
$errs = @($lines | Select-String -Pattern 'not implemented|unhandled|assert|crash|exception|fatal|device lost|failed|error' |
          Where-Object { $_.Line -notmatch 'syscall 0x5B|unimplemented eret' } | Select-Object -First 12 | ForEach-Object { $_.Line })
$frames = Join-Path $dir 'frames'
$nf = @(Get-ChildItem $frames -Filter 'frame_*.png' -ErrorAction SilentlyContinue).Count
$np = @(Get-ChildItem $frames -Filter 'pair_*_hw.png' -ErrorAction SilentlyContinue).Count
$sum = @(
    "scene ${Name}: episode $Episode job '$Job' direct $Direct play $Play; renderer $(if ($Sw) { 'sw' } else { 'hw' }) scale $Scale verify $Verify; $([int]((Get-Date) - $t0).TotalSeconds) s",
    "frames: $nf dumps, $np verify pairs ($frames)",
    "verify: $verifyLast; beyond-tolerance lines $verifyBad",
    "PUNT: thread-PUNT events $punts, PUNT count (last stats line, per second) max $puntMax; target rescales $resc, TRXDIR=1 reads $reads (whole run)",
    ("stats ({0} lines, {1} used): flips/s {2:N1}, GS {3:N2}, frontend {4:N2} (max {5:N2}), render CPU {6:N2}, GPU {7:N2} (max {8:N2}), latency {9:N2}, prims/VBlank {10:N0}, draws {11:N0}, downloads {12:N1}, dropped {13}" -f `
        $rows.Count, $use.Count, (Med ($use | ForEach-Object flips)), (Med ($use | ForEach-Object gs)), (Med ($use | ForEach-Object front)),
        (Med ($use | ForEach-Object front_max)), (Med ($use | ForEach-Object render)), (Med ($use | ForEach-Object gpu)),
        (Med ($use | ForEach-Object gpu_max)), (Med ($use | ForEach-Object latency)), (Med ($use | ForEach-Object prims)),
        (Med ($use | ForEach-Object draws)), (Med ($use | ForEach-Object downloads)), (($use | Measure-Object dropped -Sum).Sum))
)
if ($np -gt 0) {                     # picture checks (scripts/m8_check.py): native hw vs sw, picture at scale vs native
    $gitdir = ((Get-Content (Join-Path $root '.git') -Raw) -replace '^gitdir:\s*', '').Trim()
    $py = Join-Path (Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent) '.venv\Scripts\python.exe'
    if (-not (Test-Path $py)) { $py = Join-Path $root '.venv\Scripts\python.exe' }
    $chk = & $py (Join-Path $PSScriptRoot 'm8_check.py') $frames
    $chk | Set-Content (Join-Path $dir 'check.txt')
    $sum += @($chk | Select-Object -Last 1) + @($chk | Where-Object { $_ -cmatch 'FLAG$' } | Select-Object -First 5)
}
$sum += @($errs | ForEach-Object { "log: $_" })
$sum | Set-Content (Join-Path $dir 'summary.txt')
$sum
