# tools/

Portable toolchain (binaries are git-ignored). Install with `setup.ps1` at the repo root
(`scripts/setup-tools.ps1`: pinned versions, every archive checked against its SHA-256; `-From <dir>`
offline, `-DryRun`, `-WithRE`). Load into a shell with `. .\scripts\env.ps1`.

Needed to build the game (installed by default):

| Tool                                  | Version   | Location                               |
|---------------------------------------|-----------|----------------------------------------|
| llvm-mingw (clang, ucrt)              | 20261006 / clang 23.1.3 | `tools/llvm-mingw` - compiles generated C |
| uv                                    | 0.13.0    | `tools/uv`                             |
| Python (uv-managed)                   | 3.13.16   | `tools/python`, venv in `.venv`        |
| rabbitizer / pyelftools               | 1.16.2 / 0.33 | `.venv` (`requirements.txt`, hash-pinned) |
| Git (MinGit)                          | 2.56.0.2  | `tools/git` (only when no git is on PATH) |

Reverse-engineering only (`setup.ps1 -WithRE`):

| Tool                                  | Version   | Location                               |
|---------------------------------------|-----------|----------------------------------------|
| Eclipse Temurin JDK                   | 21.0.12.1 | `tools/jdk`                            |
| Ghidra                                | 12.1.4    | `tools/ghidra` (headless: `analyzeHeadless.bat`) |
| ghidra-emotionengine-reloaded (PS2)   | 2.1.38    | `tools/ghidra/Ghidra/Extensions`       |
| objdiff-cli                           | 3.8.2     | `tools/objdiff-cli.exe`                |
| splat64 (+ spimdisasm) / capstone     | 0.50.0 / 5.0.9 | `.venv` (`requirements-re.txt`)   |
| Original PS2 compiler                 | unknown   | must match what Sucker Punch used - research needed |

The system Python is not used (the project's Python 3.13 is self-contained in `tools/python`).
