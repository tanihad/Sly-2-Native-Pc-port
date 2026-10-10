# Audio: findings and implementation plan

Status (2026-10-10): research only, nothing implemented. `runtime/snd.c` still answers the 989snd
RPCs with empty replies, so the game is silent.

**Recommendation: approach B.** Run the game's own IOP sound modules (libsd, 989snd, spiop) on an
emulated IOP: an R3000A interpreter, C stand-ins for the IOP kernel libraries they import, an SPU2
emulator and WASAPI output. The two RPC servers 0x123456 and 0x123457 are then served by the real
989snd. Estimated effort is 4 to 5 weeks of focused work, at medium-low risk. Approach A, a
from-scratch C reimplementation of 989snd plus Sucker Punch's spiop, needs the same synthesizer and
also about 35k instructions of undocumented code reverse-engineered. That is 8 to 10 weeks, at high
risk. See section 3.

All addresses below were checked against the shipped binaries, using `scripts/irx_tool.py` (IOP side)
and `python -m recomp disasm` (EE side). Module addresses are module-relative, because IRX files are
linked at 0 and relocated by the loader. Statements marked *(inferred)* come from reading code and
have not been confirmed by running anything.

---

## 1. IOP modules

### 1.1 Modules embedded in the executable (`.irx`, 0x00472200-0x004E5E40)

These are nine relocatable IRX files stored back to back, loaded in this order by the EE through
LOADFILE (fn 6, "load from IOP buffer").

| # | EE address | Size | iopmod name | Ver | text / data / bss | Role | Under plan B |
|---|-----------|------|-------------|-----|-------------------|------|--------------|
| 1 | 0x00472200 | 6,641 | sio2man | 3.1 | 0xF40 / 0x80 / 0x30 | SIO2 driver | keep C stand-in |
| 2 | 0x00473C40 | 44,869 | padman | 4.16 | 0x7D20 / 0x320 / 0x1BE0 | pads | keep `pad.c` |
| 3 | 0x0047EC00 | 95,909 | mcman | 2.38 | 0x11B90 / 0x3D0 / 0x15344 | memory card | keep `mc.c` |
| 4 | 0x00496300 | 7,417 | mcserv | 2.16 | 0x1260 / 0x60 / 0x24A0 | memory card RPC | keep `mc.c` |
| 5 | 0x00498040 | 28,661 | Sound_Device_Library (libsd) | 3.3 | 0x4890 / 0xAC0 / 0x660 | SPU2 driver | **run** |
| 6 | 0x0049F080 | 34,985 | USB_driver (usbd) | 2.4 | 0x62D0 / 0x330 / 0x10 | USB host | not run |
| 7 | 0x004A7980 | 49,693 | lgAud | 1.10 | 0x8E60 / 0x410 / 0x1150 | Logitech USB audio (headset) | C stub library |
| 8 | 0x004B3C00 | 178,136 | 989snd_Library | 3.1 | 0x1C9C4 / 0x2B64 / 0x7960 | SCEA sound system | **run** |
| 9 | 0x004DF440 | 26,828 | spiop | 1.0 | 0x54B0 / 0x1E0 / 0x70 | Sucker Punch 989snd extension | **run** |

Instruction counts: libsd 4,644, 989snd 29,297, spiop 5,420, lgAud 9,112. None of the three sound
modules uses COP0, COP2 or `syscall`: all interrupt control goes through intrman. They need plain
MIPS I plus `lwl/lwr/swl/swr` (989snd uses 48 of each), `mult/div` and `break` (GCC's
divide-by-zero trap, never reached in normal operation).

989snd's banner gives the build:
`989snd (c)2000-2003 Sony Computer Entertainment America by Buzz Burrowes`, built Mar 22 2004. It
also prints **"NO MIDI VERSION! No MIDI, AME, or basic VAG sounds (banks and streams only)"**, so
this build has only the SFX block-sound engine and VAG streaming. lgAud's string is
`LGTGliblgaud 1.10.001, built on Mar 23 2004`.

### 1.2 IOPRP300.IMG

IOPRP300.IMG is a ROMDIR image (`RESET`, `ROMDIR`, `EXTINFO`, then modules). Its comment is
`20031022-190110,conffile,ioprp300.img`. It holds SDK 3.0 replacements for the IOP kernel modules,
which the EE installs with an IOP reboot (`rom0:UDNL cdrom0:\IOPRP300.IMG`):

| Entry | iopmod name | Ver | Exports |
|-------|-------------|-----|---------|
| SYSMEM | System_Memory_Manager | 2.3 | sysmem |
| LOADCORE | Module_Manager | 2.6 | loadcore |
| SIFCMD | IOP_SIF_rpc_interface | 2.8 | sifcmd |
| SIFMAN | IOP_SIF_manager | 2.3 | sifman |
| THREADMAN | Multi_Thread_Manager | 2.3 | thbase, thevent, thsemap, thmsgbx, thfpool, thvpool, thrdman |
| IOMAN | IO/File_Manager | 2.3 | ioman |
| MODLOAD | Moldule_File_loader (sic) | 2.9 | modload |
| FILEIO | FILEIO_service | 2.18 | (RPC server 0x80000001) |
| CDVDMAN | cdvd_driver | 2.37 | cdvdman |
| CDVDFSV | cdvd_ee_driver | 2.37 | cdvdfsv (CD RPC servers 0x8000059x) |
| LOADFILE | LoadModuleByEE | 2.2 | (RPC server 0x80000006) |
| TIMEMANI | Timer_Manager | 2.2 | timrman |
| ROMDRV | ROM_file_driver | 2.1 | romdrv |
| EESYNC | SyncEE | 2.2 | eesync |
| SYSCLIB | System_C_lib | 2.3 | sysclib, stdio |
| STDIO | Stdio | 2.3 | stdio |

The image does not contain intrman, dmacman, ssbusc, vblank, heaplib, excepman or secrman. Those
come from the console's BIOS ROM, which we do not have and must not use. Plan B does not run any of
these modules either. It implements the libraries in C (section 4.5). The image is still useful as
a reference for library versions and export-table sizes, and the existing RPC stand-ins
(`cdvd.c`, `iop_modules.c`) already replace FILEIO, CDVDFSV and LOADFILE.

