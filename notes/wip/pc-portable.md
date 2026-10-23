# WIP: pc-portable (build and run on any Windows PC from the GitHub repo)

**Goal**: fresh clone -> one setup command -> disc image in orig\disc -> one build command ->
dist\Sly2\sly2.exe that runs on other Windows 10/11 x64 PCs.

**Status**: in progress

## Done
- requirements.txt (rabbitizer 1.16.2 + pyelftools 0.33, PyPI hashes), requirements-re.txt.
- setup.ps1 (root) -> scripts/setup-tools.ps1: pinned llvm-mingw 20261006, uv 0.13.0, MinGit 2.56.0.2
  (only without git on PATH) with SHA-256; Python 3.13.16 via uv; -From <dir> offline (archives,
  tools\python copy, uv-cache seed via robocopy), -DryRun, -WithRE. Verified offline in a fresh clone
  (the "PAUSED" commit f525417 is the robocopy seeding fix, the exact version that ran there).
- build.ps1 (root): extract_elf + package.py. scripts/extract_elf.py: SCUS_973.16 from the ISO
  (SHA-256 checked); build_game.py extracts it automatically when missing.
- Machine-specific paths removed: scripts/project.ps1 ($MainRoot/$ProjectPython/$ProjectClang),
  scripts/gpu_corpus.ps1 (Resolve-GpuCorpus: -CorpusRoot, SLY2_GPU_CORPUS, sibling sly2-worktrees;
  exit 0 SKIPPED when absent) used by gpu_*_all/bench, gs_census, gs_vram_hashes, m9_*; live_*.ps1,
  pad/vif run.ps1, test_vu.c default, corpus.txt comment.
- Fresh clone %TEMP%\sly2fc (git clone --no-local -b next/pc-portable): setup -From main checkout
  11 s, build.ps1 -Iso <iso> 50 s (28 threads) -> dist\Sly2\sly2.exe 27.1 MB.
- Imports: KERNEL32, USER32, GDI32, WINMM, api-ms-win-core-synch-l1-2-0 (WaitOnAddress), UCRT
  api-ms-win-crt-*. No MinGW runtime DLLs. Run-time loaded: d3d11, dxgi, d3dcompiler_47 (inbox),
  xinput, ole32, comdlg32, shell32, avrt, dwmapi.
- ISA scan (scratchpad isa_scan.py, objdump -M intel): 4.48 M instructions, only SSE2 baseline plus
  `tzcnt` (19, = `rep bsf` encoding LLVM uses for cttz with nonzero input; runs as BSF on pre-BMI CPUs).
- GPU boot (visible window, before the owner asked for headless): OK, RTX 4060 Ti.

## In progress
- Headless boot matrix of the fresh-clone exe (PS2_HEADLESS=1, frame dumps every 600):
  gpu, sw, warp, NO_ROV, FL=11_0, FAIL=device/caps/shader/swapchain, first run without ini (PS2_ISO).
  Script: scratchpad boot_test.ps1 (copy into %TEMP%\sly2fc_play\<name>, portable.txt).

## Next
1. Finish the boot matrix (headless only; owner is using the PC).
2. Gates in the worktree: tests\run_tests.py, tests\unit\*\run.ps1, debug + --release, package.py.
3. README.md rewrite + notes/install.md + tools/README.md.
4. Delete %TEMP%\sly2fc, %TEMP%\sly2fc_play, logs; final commit, Status: complete.

## Findings / traps
- Build Python deps: only rabbitizer + pyelftools (recomp/). splat/capstone are RE-only.
- uv venv creates tools\uv-cache, so seed the cache by merging (robocopy), not "if missing".
- uv prints progress on stderr: harmless NativeCommandError noise only when redirected with 2>&1.
- GPU renderer requires DoublePrecisionFloatShaderOps: Intel Xe/Arc iGPUs/GPUs lack FP64 ->
  software renderer there (renderer owner's call; documented).
- All high-res waitable timer users handle NULL (pre-1803) already.
- Boot tests must be headless (PS2_HEADLESS=1): the owner uses the PC.
