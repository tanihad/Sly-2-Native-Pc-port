# WIP: the owner's art inside Setup (branch next/setup-art, worktree C:\Users\tanih\Projects\sly2-worktrees\setup-art)

**Goal**: `make_installer.py --art <folder>` embeds backdrop.* + logo.* as resources and the logo's mask badge as the
exe icon; launcher order user folder > embedded > disc > theme; window icon sly2.ico > user-folder art (in memory)
> exe resource; check_no_game_data allows exactly the embedded files by SHA-256 (allow-list written by the build).
Owner decision (2026-11-05): release Setups are built with `--art %LOCALAPPDATA%\Sly2\art`; the source bundle never
carries art; document in notes/install.md "Publishing an update"; output line "Setup includes the owner's art from <folder>".

## Done

## In progress
- art.c/launcher.c, make_installer.py, check_no_game_data.py

## Next
1. tests (launcher run.ps1: no-art = no pictures + procedural icon; --art with synthetic pictures)
2. build the owner Setup into build\installer-owner, screenshots build\setup-art\setup_welcome.png, exe_icon.png
3. notes (install.md Look/Pictures + Publishing, disc-art.md), final commit

## Findings / traps
