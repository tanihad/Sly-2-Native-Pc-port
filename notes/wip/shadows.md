# WIP: Reimagined shadows (branch next/shadows, worktree sly2-worktrees\shadow, from main 336e78e)

**Status: complete** (2026-11-12). Results, design and gates: notes/reimagined-lighting.md "Shadows rework". Videos dropped (owner).

**Goal**: owner reports (Look = Reimagined) - (1) overlapping shadows flick (Sly under a tree's shadow), (2) title
museum rope shot: Sly brown instead of hidden in shadow. Fix the causes (re-architecture allowed, PS2_REIM3=0 = old path);
Faithful / Retro byte-identical.

## Done
- (start) worktree has no tools\ / .venv: git = C:\Users\tanih\Projects\sly2-decomp\tools\git\cmd\git.exe; scripts use
  project.ps1 ($MainRoot) for clang / python.

- Baseline exe (336e78e) at build\shadows\base\sly2.exe (+ its build\gen\gpu_shaders.h for the block compare).
- Helper scripts (local, build\shadows\tools): title.ps1 (title / intro run, dumps), batch.ps1 (cases 2 at a time),
  sheet.ps1 (contact sheet, crops), maskstat.ps1 (mean RGB under a Faithful dark mask), trees.ps1 (ep2 path, owner
  settings, 165 fps, BMP dumps).
- Report 2 reproduced + root cause (see Findings).

## Done (code, commits f59bde1, 063975e, + working tree)
- PS2_REIM3 (default on; PS2_REIM_X=64 + PS2_GPU_AB_R8=4096: A old / B new in one run).
- Game-lit mask (reim_litmask, r2_render shadow_sel 3, light.hlsl r_gamelit): lit entries from the camera, half res.
- RT_SH RGBA8: x static-world visibility, y N.L, z per-frame casters' visibility (shorter history, none on game-lit), w mask.
- History: disocclusion test with last frame's depth (RT_PW), none on game-lit.
- No relight / no local light on game-lit pixels; local-light floor off near-black (report 2).
- GSM: mft_shfuse also writes the game shadow's darkening (ps_mft_shfuse*_g / _sg, perms.txt); composite: min(game, ours).
- Static cache: main never read it; v3 keeps "no static-world shadows" by default and SKIPS drawing the cache (GPU saved);
  PS2_REIM_SC_CAST=1 reads it (radius fixed, no restarts on radius changes) - more shadow flicker / big look change.
- DBG 6 (mask), 7 (game shadow vs ours), 8 (the post pass's light x game shadow: the overlap gate's picture).
- scripts/reim_shadow_gate.ps1 (new gate), stability_check region argument.

## Results so far (code 32a7534 = build\shadows\v3k.exe)
- reim_shadow_gate PASS (v3i): stacked 2.27% -> 0.31%, world-shadow flicker 0.679x, character-light flicker 0.767x.
- Cost in-run A/B (owner settings, 165 fps, no dumps): v3i +0.15 ms, p99 +0.4; v3k (lean mask, cheap GSM) +0.03 ms,
  p99 14.22 / 14.20.
- run_tests 16/16.

## In progress
- unit suites (bg), title videos main | new (build\shadows\runs\vid_title_*), re-recording corpus scenes.

## Next
1. m9_exact / gpu_diff_all: the 28 corpus recordings are GONE from this PC (other worktrees' build folders deleted by a
   cleanup). Re-record ep0_museum + ep2_hub (gs_record_scene, corpus.txt pads) into build\gsrec, m9_exact -Only them with
   -CorpusRoot build\shadows\corpus (gpu-C\build\gsrec\...). gpu_diff_all cannot pass on new recordings (baseline sw hashes).
2. r2_sweep Faithful + Reimagined, reim_stability_gate warm + cold, gfx_stress_gate -Cold, reim_shadow_gate on final.
3. ep2 trees video old | new (two runs, same seed copies).
4. Notes numbers, Status complete.

## Findings / traps
- Report 2 (intro, not the title menu: the museum rope shot is VBlank ~2700-3300 of a boot with no input; deterministic
  with PS2_FRAME_DT_FIXED=1, dump VBlank 3100 / 3200). Sly's silhouette mean RGB (Faithful dark mask, rect 800,250,700,800
  of the 2389x1344 dump): Faithful 6.3/5.5/6.0, Reimagined 17.2/11.5/7.8 (brown). Ablations: rim+spec off 17.1/11.5/7.8,
  bloom/fog/grade/shafts/CAS/relight/fire off: unchanged; **PS2_REIM_LIGHTS=0: 5.5/4.5/5.0** -> the local lights cause it:
  lin += (lin + floor 0.025) x E lights the black silhouette from the museum's lamps. But the game already lights every lit
  entry (characters, lit props) with its own best four point lights in the VU (0x1A4478) - the post pass double-counts them
  on characters, and overrides the game where it deliberately leaves a character dark.
