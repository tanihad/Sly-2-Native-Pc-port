# (reimagine) Look = Reimagined's stability gate (notes/reimagined-lighting.md "Stability"): what idle-scene sweeps miss.
# scripts\reim_ab_stability.ps1 -Mask 1024, one game run per shot on the owner's settings (High: 1440p, MSAA 8x, thin
# outlines; 120 fps), deterministic (reimagine2): START anchored at the title, one X at the safehouse, the pad and the
# dump window relative to it, the game clock stepped 1/120 s per frame (PS2_FRAME_DT_FIXED) - the same pictures every run.
# Faithful lighting (A) and Reimagined (B) alternate every 6 pictures inside the run (the same camera path for both);
# triples of consecutive pictures of one phase are measured (8 x 8 block luma reversals - flicker - per 1000 blocks, over
# the mean block change - motion). Shots: orbit (ep1 hub: Sly runs, the camera orbits) and trees (ep2 hub: past a
# torch into the trees' shadows). Passes when, in every shot, Reimagined's flicker per motion in the dark (shadowed)
# blocks is at most 1.15 x Faithful's, no texture changed more than once (PS2_TEX_SWAPLOG) and the world camera rebuilt
# from the chosen world matrix never flipped away and straight back inside the window (a real camera cut is fine) (the trees' "confused" shadows; the reversal count alone
# does not see a few large steps - 0.1.1214 passes the ratio on these paths). A shot whose pictures
# barely move (the pad path missed: motion < 0.3) is run once more. -Cold: an empty Reimagined texture cache.
# -Shots orbit,trees (default both). One game run at a time; the dumps are deleted.
param([switch]$Cold, [string]$Shots = 'orbit,trees', [double]$Limit = 1.15, [string]$Extra = '')   # (-Extra: env for both looks, e.g. PS2_REIM2=0)
$wt = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'project.ps1')   # (tex-prebuild: the main checkout's clang, wherever the worktree lives)
$clang = $ProjectClang
$chk = Join-Path $wt 'build\stability_check.exe'
if (-not (Test-Path $chk) -or (Get-Item $chk).LastWriteTime -lt (Get-Item (Join-Path $PSScriptRoot 'stability_check.c')).LastWriteTime) {
    & $clang -O2 -o $chk (Join-Path $PSScriptRoot 'stability_check.c')
}
$free = (Get-PSDrive C).Free / 1GB
if ($free -lt 70) { "C: has $([math]::Round($free)) GB free (< 70): not starting"; exit 2 }
if ($Cold) {
    $env:PS2_TEXCACHE_SEED = Join-Path $wt 'build\reimagine\texcache_gate_cold'
    Remove-Item -Recurse -Force $env:PS2_TEXCACHE_SEED -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $env:PS2_TEXCACHE_SEED | Out-Null
}
$ok = $true
$sum = @()
foreach ($shot in ($Shots -split ',')) {
    for ($try = 0; $try -lt 2; $try++) {
        if ($Cold -and $try) { Get-ChildItem $env:PS2_TEXCACHE_SEED -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue }
        $frames = if ($shot -eq 'trees') { 850 } else { 600 }
        $a = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'reim_ab_stability.ps1'), '-Mask', '1024', '-Dbg', '0',
               '-Shot', $shot, '-Frames', "$frames", '-Tag', "gate_$shot$(if ($Cold) { '_cold' })")
        if ($Extra) { $a += @('-Extra', $Extra) }   # (an empty argument would be dropped by -File: "missing an argument")
        $out = & powershell @a
        $out
        $v = @{}; $mo = @{}
        foreach ($l in $out) {
            $m = [regex]::Match("$l", ' ([AB]) \(mask.*per 1000 \(all\), ([\d.]+) per 1000 \(dark.*motion ([\d.]+)')
            if ($m.Success) { $mo[$m.Groups[1].Value] = [double]$m.Groups[3].Value; $v[$m.Groups[1].Value] = [double]$m.Groups[2].Value / [math]::Max([double]$m.Groups[3].Value, 0.05) }
        }
        if ($v.A -ne $null -and $v.B -ne $null -and $mo.A -ge 0.3) { break }
        "($shot`: no usable pictures or no motion - the pad path missed; once more)"
    }
    if ($v.A -eq $null -or $v.B -eq $null -or $mo.A -lt 0.3) { "FAIL ($shot): no measurement"; $ok = $false; continue }
    $r = $v.B / [math]::Max($v.A, 1e-6)
    $sum += "$shot $([math]::Round($r, 3))x (Faithful $([math]::Round($v.A, 3)), Reimagined $([math]::Round($v.B, 3)))"
    if ($r -gt $Limit) { "FAIL ($shot): Reimagined flicker per motion $([math]::Round($v.B, 3)) = $([math]::Round($r, 3)) x Faithful's $([math]::Round($v.A, 3)) (> $Limit)"; $ok = $false }
    $sw = $out | Where-Object { $_ -match 'most (\d+)\)' } | Select-Object -Last 1
    if ($sw -and [int]([regex]::Match($sw, 'most (\d+)\)').Groups[1].Value) -gt 1) { "FAIL ($shot): a texture changed more than once"; $ok = $false }
    if (-not $sw) { "($shot`: no texture swap summary)" }
    $wj = $out | Where-Object { $_ -match 'world camera flips in the window: (\d+)' } | Select-Object -Last 1
    if ($wj -and [int]([regex]::Match($wj, ': (\d+)$').Groups[1].Value) -gt 0) { "FAIL ($shot): the world matrix choice flipped inside the window"; $ok = $false }
}
Remove-Item Env:\PS2_TEXCACHE_SEED -ErrorAction SilentlyContinue
if ($ok) { "stability: PASS ($($sum -join '; '))"; exit 0 } else { "stability: FAIL ($($sum -join '; '))"; exit 1 }
