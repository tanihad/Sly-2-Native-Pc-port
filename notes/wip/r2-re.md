# r2-re: reverse-engineering the game's rendering for a modern high-level renderer

**Goal**: map Sly 2's rendering data and code (objects, meshes, materials, textures, animation,
VU1 programs, effects) with evidence, measure scenes, and propose where a modern renderer takes
over (summary in notes/modern-renderer-re.md).

**Status**: in progress (started 2026-10-26)

## Done
- (nothing yet) read notes/widescreen.md, notes/vu-microcode.md

## In progress
- reading the existing notes

## Next
1. read d3d11-renderer.md (census), perf-plan.md, pc-research.md, hle-architecture.md, frame-rate.md
2. Sly 1 decomp (web) for SO/ALO/GLOB/SUBGLOB structures
3. EE code: render list allocation callers, per-class begin/end, mesh submit functions
4. tracing tool scripts/r2_* (debug hook, off by default)
5. measurements in 5 scenes
6. write up

## Findings
