# Controller protocol: libpad, padman and the DualShock 2

How Sly 2 talks to its controller, as read out of the game's own binaries. `runtime/pad.c`
implements the IOP side (padman) and the controller at a high level, following this.

Sources: the EE code of `SCUS_973.16` (Sony libpad 4.0, statically linked) and the `padman.irx`
module embedded in the same executable (IRX at file offset 0x1DEDC0, module "padman", version
0x0410). Addresses below are EE virtual addresses, or padman `.text` offsets (module linked at 0)
written `padman+0x....`. ps2sdk's libpad/padman sources were used only to guess names; every layout
here was checked against the game's code.

## Who does what

```
game (0x1871C0 UpdateJoy) --libpad--> SIF RPC 0x80000100 --> padman (IOP) --SIO2--> DualShock 2
       ^                                                         |
       +---- 2 x 128-byte buffer in EE RAM <--- SIF DMA every frame
```

- libpad never reads the controller itself. padman polls it once per frame and DMAs a 128-byte
  record per open port/slot to the EE buffer given to `scePadPortOpen`.
- Requests (mode changes, actuator setup, pressure mode) go over the RPC and complete
  asynchronously; libpad tracks them through the "request state" in the record.
- Vibration values do not use the RPC: libpad DMAs a small block straight into padman's memory.

## EE side: libpad in the game

| Address    | Function                 | Notes |
|------------|--------------------------|-------|
| 0x242D08   | scePadInit(mode)         | binds 0x80000100 (client 0x450580) and 0x80000101 (client 0x4505A8), checks the version, sends Init |
| 0x242E48   | SIF command 0x80000019 handler | calls an optional user callback (0x450844, never set by Sly 2), then `ei` |
| 0x242E90   | (Init)                   | clears the slot table, RPC command 0x10, registers the 0x80000019 handler |
| 0x242F70   | (actuator block DMA)     | see "Actuators" |
| 0x243088   | scePadPortOpen(port, slot, addr) | buffer must be 64-byte aligned; RPC command 0x01 |
| 0x243270   | (get record)             | SyncDCache, returns the copy with the larger frame counter |
| 0x2432D0   | scePadRead               | copies `length` (record +0x60) bytes from record +0x00 |
| 0x243388   | scePadGetState           | record +0x70, but 5 (EXECCMD) if it is 6 and +0x71 is 2 (BUSY) |
| 0x243458   | scePadSetReqState        | writes record +0x71 of the newest copy |
| 0x243500   | scePadGetReqState        | record +0x71 |
| 0x243558   | scePadInfoAct            | computed from the record |
| 0x243680   | scePadInfoMode           | computed from the record |
| 0x2437B8   | scePadSetMainMode        | RPC command 0x06; on success sets the request state to BUSY |
| 0x243870   | scePadSetActDirect       | DMA, not RPC |
| 0x243930   | scePadSetActAlign        | RPC command 0x08 |
| 0x243A08   | scePadGetButtonMask      | record +0x79..+0x7C |
| 0x243AC0   | scePadSetButtonInfo      | RPC command 0x0A |
| 0x243B70   | scePadInfoPressMode      | `GetButtonMask == 0x3FFFF` |
| 0x243BD0   | scePadEnterPressMode     | `SetButtonInfo(0xFFF)` (no ExitPressMode is linked) |
| 0x243C40   | scePadGetModVersion      | RPC command 0x12 |

libpad's own state: RPC clients at 0x450580/0x4505A8, slot table at 0x4505D0 (2 ports x 4 slots
x 0x1C: +0x00 EE buffer, +0x04 actuator block, +0x08 padman's IOP block address, +0x0C DMA id,
+0x10 open flag), actuator blocks at 0x4506C0 + port*0x80 + slot*0x20, RPC buffer at 0x4507C0.

`scePadInit` fails (returns 0, prints "libpad: Module version mismatch") unless the module
version's major byte is 4 — the reason the stub's zero replies stopped the pad from working.

## RPC

Server 0x80000100, always function number 1 (padman ignores it), 128 bytes sent and received in
the same EE buffer. Word 0 is the command; padman answers in place, so the reply is the request
with result words filled in. Server 0x80000101 ("extended service") only prints
"this service is not support" and echoes the buffer.

