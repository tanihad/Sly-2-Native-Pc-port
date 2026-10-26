"""r2 (modern-renderer RE): texture memory a frame uses, from a GS recording (runtime/gs_record.c format).

    python scripts/r2_texstats.py <file.gsrec>

Per VBlank with drawing: distinct TEX0 (TBP, TBW, PSM, size, CBP, CPSM) bound by textured primitives, their nominal
bytes (2^TW x 2^TH x bpp), distinct CLUTs (by CBP + PSM), and the VRAM footprint (8 KB pages touched by the textures'
TBP .. TBP + size, a page-granular estimate), plus image uploads (IMAGE data) and where they go (BITBLTBUF DBP).
"""
import struct
import sys
from collections import defaultdict

BPP = {0x00: 32, 0x01: 24, 0x02: 16, 0x0A: 16, 0x13: 8, 0x14: 4, 0x1B: 32, 0x24: 32, 0x2C: 32, 0x30: 32, 0x31: 24,
       0x32: 16, 0x3A: 16}
NAMES = {0x00: "CT32", 0x01: "CT24", 0x02: "CT16", 0x0A: "CT16S", 0x13: "T8", 0x14: "T4", 0x1B: "T8H", 0x24: "T4HL",
         0x2C: "T4HH", 0x30: "Z32", 0x31: "Z24", 0x32: "Z16", 0x3A: "Z16S"}


def main():
    b = open(sys.argv[1], "rb").read()
    pos = 8 + 4 * 1024 * 1024 + 1024 + 0x64 * 8
    tex0 = [0, 0]
    prim = 0
    prmodecont = 1
    prmode = 0
    frame = None
    used = defaultdict(set)
    uploads = defaultdict(list)
    bitblt = 0
    prims = defaultdict(int)
    while pos + 8 <= len(b):
        tag, ln = struct.unpack_from("<II", b, pos)
        pos += 8
        if tag == 1:
            v, = struct.unpack_from("<Q", b, pos)
            pos += 8
            r = ln
            if r in (0x06, 0x07):
                tex0[r - 6] = v
            elif r == 0x00:
                prim = v
            elif r == 0x1A:
                prmodecont = v & 1
            elif r == 0x1B:
                prmode = v
            elif r == 0x50:
                bitblt = v
            elif r in (0x04, 0x05) and frame is not None:      # XYZF2 / XYZ2 kick
                attr = prim if prmodecont else prmode
                if (attr >> 4) & 1:
                    ctx = (attr >> 9) & 1
                    used[frame].add(tex0[ctx] & 0x1FFFFFFFFFFFFFFF)
                prims[frame] += 1
        elif tag == 2:
            uploads[frame].append((ln, (bitblt >> 32) & 0x3FFF, (bitblt >> 56) & 0x3F))
            pos += (ln + 3) & ~3
        elif tag == 3:
            frame = ln
            pos += 16 * 8
        else:
            break
    for f in sorted(k for k in used if k is not None):
        if prims[f] < 1000:
            continue
        texb = 0
        cluts = set()
        pages = set()
        bypsm = defaultdict(int)
        sizes = set()
        for t in used[f]:
            tbp, tbw, psm = t & 0x3FFF, (t >> 14) & 0x3F, (t >> 20) & 0x3F
            tw, th = (t >> 26) & 0xF, (t >> 30) & 0xF
            cbp, cpsm = (t >> 37) & 0x3FFF, (t >> 51) & 0xF
            key = (tbp, tbw, psm, tw, th)
            if psm in (0x13, 0x14, 0x1B, 0x24, 0x2C):
                cluts.add((cbp, cpsm, psm in (0x13, 0x1B)))
            if key in sizes or psm in (0x00, 0x02):     # (CT32 / CT16: the frame buffers read by post passes)
                continue
            sizes.add(key)
            nbytes = (1 << tw) * (1 << th) * BPP.get(psm, 32) // 8
            texb += nbytes
            bypsm[NAMES.get(psm, hex(psm))] += 1
            p0 = tbp // 32
            for p in range(p0, p0 + max(1, (nbytes + 8191) // 8192)):
                pages.add(p)
        clutb = sum(1024 if big else 64 for (_, _, big) in cluts)
        up = uploads.get(f, [])
        print(f"VBlank {f}: {prims[f]} primitives, {len(sizes)} distinct textures ({dict(bypsm)}), nominal "
              f"{texb / 1024:.0f} KB, {len(cluts)} CLUTs ({clutb / 1024:.1f} KB), VRAM pages touched {len(pages)} "
              f"({len(pages) * 8} KB) span {min(pages):03X}-{max(pages):03X}; uploads {len(up)} "
              f"({sum(u[0] for u in up) / 1024:.0f} KB) to DBP {sorted(set(u[1] for u in up))[:8]}")


if __name__ == "__main__":
    main()
