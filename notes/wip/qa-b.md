# qa-b: visual QA of the Modern renderer, part B (episodes 5-8, bosses, effect-heavy scenes)

Branch next/qa-b (main 38a38df), worktree C:\Users\tanih\Projects\sly2-worktrees\qa-b. Report goes to the caller via SubagentHandback.

## Status
- [x] release build in worktree (build\rel\sly2.exe) done (6 jobs)
- [ ] baseline sweep tf for ep5..ep8, m8_boss
- [ ] matrix runs
## Findings
(none yet)
- [x] harness: scripts\qa_scene.ps1 (scene + config via sly2.ini + env, tf check, digest, shots), qa_scenes.txt, qa_batch.ps1; output build\qa\<scene>__<config>\
- [x] batch 1: balanced @60 over all part-B scenes -> build\qa\batch_balanced60.txt (sheets build\qa\sheet1/2.png). Many scenes at At 5200 sit in cutscenes / briefings / comic pages (m5_boss1, boss2, chase, ewok, m6_train_red, rc_battle, bearcubs, heist_rc): need later At.
- [ ] batch 2: exact / performance / quality @60 on m5_battlezone, m7_lighthouse, m8_tntbarrel, m7_laser, m8_boss -> build\qa\batch_cfg60.txt (running)
## Traps
- Dump runs at 1440p MFT: ~20-30% of the pictures are 2048x1344 grid frames ("not modern") that a no-dump run does not have (counters: nodump 0 exact after loading). Use -NoDump + mft digest for fallback counts; dump runs for temporal / pictures. Cause not yet found.
- PS2_MFT_LOG has a 400-line budget; the log v<n> is not the dump VBlank number.
- Do not kill by name pattern: other agents run sly2_warp_* exes too.

- GPU governor ("the GPU could not draw it in real time ... drawing at 2x/1x") kicks in on this shared machine and lowers the grid scale (pictures 5.3 MB = 2x, 0.9 MB = 1x). qa_scene now sets PS2_GS_GOVERNOR=0 (use -Gov to keep it). Pictures of other sizes are still seen: grid frames ("not modern" = no R2 op in the VBlank) in bursts of 6-12 in the ep5 hub in dump runs (with PS2_GS_GOVERNOR=0 too). No-dump runs: 0 grid frames in steady hub play, but 25-30% in loading/safehouse phases. Not yet attributed.
- tf flags near size changes (grid <-> MFT neighbours) and in fast camera motion: false positives; judge only runs whose pictures are all one size.

## Results so far (2026-11-03)
- batch 1 (balanced 60, dumps): no flicker verdict that is not explained by size changes / cutscenes / fast motion. m5_chase seams 11, ep6_hub blobs 750, ep8_hub rows 10 / run 0.209 are all on pictures next to a size change (grid <-> MFT) or in cutscenes; reference runs also show seams (m5_battlezone exact: 14).
- Dump runs: Performance (75%, no MSAA) -> 60 of 60 pictures modern; Quality (100%, 8x) 15 of 60 grid pictures in 4 scenes; Balanced mixed. Cutscene / briefing frames are grid frames by design (no R2 ops: "not modern"; "target view" = a draw that samples a display target).
- nodump series (digest series.txt = exact frames per 300 VBlanks and reason): m6_paraglide ALIAS16 x~1500 frames in a row (+ "target view" ~900), m5_ewok "target view" ~3000 frames, m5_ghost ~1500, m5_battlezone ~900, m7_boss 40-80 "not modern" per 300 all through (also with PS2_R2_BUDGET_MS=100 and KEEPCHAIN=0 VIFSTATE=0: not from the flicker fix).
- qa_pair.c (new): ALT pairs MFT vs grid: quiet scene baseline 5-13 cells / pair (of 4800), moving scene up to ~100.

## Final state (2026-11-03)
- ALT sweep (balanced, qa_pair): quiet scenes 0-14 mean cells (m5_battlezone, m5_turret, m7_laser, m5_boss1, ep6/8 hubs); busy 40-390 are motion between consecutive pictures; dump runs also thin the pairs (dumping causes grid fallbacks; qa-a tip: sample every 20-25th VBlank).
- Unconfirmed (1 of 2 runs): m7_thermals ALT pair 5254/5255: modern picture has a full-width dark banded strip across the water (rows ~130-230 of 360), exact has a thin line. Second run (same params) did not reproduce.
- Confirmed twice: long all-grid stretches (counters, no dumps): m6_paraglide ALIAS16 ~1500 frames, m5_ewok "target view" ~3000 frames, m5_ghost / m5_battlezone / m6_train_red "target view" 700-1500; m7_boss 10-25% "not modern" through the fight.
- COMPLETE: report delivered via SubagentHandback.
