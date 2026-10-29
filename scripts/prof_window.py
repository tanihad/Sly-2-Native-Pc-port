"""(r2-m7) GPU time per kind of work over the later part of a run: PS2_GPU_PROF=1 logs cumulative averages ("prof: N
frames, GPU ms per frame T: kind ms (marks) ..." every 60 frames); this differences the last line and the first line at or
after --from (a fraction of the run's frames, default 0.45) into per-frame averages over that window.
    python scripts/prof_window.py <run.log> [--from 0.45]"""
import re
import sys


def parse(line):
    m = re.search(r'prof: (\d+) frames, GPU ms per frame ([\d.]+):(.*)$', line)
    if not m:
        return None
    n, tot = int(m.group(1)), float(m.group(2))
    kinds = {k: float(v) for k, v in re.findall(r' ([a-z0-9-]+) ([\d.]+) \(', m.group(3))}
    return n, tot, kinds


def main():
    path = sys.argv[1]
    frac = float(sys.argv[sys.argv.index('--from') + 1]) if '--from' in sys.argv else 0.45
    rows = [r for r in (parse(l) for l in open(path, encoding='utf-8', errors='replace')) if r]
    if len(rows) < 2:
        print('no prof lines')
        return
    last = rows[-1]
    first = next(r for r in rows if r[0] >= last[0] * frac)
    dn = last[0] - first[0]
    if dn <= 0:
        print('window empty')
        return
    tot = (last[1] * last[0] - first[1] * first[0]) / dn
    out = []
    for k in last[2]:
        v = (last[2][k] * last[0] - first[2].get(k, 0.0) * first[0]) / dn
        if v >= 0.005:
            out.append((v, k))
    out.sort(reverse=True)
    print('window %d frames: GPU %.3f ms/frame | ' % (dn, tot) + ' '.join('%s %.3f' % (k, v) for v, k in out))


if __name__ == '__main__':
    main()
