# pc-research: GPU work per VBlank of gs_replay builds on corpus recordings (PS2_GPU_PROF=2: GPU ms per phase, pixel
# shader invocations, samples passing depth). PS invocations do not depend on other programs using the GPU; the ms do.
#   powershell -ExecutionPolicy Bypass -File scripts\gpu_prof_ab.ps1 -Exes "base=build\gsr_base\gs_replay.exe,ez=build\gsr_ez\gs_replay.exe"
#       [-Recs ep1_hub,ep3_hub] [-Scale 3] [-Aspect 16:9] [-Rounds 2]
param([Parameter(Mandatory = $true)][string]$Exes, [string[]]$Recs = @('ep1_hub', 'ep3_hub', 'ep5_street', 'ep8_boss', 'ep7_boss'),
      [int]$Scale = 3, [string]$Aspect = "16:9", [int]$Rounds = 1, [string]$Extra = "")
$Recs = @($Recs | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path $PSScriptRoot -Parent
$corpus = 'C:\Users\tanih\Projects\sly2-worktrees'
$map = @{ 'ep8_boss' = 'ep8\build\gsrec\boss.gsrec'; 'ep3_hub' = 'ep3\build\gsrec\hub.gsrec'; 'ep5_street' = 'ep5\build\gsrec\hub_street.gsrec';
          'ep1_hub' = 'gpu-C\build\gsrec\ep1_hub.gsrec'; 'ep7_boss' = 'ep7\build\boss.gsrec'; 'ep2_hub' = 'gpu-C\build\gsrec\ep2_hub.gsrec';
          'ws_ep3_hud' = 'gpu-M10\build\gsrec\ws_ep3_hud.gsrec'; 'load_m3' = 'gpu-C\build\gsrec\load_m3.gsrec' }
$builds = @()
foreach ($e in ($Exes -split ',')) { $n, $p = $e -split '=', 2; $src = (Resolve-Path (Join-Path $root $p)).Path
    $run = Join-Path (Split-Path $src -Parent) ("prof_ab_{0}_{1}.exe" -f $n, $PID); Copy-Item $src $run -Force
    $builds += [pscustomobject]@{ name = $n; exe = $run } }
foreach ($round in 1..$Rounds) {
    foreach ($r in $Recs) {
        foreach ($b in $builds) {
            $log = Join-Path $root ("build\prof_ab_{0}_{1}.log" -f $b.name, $PID)
            $env:PS2_GPU_PROF = '2'
            $a = @((Join-Path $corpus $map[$r]), '-', '0', '--backend', 'hw', '--adapter', 'hw', '--mixed', '--bench-skip', '10', '--scale', "$Scale", '--aspect', $Aspect)
            if ($Extra) { $a += ($Extra -split ' ') }
            & $b.exe @a 2> $log | Out-Null
            Remove-Item Env:PS2_GPU_PROF
            $t = (Get-Content $log) -join ' '
            $ms = if ($t -match 'prof: \d+ frames, GPU ms per frame ([\d.]+)(?!.*prof: \d+ frames)') { $Matches[1] } else { '?' }
            $all = [regex]::Matches($t, 'prof: \d+ frames, GPU ms per frame ([\d.]+)'); if ($all.Count) { $ms = $all[$all.Count - 1].Groups[1].Value }
            $st = [regex]::Matches($t, 'prof-stats per frame \(PS invocations M / samples passed M / primitives k\): (.*?)(mem-bench|\[gpu\]|$)')
            $stats = if ($st.Count) { $st[$st.Count - 1].Groups[1].Value } else { '' }
            $tot = 0.0; foreach ($m in [regex]::Matches($stats, '([\w-]+) ([\d.]+)/([\d.]+)/')) { $tot += [double]$m.Groups[2].Value }
            $z = if ($stats -match 'ff-tri-fast-z ([\d.]+)/([\d.]+)') { "tri-z PS $($Matches[1]) M (pass $($Matches[2]))" } else { '' }
            $ph = [regex]::Matches($t, 'prof: \d+ frames, GPU ms per frame [\d.]+: (.*?)\[gpu\] prof-stats')
            $phs = if ($ph.Count) { ($ph[$ph.Count - 1].Groups[1].Value -replace '\s+', ' ').Trim() } else { '' }
            "{0,-11} S{1} {2,-5} GPU {3} ms/VBlank | PS total {4:N2} M | {5}" -f $r, $Scale, $b.name, $ms, $tot, $z
            "      phases: $phs"
            Remove-Item $log -ErrorAction SilentlyContinue
        }
    }
}
foreach ($b in $builds) { Remove-Item $b.exe -ErrorAction SilentlyContinue }
