# One-time setup: the build toolchain in tools\ and Python in .venv\ (README.md, "Build and play").
#   powershell -ExecutionPolicy Bypass -File setup.ps1 [-From <dir>] [-DryRun] [-WithRE]
# Details and options: scripts\setup-tools.ps1.
[CmdletBinding()]
param([string]$From = '', [switch]$DryRun, [switch]$WithRE)
& (Join-Path $PSScriptRoot 'scripts\setup-tools.ps1') @PSBoundParameters
