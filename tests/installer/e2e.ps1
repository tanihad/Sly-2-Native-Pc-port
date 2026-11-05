# End-to-end test of the launcher (= the installer): the real toolchain, the real build from the owner's
# disc image, the import, the game boots, updates from a local server (a failing build, a good one), rollback,
# uninstall - in build\e2e-launcher, without internet and without touching the player's Start menu, desktop,
# saves or Apps & features list. Every window stays hidden (/uitest drives the real window, never shown).
#   powershell -ExecutionPolicy Bypass -File tests\installer\e2e.ps1 [-SkipOffline] [-Keep]
#
# Needs the owner's disc image (orig\disc\*.iso, else [game] disc in %LOCALAPPDATA%\Sly2\sly2.ini - read only)
# and a set-up checkout (tools\_downloads, tools\python, tools\uv-cache: only read). Steps:
#  1. failures first (/noimport: no disc copy): a damaged llvm-mingw archive (SHA-256 refused), a 404
#  2. install through the launcher window: the image dropped on it, Install, Cancel during the work, Retry;
#     the toolchain over HTTP from 127.0.0.1 (range_server.py), Python + packages from a /from folder
#  3. the result: game\ + data\sly2.img (= the image) + bundle\, shortcuts -> the launcher, sly2.ini, registry;
#     the game boots headless from the imported copy, also when sly2.ini names an image that is gone
#  4. updates from 127.0.0.1 signed by a test key (test build): a release whose build fails -> the installed
#     version untouched; a good release -> staged build, swap, the previous version kept; a save and the
#     settings untouched; the window's update check; rollback and forward again
#  5. uninstall: everything goes except the saves
#  6. (unless -SkipOffline) Sly2-Setup-Offline.exe /offline /noimport, then Uninstall.exe /deletesaves
# Disk: about 5 GB in build\e2e-launcher while it runs (the image copy is 2.9 GB); removed at the end.
param([switch]$SkipOffline, [switch]$Keep)
$ErrorActionPreference = 'Continue'
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
$iso = Get-ChildItem (Join-Path $main 'orig\disc') -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1
$iso = if ($iso) { $iso.FullName } else { '' }
if (-not $iso) {
    $ini = Join-Path $env:LOCALAPPDATA 'Sly2\sly2.ini'
    if (Test-Path $ini) { $m = Select-String -Path $ini -Pattern '^\s*disc\s*=\s*(.+)$' | Select-Object -First 1; if ($m) { $iso = $m.Matches[0].Groups[1].Value.Trim() } }
}
if (-not $iso -or -not (Test-Path $iso)) { Write-Output "skip: no disc image"; exit 0 }
$freeGB = [math]::Round((Get-PSDrive C).Free / 1GB, 1)
if ($freeGB -lt 60) { Write-Output "stop: only $freeGB GB free on C: (needs 60)"; exit 1 }

$t = Join-Path $root 'build\e2e-launcher'
if (Test-Path $t) { cmd /c rmdir /s /q "$t" }
New-Item -ItemType Directory -Force $t | Out-Null
$setupDir = Join-Path $root 'build\installer'
$key = 'Software\Sly2SetupTest\Uninstall\Sly2-PC'
$dir = Join-Path $t 'Sly 2'                                  # (a space in the path, as players have)
$ud = Join-Path $t 'userdata'
$ov = "/regkey=$key `"/startmenu=$(Join-Path $t 'startmenu')`" `"/desktopdir=$(Join-Path $t 'desktop')`" `"/userdata=$ud`""

