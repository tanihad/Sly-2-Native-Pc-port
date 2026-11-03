# (qa-b) One QA run: a scene of scripts\qa_scenes.txt in one settings configuration, every picture of a window of VBlanks
# dumped, the temporal check (scripts\flicker_check.c, tests\r2\gates.json level3_temporal) and, with -Shots, the
# pictures to look at. Settings go through a sly2.ini next to the exe copy (the way a player's file would) plus
# PS2_* switches, so the run goes through the same gfx.c / settings.c paths as the game (presets, MFT, outlines, ...).
#   .\scripts\qa_scene.ps1 -Scene m5_boss -Config balanced [-Fps 60] [-At 5200] [-Frames 90] [-Size 2560x1440]
#                          [-Extra "K=V;K=V"] [-Ini "graphics.outlines=thin;graphics.motion_blur=off"] [-Keep]
#                          [-ShotAt 30,60] [-Tag x]
# Configs (-Config): exact (Reference, renderer_mode reference, integer grid at 100%), performance, balanced, quality
#   (the presets, renderer_mode modern: MFT at the render scale + MSAA), grid3 (modern, 3x grid: PS2_GS_SCALE=3,
#   the pre-R8 path), custom (only -Ini). -Ini adds / overrides "section.key=value" lines (after the preset: the file
#   says preset = custom then). -Mft0: PS2_MFT=0.
# Output: build\qa\<scene>__<config>[__tag]\: run.log, check.txt (the tf verdict), worst.png, shot_<n>.png (crops of
#   the dumped pictures, 960 wide), mft.txt (PS2_MFT_LOG lines: why frames stayed on the grid). Frames are deleted
#   unless -Keep (they are 6-11 MB each).
param([Parameter(Mandatory = $true)][string]$Scene, [string]$Config = 'balanced', [int]$Fps = 60, [int]$At = 0,
      [int]$Frames = 90, [string]$Size = '2560x1440', [string]$Extra = '', [string]$Ini = '', [string]$Tag = '',
      [switch]$Keep, [switch]$Mft0, [switch]$NoDump, [switch]$Gov, [int[]]$ShotAt = @(20, 45), [string]$Exe = '', [string]$Check = '')
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {
    $gitdir = ((Get-Content (Join-Path $root '.git') -Raw) -replace '^gitdir:\s*', '').Trim()
    $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
}
$py = Join-Path $main '.venv\Scripts\python.exe'
$free = [math]::Round((Get-PSDrive C).Free / 1GB, 1)
if ($free -lt 50) { "C: has $free GB free (< 50): not starting"; exit 2 }
# one game at a time on this machine (shared with other agents: at most one of MY instances)
$lock = Join-Path $root 'build\qa\.lock'
New-Item -ItemType Directory -Force (Join-Path $root 'build\qa') | Out-Null

$GP = '+2600:lup:90,+90:rright:60,+60:lup:60,+60:cross:6,+30:square:6,+30:lleft:60,+200:lup:120,+120:rleft:80,+100:cross:6,+300:lup:200,+200:circle:6,+300:lup:150,+150:square:6'
$HP = '+500:lup:90,+90:rright:60,+60:lup:60,+60:cross:6,+30:lleft:60,+200:lup:120,+120:rleft:80,+300:lup:200,+200:square:6,+300:lup:150'
# scene: name | episode | job | flags (direct play set= make= env=) | At | pad | note
$line = Get-Content (Join-Path $PSScriptRoot 'qa_scenes.txt') | Where-Object { $_ -and -not $_.StartsWith('#') } |
    Where-Object { ($_ -split '\|')[0].Trim() -eq $Scene } | Select-Object -First 1
if (-not $line) { "unknown scene $Scene"; exit 2 }
$f = @($line -split '\|' | ForEach-Object { $_.Trim() })
$episode = [int]$f[1]; $job = $f[2]; $flags = $f[3]; $pad = $f[5] -replace '^GP$', $GP -replace '^HP$', $HP
if ($At -le 0) { $At = [int]$f[4] }
$name = "$Scene`__$Config$(if ($Tag) { "__$Tag" })" -replace '[^A-Za-z0-9_]', '_'
$out = Join-Path $root "build\qa\$name"
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null

