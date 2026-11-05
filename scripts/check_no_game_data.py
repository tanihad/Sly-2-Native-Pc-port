"""Prove that what we ship contains nothing of the game: no picture, icon, sound or movie, and none of the disc's files.

    python scripts/check_no_game_data.py [--allow <Sly2-Setup.art-allow.txt>] <sly2-source-*.zip | Sly2-Setup*.exe | folder> ...

Game art (the icon and backdrop made from the player's disc by scripts/disc_art.py, the owner's own pictures in
%LOCALAPPDATA%\\Sly2\\art) exists only on the player's PC; the sources and a release bundle carry none, and Setup
none either - except a Setup built with `make_installer.py --art <folder>` (the owner's release Setup): its program
carries the owner's backdrop and logo (RCDATA) and the icon made from that logo, and make_installer writes their
SHA-256 to Sly2-Setup.art-allow.txt. With --allow <that file> exactly those resources are allowed; anything else
(another picture, an icon that is neither ours nor on the list, a picture in the payload) still fails.
For a Setup exe the program's resources are checked (the icon must be scripts/make_icon.py's or on the allow-list),
then the payload (SLY2PAY1, scripts/make_installer.py) is opened and source.zip checked inside it;
toolchain archives of the offline variant are third-party downloads: only their hashes are checked.

Checked for every file: (1) no media file name (.png .bmp .ico .jpg .webp .gif .tga .dds .tif .pss .vag .wav ...),
(2) no picture by its content (PNG, BMP, ICO / PS2 icon model, JPEG, GIF, WebP, TIFF), (3) its SHA-256 is none of
the disc files we know (the boot executable, the save icon of every disc in scripts/disc_art.py's table).
Exit code 0 = clean, 1 = something found (each listed), 2 = cannot read.
"""
import hashlib
import io
import struct
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import disc_art  # noqa: E402
import extract_elf  # noqa: E402

MEDIA_EXT = {".png", ".bmp", ".ico", ".icns", ".cur", ".jpg", ".jpeg", ".webp", ".gif", ".tga", ".dds", ".tif", ".tiff",
             ".psd", ".svg", ".pss", ".mpg", ".m2v", ".vag", ".vgs", ".wav", ".ogg", ".mp3", ".at3", ".iso", ".img",
             ".bin", ".elf", ".irx", ".tex", ".tm2", ".wac", ".wal"}
KNOWN = {extract_elf.EXPECTED_SHA256: "the game's executable (SCUS_973.16)"}
for d in disc_art.DISCS:
    KNOWN[d["elf_sha256"]] = f"the {d['region']} disc's executable"
    KNOWN[d["icon"]["sha256"]] = f"the {d['region']} disc's save icon (memcard_icon.ico)"


def picture_kind(data: bytes) -> str | None:
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return "PNG"
    if data[:3] == b"\xff\xd8\xff":
        return "JPEG"
    if data[:6] in (b"GIF87a", b"GIF89a"):
        return "GIF"
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "WebP"
    if data[:4] in (b"II*\0", b"MM\0*"):
        return "TIFF"
    if data[:2] == b"BM" and len(data) >= 18 and struct.unpack_from("<I", data, 14)[0] in (12, 40, 52, 56, 108, 124):
        return "BMP"
    if data[:4] == b"\0\0\1\0" and len(data) >= 6:
        n = struct.unpack_from("<H", data, 4)[0]
        if 0 < n < 64 or data[4:8] in (b"\1\0\0\0",):  # .ico directory, or a PS2 icon model (shape count 1)
            return "icon"
    return None


def check_blob(name: str, data: bytes, problems: list[str], media_check: bool = True) -> None:
    digest = hashlib.sha256(data).hexdigest()
    if digest in KNOWN:
        problems.append(f"{name}: is {KNOWN[digest]}")
    if not media_check:
        return
    ext = Path(name).suffix.lower()
    if ext in MEDIA_EXT:
        problems.append(f"{name}: a media / disc file name ({ext})")
    kind = picture_kind(data)
    if kind:
        problems.append(f"{name}: contains a {kind} picture")


def check_zip(label: str, data: bytes, problems: list[str]) -> int:
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        n = 0
        for info in z.infolist():
            if info.is_dir():
                continue
            check_blob(f"{label}:{info.filename}", z.read(info), problems)
            n += 1
    return n


RT_NAMES = {3: "ICON", 10: "RCDATA", 14: "GROUP_ICON", 16: "VERSION", 24: "MANIFEST"}


