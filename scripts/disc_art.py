"""Game art made from the player's own disc image, on the player's PC (notes/disc-art.md).

    python scripts/disc_art.py icon --iso <image> --out sly2.ico [--preview icon.png]
    python scripts/disc_art.py backdrop --iso <image> --exe <console sly2.exe> --out backdrop.bmp [--work <dir>]

- **icon**: the save icon the game writes to the memory card (memcard_icon.ico: a small 3D model of Sly's head with
  a gold "2") is read from its sector of the disc image, checked against its SHA-256, and rendered from the front
  into a multi-size Windows .ico (16..256 px).
- **backdrop**: the freshly built game is run headless for a few seconds (no window, no sound, no controllers, a
  throw-away user folder) and one picture of its opening scene is kept: the letterbox bars are cut off and it is
  written as a 24-bit .bmp for the launcher (installer/launcher.c).

Nothing made here is ever committed or shipped: scripts/build_game.py writes it into the build / install folder.
Each disc this works with has one entry in DISCS (sectors, sizes, hashes, picture times), so another region is
one more entry. Every failure returns False with a reason; callers fall back to the original art
(scripts/make_icon.py, the launcher's own dark theme) - art never fails a build or an install.
"""
import argparse
import hashlib
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import extract_elf  # noqa: E402

SECTOR = 2048

# One entry per supported disc, identified by its boot executable (name + SHA-256, as extract_elf.py checks it).
DISCS = [
    dict(
        region="USA", boot="SCUS_973.16",
        elf_sha256="a511a94b1dbe8cd9bc9b764c34ffda3f88869713a338e188b4f407732fec6a73",
        # memcard_icon.ico as the game copies it to the card: a raw file in the game's own data area
        icon=dict(sector=1169578, size=25688, sha256="708e8fd5acddece90865a3713734b39629fcbabc2d8df07445b7841036bfa08c"),
        # the opening scene: the camera pans over the pyramids of Cairo at sunset (letterboxed, no text). It starts
        # between VBlank ~600 and ~1000 depending on the PC (loading), so it is found by its look: the first
        # letterboxed picture after the black; `after` VBlanks later is the one kept.
        backdrop=dict(first=120, last=2400, every=20, after=60, size="1920x1080"),
    ),
]

ICON_SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)


class ArtError(Exception):
    pass


def log(msg: str) -> None:
    print(f"[art] {msg}", flush=True)


# ---- which disc ------------------------------------------------------------------------------------------

def identify(iso: Path) -> dict:
    """The DISCS entry of this image (its boot executable's name and SHA-256)."""
    try:
        with open(iso, "rb") as f:
            boot = extract_elf.boot_path(f)
            lba, size = extract_elf.find_file(f, boot)
            f.seek(lba * SECTOR)
            digest = hashlib.sha256(f.read(size)).hexdigest()
    except (OSError, extract_elf.DiscError) as e:
        raise ArtError(f"cannot read the disc image: {e}")
    name = boot.strip("\\").split(";")[0].upper()
    for d in DISCS:
        if d["boot"] == name and d["elf_sha256"] == digest:
            return d
    raise ArtError(f"no art table for this disc ({name}, {digest[:16]}...)")


def read_asset(iso: Path, a: dict) -> bytes:
    with open(iso, "rb") as f:
        f.seek(a["sector"] * SECTOR)
        data = f.read(a["size"])
    if len(data) != a["size"] or hashlib.sha256(data).hexdigest() != a["sha256"]:
        raise ArtError(f"the data at sector {a['sector']} is not the expected file")
    return data


# ---- PNG / ICO / BMP writers (stdlib only) ---------------------------------------------------------------

def png_bytes(w: int, h: int, rgba: bytes | bytearray) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def ico_bytes(images: list[tuple[int, bytes]]) -> bytes:
    """A .ico of PNG images (Windows Vista and later read PNG entries at every size)."""
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, blobs = b"", b""
    for s, data in images:
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset + len(blobs))
        blobs += data
    return header + entries + blobs


def write_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)


# ---- the save icon: a PS2 icon model (memcard_icon.ico) --------------------------------------------------

