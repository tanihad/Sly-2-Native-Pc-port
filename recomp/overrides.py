"""Native replacements for whole recompiled functions.

The recompiler owns every function of the game (notes/native-port-principles.md, rule 4), so a
function whose work means nothing on the host can be replaced by native C instead of being
translated instruction by instruction. Each entry is keyed by the function's start address and
guarded by a hash of its original bytes: if the executable differs (another region, a test ELF),
the function is translated normally.

Cache maintenance. The EE's caches are not modelled: the recompiled code reads and writes guest
memory directly, so memory is always coherent with what DMA, VIF, GIF and the IOP stand-ins see.
Sony's cache maintenance routines are therefore no-ops by definition - but their loops still run:
`cache` and `sync` translate to nothing, yet the loops step through every cache line of a range
(or the whole 64 x 2 tag RAM, reading TagLo through ps2_mfc0 each time). The overrides below
return at once. They leave the scratch registers ($v0, $v1, $a2, $a3, $t0-$t3, $t9) as they were,
where the loops would have left loop values: callers are compiled C, which cannot rely on
caller-saved registers after a call, and the functions return nothing.
See notes/recompiler-design.md ("Native overrides").
"""
from dataclasses import dataclass
import hashlib


@dataclass(frozen=True)
class Override:
    name: str
    sha1: str          # of the function's original bytes [start, end)
    body: str          # C statements; `ctx` is the guest context
    reason: str


OVERRIDES: dict[int, Override] = {
    0x00234EA0: Override(
        "SyncDCache_lines", "2170247e57cae1bad706bfd0b8f82d37991f3c46", "return;",
        "libkernel: write back the data cache lines holding [a0, a1) by walking all 64 x 2 tags "
        "(cache 0x10 index-load-tag, mfc0 TagLo, cache 0x14 index-writeback-invalidate); called "
        "with interrupts off by 0x234F48 (SyncDCache)"),
    0x00234FD0: Override(
        "InvalidDCache_lines", "d2a3b797096051da25e1b0a34272bf5d08315149", "return;",
        "libkernel: invalidate the data cache lines holding [a0, a1), same tag walk with cache "
        "0x16 (index-invalidate); called by 0x235078 (InvalidDCache)"),
    0x0023B170: Override(
        "sceSifWriteBackDCache", "fd9228c81ce6eda97e02d07c212e79d7d3e15c87", "return;",
        "libkernel: write back [a0, a0 + a1) line by line (cache 0x18 hit-writeback-invalidate, "
        "unrolled by 8) before SIF/DMA transfers; 22 call sites"),
}


def function_sha1(prog, start: int, end: int) -> str:
    data = bytearray()
    for a in range(start, end, 4):
        w = prog.read_u32(a)
        if w is None:
            return ""
        data += w.to_bytes(4, "little")
    return hashlib.sha1(bytes(data)).hexdigest()


def override_for(prog, start: int, end: int) -> Override | None:
    """The override for the function at start, if its bytes are the expected ones."""
    o = OVERRIDES.get(start)
    if o and function_sha1(prog, start, end) == o.sha1:
        return o
    return None
