# (r2-m2) GPU cost of the modern renderer on a fixed stream: records <Frames> VBlanks of a scene in PS2_R2=bracket mode
# (scripts\warp_run.ps1, release build: GS recording + one EE RAM dump for the meshes), then replays the recording at
# -Scale through the game's GPU path with PS2_GPU_PROF=1 (per-kind GPU ms per frame), alternately exact (--r2-ab 0) and
# hybrid (--r2-ab 1), -Repeat times each, and prints the "prof:" averages. Deterministic: same stream, same frames.
#   .\scripts\r2_prof_ab.ps1 -Name ep1 -Episode 1 -Job m1_brawl -At 5000 -Pad "<pad>" [-Frames 40] [-Scale 3] [-NoRecord]
# Output: build\r2prof\<Name>\ (recording, RAM dump: delete when done).
param([Parameter(Mandatory = $true)][string]$Name, [int]$Episode = 1, [string]$Job = "", [switch]$Direct, [switch]$Play,
      [int]$At = 5000, [string]$Pad = "", [int]$Frames = 40, [int]$Scale = 3, [int]$Repeat = 2, [switch]$NoRecord,
      [string]$Extra = "")
$root = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $root "build\r2prof\$Name"
New-Item -ItemType Directory -Force $dir | Out-Null
$rec = Join-Path $dir 'frame.gsrec'
if (-not $NoRecord) {
    Remove-Item (Join-Path $dir '*') -ErrorAction SilentlyContinue
    $env:PS2_R2 = 'bracket'; $env:PS2_R2_LEVELS = 'all'
    $env:PS2_R2_TRACE = "${At}:1:$dir"
    $env:PS2_GS_RECORD = $rec; $env:PS2_GS_RECORD_FROM = "$At"; $env:PS2_GS_RECORD_FRAMES = "$Frames"
    $wa = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'warp_run.ps1'), '-Episode', "$Episode", '-Release',
            '-Seconds', "$([int]($At / 60) + 30)", '-DumpEvery', '100000', '-Name', "r2prof_$Name")
    if ($Job) { $wa += @('-Job', $Job) }
    if ($Direct) { $wa += '-Direct' }
    if ($Play) { $wa += '-Play' }
    if ($Pad) { $wa += @('-Pad', $Pad) }
    & powershell @wa | Out-Null
    Remove-Item Env:\PS2_R2, Env:\PS2_R2_LEVELS, Env:\PS2_R2_TRACE, Env:\PS2_GS_RECORD, Env:\PS2_GS_RECORD_FROM,
                Env:\PS2_GS_RECORD_FRAMES -ErrorAction SilentlyContinue
    $w = Join-Path $root "build\warp\r2prof_$Name"
    Remove-Item (Join-Path $w 'sly2_warp_*.exe') -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $w 'frames\*.png') -ErrorAction SilentlyContinue
}
$ram = Join-Path $dir 'ram_0.bin'
if (-not (Test-Path $rec) -or -not (Test-Path $ram)) { "no recording in $dir"; exit 1 }
$o = Join-Path $dir 'replay'
New-Item -ItemType Directory -Force $o | Out-Null
& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'gs_replay.ps1') -BuildOnly -Out $o | Out-Null
$exe = Join-Path $o 'gs_replay.exe'
$run = Join-Path $o ("gs_replay_prof_{0}.exe" -f $PID)
Copy-Item $exe $run -Force
$env:PS2_GPU_PROF = '1'; $env:PS2_GPU_PROF_EVERY = "$([math]::Max(1, $Frames - 6))"
foreach ($kv in ($Extra -split ';' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; [Environment]::SetEnvironmentVariable($k, $v) }
for ($r = 0; $r -lt $Repeat; $r++) {
    foreach ($ab in 0, 1) {
        $args2 = @($rec, $o, '0', '--backend', 'hw', '--game', '--r2-ram', $ram, '--r2-ab', "$ab", '--scale', "$Scale")
        $lines = & $run @args2 2>&1 | ForEach-Object { "$_" } | Select-String 'prof:'
        $l = $lines | Select-Object -Last 1
        "[$(@('exact', 'hybrid')[$ab]) #$r] $(if ($l) { $l.Line -replace '^.*prof: ', '' } else { 'no prof line' })"
    }
}
Remove-Item Env:\PS2_GPU_PROF, Env:\PS2_GPU_PROF_EVERY -ErrorAction SilentlyContinue
Remove-Item $run -ErrorAction SilentlyContinue
