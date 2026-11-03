# mft-cover: MFT covers the gameplay stretches QA found on the grid

**Goal**: make the Modern frame-target path (runtime/gpu/gpu_exec_mft.h mft_plan) cover the gameplay grid stretches
from qa-b (m6_paraglide ALIAS16 + target view, m5_ewok / m5_ghost / m5_battlezone / m6_train_red target view, m7_boss
not modern), same picture as Exact apart from resolution / AA.

Worktree C:\Users\tanih\Projects\sly2-worktrees\mft-cover, branch next/mft-cover (from main ab15b52).

## Done
- QA harness copied from qa-b (scripts/qa_scene.ps1, qa_scenes.txt, qa_pair.c, qa_batch.ps1).

## In progress
- release build (build/rel), triage runs.

## Next
1. Triage per scene (no-dump runs with PS2_MFT_LOG=<n> + sparse dumps every ~25 VBlanks to see what is on screen).
2. Root-cause per reason; extend mft_plan / mft_op.
3. m7_thermals banded strip: try to reproduce.
4. Verify: fallback table, gates.json, m9_exact, r2_sweep ab/hc/tf ep1, run_tests.py, PS2_GPU_AB frame times.

## Findings / traps
- Triage tool: scripts/qa_scene.ps1 -DumpEvery N (PNG pictures into build\qa\<run>\pics, no flicker check) +
  scratchpad sheets.py contact sheets (nearest-neighbour; scripts/contact_sheet.py box filter is too slow). PNGs are
  ~9.6 MB each: delete after looking. Log vblank (fd->vblank) ~= dump frame number.
- m6_paraglide (tri run, VBlanks 2400-9000): "target view" 1996-3006 and 3936-4869 = the mission briefing (PDA page with
  3D view + portraits); "ALIAS16" 3007-3935 and 4854-5308 = briefing + intro cutscene (letterboxed); gameplay loops
  (no input: crash / restart cutscene / paraglide, period ~600) with ~53 ALIAS16 frames per cycle around the cutscene
  part (v6332-6372, 6938-6990, ...). Rejected sequence: ALIAS16 head on target 3 (other frame), copy ct 3 tex 1, clear
  1 G 7E00, one SVOL draw, flat NPOST sprite fmask FFFF80FF, ROV_END, tail ALIAS16 on 1 (2 copies), COPY_SHADOW, darken
  - looks standard; mft_shadow_seq says "head" -> check codes added (mft_ssf) to find which check.
- m5_ewok: "target view" 2292-5906 = briefing; the rejected draw is the right portrait (tri, ct 1 tex 5 feat 0x429 =
  VIEW|TEX|HIRES|HUD, bb 370,124-431,170). Gameplay after the briefing is modern (only stray single "not modern").
  The nd run's "target view to the end" was the briefing lasting longer in that run.
- m5_ghost (2042-3586), m5_battlezone (2014-3138), m6_train_red (1976-2701): "target view" = the same briefing portrait
  draw; gameplay after it modern. mft-pop (now in main) documents the briefing as grid by design.
- m7_boss: both my tri run and qa-b's shots (f5428 etc.) show the long intro cutscene, not the fight. Its "not modern"
  frames = PRESENT-only VBlanks or the cut-off end of a game frame (1 display draw of 16 sprites + PRESENT): mft-pop's
  cause 2 ("continued"), fixed in main (aeaf310). Re-measure on main.
- Branch rebased onto main abe57ff (mft-pop, qa-a, qa-b merged); took main's gpu_exec_mft.h + qa_scene.ps1, re-added
  -DumpEvery / -Seconds and the mft_ssf check codes. Build with PYTHON_CPU_COUNT=6.
