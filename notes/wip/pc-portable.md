# WIP: pc-portable (build and run on any Windows PC from the GitHub repo)

**Goal**: fresh clone -> one setup command -> disc image in orig\disc -> one build command ->
dist\Sly2\sly2.exe that runs on other Windows 10/11 x64 PCs.

**Status**: in progress

## Done
- (nothing yet)

## In progress
- Audit + plan.

## Next
1. requirements.txt (pinned + hashes), setup.ps1 at the root (pinned tool versions + SHA-256,
   -From <dir> offline install, -DryRun, -WithRE for Ghidra/JDK/objdiff).
2. ELF extracted from the disc image by the build (scripts/extract_elf.py), build.ps1 at the root.
3. Remove machine-specific paths (live_*.ps1, m9_*.ps1, corpus scripts, pad/vif run.ps1, test_vu.c).
4. Runtime audits: DLL imports, CPU instruction scan, Windows API floor, GPU fallbacks.
5. Fresh clone test in %TEMP% (no downloads; -From main checkout's tools).
6. README.md + notes/install.md.

## Findings / traps
- Build Python deps: only rabbitizer + pyelftools (recomp/). splat/capstone are RE-only.
