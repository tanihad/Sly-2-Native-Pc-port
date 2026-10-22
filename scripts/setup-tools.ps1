# Sets up the portable toolchain in tools\ and the Python environment in .venv\ (nothing is installed
# system-wide; deleting the repo folder removes everything). Safe to re-run: skips what is present.
#
#   powershell -ExecutionPolicy Bypass -File setup.ps1                  # download (about 260 MB)
#   powershell -ExecutionPolicy Bypass -File setup.ps1 -From <dir>      # offline: archives from <dir>
#   powershell -ExecutionPolicy Bypass -File setup.ps1 -DryRun          # print what would be fetched
#   powershell -ExecutionPolicy Bypass -File setup.ps1 -WithRE          # + Ghidra, JDK, objdiff, splat
#
# What the game build needs (pinned versions; every archive is checked against its SHA-256 before it
# is extracted - the values are the digests GitHub publishes for these release assets):
#   llvm-mingw (clang, Windows headers/libraries)   tools\llvm-mingw
#   uv (Python manager)                             tools\uv
#   Python 3.13 (uv-managed, checksum-verified by uv) tools\python, venv in .venv
#   rabbitizer + pyelftools (requirements.txt, --require-hashes)
#   MinGit (only when no git is on PATH; used for the build's version number) tools\git
#
# -From <dir>: install without network access. <dir> is searched for the archives by file name, and
# so are <dir>\tools\_downloads (another checkout of this repo). Python can come from a
# python-build-standalone install in <dir>\python or <dir>\tools\python, and the packages from
# wheel files in <dir>\wheels or a uv cache in <dir>\uv-cache or <dir>\tools\uv-cache. <dir> is only
# read. With -From, nothing is downloaded: whatever is missing is reported and setup fails.
#
# -WithRE: the reverse-engineering tools (not needed to build or play): Eclipse Temurin JDK 21, Ghidra
# + the Emotion Engine plugin, objdiff-cli, and splat/capstone (requirements-re.txt). These follow the
# latest releases and are checked against the digests their publishers list at download time.
[CmdletBinding()]
param(
    [string]$From = '',
    [switch]$DryRun,
    [switch]$WithRE
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # PS 5.1 progress bar makes downloads very slow
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$root  = Split-Path $PSScriptRoot -Parent
$tools = Join-Path $root 'tools'
$dl    = Join-Path $tools '_downloads'
$venv  = Join-Path $root '.venv'
if ($From) {
    if (-not (Test-Path $From -PathType Container)) { throw "-From: folder not found: $From" }
    $From = (Resolve-Path $From).Path
}

# ---- pinned toolchain for the game build ---------------------------------------------------------
$PythonVersion = '3.13.16'
$Pinned = @(
    [pscustomobject]@{ Name = 'llvm-mingw'; Dir = 'llvm-mingw'; Flatten = $true
        File = 'llvm-mingw-20261006-ucrt-x86_64.zip'
        Url  = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-x86_64.zip'
        Sha256 = '317492C456AA27EE607A5919F1D2D38DCDC1112516A24D0BF4B00D078F52D17A'; Check = 'bin\clang.exe' },
    [pscustomobject]@{ Name = 'uv 0.13.0'; Dir = 'uv'; Flatten = $false
        File = 'uv-x86_64-pc-windows-msvc.zip'
        Url  = 'https://github.com/astral-sh/uv/releases/download/0.13.0/uv-x86_64-pc-windows-msvc.zip'
        Sha256 = '088962F9E7B7BD9EA740C04C650B2A21C8928C345BD99AC24350DC924DBA656C'; Check = 'uv.exe' }
)
$MinGit = [pscustomobject]@{ Name = 'MinGit 2.56.0.2'; Dir = 'git'; Flatten = $false
        File = 'MinGit-2.56.0.2-64-bit.zip'
        Url  = 'https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.2/MinGit-2.56.0.2-64-bit.zip'
        Sha256 = 'DA35E72AA21C005A5A0D298CFBAE110BC1609A815730EA0DDE84B01A1B3CD3BE'; Check = 'cmd\git.exe' }

function Find-Local($file) {
    if (-not $From) { return $null }
    foreach ($d in @($From, (Join-Path $From '_downloads'), (Join-Path $From 'tools\_downloads'))) {
        $p = Join-Path $d $file
        if (Test-Path $p -PathType Leaf) { return $p }
    }
    $null
}

function Test-Sha256($path, $sha256) {
    $actual = (Get-FileHash $path -Algorithm SHA256).Hash
    if ($actual -ne $sha256.ToUpper()) { throw "Checksum mismatch for $path (expected $sha256, got $actual)" }
}

# Returns the path of a verified archive: tools\_downloads\<file> (downloaded or already there) or the
# copy found under -From (verified in place, never modified).
function Get-Archive($file, $url, $sha256) {
    $cached = Join-Path $dl $file
    $local  = Find-Local $file
    if ($DryRun) {
        if (Test-Path $cached) { $src = "cached $cached" } elseif ($local) { $src = "local $local" }
        elseif ($From) { $src = 'MISSING (offline)' } else { $src = "download $url" }
        Write-Host "  $file  <- $src"
        Write-Host "    SHA-256 $(if ($sha256) { $sha256 } else { '(publisher digest at download time)' })"
        return $null
    }
    foreach ($p in @($cached, $local)) {
        if ($p -and (Test-Path $p)) {
            if ($sha256) { Test-Sha256 $p $sha256 }
            Write-Host "  $file (SHA-256 OK, $(if ($p -eq $cached) { 'cached' } else { 'from ' + (Split-Path $p -Parent) }))"
            return $p
        }
    }
    if ($From) { throw "-From: $file not found in $From (or its _downloads / tools\_downloads)" }
    New-Item -ItemType Directory -Force $dl | Out-Null
    Write-Host "  downloading $url"
    $part = "$cached.part"
    Invoke-WebRequest -UseBasicParsing $url -OutFile $part
    if ($sha256) {
        try { Test-Sha256 $part $sha256 } catch { Remove-Item $part; throw }
        Write-Host "  SHA-256 OK"
    } else { Write-Host "  WARNING: no published checksum for $file" }
    Move-Item $part $cached
    $cached
}

function Expand-Into($zip, $dest, [bool]$flatten) {
    $tmp = "$dest.tmp"
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'          # Windows 10 1803+: much faster than Expand-Archive
    if (Test-Path $tar) { & $tar -xf $zip -C $tmp; if ($LASTEXITCODE) { throw "extracting $zip failed" } }
    else { Expand-Archive $zip $tmp }
    $items = @(Get-ChildItem $tmp)
    if ($flatten -and $items.Count -eq 1 -and $items[0].PSIsContainer) {   # single top-level folder
        Move-Item $items[0].FullName $dest
        Remove-Item -Recurse -Force $tmp
    } else { Move-Item $tmp $dest }
}

function Install-Tool($t) {
    $dest = Join-Path $tools $t.Dir
    if (Test-Path (Join-Path $dest $t.Check)) { Write-Host "$($t.Name): present"; return }
    Write-Host "$($t.Name):"
    $zip = Get-Archive $t.File $t.Url $t.Sha256
    if ($DryRun) { return }
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }      # an interrupted earlier run
    Expand-Into $zip $dest $t.Flatten
    if (-not (Test-Path (Join-Path $dest $t.Check))) { throw "$($t.Name): $($t.Check) missing after extraction" }
}

