# Play the end of the game unattended: Episode 8's final boss from its Murray stage (direct start of
# t8_open_head_intro) through the Bentley stage, the Hate Chip, the ending cutscenes and the credits,
# up to the autosave and the episode menu. A regression run for the whole tail of the game.
#   .\scripts\ep8_finale.ps1 [-Release] [-Wav] [-Name finale] [-Minutes 40] [-Credits 8] [-Keep]
# Steered live (PS2_INPUT_FILE, live_goto.py / live_hop.py / live_waitpos.py on PS2_PEEK 2DE1C0);
# health is held with PS2_POKE (Murray 0x3D4AE0, Bentley 0x3D4AC8) because steering does not dodge
# lasers. Steps (world x,y): Murray walks to the beak (60,250), circle grabs the hold (Murray moves to
# (283,448)) and any face button mashed >= 4/s for ~8 s pries the mouth open; Bentley drops a bomb
# (triangle) at each laser emitter on the mouth corners (~(140,345) and (-72,345)); after Clock-La's
# cutscene he hops onto the lower jaw (60,650) and takes the Hate Chip with circle; the game
# autosaves (save1: episode.blimp.percent_complete 100) as the ending starts. Progress:
# <dir>\steps.txt; frames <dir>\frames (latest.png); -Wav records <dir>\run.wav. Exits 0 once that
# autosave is written (1 otherwise), after letting the ending and credits run -Credits more minutes
# (about 7 to the episode menu). -Keep leaves the game running.
param([switch]$Release, [switch]$Wav, [string]$Name = "finale", [int]$Minutes = 40, [int]$Credits = 8, [switch]$Keep)
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'orig\SCUS_973.16'))) {   # worktree: find the main checkout
    $gitdir = ((Get-Content (Join-Path $root '.git') -Raw) -replace '^gitdir:\s*', '').Trim()
    $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
}
$py = Join-Path $main '.venv\Scripts\python.exe'
$dir = Join-Path $root "build\warp\$Name"
$card = Join-Path $dir 'card'
New-Item -ItemType Directory -Force $dir | Out-Null
Remove-Item -Recurse -Force $card -ErrorAction SilentlyContinue
Remove-Item (Join-Path $dir 'steps.txt') -ErrorAction SilentlyContinue
Push-Location $root
$made = & $py scripts\warp.py make --episode 8 --job t8_open_head_intro --direct --card $card
Pop-Location
$w = $made | Select-String 'PS2_WARP_TASK=(\S+)' | Select-Object -First 1
if (-not $w) { $made; "warp.py printed no PS2_WARP_TASK"; exit 1 }
$env:PS2_WARP_TASK = $w.Matches[0].Groups[1].Value
$env:PS2_MEMCARD_DIR = $card
$env:PS2_PEEK = '2DE1C0:4,2DDFD0:4'
$env:PS2_PEEK_EVERY = '10'
$env:PS2_POKE = '3D4AE0=3C,3D4AC8=28'
if ($Wav) { $env:PS2_AUDIO_WAV = Join-Path $dir 'run.wav' } else { Remove-Item Env:\PS2_AUDIO_WAV -ErrorAction SilentlyContinue }
$exe = Join-Path $dir "sly2_$Name.exe"
Copy-Item (Join-Path $root $(if ($Release) { 'build\rel\sly2.exe' } else { 'build\sly2\sly2.exe' })) $exe -Force
& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'live_game.ps1') -Script '@187D80/1DD96C+30:start:10' `
    -Minutes $Minutes -Dir $dir -DumpEvery 10 -Exe $exe
foreach ($v in 'PS2_WARP_TASK', 'PS2_MEMCARD_DIR', 'PS2_PEEK', 'PS2_PEEK_EVERY', 'PS2_POKE', 'PS2_AUDIO_WAV') { Remove-Item "Env:\$v" -ErrorAction SilentlyContinue }
$gamePid = (Get-Process -Name "sly2_$Name" -ErrorAction SilentlyContinue | Sort-Object StartTime | Select-Object -Last 1).Id   # (capturing live_game's output would wait for the game)
$save = Join-Path $card 'BASCUS-97316YAOTWTD!\save1'
$saveTime = (Get-Item $save).LastWriteTime

function Step($m) { $t = Get-Date -Format HH:mm:ss; Add-Content (Join-Path $dir 'steps.txt') "$t $m"; Write-Host "$t $m" }
function Send($s) { Add-Content (Join-Path $dir 'input.txt') $s -Encoding ascii }
function Py([string[]]$a) { $o = & $py @a 2>&1; $script:pyOk = $LASTEXITCODE -eq 0; ($o | Select-Object -Last 1) }
function Goto([string[]]$pts, [int]$tol = 40, [int]$timeout = 150) {
    Step ("goto " + ($pts -join ' ') + ": " + (Py (@('scripts\live_goto.py', '--dir', $dir, '--timeout', "$timeout", '--tol', "$tol", '--') + $pts)))
}
function WaitPos([string]$pt, [int]$tol, [int]$timeout, [int]$stable = 3, [switch]$Far) {
    $a = @('scripts\live_waitpos.py', '--dir', $dir, '--tol', "$tol", '--timeout', "$timeout", '--stable', "$stable")
    if ($Far) { $a += '--far' }
    $r = Py ($a + @('--', $pt))
    Step "waitpos $pt : $r"
    return $script:pyOk
}
Push-Location $root
try {
    Step "game pid $gamePid"
    if (-not (WaitPos "0,-2168" 20 900 5)) { exit 1 }                 # Murray's start point
    Goto @("0,-1200", "60,0", "60,250")
    $mash = (",+5:square:2,+5:triangle:2" * 150)
    for ($try = 0; $try -lt 6; $try++) {                               # circle -> Murray takes the hold
        Send ("+0:circle:6,+60:none" + $mash)
        Start-Sleep 4
        if (WaitPos "283,448" 40 8 2) { break }
        Goto @("60,0", "60,250")
    }
    if (-not (WaitPos "-250,-2100" 250 300)) { exit 1 }                # Bentley's start point
    for ($round = 0; $round -lt 4; $round++) {                         # the two laser emitters
        foreach ($e in "140,345", "-72,345") {                         # (a laser hit can cancel a drop)
            Goto @("-100,-800", "60,150", $e) 12
            Start-Sleep 1
            Send "+0:triangle:4"
            Start-Sleep 1
            Goto @("35,150")
            Start-Sleep 5
        }
        # both gone -> Clock-La's cutscene, then Bentley stands at (172.7,224.4) (pending stick input
        # may move him a little)
        if (WaitPos "180,232" 30 45 3) { break }
    }
    for ($try = 0; $try -lt 4; $try++) {                               # the Hate Chip on the lower jaw
        Goto @("60,150", "40,480") 25
        Start-Sleep 1                                                  # let the last stick command end
        Step ("hop: " + (Py @('scripts\live_hop.py', '60,650', '--dir', $dir, '--hold', '40', '--jumps', '2', '--gap', '12')))
        Start-Sleep 3
        Send "+0:circle:6,+30:circle:6"
        Start-Sleep 8
        if (WaitPos "60,650" 800 4 3 -Far) { break }                   # the ending took the focus away
    }
    $deadline = (Get-Date).AddMinutes(5)                               # the autosave as the ending starts
    while ((Get-Date) -lt $deadline -and (Get-Item $save).LastWriteTime -eq $saveTime) { Start-Sleep 5 }
    $ok = (Get-Item $save).LastWriteTime -ne $saveTime
    Step $(if ($ok) { "autosave written: game completed" } else { "no autosave within 5 minutes" })
    if ($ok) {
        Start-Sleep 5
        $s = & $py scripts\warp.py dump $save 2>$null | Select-String 'episode.blimp.percent_complete'
        Step "save1: $s"
        Start-Sleep ($Credits * 60)                                    # ending cutscenes and credits
        Step "ending and credits ran $Credits minutes"
    }
    if ($ok) { exit 0 } else { exit 1 }
} finally {
    Pop-Location
    if (-not $Keep) { Stop-Process -Id $gamePid -ErrorAction SilentlyContinue }
}