### 1.3 Imports of the sound modules

Function numbers are taken from the import stubs (`jr $ra; addiu $zero,$zero,N`). Names follow
Sony's export order as listed in ps2sdk headers. Several numbers were checked against the code:
libsd 4 = sceSdInit, which registers interrupts 0x24, 0x28 and 9; libsd 13 = sceSdNote2Pitch, which
divides by 12; cdvdman 24 = sceCdReadClock, used by mcman; sysmem 14 = Kprintf, used by padman. The
timrman and thbase call sequences also match their documented signatures.

**libsd (Sound_Device_Library 3.3)**
- intrman: 4 RegisterIntrHandler, 5 ReleaseIntrHandler, 6 EnableIntr, 7 DisableIntr,
  17 CpuSuspendIntr, 18 CpuResumeIntr, 23 QueryIntrContext
- loadcore: 5 FlushDcache, 6 RegisterLibraryEntries
- sifman: 7 sceSifSetDma, 8 sceSifDmaStat
- sysclib: 17 bzero
- thevent: 4 CreateEventFlag, 5 DeleteEventFlag, 6 SetEventFlag, 7 iSetEventFlag, 8 ClearEventFlag,
  9 iClearEventFlag, 10 WaitEventFlag
- Exports `libsd` v1.5 with 34 entries (0..33).

**989snd_Library 3.1**
- sysmem: 4 AllocSysMemory, 5 FreeSysMemory, 7 QueryMaxFreeMemSize
- intrman: 17 CpuSuspendIntr, 18 CpuResumeIntr
- ioman: 4 open, 5 close, 6 read, 8 lseek (only for loads by file name, which Sly 2 does not use)
- loadcore: 6 RegisterLibraryEntries
- sifcmd: 14 sceSifInitRpc, 17 sceSifRegisterRpc, 19 sceSifSetRpcQueue, 22 sceSifRpcLoop,
  23 sceSifGetOtherData
- sifman: 5 sceSifInit, 8 sceSifDmaStat, 29 sceSifCheckInit, 32 sceSifSetDmaIntr
- stdio: 4 printf
- sysclib: 12 memcpy, 13 memmove, 14 memset, 29 strncmp, 36 strtol
- thbase: 4 CreateThread, 5 DeleteThread, 6 StartThread, 10 TerminateThread, 20 GetThreadId,
  24 SleepThread, 25 WakeupThread, 26 iWakeupThread, 33 DelayThread, 39 USec2SysClock
- thsemap: 4 CreateSema, 5 DeleteSema, 6 SignalSema, 7 iSignalSema, 8 WaitSema, 9 PollSema
- timrman: 4 AllocHardTimer, 6 FreeHardTimer, 20 SetTimerHandler, 22 SetupHardTimer,
  23 StartHardTimer, 24 StopHardTimer
- libsd: 4 sceSdInit, 5 SetParam, 6 GetParam, 7 SetSwitch, 8 GetSwitch, 9 SetAddr, 10 GetAddr,
  11 SetCoreAttr, 13 Note2Pitch, 17 VoiceTrans, 18 BlockTrans, 19 VoiceTransStatus,
  20 BlockTransStatus, 23 SetEffectAttr, 25 ClearEffectWorkArea, 26 SetTransIntrHandler.
  It does not import SetSpu2IntrHandler or SetIRQCallback, so 989snd never uses SPU2 IRQs.
- cdvdman: 6 sceCdRead, 8 sceCdGetError, 10 sceCdSearchFile, 11 sceCdSync, 15 sceCdStop,
  28 sceCdStatus, 37 sceCdCallback, 39 sceCdBreak
- Exports `snd989` v1.8 with 130 entries.

**spiop 1.0**
- cdvdman: 6 sceCdRead, 11 sceCdSync, 14 sceCdTrayReq
- snd989: 25, 27, 28, 29, 32, 48, 49, 93, 99, 119, 121, 126. 93 registers an extern-proc handler,
  126 plays a sound and 121 is called three times at init with voice ranges *(inferred)*.
- lgaud: 4, 5, 6, 7, 11, 13, 14, 15, 16, 18, 19, 22
- sysmem: 4 AllocSysMemory, 5 FreeSysMemory
- sysclib: 5 longjmp (error path only), 14 memset, 22 strcmp, 40 wmemcopy
- thbase: 4 CreateThread, 5 DeleteThread, 6 StartThread, 14 ChangeThreadPriority, 33 DelayThread
- thsemap: 4 CreateSema, 5 DeleteSema, 6 SignalSema, 8 WaitSema
- No exports and no RPC server of its own. The EE reaches it through 989snd's extern-call commands
  (section 2.3).

**lgAud 1.10** (needed only if run; plan B replaces it with a stub)
- usbd: 4..11, 13, 18; modload: 17, 18; sysmem 4, 5; intrman 17, 18; loadcore 6, 7;
  sifcmd 14, 17, 19, 22, 24, 25; sifman 7, 8; stdio 4; sysclib 7, 12, 14, 27, 36;
  thbase 4, 5, 6, 9, 10, 16, 20, 33; thevent 4, 5, 6, 8, 10, 11; thsemap 4, 5, 6, 8.
- Exports `lgaud` v1.8 with 23 entries. Its strings suggest it only starts an RPC server when given
  `maxstream=` *(inferred)*. The game never binds an lgAud server: none appears in the bound-server
  list in `progress.md`.

#### Union: what the emulated IOP kernel must provide (libsd + 989snd + spiop)

