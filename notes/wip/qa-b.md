# qa-b: visual QA of the Modern renderer, part B (episodes 5-8, bosses, effect-heavy scenes)

Branch next/qa-b (main 38a38df), worktree C:\Users\tanih\Projects\sly2-worktrees\qa-b. Report goes to the caller via SubagentHandback.

## Status
- [x] release build in worktree (build\rel\sly2.exe) done (6 jobs)
- [ ] baseline sweep tf for ep5..ep8, m8_boss
- [ ] matrix runs
## Findings
(none yet)
- [x] harness: scripts\qa_scene.ps1 (scene + config via sly2.ini + env, tf check, digest, shots), qa_scenes.txt, qa_batch.ps1; output build\qa\<scene>__<config>\
- [ ] batch 1: balanced @60 over all part-B scenes -> build\qa\batch_balanced60.txt
## Traps
- Dump runs at 1440p MFT: ~20-30% of the pictures are 2048x1344 grid frames ("not modern") that a no-dump run does not have (counters: nodump 0 exact after loading). Use -NoDump + mft digest for fallback counts; dump runs for temporal / pictures. Cause not yet found.
- PS2_MFT_LOG has a 400-line budget; the log v<n> is not the dump VBlank number.
- Do not kill by name pattern: other agents run sly2_warp_* exes too.

