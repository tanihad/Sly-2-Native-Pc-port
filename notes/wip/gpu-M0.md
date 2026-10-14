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
- scripts/hlslc.c + scripts/gpu_shaders.ps1 (build/tools/hlslc.exe -> build/gen/gpu_shaders.h;
  warnings are errors, verified it fails on an uninitialised variable).
- runtime/gpu: gpu.h, gpu_cmd.c/.h, gpu_internal.h, gpu_d3d11.c (render thread, device, caps),
  gpu_present.c (picture upload/compose, readback ring, swap chain / offscreen, timestamps, stats),
  gpu_conf.c (conformance), gpu_verify.c/.h (picture compare + "both" verifier),
  shaders/{int64.hlsli, present.hlsl, conformance.hlsl, perms.txt}.
- tests/unit/gs-hw (run.ps1, test_gs_hw.c): 109 checks pass on 4060 Ti and WARP
  (conformance 2 seeds x 131072 vectors + 65536 depth texels: 0 mismatches on both).

- scripts/gs_replay.c/.ps1: -Backend sw|hw|both, -Adapter, -Metrics, -Log, sw-hash, watchdog
  (exit 4 after 60 s without progress, prints render-thread phase + ring positions).
- sw unchanged: old gs_replay.c (2571d8e) vs new -Backend sw vs new -Backend hw PNGs byte
  identical on rv_intro (16) and title (30); runtime/gs*.c untouched by M0.
- scripts/gpu_diff_all.ps1 + tests/gpu/baseline.json: 13/13 recordings, 2036 pictures, 0 diff.
- build_game.py: hlslc + shaders + runtime/gpu/*.c; debug and --release build; release game
  smoke run with PS2_GS_BACKEND=hw logs the "not in the game yet; using sw" line and renders.
- notes/d3d11-renderer.md appendix B (as built).

## In progress
- final verification: run_tests.py + every tests/unit/*/run.ps1 (scratch script all_tests.ps1),
  then final gpu_diff_all (gate mode, no -Update) and gs-hw.

## Next
1. if all green: Status complete, final commit, report.

## Findings / traps
- Caps (D3D11, FL 11_1 on both): RTX 4060 Ti (10DE:2803, driver 32.0.16.1656) and WARP
  ("Microsoft Basic Render Driver" 1414:008C) both report ROVsSupported=1,
  TypedUAVLoadAdditionalFormats=1, dual source, ConstantBufferOffsetting=1, partial CB update,
  MapNoOverwriteOnDynamicConstantBuffer=1, doubles=1. Swap chain: waitable=1, tearing=1.
  So WARP can run the ROV tier in headless tests (risk in section 12 does not materialise).
- **fxc miscompile**: `(uint)clamp((int)u + delta, 0, K)` with `u` a uint compiles to
  `iadd; umin` (the max with 0 is dropped and the min is unsigned), so -1 becomes K. Found by the
  conformance kernel. Rule for the ubershader: never clamp an int built from a uint cast; use
  explicit comparisons (as gs_z_clamp in int64.hlsli does) and keep conformance coverage for any
  such helper.
- The first corpus run stalled once on ep3/rope (gs_replay idle, 1.4 s CPU) when its stderr went
  through nested PowerShell pipes (gpu_diff_all -> powershell gs_replay.ps1 -> exe 2>&1 | ...).
  20 direct reruns never hung. Now the replay's stderr goes to a file through `cmd /c ... 2>log`
  and the watchdog guarantees an exit with diagnostics if a real deadlock ever happens.
- Stopping processes by PID is refused by the agent's permission classifier; stop a background
  task with TaskStop instead (it took the stuck replay with it).
- No D3D debug layer installed (d3d11_3SDKLayers.dll absent): debug flag must fall back.
- llvm-mingw headers have D3D11_FEATURE_D3D11_OPTIONS2 (ROVsSupported etc.) and dxgi1_2..1_6.
