"""What a Setup program carries as resources (for tests/unit/launcher/run.ps1).

    python setup_resources.py <Sly2-Setup.exe>
prints one line per RCDATA resource ("rcdata <name> <sha256>") and one for the icon: "icon own" (every image is
scripts/make_icon.py's), else "icon other <sha256 of each image, comma-separated>".
"""
import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
import check_no_game_data  # noqa: E402


def main(exe: Path) -> None:
    res = check_no_game_data.pe_resources(exe.read_bytes())
    for rtype, name, blob in res:
        if rtype == "RCDATA":
            print(f"rcdata {name} {hashlib.sha256(blob).hexdigest()}")
    icons = [hashlib.sha256(b).hexdigest() for t, _n, b in res if t == "ICON"]
    own = check_no_game_data.own_icon_images()
    print("icon own" if icons and all(h in own for h in icons) else f"icon other {','.join(icons)}")


if __name__ == "__main__":
    main(Path(sys.argv[1]))
