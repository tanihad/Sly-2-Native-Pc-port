# Hash all of GS memory after every VBlank for every corpus recording (tests/gpu/corpus.txt), with
# the software renderer replaying synchronously, and optionally compare with an earlier run. Used to
# prove that a refactor of the software GS changes nothing (notes/d3d11-renderer.md, M1).
#   powershell -ExecutionPolicy Bypass -File scripts\gs_vram_hashes.ps1 -Out build\m1\vram_before
#   powershell -ExecutionPolicy Bypass -File scripts\gs_vram_hashes.ps1 -Out build\m1\vram_after -Compare build\m1\vram_before
#   [-Only ep2_hub,title] [-Backend sw|hwdry] [-Runtime <dir>] [-CorpusRoot <dir>]
# Output per recording: <Out>\<name>.txt ("vblank hash" lines) and <name>.log (the replay's stderr).
# -Backend hwdry runs the GPU frontend alongside the software renderer: the hashes must not change.
param([Parameter(Mandatory = $true)][string]$Out, [string]$Compare = "", [string[]]$Only = @(),
      [ValidateSet('sw', 'hwdry')][string]$Backend = 'sw', [string]$Runtime = "", [string]$CorpusRoot = "")
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
if (-not $CorpusRoot) { $CorpusRoot = Join-Path (Split-Path $main -Parent) 'sly2-worktrees' }
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
if ($Compare -and -not [IO.Path]::IsPathRooted($Compare)) { $Compare = Join-Path $root $Compare }
New-Item -ItemType Directory -Force $Out | Out-Null
$recs = Get-Content (Join-Path $root 'tests\gpu\corpus.txt') | Where-Object { $_ -and -not $_.StartsWith('#') } |
    ForEach-Object { ($_ -split '\|')[0].Trim() }
$failed = 0
$n = 0
foreach ($rel in $recs) {
    $name = ($rel -replace '\\build\\gsrec\\', '_' -replace '\\build\\', '_' -replace '\.gsrec$', '')
    if ($Only.Count -and -not ($Only | Where-Object { $name -like "*$_*" })) { continue }
    $rec = Join-Path $CorpusRoot $rel
    if (-not (Test-Path $rec)) { Write-Host "$name : MISSING $rec"; $failed++; continue }
    $file = Join-Path $Out "$name.txt"
    $log = Join-Path $Out "$name.log"
    $t0 = Get-Date
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'gs_replay.ps1'), '-Rec', $rec,
           '-Out', (Join-Path $Out 'bin'), '-Bench', '-Workers', '0', '-VramHash', $file, '-Log', $log, '-Backend', $Backend)
    if ($Runtime) { $a += @('-Runtime', $Runtime) }
    & powershell @a | Out-Null
    $code = $LASTEXITCODE
    $secs = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    $n++
    $line = (Get-Content $log -ErrorAction SilentlyContinue | Where-Object { $_ -like 'vram-hash*' }) -join ''
    $status = if ($code -eq 0 -and $line) { 'ok' } else { 'FAIL'; $failed++ }
    $cmp = ''
    if ($Compare -and $status -eq 'ok') {
        $before = Join-Path $Compare "$name.txt"
        if (-not (Test-Path $before)) { $cmp = 'no earlier run'; $failed++ }
        else {
            $x = Get-Content $before; $y = Get-Content $file
            $diff = 0; $first = ''
            for ($i = 0; $i -lt [math]::Max($x.Count, $y.Count); $i++) {
                if ($x[$i] -ne $y[$i]) { $diff++; if (-not $first) { $first = "first at line $($i + 1): '$($x[$i])' vs '$($y[$i])'" } }
            }
            if ($diff) { $cmp = "DIFFERS on $diff of $($x.Count) VBlanks, $first"; $failed++ } else { $cmp = "identical ($($x.Count) VBlanks)" }
        }
    }
    Write-Host ("{0,-28} {1,-4} {2} {3,6} s {4}" -f $name, $status, $line, $secs, $cmp)
}
if ($failed) { Write-Host "gs_vram_hashes: $failed problem(s)"; exit 1 }
Write-Host "gs_vram_hashes: $n recording(s) done"
exit 0
