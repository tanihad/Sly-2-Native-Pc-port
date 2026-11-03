# (qa-a) One cell of the modern renderer's visual QA matrix: a scene (scripts\qa_scenes.txt) in one settings combo (-Cfg)
# at one frame rate, every VBlank of the window dumped at 2560 x 1440 (raw BMP), then
#   tf    scripts\flicker_check.c over the window (one-picture blinks, blinking lines / blobs / seams: gates.json level3_temporal)
#   mft   grid fallbacks inside the window (PS2_MFT_LOG: "mft: grid frame vN (why)")
#   cmp   every -Sample-th picture against the same VBlank of the reference run (-Cfg ref, same scene and rate; run it first):
#         scripts\qa_cmp.c block-mean differences (clusters of 16 x 16 blocks that differ by > 24)
#   .\scripts\qa_run.ps1 -Scene ep1_hub -Cfg bal [-Rate 60] [-Sample 15] [-Frames 60] [-At N] [-Pad "..."] [-Env "K=V;K=V"] [-Keep]
# Configs (sly2.ini written next to the exe copy, preset = custom so every key is explicit):
#   ref      Reference (exact), 4x integer grid, PS2 outlines, modern filters      perf  preset Performance (Modern 75%)
#   bal      preset Balanced (Modern 100%, MSAA 4x, PS2 outlines)                  qual  preset Quality (100%, 8x, thin)
#   bal_thin Balanced + thin outlines     qual_ps2 Quality + PS2 outlines          bal_faith Balanced + faithful filters
#   bal_nomb Balanced, motion blur off    bal_auto Balanced, render_scale auto     bal_g3 Balanced, fixed 3x (grid frames)
#   perf_thin Performance + thin          bal_msaa0 Balanced, MSAA off             any other: -Ini "<ini text with ; for newlines>"
# Output: build\qa\runs\<scene>__<cfg>__<rate>\ (summary.txt, tf.txt, run excerpt, samples\*.bmp until -Clean).
param([Parameter(Mandatory = $true)][string]$Scene, [string]$Cfg = 'bal', [int]$Rate = 60, [int]$At = 0, [int]$Frames = 0,
      [int]$Sample = 15, [string]$Pad = $null, [string]$Env = '', [string]$Ini = '', [string]$Size = '2560x1440', [switch]$Keep,
      [switch]$Tf, [string]$Exe = '', [string]$Seq = '', [int]$Seconds = 0)
# -Seq "outlines:240,outlines:240,filters:240": live settings toggles (PS2_INPUT tokens, as F3 / F4 / F5 ...: outlines filters blur
#   scaleup scaledown scaleauto flicker interlace, menu mup mdown mok mback ...) <frames> after the previous token (the scene's pad);
#   run.log's "pad frame N (VBlank V): <token>" lines are collected into segments.txt. A static scene shows every setting once.
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {
    $gitdir = ((Get-Content (Join-Path $root '.git') -Raw) -replace '^gitdir:\s*', '').Trim()
    $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
}
$free = [math]::Round((Get-PSDrive C).Free / 1GB, 1)
if ($free -lt 55) { "C: has $free GB free (< 55): not starting"; exit 2 }
$GP = '+2600:lup:90,+90:rright:60,+60:lup:60,+60:cross:6,+30:square:6,+30:lleft:60,+200:lup:120,+120:rleft:80,+100:cross:6,+300:lup:200,+200:circle:6,+300:lup:150,+150:square:6'
$HP = '+500:lup:90,+90:rright:60,+60:lup:60,+60:cross:6,+30:lleft:60,+200:lup:120,+120:rleft:80,+300:lup:200,+200:square:6,+300:lup:150'
$line = Get-Content (Join-Path $PSScriptRoot 'qa_scenes.txt') | Where-Object { $_ -and -not $_.StartsWith('#') } |
    Where-Object { ($_ -split '\|')[0].Trim() -eq $Scene } | Select-Object -First 1
