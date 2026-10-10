"""Compare two directories of dumped frames (frame_NNNNNN.png from run_game.ps1 -Dump).

Usage: python scripts/compare_frames.py <dirA> <dirB> [--tol N]

For every frame number present in both, prints the number of differing pixels (any channel differs by
more than --tol, default 0) and the bounding box of the differences. PNG decoding is self-contained
(8-bit RGB/RGBA, all filter types), so no imaging package is needed.
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path


def read_png(path: Path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, w = 8, [], 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            if depth != 8 or ctype not in (2, 6):
                raise ValueError(f"{path}: unsupported PNG (depth {depth}, colour type {ctype})")
            bpp = 3 if ctype == 2 else 4
        elif kind == b"IDAT":
            idat.append(body)
        pos += 12 + n
    raw = zlib.decompress(b"".join(idat))
    stride = w * bpp
    out = bytearray(h * stride)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, bpp, bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--tol", type=int, default=0)
    args = ap.parse_args()
    fa = {p.name: p for p in Path(args.a).glob("frame_*.png")}
    fb = {p.name: p for p in Path(args.b).glob("frame_*.png")}
    common = sorted(set(fa) & set(fb))
    same = 0
    for name in common:
        wa, ha, ba, pa = read_png(fa[name])
        wb, hb, bb, pb = read_png(fb[name])
        if (wa, ha) != (wb, hb):
            print(f"{name}: size {wa}x{ha} vs {wb}x{hb}")
            continue
        diff, x0, y0, x1, y1 = 0, wa, ha, -1, -1
        for y in range(ha):
            ra = pa[y * wa * ba:(y + 1) * wa * ba]
            rb = pb[y * wb * bb:(y + 1) * wb * bb]
            if ba == bb and ra == rb:
                continue
            for x in range(wa):
                ca, cb = ra[x * ba:x * ba + 3], rb[x * bb:x * bb + 3]
                if any(abs(ca[k] - cb[k]) > args.tol for k in range(3)):
                    diff += 1
                    x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
        if diff:
            print(f"{name}: {diff} pixels differ, box {x0},{y0}..{x1},{y1}")
        else:
            same += 1
    print(f"{same}/{len(common)} common frames identical (tolerance {args.tol})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
