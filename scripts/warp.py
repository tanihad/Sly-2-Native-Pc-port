"""Start Sly 2 at any episode (or job) by writing a memory-card save that the game resumes from.

Usage (from the project root or a worktree, with the main checkout's .venv python):
    warp.py list                                     episodes, their hub worlds and jobs
    warp.py make --episode 4 [--job m4_recon] [--card build/rel/memcard/slot0] [--slot 1]
    warp.py dump <save file> [--all]                 decode a save (non-default fields)
    warp.py set <save file> path=value ...           edit fields of a save in place
    warp.py schema --ram <ram.bin>                   regenerate scripts/warp_schema.txt

Then boot the game with that memory card and press START on the title ("Resume Game"); see
notes/warp.md for the run commands and what each episode start looks like.

How it works (notes/warp.md): a save file is the game's whole progress blob (0x5408 bytes at EE
0x3D4A60), laid out by a schema the game loads from every level file (scripts/warp_schema.txt,
extracted from a RAM dump). "Resume" loads the hub of save.episodeCurrent; the hub's scripts then
run from the task states (t<episode>_<task>.state / is_complete) and mission states
(m<episode>_<job>.is_complete).
"""
import argparse
import datetime
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHEMA_FILE = Path(__file__).resolve().parent / "warp_schema.txt"
SAVE_SIZE = 0x5408
SAVE_DIR = "BASCUS-97316YAOTWTD!"

# Episodes: (schema name, title, place, hub world id). World ids follow the order of the level
# lists in the schema (episode.<name>.level.*); the game maps wid -> episode with a table at
# *(0x3E1C44). Resume loads the hub (exterior) of episodeCurrent, not widCurrent.
EPISODES = [
    ("intro",     "Prologue: Cairo Museum Break-In", "Cairo", 0),
    ("nightclub", "Episode 1: The Black Chateau", "Paris", 2),
    ("palace",    "Episode 2: A Starry Eyed Encounter", "India", 8),
    ("temple",    "Episode 3: The Predator Awakens", "India", 12),
    ("prison",    "Episode 4: Jailbreak", "Prague", 14),
    ("castle",    "Episode 5: A Tangled Web", "Prague", 17),
    ("train",     "Episode 6: He Who Tames the Iron Horse", "Canada", 27),
    ("sawmill",   "Episode 7: Menace from the North, Eh!", "Canada", 32),
    ("blimp",     "Episode 8: Anatomy for Disaster", "Blimp", 38),
]

INT_TYPES = {0, 1, 2, 9, 10}
T_FLOAT, T_BOOL = 3, 4


class Schema:
    def __init__(self, path=SCHEMA_FILE):
        self.fields = {}          # path -> (type, count, offset)
        self.order = []
        self.trailer = None
        for line in path.read_text().splitlines():
            if line.startswith("# trailer"):
                a, b = line.split()[2:4]
                self.trailer = (int(a, 16), int(b))
                continue
            if not line or line.startswith("#"):
                continue
            p, t, c, o = line.split()
            self.fields[p] = (int(t), int(c), int(o, 16))
            self.order.append(p)
        if not self.trailer:
            sys.exit(f"{path}: no trailer line")

    def groups(self, prefix):
        """Names of the top-level groups starting with prefix (e.g. 't1_'), in schema order."""
        seen = []
        for p in self.order:
            g = p.split(".")[0]
            if g.startswith(prefix) and "." in p and g not in seen:
                seen.append(g)
        return seen


