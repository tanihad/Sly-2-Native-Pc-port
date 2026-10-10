# Start the game at an episode (or job): writes a memory card with scripts\warp.py, then runs the game
# headless on it and presses START on the title screen ("Resume Game"). Each run gets its own
# directory (build\warp\<name>: card, exe copy, run.log, frames), so several can run in parallel.
#   .\scripts\warp_run.ps1 -Episode 3 [-Job m3_recon] [-Seconds 150] [-Release] [-Pad "<more input>"]
#                          [-DumpEvery 120] [-Name ep3] [-Window] [-Set "path=value,path=value"]
#                          [-Make "--complete t1_sat_ --activate t1_recon_intro"] (more warp.py make options)
# Debug build (default, build\sly2): START is anchored to the title screen's START check
# (@187D80/1DD96C+30). -Release (build\rel, no anchors): START at pad frame -TitleFrame (default 2400,
# ~40 s), which is after the title appears in both builds. -Pad tokens are appended after START,
# e.g. "+900:cross" (relative to the START press). Frames: <dir>\frames\latest.png. See notes/warp.md.
param([Parameter(Mandatory = $true)][int]$Episode, [string]$Job = "", [int]$Seconds = 150,
      [switch]$Release, [string]$Pad = "", [int]$DumpEvery = 120, [string]$Name = "", [switch]$Window,
      [string]$Set = "", [string]$Make = "", [int]$TitleFrame = 2400)
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'orig\SCUS_973.16'))) {   # worktree: find the main checkout
    $gitdir = ((Get-Content (Join-Path $root '.git') -Raw) -replace '^gitdir:\s*', '').Trim()
    $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
}
$py = Join-Path $main '.venv\Scripts\python.exe'
if (-not $Name) { $Name = "ep$Episode$(if ($Job) { "_$Job" })" }
$dir = Join-Path $root "build\warp\$Name"
$card = Join-Path $dir 'card'
$frames = Join-Path $dir 'frames'
New-Item -ItemType Directory -Force $dir, $frames | Out-Null
Remove-Item (Join-Path $frames '*.png') -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $card -ErrorAction SilentlyContinue

$mk = @('scripts\warp.py', 'make', '--episode', "$Episode", '--card', $card)
if ($Job) { $mk += @('--job', $Job) }
foreach ($s in ($Set -split ',' | Where-Object { $_ })) { $mk += @('--set', $s.Trim()) }
if ($Make) { $mk += ($Make -split '\s+' | Where-Object { $_ }) }   # more warp.py make options
Push-Location $root
& $py @mk
if ($LASTEXITCODE) { Pop-Location; exit 1 }
Pop-Location

$build = Join-Path $root $(if ($Release) { 'build\rel' } else { 'build\sly2' })
Copy-Item (Join-Path $build 'sly2.exe') (Join-Path $dir 'sly2.exe') -Force   # own exe: parallel runs
$start = if ($Release) { "${TitleFrame}:start:10" } else { '@187D80/1DD96C+30:start:10' }
$script = $start + $(if ($Pad) { ",$Pad" })
$runArgs = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'run_game.ps1'), '-Seconds', "$Seconds",
          '-Out', $dir, '-Card', $card, '-Dump', $frames, '-DumpEvery', "$DumpEvery", '-Pad', $script)
if ($Window) { $runArgs += '-Window' }
& powershell @runArgs | Select-Object -First 1
"log: $dir\run.log; frames: $frames (latest.png)"
Select-String -Path (Join-Path $dir 'run.log') -Pattern 'pad frame|not implemented|unhandled|error|assert|crash|exception' |
    Select-Object -First 40 | ForEach-Object { $_.Line }
