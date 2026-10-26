# (r2-m1) Modern renderer A/B on one game frame (notes/modern-renderer.md 5, levels 2-3): records a scene with
# PS2_R2=bracket (scripts\r2_scene.ps1: GS recording + EE RAM dump of the same frame; the native entries' exact CALLs stay,
# bracketed by R2 hints), replays it twice through the game's GPU path (scripts\gs_replay.ps1 -Game): --r2-ab 0 (the exact
# picture) and --r2-ab 1 (R2 draws the bracketed entries: the hybrid picture of the same stream, every other draw identical),
# and compares GS memory (colour, alpha, Z at sample 0) and the pictures (scripts\r2_ab_compare.py).
#   .\scripts\r2_ab.ps1 -Name ep3 -Episode 3 -Play -At 5000 [-Scale 1] [-NoRecord] [-Levels all]
# Output: build\r2ab\<Name>\{exact,hybrid}_s<Scale>\ (pictures, vram_<vblank>.bin) - delete when done (4 MB per VBlank).
param([Parameter(Mandatory = $true)][string]$Name, [int]$Episode = 3, [string]$Job = "", [switch]$Direct, [switch]$Play,
      [int]$At = 5000, [string]$Pad = "", [int]$Scale = 1, [switch]$NoRecord, [string]$Levels = "")
$root = Split-Path $PSScriptRoot -Parent
$py = (Get-Command python -ErrorAction SilentlyContinue).Source
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {
    $gitdir = ((Get-Content (Join-Path $root '.git') -Raw) -replace '^gitdir:\s*', '').Trim()
    $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
}
$py = Join-Path $main '.venv\Scripts\python.exe'
$trace = Join-Path $root "build\r2trace\ab_$Name"
if (-not $NoRecord) {
    $env:PS2_R2 = 'bracket'
    if ($Levels) { $env:PS2_R2_LEVELS = $Levels }
    $a = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'r2_scene.ps1'), '-Name', "ab_$Name", '-Episode', "$Episode",
           '-At', "$At", '-Frames', '1')
    if ($Job) { $a += @('-Job', $Job) }
    if ($Direct) { $a += '-Direct' }
    if ($Play) { $a += '-Play' }
    if ($Pad) { $a += @('-Pad', $Pad) }
    & powershell @a | Out-Null
    Remove-Item Env:\PS2_R2, Env:\PS2_R2_LEVELS -ErrorAction SilentlyContinue
}
$rec = Join-Path $trace 'frame.gsrec'
$ram = Join-Path $trace 'ram_0.bin'
if (-not (Test-Path $rec) -or -not (Test-Path $ram)) { "no recording in $trace"; exit 1 }
$base = Join-Path $root "build\r2ab\$Name"
$dirs = @()
foreach ($ab in 0, 1) {
    $o = Join-Path $base ("{0}_s{1}" -f @('exact', 'hybrid')[$ab], $Scale)
    New-Item -ItemType Directory -Force $o | Out-Null
    Get-ChildItem $o -Filter 'vram_*.bin' -ErrorAction SilentlyContinue | Remove-Item
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'gs_replay.ps1') -BuildOnly -Out $o | Out-Null
    $exe = Join-Path $o 'gs_replay.exe'
    $run = Join-Path $o ("gs_replay_ab_{0}.exe" -f $PID)
    Copy-Item $exe $run -Force
    $args2 = @($rec, $o, '0', '--backend', 'hw', '--game', '--game-save', '--vram-out', '--r2-ram', $ram, '--r2-ab', "$ab")
    if ($Scale -gt 1) { $args2 += @('--scale', "$Scale") }
    & $run @args2 2>$null | Select-String 'game:|pic-hash' | ForEach-Object { "  [$ab] $($_.Line)" }
    Remove-Item $run -ErrorAction SilentlyContinue
    $dirs += $o
}
& $py (Join-Path $PSScriptRoot 'r2_ab_compare.py') $dirs[0] $dirs[1]