| Library | Functions | Notes |
|---------|-----------|-------|
| loadcore | 5 FlushDcache, 6 RegisterLibraryEntries | FlushDcache does nothing; Register records exports for linking |
| sysmem | 4 AllocSysMemory, 5 FreeSysMemory, 7 QueryMaxFreeMemSize | real allocator over 2 MB; 989snd prints a warning above 2 MB free |
| intrman | 4 RegisterIntrHandler, 5 ReleaseIntrHandler, 6 EnableIntr, 7 DisableIntr, 17 CpuSuspendIntr, 18 CpuResumeIntr, 23 QueryIntrContext | libsd registers 0x24 (DMA ch4), 0x28 (DMA ch7), 9 (SPU2) |
| thbase | 4 CreateThread, 5 DeleteThread, 6 StartThread, 10 TerminateThread, 14 ChangeThreadPriority, 20 GetThreadId, 24 SleepThread, 25 WakeupThread, 26 iWakeupThread, 33 DelayThread, 39 USec2SysClock | priority scheduler; thread param `{attr, option, entry, stacksize, priority}` |
| thsemap | 4 CreateSema, 5 DeleteSema, 6 SignalSema, 7 iSignalSema, 8 WaitSema, 9 PollSema | sema param `{attr, option, initial, max}` |
| thevent | 4 CreateEventFlag, 5 DeleteEventFlag, 6 SetEventFlag, 7 iSetEventFlag, 8 ClearEventFlag, 9 iClearEventFlag, 10 WaitEventFlag | used by libsd for transfer completion |
| timrman | 4 AllocHardTimer, 6 FreeHardTimer, 20 SetTimerHandler, 22 SetupHardTimer, 23 StartHardTimer, 24 StopHardTimer | one 240 Hz timer (section 2.5) |
| sifman | 5 sceSifInit, 7 sceSifSetDma, 8 sceSifDmaStat, 29 sceSifCheckInit, 32 sceSifSetDmaIntr | IOP-to-EE DMA becomes a memcpy into EE RAM plus a deferred callback |
| sifcmd | 14 sceSifInitRpc, 17 sceSifRegisterRpc, 19 sceSifSetRpcQueue, 22 sceSifRpcLoop, 23 sceSifGetOtherData | server side of RPC, tied into `sif.c` |
| sysclib | 5 longjmp, 12 memcpy, 13 memmove, 14 memset, 17 bzero, 22 strcmp, 29 strncmp, 36 strtol, 40 wmemcopy | written in C against guest memory |
| stdio | 4 printf | guest varargs; output to stderr as `[iop]` |
| ioman | 4 open, 5 close, 6 read, 8 lseek | open returns -1 (no host files needed) |
| cdvdman | 6 sceCdRead, 8 sceCdGetError, 10 sceCdSearchFile, 11 sceCdSync, 14 sceCdTrayReq, 15 sceCdStop, 28 sceCdStatus, 37 sceCdCallback, 39 sceCdBreak | reads from the ISO into IOP RAM; completion callback is deferred |
| lgaud | 4, 5, 6, 7, 11, 13, 14, 15, 16, 18, 19, 22 | stub that reports "no device" |
| libsd, snd989 | (see above) | supplied by the real modules, linked guest to guest |

That is 75 kernel functions plus 12 lgaud stubs. Imports of the other modules (not needed for
sound) are listed in the appendix.

### 1.4 Hardware libsd touches directly

libsd drives the hardware itself rather than going through dmacman:
- SPU2 registers 0x1F900000-0x1F9007FF. These are the per-core voice registers (+0x000), voice
  addresses SSA/LSAX/NAX (+0x1C0), core registers PMON/NON/VMIX*/MMIX (+0x180), ATTR (+0x19A),
  IRQA (+0x19C), KON/KOFF (+0x1A0/+0x1A4), TSA (+0x1A8), data port (+0x1AC), ADMAS (+0x1B0),
  effect addresses (+0x2E0..), ENDX/STATX (+0x340/+0x344), with core 1 at +0x400. Also the shared
  volume block 0x760-0x7AF (MVOL, EVOL, AVOL, BVOL and the reverb volume registers) and SPDIF
  0x7C0-0x7CA.
- IOP DMA channel 4 (0x1F8010C0 MADR/BCR/CHCR) for core 0 and channel 7 (0x1F801500) for core 1,
  plus DPCR 0x1F8010F0 and DPCR2 0x1F801570.
- SSBUS delay and config registers 0x1F801014, 0x1F801404, 0x1F80140C, 0x1F801414 (stored and
  ignored).
- Interrupts through intrman: 0x24 (DMA ch4 done), 0x28 (DMA ch7 done), 9 (SPU2 IRQ).

---

## 2. The EE-to-IOP sound protocol

### 2.1 EE side (989snd EE library, linked into the game)

Assert strings name the source file `c:\vss\p3/989snd.cpp` and these functions: snd_BankLoad,
snd_BankLoadByLoc, snd_BankLoadFromEE, snd_BankLoadFromIOP, snd_BankLoadFromIOPPartialEx,
snd_MMDLoad, snd_MMDLoadByLoc and snd_SendIOPCommandNoWait.