def pe_resources(data: bytes) -> list[tuple[str, str, bytes]]:
    """(type, name, bytes) of every resource of a PE file (the setup program before its payload)."""
    if data[:2] != b"MZ":
        raise ValueError("not a Windows program")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a Windows program")
    nsec, = struct.unpack_from("<H", data, pe + 6)
    optsize, = struct.unpack_from("<H", data, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from("<H", data, opt)
    dirs = opt + (112 if magic == 0x20B else 96)
    rva, _size = struct.unpack_from("<II", data, dirs + 2 * 8)     # data directory 2: resources
    if not rva:
        return []
    secs = []
    for i in range(nsec):
        s = opt + optsize + 40 * i
        vsize, vaddr, rsize, raw = struct.unpack_from("<IIII", data, s + 8)
        secs.append((vaddr, max(vsize, rsize), raw))

    def off(r: int) -> int:
        for vaddr, size, raw in secs:
            if vaddr <= r < vaddr + size:
                return raw + r - vaddr
        raise ValueError(f"resource address {r:#x} outside the sections")

    base = off(rva)

    def entries(d: int):
        named, ids = struct.unpack_from("<HH", data, base + d + 12)
        for i in range(named + ids):
            name, target = struct.unpack_from("<II", data, base + d + 16 + 8 * i)
            if name & 0x80000000:
                n = struct.unpack_from("<H", data, base + (name & 0x7FFFFFFF))[0]
                p = base + (name & 0x7FFFFFFF) + 2
                label = data[p:p + 2 * n].decode("utf-16-le")
            else:
                label = name
            yield label, target

    out = []
    for t, t_dir in entries(0):
        for n, n_dir in entries(t_dir & 0x7FFFFFFF):
            for _lang, leaf in entries(n_dir & 0x7FFFFFFF):
                drva, dsize = struct.unpack_from("<II", data, base + (leaf & 0x7FFFFFFF))
                o = off(drva)
                out.append((RT_NAMES.get(t, str(t)), str(n), data[o:o + dsize]))
    return out


_OWN_ICON: set[str] | None = None


def own_icon_images() -> set[str]:
    """SHA-256 of the images of our own icon (scripts/make_icon.py: drawn here, nothing of the game)."""
    global _OWN_ICON
    if _OWN_ICON is None:
        import make_icon
        _OWN_ICON = {hashlib.sha256(make_icon.png(s, make_icon.render(s))).hexdigest() for s in make_icon.SIZES}
    return _OWN_ICON


def check_resources(label: str, data: bytes, problems: list[str], allowed: set[str]) -> int:
    """The program's resources: our own icon; the version, the manifest; with `allowed` (make_installer.py --art's
    allow-list: SHA-256 of the owner's pictures and of the images of the icon made from his logo) exactly those."""
    n = 0
    for rtype, name, blob in pe_resources(data):
        what = f"{label}:resource {rtype}/{name}"
        digest = hashlib.sha256(blob).hexdigest()
        n += 1
        if rtype == "GROUP_ICON":                      # (an icon's directory: sizes and ids, no pixels)
            check_blob(what, blob, problems, media_check=False)
        elif digest in allowed:
            check_blob(what, blob, problems, media_check=False)
        elif rtype == "ICON":                          # (any image format: only ours, or the allowed ones)
            check_blob(what, blob, problems, media_check=False)
            if digest not in own_icon_images():
                problems.append(f"{what}: an icon image that is not ours (scripts/make_icon.py) nor on the allow-list")
        else:
            check_blob(what, blob, problems)
    return n


def check_setup_exe(path: Path, problems: list[str], allowed: set[str] = frozenset()) -> int:
    data = path.read_bytes()
    if len(data) < 24 or data[-24:-16] != b"SLY2PAY1":
        raise ValueError("no payload (not a Sly2-Setup exe, or a signed one: check the source zip instead)")
    _magic, count, _zero, tab = struct.unpack("<8sIIQ", data[-24:])
    n = check_resources(path.name, data, problems, set(allowed))
    for i in range(count):
        raw, off, size, _h = struct.unpack_from("<96sQQ32s", data, tab + i * 144)
        name = raw.rstrip(b"\0").decode("utf-8")
        blob = data[off:off + size]
        if name == "source.zip":
            n += check_zip(f"{path.name}:source.zip", blob, problems)
        else:                                          # (toolchain archives: third-party; hashes only)
            check_blob(f"{path.name}:{name}", blob, problems, media_check=False)
            n += 1
    return n


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__)
        return 2
    problems: list[str] = []
    total = 0
    allowed: set[str] = set()
    try:
        while argv and argv[0] == "--allow":           # --allow <make_installer --art's Sly2-Setup.art-allow.txt>
            if len(argv) < 2:
                print(__doc__)
                return 2
            for line in Path(argv[1]).read_text(encoding="utf-8").splitlines():
                h = line.split()[0].lower() if line.strip() else ""
                if len(h) == 64 and all(c in "0123456789abcdef" for c in h):
                    allowed.add(h)
            argv = argv[2:]
        for a in argv:
            p = Path(a)
            if p.is_dir():
                for f in sorted(p.rglob("*")):
                    if f.is_file():
                        check_blob(str(f.relative_to(p)), f.read_bytes(), problems)
                        total += 1
            elif p.suffix.lower() == ".zip":
                total += check_zip(p.name, p.read_bytes(), problems)
            else:
                total += check_setup_exe(p, problems, allowed)
    except (OSError, ValueError, zipfile.BadZipFile) as e:
        print(f"check_no_game_data: cannot check: {e}")
        return 2
    for pr in problems:
        print(f"GAME DATA? {pr}")
    print(f"check_no_game_data: {total} files, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
