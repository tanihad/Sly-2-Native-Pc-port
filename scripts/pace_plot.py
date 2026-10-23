"""Frame-time plot of PS2_PACE_LOG runs (pc-pacing): one panel per run, the interval between consecutive pictures as the
display showed them ("shown present" lines, DXGI frame statistics) or, offscreen, between Present calls - one dot per
picture, a line at 16.68 ms (the PS2's VBlank) and faint lines at the display's refresh multiples. Pure Python (zlib PNG).

    python scripts/pace_plot.py out.png "label=build/pace/R1/run.log" ["label2=..."] [--from 900] [--count 600] [--refresh 165]
"""
import re
import struct
import sys
import zlib

W, PH, M = 900, 170, 6          # width, panel height, margin
YMAX = 40.0                     # ms at the top of a panel

FONT = {  # 3x5 digits and a few letters for labels
    '0': "111101101101111", '1': "010110010010111", '2': "111001111100111", '3': "111001111001111", '4': "101101111001001",
    '5': "111100111001111", '6': "111100111101111", '7': "111001001001001", '8': "111101111101111", '9': "111101111001111",
    '.': "000000000000010", ' ': "000000000000000", 'm': "101111111101101", 's': "011100010001110", '-': "000000111000000",
    'x': "000101010101000", ':': "000010000010000", '%': "101001010100101", '/': "001001010100100",
    'a': "010101111101101", 'b': "110101110101110", 'c': "011100100100011", 'd': "110101101101110", 'e': "111100110100111",
    'f': "111100110100100", 'g': "011100101101011", 'h': "101101111101101", 'i': "111010010010111", 'j': "001001001101010",
    'k': "101101110101101", 'l': "100100100100111", 'n': "110101101101101", 'o': "010101101101010",
    'p': "110101110100100", 'q': "010101101110011", 'r': "110101110101101", 't': "111010010010010", 'u': "101101101101111",
    'v': "101101101101010", 'w': "101101101111101", 'y': "101101010010010", 'z': "111001010100111", '(': "001010010010001",
    ')': "100010010010100", '+': "000010111010000", '=': "000111000111000", '<': "001010100010001", '>': "100010001010100",
}


def glyph(ch):
    return FONT.get(ch.lower(), "111101101101111" if ch.isalpha() else FONT[' '])


def parse(path, first, count):
    shown = re.compile(r"pace: shown present \d+ \(v(\d+)\).* interval ([-\d.]+) ms")
    off = re.compile(r"pace: offscreen present v(\d+):.* interval ([-\d.]+) ms")
    out = []
    with open(path, errors="replace") as f:
        for line in f:
            m = shown.search(line) or off.search(line)
            if m and int(m.group(1)) >= first and float(m.group(2)) > 0:
                out.append(float(m.group(2)))
                if len(out) >= count:
                    break
    return out


def main():
    args = sys.argv[1:]
    out = args[0]
    first, count, refresh = 900, 600, 165.0
    runs = []
    i = 1
    while i < len(args):
        if args[i] == "--from":
            first = int(args[i + 1]); i += 2
        elif args[i] == "--count":
            count = int(args[i + 1]); i += 2
        elif args[i] == "--refresh":
            refresh = float(args[i + 1]); i += 2
        else:
            label, path = args[i].split("=", 1)
            runs.append((label, parse(path, first, count)))
            i += 1
    H = PH * len(runs)
    px = bytearray(b"\xff" * (W * H * 3))

    def put(x, y, c):
        if 0 <= x < W and 0 <= y < H:
            o = (y * W + x) * 3
            px[o:o + 3] = bytes(c)

    def text(x, y, s, c=(0, 0, 0)):
        for ch in s:
            g = glyph(ch)
            for r in range(5):
                for k in range(3):
                    if g[r * 3 + k] == "1":
                        for dy in range(2):
                            for dx in range(2):
                                put(x + k * 2 + dx, y + r * 2 + dy, c)
            x += 8

    for p, (label, iv) in enumerate(runs):
        y0 = p * PH
        def ymap(ms):
            return y0 + PH - M - int((PH - 2 * M) * min(ms, YMAX) / YMAX)
        for x in range(W):
            put(x, y0, (120, 120, 120))
        k = 1
        while k * 1000.0 / refresh <= YMAX:          # the display's refresh grid
            yy = ymap(k * 1000.0 / refresh)
            for x in range(0, W, 3):
                put(x, yy, (200, 200, 200))
            k += 1
        yy = ymap(1000.0 * 1001.0 / 60000.0)
        for x in range(W):
            put(x, yy, (0, 160, 0))
        n = len(iv)
        for j, v in enumerate(iv):
            x = M + int((W - 2 * M) * j / max(1, count - 1))
            y = ymap(v)
            col = (30, 60, 200) if abs(v - 16.683) < 1.0 else (210, 40, 40)
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    put(x + dx, y + dy, col)
        within = sum(1 for v in iv if abs(v - 16.683) < 1.0)
        text(M + 2, y0 + 4, f"{label} {n} {100.0 * within / max(1, n):.0f}% in 16.7-1ms")
    raw = b"".join(b"\x00" + bytes(px[y * W * 3:(y + 1) * W * 3]) for y in range(H))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(out, "wb") as f:
        f.write(png)
    print(f"{out}: {len(runs)} panels, {len(png)} bytes")


if __name__ == "__main__":
    main()