$script:fails = 0
function Check($cond, $what) { if ($cond) { Write-Output "ok   $what" } else { Write-Output "FAIL $what"; $script:fails++ } }
$stdout = Join-Path $t 'stdout.txt'
function Run([string]$exe, [string]$argline, [int]$timeout = 3600) {
    if (Test-Path $stdout) { Remove-Item $stdout }
    $p = Start-Process $exe -ArgumentList $argline -PassThru -NoNewWindow -RedirectStandardOutput $stdout
    $null = $p.Handle
    if (-not $p.WaitForExit($timeout * 1000)) { Stop-Process -Id $p.Id -Force; return -1 }
    $p.ExitCode
}
function Out() { if (Test-Path $stdout) { (Get-Content $stdout -Raw).Trim() } else { '' } }
function MB($path) {
    if (-not (Test-Path $path)) { return 0 }
    [math]::Round(((Get-ChildItem $path -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum) / 1MB, 0)
}
function Ver($sub) { $f = Join-Path $dir "$sub\sly2-version.txt"; if (Test-Path $f) { ((Get-Content $f -Raw).Trim() -split ' ')[0] } else { '' } }
function LinkTarget($lnk) { (New-Object -ComObject WScript.Shell).CreateShortcut($lnk).TargetPath }

Write-Output "== building the programs"
$mk = @((Join-Path $root 'scripts\make_installer.py'), '--out', $setupDir, '--test-build')
if (-not $SkipOffline) { $mk += '--offline' }
& $py @mk
if ($LASTEXITCODE) { Write-Output "FAIL make_installer.py"; exit 1 }
$setup = Join-Path $setupDir 'Sly2-Setup.exe'
$test = Join-Path $setupDir 'Sly2-Setup-Test.exe'            # (only the test build takes updates from 127.0.0.1)

# a /from folder with only Python and the package cache (copies; the main checkout is never written)
$from = Join-Path $t 'from'
$pydir = Get-ChildItem (Join-Path $main 'tools\python') -Directory -Filter 'cpython-3.13.16-windows-x86_64-*' |
         Where-Object { -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) } | Select-Object -First 1
New-Item -ItemType Directory -Force (Join-Path $from 'python') | Out-Null
robocopy $pydir.FullName (Join-Path $from "python\$($pydir.Name)") /E /XJ /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy (Join-Path $main 'tools\uv-cache') (Join-Path $from 'uv-cache') /E /XJ /XF .lock /NFL /NDL /NJH /NJS /NP | Out-Null

