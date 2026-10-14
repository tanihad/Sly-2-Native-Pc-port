# Record one scene of the game as a GS recording (PS2_GS_RECORD, runtime/gs_record.c) for the GPU
# renderer corpus (tests/gpu/corpus.txt, notes/d3d11-renderer.md 10.4), with the live frames dumped
# over the same VBlanks so the recording can be checked against them (gs_replay.ps1 +
# compare_frames.py).
#   .\scripts\gs_record_scene.ps1 -Name ep1_hub -Episode 1 -Play -Pad "+2400:lup:120" -From 4200 -Frames 150
#   .\scripts\gs_record_scene.ps1 -Name m1_brawl -Episode 1 -Job m1_brawl -Direct -From 3000 -Frames 150
#   .\scripts\gs_record_scene.ps1 -Name scout -Episode 2 -Seconds 200 -DumpEvery 60      # no -From: scouting only
# Like scripts\warp_run.ps1 (same card, same START anchor, -Play/-Direct/-Job/-Set/-Make), but:
#  - the exe is copied to build\gsrec\<Name>\sly2_gpuC_<Name>.exe (a unique process name: other agents'
#    games run in parallel and nobody kills processes by name);
#  - -From/-Frames record VBlanks [From, From+Frames) into build\gsrec\<Name>.gsrec and dump every
#    -RecDumpEvery-th VBlank of that window as frames\frame_<vblank>.png (outside it every -DumpEvery-th);
#  - a "rec" token in -Pad (e.g. "+600:rec", relative to the token before it) starts the recording
#    instead of -From; frames are then dumped from the recording's start (PS2_DUMP_FROM=rec);
#  - -Live: steer the running game by appending PS2_INPUT lines to build\gsrec\<Name>\input.txt
#    (e.g. "+0:r3:10", then "+0:rec" when the scene is right); every -RecDumpEvery-th VBlank is dumped
#    all along (frames\latest.png to look at), the oldest deleted after -Ring dumps;
#  - -NoCard runs without a warp card (a fresh card directory: new game, no save).
# The run stops by itself (PS2_WATCHDOG) -Seconds after the start, or soon after the recording ends.
param([Parameter(Mandatory = $true)][string]$Name, [int]$Episode = 0, [string]$Job = "", [switch]$Direct,
      [switch]$Play, [switch]$NoCard, [switch]$NoStart, [int]$Seconds = 300, [switch]$DebugBuild, [string]$Pad = "",
      [int]$From = 0, [int]$Frames = 120, [int]$DumpEvery = 60, [int]$RecDumpEvery = 10, [switch]$Live, [int]$Ring = 300,
      [string]$Set = "", [string]$Make = "", [switch]$KeepRunning)
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'orig\SCUS_973.16'))) {   # worktree: find the main checkout
    $gitdir = ((Get-Content (Join-Path $root '.git') -Raw) -replace '^gitdir:\s*', '').Trim()
    $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
}
$py = Join-Path $main '.venv\Scripts\python.exe'
if ($Direct -and -not $Job) { "-Direct needs -Job"; exit 1 }
$dir = Join-Path $root "build\gsrec\$Name"
$card = Join-Path $dir 'card'
$fdir = Join-Path $dir 'frames'
New-Item -ItemType Directory -Force $dir, $fdir | Out-Null
Remove-Item (Join-Path $fdir '*.png') -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force (Join-Path $dir 'rec_frames') -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $card -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $card | Out-Null

Remove-Item Env:\PS2_WARP_TASK -ErrorAction SilentlyContinue
if (-not $NoCard) {
    $mk = @('scripts\warp.py', 'make', '--episode', "$Episode", '--card', $card)
    if ($Job) { $mk += @('--job', $Job) }
    if ($Direct) { $mk += '--direct' }
    foreach ($s in ($Set -split ',' | Where-Object { $_ })) { $mk += @('--set', $s.Trim()) }
    if ($Make) { $mk += ($Make -split '\s+' | Where-Object { $_ }) }
    Push-Location $root
    $made = & $py @mk
    $ok = $LASTEXITCODE -eq 0
    Pop-Location
    $made
    if (-not $ok) { exit 1 }
    if ($Direct) {
        $w = $made | Select-String 'PS2_WARP_TASK=(\S+)' | Select-Object -First 1
        if (-not $w) { "warp.py printed no PS2_WARP_TASK"; exit 1 }
        $env:PS2_WARP_TASK = $w.Matches[0].Groups[1].Value
    }
}

$build = Join-Path $root $(if ($DebugBuild) { 'build\sly2' } else { 'build\rel' })
$exe = Join-Path $dir "sly2_gpuC_$Name.exe"
Copy-Item (Join-Path $build 'sly2.exe') $exe -Force
$script = if ($NoStart) { '' } else { '@187D80/1DD96C+30:start:10' }
if ($Play -and -not $Direct -and $Episode -gt 0) { $script += ',+1500:cross:6' }
if ($Pad) { $script = if ($script) { "$script,$Pad" } else { $Pad } }

