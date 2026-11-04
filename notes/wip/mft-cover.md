# mft-cover: MFT covers the gameplay stretches QA found on the grid

**Goal**: make the Modern frame-target path (runtime/gpu/gpu_exec_mft.h mft_plan) cover the gameplay grid stretches
from qa-b (m6_paraglide ALIAS16 + target view, m5_ewok / m5_ghost / m5_battlezone / m6_train_red target view, m7_boss
not modern), same picture as Exact apart from resolution / AA.

Worktree C:\Users\tanih\Projects\sly2-worktrees\mft-cover, branch next/mft-cover (rebased on main abe57ff, merged
main 669ae0c).

## Done
- QA harness: main's qa_scene.ps1 + -DumpEvery N (sampled PNGs into build\qa\<run>\pics) / -Seconds.
- Triage of all six scenes (sampled pictures + PS2_MFT_LOG): no QA grid stretch is gameplay (see Findings).
- Re-measured on main (mft-pop merged), tag "main" runs (build\qa\<scene>__balanced__main\series.txt).
- Diagnostics in gpu_exec_mft.h (PS2_MFT_LOG only): mft_ssf check codes of mft_shadow_seq ("ALIAS16 check N"), flat
  pass details, what a frame without R2 ops holds.

## Status: complete (2026-11-04; wrapped up on the coordinator's request: no new coverage work)
- thermals: 1 ALT run, band not reproduced (alt2 cut by the pause).
- Notes: notes/modern-renderer.md "MFT coverage of the qa-b grid stretches".

## Next
1. thermals result; if it reproduces, root-cause.
2. Notes in notes/modern-renderer.md (R8 / MFT pops area): the triage result.
3. Verify: gates.json, m9_exact level, r2_sweep ab/hc/tf ep1, run_tests.py; A/B frame time only if code paths change.
4. Final report.

## Findings / traps
- Triage tool: qa_scene.ps1 -DumpEvery N + scratchpad sheets.py contact sheets (nearest-neighbour; scripts/
  contact_sheet.py's box filter is too slow). PNGs ~9.6 MB each: delete after looking. Log vblank ~= dump frame number.
- m6_paraglide: "target view" + "ALIAS16" stretches = the mission briefing (PDA page with 3D view + live portraits)
  and the intro cutscene; gameplay (loops without input: crash / cutscene / paraglide) is MFT on main.
- m5_ewok / m5_ghost / m5_battlezone / m6_train_red: "target view" = the briefing's right portrait (tri, ct 1 tex 5
  feat 0x429 = VIEW|TEX|HIRES|HUD, bb 370,124-431,170: a view of a 128x128 render target); gameplay after it MFT.
- m7_boss: QA's runs (and mine) never reached the fight: the intro cutscene; its "not modern" = PRESENT-only /
  cut-off VBlanks = mft-pop cause 2, fixed in main ("continued 1176" in 6900 frames, 0 pops).
- Paraglide's remaining ALIAS16 = check 19: the flat pass between the volumes is one draw of 32 sprites in two states /
  colours (16 + 16); npost_prep wants one, the fused pass's flat passes are whole-frame with one constant. Only seen in
  the briefing (on the grid anyway for the portraits) and once in a cutscene: left.
- Gotcha: a gpu_log format with a missing argument crashed the game (0xC0000005).
- Set-Content -Encoding utf8 writes a BOM (PS 5.1): use [IO.File]::WriteAllText.