if (-not $line) { "unknown scene $Scene"; exit 2 }
$f = @($line -split '\|' | ForEach-Object { $_.Trim() })
if (-not $At) { $At = [int]$f[4] }
if (-not $Frames) { $Frames = if ($Tf) { [int]$f[5] } else { 300 } }
if ($null -eq $Pad -or $Pad -eq '') { $Pad = ($f[6] -replace '^GP$', $GP -replace '^HP$', $HP) }
if ($Seq) {
    $st = @($Seq -split ',' | Where-Object { $_ } | ForEach-Object { $a, $b = $_ -split ':'; "+${b}:${a}:2" })
    $Pad = (@($Pad, ($st -join ',')) | Where-Object { $_ }) -join ','
}
$cfgs = @{
    ref = @{ mode = 'reference'; scale = '4x'; msaa = 'off'; outl = 'ps2'; filt = 'modern'; mb = 'preset' }
    bal = @{ preset = 'balanced' }; perf = @{ preset = 'performance' }; qual = @{ preset = 'quality' }
    bal_thin = @{ mode = 'modern'; scale = '100%'; msaa = '4x'; outl = 'thin'; filt = 'modern'; mb = 'preset' }
    perf_thin = @{ mode = 'modern'; scale = '75%'; msaa = 'off'; outl = 'thin'; filt = 'modern'; mb = 'preset' }
    qual_ps2 = @{ mode = 'modern'; scale = '100%'; msaa = '8x'; outl = 'ps2'; filt = 'modern'; mb = 'preset' }
    bal_faith = @{ mode = 'modern'; scale = '100%'; msaa = '4x'; outl = 'ps2'; filt = 'faithful'; mb = 'preset' }
    bal_nomb = @{ mode = 'modern'; scale = '100%'; msaa = '4x'; outl = 'ps2'; filt = 'modern'; mb = 'off' }
    bal_auto = @{ mode = 'modern'; scale = 'auto'; msaa = '4x'; outl = 'ps2'; filt = 'modern'; mb = 'preset' }
    bal_g3 = @{ mode = 'modern'; scale = '3x'; msaa = '4x'; outl = 'ps2'; filt = 'modern'; mb = 'preset' }
    bal_msaa0 = @{ mode = 'modern'; scale = '100%'; msaa = 'off'; outl = 'ps2'; filt = 'modern'; mb = 'preset' }
}
if ($Ini) { $iniText = ($Ini -replace ';', "`r`n") }
else {
    $c = $cfgs[$Cfg]
    if (-not $c) { "unknown cfg $Cfg"; exit 2 }
    if ($c.preset) { $iniText = "[graphics]`r`npreset = $($c.preset)`r`n" }
    else { $iniText = "[graphics]`r`npreset = custom`r`nrenderer_mode = $($c.mode)`r`nrender_scale = $($c.scale)`r`nmsaa = $($c.msaa)`r`noutlines = $($c.outl)`r`nfilters = $($c.filt)`r`nmotion_blur = $($c.mb)`r`n" }
}
$name = "qa_${Scene}_${Cfg}_$Rate"
$runDir = Join-Path $root "build\qa\runs\${Scene}__${Cfg}__$Rate"
New-Item -ItemType Directory -Force $runDir | Out-Null
Get-ChildItem $runDir -Recurse -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
$warpDir = Join-Path $root "build\warp\flk_$name"
New-Item -ItemType Directory -Force $warpDir | Out-Null
[IO.File]::WriteAllText((Join-Path $warpDir 'sly2.ini'), $iniText)
$e = @("PS2_R2_STATS=1", "PS2_MFT_LOG=1", "PS2_MFT_LOG_MAX=100000", "PS2_GS_PRESENT_SIZE=$Size", "PS2_FRAME_RATE=$Rate", "PS2_GS_GOVERNOR=0")
if ($Rate -gt 60) { $e += 'PS2_DUMP_WAIT=1' }
if ($Cfg -eq 'ref') { $e += 'PS2_R2=off' }
if ($Env) { $e += @($Env -split ';' | Where-Object { $_ }) }
$ia = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'flicker_run.ps1'), '-Name', $name, '-Episode', $f[1],
        '-At', "$At", '-Frames', "$Frames", '-Scale', 'auto', '-Mode', 'default', '-Keep', '-Env', ($e -join ';'), '-DumpEvery', $(if ($Tf) { '1' } else { "$Sample" }))
