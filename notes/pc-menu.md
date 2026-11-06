# The in-game Options menu, the pause menu and Quit to desktop

The PC port's settings screen, as a PC release has one: pause, choose **Options**, change graphics, display, audio and
controls with the keyboard, the mouse or a controller; (menu overhaul, 2026-11-06) every change is staged and put into
effect with **Apply** (the game's paused picture behind the menu; saved to sly2.ini then, notes/settings.md), display
changes then confirmed ("Keep these display settings? Reverting in 15 s"); see "Apply / Revert" and "Pages and options".
**Quit to Desktop** is in the pause menu, in Options and on the title screen, and the window's close button asks the
same question.

Contact sheets: `notes/pc-menu2/contact_sheet.png` (pc-menu2: 1 the old menu (before), 2-4 the game's own pause menu,
Data Access and title menu, 5 our Options in game at 1920 x 1080, 6 at 1024 x 768 (4:3), 7 the quit question over the
pause menu, 8 a detail of a 3840 x 2160 picture), `notes/pc-menu/contact_sheet.png` (pc-polish: the old look, every page
and dialog from the unit test), `notes/pc-menu/ingame_sheet.png` (pc-polish headless game runs).

## pc-menu2 (2026-10-24): the game's menus first, a Sly-looking Options menu, drawn at the display's refresh

The owner: "the menu is a little bit choppy, mouse movement is weird ... integrate this new menu to match the theme of the
game ... you pause the game and you have the game's pause menu ... and you press on settings where you have an actual
Sly-looking menu". Checkpoint history and every measurement: notes/wip/pc-menu2.md.

**Flow** (runtime/menu_game.c `request_open`): Esc and the PS button go to the game's own menus, never straight to ours:
- playing: the game's pause menu, exactly as START (the `pcmenu_start` patch answers START where the game asks for it);
- on the pause list: START (Keep Playing, as START does there); on a page below it (Data Access, a yes / no question):
  Triangle (back, as the game's own prompts say). The keys are pressed for the game as pad buttons for 2 of its pad reads
  (`pcmenu_pad`, called by window.c `window_get_input`);
- the title screen: its menu (New Game... / Load Game... / Options / Quit to Desktop), opened exactly as SELECT opens it -
  the patch `pcmenu_title` (f_001DD920 before its START check at 0x1DD964) records that the title is up and calls the
  game's f_001B04E0(0) when Esc asked for it; on that menu Esc is Triangle (back);
- where the game has no menu to show (movies, loading) the Options menu opens directly after 0.6 s, so the settings and
  Quit to Desktop are always reachable;
- the pause / title menu's **Options** opens ours; Back / Esc / Circle on a main page closes it - back to the game's menu;
  Esc there again resumes the game. Other game screens (Job Help) take Esc as START, as the game takes START there.
Verified in headless runs (build\warp\flow, title, jobhelp, after): Esc -> pause, down x5 + Cross -> Options, Back -> the
pause menu with Options highlighted, Esc -> playing; title Esc -> its menu, Options -> ours, Back -> its menu, Esc ->
the title; on the Data Access question Esc -> back to the slots.

**How it is drawn** (runtime/sly_font.c, menu_draw.c, menu_overlay.c). Options considered: (a) draw inside the game's 2D
system (its scenes, text objects, render priority 21) through hooks; (b) a runtime overlay with the game's own font. (b) is
the one built, because the game's 2D is drawn only with the game's own frames (55-60 per second, through the frame
queue and the internal scale) - the menu could never follow the mouse at the display's refresh nor be sharper than the
internal scale - and every new widget would need scene data the game does not have; (b) keeps the menu independent of
the game's state (title, loading, movies) and of the renderer.
- **The game's font from the disc, at run time.** Sly 2's font is a vector font: one object in the level data (EE RAM, e.g.
  0x0076CAC0 in the ep2 hub): +0x20 count (224: Windows-1252 0x20..0xFF), +0x24 the sorted character table, +0x14 the
  glyphs (0x40 bytes; table entry i's glyph at +0x40 * (i + 1); f_00216B40), +0x1C the advances (f_00216B80). A glyph's
  +0x1C is a DMA chain (CNT .. RET) of VIF codes for the 2D VU1 program (vuc_188055835, MSCAL 40), which f_00217058 CALLs:
  STROW 0; V2-32 @7 the segment count; STCYCL 2,1; V3-8 @8 per segment (kind, vertices, flag); V4-8 @9 colours (0,0,0,0x80;
  alpha 0 for flagged ones); STCYCL 1,1; STMASK 0x50505050; V2-16 x, y per vertex (1/16 font unit, y up negative, the
  baseline at 0); optionally STMASK 0x3F3F3F3F + V1-8 at the same address (w != 0: ADC, no triangle). The VU program draws
  kind 1 as a triangle fan, kind 2 as a strip (GIF tags at VU 14..19, EE 0x4421A0), and the GS fills them stencil-then-cover:
  each triangle adds +-2 to the green channel (FBMSK FFFF00FF, ALPHA (0 - Cs) * FIX + Cd), then sprites read the buffer
  through a CLUT (nonzero winding). sly_font.c does the same on the CPU: fan triangles count with their orientation
  (normalised so a segment adds, a flagged one - a counter - takes away), strip triangles +1 / -1; 4 x 4 samples per pixel
  -> a 2048 x 1574 R8 coverage atlas at 2.5 pixels per font unit with 8 mip levels (~100-160 ms on a worker thread). Found
  by scanning EE RAM for the object's shape once a second until it is there (1.5 ms per scan; the title has it); a damaged
  or unknown packet is refused (the menu keeps a Windows stand-in, Segoe UI, until then). Nothing of the disc is stored in
  the repository, the executable or on disk. Every glyph is correct, holes included (scratch renders of the whole font).
- **The look**: the game's pause menu draws flat-coloured words of this font - the highlighted entry warm orange, the
  others slate blue - over the darkened game; its Data Access pages use night-blue bands with an orange header. Ours: the
  game picture darkened, a night-blue panel with a thin blue border, the title "OPTIONS" in the game's lettering in red
  with a heavy black outline and drop shadow (as its comic headers), tabs and rows in the font (selected orange, others
  slate / pale blue, disabled dim), a selection band in translucent orange with an orange marker that glides between rows,
  arrows and sliders in the highlight orange (white knob), descriptions in a darker box, prompts with PlayStation / Xbox
  symbols (drawn shapes: the font has no button glyphs) or key caps. Everything is sized in units of the output's height
  (1 unit = 1 px at 1080 rows; narrower outputs shrink it): crisp from 720p to 4K at 16:9 and 4:3 (contact sheet 6, 8).
- **Drawing**: menu_draw.c builds a list of primitives (rounded boxes with gradient and border, capsules, arrows, glyph
  quads; a scissor rectangle for the scrolling list) for the output size; the Direct3D 11 presenters draw it in one batch
  with a small shader (signed distances with a one-pixel ramp, the atlas sampled trilinearly, the title's outline by
  dilation) at every present: no CPU picture, no upload but ~600 vertices. GDI and the unit tests rasterise the same list
  on the CPU (menu_draw / ui_raster).

**Responsiveness: why it was choppy, what changed** (measured on the owner's PC, release, headless, 2560 x 1440)
- Root cause: every change of the picture (a hover, a selection, a value) redrew the whole menu on the CPU - signed
  distance shapes over the full output and GDI text - inside the render thread's present: **78 ms at 1080p, 137 ms at 1440p,
  417 ms at 4K per redraw** (old menu_draw.c, -O2), plus a 14.7 MB texture upload. Moving the mouse over the rows meant a
  redraw for every row crossed: the menu updated ~7 times a second at 1440p and the paused game's pictures stalled behind
  it. On top, the menu was only drawn with the game's pictures (55 per second on the 165 Hz monitor), the window thread
  read the controllers / scripted events every 15 ms, the wheel moved the selection (the list jumped under the pointer),
  and the pointer appeared only after the mouse moved (fullscreen).
- Now: a picture costs 0.085-0.09 ms of CPU (PS2_MENU_STATS); while the menu is open the GPU renderer's render thread
  presents the last game picture + the menu whenever the swap chain has room (gpu_present.c `gpu_present_menu`, polled
  every 1 ms when idle: once per display refresh with frame latency 1), between the game's own pictures; the frame
  statistics ignore that time (no VRR / pacing verdicts from menu presents). The software renderer's window presenter does
  the same (window.c). Mouse and keys still wake the window thread at once; the hover uses the layout as shown (the
  animated scroll); the wheel scrolls the list (3 rows a notch, fractions of a notch add up); sliders and the scroll bar
  follow the mouse while the left button is held (SetCapture); the list scrolls and the selection band glides with a
  60 ms time constant; the pointer shows / hides at once when the menu opens / closes.
- Measured: input -> the picture that shows it: **before** 13-22 ms + 78-417 ms of redraw (one picture per game frame);
  **after**, game cadence only (a 60 Hz display): 60 pictures/s, avg 13-19 ms, max 22 ms (the game's frame queue); with
  menu presents at a (headless-simulated, PS2_MENU_SIM_HZ=165) 165 Hz display: 224 pictures/s (165 menu + 60 game), avg
  2.3-3.3 ms, max 6.4 ms - one refresh.
- Tools: `PS2_MENU_STATS=1` (every 2 s: pictures/s, input -> picture avg / max, CPU per picture), `PS2_MENU_SIM_HZ=<Hz>`
  (headless: menu pictures into the offscreen target at that rate), `PS2_MENU_FONT=system` (the stand-in font).

**Not done**: the game's menu sounds (move / select / back) are played by its scenes' timelines (f_002121E8 checks scene
labels such as 0x314, 0x331, 0x332), not by a call that could be made from outside without its scene objects; the menu is
silent. Button prompts are drawn shapes (the font has no PlayStation symbols; the game's prompt icons are scene art).

## One menu (pc-polish, 2026-10-23): how the game's menus and ours fit together

The owner: "there is no way to quit the game, also what's the use of the old pause menu being there?". The game's pause
menu keeps what only the game can do (its save system, job help, episode menu); everything that is a *setting* lives in
one place, our Options; nothing is duplicated and no PS2-only item is left.

**Controls...** (pc-small): the pause menu's entry (text 0x43, callback 0x1B0740 = the game's controller diagram
pages "Controller: Sly" ...) is copied without callbacks by `pcmenu_pause_list`; `pcmenu_select` opens our Options on
the Controls page (`menu_open_page`), Back returns to the pause menu (notes/pc-menu2/controls_before_after.jpg). The
title menu has no Controls entry. The game's diagram (which button does what per character) is no longer reachable.

**Pause menu** ("WE'LL BE RIGHT BACK!", the game's own wheel, its own font):
Keep Playing / Save and Quit to Title / [Exit to Episode Menu] / [Abandon Job] / **Save Game...** / **Load Game...** /
**Options** / [Controls...] / [Job Help] / **Quit to Desktop** ([..]: shown where the game shows them).
- The game's own "Options" entry (text 0x2C) is gone. Its page held Save Game... / Load Game... (now directly in the
  pause menu: the game's own items, copied from its Options table 0x2BFEC8, same callbacks and memory card pages), PS2
  hardware items (USB Headset, Center Screen...: hidden since pc-text) and options that are now in our menu (below).
- "Save and Quit" (the game saves and goes back to the title screen) reads **"Save and Quit to Title"** next to
  "Quit to Desktop" (text id 0x47 answered by the text lookup patch: the bank's text cannot grow in place).
- **Options** opens our menu; **Quit to Desktop** asks "Quit to desktop? Unsaved progress since the last save will be
  lost." (Quit / Cancel; Cancel returns to the pause menu).

**Title screen** (SELECT: the game's Options page, top level): New Game... / Load Game... / **Options** / **Quit to
Desktop**. Its Speakers / volume / inversion / Vibrations items are hidden (in our menu).

**The game's options in our menu, two-way** (menu_game.c): Music / Effects Volume (Audio, as before), **Speakers**
(Audio: Stereo / Mono / Surround - the game's own setting, default Stereo on PC; the disc's default is Surround, a Pro
Logic II matrix), **Vibration** (Controls: our Vibration row is both the controller's and the game's Vibrations),
**Invert Binocucom aiming**, **Invert turret aiming** (Controls; [game] invert_binocucom / invert_turret). They are read
with the game's own getter f_001AF9C8(item) (it writes the value's text id into the item) and set with its setter
f_001AFB88(menu object 0x3D9ED0, item) - the same code its Options page ran - twice a second and at once after a change;
a loaded save brings its own values and ours are put back (as for the volumes). Value text ids: Vibrations On 0x48 / Off
0x49, Speakers Mono 0x4C / Stereo 0x4D / Surround 0x4E, Invert Binocucom Off 0x51 / On 0x52, Turret Off 0x53 / On 0x54.
Our camera inversion rows (right stick) stay: the game's two inversions are different (aiming modes).

## Quit to desktop

- Asked from: the pause menu's entry, the title menu's entry, "Quit to desktop" at the end of every Options page, the
  window's close button and Alt+F4 (WM_CLOSE). With the menu closed the question opens alone over the game (no panel;
  the game is paused behind it where it can be, as for Esc); from Options it opens over the panel. A second close while
  the question shows quits at once (a game that stopped answering can always be closed). Logging off / shutting down
  Windows (WM_ENDSESSION) quits without asking.
- Quit (menu_game.c `menu_game_quit`, own thread): waits for the game's card worker (0x2F633C +4 == -1, at most 5 s: a
  save in progress finishes), then the window thread's `close_window`: the window's placement and display mode saved
  ([display] mode, [window] x/y/...; every setting is already in sly2.ini, written when it changed), the controllers'
  motors stopped and their light bar and player LEDs turned dark (input_hid.c, at the HID thread's stop), the desktop's
  display mode restored, `launch_exit(0)` (mc_quiesce: the memory card files flushed), exit code 0. Headless: the same
  without the window.
- Tests: headless runs `mquit` (the close button token) twice and the pause entry + `mok`: exit code 0, `[menu] quit to
  desktop`, `[launch] exit (0)`; a save started a few frames before the quit is on disk.

## How it is reached

- **The game's pause menu**: Options / Quit to Desktop as above. Instruction patches (recomp/patches.py, SHA-1 guarded;
  runtime/menu_game.c):
  - `pcmenu_list` (f_001B0290 before its `jal f_001AF4D8` at 0x1B0490): the list the game filtered into 0x2F65D0 is
    copied to unused EE RAM 0xFF000: the 0x2C entry replaced by Save Game... (when f_1D9880(0x3DA530) = 0, the same test
    the Options page uses), Load Game... and our "Options" (text id 0x10000, no callbacks, flags 2 = stay open); "Quit to
    Desktop" (0x10001) last. `$a1`/`$a2` get the copy.
  - `pcmenu_options_list` (f_001B04E0 before 0x1B05E4): Options and Quit to Desktop appended to the Options page
    (`$s1` = the end of its list at 0x2F6470).
  - `pc_options_hide` (f_001B04E0 at 0x1B058C): also hides Speakers 0x4C, Music / Effects Volume 0x67 / 0x68, Invert
    Binocucom 0x51, Invert Turret 0x53, Vibrations 0x48 (`pcmenu_option_hidden`).
  - `pcmenu_text` (f_0013E368 entry, text id -> string): 0x10000 "Options" (EE 0xFF200), 0x10001 "Quit to Desktop"
    (0xFF240), 0x47 "Save and Quit to Title" (0xFF280, with the PC wording on).
  - `pcmenu_select` (f_001AEEE0 before 0x1AF154, Cross, `$s0` = the entry): 0x10000 opens the menu, 0x10001 the quit
    question; the game's pause menu stays open, idle, underneath.
- **Esc** (keyboard) and the **PS button**: (pc-menu2) the game's own pause / title menu (section "pc-menu2" above); our
  menu opens directly only where the game has no menu to show (after 0.6 s).
- **Closing** (Esc / Circle / B on a main page, Options / PS button anywhere) returns to the game's pause menu; with
  staged changes it asks first ("Apply the changed settings?" Apply / Discard / Cancel).
- Pause signal: page depth `*(s32*)0x3D9EF4 >= 1` and page 1's entry list `*(u32*)0x3D9EFC == 0xFF000`, state
  `*(s32*)0x3DA0B8 == 7` = idle.

## Resolution, made understandable

Before: "Internal resolution" (Auto / 1x..8x / Native) and "Auto: highest resolution" on the Graphics page and "Window
size" on the Display page - the owner: "Internal resolution and actual resolution are confusing". Now, as modern games:

**Display page** - the picture on the screen:
- **Display mode**: Windowed / Borderless (fills the monitor at the desktop's resolution) / Fullscreen (the game sets
  the monitor to the resolution below; the window stays a flip-model popup - exclusive's latency - and the desktop's
  mode comes back when the game goes to the background or ends). [display] mode (old [window] fullscreen = off / on /
  exclusive is read and migrated).
- **Resolution**: windowed = the window's size (client area; a maximized window is restored first); borderless = the
  desktop's, shown read-only; fullscreen = the monitor's modes ("Desktop (2560 x 1440)", then every mode, largest first;
  [display] resolution = desktop | WxH; display_mode.c switches with CDS_FULLSCREEN at the refresh rate
  `display_mode_rate_at` picks: fullscreen_refresh's choice, else the desktop's rate if that mode has it, else the
  fastest not above it).
- Monitor, Aspect ratio, HUD layout, Frame rate, Frame pacing, VSync, Variable refresh (status), Show frame rate.

**Graphics page** - how sharp the game is drawn: **Render scale**: Auto (up to 100%) / Original PS2 (512 x 448) / 50% /
67% / 75% / 100% / 125% / 150% / 200%, each with the size it means on this output ("100% (2560 x 1440)"); the
description says what is really drawn: "Rendering at 2560 x 1792 -> shown at 2560 x 1440". The renderer draws integer
multiples N of the PS2's 512 x 448 (display targets N x 2N samples); a percentage P becomes the smallest N whose 448 N rows
reach P % of the output's rows, with 10 % slack (`settings_scale_for`): N = ceil(P/100 * rows / 448 - 0.1), 1..8.

| rows \ P | 50 | 67 | 75 | 100 | 125 | 150 | 200 |
|---|---|---|---|---|---|---|---|
| 720 | 1 | 1 | 2 | 2 | 2 | 3 | 4 |
| 1080 | 2 | 2 | 2 | 3 | 3 | 4 | 5 |
| 1440 | 2 | 3 | 3 | 4 | 4 | 5 | 7 |
| 2160 | 3 | 4 | 4 | 5 | 6 | 8 | 8 |

("Next higher" so 100 % is never softer than the screen - 1440p at 100 % draws 1792 rows, not 1344; the slack keeps
125 % of 1440 = 1800 rows at 4x = 1792 instead of jumping to 5x.) Auto = the governor's choice up to N(100 %) (3x at
1080p, 4x at 1440p: on the owner's RTX 4060 Ti it settles at 3x in the heaviest hubs, pc-pacing's measurements); every
scale is a maximum the governor may lower while the GPU cannot keep up. An exact multiple (F2, an old file's
internal_scale) shows as "3x (2048 x 1344)". [graphics] render_scale = auto | original | 25%..400% | 2x..8x (old
internal_scale / auto_max read and migrated). Headless runs (no window) assume 1080 output rows.

Presets (before r2-m8): **Original** = Original PS2, Faithful, PS2 outlines, 4:3; **Performance** = 50 % (2x at 1080p / 1440p);
**Balanced** (default) = Auto; **Quality** = 100 % + thin outlines; **Custom** = anything else.

(r2-m8, decision D6) In **Modern** rendering the render scale is a free slider 25 %..200 % (step 5 %) of the output's
resolution, drawn at exactly that size (modern frames, notes/modern-renderer.md "R8 as built"; no integer grid), and an
**MSAA** row follows it: Off / 2x / 4x / 8x (graphics.msaa; greyed out in Reference or at an exact Nx). Reference keeps
the integer grid choices above. Presets: **PS2** (= original) = Reference rendering, Auto (the integer grid up to
100 %), Faithful, PS2 outlines, 4:3; **Performance** = Modern 75 %, MSAA off; **Balanced** (default) = Modern 100 %,
MSAA 4x; **Quality** = Modern 100 %, MSAA 8x, thin outlines; **Custom** = anything else. A preset is applied live like
any option (Revert / Discard restore the previous values as before).

(light, owner 2026-11-05) A **Lighting** row (Enhanced / Original) follows MSAA: Balanced and Quality set Enhanced,
Performance and PS2 set Original (notes/enhanced-lighting.md).

## Apply / Revert (menu overhaul, 2026-11-06: Apply for everything)

The owner: "I want to have to apply the settings every time". (Before: everything live except display mode, resolution,
monitor and renderer.)
- **Every change is staged** (menu.c `sg`: options of the settings table as the player's own values, the controls
  configuration with the bindings, the window size). A changed row has an orange dot before its value; the footer
  shows "N changes not applied" and two buttons: **Apply** (Square / X on a controller, F on the keyboard, a click) and
  **Discard** (Triangle / Y, R, a click). Stepping back to the value in effect un-stages a row. The rows show the staged
  values (a preview: menu.c `preview`, PO = the player's options, PE = what would be in effect).
- **Apply** puts every staged change into effect at once (`apply_pending`: each through `settings_change`, written to
  sly2.ini, [chosen]; the look left first / entered last so the options Retro sets become the player's own; the renderer
  last; the controls with one `input_config_set`, a test rumble when the vibration strength changed). **Discard** forgets
  them. Changing **Quality** restages the rows it sets (Resolution scale, Anti-aliasing, Outlines; Lighting, Textures
  in Advanced) - applied with Apply like the rest. **Reset to defaults** stages the page's defaults (no question:
  Discard undoes it).
- **Leaving with changes** (Back / Esc / Circle on a main page, the PS / Options button, closing the pause): "Apply the
  changed settings? N changes are not applied yet." **Apply** / **Discard** / **Cancel** (left / right move the focus,
  Confirm presses the focused one, Square / Triangle / Circle - F / R / Esc - press them directly, the mouse clicks
  them). Apply then closes the menu (after "Keep these display settings?" when a display change was in it); Discard
  closes without them; Cancel stays. The menu opens with nothing staged.
- After a display change (display mode, resolution, window size, monitor) is applied: "Keep these display settings?
  Reverting to the previous settings in 15 s." Keep / Revert; no answer in 15 s (or Back / Esc / the PS button) restores
  the previous display mode, resolution, monitor and window size.
- Needs a restart: the renderer when the game started on the software renderer (the row says "Needs a restart of the
  game", afterwards "Restart the game to use it").
- Tests: tests/unit/menu `test_menu_nav` (staging, Apply, Discard, the leave question by keys / pad / mouse, Keep after
  leaving, Quality restaging, Retro, bindings and controls staged).

## Pages and options (menu overhaul, 2026-11-06)

Fewer, clearer options; no "PS2" in any player-facing text (the unit test checks the Graphics page). Sub-pages
(Advanced graphics, Advanced controls, Keyboard controls, Controller buttons) open from a row of their page; Back returns
to that row; the page's tab stays lit.

| page | option | values | key |
|---|---|---|---|
| Graphics | Look | Faithful / Retro (Reimagined: only shown when sly2.ini has it) | graphics.look |
| | Quality | Low / Medium / High / Ultra (Custom when the options match none; "Original", locked, under Retro) | graphics.preset |
| | Resolution scale | Auto (W x H) / 50 / 67 / 75 / 100 / 125 / 150 / 200 % (W x H); Reference: Auto (up to 100%), Original (512 x 448) and the same steps; a file's other value or Nx in its place | graphics.render_scale |
| | Anti-aliasing | Off / Low (4x MSAA) / High (8x MSAA) (2x only when a file has it; Modern at a percentage only) | graphics.msaa |
| | Outlines | Original / Thin | graphics.outlines |
| | Advanced graphics... | the sub-page | |
| Advanced graphics | Lighting, Textures | Original / Enhanced (Modern at a percentage only; a later "AI" textures value goes after Enhanced) | graphics.lighting, graphics.textures |
| | Renderer | GPU (Direct3D 11) / Software (CPU) - the fallback for other PCs | graphics.renderer |
| | Rendering | Modern / Reference (exact) | graphics.renderer_mode |
| | Scaling filter | Automatic / Lanczos / Catmull-Rom / Box / Bilinear / Nearest | graphics.scaler |
| | Motion blur | On (the game's own) / Off | graphics.motion_blur |
| | TV effects | Off / On (flicker filter + interlacing; "Custom" when a file overrides one of them) | graphics.filters (+ flicker_filter / interlacing back to preset) |
| | HUD layout | At the screen's edges / Centered (widescreen only) | display.hud |
| Display | Display mode | Windowed / Borderless / Fullscreen | display.mode (+ Keep) |
| | Resolution | window sizes / desktop (read-only) / the monitor's modes | [window] size, display.resolution (+ Keep) |
| | Monitor | only with 2+ monitors: Automatic / 1: 2560 x 1440, 165 Hz (main) / ... | display.monitor (+ Keep) |
| | Aspect ratio | 4:3 (original) / 16:10 / 16:9 / 21:9 / 32:9 / the display's | display.aspect |
| | Frame rate limit | 60 fps (original) / 120 / 144 / 165 / 200 / Display / Unlimited | display.frame_rate |
| | VSync | On / Off / Adaptive | graphics.vsync |
| | Show frame rate | Off / On | window.show_fps |
| Audio | Master, Music, Effects volume, Speakers (Stereo / Mono / Surround), Sound in the background (unchanged) | | audio.* |
| Controls | Invert camera | Off / Vertical / Horizontal / Both (the X and Y inversions in one row) | [controls] invert_camera_x / _y |
| | Vibration | Off / On (also the game's Vibrations) | [controls] vibration |
| | Keyboard controls..., Controller buttons... | the binding pages (staged too) | [controls] |
| | Advanced controls... | Vibration strength, Invert Binocucom aiming, Invert turret aiming, Left / Right stick dead zone, Light bar | [controls], game.invert_* |
| every main page | Reset to defaults (staged; Display keeps display mode / resolution / monitor), Quit to desktop (asks) | | |

Frame pacing and the variable refresh status rows are gone: pacing is automatic (`graphics.pacing = auto`, the default:
paced on G-Sync / FreeSync, immediate on a fixed refresh, the display's clock on 60 / 120 / 240 Hz); the ini keys
(`pacing`, `fullscreen_refresh`) still work and Reset on Display stages pacing = auto. HUD layout moved to Advanced
graphics.

**Look** (owner decision 2026-11-06): **Faithful** (default) = the game's own look drawn by the modern renderer at the
screen's resolution, with the Quality's options (Enhanced lighting and textures from Medium up); **Retro** = everything as
the original console drew it (Reference rendering on the integer grid, render scale auto, TV effects, original outlines,
lighting and textures, 4:3) - the rows it sets are greyed with those values ("Set by the Retro look"), Quality shows
"Original"; choosing Faithful again brings the player's Quality and options back (they stay in sly2.ini all along);
**Reimagined** (planned: stronger lighting / colour grading, AI textures) has its plumbing (graphics.look, the renderer's
query `settings_look()`) and renders as Faithful; the menu offers it only when sly2.ini already says `look = reimagined`.
Quality works with Faithful and Reimagined and is locked under Retro.

**Quality** presets (notes/settings.md "Presets"): Low = 75 %, MSAA off, original lighting / textures; Medium (default) =
100 %, 4x, enhanced; High = 100 %, 8x, thin outlines, enhanced; Ultra = 125 %, 8x, thin outlines, enhanced. Ultra measured
on the owner's RTX 4060 Ti at 2560 x 1440 (ep1 hub, release, headless with PS2_GS_PRESENT_SIZE, PS2_GS_HW_STATS medians):

| Quality | GPU ms per frame (median) | 60 fps held |
|---|---|---|
| Medium | 3.8 | yes |
| High | 5.4 | yes |
| Ultra 125 % | 5.3 | yes (video memory ~3.1 GB) |
| (150 % + 8x) | 10-16, then 270-340 (video memory exhausted on the 8 GB card) | no |
| (133 % + 8x) | 15-24 after 300+ ms stalls | no |

So Ultra stays at 125 % (another agent's GPU work ran at the same time; the 150 % / 133 % runs are the reason not to go
higher on 8 GB cards, not exact numbers).

"Voices": not offered (the game has no voice volume of its own: its Effects slider sets sound manager categories 0 and 3).

## How it is drawn

(pc-menu2: see "How it is drawn" in the pc-menu2 section; the old CPU / GDI drawing is gone.) The title is "OPTIONS".
Dialogs (`draw_dialog`): a title, an optional line, two buttons
with the device's hints (Enter / Esc, Cross / Circle, A / B); with `dialog_only` (Quit from the game's menus or the close
button) only the dimmed game and the dialog are drawn.

## Code

| file | what |
|---|---|
| runtime/settings.c/.h | the option table (render_scale, display.mode / resolution, game options, SET_STAGED, SET_LEGACY + migration), presets, `settings_scale_for` |
| runtime/menu.c/.h | the model: pages, rows, staging, Apply / Keep / Revert, dialogs (reset, quit, keep, pending), navigation, mouse, layout |
| runtime/menu_draw.c/.h | the picture (CPU) |
| runtime/menu_overlay.c/.h | on the screen: D3D11 pass, GDI, the frame rate layer, screenshots |
| runtime/menu_game.c/.h | the game: pause list / Options page patches, texts, game options two-way, Esc / PS -> pause -> menu, volumes, quit, the menu's host |
| runtime/window.c | close button -> quit question, WM_APP_QUIT, display modes list, Fullscreen resolution, WM_DISPLAYCHANGE |
| runtime/display_mode.c | Fullscreen's display mode (resolution + refresh) |
| recomp/patches.py | pcmenu_list, pcmenu_options_list, pc_options_hide, pcmenu_text, pcmenu_select, pcmenu_start, pcmenu_frame |

## Testing

- (menu overhaul) `tests\unit\menu\run.ps1` 450+ checks: also Look / Quality / Ultra, Retro and back, old preset names and
  the owner's own file migrated, staging / Apply / Discard / the leave question / Keep after leaving, the sub-pages.
  `-Shots` also writes `pc_menu_changes_1440p` (staged changes: dots + footer), `pc_menu_leave_1440p` (+ `_keys`: the
  leave question), `pc_menu_retro_1440p` (Retro staged: locked rows) and the Advanced pages. With PS2_MENU_FONT_RAM
  (an EE RAM dump of the title screen, `PS2_DUMP_RAM=<vblank>` in a headless run; never committed) the pictures use
  the game's font.
- `tests\unit\menu\run.ps1` (305 checks, no window): option table, render scale values and the N(P, rows) mapping,
  legacy keys (the new key wins; files migrated on load; old keys through settings_change), presets, persistence, the
  menu (render scale texts and "Rendering at ... -> shown at ...", staged renderer / mode / resolution, Fullscreen's
  modes, Apply -> Keep with a fake clock: countdown, revert after 15 s, Keep, Revert; close with staged changes -> Apply /
  Discard; quit from the game's menus / the close button (dialog only), from the Quit row, Cancel, mouse on the dialog's
  buttons), the drawing. `-Shots` writes every page and dialog as PNG and the contact sheet. `tests\unit\window`:
  `display_mode_rate_at`, F2 on render_scale, the template.
- In the game, headless with scripted input (PS2_INPUT tokens `menu`, `mup mdown mleft mright mok mback mtabn mtabp`,
  `menushot`, `mquit` = the close button; `PS2_GS_PRESENT_SIZE=WxH`): build\polish_run\run.ps1 (not committed; a copy of
  a memory card with a save): pause list, Save Game... (save1 rewritten), Load Game..., Job Help, Exit to Episode Menu,
  Save and Quit to Title, title SELECT menu -> Options / New Game..., Options pages, Apply -> Keep, the game options
  (log: `[menu] game option invert binocucom: text 0x51 -> 0x52`), quit (exit code 0).
- Not testable here (headless only, no visible window): a real display mode change for Fullscreen at another
  resolution and the close button of a visible window; the code paths are the ones above (display_mode.c,
  window.c WM_CLOSE) and the pure parts are unit-tested.

- **Screenshot key (pc-shot)**: **F12** (a window key, not bindable; works with the Options menu open) saves the presented back buffer - full output resolution, HUD, menu and counter included, so Modern / MFT at 1440p too - as `%USERPROFILE%\Pictures\Sly 2\Sly2_YYYY-MM-DD_HH-MM-SS.png` (`_2` ... on a name clash). menu_overlay.c: a GPU copy into a staging texture at the present, read back without waiting at a later present, PNG encoded and written by a worker thread (no hitch); `Screenshot saved` toast for 1.5 s (menu_draw.c `menu_set_toast`; D3D11 presenters only, the GDI fallback logs). PS2_INPUT token `screenshot`; `PS2_SCREENSHOT_DIR` names another folder. Test: `tests\unit\shot\run.ps1` (headless game, needs build\rel). Not rebindable (would need an ini key in input_map.c / a Controls row).