| EE address | What |
|------------|------|
| 0x002319E8 | snd_StartSoundSystem: binds 0x123456 (client struct 0x00443800) and 0x123457 (client 0x00448440), then sends command 8 |
| 0x002325E0 | send-and-wait(cmd, size, data): copies args to 0x00443880, then `sceSifCallRpc(0x443800, cmd, NOWAIT, ..., recv 0x00443840, 12 bytes)` and polls `sceSifCheckStatRpc` |
| 0x00232838 | snd_SendIOPCommandNoWait(cmd, size, data, callback, user): appends to the current batch buffer |
| 0x00232BF8 | flush a batch: `sceSifCallRpc(0x443800, fno 77, NOWAIT, batch, 0x1000-free, results, count*4+8)`; double-buffered |
| 0x00231C48 | poll and process returned results (calls each command's callback with its result word) |
| 0x00231FB0 | snd_BankLoadByLoc(sector, offset): `sceSifCallRpc(0x448440, fno 3, ...)`, 8 bytes in, 4 bytes out (bank handle) |
| 0x00233048 / 0x00233088 / 0x002330D0 | extern call, command 76 (28 bytes = id, proc, 5 args), waited or batched with callback |
| 0x00233130 | extern call with data, command 104 (at most 500 bytes) |

**Batch format** (fn 77, handler at 989snd 0x1880): `int count`, then for each command
`s16 cmd, s16 size, data[size rounded up to 4]`. The IOP runs `table[cmd](data)` for each one. The
reply buffer (`[0x1D63C]`, 0x420 bytes) is `0xFFFFFFFF, result[count], 0xFFFFFFFF`. The EE checks
both sentinels before trusting the results, which is why `snd.c` writes them.

### 2.2 IOP side (989snd)

- Module entry (0x1C14) parses the args `tick_priority=`, `rpc_priority=`, `stream_priority=`,
  `do_rpc=` and `limit_2meg`. It allocates a 4 KB RPC receive buffer (filled with 0xAB) and a 0x420
  result buffer, then starts two RPC threads:
  - 0x19E4: `sceSifRegisterRpc(0x123456, func 0x197C, buff 4 KB)`. The handler indexes a table of
    **107 command handlers** at 0x1D648.
  - 0x1BA0: `sceSifRegisterRpc(0x123457, func 0x1A68, buff 0x1F578)`, the "Loading RPC". It handles
    fno 2, 3, 4, 5, 0x57, 0x59 and 0x6A and returns one word at 0x1F678.
- snd_StartSoundSystem (export 4, command 0) calls sceSdInit and sets up the voices, cores and
  reverb. It creates the **sound tick** thread (0x78D8) and a timrman hard timer
  (`AllocHardTimer(SYSCLOCK, 32, 1)`) with compare = `USec2SysClock(1 s) / 240`. That is a
  **240 Hz** tick: the handler at 0x790C calls `iWakeupThread(tick)` and returns the next compare.
- Four more threads are created at 0x1077C-0x10824. These are the streaming threads, which read the
  disc with `sceCdRead` and feed SPU2 RAM through `sceSdVoiceTrans` *(inferred)*.
- Extern procs: export 93 registers a handler table `{4CC id, count, procs[]}`. Commands 76 and 104
  look the id up (export 95) and call `procs[n]`. spiop registers **'SUKR'** (0x524B5553) with 14
  procs, and the game calls procs 1..7 from 0x001FC630-0x001FDBF0.

The command table maps each command to a small handler that unpacks the arguments and calls one
export. `scripts/irx_tool.py table 989snd 1D648 -n 107` lists the handlers, and `dis` on a handler
shows which export it calls. Commands used by the game's EE code (found from the call sites of the
two senders):

| Cmd | EE send | Bytes | IOP handler | Meaning (from code) |
|-----|---------|-------|-------------|---------------------|
| 0 | wait | 8 | export 4 | start sound system (sent via 0x231C0C) |
| 3 (on 0x123457) | loader | 8 | export 10 | **snd_BankLoadByLoc(sector, offset)**: how Sly 2 loads every bank |
| 6 | batch | 4 | export 11 | unload bank *(inferred)* |
| 8 | batch | 0 | export 15 | |
| 9 | batch | 8 | export 16 | SetParam: master/group volume *(inferred)* |
| 10 | wait | 4 | export 17 | getter |
| 11 | batch | 4 | export 18 | |
| 19, 20, 21 | batch | 4 | exports 27, 28, 29 | per-sound-handle operations (pause, continue, stop *(inferred)*) |
| 22, 23 | batch | 4 | exports 67, 68 | |
| 24 | batch | 0 | export 31 | stop all *(inferred)* |
| 25 | batch | 4 | export 25 | |
| 26 | wait | 8 | export 26 | |
| 33 | batch | 24 | export 126 | **play sound** (6 words: bank, sound, vol, pan, pitch mod, pitch bend *(inferred)*); returns a handle |
| 34 | batch | 16 | export 40 | |
| 54 | wait | 4 | export 61 | |
| 59, 60, 61, 62, 64, 90, 91 | wait | 24/0/0/20/0/8/0 | internal 0xB284, 0xB6F4, 0xB7D8, 0xB920, 0xC1DC, 0xB5C0, 0xB610 | **movie sound** (60 is the "closing movie sound" step of shutdown); uses sceSdBlockTrans (ADMA) |
| 76 | wait/batch | 28 | 0x18C | extern call, 'SUKR' procs 2-7 |
| 92 | batch | 48 | export 114 | |
| 104 | batch | variable | 0x250 | extern call with payload, 'SUKR' proc 1 |
| 77 | (envelope) | up to 4 KB | 0x1880 | batch |

The loader server's other functions are 2 = snd_BankLoad (file name, export 9),
4/5 = snd_MMDLoad/ByLoc (exports 12/13), 0x57 = snd_BankLoadFromEE (export 111, uses
sceSifGetOtherData), 0x59 = snd_BankLoadFromIOP (export 113) and 0x6A = ...FromIOPPartialEx
(export 129). Its source modes are at 0x7BDC (ioman file), 0x7C34 (disc sector, 2048-byte
granularity, read through export 98 into sceCdRead) and 0x7C74 (EE memory).

### 2.3 Where the sound data comes from

- **Sound banks** are read from the disc by the IOP. The game calls snd_BankLoadByLoc(sector, 0)
  (call site 0x001FD670; the sector is computed from a file-table entry at 0x001FD620-0x001FD650,
  with an XOR and `+ 0xCA0`, *inferred*). 989snd then reads the bank with its own `sceCdRead`
  (export 98: 2048-byte sector cache, read mode trycount 100), keeps headers in IOP RAM and uploads
  samples to SPU2 RAM with `sceSdVoiceTrans`, which is IOP DMA ch4/7.
- **VAG streams** (`HANDLER_TYPE_VAG_STREAM`, "Stream Start Sector") are also read by 989snd's
  streaming threads with sceCdRead, double-buffered into SPU2 RAM and played by voices. There are
  no SPU2 IRQs (libsd's IRQ functions are not imported). 989snd most likely tracks progress by
  polling the voices' NAX with `sceSdGetAddr` *(inferred)*.
- **spiop** runs its own disc streaming (sceCdRead at 0x211C/0x28BC, then `wmemcopy`) and plays
  through snd989 exports, with optional output to a USB headset through lgaud. *(inferred: music or
  dialogue channels; the game drives up to 6 channels through 'SUKR' proc 6.)*
- **Movie audio** comes from the EE: the movie player sends commands 59-64/90/91. The audio data is
  DMA'd from the EE into IOP buffers, and libsd plays it with `sceSdBlockTrans` (SPU2 auto-DMA PCM
  input).

So the IOP needs disc access of its own. `cdvd.c` currently serves only the EE's
cdvdfsv RPCs.

