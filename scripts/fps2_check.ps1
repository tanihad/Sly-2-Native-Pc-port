# Correctness sweep of the graphics worker / raw PATH1 packets / VIF unpack fast paths (pc-fps2): scenes run in parallel
# (-Parallel at a time) with the in-game checks on - PS2_GFX_ASYNC_CHECK (the worker reads only memory as it was at the
# kick), PS2_GIF_CHECK (raw packets parse like gif.c), PS2_VIF_UPK_FAST=check (fast unpack == generic) - and frame dumps
# every -DumpSecs s. Prints per scene: the checks' last counts, errors, and whether the game kept running.
#   .\scripts\fps2_check.ps1 [-Rate 120] [-Scenes "2|m2_turret|direct|GP",...] [-Seconds 100] [-Parallel 4] [-Exe <exe>]
param([string]$Rate = "120", [string[]]$Scenes = @(), [int]$Seconds = 100, [int]$Parallel = 4, [string]$Exe = "",
      [string]$Tag = "chk", [int]$DumpSecs = 20, [string]$Extra = "")
$root = Split-Path $PSScriptRoot -Parent
if (-not $Scenes.Count) {
    $Scenes = @('2||play|HPT', '2|m2_turret|direct|GP', '2|m2_dance_audition|direct|GP', '2|m2_rc_chopper|direct|GP',
                '5|t5_heist_free_carmelita_hack|direct|GP', '6|m6_paraglide|direct|GP', '7|m7_boss|direct|GP', '4|m4_brawl|direct|GP')
}
$jobs = @()
$i = 0
foreach ($s in $Scenes) {
    while (@($jobs | Where-Object { $_.State -eq 'Running' }).Count -ge $Parallel) { Start-Sleep 5 }
    $name = ($s -split '\|')[1]; if (-not $name) { $name = "ep$(($s -split '\|')[0])" }
    $ex = "PS2_GFX_ASYNC_CHECK=5;PS2_GIF_CHECK=1;PS2_VIF_UPK_FAST=check;PS2_DUMP_SECS=$DumpSecs"
    if ($Extra) { $ex += ";$Extra" }
    $jobs += Start-Job -ArgumentList $root, $s, $Rate, $Seconds, $Exe, "_$Tag$i", $ex -ScriptBlock {
        param($root, $s, $Rate, $Seconds, $Exe, $tag, $ex)
        Set-Location $root
        $a = @('-ExecutionPolicy', 'Bypass', '-File', 'scripts\fps2_scene.ps1', '-Scene', $s, '-Rate', $Rate, '-Seconds', "$Seconds",
               '-From', "$($Seconds - 30)", '-Scale', '1', '-Tag', $tag, '-Keep', '-Extra', $ex)
        if ($Exe) { $a += @('-Exe', $Exe) }
        & powershell @a
    }
    $i++
}
$jobs | Wait-Job | Out-Null
$i = 0
foreach ($s in $Scenes) {
    $dir = Get-ChildItem (Join-Path $root 'build\warp') -Directory -Filter "fps2_*_$Rate`_$Tag$i" | Select-Object -First 1
    $i++
    if (-not $dir) { "$s : no run directory"; continue }
    $log = Join-Path $dir.FullName 'run.log'
    $l = Get-Content $log
    $g = ($l | Select-String 'PS2_GFX_ASYNC_CHECK: \d+ jobs' | Select-Object -Last 1).Line
    $p = ($l | Select-String 'PS2_GIF_CHECK: \d+' | Select-Object -Last 1).Line
    $v = ($l | Select-String 'PS2_VIF_UPK_FAST=check: \d+' | Select-Object -Last 1).Line
    $bad = @($l | Select-String 'differ\b(?!.*, 0 differ)|changed after the kick|exception|assert|fatal|crash' | Where-Object { $_.Line -notmatch ' 0 differ' })
    $perf = ($l | Select-String '\[perf\]' | Select-Object -Last 1).Line
    $fps = if ($perf -match 'flips \d+ \(([\d.]+)/s\)') { $Matches[1] } else { '?' }
    "{0} : fps at the end {1} | {2} | {3} | {4} | problems: {5}" -f $s, $fps, ($g -replace '^.*CHECK: ', ''), ($p -replace '^.*CHECK: ', ''),
        ($v -replace '^.*check: ', ''), $bad.Count
    $bad | Select-Object -First 3 | ForEach-Object { "    " + $_.Line }
}
