# WIP: r2-design (final design of the modern high-level renderer)

**Goal**: combine notes/modern-renderer-re.md and notes/modern-renderer-api.md into the final design
notes/modern-renderer.md (architecture, Original vs Enhanced, verification, perf targets, risks, milestones R0..Rn,
owner decisions).

## Done
- Read: modern-renderer-re.md + wip/r2-re.md, modern-renderer-api.md + wip/r2-api.md, gpu-renderer.md, d3d11-renderer.md
  (2.1, 3.1-3.6, 8, 10.2, 13, appendix P), perf-plan.md, widescreen.md (hint mechanism), native-port-principles.md.

- notes/modern-renderer.md written (final design, milestones R0-R11, owner decisions D1-D8).

**Status: complete** (2026-10-26).

## Next
- Owner review of notes/modern-renderer.md; decisions D1 (Original fidelity) before R1, D2/D3 before R2.

## Findings / traps
- git is not on PATH in the worktree: use C:\Users\tanih\Projects\sly2-decomp\tools\git\cmd\git.exe.
- Side-band hints already travel inside the VIF1 chain (aspect_hint: VIF DIRECT + A+D write of GS register 0x7F at the
  DMAS write position 0x2DEA28 +0xC): the same channel carries NATIVE hints, so ordering with exact draws is free.
