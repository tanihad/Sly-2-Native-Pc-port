# Settings (sly2.ini)

`sly2.ini` in the user data folder (see below), read once at start-up (runtime/settings.c,
`settings_load`, before the game runs). When the window opens and there is no file yet, window.c
writes a commented template with the defaults (`settings_save_if_missing`); headless runs
(PS2_HEADLESS) never write one. Delete the file to get the defaults back (the game then asks for
the disc image again).

## Where: the user data folder

`settings_user_dir()` (runtime/settings.c) holds `sly2.ini`, the memory card (`memcard\slot0`) and,
for the GUI build, the log (`sly2.log`, the previous run's `sly2.previous.log`):

1. `PS2_USER_DIR=<folder>` if set (tests, tools);
2. the executable's folder for a **console** program - every development build
   (`scripts\build_game.py` without `--package`) and every test: data stays next to the exe exactly
   as before, wherever the exe is copied;
3. the executable's folder if a file `portable.txt` is next to it (portable install);
4. otherwise (the player's GUI build, `dist\Sly2\sly2.exe`) `%LOCALAPPDATA%\Sly2`.

The memory card folder can still be overridden with `PS2_MEMCARD_DIR` (tests, scripts\warp.py).

## Format

INI. `key = value`, comments start with `;` or `#` (also after a value), `[section]` headers; keys
before the first header belong to `[display]`. A value in double quotes keeps `;` and `#` (paths).
Unknown keys and bad values are reported on stderr (`[settings] <path>: line N: ...`) and ignored;
the other keys still apply. A UTF-8 BOM (Notepad) is accepted; the file is UTF-8 (non-ASCII paths).

```ini
[display]
aspect = 16:9          ; 16:9 (default) or 4:3; any W:H (21:9, 16:10) or a ratio (1.7778)
hud = edges            ; edges (default): corner HUD widgets at the display's edges | centered

[graphics]
renderer = software    ; software (default) | gpu (the Direct3D 11 renderer; software if the GPU can't)
pacing = vsync         ; gpu renderer: vsync (default) | vrr | display
internal_scale = 3     ; 1 (PS2 resolution) .. 8   (the keys from here on: Direct3D 11 renderer only)
outlines = ps2         ; ps2 (one PS2 pixel wide, exact) | thin (high-resolution lines)
filters = modern       ; modern | faithful (preset for the three toggles below)
motion_blur = preset   ; on | off | preset   (modern: on,  faithful: on)
flicker_filter = preset;                      (modern: off, faithful: on)
interlacing = preset   ;                      (modern: off, faithful: on)

[game]
disc = D:\Games\Sly 2 - Band of Thieves (USA).iso   ; written by the game when you choose the image

[window]               ; written by the game when the window closes
fullscreen = off       ; on: start in borderless fullscreen (Alt+Enter / F11 switch)
show_fps = off         ; on: frame rate in the title bar
x = 418                ; the window's position and size (not maximized), physical pixels;
y = 204                ;   ignored if no longer on a monitor (the window is then centred)
width = 1723
height = 999
maximized = off
```

| key | values | effect |
|---|---|---|
| display.aspect | W:H, W/H or a ratio between 1 and 4 | display aspect; 4:3 = exactly the PS2 picture (notes/widescreen.md). F7 switches wide <-> 4:3 while playing |
| display.hud | edges / centered (centred) | wide aspects only: the HUD widgets at the sides of the 4:3 screen (health meter, boss meters, counters) sit at the display's edges, or the whole HUD stays in the centre 4:3 area. F8 switches it while playing |
| graphics.renderer | software / gpu (also sw / hw / d3d11) | the GS renderer (notes/d3d11-renderer.md, milestone M6: opt-in). gpu needs a Direct3D 11.1 GPU with rasterizer-ordered views (NVIDIA Maxwell 2 and later class); without one the software renderer is used and the log says why |
| graphics.pacing | vsync / vrr / display | gpu renderer: vsync = Present(1) on the display's refresh (default); vrr = Present(0) with tearing allowed (G-Sync/FreeSync: every picture shown once, as soon as it is ready); display = on a 59.94/60 Hz display (within 0.5%, measured) the display's VBlanks drive the game instead of the 59.94 Hz timer (no repeated picture every 16 s); on other displays the timer stays (logged) |
| graphics.* (others) | see above | read and kept in `ps2_settings` for the GPU renderer (notes/d3d11-renderer.md section 13); M6 draws at scale 1 whatever internal_scale says (scaling is M7) |
| game.disc | path of a .iso | the disc image (runtime/launch.c; notes/install.md). Empty or unusable: asked for with a file dialog (GUI build) |
| window.fullscreen | on/off | start fullscreen; remembered on close |
| window.x/y/width/height/maximized | integers / on/off | the window's normal placement; remembered on close |
| window.show_fps | on/off | title "Sly 2: Band of Thieves - 59.9 fps" instead of "Sly 2: Band of Thieves" |

Environment overrides (for testing; they win over the file and are never written to it):
`PS2_ASPECT=16:9|4:3|<W>:<H>`, `PS2_HUD=edges|centered`, `PS2_SHOW_FPS=1`, `PS2_ISO=<image>` (the disc,
see notes/install.md), `PS2_GS_BACKEND=sw|hw` (renderer), `PS2_GS_PACING=vsync|vrr|display` and
`PS2_VBLANK=display` (pacing). GPU renderer diagnostics (runtime/gpu/gpu_hw.h): `PS2_GS_HW_STATS=1|2`,
`PS2_GS_VERIFY=N`, `PS2_GS_DEBUG_KEYS=1` (F9 switch GPU <-> software, Shift+F9 simulated device loss),
`PS2_GS_SAME_THREAD=1`, `PS2_GS_INFLIGHT=N`, `PS2_GPU_ADAPTER=warp`.

Writing: the game changes only the keys it remembers (`settings_set` -> `settings_update_text`, a
pure function): the key's line is rewritten in place (a comment after the value is kept), a missing
key is added at the end of its section, a missing section at the end of the file; every other line,
including the player's comments, stays as it was. The file is replaced atomically (written to
`sly2.ini.tmp`, then moved over it).

Applying changes: `aspect` and `hud` also change while the game runs. **F7** switches between the
wide aspect and 4:3 (back to the wide aspect the game had, 16:9 if it started at 4:3), **F8** between
`hud = edges` and `centered`; both are remembered in sly2.ini (not when the key comes from
PS2_ASPECT / PS2_HUD). An edit of `[display]` in sly2.ini while the game runs applies when the game's
window is activated again (`settings_read_display`). The switch takes effect at the top of the
game's next frame (aspect.c `aspect_apply`, called from the main loop by the `aspect_switch`
instruction patch, which also recomputes the camera's frustum with the game's 0x14DB70); scripted
runs can press them with the PS2_INPUT tokens `aspect` and `hud`. The other keys are read at start-up.

For code: `ps2_settings` (settings.h) holds the values; `settings_motion_blur/_flicker_filter/
_interlacing` resolve the preset. New keys go into `apply()` and `settings_format()` in settings.c,
with a test in tests/unit/window/test_window.c (`test_settings`).
