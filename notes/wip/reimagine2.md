# WIP: next/reimagine2 (owner playtest fixes on 0.1.1214)

Branch next/reimagine2 from main 71ff737 (= d2de0e7 + progress note). Worktree sly2-worktrees\light.
Evidence videos: build\reimagine\videos2 (not committed).

Tasks (owner priority order):
1. [ ] Fires cast light shafts -> shafts only from sky/moon key light; fires get warm glow + local light flicker.
2. [ ] Tree shadows flicker when Sly walks under them (ep2, ep3 swamp) -> find cause, fix, add shot to stability gate.
3. [ ] Sly blurry -> crisp at 1440p (anime_6B for characters? CAS sharpen? surface detail?), 1:1 crops.
4. [ ] Reimagined on High (4x internal, 8x MSAA) <= 7.5 ms GPU at 1440p (120 fps + 10%).
5. [ ] Deterministic stability gate (3/3 passes, each <= 1.15x Faithful).
6. [ ] Videos: ep2 fire, trees, Sly close-up orbit (+1:1 stills), ep1 orbit High with frame time; before/after.
7. [ ] Gates alone, commit, report.

## Log