### 2.4 989snd threads, memory and timing

- Threads: main RPC (stack 4 KB), loading RPC (2 KB), sound tick (4 KB, woken at 240 Hz), 4 stream
  threads, plus spiop's threads (created at module start and on demand).
- IOP memory: 989snd allocates with AllocSysMemory. If QueryMaxFreeMemSize reports more than 2 MB
  it assumes a dev kit, subtracts 6 MB and prints a warning, so the allocator must report the free
  space of a 2 MB machine.
- SPU2 RAM (2 MB) holds bank samples, stream buffers and the reverb work areas, all managed by
  989snd.

---

## 3. Approaches

Both approaches need a voice synthesizer that matches the SPU2: ADPCM decoding, pitch with
interpolation, ADSR, volumes, reverb, mixing and output. The difference is what drives it.

### A. Reimplement 989snd (and spiop) in C

Work involved:
1. A command layer for the ~30 commands the game sends (above). This part is easy.
2. The bank format (SBlk/"SBv2" block sounds with "grains") and the grain interpreter, which has
   about 30 grain types: tone, LFO, branch, registers, random, child sounds, markers and so on.
   989snd builds differ between games. The OpenGOAL project reimplemented 989snd's players for Jak
   and could serve as a format reference; check its license before reusing anything.
3. The voice allocator: priorities, groups (spiop sets the group voice ranges), voice stealing,
   ducking and pitch (Note2Pitch).
4. VAG streaming from disc sectors, and movie sound (PCM from the EE).
5. **spiop**: 5,420 instructions of undocumented Sucker Punch code (threads, disc streaming, 14
   extern procs, USB headset paths) that would have to be reverse-engineered with no reference.
6. The synthesizer: a simplified SPU2 with an approximated reverb.

Effort: about 8-10 weeks, mostly reverse-engineering 989snd's internals (29k instructions) and
spiop. **Risk is high.** Every feature has to be found and reproduced, and the only test is
"sounds right". Subtle differences (voice stealing, envelopes, music transitions, stream timing)
are likely and hard to diagnose. A also ties the port to this exact 989snd build.

### B. Run the real modules on an emulated IOP

Work involved: an R3000A interpreter (about 60 opcodes), an IRX loader and linker, C stand-ins for
the 75 kernel functions in 1.3 plus an lgaud stub, an SPU2 emulator, WASAPI output, and wiring into
`sif.c`/`cdvd.c`. Every piece is documented: IOP and SPU2 hardware on ps2tek and in the PS1 SPU
documentation (psx-spx, nocash), whose voice, ADPCM, ADSR and reverb algorithms the SPU2 extends;
library semantics in the ps2sdk headers. 989snd and spiop then behave exactly as on a PS2, including
everything not yet discovered.

Effort: about 4-5 weeks (breakdown in 4.8). **Risk is medium-low.** The main risks are IOP kernel
semantics (thread and interrupt ordering), SPU2 details (ADMA, reverb) and timing. All of them can
be tested in isolation, and 989snd's own printf diagnostics help.

| | A: reimplement 989snd | B: emulate the IOP |
|-|-----------------------|--------------------|
| Reverse engineering | 989snd (29k insns) + spiop (5.4k insns) + bank format | none (only confirming a few call conventions) |
| New emulation | simplified SPU2 synth | R3000A, IOP kernel functions, full SPU2, IOP DMA ch4/7 |
| Documentation | none for 989snd/spiop internals | ps2tek, psx-spx, ps2sdk headers |
| Fidelity | approximate | exact for the sound logic; SPU2 accuracy limited only by the emulator |
| Effort | 8-10 weeks | 4-5 weeks |
| Risk | high | medium-low |
| CPU cost | lowest | IOP interpreter about 1-2% of a core (240 Hz tick, mostly idle) plus SPU2 mixing |
| Reuse | none | the same IOP can later run other modules if a stand-in falls short |

**Recommendation: B.** A hybrid fallback stays open: if register-level libsd and DMA emulation
cause trouble, replace libsd with a C implementation of its 16 used `sceSd*` calls on top of the
SPU2 model, and keep running 989snd and spiop for real.

---

## 4. Implementation plan (approach B)

### 4.1 Components and files

| File (new) | Contents |
|------------|----------|
| `runtime/iop.h` | interface below |
| `runtime/iop_cpu.c` | R3000A interpreter and IOP memory map: 2 MB RAM (mirrored, kseg0/kseg1), 1 KB scratchpad 0x1F800000, I/O 0x1F801000-0x1F80FFFF (DMA ch4/ch7, DPCR/DICR, SSBUS), SPU2 0x1F900000-0x1F9007FF |
| `runtime/iop_load.c` | IRX parsing, relocation (R_MIPS_32/26/HI16/LO16), import linking, export registry (loadcore) |
| `runtime/iop_kernel.c` | threads and scheduler, semaphores, event flags, DelayThread, intrman, timrman, sysmem, sysclib, stdio, HLE dispatch, guest-callback trampolines |
| `runtime/iop_sif.c` | sifman/sifcmd server side, cdvdman (IOP side), ioman stub, lgaud stub |
| `runtime/spu2.c`, `spu2_reverb.c`, `spu2.h` | SPU2: registers, voices, mixing, reverb, DMA port, ADMA, IRQA |
| `runtime/audio_out.c`, `audio_out.h` | WASAPI on its own OS thread, lock-free SPSC ring buffer, WAV dump |
| `tests/unit/iop_cpu/`, `tests/unit/spu2/`, `tests/unit/iop_kernel/` | unit tests with `run.ps1`, per the hle-architecture rules |

Integrator-owned changes (described here, not done by the implementer):
- `ps2_runtime.c`: call `audio_init()` and `iop_init()` at startup, and `iop_service()` in
  `ps2_service_irq` next to `timer_service()`. The host timer already raises `PS2_IRQ_TICK` at
  about 1 kHz.
