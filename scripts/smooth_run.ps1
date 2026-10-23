# Smoothness measurement run (pc-smooth, notes/wip/pc-smooth.md): warps to an episode hub or job (scripts\warp.py), runs a
# copy of the release build (sly2_smooth_<Name>.exe) with PS2_PRESENT_TRACE (every present, GPU time, shown present),
# PS2_FPS_TRACE (the game's dt per VBlank), PS2_PERF + PS2_GS_HW_STATS, then prints scripts\smooth_trace.py's summary.
#   powershell -ExecutionPolicy Bypass -File scripts\smooth_run.ps1 -Name ep1_60 -Episode 1 -Job m1_brawl -Play -Rate 60
#       [-Scale auto|1..8] [-Mode headless|window|borderless] [-Seconds 75] [-From 40] [-Extra "A=1;B=2"] [-Ini "s.k=v;..."]
#       [-Pad "<PS2_INPUT tokens>"] [-Direct] [-Exe build\rel\sly2.exe] [-Size 2560x1440]
# Sound is off (null sink). Headless: the offscreen present target is -Size (the owner's 2560 x 1440). Run dir:
# build\smooth\<Name> (the exe copy is deleted afterwards).
param([Parameter(Mandatory = $true)][string]$Name, [int]$Episode = 1, [string]$Job = "", [switch]$Direct, [switch]$Play,
      [string]$Rate = "60", [string]$Scale = "auto", [string]$Mode = "headless", [int]$Seconds = 75, [int]$From = 40,
      [string]$Exe = "", [string]$Extra = "", [string]$Ini = "", [string]$Pad = "", [string]$Size = "2560x1440", [switch]$NoStats)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'orig\SCUS_973.16'))) {
    $gitdir = ((Get-Content (Join-Path $root '.git') -Raw) -replace '^gitdir:\s*', '').Trim()
    $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
}
$py = Join-Path $main '.venv\Scripts\python.exe'
$dir = Join-Path $root "build\smooth\$Name"
$card = Join-Path $dir 'card'
New-Item -ItemType Directory -Force $dir | Out-Null
Remove-Item -Recurse -Force $card -ErrorAction SilentlyContinue
Remove-Item (Join-Path $dir '*.txt'), (Join-Path $dir 'run.log') -ErrorAction SilentlyContinue
$mk = @('scripts\warp.py', 'make', '--episode', "$Episode", '--card', $card)
if ($Job) { $mk += @('--job', $Job) }
if ($Direct) { $mk += '--direct' }
Push-Location $root
$made = & $py @mk
Pop-Location
Remove-Item Env:\PS2_WARP_TASK -ErrorAction SilentlyContinue
if ($Direct) {
    $w = $made | Select-String 'PS2_WARP_TASK=(\S+)' | Select-Object -First 1
    $env:PS2_WARP_TASK = $w.Matches[0].Groups[1].Value
}
$sections = [ordered]@{ graphics = [ordered]@{}; display = [ordered]@{}; window = [ordered]@{} }
$sections.graphics['renderer'] = 'gpu'
if ($Scale -ne 'env') { $sections.graphics['internal_scale'] = $Scale }
$sections.display['aspect'] = '16:9'
if ($Mode -eq 'borderless') { $sections.window['fullscreen'] = 'on' }
elseif ($Mode -eq 'window') {
    $sections.window['fullscreen'] = 'off'; $sections.window['maximized'] = 'on'
    $sections.window['x'] = '100'; $sections.window['y'] = '100'; $sections.window['width'] = '1600'; $sections.window['height'] = '900'
}
foreach ($kv in ($Ini -split ';' | Where-Object { $_ })) {
    $k, $v = $kv -split '=', 2
    $s, $key = $k -split '\.', 2
    if (-not $sections.Contains($s)) { $sections[$s] = [ordered]@{} }
    $sections[$s][$key] = $v
}
$iso = Get-ChildItem (Join-Path $main 'orig\disc') -Filter *.iso | Select-Object -First 1
$txt = "[game]`r`ndisc = `"$($iso.FullName)`"`r`n"
foreach ($s in $sections.Keys) { $txt += "[$s]`r`n"; foreach ($k in $sections[$s].Keys) { $txt += "$k = $($sections[$s][$k])`r`n" } }
[IO.File]::WriteAllText((Join-Path $dir 'sly2.ini'), $txt)

if (-not $Exe) { $Exe = Join-Path $root 'build\rel\sly2.exe' }
$exeName = "sly2_smooth_$Name.exe"
Copy-Item $Exe (Join-Path $dir $exeName) -Force
$script = '@187D80/1DD96C+30:start:10'
# (play: X at the safehouse 25 s after START in wall time; time-based tokens give every frame rate the same input)
if ($Play -and -not $Direct -and $Episode -gt 0) { $script += ',r25:cross:0.2s' }
if ($Pad) { $script += ",$Pad" }
$env:PS2_INPUT = $script
$env:PS2_MEMCARD_DIR = $card
$env:PS2_ISO = $iso.FullName
$env:PS2_WATCHDOG = "$Seconds"
$env:PS2_AUDIO_NULL = '1'
if (-not $NoStats) { $env:PS2_PERF = '1'; $env:PS2_GS_HW_STATS = '1' }
else { Remove-Item Env:\PS2_PERF, Env:\PS2_GS_HW_STATS -ErrorAction SilentlyContinue }
$env:PS2_FRAME_RATE = $Rate
$env:PS2_PRESENT_TRACE = Join-Path $dir 'present.txt'
$env:PS2_FPS_TRACE = Join-Path $dir 'trace.txt'
if ($Mode -eq 'headless') { $env:PS2_HEADLESS = '1'; $env:PS2_GS_PRESENT_SIZE = $Size }
else { Remove-Item Env:\PS2_HEADLESS, Env:\PS2_GS_PRESENT_SIZE -ErrorAction SilentlyContinue }
Remove-Item Env:\PS2_DUMP_FRAMES, Env:\PS2_GS_SCALE -ErrorAction SilentlyContinue
$set = @()
foreach ($kv in ($Extra -split ';' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; Set-Item "Env:\$k" $v; $set += $k }
$log = Join-Path $dir 'run.log'
$p = Start-Process -FilePath (Join-Path $dir $exeName) -ArgumentList (Join-Path $main 'orig\SCUS_973.16') -WorkingDirectory $dir `
    -RedirectStandardError $log -RedirectStandardOutput (Join-Path $dir 'run.out') -PassThru -NoNewWindow
