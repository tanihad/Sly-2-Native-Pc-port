"""Aggregate an address profile (lines "  <pct>%  0x<addr>", e.g. vu_replay's VU_REPLAY_PROFILE output)
by function, using the executable's symbol table (llvm-nm).
    <python> scripts\\prof_symbols.py <exe> <profile.txt> [--top 30]"""
import argparse
import bisect
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
    ap.add_argument("--top", type=int, default=30)
    args = ap.parse_args()
    nm = subprocess.run([str(TOOLS / "llvm-mingw" / "bin" / "llvm-nm.exe"), "-n", "--defined-only", args.exe],
                        capture_output=True, text=True).stdout
    syms = []
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1].lower() in ("t", "w"):
            syms.append((int(parts[0], 16), parts[2]))
    syms.sort()
    addrs = [a for a, _ in syms]
    agg = Counter()
    for line in Path(args.profile).read_text(errors="replace").splitlines():
        m = re.match(r"\s*([\d.]+)%\s+0x([0-9a-fA-F]+)", line)
        if not m:
            continue
        a = int(m.group(2), 16)
        i = bisect.bisect_right(addrs, a) - 1
        agg[syms[i][1] if i >= 0 else "?"] += float(m.group(1))
    for name, pct in agg.most_common(args.top):
        print(f"{pct:6.2f}%  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
