"""How much of a frame's VU1 input is the same from frame to frame (pc-research, notes/perf-plan.md).

    python scripts/chain_stats.py <PS2_CHAIN_STATS file>

Input: runtime/chain_dump.c PS2_CHAIN_STATS lines (C <tadr> <vblank> per chain, then T <id> <data addr> <qwc> <hash>
<unpack words> <direct words> <mpg words> <other words> <mscals> per DMA tag). For each pair of consecutive chains:
the bytes per tag kind and the share of data blocks (by address and content) that the next chain sends again unchanged
- i.e. what could stay on the GPU (or in a host-side cache) instead of being re-sent.
"""
import sys
from collections import defaultdict

IDS = ["refe", "cnt", "next", "ref", "refs", "call", "ret", "end"]


def load(path):
    chains = []
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == "C":
            chains.append({"tadr": p[1], "vblank": int(p[2]), "tags": []})
        elif p[0] == "T" and chains:
            t = dict(id=int(p[1]), addr=int(p[2], 16), qwc=int(p[3]), h=p[4], unpack=int(p[5]), direct=int(p[6]),
                     mpg=int(p[7]), other=int(p[8]), mscal=int(p[9]))
            chains[-1]["tags"].append(t)
    return chains


def summary(c):
    by = defaultdict(lambda: [0, 0, 0, 0])
    for t in c["tags"]:
        k = IDS[t["id"]]
        by[k][0] += 1
        by[k][1] += t["qwc"] * 16
        by[k][2] += t["unpack"] * 4
        by[k][3] += t["direct"] * 4
    tot = sum(t["qwc"] * 16 for t in c["tags"])
    up = sum(t["unpack"] * 4 for t in c["tags"])
    di = sum(t["direct"] * 4 for t in c["tags"])
    ms = sum(t["mscal"] for t in c["tags"])
    print(f"chain at VBlank {c['vblank']}: {len(c['tags'])} tags, {tot / 1024:.0f} KB data, unpack {up / 1024:.0f} KB, "
          f"direct (PATH2) {di / 1024:.0f} KB, MSCALs {ms}")
    for k, v in sorted(by.items(), key=lambda kv: -kv[1][1]):
        print(f"    {k:5s} {v[0]:6d} tags {v[1] / 1024:8.0f} KB (unpack {v[2] / 1024:.0f} KB, direct {v[3] / 1024:.0f} KB)")


def compare(a, b):
    seen = {(t["addr"], t["qwc"], t["h"]) for t in a["tags"]}
    addrs = {(t["addr"], t["qwc"]) for t in a["tags"]}
    same = defaultdict(int)
    moved = defaultdict(int)
    new = defaultdict(int)
    total = defaultdict(int)
    hashes_a = {t["h"] for t in a["tags"]}
    for t in b["tags"]:
        k = IDS[t["id"]]
        n = t["unpack"] * 4
        total[k] += n
        if (t["addr"], t["qwc"], t["h"]) in seen:
            same[k] += n
        elif t["h"] in hashes_a:
            moved[k] += n            # same content at another address (e.g. the other half of a double buffer)
        else:
            new[k] += n
    T = sum(total.values()) or 1
    print(f"  VBlank {a['vblank']} -> {b['vblank']}: unpack bytes {T / 1024:.0f} KB; unchanged at the same address "
          f"{100 * sum(same.values()) / T:.1f}%, same content elsewhere {100 * sum(moved.values()) / T:.1f}%, "
          f"new {100 * sum(new.values()) / T:.1f}%")
    for k in total:
        tk = total[k] or 1
        print(f"    {k:5s} unpack {total[k] / 1024:7.0f} KB: same {100 * same[k] / tk:5.1f}%  moved {100 * moved[k] / tk:5.1f}%  "
              f"new {100 * new[k] / tk:5.1f}%")


def main():
    chains = load(sys.argv[1])
    for c in chains:
        summary(c)
    for i in range(len(chains) - 1):
        compare(chains[i], chains[i + 1])


main()
