# play-1: from the Binocucom briefing into gameplay

**Goal**: push the game from the Bentley/Sly Binocucom briefing into gameplay and beyond, fixing
runtime/recompiler blockers at the root; check rendering per stage; best frames in build\screenshots.

## Done
- **Gameplay works** (Cairo Museum Break-in, the prologue): briefing ends by itself, Sly
  walks/runs/jumps/double-jumps/attacks, bounces on the trampoline, balances on railings, climbs
  the stairs; HUD; camera (right stick). Pause menu ("WE'LL BE RIGHT BACK!": Keep Playing / Save
  and Quit / Options / Controls), Gadget Grid (SELECT), Binocucom view (R3) all work.
- **Memory card save works**: pause -> Save and Quit -> Yes writes save1 (21512 bytes) with the
  "progress is being saved" notice, then the game restarts at the intro; the title then says
  "Press START button to Resume Game" and START resumes in the museum (no briefing).
- Merged fix/fix-perf 7491a63 (GS thread + parallel pixel workers): debug build ~52 VBlanks/s
  instead of ~26 (boot -> gameplay ~90 s).
- **GS AA1 lines** (gs_draw.c draw_line, unit test in t_lines_points): Sly 2 draws its character
  outlines as AA1 lines, colour 0x101010, ZTST GREATER, ATST NEVER + AFAIL RGB_ONLY (then a second
  non-AA ZB_ONLY pass). The GS covers the two pixels straddling the line with coverage alpha; we
  drew one pixel, so where that pixel was inside the silhouette (Z fails) the outline vanished.
  Now: two pixels, coverage replaces As; P.iip forced on for AA1 so the per-pixel alpha survives
  the flat-shading path (prim_iip keeps the real IIP for plane setup).
- Syscalls: Get/SetOsdConfigParam (0x4B/0x4A: US console, English, 4:3), iFlushCache (0x68),
  sceSifStopDma (0x6B) no-ops (were "not implemented").
- Debug/play tooling (all documented in the file headers):
  - PS2_INPUT tokens: `+n:btn` (relative), `@func[/ra][+n]:btn` (after a call of a recompiled
    function, optionally from one call site), `@!func[+n]:btn` (after it went silent for n frames),
    `none`, `lxHH/lyHH/rxHH/ryHH` (partial stick), `ram` (dump EE RAM). Every press logs pad frame
    + VBlank. Unit-tested in tests/unit/window. Anchors need the debug build.
  - PS2_INPUT_FILE: live input, lines appended while the game runs (scripts/live_game.ps1 starts
    a background game; scripts/live_look.ps1 sends input, waits, tiles the newest frames into
    build/live/look.png and prints PS2_PEEK values as floats; scripts/live_goto.py steers the player
    to world x,y waypoints).
  - PS2_DUMP_FROM, PS2_DUMP_RING (keep newest n), <dump>/latest.png, PS2_DUMP_RAM=<vblank,...>,
    PS2_CALLS with up to 8 windows, scripts/contact_sheet.py (tiles, --crop/--zoom).

## Scripts that work (save file on the memory card)
- New game: `PS2_INPUT="600:square:12,@187D80/1DD96C+30:start:10"` (square only if the memory
  card is empty), gameplay after the briefing: `...,@!227820+90:none,+30:lup:100,...`
- Resume: `PS2_INPUT="@187D80/1DD96C+30:start:10"` -> gameplay at ~v3000.
- 0x187D80 called from 0x1DD968 (ra 0x1DD96C) = title screen's START check (only once the title
  accepts input); 0x227820 = Binocucom dialogue text (every frame of a Binocucom conversation).

## Prologue route (not finished by remote control)
From the wiki (slycooper.wiki.gg "Cairo Museum Break-In"): bounce on the trampoline below the whale
skeleton up onto the skeleton, walk along it to the tail, leap to the elevator switch (holographic
marker), press Circle. Then Bentley hacks the security computer (lasers/spotlights off, gate open),
Sly meets Murray (ropes: jump + Circle), Murray lifts the gate, exhibit room, Carmelita/Neyla.
World coordinates (Z up, floor 625 = 0x2DE1C0.z): start (4915,-5211); stairs top/door (6262,-3387,845);
blue dome trampoline ~(5130,-2790) (north of the hall centre); **brown drum trampoline right under
the whale skull at ~(5300,-4586)**, next to the start: `scripts\live_climb.ps1` walks to
(5060,-4740) and hops onto it; Sly then bounces automatically to the camera-focus ceiling z=1661
beside the skull (the focus z seems clamped there, so it does not tell whether he stands on bone).
Steering west at the apex carries him ~500 units along y=-4571 to x~4790, then he descends towards
(4690,-4660) and falls; slow/partial-stick walking falls off sooner. The real player position
(not the camera focus) has not been found in RAM yet; the bone matrices in the 0x306000/0x35F000
scratch buffers carry it per frame. Security station / lasers on the north balcony around
(4300,-900,~1200); Binocucom briefing camera (5434,-4595,952) looking (-0.30,0.95,0.07).

## Next
1. Get onto the whale skeleton (needs a view of it from the trampoline; try steering at the peak of
   a double jump, or Circle near its blue sparkles), switch -> Bentley's hacking minigame.
2. After the prologue: Episode 1 (Paris) load, episode intro movie, hub; watch for blockers.
3. Bottom line of the display shows the other buffer's first row (circuit 1 DBY=1 reads row 224):
   faithful to the hardware (overscan), left as is.

## Findings / traps
- VBlanks are wall-clock driven; the voice streams (spiop model) too, so the briefing length in
  VBlanks depends on emulation speed -> anchor scripts to game code, not to absolute frames.
- Title: 0x1DD920 (title handler) runs from boot on (~v300) but ignores START until this+0x24 != 0.
- Player position 0x2DE1C0 (camera focus: x,y follow Sly, z = ground + 125, does not follow jumps),
  camera at 0x2DDFA0 (3x3 rotation, row 0 = view direction) + position 0x2DDFD0. The stick is
  camera-relative; camera rotation with the right stick is fast and hard to control blind.
- Level text is in RAM after loading (objectives "Power up the elevator", "Rendezvous with
  Murray", "Locate Clockwerk Parts", all Binocucom lines): `strings` on a RAM dump (token `ram`).
- The big brown rounded shape near the start is real geometry (textured PSMT8 + additive pass).
- Rendering looked right in every scene seen (museum, menus, pause, Binocucom, saving notice);
  outlines are thin and low-contrast in this dark level (as the GS stream specifies, now with AA1).
