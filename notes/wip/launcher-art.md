# WIP: launcher art (branch next/launcher-art, worktree C:\Users\tanih\Projects\sly2-worktrees\launcher-art)

**Goal**: game icon + launcher backdrop made from the player's own disc at install/build time (never committed or
shipped); procedural icon (scripts/make_icon.py) + our own dark theme as fallback; nicer launcher window.

## Findings
- The save icon `memcard_icon.ico` the game writes to the card is a raw file on the USA disc image at
  **sector 1169578**, 25,688 bytes (found by searching the image; not in SCUS_973.16). PS2 icon format: magic
  0x00010000, 1 shape, texture type 3 (= **no texture**, vertex colours only), 1068 vertices (356 triangles),
  24 bytes each (xyzw s16, normal s16x4, uv s16x2, rgba8). Front view = Sly's head (blue mask, grey face, ears)
  with a big gold "2" on top: the icon.
- Disc asset formats (textures, levels) are not documented in notes/; the backdrop needs another way.

## Plan / status
- [ ] backdrop source
- [ ] scripts/make_disc_art.py (icon from the disc, multi-size .ico)
- [ ] build_game.py / installer wiring
- [ ] launcher look
- [ ] tests, notes
