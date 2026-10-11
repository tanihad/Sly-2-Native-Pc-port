# Unit tests for scripts/warp.py (episode/job warp saves). Usage: .\tests\unit\warp\run.ps1 [-UnitOnly]
param([switch]$UnitOnly)
$root = Split-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) -Parent
$main = $root
if (-not (Test-Path (Join-Path $root '.venv'))) {          # worktree: python lives in the main checkout
    $gitdir = ((Get-Content (Join-Path $root '.git') -Raw) -replace '^gitdir:\s*', '').Trim()
    $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
}
& (Join-Path $main '.venv\Scripts\python.exe') (Join-Path $PSScriptRoot 'test_warp.py')
exit $LASTEXITCODE
