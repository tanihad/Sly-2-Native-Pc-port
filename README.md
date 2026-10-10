# sly2-decomp

Personal reverse-engineering workspace for **Sly 2: Band of Thieves** (PS2), with the long-term
goal of contributing to a matching decompilation and, eventually, a native x86 port.

This repo contains **no game code or assets**. You must supply the executable dumped from your
own retail disc (see `orig/README.md`).

## Layout

| Folder      | Purpose                                                              |
|-------------|----------------------------------------------------------------------|
| `orig/`     | Your dumped game files (git-ignored, never committed)                |
| `src/`      | Decompiled C source, one file per original translation unit          |
| `include/`  | Headers: types, structs, function prototypes                         |
| `asm/`      | Disassembly split out of the original executable (generated)         |
| `config/`   | Symbol maps, splitter config, compiler flags                         |
| `tools/`    | Toolchain: Ghidra, objdiff-cli, compiler (git-ignored binaries)      |
| `ghidra/`   | Ghidra project files (git-ignored)                                   |
| `build/`    | Build output and object files (generated)                            |
| `notes/`    | Research notes, findings, progress log                               |

## Status

See `notes/progress.md`.

## Workflow (matching decomp loop)

1. Pick an unmatched function from the original executable.
2. Read its disassembly / Ghidra pseudo-C.
3. Write C in `src/` that should compile to the same instructions.
4. Build with the original compiler and flags, then diff against the original with `objdiff-cli`.
5. Iterate until the function matches 100%, then record it in `notes/progress.md`.