def parse_ps2_icon(data: bytes) -> list[tuple[tuple[float, float, float], tuple[float, float, float], tuple[int, int, int]]]:
    """Vertices (position, normal, colour) of the first shape; three per triangle. PS2 icon format: header
    (magic 0x00010000, shape count, texture type, 1.0f, vertex count), then per vertex one xyzw s16 per shape, a
    normal (s16 x4), uv (s16 x2), rgba8; then the animation and, if texture type bit 2, a 128x128 texture
    (this icon has none: vertex colours only)."""
    if len(data) < 20:
        raise ArtError("icon too short")
    magic, shapes, _tex, _one, nv = struct.unpack_from("<IIIfI", data, 0)
    if magic != 0x00010000 or not 1 <= shapes <= 16 or not 3 <= nv <= 20000 or nv % 3:
        raise ArtError("not a PS2 icon model")
    stride = 8 * shapes + 16
    if 20 + nv * stride > len(data):
        raise ArtError("icon model truncated")
    out, off = [], 20
    for _ in range(nv):
        x, y, z, _w = struct.unpack_from("<4h", data, off)
        nx, ny, nz, _nw = struct.unpack_from("<4h", data, off + 8 * shapes)
        r, g, b, _a = data[off + 8 * shapes + 12:off + 8 * shapes + 16]
        out.append(((x / 4096, y / 4096, z / 4096), (nx / 4096, ny / 4096, nz / 4096), (r, g, b)))
        off += stride
    return out


def _shade(model):
    """Per-vertex lit colours (0..1): a key light from the upper left in front, soft fill, a little rim."""
    L = (-0.38, -0.55, -0.74)                    # towards the light (model space: y down, the viewer at -z)
    ll = math.sqrt(sum(c * c for c in L))
    L = tuple(c / ll for c in L)
    H = (L[0], L[1], L[2] - 1.0)                 # half vector with the view direction (0, 0, -1)
    hl = math.sqrt(sum(c * c for c in H))
    H = tuple(c / hl for c in H)
    cols = []
    for _p, n, c in model:
        nl = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2) or 1.0
        n = (n[0] / nl, n[1] / nl, n[2] / nl)
        d = max(0.0, n[0] * L[0] + n[1] * L[1] + n[2] * L[2])
        s = max(0.0, n[0] * H[0] + n[1] * H[1] + n[2] * H[2]) ** 24
        k = 0.52 + 0.58 * d
        sat = max(c) / 255                                      # bright colours (the face) keep their shading
        base = [min(1.0, (v / 255) * (1.18 - 0.2 * sat)) for v in c]
        cols.append(tuple(min(1.0, b * k + 0.16 * s) for b in base))
    return cols


def _raster(model, cols, n: int, ss: int, margin: float):
    """Coverage and colour at n*ss samples (front view, orthographic, z-buffered, Gouraud)."""
    xs = [p[0][0] for p in model]
    ys = [p[0][1] for p in model]
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    span = max(max(xs) - min(xs), max(ys) - min(ys))
    N = n * ss
    sc = N * (1 - 2 * margin) / span
    W = N
    zb = [1e30] * (W * W)
    cr = [0.0] * (W * W)
    cg = [0.0] * (W * W)
    cbl = [0.0] * (W * W)
    cov = bytearray(W * W)
    P = [((p[0][0] - cx) * sc + N / 2, (p[0][1] - cy) * sc + N / 2, p[0][2]) for p in model]
    for t in range(0, len(P) - 2, 3):
        (ax, ay, az), (bx, by, bz), (qx, qy, qz) = P[t], P[t + 1], P[t + 2]
        den = (by - qy) * (ax - qx) + (qx - bx) * (ay - qy)
        if abs(den) < 1e-12:
            continue
        ca, cb, cc = cols[t], cols[t + 1], cols[t + 2]
        y0 = max(0, math.ceil(min(ay, by, qy) - 0.5))
        y1 = min(W - 1, math.floor(max(ay, by, qy) - 0.5))
        for y in range(y0, y1 + 1):
            py = y + 0.5
            lo, hi = -1e30, 1e30
            ok = True
            for (ex0, ey0, ex1, ey1, ox, oy) in ((ax, ay, bx, by, qx, qy), (bx, by, qx, qy, ax, ay), (qx, qy, ax, ay, bx, by)):
                # half-plane of edge e0->e1 containing the opposite vertex o, intersected with this row
                a_ = ey0 - ey1
                b_ = ex1 - ex0
                c_ = -(a_ * ex0 + b_ * ey0)
                side = a_ * ox + b_ * oy + c_
                k_ = b_ * py + c_                          # a_*x + k_ has the sign of `side` inside
                if side < 0:
                    a_, k_ = -a_, -k_
                if abs(a_) < 1e-12:
                    if k_ < 0:
                        ok = False
                        break
                    continue
                xb = -k_ / a_
                if a_ > 0:
                    lo = max(lo, xb)
                else:
                    hi = min(hi, xb)
            if not ok:
                continue
            xa = max(0, math.ceil(lo - 0.5))
            xe = min(W - 1, math.floor(hi - 0.5))
            if xa > xe:
                continue
            row = y * W
            for x in range(xa, xe + 1):
                px = x + 0.5
                w0 = ((by - qy) * (px - qx) + (qx - bx) * (py - qy)) / den
                w1 = ((qy - ay) * (px - qx) + (ax - qx) * (py - qy)) / den
                w2 = 1.0 - w0 - w1
                z = w0 * az + w1 * bz + w2 * qz
                i = row + x
                if z < zb[i]:
                    zb[i] = z
                    cr[i] = w0 * ca[0] + w1 * cb[0] + w2 * cc[0]
                    cg[i] = w0 * ca[1] + w1 * cb[1] + w2 * cc[1]
                    cbl[i] = w0 * ca[2] + w1 * cb[2] + w2 * cc[2]
                    cov[i] = 1
    return W, cov, cr, cg, cbl


