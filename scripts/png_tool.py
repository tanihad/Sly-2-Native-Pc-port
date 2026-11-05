"""(light) Small PNG helper without third-party modules (the venv has no PIL): crops, side-by-side pairs and amplified
differences of the game's dumped pictures (8-bit RGB / RGBA PNGs).

    python scripts/png_tool.py crop IN OUT x y w h [scale]          (scale: integer downscale by averaging, default 1)
    python scripts/png_tool.py pair A B OUT x y w h [scale]         (A | B side by side, same crop)
    python scripts/png_tool.py diff A B OUT [gain] [scale]          (|A - B| x gain, whole picture)
    python scripts/png_tool.py stats A B                            (mean / max channel difference, share of pixels > 8)
"""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, idat, w = 8, [], 0
    while pos < len(data):
        n, = struct.unpack('>I', data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if typ == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            assert depth == 8 and ctype in (2, 6), (depth, ctype)
            bpp = 3 if ctype == 2 else 4
        elif typ == b'IDAT':
            idat.append(body)
        pos += 12 + n
    raw = zlib.decompress(b''.join(idat))
    stride = w * bpp
    rows = []
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else b if pb <= pc else c
                line[i] = (line[i] + pr) & 255
        prev = line
        if bpp == 4:
            rgb = bytearray(w * 3)
            rgb[0::3] = line[0::4]; rgb[1::3] = line[1::4]; rgb[2::3] = line[2::4]
            line = rgb
        rows.append(bytes(line))
    return w, h, rows


def write_png(path, w, h, rows):
    raw = b''.join(b'\x00' + r for r in rows)
    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def crop(rows, x, y, w, h, s=1):
    out = []
    for yy in range(y, y + h, s):
        if s == 1:
            out.append(rows[yy][x * 3:(x + w) * 3])
            continue
        line = bytearray()
        for xx in range(x, x + w, s):
            acc = [0, 0, 0]
            for dy in range(s):
                r = rows[yy + dy]
                for dx in range(s):
                    o = (xx + dx) * 3
                    acc[0] += r[o]; acc[1] += r[o + 1]; acc[2] += r[o + 2]
            line += bytes(v // (s * s) for v in acc)
        out.append(bytes(line))
    return out


def main():
    cmd = sys.argv[1]
    a = sys.argv[2:]
    if cmd == 'crop':
        w, h, rows = read_png(a[0])
        x, y, cw, ch = map(int, a[2:6])
        s = int(a[6]) if len(a) > 6 else 1
        out = crop(rows, x, y, cw, ch, s)
        write_png(a[1], len(out[0]) // 3, len(out), out)
    elif cmd == 'pair':
        _, _, ra = read_png(a[0])
        _, _, rb = read_png(a[1])
        x, y, cw, ch = map(int, a[3:7])
        s = int(a[7]) if len(a) > 7 else 1
        ca, cb = crop(ra, x, y, cw, ch, s), crop(rb, x, y, cw, ch, s)
        out = [p + b'\xff\xff\xff' * 4 + q for p, q in zip(ca, cb)]
        write_png(a[2], len(out[0]) // 3, len(out), out)
    elif cmd == 'diff':
        w, h, ra = read_png(a[0])
        _, _, rb = read_png(a[1])
        g = float(a[3]) if len(a) > 3 else 8.0
        s = int(a[4]) if len(a) > 4 else 2
        out = [bytes(min(255, int(abs(p - q) * g)) for p, q in zip(x, y)) for x, y in zip(ra, rb)]
        out = crop(out, 0, 0, w - w % s, h - h % s, s)
        write_png(a[2], len(out[0]) // 3, len(out), out)
    elif cmd == 'stats':
        w, h, ra = read_png(a[0])
        _, _, rb = read_png(a[1])
        tot = mx = big = 0
        for x, y in zip(ra, rb):
            for i in range(0, len(x), 3):
                d = max(abs(x[i] - y[i]), abs(x[i + 1] - y[i + 1]), abs(x[i + 2] - y[i + 2]))
                tot += d; mx = max(mx, d); big += d > 8
        print(f'mean {tot / (w * h):.3f} max {mx} share>8 {100.0 * big / (w * h):.2f}%')


if __name__ == '__main__':
    main()
