# Run scripts\fps2_scene.ps1 over scenes x rates x builds one after another (alone on the machine: fps measurements;
# builds interleaved per scene and rate) and collect the summary lines in build\warp\fps2_<Tag>.txt.
#   .\scripts\fps2_batch.ps1 -Tag cmp -Exes "base=build\base\sly2_base.exe,new=build\proto\sly2_new.exe"
#                            [-Scenes ep1_hub,ep3_hub] [-Rates 120,165] [-Extra ...] [-Seconds 95 -From 60]
param([string]$Tag = "run", [string]$Exes = "", [string[]]$Scenes = @('ep1_hub', 'ep3_hub', 'ep5_hub', 'm8_boss', 'm3_heist'),
      [string[]]$Rates = @('120', '165'), [int]$Seconds = 95, [int]$From = 60, [int]$Scale = 3, [string]$Extra = "")
$Scenes = @($Scenes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Rates = @($Rates | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root "build\warp\fps2_$Tag.txt"
$builds = @()
foreach ($e in ($Exes -split ',' | Where-Object { $_ })) {
    $n, $p = $e -split '=', 2
    $builds += [pscustomobject]@{ name = $n; exe = (Resolve-Path (Join-Path $root $p)).Path }
}
if (-not $builds.Count) { $builds = @([pscustomobject]@{ name = $Tag; exe = '' }) }
foreach ($s in $Scenes) {
    foreach ($r in $Rates) {
        foreach ($b in $builds) {
            $a = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'fps2_scene.ps1'), '-Scene', $s, '-Rate', $r,
                   '-Seconds', "$Seconds", '-From', "$From", '-Scale', "$Scale", '-Tag', "_$($b.name)")
            if ($b.exe) { $a += @('-Exe', $b.exe) }
            if ($Extra) { $a += @('-Extra', $Extra) }
            $res = & powershell @a
            $res | Add-Content $out
            $res
        }
    }
}