if (-not $p.WaitForExit(($Seconds + 30) * 1000)) { Stop-Process -Id $p.Id -Force; "-- killed (watchdog did not fire)" }
foreach ($k in $set) { Remove-Item "Env:\$k" -ErrorAction SilentlyContinue }
Remove-Item Env:\PS2_WARP_TASK, Env:\PS2_FRAME_RATE, Env:\PS2_PRESENT_TRACE, Env:\PS2_FPS_TRACE, Env:\PS2_HEADLESS,
    Env:\PS2_GS_PRESENT_SIZE, Env:\PS2_INPUT, Env:\PS2_WATCHDOG, Env:\PS2_PERF, Env:\PS2_GS_HW_STATS -ErrorAction SilentlyContinue
Remove-Item (Join-Path $dir $exeName) -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $card -ErrorAction SilentlyContinue
$tgt = if ($Rate -match '^\d+$' -and [int]$Rate -gt 60) { $Rate } else { '59.94' }
& $py (Join-Path $PSScriptRoot 'smooth_trace.py') "$dir=$Name" --from $From --target $tgt
Get-Content $log | Select-String '^\[gpu\] (internal scale|graphics: internal)|^\[fps\]|pace:|VRR' | Select-Object -Last 10 | ForEach-Object { "  " + $_.Line }
