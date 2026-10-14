# WIP gpu-C: corpus extension for the D3D11 renderer (notes/d3d11-renderer.md section 11 "C")

**Goal**: new .gsrec recordings (prologue, ep1 hub + m1_brawl, ep2 hub + photo mission, ep4 hub +
m4_hack, FMV, loading screen, pause/save/ThiefNet menus) with the sw renderer, census on each,
hazard counters (TRXDIR=1/2, CLUT from drawn memory), tests/gpu/corpus.txt, appendix in the design.

Worktree C:\Users\tanih\Projects\sly2-worktrees\gpu-C, branch gpu/C. Recordings: build\gsrec\<name>.gsrec
(+ build\gsrec\<name>\ = card, exe copy sly2_gpuC_<name>.exe, run.log, frames, rec_frames). Not committed.

## Done
- release build build\rel, debug build build\sly2 (both from this worktree).
- scripts/gs_census.c: hazard counters (summary line `hazards:`; first 64 READBACK / COPY and first
  16 CLUT-FROM-DRAWN events printed in place with the drawing that last wrote the source pages);
  `-a` prints every table entry (gs_census.ps1 -Full).
- scripts/census_table.py: hazards table + `--new <glob>` novelty listing vs the baseline reports.
- runtime: PS2_INPUT token `rec` starts PS2_GS_RECORD at the next VBlank (PS2_GS_RECORD_FROM=-1 =
  only the token), PS2_DUMP_FROM=rec; window unit test covers the token. run_tests.py 16/16, window
  unit PASSED.
- scripts/gs_record_scene.ps1: warp card + unique exe + recording (-From or `rec` token or -Live with
  PS2_INPUT_FILE) + dumps; copies the recorded window's live frames to <name>\rec_frames.
- census of the old 13 with the new tool: no readback, no TRXDIR=2, no CLUT from drawn pages.

## Recordings (build\gsrec)
| name | how | VBlanks | MB | verified |
|---|---|---|---|---|
| ep0_museum | -Episode 0, rec at START+2850, moves | 5106+150 | 244 | 15/15 identical |
| ep2_binoc_toofar | live t2_recon_enter_ballroom, Binocucom on the wing, R1 "Too far" | 14028+240 | 247 | frames lost (ring) |
| ep2_photo (1st) | live, successful photo of Clockwerk's Wing | 10246+300 | ? | frames lost -> redone scripted |

## In progress
- ep2_photo scripted redo (aim sequence below), ep2_hub, ep4_hub, m1_brawl recordings running.

## Next
1. m4_hack: -Job t4_hack_path2 -Direct -Make "--activate t4_hack_station1" -> Circle at the green
   terminal -> hack minigame (ep4 notes). Scout first.
2. FMV: -Episode 2 -Set episode.palace.percent_complete=100 -> episode wheel -> "Watch a Movie"
   (top) -> film strip -> Prelude (IPU movie). Live steer.
3. Loading screen: direct job start with "+0:rec" right after START (240 VBlanks).
4. Pause/save: hub gameplay, START -> pause menu, Save and Quit -> Yes. ThiefNet: safehouse menu
   RIGHT (ep1) / LEFT x2 (ep5) -> laptop, X -> Buy Gadgets (--coins 5000).
5. ep1 hub: X at START+1800 -> Satellite Sabotage briefing -> Sly on roofs (scout length).
6. gs_replay + compare_frames for each; census -Full on all (old 13 + new); census_table --new.
7. tests/gpu/corpus.txt; appendix in notes/d3d11-renderer.md; final report.

## Findings / traps
- PowerShell variables are case-insensitive: `$frames` clobbered the `[int]$Frames` param (dumps went
  to a dir "120"); `$full` vs `[switch]$Full` likewise.
- -Live + dump ring deletes the recorded window's frames once the game runs on: the script now copies
  them to rec_frames when the recording completes.
- Timeline (release, loaded machine): START at ~VBlank 2400; episode safehouse menu ~+1300..+1800,
  X at START+1800 works (+1500 is too early); hub gameplay ~+1900. Direct job: level ~+150 after START.
- ep2 photo (t2_recon_enter_ballroom): Binocucom = R3; Y aim inverted; the circle is gold when in
  view, "Too far" until zoomed, olive then green + name when locked. Scripted aim from the spawn:
  r3, lleft:22, ldown:11, lright:12, lup:5, rup:14, lright:7, rup:10, lright:6, lup:5, rup:12,
  rdown:4, lright:4, lup:4, rup:3 (60-frame gaps) -> locked; R1:10 -> photo.
- **Photo mechanism (census of the 1st ep2_photo)**: no TRXDIR=1, no TRXDIR=2. At the shutter two
  256x224 sprites copy both field buffers (FBP 000 / 038, CT32) at 2:1 horizontally into FBP 070 =
  the Z buffer's pages (CT32 512x224, left/right halves), then tristrips sample TBP 0E00 (page 070) =
  the Polaroid. A render-to-texture through the Z region, no readback.
- Binocucom "Too far" judgement (ep2_binoc_toofar): no readback either (EE-side test).
- Input: menu presses need ~40 frames between them and >=10-frame holds on a loaded machine.
- ThiefNet: safehouse menu LEFT x2 (ep5) / right (ep1) -> laptop, X -> Buy Gadgets.
- Pause: START in gameplay -> "We'll be right back!" Keep Playing / Save and Quit / Options / Controls.
