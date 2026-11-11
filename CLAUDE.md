# Project guide for Claude

Matching decompilation of Sly 2: Band of Thieves (PS2, MIPS R5900 "Emotion Engine").
The user owns the game and supplies the executable from their own disc.

## Goal: a native PC port, not an emulator

Read `notes/native-port-principles.md` before designing anything. Game code (EE and VU
microcode) is statically recompiled, never interpreted; PS2 hardware requests are translated to
PC APIs (Direct3D 11, WASAPI, XInput, files) at a high level, not simulated cycle- or
pixel-accurately. The project owner explicitly does not want "another PCSX2".

## Checkpoints (work must survive interruptions)

Follow `notes/agent-checkpoints.md`: at start, look for `notes/wip/<task>.md` and WIP commits from
an earlier attempt and continue from them; while working, update that file and commit after every
meaningful step, so an interrupted run resumes where it stopped instead of starting over.

## Rules

- Never commit anything from `orig/`, `build/`, `asm/`, `ghidra/`, or tool binaries.
- A function is only "matched" when objdiff reports 100% against the original. Don't record
  partial matches as done.
- Ask before downloading or installing any tool.
- Record findings (struct layouts, compiler quirks, file formats) in `notes/` as you go.

## Environment

- Windows 11, PowerShell 5.1. All tools are portable copies inside `tools/` (see
  `tools/README.md`). Start every shell command that needs them with `. .\scripts\env.ps1`.
- Use `.venv` (Python 3.13), not the system Python 3.15.
- Ghidra runs headless only (`analyzeHeadless.bat`); the PS2 Emotion Engine plugin is installed.
- The original compiler is still unknown, so nothing can be byte-matched yet.

## Current direction: static recompiler (PS2 -> native x86-64)

- Design and milestones: `notes/recompiler-design.md`.
- `recomp/` (Python) translates the ELF to C; `runtime/` (C) is the hardware/BIOS stand-in.
- Run `.venv\Scripts\python.exe tests\run_tests.py` after any codegen or runtime change. It
  compiles a synthetic PS2 program with clang and checks results against real R5900 semantics.
  Add a case to `tests/make_test_elf.py` for each newly translated instruction.

## GitHub

- Remote: https://github.com/tanihad/Sly-2-Native-Pc-port (renamed by the owner from sly2-decomp on 2026-11-11;
  GitHub redirects the old name, which released launchers and manifests still use). Public since 2026-11-11, the
  owner's choice; launchers self-update from releases/latest. Commits are authored as `tanihad`
  with the no-reply email (repo-local git config). Credentials are stored by Git Credential
  Manager, so `git push` works from the agent shell.
- The user's GitHub account is **tanihad**. Two Chrome browsers are connected to Claude in
  Chrome: the user's Windows one (signed in as tanihad) and a Mac one signed in to a different
  person's account. Only use the Windows browser, and confirm the account shown is tanihad before
  acting on GitHub.
- The installed `reverse-engineering` skill (~/.claude/skills) covers general binary triage and
  assembly-reading methodology.
