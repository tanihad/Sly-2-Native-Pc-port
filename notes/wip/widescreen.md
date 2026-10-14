# WIP: native 16:9 widescreen (branch next/widescreen)

**Goal**: runtime aspect setting (16:9 default, 4:3 bit-identical), EE patches for projection /
culling / 2D, HUD hints via GS reg 0x7F, sw-renderer HUD transform, 16:9 presentation, settings file.

## Done
- recomp/patches.py + codegen: "instruction patches" (C before/after one instruction, SHA-1 of the
  containing function guarded). Patches: ws_projection (0x14DBA4, camera aspect * A/(4/3) in
  RecalcCmFrustrum f_14DB70), ws_render_alloc (0x1CD9D4), ws_render_entry (0x158378),
  ws_render_done (0x1583FC).
- runtime/settings.c/.h (sly2.ini, PS2_ASPECT), aspect.c/.h (factors, aspect_hint into DMAS 0x2DEA28,
  UI entry classification), gs_hint.h, gs_draw.c (0x7F hints, per-primitive HUD transform at kick,
  full-width prims exempt), gs_thread.c (content43 per picture), ps2_runtime.c (display-shaped dumps,
  PS2_DUMP_RAW=1 raw), window.c (fit_aspect, per-frame aspect, default 1707x960 at 16:9,
  settings_save_if_missing).

- 14F308/14F268/1CD7E0 patches, movie ASPECT_4_3 hints (f_1B3BD0 entry/return), temp debug removed.
- Tests: gs-draw t_hud_hints, window test_fit_aspect/test_settings, run_tests.py check_patches; all
  tests/unit/*/run.ps1 pass (window also full run -NoFullscreen), run_tests.py 16/16 + guards.
- Docs: notes/widescreen.md, notes/settings.md, recompiler-design.md, controls.md.
- Verified 16:9: title, briefing (portraits fine without the 0x1BE808 patch), museum, pause,
  hubs ep1-5, ep1 boss (health pie), Binocucom (ep3 recon), memcard prompt, boot movie pillarboxed
  (content width 443 px of 796 = 4:3). Screenshots build/screenshots/ws_*.png.
- 4:3 identity: gs_replay of main recordings (title 30, museum 150, ep2 photo 240 VBlanks) with main
  vs this runtime: all identical; a 4:3 recording of this build has 0 hints (16:9: 28+28 per 60 VB).

## In progress
- camera-turn edge check (ws_ep2turn), chalk-talk movie attempt (ws_ep3movie).

## Next
1. Final report (SubagentHandback).

## Findings / traps
- CM (camera) is the global at 0x2DDF40: +0x20 fov, +0x3C aspect (1.2987 = 640/492.8), +0x40 xRange
  = tan(fov/2)*aspect, +0x44 yRange, +0x48 near radius; f_14DB70 recomputes all + projection
  (0x11C448) + frustum planes (f_14DC58). No other writer of +0x3C.
- Frame = one VIF1 DMA chain kicked once per frame: hints from EE must be written INTO the chain.
  Frame builder DMAS at 0x2DEA28 (+0 end, +4 start, +8 open CNT, +C cur, +10 VIF word mode,
  +14 open GIF prim). Render list 0x3B3980 (16-byte entries {rp, key, call, call}), alloc 0x1CD998,
  sorted+emitted by f_157FA0; class table rp->class 0x2BDD20, class begin/end 0x2BDD88.
- All 2D (HUD, menus, text, vector-art UI) = rp 21 (class 5, 0x211A80/0x211C10, VU1 prog 0xC0).
  UI virtual space 640x492.8; scene objects (f_213300 vtable draw) have GS x offset at +0x9C (1792).
  Entries by 0x2130C0 (ra 0x213278) = scene set-up: 16 Z-clear sprites full width -> must stay as is.
- Pause menu: full-width black background trifan + composition: C anchor + full-width exemption
  gives a pillarbox-like look with the menu centred.
- Sly 2 draws 512x224 field buffers (GS trace rects: halve y).
- Worktree files are CRLF (autocrlf): edit with the Edit tool, not PowerShell string Replace on `n.
