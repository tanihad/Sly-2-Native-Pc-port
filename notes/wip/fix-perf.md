# fix-perf: full speed (steady 59.94 fields/s) with headroom, release build

Status: in progress

## Goal
Game runs at full NTSC speed with plenty of headroom, measured in the release build; pacing never
faster than real time; smooth presentation. Report before/after numbers per change.

## Done
- Instrumentation (runtime/perf.h, runtime/perf.c):
  - PS2_PERF=1: once per second `[perf]` line: VBlanks serviced/due, flips (DISPFB changes),
    busy ms per serviced VBlank, exclusive time share EE/GS/VU/VIF/IPU/IOP/present/idle.
    Hooks: gif_feed (GS), vu.c run() (VU), vif.c feed() (VIF), ipu entry points (IPU),
    sif_set_dma (IOP), vblank() display/present (present), threads.c idle wait (idle).
    State is a selectany global in perf.h so unit tests need not link perf.c.
  - PS2_PROFILE=1: sampler thread (0.5 ms high-res waitable timer) suspends the main thread,
    records RIP; at exit (watchdog/exit) resolves through the exe's own COFF symbol table and
    prints the top 40 (system DLLs per module).

## Baseline (before any optimisation), 30 s runs, headless
Debug (-O1): movie 60/60 vblanks but busy 16.5 ms/vblank (EE 75-80%: spin), logo/prompt
35/61 vblanks, ~33 flips/s, busy 22-26 ms/vblank: GS 60%, VU 18%, idle 13%.
Release (-O2): same picture; late in the run (prompt) 20/60 vblanks, busy 48-52 ms/vblank.
Profile (release): flush 17.8%, ntdll (idle sleep) 13%, vu_run 8.7%, write_32_2_1 7.6%,
gs_tex_ensure 4.8%, sample_stq_bilin 4.4%, f_0024D5E8 3.5%, write_16_2_1 3.3%, f_0024F390 3.1%,
write_32_1_1 2.7%, gs_display 2.4%, draw 2.1%, u_madd_bc 1.9%.

## Next
1. Find what `flush` (GS pixel pipeline) does and speed it up; pixel write paths.
2. VU interpreter cost (vu_run, u_*).
3. Game functions f_0024D5E8 / f_0024F390 / f_0024CF40 (library memcpy/memset?) -> native.
4. Cache-maintenance functions (0x234EA0/0x234FD0 loops) -> native override table in recomp.
5. EE spin during the movie (EE 80%, idle 0%): find the wait loop.
6. Frame pacing: vblank skipping when behind; presentation smoothness.

## Findings / traps
- Runtime sources are CRLF in the working tree (autocrlf); PowerShell string Replace with `n
  fails silently on them - use the Edit tool.
