"""Frame-rate comparison sheet (pc-fps, notes/frame-rate.md): the pictures of several fps_run.ps1 runs (PS2_DUMP_SECS) at the
same game times, side by side (one column per run, one row per game time), via scripts/contact_sheet.py.

    python scripts/fps_sheet.py <out.png> --from <game s> --step <game s> --rows <n> <run dir> [<run dir> ...] [--scale 3]

Game time comes from each run's trace.txt (VBlank -> game clock t); for each row the dumped frame nearest that game time is
taken from every run (prints the frames and how far off they are).
"""
import argparse
import subprocess
import sys
from pathlib import Path


def frames_by_t(d, col=2):
    t_of = {}
    for line in (Path(d) / "trace.txt").read_text(encoding="latin-1").splitlines():
        p = line.split()
        if len(p) >= 4:
            t_of[int(p[0])] = float(p[col])
    out = []
    for f in (Path(d) / "frames").glob("frame_*.png"):
        v = int(f.stem[6:])
        if v in t_of:
            out.append((t_of[v], f))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--from", dest="t0", type=float, required=True)
    ap.add_argument("--step", type=float, default=2.0)
    ap.add_argument("--rows", type=int, default=6)
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--wall", action="store_true", help="by wall-clock seconds since the start instead of game time")
    a = ap.parse_args()
    runs = [frames_by_t(d, 1 if a.wall else 2) for d in a.runs]
    pick = []
    for k in range(a.rows):
        t = a.t0 + k * a.step
        row = []
        for d, fr in zip(a.runs, runs):
            if not fr:
                print(f"{d}: no frames")
                return 1
            tt, f = min(fr, key=lambda x: abs(x[0] - t))
            row.append(f"{Path(d).name}@{tt:.2f}")
            pick.append(str(f))
        print(f"t {t:7.2f}: " + "  ".join(row))
    here = Path(__file__).resolve().parent
    return subprocess.run([sys.executable, str(here / "contact_sheet.py"), a.out, *pick, "--cols", str(len(a.runs)),
                           "--scale", str(a.scale)]).returncode


if __name__ == "__main__":
    sys.exit(main())
