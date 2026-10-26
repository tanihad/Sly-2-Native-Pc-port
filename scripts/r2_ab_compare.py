"""(r2-m1) Modern renderer A/B (notes/modern-renderer.md 5, levels 2-3): the exact and the hybrid replay of one
PS2_R2=bracket recording (scripts/gs_replay.c --game --game-save --vram-out --r2-ram <dump> --r2-ab 0 / 1), compared.

    python scripts/r2_ab_compare.py <exact dir> <hybrid dir> [--vblank N] [--frame 0x00,0x38] [--zbuf 0x70] [--pages 56]

GS memory (sample 0 of every pixel as the GPU has it after the VBlank, vram_<vblank>.bin): every word of the frame
buffers (--pages pages from each FBP: a 512 x 448 PSMCT32 buffer is 56 pages, so position does not matter) and of the Z32
buffer: colour channel differences, destination alpha bit 7 (DATE) and the alpha byte, |dZ| where either holds Z.
Pictures (frame_<vblank>.png): PSNR, share of pixels differing by > 16, mean signed error per channel. No dependencies.
"""
import argparse
import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from contact_sheet import read_png  # noqa: E402


def words(path):
    b = Path(path).read_bytes()
    return struct.unpack(f"<{len(b) // 4}I", b)


STATS = []   # (r2-m2) per frame buffer: the numbers the gates check (--gates)
ZSTATS = []


def colour_stats(wa, wb, first, n):
    tot = d2 = d8 = d16 = ad = a7 = 0
    se = 0.0
    ssum = [0, 0, 0]
    for i in range(first, first + n):
        p, q = wa[i], wb[i]
        tot += 1
        if p == q:
            continue
        m = 0
        for c in range(3):
            dc = ((q >> (8 * c)) & 255) - ((p >> (8 * c)) & 255)
            ssum[c] += dc
            se += dc * dc
            if abs(dc) > m:
                m = abs(dc)
        d2 += m > 2
        d8 += m > 8
        d16 += m > 16
        ad += abs((q >> 24) - (p >> 24)) > 2
        a7 += ((q >> 31) & 1) != ((p >> 31) & 1)
    mse = se / (3 * tot)
    psnr = 10 * math.log10(255 * 255 / mse) if mse else 999.0
    STATS.append({"psnr": psnr, "over2": d2 / tot, "over8": d8 / tot, "over16": d16 / tot,
                  "mean": max(abs(x) / tot for x in ssum), "bit7": a7 / tot})
    return (f"{tot} px: PSNR {psnr:.2f} dB; channel > 2: {100 * d2 / tot:.3f}%, > 8: {100 * d8 / tot:.3f}%, > 16: {100 * d16 / tot:.3f}%; "
            f"mean signed {ssum[0] / tot:+.4f} {ssum[1] / tot:+.4f} {ssum[2] / tot:+.4f}; alpha bit 7 {100 * a7 / tot:.4f}%, "
            f"alpha byte > 2: {100 * ad / tot:.3f}%")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--frame", default="0x00,0x38")
    ap.add_argument("--zbuf", type=lambda s: int(s, 0), default=0x70)
    ap.add_argument("--pages", type=int, default=56)
    ap.add_argument("--vblank", type=int, default=0)
    ap.add_argument("--gates", default="", help="tests/r2/gates.json: check level 3 (S = 1); exit 1 on a violation")
    args = ap.parse_args()
    A, B = Path(args.a), Path(args.b)
    vbs = sorted(int(p.stem.split("_")[1]) for p in A.glob("vram_*.bin"))
    if args.vblank:
        vbs = [args.vblank]
    for vb in vbs:
        wa, wb = words(A / f"vram_{vb}.bin"), words(B / f"vram_{vb}.bin")
        for fbp in (int(x, 0) for x in args.frame.split(",")):
            print(f"v{vb} frame {fbp:#04x}: " + colour_stats(wa, wb, fbp * 2048, args.pages * 2048))
        zn = z0 = z2 = z4 = z16 = 0
        zmax = 0
        for i in range(args.zbuf * 2048, (args.zbuf + args.pages) * 2048):
            p, q = wa[i], wb[i]
            if not p and not q:
                continue
            zn += 1
            d = abs(q - p)
            z0 += d == 0
            z2 += d <= 2
            z4 += d <= 4
            z16 += d <= 16
            zmax = max(zmax, d)
        if zn:
            ZSTATS.append(z4 / zn)
            print(f"v{vb} Z {args.zbuf:#04x}: {zn} px: |dZ| = 0 {100 * z0 / zn:.2f}%, <= 2 {100 * z2 / zn:.2f}%, <= 4 {100 * z4 / zn:.3f}%, "
                  f"<= 16 {100 * z16 / zn:.3f}% (max {zmax})")
        pa, pb = A / f"frame_{vb:06d}.png", B / f"frame_{vb:06d}.png"
        if pa.exists() and pb.exists():
            w, h, x1 = read_png(pa)
            w2, h2, x2 = read_png(pb)
            if (w, h) == (w2, h2):
                se = 0.0
                big = 0
                s = [0, 0, 0]
                for k in range(0, len(x1), 3):
                    m = 0
                    for c in range(3):
                        d = x2[k + c] - x1[k + c]
                        se += d * d
                        s[c] += d
                        m = max(m, abs(d))
                    big += m > 16
                npx = w * h
                mse = se / (3 * npx)
                psnr = 10 * math.log10(255 * 255 / mse) if mse else 999.0
                print(f"v{vb} picture {w}x{h}: PSNR {psnr:.2f} dB, > 16: {100 * big / npx:.3f}%, mean signed {s[0] / npx:+.3f} "
                      f"{s[1] / npx:+.3f} {s[2] / npx:+.3f}")
    if args.gates:
        import json
        g = json.loads(Path(args.gates).read_text())["level3_frame_s1"]
        bad = []
        for i, st in enumerate(STATS):
            if st["psnr"] < g["psnr_min_db"]:
                bad.append(f"buffer {i}: PSNR {st['psnr']:.2f} < {g['psnr_min_db']}")
            for k, gk in (("over16", "share_over16_max"), ("over8", "share_over8_max"), ("over2", "share_over2_max"),
                          ("bit7", "alpha_bit7_diff_max")):
                if st[k] > g[gk]:
                    bad.append(f"buffer {i}: {k} {st[k]:.5f} > {g[gk]}")
            if st["mean"] > g["mean_signed_abs_max"]:
                bad.append(f"buffer {i}: mean signed {st['mean']:.3f}")
        for i, z in enumerate(ZSTATS):
            if z < g["z_within4_min"]:
                bad.append(f"Z {i}: within 4 {z:.5f} < {g['z_within4_min']}")
        print("gates: " + ("PASS" if not bad else "FAIL: " + "; ".join(bad)))
        sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
