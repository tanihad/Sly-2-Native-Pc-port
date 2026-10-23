# GPU cost per internal scale at a given output size (pc-pacing): headless runs with the offscreen present target at
# -Size (default 2560x1440), the scale fixed (PS2_GS_SCALE: no governor), PS2_GS_HW_STATS. One line per scene and scale:
# GPU ms per game frame (drawing + compose; median and p90 of the per-second medians), the present's GPU ms (resample to
# the output), the load (GPU ms per VBlank picture / 16.68 ms) and the game speed.
#   powershell -ExecutionPolicy Bypass -File scripts\pace_cost.ps1 [-Scenes ep1,ep3,m7] [-Scales 1,2,3,4] [-Size 2560x1440]
param([string[]]$Scenes = @('ep1', 'ep3', 'm7'), [string[]]$Scales = @('1', '2', '3', '4'), [string]$Size = '2560x1440', [int]$Seconds = 125,
      [string]$Out = "")
$root = Split-Path $PSScriptRoot -Parent
$Scenes = @($Scenes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Scales = @($Scales | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$def = @{ ep1 = @('-Episode', '1', '-Play'); ep3 = @('-Episode', '3', '-Play'); m7 = @('-Episode', '7', '-Job', 'm7_boss', '-Direct') }
if (-not $Out) { $Out = Join-Path $root 'build\pace\cost.txt' }
foreach ($sc in $Scenes) {
    foreach ($n in $Scales) {
        $name = "cost_${sc}_${n}x"
        $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'pace_run.ps1'), '-Name', $name) + $def[$sc] +
             @('-Scale', 'env', '-Seconds', "$Seconds", '-From', '90', '-Extra', "PS2_GS_SCALE=$n;PS2_GS_PRESENT_SIZE=$Size")
        $res = & powershell @a
        $log = Join-Path $root "build\pace\$name\run.log"
        $g = @(Select-String -Path $log -Pattern '\| GPU ([\d.]+) / ([\d.]+) \|' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value } |
               Select-Object -Last 30 | Where-Object { $_ -gt 0 } | Sort-Object)
        $pg = @(Select-String -Path $log -Pattern 'GPU present ([\d.]+) ms' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value } | Select-Object -Last 30 | Sort-Object)
        $ld = @(Select-String -Path $log -Pattern 'load (\d+)%' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value } | Select-Object -Last 30 | Sort-Object)
        $others = @(Get-Process | Where-Object { $_.ProcessName -like 'sly2*' -and $_.ProcessName -ne "sly2_pace_$name" }).Count
        $line = "{0,-4} {1}x: GPU ms/frame median {2:N2} p90 {3:N2} | present {4:N2} ms | load median {5}% | {6} | other games running: {7}" -f $sc, $n,
            ($(if ($g.Count) { $g[[int]($g.Count / 2)] } else { 0 })), ($(if ($g.Count) { $g[[int]($g.Count * 0.9)] } else { 0 })),
            ($(if ($pg.Count) { $pg[[int]($pg.Count / 2)] } else { 0 })), ($(if ($ld.Count) { $ld[[int]($ld.Count / 2)] } else { 0 })),
            (($res | Select-Object -Skip 1 -First 2 | ForEach-Object { $_.Trim() }) -join '; '), $others
        $line
        Add-Content $Out $line
    }
}
