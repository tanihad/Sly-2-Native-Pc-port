# WIP: Reimagined lighting (branch next/reimagine, worktree C:\Users\tanih\Projects\sly2-worktrees\light)

Goal: Look = Reimagined gets a bold modern lighting look (grading, bloom / halos / shafts, stronger AO + rim + spec,
shadows, atmosphere); Faithful and Retro stay bit-identical to main (81cde3d).

## Done
- (nothing yet)

## In progress
- step 1: plumbing (gfx.c -> gpu_look_config, PS2_REIMAGINE=0|1, R8P_REIM A/B bit) + light-data logging

## Next
1. plumbing + build
2. look at the light data (directions / colours of the light blocks, world matrix) -> shadow-map feasibility
3. Reimagined post pass (light.hlsl REIM perms): grading, bloom v2 + halos, shafts, fog, vignette, stronger AO, SS shadows
4. R2 rim / spec (Reimagined only)
5. shots, perf, gates

## Rules / traps
- absolute paths only; PS2_USER_DIR under build\; no window.
