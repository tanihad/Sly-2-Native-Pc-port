# Game art from the player's disc (and the owner's own pictures)

The game icon and the launcher's backdrop are made **on the player's PC**, from the player's own disc image, by the
build (like the game itself). Nothing of it is in the repository, a source bundle or Setup:
`scripts/check_no_game_data.py` proves that for every Setup / bundle the tests build (no picture, icon or media file
by name or content, none of the disc's known files by SHA-256), and `make_installer.py` refuses to package one.

## Where the pictures come from (first match wins)

| what | 1. the owner's own (`%LOCALAPPDATA%\Sly2\art\`) | 2. from the disc (`<install>\game\art\`) | 3. ours |
|---|---|---|---|
| game icon | `icon.ico` (as it is), `icon.png/webp/jpg/jpeg/bmp` (fitted), `logo.png/webp/...` (its raccoon mask on a badge) | `sly2.ico` (the save icon, below) | `scripts/make_icon.py` (thief's mask + moon) |
| launcher backdrop | `backdrop.png/jpg/jpeg/webp/bmp` (any size: cover, anchored right) | `backdrop.bmp` (the opening scene, below) | a drawn night sky with the icon as an emblem |
| launcher title | `logo.png/webp/...` (the whole logo) | - | "Sly 2" + PC + "Band of Thieves" in text |

- Pictures are decoded with Windows' own WIC: PNG, JPEG, BMP, GIF, TIFF everywhere; **WebP only if Windows has the
  WebP codec** ("WebP Image Extensions"; not installed on the owner's PC: his `logo.webp` was converted once to
  `logo.png` with the installed Edge, headless - `logo.png` is tried before `logo.webp`). A picture that cannot be
  read is skipped (the next source is used, one log line).
- The icon's choice (`installer/art.c art_icon_refresh`) writes `<install>\art\sly2.ico` + `sly2.ico.source` (which
  file, its size and time): at the end of every install / repair / update and at every launcher start; when it
  changed, the Start menu / desktop shortcuts and the Apps & features `DisplayIcon` are pointed at it again
  (`setup_refresh_icon`) and Explorer is told (`SHChangeNotify`). No source left: the file is removed and they show
  the launcher's own icon. `Uninstall.exe` removes `<install>\art`; the owner's folder is never touched.
- The logo's mask: the outline of the Cooper mask between "SLY" and "2" was traced on the official logo (2000 x 1187,
  20 points, `MASK_POLY` in art.c; mirror-checked against the picture) and is used for the same picture at any size
  (aspect within 2%). It is cut out with 4x4 samples per pixel and put on a round Sly-blue badge with a white ring
  and a thin dark edge (readable on light and dark taskbars), 84-94% of the badge at 16-48 px.
- `.ico` files we write: 16, 20, 24, 32, 40, 48, 64, 96, 128 px as 32-bit DIBs + 256 px as PNG (as Windows' own).

## The icon from the disc: the game's memory card icon

The game copies its save icon to the card as `BASCUS-97316YAOTWTD!/memcard_icon.ico` (notes/memcard-protocol.md).
It is **not** in SCUS_973.16; it is a raw file in the game's own data area of the disc (the game reads by sector
number, notes/install.md "Import"):

| disc | file | sector | bytes | SHA-256 |
|---|---|---|---|---|
| USA (SCUS-97316) | memcard_icon.ico | 1,169,578 (byte 2,395,295,744) | 25,688 | 708e8fd5...6bfa08c |

Found by searching the image for the card file's bytes. Format (PS2 icon model): header `u32 0x00010000, u32 shapes
(1), u32 texture type (3 = **no texture**: vertex colours only), f32 1.0, u32 vertex count (1068 = 356 triangles)`;
per vertex `s16 x,y,z,w` per shape (4.12 fixed point), `s16` normal x4, `s16` u,v, `u8` r,g,b,a (24 bytes); then a
36-byte animation block (one frame). The front view (y down, the viewer at -z) is Sly's head - blue mask, grey face,
ears - with a gold "2" on top.

`scripts/disc_art.py icon` reads it (checking the SHA-256 from its `DISCS` table), lights it (key light upper left,
soft fill, a little specular; light colours keep their shading), rasterises it with z-buffer and Gouraud colours at
3-6x supersampling per size, adds a dark rim (~1 px at 16-48) and writes the .ico (PNG entries). Python stdlib only,
~2 s. `build_game.py --package` embeds it as sly2.exe's icon resource when it knows the disc image (`--disc`, else
PS2_ISO / `orig\disc\*.iso`), else draws `make_icon.py`'s; art never fails the build.

## The backdrop from the disc: the opening scene

No disc texture format is known here, so the picture comes from the game itself: `disc_art.py backdrop` runs the
console build just made **headless** (no window, `PS2_AUDIO_NULL`, **`PS2_INPUT_DEVICES=0`**: no controller is opened -
no stray START, light bar or rumble - a throw-away user folder with `motion_blur = off`, `render_scale = 2` (fixed:
the automatic scale drops on a busy GPU and the picture would have 448 lines), 1920x1080 present, `PS2_DUMP_BMP`
every 20 VBlanks from 120 to 2400) under its own exe name, below-normal priority.
After the legal screen the game plays a letterboxed in-engine intro: the camera pans over the pyramids of Cairo at
sunset (no text), then a rooftop with a studio billboard, then the title. Its VBlank varies a lot between runs
(scene start seen at 380, 440, 760, 780: loading speed), so the picture is found by its look: the first picture with
black bars above and below (rows with mean luminance < 5, >= 6% of the height each) and a lit scene between (mean 25..200)
after a picture without bars, kept only if the bars last 60 VBlanks; that last picture is cropped to the scene (and 1/240
of the width at each edge: a stray column) and written as a 24-bit BMP (~1580 x 640, 3 MB). Pictures are read once the
next one exists and deleted at once (never more than two on disk); the process is killed when the picture is taken
(10-30 s on the i7-14700F / RTX 4060 Ti; 150 s limit, then the launcher's own theme). `build_game.py --art` (the
installer passes it) puts `sly2.ico` + `backdrop.bmp` into `<package>\art`; `engine.c` stages them in `game\art`.
The launcher shows the left 80% of it (the pyramid then stands right of the controls).

## Another region

One more entry in `DISCS` (scripts/disc_art.py): the boot executable's name + SHA-256 (as extract_elf.py checks it),
the save icon's sector / size / SHA-256 (search the image for the card file the game writes), the backdrop's search
window. The launcher needs nothing. The runtime and build accept only the USA disc today anyway.

## Tests

`tests\unit\launcher\run.ps1`: synthetic pictures only (tests/unit/launcher/make_pictures.py): the icon's choice
through `/articon <dir> <user data>` (nothing -> the launcher's own; the build's icon; the owner's icon.png wins (10
sizes); an unreadable one falls back; the owner's logo; no source -> back), `/makeicon` refuses a picture that is not
the logo for the mask, the window rendered off-screen (`/uishot`) with no art and with owner art, and
`check_no_game_data.py` on Sly2-Setup.exe, Sly2-Setup-Test.exe and the release bundle (plus a bundle with a hidden
picture, which it must find). Screenshots of the real thing (local only, never committed):
`build\launcher-art\launcher_disc.png`, `launcher_fallback.png`, `launcher_owner.png`, `icon_preview.png`.