$rec = Join-Path $root "build\gsrec\$Name.gsrec"
Remove-Item Env:\PS2_GS_RECORD, Env:\PS2_GS_RECORD_FROM, Env:\PS2_GS_RECORD_FRAMES, Env:\PS2_DUMP_FROM,
            Env:\PS2_INPUT_FILE, Env:\PS2_DUMP_RING -ErrorAction SilentlyContinue
$every = $DumpEvery
$token = $script -match ':rec(\b|$)'
$recording = $From -gt 0 -or $token -or $Live
if ($recording) {
    Remove-Item $rec -ErrorAction SilentlyContinue
    $env:PS2_GS_RECORD = $rec
    $env:PS2_GS_RECORD_FROM = if ($From -gt 0) { "$From" } else { '-1' }   # -1: only the "rec" token
    $env:PS2_GS_RECORD_FRAMES = "$Frames"
    $every = $RecDumpEvery
    if ($From -gt 0) { $env:PS2_DUMP_FROM = "$From" } elseif (-not $Live) { $env:PS2_DUMP_FROM = 'rec' }
}
if ($Live) {
    $inp = Join-Path $dir 'input.txt'
    Set-Content -Path $inp -Value '' -Encoding ascii
    $env:PS2_INPUT_FILE = $inp
    $env:PS2_DUMP_RING = "$Ring"
    "live input: append lines to $inp"
}
$env:PS2_WATCHDOG = "$Seconds"
$env:PS2_HEADLESS = "1"
$env:PS2_AUDIO_NULL = "1"
Remove-Item Env:\PS2_AUDIO_WAV -ErrorAction SilentlyContinue
$env:PS2_INPUT = $script
$env:PS2_MEMCARD_DIR = $card
$env:PS2_DUMP_FRAMES = $fdir
$env:PS2_DUMP_EVERY = "$every"
$iso = Get-ChildItem (Join-Path $main 'orig\disc') -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1
if ($iso) { $env:PS2_ISO = $iso.FullName }
$log = Join-Path $dir 'run.log'
"input: $script"
if ($From -gt 0) { "recording VBlanks $From..$($From + $Frames - 1) -> $rec" }
$p = Start-Process -FilePath $exe -ArgumentList (Join-Path $main 'orig\SCUS_973.16') `
    -RedirectStandardError $log -RedirectStandardOutput (Join-Path $dir 'run.out') -PassThru -NoNewWindow
$t0 = Get-Date
$done = $false
$recdir = Join-Path $dir 'rec_frames'          # the live frames of the recorded VBlanks, kept apart
function Save-RecFrames {                      # (-Live's dump ring would delete them later)
    $m = Select-String -Path $log -Pattern 'recording the GS stream from VBlank (\d+) for (\d+) VBlanks' | Select-Object -First 1
    if (-not $m) { return }
    $v0 = [int]$m.Matches[0].Groups[1].Value; $nv = [int]$m.Matches[0].Groups[2].Value
    New-Item -ItemType Directory -Force $recdir | Out-Null
    Get-ChildItem $fdir -Filter 'frame_*.png' | Where-Object {
        $v = [int]($_.BaseName.Substring(6)); $v -ge $v0 -and $v -lt $v0 + $nv } | Copy-Item -Destination $recdir
    "-- live frames of VBlanks $v0..$($v0 + $nv - 1) -> $recdir ($(@(Get-ChildItem $recdir -Filter 'frame_*.png').Count) frames)"
}
while (-not $p.HasExited) {
    Start-Sleep -Seconds 2
    if (((Get-Date) - $t0).TotalSeconds -gt $Seconds + 20) { Stop-Process -Id $p.Id -Force; "-- killed (watchdog didn't fire)"; break }
    if ($recording -and -not $done -and (Select-String -Path $log -Pattern 'GS recording complete' -Quiet)) {
        $done = $true
        Start-Sleep -Seconds 3                      # the last dumps are written by the GS thread
        Save-RecFrames
        if (-not $KeepRunning) {
            Stop-Process -Id $p.Id -Force
            "-- recording complete, stopped"
            break
        }
    }
}
if ($recording -and -not $done -and (Select-String -Path $log -Pattern 'GS recording complete' -Quiet)) { Save-RecFrames }
"-- ran $([int]((Get-Date) - $t0).TotalSeconds) s"
Remove-Item Env:\PS2_GS_RECORD, Env:\PS2_GS_RECORD_FROM, Env:\PS2_GS_RECORD_FRAMES, Env:\PS2_DUMP_FROM, Env:\PS2_WARP_TASK,
            Env:\PS2_INPUT_FILE, Env:\PS2_DUMP_RING -ErrorAction SilentlyContinue
Select-String -Path $log -Pattern '\[warp\]|\[gs\]|not implemented|unhandled|error|assert|crash|exception|fatal' |
    Where-Object { $_.Line -notmatch 'syscall 0x5B' } | Select-Object -First 30 | ForEach-Object { $_.Line }
if (Test-Path $rec) { "recording: $rec ($([int]((Get-Item $rec).Length / 1MB)) MB)" }
$n = @(Get-ChildItem $fdir -Filter 'frame_*.png').Count
"frames: $fdir ($n dumps; latest.png)"
