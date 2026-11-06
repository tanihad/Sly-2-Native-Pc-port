# WIP: AI texture upscaler, phase 1 (branch next/ai, worktree C:\Users\tanih\Projects\sly2-worktrees\ai)

Goal: Real-ESRGAN x4 anime 6B on our own D3D11 compute shaders; upscale the test set (ep1/ep3/ep6 hubs, m2 boardroom
dumps); 3-way in-game pictures Original | Enhanced | AI (build\ai\shots); notes/ai-textures.md with verdict + phase 2 plan.
Owner direction (2026-11-06): judge as "Reimagined" look (bold detail welcome; Faithful keeps Enhanced). Real defects
(seams, broken cut-outs, garbled text, mush) still count. If anime 6B is too conservative, say what would go further.

## Done
- scripts/ai_model.py: safe loader (restricted Unpickler, sha256 check), arch check (64/32/6 blocks, 4.47 M params),
  export build\ai\x4anime6b.bin, pure-Python reference.
- runtime/gpu/ai_up.c/.h + shaders/ai_up.hlsl (one conv per dispatch, permutations per buffer set, DXBC cache in
  build\ai\shcache via AI_UP_CACHE), scripts/ai_up_tool.c (build\tools\ai_up.exe cpu / gpu / batch).
- selftest 5x4: python vs C max 4.8e-7, C vs GPU 1.0e-5; gpucheck 70x37 C vs GPU max 3.3e-6.
- release build build\rel; dumps: build\ai\user\textures\dump (1173 PNGs) + Original/Enhanced A/B shots in
  build\tex\shots\<scene>__enh (tex_shots.ps1, At 5600 ep1/ep3, 5400 ep6/m2, -Frames 70 -Every 10 -Phase 30).

## Next
1. upscale dumps -> build\ai\user\textures\<hash>.png (scripts/ai_textures.py upscale); time + VRAM.
2. AI shots: tex_shots -Tag ai (same params, HD pack present), pick B-phase frames (5620/5630; ep6/m2 5440/5450).
3. 3-way compose + close-ups -> build\ai\shots; judge; notes/ai-textures.md; delete dumps / frames.

## Findings / traps
- No numpy / PIL anywhere (venv or system): the reference is pure Python (8 s for 5x4) + a C reference.
- d3dcompiler: a switch over 8 UAVs inside the unrolled kernel never finished compiling; permutations ~4 s each.
