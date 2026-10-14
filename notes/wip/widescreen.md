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

## In progress
- verify first 16:9 frames (museum, pause) with HUD_BEGIN(C) around rp21 entries.

## Next
1. 14F308 (x 4:3-normalised: f4 *= (4/3)/A after 0x14F36C) + inverse 14F268 (0x14F2B4 f2) + 1CD7E0 size.
2. Portrait classes 8/9 (rp 15/18): check visually; probably wrap in HUD_BEGIN(C) instead of the
   community 0x1BE808 patch.
3. Movies -> ASPECT_4_3 hint (find movie player start/stop; gs_write_reg direct is OK: EE time).
4. Remove aspect_debug_trace (TEMP) from aspect.c/.h.
5. Unit tests: settings parser, HUD transform (gs-draw), window fit_aspect; run_tests.py guard test
   for patches; all tests/unit/*/run.ps1.
6. Verification runs + screenshots build/screenshots/ws_*.png; 4:3 bit-identity vs main (gs_replay).
7. notes/widescreen.md, notes/settings.md, recompiler-design.md section, controls.md.

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
