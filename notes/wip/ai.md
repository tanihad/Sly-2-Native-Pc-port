# WIP: AI textures (branch next/ai, worktree C:\Users\tanih\Projects\sly2-worktrees\ai)

Phase 1 (prototype, anime 6B): COMPLETE 2026-11-06, notes/ai-textures.md.

## Phase 2 goal (2026-11-06): the textures half of Look = Reimagined
Bold AI textures (x4plus / chain), height + normal maps for the lighting agent (gpu_tex_normals.h, notes/reimagined.md),
in-game background build on the GPU into a local cache (user dir), Enhanced fallback, Faithful / Retro bit-identical.

STATUS: IN PROGRESS

## Plan
1. [~] quality eval: dumps (ep1, ep3, ep6, m2: 1120 textures) -> packs build\ai\eval\pack_{x4,x4g,chain,dd}; texture
       sheets: x4plus alone cleanest (text legible); chain adds orange / white outline artifacts and garbles small text;
       dedither garbles small text; grain brings back dither dots. In-game x4 / x4g runs: scratchpad eval_runs.ps1
2. [x] height + normal maps (1a24f82; preview pending) (compute), gpu_tex_normals.h interface, notes/reimagined.md
3. [x] runtime (1a24f82; first ep1 run: 237 built + saved, JPEG ~19 KB each): x4plus net in the game (hlslc perms), time-sliced per frame, cache files in <user dir>\textures\ai,
       Enhanced fallback, VRAM budget, Look = Reimagined only
4. [ ] census of the game's textures, projected build time
5. [ ] gates + perf + pictures (build\ai\shots2)

## Phase 1 done (kept)
- scripts/ai_model.py (safe loader), runtime/gpu/ai_up.c/.h + shaders/ai_up.hlsl, scripts/ai_up_tool.c,
  ai_textures.py, ai_shots.py; selftest python vs C 4.8e-7, C vs GPU 1.0e-5. Pictures build\ai\shots.

## Findings / traps
- tex_shots -Ini does NOT apply when PS2_USER_DIR is set (sly2.ini is read from the user dir): use PS2_TEX_AI=1.
- ai_up.c had L[256]: x4plus has 351 layers (crash) - fixed (AI_MAXOPS 512).
- x4plus offline 0.2 Mpx/s (7 TFLOPS) with VALORANT on the GPU; padding 12 vs 24: edge diff 0.9/255 (fine).
- launching a .ps1 with -File and an array param: "a,b" arrives as ONE string: split inside.
- No numpy / PIL anywhere (venv or system): pure Python + C. d3dcompiler: no switch over UAVs in the unrolled kernel;
  permutations ~4 s each (DXBC cache via AI_UP_CACHE).
- The dither pre-smoothing fired on 95 % of textures and erased all grain: off; the net removes dither itself.
- Snow particles (soft dots) become rings; m2 pool grout fades; anime 6B flattens material grain (conservative).
- PowerShell: `($_ -split ' ')[4]` on "score n WxH hash" is $null (index 3); Set-Content -Encoding utf8 adds a BOM;
  [IO.File] resolves relative paths against the process cwd (the main checkout): absolute paths only.
- tex_shots A/B runs: the two runs' Original pictures differ 3-6 / 255 (Sly's idle animation): poses differ per column.
- 2026-11-06: VALORANT was running on the owner's GPU (39 % busy): perf numbers noisy - check Get-Process first.