class Save:
    def __init__(self, schema, data=None):
        self.s = schema
        self.b = bytearray(data if data is not None else SAVE_SIZE)
        if len(self.b) != SAVE_SIZE:
            sys.exit(f"save must be {SAVE_SIZE} bytes, got {len(self.b)}")

    def _field(self, path):
        idx = None
        if path.endswith("]"):
            path, i = path[:-1].split("[")
            idx = int(i, 0)
        if path not in self.s.fields:
            sys.exit(f"unknown field {path}")
        t, c, o = self.s.fields[path]
        return t, c, o, idx

    def get(self, path):
        t, c, o, i = self._field(path)
        if t == T_BOOL and c:
            k = i or 0
            return (self.b[o + k // 8] >> (k % 8)) & 1
        o += 4 * (i or 0)
        if t == T_FLOAT:
            return struct.unpack_from("<f", self.b, o)[0]
        return struct.unpack_from("<i", self.b, o)[0]

    def set(self, path, v):
        t, c, o, i = self._field(path)
        if t == T_BOOL and c:
            k = i or 0
            if v:
                self.b[o + k // 8] |= 1 << (k % 8)
            else:
                self.b[o + k // 8] &= ~(1 << (k % 8)) & 0xFF
            return
        o += 4 * (i or 0)
        if t == T_FLOAT:
            struct.pack_into("<f", self.b, o, float(v))
        else:
            struct.pack_into("<i", self.b, o, int(v))

    def has(self, path):
        return path.split("[")[0] in self.s.fields

    def finish(self):
        """Mark every task as stored (the game sets from_memcard when it saves) and write the trailer."""
        for p, (t, c, o) in self.s.fields.items():
            if p.endswith(".from_memcard"):
                struct.pack_into("<i", self.b, o, 1)
        struct.pack_into("<II", self.b, 0x5400, *self.s.trailer)
        return bytes(self.b)


def parse_value(s):
    try:
        return int(s, 0)
    except ValueError:
        return float(s)


# ---- what a save at the start of an episode holds ----------------------------------------------

def complete_group(sv, g):
    """Mark a mission (m*) or task (t*) group as done."""
    for f, v in (("is_complete", 1), ("complete_count", 1), ("state", TASK_DONE)):
        if sv.has(f"{g}.{f}"):
            sv.set(f"{g}.{f}", v)


# Task states (observed): 0 not started, 1 active (current objective / available), 2 completed
# (complete_count +1), 3 closed without completing (e.g. intro steps the story skipped past).
TASK_DONE = 2


def build(schema, episode, job=None, sets=()):
    sv = Save(schema)
    name, _, _, hub = EPISODES[episode]
    sv.set("episodeCurrent", episode)
    sv.set("episodeBest", episode)
    sv.set("widCurrent", hub)
    sv.set("oidPoCurrent", 7)              # Sly (JT)
    sv.set("dt", 60.0)                     # play time: 0 would mean "empty slot / new game"
    sv.set(f"episode.{name}.dt", 1.0)
    sv.set("flags.fAutosaveWarned", 1)     # skip the autosave notice
    for who, hp in (("sly", 40), ("bentley", 40), ("murray", 60)):
        sv.set(f"{who}.hpCur", hp)
        sv.set(f"{who}.mpCur", 100)
    for e in range(episode):               # earlier episodes: every job and task finished
        en = EPISODES[e][0]
        sv.set(f"episode.{en}.percent_complete", 1)
        for g in schema.groups(f"m{e}_") + schema.groups(f"t{e}_"):
            complete_group(sv, g)
    if job:
        for g in job_prerequisites(schema, episode, job):
            complete_group(sv, g)
    for s in sets:
        p, v = s.split("=", 1)
        sv.set(p, parse_value(v))
    return sv.finish()


def job_prerequisites(schema, episode, job):
    raise SystemExit("--job is not supported yet")


# ---- memory card ----------------------------------------------------------------------------------

def jst_now(offset_s=0):
    t = datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(hours=9, seconds=offset_s)
    return t.strftime("%Y-%m-%dT%H:%M:%S")


def write_card(card, slot, data, keep_others):
    d = Path(card) / SAVE_DIR
    d.mkdir(parents=True, exist_ok=True)
    tag = d / SAVE_DIR
    if not tag.exists():
        tag.write_bytes(b"Sly 2")
    for n in (1, 2, 3):
        f = d / f"save{n}"
        if n == slot:
            f.write_bytes(data)
        elif not keep_others or not f.exists():
            f.write_bytes(bytes(SAVE_SIZE))   # empty slot (play time 0)
    # mc.c's sidecar holds each file's card timestamps; the game resumes the newest slot.
    meta = d / "%meta"
    lines = meta.read_text().splitlines() if meta.exists() else [
        "# memory card directory metadata: name mode attr created modified (JST)",
        f". 8427 00000000 {jst_now()} {jst_now()}"]
    names = [l.split()[0] for l in lines]
    for n in (1, 2, 3):
        if n != slot and keep_others:
            continue
        stamp = jst_now(0 if n == slot else -3600)
        line = f"save{n} 8497 00000000 {stamp} {stamp}"
        if f"save{n}" in names:
            lines[names.index(f"save{n}")] = line
        else:
            lines.append(line)
    meta.write_text("\n".join(lines) + "\n")
    return d / f"save{slot}"


# ---- commands --------------------------------------------------------------------------------------

def cmd_list(schema, args):
    for i, (name, title, place, hub) in enumerate(EPISODES):
        jobs = " ".join(g[3:] for g in schema.groups(f"m{i}_"))
        print(f"{i}  {title} ({place}), hub world {hub}\n   jobs: {jobs}")


def cmd_make(schema, args):
    if args.episode == 0 and not args.job and not args.set:
        # The prologue starts from a new game: a save directory with empty slots (play time 0)
        # skips the "create a save file?" prompt, and START on the title begins the prologue.
        data = bytes(SAVE_SIZE)
    else:
        data = build(schema, args.episode, args.job, args.set or ())
    card = args.card or str(ROOT / "build" / "rel" / "memcard" / "slot0")
    f = write_card(card, args.slot, data, args.keep_others)
    print(f"wrote {f} ({EPISODES[args.episode][1]})")


def cmd_dump(schema, args):
    sv = Save(schema, Path(args.save).read_bytes())
    for p in schema.order:
        t, c, o = schema.fields[p]
        if p.endswith(".from_memcard") and not args.all:
            continue
        raw = sv.b[o:o + (4 * max(c, 1) if not (t == T_BOOL and c) else (c + 7) // 8)]
        if not any(raw) and not args.all:
            continue
        if t == T_BOOL and c:
            v = "".join(str(sv.get(f"{p}[{k}]")) for k in range(c))
        elif c:
            v = " ".join(f"{sv.get(f'{p}[{k}]'):g}" for k in range(c))
        else:
            v = f"{sv.get(p):g}"
        print(f"{p} = {v}")
    tr = struct.unpack_from("<II", sv.b, 0x5400)
    print(f"# trailer {tr[0]:X} {tr[1]} ({'ok' if tr == schema.trailer else 'MISMATCH: the game would wipe this save'})")


def cmd_set(schema, args):
    f = Path(args.save)
    sv = Save(schema, f.read_bytes())
    for s in args.assign:
        p, v = s.split("=", 1)
        sv.set(p, parse_value(v))
    f.write_bytes(bytes(sv.b))


def cmd_schema(schema_unused, args):
    ram = Path(args.ram).read_bytes()
    u32 = lambda a: struct.unpack_from("<I", ram, a & 0x1FFFFFF)[0]
    gs = 0x3E0EA8                              # the game-state object: {count, entries, symbol hash}
    count, entries = u32(gs), u32(gs + 4)
    names = {}
    for bucket in range(0x35):
        n = u32(gs + 8 + bucket * 4)
        while n:
            a = u32(n + 4) & 0x1FFFFFF
            names[u32(n + 8)] = ram[a:ram.index(b"\0", a)].decode()
            n = u32(n + 12)
    ents = [struct.unpack_from("<hBBHHH", ram, (entries + i * 10) & 0x1FFFFFF) for i in range(count)]
    out = []

    def walk(i, path):
        while i:
            sym, t, cnt, off, child, sib = ents[i]
            p = path + [names[sym]]
            if t == 0xFF:
                walk(child, p)
            else:
                out.append(f"{'.'.join(p)} {t} {cnt} {off:X}")
            i = sib

    walk(ents[0][4], [])
    tab = ram[entries & 0x1FFFFFF:(entries & 0x1FFFFFF) + count * 10]
    head = SCHEMA_FILE.read_text().split("# trailer")[0] if SCHEMA_FILE.exists() else ""
    trailer = f"# trailer {struct.unpack_from('<H', tab, count * 10 - 6)[0]:X} {sum(tab)}\n"
    SCHEMA_FILE.write_text(head + trailer + "\n".join(out) + "\n")
    print(f"{SCHEMA_FILE}: {len(out)} fields, {trailer.strip()}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    m = sub.add_parser("make")
    m.add_argument("--episode", type=int, required=True, choices=range(len(EPISODES)))
    m.add_argument("--job", help="start at this job (mission name from 'list', e.g. m1_recon)")
    m.add_argument("--card", help="memory card directory (default build/rel/memcard/slot0)")
    m.add_argument("--slot", type=int, default=1, choices=(1, 2, 3))
    m.add_argument("--keep-others", action="store_true", help="keep the other two save slots")
    m.add_argument("--set", action="append", metavar="PATH=VALUE", help="extra field (repeatable)")
    d = sub.add_parser("dump")
    d.add_argument("save")
    d.add_argument("--all", action="store_true")
    s = sub.add_parser("set")
    s.add_argument("save")
    s.add_argument("assign", nargs="+")
    r = sub.add_parser("schema")
    r.add_argument("--ram", required=True)
    args = ap.parse_args()
    schema = Schema() if args.cmd != "schema" else None
    {"list": cmd_list, "make": cmd_make, "dump": cmd_dump, "set": cmd_set, "schema": cmd_schema}[args.cmd](schema, args)


if __name__ == "__main__":
    main()
