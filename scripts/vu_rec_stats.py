"""Summarise VU microprogram recordings (PS2_VU_RECORD, runtime/vu.c; format in runtime/vu_record.h):
which micro memory images and start addresses the game uses, matched against the overlays the VU
recompiler compiles (recomp/vu_recomp.py), and how many programs/pairs each accounts for.

    <python> scripts\\vu_rec_stats.py <SCUS_973.16> rec1.vurec [rec2.vurec ...] [--dump-unknown <dir>]

--dump-unknown writes the micro memory of start addresses no overlay covers (for a look with
tests/unit/vu/vudis.py --bin)."""
import argparse
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from recomp.vu_recomp import Image, decode, default_entries, elf_overlays  # noqa: E402

HEAD = struct.Struct("<4s5I" + "16s" * 33 + "16H" + "9I")


def records(path: str):
    b = Path(path).read_bytes()
    pos, micro = 0, {}
    while pos + HEAD.size <= len(b):
        h = HEAD.unpack_from(b, pos)
        magic, idx, addr, vblank, msize, dsize = h[:6]
        if magic != b"VUP1":
            raise SystemExit(f"{path}: bad record at {pos}")
        pos += HEAD.size
        if msize:
            micro[idx] = b[pos:pos + msize]
            pos += msize
        pos += dsize
        pairs, cycles, kicks, _pad = struct.unpack_from("<4I", b, pos)
        pos += 4 * 4 + 16
        if pos > len(b):
            break
        yield idx, addr, vblank, micro[idx], pairs, cycles


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("recs", nargs="+")
    ap.add_argument("--dump-unknown")
    args = ap.parse_args()
    imgs = []
    for ident, chunks in elf_overlays(args.elf):
        for unit in (0, 1):
            img = Image(ident, unit, chunks)
            if unit == 0 and max(s + len(r) for s, r in chunks) > 512:
                continue
            ops = {pc: decode(r, pc, img.pcmask) for pc, r in img.pairs().items()}
            img.entries = default_entries(img, ops)
            raw = b"".join(struct.pack("<Q", r) for _, rs in chunks for r in rs)
            imgs.append((img, chunks, raw))
    progs, pairs = Counter(), Counter()
    unknown = {}
    for path in args.recs:
        for idx, addr, vb, micro, np, nc in records(path):
            pc = addr & (0x7FF if idx else 0x1FF)
            match = None
            for img, chunks, _ in imgs:          # a matching image, preferably one with this entry
                if img.unit != idx:
                    continue
                ok = all(micro[s * 8:(s + len(r)) * 8] == b"".join(struct.pack("<Q", x) for x in r) for s, r in chunks)
                if ok and (match is None or pc in img.entries):
                    match = img
                    if pc in img.entries:
                        break
            if match is None:
                key = (idx, pc, "no image")
                unknown[(idx, pc)] = micro
            elif pc not in match.entries:
                key = (idx, pc, f"{match.name}: not an entry")
            else:
                key = (idx, pc, match.name)
            progs[key] += 1
            pairs[key] += np
    tot = sum(pairs.values())
    for key, n in sorted(pairs.items(), key=lambda kv: -kv[1]):
        idx, pc, what = key
        print(f"VU{idx} 0x{pc * 8:04X} {what:32s} {progs[key]:6d} programs {n:9d} pairs ({100 * n / tot:5.1f}%)")
    if args.dump_unknown:
        out = Path(args.dump_unknown)
        out.mkdir(parents=True, exist_ok=True)
        for (idx, pc), micro in unknown.items():
            (out / f"vu{idx}_{pc * 8:04X}.bin").write_bytes(micro)
    return 0


if __name__ == "__main__":
    sys.exit(main())
