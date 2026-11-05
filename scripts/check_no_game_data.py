"""Prove that what we ship contains nothing of the game: no picture, icon, sound or movie, and none of the disc's files.

    python scripts/check_no_game_data.py <sly2-source-*.zip | Sly2-Setup*.exe | folder> ...

Game art (the icon and backdrop made from the player's disc by scripts/disc_art.py, the owner's own pictures in
%LOCALAPPDATA%\\Sly2\\art) exists only on the player's PC; the sources, a release bundle and Setup carry none.
For a Setup exe the payload (SLY2PAY1, scripts/make_installer.py) is opened and source.zip checked inside it;
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


def check_setup_exe(path: Path, problems: list[str]) -> int:
    data = path.read_bytes()
    if len(data) < 24 or data[-24:-16] != b"SLY2PAY1":
        raise ValueError("no payload (not a Sly2-Setup exe, or a signed one: check the source zip instead)")
    _magic, count, _zero, tab = struct.unpack("<8sIIQ", data[-24:])
    n = 0
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
    try:
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
                total += check_setup_exe(p, problems)
    except (OSError, ValueError, zipfile.BadZipFile) as e:
        print(f"check_no_game_data: cannot check: {e}")
        return 2
    for pr in problems:
        print(f"GAME DATA? {pr}")
    print(f"check_no_game_data: {total} files, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
