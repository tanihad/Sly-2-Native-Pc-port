# Memory card protocol (libmc / mcserv / mcman) as used by Sly 2

Everything below was read out of the game's own code, not from emulators:

- **EE side**: Sony libmc, statically linked (no symbols). Found through the RPC server id
  0x80000400 (`lui 0x8000 / ori 0x400` at 0x243DB4, in sceMcInit).
- **IOP side**: the game embeds the IOP modules it loads in the ELF's `.irx` section
  (0x472200+). `modules\mcman.irx` (ELF image at 0x47EC00, "PsIImcman 3000", module version
  **2.26**) and `modules\mcserv.irx` (0x496300, "PsIImcserv 3000", version **2.10**) are the
  XMCMAN/XMCSERV generation. They are relocatable IRX ELFs linked at 0, so their code can be
  disassembled directly; imports are `jr ra / addiu zero, zero, N` stubs after a 0x41E00000
  table header (library name at +12). mcserv's dispatcher is at 0x33C, its receive buffer at
  0x32D0 and its reply buffer at 0x12C0. mcman's export table starts at 0x14 (#5 McDetectCard
  0x768, #6 McOpen 0xC00, #7 McClose 0xD20, #8 McRead 0x1188, #9 McWrite 0x1340, #10 McSeek
  0x10B0, #11 McFormat 0x1D3C, #12 McGetDir 0x14F8, #13 McDelete 0x1C24, #14 McFlush 0xEE0,
  #15 McChDir 0x1B28, #16 McSetFileInfo 0x19C4, #21 McDetectCard2 0x9B4, #22 McGetFormat 0x278,
  #23 McGetEntSpace 0x162C, #24 McReplaceBadBlock, #36 McUnformat 0x1E20, #37 init (returns 0),
  #38 McGetFreeClusters 0x310, #39 McGetMcType 0x2B8, #42 module info, #53 McGetIoSema).

The HLE is `runtime/mc.c`; tests in `tests/unit/mc/`.

## Transport

- Server id **0x80000400**, bound by sceMcInit. Server receive buffer: whatever sif.c assigns.
- sceMcInit calls fn **0xFE** synchronously (mode 0) with the 48-byte descriptor and a **12-byte**
  reply; every other call is **asynchronous**: `sceSifCallRpc(cd, fno, SIF_RPC_M_NOWAIT, send,
  size, rdata, 4, endfunc, enddata)`.