if ($f[2]) { $ia += @('-Job', $f[2]) }
if ($f[3] -match 'direct') { $ia += '-Direct' }
if ($f[3] -match 'play') { $ia += '-Play' }
if ($Pad) { $ia += @('-Pad', $Pad) }
# sampled pictures (every -Sample-th VBlank, the game less disturbed than by dumping each one): no temporal check
if (-not $Tf) { $ia += @('-Check', '--max-share 1 --max-run 1 --max-blobs 99999 --max-rows 99999 --seam 99999 --max-seams 99999') }
if ($Exe) { $ia += @('-Exe', $Exe) }
if ($Seconds) { $ia += @('-Seconds', "$Seconds") }   # wall-clock budget (a loaded machine runs 120 fps scenes slower than real time)
$t0 = Get-Date
$tfOut = & powershell @ia
$secs = [int]((Get-Date) - $t0).TotalSeconds
$tfOut | Set-Content (Join-Path $runDir 'tf.txt')
$tfLine = ($tfOut | Select-String '^flicker:' | Select-Object -Last 1)
$fr = Join-Path $warpDir 'frames'
$log = Join-Path $warpDir 'run.log'
$sum = @("scene $Scene cfg $Cfg rate $Rate window $At..$($At + $Frames - 1) ($secs s)", "ini: " + (($iniText -split "`r`n" | Where-Object { $_ -and $_ -notmatch '^\[' }) -join '; '))
$sum += $(if ($tfLine) { $tfLine.Line } else { 'flicker: (no result)' })
$files = @(Get-ChildItem $fr -Filter 'frame_*.bmp' -ErrorAction SilentlyContinue | Sort-Object Name)
$sum += "dumped pictures: $($files.Count) of $Frames"
# the picture sizes: a modern frame is Size, a grid frame (a fallback, one-frame resolution / AA pop) is the grid's size
$sizes = @{}
$vsz = @{}
foreach ($p in $files) {
    $fs = [IO.File]::OpenRead($p.FullName); $hb = New-Object byte[] 30; [void]$fs.Read($hb, 0, 30); $fs.Close()
    $k = "$([BitConverter]::ToInt32($hb, 18))x$([Math]::Abs([BitConverter]::ToInt32($hb, 22)))"
    $sizes[$k] = 1 + [int]$sizes[$k]
    $vsz[[int]($p.BaseName -replace 'frame_', '')] = $k
}
$sum += 'picture sizes: ' + (($sizes.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object { "$($_.Key) x$($_.Value)" }) -join ', ')
$modal = ($sizes.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Key
$odd = @($vsz.GetEnumerator() | Where-Object { $_.Value -ne $modal } | ForEach-Object { $_.Key } | Sort-Object)
if ($odd.Count) { $sum += "pictures not $modal : $($odd.Count) (vblanks " + (($odd | Select-Object -First 24) -join ' ') + ')' }
if (Test-Path $log) {
    $lines = Get-Content $log
    $grid = @($lines | Select-String 'mft: grid frame v(\d+) \((.*?)\)' | ForEach-Object {
        $m = $_.Matches[0]; [pscustomobject]@{ v = [int]$m.Groups[1].Value; why = $m.Groups[2].Value } } | Where-Object { $_.v -ge $At -and $_.v -lt $At + $Frames })
    $sum += "grid fallbacks in the window: $($grid.Count)" + $(if ($grid.Count) { ' [' + (($grid | Group-Object why | ForEach-Object { "$($_.Name) x$($_.Count)" }) -join ', ') + '; first v' + $grid[0].v + ']' } else { '' })
    $mf = $lines | Select-String '\[r2\] mft:|mft: .*modern frames' | Select-Object -Last 1
    if ($mf) { $sum += $mf.Line }
    $err = @($lines | Select-String -Pattern 'not implemented|unhandled|assert|crash|exception|fatal|device lost|error|unknown general register' |
        Where-Object { $_.Line -notmatch 'syscall 0x5B|unimplemented eret' } | Select-Object -First 6 | ForEach-Object { 'log: ' + $_.Line })
    $sum += $err
    $seg = @($lines | Select-String 'pad frame \d+ \(VBlank (\d+)\): (\w+) \(' | ForEach-Object { "$($_.Matches[0].Groups[1].Value) $($_.Matches[0].Groups[2].Value)" })
    $seg | Set-Content (Join-Path $runDir 'segments.txt')
    $lines | Select-String 'settings\]|scale|MSAA|msaa' | Select-Object -First 6 | ForEach-Object { $_.Line } | Set-Content (Join-Path $runDir 'run_excerpt.txt')
    Copy-Item $log (Join-Path $runDir 'run.log')
}
# keep every -Sample-th picture; delete the rest
$sd = Join-Path $runDir 'samples'
New-Item -ItemType Directory -Force $sd | Out-Null
foreach ($p in $files) {
    $v = [int]($p.BaseName -replace 'frame_', '')
    $nearOdd = $odd -contains $v -or $odd -contains ($v + 1) -or $odd -contains ($v - 1)   # a fallback picture and its neighbours
    if ($v % $Sample -eq 0 -or ($nearOdd -and $odd.Count -le 12)) { Move-Item $p.FullName (Join-Path $sd $p.Name) -Force }
}
Remove-Item -Recurse -Force $fr -ErrorAction SilentlyContinue
$cmpTool = Join-Path $root 'build\qa_cmp.exe'
if (-not (Test-Path $cmpTool) -or (Get-Item (Join-Path $PSScriptRoot 'qa_cmp.c')).LastWriteTime -gt (Get-Item $cmpTool).LastWriteTime) {
    & (Join-Path $main 'tools\llvm-mingw\bin\clang.exe') -O2 -o $cmpTool (Join-Path $PSScriptRoot 'qa_cmp.c')
}
$refDir = Join-Path $root "build\qa\runs\${Scene}__ref__$Rate\samples"
if ($Cfg -ne 'ref' -and (Test-Path $refDir)) {
    $cmpAll = @()
    foreach ($p in (Get-ChildItem $sd -Filter 'frame_*.bmp' | Sort-Object Name)) {
        $rp = Join-Path $refDir $p.Name
        if (-not (Test-Path $rp)) { continue }
        $o = & $cmpTool --diff $p.FullName $rp --top 5
        $cmpAll += "== $($p.Name)"
        $cmpAll += $o
    }
    $cmpAll | Set-Content (Join-Path $runDir 'cmp.txt')
    $nb = @($cmpAll | Where-Object { $_ -match '^blocks' } | ForEach-Object { if ($_ -match 'differing > \d+: (\d+)') { [int]$Matches[1] } })
    if ($nb.Count) { $sum += "cmp vs ref: $($nb.Count) pictures, differing blocks per picture: " + ($nb -join ' ') }
} elseif ($Cfg -ne 'ref') { $sum += 'cmp: no reference samples (run -Cfg ref first)' }
$sum | Set-Content (Join-Path $runDir 'summary.txt')
$sum
if (-not $Keep -and $Cfg -ne 'ref') { }   # samples stay for looking; scripts\qa_clean.ps1 removes them
Remove-Item (Join-Path $warpDir 'sly2_warp_*.exe') -ErrorAction SilentlyContinue
