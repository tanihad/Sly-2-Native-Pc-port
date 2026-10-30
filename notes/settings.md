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

Almost every key can also be changed in the game: **pause -> Options** (or Esc / the PS button), the Options
menu (notes/pc-menu.md); it writes the same keys.

**Launch defaults and their migration** (pc-menu2, 2026-10-24; the owner: "start fullscreen with the monitor's resolution and 60 fps ... don't switch the monitor's refresh rate ... this is meant for other people too"): the game starts **borderless** - the monitor's own resolution and refresh, no display mode change on any PC - at **60 fps** (frame_rate 60, pacing auto: a plain 59.94 cadence; the display lock that made 55 fps on a 165 Hz monitor is now only `pacing = display`, Options > Display > Frame pacing > "Match display", described as the smoothest choice without G-Sync / FreeSync). Exclusive fullscreen with another resolution / refresh stays an explicit choice (Display mode: Fullscreen; `fullscreen_refresh`). Files carry `[settings] version` (the defaults they were written with; the template writes 2). On start a file of an older version is moved to the new defaults where it still has the old ones - `[display] mode = windowed` (or no mode line) becomes borderless - unless the player chose that value: every change made in Options (and Alt+Enter / F11, and an applied display mode) is recorded in `[chosen]` (`display.mode = on`, ...), and listed values are never changed by a migration (`settings_migrate_defaults`, a pure function; tests/unit/menu `test_defaults_migration`). Old files predate `[chosen]`, so their windowed (the old default, also written on every close) is migrated once. The log says `[settings] ...: updated to the defaults of version 2 (display mode: borderless)`.

