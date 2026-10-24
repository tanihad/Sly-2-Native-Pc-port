# (perf-p2) wait until the GPU is quiet (utilisation below -Max % for -Secs seconds in a row, polled every 5 s), then run
# the rest of the command line (a script and its arguments). Gives up after -Timeout minutes and runs anyway.
#   powershell -File scripts\p2_quiet.ps1 [-Max 10] [-Secs 30] [-Timeout 120] -- scripts\p2_ab.ps1 -Exes ...
param([int]$Max = 10, [int]$Secs = 30, [int]$Timeout = 120, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)
$smi = Join-Path $env:SystemRoot 'System32\nvidia-smi.exe'
$t0 = Get-Date; $ok = 0
while (((Get-Date) - $t0).TotalMinutes -lt $Timeout) {
    $u = [int]((& $smi --query-gpu=utilization.gpu --format=csv,noheader,nounits) -join '')
    if ($u -lt $Max) { $ok += 5 } else { $ok = 0 }
    if ($ok -ge $Secs) { break }
    Start-Sleep 5
}
"quiet after {0:N1} min" -f ((Get-Date) - $t0).TotalMinutes
$script = $Rest[0]; $args2 = @($Rest | Select-Object -Skip 1)
& powershell -NoProfile -ExecutionPolicy Bypass -File $script @args2