- `sif.c`: (1) server entries can be IOP servers (registered by guest `sceSifRegisterRpc`). Bind
  returns the guest's `sd` and `buff`, so the EE's argument DMA lands in 989snd's own buffer.
  (2) `handle_rpc_call` for an IOP server queues the request with `iop_rpc_request()` instead of
  calling `rpc_dispatch`. (3) A new `sif_rpc_reply(req, iop_reply_addr)` copies `recv_size` bytes
  from IOP RAM to the EE receive address and queues RPC_END; this lets replies arrive later.
  (4) RPC receive buffers for the C servers come from `iop_alloc()`. This also removes a latent
  overlap: today `RPC_BUF_BASE` 0x100000 + 32 x 16 KB runs into the IOPHEAP range starting at
  0x140000 after 16 servers.
- `iop_modules.c`: LOADFILE fn 6 calls `iop_load_module()` for modules the IOP runs (by iopmod
  name: Sound_Device_Library, 989snd_Library, spiop) and registers the lgaud stub when lgAud is
  loaded. Other modules stay "logged and acknowledged". IOPHEAP alloc/free go to the sysmem
  allocator.
- `cdvd.c`: factor out `int cdvd_read_host(uint32_t lbn, uint32_t count, void* dst)` (ISO to host
  buffer), shared by the EE path and the IOP's cdvdman.
- `snd.c`: kept only as a fallback (`PS2_AUDIO=0`, or automatically if the IOP faults).

### 4.2 Interfaces

```c
/* iop.h */
void     iop_init(void);                                  /* RAM map, kernel, SPU2 */
int      iop_load_module(uint32_t image, uint32_t size,   /* IRX already in IOP RAM (LOADFILE fn 6) */
                         const char* args, uint32_t args_len, int32_t* result);   /* -> module id or <0 */
uint32_t iop_alloc(uint32_t size);                        /* sysmem: also IOPHEAP and C-server buffers */
void     iop_free(uint32_t addr);
void     iop_rpc_request(const iop_rpc_req* r);           /* from sif.c: a call for an IOP server */
void     iop_service(void);                               /* advance IOP time to the audio clock */
/* provided by sif.c for the IOP */
void     sif_iop_server_registered(uint32_t sid, uint32_t sd, uint32_t buff);
void     sif_rpc_reply(const iop_rpc_req* r, uint32_t reply_iop_addr);
/* spu2.h */
void     spu2_access(uint32_t phys, void* data, unsigned size, int is_write);   /* 0x1F900000.. */
uint32_t spu2_dma_write(int core, const uint16_t* src, uint32_t halfwords);     /* ch4 = core 0, ch7 = core 1 */
void     spu2_render(int16_t* out_stereo, uint32_t frames);  /* advances SPU2 time; raises DMA/IRQ events */
/* audio_out.h */
void     audio_init(void);
uint32_t audio_queued_frames(void);
void     audio_push(const int16_t* stereo, uint32_t frames);
```

`iop_rpc_req` holds the RPC_CALL packet fields that RPC_END must echo (rec_id, pkt_addr, rpc_id,
client), plus fno, send_size, receive and recv_size. EE memory is reached through `ps2_fast()`,
because all SIF addresses are in RAM; no `ps2_ctx` is needed in IOP context.

### 4.3 Timing model

- **The audio sample clock drives IOP time.** The IOP clock is 36.864 MHz and the SPU2 produces
  one 48 kHz sample every 768 IOP cycles.
- `iop_service()` works out how many frames to produce. With an audio device, it fills the ring up
  to the target latency (about 2048 frames, 43 ms). Headless or with `PS2_AUDIO_WAV`, it uses
  elapsed real time (QPC). A fixed-step mode (800.8 frames per VBlank) gives reproducible test runs.
- It produces audio in blocks of 32-64 frames. For each block it advances IOP time and fires due
  events in time order: the 240 Hz hard timer, DelayThread wakeups, deferred CD, SIF-DMA and
  SPU2-DMA completions, and ADMA block requests. It then runs runnable IOP threads until all are
  blocked, with an instruction budget per slice that logs runaway loops. Finally `spu2_render()`
  produces the block.
- EE RPC calls run the IOP immediately (wake the server thread, run until idle) so replies are
  usually ready before the EE's next poll. Slow operations, like a bank load that waits for
  deferred CD completions, reply later through `sif_rpc_reply`. The EE uses NOWAIT plus
  `sceSifCheckStatRpc` polling, so late replies are fine. Its bind loop also retries until the
  server exists.
- If the EE stalls, IOP time stops with it and the audio thread plays silence for the gap. That is
  acceptable.

### 4.4 IOP memory and module loading

- 0x00000-0x1FFFF is reserved as the "kernel" area. It holds `IOP_CMD_BUF` (0x1F000, where the EE
  writes SIF commands), HLE trampolines, the interrupt stack and the idle thread. sysmem manages
  0x20000-0x1FFFFF with first-fit allocation and free.
- `iop_load_module`:
  1. Parse the IRX already copied into IOP RAM.
  2. AllocSysMemory text+data+bss, copy, zero bss, apply relocations.
  3. Link imports. A guest library (`libsd`, `snd989`) gets its stub rewritten to `j target; nop`,
     as loadcore does. An HLE library gets a trap opcode that the interpreter dispatches by index.
  4. Run the entry `_start(argc, argv)` (argv[0] = module name, then the LOADFILE args) in the
     loader context, then run the IOP until idle.
  5. Return the module id and the start result (0 = resident, 1 = no resident).
- Calls from HLE code into guest code (thread entry points, RPC server functions, interrupt and
  timer handlers, sceCdCallback, sceSifSetDmaIntr callbacks) set `$ra` to a trap address in the
  kernel area. The HLE continuation runs when the guest returns there, and reads `$v0`.

### 4.5 Kernel library behaviour (the parts that matter)

- **Threads**: priority-preemptive, where a lower number means higher priority. Each thread has
  its own register file. Thread states are ready, running, waiting (sema, event flag, sleep, delay,
  RPC queue), dormant and exited. StartThread puts `arg` in `$a0`. When the entry function returns
  the thread exits. Sleep/Wakeup keep a wakeup count. TerminateThread and DeleteThread are only
  used at shutdown.
