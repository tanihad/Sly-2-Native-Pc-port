"""(r2-m5) The hybrid check (notes/modern-renderer.md 5, level 3 "hybrid vs reference" in game): judge consecutive
pictures of a still scene taken with PS2_R2_ALT (hybrid frames alternate with all-exact ones, scripts/r2_hcheck.ps1).

The bracket A/B (r2_ab.ps1) replays one GS stream, so it cannot see what dropping the native entries' DMA does to the
VU1 / VIF / GS state of the exact draws after them. Here the game itself draws hybrid and reference frames in turn: a
pixel is "alternating" when pictures i and i+2 (same mode) agree within --same but picture i+1 (the other mode) differs
from both by more than --diff. Animation, flicker and camera drift do not alternate; a missing object or wrong
geometry of either mode does. Gate: the worst triple's alternating share <= --max (default 0.002) and no 16x16 block
with more than half of its pixels alternating (a blob, not edge noise).

    python scripts/r2_hcheck.py <frames dir> [--max 0.002] [--out worst.png]
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from contact_sheet import read_png, write_png  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--same", type=int, default=12)
    ap.add_argument("--diff", type=int, default=40)
    ap.add_argument("--max", type=float, default=0.002)
    ap.add_argument("--blob", type=float, default=0.5)
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    files = sorted(p for p in Path(a.dir).glob("frame_*.png"))
    pics = []
    for f in files:
        w, h, px = read_png(f)
        if pics and pics[-1][3] == px:          # (a picture shown for two VBlanks)
            continue
        pics.append((f.name, w, h, px))
    if len(pics) < 3:
        print(f"hcheck: {len(pics)} distinct pictures in {a.dir} (need 3)")
        return 2
    worst = (-1.0, None)
    blobs_any = 0
    for i in range(len(pics) - 2):
        (n0, w, h, p0), (n1, _, _, p1), (n2, _, _, p2) = pics[i], pics[i + 1], pics[i + 2]
        if len(p0) != len(p1) or len(p0) != len(p2):
            continue
        same, diff = a.same, a.diff
        bw, bh = (w + 15) // 16, (h + 15) // 16
        blk = [0] * (bw * bh)
        alt = 0
        mask = bytearray(w * h)
        for y in range(h):
            row = y * w * 3
            by = (y >> 4) * bw
            for x in range(w):
                o = row + 3 * x
                d02 = max(abs(p0[o] - p2[o]), abs(p0[o + 1] - p2[o + 1]), abs(p0[o + 2] - p2[o + 2]))
                if d02 > same:
                    continue
                d01 = max(abs(p0[o] - p1[o]), abs(p0[o + 1] - p1[o + 1]), abs(p0[o + 2] - p1[o + 2]))
                if d01 <= diff:
                    continue
                d21 = max(abs(p2[o] - p1[o]), abs(p2[o + 1] - p1[o + 1]), abs(p2[o + 2] - p1[o + 2]))
                if d21 <= diff:
                    continue
                alt += 1
                mask[y * w + x] = 1
                blk[by + (x >> 4)] += 1
        share = alt / (w * h)
        blobs = sum(1 for c in blk if c > a.blob * 256)
        blobs_any += blobs
        print(f"  {n0} {n1} {n2}: alternating {alt} px ({share * 100:.4f}%), blobs {blobs}")
        if share > worst[0]:
            worst = (share, (n1, w, h, p1, mask))
    ok = worst[0] <= a.max and blobs_any == 0
    if a.out and worst[1]:
        n1, w, h, p1, mask = worst[1]
        img = bytearray(p1)
        for k in range(w * h):
            if mask[k]:
                img[3 * k:3 * k + 3] = b"\xff\x00\xff"
        write_png(Path(a.out), w, h, bytes(img))
    print(f"hcheck: {len(pics)} pictures, worst alternating share {worst[0] * 100:.4f}% (max {a.max * 100:.2f}%), "
          f"blobs {blobs_any}: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
