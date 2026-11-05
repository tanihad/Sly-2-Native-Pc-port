# Launcher unit tests: quick, headless, no network (127.0.0.1 only), nothing of the player's touched.
#   powershell -ExecutionPolicy Bypass -File tests\unit\launcher\run.ps1 [-NoBuild]
# - test_release.c: Ed25519 (RFC 8032 vectors, tampering, S >= L, bad keys), signatures made by
#   scripts/sly2_ed25519.py, manifest parsing, version order, the HTTPS-only URL policy, the staged game
#   folder swap (commit, a locked game, interrupted commits, recover, rollback)
# - scripts: release_keygen.py refuses a key inside the repository; make_release.py makes a signed release
# - the program (scripts\make_installer.py --test-build): the window driven hidden (/uitest: drop a wrong
#   file, a folder, the right image; the Play screen of an installation with its update check)
# - updates from a local server: a newer version found; the release build refuses plain HTTP; a manifest
#   signed by another key, a changed manifest, a changed bundle -> rejected, the installation untouched
# The full flow (import, build, update with a staged rebuild, a failing build, rollback, uninstall) is
# tests\installer\e2e.ps1.
param([switch]$NoBuild)
$ErrorActionPreference = 'Continue'                  # (native tools write to stderr; Check counts failures)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {      # worktree: tools live in the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$py = Join-Path $main '.venv\Scripts\python.exe'
$clang = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
$out = Join-Path $root 'build\unit\launcher'
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($d in 'srv', 'fake', 'stage', 'isos', 'ud') { $p = Join-Path $out $d; if (Test-Path $p) { Remove-Item -Recurse -Force $p } }
$script:fails = 0
function Check($cond, $what) { if ($cond) { Write-Output "ok   $what" } else { Write-Output "FAIL $what"; $script:fails++ } }
$stdout = Join-Path $out 'stdout.txt'
function Run([string]$exePath, [string]$argline, [int]$timeout = 300) {
    if (Test-Path $stdout) { Remove-Item $stdout }
    $p = Start-Process $exePath -ArgumentList $argline -PassThru -NoNewWindow -RedirectStandardOutput $stdout
    $null = $p.Handle
    if (-not $p.WaitForExit($timeout * 1000)) { Stop-Process -Id $p.Id -Force; return @{ Code = -1; Out = 'TIMEOUT' } }
    $text = if (Test-Path $stdout) { (Get-Content $stdout -Raw) } else { '' }
    @{ Code = $p.ExitCode; Out = "$text".Trim() }
}
$tool = Join-Path $PSScriptRoot 'release_tool.py'

