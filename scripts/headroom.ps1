# Headroom test: run the game with extra busy work per VBlank and see whether it keeps 59.94 VBlanks
# per second and its own frame rate (flips per second: the game drops frames when one is late).
# The largest burn that changes neither is the margin left on that thread.
#   powershell -ExecutionPolicy Bypass -File scripts\headroom.ps1 [-Burns 0,2,4,6] [-Gs] [-Seconds 60]
#       [-Pad "600:square:12,2100:start:10"] [-Out build\rel] [-From <VBlank>] [-To <VBlank>]
# -Gs burns on the GS thread (per composed picture) instead of the EE thread (per VBlank), see
# PS2_PERF_BURN / PS2_PERF_BURN_GS in runtime/perf.c. Prints, per 5-second window of the run, the
# VBlanks serviced as a share of those due, the game's flips per second and VU pairs per VBlank (a
# fingerprint of the scene: a slowed run can take the scripted inputs in another game state, and
# its later windows are then not comparable) for every burn level, and the busy times of the first
# run. -From/-To burn only in that VBlank range (PS2_PERF_BURN_FROM/_TO), so every run reaches the
# measured scene in the same state; windows inside the range are marked with *. Scenes with the
# default -Pad: memory card prompt ~VBlank 350-600, title ~1700-2100, briefing from ~2600.
# Use a release build (scripts\build_game.py --release --out build\rel).
param([string]$Burns = "0,2,4,6", [switch]$Gs, [int]$Seconds = 60,
      [string]$Pad = "600:square:12,2100:start:10", [string]$Out = "", [int]$Window = 5,
      [int]$From = 0, [int]$To = 0)
$root = Split-Path $PSScriptRoot -Parent
$BurnList = @($Burns -split '[ ,]+' | Where-Object { $_ } | ForEach-Object { [double]::Parse($_, [Globalization.CultureInfo]::InvariantCulture) })
if (-not $Out) { $Out = Join-Path $root 'build\rel' }
$var = if ($Gs) { 'PS2_PERF_BURN_GS' } else { 'PS2_PERF_BURN' }
$results = @{}
foreach ($b in $BurnList) {
    $env:PS2_PERF = '1'
    $env:PS2_INPUT = $Pad
    foreach ($n in 'PS2_PERF_BURN', 'PS2_PERF_BURN_GS', 'PS2_PERF_BURN_FROM', 'PS2_PERF_BURN_TO') {
        [Environment]::SetEnvironmentVariable($n, $null)
    }
    [Environment]::SetEnvironmentVariable($var, "$b")
    if ($From) { $env:PS2_PERF_BURN_FROM = "$From" }
    if ($To) { $env:PS2_PERF_BURN_TO = "$To" }
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run_game.ps1') -Seconds $Seconds -Out $Out | Out-Null
    $win = @{}
    foreach ($line in Get-Content (Join-Path $Out 'run.log')) {
        if ($line -notmatch '^\[perf\]\s+([\d.]+)s vblanks (\d+)/(\d+) due.*flips (\d+).*busy ([\d.]+) ms/vblank(, GS thread ([\d.]+) ms/vblank)?(, VU (\d+)k pairs)?') { continue }
        $w = [int]([math]::Floor([double]$Matches[1] / $Window) * $Window)
        if (-not $win[$w]) { $win[$w] = @(0, 0, 0.0, 0.0, 0, 0, 0) }
        $win[$w][0] += [int]$Matches[2]; $win[$w][1] += [int]$Matches[3]
        $win[$w][2] += [double]$Matches[5]; if ($Matches[7]) { $win[$w][3] += [double]$Matches[7] }; $win[$w][4]++
        $win[$w][5] += [int]$Matches[4]; if ($Matches[9]) { $win[$w][6] += [int]$Matches[9] }
    }
    $results[$b] = $win
    Write-Host ("burn {0} ms ({1}): done" -f $b, $var)
}
$header = "window        " + (($BurnList | ForEach-Object { "{0,20}" -f ("burn " + $_) }) -join '') + "   busy EE / GS (ms per VBlank, burn $($BurnList[0]))"
Write-Host $header
Write-Host ("              " + (($BurnList | ForEach-Object { "{0,20}" -f "vb% flips/s VU" }) -join ''))
$windows = $results[$BurnList[0]].Keys | Sort-Object
foreach ($w in $windows) {
    $inside = ($From -or $To) -and (-not $From -or $w * 59.94 -ge $From) -and (-not $To -or ($w + $Window) * 59.94 -le $To)
    $row = "{0,3}-{1,3} s {2}  " -f $w, ($w + $Window), $(if ($inside) { '*' } else { ' ' })
    foreach ($b in $BurnList) {
        $v = $results[$b][$w]
        $row += if ($v -and $v[1]) { "{0,20}" -f ("{0:P0} {1,4:N1} {2,5:N0}k" -f ($v[0] / $v[1]), ($v[5] / $v[4]), ($v[6] / $v[4])) } else { "{0,20}" -f '-' }
    }
    $v = $results[$BurnList[0]][$w]
    if ($v[4]) { $row += "   {0,5:N1} / {1,5:N1}" -f ($v[2] / $v[4]), ($v[3] / $v[4]) }
    Write-Host $row
}
