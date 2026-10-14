"""Start Sly 2 at any episode or job by writing a memory-card save that the game resumes from.

Usage (from the project root or a worktree, with the main checkout's .venv python):
    warp.py list [--episode N]                       episodes, hub worlds, jobs (in game order)
    warp.py make --episode 4 [--card DIR]            save at the start of episode 4
    warp.py make --job m4_recon [--card DIR]         save with episode 4's jobs before "recon" done
    warp.py dump <save file> [--all]                 decode a save (non-default fields)
    warp.py set <save file> path=value ...           edit fields of a save in place
    warp.py schema --ram <ram.bin>                   regenerate scripts/warp_schema.txt
    warp.py tasks --ram <ram.bin> [--ram ...]        regenerate scripts/warp_tasks.txt (task graph)

make options: --card DIR (default build/rel/memcard/slot0), --slot 1..3, --keep-others,
--set path=value, --complete PREFIX, --activate TASK, --character sly|bentley|murray.
Then boot the game with that card (run_game.ps1 -Card DIR, or scripts/warp_run.ps1 which does
everything) and press START on the title ("Resume Game"). Full guide: notes/warp.md.

How it works: a save file is the game's whole progress blob (0x5408 bytes at EE 0x3D4A60), laid out
by a schema the game loads with every level (scripts/warp_schema.txt). "Resume" loads the hub of
save.episodeCurrent; the hub's task objects then take their states from the save. Jobs are chains
of tasks (scripts/warp_tasks.txt: each task's prerequisites, read from the game's own task objects),
so a job warp marks every task the job depends on as done and makes the next ones available, as the
game would have done when those tasks completed.
"""
import argparse
import datetime
import struct
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHEMA_FILE = Path(__file__).resolve().parent / "warp_schema.txt"
TASKS_FILE = Path(__file__).resolve().parent / "warp_tasks.txt"
SAVE_SIZE = 0x5408
SAVE_DIR = "BASCUS-97316YAOTWTD!"

# Episodes: (schema name, title, place, hub world id). World ids follow the order of the level
# lists in the schema (episode.<name>.level.*); the game maps wid -> episode with a table at
# *(0x3E1C44). Resume loads the hub (exterior) of episodeCurrent, not widCurrent.
EPISODES = [
    ("intro",     "Prologue: Cairo Museum Break-In", "Cairo", 0),
    ("nightclub", "Episode 1: The Black Chateau", "Paris", 2),
    ("palace",    "Episode 2: A Starry Eyed Encounter", "India", 8),
    ("temple",    "Episode 3: The Predator Awakes", "India", 12),
    ("prison",    "Episode 4: Jailbreak", "Prague", 14),
    ("castle",    "Episode 5: A Tangled Web", "Prague", 17),
    ("train",     "Episode 6: He Who Tames the Iron Horse", "Canada", 27),
    ("sawmill",   "Episode 7: Menace in the North, Eh?", "Canada", 32),
    ("blimp",     "Episode 8: Anatomy for Disaster", "Blimp", 38),
]
CHARACTERS = {"sly": 7, "bentley": 8, "murray": 9}       # player object ids (oidPoCurrent)
CHAR_NAMES = {v: k for k, v in CHARACTERS.items()}
# powerUps.mppukfAcquired bits (power-up table at 0x2BCD00, 32-byte records: kind, character, cost,
# ..., shop position, name/description text ids): 7-14 Bentley's gadgets, 15-22 Murray's, 23-30 Sly's
# (23 Smoke Bomb, 24 Combat Dodge, 25 Stealth Slide, 26 Alarm Clock, 27 Paraglide, 28 Silent
# Obliteration, 29, 30), 31-42 moves that cost nothing. Two are bought because the story needs them:
STORY_GADGETS = {"t5_paraglide_purchase": 27, "t7_alarmclock_purchase": 26}
SHOP_GADGETS = range(7, 31)

T_FLOAT, T_BOOL = 3, 4
# Task states (the game's task objects, FUN_001657d0): 0 not started, 1 active (available job /
# current objective), 2 completed, 3 closed. Loading a save (FUN_00164270) turns state 1/2 tasks of a
# job back into 0/1 - a job that was not finished restarts - so finished tasks are saved as 3, as the
# game itself leaves them once the story has moved on.
TASK_ACTIVE, TASK_DONE = 1, 3


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


