# ep2: playtest Episode 2 "A Starry Eyed Encounter" (India)

**Goal**: play episode 2 (hub world 8, jobs m2_*) as far as possible with warp + scripted/live
input; fix every runtime/recompiler problem found at its root, with tests; frames in
build\screenshots\ep2_*.png.

## Done
- Merged next/warp (178353c). Release build in build\rel, debug build in build\sly2.
- Episode 2 start (release, live input in build\live_ep2): title -> safehouse -> X -> Sly in the
  palace grounds hub; walking, pause menu, Job Help ("Recon the Ballroom") all fine.
- Direct starts (debug, warp_run -Direct, build\warp\ep2_<job>_direct\sheet.png): m2_recon,
  m2_turret, m2_rc_chopper, m2_dance_audition_fake (cutscene with subtitles), m2_lower_drawbridge,
  m2_steal_tuxedo (world 9): all load and play, no new log lines (only one
  "[sd] VoiceTrans ... overwrites the block voice c0 v4 is playing" at a level load).

## In progress
- Direct starts batch 3: m2_dance_audition, m2_boardroom_brawl, m2_elephant_rampage.

## Next
1. Direct starts: m2_heist_bomb_bridge, m2_heist_dance, m2_heist_cover_murray.
2. Live play inside jobs (debug build + PS2_INPUT_FILE): rc_chopper bombs, turret, dance minigame.
3. Audio (-Wav) and perf (PS2_PERF) checks in the hub and a job (machine is heavily loaded by
   other agents: ~11 sly2.exe at once, release hub ran at 15-27 VBlanks/s, VU 25-50 ns/pair).

## Findings / traps