- Report 2 fix (f59bde1+): the cutscene's Sly is NOT a lit entry (no light block, his entries even share the world matrix):
  the lit-entry mask does not cover him. What fixed it: the local lights' floor is weighted by smoothstep(0.02, 0.08,
  picture luma) (v3) -> silhouette 8.8/6.2/6.2 (Faithful 6.3/5.5/6.0, main 17.2/11.5/7.8). The lit-entry mask stays for
  gameplay characters (lit: the VU already lit them with the same lights; mask pass = every lit draw, any pass / Z write,
  full vertex through r2_vs_shadow_at, depth 1/w; a pixel is game-lit when the nearest lit entry is at or in front of the
  picture's depth - characters are often drawn without Z writes).
- **main bug**: the static shadow cache (reimagine2) was never bound for ps_reim_shadow (t27 only bound in sc_move): the
  static world's casters (kept out of the per-frame map) cast NOTHING since reimagine2. DBG=1 on the trees path showed only
  plants / Sly. v3 binds it.
- Trees path baseline (owner settings, 165 fps, separate deterministic runs, Sly region "0.33,0.40,0.67,0.95"):
  Faithful region flicker 6.20 / motion 1.105 (5.61 per motion), pops 0.91; Reimagined main 15.78 / 1.832 (8.61, 1.53x),
  pops 1.71. The gate's dark-block metric sees nothing (0.96x).
- Faithful shows the GAME's own shadows on that path: Sly's shadow volume (lower right) and stepped dark bands on the
  ground (baked / volume); our key light casts Sly's shadow in nearly the same direction -> two Sly shadows, product.
- Separate "deterministic" runs of the trees path did NOT give the same pictures here (v3a vs base: Sly elsewhere at the
  same VBlank - hub load timing / texture cache): compare only inside one run (PS2_GPU_AB_R8=4096 + PS2_REIM_X=64: A old,
  B new; or 1024: A Faithful, B Reimagined).
- Region flicker is dominated by texture / animation content: main Reimagined with PS2_REIM_SS=0 (no key-light terms)
  8.26 per motion vs 8.61 with them (Faithful 5.61) - the AI textures / grade make most of the gap, not shadows.
- Game shadows: the fused shadow pass (mft_shfuse) runs every frame in the museum attract (12000 / 12000); GSM target
  works there after fixing the RTV (mrtv forces RGBA8: use a NULL desc). mrtv gotcha.
- Static cache now bound (v3): DBG=7 shows big key-light shadows (interiors: the roof shadows the whole museum floor) -
  a large look change vs what the owner has (main never showed static-world shadows). Decide by stills in several scenes.
- Game-lit mask cost: ~470 lit draws a frame on the ep2 path (all passes) - measure GPU ms.
- Parallel game runs: at most 2 at once (7 at once: VRAM full, the game crawled - no dumps).