| Cmd  | Name          | Arguments (byte offsets)              | Result at | padman |
|------|---------------|---------------------------------------|-----------|--------|
| 0x01 | PortOpen      | +4 port, +8 slot, +0x10 EE buffer     | +0x0C (1 ok, 0 bad port/slot or already open); +0x14 IOP address of the actuator blocks | +0x3778 |
| 0x03 | InfoAct       | +0x0C actuator, +0x10 term            | +0x14     | +0x3F04 |
| 0x04 | InfoComb      | +0x0C index, +0x10 term (-1..2)       | +0x14     | +0x4004 |
| 0x05 | InfoMode      | +0x0C term, +0x10 offset              | +0x14     | +0x40F0 |
| 0x06 | SetMainMode   | +0x0C mode offset, +0x10 lock (3 = lock) | +0x14  | +0x42E8 |
| 0x07 | SetActDirect  | +0x0C six bytes                       | +0x14     | +0x4228 |
| 0x08 | SetActAlign   | +0x0C six bytes                       | +0x14     | +0x4398 |
| 0x09 | GetButtonMask |                                       | +0x0C     | +0x45F8 |
| 0x0A | SetButtonInfo | +0x0C info (12 pressure bits)         | +0x10     | +0x4514 |
| 0x0B | SetVrefParam  | +0x0C twelve bytes                    | +0x1C     | +0x444C |
| 0x0C | GetPortMax    |                                       | +0x0C (2) | +0x469C |
| 0x0D | GetSlotMax    | +4 port                               | +0x0C (1 without multitap) | +0x46A4 |
| 0x0E | PortClose     | +4 port, +8 slot, +0x10 wait          | +0x0C     | +0x3A24 |
| 0x0F | End           |                                       | +0x0C     | +0x3644 |
| 0x10 | Init          | +0x10 (libpad sends 0)                | +0x0C (1) | +0x3430 |
| 0x12 | GetModVersion |                                       | +0x0C (0x0410) | +0x46C4 |
| 0x13 | (nothing)     |                                       |           |        |
| 0x14 | SetDebug      | +4 flag                               | +0x08 (1) | +0x46D4 |
| 0x18 | SetNotify     | +4 flag: per-frame SIF command 0x80000019 | +0x08 (1) | +0x46E4 |

Anything else prints "invalid function code" and returns the buffer unchanged. Libpad 4.0 in this
game only sends 0x01, 0x06, 0x08, 0x0A, 0x10 and 0x12; the information queries are answered by
libpad from the record, with the same rules padman uses for 0x03-0x05:

- InfoMode / InfoAct / requests need record +0x72 ("task") == 1; InfoMode also needs no request
  in progress. InfoAct and all requests need +0x64 ("mode config") >= 2; GetButtonMask and
  SetVrefParam also need +0x66 ("model") >= 2, SetButtonInfo needs model bit 1.
- InfoMode term 1: current ID >> 4 (0 while 0xF3); term 2: mode table[current offset];
  term 3: current offset; term 4: table entry or (offset -1) the number of modes. Terms 2-4 return
  0 if mode config is 1. InfoAct actuator -1: count; otherwise byte term-1 of the actuator entry.
- Requests are refused while one is in progress (request state BUSY). On acceptance padman sets
  the request state to BUSY and queues the job (record +0x73).

## The record padman DMAs every frame

128 bytes, two copies at the PortOpen address (+0x00 and +0x80). Frame n goes to copy n & 1, so
the copy with the larger frame counter is the newest (libpad compares them signed). PortOpen
initialises both copies (frame 0, state 5, request state 2, data 0xFF).