# ---- the task graph -------------------------------------------------------------------------------

class Task:
    def __init__(self, name, oid, mission, pre, wid, warp, po):
        self.name, self.oid, self.mission, self.pre = name, oid, mission, pre
        self.wid, self.warp, self.po = wid, warp, po


def load_tasks(path=TASKS_FILE):
    tasks = {}
    for line in path.read_text().splitlines():
        if not line or line.startswith("#") or line.startswith("job "):
            continue
        name, oid, mission, pre, wid, warp, po = line.split()
        tasks[name] = Task(name, int(oid), None if mission == "-" else mission, [] if pre == "-" else pre.split(","),
                           int(wid), int(warp), int(po))
    return tasks


def load_job_titles(path=TASKS_FILE):
    titles = {}
    for line in path.read_text().splitlines():
        if line.startswith("job "):
            _, m, title = line.split(" ", 2)
            titles[m] = title
    return titles


def episode_of(name):
    return int(name[1])                    # t3_..., m3_...


def topo(tasks, names):
    """names (only) in an order where prerequisites come first (ties: given order)."""
    order, seen = [], set()

    def visit(n):
        if n in seen:
            return
        seen.add(n)
        for p in tasks[n].pre:
            if p in tasks:
                visit(p)
        order.append(n)
    for n in names:
        visit(n)
    keep = set(names)
    return [n for n in order if n in keep]


def mission_tasks(tasks, mission):
    return [t for t in tasks if tasks[t].mission == mission]


def mission_entry(tasks, mission):
    """The first task of a job: its prerequisites all lie outside the job."""
    mine = mission_tasks(tasks, mission)
    entries = [t for t in mine if all(tasks.get(p) is None or tasks[p].mission != mission for p in tasks[t].pre)]
    if not entries:
        sys.exit(f"{mission}: no tasks in {TASKS_FILE.name}")
    return topo(tasks, entries)[0]


def ancestors(tasks, name):
    out, todo = set(), list(tasks[name].pre)
    while todo:
        n = todo.pop()
        if n in out or n not in tasks:
            continue
        out.add(n)
        todo.extend(tasks[n].pre)
    return out


def job_order(tasks, episode):
    """The episode's jobs in the order their first tasks become available (a valid play order)."""
    ms = {t.mission for t in tasks.values() if t.mission and episode_of(t.mission) == episode}
    entries = {m: mission_entry(tasks, m) for m in ms}
    ep_tasks = [t for t in tasks if episode_of(t) == episode]
    rank = {t: i for i, t in enumerate(topo(tasks, ep_tasks))}
    return sorted(ms, key=lambda m: (len(ancestors(tasks, entries[m])), rank[entries[m]]))


def resolve_job(tasks, job, episode):
    """job: 'm4_recon', 'recon' (with --episode) or a task name 't4_recon_intro'."""
    if job in tasks:
        return job, tasks[job].mission
    m = job if job.startswith("m") and job[1:2].isdigit() else (f"m{episode}_{job}" if episode is not None else None)
    if m is None or not mission_tasks(tasks, m):
        sys.exit(f"unknown job {job!r}; see 'warp.py list'")
    return mission_entry(tasks, m), m


# ---- what a save holds ----------------------------------------------------------------------------

def complete_group(sv, g, state=TASK_DONE):
    """Mark a mission (m*) or task (t*) group as done."""
    for f, v in (("is_complete", 1), ("complete_count", 1), ("state", state)):
        if sv.has(f"{g}.{f}"):
            sv.set(f"{g}.{f}", v)


def start_point(tasks, entry, mission):
    """The task the game starts when the player enters the job: the entry itself if it has a start
    world, else the job's first task (in play order) that comes after it and has one."""
    if tasks[entry].wid != -1:
        return entry
    after = [t for t in topo(tasks, mission_tasks(tasks, mission)) if entry in ancestors(tasks, t)]
    return next((t for t in after if tasks[t].wid != -1), entry)


