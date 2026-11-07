"""(ai) Original | Enhanced | AI pictures from two tex_shots.ps1 A/B runs of a scene (notes/ai-textures.md).

    python -I scripts/ai_shots.py <scene> <enh run dir> <ai run dir> <out dir> --a <vblank> --b <vblank>
                                  [--ref <closeup.png of build\\tex\\shots\\final>] [--region x,y,w,h] [--tag name]

The runs are tex_shots.ps1 A/B runs (A = Original, B = Enhanced textures): <enh run> without an HD pack, <ai run> with
the AI pack in the user dir's textures folder (B = AI). Writes <scene>_original/_enhanced/_ai.png (2560 x 1440),
<scene>_compare3.png (the middle 853 columns of each side by side: Original | Enhanced | AI) and <scene>_closeup3.png
(a 640 x 720 region of each at 2x). The close-up region is either given or found by matching the Original half of a
reference close-up (the Enhanced-textures pictures' regions). Also prints how far the two runs' Original pictures are
apart (the same scene?). Standard library only.
"""
import shutil
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import png_tool  # noqa: E402


def load(p):
    w, h, rows = png_tool.read_png(str(p))
    return w, h, rows


def write(path, w, h, rows):
    png_tool.write_png(str(path), w, h, rows)


def diff_stats(a, b):
    tot = n = big = 0
    for ra, rb in zip(a[2][::4], b[2][::4]):
        for i in range(0, len(ra), 12):
            d = abs(ra[i] - rb[i]) + abs(ra[i + 1] - rb[i + 1]) + abs(ra[i + 2] - rb[i + 2])
            tot += d; n += 1; big += d > 24
    return tot / (3 * n), big / n


def down(img, s):
    w, h, rows = img
    W, H = w // s, h // s
    out = []
    for y in range(H):
        r = rows[y * s + s // 2]
        out.append([sum(r[(x * s + s // 2) * 3:(x * s + s // 2) * 3 + 3]) for x in range(W)])
    return W, H, out


def find_region(frame, ref):
    """(x, y) of the 640 x 720 Original crop whose 2x is the left half of ref (2560 x 1440)."""
    rw, rh, rrows = ref
    # the reference's left half at frame scale: 640 x 720 (every 2nd pixel)
    half = (640, 720, [bytes(r[c] for x in range(0, 1280, 2) for c in (x * 3, x * 3 + 1, x * 3 + 2)) for r in rrows[0::2]])
    s = 16
    F = down(frame, s)
    T = down(half, s)
    best = None
    for y in range(0, F[1] - T[1] + 1):
        for x in range(0, F[0] - T[0] + 1):
            e = 0
            for ty in range(0, T[1], 2):
                fr, tr = F[2][y + ty], T[2][ty]
                for tx in range(0, T[0], 2):
                    e += abs(fr[x + tx] - tr[tx])
            if best is None or e < best[0]:
                best = (e, x * s, y * s)
    # refine at full resolution (sparse samples), +-16 pixels
    _, bx, by = best
    fw, fh, frows = frame
    best = None
    for y in range(max(0, by - 16), min(fh - 720, by + 16) + 1, 2):
        for x in range(max(0, bx - 16), min(fw - 640, bx + 16) + 1, 2):
            e = 0
            for ty in range(0, 720, 24):
                fr, tr = frows[y + ty], half[2][ty]
                for tx in range(0, 640, 24):
                    e += abs(fr[(x + tx) * 3] - tr[tx * 3]) + abs(fr[(x + tx) * 3 + 1] - tr[tx * 3 + 1])
            if best is None or e < best[0]:
                best = (e, x, y)
    return best[1], best[2]


def crop2x(img, x, y, w, h):
    rows = []
    for yy in range(y, y + h):
        r = img[2][yy][x * 3:(x + w) * 3]
        d = bytearray(w * 6)
        d[0::6], d[1::6], d[2::6] = r[0::3], r[1::3], r[2::3]
        d[3::6], d[4::6], d[5::6] = r[0::3], r[1::3], r[2::3]
        rows += [bytes(d), bytes(d)]
    return rows


def side(parts, sep=4):
    out = []
    for rs in zip(*parts):
        line = bytearray()
        for i, r in enumerate(rs):
            if i:
                line += b'\xff' * (3 * sep)
            line += r
        out.append(bytes(line))
    return out


def main(argv):
    args = argv[1:]
    opt = {}
    pos = []
    i = 0
    while i < len(args):
        if args[i].startswith('--'):
            opt[args[i][2:]] = args[i + 1]
            i += 2
        else:
            pos.append(args[i])
            i += 1
    scene, enh, ai, out = pos[0], Path(pos[1]), Path(pos[2]), Path(pos[3])
    out.mkdir(parents=True, exist_ok=True)
    tag = opt.get('tag', scene)
    fa, fb = int(opt['a']), int(opt['b'])
    o_e = load(enh / f'frame_{fa:06d}.png')
    o_a = load(ai / f'frame_{fa:06d}.png')
    e = load(enh / f'frame_{fb:06d}.png')
    a = load(ai / f'frame_{fb:06d}.png')
    m, big = diff_stats(o_e, o_a)
    print(f'{tag}: the two runs\' Original pictures (v{fa}) differ by {m:.2f} / 255 on average, {100 * big:.1f} % of pixels > 8')
    shutil.copy(enh / f'frame_{fa:06d}.png', out / f'{tag}_original.png')
    shutil.copy(enh / f'frame_{fb:06d}.png', out / f'{tag}_enhanced.png')
    shutil.copy(ai / f'frame_{fb:06d}.png', out / f'{tag}_ai.png')
    W = o_e[0]
    cw = 852
    x0 = (W - cw) // 2
    write(out / f'{tag}_compare3.png', 3 * cw + 8, o_e[1],
          side([[r[x0 * 3:(x0 + cw) * 3] for r in im[2]] for im in (o_e, e, a)]))
    if 'region' in opt:
        x, y, w, h = (int(v) for v in opt['region'].split(','))
    elif 'ref' in opt:
        x, y = find_region(o_e, load(opt['ref']))
        w, h = 640, 720
    else:
        x, y, w, h = (W - 640) // 2, (o_e[1] - 720) // 2, 640, 720
    print(f'{tag}: close-up region {x},{y} 640x720')
    write(out / f'{tag}_closeup3.png', 3 * 2 * w + 8, 2 * h, side([crop2x(im, x, y, w, h) for im in (o_e, e, a)]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
