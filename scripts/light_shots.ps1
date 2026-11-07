# (light) Original vs Enhanced lighting pictures of one scene from ONE run: the Enhanced lighting pass follows in-run A/B
# phases (PS2_GPU_AB=<Phase> PS2_GPU_AB_R8=64: Original in A phases, Enhanced in B), so pictures Phase VBlanks apart show
# the same scene both ways (an idle scene: Sly stands still). Balanced preset, 2560 x 1440, frame rate 60, headless.
#   .\scripts\light_shots.ps1 -Scene ep1_hub [-At 5600] [-Frames 72] [-Phase 30] [-Extra "K=V;K=V"] [-Ini "graphics.x=y"] [-Tag t]
# Scenes: scripts\qa_scenes_a.txt (ep1-4 hubs, m2_boardroom, m3_boss) and scripts\qa_scenes.txt (ep5-8).
# Output: build\light\shots\<scene>[__tag]\frame_<v>.png (every dumped picture) + run.log. Local only: never commit them.
param([Parameter(Mandatory = $true)][string]$Scene, [int]$At = 0, [int]$Frames = 72, [int]$Phase = 30, [string]$Extra = '',
      [string]$Ini = '', [string]$Tag = '', [string]$Size = '2560x1440', [int]$Every = 1, [switch]$NoAB, [string]$Fps = '60', [int]$Seconds = 0)
$root = Split-Path $PSScriptRoot -Parent
$free = [math]::Round((Get-PSDrive C).Free / 1GB, 1)
if ($free -lt 40) { "C: has $free GB free (< 40): not starting"; exit 2 }
$line = @(Get-Content (Join-Path $PSScriptRoot 'qa_scenes_a.txt'), (Join-Path $PSScriptRoot 'qa_scenes.txt')) | Where-Object { $_ -and -not $_.StartsWith('#') } |
    Where-Object { ($_ -split '\|')[0].Trim() -eq $Scene } | Select-Object -First 1
if (-not $line) { "unknown scene $Scene"; exit 2 }
$f = @($line -split '\|' | ForEach-Object { $_.Trim() })
$episode = [int]$f[1]; $job = $f[2]; $flags = $f[3]
$pad = if ($f[5] -match '^\d+$') { if ($f.Count -ge 7) { $f[6] } else { '' } } else { $f[5] }   # (qa_scenes_a: name|ep|job|flags|at|frames|pad; qa_scenes: name|ep|job|flags|at|pad|note)
if ($pad -match '^[A-Z]+$') { $pad = '' }   # (the GP / HP walk macros of qa_scene.ps1: not here - an idle scene)
if ($At -le 0) { $At = [int]$f[4] }
$name = "$Scene$(if ($Tag) { "__$Tag" })" -replace '[^A-Za-z0-9_]', '_'
$out = Join-Path $root "build\light\shots\$name"
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
$kv = [ordered]@{ 'graphics.preset' = 'balanced' }
foreach ($p in ($Ini -split ';' | Where-Object { $_ })) { $k, $v = $p -split '=', 2; $kv[$k.Trim()] = $v.Trim(); if ($k.Trim() -ne 'graphics.preset') { $kv['graphics.preset'] = 'custom' } }
$kv['display.frame_rate'] = $Fps; $kv['display.aspect'] = '16:9'
$sec = [ordered]@{}
foreach ($k in $kv.Keys) { $s, $n = $k -split '\.', 2; if (-not $sec.Contains($s)) { $sec[$s] = @() }; $sec[$s] += "$n = $($kv[$k])" }
$ini = ''
foreach ($s in $sec.Keys) { $ini += "[$s]`r`n" + (($sec[$s]) -join "`r`n") + "`r`n" }
$runName = "light_$name"
$w = Join-Path $root "build\warp\$runName"
New-Item -ItemType Directory -Force $w | Out-Null
[IO.File]::WriteAllText((Join-Path $w 'sly2.ini'), $ini)
$set = [ordered]@{ PS2_DUMP_FROM = "$At"; PS2_DUMP_TO = "$($At + $Frames - 1)"; PS2_DUMP_WAIT = '0'; PS2_GS_PRESENT_SIZE = $Size;
    PS2_GS_GOVERNOR = '0'; PS2_R2 = $null; PS2_GS_SCALE = $null; PS2_MFT = $null; PS2_LIGHT = '1'; PS2_LIGHT_LOG = '1';
    PS2_GPU_AB = "$Phase"; PS2_GPU_AB_R8 = '64'; PS2_USER_DIR = $w }   # (the run's own folder: never %LOCALAPPDATA%\Sly2)
if ($NoAB) { $set.PS2_GPU_AB = $null; $set.PS2_GPU_AB_R8 = $null }
foreach ($e in ($flags -split ' ' | Where-Object { $_ -like 'env=*' })) { $a, $b = $e.Substring(4) -split '=', 2; $set[$a] = $b }
foreach ($e in ($Extra -split ';' | Where-Object { $_ })) { $a, $b = $e -split '=', 2; $set[$a.Trim()] = $b }
$saved = @{}
foreach ($k in $set.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $set[$k]) }
$secs = if ($Seconds -gt 0) { $Seconds } else { [int](($At + $Frames) / 60) + 40 }
$wa = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'warp_run.ps1'), '-Episode', "$episode", '-Release',
        '-Seconds', "$secs", '-DumpEvery', "$Every", '-Name', $runName)
if ($job) { $wa += @('-Job', $job) }
if ($flags -match '\bdirect\b') { $wa += '-Direct' }
if ($flags -match '\bplay\b') { $wa += '-Play' }
if ($pad) { $wa += @('-Pad', $pad) }
& powershell @wa | Out-Null
foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
Remove-Item (Join-Path $w 'sly2_warp_*.exe') -ErrorAction SilentlyContinue
Copy-Item (Join-Path $w 'run.log') (Join-Path $out 'run.log') -ErrorAction SilentlyContinue
$fr = Join-Path $w 'frames'
Remove-Item (Join-Path $fr 'latest.png') -ErrorAction SilentlyContinue
Move-Item (Join-Path $fr 'frame_*.png') $out -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $w -ErrorAction SilentlyContinue
$n = @(Get-ChildItem $out -Filter 'frame_*.png').Count
"$Scene`: $n pictures in $out"
Select-String -Path (Join-Path $out 'run.log') -Pattern 'light:|GPU renderer unavailable|device lost|error' | Select-Object -First 12 | ForEach-Object { $_.Line }
