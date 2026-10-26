"""r2 (modern-renderer RE): decode one traced frame (runtime/r2_trace.c, PS2_R2_TRACE) offline.

    python scripts/r2_frame.py <dir> [<n>] [--entries] [--dump <entry index>]

Reads <dir>/frame_<n>.txt (sorted render list + submitter registers) and <dir>/ram_<n>.bin (EE RAM). Walks the
frame's whole VIF1 DMA chain (DMAS start .. write position, CALL/RET followed) in emission order, tracking which VU1
microprogram is resident (MPG uploads from the .vutext overlays), and attributes every DMA tag / VIF code to the
render-list entry whose CALL it sits under. Prints: per priority and per submitter: entries, MSCAL routines (program
+ address), UNPACK formats and bytes, DIRECT (PATH2) qwords, static (outside the per-frame buffer) vs per-frame
bytes. --entries lists every entry; --dump N prints entry N's DMA/VIF stream in full.
"""
import collections
import struct
import sys
from pathlib import Path

PROGS = [  # (name, first EE address, end) of the .vutext overlays (notes/vu-microcode.md)
    ("vu0lib", 0x259DF0, 0x25A0C8), ("P376109499", 0x25A0C8, 0x25AFA8), ("P188055835_2d", 0x25AFA8, 0x25B880),
    ("P188054331", 0x25B880, 0x25C1C8), ("P188054619_clip", 0x25C1C8, 0x25E060), ("stub", 0x25E060, 0x25E098),
    ("P752221467", 0x25E098, 0x25E680), ("P376110651_main", 0x25E680, 0x262330)]
SUBMIT = {0x169020: "glob(168638)", 0x168468: "glob-attach(1681F8)", 0x13A438: "13A1E8", 0x15E178: "15DD48",
          0x16901C: "glob", 0x1698F0: "portrait(169198)", 0x213278: "2d-scene", 0x216654: "2d-text", 0x216ABC: "2d-text2",
          0x217474: "2d-pass", 0x21777C: "2d-shape", 0x217B14: "2d-shape2", 0x217478: "2d-pass", 0x217780: "2d-shape",
          0x00168468: "glob-attach(1681F8)", 0x13A43C: "13A1E8", 0x15E17C: "15DD48", 0x16846C: "glob-attach(1681F8)"}
UNPK = {0: "S-32", 1: "S-16", 2: "S-8", 4: "V2-32", 5: "V2-16", 6: "V2-8", 8: "V3-32", 9: "V3-16", 10: "V3-8",
        12: "V4-32", 13: "V4-16", 14: "V4-8", 15: "V4-5"}


def prog_of(a):
    for n, s, e in PROGS:
        if s <= a < e:
            return n
    return None


class Ram:
    def __init__(self, path):
        self.b = Path(path).read_bytes()

    def u32(self, a):
        return struct.unpack_from("<I", self.b, a & 0x1FFFFFF)[0]

    def u64(self, a):
        return struct.unpack_from("<Q", self.b, a & 0x1FFFFFF)[0]

    def words(self, a, n):
        return struct.unpack_from(f"<{n}I", self.b, a & 0x1FFFFFF)

    def floats(self, a, n):
        return struct.unpack_from(f"<{n}f", self.b, a & 0x1FFFFFF)


class Stat:
    def __init__(self):
        self.mscal = collections.Counter()
        self.unpack = collections.Counter()   # (fmt, mask) -> bytes
        self.unpack_n = collections.Counter()  # (fmt) -> vectors
        self.direct_qw = 0
        self.static_bytes = 0
        self.frame_bytes = 0
        self.tags = 0
        self.stmod = 0
        self.mpg = collections.Counter()
        self.verts = 0      # mesh vertices: V4-32 positions unpacked with a skipping cycle (CL > WL)

    def add(self, o):
        self.mscal.update(o.mscal)
        self.unpack.update(o.unpack)
        self.unpack_n.update(o.unpack_n)
        self.mpg.update(o.mpg)
        self.direct_qw += o.direct_qw
        self.static_bytes += o.static_bytes
        self.frame_bytes += o.frame_bytes
        self.tags += o.tags
        self.verts += o.verts


