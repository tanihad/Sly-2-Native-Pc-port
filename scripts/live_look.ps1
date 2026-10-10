# Live play helper: optionally send input, wait, then tile the newest dumped frames into one sheet.
#   .\scripts\live_look.ps1 [-Send "+0:lup:60"] [-Wait 4] [-Count 6] [-Cols 3] [-Scale 2] [-Dir build\live]
# Prints the sheet path (<Dir>\look.png) for viewing.
param([string]$Send = "", [double]$Wait = 0, [int]$Count = 6, [int]$Cols = 3, [int]$Scale = 2, [string]$Dir = "")
$root = Split-Path $PSScriptRoot -Parent
if (-not $Dir) { $Dir = Join-Path $root 'build\live' }
$py = 'C:\Users\tanih\Projects\sly2-decomp\.venv\Scripts\python.exe'
if ($Send) { Add-Content -Path (Join-Path $Dir 'input.txt') -Value $Send -Encoding ascii }
if ($Wait -gt 0) { Start-Sleep -Milliseconds ([int]($Wait * 1000)) }
$frames = Get-ChildItem (Join-Path $Dir 'frames') -Filter 'frame_*.png' | Sort-Object Name | Select-Object -Last $Count
$sheet = Join-Path $Dir 'look.png'
& $py (Join-Path $root 'scripts\contact_sheet.py') $sheet ($frames | ForEach-Object { $_.FullName }) --cols $Cols --scale $Scale | Out-Null
"frames: $(($frames | ForEach-Object { $_.BaseName -replace 'frame_0*', '' }) -join ' ')"
Get-Content (Join-Path $Dir 'run.log') -Tail 400 | Where-Object { $_ -notlike '*live tokens*' -and $_ -notlike '*pad frame*' } | Select-Object -Last 3
