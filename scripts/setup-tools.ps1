# Downloads the portable toolchain into tools/. Safe to re-run: skips anything already present.
# Every archive is checked against the SHA-256 published by its official source before extracting.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # PS 5.1 progress bar makes downloads very slow

$root  = Split-Path $PSScriptRoot -Parent
$tools = Join-Path $root 'tools'
$dl    = Join-Path $tools '_downloads'
New-Item -ItemType Directory -Force $dl | Out-Null

$GhidraVersion = '12.1.4'

function Get-GitHubAsset($repo, $pattern) {
    $rel = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest"
    $asset = $rel.assets | Where-Object { $_.name -like $pattern } | Select-Object -First 1
    if (-not $asset) { throw "No asset matching '$pattern' in $repo $($rel.tag_name)" }
    [pscustomobject]@{ Name = $asset.name; Url = $asset.browser_download_url
                       Sha256 = ($asset.digest -replace '^sha256:', ''); Tag = $rel.tag_name }
}

function Save-Verified($name, $url, $sha256) {
    $out = Join-Path $dl $name
    if (-not (Test-Path $out)) {
        Write-Host "Downloading $name"
        Invoke-WebRequest -UseBasicParsing $url -OutFile $out
    }
    if ($sha256) {
        $actual = (Get-FileHash $out -Algorithm SHA256).Hash
        if ($actual -ne $sha256.ToUpper()) {
            Remove-Item $out
            throw "Checksum mismatch for $name (expected $sha256, got $actual)"
        }
        Write-Host "  SHA-256 OK"
    } else {
        Write-Host "  WARNING: no published checksum for $name"
    }
    $out
}

function Expand-Into($zip, $dest) {
    $tmp = "$dest.tmp"
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Expand-Archive $zip $tmp
    # Flatten a single top-level folder so paths stay predictable
    $items = Get-ChildItem $tmp
    if ($items.Count -eq 1 -and $items[0].PSIsContainer) { Move-Item $items[0].FullName $dest }
    else { Move-Item $tmp $dest }
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
}

# --- Git (MinGit: portable, no installer) ---
$gitDir = Join-Path $tools 'git'
if (-not (Test-Path $gitDir)) {
    $a = Get-GitHubAsset 'git-for-windows/git' 'MinGit-*-64-bit.zip'
    $z = Save-Verified $a.Name $a.Url $a.Sha256
    New-Item -ItemType Directory -Force $gitDir | Out-Null
    Expand-Archive $z $gitDir
}

# --- Java: Eclipse Temurin JDK 21 (required by Ghidra) ---
$jdkDir = Join-Path $tools 'jdk'
if (-not (Test-Path $jdkDir)) {
    $info = (Invoke-RestMethod 'https://api.adoptium.net/v3/assets/latest/21/hotspot?architecture=x64&image_type=jdk&os=windows&vendor=eclipse') |
            Select-Object -First 1
    $pkg = $info.binary.package
    $z = Save-Verified $pkg.name $pkg.link $pkg.checksum
    Expand-Into $z $jdkDir
}

# --- Ghidra ---
$ghidraDir = Join-Path $tools 'ghidra'
if (-not (Test-Path $ghidraDir)) {
    $rel = Invoke-RestMethod "https://api.github.com/repos/NationalSecurityAgency/ghidra/releases/tags/Ghidra_${GhidraVersion}_build"
    $asset = $rel.assets | Where-Object { $_.name -like '*.zip' } | Select-Object -First 1
    $z = Save-Verified $asset.name $asset.browser_download_url ($asset.digest -replace '^sha256:', '')
    Expand-Into $z $ghidraDir
}

# --- Emotion Engine (PS2 R5900) plugin, build matching the Ghidra version ---
$eeZip = Get-ChildItem $dl -Filter "ghidra_${GhidraVersion}_PUBLIC_*emotionengine*.zip" -ErrorAction SilentlyContinue
if (-not $eeZip) {
    $a = Get-GitHubAsset 'chaoticgd/ghidra-emotionengine-reloaded' "ghidra_${GhidraVersion}_PUBLIC_*.zip"
    $null = Save-Verified $a.Name $a.Url $a.Sha256
}
$eeDest = Join-Path $ghidraDir 'Ghidra\Extensions\ghidra-emotionengine-reloaded'
if (-not (Test-Path $eeDest)) {
    $eeZip = Get-ChildItem $dl -Filter "ghidra_${GhidraVersion}_PUBLIC_*emotionengine*.zip" | Select-Object -First 1
    Expand-Archive $eeZip.FullName (Join-Path $ghidraDir 'Ghidra\Extensions')
}

# --- objdiff-cli ---
$objdiff = Join-Path $tools 'objdiff-cli.exe'
if (-not (Test-Path $objdiff)) {
    $a = Get-GitHubAsset 'encounter/objdiff' 'objdiff-cli-windows-x86_64.exe'
    $f = Save-Verified $a.Name $a.Url $a.Sha256
    Copy-Item $f $objdiff
}

# --- uv (Python manager; gives the project its own Python 3.13, since splat's deps lack 3.15 wheels) ---
$uvDir = Join-Path $tools 'uv'
if (-not (Test-Path (Join-Path $uvDir 'uv.exe'))) {
    $a = Get-GitHubAsset 'astral-sh/uv' 'uv-x86_64-pc-windows-msvc.zip'
    $z = Save-Verified $a.Name $a.Url $a.Sha256
    New-Item -ItemType Directory -Force $uvDir | Out-Null
    Expand-Archive $z $uvDir -Force
}

# --- Python 3.13 venv with splat, pyelftools, capstone (kept inside the project) ---
$env:UV_PYTHON_INSTALL_DIR = Join-Path $tools 'python'
$env:UV_CACHE_DIR          = Join-Path $tools 'uv-cache'
$uv   = Join-Path $uvDir 'uv.exe'
$venv = Join-Path $root '.venv'
if (-not (Test-Path (Join-Path $venv 'Scripts\python.exe'))) {
    & $uv venv $venv --python 3.13
    if ($LASTEXITCODE -ne 0) { throw "uv venv failed" }
}
& $uv pip install --python (Join-Path $venv 'Scripts\python.exe') 'splat64[mips]' pyelftools capstone
if ($LASTEXITCODE -ne 0) { throw "uv pip install failed" }

Write-Host "Done."