# ---- C unit tests ------------------------------------------------------------------------------------
$t = Join-Path $out 'test_release.exe'
& $clang -O2 -std=c11 -Wall -Wextra -Werror -I (Join-Path $root 'installer') (Join-Path $PSScriptRoot 'test_release.c') `
    (Join-Path $root 'installer\ed25519.c') (Join-Path $root 'installer\release.c') -lbcrypt -o $t
if ($LASTEXITCODE) { Write-Output "FAIL: building test_release"; exit 1 }
$cases = Join-Path $out 'cases.txt'
& $py $tool cases $cases
$r = & $t (Join-Path $out 'stage') $cases
$r | ForEach-Object { if ($_ -match 'FAIL') { Write-Output "  $_" } }
Check ($LASTEXITCODE -eq 0) "test_release: $($r[-1])"

# ---- scripts -----------------------------------------------------------------------------------------
& $py (Join-Path $root 'scripts\release_keygen.py') --key (Join-Path $out 'inside.key') --no-header 2>&1 | Out-Null
Check ($LASTEXITCODE -ne 0 -and -not (Test-Path (Join-Path $out 'inside.key'))) "release_keygen.py refuses a private key inside the repository"
$realKey = Join-Path $env:USERPROFILE '.sly2-release\release-ed25519.key'
if (Test-Path $realKey) {                           # (read only: is the built-in public key the release key's?)
    $pub = & $py -c "import sys; sys.path.insert(0, r'$root\scripts'); import sly2_ed25519, release_keygen, pathlib; print(sly2_ed25519.public_key(release_keygen.read_private_key(pathlib.Path(r'$realKey'))).hex())"
    Check ((Get-Content (Join-Path $root 'installer\release_key.h') -Raw) -match $pub) "installer/release_key.h holds the release key's public half"
}

# ---- the program ---------------------------------------------------------------------------------------
$setupDir = Join-Path $out 'setup'
$exe = Join-Path $setupDir 'Sly2-Setup.exe'
$texe = Join-Path $setupDir 'Sly2-Setup-Test.exe'
if (-not $NoBuild -or -not (Test-Path $texe)) {
    & $py (Join-Path $root 'scripts\make_installer.py') --out $setupDir --test-build | Out-Null
    if ($LASTEXITCODE) { Write-Output "FAIL: make_installer.py"; exit 1 }
}
$r = Run $exe '/version'
Check ($r.Out -match '^Sly2-Setup 0\.1\.\d+' -and $r.Out -notmatch 'test') "release build: $($r.Out)"
$r = Run $texe '/version'
Check ($r.Out -match 'test build') "test build: $($r.Out)"
$version = [regex]::Match($r.Out, '0\.1\.(\d+)').Groups[1].Value

$isos = Join-Path $out 'isos'
& $py (Join-Path $root 'tests\unit\installer\make_isos.py') $isos | Out-Null
$realIso = Get-ChildItem (Join-Path $main 'orig\disc') -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1
$uiIso = if ($realIso) { $realIso.FullName } else { '' }
$uilog = Join-Path $out 'uitest.log'
Remove-Item $uilog -ErrorAction SilentlyContinue
$ud = Join-Path $out 'ud'
$r = Run $exe "/uitest `"/iso=$uiIso`" `"/badiso=$(Join-Path $isos 'pal.iso')`" `"/dir=$(Join-Path $out 'ui-dir')`" `"/userdata=$ud`" `"/log=$uilog`" /regkey=Software\Sly2LauncherTest\Uninstall\Sly2-PC"
$ul = Get-Content $uilog
if ($realIso) {
    Check ($r.Code -eq 0 -and ($ul -match '0 failure')) "/uitest (drop screen): $(@($ul -match '\[uitest\] ok').Count) checks passed"
} else { Write-Output "skip /uitest drop checks (no disc image in orig\disc)" }
Check (-not (Test-Path (Join-Path $out 'ui-dir'))) "/uitest leaves no install folder"