def render_icon(model, cols, n: int) -> bytearray:
    """Straight-alpha RGBA n x n: the model, with a thin dark rim so it reads on light and dark backgrounds."""
    ss = 6 if n <= 32 else 4 if n <= 96 else 3
    margin = 0.06 if n >= 48 else 0.03
    W, cov, cr, cg, cbl = _raster(model, cols, n, ss, margin)
    A = [0.0] * (n * n)
    C = [(0.0, 0.0, 0.0)] * (n * n)
    for oy in range(n):
        for ox in range(n):
            k = r = g = b = 0.0
            for sy in range(ss):
                base = (oy * ss + sy) * W + ox * ss
                for sx in range(ss):
                    i = base + sx
                    if cov[i]:
                        k += 1
                        r += cr[i]
                        g += cg[i]
                        b += cbl[i]
            if k:
                A[oy * n + ox] = k / (ss * ss)
                C[oy * n + ox] = (r / k, g / k, b / k)
    # rim: the silhouette grown by `rad` pixels, drawn behind in a deep night blue
    rad = 0.9 if n <= 24 else 1.2 if n <= 48 else n / 48
    offs = [(dx, dy, max(0.0, min(1.0, rad + 0.5 - math.hypot(dx, dy))))
            for dy in range(-math.ceil(rad), math.ceil(rad) + 1) for dx in range(-math.ceil(rad), math.ceil(rad) + 1)]
    offs = [o for o in offs if o[2] > 0]
    rim = (0.03, 0.04, 0.10)
    out = bytearray(n * n * 4)
    for y in range(n):
        for x in range(n):
            ra = 0.0
            for dx, dy, wgt in offs:
                xx, yy = x + dx, y + dy
                if 0 <= xx < n and 0 <= yy < n:
                    v = A[yy * n + xx] * wgt
                    if v > ra:
                        ra = v
            a = A[y * n + x]
            c = C[y * n + x]
            ra *= 0.92
            oa = a + ra * (1 - a)                     # model over rim
            if oa > 0:
                col = [(c[i] * a + rim[i] * ra * (1 - a)) / oa for i in range(3)]
            else:
                col = [0.0, 0.0, 0.0]
            j = (y * n + x) * 4
            out[j:j + 4] = bytes((*(min(255, round(v * 255)) for v in col), round(oa * 255)))
    return out


def make_icon(iso: Path, out: Path, preview: Path | None = None, disc: dict | None = None) -> bool:
    """sly2.ico from the disc's save icon. False (with a log line) when it cannot be made."""
    t0 = time.time()
    try:
        disc = disc or identify(iso)
        model = parse_ps2_icon(read_asset(iso, disc["icon"]))
        cols = _shade(model)
        images = [(s, render_icon(model, cols, s)) for s in ICON_SIZES]
        write_atomic(out, ico_bytes([(s, png_bytes(s, s, px)) for s, px in images]))
        if preview:
            write_atomic(preview, icon_preview(images))
    except (ArtError, OSError) as e:
        log(f"icon: {e}: using the original icon")
        return False
    log(f"icon: the {disc['region']} disc's save icon, {len(ICON_SIZES)} sizes ({time.time() - t0:.1f} s) -> {out}")
    return True


