# WIP: HUD edge anchoring, run-time aspect switch, Binocucom 16:9 (branch next/hud)

**Goal**: HUD widgets at the 4:3 screen's left/right anchor to the 16:9 edges (setting
`[display] hud = edges|centered`), aspect/hud switchable at run time, Binocucom overlay covering 16:9.

## Done
- (nothing yet) read notes/widescreen.md, settings.md, wip/widescreen.md.

## In progress
- RE: how HUD widgets are created/drawn (who allocates the priority-21 render entries).

## Next
1. Find the per-widget draw loop (Sly 1: BLOT list, DrawBlots) and widget classes/positions.
2. aspect.c: per-entry anchor (current anchor set around each widget's draw), HUD_BEGIN(L/C/R).
3. settings hud=edges|centered; tests (gs-draw t_hud_hints L/R, window test_settings).
4. Run-time switch (hotkey, live camera aspect).
5. Binocucom overlay.
6. Verification screenshots build\screenshots\hud_*.png, 4:3 identity, tests.

## Findings / traps
