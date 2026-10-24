# End-to-end installer test: the real toolchain, the real build, the game boots, update in place,
# uninstall - all in a temporary folder, without network and without touching the player's Start menu,
# desktop, saves or Apps & features list.
#   powershell -ExecutionPolicy Bypass -File tests\installer\e2e.ps1 [-SkipOffline] [-Keep]
#
# Needs the owner's disc image in orig\disc and a set-up checkout (tools\_downloads, tools\python,
# tools\uv-cache: only read). Steps:
#  1. failures first: a download folder with a damaged llvm-mingw archive (SHA-256 refused), a server
#     without the files (404)
#  2. "online" install: Sly2-Setup.exe /silent, the archives downloaded over HTTP from a local server on
#     127.0.0.1 serving tools\_downloads (range_server.py), Python and the packages from a /from folder
#     (no python-build-standalone archive or wheels exist offline); /keepbuild: everything is measured
#  3. the installed game boots headless from the disc image Setup put into sly2.ini (watchdog exit 3);
#     Start menu / desktop shortcuts, the registry entry
#  4. update in place: the same setup again (tools reused), a save in the user data folder survives;
#     the build files and tools are removed at the end -> final size
#  5. uninstall: everything goes except the saves
#  6. (unless -SkipOffline) Sly2-Setup-Offline.exe /offline: the toolchain from the setup file only,
#     then Uninstall.exe /deletesaves
# Disk: about 2 GB in %TEMP% while it runs; removed at the end (unless -Keep).
param([switch]$SkipOffline, [switch]$Keep)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$py = Join-Path $main '.venv\Scripts\python.exe'
$iso = (Get-ChildItem (Join-Path $main 'orig\disc') -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1)
if (-not $iso) { Write-Output "skip: no disc image in orig\disc"; exit 0 }
$iso = $iso.FullName
$freeGB = [math]::Round((Get-PSDrive C).Free / 1GB, 1)
if ($freeGB -lt 50) { Write-Output "stop: only $freeGB GB free on C: (needs 50)"; exit 1 }

$t = Join-Path $env:TEMP "sly2-installer-e2e"
if (Test-Path $t) { cmd /c rmdir /s /q "$t" }
New-Item -ItemType Directory -Force $t | Out-Null
$setupDir = Join-Path $root 'build\installer'
$key = 'Software\Sly2SetupTest\Uninstall\Sly2-PC'
$dir = Join-Path $t 'Sly 2'                                  # (a space in the path, as players have)
$ud = Join-Path $t 'userdata'
$ov = "/regkey=$key `"/startmenu=$(Join-Path $t 'startmenu')`" `"/desktopdir=$(Join-Path $t 'desktop')`" `"/userdata=$ud`""

$script:fails = 0
function Check($cond, $what) { if ($cond) { Write-Output "ok   $what" } else { Write-Output "FAIL $what"; $script:fails++ } }
function Run([string]$exe, [string]$argline, [int]$timeout = 3600) {
    $p = Start-Process $exe -ArgumentList $argline -PassThru -NoNewWindow
    $null = $p.Handle
    if (-not $p.WaitForExit($timeout * 1000)) { Stop-Process -Id $p.Id -Force; return -1 }
    $p.ExitCode
}
function MB($path) {
    if (-not (Test-Path $path)) { return 0 }
    [math]::Round(((Get-ChildItem $path -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum) / 1MB, 0)
}

Write-Output "== building the setup programs"
$mk = @((Join-Path $root 'scripts\make_installer.py'))
if (-not $SkipOffline) { $mk += '--offline' }
& $py @mk
if ($LASTEXITCODE) { Write-Output "FAIL make_installer.py"; exit 1 }
$setup = Join-Path $setupDir 'Sly2-Setup.exe'

# a /from folder with only Python and the package cache (copies; the main checkout is never written)
$from = Join-Path $t 'from'
$pydir = Get-ChildItem (Join-Path $main 'tools\python') -Directory -Filter 'cpython-3.13.16-windows-x86_64-*' |
         Where-Object { -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) } | Select-Object -First 1
New-Item -ItemType Directory -Force (Join-Path $from 'python') | Out-Null
robocopy $pydir.FullName (Join-Path $from "python\$($pydir.Name)") /E /XJ /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy (Join-Path $main 'tools\uv-cache') (Join-Path $from 'uv-cache') /E /XJ /XF .lock /NFL /NDL /NJH /NJS /NP | Out-Null

