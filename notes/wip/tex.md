# WIP: Enhanced textures (branch next/tex, worktree C:\Users\tanih\Projects\sly2-worktrees\light)

Goal: textures clearly sharper / cleaner at 1440p (HD-remaster look), Original bit-identical and free. Design:
notes/enhanced-textures.md. Setting graphics.textures = enhanced | original (live), env PS2_TEX=0/1 + per feature.

## Status
- [ ] 1 design note + texture statistics (classification numbers)
- [ ] 2 ET cache: class arrays, etab lookup, GPU upscaler (edge-directed 2x passes + sharpen), mips (coverage), aniso
- [ ] 3 R2 permutations (R2_ETEX) + MFT FF triangle permutations (phase A only)
- [ ] 4 setting / menu / presets / tests
- [ ] 5 HD-pack folder hook + hash dump (GPU hash readback, WIC PNG)
- [ ] 6 tuning with pictures (build\tex\shots), perf A/B (avg + worst)
- [ ] 7 gates

## How to run (worktree has no .venv)
- `. C:\Users\tanih\Projects\sly2-decomp\scripts\env.ps1; cd C:\Users\tanih\Projects\sly2-worktrees\light`
- python: C:\Users\tanih\Projects\sly2-decomp\.venv\Scripts\python.exe
- set PS2_USER_DIR to build\tex\user for every game run (HD pack folder lives there)

## Findings / traps
- (see notes/wip/light.md: never relative paths with [IO.File]; motion_blur=off shrinks pictures)

## Log
- branch next/tex from main 7601e3f
