# (r2-m2) The modern renderer's A/B sweep (notes/modern-renderer.md 5, levels 2-3): scripts\r2_ab.ps1 on every episode hub
# and a few jobs (bracket recording of one frame, replayed exact and hybrid, compared against tests\r2\gates.json), -Parallel
# at a time. Prints one line per scene: entries bracketed, the gate verdict and the S = 1 numbers of the main frame buffer.
#   .\scripts\r2_sweep.ps1 [-Scenes ep1,ep3] [-Parallel 3] [-Keep]
# Output: build\r2ab\<scene>\ (deleted unless -Keep: ~70 MB per scene).
param([string[]]$Scenes = @(), [int]$Parallel = 3, [switch]$Keep)
$Scenes = @($Scenes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path $PSScriptRoot -Parent
$list = @(
    @{ n = 'ep1'; a = @('-Episode', '1', '-Job', 'm1_brawl', '-Play', '-At', '4800') },
    @{ n = 'ep2'; a = @('-Episode', '2', '-Play', '-At', '4800') },
    @{ n = 'ep3'; a = @('-Episode', '3', '-Play', '-At', '5000') },
    @{ n = 'ep4'; a = @('-Episode', '4', '-Play', '-At', '4800') },
    @{ n = 'ep5'; a = @('-Episode', '5', '-Play', '-At', '4800') },
    @{ n = 'ep6'; a = @('-Episode', '6', '-Play', '-At', '4800') },
    @{ n = 'ep7'; a = @('-Episode', '7', '-Play', '-At', '4800') },
    @{ n = 'ep8'; a = @('-Episode', '8', '-Play', '-At', '4800') },
    @{ n = 'm2_boardroom'; a = @('-Episode', '2', '-Job', 'm2_boardroom_brawl', '-Direct', '-At', '5400') },
    @{ n = 'm3_boss'; a = @('-Episode', '3', '-Job', 'm3_boss', '-Direct', '-At', '6600') },
    @{ n = 'm8_boss'; a = @('-Episode', '8', '-Job', 'm8_boss', '-Direct', '-At', '5200', '-Pad', '+2600:lup:90') }
)
if ($Scenes.Count) { $list = @($list | Where-Object { $Scenes -contains $_.n }) }
$free = [math]::Round((Get-PSDrive C).Free / 1GB, 1)
if ($free -lt 50) { "C: has $free GB free (< 50): not starting"; exit 1 }
$logs = Join-Path $root 'build\r2ab\sweep'
New-Item -ItemType Directory -Force $logs | Out-Null
$queue = [System.Collections.ArrayList]@($list)
$running = @()
$fail = 0
while ($queue.Count -or $running.Count) {
    while ($queue.Count -and $running.Count -lt $Parallel) {
        $s = $queue[0]; $queue.RemoveAt(0)
        $log = Join-Path $logs "$($s.n).txt"
        $args1 = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'r2_ab.ps1'), '-Name', "sw_$($s.n)", '-Levels', 'all') + $s.a
        $p = Start-Process -FilePath 'powershell' -ArgumentList $args1 -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru -NoNewWindow
        $running += [pscustomobject]@{ p = $p; s = $s; log = $log }
    }
    Start-Sleep -Seconds 5
    $still = @()
    foreach ($r in $running) {
        if (-not $r.p.HasExited) { $still += $r; continue }
        $t = Get-Content $r.log -ErrorAction SilentlyContinue
        $g = ($t | Select-String '^gates:' | Select-Object -Last 1)
        $f = ($t | Select-String 'frame 0x38: ' | Select-Object -Last 1)
        $z = ($t | Select-String ' Z 0x70: ' | Select-Object -Last 1)
        $tr = Get-ChildItem (Join-Path $root "build\r2trace\ab_sw_$($r.s.n)") -Filter 'frame_0.txt' -ErrorAction SilentlyContinue
        $ent = if ($tr) { (Get-Content $tr.FullName -TotalCount 1) -replace '^.*entries (\d+).*$', '$1' } else { '?' }
        $gl = if ($g) { $g.Line } else { 'gates: (no result)' }
        if ($gl -notmatch 'PASS') { $fail++ }
        "{0,-13} entries {1,4} | {2} | {3} | {4}" -f $r.s.n, $ent, $gl, $(if ($f) { $f.Line -replace '^v\d+ frame 0x38: ', '' -replace ';.*alpha bit', '; alpha bit' } else { '' }),
            $(if ($z) { $z.Line -replace '^v\d+ ', '' } else { '' })
        if (-not $Keep) {
            Remove-Item -Recurse -Force (Join-Path $root "build\r2ab\sw_$($r.s.n)"), (Join-Path $root "build\r2trace\ab_sw_$($r.s.n)") -ErrorAction SilentlyContinue
            Remove-Item -Recurse -Force (Join-Path $root "build\warp\r2_ab_sw_$($r.s.n)\frames") -ErrorAction SilentlyContinue
        }
    }
    $running = $still
}
"sweep: $($list.Count - $fail) of $($list.Count) scenes pass"
exit $(if ($fail) { 1 } else { 0 })
