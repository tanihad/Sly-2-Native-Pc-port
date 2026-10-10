# Sly 2 prologue: from anywhere in the museum hall, bounce on the drum south of the hall centre up
# onto the whale skeleton's skull (lands at about 5309,-4586,1661). Needs a game from live_game.ps1
# with PS2_PEEK="2DE1C0:4,2DDFD0:4".
$py = 'C:\Users\tanih\Projects\sly2-decomp\.venv\Scripts\python.exe'
$s = $PSScriptRoot
& $py (Join-Path $s 'live_goto.py') 5300,-4200 5060,-4740 --timeout 30 --tol 40 | Select-Object -Last 1
& $py (Join-Path $s 'live_hop.py') 5300,-4586 --hold 20
$log = Join-Path (Split-Path $s -Parent) 'build\live\run.log'
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Milliseconds 500
    $l = Get-Content $log -Tail 200 | Where-Object { $_ -like '*002DE1C0*' } | Select-Object -Last 3
    $z = $l | ForEach-Object { $w = ($_ -split ':')[1].Trim() -split ' '; [BitConverter]::ToSingle([BitConverter]::GetBytes([Convert]::ToUInt32($w[2], 16)), 0) }
    if (($z | Measure-Object -Minimum).Minimum -gt 1655) { "on the skull: $($l[-1])"; break }
}
