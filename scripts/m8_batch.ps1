# Run scenes of scripts\m8_scenes.txt through scripts\m8_sweep.ps1 one after another (M8 in-game sweep).
#   powershell -ExecutionPolicy Bypass -File scripts\m8_batch.ps1 [-Only p0_chase,ep1_hub] [-Part 0 -Parts 2] [-Verify 60]
#       [-Scale 3] [-Sw] [-Suffix _perf] [-DumpFrom 3000]
# -Part/-Parts: every Parts-th scene starting at Part (two batches in parallel = two game instances).
# One line per scene (the summary's verify / PUNT / stats lines) in build\warp\m8_batch_<part>.txt.
param([string[]]$Only = @(), [int]$Part = 0, [int]$Parts = 1, [int]$Verify = 60, [int]$Scale = 3, [switch]$Sw,
      [string]$Suffix = "", [int]$DumpFrom = 3000, [int]$DumpEvery = 600, [string]$Extra = "", [int]$StatsFrom = 0,
      [int]$Seconds = 0)
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path $PSScriptRoot -Parent
$GP = '+2600:lup:90,+90:rright:60,+60:lup:60,+60:cross:6,+30:square:6,+30:lleft:60,+200:lup:120,+120:rleft:80,+100:cross:6,+300:lup:200,+200:circle:6,+300:lup:150,+150:square:6'
$HP = '+500:lup:90,+90:rright:60,+60:lup:60,+60:cross:6,+30:lleft:60,+200:lup:120,+120:rleft:80,+300:lup:200,+200:square:6,+300:lup:150'
$scenes = Get-Content (Join-Path $PSScriptRoot 'm8_scenes.txt') | Where-Object { $_ -and -not $_.StartsWith('#') }
$out = Join-Path $root "build\warp\m8_batch_$Part$Suffix.txt"
$i = -1
foreach ($line in $scenes) {
    $f = @($line -split '\|' | ForEach-Object { $_.Trim() })
    $name = $f[0]
    if ($Only.Count) { if (-not ($Only -contains $name)) { continue } }
    else { $i++; if ($i % $Parts -ne $Part) { continue } }
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'm8_sweep.ps1'), '-Name', "$name$Suffix",
           '-Episode', $f[1], '-Seconds', $(if ($Seconds -gt 0) { "$Seconds" } else { $f[4] }), '-Verify', "$Verify", '-Scale', "$Scale",
           '-DumpEvery', "$DumpEvery", '-StatsFrom', "$StatsFrom")
    if ($f[2]) { $a += @('-Job', $f[2]) }
    $envx = @()
    foreach ($fl in ($f[3] -split ' ' | Where-Object { $_ })) {
        if ($fl -eq 'direct') { $a += '-Direct' }
        elseif ($fl -eq 'play') { $a += '-Play' }
        elseif ($fl -like 'set=*') { $a += @('-Set', $fl.Substring(4)) }
        elseif ($fl -like 'make=*') { $a += @('-Make', ($fl.Substring(5) -replace '~', ' ')) }
        elseif ($fl -like 'env=*') { $envx += @($fl.Substring(4)) }
    }
    $pad = $f[5] -replace '^GP$', $GP -replace '^HP$', $HP
    if ($pad) { $a += @('-Pad', $pad) }
    if ($Sw) { $a += '-Sw' }
    $ex = @("PS2_DUMP_FROM=$DumpFrom") + $envx + @($Extra -split ';' | Where-Object { $_ })
    $a += @('-Extra', ($ex -join ';'))
    $res = & powershell @a
    $res | Select-Object -First 12 | ForEach-Object { "$name$Suffix | $_" } | Add-Content $out
    "$name$Suffix done: $($res | Select-Object -Skip 2 -First 1)"
}
