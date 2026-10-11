"""Aggregate an address profile (lines "  <pct>%  0x<addr>", vu_replay's VU_REPLAY_PROFILE output) by
source line of an executable built with -g (llvm-symbolizer), e.g. to find the hot pairs of
recompiled VU microcode (vu_replay.ps1 -Compiled <dir> -DebugInfo).
    <python> scripts\\prof_lines.py <exe> <profile.txt> [--top 40] [--context 0]"""
import argparse
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import TOOLS  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("profile")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--outer", default="", help="attribute inlined code to the outermost frame in a file "
                                                "whose name contains this (e.g. vuc_)")
    args = ap.parse_args()
    samples = []
    for line in Path(args.profile).read_text(errors="replace").splitlines():
        m = re.match(r"\s*([\d.]+)%\s+(0x[0-9a-fA-F]+)", line)
        if m:
            samples.append((float(m.group(1)), m.group(2)))
    sym = subprocess.run([str(TOOLS / "llvm-mingw" / "bin" / "llvm-symbolizer.exe"), f"--obj={args.exe}"],
                         input="\n".join(a for _, a in samples) + "\n", capture_output=True, text=True).stdout
    blocks = [b for b in sym.split("\n\n") if b.strip()]
    agg, srcs = Counter(), {}
    for (pct, _), blk in zip(samples, blocks):
        lines = blk.strip().splitlines()
        loc = lines[1] if len(lines) > 1 else "?"
        if args.outer:                   # frames come in pairs (function, location), innermost first
            locs = [lines[i] for i in range(1, len(lines), 2) if args.outer in lines[i]]
            if locs:
                loc = locs[-1]
        m = re.match(r"(.*):(\d+):\d+$", loc)
        key = (m.group(1), int(m.group(2))) if m else (loc, 0)
        agg[key] += pct
    for (f, ln), pct in agg.most_common(args.top):
        text = ""
        try:
            if f not in srcs:
                srcs[f] = Path(f).read_text(errors="replace").splitlines()
            if ln:
                text = srcs[f][ln - 1].strip()[:110]
        except OSError:
            pass
        print(f"{pct:6.2f}%  {Path(f).name}:{ln}  {text}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