class Walker:
    """VIF1 chain walker (source chain, CALL depth 2 as on the DMAC)."""

    def __init__(self, ram, fb_lo, fb_hi, log=None):
        self.ram, self.lo, self.hi, self.log = ram, fb_lo, fb_hi, log
        self.prog = "?"
        self.cycle = 0x0101
        self.skip = 0
        self.cur_mpg_src = None

    def vif(self, data_addr, qwc, st):
        nw = qwc * 4
        if not qwc:
            return
        w = self.ram.words(data_addr, nw)
        i = 0
        if self.skip:
            s = min(self.skip, nw)
            i = s
            self.skip -= s
        while i < nw:
            code = w[i]
            i += 1
            cmd, num, imm = (code >> 24) & 0x7F, (code >> 16) & 0xFF, code & 0xFFFF
            ln = 0
            if cmd >= 0x60:
                vn, vl = (cmd >> 2) & 3, cmd & 3
                nn = num or 256
                cl, wl = self.cycle & 0xFF, (self.cycle >> 8) & 0xFF
                nin = nn if (not wl or wl <= cl) else (nn // wl) * cl + min(nn % wl, cl)
                vb = 2 if vl == 3 else (4 >> vl) * (vn + 1)
                ln = (nin * vb + 3) // 4
                fmt = UNPK.get(cmd & 0xF, hex(cmd))
                st.unpack[(fmt, bool(cmd & 0x10))] += ln * 4
                st.unpack_n[fmt] += nn
                if fmt == "V4-32" and cl > wl:
                    st.verts += nn
                if self.log is not None:
                    vals = ""
                    if i + ln <= nw and ln <= 64:
                        if vl == 0:
                            vals = " | " + " ".join(f"{x:.4g}" for x in struct.unpack_from(f"<{ln}f", struct.pack(f"<{ln}I", *w[i:i + ln])))
                        else:
                            vals = " | " + " ".join(f"{x:08X}" for x in w[i:i + ln])
                    self.log.append(f"      UNPACK {fmt}{' mask' if cmd & 0x10 else ''}{' usn' if imm & 0x4000 else ''} "
                                    f"addr 0x{imm & 0x3FF:03X}{'+tops' if imm & 0x8000 else ''} num {nn} "
                                    f"cycle {self.cycle & 0xFF}/{self.cycle >> 8}{vals}")
            else:
                if cmd == 0x20:
                    ln = 1
                elif cmd in (0x30, 0x31):
                    ln = 4
                elif cmd == 0x4A:
                    ln = (num or 256) * 2
                    self.prog = prog_of(data_addr + i * 4) or f"mpg@{data_addr + i * 4:08X}"
                    st.mpg[self.prog] += 1
                elif cmd in (0x50, 0x51):
                    ln = (imm or 65536) * 4
                    st.direct_qw += ln // 4
                elif cmd == 0x01:
                    self.cycle = imm
                elif cmd in (0x14, 0x15, 0x17):
                    st.mscal[(self.prog, imm * 8 if cmd != 0x17 else -1)] += 1
                elif cmd == 0x05:
                    st.stmod += 1
                if self.log is not None and cmd != 0:
                    extra = ""
                    if cmd in (0x20, 0x30, 0x31) and i + ln <= nw:
                        extra = " | " + " ".join(f"{x:08X}" for x in w[i:i + ln])
                    if cmd in (0x50, 0x51) and i + ln <= nw:
                        extra = " | first GIFtag " + " ".join(f"{x:08X}" for x in w[i:i + 4])
                    names = {1: "STCYCL", 2: "OFFSET", 3: "BASE", 4: "ITOP", 5: "STMOD", 6: "MSKPATH3", 7: "MARK",
                             0x10: "FLUSHE", 0x11: "FLUSH", 0x13: "FLUSHA", 0x14: "MSCAL", 0x15: "MSCALF", 0x17: "MSCNT",
                             0x20: "STMASK", 0x30: "STROW", 0x31: "STCOL", 0x4A: "MPG", 0x50: "DIRECT", 0x51: "DIRECTHL"}
                    self.log.append(f"      {names.get(cmd, hex(cmd))} imm 0x{imm:04X} num {num}"
                                    f"{' [prog ' + self.prog + ']' if cmd in (0x14, 0x15, 0x4A) else ''}{extra}")
            take = min(ln, nw - i)
            i += take
            self.skip = ln - take

    def walk(self, tadr, st_for, max_tags=500000, depth_stack=None, stop_at_ret=False):
        """st_for(tag address, call target or None) -> Stat to charge."""
        stack = depth_stack if depth_stack is not None else []
        base_depth = len(stack)
        st = st_for(None)
        for _ in range(max_tags):
            t = self.ram.u64(tadr)
            qwc, tid, addr = t & 0xFFFF, (t >> 28) & 7, (t >> 32) & 0xFFFFFFF0
            data = addr if tid in (0, 3, 4) else tadr + 16
            nxt = tadr + 16 + qwc * 16
            st.tags += 1
            if self.lo <= data < self.hi:
                st.frame_bytes += qwc * 16
            else:
                st.static_bytes += qwc * 16
            if self.log is not None:
                self.log.append(f"   tag @{tadr:08X} {['REFE','CNT','NEXT','REF','REFS','CALL','RET','END'][tid]} "
                                f"qwc {qwc} addr {addr:08X}{' (static)' if not (self.lo <= data < self.hi) else ''}")
            # the tag's upper 64 bits are VIF codes (TTE off on Sly 2? they are transferred with the tag: two words)
            tw = self.ram.words(tadr + 8, 2)
            for c in tw:
                cmd = (c >> 24) & 0x7F
                if cmd and self.log is not None:
                    self.log.append(f"      (tag word) {c:08X}")
                if cmd in (0x14, 0x15):
                    st.mscal[(self.prog, (c & 0xFFFF) * 8)] += 1
                elif cmd == 0x01:
                    self.cycle = c & 0xFFFF
            self.vif(data, qwc, st)
            if tid == 0:
                tadr += 16
                if stop_at_ret and len(stack) <= base_depth:
                    return
                break
            if tid == 1:
                tadr = nxt
            elif tid == 2:
                tadr = addr
            elif tid in (3, 4):
                tadr += 16
            elif tid == 5:
                stack.append(nxt)
                st = st_for(addr) or st
                tadr = addr
            elif tid == 6:
                if not stack:
                    return
                tadr = stack.pop()
                st = st_for(-1) or st
            else:
                return


def matrix_of(ram, c1):
    """The world matrix an entry's header packet uploads (UNPACK V4-32 x4 to VU 0, code 0x6C04C000) + the qw after."""
    b = ram.b[(c1 & 0x1FFFFFF):(c1 & 0x1FFFFFF) + 0x400]
    i = b.find(bytes.fromhex("00C0046C"))
    return b[i + 4:i + 4 + 64 + 20] if i >= 0 else b[:0x60]


def load_frame(d, n):
    rows = []
    for line in open(Path(d) / f"frame_{n}.txt"):
        if line.startswith("#"):
            if "DMAS" in line:
                parts = line.split()
                dmas = [int(x.strip(";"), 16) for x in parts[3:9]] + [int(parts[-1], 16)]
            continue
        p = line.split()
        r = {"i": int(p[0]), "e": int(p[1], 16), "prio": int(p[2]), "key": float(p[4]), "c1": int(p[5], 16),
             "c2": int(p[6], 16), "ra": int(p[7], 16) if len(p) > 7 else None,
             "s": [int(x, 16) for x in p[8:16]] if len(p) > 8 else []}
        rows.append(r)
    return rows, dmas


def main():
    d = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else 0
    rows, dmas = load_frame(d, n)
    ram = Ram(Path(d) / f"ram_{n}.bin")
    fg = dmas[6]
    start, lo = ram.u32(fg + 0x24), ram.u32(fg + 0x14)
    hi, wpos = lo + ram.u32(fg + 0x1C) * 16, dmas[3]
    dump = None
    if "--dump" in sys.argv:
        dump = int(sys.argv[sys.argv.index("--dump") + 1])
    by_call = collections.defaultdict(list)
    for r in rows:
        if r["c1"]:
            by_call[r["c1"]].append((r, 1))
        if r["c2"]:
            by_call[r["c2"]].append((r, 2))
    est = {r["i"]: Stat() for r in rows}
    glob = Stat()      # outside any entry (frame globals, class begin/end)
    cur = {"st": glob, "depth": 0}
    used = collections.Counter()
    logs = {}
    w = Walker(ram, lo, hi)

    # charge by CALL target: top-level CALLs of the frame chain whose target is an entry's c1/c2
    state = {"stack": []}

    def st_for(target):
        if target is None:
            return glob
        if target == -1:
            if state["stack"]:
                state["stack"].pop()
            return state["stack"][-1] if state["stack"] else glob
        cands = by_call.get(target)
        if cands:
            k = used[target]
            used[target] += 1
            r, _ = cands[min(k, len(cands) - 1)]
            s = est[r["i"]]
            if dump is not None and r["i"] == dump:
                w.log = logs.setdefault(dump, [])
            elif w.log is not None and dump is not None:
                w.log = None
        else:
            s = state["stack"][-1] if state["stack"] else glob
        state["stack"].append(s)
        return s

    w.walk(start, st_for)
    if dump is not None:
        r = rows[dump]
        print(f"entry {dump}: prio {r['prio']} key {r['key']:g} c1 {r['c1']:08X} c2 {r['c2']:08X} ra "
              f"{r['ra'] and hex(r['ra'])} s {[hex(x) for x in r['s']]}")
        print("\n".join(logs.get(dump, ["(not reached)"])))
        return

    # per priority / submitter
    def sub_of(r):
        if r["ra"] is None:
            return "world(1D5808)" if r["c2"] == 0 or True else "?"
        return SUBMIT.get(r["ra"], hex(r["ra"]))

    agg = collections.defaultdict(Stat)
    cnt = collections.Counter()
    for r in rows:
        k = (r["prio"], sub_of(r), "static-only" if not r["c2"] and r["ra"] is None else "")
        agg[k].add(est[r["i"]])
        cnt[k] += 1
    print(f"frame {n}: {len(rows)} entries; per-frame buffer {lo:08X}-{hi:08X} (used {wpos - lo} bytes), chain at {start:08X}")
    print(f"outside entries: tags {glob.tags}, unpack {sum(glob.unpack.values())} B, direct {glob.direct_qw} qw, "
          f"MPG {dict(glob.mpg)}, MSCAL {dict(glob.mscal)}")
    tot = Stat()
    for k in sorted(agg):
        s = agg[k]
        tot.add(s)
        ms = ", ".join(f"{p}@{a:04X}x{c}" for (p, a), c in s.mscal.most_common(6))
        up = ", ".join(f"{f}{'m' if m else ''}:{b}" for (f, m), b in s.unpack.most_common(6))
        print(f"prio {k[0]:2} {k[1]:22} {k[2]:11} n={cnt[k]:4} static {s.static_bytes:8} B frame {s.frame_bytes:7} B "
              f"direct {s.direct_qw:6} qw | MSCAL {ms} | UNPACK {up}")
    print(f"TOTAL static {tot.static_bytes} B, per-frame {tot.frame_bytes} B, direct {tot.direct_qw} qw, "
          f"MSCAL {sum(tot.mscal.values())}, unpack vectors {dict(tot.unpack_n)}")
    print("MSCAL by routine:", ", ".join(f"{p}@{a:04X}:{c}" for (p, a), c in (tot.mscal + glob.mscal).most_common()))
    # per kind (by the VU program / routines the entry's chain starts)
    def kind_of(st):
        progs = {p for (p, a) in st.mscal}
        addrs = {a for (p, a) in st.mscal if p == "P376110651_main"}
        if "P188054331" in progs:
            return "outline (188054331)"
        if "P188054619_clip" in progs:
            return "shadow volume (188054619)"
        if "P376109499" in progs:
            return "particles (376109499)"
        if "P188055835_2d" in progs or not st.mscal:
            return "2d / no VU (188055835)"
        if "P752221467" in progs:
            return "attachment (752221467)"
        if addrs & {0x2EF8, 0x2F80, 0x2FC8, 0x3358}:
            return "mesh lit/skinned (main)"
        if addrs & {0x3628, 0x37A8}:
            return "untextured volume (main 3628/37A8)"
        return "mesh prelit (main)"
    kinds = collections.defaultdict(Stat)
    kcnt = collections.Counter()
    for r in rows:
        k = kind_of(est[r["i"]])
        r["kind"] = k
        kinds[k].add(est[r["i"]])
        kcnt[k] += 1
    print("by kind:")
    for k in sorted(kinds, key=lambda k: -kcnt[k]):
        s = kinds[k]
        print(f"  {k:34} entries {kcnt[k]:4}  vertices~ {s.verts:6}  static {s.static_bytes / 1024:8.1f} KB  "
              f"per-frame {s.frame_bytes / 1024:6.1f} KB  MSCAL {sum(s.mscal.values()):5}")
    print(f"  TOTAL vertices~ {tot.verts}")
    if "--prev" in sys.argv:          # static vs dynamic: compare with another traced frame of the same run
        n2 = int(sys.argv[sys.argv.index("--prev") + 1])
        rows2, _ = load_frame(d, n2)
        ram2 = Ram(Path(d) / f"ram_{n2}.bin")
        occ = collections.Counter()
        prev = {}
        for r in rows2:
            key = (r["c2"] or r["c1"], occ[r["c2"] or r["c1"]])
            occ[r["c2"] or r["c1"]] += 1
            prev[key] = r
        occ = collections.Counter()
        same = collections.Counter()
        moved = collections.Counter()
        new = collections.Counter()
        for r in rows:
            key = (r["c2"] or r["c1"], occ[r["c2"] or r["c1"]])
            occ[r["c2"] or r["c1"]] += 1
            o = prev.get(key)
            if not o:
                new[r["kind"]] += 1
            elif not r["c2"] or matrix_of(ram, r["c1"]) == matrix_of(ram2, o["c1"]):
                same[r["kind"]] += 1
            else:
                moved[r["kind"]] += 1
        print(f"static vs dynamic against frame {n2} (same mesh + identical header packet = unchanged):")
        for k in sorted(kcnt, key=lambda k: -kcnt[k]):
            print(f"  {k:34} unchanged {same[k]:4}  changed {moved[k]:4}  not in other frame {new[k]:4}")
    if "--entries" in sys.argv:
        for r in rows:
            s = est[r["i"]]
            ms = ",".join(f"{p[1:4]}@{a:04X}x{c}" for (p, a), c in s.mscal.items())
            print(f"{r['i']:4} prio {r['prio']:2} {sub_of(r):20} c1 {r['c1']:08X} c2 {r['c2']:08X} "
                  f"st {s.static_bytes:6} fr {s.frame_bytes:5} dq {s.direct_qw:4} {ms}")


if __name__ == "__main__":
    main()