def icon_preview(images: list[tuple[int, bytearray]]) -> bytes:
    """Every size at 1:1 on a light and a dark strip, then the small ones enlarged 4x (nearest)."""
    sizes = [s for s, _ in images]
    pad = 12
    W = sum(sizes) + pad * (len(sizes) + 1)
    small = [(s, px) for s, px in images if s <= 48]
    W = max(W, sum(s * 4 for s, _ in small) + pad * (len(small) + 1))
    H = (256 + 2 * pad) * 2 + max(s * 4 for s, _ in small) + 2 * pad
    can = bytearray(W * H * 4)
    def band(y0, y1, rgb):
        for y in range(y0, y1):
            can[y * W * 4:(y + 1) * W * 4] = bytes((*rgb, 255)) * W
    band(0, 256 + 2 * pad, (243, 243, 243))
    band(256 + 2 * pad, 2 * (256 + 2 * pad), (32, 32, 32))
    band(2 * (256 + 2 * pad), H, (0x13, 0x1c, 0x36))
    def put(px, s, ox, oy, z=1):
        for y in range(s * z):
            for x in range(s * z):
                j = ((y // z) * s + x // z) * 4
                a = px[j + 3] / 255
                k = ((oy + y) * W + ox + x) * 4
                for c in range(3):
                    can[k + c] = round(px[j + c] * a + can[k + c] * (1 - a))
    for row in range(2):
        x = pad
        for s, px in images:
            put(px, s, x, row * (256 + 2 * pad) + pad + 256 - s)
            x += s + pad
    x = pad
    for s, px in small:
        put(px, s, x, 2 * (256 + 2 * pad) + pad, 4)
        x += s * 4 + pad
    return png_bytes(W, H, can)


# ---- the backdrop: one picture of the opening scene from a headless run of the built game ----------------

def _read_dump_bmp(path: Path):
    """runtime/ps2_runtime.c PS2_DUMP_BMP: 32-bit BGRX, top-down (negative height)."""
    d = path.read_bytes()
    if d[:2] != b"BM":
        raise ArtError(f"{path.name}: not a BMP")
    off, = struct.unpack_from("<I", d, 10)
    w, h = struct.unpack_from("<ii", d, 18)
    bpp, = struct.unpack_from("<H", d, 28)
    if bpp != 32 or w <= 0 or h == 0:
        raise ArtError(f"{path.name}: unexpected format")
    top_down = h < 0
    h = abs(h)
    rows = [d[off + y * w * 4: off + (y + 1) * w * 4] for y in range(h)]
    if not top_down:
        rows.reverse()
    return w, h, rows


def _row_mean(r) -> float:
    """Mean luminance of a BGRX row (every 4th pixel)."""
    return (sum(r[2::16]) * 3 + sum(r[1::16]) * 6 + sum(r[0::16])) / (10 * len(r[0::16]))


def _letterbox(w: int, h: int, rows):
    """(top, bottom, mean luminance) of a letterboxed picture (black bars above and below, a lit scene between),
    else None."""
    top = 0
    while top < h // 3 and _row_mean(rows[top]) < 5:
        top += 1
    bot = h
    while bot > h - h // 3 and _row_mean(rows[bot - 1]) < 5:
        bot -= 1
    if top < h * 0.06 or h - bot < h * 0.06:     # no bars: a movie, a menu, a loading screen
        return None
    top += max(2, h // 360)                      # (the bars' soft edge)
    bot -= max(2, h // 360)
    lum = [_row_mean(rows[y]) for y in range(top, bot, max(1, (bot - top) // 48))]
    mean = sum(lum) / len(lum)
    if mean < 25 or mean > 200:                  # black, a fade, or a flash
        return None
    return top, bot, mean


def write_bmp24(path: Path, w: int, rows) -> None:
    """Bottom-up 24-bit BMP (LoadImage reads it) from BGRX rows."""
    stride = (w * 3 + 3) & ~3
    h = len(rows)
    body = bytearray(stride * h)
    for i, r in enumerate(reversed(rows)):
        line = bytearray(w * 3)
        line[0::3] = r[0::4]
        line[1::3] = r[1::4]
        line[2::3] = r[2::4]
        body[i * stride:i * stride + w * 3] = line
    hdr = struct.pack("<2sIHHI", b"BM", 54 + len(body), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(body), 2835, 2835, 0, 0)
    write_atomic(path, hdr + info + bytes(body))


def make_backdrop(iso: Path, exe: Path, out: Path, work: Path | None = None, timeout: float = 150.0,
                  disc: dict | None = None) -> bool:
    """backdrop.bmp from a headless run of the built game (console build). False when it cannot be made."""
    t0 = time.time()
    own_work = work is None
    work = Path(work or tempfile.mkdtemp(prefix="sly2-art-"))
    proc = None
    try:
        disc = disc or identify(iso)
        b = disc["backdrop"]
        if not exe.is_file():
            raise ArtError(f"{exe} is missing")
        frames, user = work / "frames", work / "user"
        for p in (frames, user):
            shutil.rmtree(p, ignore_errors=True)
            p.mkdir(parents=True)
        # the throw-away settings: sharp (no motion blur while the camera pans), 16:9, the GPU renderer as default
        (user / "sly2.ini").write_text("[graphics]\nmotion_blur = off\n", encoding="ascii")
        every = b["every"]
        env = {k: v for k, v in os.environ.items() if not k.startswith("PS2_")}
        env.update(PS2_HEADLESS="1", PS2_AUDIO_NULL="1", PS2_INPUT_DEVICES="0", PS2_USER_DIR=str(user),
                   PS2_DUMP_FRAMES=str(frames), PS2_DUMP_BMP="1", PS2_DUMP_EVERY=str(every), PS2_DUMP_FROM=str(b["first"]),
                   PS2_DUMP_TO=str(b["last"]), PS2_GS_PRESENT_SIZE=b["size"], PS2_WATCHDOG=str(int(timeout)))
        # the copy runs under its own name: tools that stop "sly2.exe" by name never catch it, and it never
        # catches theirs
        run_exe = work / "sly2-art-capture.exe"
        shutil.copy2(exe, run_exe)
        log("backdrop: running the game headless (no window, sound or controllers) for a picture of its opening scene")
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) | getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0)
        start = best = None
        seen_other = False
        with open(work / "run.log", "wb") as lf:
            proc = subprocess.Popen([str(run_exe), str(iso)], cwd=str(work), env=env, stdout=lf, stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL, creationflags=flags)
            v = b["first"] - b["first"] % every
            if v < b["first"]:
                v += every
            # each picture is looked at once the next one exists (then it is complete) and deleted at once, so the
            # run never holds more than two of them (~8 MB each)
            while best is None and v <= b["last"] and time.time() - t0 < timeout:
                f, nxt = frames / f"frame_{v:06d}.bmp", frames / f"frame_{v + every:06d}.bmp"
                if not nxt.exists():
                    if proc.poll() is not None:
                        break
                    time.sleep(0.1)
                    continue
                w, h, rows = _read_dump_bmp(f)
                box = _letterbox(w, h, rows)
                if start is None and box and seen_other:
                    start = v                    # the scene's first picture (after the black before it)
                seen_other |= not box
                if start is not None and v >= start + b["after"]:
                    if not box:
                        raise ArtError(f"the opening scene ended early (VBlank {start}..{v})")
                    best = (v, w, rows[box[0]:box[1]], box[2])
                f.unlink()
                v += every
            if proc.poll() is None:
                proc.kill()
                proc.wait(10)
        if not best:
            raise ArtError("the game did not reach its opening scene in time")
        v, w, rows, mean = best
        cut = max(4, w // 240)                   # (a stray column at the picture's edges)
        write_bmp24(out, w - 2 * cut, [r[cut * 4:(w - cut) * 4] for r in rows])
        log(f"backdrop: VBlank {v} (scene from {start}), {w - 2 * cut}x{len(rows)}, brightness {mean:.0f} "
            f"({time.time() - t0:.0f} s) -> {out}")
        return True
    except (ArtError, OSError, subprocess.SubprocessError, ValueError) as e:
        log(f"backdrop: {e}: the launcher uses its own theme")
        return False
    finally:
        if proc and proc.poll() is None:
            proc.kill()
        # nothing of the run is kept: frames (~15 MB each), its card and settings (SLY2_ART_KEEP=1: debugging)
        for p in () if os.environ.get("SLY2_ART_KEEP") == "1" else ("frames", "user", "sly2-art-capture.exe"):
            q = work / p
            if q.is_dir():
                shutil.rmtree(q, ignore_errors=True)
            elif q.exists():
                try:
                    q.unlink()
                except OSError:
                    pass
        if own_work:
            shutil.rmtree(work, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="what", required=True)
    a = sub.add_parser("icon")
    a.add_argument("--iso", required=True)
    a.add_argument("--out", required=True)
    a.add_argument("--preview")
    b = sub.add_parser("backdrop")
    b.add_argument("--iso", required=True)
    b.add_argument("--exe", required=True)
    b.add_argument("--out", required=True)
    b.add_argument("--work")
    b.add_argument("--timeout", type=float, default=150.0)
    args = ap.parse_args(argv)
    if args.what == "icon":
        ok = make_icon(Path(args.iso), Path(args.out), Path(args.preview) if args.preview else None)
    else:
        ok = make_backdrop(Path(args.iso), Path(args.exe), Path(args.out), Path(args.work) if args.work else None,
                           args.timeout)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
