# WIP: the owner's art inside Setup (branch next/setup-art, worktree C:\Users\tanih\Projects\sly2-worktrees\setup-art)

**Status: complete** (not merged, not released).

**Goal**: `make_installer.py --art <folder>` embeds backdrop.* + logo.* as resources and the logo's mask badge as the
exe icon; launcher order art folder > embedded > disc > theme; window icon sly2.ico > art-folder art (in memory)
> exe resource; check_no_game_data allows exactly the embedded files by SHA-256 (allow-list written by the build).
Owner decision (2026-11-05): release Setups are built with `--art %LOCALAPPDATA%\Sly2\art`; the source bundle never
carries art; documented in notes/install.md "Publishing an update".

## Done
- installer/art.c: WIC decode from memory, art_load_resource (RCDATA), art_make_hicon (badge / fitted, in memory).
- installer/launcher.c: load_art art folder > ART_BACKDROP/ART_LOGO > disc; choose_icon/icon_at for the window
  icon and the theme emblem; log lines "[ui] art: ... built in", "[ui] window icon: ...".
- scripts/make_installer.py --art: find_art, two-pass icon (/makeicon of a procedural-icon build), RCDATA in the rc,
  Sly2-Setup.art-allow.txt, checks its Setup (+ Offline) with it; prints "Setup includes the owner's art from <folder>".
- scripts/check_no_game_data.py: PE resources parsed; icon must be make_icon.py's or allowed; --allow <file>.
- scripts/make_release.py: hint for the art Setup + check command.
- tests/unit/launcher/run.ps1 (+ setup_resources.py): no-art build = no RCDATA, own icon; --art with synthetic
  pictures = resources, badge icon on the list, check fails without / with a partial list, passes with it; built-in
  pictures used, art folder wins; window icon from the art folder's logo before install.
- notes/install.md (Pictures, window icon, Publishing an update), notes/disc-art.md.

## Findings / traps
- This agent's shell runs inside the Claude app package: %LOCALAPPDATA% is virtualised and Path.resolve() turns
  `C:\Users\tanih\AppData\Local\Sly2\art` into `...\Packages\Claude_...\LocalCache\Local\Sly2\art` -> make_installer
  uses absolute(), not resolve(), for --art paths.
- A Setup's icon is resource 1 (RT_GROUP_ICON); its RT_ICON images are the .ico's image blobs byte for byte, so the
  allow-list hashes the .ico's images.
- PowerShell 5.1 has no [uint[]] type accelerator: New-Object UInt32[].
