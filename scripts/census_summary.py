"""Condense gs_census reports (scripts/gs_census.ps1 output) for reading side by side.

    python scripts/census_summary.py build/census/*.txt [-s section[,section...]] [-n rows]

Prints, per report, the totals block and the first `rows` rows of the chosen '== section' tables
(default: all sections, 12 rows). Section names match by substring, case-insensitively."""
import glob
import sys


def main():
    args = sys.argv[1:]
    sections = None
    rows = 12
    files = []
    i = 0
    while i < len(args):
        if args[i] == '-s':
            sections = [s.lower() for s in args[i + 1].split(',')]
            i += 2
        elif args[i] == '-n':
            rows = int(args[i + 1])
            i += 2
        else:
            files.extend(glob.glob(args[i]) or [args[i]])
            i += 1
    for f in files:
        lines = open(f, encoding='utf-8-sig').read().splitlines()
        print('#' * 100)
        print(lines[0] if lines else f)
        in_totals = False
        cur = None
        printed = 0
        for ln in lines:
            if ln.startswith('# totals'):
                in_totals = True
                cur = None
                print(ln)
                continue
            if ln.startswith('== '):
                in_totals = False
                name = ln[3:].lower()
                cur = ln if sections is None or any(s in name for s in sections) else None
                printed = 0
                if cur:
                    print(ln)
                continue
            if in_totals:
                if ln.strip():
                    print(ln)
                continue
            if cur and ln.strip():
                if printed < rows:
                    print(ln)
                printed += 1


if __name__ == '__main__':
    main()