| Offset | Size | Content |
|--------|------|---------|
| 0x00 | 32 | controller data (below); only rewritten while valid |
| 0x20 | 6+2 | actuator values last sent (SetActDirect) |
| 0x28 | 6+2 | actuator alignment; padman copies 8 bytes, so +0x2E holds the state |
| 0x30 | 16 | actuator table: 4 x (function, sub-function, size, current) |
| 0x40 | 16 | actuator combinations: 4 x 4 bytes |
| 0x50 | 8  | mode table: 4 x u16 (controller types, 4 = digital, 7 = DualShock) |
| 0x58 | 4  | frame counter |
| 0x5C | 4  | failed polls since the last detection |
| 0x60 | 4  | data length: 32 while the data is valid, else 0 |
| 0x64 | 1  | mode config: 0 unknown, 1 controller without configuration mode, 2 configurable |
| 0x65 | 1  | current ID (0x41 digital, 0x73 analog, 0x79 analog + pressure, 0xF3 config mode) |
| 0x66 | 1  | model, from query 0x45 (3 = DualShock 2) |
| 0x67 | 1  | data valid |
| 0x68..0x6B | 4 | number of modes, current mode offset, number of actuators, of combinations |
| 0x6C | 1  | button info has been set |
| 0x6D, 0x6E | 2 | requested main mode, lock |
| 0x6F | 1  | number of actuator values (6 once SetActDirect was used) |
| 0x70 | 1  | state: 0 DISCONN, 1 FINDPAD, 2 FINDCTP1, 5 EXECCMD, 6 STABLE, 7 ERROR |
| 0x71 | 1  | request state: 0 COMPLETE, 1 FAILED, 2 BUSY |
| 0x72 | 1  | task: 1 polling (information valid), 2 detecting, 3 closing, 4-7 executing a request |
| 0x73 | 1  | queued request (4 main mode, 5 alignment, 6 button info, 7 vref) |
| 0x74 | 1  | multitap slot adjustment flag |
| 0x75 | 4  | response mask requested by SetButtonInfo: (info << 6) \| 0x3F |
| 0x79 | 4  | response mask reported by the controller (query 0x41); 0x3FFFF on a DualShock 2 in analog mode |
| 0x7D | 3  | unused |

### Controller data (padman+0x3AF4)

`data[0]` = 0, `data[1]` = ID, `data[2..30]` = the poll reply after its 3-byte header
(0xFF, ID, 0x5A), zero-padded, `data[31]` = 0x5A. Length is always 32. For a DualShock 2:

| Bytes | Content |
|-------|---------|
| 2 | Select, L3, R3, Start, Up, Right, Down, Left (bit 0..7), active low |
| 3 | L2, R2, L1, R1, Triangle, Circle, Cross, Square, active low |
| 4..7 | right stick X, Y, left stick X, Y (analog modes, 0x80 = centre) |
| 8..19 | pressures: right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2 (0x79 only) |

In mode 0x79 padman makes pressures consistent with the button bits: a pressed button reads at
least 1, a released one 0. The game decodes buttons as `(data[2] << 8 | data[3]) ^ 0xFFFF`
(0x187998), sticks from data[4..7] and, once in pressure mode, copies data[8..19].

## Actuators (SetActDirect)

PortOpen returns, at +0x14, the IOP address of two 0x20-byte blocks. libpad increments a counter
in its own block, writes `{counter, 1 (new), 6, six motor bytes at +0x0C}` and SIF-DMAs it to
`address + (counter & 1) * 0x20`. Each frame, before polling, padman takes the block with the
larger counter; if its "new" flag is set it applies it as SetActDirect and clears the flag.
SetActDirect drops motor bytes that would exceed a power budget of 60 units over all pads (each
running actuator draws the "current" byte of its table entry: 10 and 20 on a DualShock 2).

The motor values travel in the next poll command: transmit byte i carries value i when the
alignment entry i is not 0xFF; the controller routes transmit byte i to motor `align[i]`.

## padman's state machine (one controller transaction per frame and pad)

- Polling (task 1, padman+0x2530): read the controller. A reply with a different ID than
  expected, or no 0x5A, starts detection; ten failed transactions too. Otherwise the data becomes
  valid, the state STABLE (FINDCTP1 for controllers without configuration mode), and a BUSY
  request with nothing queued becomes COMPLETE.
- Scheduler (padman+0xA10): a queued request starts its task if the slot is polling; otherwise
  it fails (request state FAILED).
- Detection (task 2, padman+0x26C0): forget everything, find the controller (state DISCONN until
  one answers), enter configuration mode, query model/modes (0x45), actuators (0x46), combinations
  (0x47), modes (0x4C), send the alignment (0x4D), query the response mask (0x41, if model bit 1),
  leave configuration mode, read the ID. On the first detection after PortOpen a controller in
  analog mode is switched to digital, unlocked, and detected again. 12 frames for a DualShock 2.
