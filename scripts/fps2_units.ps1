# Every tests\unit\*\run.ps1 (window / warp / menu with -UnitOnly, gs-hw with -NoWindow), one line per suite.
$root = Split-Path $PSScriptRoot -Parent
$fail = 0
foreach ($d in Get-ChildItem (Join-Path $root 'tests\unit') -Directory) {
    $run = Join-Path $d.FullName 'run.ps1'
    if (-not (Test-Path $run)) { continue }
    $a = @('-ExecutionPolicy', 'Bypass', '-File', $run)
    if ($d.Name -in @('window', 'warp', 'menu')) { $a += '-UnitOnly' }
    if ($d.Name -eq 'gs-hw') { $a += '-NoWindow' }
    $t = Get-Date
    $out = & powershell @a 2>&1
    $code = $LASTEXITCODE
    $last = ($out | Where-Object { "$_".Trim() } | Select-Object -Last 1)
    "{0,-12} exit {1} ({2:N0} s): {3}" -f $d.Name, $code, ((Get-Date) - $t).TotalSeconds, "$last"
    if ($code) { $fail++; $out | Select-Object -Last 15 | ForEach-Object { "      $_" } }
}
"failed suites: $fail"
exit $fail