**Old keys** (pc-polish, 2026-10-23) are still read and the file is rewritten with their successors when the game
starts (`settings_migrate_text`: the successor written with the value the old key meant, the old line removed, every
other line kept; a successor already in the file wins): `[window] fullscreen` (off / on / exclusive) -> `[display] mode`
(windowed / borderless / fullscreen); `[graphics] internal_scale` (+ `auto_max`) -> `[graphics] render_scale` (auto ->
auto, native -> 100%, 1 -> original, N -> Nx; with a preset other than custom the preset's render scale). Code that still
calls `settings_change_key("window", "fullscreen")` / `("graphics", "internal_scale")` is redirected to the new keys.

```ini
[display]
mode = borderless      ; (pc-menu2 default) borderless (the monitor's own resolution and refresh) | windowed | fullscreen (`resolution`)
resolution = desktop   ; (pc-polish) Fullscreen's display mode: desktop (default) | WxH (one of the monitor's modes)
aspect = 16:9          ; 16:9 (default) or 4:3; any W:H (21:9, 16:10) or a ratio (1.7778)
hud = edges            ; edges (default): corner HUD widgets at the display's edges | centered
monitor = 0            ; 0 (default: where the window is) | 1, 2, ...: fullscreen (and the window) on that monitor
fullscreen_refresh = desktop ; desktop (default) | auto (a multiple of 59.94 Hz while fullscreen) | <Hz>
frame_rate = 60         ; (pc-fps) 60 (default: the PS2's 59.94) | 120 | 144 | 165 | 200 | <30..1000> | display | unlimited

[graphics]
preset = balanced      ; original | performance | balanced (default) | quality | custom (see "Presets")
renderer = gpu         ; gpu (default: the Direct3D 11 renderer; software if it can't start) | software
pacing = auto          ; gpu renderer: auto (default) | paced | immediate | display | vrr (old: paced + vsync off)
vsync = on             ; gpu renderer: on (default) | off (tearing allowed) | adaptive
render_scale = auto    ; (pc-polish) auto (default: up to 100%, lowered while the GPU can't keep up) | original (512x448) |
                       ;   25%..400% of the output's height | 2x..8x (the keys from here on: Direct3D 11 renderer, except motion_blur)
outlines = ps2         ; ps2 (one PS2 pixel wide, exact) | thin (high-resolution lines)
filters = modern       ; modern | faithful (preset for the three toggles below)
motion_blur = preset   ; on | off | preset   (modern: on,  faithful: on)
flicker_filter = preset;                      (modern: off, faithful: on)
interlacing = preset   ;                      (modern: off, faithful: on)
scaler = auto          ; picture -> window: auto | lanczos | catmull-rom | box | bilinear | point

[audio]
master = 100           ; 0..100: everything (the PC's mixer, audio_out.c)
music = 100            ; 0..100: the game's own music volume (as its Options page)
effects = 100          ; 0..100: the game's own effects volume (sound effects and voices)
background = on        ; off: silent while another window is active
speakers = stereo      ; (pc-polish) the game's own Speakers: stereo (default) | mono | surround (Pro Logic II matrix)

[game]
disc = D:\Games\Sly 2 - Band of Thieves (USA).iso   ; written by the game when you choose the image
invert_binocucom = off ; (pc-polish) the game's own "Invert Binocucom Controls"
invert_turret = off    ; (pc-polish) the game's own "Invert Turret Controls"

[window]               ; written by the game when the window closes
show_fps = off         ; on: frame rate in the title bar and in the picture's top left corner
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
| graphics.pacing | auto / paced / immediate / display / vrr (vsync = auto) | (pc-pacing, live) gpu renderer, how pictures are presented (runtime/gpu/gpu_present.c, gpu_pace.c): **paced** = picture v is presented at its VBlank's due time + D, D = the 95th percentile of how long pictures take to be ready (+1.5 ms), so pictures leave at an even 16.68 ms cadence - a VRR display (G-Sync / FreeSync) shows them exactly so; **immediate** = each picture as soon as it is drawn (lowest latency; uneven by the drawing time's variation); **display** = the display's VBlanks drive the game on a 60 / 120 / 240 Hz display (every k-th refresh is a PS2 VBlank; within 0.5%, measured), else the 59.94 Hz timer (logged); **auto** (default) = paced until the display's own record of the presents (DXGI frame statistics) shows what it does: variable refresh -> paced; a fixed refresh -> immediate (the picture waits for the next refresh anyway: pacing would only add latency), and on 60 / 120 / 240 Hz the display drives the game as with display. The log says what it found: `[gpu] pace: display 165.08 Hz, fixed refresh: ... judder ...` or `... variable refresh (G-Sync / FreeSync) is on`. vrr = the old setting (paced, vsync off) |
| graphics.vsync | on / off / adaptive | (pc-pacing, live) gpu renderer: on = Present(1) (no tearing; default); off = Present(0) with tearing allowed (DXGI_PRESENT_ALLOW_TEARING when the system supports it); adaptive = synced when the picture is on time, at once (tearing allowed) when it is more than 2 ms late |
| display.fullscreen_refresh | desktop / auto / Hz | (pc-pacing, runtime/display_mode.c) while the window is fullscreen and in front, the display is switched to this refresh rate at the desktop's resolution (a temporary mode, CDS_FULLSCREEN: Windows restores the desktop's when the game ends; the game restores it when it leaves fullscreen or goes to the background). auto = the highest multiple of 59.94 Hz the display offers (120 Hz on the owner's 165 Hz Dell) when the desktop's rate is not one: every picture then lasts exactly 2 refreshes and the display's VBlanks drive the game (no judder on displays without variable refresh). desktop (default) never changes the display; window.fullscreen = exclusive uses auto unless a rate is set |
| graphics.render_scale | auto (default) / original / 25%..400% / 2x..8x | (pc-polish, live, GPU renderer; `PS2_GS_SCALE` overrides) how sharp the game is drawn relative to the output (the window's client area / Fullscreen's display mode; 1080 rows when there is no window). It sets the internal scale N below: a percentage P = the smallest N with 448 N >= P% of the output's rows minus 10% of a step (`settings_scale_for`: ceil(P/100 * rows / 448 - 0.1), 1..8: 100% = 3x at 1080p, 4x at 1440p, 5x at 2160p; 50% = 2x at 1080p and 1440p); original = 1x; Nx = exactly N; auto = the governor below with its maximum at 100% (3x at 1080p, 4x at 1440p). A percentage and auto follow the window when it is resized (gfx_output_resized). Menu: Graphics > Render scale ("100% (2560 x 1440)", description "Rendering at 2560 x 1792 -> shown at 2560 x 1440"); notes/pc-menu.md has the table of N per output |
| display.mode | borderless (default since pc-menu2; windowed before) / windowed / fullscreen (also off / on / exclusive) | (pc-polish; was window.fullscreen) the display mode, remembered on close: borderless = a popup over the monitor at the desktop's resolution (Alt+Enter / F11 switch windowed <-> borderless); fullscreen = the same popup (DXGI flip model: independent flip, exclusive's latency) with the game owning the display's mode while in front: `resolution` and display.fullscreen_refresh (auto unless a rate is set). Menu: staged, Apply + "Keep these display settings?" (15 s) |
| display.resolution | desktop (default) / WxH | (pc-polish) Fullscreen's display mode (one of the monitor's modes; display_mode.c, CDS_FULLSCREEN: Windows restores the desktop's when the game ends, the game when it leaves fullscreen or goes to the background). The refresh rate: fullscreen_refresh's choice, else the desktop's if that mode has it, else the fastest not above it (`display_mode_rate_at`). A mode the display does not have: the desktop's resolution (logged) |
| audio.speakers | stereo (default) / mono / surround | (pc-polish) the game's own Speakers option (its Options page's item, now in Audio): set through the game's own setter (menu_game.c), again after a loaded save; the disc's default is surround (a Dolby Pro Logic II matrix mix) |
| game.invert_binocucom / game.invert_turret | on / off (default off) | (pc-polish) the game's own "Invert Binocucom Controls" / "Invert Turret Controls" (aiming up / down), set through its setter like speakers. The game's Vibrations option follows [controls] vibration |
| graphics.internal_scale | auto (default) or 1..8 | **old key** (pc-polish: read and migrated to render_scale). gpu renderer: the internal resolution, N x the PS2's (512 x 448 -> 1536 x 1344 at 3x), shrunk or enlarged to the window by `scaler` (supersampling: the anti-aliasing). Every target is drawn with N x N samples per PS2 pixel (the displayed buffers N x 2N while progressive, 4 x 6 at 3x 16:9); sample 0 of every pixel is exactly the PS2's. **auto** (M11): 3x (the owner's choice), lowered while the GPU cannot keep up and raised again (never above 3, never below 1) - the renderer measures the GPU time of every picture; when the GPU is busy more than 85% of the time for 3 s in a row it goes to the highest lower scale predicted (time ~ N^2) to fit in 70%; when the next scale up is predicted at <= 60% for 15 s in a row it goes up one, but not to a scale it left for being too slow in the last 120 s; nothing is judged for 3 s after a change. Each change is logged. (pc-pacing) The rules now (gpu_pace.c pace_gov): load = GPU ms per picture / 16.68 ms; auto goes down above 90% for 3 s (to the scale predicted at <= 80%) and up when the next scale is predicted at <= 75%; **every scale is a maximum**: a fixed scale (or native) the GPU cannot draw in real time is drawn lower for the time being (logged: `internal scale 7x: the GPU could not draw it in real time (145% of the frame time at 7x) -> drawing at 5x, the game keeps its speed`) and raised back when it fits - the game's speed never depends on the GPU (`PS2_GS_SCALE` and `PS2_GS_GOVERNOR=0` keep a scale exactly, for tests). When even 1x is more than the GPU can draw (above 120% for 10 s) the software renderer takes over (logged; `PS2_GS_SW_FALLBACK=0` keeps the GPU). At 1440p on the RTX 4060 Ti 3x takes 7-11 ms per picture (43-68%), 4x 13-19 ms (80-112%: drawn at 3x in the heaviest scenes), see notes/pc-experience.md. F2 / Shift+F2 step up / down from the scale in use and make it fixed (written as a number), Ctrl+F2 sets auto again; the targets are rescaled at the next frame (nothing restarts). `PS2_GS_SCALE=N` overrides it (not written); `PS2_GS_AUTO_SLOW=<factor>[:<seconds>]` multiplies the measured GPU time (tests of a slower GPU) |
| graphics.outlines | ps2 / thin | gpu renderer, scale > 1: ps2 = the cel outlines one PS2 pixel wide, as the PS2 draws them (exact; thicker at high resolution); thin = each line drawn analytically at the high resolution (coverage from the distance to the line, width `PS2_GS_THIN_WIDTH` samples, default half the scale), so outlines stay fine at 3x and above (not exact). F3 switches |
| graphics.filters | modern / faithful | the preset for the three toggles below: modern = motion blur on, flicker filter off, progressive; faithful = everything as a PS2 on a TV. F4 switches the preset and puts the toggles back to `preset` |
| graphics.motion_blur | on / off / preset | the game's own motion blur (previous frame blended into the new one). off = the game's own switch for it: the flag at 0x2DEB24 the effect's function f_00160F98 tests first reads 1 (instruction patch `motion_blur_off`, recomp/patches.py; both renderers). F5 switches |
| graphics.flicker_filter | on / off / preset | gpu renderer: the PS2's TV flicker filter (both display circuits on one buffer, one line apart, mixed at ALP 128 - the game alternates it with one circuit every VBlank). off shows the circuit with the lower DBY alone (sharper; the alternation stops flickering). F6 switches |
| graphics.interlacing | on / off / preset | gpu renderer: on = field display as the PS2 sends it to the TV - the field buffers drawn at field resolution (N rows per PS2 row) and shown line-doubled, fields drawn with the half-line offset (movies, the intro) woven by gs.c's deinterlacer: at internal_scale 1 identical to the software renderer's picture; off = progressive: the field buffers drawn with 2N rows per PS2 row (the full 448-line picture at every scale), odd fields of half-line-offset content shown one frame line lower (no bob). Shift+F6 switches |
| graphics.scaler | auto / lanczos / catmull-rom / box / bilinear / point | gpu renderer: the filter from the picture to the window (separable, two passes). auto = Lanczos-2 when shrinking (the "sharp downscale" of 3x supersampling), Catmull-Rom when enlarging, point at 1:1. The picture's sample grid (sample 0 at the PS2 pixel) is taken out, so it is centred like the PS2's. `PS2_GS_SCALER` overrides it |
| graphics.renderer_mode | reference / modern (also exact / original / off = reference; r2 / on = modern) | (r2-m1, live, gpu renderer; not in the template, not in a preset; `PS2_R2=off|hybrid|bracket` overrides it) reference (default) = the exact path draws everything, the GS stream is the game's own; modern = the modern renderer R2 (notes/modern-renderer.md) draws what it covers natively into the same targets: in milestone R1 the static world of the levels on its allowlist (`PS2_R2_LEVELS`, default the ep3 hub, widCurrent 12; `all`), the exact path everything else. (r2-m2) Milestone R2: every prelit mesh (static world + glob props; objects with cel outlines stay exact) in every level (`PS2_R2_LEVELS` = an allowlist of world ids, default all; `PS2_R2_BLOCK` = blocked world ids), meshes crossing the guard band stay exact (`PS2_R2_CLIP=1`: GPU-clipped, not within the A/B Z gate), `PS2_R2_KINDS=world,props`, `PS2_R2_CONSOLIDATE=1` (opt-in: one draw per entry, pass and state; changes foliage edges, see notes/modern-renderer.md "R2 as built"), `PS2_R2_DBG` (GPU cost experiments: 1 no texture, 2 rasterized Z, 4 float transform, 8 constant colour - never for play). Options > Graphics > **Rendering**: Modern / Reference (exact); **F10** switches while playing (persisted). Diagnostics: `PS2_R2_STATS=1` (a line every 300 VBlanks: native / exact entries per frame and why, capture ms, hint KB; slow capture / frontend / render-segment frames logged), `PS2_R2_BUDGET_MS` (mesh decoding per frame, default 3), `PS2_R2_ZBIAS` (debugging); (r2-m5) the stats line ends with a census (native share of the mesh vertices, exact by reason), `PS2_R2_VERIFY=1` (log stale mesh decodes), `PS2_R2_GCLEAN=0` (guard-band meshes with no crossing triangle stay exact), `PS2_R2_ALT=<vblank>` (hybrid and all-exact frames alternate from that VBlank on: scripts\r2_hcheck.ps1), `PS2_DUMP_TO=<vblank>` (no frame dumps after it); (r2-m7) the native GPU passes of modern frames (notes/modern-renderer.md "R7 as built": shadow volumes without rasterizer order, shadow darken and motion blur as compute passes) - `PS2_R2_FX=0` all off, `PS2_R2_NPOST=0` only the darken / blur ones; `PS2_GPU_PROF_OPS=<frame>` with `PS2_GPU_PROF=1`: GPU ms per op signature of that frame; (r2-m7b, notes/modern-renderer.md "R7 part 2 as built") `PS2_R2_TEXDEC=0` (decoded texture cache off; 3: only FF draws without a depth target), `PS2_R2_SHFUSE=0` (the shadow pipeline's passes unfused), `PS2_GPU_AB=<frames>` with `PS2_GPU_PROF=1` (in-run A/B: those features off in A phases and on in B phases, alternating every <frames> frames, `prof-ab` lines per phase), `PS2_GPU_PROF_SEQ=1` (the `PS2_GPU_PROF_OPS` frame's ops in order), `PS2_GPU_OPDUMP=<frame>` (every op of that frame logged) |
| graphics.preset | original / performance / balanced / quality / custom | (pc-menu) the graphics preset, see "Presets". A preset other than custom sets its options when the file is read (it wins over them: `preset = custom` to set them by hand); the game writes `custom` as soon as an option leaves the preset (menu or F-keys). A file without the key (older builds): derived from the options |
| graphics.internal_scale = native | native | (old key; render_scale 100% now) (pc-menu) the smallest scale N with 448 N rows >= the window's client height (4x at 1440p, 3x at 1080p, 5x at 2160p), followed when the window is resized (gfx.c gfx_output_resized) |
| graphics.auto_max | 1..8 (default 3) | **old key** (pc-polish: removed; auto's maximum is 100% of the output now) (pc-menu) the highest scale `internal_scale = auto` ran at |
| display.monitor | 0..16 (default 0) | (pc-menu) 0: the monitor the window is on; n: fullscreen on monitor n (EnumDisplayMonitors order), a window moves there |
| audio.master | 0..100 (default 100) | (pc-menu) the volume of the PC's mixer output (audio_out.c audio_set_gain, gain (v/100)^2; 100 = untouched, bit-exact) |
| audio.music / audio.effects | 0..100 (default 100) | (pc-menu) the game's own volumes (its Options page's Music / Effects Volume): put into its sound manager 0x3DD040 with its own function 0x1FEA60 (music: category 1, effects: categories 0 and 3) at the game's next frame; again when a loaded save resets them; changes on the game's own Options page are taken over into sly2.ini (menu_game.c) |
| audio.background | on / off (default on) | (pc-menu) off: the output is silent while the game's window is not the active one |
| game.disc | path of a .iso | the disc image (runtime/launch.c; notes/install.md). Empty or unusable: asked for with a file dialog (GUI build) |
| display.frame_rate | 60 / 120 / 144 / 165 / 200 / 30..1000 / display / unlimited (default 60) | (pc-fps, live; `PS2_FRAME_RATE`) how many game frames per second (notes/frame-rate.md): 60 = the PS2's 59.94 Hz VBlank timer, exactly as before; above, a frame limiter gives the game a VBlank as soon as it is ready for its next flip, at most that many per second, and the game clock's 1/60 s lower dt clamp is lowered (patch frame_dt_min) - the game runs at real-time speed at any rate. display = the display's measured refresh rate; unlimited = at most 500. Movies and RC chopper flights stay at 59.94. Menu: Display > Frame rate |
| window.fullscreen | off / on (borderless) / exclusive | **old key** (pc-polish: read and migrated to display.mode). start fullscreen; remembered on close. exclusive (pc-pacing: `pacing_exclusive_fullscreen`, runtime/pacing_menu.c): the window stays a popup over the monitor (DXGI flip model: independent flip, the latency of exclusive fullscreen) and the game owns the display's mode while it is in front: display.fullscreen_refresh = auto unless a rate is set (120 Hz on a 144 / 165 Hz display without variable refresh) |
| window.x/y/width/height/maximized | integers / on/off | the window's normal placement; remembered on close |
| window.show_fps | on/off | title "Sly 2: Band of Thieves - 59.9 fps" instead of "Sly 2: Band of Thieves", and (pc-menu) a counter in the top left corner of the picture (menu_overlay.c; also fullscreen) |

### Presets (graphics.preset; runtime/settings.c settings_apply_preset / settings_detect_preset)

(r2-m8, owner decision D6) The rendering mode is part of the presets. "PS2 mode" is the Original preset, shown as
**PS2** in the menu (`preset = original`, also `ps2`):

| preset | renderer_mode | render_scale | msaa | outlines | filters | motion_blur / flicker_filter / interlacing | scaler | aspect |
|---|---|---|---|---|---|---|---|---|
| **original** ("PS2") | reference | auto (integer grid, up to 100%) | (kept) | ps2 | faithful | preset (all on: as a PS2 on a TV) | auto | 4:3 |
| **performance** | modern | 75% | off | ps2 | modern | preset | auto | kept |
| **balanced** (default) | modern | 100% (the output's own pixels) | 4x | ps2 | modern | preset | auto | kept |
| **quality** | modern | 100% | 8x | thin | modern | preset | auto | kept |
| **custom** | anything else | | | | | | | |

(Before r2-m8: original = 1x, performance = 50%, balanced = auto, quality = 100% + thin, the renderer mode was not in a
preset. Before pc-polish: performance = auto up to 2x, balanced = auto up to 3x, quality = native.) Why Balanced = 100% +
4x: modern frames are drawn at the screen's own pixels (no resampling, sharper than the 3x grid shrunk to 1440p) and 4x
MSAA gives edges about as smooth as the 3x grid's supersampling for roughly the GPU time of the 3x grid
(notes/modern-renderer.md "R8 as built").

Choosing a modern preset in the menu while the aspect is the 4:3 that Original set returns to 16:9. Detection: a set
of options is the preset whose values it has (aspect only matters for Original, which needs 4:3; msaa does not matter
for Original); anything else is custom.

`graphics.msaa` = off | 2x | 4x (default) | 8x (r2-m8, live, gpu renderer): multisampling of modern frames. **Modern
frames** (R8) are drawn when renderer_mode = modern and render_scale is a percentage or auto (auto = 100% there): the 3D
at that share of the output's resolution with MSAA (alpha-to-coverage for cut-outs), the HUD and 2D after the resolve at
the output's resolution (gfx.c -> gpu_mft_config). An exact multiple (original, 2x..8x) or `PS2_GS_SCALE` keeps the
integer grid in modern mode too (the gates use that); menus, loading screens and other frames the planner can't take
stay on the grid frame by frame. Menu: Graphics > Render scale is a 25%..200% slider (step 5) in Modern; MSAA row
(Off / 2x / 4x / 8x), disabled in Reference. Environment (tests; win over the file): `PS2_MFT=0|1` (modern frames off /
on), `PS2_MFT_SCALE=<%>`, `PS2_MFT_MSAA=1|2|4|8`, `PS2_MFT_MV=1` (motion vectors), `PS2_MFT_MV_VIEW=1` (shown over the
picture), `PS2_MFT_LOG=<n>` (why frames stay on the grid), `PS2_MFT_NOROV=1` (frames with ROV-tier draws stay on the
grid), `PS2_R2_MV=0` (no previous matrices on the EE). With `PS2_R2_STATS=1` a line `mft: ... modern frames, exact
(reasons), ...` every 300 VBlanks.

`[controls]` (bindings, vibration, dead zones, camera inversion, light bar) belongs to runtime/input.c:
settings.c accepts its keys without parsing them, and input.c reads the section with
`settings_read_section` and writes it with `settings_set` (keys and values: notes/controls.md). It
is not in the template; the keys appear when the Options menu saves them, or can be added by hand.
Environment (hardware tests): `PS2_DS_BT_HEADER=sdl`, `PS2_DS_RUMBLE=classic|improved`.

Environment overrides (for testing; they win over the file and are never written to it):
`PS2_ASPECT=16:9|4:3|<W>:<H>`, `PS2_HUD=edges|centered`, `PS2_SHOW_FPS=1`, `PS2_ISO=<image>` (the disc,
see notes/install.md), `PS2_GS_BACKEND=sw|hw` (renderer), `PS2_GS_PACING=auto|paced|immediate|display|vrr`, `PS2_GS_VSYNC=on|off|adaptive` and
`PS2_VBLANK=display` (pacing). Pacing diagnostics (pc-pacing): `PS2_PACE_LOG=1` (one line per shown present: VBlank, planned, Present call, shown, interval; =2 also each picture's ready time; =3 also the display circuits of every VBlank), `PS2_PACE_HEADLESS=1` (pace the offscreen presents of a headless run), `PS2_GS_PRESENT_SIZE=WxH` (headless present target size), `PS2_GS_GOVERNOR=0` (a fixed scale exactly), `PS2_GS_SW_FALLBACK=0`. GPU renderer diagnostics (runtime/gpu/gpu_hw.h): `PS2_GS_HW_STATS=1|2`,
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

For code (pc-menu: the settings model): `ps2_settings` (settings.h) holds the values;
`settings_motion_blur/_flicker_filter/_interlacing` resolve the filter preset. Every key is one row of the typed
table `opts[]` in settings.c (`settings_opt`: section, key, type BOOL / ENUM / INT / TRI / ASPECT / STRING, range or
value names and accepted aliases, default, flags LIVE / PRESET / GPU / STATE, the subsystem that applies it
SET_APPLY_GFX / DISPLAY / WINDOW / AUDIO / RENDERER / PACING, the environment variable that overrides it). The
parser, the template, the Options menu and the hotkeys all use it: `settings_change(i, value, persist)` validates,
stores, writes the key (and `graphics.preset` when it changes; a preset writes all its options) and calls the
applier registered with `settings_on_apply` (gfx.c: GFX; window.c: DISPLAY, WINDOW; menu_game.c: AUDIO, RENDERER,
PACING, and DISPLAY when headless); `settings_serial` tells the menu to redraw. A new key: one row in `opts[]`, a line
in `settings_format()`'s template, a test in tests/unit/menu/test_menu.c (`test_table` checks every row's default and
text round trip). `[controls]` is input.c's (settings_read_section).

Live or at the next start: everything the menu changes applies at once (graphics from the GPU renderer's next frame,
aspect / HUD at the game's next frame, volumes at the game's next frame, display mode / size / monitor on the window
thread), except: `renderer` when the game started on the software renderer (a game started on the GPU renderer
switches both ways live). (pc-pacing) `pacing` and `vsync` are live (`pacing_apply_mode`); `window.fullscreen = exclusive`
switches the display's refresh as described above; `display.fullscreen_refresh` applies when the window next goes fullscreen.
(pc-polish) In the menu, the options flagged `SET_STAGED` in the table - display.mode, display.resolution, display.monitor,
graphics.renderer - are not changed until **Apply changes**; a display change then asks "Keep these display settings?"
and reverts after 15 s without an answer (notes/pc-menu.md "Apply / Revert"). `settings_reset_section` leaves them alone.