- **Interrupts**: CpuSuspendIntr/CpuResumeIntr keep an interrupts-disabled flag per thread
  context, as Status.IEc does on hardware. Delivery happens only at HLE boundaries and between
  slices, and only when enabled. Handlers run on the interrupt stack and must not block (assert).
  QueryIntrContext returns 1 inside a handler. The `i*` variants (iSignalSema, iWakeupThread,
  iSetEventFlag) mark threads ready, and the scheduler switches after the handler.
- **timrman**: one 32-bit SYSCLOCK timer. SetTimerHandler(id, compare, handler, arg) calls the
  handler every `compare` cycles; the handler returns the next compare (0 stops it).
  `USec2SysClock` uses 36.864 MHz.
- **sifcmd/sifman**:
  - sceSifRegisterRpc reports `(sid, sd, buff)` to `sif.c`.
  - sceSifRpcLoop blocks until a request is queued. It then calls `func(fno, buff, size)` and hands
    the returned pointer to `sif_rpc_reply`.
  - sceSifGetOtherData is an immediate EE-to-IOP memcpy.
  - sceSifSetDma/SetDmaIntr copy IOP to EE immediately and call the callback, in interrupt context,
    at the next event point.
  - sceSifDmaStat returns <0 once a transfer is done.
- **cdvdman**:
  - sceCdRead copies from the ISO into IOP RAM and returns 1. Completion is deferred: start with
    "next block" and allow a size-based delay. Completion runs the sceCdCallback function with
    reason 1 (read).
  - sceCdSync(0) waits; sceCdSync(1) returns 0/1.
  - GetError returns 0. Status returns a "paused/ready" value; check what 989snd's "CD busy" test
    expects. TrayReq reports no tray change. Break and Stop are no-ops. SearchFile returns 0.
- **stdio printf**: format from the guest's `$a1..$a3` and the stack (`%d %i %u %x %X %c %s %p`,
  widths, `%8.8x`). Prefix each line with `[iop]`.
- **lgaud stub**: every call fails with "no device". Pick the exact return codes by reading
  spiop's callers (0xE34-0x14D4, 0x1880-0x1A44, 0x2598, 0x2660). Running the real lgAud on a stub
  usbd is possible but needs modload queries and usbd version checks, so the stub is simpler.

### 4.6 SPU2 checklist

- 2 MB SPU2 RAM, addressed in halfwords. Two cores of 24 voices.
- Voice:
  - ADPCM 16-byte blocks of 28 samples, with filter and shift and the flags loop-end, loop-repeat
    and loop-start (LSA set on loop-start).
  - Pitch is 4.12 fixed point, 0x1000 = 48 kHz, with psx-spx's 4-tap Gaussian interpolation.
  - ADSR (attack, decay, sustain, release; linear and exponential), ENVX readback, fixed and sweep
    volumes, noise (NON), pitch modulation (PMON).
  - KON/KOFF, ENDX, NAX readback. NAX must advance in step with rendering, because 989snd's
    streaming polls it.
- Core mixing:
  - Voice dry and wet sends (VMIXL/R, VMIXEL/ER).
  - MMIX input routing: core 0 output into core 1, and ADMA "sound data input" to each core.
  - Reverb with per-core ESA/EEA and work area, using the PS1-style algorithm with SPU2 register
    layout, plus master, effect and input volumes.
  - Core 1 output goes to the DAC.
- Transfers:
  - TSA plus the data port.
  - DMA ch4/ch7 in manual block mode, writing to SPU2 RAM at TSA. Completion raises interrupt
    0x24/0x28 after a short delay, not instantly.
  - ADMA (ADMAS bit plus DMA): 512-byte blocks fill the core's input double buffer (L and R
    halves). Each consumed half requests the next block. Exact buffer addresses and block layout
    come from ps2tek.
  - STATX/ATTR busy bits must report idle promptly, because libsd busy-waits on them.
- IRQA match on voice reads and DMA, raising interrupt 9. Implement it, although 989snd doesn't
  use it.
- Debug aids:
  - `PS2_LOG_SPU2=1` logs KON (voice, SSA, pitch, ADSR), KOFF, DMA and ADMA.
  - `PS2_SPU2_DUMP=<file>` writes SPU2 RAM at exit.

### 4.7 Audio output

`audio_out.c` creates a WASAPI shared-mode, event-driven stream on its own thread (like
`window.c`): 48 kHz, 2 channels, 16-bit, with
`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY`, so Windows resamples
to the device rate. It needs mingw-w64's `mmdeviceapi.h`/`audioclient.h` with COBJMACROS and links
`ole32`. Underruns play silence. `PS2_AUDIO_WAV=<file>` writes everything rendered to a WAV for
headless runs. `PS2_AUDIO_TEST=1` plays a 440 Hz tone without the IOP, to check the output path
alone.

### 4.8 Order of work, milestones and tests

