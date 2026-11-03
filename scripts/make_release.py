"""Make a signed launcher update: the three files a GitHub release needs (notes/install.md, "Publishing an update").

    <main checkout>\\.venv\\Scripts\\python.exe scripts\\make_release.py [--notes "..."] [--min-launcher 0.1.N]

Writes into build\\release\\<version>\\:
  sly2-source-<version>.zip   the source bundle (exactly what Sly2-Setup.exe carries as source.zip: our
                              recompiler, runtime and build scripts; never anything from the game)
  sly2-release.txt            the manifest: version, the bundle's URL, size and SHA-256
  sly2-release.txt.sig        the Ed25519 signature of the manifest (scripts/release_keygen.py's key)
then prints the `gh release create` command that publishes them. It uploads nothing itself.

Launchers fetch https://github.com/tanihad/sly2-decomp/releases/latest/download/sly2-release.txt (+ .sig),
check the signature with the public key built into them, then download the bundle from the manifest's
URL and check its size and SHA-256 before using it.

Test options: --count N (pretend the commit count is N), --allow-dirty, --base-url (where the bundle
will be served, e.g. http://127.0.0.1:8123), --key (another key, e.g. a test key). --out <folder>.
"""
import argparse
import datetime
import hashlib
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import make_installer  # noqa: E402
import sly2_ed25519  # noqa: E402
from release_keygen import DEFAULT_KEY, inside_repo, read_private_key  # noqa: E402

ROOT = HERE.parent
REPO = "tanihad/sly2-decomp"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--key", default=str(DEFAULT_KEY), help="the private release key (outside the repository)")
    ap.add_argument("--out", help="output folder (default build/release/<version>)")
    ap.add_argument("--notes", default="", help="one line shown to players")
    ap.add_argument("--min-launcher", default="", help="oldest launcher that can install this release (e.g. 0.1.1100)")
    ap.add_argument("--base-url", help="where the bundle is served (default: this release's GitHub download URL)")
    ap.add_argument("--count", type=int, help="(tests) use this commit count as the version")
    ap.add_argument("--allow-dirty", action="store_true", help="(tests) allow uncommitted changes")
    args = ap.parse_args()

    key = Path(args.key).expanduser()
    if inside_repo(key):
        sys.exit("make_release: the private key must not be inside the repository")
    if not key.exists():
        sys.exit(f"make_release: no key at {key} (run scripts/release_keygen.py first)")
    seed = read_private_key(key)
    pub = sly2_ed25519.public_key(seed)

    count, ghash = make_installer.version()
    if ghash.endswith("-dirty") and not args.allow_dirty:
        sys.exit("make_release: the working tree has uncommitted changes in the game's sources; commit first")
    if args.count is not None:
        count = args.count
    version = f"0.1.{count}"
    commit = ghash
    out = Path(args.out) if args.out else ROOT / "build" / "release" / version
    out.mkdir(parents=True, exist_ok=True)

    bundle = out / f"sly2-source-{version}.zip"
    nfiles = make_installer.make_source_zip(bundle, count, ghash)
    data = bundle.read_bytes()
    base = args.base_url.rstrip("/") if args.base_url else f"https://github.com/{REPO}/releases/download/v{version}"
    if not base.startswith("https://") and not base.startswith(("http://127.0.0.1", "http://localhost")):
        sys.exit("make_release: the bundle must be served over https://")
    lines = [
        "# Sly 2: Band of Thieves PC port - release manifest. Signed: sly2-release.txt.sig (Ed25519).",
        "format = 1",
        "product = sly2-pc",
        f"version = {version}",
        f"commit = {commit}",
        f"date = {datetime.date.today().isoformat()}",
        f"bundle = {base}/{bundle.name}",
        f"bundle_size = {len(data)}",
        f"bundle_sha256 = {hashlib.sha256(data).hexdigest()}",
    ]
    if args.min_launcher:
        lines.append(f"min_launcher = {args.min_launcher}")
    lines.append(f"page = https://github.com/{REPO}/releases/tag/v{version}")
    if args.notes:
        lines.append(f"notes = {args.notes.splitlines()[0]}")
    manifest = ("\n".join(lines) + "\n").encode("utf-8")
    sig = sly2_ed25519.sign(seed, manifest)
    if not sly2_ed25519.verify(pub, manifest, sig):
        sys.exit("make_release: self-check of the signature failed")
    (out / "sly2-release.txt").write_bytes(manifest)
    (out / "sly2-release.txt.sig").write_text(sig.hex() + "\n", encoding="ascii", newline="\n")

    built_in = None
    header = ROOT / "installer" / "release_key.h"
    if header.exists() and pub.hex() not in header.read_text(encoding="utf-8"):
        built_in = "WARNING: this key is not the one in installer/release_key.h: launchers will reject this release."
    print(f"release {version} ({commit}), {nfiles} source files, bundle {len(data) / 1024:.0f} KB")
    print(f"  {bundle}\n  {out / 'sly2-release.txt'}\n  {out / 'sly2-release.txt.sig'}")
    print(f"signed with public key {pub.hex()}")
    if built_in:
        print(built_in)
    print("\nPublish (after the owner's go-ahead) with the GitHub CLI, from this checkout:")
    print(f'  gh release create v{version} --repo {REPO} --title "Sly 2 PC {version}" --notes "{args.notes or "Update"}" '
          f'"{bundle}" "{out / "sly2-release.txt"}" "{out / "sly2-release.txt.sig"}"')
    print("Attach the Setup files too (build\\installer\\Sly2-Setup.exe, SHA256SUMS.txt) so new players get this version.")
    print("It must be the newest (\"latest\") release: launchers read releases/latest/download/sly2-release.txt.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
