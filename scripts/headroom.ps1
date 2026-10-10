# Headroom test: run the game with extra busy work per VBlank and see whether it keeps 59.94 VBlanks
# per second. The largest burn that still does is the margin left on that thread.
#   powershell -ExecutionPolicy Bypass -File scripts\headroom.ps1 [-Burns 0,2,4,6] [-Gs] [-Seconds 60]
#       [-Pad "600:square:12,2100:start:10"] [-Out build\rel]
# -Gs burns on the GS thread (per composed picture) instead of the EE thread (per VBlank), see
# PS2_PERF_BURN / PS2_PERF_BURN_GS in runtime/perf.c. Prints, per 5-second window of the run, the
# VBlanks serviced as a share of those due for every burn level, and the busy times of the
# unburned run. Use a release build (scripts\build_game.py --release --out build\rel).
param([string]$Burns = "0,2,4,6", [switch]$Gs, [int]$Seconds = 60,
      [string]$Pad = "600:square:12,2100:start:10", [string]$Out = "", [int]$Window = 5)
$root = Split-Path $PSScriptRoot -Parent
$BurnList = @($Burns -split '[ ,]+' | Where-Object { $_ } | ForEach-Object { [double]::Parse($_, [Globalization.CultureInfo]::InvariantCulture) })
if (-not $Out) { $Out = Join-Path $root 'build\rel' }
$var = if ($Gs) { 'PS2_PERF_BURN_GS' } else { 'PS2_PERF_BURN' }
$results = @{}
$busy = @{}
foreach ($b in $BurnList) {
    $env:PS2_PERF = '1'
    $env:PS2_INPUT = $Pad
    Remove-Item Env:\PS2_PERF_BURN, Env:\PS2_PERF_BURN_GS -ErrorAction SilentlyContinue
    Set-Item "Env:\$var" "$b"
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run_game.ps1') -Seconds $Seconds -Out $Out | Out-Null
    $win = @{}
    foreach ($line in Get-Content (Join-Path $Out 'run.log')) {
        if ($line -notmatch '^\[perf\]\s+([\d.]+)s vblanks (\d+)/(\d+) due.*busy ([\d.]+) ms/vblank(, GS thread ([\d.]+) ms/vblank)?') { continue }
        $w = [int]([math]::Floor([double]$Matches[1] / $Window) * $Window)
        if (-not $win[$w]) { $win[$w] = @(0, 0, 0.0, 0.0, 0) }
        $win[$w][0] += [int]$Matches[2]; $win[$w][1] += [int]$Matches[3]
        $win[$w][2] += [double]$Matches[4]; if ($Matches[6]) { $win[$w][3] += [double]$Matches[6] }; $win[$w][4]++
    }
    $results[$b] = $win
    Write-Host ("burn {0} ms ({1}): done" -f $b, $var)
}
$header = "window      " + (($BurnList | ForEach-Object { "{0,10}" -f ("burn " + $_) }) -join '') + "   busy EE / GS (ms per VBlank, burn $($BurnList[0]))"
Write-Host $header
$windows = $results[$BurnList[0]].Keys | Sort-Object
foreach ($w in $windows) {
    $row = "{0,3}-{1,3} s   " -f $w, ($w + $Window)
    foreach ($b in $BurnList) {
        $v = $results[$b][$w]
        $row += if ($v -and $v[1]) { "{0,10}" -f ("{0:P1}" -f ($v[0] / $v[1])) } else { "{0,10}" -f '-' }
    }
    $v = $results[$BurnList[0]][$w]
    if ($v[4]) { $row += "   {0,5:N1} / {1,5:N1}" -f ($v[2] / $v[4]), ($v[3] / $v[4]) }
    Write-Host $row
}
