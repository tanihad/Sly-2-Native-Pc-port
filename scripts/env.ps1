# Dot-source this to use the project toolchain in the current shell:  . .\scripts\env.ps1
$root  = Split-Path $PSScriptRoot -Parent
$tools = Join-Path $root 'tools'

$env:JAVA_HOME             = Join-Path $tools 'jdk'
$env:GHIDRA_INSTALL_DIR    = Join-Path $tools 'ghidra'
$env:UV_PYTHON_INSTALL_DIR = Join-Path $tools 'python'
$env:UV_CACHE_DIR          = Join-Path $tools 'uv-cache'

$env:PATH = (@(
    (Join-Path $root  '.venv\Scripts'),
    (Join-Path $tools 'jdk\bin'),
    (Join-Path $tools 'git\cmd'),
    (Join-Path $tools 'ghidra\support'),
    (Join-Path $tools 'uv'),
    (Join-Path $tools 'llvm-mingw\bin'),
    $tools                                   # objdiff-cli.exe
) -join ';') + ';' + $env:PATH
