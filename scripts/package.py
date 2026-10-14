"""Build the game folder a player starts like any PC game: dist\\Sly2\\sly2.exe (+ README.txt).

    <main checkout>\\.venv\\Scripts\\python.exe scripts\\package.py [--dest dist/Sly2] [--out build/rel]

Builds the release version into --out (as scripts/play.ps1 does; reused if up to date), then links
the same objects as a GUI program with icon and version information into --dest. The folder holds
no game data: on its first start the game asks for the player's own disc image and reads the game
executable from it (notes/install.md). dist/ is git-ignored and must never be committed.
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dest", default=str(ROOT / "dist" / "Sly2"))
    ap.add_argument("--out", default=str(ROOT / "build" / "rel"))
    args = ap.parse_args()
    return subprocess.run([sys.executable, str(ROOT / "scripts" / "build_game.py"), "--release", "--out", args.out,
                           "--package", args.dest]).returncode


if __name__ == "__main__":
    sys.exit(main())