if ($DryRun) { Write-Host "Dry run: nothing is downloaded, extracted or installed.`n" }
New-Item -ItemType Directory -Force $tools | Out-Null
foreach ($t in $Pinned) { Install-Tool $t }

$git = Get-Command git -ErrorAction SilentlyContinue
if ((Test-Path (Join-Path $tools 'git\cmd\git.exe')) -or ($git -and -not $git.Source.StartsWith($tools))) {
    Write-Host "git: present"
} else { Install-Tool $MinGit }

# ---- Python 3.13 + .venv ---------------------------------------------------------------------------
$uv = Join-Path $tools 'uv\uv.exe'
$env:UV_PYTHON_INSTALL_DIR = Join-Path $tools 'python'
$env:UV_CACHE_DIR          = Join-Path $tools 'uv-cache'
$env:UV_PYTHON_DOWNLOADS   = 'manual'                 # uv never fetches a Python by itself
$venvPy = Join-Path $venv 'Scripts\python.exe'
$req   = Join-Path $root 'requirements.txt'
$reqRE = Join-Path $root 'requirements-re.txt'

function Find-LocalPython {
    if (-not $From) { return $null }
    foreach ($d in @((Join-Path $From 'python'), (Join-Path $From 'tools\python'))) {
        $p = Get-ChildItem $d -Directory -Filter "cpython-$PythonVersion-windows-x86_64-*" -ErrorAction SilentlyContinue |
             Where-Object { Test-Path (Join-Path $_.FullName 'python.exe') } | Select-Object -First 1
        if ($p) { return $p.FullName }
    }
    $null
}

