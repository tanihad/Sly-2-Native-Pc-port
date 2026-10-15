# WIP: HUD edge anchoring, run-time aspect switch, Binocucom 16:9 (branch next/hud)

**Goal**: HUD widgets at the 4:3 screen's left/right anchor to the 16:9 edges (setting
`[display] hud = edges|centered`), aspect/hud switchable at run time, Binocucom overlay covering 16:9.

## Done
- RE of the 2D system (see Findings). Patches (recomp/patches.py): ws_scene_begin/end (0x213300 entry,
  0x2137E8), ws_item_draw (0x213408), ws_dl_begin/end/object (0x21A998, 0x21AA54, 0x21A9F8),
  ws_dl_queue_mask (0x21AA24), ws_item_queue (0x2159CC).
- aspect.c: per render entry: scene, root-level object, anchor; scene policy table by object id
  (0x36B health, 0x36D/E/F counters: by root) + automatic "meter" scenes (sprite id 0x34F/0x350:
  health and every boss meter) -> by root (third of the scene width the root's origin tx is in).
  HUD_BEGIN(anchor) per change. PS2_HUD_LOG=1 diagnostics (EE scene/root dumps, GS PHASE extents).
- settings: hud_centered (display.hud = edges|centered, PS2_HUD env), template text (F7/F8 mention).

## In progress
- Verify boss (0x78A meter top right -> R) and health (L): runs build\warp\hud_boss_edges / _centered.

## Next
1. Gadget button icons text (0x1DAC58, HUD manager 0x3DA0F0) - where on screen? anchor?
2. Run-time switch: hotkeys F7 aspect (16:9 <-> 4:3), F8 hud; apply at main loop (patch before
   0x1A75A8 after camera update 0x14D470: recompute frustum by calling f_0014DB70(0x2DDF40));
   presenter reads ps2_aspect live; settings_set to remember.
3. Binocucom (0x373, 0x1A3020 owner 0x2E1E60): dim at the sides?
4. Tests: gs-draw t_hud_hints L/R; window test_settings hud key; run_tests patch guards.
5. Docs: notes/widescreen.md, notes/settings.md. Screenshots build\screenshots\hud_*.png. 4:3 identity.

## Findings / traps
- 2D = Flash-like scenes (class vt +0xC 0x2DAF30 / 0x2C9670, size 640x480 at +0x94/+0x98, GS
  offsets +0x9C/+0xA0 = 2048 - w*512/640/2, +0xA4 layer (sort key << 18, modal test 0x2137F0),
  +0x18 object id, +0x84 sprite instances (0x30 each, +0x80 count), +0xFC queued item list).
  Draw 0x213300: 0x2185D0 queues root sprite 0's display list (0x2185F8 -> 0x21A998, recursive via
  0x215960 kind 4), items = 0x70-byte placed objects {+0 color, +4 mul, +0x10 world matrix a b c d
  tx ty, +0x30 character (kind +4: 1 shape, 3 text, 4 sprite, 6/7 ?), +0x34 type (1 mask begin,
  4 mask end), +0x58 frame, +0x61 flag}; then 0x2130C0 set-up and the loop at 0x213408 drawing
  each item with 0x2159E0 (1 -> 0x2174C8, 3 -> 0x216BC8 text, 6/7 -> 0x2177E0).
- Object lookup by id: 0x15E938(world 0x3DD490, 0x405, oid, 0). HUD manager 0x3D9E70 (init
  0x1D9520, update 0x1D9648): +0x10 health (0x36B), +0x60 (0x370), +0x250 ticker (0x371),
  0x2E1E60 Binocucom (0x373), +0x280 gadget menu (0x375/376/377 per character) with button icons
  text 0x1DAC58, +0x2E0 (0x378). Counters at world+0x3BA0 {0x371, 0x36D coins, 0x36E, 0x36F}.
  Briefing scenes 0x35F/0x368, 0x360/0x369, 0x361/0x36A (0x19D540). 0x362/0x365 (0x201DC8).
- Boss meters are level objects (e.g. ep1 Dimitri 0x78A) found through boss classes (+0x2A0 in
  0x17FD08 class, +0x568 in 0x1D4940 class): detected by their 0x34F sprite.
- Root tx is the widget's origin; the drawn art can be far from it (ticker arrow tx 188 drawn at
  x 489..527): fine for meters/counters, so only listed/meter scenes use it.
- Pad script "+N" tokens are relative to the previous token, not to START.
- Worktree files are CRLF: edit with the Edit tool.
