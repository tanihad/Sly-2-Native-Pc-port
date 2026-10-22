# Project paths for PowerShell scripts, wherever the repo is cloned. Dot-source it:
#   . (Join-Path $PSScriptRoot 'project.ps1')          # from scripts\
#   . (Join-Path $root 'scripts\project.ps1')           # from elsewhere, $root = the repo root
# Sets:
#   $ProjectRoot    this checkout (or git worktree)
#   $MainRoot       the checkout holding the git-ignored tools\, orig\ and .venv\ (for a worktree: the
#                   main checkout, found through the worktree's .git file; otherwise $ProjectRoot)
#   $ProjectPython  $MainRoot\.venv\Scripts\python.exe
#   $ProjectClang   $MainRoot\tools\llvm-mingw\bin\clang.exe
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$MainRoot = $ProjectRoot
if (-not (Test-Path (Join-Path $ProjectRoot 'tools\llvm-mingw'))) {
    $gitfile = Join-Path $ProjectRoot '.git'
    if (Test-Path $gitfile -PathType Leaf) {        # "gitdir: <main>/.git/worktrees/<name>"
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $MainRoot = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$ProjectPython = Join-Path $MainRoot '.venv\Scripts\python.exe'
$ProjectClang = Join-Path $MainRoot 'tools\llvm-mingw\bin\clang.exe'
