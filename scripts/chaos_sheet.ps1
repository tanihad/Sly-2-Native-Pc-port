# Contact sheet of a chaos-bench scout run's frames (System.Drawing, fast for 3x frames), each labelled with its VBlank
# and the trace's wall-clock second.
#   .\scripts\chaos_sheet.ps1 -Dir build\chaos\scout\m1_boss_run_0 [-Cols 6] [-Width 320]
param([Parameter(Mandatory = $true)][string]$Dir, [int]$Cols = 6, [int]$Width = 320)
Add-Type -AssemblyName System.Drawing
$frames = @(Get-ChildItem (Join-Path $Dir 'frames') -Filter 'frame_*.png' | Sort-Object Name)
if (-not $frames.Count) { "no frames"; exit 1 }
$vt = @{}
$tr = Join-Path $Dir 'trace.txt'
if (Test-Path $tr) { foreach ($l in [IO.File]::ReadLines($tr)) { $f = $l.Split(' '); if ($f.Count -ge 2) { $vt[[int]$f[0]] = [double]$f[1] } } }
$first = [System.Drawing.Image]::FromFile($frames[0].FullName)
$h = [int]($Width * $first.Height / $first.Width)
$first.Dispose()
$rows = [math]::Ceiling($frames.Count / $Cols)
$sheet = New-Object System.Drawing.Bitmap ($Cols * $Width), ($rows * $h)
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.InterpolationMode = 'HighQualityBilinear'
$font = New-Object System.Drawing.Font 'Arial', 10
for ($i = 0; $i -lt $frames.Count; $i++) {
    $img = [System.Drawing.Image]::FromFile($frames[$i].FullName)
    $x = ($i % $Cols) * $Width; $y = [math]::Floor($i / $Cols) * $h
    $g.DrawImage($img, $x, $y, $Width, $h)
    $img.Dispose()
    $v = [int]($frames[$i].BaseName.Substring(6))
    $lab = "v$v" + $(if ($vt.ContainsKey($v)) { " t{0:N0}" -f $vt[$v] } else { '' })
    $g.FillRectangle([System.Drawing.Brushes]::Black, $x, $y, 90, 16)
    $g.DrawString($lab, $font, [System.Drawing.Brushes]::Yellow, $x + 2, $y + 1)
}
$out = Join-Path $Dir 'sheet.jpg'
$sheet.Save($out, [System.Drawing.Imaging.ImageFormat]::Jpeg)
$g.Dispose(); $sheet.Dispose()
$out
