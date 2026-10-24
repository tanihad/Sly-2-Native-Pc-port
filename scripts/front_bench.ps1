# (perf-p3) Offline frontend benchmark: gs_replay --backend hwsubmit --mixed --front-thread --pin over recordings, builds
# interleaved (A B A B ...), N repeats; the thread-bench line per run (GS thread / frontend thread ms per game frame:
# median, p95, max) and the prepare pool's CPU, then medians over the repeats.
#   .\scripts\front_bench.ps1 -Exes "base=build\p3\gsr_base\gs_replay.exe,pool3=build\p3\gsr_new\gs_replay.exe|--front-pool 3"
#                             -Recs "ep8_boss=<path>,..." [-Repeat 3] [-Scale 1] [-Tag x]
# An exe entry "name=path|extra args" passes extra arguments to that build. -Chaos adds build\gsrec\chaos_*.gsrec.
param([Parameter(Mandatory = $true)][string]$Exes, [string]$Recs = "", [switch]$Chaos, [string]$Only = "", [int]$Repeat = 3,
      [int]$Scale = 1, [string]$Tag = "fb", [switch]$NoPin, [switch]$Hold)
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root "build\p3\bench_$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
$builds = @()
foreach ($e in ($Exes -split ',' | Where-Object { $_ })) {
    $n, $rest = $e -split '=', 2
    $p, $x, $ev = $rest -split '\|', 3
    $exe = Join-Path $out ("gsr_{0}_{1}.exe" -f $n, $PID)
    Copy-Item (Resolve-Path (Join-Path $root $p)).Path $exe -Force
    $pre = if ($Hold) { 'set "PS2_GPU_FTHREAD_HOLD=1"&& set "PS2_GPU_FTHREAD_RING_MB=2048"&& ' } else { '' }
    foreach ($kv in ("$ev" -split ';' | Where-Object { $_ })) { $pre += "set `"$kv`"&& " }
    $builds += [pscustomobject]@{ name = $n; exe = $exe; extra = "$x"; pre = $pre }
}
$list = @()
foreach ($r in ($Recs -split ',' | Where-Object { $_ })) { $n, $p = $r -split '=', 2; $list += [pscustomobject]@{ name = $n; path = $p } }
if ($Chaos) { $list += @(Get-ChildItem (Join-Path $root 'build\gsrec') -Filter 'chaos_*.gsrec' | Sort-Object Name | ForEach-Object { [pscustomobject]@{ name = $_.BaseName; path = $_.FullName } }) }
if ($Only) { $o = $Only -split ','; $list = @($list | Where-Object { $o -contains $_.name }) }
$res = @{}
for ($k = 0; $k -lt $Repeat; $k++) {
    foreach ($r in $list) {
        $order = if ($k % 2) { @($builds[($builds.Count - 1)..0]) } else { $builds }
        foreach ($b in $order) {
            $log = Join-Path $out "$($r.name).$($b.name).$k.log"
            $a = "`"$($r.path)`" - 0 $(if ($Hold) { '--backend hwdry --front-only' } else { '--backend hwsubmit' }) --adapter hw --mixed --front-thread --scale $Scale $(if (-not $NoPin) { '--pin' }) $($b.extra)"
            & cmd /c "$($b.pre)`"$($b.exe)`" $a 2>`"$log`""
            $tb = Get-Content $log | Where-Object { $_ -like 'thread-bench*' } | Select-Object -Last 1
            $pl = Get-Content $log | Where-Object { $_ -like 'prepare pool*' } | Select-Object -Last 1
            $ftl = Get-Content $log | Where-Object { $_ -like 'frontend thread CPU*' } | Select-Object -Last 1
            $ftm = [regex]::Match("$ftl", 'CPU ([\d.]+) ms')
            $ftot = if ($ftm.Success) { [double]$ftm.Groups[1].Value } else { 0 }
            $m = [regex]::Match("$tb", 'gs_med=([\d.]+).*?ft_med=([\d.]+) ft_p95=([\d.]+) ft_max=([\d.]+) ft_max_steady=([\d.]+)')
            $pm = [regex]::Match("$pl", '\(([\d.]+) ms per VBlank\)')
            $key = "$($r.name)|$($b.name)"
            if (-not $res.ContainsKey($key)) { $res[$key] = @() }
            if ($m.Success) {
                $res[$key] += ,@([double]$m.Groups[2].Value, [double]$m.Groups[3].Value, [double]$m.Groups[5].Value, [double]$m.Groups[1].Value, $(if ($pm.Success) { [double]$pm.Groups[1].Value } else { 0 }), $ftot)
                "{0,-26} {1,-10} #{2}: frontend med {3} p95 {4} max(steady) {5} total {8} ms | GS med {6} | pool {7} ms/VBlank" -f $r.name, $b.name, $k, $m.Groups[2].Value, $m.Groups[3].Value, $m.Groups[5].Value, $m.Groups[1].Value, $(if ($pm.Success) { $pm.Groups[1].Value } else { '-' }), $ftot
            } else { "{0,-26} {1,-10} #{2}: no thread-bench line ({3})" -f $r.name, $b.name, $k, $log }
        }
    }
}
function Med($a) { $s = @($a | Sort-Object); if (-not $s.Count) { return 0 }; return $s[[int][math]::Floor(($s.Count - 1) / 2)] }
"--- medians over $Repeat repeats (ms per game frame): frontend med / p95 / max steady, GS med, pool CPU per VBlank"
foreach ($r in $list) {
    foreach ($b in $builds) {
        $v = $res["$($r.name)|$($b.name)"]
        if (-not $v -or -not $v.Count) { continue }
        "{0,-26} {1,-10} front {2:N3} / {3:N3} / {4:N3} | total {7:N0} ms (min {8:N0}) | GS {5:N3} | pool {6:N3}" -f $r.name, $b.name, (Med ($v | ForEach-Object { $_[0] })), (Med ($v | ForEach-Object { $_[1] })), (Med ($v | ForEach-Object { $_[2] })), (Med ($v | ForEach-Object { $_[3] })), (Med ($v | ForEach-Object { $_[4] })), (Med ($v | ForEach-Object { $_[5] })), (($v | ForEach-Object { $_[5] }) | Measure-Object -Minimum).Minimum
    }
}
foreach ($b in $builds) { Remove-Item $b.exe -ErrorAction SilentlyContinue }
