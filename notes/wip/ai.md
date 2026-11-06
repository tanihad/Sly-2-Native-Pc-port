# WIP: AI texture upscaler, phase 1 (branch next/ai, worktree C:\Users\tanih\Projects\sly2-worktrees\ai)

STATUS: COMPLETE (2026-11-06). Design, results, verdict, phase 2 plan: notes/ai-textures.md.

Goal: Real-ESRGAN x4 anime 6B on our own D3D11 compute shaders; upscale the test set (ep1/ep3/ep6 hubs, m2 boardroom
dumps); 3-way in-game pictures Original | Enhanced | AI (build\ai\shots); verdict + phase 2 plan.
Owner direction (2026-11-06): Look = Retro / Faithful / Reimagined; AI is for Reimagined (bold detail welcome).

## Done
- scripts/ai_model.py: safe loader (restricted Unpickler, sha256 check; refuses os.system / eval / Popen pickles),
  arch check (64/32/6 blocks, 4.47 M params), export build\ai\x4anime6b.bin, pure-Python reference.
- runtime/gpu/ai_up.c/.h + shaders/ai_up.hlsl, scripts/ai_up_tool.c (cpu / gpu / batch / alpha), ai_textures.py
  (selftest, gpucheck, upscale), ai_shots.py (3-way pictures).
- selftest: python vs C 4.8e-7, C vs GPU 1.0e-5; gpucheck 70x37 3.3e-6.
- test set 1173 textures: net 22 s on the 4060 Ti, 1.33 GB VRAM peak (351 MB / 60 s with --max-pixels 32768).
- pictures build\ai\shots (kept); dumps, packs, frames deleted.

## Findings / traps
- No numpy / PIL anywhere (venv or system): the reference is pure Python (8 s for 5x4) + a C reference.
- d3dcompiler: a switch over 8 UAVs inside the unrolled kernel never finished compiling; permutations ~4 s each
  (DXBC cache via AI_UP_CACHE).
- The dither pre-smoothing fired on 95 % of textures and erased all grain: off; the net removes dither itself.
- Snow particles (soft dots) become rings; m2 pool grout fades; anime 6B flattens material grain (conservative).
- PowerShell: `($_ -split ' ')[4]` on "score n WxH hash" is $null (index 3); Set-Content -Encoding utf8 adds a BOM.
- tex_shots A/B runs: the two runs' Original pictures differ 3-6 / 255 (Sly's idle animation): poses differ per column.
