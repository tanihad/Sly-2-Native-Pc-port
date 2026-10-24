"""Synthetic disc images for the installer's disc check (installer/disc.c), written into <folder>:

    python make_isos.py <folder>

Minimal ISO 9660 (a primary volume descriptor and one root directory) - enough for the checks, which
only follow PVD -> root -> SYSTEM.CNF -> BOOT2 -> the boot file. Names and the expected result (the
disc_check_iso return code) are in cases.txt: "<file> <code>".
"""
import os
import struct
import sys
from pathlib import Path

SECTOR = 2048


def dir_record(name: bytes, lba: int, size: int, is_dir: bool) -> bytes:
    n = 33 + len(name)
    n += n & 1
    rec = bytearray(n)
    rec[0] = n
    struct.pack_into("<I", rec, 2, lba)
    struct.pack_into(">I", rec, 6, lba)
    struct.pack_into("<I", rec, 10, size)
    struct.pack_into(">I", rec, 14, size)
    rec[25] = 2 if is_dir else 0
    rec[32] = len(name)
    rec[33:33 + len(name)] = name
    return bytes(rec)


def make_iso(files: dict[str, bytes], truncate_to: int | None = None) -> bytes:
    root_lba = 20
    lba = 21
    recs = [dir_record(b"\x00", root_lba, SECTOR, True), dir_record(b"\x01", root_lba, SECTOR, True)]
    data = {}
    for name, body in files.items():
        recs.append(dir_record(name.encode() + b";1", lba, len(body), False))
        data[lba] = body
        lba += (len(body) + SECTOR - 1) // SECTOR or 1
    img = bytearray(lba * SECTOR)
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[156:156 + 34] = dir_record(b"\x00", root_lba, SECTOR, True)
    img[16 * SECTOR:17 * SECTOR] = pvd
    term = bytearray(SECTOR)
    term[0] = 255
    term[1:6] = b"CD001"
    img[17 * SECTOR:18 * SECTOR] = term
    root = b"".join(recs)
    img[root_lba * SECTOR:root_lba * SECTOR + len(root)] = root
    for at, body in data.items():
        img[at * SECTOR:at * SECTOR + len(body)] = body
    if truncate_to is not None:
        del img[truncate_to:]
    return bytes(img)


def cnf(boot: str) -> bytes:
    return f"BOOT2 = cdrom0:\\{boot};1\r\nVER = 1.00\r\nVMODE = NTSC\r\n".encode()


def main() -> int:
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    cases = {
        "random.iso": (os.urandom(64 * 1024), 2),                                   # DISC_NOT_ISO
        "no_cnf.iso": (make_iso({"README.TXT": b"hello"}), 4),                      # DISC_NO_CNF
        "ps1.iso": (make_iso({"SYSTEM.CNF": b"BOOT = cdrom:\\SCUS_944.26;1\r\n"}), 5),   # DISC_NOT_PS2
        "pal.iso": (make_iso({"SYSTEM.CNF": cnf("SCES_524.56"), "SCES_524.56": b"\x7fELF" + bytes(1000)}), 6),
        "patched.iso": (make_iso({"SYSTEM.CNF": cnf("SCUS_973.16"), "SCUS_973.16": b"\x7fELF" + bytes(5000)}), 7),
        "truncated.iso": (make_iso({"SYSTEM.CNF": cnf("SCUS_973.16"), "SCUS_973.16": b"\x7fELF" + bytes(2_597_044)},
                                   truncate_to=22 * SECTOR + 100), 3),             # DISC_SHORT: right size, data cut
    }
    with open(out / "cases.txt", "w", encoding="ascii") as f:
        for name, (img, code) in cases.items():
            (out / name).write_bytes(img)
            f.write(f"{name} {code}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
