"""Options-menu pictures from tests/unit/menu (test_menu.exe --shots): raw premultiplied BGRA overlays -> PNGs over a
stand-in backdrop, and a contact sheet. No dependencies (zlib only).

    python scripts/menu_shots.py <raw dir> <png dir> <sheet dir>

Writes <png dir>/pc_menu_<page>_<size>.png (the overlay as every renderer shows it, over a dark backdrop where the
game's picture would be) and <sheet dir>/contact_sheet.png (every page at 2560x1440, scaled down, plus the
720p / 4:3 / 800x600 Graphics pages)."""
import struct
import sys
import zlib
from pathlib import Path


def backdrop(w: int, h: int) -> bytearray:
    """a dusk-blue gradient with a faint grid: where the game picture would be"""
    out = bytearray(w * h * 3)
    for y in range(h):
        t = y / max(1, h - 1)
        r, g, b = int(40 + 50 * t), int(70 + 40 * t), int(110 - 30 * t)
        row = bytes((r, g, b)) * w
        if y % 64 == 0:
            row = bytes((r + 25, g + 25, b + 25)) * w
        out[y * w * 3:(y + 1) * w * 3] = row
    for x in range(0, w, 64):
        for y in range(h):
            i = (y * w + x) * 3
            out[i:i + 3] = bytes((min(255, out[i] + 25), min(255, out[i + 1] + 25), min(255, out[i + 2] + 25)))
    return out


def composite(raw: bytes, w: int, h: int) -> bytearray:
    rgb = backdrop(w, h)
    for i in range(w * h):
        b, g, r, a = raw[4 * i], raw[4 * i + 1], raw[4 * i + 2], raw[4 * i + 3]
        if a == 0:
            continue
        inv = 255 - a
        j = 3 * i
        rgb[j] = min(255, r + (rgb[j] * inv + 127) // 255)
        rgb[j + 1] = min(255, g + (rgb[j + 1] * inv + 127) // 255)
        rgb[j + 2] = min(255, b + (rgb[j + 2] * inv + 127) // 255)
    return rgb


def write_png(path: Path, w: int, h: int, rgb: bytes) -> None:
    raw = b"".join(b"\x00" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(t: bytes, d: bytes) -> bytes:
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def shrink(rgb: bytes, w: int, h: int, f: int) -> tuple[bytearray, int, int]:
    nw, nh = w // f, h // f
    out = bytearray(nw * nh * 3)
    n = f * f
    for y in range(nh):
        for x in range(nw):
            s = [0, 0, 0]
            for dy in range(f):
                base = ((y * f + dy) * w + x * f) * 3
                for dx in range(f):
                    k = base + dx * 3
                    s[0] += rgb[k]; s[1] += rgb[k + 1]; s[2] += rgb[k + 2]
            j = (y * nw + x) * 3
            out[j], out[j + 1], out[j + 2] = s[0] // n, s[1] // n, s[2] // n
    return out, nw, nh


def main() -> int:
    raw_dir, png_dir, sheet_dir = (Path(a) for a in sys.argv[1:4])
    png_dir.mkdir(parents=True, exist_ok=True)
    sheet_dir.mkdir(parents=True, exist_ok=True)
    tiles = []
    order = ["graphics", "display", "audio", "controls", "keyboard", "controller", "reset"]
    for f in sorted(raw_dir.glob("*.bgra")):
        data = f.read_bytes()
        w, h = struct.unpack("<II", data[:8])
        rgb = composite(data[8:], w, h)
        write_png(png_dir / (f.stem + ".png"), w, h, rgb)
        name = f.stem.replace("pc_menu_", "")
        page, size = name.rsplit("_", 1)
        if size == "1440p" or (page == "graphics" and size != "1440p"):
            factor = 4 if size == "1440p" else 2
            s, sw, sh = shrink(rgb, w, h, factor)
            key = (0 if size == "1440p" else 1, order.index(page) if page in order else 9, size)
            tiles.append((key, s, sw, sh))
        print(f"{f.stem}.png {w}x{h}")
    tiles.sort(key=lambda t: t[0])
    cols, tw, th = 4, 640, 384
    rows = (len(tiles) + cols - 1) // cols
    sheet = bytearray(bytes((24, 24, 28)) * (cols * tw * rows * th))
    for i, (_, s, sw, sh) in enumerate(tiles):
        ox, oy = (i % cols) * tw, (i // cols) * th
        for y in range(min(sh, th)):
            d = ((oy + y) * cols * tw + ox) * 3
            sheet[d:d + min(sw, tw) * 3] = s[y * sw * 3:y * sw * 3 + min(sw, tw) * 3]
    write_png(sheet_dir / "contact_sheet.png", cols * tw, rows * th, sheet)
    print(f"contact sheet: {len(tiles)} pictures -> {sheet_dir / 'contact_sheet.png'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
