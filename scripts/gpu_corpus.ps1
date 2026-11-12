# Where the GPU corpus recordings (tests/gpu/corpus.txt) are: dot-source this, then
#   $CorpusRoot = Resolve-GpuCorpus $CorpusRoot $main
#   if (-not $CorpusRoot) { exit 0 }                      # (a message was printed: skipped)
# Order: the script's -CorpusRoot, the SLY2_GPU_CORPUS environment variable, then the corpus folder on the
# development PC: sly2-corpus next to the main checkout (C:\Users\tanih\Projects\sly2-corpus), then the old
# location sly2-worktrees next to the main checkout (the recordings used to live inside worktrees' build folders,
# which were lost in a disk cleanup - the corpus must never live inside a build folder again). The recordings are
# made from the player's own disc and are never committed, so a fresh clone has none: the corpus tools then say so
# and exit 0 (skipped) instead of failing.
function Resolve-GpuCorpus([string]$given, [string]$main) {
    $c = $given
    if (-not $c) { $c = $env:SLY2_GPU_CORPUS }
    if (-not $c -and $main) {
        foreach ($d in @('sly2-corpus', 'sly2-worktrees')) {
            $p = Join-Path (Split-Path $main -Parent) $d
            if (Test-Path $p -PathType Container) { $c = $p; break }
        }
        if (-not $c) { $c = Join-Path (Split-Path $main -Parent) 'sly2-corpus' }
    }
    if ($c -and (Test-Path $c -PathType Container)) { return (Resolve-Path $c).Path }
    Write-Host "SKIPPED: no GPU corpus recordings ($(if ($c) { "$c not found" } else { 'none configured' })). Pass -CorpusRoot <dir> or set SLY2_GPU_CORPUS (tests/gpu/corpus.txt)."
    return $null
}
