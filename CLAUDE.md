# Project guide for Claude

Matching decompilation of Sly 2: Band of Thieves (PS2, MIPS R5900 "Emotion Engine").
The user owns the game and supplies the executable from their own disc.

## Rules

- Never download game ISOs/executables or copyrighted assets. Original files come only from the
  user's own dump in `orig/`.
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

## GitHub

- Remote: https://github.com/tanihad/sly2-decomp (private). Commits are authored as `tanihad`
  with the no-reply email (repo-local git config). Credentials are stored by Git Credential
  Manager, so `git push` works from the agent shell.
- The user's GitHub account is **tanihad**. Two Chrome browsers are connected to Claude in
  Chrome: the user's Windows one (signed in as tanihad) and a Mac one signed in to a different
  person's account. Only use the Windows browser, and confirm the account shown is tanihad before
  acting on GitHub.
- The installed `reverse-engineering` skill (~/.claude/skills) covers general binary triage and
  assembly-reading methodology.