$rel = Join-Path $t 'release'
New-Item -ItemType Directory -Force $rel | Out-Null
$servers = @()
function Serve($folder, $name) {
    $pf = Join-Path $t "$name-port.txt"; $lg = Join-Path $t "$name-server.log"
    $s = Start-Process $py -ArgumentList "-I `"$(Join-Path $root 'tests\unit\installer\range_server.py')`" `"$folder`" `"$pf`" `"$lg`"" -PassThru -WindowStyle Hidden
    for ($i = 0; $i -lt 50 -and -not (Test-Path $pf); $i++) { Start-Sleep -Milliseconds 100 }
    @{ Proc = $s; Base = "http://127.0.0.1:$((Get-Content $pf -Raw).Trim())/files"; Log = $lg }
}
$keyFile = Join-Path $env:TEMP "sly2-e2e-test-$PID.key"         # a test key for this run, outside the repository
try {
    $tools = Serve (Join-Path $main 'tools\_downloads') 'tools'
    $servers += $tools
    $mirror = $tools.Base
    $relSrv = Serve $rel 'release'
    $servers += $relSrv

    Write-Output "== 1. failures"
    $badMirror = Join-Path $t 'badmirror'
    New-Item -ItemType Directory -Force $badMirror | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $badMirror 'llvm-mingw-20261006-ucrt-x86_64.zip'), [byte[]](1..200))
    $log = Join-Path $t 'fail1.log'
    $c = Run $setup "/silent /noimport `"/iso=$iso`" `"/dir=$dir`" `"/mirror=$badMirror`" `"/from=$from`" $ov `"/log=$log`""
    Check ($c -eq 1 -and ((Get-Content $log) -match 'damaged or not the expected file')) "a damaged archive is refused (SHA-256)"
    $log = Join-Path $t 'fail2.log'
    $c = Run $setup "/silent /noimport `"/iso=$iso`" `"/dir=$dir`" `"/mirror=$($mirror -replace '/files$', '/nothing')`" `"/from=$from`" $ov `"/log=$log`""
    Check ($c -eq 1 -and ((Get-Content $log) -match 'answered 404')) "a missing file on the server -> 404 message"

    Write-Output "== 2. install through the launcher window (hidden): drop the image, Install, Cancel, Retry"
    $log = Join-Path $t 'install.log'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $c = Run $test "/uitest /uiinstall /uicancel=5 `"/iso=$iso`" `"/dir=$dir`" `"/mirror=$mirror`" `"/from=$from`" $ov /keeptools /noupdatecheck `"/log=$log`""
    $secs = [math]::Round($sw.Elapsed.TotalSeconds)
    $il = Get-Content $log
    Check ($c -eq 0 -and -not ($il -cmatch 'FAIL:')) "install through the window: exit $c in $secs s, $(@($il -match '\[uitest\] ok').Count) checks"
    if ($c -ne 0) { $il | Select-Object -Last 30 }
    Check (($il -match 'ok: Cancel stops it') -and ($il -match 'ok: Retry starts again')) "cancelled while working, then Retry finished it"
    Check (@(Get-Content $tools.Log | Where-Object { $_ -match '/files/' }).Count -ge 2) "the toolchain archives came over HTTP"

    Write-Output "== 3. the installed game"
    $img = Join-Path $dir 'data\sly2.img'
    $v0 = Ver 'game'
    Check ((Test-Path (Join-Path $dir 'game\sly2.exe')) -and $v0 -match '^0\.1\.\d+$') "game\sly2.exe, version $v0"
    Check ((Test-Path $img) -and (Get-Item $img).Length -eq (Get-Item $iso).Length) "the disc image was imported: data\sly2.img ($([math]::Round((Get-Item $img).Length / 1GB, 2)) GB)"
    Check (Test-Path (Join-Path $dir "bundle\sly2-source-$v0.zip")) "the sources of $v0 are kept (bundle\)"
    Check (-not (Test-Path (Join-Path $dir 'sly2.exe')) -and -not (Test-Path (Join-Path $dir 'game.new'))) "no old-layout files, no staging left"
    $launcher = Join-Path $dir 'Sly 2 Launcher.exe'
    $c = Run $launcher '/payload'
    Check ($c -eq 0 -and (Out) -match '0 payload entries') "Sly 2 Launcher.exe: the program alone (no payload)"
    $ini = Join-Path $ud 'sly2.ini'
    Check ((Test-Path $ini) -and ((Get-Content $ini -Raw) -match [regex]::Escape("disc = $img"))) "sly2.ini names the imported copy"
    $sm = Join-Path $t 'startmenu\Sly 2 - Band of Thieves.lnk'; $dk = Join-Path $t 'desktop\Sly 2 - Band of Thieves.lnk'
    Check ((Test-Path $sm) -and (LinkTarget $sm) -eq $launcher) "Start menu shortcut -> the launcher"
    Check ((Test-Path $dk) -and (LinkTarget $dk) -eq $launcher) "desktop shortcut -> the launcher"
    # the game's icon from the player's disc (scripts/disc_art.py, notes/disc-art.md): built, chosen, on the shortcuts and
    # the Apps & features entry; the backdrop is a headless run of the game (optional: logged, not required)
    $artIco = Join-Path $dir 'art\sly2.ico'
    Check ((Test-Path (Join-Path $dir 'game\art\sly2.ico')) -and (Test-Path $artIco)) "the game icon made from the disc (game\art\sly2.ico -> art\sly2.ico)"
    $lnkIcon = (New-Object -ComObject WScript.Shell).CreateShortcut($sm).IconLocation
    Check ($lnkIcon -like "$artIco,*") "the shortcuts show it ($lnkIcon)"
    Write-Output "     backdrop from the disc: $(if (Test-Path (Join-Path $dir 'game\art\backdrop.bmp')) { 'yes' } else { 'no (the launcher uses its own theme)' })"
    $reg = Get-ItemProperty "HKCU:\$key" -ErrorAction SilentlyContinue
    Check ($reg -and $reg.InstallLocation -eq $dir -and $reg.DisplayVersion -eq $v0 -and $reg.DisplayIcon -eq "$artIco,0") "uninstall entry (test key), version $($reg.DisplayVersion), the game icon"
    Write-Output ("     game {0} MB, data {1} MB, builder (tools kept) {2} MB, whole folder {3} MB" -f (MB "$dir\game"), (MB "$dir\data"), (MB "$dir\builder"), (MB $dir))

    $game = Join-Path $dir 'game\sly2_launcher_e2e.exe'          # (a copy under its own name: no kill-by-name accidents)
    Copy-Item (Join-Path $dir 'game\sly2.exe') $game
    function Boot([string]$argline, [string]$name) {
        $env:PS2_HEADLESS = '1'; $env:PS2_AUDIO_NULL = '1'; $env:PS2_WATCHDOG = '8'; $env:PS2_USER_DIR = $ud
        Remove-Item Env:\PS2_ISO -ErrorAction SilentlyContinue
        $glog = Join-Path $t "$name.log"
        $sp = @{ FilePath = $game; PassThru = $true; NoNewWindow = $true; RedirectStandardError = $glog; RedirectStandardOutput = (Join-Path $t "$name.out") }
        if ($argline) { $sp.ArgumentList = $argline }
        $p = Start-Process @sp
        $null = $p.Handle
        if (-not $p.WaitForExit(60000)) { Stop-Process -Id $p.Id -Force }
        foreach ($v in 'PS2_HEADLESS', 'PS2_AUDIO_NULL', 'PS2_WATCHDOG', 'PS2_USER_DIR') { Remove-Item "Env:\$v" -ErrorAction SilentlyContinue }
        @{ Code = $p.ExitCode; Log = (Get-Content $glog) }
    }
    $b = Boot "`"$img`"" 'boot-launcher'
    Check ($b.Code -eq 3 -and ($b.Log -match [regex]::Escape("from the disc image $img"))) "the game boots headless from data\sly2.img as the launcher starts it (exit $($b.Code))"
    $iniText = Get-Content $ini -Raw
    [IO.File]::WriteAllText($ini, ($iniText -replace '(?m)^disc = .*$', "disc = $(Join-Path $t 'deleted-after-import.iso')"))
    $b = Boot '' 'boot-fallback'
    Check ($b.Code -eq 3 -and ($b.Log -match 'using the imported disc image')) "the original image gone from sly2.ini: the game finds its imported copy"
    [IO.File]::WriteAllText($ini, $iniText)
    Remove-Item $game

    Write-Output "== 4. updates from 127.0.0.1 (test key)"
    New-Item -ItemType Directory -Force (Join-Path $ud 'memcard\slot0') | Out-Null
    Set-Content (Join-Path $ud 'memcard\slot0\e2e-save.txt') 'keep me'
    $saveHash = (Get-FileHash (Join-Path $ud 'memcard\slot0\e2e-save.txt')).Hash
    $iniHash = (Get-FileHash $ini).Hash
    $pub = (& $py (Join-Path $root 'tests\unit\launcher\release_tool.py') key $keyFile).Trim()
    $n0 = [int]($v0 -split '\.')[2]
    $vGood = "0.1.$($n0 + 1000)"; $vBad = "0.1.$($n0 + 2000)"
    $null = & $py (Join-Path $root 'scripts\make_release.py') --key $keyFile --count ($n0 + 1000) --allow-dirty --base-url $relSrv.Base --out $rel
    $good = Join-Path $rel "sly2-source-$vGood.zip"
    # a correctly signed release whose sources do not compile
    $broken = Join-Path $rel "sly2-source-$vBad.zip"
    & $py (Join-Path $root 'tests\unit\launcher\release_tool.py') rezip $good $broken ($n0 + 2000) --break
    & $py (Join-Path $root 'tests\unit\launcher\release_tool.py') sign $keyFile $broken "$($relSrv.Base)/sly2-source-$vBad.zip" $vBad (Join-Path $rel 'broken-release.txt')
    $upd = "`"/dir=$dir`" `"/mirror=$mirror`" `"/from=$from`" $ov /pubkey=$pub /noupdatecheck"

    $log = Join-Path $t 'update-broken.log'
    $c = Run $test "/silent /update `"/updateurl=$($relSrv.Base)/broken-release.txt`" $upd `"/log=$log`""
    $bl = Get-Content $log
    Check ($c -ne 0 -and ($bl -match 'Building the game failed')) "a release whose build fails: exit $c"
    Check ((Ver 'game') -eq $v0 -and -not (Test-Path (Join-Path $dir 'game.new')) -and -not (Test-Path (Join-Path $dir 'game.old'))) "the installed version ($v0) is untouched"

    $log = Join-Path $t 'update.log'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $c = Run $test "/silent /update `"/updateurl=$($relSrv.Base)/sly2-release.txt`" $upd `"/log=$log`""
    $usecs = [math]::Round($sw.Elapsed.TotalSeconds)
    $gl = Get-Content $log
    Check ($c -eq 0 -and ($gl -match 'size and SHA-256 match the signed manifest') -and ($gl -match 'llvm-mingw.*already installed')) "update: exit $c in $usecs s (bundle verified, tools reused)"
    Check ((Ver 'game') -eq $vGood -and (Ver 'game.old') -eq $v0) "swapped: game $vGood, previous $v0 kept"
    Check ((Test-Path (Join-Path $dir "bundle\sly2-source-$vGood.zip")) -and (Test-Path (Join-Path $dir "bundle\sly2-source-$v0.zip")) -and
           -not (Get-ChildItem (Join-Path $dir 'updates') -Filter *.zip -ErrorAction SilentlyContinue)) "sources of both versions kept; the download removed"
    Check ((Get-FileHash (Join-Path $ud 'memcard\slot0\e2e-save.txt')).Hash -eq $saveHash -and (Get-FileHash $ini).Hash -eq $iniHash) "the save and sly2.ini are untouched"
    Check ((Get-ItemProperty "HKCU:\$key").DisplayVersion -eq $vGood) "Apps & features shows $vGood"
    $log = Join-Path $t 'uitest-ready.log'
    $c = Run $test "/uitest `"/dir=$dir`" `"/updateurl=$($relSrv.Base)/sly2-release.txt`" /pubkey=$pub `"/log=$log`""
    $rl = Get-Content $log
    Check ($c -eq 0 -and ($rl -match 'ok: set up: the Play screen') -and ($rl -match "update check: state 3, latest $vGood")) "the window: Play screen, 'You have the latest version'"

    $c = Run $test "/rollback `"/dir=$dir`" $ov"
    Check ($c -eq 0 -and (Ver 'game') -eq $v0 -and (Ver 'game.old') -eq $vGood) "rollback: $(Out)"
    $c = Run $test "/rollback `"/dir=$dir`" $ov"
    Check ($c -eq 0 -and (Ver 'game') -eq $vGood) "forward again: $(Out)"
} finally {
    foreach ($s in $servers) { Stop-Process -Id $s.Proc.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item $keyFile -ErrorAction SilentlyContinue
}

Write-Output "== 5. uninstall"
$c = Run (Join-Path $dir 'Uninstall.exe') '/silent'
for ($i = 0; $i -lt 50 -and (Test-Path $dir); $i++) { Start-Sleep -Milliseconds 200 }
Check ($c -eq 0 -and -not (Test-Path $dir)) "Uninstall.exe removed the install folder (game, previous version, imported image, tools)"
Check (-not (Test-Path "HKCU:\$key")) "the uninstall entry is gone"
Check (-not (Test-Path (Join-Path $t 'startmenu\Sly 2 - Band of Thieves.lnk')) -and -not (Test-Path (Join-Path $t 'desktop\Sly 2 - Band of Thieves.lnk'))) "shortcuts removed"
Check ((Test-Path (Join-Path $ud 'memcard\slot0\e2e-save.txt')) -and (Test-Path (Join-Path $ud 'sly2.ini'))) "saves and settings kept"

if (-not $SkipOffline) {
    Write-Output "== 6. offline setup (toolchain from the setup file only; no import)"
    $off = Join-Path $setupDir 'Sly2-Setup-Offline.exe'
    $log = Join-Path $t 'offline.log'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $c = Run $off "/silent /offline /noimport `"/iso=$iso`" `"/dir=$dir`" $ov /nodesktop `"/log=$log`""
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
Write-Output "launcher e2e passed"
exit 0
