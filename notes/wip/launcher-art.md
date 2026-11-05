# WIP: launcher art (branch next/launcher-art, worktree C:\Users\tanih\Projects\sly2-worktrees\launcher-art)

**Goal**: game icon + launcher look from the player's own disc (never committed/shipped), owner override
(%LOCALAPPDATA%\Sly2\art: logo/backdrop/icon) > disc art > our own theme + procedural icon.
Documentation: notes/disc-art.md, notes/install.md "Launcher" (Look, Pictures).

## Done
- scripts/disc_art.py: icon from the disc's save icon (sector 1169578, PS2 icon model, rendered, 10-size .ico),
  backdrop from a headless run (letterbox detection of the opening scene); runtime PS2_INPUT_DEVICES=0.
- build_game.py --disc/--art (icon resource from the disc; <package>\art), engine.c stages dist\art, passes --disc/--art,
  icon refresh -> shortcuts/DisplayIcon (<dir>\art\sly2.ico), uninstall removes art.
- installer/art.c (+art.h): WIC load, resampler, AA shapes, logo mask polygon, .ico writer (DIB + PNG 256), choice.
- launcher.c: new look (1120x440, left column, backdrop/theme, logo/title, gold PLAY, glass buttons, hover fades,
  dark title bar), what's new from manifest notes, /uishot 8 screens (8-hover), /articon, /makeicon tool commands.
- scripts/check_no_game_data.py + make_installer refuses game data; tests in tests\unit\launcher\run.ps1.
- Owner's logo.webp converted once (Edge headless) to logo.png in his art folder (coordinator OK'd).

## Findings / traps
- No WIC WebP codec on this PC (only RAW + JPEG XL decoders registered).
- Headless dumps follow the internal scale; auto scale drops on a busy GPU -> fix render_scale = 2 in the capture ini.
- Intro timing varies 380..780 VBlanks between runs -> detect the letterboxed scene, don't use fixed VBlanks.
- .NET Framework's Icon class can't read PNG icon entries (preview tools must use LoadImage).
- [IO.File] relative paths resolve against the main checkout: always absolute.
- build\setup\Sly2-Setup.exe goes stale: a tool command it does not know opens the (visible) launcher window.
