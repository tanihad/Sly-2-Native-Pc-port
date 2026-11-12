# WIP corpus: re-record the GPU gate corpus (branch next/corpus, worktree sly2-worktrees\corpus, from main 336e78e)

**Status: in progress**

**Goal**: the 28 GS recordings of tests/gpu/corpus.txt (destroyed with other worktrees' build folders) re-recorded
into a durable root C:\Users\tanih\Projects\sly2-corpus (same relative paths), verified, baseline regenerated, gates
green on 336e78e.

## Done
- (start) checkpoint file.

## In progress
- release build build\rel (build_game.py --release).

## Next
1. Record each scene with gs_record_scene.ps1 (env: PS2_USER_DIR scratch, PS2_GS_BACKEND=sw, PS2_ASPECT=4:3,
   PS2_PROGRESSIVE_FIX=0; ws_* at 16:9), gs_corpus_verify, copy .gsrec into sly2-corpus\<path>, delete frames.
2. gpu_corpus.ps1 root order; corpus.txt windows/MB/how; baseline.json + vram_hashes.txt regenerated.
3. Gates: gpu_diff_all 28/28, m9_exact S1+S3 -Game, sanity (broken shader constant detected).
4. notes/d3d11-renderer.md corpus section.

## Findings / traps
- Recording settings: settings defaults are now 16:9, renderer gpu, interlacing off (filters modern -> ps2_progressive
  = 1 changes the game's field offsets via recomp patches). The original corpus (2026-10-14 and before) was 4:3,
  interlaced, sw. So every run sets PS2_GS_BACKEND=sw, PS2_ASPECT=4:3 (16:9 for ws_*), PS2_PROGRESSIVE_FIX=0.
- gs_record_scene.ps1 does not set PS2_USER_DIR: set it per run (scratch under build\).