$portFile = Join-Path $t 'port.txt'; $srvLog = Join-Path $t 'server.log'
$server = Start-Process $py -ArgumentList "-I `"$(Join-Path $root 'tests\unit\installer\range_server.py')`" `"$(Join-Path $main 'tools\_downloads')`" `"$portFile`" `"$srvLog`"" -PassThru -WindowStyle Hidden
try {
    for ($i = 0; $i -lt 50 -and -not (Test-Path $portFile); $i++) { Start-Sleep -Milliseconds 100 }
    $mirror = "http://127.0.0.1:$((Get-Content $portFile -Raw).Trim())/files"

    Write-Output "== 1. failures"
    $badMirror = Join-Path $t 'badmirror'
    New-Item -ItemType Directory -Force $badMirror | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $badMirror 'llvm-mingw-20261006-ucrt-x86_64.zip'), [byte[]](1..200))
    $log = Join-Path $t 'fail1.log'
    $c = Run $setup "/silent `"/iso=$iso`" `"/dir=$dir`" `"/mirror=$badMirror`" `"/from=$from`" $ov `"/log=$log`""
    Check ($c -eq 1 -and ((Get-Content $log) -match 'damaged or not the expected file')) "a damaged archive is refused (SHA-256)"
    $log = Join-Path $t 'fail2.log'
    $c = Run $setup "/silent `"/iso=$iso`" `"/dir=$dir`" `"/mirror=$($mirror -replace '/files$', '/nothing')`" `"/from=$from`" $ov `"/log=$log`""
    Check ($c -eq 1 -and ((Get-Content $log) -match 'answered 404')) "a missing file on the server -> 404 message"

    Write-Output "== 2. install (archives over HTTP from 127.0.0.1)"
    $log = Join-Path $t 'install.log'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $c = Run $setup "/silent `"/iso=$iso`" `"/dir=$dir`" `"/mirror=$mirror`" `"/from=$from`" $ov /keepbuild `"/log=$log`""
    $secs = [math]::Round($sw.Elapsed.TotalSeconds)
    Check ($c -eq 0) "silent install: exit $c in $secs s"
    if ($c -ne 0) { Get-Content $log | Select-Object -Last 30 }
    $served = @(Get-Content $srvLog | Where-Object { $_ -match '/files/' })
    Check ($served.Count -ge 2) "the toolchain archives were downloaded over HTTP ($($served.Count) requests)"
    $b = Join-Path $dir 'builder'
    $sizes = [ordered]@{
        'llvm-mingw (trimmed)' = MB "$b\tools\llvm-mingw"; 'uv' = MB "$b\tools\uv"; 'python' = MB "$b\tools\python"
        'uv-cache' = MB "$b\tools\uv-cache"; '.venv' = MB "$b\.venv"; 'sources' = (MB "$b\recomp") + (MB "$b\runtime") + (MB "$b\scripts")
        'build intermediates' = (MB "$b\build") + (MB "$b\orig") + (MB "$b\dist"); 'whole folder' = MB $dir
    }
    $sizes.GetEnumerator() | ForEach-Object { Write-Output ("     {0,-22} {1,6} MB" -f $_.Key, $_.Value) }

    Write-Output "== 3. the installed game"
    $ini = Join-Path $ud 'sly2.ini'
    Check ((Test-Path $ini) -and ((Get-Content $ini -Raw) -match [regex]::Escape("disc = $iso"))) "sly2.ini has the disc image"
    Check (Test-Path (Join-Path $t 'startmenu\Sly 2 - Band of Thieves.lnk')) "Start menu shortcut (test folder)"
    Check (Test-Path (Join-Path $t 'desktop\Sly 2 - Band of Thieves.lnk')) "desktop shortcut (test folder)"
    $reg = Get-ItemProperty "HKCU:\$key" -ErrorAction SilentlyContinue
    Check ($reg -and $reg.InstallLocation -eq $dir -and $reg.UninstallString -match 'Uninstall\.exe' -and $reg.EstimatedSize -gt 0) "uninstall entry (test key)"
    $game = Join-Path $dir 'sly2_installer_e2e.exe'                # (a copy under its own name: no kill-by-name accidents)
    Copy-Item (Join-Path $dir 'sly2.exe') $game
    $env:PS2_HEADLESS = '1'; $env:PS2_AUDIO_NULL = '1'; $env:PS2_WATCHDOG = '8'; $env:PS2_USER_DIR = $ud
    Remove-Item Env:\PS2_ISO -ErrorAction SilentlyContinue
    $glog = Join-Path $t 'game.log'
    $p = Start-Process $game -PassThru -NoNewWindow -RedirectStandardError $glog -RedirectStandardOutput (Join-Path $t 'game.out')
    $null = $p.Handle
    if (-not $p.WaitForExit(60000)) { Stop-Process -Id $p.Id -Force }
    foreach ($v in 'PS2_HEADLESS', 'PS2_AUDIO_NULL', 'PS2_WATCHDOG', 'PS2_USER_DIR') { Remove-Item "Env:\$v" -ErrorAction SilentlyContinue }
    $gl = Get-Content $glog
    Check ($p.ExitCode -eq 3 -and ($gl -match 'from the disc image')) "the game boots headless from the image in sly2.ini (exit $($p.ExitCode), $(@($gl).Count) log lines)"
    Remove-Item $game

    Write-Output "== 4. update in place"
    New-Item -ItemType Directory -Force (Join-Path $ud 'memcard\slot0') | Out-Null
    Set-Content (Join-Path $ud 'memcard\slot0\e2e-save.txt') 'keep me'
    $log = Join-Path $t 'update.log'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $c = Run $setup "/silent `"/iso=$iso`" `"/dir=$dir`" `"/mirror=$mirror`" `"/from=$from`" $ov `"/log=$log`""
    $usecs = [math]::Round($sw.Elapsed.TotalSeconds)
    Check ($c -eq 0 -and ((Get-Content $log) -match 'llvm-mingw.*already installed')) "update: exit $c in $usecs s, tools reused"
    Check (Test-Path (Join-Path $ud 'memcard\slot0\e2e-save.txt')) "the save survived the update"
    Check (-not (Test-Path (Join-Path $dir 'builder'))) "build files and tools removed after the build"
    Write-Output ("     installed game folder: {0} MB" -f (MB $dir))
} finally {
    Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
}

