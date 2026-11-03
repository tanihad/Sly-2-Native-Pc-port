"""Helpers for the launcher update tests (tests/unit/launcher/run.ps1, tests/installer/e2e.ps1). Test keys only:
they are made fresh for each run in %TEMP%, never the release key.

    python release_tool.py key <key file>                      a random test key (prints the public key hex)
    python release_tool.py sign <key file> <bundle> <bundle URL> <version> <manifest out>
                                                               a signed manifest (+ .sig) for any bundle
    python release_tool.py cases <out file>                    signed / tampered cases for test_release.c
    python release_tool.py rezip <bundle> <out> <version count> [--break]
                                                               the bundle with another version.txt
                                                               (--break: a runtime file that does not compile)
"""
import hashlib
import os
import secrets
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
import sly2_ed25519  # noqa: E402
from release_keygen import MAGIC, read_private_key  # noqa: E402


def key(path: str) -> None:
    seed = secrets.token_bytes(32)
    Path(path).write_text(f"# test key\n{MAGIC}\n{seed.hex()}\n", encoding="ascii")
    print(sly2_ed25519.public_key(seed).hex())


def sign(keyfile: str, bundle: str, url: str, version: str, out: str) -> None:
    seed = read_private_key(Path(keyfile))
    data = Path(bundle).read_bytes()
    text = (f"# test release\nformat = 1\nproduct = sly2-pc\nversion = {version}\ncommit = test\nbundle = {url}\n"
            f"bundle_size = {len(data)}\nbundle_sha256 = {hashlib.sha256(data).hexdigest()}\n").encode()
    Path(out).write_bytes(text)
    Path(out + ".sig").write_text(sly2_ed25519.sign(seed, text).hex() + "\n", encoding="ascii")


def cases(out: str) -> None:
    lines = []
    for i in range(12):
        seed = secrets.token_bytes(32)
        pub = sly2_ed25519.public_key(seed)
        msg = secrets.token_bytes(i * 37)
        sig = sly2_ed25519.sign(seed, msg)
        mh = msg.hex() or "-"
        lines.append(f"{pub.hex()} {mh} {sig.hex()} 1")
        bad = bytearray(sig)
        bad[i % 64] ^= 1 << (i % 8)
        lines.append(f"{pub.hex()} {mh} {bytes(bad).hex()} 0")
        if msg:
            m2 = bytearray(msg)
            m2[0] ^= 0x80
            lines.append(f"{pub.hex()} {bytes(m2).hex()} {sig.hex()} 0")
    Path(out).write_text("\n".join(lines) + "\n", encoding="ascii")


def rezip(bundle: str, out: str, count: str, broken: bool) -> None:
    with zipfile.ZipFile(bundle) as zin, zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zout:
        for info in zin.infolist():
            data = zin.read(info.filename)
            if info.filename == "version.txt":
                data = f"{count} test{'-broken' if broken else ''}\n".encode()
            elif broken and info.filename == "runtime/cdvd.c":
                data += b"\n#error deliberately broken (launcher test: a release whose build fails)\n"
            zout.writestr(info, data)


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[0] == "key":
        key(a[1])
    elif a[0] == "sign":
        sign(*a[1:6])
    elif a[0] == "cases":
        cases(a[1])
    elif a[0] == "rezip":
        rezip(a[1], a[2], a[3], "--break" in a)
    else:
        sys.exit(__doc__)
    sys.stdout.flush()
    os._exit(0)
