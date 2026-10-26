# WIP: r2-api (modern high-level renderer: prior art + graphics API)

**Goal**: prior art of high-level renderers for recompiled/ported console games, a graphics API recommendation and an
architecture sketch for the Sly 2 modern renderer -> notes/modern-renderer-api.md. (RE of Sly's own rendering is
notes/wip/r2-re.md, another agent.)

## Done
- Read: native-port-principles, gpu-renderer, d3d11-renderer 0-2 + 13, perf-plan, pc-research-sources, frame-rate,
  install, agent-checkpoints, wip/d3d11-prior-art (RT64 / Unleashed sections).

## In progress
- Web research: RT64, libultraship, Unleashed/plume, SM64 PC, Zelda64Recomp, PS2 recomp attempts.

## Next
1. Research APIs: D3D11/12, Vulkan+MoltenVK, Metal, WebGPU (Dawn/wgpu), SDL3 GPU, bgfx, Diligent, NVRHI, plume.
2. Shader toolchain: DXC / glslang / SPIRV-Cross / SDL_shadercross; what d3dcompiler_47 + clang can do today.
3. Modern techniques list.
4. Write notes/modern-renderer-api.md; commit; summary.

## Findings / traps
- git is not on PATH in the worktree: `. C:\Users\tanih\Projects\sly2-decomp\scripts\env.ps1` (worktree has no tools/).
- Context facts: game dt is real time (clamp 1/60 lifted by frame_rate patch) -> high fps = real ticks, no interpolation
  needed; CPU stages (worker/VU1, GS decode, frontend) 5-9 ms each limit fps; perf-plan option I = HLE renderer.
