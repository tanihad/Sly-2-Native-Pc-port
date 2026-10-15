# WIP: HUD edge anchoring, run-time aspect switch, Binocucom 16:9 (branch next/hud)

**Goal**: HUD widgets at the 4:3 screen's left/right anchor to the 16:9 edges (setting
`[display] hud = edges|centered`), aspect/hud switchable at run time, Binocucom overlay covering 16:9.

## Done
- RE of the 2D system (see Findings, notes/widescreen.md "HUD anchors"). Patches (recomp/patches.py):
  ws_scene_begin/end, ws_item_draw, ws_dl_begin/end/object, ws_dl_queue_mask, ws_item_queue,
  aspect_switch (main loop 0x1A75A8).
- aspect.c: per render entry scene / root object / anchor / side fill; policies by object id
  (0x36B health, 0x36D/E/F counters by root; 0x35F-0x361 Binocucom frame C + SIDE_FILL navy) +
  automatic meters (sprite id 0x34F/0x350: health and boss meters) by root. PS2_HUD_LOG=1.
- gs_hint.h SIDE_FILL (6); gs_draw.c side_fill + hudlog diagnostics.
- settings hud = edges|centered (+PS2_HUD), template; settings_read_display; window.c F7/F8,
  WM_ACTIVATEAPP re-read, PS2_INPUT tokens aspect/hud; aspect_request/target/apply.
- Tests: gs-draw t_hud_hints (L/R/C switch, SIDE_FILL), window test_settings (hud key, read
  display, env) + test_display_switch; run_tests.py 16/16 + guards; all tests/unit pass.
- Docs: notes/widescreen.md, settings.md, controls.md.
- Verified (screenshots build\screenshots\hud_*.png): museum, ep1 hub, ep3 hub + L3 counters
  (top right at the edge), ep1 boss (health L, Dimitri R; centered variant), Binocucom (side fill),
  ep1 briefing (side fill), pause (C), ThiefNet (C), job completed (C), run-time switch sequence
  16:9 -> 4:3 -> 16:9 -> centered -> edges. gs_replay of the widescreen worktree's museum 4:3 and
  16:9 recordings: main runtime vs this one, 60/60 identical each.

- Job counters: scene subclass vtable 0x2DAD90 (counts its tasks, 0x211F90) -> by root (m1_brawl
  "1/3" bottom right at the edge).
- Live 4:3 recording (ep2 hub, 120 VBlanks from 6800, PS2_ASPECT=4:3): 0 register 0x7F writes;
  replayed with main's runtime (git archive bab84fb) and this one: 120/120 identical.
- More shots: ep2 hub 4:3 / 16:9 edges / 16:9 centered (same moment), gadget grid (C), ep5 hub,
  job counter. Release and debug builds link.

**Status: complete** (branch next/hud, not merged).

## Possible follow-ups
- Other level-specific HUD scenes at a side that are neither meters nor job counters stay centred
  (none seen in ep1/2/3/5 play; PS2_HUD_LOG=1 lists scenes with object id, class and policy).
- The Binocucom side fill is opaque from the frame's first frame (no fade with the frame's
  transition).

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
  0x2E1E60 cutscene letterbox + subtitles (0x373), +0x280 gadget menu (0x375/376/377 per
  character) with button glyph text 0x1DAC58, +0x2E0 (0x378). Counters at world+0x3BA0 {0x371,
  0x36D coins, 0x36E bottles, 0x36F timed}; L3 shows coins/bottles. Binocucom frame / briefings
  0x35F/0x368, 0x360/0x369, 0x361/0x36A (0x19D540). 0x362/0x365 (0x201DC8).
- Boss meters are level objects (ep1 Dimitri 0x78A) held by boss classes (+0x2A0 in the 0x17FD08
  class, +0x568 in 0x1D4940) and script 0x10DD08: detected by their 0x34F sprite.
- 0x212E40(frac, scene, id) sets a sprite's timeline position; 0x213B00(scene, id) goto label;
  0x212A50(scene, on) show/hide.
- Root tx is the widget's origin; drawn art can be far from it (ticker arrow tx 188 drawn at
  489..527): only listed/meter scenes use it.
- Pad script "+N" tokens are relative to the previous token, not to START.
- Worktree files are CRLF: edit with the Edit tool.