Write-Output "== 5. uninstall"
$c = Run (Join-Path $dir 'Uninstall.exe') '/silent'
for ($i = 0; $i -lt 50 -and (Test-Path $dir); $i++) { Start-Sleep -Milliseconds 200 }
Check ($c -eq 0 -and -not (Test-Path $dir)) "Uninstall.exe removed the install folder"
Check (-not (Test-Path "HKCU:\$key")) "the uninstall entry is gone"
Check (-not (Test-Path (Join-Path $t 'startmenu\Sly 2 - Band of Thieves.lnk')) -and -not (Test-Path (Join-Path $t 'desktop\Sly 2 - Band of Thieves.lnk'))) "shortcuts removed"
Check ((Test-Path (Join-Path $ud 'memcard\slot0\e2e-save.txt')) -and (Test-Path (Join-Path $ud 'sly2.ini'))) "saves and settings kept"

if (-not $SkipOffline) {
    Write-Output "== 6. offline setup (toolchain from the setup file only)"
    $off = Join-Path $setupDir 'Sly2-Setup-Offline.exe'
    $log = Join-Path $t 'offline.log'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $c = Run $off "/silent /offline `"/iso=$iso`" `"/dir=$dir`" $ov /nodesktop `"/log=$log`""
    $osecs = [math]::Round($sw.Elapsed.TotalSeconds)
    $ol = Get-Content $log
    Check ($c -eq 0 -and ($ol -match 'from this setup program') -and -not ($ol -match 'downloading')) "offline install: exit $c in $osecs s, nothing downloaded"
    Check (-not (Test-Path (Join-Path $t 'desktop\Sly 2 - Band of Thieves.lnk'))) "/nodesktop: no desktop shortcut"
    $c = Run (Join-Path $dir 'Uninstall.exe') '/silent /deletesaves'
    for ($i = 0; $i -lt 50 -and (Test-Path $dir); $i++) { Start-Sleep -Milliseconds 200 }
    Check ($c -eq 0 -and -not (Test-Path $dir) -and -not (Test-Path $ud)) "Uninstall.exe /deletesaves removed the game and the user data"
}

if (Test-Path 'HKCU:\Software\Sly2SetupTest') { Remove-Item -Recurse 'HKCU:\Software\Sly2SetupTest' }
if (-not $Keep) { cmd /c rmdir /s /q "$t" }
Write-Output ("C: free: {0} GB" -f [math]::Round((Get-PSDrive C).Free / 1GB, 1))
if ($script:fails) { Write-Output "$($script:fails) e2e check(s) FAILED"; exit 1 }
Write-Output "installer e2e passed"
exit 0