def build(schema, episode, job=None, sets=(), complete=(), activate=(), character=None, coins=None,
          gadgets=False, direct=False):
    sv = Save(schema)
    name, _, _, hub = EPISODES[episode]
    sv.set("episodeCurrent", episode)
    sv.set("episodeBest", episode)
    sv.set("widCurrent", hub)
    sv.set("oidPoCurrent", CHARACTERS["sly"])
    sv.set("dt", 60.0)                     # play time: 0 would mean "empty slot / new game"
    sv.set(f"episode.{name}.dt", 1.0)
    sv.set("flags.fAutosaveWarned", 1)     # skip the autosave notice
    for who, hp in (("sly", 40), ("bentley", 40), ("murray", 60)):
        sv.set(f"{who}.hpCur", hp)
        sv.set(f"{who}.mpCur", 100)
    for e in range(episode):               # earlier episodes: every job and task finished
        en = EPISODES[e][0]
        # 100 as the game saves a finished episode (seen after episode 5's last stage). The game's own
        # save at the next episode's menu has episodeCurrent 0, and resume then opens the episode
        # menu only if the prologue is above 99 (1 here resumed into the Cairo museum).
        sv.set(f"episode.{en}.percent_complete", 100)
        for g in schema.groups(f"m{e}_") + schema.groups(f"t{e}_"):
            complete_group(sv, g)
    notes = []
    po = -1
    if job:
        tasks = load_tasks()
        entry, mission = resolve_job(tasks, job, episode)
        if direct:                         # the runtime starts this task on resume (PS2_WARP_TASK)
            entry = start_point(tasks, entry, mission)
            t = tasks[entry]
            notes.append(f"direct start: PS2_WARP_TASK={t.oid}" + (f":{t.wid}" if t.wid != -1 else ""))
        done = ancestors(tasks, entry)
        ep_tasks = [t for t in tasks if episode_of(t) == episode]
        # A job whose exit tasks (those later tasks wait for) are done is finished as a whole, side
        # tasks included.
        for m in {tasks[t].mission for t in ep_tasks if tasks[t].mission} - {mission}:
            mt = set(mission_tasks(tasks, m))
            exits = {p for t in ep_tasks if t not in mt for p in tasks[t].pre if p in mt}
            if exits and exits <= done:
                done.update(mt)
        for t in done:
            complete_group(sv, t)
        # Tasks whose prerequisites are now all done are what the game would have made available
        # (state 1): the target job's first task, jobs open in parallel, reminders.
        frontier = [t for t in ep_tasks if t not in done and tasks[t].pre and all(p in done for p in tasks[t].pre)]
        for t in frontier:
            sv.set(f"{t}.state", TASK_ACTIVE)
        for m in {tasks[t].mission for t in ep_tasks if tasks[t].mission}:
            if all(t in done for t in mission_tasks(tasks, m)):
                complete_group(sv, m)
        po = tasks[entry].po if tasks[entry].po != -1 else next(
            (tasks[t].po for t in topo(tasks, mission_tasks(tasks, mission)) if tasks[t].po != -1), -1)
        notes.append(f"job {mission}: first task {entry} active; {len(done)} earlier tasks done; also active: "
                     + (" ".join(t for t in sorted(frontier) if t != entry) or "-"))
        if po in CHAR_NAMES:
            notes.append(f"the job is played as {CHAR_NAMES[po]} (the game switches when the job starts)")
    if episode == 4 and (not job or po == CHARACTERS["bentley"]):
        # Jailbreak opens with Sly and Murray in prison: Bentley alone runs the first jobs, and the
        # safehouse only offers him (with Sly as the saved player it opens on ThiefNet instead).
        sv.set("oidPoCurrent", CHARACTERS["bentley"])
    if character:
        sv.set("oidPoCurrent", CHARACTERS[character])
    for task, bit in STORY_GADGETS.items():    # gadgets the story made the player buy
        if sv.get(f"{task}.is_complete"):
            sv.set(f"powerUps.mppukfAcquired[{bit}]", 1)
    if gadgets:
        for bit in SHOP_GADGETS:
            sv.set(f"powerUps.mppukfAcquired[{bit}]", 1)
    if coins is not None:
        sv.set("loot.ccoin", coins)
    for prefix in complete:                # --complete t1_sat_ : every m*/t* group with that prefix
        groups = schema.groups(prefix)
        if not groups:
            sys.exit(f"--complete {prefix}: no mission/task group starts with that")
        for g in groups:
            complete_group(sv, g)
    for task in activate:                  # --activate t1_recon_intro : state 1 (active/available)
        if not sv.has(f"{task}.state"):
            sys.exit(f"--activate {task}: not a task")
        sv.set(f"{task}.state", TASK_ACTIVE)
    for s in sets:
        p, v = s.split("=", 1)
        sv.set(p, parse_value(v))
    return sv.finish(), notes


