# Render a GS recording (PS2_GS_RECORD, runtime/gs_record.c) with the GS code of a checkout.
#   powershell -ExecutionPolicy Bypass -File scripts\gs_replay.ps1 -Rec build\x.gsrec -Out build\replay_new [-Runtime <dir>] [-Workers 0]
#   ... -Bench: no pictures, just the time the GS code takes (a GS-only benchmark, no EE involved)
# -Runtime: the runtime/ directory whose GS code renders it (default: this checkout's), e.g. an
# older version exported with `git archive`, to compare two renderers on the same input:
#   scripts\compare_frames.py build\replay_old build\replay_new
# -Workers > 0 renders through the GS thread with that many pixel workers (deferred drawing).
# GPU backend (runtime/gpu, notes/d3d11-renderer.md 10.1):
#   -Backend sw|hw|both   sw pictures (default) / GPU pictures read back / both, compared per VBlank
#                         (exit code 3 when any pixel differs; differing frames as _sw/_hw/_diff.png)
#   -Backend hwdry        the GPU frontend dry run (M1): classification, records, ownership, counters
#                         (synchronous; exit code 5 on any PUNT); -FrontOnly: without the software
#                         rasterizer (frontend timing), -Ref, -StraddleRov, -SelfCheck (Z records vs sw)
#   -Backend hwsubmit     M2 submission benchmark: the frontend's frames executed on the render thread with
#                         real D3D11 objects and trivial shaders (gpu_exec.c); prints render-thread ms per
#                         frame, us per D3D draw, GPU ms ('submit-bench' line); -Scale N target scale,
#                         -WithSw also runs the software rasterizer, -Pace replays VBlanks at 59.94 Hz
#   -Ref (hw / both)      M3 REF tier: the GPU draws every primitive exactly (ROV pixel pipeline, shaders/gs.hlsl);
#                         pictures composed from the GPU's GS memory (reference compose); both: compared with sw
#                         (exit 3 on differing pixels); synchronous (-Workers 0); -Bench with hw: GPU ms only
#   -Lockstep             (both -Ref) GS memory compared with sw at every FRAME/ZBUF change and VBlank, post
#                         passes included (M4); the first divergence is bisected to its primitive (dump +
#                         bisect_{hw,sw,diff}.png crops in -Out); exit 6 on divergence; -NoResync: no restart from
#                         sw memory after a differing frame (the gate)
#   -Mixed (hw / both)    M5 default mode: FF tier (RTV + DSV, OM blend) and ROV tier in one frame; -Lockstep compares
#                         with the tolerances of 10.2 (colour +-16 tolerated and counted; alpha, Z, CPU data exact;
#                         -LockstepExact: everything exact); pictures gated by PSNR >= 40 dB and <= 0.2% pixels > 16
#                         (exit 3 beyond); -StqExact: FF triangles step S/T/Q exactly (256-byte records);
#                         -FfRovMask <GPU_FFC_* bits>: classes kept on the ROV tier; straddling pass-A primitives take the ROV tier
#                         (escalation, default) unless -SplitFf (the FF two-draw split)
#   -PostProbe F:L        (-Ref) scripts/gs_post_probe.c's channel report of the two frame buffers for VBlank indices
#                         F..L from the GPU's memory, on stdout (compare with gs_post_probe.ps1's)
#   -Profile              sampling profile of the replay thread (self / inclusive per function)
#   -StreamHash           (hwdry) hash of the frontend's command stream and data of every frame
#   -FrontThread          (hwdry -FrontOnly, hwsubmit) the frontend on its own thread, the replay thread only decodes
#   -Pin                  (hwdry, hwsubmit) replay / frontend / render threads each on its own performance core
#   -Adapter hw|warp      hardware adapter (default) or the WARP software rasterizer
#   -Game                 (hw) M6: the game's GPU renderer path (runtime/gpu/gpu_hw.c): frontend thread, pictures
#                         composed on the GPU, each read back and compared with the reference compose of the GPU's
#                         memory (exact; exit 3 otherwise); -GameVerify N: the frontend on this thread next to sw,
#                         gpu_hw's memory verifier every Nth VBlank, pictures also compared with sw (picture gate);
#                         -Bench: no read-backs
#   -Metrics <file.json>  per-VBlank comparison counts and D3D/CPU/GPU timings
#   -Log <file>           the replay's stderr goes to this file
#   -VramHash <file>      (synchronous, -Workers 0) a hash of all GS memory after every VBlank, one line each
# A replay without progress for 60 s prints its state and exits with code 4 (watchdog).
# Every run prints "sw-hash", a hash of all sw pictures (unchanged renderer -> unchanged hash).
param([Parameter(Mandatory = $true)][string]$Rec, [string]$Out = "", [string]$Runtime = "", [int]$Workers = 0,
      [switch]$Bench, [ValidateSet('sw', 'hw', 'both', 'hwdry', 'hwsubmit')][string]$Backend = 'sw', [int]$Scale = 1, [switch]$WithSw, [switch]$Pace, [switch]$Profile, [switch]$StreamHash, [switch]$FrontThread, [switch]$Pin, [switch]$FrontOnly, [switch]$Ref, [switch]$StraddleRov, [switch]$SelfCheck, [switch]$DecodeOnly, [switch]$Lockstep, [switch]$NoResync, [string]$PostProbe = "", [string[]]$Define = @(),
      [ValidateSet('hw', 'warp')][string]$Adapter = 'hw', [string]$Metrics = "", [string]$Log = "", [string]$VramHash = "",
      [switch]$Mixed, [switch]$StqExact, [string]$FfRovMask = "", [switch]$LockstepExact, [switch]$SplitFf,
      [switch]$Game, [int]$GameVerify = 0)
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
if (-not $Out) { $Out = Join-Path $root 'build\gs_replay' }
New-Item -ItemType Directory -Force $Out | Out-Null
$names = @('gs.c', 'gs_draw.c', 'gs_draw_pixel.c', 'gs_draw_tex.c', 'gs_draw_swz.c', 'gs_setup.c', 'gs_thread.c', 'gs_record.c', 'debug.c')
$src = @($names | ForEach-Object { Join-Path $Runtime $_ } | Where-Object { Test-Path $_ })
$src += @(Get-ChildItem $Runtime -Filter 'gs_mem*.c' | ForEach-Object { $_.FullName })
$src += Join-Path $PSScriptRoot 'gs_replay.c'
$flags = @('-O2', '-std=c11', '-w', '-I', $Runtime) + @($Define | ForEach-Object { '-D' + $_ })
$libs = @('-lsynchronization')
$gpuDir = Join-Path $Runtime 'gpu'
if (Test-Path (Join-Path $gpuDir 'gpu.h')) {                           # runtimes with the GPU backend
    $gen = Join-Path $Out 'gen'                                           # that runtime's shaders
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'gpu_shaders.ps1') -Root $root -Gen $gen -Shaders (Join-Path $gpuDir 'shaders') | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host 'shader build failed'; exit 1 }
    $src += @(Get-ChildItem $gpuDir -Filter '*.c' | ForEach-Object { $_.FullName })
    $flags += @('-DGS_REPLAY_GPU', '-I', $gpuDir, '-I', $gen)
    $libs += @('-luser32', '-lgdi32')
} elseif ($Backend -ne 'sw') { Write-Host "$Runtime has no GPU backend: only -Backend sw"; exit 2 }
$exe = Join-Path $Out 'gs_replay.exe'
& $clang @flags @src @libs -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host 'gs_replay build failed'; exit 1 }
# run a uniquely named copy (other agents run replays on this machine too)
$run = Join-Path $Out ("gs_replay_{0}.exe" -f $PID)
Copy-Item $exe $run -Force
$runArgs = @($Rec, $(if ($Bench) { '-' } else { $Out }), $Workers, '--backend', $Backend, '--adapter', $Adapter)
if ($Metrics) { $runArgs += @('--metrics', $Metrics) }
if ($VramHash) { $runArgs += @('--vram-hash', $VramHash) }
if ($FrontOnly) { $runArgs += '--front-only' }
if ($Ref) { $runArgs += '--ref' }
if ($Mixed) { $runArgs += '--mixed' }
if ($StqExact) { $runArgs += '--ff-stq-exact' }
if ($FfRovMask) { $runArgs += @('--ff-rov-mask', $FfRovMask) }
if ($LockstepExact) { $runArgs += '--lockstep-exact' }
if ($SplitFf) { $runArgs += '--split-ff' }
if ($StraddleRov) { $runArgs += '--straddle-rov' }
if ($SelfCheck) { $runArgs += '--selfcheck' }
if ($Lockstep) { $runArgs += '--lockstep' }
if ($NoResync) { $runArgs += '--no-resync' }
if ($PostProbe) { $runArgs += @('--post-probe', $PostProbe) }
if ($DecodeOnly) { $runArgs += '--decode-only' }
if ($Scale -ne 1) { $runArgs += @('--scale', $Scale) }
if ($WithSw) { $runArgs += '--with-sw' }
if ($Pace) { $runArgs += '--pace' }
if ($Profile) { $runArgs += '--profile' }
if ($StreamHash) { $runArgs += '--stream-hash' }
if ($FrontThread) { $runArgs += '--front-thread' }
if ($Pin) { $runArgs += '--pin' }
if ($Game) { $runArgs += '--game' }
if ($GameVerify -gt 0) { $runArgs += @('--game-verify', $GameVerify) }
if ($Log) {
    # real redirection by cmd: nested PowerShell pipes of a chatty native stderr are slow and once
    # stalled a corpus run (notes/wip/gpu-M0.md)
    $quoted = ($runArgs | ForEach-Object { '"' + $_ + '"' }) -join ' '
    & cmd /c "`"`"$run`" $quoted 2>`"$Log`"`""
} else {
    & $run @runArgs
}
$rc = $LASTEXITCODE
Remove-Item $run -ErrorAction SilentlyContinue
exit $rc
