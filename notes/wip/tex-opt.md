# WIP: Reimagined AI textures lighter and faster (tex-opt)

**Status: in progress**

Branch next/tex-opt from main 336e78e, worktree sly2-worktrees\opt. Do not push / merge.
Tools: the worktree has no tools\ or .venv: `. C:\Users\tanih\Projects\sly2-decomp\scripts\env.ps1` then cd here.
Build: `python scripts\build_game.py --release --out build\rel`. Model: build\models\x4plus.bin (copy of ai's; never commit).
Seeds (copied from worktree ai): build\prebuild\texcache_warm, texcache_lists2, build\reimagine\texcache.
Owner's cache copy (read only source, 887 files 29 MB): build\texopt\owner_textures.

## Goal
BC7 AI images in VRAM, fp16 net, dedupe, full-build feasibility; no visible quality loss, measured.

## Done
- (nothing yet)

## In progress
- baseline build of 336e78e (build\rel -> copy to build\texopt\base)

## Next
1. Dedupe analysis of the caches (raw = key with bits 60..62 cleared: flag / ref variants).
2. BC7 encoder (shaders, own) + tool: probe partition tables on HW, PSNR over owner + warm caches, ms per M texels.
3. fp16 storage net (ai_up.hlsl / ai_up.c), tool benchmark + diff vs fp32.
4. Game: BC7 pool for AI images (Reimagined only), fp16 net; keep cache key x4plus-v3.
5. Full-build feasibility (warp through levels), prototype script.
6. Measurements at owner's settings + gates.

## Findings / traps
- Worktree files are CRLF.