# ---- memory card ----------------------------------------------------------------------------------

def jst_now(offset_s=0):
    t = datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(hours=9, seconds=offset_s)
    return t.strftime("%Y-%m-%dT%H:%M:%S")


def write_card(card, slot, data, keep_others):
    """Write the save into <card>/BASCUS-97316YAOTWTD!/save<slot> (runtime/mc.c's host layout).
    Saves that would be overwritten (play time not 0) are copied to <card>.warp-backup-<time>/ first."""
    d = Path(card) / SAVE_DIR
    olds = [f for f in sorted(d.glob("save[123]")) if f.stat().st_size == SAVE_SIZE
            and struct.unpack_from("<f", f.read_bytes(), 0x10)[0] != 0.0
            and (f.name == f"save{slot}" or not keep_others)]
    if olds:
        bak = Path(f"{Path(card).resolve()}.warp-backup-{datetime.datetime.now():%Y%m%d-%H%M%S}") / SAVE_DIR
        bak.mkdir(parents=True, exist_ok=True)
        for f in olds:
            (bak / f.name).write_bytes(f.read_bytes())
        print(f"kept the saves it replaces in {bak}")
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


# ---- reading the game's structures from an EE RAM dump (PS2_INPUT token "ram") --------------------

class Ram:
    GS = 0x3E0EA8                  # game-state object: {count, entries, 0x35-bucket symbol hash}

    def __init__(self, path):
        self.b = Path(path).read_bytes()
        if len(self.b) < 0x2000000:
            sys.exit(f"{path}: not a 32 MB EE RAM dump")
        self.names = {}
        for bucket in range(0x35):
            n = self.u32(self.GS + 8 + bucket * 4)
            while n:
                self.names[self.u32(n + 8)] = self.cstr(self.u32(n + 4))
                n = self.u32(n + 12)

    def u32(self, a):
        return struct.unpack_from("<I", self.b, a & 0x1FFFFFF)[0]

    def i32(self, a):
        return struct.unpack_from("<i", self.b, a & 0x1FFFFFF)[0]

    def u16(self, a):
        return struct.unpack_from("<H", self.b, a & 0x1FFFFFF)[0]

    def cstr(self, a):
        a &= 0x1FFFFFF
        return self.b[a:self.b.index(b"\0", a)].decode("latin1")

    def path_name(self, a):
        """A game-state path {count, u16 *symbols} stored at a -> first symbol's name."""
        c, p = self.i32(a), self.u32(a + 4)
        if not (1 <= c <= 8 and 0x100000 <= p < 0x2000000 and not p & 1):
            return None
        return self.names.get(self.u16(p))


def task_objects(ram):
    """Native task objects: game-state path at +0xE0 = <task>.state (FUN_001643b0 writes +0xC8
    focus_count, +0xD0 complete_count, +0xD8 is_complete, +0xE0 state, +0xE8 from_memcard).
    Fields: +0x18 oid, +0x54 state, +0x7C mission object, +0x94/+0x98 prerequisites (count, object
    pointers), +0xA0/+0xA4 successors, +0xB8 start warp oid, +0xBC start world, +0xC0 character."""
    sid_state = next(k for k, v in ram.names.items() if v == "state")
    found = []
    for a in range(0x100000, 0x2000000 - 0x100, 4):
        if ram.i32(a + 0xE0) != 2:
            continue
        p = ram.u32(a + 0xE4)
        if not (0x100000 <= p < 0x2000000) or p & 1 or ram.u16(p + 2) != sid_state:
            continue
        n = ram.names.get(ram.u16(p))
        if n and n.startswith("t") and n[1:2].isdigit():
            found.append((a, n))
    vt = Counter(ram.u32(a + 0xC) for a, _ in found).most_common(1)[0][0]
    return {a: n for a, n in found if ram.u32(a + 0xC) == vt}


