"""Checks for scripts/warp.py (save generator for episode / job warps): schema, saves, task graph.
Runs without the game; the facts checked were confirmed in the game (notes/warp.md)."""
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
import warp  # noqa: E402

fails = 0
checks = 0


def check(cond, what):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print("FAIL", what)


schema = warp.Schema()
check(len(schema.fields) == 4472, "schema has 4472 fields")
check(schema.trailer == (0x5098, 2582452), "schema trailer")
check(schema.fields["episodeCurrent"][2] == 0 and schema.fields["widCurrent"][2] == 8, "header offsets")
check(schema.fields["dt"] == (3, 0, 0x10), "dt is a float at 0x10")
check(schema.fields["powerUps.mppukfAcquired"] == (4, 43, 0x98), "power-up bit field")
check(len([g for g in schema.groups("m") if g[1].isdigit()]) == 78
      and len([g for g in schema.groups("t") if g[1].isdigit()]) == 795, "78 missions, 795 tasks")

tasks = warp.load_tasks()
check(len(tasks) == 795, "task graph has every task")
check(set(tasks) == {g for g in schema.groups("t") if g[1].isdigit()}, "task graph names = schema task groups")
check(all(p in tasks for t in tasks.values() for p in t.pre), "prerequisites are known tasks")
missions = {t.mission for t in tasks.values() if t.mission}
check(len(missions) == 78, "every mission has tasks")
for m in missions:
    e = warp.mission_entry(tasks, m)
    check(tasks[e].mission == m, f"{m} entry inside the job")
check(warp.mission_entry(tasks, "m1_moulin_power") == "t1_moulin_intro_exterior", "entry = mission start task")
order = warp.job_order(tasks, 1)
check(order[0] == "m1_sat" and order[-1] == "m1_boss", "episode 1 job order")

# episode start
data, notes = warp.build(schema, 5)
sv = warp.Save(schema, data)
check(len(data) == warp.SAVE_SIZE, "save size")
check(struct.unpack_from("<II", data, 0x5400) == schema.trailer, "trailer written")
check(sv.get("episodeCurrent") == 5 and sv.get("episodeBest") == 5 and sv.get("widCurrent") == 17, "episode 5 header")
check(sv.get("dt") > 0, "play time set (0 = empty slot)")
check(sv.get("t4_complete.state") == warp.TASK_DONE and sv.get("t4_complete.is_complete") == 1, "earlier episode done")
check(sv.get("t5_intro.state") == 0, "current episode untouched")
check(sv.get("t0_attract_mode.from_memcard") == 1, "from_memcard set like the game's saves")
check(sv.get("powerUps.mppukfAcquired[27]") == 0, "no paraglide before episode 6")
sv6 = warp.Save(schema, warp.build(schema, 6)[0])
check(sv6.get("powerUps.mppukfAcquired[27]") == 1 and sv6.get("powerUps.mppukfAcquired[26]") == 0,
      "episode 6: paraglide bought in episode 5, no alarm clock yet")
sv4 = warp.Save(schema, warp.build(schema, 4)[0])
check(sv4.get("oidPoCurrent") == 8, "episode 4 starts as Bentley")

# job warp: Silence the Alarms (brawl) needs chalktalk2 = bug + waterpump (+ follow, recon, sat)
sv = warp.Save(schema, warp.build(schema, 1, "m1_brawl")[0])
check(sv.get("t1_brawl_intro.state") == 1, "job entry active")
check(sv.get("t1_chalktalk2.state") == warp.TASK_DONE, "prerequisite chalk talk done")
check(sv.get("m1_waterpump.is_complete") == 1 and sv.get("m1_bug.is_complete") == 1, "earlier jobs complete")
check(sv.get("t1_neela_intro.state") == 1 and sv.get("t1_moulin_intro_exterior.state") == 1, "parallel jobs available")
check(sv.get("t1_disco_ext_intro.state") == 0 and sv.get("m1_neela.is_complete") == 0, "later jobs untouched")
check(sv.get("oidPoCurrent") == 7, "hub player stays Sly")
# a job after a finished job with side tasks: disco's side task closes with it
sv = warp.Save(schema, warp.build(schema, 1, "m1_boss")[0])
check(sv.get("t1_disco_laser_bar.state") == warp.TASK_DONE, "finished job's side task closed")
# direct start
_, notes = warp.build(schema, 1, "m1_recon", direct=True)
check(any("PS2_WARP_TASK=1773:3" in n for n in notes), "direct start = first task with a start world")

# memory card layout (runtime/mc.c host directory)
with tempfile.TemporaryDirectory() as d:
    f = warp.write_card(d, 1, data, False)
    sd = Path(d) / warp.SAVE_DIR
    check(f.read_bytes() == data, "slot written")
    check((sd / "save2").read_bytes() == bytes(warp.SAVE_SIZE), "other slots empty")
    check((sd / warp.SAVE_DIR).read_bytes() == b"Sly 2", "directory tag file")
    meta = (sd / "%meta").read_text().splitlines()
    check(meta[1].startswith(". 8427") and any(l.startswith("save1 8497") for l in meta), "sidecar entries")
    s1 = next(l for l in meta if l.startswith("save1")).split()[4]
    s2 = next(l for l in meta if l.startswith("save2")).split()[4]
    check(s1 > s2, "the warp slot is the newest")

print(f"warp: {checks} checks, {fails} failures")
sys.exit(1 if fails else 0)
