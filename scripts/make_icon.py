"""Draw the PC build's application icon: an original picture (a thief's mask under a crescent moon
on a night-blue tile), rendered procedurally at each icon size, written as a .ico with PNG images.

No game artwork is used; nothing generated here is committed (scripts/build_game.py --package
writes it into the build folder).

Usage: python scripts/make_icon.py <out.ico>
"""
import math
import struct
import sys
import zlib
from pathlib import Path

SIZES = (16, 24, 32, 48, 64, 128, 256)
SS = 4                                          # supersampling: SS x SS samples per pixel


def _ellipse(x, y, cx, cy, rx, ry, tilt=0.0):
    c, s = math.cos(tilt), math.sin(tilt)
    dx, dy = x - cx, y - cy
    u, v = dx * c + dy * s, -dx * s + dy * c
    return (u / rx) ** 2 + (v / ry) ** 2 <= 1.0


def _rounded_square(x, y, r):
    qx, qy = abs(x - 0.5) - (0.5 - r), abs(y - 0.5) - (0.5 - r)
    if qx <= 0 or qy <= 0:
        return max(qx, qy) <= r
    return qx * qx + qy * qy <= r * r


def _sample(x, y):
    """RGBA (0..1) of the picture at (x, y) in [0, 1]^2."""
    if not _rounded_square(x, y, 0.2):
        return (0.0, 0.0, 0.0, 0.0)
    t = y                                        # night sky: deep blue at the top, lighter below
    col = (0.08 + 0.10 * t, 0.12 + 0.18 * t, 0.36 + 0.30 * t)
    # crescent moon, top right
    if _ellipse(x, y, 0.75, 0.25, 0.13, 0.13) and not _ellipse(x, y, 0.81, 0.20, 0.12, 0.12):
        col = (1.0, 0.85, 0.35)
    # the mask: two lobes and a bridge, near-black
    mask = (_ellipse(x, y, 0.33, 0.58, 0.22, 0.15, -0.20) or _ellipse(x, y, 0.67, 0.58, 0.22, 0.15, 0.20)
            or (0.33 <= x <= 0.67 and 0.50 <= y <= 0.63))
    if mask:
        col = (0.07, 0.07, 0.09)
        # eye holes, slanted
        if _ellipse(x, y, 0.35, 0.57, 0.095, 0.05, -0.30) or _ellipse(x, y, 0.65, 0.57, 0.095, 0.05, 0.30):
            col = (0.97, 0.97, 1.0)
    return (*col, 1.0)


def render(size: int) -> bytes:
    """Straight-alpha RGBA rows."""
    out = bytearray()
    for py in range(size):
        for px in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(SS):
                for sx in range(SS):
                    r, g, b, a = _sample((px + (sx + 0.5) / SS) / size, (py + (sy + 0.5) / SS) / size)
                    acc[0] += r * a; acc[1] += g * a; acc[2] += b * a; acc[3] += a
            a = acc[3] / (SS * SS)
            if a > 0:
                rgb = [min(255, round(c / acc[3] * 255)) for c in acc[:3]]
            else:
                rgb = [0, 0, 0]
            out += bytes((*rgb, round(a * 255)))
    return bytes(out)


def png(size: int, rgba: bytes) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + rgba[y * size * 4:(y + 1) * size * 4] for y in range(size))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def make_ico(path: Path) -> None:
    images = [(s, png(s, render(s))) for s in SIZES]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, blobs = b"", b""
    for s, data in images:
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset + len(blobs))
        blobs += data
    path.write_bytes(header + entries + blobs)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    make_ico(Path(sys.argv[1]))