- Requests (tasks 4-7, padman+0x2C10/0x2D48/0x2E80/0x3024): enter configuration mode, the
  command (0x44 main mode, 0x4D alignment, 0x4F response mask then 12 x 0x40 pressure parameters,
  or 12 x 0x40), leave. The data is invalid meanwhile (state EXECCMD).

Measured with the HLE (and the transaction counts above): PortOpen to STABLE 14 frames,
SetMainMode to COMPLETE 17 frames (the ID changes, so detection runs again), SetActAlign 4,
EnterPressMode 29.

## DualShock 2 answers used

| Query | Reply bytes used |
|-------|------------------|
| 0x45 model | model 3, 2 modes, current offset (0 digital / 1 analog), 2 actuators, 1 combination |
| 0x46 actuator 0 / 1 | 01 02 00 0A / 01 01 01 14 (function, sub-function, size, current) |
| 0x47 combination 0 | 02 00 01 00 |
| 0x4C mode 0 / 1 | 0x0004 / 0x0007 |
| 0x41 response mask | FF FF 03 (analog mode), zeros in digital mode |

## What Sly 2 does with it (0x187030 init, 0x1871C0 per frame)

1. `scePadInit(0)`, `scePadPortOpen(0, 0, 0x2DFC00)` (port 0 only).
2. While the state is not STABLE or FINDCTP1, wait. (A DISCONN state, or two seconds in one step,
   restarts the sequence.)
3. InfoMode: the type is the extended ID (term 2) if non-zero, else the ID (term 1). For type 4
   or 7 with a non-zero extended ID (configurable controller): `SetMainMode(1, 3)` (analog,
   locked) and wait for COMPLETE; type 4 or 7 without one is used as is; other types keep waiting.
4. Requires exactly two actuators with (function, sub, size) = (1, 2, 0) and (1, 1, 1), then
   `SetActAlign({0, 1, FF, FF, FF, FF})` and waits.
5. If `InfoPressMode`, `EnterPressMode` and wait (the game then copies pressures every frame);
   otherwise it runs without pressure.
6. Every frame: `scePadRead`; it must return data, and a changed ID byte sends the game back
   into the setup sequence. Vibration uses `scePadSetActDirect` (0x1D5150, 0x1D51B8).

## HLE choices (runtime/pad.c)

- One DualShock 2 in port 0, slot 0; port 1 and slots 1-3 (multitap) are empty. Opening an empty
  port gives ERROR for ten frames, then DISCONN, as padman does.
- The controller starts in digital mode with the alignment unset. Input is
  `window_get_input(port)`; a button pressed without a pressure value (keyboard, digital
  triggers) reads as full pressure 0xFF. L3/R3 do not exist in digital mode.
- Setting a main mode (0x44) resets the controller's response mask, so pressure mode has to be
  re-entered after a mode change (assumption; games do it in this order anyway).
- With a partial SetButtonInfo mask the controller still answers ID 0x79 with 12 pressure bytes,
  unrequested ones reading 0 (a real pad would shorten the reply).
- Actuator blocks live at IOP address 0x1E000 + (port * 4 + slot) * 0x40 (nothing else in the
  HLE uses IOP RAM below the SIF command buffer at 0x1F000).
- PortClose takes effect immediately. The per-frame SIF notification (command 0x18, SIF command
  0x80000019 carrying the IOP VBlank count) is not emulated; Sly 2 never enables it.
- Motor values are kept per port (`pad_get_motors`) for a future host rumble hook; nothing drives
  a real motor yet.

## Verification

- `tests/unit/pad/run.ps1`: RPC replies, buffer formatting and frame alternation, detection,
  and the game's whole setup sequence through a C copy of libpad's buffer logic.
- Boot log: `[pad] GetModVersion(0, 0) -> 0x410`, `[pad] Init(0, 0) -> 0x1`,
  `[pad] port 0 slot 0 opened, EE buffer 0x002DFC00`. The record reaches STABLE with ID 0x41 14
  frames later. The game's own joystick state machine (JOY at 0x2DFC00, state at +0x100C) stays
  at its first step because UpdateJoy is not reached while the intro movie waits on the IPU.
