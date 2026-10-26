# (r2-re) Trace one scene for the modern-renderer research (notes/wip/r2-re.md): the release build at a warp scenario
# (scripts\warp_run.ps1, headless) with PS2_R2_TRACE (render list + EE RAM of <Frames> game frames from VBlank <At>) and
# a GS recording of the same frames (PS2_GS_RECORD: VRAM + stream), optionally a VU1 recording (-Vu).
#   .\scripts\r2_scene.ps1 -Name ep1_hub -Episode 1 -Job m1_brawl -Play -At 4800
#   .\scripts\r2_scene.ps1 -Name m8_boss -Episode 8 -Job m8_boss -Direct -At 5200 -Pad "+2600:lup:90"
# Output: build\r2trace\<Name>\ (frame_<n>.txt, ram_<n>.bin, frame.gsrec[, vu.vurec]) and build\warp\r2_<Name>\frames.
# Decode with scripts\r2_frame.py build\r2trace\<Name> 0. Large files: delete build\r2trace\* when done.
param([Parameter(Mandatory = $true)][string]$Name, [Parameter(Mandatory = $true)][int]$Episode, [string]$Job = "",
      [switch]$Direct, [switch]$Play, [int]$At = 4800, [int]$Frames = 1, [int]$Seconds = 0, [string]$Pad = "",
      [switch]$Vu, [switch]$NoGs)
$root = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $root "build\r2trace\$Name"
New-Item -ItemType Directory -Force $dir | Out-Null
Remove-Item (Join-Path $dir '*') -ErrorAction SilentlyContinue
if (-not $Seconds) { $Seconds = [int]($At / 60) + 25 }
$env:PS2_R2_TRACE = "${At}:${Frames}:$dir"
if (-not $NoGs) {
    $env:PS2_GS_RECORD = Join-Path $dir 'frame.gsrec'; $env:PS2_GS_RECORD_FROM = "$At"; $env:PS2_GS_RECORD_FRAMES = "$($Frames + 2)"
}
if ($Vu) { $env:PS2_VU_RECORD = Join-Path $dir 'vu.vurec'; $env:PS2_VU_RECORD_FROM = "$At"; $env:PS2_VU_RECORD_COUNT = '6000' }
$wa = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'warp_run.ps1'), '-Episode', "$Episode", '-Release',
        '-Seconds', "$Seconds", '-DumpEvery', '300', '-Name', "r2_$Name")
if ($Job) { $wa += @('-Job', $Job) }
if ($Direct) { $wa += '-Direct' }
if ($Play) { $wa += '-Play' }
if ($Pad) { $wa += @('-Pad', $Pad) }
& powershell @wa | Out-Null
Remove-Item Env:\PS2_R2_TRACE, Env:\PS2_GS_RECORD, Env:\PS2_GS_RECORD_FROM, Env:\PS2_GS_RECORD_FRAMES, Env:\PS2_VU_RECORD,
            Env:\PS2_VU_RECORD_FROM, Env:\PS2_VU_RECORD_COUNT -ErrorAction SilentlyContinue
$w = Join-Path $root "build\warp\r2_$Name"
Remove-Item (Join-Path $w 'sly2_warp_*.exe') -ErrorAction SilentlyContinue
Select-String -Path (Join-Path $w 'run.log') -Pattern '\[r2\]|\[vu\] record|gsrec|GS record' | ForEach-Object { $_.Line }
Get-ChildItem $dir | Select-Object Name, Length | Format-Table -AutoSize | Out-String
