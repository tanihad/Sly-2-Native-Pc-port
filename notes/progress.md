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
5. SIF setup: AddDmacHandler(5 = SIF0), EnableDmac, polls SifGetReg(4) for IOP boot.
6. SIF RPC init (sreg 0 handshake), then CD/DVD init. Game halts unless disc type == 0x14
   (PS2 DVD); runtime answers from the ISO.
7. IOP reboot with IOPRP300.IMG, LOADFILE version check ("3000"), modules loaded:
   sio2man, padman, mcman, mcserv, Sound_Device_Library (libsd), USB_driver, lgAud,
   989snd_Library, spiop (Sucker Punch).
8. GS reset + display mode (sceGsResetGraph, NTSC interlaced), pad, memory card, fileio and
   989snd servers bound. 989snd batches need 0xFFFFFFFF sentinels around results.
9. Worker thread (f_00160B70) started; requires real threads (fibers in runtime/threads.c).
10. VSync interrupt at 59.94 Hz, serviced at loop heads.
11. CD streaming starts (lbn ~959782, 128-sector chunks, double-buffered at 0x01BE0400 /
    0x01C20400). **Now at the intro movie: sceMpeg drives the IPU (0x10002000) and waits.**

## IOP services (runtime stand-ins)

| SID        | Service                     | Status                                  |
|------------|-----------------------------|-----------------------------------------|
| 0x80000592 | CD init                     | ok                                      |
| 0x80000593 | CD S-commands               | fn 3 disc type, fn 4 error; fn 34 TODO  |
| 0x80000595 | CD N-commands               | fn 1 read (from ISO), 5-8, 14 ready     |
| 0x8000059C | CD (callback?)              | zeros                                   |
| 0x80000003 | IOP heap                    | alloc/free                              |
| 0x80000006 | LOADFILE                    | version, module loads (logged)          |
| 0x80000100/101 | padman                  | zeros (no controller yet)               |
| 0x80000400 | mcserv (memory card)        | zeros                                   |
| 0x80000001 | fileio                      | zeros                                   |
| 0x00123456/57 | 989snd                   | sentinel replies, silent                |

## Bugs found and fixed

- RAM fast-path mask `0xCFFFFFFF` also matched 0x1xxxxxxx, so all hardware registers
  (DMA, IPU, VIF, INTC, VU memory) silently read/wrote RAM. Covered by a test now.

## Blockers

- (resolved) Windows Smart App Control blocked fresh builds; user turned it off.

## Open questions

- [ ] Which IOP modules does the game load (from .irx and IOPRP300.IMG), and what RPCs does it use?
- [ ] How does the game locate its data on disc (sector table in the executable?)

## Matched functions (decomp work, paused)

| Function | Address | Source file | Date |
|----------|---------|-------------|------|
