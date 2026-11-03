# (r2-flicker) The temporal check of one scene: the release build at a warp scenario in one renderer mode, every
# picture of VBlanks At .. At + Frames - 1 dumped, then scripts\flicker_check.c: one-picture blinks (a pixel whose
# pictures before and after agree but differ from it), the longest blinking run in a row (a line that flicks) and
# blinking 16 x 16 blobs and seams, judged by tests\r2\gates.json "level3_temporal" (or -Check: flicker_check options).
#   .\scripts\flicker_run.ps1 -Name ep1 -Episode 1 -Job m1_brawl -Play -At 4800 [-Frames 240] [-Mode hybrid|off]
#                             [-Scale 3|auto] [-Env "K=V;K=V"] [-Pad ...] [-Keep] [-Check "--max-share 0.002"]
#                             [-Ini "graphics.outlines=thin;graphics.msaa=8x"]  (sly2.ini of the run; default: none)
# Dumps: raw BMPs of every VBlank (PS2_DUMP_BMP, PS2_DUMP_RAW: anamorphic, no aspect shaping; 11 MB each at 3x 16:9).
# At 60 fps they keep up (PS2_DUMP_WAIT=0); at a higher frame rate (-Env "PS2_FRAME_RATE=120") add PS2_DUMP_WAIT=1 so no
# VBlank is skipped (at 60 fps waiting stalls the EE: VBlanks pile up behind the blocked GS thread).
# Output: build\flicker\<Name>\ (frames deleted unless -Keep; worst.png = the worst picture, blinking pixels magenta).
param([Parameter(Mandatory = $true)][string]$Name, [int]$Episode = 1, [string]$Job = "", [switch]$Direct, [switch]$Play,
      [int]$At = 4800, [string]$Pad = "", [int]$Frames = 240, [string]$Mode = 'hybrid', [string]$Scale = '3',
      [string]$Env = "", [switch]$Keep, [string]$Check = "", [string]$Exe = "", [string]$Ini = "")
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {
    $gitdir = ((Get-Content (Join-Path $root '.git') -Raw) -replace '^gitdir:\s*', '').Trim()
    $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
}
$free = [math]::Round((Get-PSDrive C).Free / 1GB, 1)
if ($free -lt 60) { "C: has $free GB free (< 60): not starting"; exit 2 }
$tool = Join-Path $root 'build\flicker_check.exe'
$src = Join-Path $PSScriptRoot 'flicker_check.c'
if (-not (Test-Path $tool) -or (Get-Item $src).LastWriteTime -gt (Get-Item $tool).LastWriteTime) {
    & (Join-Path $main 'tools\llvm-mingw\bin\clang.exe') -O2 -o $tool $src
    if ($LASTEXITCODE) { "flicker_check.c does not compile"; exit 2 }
}
$out = Join-Path $root "build\flicker\$Name"
New-Item -ItemType Directory -Force $out | Out-Null
$set = @{ PS2_R2 = $Mode; PS2_R2_STATS = '1'; PS2_DUMP_FROM = "$At"; PS2_DUMP_TO = "$($At + $Frames - 1)"; PS2_ASPECT = '16:9'; PS2_DUMP_WAIT = '0'; PS2_DUMP_BMP = '1'; PS2_DUMP_RAW = '1' }
if ($Scale -eq 'auto') { $set.PS2_GS_SCALE = $null } else { $set.PS2_GS_SCALE = $Scale }
foreach ($kv in ($Env -split ';' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; $set[$k.Trim()] = $v }
$saved = @{}
foreach ($k in $set.Keys) {
    $saved[$k] = [Environment]::GetEnvironmentVariable($k)
    [Environment]::SetEnvironmentVariable($k, $set[$k])
}
if ($Exe) { $env:M9_GAME_EXE = $Exe }
$w = Join-Path $root "build\warp\flk_$Name"
New-Item -ItemType Directory -Force $w | Out-Null
Remove-Item (Join-Path $w 'sly2.ini') -ErrorAction SilentlyContinue
if ($Ini) {                             # the game's settings (a console exe reads sly2.ini next to itself)
    $sec = [ordered]@{}
    foreach ($kv in ($Ini -split ';' | Where-Object { $_ })) {
        $k, $v = $kv -split '=', 2; $s, $key = $k.Trim() -split '\.', 2
        if (-not $sec.Contains($s)) { $sec[$s] = @() }
        $sec[$s] += "$key = $($v.Trim())"
    }
    $txt = ($sec.Keys | ForEach-Object { "[$_]"; $sec[$_] }) -join "`r`n"
    [IO.File]::WriteAllText((Join-Path $w 'sly2.ini'), $txt + "`r`n")
}
$wa = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'warp_run.ps1'), '-Episode', "$Episode", '-Release',
        '-Seconds', "$([int](($At + $Frames) / 60) + 20 + [int]($Frames / 6))", '-DumpEvery', '1', '-Name', "flk_$Name")
if ($Job) { $wa += @('-Job', $Job) }
if ($Direct) { $wa += '-Direct' }
if ($Play) { $wa += '-Play' }
if ($Pad) { $wa += @('-Pad', $Pad) }
& powershell @wa | Out-Null
foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
Remove-Item Env:\M9_GAME_EXE -ErrorAction SilentlyContinue
$w = Join-Path $root "build\warp\flk_$Name"
Remove-Item (Join-Path $w 'sly2_warp_*.exe') -ErrorAction SilentlyContinue
Copy-Item (Join-Path $w 'run.log') (Join-Path $out 'run.log') -ErrorAction SilentlyContinue
$fr = Join-Path $w 'frames'
Remove-Item (Join-Path $fr 'latest.png') -ErrorAction SilentlyContinue
if (-not $Check) {                      # the gates (tests\r2\gates.json level3_temporal)
    $g = (Get-Content (Join-Path $root 'tests\r2\gates.json') -Raw | ConvertFrom-Json).level3_temporal
    $Check = "--max-share $($g.spike_share_max) --max-run $($g.longest_run_max) --max-rows $($g.rows_max) --max-blobs $($g.blobs_max) --seam $($g.seam_min_px) --max-seams $($g.seams_max)"
}
$ca = @($fr, '--quiet', '--out', (Join-Path $out 'worst.png')) + @($Check -split ' ' | Where-Object { $_ })
& $tool @ca
$rc = $LASTEXITCODE
# (r2-flicker) hybrid frames that reached the shadow-volume class with VIF1's double-buffer parity unpaid (r2_capture.c
# before_foreign): their shadows differ from an exact frame's, and toggling between paid and unpaid frames flickers.
# PS2_R2_STATS prints the count per 60 frames; any inside the dump window fails the check
$up = 0
foreach ($m in (Select-String -Path (Join-Path $w 'run.log') -Pattern '^\[r2\] v(\d+) mode .*parity unpaid (\d+)' -ErrorAction SilentlyContinue)) {
    $v = [int]$m.Matches[0].Groups[1].Value
    if ($v -gt $At -and $v -le $At + $Frames + 60) { $up += [int]$m.Matches[0].Groups[2].Value }
}
if ($Mode -eq 'hybrid') {
    "parity: $(if ($up) { 'FAIL' } else { 'PASS' }) - $up hybrid frames reached the shadow class with the double-buffer parity unpaid"
    if ($up -and -not $rc) { $rc = 1 }
}
if (-not $Keep) { Remove-Item -Recurse -Force $fr -ErrorAction SilentlyContinue }
exit $rc