def ram_text(ram, tid):
    """A string of the loaded level's text bank: {id, char *} pairs (English: the language setting)."""
    def string_at(p):
        if not 0x100000 <= p < 0x2000000:
            return None
        s = ram.cstr(p)
        return s if len(s) > 2 and all(32 <= ord(c) < 127 for c in s) else None

    key = struct.pack("<I", tid)
    pos = ram.b.find(key, 0x100000)
    while pos != -1:
        # a table entry: its neighbours are {id, string} pairs too
        if not pos & 3 and ((ram.u32(pos + 8) < 0x10000 and string_at(ram.u32(pos + 12)))
                            or (ram.u32(pos - 8) < 0x10000 and string_at(ram.u32(pos - 4)))):
            s = string_at(ram.u32(pos + 4))
            if s:
                return s
        pos = ram.b.find(key, pos + 1)
    return None


def cmd_tasks(schema_unused, args):
    graph = load_tasks() if TASKS_FILE.exists() else {}
    titles = load_job_titles() if TASKS_FILE.exists() else {}
    for dump in args.ram:
        ram = Ram(dump)
        objs = task_objects(ram)
        eps = {episode_of(n) for n in objs.values()}
        graph = {k: v for k, v in graph.items() if episode_of(k) not in eps}
        for a, n in objs.items():
            pre = [objs.get(ram.u32(ram.u32(a + 0x98) + 4 * k)) for k in range(max(0, min(ram.i32(a + 0x94), 64)))]
            m = ram.u32(a + 0x7C)
            mission = ram.path_name(m + 0xD0) if m else None
            graph[n] = Task(n, ram.i32(a + 0x18), mission, [p for p in pre if p],
                            ram.i32(a + 0xBC), ram.i32(a + 0xB8), ram.i32(a + 0xC0))
            if mission and mission not in titles:      # mission object +0x6C: the job's name (text id)
                t = ram_text(ram, ram.u32(m + 0x6C))
                if t:
                    titles[mission] = t
        print(f"{dump}: {len(objs)} tasks of episode(s) {sorted(eps)}")
    lines = ["# Sly 2 task graph, read from the game's task objects in EE RAM dumps of each episode hub",
             "# (warp.py tasks --ram <dump>). <task> <object id> <job (mission)> <prerequisite tasks>",
             "# <start world> <start warp oid> <character oid: 7 Sly, 8 Bentley, 9 Murray>; '-' = none,",
             "# -1 = not a start point. A job's first task is its mission object's start task (+0x64).",
             "# 'job <mission> <title>' lines: the name the game shows (Job Help), mission object +0x6C."]
    for m in sorted(titles, key=lambda m: (episode_of(m), m)):
        lines.append(f"job {m} {titles[m]}")
    for n in sorted(graph, key=lambda n: (episode_of(n), n)):
        t = graph[n]
        lines.append(f"{n} {t.oid} {t.mission or '-'} {','.join(t.pre) or '-'} {t.wid} {t.warp} {t.po}")
    TASKS_FILE.write_text("\n".join(lines) + "\n")
    missing = [e for e in range(len(EPISODES)) if not any(episode_of(n) == e for n in graph)]
    print(f"{TASKS_FILE}: {len(graph)} tasks" + (f"; no dump yet for episodes {missing}" if missing else ""))


def cmd_schema(schema_unused, args):
    ram = Ram(args.ram)
    count, entries = ram.u32(Ram.GS), ram.u32(Ram.GS + 4)
    ents = [struct.unpack_from("<hBBHHH", ram.b, (entries + i * 10) & 0x1FFFFFF) for i in range(count)]
    out = []

    def walk(i, path):
        while i:
            sym, t, cnt, off, child, sib = ents[i]
            p = path + [ram.names[sym]]
            if t == 0xFF:
                walk(child, p)
            else:
                out.append(f"{'.'.join(p)} {t} {cnt} {off:X}")
            i = sib

    walk(ents[0][4], [])
    tab = ram.b[entries & 0x1FFFFFF:(entries & 0x1FFFFFF) + count * 10]
    head = SCHEMA_FILE.read_text().split("# trailer")[0] if SCHEMA_FILE.exists() else ""
    trailer = f"# trailer {struct.unpack_from('<H', tab, count * 10 - 6)[0]:X} {sum(tab)}\n"
    SCHEMA_FILE.write_text(head + trailer + "\n".join(out) + "\n")
    print(f"{SCHEMA_FILE}: {len(out)} fields, {trailer.strip()}")


