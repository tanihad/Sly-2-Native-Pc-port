# ep5: playtest Episode 5 "A Tangled Web" (Prague)

**Goal**: play Episode 5 (hub world 17) end to end via warp saves / direct job starts, find and fix
runtime/recompiler problems at the root (crash, hang, missing syscall/RPC, rendering, audio, perf).

## Done
- Merged next/warp (178353c) into next/ep5.

## In progress
- Building release (build\rel) and debug (build\sly2).

## Next
1. Episode 5 start (warp_run -Episode 5 -Play -Release): hub, explore, frames.
2. Jobs (warp.py list --episode 5): m5_recon, m5_ghost, m5_ewok_trap (Bentley), m5_kidnap (Murray),
   m5_battlezone (Murray tank), m5_pickpocket, m5_hack (Bentley hacking), m5_heist (Murray), boss?
3. Check cutscenes, saving at checkpoints, perf (PS2_PERF), audio (-Wav).

## Findings / traps