| Step | Work | Milestone and test | Days |
|------|------|--------------------|------|
| 1 | `iop_cpu.c` interpreter and memory map | Unit tests with hand-encoded instruction sequences (encoders in the style of `tests/make_test_elf.py`, checked against rabbitizer): ALU, branches and delay slots, load delay, `lwl/lwr/swl/swr`, `mult/div` edge cases (divide by zero, INT_MIN/-1), sign extension | 2 |
| 2 | `iop_load.c` loader and linker | Load all 9 modules from the ELF's `.irx` at a non-zero base. Check every R_MIPS_32 word moved by the base and every HI16/LO16 pair gives the original address + base. Check the import list matches section 1.3, and that each module's unresolved imports are exactly the libraries we don't provide | 1 |
| 3 | Kernel core: HLE dispatch, sysmem, loadcore, sysclib, stdio, thread/sema/event/sleep/delay, intrman, timrman, trampolines | `tests/unit/iop_kernel/`: small guest programs check priority preemption, WaitSema/SignalSema order, DelayThread wake time and a timer handler firing at the expected rate | 4 |
| 4 | Standalone **IOP smoke test** (`tests/unit/iop_smoke/`, no EE) | Load libsd + 989snd + spiop from the ELF and start them. Expect: the 989snd banner on stderr, servers 0x123456/0x123457 registered, 240 tick handler calls per emulated second, 'SUKR' registered, no unimplemented-HLE log. Send a fake batch with 0 commands and expect the reply `FFFFFFFF FFFFFFFF` | 1 |
| 5 | `iop_sif.c` (sifman, sifcmd, cdvdman, ioman, lgaud stub) plus the integrator changes to `sif.c`, `iop_modules.c` and `cdvd.c` | The game boots exactly as before. `PS2_LOG_SIF=1` shows 989snd replies coming from the real module. Bank loads (`snd_BankLoadByLoc`) return non-zero handles and `[iop]` shows no "989snd Error" | 3 |
| 6 | SPU2 registers, DMA ch4/7, transfer port | Unit tests: a DMA of N halfwords lands at TSA and raises its interrupt once. In the game, dump SPU2 RAM after the first bank load and check it holds ADPCM blocks (valid filter/shift bytes, loop-end flags) where 989snd uploaded them | 2 |
| 7 | Voices and mixing (no reverb yet), `audio_out.c` with WAV dump | Unit tests: ADPCM decode against a reference decoder written from psx-spx's formulas; ADSR reaches its documented levels at the documented sample counts; a sine encoded to ADPCM plays at the right frequency for pitch 0x1000/0x800 (zero crossings); loop flags and ENDX. Game: `PS2_AUDIO_WAV` capture of boot, intro and menu is not silent; listen to it | 5 |
| 8 | WASAPI output, latency and pacing | `PS2_AUDIO_TEST=1` tone. Game sound plays live without crackle at 43 ms latency; ring fill logged once per second | 1 |
| 9 | ADMA (movie sound), reverb, IRQA, volume/pan details | Intro movie has audio. Reverb impulse response is bounded and decays. Compare levels and panning by ear and against WAV captures | 4 |
| 10 | Robustness and cleanup | A guest fault logs the thread, PC and registers and falls back to `snd.c` replies, so the game keeps running. Interpreter and mixer cost measured (target under 5% of a core) | 2 |

That totals about 25 working days.

### 4.9 Risks and mitigations

- **Kernel semantics mismatches** such as return codes or wake order. Follow the ps2sdk header
  documentation, log every HLE call with `PS2_LOG_IOP=1`, and make unimplemented functions loud.
- **Busy-wait loops on SPU2/DMA status.** The per-slice instruction budget plus a "spinning at PC"
  log shows which register needs work.
- **Timing-sensitive streaming.** IOP time and SPU2 time are the same clock by construction, so
  NAX polling sees consistent positions. Deferred CD completion avoids reads that complete
  impossibly fast.
- **ADMA details.** These are the least documented part. The hybrid fallback is to have the
  movie-sound path feed PCM to the mixer directly.
- **Licensing.** Implement from ps2tek, psx-spx and the ps2sdk headers. Do not copy code from
  PCSX2/SPU2-X or other GPL emulators.

---

## 5. Tool: `scripts/irx_tool.py`

```
python scripts/irx_tool.py list                    # modules in the ELF + IOPRP300.IMG
python scripts/irx_tool.py imports 989snd spiop    # import tables with names
python scripts/irx_tool.py exports sound           # export table (libsd)
python scripts/irx_tool.py dis 989snd e10 -n 40    # disassemble export 10 (snd_BankLoadByLoc)
python scripts/irx_tool.py dis 989snd 1880 -n 60   # batch handler
python scripts/irx_tool.py table 989snd 1D648 -n 107   # RPC command table
python scripts/irx_tool.py calls 989snd cdvdman    # call sites of a library's imports
python scripts/irx_tool.py extract build/irx       # write the .irx files (never commit them)
```

It reads `orig/` from the main checkout through `scripts/paths.py`.

---

## Appendix: imports of the other modules (not needed for sound)

- **sio2man 3.1**: loadcore 6; intrman 4, 5, 6, 7, 17, 18; stdio 4; dmacman 28, 32, 33, 34, 35;
  thbase 4, 6, 20; thevent 4, 6, 7, 8, 10; thsemap 4, 6, 8, 11; sysclib 8, 29, 36.
- **padman 4.16**: sysmem 14 (Kprintf); loadcore 6; intrman 17, 18; stdio 4;
  sio2man 11, 46, 51, 52, 57, 58, 60; thbase 4, 5, 6, 8, 20, 22, 23; thevent 4, 5, 6, 7, 8, 10;
  thsemap 4, 5, 7, 8; sifman 5, 7, 8, 29, 32; sifcmd 14, 17, 19, 22, 28; vblank 8, 9;
  sysclib 8, 12, 29, 36.
- **mcman 2.38**: loadcore 6, 7; intrman 17, 18; sio2man 24, 25, 26, 57, 59;
  sysclib 12, 14, 20, 22, 23, 27, 29, 30; stdio 4; thbase 33, 35, 39; thsemap 4, 5, 6, 7, 8;
  modload 13; ioman 20, 21 (AddDrv/DelDrv); secrman 4, 5, 6; cdvdman 24 (sceCdReadClock).
- **mcserv 2.16**: loadcore 6, 7; intrman 17, 18; sysclib 12, 14, 29, 36; stdio 4;
  sifman 5, 7, 8, 29; sifcmd 14, 17, 19, 22, 23, 24, 25; thbase 4, 5, 6, 10, 14, 20, 22, 33;
  thsemap 6, 8; mcman 6-16, 21-24, 26, 36-39, 42, 45, 53.
- **USB_driver 2.4**: sysmem 4, 5; loadcore 6, 7; intrman 4, 5, 6, 7, 17, 18; stdio 4;
  thbase 4, 5, 6, 10, 14, 33; thevent 4, 5, 6, 7, 10; thsemap 4, 5, 6, 8; sysclib 16, 17.

## Open questions

- [ ] The exact meaning of the less common 989snd commands the game uses (8, 10, 11, 22, 23, 25,
      26, 34, 54, 92). This is not needed for plan B.
- [ ] What spiop's 'SUKR' procs do (music or dialogue streaming?). Not needed for plan B. Under B,
      run the game and log the proc numbers and arguments.
- [ ] The value `sceCdStatus` must return so that the EE's "snd_BankLoadByLoc: CD BUSY!" check
      passes (989snd command 101 forwards sceCdStatus to the EE).
- [ ] The module arguments the EE passes to 989snd and spiop through LOADFILE. Log them in
      `iop_modules.c`.
