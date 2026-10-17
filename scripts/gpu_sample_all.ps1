# FF vs REF on every sample of every target (M7 / M8 gate at scale, notes/d3d11-renderer.md 10.2 "FF vs REF at S > 1"):
# for each corpus recording (tests/gpu/corpus.txt) gs_replay --ref --scale N --sample-dump, then --mixed --scale N
# --sample-compare (every --sample-every VBlanks: 60, 8 on recordings shorter than 120 VBlanks); the compare deletes
# each dump it read. Budget per target and point: <= 2.5% colour words > 2, <= 0.1% > 16, <= 0.1% Z words.
#   powershell -ExecutionPolicy Bypass -File scripts\gpu_sample_all.ps1 [-Scale 3] [-Only name,...] [-Exe gs_replay.exe]
param([int]$Scale = 3, [string[]]$Only = @(), [string]$Exe = "", [string]$Out = "", [string]$CorpusRoot = "")
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path $PSScriptRoot -Parent
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {
    $gitdir = ((Get-Content (Join-Path $root '.git') -Raw) -replace '^gitdir:\s*', '').Trim()
    $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
}
if (-not $CorpusRoot) { $CorpusRoot = Join-Path (Split-Path $main -Parent) 'sly2-worktrees' }
if (-not $Out) { $Out = Join-Path $root "build\samples_s$Scale" }
New-Item -ItemType Directory -Force $Out | Out-Null
$replay = Join-Path $Out 'gs_replay.exe'
$recs = Get-Content (Join-Path $root 'tests\gpu\corpus.txt') | Where-Object { $_ -and -not $_.StartsWith('#') }
if ($Exe) { Copy-Item $Exe $replay -Force }
else {
    Remove-Item $replay -ErrorAction SilentlyContinue
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'gs_replay.ps1') -Rec (Join-Path $CorpusRoot (($recs[0] -split '\|')[0].Trim())) `
        -Backend hw -Ref -Bench -Out $Out -Log (Join-Path $Out 'build.log') | Out-Null
    if (-not (Test-Path $replay)) { Write-Host 'gs_replay build failed'; exit 1 }
}
$failed = 0
foreach ($line in $recs) {
    $f = $line -split '\|'
    $rel = $f[0].Trim()
    $name = ($rel -replace '\\build\\gsrec\\', '_' -replace '\\build\\', '_' -replace '\.gsrec$', '')
    if ($Only.Count -and -not ($Only | Where-Object { $name -like "*$_*" })) { continue }
    $vb = [regex]::Match($f[2], '(\d+)-(\d+)')
    $n = if ($vb.Success) { [int]$vb.Groups[2].Value - [int]$vb.Groups[1].Value + 1 } else { 240 }
    $every = if ($n -lt 120) { 8 } else { 60 }
    $rec = Join-Path $CorpusRoot $rel
    $d = Join-Path $Out $name
    Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $d | Out-Null
    $run = Join-Path $Out ("gs_replay_{0}.exe" -f $PID)
    Copy-Item $replay $run -Force
    & $run $rec - 0 --backend hw --ref --scale $Scale --sample-dump $d --sample-every $every 2> (Join-Path $Out "$name.dump.log") | Out-Null
    $rc1 = $LASTEXITCODE
    & $run $rec - 0 --backend hw --mixed --scale $Scale --sample-compare $d --sample-every $every 2> (Join-Path $Out "$name.log") | Out-Null
    $rc2 = $LASTEXITCODE
    Remove-Item $run -ErrorAction SilentlyContinue
    $r = (Select-String -Path (Join-Path $Out "$name.log") -Pattern '^sample-compare: \d+ points' | Select-Object -First 1).Line
    $ok = $rc1 -eq 0 -and $rc2 -eq 0 -and "$r" -match ', 0 beyond the budget, 0 missing'
    if (-not $ok) { $failed++ }
    Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue
    Write-Host ("{0,-24} {1,-6} {2}" -f $name, $(if ($ok) { 'ok' } else { "FAIL ($rc1/$rc2)" }), $r)
}
Write-Host "gpu_sample_all (S=$Scale): $failed recording(s) failed"
if ($failed) { exit 1 }
exit 0
