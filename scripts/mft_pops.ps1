# (mft-pop) The R8 "modern frame targets" check of one scene: the release build at a warp scenario in modern mode with MFT
# at 2560 x 1440 MSAA 4x (frame_rate 120, no dumps), PS2_R2_STATS + PS2_MFT_LOG. A "pop" is a run of 1..8 grid frames
# between MFT frames (gpu_exec_mft.h: a one-frame resolution / AA switch in gameplay; menus and loading screens are longer
# runs); the check passes when MFT ran and no pop happened. (stray-tri) It also fails on any one-frame stray triangle: a
# triangle with a vertex Q no clipped vertex can have (PS2_GF_BIGTRI 'badq' lines from VBlank At on, gpu_front.c; the
# R2 clipper's fans of degenerate objects drew such huge / sliver wedges for one frame on MFT).
#   .\scripts\mft_pops.ps1 -Name ep1 -Episode 1 -Job m1_brawl -Play -At 4800 [-Secs 40] [-Exe <exe>]
# Output: one line "mftpops: PASS|FAIL pops N; <the last PS2_R2_STATS mft line>", the pops' "mft: pop:" lines, and the
# run log at build\warp\mftpops_<Name>\run.log (deleted on PASS unless -Keep).
param([Parameter(Mandatory = $true)][string]$Name, [int]$Episode = 1, [string]$Job = "", [switch]$Direct, [switch]$Play,
      [int]$At = 4800, [string]$Pad = "", [int]$Secs = 40, [switch]$Keep, [string]$Exe = "")
$root = Split-Path $PSScriptRoot -Parent
$set = @{ PS2_R2 = 'hybrid'; PS2_MFT = '1'; PS2_MFT_MSAA = '4'; PS2_GS_PRESENT_SIZE = '2560x1440'; PS2_FRAME_RATE = '120';
          PS2_R2_STATS = '1'; PS2_MFT_LOG = '1'; PS2_GS_SCALE = $null }
$wdir = Join-Path $root "build\warp\mftpops_$Name"
New-Item -ItemType Directory -Force $wdir | Out-Null
$bq = Join-Path $wdir 'badq.txt'
Remove-Item $bq -ErrorAction SilentlyContinue
$set.PS2_GF_BIGTRI = '100000'; $set.PS2_GF_BIGTRI_FROM = "$At"; $set.PS2_GF_BIGTRI_FILE = $bq   # (only 'badq' lines)
$saved = @{}
foreach ($k in $set.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $set[$k]) }
if ($Exe) { $env:M9_GAME_EXE = $Exe }
$wa = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'warp_run.ps1'), '-Episode', "$Episode", '-Release',
        '-Seconds', "$([int]($At / 60) + $Secs)", '-DumpEvery', '100000', '-Name', "mftpops_$Name")
if ($Job) { $wa += @('-Job', $Job) }
if ($Direct) { $wa += '-Direct' }
if ($Play) { $wa += '-Play' }
if ($Pad) { $wa += @('-Pad', $Pad) }
& powershell @wa | Out-Null
foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
Remove-Item Env:\M9_GAME_EXE -ErrorAction SilentlyContinue
$w = Join-Path $root "build\warp\mftpops_$Name"
Remove-Item (Join-Path $w 'sly2_warp_*.exe') -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force (Join-Path $w 'frames') -ErrorAction SilentlyContinue
$log = Join-Path $w 'run.log'
$pops = @(Select-String -Path $log -Pattern 'mft: pop: ' -ErrorAction SilentlyContinue)
$last = Select-String -Path $log -Pattern 'mft: (\d+) modern frames' -ErrorAction SilentlyContinue | Select-Object -Last 1
$modern = if ($last) { [int64]$last.Matches[0].Groups[1].Value } else { 0 }
$badq = @(Select-String -Path $bq -Pattern '^badq' -ErrorAction SilentlyContinue)
$ok = $modern -gt 0 -and $pops.Count -eq 0 -and $badq.Count -eq 0
$badq | Select-Object -First 5 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(240, $_.Line.Length)) }
$pops | Select-Object -First 20 | ForEach-Object { $_.Line -replace '^\[gpu\] ', '' }
"mftpops: $(if ($ok) { 'PASS' } else { 'FAIL' }) pops $($pops.Count), stray triangles $($badq.Count); $(if ($last) { $last.Line -replace '^\[gpu\] mft: ', '' } else { '(no MFT statistics)' })"
if ($ok -and -not $Keep) { Remove-Item -Recurse -Force $w -ErrorAction SilentlyContinue }
exit $(if ($ok) { 0 } else { 1 })
