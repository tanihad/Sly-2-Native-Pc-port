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
renderer = gpu         ; gpu (default: the Direct3D 11 renderer; software if it can't start) | software
pacing = vsync         ; gpu renderer: vsync (default) | vrr | display
internal_scale = auto  ; auto (default: 3x, lowered while the GPU can't keep up) | 1 (PS2 resolution) .. 8
                       ;   (the keys from here on: Direct3D 11 renderer, except motion_blur)
outlines = ps2         ; ps2 (one PS2 pixel wide, exact) | thin (high-resolution lines)
filters = modern       ; modern | faithful (preset for the three toggles below)
motion_blur = preset   ; on | off | preset   (modern: on,  faithful: on)
flicker_filter = preset;                      (modern: off, faithful: on)
interlacing = preset   ;                      (modern: off, faithful: on)
scaler = auto          ; picture -> window: auto | lanczos | catmull-rom | box | bilinear | point

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
| display.aspect | W:H, W/H or a ratio between 1 and 4 | display aspect; 4:3 = exactly the PS2 picture (notes/widescreen.md). F7 switches wide <-> 4:3 while playing. gpu renderer at internal_scale >= 2: the displayed buffers get round(N * W) columns of samples per PS2 pixel (W = aspect / (4/3): 4 at 3x 16:9), so the 512-wide picture stretched to the wide display keeps the horizontal resolution of 4:3 at N (anamorphic; notes/d3d11-renderer.md appendix K) |
| display.hud | edges / centered (centred) | wide aspects only: the HUD widgets at the sides of the 4:3 screen (health meter, boss meters, counters) sit at the display's edges, or the whole HUD stays in the centre 4:3 area. F8 switches it while playing |
| graphics.renderer | gpu / software (also hw / d3d11 / sw) | the GS renderer. **gpu (default since M11)**: the Direct3D 11 renderer (notes/d3d11-renderer.md). It needs a Direct3D 11 device of feature level 11_0 or 11_1 with typed UAV loads (R8G8B8A8_UINT), double-precision shader arithmetic and dual-source blending; rasterizer-ordered views are used when the GPU has them, otherwise the ROV-tier draws run as plain UAV draws of disjoint primitives (same pictures, more GPU time; appendix M). At start-up every shader the game uses is created; if anything fails (no device, a missing capability, the swap chain, a shader) the game runs on the **software renderer** and the log has one line `[ps2] GPU renderer unavailable (<reason>): using the software renderer`. A device lost while playing is recreated (twice); the third loss switches to software. software: the PS2's renderer on the CPU, at the PS2's resolution (the [graphics] keys below except motion_blur do nothing). An existing sly2.ini written by an older build says `renderer = software` (the old default): change it to gpu or delete the file |
| graphics.pacing | vsync / vrr / display | gpu renderer: vsync = Present(1) on the display's refresh (default); vrr = Present(0) with tearing allowed (G-Sync/FreeSync: every picture shown once, as soon as it is ready); display = on a 59.94/60 Hz display (within 0.5%, measured) the display's VBlanks drive the game instead of the 59.94 Hz timer (no repeated picture every 16 s); on other displays the timer stays (logged) |
| graphics.internal_scale | auto (default) or 1..8 | gpu renderer: the internal resolution, N x the PS2's (512 x 448 -> 1536 x 1344 at 3x), shrunk or enlarged to the window by `scaler` (supersampling: the anti-aliasing). Every target is drawn with N x N samples per PS2 pixel (the displayed buffers N x 2N while progressive, 4 x 6 at 3x 16:9); sample 0 of every pixel is exactly the PS2's. **auto** (M11): 3x (the owner's choice), lowered while the GPU cannot keep up and raised again (never above 3, never below 1) - the renderer measures the GPU time of every picture; when the GPU is busy more than 85% of the time for 3 s in a row it goes to the highest lower scale predicted (time ~ N^2) to fit in 70%; when the next scale up is predicted at <= 60% for 15 s in a row it goes up one, but not to a scale it left for being too slow in the last 120 s; nothing is judged for 3 s after a change. Each change is logged (`[gpu] internal scale auto: the GPU was busy 92% of the time at 3x -> 2x`). On the RTX 4060 Ti 3x 16:9 keeps the GPU 50-65% busy: auto stays at 3x. F2 / Shift+F2 step up / down from the scale in use and make it fixed (written as a number), Ctrl+F2 sets auto again; the targets are rescaled at the next frame (nothing restarts). `PS2_GS_SCALE=N` overrides it (not written); `PS2_GS_AUTO_SLOW=<factor>[:<seconds>]` multiplies the measured GPU time (tests of a slower GPU) |
| graphics.outlines | ps2 / thin | gpu renderer, scale > 1: ps2 = the cel outlines one PS2 pixel wide, as the PS2 draws them (exact; thicker at high resolution); thin = each line drawn analytically at the high resolution (coverage from the distance to the line, width `PS2_GS_THIN_WIDTH` samples, default half the scale), so outlines stay fine at 3x and above (not exact). F3 switches |
| graphics.filters | modern / faithful | the preset for the three toggles below: modern = motion blur on, flicker filter off, progressive; faithful = everything as a PS2 on a TV. F4 switches the preset and puts the toggles back to `preset` |
| graphics.motion_blur | on / off / preset | the game's own motion blur (previous frame blended into the new one). off = the game's own switch for it: the flag at 0x2DEB24 the effect's function f_00160F98 tests first reads 1 (instruction patch `motion_blur_off`, recomp/patches.py; both renderers). F5 switches |
| graphics.flicker_filter | on / off / preset | gpu renderer: the PS2's TV flicker filter (both display circuits on one buffer, one line apart, mixed at ALP 128 - the game alternates it with one circuit every VBlank). off shows the circuit with the lower DBY alone (sharper; the alternation stops flickering). F6 switches |
| graphics.interlacing | on / off / preset | gpu renderer: on = field display as the PS2 sends it to the TV - the field buffers drawn at field resolution (N rows per PS2 row) and shown line-doubled, fields drawn with the half-line offset (movies, the intro) woven by gs.c's deinterlacer: at internal_scale 1 identical to the software renderer's picture; off = progressive: the field buffers drawn with 2N rows per PS2 row (the full 448-line picture at every scale), odd fields of half-line-offset content shown one frame line lower (no bob). Shift+F6 switches |
| graphics.scaler | auto / lanczos / catmull-rom / box / bilinear / point | gpu renderer: the filter from the picture to the window (separable, two passes). auto = Lanczos-2 when shrinking (the "sharp downscale" of 3x supersampling), Catmull-Rom when enlarging, point at 1:1. The picture's sample grid (sample 0 at the PS2 pixel) is taken out, so it is centred like the PS2's. `PS2_GS_SCALER` overrides it |
| game.disc | path of a .iso | the disc image (runtime/launch.c; notes/install.md). Empty or unusable: asked for with a file dialog (GUI build) |
| window.fullscreen | on/off | start fullscreen; remembered on close |
| window.x/y/width/height/maximized | integers / on/off | the window's normal placement; remembered on close |
| window.show_fps | on/off | title "Sly 2: Band of Thieves - 59.9 fps" instead of "Sly 2: Band of Thieves" |

Environment overrides (for testing; they win over the file and are never written to it):
`PS2_ASPECT=16:9|4:3|<W>:<H>`, `PS2_HUD=edges|centered`, `PS2_SHOW_FPS=1`, `PS2_ISO=<image>` (the disc,
see notes/install.md), `PS2_GS_BACKEND=sw|hw` (renderer), `PS2_GS_PACING=vsync|vrr|display` and
`PS2_VBLANK=display` (pacing). GPU renderer diagnostics (runtime/gpu/gpu_hw.h): `PS2_GS_HW_STATS=1|2`,
`PS2_GS_VERIFY=N`, `PS2_GS_DEBUG_KEYS=1` (F9 switch GPU <-> software, Shift+F9 simulated device loss),
`PS2_GS_SAME_THREAD=1`, `PS2_GS_INFLIGHT=N`, `PS2_GPU_ADAPTER=warp`. Start-up tests (M11): `PS2_GPU_FAIL=device|caps|
swapchain|shader` (that step fails: the software renderer runs), `PS2_GPU_FL=11_0` (a feature level 11_0 device),
`PS2_GPU_NO_ROV=1` (rasterizer-ordered views treated as missing: the no-ROV path), `PS2_GS_AUTO_SLOW=<factor>[:<s>]`
(internal_scale = auto sees the GPU time x factor). Graphics (M7): `PS2_GS_SCALE=1..8` (internal
scale), `PS2_GS_SCALER=auto|lanczos|catmull-rom|box|bilinear|point`, `PS2_GS_THIN_WIDTH=<samples>` (thin outlines).
Widescreen (M10): `PS2_GS_ASPECT=W:H` - the aspect the GPU renderer widens its displayed buffers for (default: the
game's display aspect, per picture; e.g. `4:3` keeps N x N samples at 16:9, the M7 behaviour).

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
runs can press them with the PS2_INPUT tokens `aspect` and `hud`.

The graphics keys change while the game runs too (runtime/gfx.c): **F2** / **Shift+F2** internal scale up /
down (1..8, round, from the scale in use; fixed from then on), **Ctrl+F2** internal_scale = auto, **F3** outlines ps2 <-> thin, **F4** filters preset modern <-> faithful (the three toggles back
to `preset`), **F5** motion blur, **F6** flicker filter, **Shift+F6** interlacing; each is written to sly2.ini
(internal_scale is ignored while PS2_GS_SCALE is set), and an edit of `[graphics]` while the game runs applies
when the window is activated again (`settings_read_graphics`). The GPU renderer takes a new scale, interlacing or
outline mode from its next frame (every target is rescaled to the new sample factors, sample 0 kept: no restart,
no lost frame; notes/d3d11-renderer.md appendix I) and the flicker filter and scaler from the next picture; motion
blur from the game's next frame. PS2_INPUT tokens: `scaleup`, `scaledown`, `scaleauto`, `outlines`, `filters`, `blur`,
`flicker`, `interlace` (not written to sly2.ini). The renderer and pacing are read at start-up.

For code: `ps2_settings` (settings.h) holds the values; `settings_motion_blur/_flicker_filter/
_interlacing` resolve the preset. New keys go into `apply()` and `settings_format()` in settings.c,
with a test in tests/unit/window/test_window.c (`test_settings`).
