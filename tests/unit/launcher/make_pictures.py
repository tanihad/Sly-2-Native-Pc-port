"""Synthetic pictures for the launcher's art tests (nothing of the game: gradients and blocks drawn here).

    python make_pictures.py <folder>
writes icon.png (64x64), logo.png (2000x1187, the official logo's proportions), backdrop.png (960x310),
broken.png (not a picture), game.zip (a source bundle with a picture in it: check_no_game_data must refuse it)
"""
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from disc_art import png_bytes  # noqa: E402


def picture(w: int, h: int, alpha_disc: bool) -> bytes:
    px = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            j = (y * w + x) * 4
            inside = not alpha_disc or (x - w / 2) ** 2 + (y - h / 2) ** 2 < (min(w, h) / 2) ** 2
            px[j:j + 4] = bytes((40 + 200 * x // w, 60 + 150 * y // h, 160, 255 if inside else 0))
    return png_bytes(w, h, px)


def main(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    (out / "icon.png").write_bytes(picture(64, 64, True))
    (out / "logo.png").write_bytes(picture(2000, 1187, False))
    (out / "backdrop.png").write_bytes(picture(960, 310, False))
    (out / "broken.png").write_bytes(b"\x89PNG\r\n\x1a\nthis is not a picture")
    with zipfile.ZipFile(out / "game.zip", "w") as z:
        z.writestr("runtime/main.c", "int main(void) { return 0; }\n")
        z.writestr("runtime/title.dat", picture(8, 8, False))       # a picture under a harmless name


if __name__ == "__main__":
    main(Path(sys.argv[1]))
