# WIP corpus: re-record the GPU gate corpus (branch next/corpus, worktree sly2-worktrees\corpus, from main 336e78e)

**Status: in progress**

**Goal**: the 28 GS recordings of tests/gpu/corpus.txt (destroyed with other worktrees' build folders) re-recorded
into a durable root C:\Users\tanih\Projects\sly2-corpus (same relative paths), verified, baseline regenerated, gates
green on 336e78e.

## Done
- gpu_corpus.ps1 root order (commit 9872809).
- release build build\rel from 336e78e.
- Local helpers (not committed, build\ctools): rec.ps1 = gs_record_scene.ps1 with the recording environment below;
  fin.ps1 = copy the .gsrec into sly2-corpus\<path> (hash-checked), keep run.log + a line in build\ctools\verify\
  summary.txt, delete frames / replay / exe copy; crlf.ps1.
- Recorded + verified + copied (name | VBlanks | MB | verify | how):
  - rv_prompt | 540-559 | 9 | 20/20 | PS2_PC_TEXT=0 -NoCard -NoStart -Pad "600:square:12,2100:start:10" -From 540 -Frames 20 -RecDumpEvery 1
  - rv_intro | 1000-1015 | 28 | 16/16 | same, -From 1000 -Frames 16
  - title | 2200-2229 | 20 | 30/30 | PS2_PC_TEXT=0 -NoCard -NoStart -Pad "600:square:12,@187D80/1DD96C+30:start:10" -From 2200 -Frames 30 -RecDumpEvery 1
  - brief | 2700-2729 | 27 | 30/30 | same, -From 2700
  - ep3 hub | 4436-4675 | 677 | 24/24 | -Episode 3 -Pad "+1800:cross:10,+90:cross:10,+600:rec,+20:lup:90,+90:rright:60,+60:lup:60,+60:cross:6,+30:lleft:60,+60:lup:80" -Frames 240
  - ep0_museum | 4791-4940 | 266 | 15/15 | corpus.txt pad as before
  - ep3 brief | 2900-3139 | 463 | 24/24 | -Episode 3 -NoStart -From 2900 -Frames 240 (the title menu over the museum attract, ep3 card)
  - ep3 recon_int | 4264-4503 | 530 | 24/24 | -Episode 3 -Job t3_recon_int_intro -Direct -Pad "+2300:rec" -Frames 240
  - ep5 hub_street | 4468-4707 | 545 | 24/24 | -Episode 5, pad as ep3 hub
  - ep8 hub | 4461-4700 | 398 | 24/24 | -Episode 8, pad as ep3 hub
  - ep7 boss | 11388-11507 | 255 | 12/12 | -Episode 7 -Job m7_boss -Direct -Pad "+9400:rec" -Frames 120 (arena after ~9300 VBlanks of cutscenes)
  - ep8 boss | 6647-6886 | 553 | 24/24 | -Episode 8 -Job m8_boss -Direct -Pad "+4700:rec" -Frames 240 (heavy debris: 41.2k prims, 6.3k states max)
  - m1_brawl | 4444-4623 | 276 | 18/18 | corpus.txt pad
  - ep2_hub | 4279-4458 | 582 | 18/18 | corpus.txt pad
  - ep4_hub | 4454-4633 | 486 | 18/18 | corpus.txt pad
  - load_m3 | 2294-2533 | 235 | 48/48 | corpus.txt how

## In progress
- m5_battlezone (+2902:rec), m6_rc_feedback (+1222:rec); scouting ep1 hub (Binocucom briefing at hub entry: the scripted pad lands in it).

## Next
1. Remaining: ep3 rope (live), ep5 hub_street, ep6 rc_battle, ep7 boss, ep8 hub, ep8 boss, gpu-C ep1_hub (live),
   m1_brawl, ep2_hub, ep2_photo (live), ep4_hub, hack_minigame (live), fmv (live), load_m3, pause_save (live),
   thiefnet, ws_museum169 (16:9, hud centered), ws_ep3_hud (16:9), m5_battlezone, m6_rc_feedback.
2. corpus.txt windows/MB/how; baseline.json + vram_hashes.txt regenerated (gpu_diff_all -Update; gs_vram_hashes).
3. Gates: gpu_diff_all 28/28, m9_exact S1+S3 -Game, sanity (broken shader constant detected).
4. notes/d3d11-renderer.md corpus section.

## Findings / traps
- Recording settings: settings defaults are now 16:9, renderer gpu, interlacing off (filters modern -> ps2_progressive
  = 1 changes the game's field offsets via recomp patches). The original corpus (2026-10-14 and before) was 4:3,
  interlaced, sw. So every run sets PS2_GS_BACKEND=sw, PS2_ASPECT=4:3 (16:9 for ws_*), PS2_PROGRESSIVE_FIX=0,
  PS2_USER_DIR=build\gsrec\<name>\user.
- gs_record_scene.ps1 does not set PS2_USER_DIR: set it per run (scratch under build\).
- The boot's "create a save file?" prompt is skipped by the PC port (pc_save_boot_create): PS2_PC_TEXT=0 brings it
  back (rv_prompt). The standard path (fresh card, square at 600) now reaches the "Sly Cooper the thief" card at
  ~1980-2250 and the title menu's START at ~2313 (hook anchor); the Binocucom briefing from ~2430.
- With a warp card the safehouse menu is already up at START+500 (loads are fast now); ep3 brief (old: "title / menu
  at load", 2900-3139) is the title menu itself: -NoStart.
- First VBlank of a recording now verifies identical too (rv_*, title, brief).
- Direct-start timing relative to START is stable (ep8 boss: two takes with +4200 had the same profile); absolute VBlanks vary by ~250.
- ep8 boss: debris fall is light (6k prims) for ~480 VBlanks, then heavy (35-41k prims, 6k states) for ~700: +4700.
- m7_boss direct: ~9300 VBlanks of cutscenes before the arena (old recordings were made at v6400: different flow then).
