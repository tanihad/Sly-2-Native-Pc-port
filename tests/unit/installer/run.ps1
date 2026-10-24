# Installer unit tests (installer/*.c, scripts/make_installer.py): quick, headless, no network.
#   powershell -ExecutionPolicy Bypass -File tests\unit\installer\run.ps1 [-NoBuild]
# - builds build\unit\installer\setup\Sly2-Setup.exe with scripts\make_installer.py
# - tool commands: /version, /payload, /sha256 (against Get-FileHash)
# - the disc check on synthetic images (make_isos.py) and on the owner's image if orig\disc has one
# - downloads from a local server on 127.0.0.1 (range_server.py, a copy of tools\_downloads\uv-*.zip):
#   complete, interrupted + resumed, resume of a partial file left by an earlier run, a server without
#   Range support, 404, a wrong SHA-256, nothing listening
# - /uitest: the wizard's pages driven in a window that is never shown
# - a damaged payload: /silent stops with a message; the partial install is removed by Uninstall.exe
#   (test registry key, test Start menu / desktop / user data folders; nothing of the player's)
# The full install (toolchain, build, game boot, uninstall) is tests\installer\e2e.ps1.
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
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
$out = Join-Path $root 'build\unit\installer'
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($d in 'isos', 'srv', 'dl', 'tamper', 'sm', 'desk', 'ud') { $p = Join-Path $out $d; if (Test-Path $p) { Remove-Item -Recurse -Force $p } }
$exe = Join-Path $out 'setup\Sly2-Setup.exe'
if (-not $NoBuild -or -not (Test-Path $exe)) {
    & $py (Join-Path $root 'scripts\make_installer.py') --out (Join-Path $out 'setup')
    if ($LASTEXITCODE) { Write-Output "FAIL: make_installer.py"; exit 1 }
}

$script:fails = 0
function Check($cond, $what) {
    if ($cond) { Write-Output "ok   $what" } else { Write-Output "FAIL $what"; $script:fails++ }
}
$stdout = Join-Path $out 'stdout.txt'
function Run([string]$exePath, [string]$argline, [int]$timeout = 120) {
    if (Test-Path $stdout) { Remove-Item $stdout }
    $p = Start-Process $exePath -ArgumentList $argline -PassThru -NoNewWindow -RedirectStandardOutput $stdout
    $null = $p.Handle                                # (else PowerShell 5.1 loses the exit code)
    if (-not $p.WaitForExit($timeout * 1000)) { Stop-Process -Id $p.Id -Force; return @{ Code = -1; Out = 'TIMEOUT' } }
    $text = if (Test-Path $stdout) { (Get-Content $stdout -Raw) } else { '' }
    @{ Code = $p.ExitCode; Out = "$text".Trim() }
}

# ---- tool commands ---------------------------------------------------------------------------------
$r = Run $exe '/version'
Check ($r.Code -eq 0 -and $r.Out -match '^Sly2-Setup 0\.1\.\d+') "/version: $($r.Out)"
$r = Run $exe '/payload'
Check ($r.Code -eq 0 -and $r.Out -match 'source\.zip') "/payload lists source.zip"
$readme = Join-Path $root 'README.md'
$r = Run $exe "/sha256 `"$readme`""
Check ($r.Out -eq (Get-FileHash $readme -Algorithm SHA256).Hash.ToLower()) "/sha256 matches Get-FileHash"

# the payload holds only our sources
Add-Type -AssemblyName System.IO.Compression.FileSystem
$tmpzip = Join-Path $out 'source-check.zip'
$bytes = [IO.File]::ReadAllBytes($exe)
$m = [regex]::Match(($r = Run $exe '/payload').Out, 'program (\d+) bytes[\s\S]*source\.zip (\d+)')
$off = [int64]$m.Groups[1].Value; $len = [int64]$m.Groups[2].Value
[IO.File]::WriteAllBytes($tmpzip, $bytes[$off..($off + $len - 1)])
$z = [IO.Compression.ZipFile]::OpenRead($tmpzip)
$names = @($z.Entries | ForEach-Object { $_.FullName }); $z.Dispose(); Remove-Item $tmpzip
$bad = @($names | Where-Object { $_ -match '^(orig|build|dist|tools|\.venv)/' -or $_ -match '\.(iso|elf|bin)$' -or $_ -match 'SCUS_' })
Check ($names.Count -gt 100 -and $bad.Count -eq 0 -and ($names -contains 'scripts/build_game.py') -and ($names -contains 'version.txt')) `
    "payload: $($names.Count) files, only recomp/runtime/build scripts (no game data)"

