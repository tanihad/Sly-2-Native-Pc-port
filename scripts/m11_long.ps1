# M11 long-session check (notes/d3d11-renderer.md appendix M): one game process travels through every episode - hub ->
# job -> next episode's hub -> ... (PS2_TASK_SET level travel items "j<task>:<world>" / "w<world>", runtime/warp.c) - with
# the default settings (GPU renderer, internal_scale auto, 16:9, Modern), while the process's private bytes and working
# set are sampled every 10 s; PS2_GS_HW_STATS=1 adds the renderer's own memory lines (committed / video memory) every
# 600 VBlanks. Output: build\warp\<Name>\ (run.log, mem.csv, frames every -DumpEvery VBlanks).
#   powershell -ExecutionPolicy Bypass -File scripts\m11_long.ps1 [-Seconds 1320] [-Every 4000] [-Window] [-Name m11_long]
param([int]$Seconds = 1320, [int]$Every = 4000, [switch]$Window, [string]$Name = "m11_long", [int]$DumpEvery = 2000)
$root = Split-Path $PSScriptRoot -Parent
# the route: episode 1 hub (warp save, X at the safehouse), then every episode's hub and one of its jobs
$route = @('j1585:2', 'w8', 'j2013:8', 'w12', 'j2298:12', 'w14', 'j2445:14', 'w17', 'j2716:17', 'w27', 'j2868:27', 'w32',
           'j3278:37', 'w38', 'j3472:43', 'w2', 'j2283:12', 'w8', 'j2889:27', 'w17')
$items = @()
for ($i = 0; $i -lt $route.Count; $i++) {
    $vb = 6000 + $i * $Every
    if ($vb / 59.94 -gt $Seconds - 30) { break }
    $items += "$($route[$i])@$vb"
}
$env:PS2_TASK_SET = $items -join ','
$env:PS2_GS_HW_STATS = '1'
Write-Host "route: $env:PS2_TASK_SET"
$dir = Join-Path $root "build\warp\$Name"
$csv = Join-Path $dir 'mem.csv'
$runner = Start-Process -FilePath powershell -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
    (Join-Path $PSScriptRoot 'warp_run.ps1'), '-Episode', '1', '-Play', '-Release', '-Seconds', "$Seconds",
    '-DumpEvery', "$DumpEvery", '-Name', $Name) + $(if ($Window) { @('-Window') } else { @() }) -PassThru -NoNewWindow
Start-Sleep 15
"t_s,private_mb,working_set_mb" | Set-Content -Encoding ascii $csv
$t0 = Get-Date
$exeName = "sly2_$Name"
while (-not $runner.HasExited) {
    $p = Get-Process | Where-Object { $_.ProcessName -like "*$Name*" } | Select-Object -First 1
    if ($p) { "{0:F0},{1:F1},{2:F1}" -f ((Get-Date) - $t0).TotalSeconds, ($p.PrivateMemorySize64 / 1MB), ($p.WorkingSet64 / 1MB) | Add-Content -Encoding ascii $csv }
    Start-Sleep 10
}
Remove-Item Env:\PS2_TASK_SET, Env:\PS2_GS_HW_STATS -ErrorAction SilentlyContinue
$rows = Import-Csv $csv
if ($rows) {
    $pm = $rows | ForEach-Object { [double]$_.private_mb }
    Write-Host ("samples {0}, private MB first {1:F0} min {2:F0} max {3:F0} last {4:F0}" -f $pm.Count, $pm[0], ($pm | Measure -Minimum).Minimum, ($pm | Measure -Maximum).Maximum, $pm[-1])
}