# the ini
$kv = [ordered]@{}
$modern = $true
switch ($Config) {
    'exact'       { $kv['graphics.preset'] = 'custom'; $kv['graphics.renderer_mode'] = 'reference'; $kv['graphics.render_scale'] = '100%'; $kv['graphics.outlines'] = 'ps2'; $modern = $false }
    'performance' { $kv['graphics.preset'] = 'performance' }
    'balanced'    { $kv['graphics.preset'] = 'balanced' }
    'quality'     { $kv['graphics.preset'] = 'quality' }
    'grid3'       { $kv['graphics.preset'] = 'custom'; $kv['graphics.renderer_mode'] = 'modern'; $kv['graphics.render_scale'] = '3x' }
    'custom'      { $kv['graphics.preset'] = 'custom'; $kv['graphics.renderer_mode'] = 'modern'; $kv['graphics.render_scale'] = '100%'; $kv['graphics.msaa'] = '4x' }
    default       { "unknown config $Config"; exit 2 }
}
foreach ($p in ($Ini -split ';' | Where-Object { $_ })) { $k, $v = $p -split '=', 2; $kv[$k.Trim()] = $v.Trim(); if ($k.Trim() -ne 'graphics.preset') { $kv['graphics.preset'] = 'custom' } }
$kv['display.frame_rate'] = "$Fps"
$kv['display.aspect'] = '16:9'
$sec = [ordered]@{}
foreach ($k in $kv.Keys) { $s, $n = $k -split '\.', 2; if (-not $sec.Contains($s)) { $sec[$s] = @() }; $sec[$s] += "$n = $($kv[$k])" }
$ini = ''
foreach ($s in $sec.Keys) { $ini += "[$s]`r`n" + (($sec[$s]) -join "`r`n") + "`r`n" }
$runName = "qa_$name"
$w = Join-Path $root "build\warp\$runName"
New-Item -ItemType Directory -Force $w | Out-Null
[IO.File]::WriteAllText((Join-Path $w 'sly2.ini'), $ini)
Copy-Item (Join-Path $w 'sly2.ini') (Join-Path $out 'sly2.ini')

# environment (everything this run sets is restored)
$set = [ordered]@{ PS2_DUMP_FROM = "$At"; PS2_DUMP_TO = "$($At + $Frames - 1)"; PS2_DUMP_WAIT = $(if ($Fps -gt 60) { '1' } else { '0' });
    PS2_DUMP_BMP = '1'; PS2_DUMP_RAW = '1'; PS2_GS_PRESENT_SIZE = $Size; PS2_MFT_LOG = '2000'; PS2_R2_STATS = '1';
    PS2_GS_GOVERNOR = $(if ($Gov) { $null } else { '0' }); PS2_R2 = $null; PS2_GS_SCALE = $null; PS2_MFT = $null }
if ($Mft0) { $set.PS2_MFT = '0' }
if ($NoDump) { $set.PS2_DUMP_FROM = '99999'; $set.PS2_DUMP_TO = '99999' }
foreach ($e in ($f[3] -split ' ' | Where-Object { $_ -like 'env=*' })) { $a, $b = $e.Substring(4) -split '=', 2; $set[$a] = $b }
foreach ($e in ($Extra -split ';' | Where-Object { $_ })) { $a, $b = $e -split '=', 2; $set[$a.Trim()] = $b }
$saved = @{}
foreach ($k in $set.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $set[$k]) }
if ($Exe) { $env:M9_GAME_EXE = $Exe }
$secs = [int](($At + $Frames) / 60) + 25 + [int]($Frames / $(if ($Fps -gt 60) { 4 } else { 8 }))
$wa = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'warp_run.ps1'), '-Episode', "$episode", '-Release',
        '-Seconds', "$secs", '-DumpEvery', '1', '-Name', $runName)