- libmc state (EE): client 0x450880 (`+0x24` = server, checked for "initialised"), descriptor
  **0x450900** (48 bytes), name descriptor **0x450930** (0x414 bytes), end-of-call struct
  **0x450D80** (0xC0 bytes), reply **0x451E40** (`rdata[0]` = result), current command
  0x2C8350, getinfo result pointers 0x4508A8/AC/B0, semaphores 0x2C8354 (one call at a time,
  PollSema'd by every call: -200 if busy) and 0x2C8358 (end of call).
- End callbacks run from the SIF RPC-end interrupt: 0x243ED8 just iSignalSema's 0x2C8358;
  0x244110 (read) and 0x244518 (getinfo) first copy results out of the end struct (read through
  the uncached mirror, address | 0x20000000).
- **sceMcSync(mode, &cmd, &result)** (0x244450): returns -1 if no command is pending; polls the
  end semaphore (mode 0 = WaitSema instead); when done: `*cmd` = current command, `*result` =
  **rdata[0]**, clears the command, releases the call semaphore, returns 1 (0 = still running).
- Every call returns 0 when queued, -100 (not initialised), -200 (busy), -210 (empty name),
  -91 (sceSifCallRpc failed).
- The current command codes (sceMcSync's `cmd`) equal the mcserv function numbers, except
  mkdir (0x0B, sent as an open with flag 0x40).

## Descriptor layouts (as received in mcserv's buffer)

Descriptor (48 bytes, libmc 0x450900):

| off  | use                                                                    |
|------|------------------------------------------------------------------------|
| 0x00 | fd                                                                     |
| 0x04 | port                                                                   |
| 0x08 | slot                                                                   |
| 0x0C | size (read/write: bytes remaining after the head) / getinfo: want format |
| 0x10 | seek offset / getinfo: want free                                       |
| 0x14 | seek origin / write: head byte count / getinfo: want type / thread priority |
| 0x18 | EE data buffer (read: destination; write: source after the head)      |
| 0x1C | EE end-of-call struct (0x450D80) for read/getinfo                     |
| 0x20 | write: up to 16 head bytes (data before the first 16-byte boundary)   |

Name descriptor (0x414 bytes, libmc 0x450930): 0x00 port, 0x04 slot, 0x08 flags/mode,
0x0C maxent, 0x10 EE pointer (getdir table / setfileinfo info / chdir current-dir buffer),
0x14 name (1024 bytes, NUL at 0x413).

mcserv's receive buffer keeps bytes of earlier calls (fn 0xFE reads descriptor word 0 that
sceMcInit never sets); the HLE keeps a persistent copy for the same effect.

## Functions

| fn   | libmc (EE)              | args                                   | result in rdata[0]                 |
|------|-------------------------|----------------------------------------|------------------------------------|
| 0xFE | sceMcInit 0x243CB0      | desc (48), reply 12                    | [0] 0, [1] mcserv version 0x210, [2] mcman version 0x226; libmc requires >= 0x20A / 0x20E (else -120 / -121) |
| 0x01 | sceMcGetInfo 0x244570   | desc: port, slot, want flags, end 0x1C | McDetectCard2: 0 same card, -1 new formatted card, -2 new unformatted, <= -10 no card |
| 0x02 | sceMcOpen 0x243EF0      | name desc: port, slot, mode, name      | fd or error                        |
| 0x02 | sceMcMkdir 0x244018     | open with mode 0x40                    | 0 or error (cmd 0x0B)              |
| 0x03 | sceMcClose 0x244050     | desc: fd                               | 0 / error                          |
| 0x04 | (sceMcSeek, not linked) | desc: fd, 0x10 offset, 0x14 origin     | new position                       |
| 0x05 | sceMcRead 0x2441C0      | desc: fd, 0x0C size, 0x18 buf, 0x1C end| bytes read / error                 |
| 0x06 | sceMcWrite 0x2442D8     | desc: fd, 0x0C rest, 0x14 head, 0x18 src+head, 0x20 head bytes | bytes written / error |
| 0x0A | (sceMcFlush)            | desc: fd                               | 0 / error                          |
| 0x0C | (sceMcChdir)            | name desc: port, slot, 0x10 curdir buf, name | 0 / error; 0x400 bytes of new current path DMA'd to 0x10 |
| 0x0D | sceMcGetDir 0x2446F0    | name desc: port, slot, flags, maxent, table, name | entries / error   |
| 0x0E | (sceMcSetFileInfo)      | name desc: port, slot, flags, 0x10 EE sceMcTblGetDir, name | 0 / error   |
| 0x0F | sceMcDelete 0x244918    | name desc: port, slot, flags (0), name | 0 / error                          |
| 0x10 | sceMcFormat 0x244848    | desc: port 0x04, slot 0x08             | 0 / error                          |
| 0x11 | (sceMcUnformat)         | desc: port, slot                       | 0 / error                          |
| 0x12 | sceMcGetEntSpace 0x244A58 | name desc: port, slot, name          | free entries                       |
| 0x14 | (change thread priority)| desc 0x14: priority                    | 0, or -403 outside 9..123          |
| 0x15 | (slot max)              | desc 0x04: port                        | slots on the port (1: no multitap) |
| 0x33 | (unknown)               | desc: port, slot, 0x10                 | mcman export #45                   |

Functions in parentheses are not linked into Sly 2, which has only sceMcInit, GetInfo, Open,
Mkdir, Close, Read, Write, Sync, GetDir, Delete, Format and GetEntSpace (0x243CB0-0x244B60);
their layouts come from mcserv. mcserv has no handler for 0x13 (and other numbers): it leaves its reply
buffer untouched, so the caller gets the previous result. After every command mcserv calls
McReplaceBadBlock and releases mcman's I/O semaphore.

### How data moves

- **Read** (mcserv 0x758): data is read in 8 KB pieces into an IOP buffer and SIF-DMA'd straight
  to the EE buffer, except what lies before the first and after the last 64-byte boundary
  (cache lines the EE may share with other data). Those travel in the end struct
  `{size1, size2, dest1, dest2, data1[64], data2[64]}` (0xC0 bytes, DMA'd to desc 0x1C) and
  libmc's callback 0x244110 copies them into place. `size1` = size if size <= 64, else
  `(64 - (buf & 63)) & 63`; `size2` = the remainder's low 6 bits. A short read (end of file)
  puts the partial last 64-byte block in `data2` too. Result: total bytes, or the McRead error.
- **Write** (mcserv 0x9DC): the EE copies the bytes before the first 16-byte boundary into the
  descriptor (0x20, at most 16; writes under 17 bytes go entirely there). mcserv writes those,
  then **pulls the rest from EE memory itself with sceSifGetOtherData** (8 KB at a time from
  desc 0x18). The EE does *not* DMA the bulk data into IOP RAM before the call. The HLE reads
  EE RAM directly at that point. Result: total, the short count + head, or the error.
- **GetInfo** (mcserv 0xD38): result struct (0xC0 bytes) to desc 0x1C: **type at 0x00**
  (2 = PS2), **free clusters at 0x04**, **formatted at 0x90**; libmc's callback stores each into
  the caller's pointer if it was non-NULL (the "want" words tell mcserv which to compute). For a
  result below -1 free and formatted are 0.
- **GetDir** (mcserv 0xAE8): for each requested entry mcserv calls mcman McGetDir with
  maxent 1 (first call with the caller's flags, later ones with 1 = "continue") and DMAs the
  64-byte entry to `table + 64*i`; returns the count (or the first error). **maxent < 0**:
  mcman counts all remaining matches without storing anything (table may be NULL) — the game
  uses this to test whether its save directory exists.
- **SetFileInfo**: mcserv fetches the caller's 64-byte sceMcTblGetDir with sceSifGetOtherData.

### sceMcTblGetDir (64 bytes)

0x00 created, 0x08 modified (sceMcStDateTime: resv, sec, min, hour, day, month, year16 —
PS2 clock time, i.e. JST), 0x10 file size (0 for directories), 0x14 mode (u16), 0x16 0,
0x18 the entry's attribute word (dirent +0x20), 0x1C 0, 0x20 name (32 bytes).

## mcman behaviour (what the HLE reproduces)

- **Result codes**: 0 ok, -1 new card, -2 not formatted, -3 card full, -4 no such entry /
  already exists, -5 denied, -6 directory not empty, -7 no free handle, -10 no card.
- **Card**: an 8 MB card has 8192 clusters of 1 KB; McGetFreeClusters counts free FAT
  entries among the allocatable clusters, which the mount code limits to
  (clusters_per_card / 1000) * 1000 + 1 = **8001**. The root directory uses one, so a fresh
  card reports **8000** free. Directory entries are 512 bytes: two per cluster.
- **Open** (flags: 1 read, 2 write, 0x200 create (implies write), 0x40 make directory, 0x0800 /
  0x1000 copied into a new entry's mode, 0x2000 stripped): empty name or wildcards ('?', '*',
  control characters) -4; three handles max (-7); missing without create -4; missing parent -4.
  New files get mode **0x8417**, new directories **0x8427**, both stamped with the current
  time; a file gains **0x80 ("closed") when closed or flushed** after writing, which also sets
  its modified time and its directory's (so a finished file is 0x8497).
  **Create on an existing file deletes it and makes a new, empty entry** (new creation time,
  attribute word 0). Opening for write needs mode bit 2, for read bit 1 (-5), and fails while
  another handle has the file open for writing (-5); re-creating an open file returns -43.
  Directories open as handles if their mode has read and execute.
  **Directories can only be created in the root** (-5 elsewhere); mkdir of an existing name -4.
  A subdirectory holds at most **20 entries** including "." and ".." (-5 beyond).
- **Read/Write/Seek/Close/Flush** on a bad or closed handle -5; read needs the read flag,
  write the write flag. Seek origins 0 SET, 2 END, anything else CUR; the result is clamped
  at 0.
- **GetDir**: the name splits at its last '/' into a directory (empty = current directory)
  and a pattern. The **root lists from entry 2** (no "." / ".."); subdirectories list "."
  (the directory's own entry) and ".." (its parent's) first. Deleted and **hidden (0x2000)**
  entries are skipped. Pattern rules: no wildcard = exact compare; '?' matches one character
  **or the end of the name** ("save?" matches "save"); '*' matches any run, and wildcards
  directly after a '*' are ignored. Listing a missing directory or a file: -4.
- **GetEntSpace**: `(entries & 1) + deleted entries` of the directory at `path`: the free
  entry slots that don't need a new cluster.
- **Delete**: last component "." / ".." -5; not found or the root -4; needs mode bit 2 (-5);
  non-empty directory -6; open file -5. flags != 0 = undelete.
- **SetFileInfo** flags: 0x01 created, 0x02 modified, 0x04 (and 0x200) mode — **directories
  only**, bits 0x180F (rwx, copy-protect 0x08, 0x800, 0x1000), 0x08 attribute word (from
  table 0x18), 0x10 rename (table 0x20, at most 31 characters; "." / ".." / '/' / wildcards /
  existing name -4). The root and "." / ".." can't be changed (-5).
- **ChDir**: sets the per-card current directory and returns its absolute path ("/",
  "/DIR").
- **Format** re-creates an empty root; **Unformat** leaves the card unformatted (GetInfo:
  formatted 0, free 0; everything else -2).

## What Sly 2 does

The game's memory card manager (0x1AB000-0x1ACC00) wraps each call as call + sceMcSync(0)
(0x1AB098) and maps results per command through 0x1AABC0 (GetInfo: 0 ok, -1 new formatted card,
-2 unformatted, <= -10 no card). It saves into **/BASCUS-97316YAOTWTD!** holding
`BASCUS-97316YAOTWTD!`, `icon.sys`, `memcard_icon.ico` and `save0..2` (format "/%s/%s%d").
It probes for the directory with `sceMcGetDir(port, slot, "/BASCUS-97316YAOTWTD!", 0, -1, NULL)`
(> 0 = exists), reads up to 18 entries per GetDir, checks room with sceMcGetEntSpace("/")
plus the free clusters (needs about 100 KB), writes files with mode 0x202 and reads them with
mode 1, checking the byte counts. At boot it calls sceMcInit then sceMcGetInfo(0, 0, ...).

## Host mapping (runtime/mc.c)

- Port 0 slot 0: card directory `PS2_MEMCARD_DIR`, else `memcard\slot0` next to the
  executable (created on first use). Port 1 and other slots: no card (-10).
- Card directories are host directories and files host files; card names are case-sensitive
  bytes, compared exactly (a host name differing only in case counts as absent, and creating
  such a name is refused with -5). Bytes outside `0-9 A-Z a-z ! # $ & ' ( ) + , - . ; = @ [ ] ^ _
  ` { } ~`, the first byte of a Windows device name (CON, AUX, COM1...) and a trailing '.' are
  stored as `%XX`. Host files whose names aren't exactly such an encoding (`%meta`, names with
  spaces or lowercase escapes, names over 32 bytes) are invisible to the card; any other host
  file in the card directory is a card file.
- Each directory has a text sidecar **`%meta`**: `. <mode> <attr> <created> <modified>` for
  the directory's own entry, then its entries in card order — files as
  `<name> <mode> <attr> <created> <modified>`, subdirectories as `<name>/` (their entry lives in
  their own sidecar, so a save directory copied elsewhere keeps its attributes). Times are
  `YYYY-MM-DDThh:mm:ss` JST. Entries missing from the sidecar get defaults (0x8497 / 0x8427,
  host file times). Sidecars are rewritten atomically (temp file + rename) on every change.
- Free space = 8001 - (sum over files of ceil(size / 1 KB) + sum over directories of
  ceil((entries + 2) / 2)); recounted when stale (our writes keep it current).
- `%unformatted` in the card directory marks an unformatted card.
- The first GetInfo of each run reports a new card (-1), like a real console after boot.
- Not emulated: deleted-entry slots (mcman keeps deleted entries and reuses them; GetEntSpace
  counts them), undelete, PS1/PDA cards, card swaps, bad blocks, fn 0x33 (answered 0).
  ChDir skips the 0x400-byte current-directory DMA when the EE pointer is NULL.
