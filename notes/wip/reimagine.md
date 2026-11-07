# WIP: Reimagined lighting (branch next/reimagine, worktree C:\Users\tanih\Projects\sly2-worktrees\light)

Goal: Look = Reimagined gets a bold modern lighting look (grading, bloom / halos / shafts, stronger AO + rim + spec,
shadows, atmosphere); Faithful and Retro stay bit-identical to main (81cde3d).

## Done
- plumbing: gfx.c -> gpu_look_config (graphics.look == reimagined); PS2_REIMAGINE=0|1 wins; A/B PS2_GPU_AB_R8=1024
  (R8P_REIM); lt_enabled() true under Reimagined even with lighting=original.
- runtime/gpu/gpu_reim.h (included by gpu_light.h): scene analysis (world x clip = most common entry matrix; lit entries'
  light blocks -> world-space light clusters; key light = brightest cluster from above, smoothed), shadow map (R2 segments
  re-drawn as casters: r2.hlsl R2_SHADOW, r2_render.c in.shadow), post pass reim_post (light.hlsl REIM perms).
- light.hlsl REIM: ps_reim_shadow (shadow map 4x4 PCF + contact march -> visibility, N.L), ps_reim_blur2, bright (MRT
  scene colour with mips), up (halo weight), stats (4x1 smoothed: avg colour + log lum, fog colour, shaft source),
  shafts, comp (AO, key-light relight + cool shadows, bloom, shafts, clarity, depth + height fog, mood grade, filmic
  curve, vignette). Debug PS2_REIM_DBG=1 vis, 2 shafts, 3 world bands, 4 fog colour, 9 cycle.
- r2.hlsl R2_ENH=2 perms (ps_*_e_r, ps_*_e_t_r): spec (r2_mft.w) + rim in |light colour| hue + r2_reim_normal hook
  (R2_NMAP). r2_render.c swaps them in per segment (in.reim). Faithful's 270 shaders byte-identical (checked against
  build\reimagine\gpu_shaders_main.h with a block compare).

## Status: COMPLETE (b3663a4 + notes; results in notes/reimagined-lighting.md)

## Was in progress
- gates: run_tests + units pass (shot: re-run - the exe was rebuilt under it), m9_exact S1+S3 -Game identical, gpu_diff_all 28/28 (on 25d1062 runtime).
- pictures: build\reimagine\shots_all.ps1 (shots.ps1 per scene) -> build\reimagine\shots; then perf A/B, r2_sweep Faithful + Reimagined.

## Next
1. tune; perf A/B (PS2_GPU_AB=24 PS2_GPU_AB_R8=1024); notes/reimagined-lighting.md
2. gates (run_tests, units, m9_exact, gpu_diff_all, r2_sweep Faithful + Reimagined), pictures Retro|Faithful|Reimagined

## Findings / traps
- light block colours are often NEGATIVE: the light then comes from -direction (f = min(d + d^3, thr) x colour). The
  Faithful rim (max(L0, 0)) is therefore 0 in ep1; Reimagined uses |colour|.
- ep1 hub key light: world dir (0.30, 0.49, 0.82), colour (0.10, 0.26, 0.48): the moon. World is z-up.
- Darkening alone made the shadows invisible at night; relighting (lit faces x (1 + 0.45 x key hue)) made them read.
- Parallel game runs (3) slowed VBlanks so hubs never reached the dump window: run <= 2 at once, -Seconds 240.
- PowerShell [IO.File] with a relative path writes into the main checkout: absolute paths only.
- env.ps1 sets $root to the MAIN checkout: a script that dot-sources it must not use $root for the worktree (my first gate run wrote build\reimagine + build\unit outputs into the main checkout; removed build\reimagine there).
- AI agent's normal maps: next/ai notes/reimagined.md (etn_sample in etn.hlsli, t40..t46); r2_reim_normal is where they plug in (not integrated: their branch not merged).
