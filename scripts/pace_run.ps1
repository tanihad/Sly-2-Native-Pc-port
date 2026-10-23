# Game speed / frame pacing measurement run (notes/pc-experience.md, pacing): warps to an episode or job, runs a
# copy of the game with PS2_PERF + PS2_GS_HW_STATS (+ PS2_PACE_LOG) and summarises game speed (EE VBlanks/s, the
# game's own flips/s), GPU ms per game frame and the present intervals. Sound is off (null audio sink).
#   powershell -ExecutionPolicy Bypass -File scripts\pace_run.ps1 -Name ep1_3x -Episode 1 -Play -Scale 3
#       [-Mode headless|window|borderless] [-Seconds 110] [-From 80] [-Exe build\rel\sly2.exe] [-Extra "A=1;B=2"]
#       [-Ini "graphics.vsync=off;display.foo=bar"] [-Job m7_boss -Direct] [-Pad "+600:lup:90"] [-Keep]
# -Scale: 1..8, auto, or native (the internal resolution for the output size). -From: seconds after which the
# summary counts (skip the boot). Run dirs: build\pace\<Name> (frames are not dumped; -Keep keeps the exe copy).
param([Parameter(Mandatory = $true)][string]$Name, [int]$Episode = 1, [string]$Job = "", [switch]$Direct, [switch]$Play,
      [string]$Scale = "3", [string]$Mode = "headless", [int]$Seconds = 110, [int]$From = 80, [string]$Exe = "",
      [string]$Extra = "", [string]$Ini = "", [string]$Pad = "", [switch]$Keep, [int]$Width = 0, [int]$Height = 0)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'orig\SCUS_973.16'))) {
    $gitdir = ((Get-Content (Join-Path $root '.git') -Raw) -replace '^gitdir:\s*', '').Trim()
    $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
}
$py = Join-Path $main '.venv\Scripts\python.exe'
$dir = Join-Path $root "build\pace\$Name"
$card = Join-Path $dir 'card'
New-Item -ItemType Directory -Force $dir | Out-Null
Remove-Item -Recurse -Force $card -ErrorAction SilentlyContinue
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
# sly2.ini next to the exe (console build: the user data folder is the exe's folder)
$sections = [ordered]@{ graphics = [ordered]@{}; display = [ordered]@{}; window = [ordered]@{} }
$sections.graphics['renderer'] = 'gpu'
if ($Scale -ne 'env') { $sections.graphics['internal_scale'] = $Scale }
if ($Mode -eq 'borderless') { $sections.window['fullscreen'] = 'on' }
elseif ($Mode -eq 'window') {
    $sections.window['fullscreen'] = 'off'
    if ($Width -gt 0) { $sections.window['x'] = '0'; $sections.window['y'] = '0'; $sections.window['width'] = "$Width"; $sections.window['height'] = "$Height" }
    else { $sections.window['maximized'] = 'on'; $sections.window['x'] = '100'; $sections.window['y'] = '100'; $sections.window['width'] = '1600'; $sections.window['height'] = '900' }
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
$exeName = "sly2_pace_$Name.exe"
Copy-Item $Exe (Join-Path $dir $exeName) -Force
$script = '@187D80/1DD96C+30:start:10'
if ($Play -and -not $Direct -and $Episode -gt 0) { $script += ',+1500:cross:6' }
if ($Pad) { $script += ",$Pad" }
$env:PS2_INPUT = $script
$env:PS2_MEMCARD_DIR = $card
$env:PS2_ISO = $iso.FullName
$env:PS2_WATCHDOG = "$Seconds"
$env:PS2_AUDIO_NULL = '1'
$env:PS2_PERF = '1'
$env:PS2_GS_HW_STATS = '1'
if ($Mode -eq 'headless') { $env:PS2_HEADLESS = '1' } else { Remove-Item Env:\PS2_HEADLESS -ErrorAction SilentlyContinue }
Remove-Item Env:\PS2_DUMP_FRAMES -ErrorAction SilentlyContinue
$set = @()
foreach ($kv in ($Extra -split ';' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; Set-Item "Env:\$k" $v; $set += $k }
$log = Join-Path $dir 'run.log'
$p = Start-Process -FilePath (Join-Path $dir $exeName) -ArgumentList (Join-Path $main 'orig\SCUS_973.16') -WorkingDirectory $dir `
    -RedirectStandardError $log -RedirectStandardOutput (Join-Path $dir 'run.out') -PassThru -NoNewWindow
if (-not $p.WaitForExit(($Seconds + 20) * 1000)) { Stop-Process -Id $p.Id -Force; "-- killed (watchdog did not fire)" }
foreach ($k in $set) { Remove-Item "Env:\$k" -ErrorAction SilentlyContinue }
Remove-Item Env:\PS2_WARP_TASK -ErrorAction SilentlyContinue
if (-not $Keep) { Remove-Item (Join-Path $dir $exeName) -ErrorAction SilentlyContinue }
Remove-Item -Recurse -Force $card -ErrorAction SilentlyContinue

# ---- summary
$lines = Get-Content $log
$perf = @($lines | Select-String '^\[perf\]\s+([\d.]+)s vblanks (\d+)/(\d+) due \(([\d.]+)/s\), flips (\d+) \(([\d.]+)/s\)' |
    ForEach-Object { $m = $_.Matches[0].Groups; [pscustomobject]@{ t = [double]$m[1].Value; vbs = [double]$m[4].Value; flips = [double]$m[6].Value } } |
    Where-Object { $_.t -ge $From })
$gpu = @($lines | Select-String '^\[gpu\] stats v\d+: ([\d.]+) flips/s .*\| GPU ([\d.]+) / ([\d.]+) \|' |
    ForEach-Object { $m = $_.Matches[0].Groups; [pscustomobject]@{ fl = [double]$m[1].Value; med = [double]$m[2].Value; max = [double]$m[3].Value } })
$gpu = @($gpu | Select-Object -Last ([Math]::Max(1, $perf.Count)))
function Stat($v) { if (-not $v.Count) { return 'n/a' }; $s = $v | Sort-Object; '{0:N2} (min {1:N2}, median {2:N2})' -f (($v | Measure-Object -Average).Average), $s[0], $s[[int]($s.Count / 2)] }
"$Name : scale $Scale, mode $Mode, $($perf.Count) s counted (from $From s)"
"  EE VBlanks/s: $(Stat @($perf | ForEach-Object vbs))"
"  game flips/s: $(Stat @($perf | ForEach-Object flips))"
"  presents/s:   $(Stat @($gpu | ForEach-Object fl))"
"  GPU ms per game frame (median of the seconds' medians): $(Stat @($gpu | ForEach-Object med)); max $(($gpu | ForEach-Object max | Measure-Object -Maximum).Maximum)"
$lines | Select-String '^\[gpu\] (internal scale auto|graphics:|pacing|present|display|governor)|^\[pace\]' | Select-Object -Last 12 | ForEach-Object { "  " + $_.Line }
"  log: $log"
