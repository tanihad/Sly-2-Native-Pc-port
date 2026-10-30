# (r2-m5) The hybrid check of one scene (notes/modern-renderer.md 5, level 3 in game): the release build at a warp
# scenario in hybrid mode with PS2_R2_ALT (from VBlank At - 60 on, hybrid frames alternate with all-exact ones), every
# picture of VBlanks At .. At + Frames dumped, then scripts\r2_hcheck.py: whatever only one mode draws alternates. This
# catches what the bracket A/B cannot: the VU1 / VIF / GS state the dropped mesh DMA would have left for later exact draws.
#   .\scripts\r2_hcheck.ps1 -Name ep1 -Episode 1 -Job m1_brawl -Play -At 4800 [-Frames 8] [-Keep]
# (r2-m8) The check runs on the integer grid (PS2_MFT=0: modern frames at any resolution are not comparable with the
# all-exact ones). -Shots instead: no alternation, modern frames at 2560 x 1440 with MSAA 4x (R8), the pictures of
# VBlanks At .. At + Frames dumped and the last one kept as build\r2hc\<Name>\shot.png (the sweep's pictures).
# Output: build\r2hc\<Name>\ (frames deleted unless -Keep; worst.png = the worst hybrid-mode picture, alternating pixels
# in magenta, kept).
param([Parameter(Mandatory = $true)][string]$Name, [int]$Episode = 1, [string]$Job = "", [switch]$Direct, [switch]$Play,
      [int]$At = 4800, [string]$Pad = "", [int]$Frames = 8, [switch]$Keep, [string]$Exe = "", [switch]$Shots, [switch]$Grid)
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {
    $gitdir = ((Get-Content (Join-Path $root '.git') -Raw) -replace '^gitdir:\s*', '').Trim()
    $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
}
$py = Join-Path $main '.venv\Scripts\python.exe'
$out = Join-Path $root "build\r2hc\$Name"
New-Item -ItemType Directory -Force $out | Out-Null
$env:PS2_R2 = 'hybrid'
if ($Shots) { $env:PS2_MFT = $(if ($Grid) { '0' } else { '1' }); $env:PS2_MFT_MSAA = '4'; $env:PS2_GS_PRESENT_SIZE = '2560x1440' }
else { $env:PS2_MFT = '0'; $env:PS2_R2_ALT = "$($At - 60)" }
$env:PS2_DUMP_FROM = "$At"
$env:PS2_DUMP_TO = "$($At + $Frames - 1)"
if ($Exe) { $env:M9_GAME_EXE = $Exe }
$wa = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'warp_run.ps1'), '-Episode', "$Episode", '-Release',
        '-Seconds', "$([int]($At / 60) + 12)", '-DumpEvery', '1', '-Name', "r2hc_$Name")
if ($Job) { $wa += @('-Job', $Job) }
if ($Direct) { $wa += '-Direct' }
if ($Play) { $wa += '-Play' }
if ($Pad) { $wa += @('-Pad', $Pad) }
& powershell @wa | Out-Null
Remove-Item Env:\PS2_R2, Env:\PS2_R2_ALT, Env:\PS2_DUMP_FROM, Env:\PS2_DUMP_TO, Env:\M9_GAME_EXE, Env:\PS2_MFT, Env:\PS2_MFT_MSAA, Env:\PS2_GS_PRESENT_SIZE -ErrorAction SilentlyContinue
$w = Join-Path $root "build\warp\r2hc_$Name"
Remove-Item (Join-Path $w 'sly2_warp_*.exe') -ErrorAction SilentlyContinue
$fr = Join-Path $w 'frames'
Remove-Item (Join-Path $fr 'latest.png') -ErrorAction SilentlyContinue
if ($Shots) {
    $last = Get-ChildItem $fr -Filter 'frame_*.png' -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
    $rc = 1
    if ($last) { & $py (Join-Path $PSScriptRoot 'shot_small.py') (Join-Path $out 'shot.png') $last.FullName --scale 2 | Out-Null; $rc = 0 }
} else {
& $py (Join-Path $PSScriptRoot 'r2_hcheck.py') $fr --out (Join-Path $out 'worst.png')
$rc = $LASTEXITCODE
}
if (-not $Keep) { Remove-Item -Recurse -Force $fr -ErrorAction SilentlyContinue }
exit $rc
