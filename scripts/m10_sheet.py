"""M10 contact sheets (no dependencies): python scripts/m10_sheet.py out.png x0,y0,x1,y1 zoom pic... : crops of the same native box (512 x 448 frame) shown as a 16:9 display shows them
(horizontal display pixels per native pixel = zoom * (448 * 16/9) / 512), area-resampled, side by side."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from contact_sheet import read_png, write_png  # noqa: E402

out, box, zoom, pics = sys.argv[1], [int(v) for v in sys.argv[2].split(',')], float(sys.argv[3]), sys.argv[4:]
x0, y0, x1, y1 = box
W = int(round((x1 - x0) * zoom * 448 * 16 / 9 / 512))
H = int(round((y1 - y0) * zoom))
tiles = []
for p in pics:
    w, h, rgb = read_png(Path(p))
    sx, sy = w / 512.0, h / 448.0
    cx0, cx1, cy0, cy1 = x0 * sx, x1 * sx, y0 * sy, y1 * sy
    t = bytearray(W * H * 3)
    for v in range(H):
        a = cy0 + (cy1 - cy0) * v / H
        b = cy0 + (cy1 - cy0) * (v + 1) / H
        ys = range(int(a), max(int(a) + 1, int(b + 0.999)))
        for u in range(W):
            c = cx0 + (cx1 - cx0) * u / W
            d = cx0 + (cx1 - cx0) * (u + 1) / W
            xs = range(int(c), max(int(c) + 1, int(d + 0.999)))
            r = g = bb = n = 0
            for yy in ys:
                if yy >= h:
                    continue
                row = yy * w * 3
                for xx in xs:
                    if xx >= w:
                        continue
                    q = row + xx * 3
                    r += rgb[q]; g += rgb[q + 1]; bb += rgb[q + 2]; n += 1
            if n:
                o = (v * W + u) * 3
                t[o] = r // n; t[o + 1] = g // n; t[o + 2] = bb // n
    tiles.append(t)
gap = 4
TW = W * len(tiles) + gap * (len(tiles) - 1)
img = bytearray(b"\xff" * (TW * H * 3))
for i, t in enumerate(tiles):
    ox = i * (W + gap)
    for v in range(H):
        img[(v * TW + ox) * 3:(v * TW + ox + W) * 3] = t[v * W * 3:(v + 1) * W * 3]
write_png(Path(out), TW, H, bytes(img))
print(out, TW, H)
