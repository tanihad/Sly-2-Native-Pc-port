# A/B of the VU microcode: interpreter (PS2_VU_INTERP=1) against the recompiled code, interleaved runs
# of the same build and input (the machine is shared, so single runs are noisy).
#   powershell -ExecutionPolicy Bypass -File scripts\vu_ab.ps1 [-Runs 2] [-Seconds 60] [-Out build\rel]
#       [-Pad "600:square:12,2100:start:10"] [-Window 5]
# Every run starts with an empty memory card (fresh copy of the exe in build\ab_<mode>). Prints per
# window of the run (averaged over the runs of each mode): VBlanks serviced %, game flips/s, EE thread
# busy ms per VBlank, GS thread ms per VBlank, VU ns per pair and VU pairs per VBlank (the scene's
# fingerprint: the game draws less when its frame gets tight). Use a release build.
param([int]$Runs = 2, [int]$Seconds = 60, [string]$Out = "", [string]$Pad = "600:square:12,2100:start:10",
      [int]$Window = 5)
$root = Split-Path $PSScriptRoot -Parent
if (-not $Out) { $Out = Join-Path $root 'build\rel' }
$modes = @('interp', 'compiled')
$res = @{ interp = @{}; compiled = @{} }
for ($r = 0; $r -lt $Runs; $r++) {
    foreach ($m in $modes) {
        $dir = Join-Path $root "build\ab_$m"
        if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
        New-Item -ItemType Directory -Force $dir | Out-Null
        Copy-Item (Join-Path $Out 'sly2.exe') (Join-Path $dir 'sly2.exe')
        $env:PS2_PERF = '1'
        $env:PS2_VU_INTERP = $(if ($m -eq 'interp') { '1' } else { '0' })
        & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run_game.ps1') -Seconds $Seconds -Out $dir -Pad $Pad | Out-Null
        foreach ($line in Get-Content (Join-Path $dir 'run.log')) {
            if ($line -notmatch '^\[perf\]\s+([\d.]+)s vblanks (\d+)/(\d+) due.*flips (\d+).*busy ([\d.]+) ms/vblank(, GS thread ([\d.]+) ms/vblank)?(, VU (\d+)k pairs in \d+ programs/vblank \(([\d.]+) ns per pair)?') { continue }
            $w = [int]([math]::Floor([double]$Matches[1] / $Window) * $Window)
            if (-not $res[$m][$w]) { $res[$m][$w] = @(0, 0, 0.0, 0.0, 0, 0, 0.0, 0.0, 0) }
            $v = $res[$m][$w]
            $v[0] += [int]$Matches[2]; $v[1] += [int]$Matches[3]; $v[2] += [double]$Matches[5]
            if ($Matches[7]) { $v[3] += [double]$Matches[7] }
            $v[4]++; $v[5] += [int]$Matches[4]
            if ($Matches[9]) { $v[6] += [double]$Matches[9]; $v[7] += [double]$Matches[10]; $v[8]++ }
        }
        Write-Host "run $($r + 1) $m done"
    }
}
$env:PS2_VU_INTERP = $null
Write-Host ("{0,-10} {1,47} {2,47}" -f 'window', 'interpreter', 'recompiled')
Write-Host ("{0,-10} {1,47} {1,47}" -f '', 'vb%  flips/s   EE ms   GS ms  ns/pair  kpairs')
foreach ($w in ($res['interp'].Keys | Sort-Object)) {
    $row = "{0,3}-{1,3} s  " -f $w, ($w + $Window)
    foreach ($m in $modes) {
        $v = $res[$m][$w]
        if (-not $v -or -not $v[4]) { $row += "{0,47}" -f '-'; continue }
        $vu = if ($v[8]) { "{0,8:N1} {1,7:N0}" -f ($v[7] / $v[8]), ($v[6] / $v[8]) } else { "{0,8} {1,7}" -f '-', '-' }
        $row += "{0,6:P0} {1,7:N1} {2,7:N2} {3,7:N2} {4}" -f ($v[0] / $v[1]), ($v[5] / $v[4]), ($v[2] / $v[4]), ($v[3] / $v[4]), $vu
    }
    Write-Host $row
}
