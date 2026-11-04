# F12 screenshot test (pc-shot): runs the game headless (no window) with PS2_INPUT "screenshot" twice, into a temp folder
# (PS2_SCREENSHOT_DIR), and checks for valid PNGs of the present size. Needs build\rel\sly2.exe (scripts\build_game.py
# --release --out build\rel) and the disc image. One game instance; the temp folder is deleted afterwards.
#   powershell -ExecutionPolicy Bypass -File tests\unit\shot\run.ps1 [-Size 2560x1440]
param([string]$Size = '2560x1440', [int]$TimeoutSec = 120)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$main = $root
if (-not (Test-Path (Join-Path $root 'orig\SCUS_973.16'))) {
    $gitdir = ((Get-Content (Join-Path $root '.git') -Raw) -replace '^gitdir:\s*', '').Trim()
    $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
}
$exe = Join-Path $root 'build\rel\sly2.exe'
if (-not (Test-Path $exe)) { Write-Output "FAILED: $exe missing (build it first)"; exit 1 }
$dir = Join-Path ([IO.Path]::GetTempPath()) ("sly2_shot_test_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $dir | Out-Null
$log = Join-Path $dir 'run.log'
$shots = Join-Path $dir 'pics\Sly 2 test'          # a folder that does not exist yet: the game creates it
$p = $null
try {
    $env:PS2_HEADLESS = '1'; $env:PS2_AUDIO_NULL = '1'; $env:PS2_WATCHDOG = "$TimeoutSec"
    $env:PS2_GS_PRESENT_SIZE = $Size
    $env:PS2_SCREENSHOT_DIR = $shots
    $env:PS2_INPUT = '300:screenshot,303:screenshot'
    $iso = Get-ChildItem (Join-Path $main 'orig\disc') -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($iso) { $env:PS2_ISO = $iso.FullName }
    $p = Start-Process -FilePath $exe -ArgumentList (Join-Path $main 'orig\SCUS_973.16') -WorkingDirectory (Split-Path $exe) `
        -RedirectStandardError $log -RedirectStandardOutput (Join-Path $dir 'run.out') -PassThru -NoNewWindow
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec) {
        Start-Sleep -Milliseconds 500
        if ((Test-Path $shots) -and @(Get-ChildItem $shots -Filter *.png).Count -ge 2) { Start-Sleep -Milliseconds 1500; break }
        if ($p.HasExited) { break }
    }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    $fails = 0
    function Check($ok, $what) { if (-not $ok) { Write-Output "FAILED: $what"; $script:fails++ } else { Write-Output "ok: $what" } }
    $files = if (Test-Path $shots) { @(Get-ChildItem $shots -Filter *.png | Sort-Object Name) } else { @() }
    Check ($files.Count -eq 2) "two PNGs in the (created) folder: $($files.Count)"
    $names = ($files | ForEach-Object Name)
    Check (@($names | Where-Object { $_ -match '^Sly2_\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}(_\d+)?\.png$' }).Count -eq $files.Count) "names Sly2_YYYY-MM-DD_HH-MM-SS[_n].png: $($names -join ', ')"
    $w, $h = $Size -split 'x' | ForEach-Object { [int]$_ }
    foreach ($f in $files) {
        $b = [IO.File]::ReadAllBytes($f.FullName)
        $sig = ($b.Length -gt 33) -and ($b[0] -eq 0x89) -and ($b[1] -eq 0x50) -and ($b[2] -eq 0x4E) -and ($b[3] -eq 0x47)
        $pw = ([int]$b[16] -shl 24) + ([int]$b[17] -shl 16) + ([int]$b[18] -shl 8) + [int]$b[19]
        $ph = ([int]$b[20] -shl 24) + ([int]$b[21] -shl 16) + ([int]$b[22] -shl 8) + [int]$b[23]
        Check ($sig -and $pw -eq $w -and $ph -eq $h) "$($f.Name) is a PNG of ${pw}x${ph} (want ${w}x${h})"
        $tail = [Text.Encoding]::ASCII.GetString($b, $b.Length - 8, 4)
        Check ($tail -eq 'IEND') "$($f.Name) ends with IEND (complete)"
        # not a blank picture: more than a few distinct bytes in the pixel data
        $distinct = ($b[100..([Math]::Min($b.Length - 1, 400000))] | Sort-Object -Unique).Count
        Check ($distinct -gt 8) "$($f.Name) has picture content ($distinct distinct byte values)"
    }
    Check ((Get-Content $log -Raw) -match 'screenshot \d+x\d+ -> ') 'log line "[menu] screenshot ... ->"'
    if ($fails) { Get-Content $log | Select-Object -Last 15; exit 1 }
    Write-Output 'PASSED'
} finally {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    foreach ($v in 'PS2_HEADLESS','PS2_AUDIO_NULL','PS2_WATCHDOG','PS2_GS_PRESENT_SIZE','PS2_SCREENSHOT_DIR','PS2_INPUT','PS2_ISO') { Remove-Item "Env:\$v" -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 300
    Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
}