# ---- commands --------------------------------------------------------------------------------------

def cmd_list(schema, args):
    tasks = load_tasks() if TASKS_FILE.exists() else {}
    titles = load_job_titles() if TASKS_FILE.exists() else {}
    for i, (name, title, place, hub) in enumerate(EPISODES):
        if args.episode is not None and i != args.episode:
            continue
        print(f"{i}  {title} ({place}), hub world {hub}  [jobs in play order: name, title, first task, played as]")
        if any(episode_of(t) == i for t in tasks):
            for m in job_order(tasks, i):
                e = mission_entry(tasks, m)
                po = next((tasks[t].po for t in topo(tasks, mission_tasks(tasks, m)) if tasks[t].po != -1), -1)
                print(f"   {m:24} {titles.get(m, '?'):30} {e:34} {CHAR_NAMES.get(po, '?')}")
        else:
            print("   jobs: " + " ".join(schema.groups(f"m{i}_")))


def cmd_make(schema, args):
    episode = args.episode
    if args.job and episode is None:
        name = args.job if args.job[0] in "mt" and args.job[1:2].isdigit() else None
        if name is None:
            sys.exit("--job without an episode prefix (m4_recon) needs --episode")
        episode = episode_of(name)
    if episode is None:
        sys.exit("make needs --episode or --job")
    notes = []
    if episode == 0 and not (args.job or args.set or args.complete or args.activate or args.character
                             or args.coins is not None or args.gadgets):
        # The prologue starts from a new game: a save directory with empty slots (play time 0)
        # skips the "create a save file?" prompt, and START on the title begins the prologue.
        data = bytes(SAVE_SIZE)
        notes.append("new game (empty slots)")
    else:
        data, notes = build(schema, episode, args.job, args.set or (), args.complete or (), args.activate or (),
                            args.character, args.coins, args.gadgets, args.direct)
    card = args.card or str(ROOT / "build" / "rel" / "memcard" / "slot0")
    f = write_card(card, args.slot, data, args.keep_others)
    print(f"wrote {f} ({EPISODES[episode][1]})")
    for n in notes:
        print("  " + n)


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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    li = sub.add_parser("list")
    li.add_argument("--episode", type=int, choices=range(len(EPISODES)))
    m = sub.add_parser("make")
    m.add_argument("--episode", type=int, choices=range(len(EPISODES)))
    m.add_argument("--job", help="start with this job available: m4_recon, recon (with --episode) or a task name")
    m.add_argument("--card", help="memory card directory (default build/rel/memcard/slot0)")
    m.add_argument("--slot", type=int, default=1, choices=(1, 2, 3))
    m.add_argument("--keep-others", action="store_true", help="keep the other two save slots")
    m.add_argument("--character", choices=sorted(CHARACTERS),
                   help="saved player character (default Sly; Bentley at the start of episode 4)")
    m.add_argument("--coins", type=int, help="coins to spend in ThiefNet (loot.ccoin)")
    m.add_argument("--gadgets", action="store_true", help="own every ThiefNet gadget")
    m.add_argument("--direct", action="store_true",
                   help="with --job: also print PS2_WARP_TASK, which makes the game start the job itself")
    m.add_argument("--set", action="append", metavar="PATH=VALUE", help="extra field (repeatable)")
    m.add_argument("--complete", action="append", metavar="PREFIX",
                   help="mark every mission/task group starting with PREFIX done (repeatable)")
    m.add_argument("--activate", action="append", metavar="TASK", help="set a task's state to 1 (repeatable)")
    d = sub.add_parser("dump")
    d.add_argument("save")
    d.add_argument("--all", action="store_true")
    s = sub.add_parser("set")
    s.add_argument("save")
    s.add_argument("assign", nargs="+")
    r = sub.add_parser("schema")
    r.add_argument("--ram", required=True)
    t = sub.add_parser("tasks")
    t.add_argument("--ram", required=True, action="append")
    args = ap.parse_args()
    schema = Schema() if args.cmd not in ("schema", "tasks") else None
    {"list": cmd_list, "make": cmd_make, "dump": cmd_dump, "set": cmd_set, "schema": cmd_schema,
     "tasks": cmd_tasks}[args.cmd](schema, args)


if __name__ == "__main__":
    main()
