# Start the game at an episode or job: writes a memory card with scripts\warp.py, runs the game
# headless on it and presses START on the title screen ("Resume Game"). Each run gets its own
# directory (build\warp\<name>: card, exe copy, run.log, frames), so several can run in parallel.
#   .\scripts\warp_run.ps1 -Episode 3 [-Job m3_recon] [-Direct] [-Play] [-Seconds 150] [-Release]
#                          [-Pad "<more input>"] [-DumpEvery 120] [-Name ep3] [-Window]
#                          [-Set "path=value,path=value"] [-Make "--coins 5000 --gadgets"]
# -Job: the save has every task the job depends on done, so the hub opens with the job available.
# -Direct: the game also starts the job by itself (PS2_WARP_TASK, runtime/warp.c; any build).
# -Play: press X at the safehouse ("Press X to play as ...") 1500 frames after START, into the hub.
# Debug build (default, build\sly2) or -Release (build\rel): START is anchored to the title screen's
# START check (@187D80/1DD96C+30, an entry hook in release builds: recomp/hooks.py); -TitleFrame N
# presses it at pad frame N instead (an exe built before the hook). -Pad tokens follow START, e.g.
# "+900:cross" (relative to the START press). Frames: <dir>\frames\latest.png. Guide: notes/warp.md.
param([Parameter(Mandatory = $true)][int]$Episode, [string]$Job = "", [switch]$Direct, [switch]$Play,
      [int]$Seconds = 150, [switch]$Release, [string]$Pad = "", [int]$DumpEvery = 120, [string]$Name = "",
      [switch]$Window, [string]$Set = "", [string]$Make = "", [int]$TitleFrame = 0)
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'orig\SCUS_973.16'))) {   # worktree: find the main checkout
    $gitdir = ((Get-Content (Join-Path $root '.git') -Raw) -replace '^gitdir:\s*', '').Trim()
    $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
}
$py = Join-Path $main '.venv\Scripts\python.exe'
if ($Direct -and -not $Job) { "-Direct needs -Job"; exit 1 }
if (-not $Name) { $Name = "ep$Episode$(if ($Job) { "_$Job" })$(if ($Direct) { '_direct' })" }
$dir = Join-Path $root "build\warp\$Name"
$card = Join-Path $dir 'card'
$frames = Join-Path $dir 'frames'
New-Item -ItemType Directory -Force $dir, $frames | Out-Null
$env:PS2_USER_DIR = $dir                        # (the run's own folder holds its settings: never %LOCALAPPDATA%\Sly2)
Remove-Item (Join-Path $frames '*.png') -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $card -ErrorAction SilentlyContinue

$mk = @('scripts\warp.py', 'make', '--episode', "$Episode", '--card', $card)
if ($Job) { $mk += @('--job', $Job) }
if ($Direct) { $mk += '--direct' }
foreach ($s in ($Set -split ',' | Where-Object { $_ })) { $mk += @('--set', $s.Trim()) }
if ($Make) { $mk += ($Make -split '\s+' | Where-Object { $_ }) }   # more warp.py make options
Push-Location $root
$made = & $py @mk
$ok = $LASTEXITCODE -eq 0
Pop-Location
$made
if (-not $ok) { exit 1 }
Remove-Item Env:\PS2_WARP_TASK -ErrorAction SilentlyContinue
if ($Direct) {
    $w = $made | Select-String 'PS2_WARP_TASK=(\S+)' | Select-Object -First 1
    if (-not $w) { "warp.py printed no PS2_WARP_TASK"; exit 1 }
    $env:PS2_WARP_TASK = $w.Matches[0].Groups[1].Value
}

$build = Join-Path $root $(if ($Release) { 'build\rel' } else { 'build\sly2' })
$exeName = "sly2_warp_$Name.exe"                # own exe and a unique process name: parallel runs
$src = if ($env:M9_GAME_EXE) { $env:M9_GAME_EXE } else { Join-Path $build 'sly2.exe' }   # (M9: M9_GAME_EXE runs another build, e.g. a baseline)
Copy-Item $src (Join-Path $dir $exeName) -Force
$script = if ($TitleFrame -gt 0) { "${TitleFrame}:start:10" } else { '@187D80/1DD96C+30:start:10' }
if ($Play -and -not $Direct -and $Episode -gt 0) { $script += ',+1500:cross:6' }
if ($Pad) { $script += ",$Pad" }
$runArgs = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'run_game.ps1'), '-Seconds', "$Seconds",
             '-Out', $dir, '-Card', $card, '-Dump', $frames, '-DumpEvery', "$DumpEvery", '-Pad', $script,
             '-Exe', $exeName)
if ($Window) { $runArgs += '-Window' }
# (reimagine) PS2_TEXCACHE_SEED=<dir>: the Reimagined textures' cache (<user dir>\textures) copied in before the run and
# the new images back after it, so runs start with a warm cache (a copy, never a junction: run folders get deleted)
$seed = $env:PS2_TEXCACHE_SEED
if ($seed -and (Test-Path $seed)) { robocopy $seed (Join-Path $dir 'textures') /E /NFL /NDL /NJH /NJS /NP | Out-Null }
& powershell @runArgs | Select-Object -First 1
if ($seed) { New-Item -ItemType Directory -Force $seed | Out-Null; if (Test-Path (Join-Path $dir 'textures')) { robocopy (Join-Path $dir 'textures') $seed /E /XO /NFL /NDL /NJH /NJS /NP | Out-Null } }
"log: $dir\run.log; frames: $frames (latest.png)"
Select-String -Path (Join-Path $dir 'run.log') -Pattern '\[warp\]|pad frame|not implemented|unhandled|error|assert|crash|exception|fatal' |
    Where-Object { $_.Line -notmatch 'syscall 0x5B' } | Select-Object -First 40 | ForEach-Object { $_.Line }
