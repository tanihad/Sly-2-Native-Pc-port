# Progress log

## Target build

- Game: Sly 2: Band of Thieves, NTSC-U, `SCUS_973.16`, VER 1.00 (SYSTEM.CNF)
- Executable SHA-1: `de2af92a54de71e33edc8a160c31c0337a95e6f5`
- Disc filesystem holds only SYSTEM.CNF, SCUS_973.16, IOPRP300.IMG, Z6TAIL.DUP (32 MB padding).
  The ~2.8 GB of game data is outside the filesystem, so the game reads it by raw sector (LBA).

## Executable layout

| Section   | Range                   | Notes                                                    |
|-----------|-------------------------|----------------------------------------------------------|
| .text     | 0x00100000-0x00259DF0   | EE code, recompiled                                      |
| .vutext   | 0x00259DF0-0x00262330   | VU microcode, different ISA, not EE code                 |
| .rodata   | 0x00262380-0x002AC864   | includes switch jump tables                              |
| .data     | 0x002AC880-0x002DD230   | vtables, init/ctor tables, libkernel patch blobs         |
| .bss      | 0x002DD380-0x00452200   |                                                          |
| .irx      | 0x00472200-0x004E5E40   | embedded IOP modules (second segment)                    |

No symbol table.

## Recompiler status (2026-10-10)

- ~6,900 functions found; 371k EE instructions, all but 14 translated (12 `vcallms`, 2 `eret`).
- VU0 macro mode and the MMI instructions the game uses are implemented and tested.
- Function discovery: jal/j targets, code constants, data pointers (vtables, ctor tables),
  after-`jr $ra` boundaries, with switch jump tables protected and a merge pass for bad guesses.

## Boot progress (what runs before it stops)

1. crt0: SetupThread, SetupHeap, FlushCache.
2. libkernel kernel patches: SetSyscall + FindAddress scan of kernel RAM to locate the syscall
   table (runtime emulates a real table at phys 0x10000), alarm patch copied from .data to
   0x80076000 and a second patch at 0x80075000 (not recompiled; runtime HLE stands in).
3. Timer 2 setup, AddIntcHandler/EnableIntc, CreateThread, ChangeThreadPriority,
   GetOsdConfigParam, CreateSema.
4. All C++ static constructors and init tables.
5. SIF setup: AddDmacHandler(5 = SIF0), EnableDmac, then polls SifGetReg(4) for IOP boot.
   Runtime now reports the IOP as booted. **Next: SIF command/RPC layer + IOP HLE.**

## Blockers

- Windows Smart App Control (on) started blocking freshly built `sly2.exe` builds.

## Open questions

- [ ] Which IOP modules does the game load (from .irx and IOPRP300.IMG), and what RPCs does it use?
- [ ] How does the game locate its data on disc (sector table in the executable?)

## Matched functions (decomp work, paused)

| Function | Address | Source file | Date |
|----------|---------|-------------|------|