if ($DryRun) {
    Write-Host "Python ${PythonVersion}:"
    if (Test-Path $venvPy) { Write-Host "  .venv present" }
    elseif (Find-LocalPython) { Write-Host "  copy $(Find-LocalPython) -> tools\python, then uv venv .venv" }
    elseif ($From) { Write-Host "  MISSING (offline): no cpython-$PythonVersion in $From\python or $From\tools\python" }
    else { Write-Host "  uv python install $PythonVersion (python-build-standalone; uv verifies its SHA-256), uv venv .venv" }
    Write-Host "Packages: uv pip install --require-hashes -r requirements.txt$(if ($From) { ' --offline' })"
    if ($WithRE) { Write-Host "  + requirements-re.txt (-WithRE)" }
} else {
    if (-not (Test-Path $venvPy)) {
        $py = $null
        $localPy = Find-LocalPython
        if ($localPy) {
            $dest = Join-Path $env:UV_PYTHON_INSTALL_DIR (Split-Path $localPy -Leaf)
            if (-not (Test-Path (Join-Path $dest 'python.exe'))) {
                Write-Host "Python ${PythonVersion}: copying $localPy"
                New-Item -ItemType Directory -Force $env:UV_PYTHON_INSTALL_DIR | Out-Null
                Copy-Item -Recurse $localPy $dest
            }
            $py = Join-Path $dest 'python.exe'
        } elseif ($From) {
            throw "-From: no Python $PythonVersion (cpython-$PythonVersion-windows-x86_64-*) in $From\python or $From\tools\python"
        } else {
            Write-Host "Python ${PythonVersion}:"
            & $uv python install $PythonVersion
            if ($LASTEXITCODE) { throw "uv python install failed" }
            $py = Get-ChildItem $env:UV_PYTHON_INSTALL_DIR -Directory -Filter "cpython-$PythonVersion-windows-x86_64-*" |
                  ForEach-Object { Join-Path $_.FullName 'python.exe' } | Where-Object { Test-Path $_ } | Select-Object -First 1
            if (-not $py) { throw "Python $PythonVersion not found in $($env:UV_PYTHON_INSTALL_DIR) after uv python install" }
        }
        & $uv venv $venv --python $py
        if ($LASTEXITCODE) { throw "uv venv failed" }
    } else { Write-Host "Python: .venv present" }

    $pipArgs = @('pip', 'install', '--python', $venvPy, '--require-hashes', '-r', $req)
    if ($From) {
        $pipArgs += '--offline'
        foreach ($c in @((Join-Path $From 'uv-cache'), (Join-Path $From 'tools\uv-cache'))) {
            if (Test-Path (Join-Path $c 'CACHEDIR.TAG')) {
                Write-Host "Packages: seeding the uv cache from $c"
                & robocopy $c $env:UV_CACHE_DIR /E /XF .lock /NFL /NDL /NJH /NJS /NP | Out-Null   # merge into it
                if ($LASTEXITCODE -ge 8) { throw "copying the uv cache from $c failed" }
                break
            }
        }
        $wheels = Join-Path $From 'wheels'
        if (Test-Path $wheels) { $pipArgs += @('--find-links', $wheels) }
    }
    & $uv @pipArgs
    if ($LASTEXITCODE) { throw "installing requirements.txt failed" }
}

# ---- optional reverse-engineering tools ----------------------------------------------------------
if ($WithRE) {
    function Get-GitHubAsset($repo, $pattern, $tag) {
        $api = if ($tag) { "https://api.github.com/repos/$repo/releases/tags/$tag" } else { "https://api.github.com/repos/$repo/releases/latest" }
        $rel = Invoke-RestMethod $api
        $asset = $rel.assets | Where-Object { $_.name -like $pattern } | Select-Object -First 1
        if (-not $asset) { throw "No asset matching '$pattern' in $repo $($rel.tag_name)" }
        [pscustomobject]@{ Name = $asset.name; Url = $asset.browser_download_url; Sha256 = ($asset.digest -replace '^sha256:', '') }
    }
    if ($From -or $DryRun) {
        Write-Host "-WithRE: the RE tools follow the latest releases and are fetched online only (skipped with -From / -DryRun)"
    } else {
        $GhidraVersion = '12.1.4'
        $jdkDir = Join-Path $tools 'jdk'
        if (-not (Test-Path $jdkDir)) {
            $info = (Invoke-RestMethod 'https://api.adoptium.net/v3/assets/latest/21/hotspot?architecture=x64&image_type=jdk&os=windows&vendor=eclipse') | Select-Object -First 1
            $z = Get-Archive $info.binary.package.name $info.binary.package.link $info.binary.package.checksum
            Expand-Into $z $jdkDir $true
        }
        $ghidraDir = Join-Path $tools 'ghidra'
        if (-not (Test-Path $ghidraDir)) {
            $a = Get-GitHubAsset 'NationalSecurityAgency/ghidra' '*.zip' "Ghidra_${GhidraVersion}_build"
            $z = Get-Archive $a.Name $a.Url $a.Sha256
            Expand-Into $z $ghidraDir $true
        }
        $eeDest = Join-Path $ghidraDir 'Ghidra\Extensions\ghidra-emotionengine-reloaded'
        if (-not (Test-Path $eeDest)) {
            $a = Get-GitHubAsset 'chaoticgd/ghidra-emotionengine-reloaded' "ghidra_${GhidraVersion}_PUBLIC_*.zip"
            $z = Get-Archive $a.Name $a.Url $a.Sha256
            Expand-Archive $z (Join-Path $ghidraDir 'Ghidra\Extensions')
        }
        $objdiff = Join-Path $tools 'objdiff-cli.exe'
        if (-not (Test-Path $objdiff)) {
            $a = Get-GitHubAsset 'encounter/objdiff' 'objdiff-cli-windows-x86_64.exe'
            Copy-Item (Get-Archive $a.Name $a.Url $a.Sha256) $objdiff
        }
        & $uv pip install --python $venvPy -r $reqRE
        if ($LASTEXITCODE) { throw "installing requirements-re.txt failed" }
    }
}

if ($DryRun) { Write-Host "`nDry run complete." }
else { Write-Host "`nSetup complete. Next: put your disc image (.iso) into orig\disc\, then run build.ps1." }
