# Run a VU microprogram recording (PS2_VU_RECORD, runtime/vu.c) with the interpreter of a checkout,
# or with the recompiled microcode:
#   powershell -ExecutionPolicy Bypass -File scripts\vu_replay.ps1 -Rec build\x.vurec [-Runtime <dir>] [-Passes 5]
#       [-Compiled <dir with recomp/vu_recomp.py output, e.g. build\rel\vu>]
# Prints the fastest pass (ns per pair of real game microcode) and how many programs produced other
# results/timing than in the recording run (exit code 1 if any). Record with e.g.
#   $env:PS2_VU_RECORD="build\brief.vurec"; $env:PS2_VU_RECORD_FROM="2500"; $env:PS2_VU_RECORD_COUNT="3000"
# -Runtime: the runtime/ directory whose vu.c/vu_exec.c run it (default: this checkout's), e.g. an
# older version exported with `git archive`, for A/B timing on the same input.
# -Compiled: link the generated VU C files and start programs through vu_execute() (the runtime's
# dispatch: recompiled code where the micro memory matches, the interpreter elsewhere). The
# recording's results come from the interpreter, so this is the recompiler's exactness check.
param([Parameter(Mandatory = $true)][string]$Rec, [string]$Runtime = "", [int]$Passes = 5, [string]$Opt = "O2",
      [string]$Compiled = "")
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
if (-not $Runtime) { $Runtime = Join-Path $root 'runtime' }
$Runtime = (Resolve-Path $Runtime).Path
$out = Join-Path $root 'build\vu_replay'
New-Item -ItemType Directory -Force $out | Out-Null
$srcs = @((Join-Path $Runtime 'vu.c'), (Join-Path $Runtime 'vu_exec.c'), (Join-Path $PSScriptRoot 'vu_replay.c'))
$mode = "interp"
$tag = [Math]::Abs($Runtime.GetHashCode())
if ($Compiled) {
    $Compiled = (Resolve-Path $Compiled).Path
    $srcs += (Get-ChildItem $Compiled -Filter *.c | ForEach-Object { $_.FullName })
    $mode = "compiled"
    $tag = "c$([Math]::Abs(($Runtime + $Compiled).GetHashCode()))"
}
$exe = Join-Path $out "vu_replay_$tag.exe"
& $clang "-$Opt" -std=c11 -w -I $Runtime -I (Join-Path $root 'runtime') @srcs -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'vu_replay build failed'; exit 1 }
& $exe (Resolve-Path $Rec).Path $Passes $mode
exit $LASTEXITCODE
