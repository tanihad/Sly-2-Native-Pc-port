# WIP gpu-C: corpus extension for the D3D11 renderer (notes/d3d11-renderer.md section 11 "C")

**Goal**: new .gsrec recordings (prologue, ep1 hub + m1_brawl, ep2 hub + photo mission, ep4 hub +
m4_hack, FMV, loading screen, pause/save/ThiefNet menus) with the sw renderer, census on each,
hazard counters (TRXDIR=1/2, CLUT from drawn memory), tests/gpu/corpus.txt, appendix in the design.

Worktree C:\Users\tanih\Projects\sly2-worktrees\gpu-C, branch gpu/C. Recordings: build\gsrec\<name>.gsrec
(+ build\gsrec\<name>\ = card, exe copy sly2_gpuC_<name>.exe, run.log, frames). Not committed.

## Done
- release build build\rel, debug build build\sly2 (both from this worktree).
- scripts/gs_census.c: hazard counters (summary line `hazards:`; first 64 READBACK / COPY and first
  16 CLUT-FROM-DRAWN events printed in place with the drawing that last wrote the source pages).
- scripts/gs_record_scene.ps1: warp card + unique exe + PS2_GS_RECORD window + dumps in the window.

## In progress
- scouting runs to find the VBlank of each scene.

## Next
1. Scout + record each scene (table below), check frames (contact_sheet.py).
2. gs_replay each -> compare_frames.py against the live dumps.
3. census on all (old 13 + new), census_table.py, hazard table.
4. tests/gpu/corpus.txt; appendix in notes/d3d11-renderer.md; final report.

## Scenes
| name | how | from | frames | status |
|---|---|---|---|---|

## Findings / traps
- Input: menu presses need ~40 frames between them and >=10-frame holds on a loaded machine.
- ep2 photo: t2_recon_enter_ballroom direct; R3 Binocucom, left stick aims (Y inverted), right stick
  zooms, R1 photo when the target circle is green -> Polaroid (ep2 worktree notes).
- ThiefNet: safehouse menu LEFT x2 (ep5) / right (ep1) -> laptop, X -> Buy Gadgets.
- Pause: START in gameplay -> "We'll be right back!" Keep Playing / Save and Quit / Options / Controls.
