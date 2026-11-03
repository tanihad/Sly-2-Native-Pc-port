"""Make the Ed25519 key pair that signs launcher updates (notes/install.md, "Publishing an update").

    <main checkout>\\.venv\\Scripts\\python.exe scripts\\release_keygen.py [--key <file>] [--force] [--no-header]

- The PRIVATE key (32 random bytes from the OS, as hex) goes to --key, by default
  %USERPROFILE%\\.sly2-release\\release-ed25519.key - always OUTSIDE the repository: the script refuses any
  path inside this checkout, its worktrees or the main checkout, and never overwrites an existing key
  without --force (a new key means launchers built with the old public key cannot verify new releases).
- The PUBLIC key is written into installer/release_key.h (unless --no-header) and built into every launcher.
  Commit that header; never commit, upload or share the private key. Keep a backup of it (a password
  manager or an offline USB stick): without it no update can be published for launchers already out there.
"""
import argparse
import os
import secrets
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sly2_ed25519  # noqa: E402

ROOT = HERE.parent
HEADER = ROOT / "installer" / "release_key.h"
DEFAULT_KEY = Path(os.environ.get("USERPROFILE", str(Path.home()))) / ".sly2-release" / "release-ed25519.key"
MAGIC = "sly2-release-ed25519-private-key-v1"


def repo_roots() -> list[Path]:
    """This checkout, the main checkout (if this is a worktree) and every worktree git knows."""
    from paths import MAIN, TOOLS
    roots = {ROOT.resolve(), MAIN.resolve()}
    git = TOOLS / "git" / "cmd" / "git.exe"
    try:
        out = subprocess.run([str(git) if git.exists() else "git", "worktree", "list", "--porcelain"], cwd=ROOT,
                             capture_output=True, text=True, check=True).stdout
        roots |= {Path(line[9:]).resolve() for line in out.splitlines() if line.startswith("worktree ")}
    except (OSError, subprocess.CalledProcessError):
        pass                                        # (this checkout and the main one are always covered)
    return sorted(roots)


def inside_repo(path: Path) -> Path | None:
    p = path.resolve()
    for r in repo_roots():
        if p == r or r in p.parents:
            return r
    return None


def read_private_key(path: Path) -> bytes:
    lines = [ln.strip() for ln in path.read_text(encoding="ascii").splitlines() if ln.strip() and not ln.startswith("#")]
    if len(lines) != 2 or lines[0] != MAGIC or len(lines[1]) != 64:
        sys.exit(f"{path}: not a release key made by release_keygen.py")
    return bytes.fromhex(lines[1])


def write_header(pub: bytes) -> None:
    body = ", ".join(f"0x{b:02x}" for b in pub)
    HEADER.write_text(
        "/* The public half of the release signing key (scripts/release_keygen.py). The launcher accepts an update\n"
        " * only if its manifest is signed by the matching private key, which never leaves the release machine.\n"
        " * Replacing this key means older launchers cannot verify newer releases. */\n"
        "#pragma once\n"
        f"#define SLY2_RELEASE_PUBKEY_HEX \"{pub.hex()}\"\n"
        f"static const unsigned char SLY2_RELEASE_PUBKEY[32] = {{ {body} }};\n", encoding="utf-8", newline="\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--key", default=str(DEFAULT_KEY), help="where the private key goes (outside the repository)")
    ap.add_argument("--force", action="store_true", help="replace an existing key file")
    ap.add_argument("--no-header", action="store_true", help="do not write installer/release_key.h")
    args = ap.parse_args()
    key = Path(args.key).expanduser()
    repo = inside_repo(key)
    if repo:
        sys.exit(f"release_keygen: refusing to write the private key inside the repository ({repo}). "
                 "Choose a folder outside it, e.g. the default " + str(DEFAULT_KEY))
    if key.exists() and not args.force:
        pub = sly2_ed25519.public_key(read_private_key(key))
        print(f"{key} exists already (public key {pub.hex()}); --force makes a new one")
        if not args.no_header:
            write_header(pub)
            print(f"wrote {HEADER}")
        return 0
    seed = secrets.token_bytes(32)
    pub = sly2_ed25519.public_key(seed)
    key.parent.mkdir(parents=True, exist_ok=True)
    tmp = key.with_suffix(".tmp")
    tmp.write_text(f"# Sly 2 PC port release signing key. PRIVATE: never commit, upload or share it; keep a backup.\n"
                   f"{MAGIC}\n{seed.hex()}\n", encoding="ascii")
    os.replace(tmp, key)
    if os.name == "nt":                              # only this user may read it
        user = os.environ.get("USERNAME", "")
        if user:
            subprocess.run(["icacls", str(key), "/inheritance:r", "/grant:r", f"{user}:F"], capture_output=True)
    sig = sly2_ed25519.sign(seed, b"self-test")
    assert sly2_ed25519.verify(pub, b"self-test", sig)
    print(f"private key: {key}  (keep it secret, keep a backup)")
    print(f"public key:  {pub.hex()}")
    if not args.no_header:
        write_header(pub)
        print(f"wrote {HEADER} - commit it so launchers trust this key")
    return 0


if __name__ == "__main__":
    sys.exit(main())