# ---- art: the game icon's choice (owner > disc > own), the fallbacks, the window with and without pictures --------
# (synthetic pictures only: tests\unit\launcher\make_pictures.py; scripts/make_icon.py stands in for the disc's icon)
$pics = Join-Path $out 'pics'
& $py (Join-Path $PSScriptRoot 'make_pictures.py') $pics
$ad = Join-Path $out 'art-dir'; $au = Join-Path $out 'art-ud'; $aua = Join-Path $au 'art'
foreach ($d in $ad, $au) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }
New-Item -ItemType Directory -Force (Join-Path $ad 'game\art'), $aua | Out-Null
function ArtIcon { (Run $exe "/articon `"$ad`" `"$au`"").Out }
function IcoCount($p) { $b = [IO.File]::ReadAllBytes($p); if ($b.Length -gt 6 -and $b[2] -eq 1) { $b[4] + 256 * $b[5] } else { 0 } }
$icon = Join-Path $ad 'art\sly2.ico'
$o = ArtIcon
Check ($o -match '^same none .*Sly 2 Launcher\.exe$') "no art anywhere: shortcuts keep the launcher's own icon ($o)"
& $py (Join-Path $root 'scripts\make_icon.py') (Join-Path $ad 'game\art\sly2.ico')
$o = ArtIcon
Check ($o -match '^changed disc .*\\art\\sly2\.ico$' -and (Test-Path $icon)) "the build's disc icon becomes the game icon ($o)"
Check ((ArtIcon) -match '^same disc') "unchanged sources: nothing rewritten"
Copy-Item (Join-Path $pics 'icon.png') $aua
$o = ArtIcon
Check ($o -match '^changed owner' -and (IcoCount $icon) -eq 10) "the owner's icon.png wins over the disc's: a 10-size .ico ($o)"
Copy-Item (Join-Path $pics 'broken.png') (Join-Path $aua 'icon.png') -Force
$o = ArtIcon
Check ($o -match '^changed disc') "an unreadable owner picture falls back to the disc's icon ($o)"
Remove-Item (Join-Path $aua 'icon.png')
Copy-Item (Join-Path $pics 'logo.png') $aua
$o = ArtIcon
Check ($o -match '^changed owner' -and (IcoCount $icon) -eq 10) "the owner's logo.png: its mask on a badge ($o)"
Remove-Item (Join-Path $aua 'logo.png'), (Join-Path $ad 'game\art\sly2.ico')
$o = ArtIcon
Check ($o -match '^changed none .*Sly 2 Launcher\.exe$' -and -not (Test-Path $icon)) "no source left: back to the launcher's own icon ($o)"
$r = Run $exe "/makeicon `"$(Join-Path $pics 'icon.png')`" `"$(Join-Path $out 'logo-bad.ico')`" logo"
Check ($r.Code -ne 0) "a picture that is not the logo gives no mask icon (the next source is used)"
foreach ($k in 'none', 'owner') {                    # the window rendered off-screen: our theme, then the owner's pictures
    if ($k -eq 'owner') { Copy-Item (Join-Path $pics 'logo.png'), (Join-Path $pics 'backdrop.png') $aua }
    $shots = Join-Path $out "shots-$k"; $slog = Join-Path $out "shots-$k.log"
    Remove-Item -Recurse -Force $shots, $slog -ErrorAction SilentlyContinue
    $r = Run $exe "`"/uishot=$shots`" `"/dir=$ad`" `"/userdata=$au`" `"/log=$slog`""
    $n = @(Get-ChildItem $shots -Filter *.bmp -ErrorAction SilentlyContinue).Count
    $want = if ($k -eq 'none') { 'backdrop none \(theme\), logo none' } else { "backdrop the owner's, logo the owner's" }
    Check ($r.Code -eq 0 -and $n -eq 8 -and ((Get-Content $slog) -match $want)) "the window with $k art: $n screens rendered"
}

# ---- nothing of the game in what we ship ---------------------------------------------------------------------
$cg = Join-Path $root 'scripts\check_no_game_data.py'
$r = @(& $py $cg $exe $texe)
Check ($LASTEXITCODE -eq 0) "Setup carries no picture, icon or disc file: $($r[-1])"
$null = & $py $cg (Join-Path $pics 'game.zip')
Check ($LASTEXITCODE -eq 1) "check_no_game_data finds a picture hidden in a bundle"

# ---- updates from 127.0.0.1 ------------------------------------------------------------------------------
$srv = Join-Path $out 'srv'
New-Item -ItemType Directory -Force $srv | Out-Null
$keyFile = Join-Path $env:TEMP "sly2-launcher-test-$PID.key"            # a test key, outside the repository
$otherKey = Join-Path $env:TEMP "sly2-launcher-test-$PID-other.key"
$pubHex = (& $py $tool key $keyFile).Trim()
$null = & $py $tool key $otherKey
# a fake installation (version 0.1.500000: newer than this Setup's own sources, older than the release) to update
$fake = Join-Path $out 'fake'
New-Item -ItemType Directory -Force (Join-Path $fake 'game') | Out-Null
[IO.File]::WriteAllText((Join-Path $fake 'game\sly2-version.txt'), '0.1.500000 fake')
[IO.File]::WriteAllText((Join-Path $fake 'game\sly2.exe'), 'not really')
New-Item -ItemType Directory -Force (Join-Path $fake 'data') | Out-Null
[IO.File]::WriteAllText((Join-Path $fake 'data\sly2.img'), 'not really')
[IO.File]::WriteAllText((Join-Path $fake 'setup-state.ini'),
    "[setup]`r`nregkey=Software\Sly2LauncherTest\Uninstall\Sly2-PC`r`niso=$(Join-Path $fake 'data\sly2.img')`r`n", [Text.Encoding]::Unicode)

$portFile = Join-Path $out 'port.txt'; $srvLog = Join-Path $out 'server.log'
Remove-Item $portFile, $srvLog -ErrorAction SilentlyContinue
$server = Start-Process $py -ArgumentList "-I `"$(Join-Path $root 'tests\unit\installer\range_server.py')`" `"$srv`" `"$portFile`" `"$srvLog`"" -PassThru -WindowStyle Hidden
try {
    for ($i = 0; $i -lt 50 -and -not (Test-Path $portFile); $i++) { Start-Sleep -Milliseconds 100 }
    $base = "http://127.0.0.1:$((Get-Content $portFile -Raw).Trim())/files"
    $rel = Join-Path $out 'release'
    if (Test-Path $rel) { Remove-Item -Recurse -Force $rel }
    $mk = & $py (Join-Path $root 'scripts\make_release.py') --key $keyFile --count 999999 --allow-dirty --base-url $base --out $rel 2>&1
    Check ($LASTEXITCODE -eq 0 -and ($mk -match 'gh release create v0\.1\.999999')) "make_release.py: bundle, manifest, signature and the gh command"
    $r = @(& $py $cg (Join-Path $rel 'sly2-source-0.1.999999.zip'))
    Check ($LASTEXITCODE -eq 0) "the release bundle carries no game data: $($r[-1])"
    Copy-Item (Join-Path $rel '*') $srv
    $man = Get-Content (Join-Path $srv 'sly2-release.txt') -Raw
    Check ($man -match 'version = 0\.1\.999999' -and $man -match "bundle = $([regex]::Escape($base))/sly2-source-0\.1\.999999\.zip") "the manifest names the bundle"
    $url = "$base/sly2-release.txt"
    $common = "`"/dir=$fake`" `"/updateurl=$url`""

    $r = Run $texe "/checkupdate $common /pubkey=$pubHex"
    Check ($r.Code -eq 0 -and $r.Out -match '^newer 0\.1\.999999 \(installed 0\.1\.500000\)') "test build: a newer version is found ($($r.Out))"
    $r = Run $exe "/checkupdate $common /pubkey=$pubHex"
    Check ($r.Code -eq 1 -and $r.Out -match 'not a secure') "release build: plain HTTP refused, /pubkey ignored ($($r.Out))"
    $r = Run $texe "/checkupdate $common"
    Check ($r.Code -eq 1 -and $r.Out -match 'not signed correctly') "a manifest not signed by the built-in key is rejected"
    # signed by another key
    $other = Join-Path $srv 'other-release.txt'
    & $py $tool sign $otherKey (Join-Path $srv 'sly2-source-0.1.999999.zip') "$base/sly2-source-0.1.999999.zip" '0.1.999999' $other
    $r = Run $texe "/checkupdate `"/dir=$fake`" `"/updateurl=$base/other-release.txt`" /pubkey=$pubHex"
    Check ($r.Code -eq 1 -and $r.Out -match 'not signed correctly') "another key's signature is rejected"
    # a changed manifest (the bundle address swapped), old signature
    $changed = Join-Path $srv 'changed-release.txt'
    [IO.File]::WriteAllText($changed, ($man -replace 'sly2-source-0\.1\.999999\.zip', 'evil.zip'))
    Copy-Item (Join-Path $srv 'sly2-release.txt.sig') "$changed.sig"
    $r = Run $texe "/checkupdate `"/dir=$fake`" `"/updateurl=$base/changed-release.txt`" /pubkey=$pubHex"
    Check ($r.Code -eq 1 -and $r.Out -match 'not signed correctly') "a changed manifest is rejected"
    # a changed bundle (same size, valid signature over the original): downloaded, then refused
    $tb = Join-Path $srv 'tampered-sly2-source.zip'
    $bytes = [IO.File]::ReadAllBytes((Join-Path $srv 'sly2-source-0.1.999999.zip')); $bytes[2000] = $bytes[2000] -bxor 0x55
    [IO.File]::WriteAllBytes($tb, $bytes)
    $tm = Join-Path $srv 'tampered-release.txt'
    [IO.File]::WriteAllText($tm, ($man -replace 'sly2-source-0\.1\.999999\.zip', 'tampered-sly2-source.zip'))
    $null = & $py -c "import sys; sys.path.insert(0, r'$root\scripts'); import sly2_ed25519, release_keygen, pathlib; s = release_keygen.read_private_key(pathlib.Path(r'$keyFile')); pathlib.Path(r'$tm.sig').write_text(sly2_ed25519.sign(s, pathlib.Path(r'$tm').read_bytes()).hex())"
    $ulog = Join-Path $out 'update-tampered.log'
    Remove-Item $ulog -ErrorAction SilentlyContinue
    $r = Run $texe "/silent /update `"/dir=$fake`" `"/updateurl=$base/tampered-release.txt`" /pubkey=$pubHex `"/log=$ulog`" /noupdatecheck"
    $tl = Get-Content $ulog -ErrorAction SilentlyContinue
    Check ($r.Code -eq 1 -and ($tl -match 'does not match the signed manifest') -and -not (Get-ChildItem (Join-Path $fake 'updates') -Filter *.zip -ErrorAction SilentlyContinue)) `
        "a changed bundle is downloaded, refused and deleted"
    Check (((Get-Content (Join-Path $fake 'game\sly2-version.txt') -Raw) -eq '0.1.500000 fake') -and -not (Test-Path (Join-Path $fake 'game.new'))) `
        "the installation is untouched"
    # the window: the Play screen offers the update (the check runs on its thread)
    $uilog2 = Join-Path $out 'uitest-ready.log'
    Remove-Item $uilog2 -ErrorAction SilentlyContinue
    $r = Run $texe "/uitest $common /pubkey=$pubHex `"/log=$uilog2`""
    $ul2 = Get-Content $uilog2
    Check ($r.Code -eq 0 -and ($ul2 -match 'ok: a newer version: Update offered')) "/uitest on an installation: Play screen, Update offered"
    $r = Run $texe "/uitest `"/dir=$fake`" `"/updateurl=$base/missing-release.txt`" /pubkey=$pubHex `"/log=$uilog2`""
    Check ($r.Code -eq 0 -and ((Get-Content $uilog2) -match 'ok: check failed: no Update button')) "/uitest: a failed check shows no Update button"
} finally {
    Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
    Remove-Item $keyFile, $otherKey -ErrorAction SilentlyContinue
}
$r = Run $texe "/checkupdate $common /pubkey=$pubHex"
Check ($r.Code -eq 1 -and $r.Out -match "Couldn't check for updates") "no server: `"$($r.Out)`""
$r = Run $exe "/checkupdate `"/dir=$fake`" /updateurl=ftp://example.com/x"
Check ($r.Code -eq 1) "a non-https update address is refused"

foreach ($d in 'srv', 'fake', 'release', 'isos', 'ud', 'pics', 'art-dir', 'art-ud', 'shots-none', 'shots-owner') { $p = Join-Path $out $d; if (Test-Path $p) { Remove-Item -Recurse -Force $p } }
if (Test-Path 'HKCU:\Software\Sly2LauncherTest') { Remove-Item -Recurse 'HKCU:\Software\Sly2LauncherTest' }
if ($script:fails) { Write-Output "$($script:fails) launcher test(s) FAILED"; exit 1 }
Write-Output "launcher unit tests passed"
exit 0
