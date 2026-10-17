"""Picture checks of an in-game sweep scene (M8, scripts/m8_sweep.ps1), no dependencies.

    python scripts/m8_check.py <run dir>\\frames [--step 2] [--flag 26]

For every VBlank v with frame_<v>.png (the GPU renderer's picture at scale) and the verifier's pair_<v>_sw.png /
pair_<v>_hw.png (native reference composes of the software renderer's and the GPU's GS memory):
  native  PSNR of pair_hw vs pair_sw (the 10.2 picture gate's measure: >= 40 dB expected) and pixels > 16;
  scaled  PSNR of the picture at scale box-downsampled to the native grid (the native pixel's own samples) vs pair_hw,
          on every --step-th pixel. The reference has the PS2's flicker filter and field display while the GPU's
          picture is composed with the graphics settings (Modern: progressive, no flicker filter), so this is a coarse
          check (typically 28-40 dB) that flags gross errors - missing layers, offsets, wrong colours - below --flag dB.
"""
import argparse
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from contact_sheet import read_png  # noqa: E402


def psnr_native(a, b, w, h):
    se = 0
    big = 0
    for i in range(0, w * h * 3, 3):
        d0, d1, d2 = a[i] - b[i], a[i + 1] - b[i + 1], a[i + 2] - b[i + 2]
        se += d0 * d0 + d1 * d1 + d2 * d2
        if max(abs(d0), abs(d1), abs(d2)) > 16:
            big += 1
    mse = se / (3.0 * w * h)
    return (10 * math.log10(255 * 255 / mse) if mse else 999.0), big


def psnr_scaled(ref, rw, rh, pic, pw, ph, step):
    kx, ky = pw // rw, ph // rh
    se = 0
    n = 0
    for y in range(0, rh, step):
        for x in range(0, rw, step):
            acc = [0, 0, 0]
            for j in range(ky):
                p = ((y * ky + j) * pw + x * kx) * 3
                for _ in range(kx):
                    acc[0] += pic[p]; acc[1] += pic[p + 1]; acc[2] += pic[p + 2]
                    p += 3
            q = (y * rw + x) * 3
            m = kx * ky
            for c in range(3):
                d = acc[c] / m - ref[q + c]
                se += d * d
            n += 3
    mse = se / n
    return 10 * math.log10(255 * 255 / mse) if mse else 999.0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("frames")
    ap.add_argument("--step", type=int, default=2)
    ap.add_argument("--flag", type=float, default=26.0)
    a = ap.parse_args()
    d = Path(a.frames)
    flagged = 0
    rows = 0
    worst_n, worst_s = 999.0, 999.0
    for f in sorted(d.glob("frame_*.png")):
        v = f.stem[6:]
        ps, ph = d / f"pair_{v}_sw.png", d / f"pair_{v}_hw.png"
        if not ps.exists() or not ph.exists():
            continue
        sw_w, sw_h, sw = read_png(ps)
        hw_w, hw_h, hw = read_png(ph)
        pw, phh, pic = read_png(f)
        if (sw_w, sw_h) != (hw_w, hw_h):
            print(f"v{v}: pair sizes differ {sw_w}x{sw_h} vs {hw_w}x{hw_h}  FLAG")
            flagged += 1
            continue
        pn, big = psnr_native(sw, hw, sw_w, sw_h)
        if pw % sw_w or phh % sw_h:
            print(f"v{v}: native {pn:.2f} dB ({big} px > 16); picture {pw}x{phh} not a multiple of {sw_w}x{sw_h}  FLAG")
            flagged += 1
            continue
        pss = psnr_scaled(hw, hw_w, hw_h, pic, pw, phh, a.step)
        flag = pn < 40.0 or big * 500 > sw_w * sw_h or pss < a.flag
        flagged += flag
        rows += 1
        worst_n, worst_s = min(worst_n, pn), min(worst_s, pss)
        print(f"v{v}: native hw vs sw {pn:.2f} dB ({big} px > 16); scaled {pw}x{phh} vs native ref {pss:.2f} dB{'  FLAG' if flag else ''}")
    print(f"check: {rows} pictures, {flagged} flagged; worst native {worst_n:.2f} dB, worst scaled {worst_s:.2f} dB")
    return 1 if flagged else 0


if __name__ == "__main__":
    sys.exit(main())