# ---- disc check --------------------------------------------------------------------------------------
$isos = Join-Path $out 'isos'
& $py (Join-Path $PSScriptRoot 'make_isos.py') $isos
foreach ($line in Get-Content (Join-Path $isos 'cases.txt')) {
    $name, $code = $line -split ' '
    $r = Run $exe "/checkdisc `"$(Join-Path $isos $name)`""
    Check ($r.Out -match "^$code ") "disc $name -> $($r.Out)"
}
$realIso = Get-ChildItem (Join-Path $main 'orig\disc') -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1
if ($realIso) {
    $r = Run $exe "/checkdisc `"$($realIso.FullName)`""
    Check ($r.Code -eq 0 -and $r.Out -match '^0 ') "the owner's image -> $($r.Out)"
} else { Write-Output "skip the owner's image (none in orig\disc)" }

# ---- downloads ---------------------------------------------------------------------------------------
$srv = Join-Path $out 'srv'; $dl = Join-Path $out 'dl'
New-Item -ItemType Directory -Force $srv, $dl | Out-Null
$uvzip = Join-Path $main 'tools\_downloads\uv-x86_64-pc-windows-msvc.zip'
if (-not (Test-Path $uvzip)) { Write-Output "FAIL: $uvzip missing (setup.ps1)"; exit 1 }
Copy-Item $uvzip $srv
$file = 'uv-x86_64-pc-windows-msvc.zip'
$sha = (Get-FileHash (Join-Path $srv $file) -Algorithm SHA256).Hash.ToLower()
$size = (Get-Item (Join-Path $srv $file)).Length
$portFile = Join-Path $out 'port.txt'; $srvLog = Join-Path $out 'server.log'
Remove-Item $portFile, $srvLog -ErrorAction SilentlyContinue
$server = Start-Process $py -ArgumentList "-I `"$(Join-Path $PSScriptRoot 'range_server.py')`" `"$srv`" `"$portFile`" `"$srvLog`"" -PassThru -WindowStyle Hidden
try {
    for ($i = 0; $i -lt 50 -and -not (Test-Path $portFile); $i++) { Start-Sleep -Milliseconds 100 }
    $port = (Get-Content $portFile -Raw).Trim()
    $base = "http://127.0.0.1:$port"
    $target = Join-Path $dl $file

    $r = Run $exe "/download $base/files/$file `"$target`" $sha $size"
    Check ($r.Code -eq 0 -and (Get-FileHash $target).Hash.ToLower() -eq $sha) "download complete, SHA-256 OK ($($r.Out))"
    Remove-Item $target

    Clear-Content $srvLog
    $r = Run $exe "/download $base/drop/$file `"$target`" $sha $size"
    $log = Get-Content $srvLog
    Check ($r.Code -eq 0 -and (Get-FileHash $target).Hash.ToLower() -eq $sha -and ($log | Where-Object { $_ -match 'range=bytes=\d+-' })) `
        "interrupted half-way, resumed with a Range request ($(@($log).Count) requests)"
    Remove-Item $target

    # a partial file from an earlier (cancelled) run: continued, not restarted
    $part = "$target.part"
    $fs = [IO.File]::OpenRead((Join-Path $srv $file)); $buf = New-Object byte[] 5000000; [void]$fs.Read($buf, 0, $buf.Length); $fs.Close()
    [IO.File]::WriteAllBytes($part, $buf)
    Clear-Content $srvLog
    $r = Run $exe "/download $base/files/$file `"$target`" $sha $size"
    Check ($r.Code -eq 0 -and (Get-FileHash $target).Hash.ToLower() -eq $sha -and ((Get-Content $srvLog) -match 'range=bytes=5000000-')) `
        "a partial download is continued at 5,000,000 bytes"
    Remove-Item $target

    [IO.File]::WriteAllBytes($part, $buf)
    $r = Run $exe "/download $base/norange/$file `"$target`" $sha $size"
    Check ($r.Code -eq 0 -and (Get-FileHash $target).Hash.ToLower() -eq $sha) "a server without Range support: downloaded again from the start"
    Remove-Item $target

    $r = Run $exe "/download $base/files/missing.zip `"$target`""
    Check ($r.Code -eq 1 -and $r.Out -match '404') "404 -> $($r.Out)"

    $r = Run $exe "/download $base/files/$file `"$target`" $('0' * 64) $size"
    Check ($r.Code -eq 1 -and $r.Out -match 'SHA-256 mismatch' -and -not (Test-Path $target) -and -not (Test-Path $part)) "wrong SHA-256 -> refused, nothing kept"
} finally {
    Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
}
$r = Run $exe "/download http://127.0.0.1:$port/files/$file `"$target`""
Check ($r.Code -eq 1 -and $r.Out -match 'could not connect') "no server -> $($r.Out)"

