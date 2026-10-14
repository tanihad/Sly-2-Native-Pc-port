# WIP gpu-C: corpus extension for the D3D11 renderer (notes/d3d11-renderer.md section 11 "C")

**Status: complete.** tests/run_tests.py 16/16 and all 19 tests/unit/*/run.ps1 suites pass after the runtime change.

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
  only the token), PS2_DUMP_FROM=rec; window unit test covers the token. run_tests.py 16/16.
- scripts/gs_record_scene.ps1 (record a scene: warp card, unique exe, -From / `rec` token / -Live),
  scripts/gs_corpus_verify.ps1 (replay + compare with the live dumps), scripts/gsrec_cut.py.
- 11 recordings, all verified (225/226 frames identical; the 1 = first VBlank of hack_minigame,
  replay start artefact), census -Full on all 24, build\census\tables.md.
- tests/gpu/corpus.txt (24 recordings), notes/d3d11-renderer.md appendix B + rows 13/15 + 10.4.

## Recordings (build\gsrec)
| name | VBlanks | MB | verified |
|---|---|---|---|
| ep0_museum | 5106-5255 | 244 | 15/15 |
| ep1_hub | 7079-7258 | 421 | 18/18 |
| m1_brawl | 4813-4992 | 270 | 18/18 |
| ep2_hub | 4680-4859 | 580 | 18/18 |
| ep2_photo | 9114-9353 | 110 | 24/24 |
| ep4_hub | 4701-4880 | 490 | 18/18 |
| hack_minigame | 3790-3969 | 34 | 17/18 (first VBlank) |
| fmv | 9852-10091 | 134 | 24/24 |
| load_m3 | 2326-2565 | 258 | 48/48 |
| pause_save | 6835-6984 | 420 | 15/15 |
| thiefnet | 4358-4457 | 342 | 10/10 |

## Next
(none) - possible extras: the episode menu wheel (van, 2D art) as its own recording; m4_hack's own
minigame would need a runtime task-state tool (PS2_TASK_FILE exists only on the episode branches).

## Findings / traps
- PowerShell variables are case-insensitive: `$frames` clobbered the `[int]$Frames` param (dumps went
  to a dir "120"); `$full` vs `[switch]$Full` likewise.
- -Live + dump ring deletes the recorded window's frames once the game runs on: the script copies
  them to rec_frames when the recording completes (and stops the game unless -KeepRunning).
- One recording per game run (gs_record.c closes the file); the `rec` token after that does nothing.
- Timeline (release, loaded machine): START at ~VBlank 2400; episode safehouse menu ~+1300..+1800,
  X at START+1800 works (+1500 is too early); hub gameplay ~+1900. Direct job: level ~+150 after START.
  m1_brawl direct: briefing until ~+2000 on a loaded machine (rec at +2500).
- ep2 photo (t2_recon_enter_ballroom): Binocucom = R3; Y aim inverted; the circle is gold when in
  view, "Too far" until zoomed, olive then green + name when locked. Live aim from the spawn:
  "+0:r3:10,+60:lleft:22,+30:ldown:11", "+0:lright:12,+14:lup:5,+30:rup:14,+30:lright:7,+20:rup:10",
  "+0:lright:8,+12:lup:5,+20:rup:8", "+0:rup:6,+10:lright:4,+8:lup:3", "+0:rup:3",
  "+0:lright:5,+8:lup:3", "+0:rup:3" -> locked; then "+0:rec,+16:r1:10". The same chain as one
  scripted line did NOT reproduce (stick acceleration / timing): do it live.
- m4_hack direct (t4_hack_intro or t4_hack_path2) cannot reach the minigame without re-activating a
  station at run time (--activate / --set on the save is undone by the level load): used ep5's
  t5_heist_free_carmelita_hack direct (same minigame, starts immediately).
- **Photo mechanism**: no TRXDIR=1/2. Shutter: scene into both field buffers, two CTX2 sprites copy
  them 2:1 into FBP 070 (Z base) as CT32 512x224, the world stops, the Polaroid samples TBP 0E00
  with REGION_CLAMP for ~180 frames. Framing judgement: EE-side (failed R1 also 0 readbacks).
- **FMV**: 640x480 movie = 1200 16x16 CT32 macroblock uploads per movie frame (30 fps) into TBW 12 at
  TBP 1800, one 512x224 DECAL sprite per field. Legal still after restart: same path.
- Loading screen: black + dim Sly-mask icon (drawn), ~0.5 s with host files.
- ThiefNet / pause menus render the full 3D scene behind the UI: 3.4 MB per VBlank of recording;
  the first ThiefNet take (300 VBlanks) was 1 GB, re-recorded as 100 VBlanks of the list.
- Input: menu presses need ~40 frames between them and >=10-frame holds on a loaded machine.
- ThiefNet: ep1 safehouse menu RIGHT -> laptop ("...and buy gadgets"), X -> "Connecting to
  ThiefNet..." (~150 VBlanks) -> Buy Gadgets. Pause: START -> wheel (Options/Controls/Job Help/
  Keep Playing/Save and Quit/Exit to Episode Menu); Save and Quit -> "No, Don't Quit" preselected, up
  = "Yes, Quit".
- Episode menu (--set episode.palace.percent_complete=100, Episode 2): wheel at Episode 3; up x3 =
  "Watch a Movie"; X -> film strip "Prelude"; X -> movie.
