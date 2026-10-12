# Build scripts/gs_census.c and run it over GS recordings, one report per recording.
#   powershell -ExecutionPolicy Bypass -File scripts\gs_census.ps1 -Rec a.gsrec[,b.gsrec...] [-Out build\census] [-Verbose]
#   powershell -ExecutionPolicy Bypass -File scripts\gs_census.ps1 -All       # the known recordings (see $known)
# Reports land in <Out>\<name>.txt; the tool itself is in <Out>\gs_census.exe.
param([string[]]$Rec = @(), [string]$Out = "", [switch]$All, [switch]$Verb)
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {            # worktree: tools live in the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$clang = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
if (-not $Out) { $Out = Join-Path $root 'build\census' }
New-Item -ItemType Directory -Force $Out | Out-Null
$exe = Join-Path $Out 'gs_census.exe'
& $clang -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter (Join-Path $PSScriptRoot 'gs_census.c') -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'gs_census build failed'; exit 1 }
$wt = Split-Path $root -Parent                                           # ..\sly2-worktrees
$known = @(
    "$wt\fix-perf\build\rv_prompt.gsrec", "$wt\fix-perf\build\rv_intro.gsrec", "$wt\fix-perf\build\title.gsrec",
    "$wt\fix-perf\build\brief.gsrec", "$wt\ep3\build\gsrec\hub.gsrec", "$wt\ep3\build\gsrec\recon_int.gsrec",
    "$wt\ep3\build\gsrec\rope.gsrec", "$wt\ep3\build\gsrec\brief.gsrec", "$wt\ep5\build\gsrec\hub_street.gsrec",
    "$wt\ep6\build\gsrec\rc_battle.gsrec", "$wt\ep7\build\boss.gsrec", "$wt\ep8\build\gsrec\hub.gsrec",
    "$wt\ep8\build\gsrec\boss.gsrec")
if ($All) { $Rec += $known }
foreach ($r in $Rec) {
    if (-not (Test-Path $r)) { Write-Host "missing $r"; continue }
    $full = (Resolve-Path $r).Path
    $tag = 'rec'
    if ($full.StartsWith($wt, [StringComparison]::OrdinalIgnoreCase)) {    # worktree name
        $tag = ($full.Substring($wt.Length).TrimStart('\') -split '\\')[0]
    }
    $name = "$tag-" + [IO.Path]::GetFileNameWithoutExtension($r) + '.txt'
    $args = @($r); if ($Verb) { $args += '-v' }
    Write-Host "census $r -> $name"
    & $exe @args | Out-File -Encoding utf8 (Join-Path $Out $name)
}
