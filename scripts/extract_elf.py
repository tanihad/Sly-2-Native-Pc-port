"""Extract the game's executable (SCUS_973.16) from the player's disc image, so a build needs only the .iso.

    python scripts\\extract_elf.py [--iso <image>] [--out orig\\SCUS_973.16] [--force]

Without --iso the first *.iso in orig\\disc\\ is used (or PS2_ISO). The image is read the way the game
reads it at run time (runtime/disc.c): ISO 9660 volume -> SYSTEM.CNF -> BOOT2 = cdrom0:\\SCUS_973.16;1 ->
that file. Its size and SHA-256 must be those of Sly 2: Band of Thieves (USA, SCUS-97316, v1.00), the
version this project is made for; anything else is refused with a message (exit code 1).
Nothing from the image is written except the executable, and only into orig\\ (git-ignored).
"""
import argparse
import hashlib
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import ELF, ISO_DIR  # noqa: E402

SECTOR = 2048
EXPECTED_NAME = "SCUS_973.16"
EXPECTED_SIZE = 2_597_048
EXPECTED_SHA256 = "a511a94b1dbe8cd9bc9b764c34ffda3f88869713a338e188b4f407732fec6a73"


class DiscError(Exception):
    pass


def _read(f, lba: int, size: int) -> bytes:
    f.seek(lba * SECTOR)
    data = f.read(size)
    if len(data) != size:
        raise DiscError("the image ends early (incomplete or damaged copy)")
    return data


def _dir_entries(f, lba: int, size: int):
    data = _read(f, lba, size)
    pos = 0
    while pos < len(data):
        n = data[pos]
        if n == 0:                                     # records never cross a sector: skip to the next
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        rec = data[pos:pos + n]
        if len(rec) < 34:
            break
        (ext,), (length,) = struct.unpack_from("<I", rec, 2), struct.unpack_from("<I", rec, 10)
        name = rec[33:33 + rec[32]].decode("ascii", "replace")
        yield name, ext, length, bool(rec[25] & 2)             # flags bit 1: directory
        pos += n


def find_file(f, path: str) -> tuple[int, int]:
    """(LBA, size) of a file given as 'DIR\\NAME;1' (case-insensitive)."""
    pvd = _read(f, 16, SECTOR)
    if pvd[0] != 1 or pvd[1:6] != b"CD001":
        raise DiscError("not an ISO 9660 disc image (.iso); other formats (.bin/.cue, .chd, .cso) must be "
                        "converted to .iso first")
    root = pvd[156:156 + 34]
    lba, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
    parts = [p for p in path.replace("/", "\\").split("\\") if p]
    for i, part in enumerate(parts):
        want = part.upper()
        for name, ext, length, is_dir in _dir_entries(f, lba, size):
            if name.upper() == want or name.upper().split(";")[0] == want.split(";")[0]:
                if i < len(parts) - 1 and not is_dir:
                    continue
                lba, size = ext, length
                break
        else:
            raise DiscError(f"{path} is not on this disc")
    return lba, size


def boot_path(f) -> str:
    lba, size = find_file(f, "SYSTEM.CNF;1")
    text = _read(f, lba, size).decode("ascii", "replace")
    for line in text.splitlines():
        key, _, value = line.partition("=")
        if key.strip().upper() == "BOOT2":
            value = value.strip()
            if not value.lower().startswith("cdrom0:"):
                break
            return value[len("cdrom0:"):]
    raise DiscError("SYSTEM.CNF has no PS2 boot file (not a PS2 game disc)")


def extract(iso: Path) -> bytes:
    with open(iso, "rb") as f:
        boot = boot_path(f)
        name = boot.strip("\\").split(";")[0]
        lba, size = find_file(f, boot)
        data = _read(f, lba, size)
    digest = hashlib.sha256(data).hexdigest()
    if name.upper() != EXPECTED_NAME:
        raise DiscError(f"this disc boots {name}, not {EXPECTED_NAME}: it is another game or another region "
                        "(this project needs Sly 2: Band of Thieves, USA, SCUS-97316)")
    if size != EXPECTED_SIZE or digest != EXPECTED_SHA256:
        raise DiscError(f"{name} on this disc is a different version ({size} bytes, SHA-256 {digest[:16]}...); "
                        f"this project needs the USA v1.00 release ({EXPECTED_SIZE} bytes, {EXPECTED_SHA256[:16]}...)")
    return data


def default_iso() -> Path | None:
    env = os.environ.get("PS2_ISO")
    if env and Path(env).is_file():
        return Path(env)
    return next(iter(sorted(ISO_DIR.glob("*.iso"))), None)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", help="disc image (default: the first *.iso in orig\\disc, or PS2_ISO)")
    ap.add_argument("--out", default=str(ELF))
    ap.add_argument("--force", action="store_true", help="extract even if --out already exists")
    args = ap.parse_args(argv)
    out = Path(args.out)
    if out.exists() and not args.force:
        print(f"{out} already present")
        return 0
    iso = Path(args.iso) if args.iso else default_iso()
    if iso is None or not iso.is_file():
        print(f"No disc image found. Put the .iso of your own Sly 2: Band of Thieves (USA) disc into {ISO_DIR}\\ "
              "(or pass --iso <image>).")
        return 1
    try:
        data = extract(iso)
    except (DiscError, OSError) as e:
        print(f"{iso}: {e}")
        return 1
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(out.suffix + ".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, out)
    print(f"extracted {EXPECTED_NAME} ({len(data):,} bytes, SHA-256 OK) from {iso.name} -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
