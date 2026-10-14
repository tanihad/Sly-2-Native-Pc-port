# Settings (sly2.ini)

`sly2.ini` next to the executable, read once at start-up (runtime/settings.c, `settings_load`,
before the game runs). When the window opens and there is no file yet, window.c writes a commented
template with the defaults (`settings_save_if_missing`); headless runs (PS2_HEADLESS) never write
one. Delete the file to get the defaults back.

Format: INI. `key = value`, comments start with `;` or `#` (also after a value), `[section]`
headers; keys before the first header belong to `[display]`. Unknown keys and bad values are
reported on stderr (`[settings] <path>: line N: ...`) and ignored; the other keys still apply. A
UTF-8 BOM (Notepad) is accepted.

```ini
[display]
aspect = 16:9          ; 16:9 (default) or 4:3; any W:H (21:9, 16:10) or a ratio (1.7778)

[graphics]             ; for the Direct3D 11 renderer; the software renderer ignores these
internal_scale = 3     ; 1 (PS2 resolution) .. 8
outlines = ps2         ; ps2 (one PS2 pixel wide, exact) | thin (high-resolution lines)
filters = modern       ; modern | faithful (preset for the three toggles below)
motion_blur = preset   ; on | off | preset   (modern: on,  faithful: on)
flicker_filter = preset;                      (modern: off, faithful: on)
interlacing = preset   ;                      (modern: off, faithful: on)
```

| key | values | effect |
|---|---|---|
| display.aspect | W:H, W/H or a ratio between 1 and 4 | display aspect; 4:3 = exactly the PS2 picture (notes/widescreen.md) |
| graphics.* | see above | read and kept in `ps2_settings` for the GPU renderer (notes/d3d11-renderer.md section 13) |

Environment overrides (for testing; they win over the file): `PS2_ASPECT=16:9|4:3|<W>:<H>`.

Applying changes: the aspect is read at start-up (the camera's frustum is recomputed by the game
whenever its field of view changes, so a run-time switch would need an in-game options screen that
also re-runs it; not done yet).

For code: `ps2_settings` (settings.h) holds the values; `settings_motion_blur/_flicker_filter/
_interlacing` resolve the preset. New keys go into `apply()` and `settings_format()` in settings.c,
with a test in tests/unit/window/test_window.c (`test_settings`).
