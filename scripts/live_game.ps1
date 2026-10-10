# Start the game in the background for live, step-by-step play from the agent shell.
#   .\scripts\live_game.ps1 [-Script "<PS2_INPUT script>"] [-Minutes 60] [-Dir build\live] [-DumpEvery 30]
# Then steer it by appending lines to <Dir>\input.txt (each line's "+n" tokens count from the moment
# it is read, e.g.  Add-Content build\live\input.txt "+0:lup:90,+100:cross"), and look at
# <Dir>\frames\latest.png (newest dump). Log: <Dir>\run.log. Stop: Stop-Process -Id <pid>.
param([string]$Script = "", [int]$Minutes = 60, [string]$Dir = "", [int]$DumpEvery = 30, [string]$Exe = "")
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'orig\SCUS_973.16'))) {   # worktree: find the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
if (-not $Dir) { $Dir = Join-Path $root 'build\live' }
if (-not $Exe) { $Exe = Join-Path $root 'build\sly2\sly2.exe' }
$frames = Join-Path $Dir 'frames'
New-Item -ItemType Directory -Force $frames | Out-Null
Remove-Item (Join-Path $frames '*.png') -ErrorAction SilentlyContinue
$ScriptFile = Join-Path $Dir 'input.txt'
Set-Content -Path $ScriptFile -Value "" -Encoding ascii

$env:PS2_WATCHDOG = "$($Minutes * 60)"
$env:PS2_HEADLESS = "1"
$env:PS2_DUMP_FRAMES = $frames
$env:PS2_DUMP_EVERY = "$DumpEvery"
$env:PS2_DUMP_RING = "200"                       # keep the newest 200 dumps only
Remove-Item Env:\PS2_DUMP_FROM -ErrorAction SilentlyContinue
$env:PS2_INPUT = $Script
$env:PS2_INPUT_FILE = $ScriptFile
$iso = Get-ChildItem (Join-Path $main 'orig\disc') -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1
if ($iso) { $env:PS2_ISO = $iso.FullName }
$p = Start-Process -FilePath $Exe -ArgumentList (Join-Path $main 'orig\SCUS_973.16') `
    -RedirectStandardError (Join-Path $Dir 'run.log') -RedirectStandardOutput (Join-Path $Dir 'run.out') -PassThru -NoNewWindow
"pid $($p.Id); input $ScriptFile; frames $frames"
