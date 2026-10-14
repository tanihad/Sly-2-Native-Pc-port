# WIP: gpu-M0 (harness, render thread, all-fallback hw backend)

**Goal**: milestone M0 of notes/d3d11-renderer.md section 11 on branch gpu/M0.

Status: in progress

## Plan (file layout)
- runtime/gpu/gpu.h            backend interface (config, caps, picture, readback, stats)
- runtime/gpu/gpu_cmd.c/.h     SPSC command ring (published in 64 KB chunks) + data ring
- runtime/gpu/gpu_d3d11.c      render thread, device (HWND/headless, hw/WARP, FL 11_1), caps, exec
- runtime/gpu/gpu_present.c    picture upload/compose, readback ring, swapchain (flip-discard,
                               waitable, tearing) or offscreen present target, GPU timestamps
- runtime/gpu/gpu_conf.c       conformance runner (cs_conformance vs C, SV_Depth round trip)
- runtime/gpu/shaders/         int64.hlsli, cs_conformance.hlsl, present.hlsl, perms.txt
- scripts/hlslc.c              D3DCompile via System32 d3dcompiler_47.dll -> build/gen/gpu_shaders.h
- scripts/gpu_shaders.ps1      builds hlslc + header for the PowerShell harnesses
- scripts/build_game.py        builds hlslc, compiles shaders, compiles+links runtime/gpu/*.c
- scripts/gs_replay.c/.ps1     -Backend sw|hw|both, -Adapter hw|warp, -Metrics, sw picture hash
- scripts/gpu_diff_all.ps1     all 13 recordings, -Backend both, gate vs tests/gpu/baseline.json
- tests/unit/gs-hw/            caps, conformance (hw + WARP), command ring stress, picture round
                               trip, swapchain present in a small window
- ps2_runtime.c                PS2_GS_BACKEND (default sw; hw -> message until M6)

## Done
(nothing yet)

## In progress
- writing runtime/gpu skeleton

## Next
1. hlslc.c + shaders + gpu_shaders.ps1
2. gpu_cmd + render thread + device/caps
3. conformance kernel + unit test (hw, WARP)
4. all-fallback picture path + readback + swapchain
5. gs_replay backends + metrics; gpu_diff_all + baseline over 13 recordings
6. build_game integration (debug + release), PS2_GS_BACKEND
7. run all unit suites + run_tests.py; sw hashes unchanged check

## Findings / traps
- No D3D debug layer installed (d3d11_3SDKLayers.dll absent): debug flag must fall back.
- llvm-mingw headers have D3D11_FEATURE_D3D11_OPTIONS2 (ROVsSupported etc.) and dxgi1_2..1_6.
