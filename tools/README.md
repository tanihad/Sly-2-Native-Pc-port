# tools/

Portable toolchain (binaries are git-ignored). Rebuild with `scripts/setup-tools.ps1`, which
verifies every download against its published SHA-256. Load into a shell with
`. .\scripts\env.ps1`.

| Tool                                  | Version   | Location                               |
|---------------------------------------|-----------|----------------------------------------|
| Git (MinGit)                          | 2.56.0.2  | `tools/git`                            |
| Eclipse Temurin JDK                   | 21.0.12.1 | `tools/jdk`                            |
| Ghidra                                | 12.1.4    | `tools/ghidra` (headless: `analyzeHeadless.bat`) |
| ghidra-emotionengine-reloaded (PS2)   | 2.1.38    | `tools/ghidra/Ghidra/Extensions`       |
| objdiff-cli                           | 3.8.2     | `tools/objdiff-cli.exe`                |
| uv                                    | latest    | `tools/uv`                             |
| Python (uv-managed)                   | 3.13.16   | `tools/python`, venv in `.venv`        |
| splat64 (+ spimdisasm, rabbitizer)    | 0.50.0    | `.venv`                                |
| pyelftools / capstone                 | 0.33 / 5.0.9 | `.venv`                             |
| Original PS2 compiler                 | unknown   | must match what Sucker Punch used - research needed |

The system Python 3.15 is not used: splat's dependencies have no 3.15 wheels yet.