if ($job) { $wa += @('-Job', $job) }
if ($flags -match '\bdirect\b') { $wa += '-Direct' }
if ($flags -match '\bplay\b') { $wa += '-Play' }
foreach ($fl in ($flags -split ' ' | Where-Object { $_ })) {
    if ($fl -like 'set=*') { $wa += @('-Set', $fl.Substring(4)) }
    elseif ($fl -like 'make=*') { $wa += @('-Make', ($fl.Substring(5) -replace '~', ' ')) }
}
if ($pad) { $wa += @('-Pad', $pad) }
$t0 = Get-Date
& powershell @wa | Out-Null
foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
Remove-Item Env:\M9_GAME_EXE -ErrorAction SilentlyContinue
$mins = [math]::Round(((Get-Date) - $t0).TotalMinutes, 1)
Remove-Item (Join-Path $w 'sly2_warp_*.exe') -ErrorAction SilentlyContinue
$log = Join-Path $w 'run.log'
Copy-Item $log (Join-Path $out 'run.log') -ErrorAction SilentlyContinue
if (Test-Path $log) {
    Select-String -Path $log -Pattern '\[mft\]|mft:|not modern|grid frame|one-frame|device lost|error|assert|fatal|unhandled|unknown general|not implemented' |
        Where-Object { $_.Line -notmatch 'syscall 0x5B' } | Select-Object -First 400 | ForEach-Object { $_.Line } | Set-Content (Join-Path $out 'mft.txt')
}
$fr = Join-Path $w 'frames'
Remove-Item (Join-Path $fr 'latest.png') -ErrorAction SilentlyContinue
$tool = Join-Path $root 'build\flicker_check.exe'
$src = Join-Path $PSScriptRoot 'flicker_check.c'
if (-not (Test-Path $tool) -or (Get-Item $src).LastWriteTime -gt (Get-Item $tool).LastWriteTime) {
    & (Join-Path $main 'tools\llvm-mingw\bin\clang.exe') -O2 -o $tool $src
}
$n = @(Get-ChildItem $fr -Filter 'frame_*.bmp' -ErrorAction SilentlyContinue).Count
if (-not $Check) {
    $g = (Get-Content (Join-Path $root 'tests\r2\gates.json') -Raw | ConvertFrom-Json).level3_temporal
    $Check = "--max-share $($g.spike_share_max) --max-run $($g.longest_run_max) --max-rows $($g.rows_max) --max-blobs $($g.blobs_max) --seam $($g.seam_min_px) --max-seams $($g.seams_max)"
}
$ca = @($fr, '--out', (Join-Path $out 'worst.png'), '--heat', (Join-Path $out 'heat.png')) + @($Check -split ' ' | Where-Object { $_ })
# picture sizes: a different size = a grid frame between modern ones (dump runs only)
$sizes = (Get-ChildItem $fr -Filter 'frame_*.bmp' -ErrorAction SilentlyContinue | Group-Object Length | ForEach-Object { "$($_.Count)x$([math]::Round($_.Name/1MB,1))MB" }) -join ' '
$res = if ($n -ge 3) { & $tool @ca } else { @('flicker: (too few pictures)') }
$rc = $LASTEXITCODE
$res | Set-Content (Join-Path $out 'check.txt')
$verdict = ($res | Select-String '^flicker:' | Select-Object -Last 1)
"{0,-14} {1,-12} fps {2,3} pics {3,3} {4,4} min | {5}" -f $Scene, $Config, $Fps, $n, $mins, $(if ($verdict) { $verdict.Line } else { 'flicker: (no result)' })
# mft digest: the last two summary lines of the run's mft counters (cumulative), as modern / exact and the reasons
$ms = @(Select-String -Path $log -Pattern 'mft: \d+ modern' | ForEach-Object { $_.Line })
if ($ms.Count -ge 2) { $a = [regex]::Match($ms[-2], 'mft: (\d+) modern frames, (\d+) exact \((.*?)\);'); $b = [regex]::Match($ms[-1], 'mft: (\d+) modern frames, (\d+) exact \((.*?)\);'); "  mft last 300 VBlanks: modern $([int]$b.Groups[1].Value - [int]$a.Groups[1].Value) exact $([int]$b.Groups[2].Value - [int]$a.Groups[2].Value) (cum. $($b.Groups[3].Value)); picture sizes $sizes" | Tee-Object (Join-Path $out 'digest.txt')}
# pictures: the first dumped frame in the list, 960-wide crops of the whole picture
$bmps = @(Get-ChildItem $fr -Filter 'frame_*.bmp' -ErrorAction SilentlyContinue | Sort-Object Name)
foreach ($i in $ShotAt) {
    if ($i -lt $bmps.Count) {
        & $tool --png $bmps[$i].FullName (Join-Path $out "shot_$i.png") | Out-Null
    }
}
if ($Keep) { "frames kept: $fr" } else { Remove-Item -Recurse -Force $fr -ErrorAction SilentlyContinue }
exit $rc