# ---- the wizard, not shown --------------------------------------------------------------------------
if ($realIso) {
    $uilog = Join-Path $out 'uitest.log'
    Remove-Item $uilog -ErrorAction SilentlyContinue
    $r = Run $exe "/uitest `"/iso=$($realIso.FullName)`" `"/badiso=$(Join-Path $isos 'pal.iso')`" `"/dir=$(Join-Path $out 'uitest-dir')`" `"/log=$uilog`" /regkey=Software\Sly2SetupTest\Uninstall\Sly2-PC"
    $ul = Get-Content $uilog
    Check ($r.Code -eq 0 -and -not ($ul -cmatch 'FAIL:') -and ($ul -match '0 failure')) "/uitest: $(@($ul -match '\[uitest\] ok').Count) checks passed"
    Check (-not (Test-Path (Join-Path $out 'uitest-dir'))) "/uitest leaves no install folder behind"
}

# ---- a damaged setup file; Uninstall.exe removes the partial install --------------------------------
if ($realIso) {
    $bad = Join-Path $out 'Sly2-Setup-damaged.exe'
    $b = [IO.File]::ReadAllBytes($exe); $b[$off + 1000] = $b[$off + 1000] -bxor 0xFF; [IO.File]::WriteAllBytes($bad, $b)
    $dir = Join-Path $out 'tamper'; $tlog = Join-Path $out 'tamper.log'
    $key = 'Software\Sly2SetupTest\Uninstall\Sly2-PC'
    Remove-Item $tlog -ErrorAction SilentlyContinue
    $ov = "/regkey=$key `"/startmenu=$(Join-Path $out 'sm')`" `"/desktopdir=$(Join-Path $out 'desk')`" `"/userdata=$(Join-Path $out 'ud')`""
    $r = Run $bad "/silent `"/iso=$($realIso.FullName)`" `"/dir=$dir`" $ov `"/log=$tlog`""
    Check ($r.Code -eq 1 -and ((Get-Content $tlog) -match 'damaged')) "a damaged payload is refused"
    Check ((Test-Path "HKCU:\$key") -and (Test-Path (Join-Path $dir 'Uninstall.exe'))) "the partial install is registered (removable)"
    $r = Run (Join-Path $dir 'Uninstall.exe') '/silent'
    for ($i = 0; $i -lt 50 -and (Test-Path $dir); $i++) { Start-Sleep -Milliseconds 200 }
    Check ($r.Code -eq 0 -and -not (Test-Path $dir) -and -not (Test-Path "HKCU:\$key")) "Uninstall.exe /silent removed it (folder and registry entry)"
    Remove-Item $bad
    if (Test-Path 'HKCU:\Software\Sly2SetupTest') { Remove-Item -Recurse 'HKCU:\Software\Sly2SetupTest' }
}

if ($script:fails) { Write-Output "$($script:fails) installer test(s) FAILED"; exit 1 }
Write-Output "installer unit tests passed"
exit 0
