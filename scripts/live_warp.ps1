# Live play from an episode or job start: writes a warp card (scripts\warp.py), starts the game in the
# background with PS2_INPUT_FILE (scripts\live_game.ps1) and presses START on the title screen.
#   .\scripts\live_warp.ps1 -Episode 2 [-Job m2_rc_chopper] [-Direct] [-Release] [-Name rc] [-DumpEvery 30]
#                           [-Pad "<more PS2_INPUT tokens after START>"] [-Minutes 60]
# Then steer it with scripts\live_look.ps1 -Dir build\live\<name> -Send "+0:cross" -Wait 3.
# -Direct (debug build): the game starts the job by itself (PS2_WARP_TASK, notes/warp.md).
# Peeks the player/camera positions (PS2_PEEK 2DE1C0, 2DDFD0) for scripts\live_goto.py.
param([Parameter(Mandatory = $true)][int]$Episode, [string]$Job = "", [switch]$Direct, [switch]$Release,
      [string]$Name = "", [int]$DumpEvery = 30, [string]$Pad = "", [int]$Minutes = 60, [int]$TitleFrame = 2400)
$root = Split-Path $PSScriptRoot -Parent
$py = 'C:\Users\tanih\Projects\sly2-decomp\.venv\Scripts\python.exe'
if ($Direct -and $Release) { "-Direct needs the debug build"; exit 1 }
if (-not $Name) { $Name = "ep$Episode$(if ($Job) { "_$Job" })" }
$dir = Join-Path $root "build\live\$Name"
$card = Join-Path $dir 'card'
New-Item -ItemType Directory -Force $dir | Out-Null
Remove-Item -Recurse -Force $card -ErrorAction SilentlyContinue
$mk = @((Join-Path $root 'scripts\warp.py'), 'make', '--episode', "$Episode", '--card', $card)
if ($Job) { $mk += @('--job', $Job) }
if ($Direct) { $mk += '--direct' }
$made = & $py @mk
$made
Remove-Item Env:\PS2_WARP_TASK -ErrorAction SilentlyContinue
if ($Direct) {
    $w = $made | Select-String 'PS2_WARP_TASK=(\S+)' | Select-Object -First 1
    $env:PS2_WARP_TASK = $w.Matches[0].Groups[1].Value
}
$build = Join-Path $root $(if ($Release) { 'build\rel' } else { 'build\sly2' })
$exe = Join-Path $dir "sly2_$Name.exe"
Copy-Item (Join-Path $build 'sly2.exe') $exe -Force
$env:PS2_MEMCARD_DIR = $card
if (-not $env:PS2_PEEK) { $env:PS2_PEEK = '2DE1C0:4,2DDFD0:4' }
if (-not $env:PS2_PEEK_EVERY) { $env:PS2_PEEK_EVERY = '30' }
$script = if ($Release) { "${TitleFrame}:start:10" } else { '@187D80/1DD96C+30:start:10' }
if ($Pad) { $script += ",$Pad" }
& (Join-Path $PSScriptRoot 'live_game.ps1') -Script $script -Minutes $Minutes -Dir $dir -DumpEvery $DumpEvery -Exe $exe
