# ep3: playtest Episode 3 "The Predator Awakens" (India, jungle)

**Goal**: play Episode 3 (hub world 12 jungle, world 13 spice factory interior) end to end via
warp saves / direct job starts; fix every runtime/recompiler problem found at its root.

## Done
- Merged next/warp (178353c). Release build (build\rel) OK.

## In progress
- Debug build; first hub load (warp_run -Episode 3 -Play -Release).

## Next
1. Hub (world 12): explore, frames, PS2_PERF.
2. Jobs in order: m3_recon (world 13 interior, spycam), m3_waterbug, m3_free_elephant,
   m3_lead_rajan (Bentley), m3_turret (Bentley turret, world 12 warp 17610), m3_steal_ruby (Murray
   parts), m3_heartneela (world 13), m3_tntbarrel (world 13), m3_heist (Murray/Bentley/Sly stages),
   m3_boss (Rajan, world 12 warp 97868/97881).
3. Cutscenes/movies, autosave at checkpoints.

## Findings / traps
- Episode 3 tasks: scripts\warp_tasks.txt (t3_*), oids 2167-2350.
