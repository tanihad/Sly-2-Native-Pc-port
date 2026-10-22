# Builds the game: dist\Sly2\sly2.exe (README.md, "Build and play").
#   powershell -ExecutionPolicy Bypass -File build.ps1 [-Iso <disc image>] [-Dest <folder>]
#
# Needs setup.ps1 to have run once, and your own Sly 2: Band of Thieves (USA) disc image: the first
# *.iso in orig\disc\, or -Iso <path>. The game's executable is taken from the image (into
# orig\SCUS_973.16, checked by SHA-256), recompiled to C and compiled with clang (5-15 minutes the
# first time, depending on the CPU; later builds reuse unchanged objects).
[CmdletBinding()]
param([string]$Iso = '', [string]$Dest = '')
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$py = Join-Path $root '.venv\Scripts\python.exe'
if (-not (Test-Path $py) -or -not (Test-Path (Join-Path $root 'tools\llvm-mingw\bin\clang.exe'))) {
    Write-Host "The toolchain is not set up yet. Run first:  powershell -ExecutionPolicy Bypass -File setup.ps1"
    exit 1
}
. (Join-Path $root 'scripts\env.ps1')

$extract = @((Join-Path $root 'scripts\extract_elf.py'))
if ($Iso) { $extract += @('--iso', $Iso) }        # (only used while orig\SCUS_973.16 is missing)
& $py @extract
if ($LASTEXITCODE) { exit 1 }

$pkg = @((Join-Path $root 'scripts\package.py'))
if ($Dest) { $pkg += @('--dest', $Dest) }
& $py @pkg
if ($LASTEXITCODE) { Write-Host "Build failed (the messages above say why)."; exit 1 }

$out = if ($Dest) { $Dest } else { Join-Path $root 'dist\Sly2' }
Write-Host ""
Write-Host "Done: $(Join-Path $out 'sly2.exe')"
Write-Host "Start it; on the first start it asks for your disc image. The folder can be copied anywhere"
Write-Host "(it contains code recompiled from your disc: do not share it publicly)."
