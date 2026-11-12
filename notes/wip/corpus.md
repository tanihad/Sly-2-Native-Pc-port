# WIP corpus: re-record the GPU gate corpus (branch next/corpus, worktree sly2-worktrees\corpus, from main 336e78e)

**Status: complete**

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
  - m6_rc_feedback | 3164-3223 | 80 | 12/12 | -Episode 6 -Job t6_rc_chopper_battle_intro -Direct -Pad "+1222:rec" -Frames 60 -RecDumpEvery 5
  - m5_battlezone | 4847-4906 | 137 | 12/12 | -Episode 5 -Job m5_battlezone -Direct -Pad "+2902:rec" -Frames 60 -RecDumpEvery 5
    (tank in the rain, 31.5k translucent prims/frame; the green gas cloud of the old take NOT found: no input = no
    cloud; live driving/firing for a while did not show one either)
  - ep6 rc_battle | 4950-5069 | 273 | 12/12 | same job, "+3000:rec" -Frames 120
  - ep1_hub | 7217-7396 | 436 | 18/18 | -Episode 1 -Pad "+1800:cross:10,+90:cross:10,+3400:rec,+10:lup:90,+90:rright:60,+60:lup:60,+60:cross:6,+30:lleft:60" (ep1 hub entry has a ~3000-VBlank Binocucom briefing)
  - thiefnet | 4020-4119 | 349 | 10/10 | corpus.txt how with -Make "--coins=5000"
  - hack_minigame | 3459-3638 | 33 | 17/18 (first VBlank, as before) | -Episode 5 -Job t5_heist_free_carmelita_hack -Direct -Pad "+1500:rec,+5:lup:60,+0:rleft:50,+60:lleft:50,+0:rup:50,+50:lright:60,+0:rright:60"
  - fmv | 2842-3081 | 132 | 24/24 | PS2_MOVIE_LAYER=0 -Episode 2 -Set episode.palace.percent_complete=100 -Pad "+600:up:10,+40:up:10,+40:up:10,+60:cross:10,+150:rec,+30:cross:10" -Frames 240
  - pause_save | 4640-4789 | 416 | 15/15 | PS2_MOVIE_LAYER=0 -Episode 2 -Pad "+1800:cross:10,+700:start:10,+60:down:10,+60:cross:10,+60:rec,+20:up:10,+50:cross:10" -Frames 300, cut to 150 (53.2k prims max, 17148 uploads = old)

## Results (main 336e78e code)
- gpu_diff_all mixed S=1 28/28 and ref S=1 28/28 on the regenerated baseline (and mixed 28/28 again after the sanity
  revert, gs_replay rebuilt from the worktree).
- m9_exact -Game S=1 + S=3, base = new (same gs_replay): 56/56 identical.
- Sanity: gs_replay built from a throwaway runtime copy with tfx MODULATE red `>> 6` (gs.hlsl): gpu_diff_all mixed
  FAILED 28/28 (sw hashes = baseline, every recording beyond the picture gate, min PSNR 7.9-30.6 dB).
- gs_vram_hashes sw 28/28 -> tests/gpu/vram_hashes.txt (now all 28; the old file had 25).

## Not matched
- m5_battlezone: the old take's green gas cloud (not found without input, nor in a while of live driving/firing).
- ep3 rope: Sly beside the salmon rope on the boss roof (m3_boss start), not on a rope.
- Windows / sizes differ from the old ones (load timings changed); old 13's exact input was never recorded.

## Findings / traps
- Recording settings: settings defaults are now 16:9, renderer gpu, interlacing off (filters modern -> ps2_progressive
  = 1 changes the game's field offsets via recomp patches). The original corpus (2026-10-14 and before) was 4:3,
  interlaced, sw. So every run sets PS2_GS_BACKEND=sw, PS2_ASPECT=4:3 (16:9 for ws_*), PS2_PROGRESSIVE_FIX=0,
  PS2_USER_DIR=build\gsrec\<name>\user.
- gs_record_scene.ps1 now sets these corpus settings itself (-Aspect, -Hud, -ExtraEnv; PS2_USER_DIR = build\gsrec\<Name>\user).
- The boot's "create a save file?" prompt is skipped by the PC port (pc_save_boot_create): PS2_PC_TEXT=0 brings it
  back (rv_prompt). The standard path (fresh card, square at 600) now reaches the "Sly Cooper the thief" card at
  ~1980-2250 and the title menu's START at ~2313 (hook anchor); the Binocucom briefing from ~2430.
- With a warp card the safehouse menu is already up at START+500 (loads are fast now); ep3 brief (old: "title / menu
  at load", 2900-3139) is the title menu itself: -NoStart.
- First VBlank of a recording now verifies identical too (rv_*, title, brief).
- IPU movies: the PC movie layer presents them natively (640x480 live dumps): PS2_MOVIE_LAYER=0 for fmv / pause_save
  so the live dumps are GS pictures (the GS stream has the macroblock uploads either way: 120000 in fmv).
- 16:9 runs: PS2_DUMP_RAW=1 (live dumps unshaped), else gs_corpus_verify sees 796x448 vs 512x448.
- -Make "--coins 5000" loses its quoting through the wrapper: use --coins=5000.
- Direct-start timing relative to START is stable (ep8 boss: two takes with +4200 had the same profile); absolute VBlanks vary by ~250.
- ep8 boss: debris fall is light (6k prims) for ~480 VBlanks, then heavy (35-41k prims, 6k states) for ~700: +4700.
- m7_boss direct: ~9300 VBlanks of cutscenes before the arena (old recordings were made at v6400: different flow then).

