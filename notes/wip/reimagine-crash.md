# WIP: Look switch crash (owner, main 6bbf81a / 0.1.1226)

Branch next/reimagine-crash from main 6bbf81a (worktree sly2-worktrees\light).
Crash: Faithful -> Reimagined + Apply, 0xC0000005 reading 0xFFFF..FF in nvwgf2umx.dll (+0x2ac780) or ntdll (+0x2736d);
logs end after "ai: Reimagined textures on", before "x4plus net ready".

Plan: stress hook (look toggles + quality changes from the pad thread, as the menu's Apply) -> repro -> prove ->
fix (all context use on the render thread, safe lifetimes) -> gate script (500 toggles cold + warm, 0 crashes) ->
standard gates + reim_stability_gate -> commit -> report.

## Log
- ROOT CAUSE (proven): gpu_tex_ai.h ra_list (the Reimagined cache listing, at every Reimagined ON) kept RA.nhave from
  the last listing with a local capacity of 0 -> appended past the end of the old buffer (heap overflow). Stress repro
  (scripts/gfx_stress_gate.ps1, PS2_GFX_STRESS look toggles, warm cache 1763 files): the logged count grew 1763 per
  toggle (26445 "in the cache") and the game crashed after 28 changes with the owner's signature (0xC0000005 reading
  0xFFFF..FF, ntdll). Cold cache survived 1405 changes only because the list stayed small at first. Fixed: reset + own
  capacity. Also: worker (ra_init_net) no longer writes RA.kcs / src / sav (handed over to the render thread);
  ra_release resets items under the lock.
- COMPLETE: stress after the fix 0 crashes in 6873 changes (warm Look-only 1366, warm all 2664, cold all 2843); gates green. Results: notes/reimagined.md 'Look switch crash'.
