# (reimagine) Look = Reimagined's stability gate (notes/reimagined-lighting.md "Stability"): what the idle-scene sweeps
# missed. scripts\reim_ab_stability.ps1 -Mask 1024: ONE run on the owner's settings (1440p, 120 fps, thin outlines,
# render scale auto, filters modern), Sly running with the camera orbiting in the ep1 hub, Faithful lighting and
# Reimagined alternating every 60 pictures (the same camera path for both); every picture measured (8 x 8 block luma
# reversals - flicker - per 1000, over the mean block change - motion). Passes when Reimagined's flicker per motion in
# the dark (shadowed) blocks is at most 1.3 x Faithful's and no texture changed more than once (PS2_TEX_SWAPLOG).
# -Cold: an empty Reimagined texture cache. One game run; the dumps are deleted.
param([switch]$Cold)
$wt = Split-Path $PSScriptRoot -Parent
if (-not (Test-Path (Join-Path $wt 'build\stability_check.exe'))) {
    $clang = Join-Path (Split-Path $wt -Parent) 'sly2-decomp\tools\llvm-mingw\bin\clang.exe'
    & $clang -O2 -o (Join-Path $wt 'build\stability_check.exe') (Join-Path $PSScriptRoot 'stability_check.c')
}
$free = (Get-PSDrive C).Free / 1GB
if ($free -lt 70) { "C: has $([math]::Round($free)) GB free (< 70): not starting"; exit 2 }
if ($Cold) {
    $env:PS2_TEXCACHE_SEED = Join-Path $wt 'build\reimagine\texcache_gate_cold'
    Remove-Item -Recurse -Force $env:PS2_TEXCACHE_SEED -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $env:PS2_TEXCACHE_SEED | Out-Null
}
$out = powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'reim_ab_stability.ps1') -Mask 1024 -Dbg 0 -Tag "gate_stab$(if ($Cold) { '_cold' })"
$out
$v = @{}
foreach ($l in $out) {
    $m = [regex]::Match("$l", ' ([AB]) \(mask.*per 1000 \(all\), ([\d.]+) per 1000 \(dark.*motion ([\d.]+)')
    if ($m.Success) { $v[$m.Groups[1].Value] = [double]$m.Groups[2].Value / [math]::Max([double]$m.Groups[3].Value, 0.05) }
}
$ok = $v.A -ne $null -and $v.B -ne $null
if ($ok -and $v.B -gt 1.3 * $v.A + 0.05) { "FAIL: Reimagined flicker per motion $([math]::Round($v.B, 3)) > 1.3 x Faithful's $([math]::Round($v.A, 3))"; $ok = $false }
$sw = $out | Where-Object { $_ -match 'most (\d+)\)' } | Select-Object -Last 1
if ($sw -and [int]([regex]::Match($sw, 'most (\d+)\)').Groups[1].Value) -gt 1) { "FAIL: a texture changed more than once"; $ok = $false }
if (-not $sw) { "(no texture swap summary)" }
Remove-Item Env:\PS2_TEXCACHE_SEED -ErrorAction SilentlyContinue
if ($ok) { "stability: PASS (flicker per motion: Faithful $([math]::Round($v.A, 3)), Reimagined $([math]::Round($v.B, 3)))"; exit 0 } else { "stability: FAIL"; exit 1 }
