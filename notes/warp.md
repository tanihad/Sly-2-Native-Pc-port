# Warp: start Sly 2 at any episode or job

Testers can start the game at the beginning of any episode, or with any job available (or already
started), without playing up to it. It works by writing a memory-card save that the game resumes
from; nothing in the game is patched. A small debug-only runtime hook (off by default) can also
start the chosen job directly.

## Quick start

From the project root or a worktree (python = the main checkout's `.venv\Scripts\python.exe`):

```powershell
# episode start, headless, debug build (build\sly2), into the hub as the episode's character:
powershell -ExecutionPolicy Bypass -File scripts\warp_run.ps1 -Episode 4 -Play
# the same with the fast release build (build\rel):
powershell -ExecutionPolicy Bypass -File scripts\warp_run.ps1 -Episode 4 -Play -Release
# a job: everything it depends on is done, the hub opens with the job available:
powershell -ExecutionPolicy Bypass -File scripts\warp_run.ps1 -Episode 1 -Job m1_brawl -Play
# a job started by the game itself (any build): title -> job briefing -> playing the job
powershell -ExecutionPolicy Bypass -File scripts\warp_run.ps1 -Episode 1 -Job m1_brawl -Direct -Release
```

Each run gets `build\warp\<name>\` (card, a copy of the exe so runs can go in parallel, run.log,
frames with latest.png). Options: `-Seconds 150`, `-DumpEvery 120`, `-Window` (visible window with
sound), `-Pad "<more PS2_INPUT tokens>"` (after the START press, e.g. `"+2400:ram"`), `-Name`,
`-Set "path=value,..."`, `-Make "<more warp.py make options>"`, `-TitleFrame N` (press START at pad
frame N instead of the title anchor; only for an exe built before the entry hooks).

To play from an episode or job in a window, with sound:

```powershell
python scripts\warp.py make --episode 6          # or: --job m6_paraglide; writes build\rel\memcard\slot0
powershell -ExecutionPolicy Bypass -File scripts\play.ps1
```

then press START on the title screen ("Press START button to Resume Game") and X at the safehouse.
To be dropped straight into a job instead, add `--direct` and set the variable it prints in the
same PowerShell before play.ps1 (and remove it afterwards, or it applies to the next game too):

```powershell
python scripts\warp.py make --job m1_brawl --direct    # prints "direct start: PS2_WARP_TASK=1585:2"
$env:PS2_WARP_TASK = "1585:2"; powershell -ExecutionPolicy Bypass -File scripts\play.ps1
Remove-Item Env:\PS2_WARP_TASK
```

`warp.py make` writes the save into slot 1 of `build\rel\memcard\slot0` (play.ps1's card) unless
`--card` names another directory, empties the other two slots (`--keep-others` keeps them) and first
copies any save it replaces to `<card>.warp-backup-<time>\`. Any exe can use another card through
`PS2_MEMCARD_DIR=<card dir>` (run_game.ps1 `-Card`); without it the card is `memcard\slot0` next to
the exe. Scripted input: anchor START to the title (`@187D80/1DD96C+30:start:10`, both builds: the
function has an entry hook, recomp/hooks.py; other `@` anchors still need the debug build). A fixed
frame (`2400:start:10`) is unreliable: the title came up between pad frames ~2070 and ~2720 in
these runs, depending on build and machine load. The safehouse menu is up ~1500 frames after
START (`+1500:cross:6`).

## warp.py

```
warp.py list [--episode N]          episodes, hub worlds, jobs in play order (name, title, first task, character)
warp.py make --episode N            save at the start of episode N (0 = new game)
warp.py make --job m3_heist         save in which every task the job depends on is done
          [--direct]                ... and print PS2_WARP_TASK=<task oid>[:<world>] for a direct start
          [--card DIR] [--slot 1..3] [--keep-others]  memory card directory, save slot, keep other slots
          [--character sly|bentley|murray] [--coins N] [--gadgets]  player, ThiefNet coins, all gadgets
          [--complete PREFIX] [--activate TASK] [--set path=value]  raw edits (see dump for names)
warp.py dump <save file> [--all]    decode a save (fields that are not 0)
warp.py set <save file> path=value  edit a save in place
warp.py schema --ram <ram.bin>      regenerate scripts/warp_schema.txt from an EE RAM dump
warp.py tasks --ram <ram.bin> ...   regenerate scripts/warp_tasks.txt (task graph, job titles)
```

`--job` takes a mission name (`m4_recon`), a short name with `--episode` (`recon`), or a task name
for a later stage of a job (`t1_heist_5_peacock_climb`: the heist from that point; with `--direct`
the game starts there). RAM dumps come from the PS2_INPUT token `ram` (written to the dump dir).

## What each episode start looks like (all verified)

The saved game resumes in the episode's hub at the safehouse ("Press X to play as ..."); X enters
the hub. Frames: `build\screenshots\warp_ep0.png` ... `warp_ep8.png` (overview `warp_all.png`).
Episode titles are the game's own (pause menu, episode menu); the back-of-box names differ slightly
for episodes 3 ("The Predator Awakens") and 7 ("Menace from the North, Eh!").

| # | Episode | Hub world | After START | After X |
|---|---------|-----------|-------------|---------|
| 0 | Prologue: Cairo Museum Break-In | 0 | new game: Binocucom briefing | (no safehouse) Sly in the museum |
| 1 | The Black Chateau (Paris) | 2 | Paris flyover, safehouse | Satellite Sabotage briefing, Sly on the roofs |
| 2 | A Starry Eyed Encounter (India) | 8 | safehouse | Sly in the palace grounds |
| 3 | The Predator Awakes (India) | 12 | safehouse | Sly in the jungle |
| 4 | Jailbreak (Prague) | 14 | safehouse, Bentley alone | Bentley in Prague (save has Bentley as player) |
| 5 | A Tangled Web (Prague) | 17 | safehouse | Sly at the castle |
| 6 | He Who Tames the Iron Horse (Canada) | 27 | safehouse | Sly at the station |
| 7 | Menace in the North, Eh? (Canada) | 32 | safehouse | Binocucom briefing, Sly in the snow |
| 8 | Anatomy for Disaster (blimp) | 38 | safehouse on the blimp | Sly on the blimp |

Episode 0 is a real new game: the card holds the save directory with three empty slots, so there
is no "create a save file?" prompt and START begins the prologue. The game autosaves into slot 1
shortly after an episode starts (the card then holds the game's own save).

## Jobs

`warp.py list` prints every episode's jobs in play order with the name the game shows in the pause
menu's Job Help. A job warp marks done (state 3) every task the job's first task depends on, closes
jobs whose exit tasks are done (side tasks included), and makes available (state 1) every task whose
prerequisites are now done: the target job, jobs that open at the same time, Bentley's reminders.
Verified with Job Help (check marks on the earlier jobs, target available): m1_brawl, m1_disco,
m1_boss, m2_rc_chopper, m3_heist, m4_hack, m5_hack, m6_paraglide, m7_boss, m8_boss.

Without `--direct` the tester walks to the job's marker in the hub (as in the game, the hub
character is Sly - Bentley at the start of episode 4 - and the game switches to the job's
character when the job starts). With `--direct` / `-Direct` (any build) the game starts the job
itself straight from the title: verified for m1_brawl (Murray, hub; debug and release),
m1_moulin_power (another level, world 6), t1_heist_5_peacock_climb (a stage in the middle of a
heist), m8_boss (the final fight, world 43), m3_heist (release) and the later parts of the prologue
that scripted play has not reached: t0_chase_hallway_start (the chase in the Clockwerk hallway)
and t0_rooftop_start (the rooftop chase). Frames: `build\screenshots\warp_job_*.png`.

Gadgets: the story makes the player buy the Paraglide (episode 5, `t5_paraglide_purchase`) and the
Alarm Clock (episode 7, `t7_alarmclock_purchase`); warp saves own them once those tasks are done.
Other ThiefNet gadgets are not owned (`--gadgets` buys all, `--coins 5000` gives money). Health,
loot and clue bottles are at their new-game values.

## How it works

Reverse-engineered from the executable (Ghidra) and RAM dumps; addresses are NTSC-U v1.00.

**Save file** (`/BASCUS-97316YAOTWTD!/save1..3`, 0x5408 bytes, host files under `<card>\BASCUS-
97316YAOTWTD!\` with runtime/mc.c's `%meta` sidecar): the game's whole progress blob at EE
0x3D4A60, copied as is (FUN_001d84b0 sets every `from_memcard` field to 1 when saving). Its layout
is a schema the game loads with every level (FUN_0013da18 -> FUN_001d7b88): object 0x3E0EA8 =
{entry count, 10-byte entries {symbol, type, count, offset, first child, next sibling}, 0x35-bucket
symbol hash}. scripts/warp_schema.txt lists its 4472 leaves (types: int, float, bool / bit field,
vectors). The trailer at 0x5400 is the schema's id (u16 offset of its last entry, 0x5098) and at
0x5404 the byte sum of the entry table (2582452); a save whose trailer differs is wiped
(FUN_001d7c80). There is no checksum over the data. Fields: `episodeCurrent`, `episodeBest`,
`widCurrent`, `oidPoCurrent` (player: 7 Sly, 8 Bentley, 9 Murray), `dt` (play time; 0 = empty slot),
`flags.*` (options), `sly/bentley/murray.*`, `powerUps.mppukfAcquired` (43 bits),
`loot.ccoin`, per-episode `dt`/`percent_complete`/level booty, clues and vaults, 78 missions
`m<ep>_<job>` {focus_count, is_complete, time_played, suck_value} and 795 tasks `t<ep>_<task>`
{[focus_count], complete_count, is_complete, state, from_memcard}.

**Which slot, which level**: the title screen loads all three slots, ignores those with `dt` 0 and
resumes the newest by card timestamp (FUN_001ab418); warp.py zeroes the other slots and dates the
warp slot now. Resume (FUN_00165e38) loads the hub of `episodeCurrent` (descriptor table
*(0x3E1C50) + ep * 64) at start point 0x189; `widCurrent` is not used (every level load rewrites
it, FUN_0020e3c0). If the current episode's `percent_complete` is above 99 it loads the episode menu
level instead (descriptor 0x2A3828, the "dvd_menu" world 1: "Start Episode", "Watch a Movie"; it
only offers the current episode).

**Tasks and jobs** are native objects in every level of an episode (class 0x105, vtable 0x2CF1E8;
FUN_00162000-00166270): +0x18 object id, +0x54 state, +0x7C mission, +0x94/+0x98 prerequisites,
+0xA0/+0xA4 successors, +0xB8 start warp, +0xBC start world, +0xC0 character, game-state paths
at +0xC8.. (focus_count, complete_count, is_complete, state, from_memcard). Missions: +0x64 start
task, +0x68 final task, +0x6C title text id, paths at +0xD0. States: 0 not started, 1 active, 2
completed, 3 closed. Progression is event driven (FUN_00164df0 propagates a state change to the
successors and autosaves), so a save that only marks tasks done activates nothing: warp.py sets
the frontier itself. Loading (FUN_00164270) turns job tasks saved in state 1 or 2 back into 0/1 (a
job interrupted by quitting restarts), so finished tasks are saved as 3. scripts/warp_tasks.txt is
the graph read from these objects (`warp.py tasks --ram`, one dump per episode hub).

**Direct job start** (runtime/warp.c, `PS2_WARP_TASK=<task oid>[:<world id>]`, every build, off
by default): when the game starts a job that lives in another level (FUN_001651d8) it stores the
task in 0x2DEB48, loads that level with transition flag 8, and after the load FUN_00165280 starts
the task (state 1, its character, its start warp). The hook catches the resume's call of the
transition function 0x22B490 (return address 0x165FE4, caller bytes checked) and turns it into that
load. It touches nothing else. 0x22B490 has an entry hook (recomp/hooks.py: the recompiled function
starts with `PS2_HOOK`, a test of a flag that only debug tools set, so release builds keep it;
notes/recompiler-design.md "Entry hooks"). The pad-button test 0x187D80 has one too, which is what
makes the title-screen START anchor work in release builds.

**Power-ups**: table at 0x2BCD00 (32-byte records: kind, character, cost, ?, ThiefNet position,
name/description text ids); bits 7-14 Bentley's gadgets, 15-22 Murray's, 23-30 Sly's (26 Alarm
Clock, 27 Paraglide, confirmed in ThiefNet), 31-42 free moves.

## Runtime findings

Every episode hub and the sampled job levels loaded and played without a crash, hang, missing
syscall/RPC or broken rendering (debug build; release spot-checked on episodes 0, 4, 6 and 7 and
the direct starts of m1_brawl and m3_heist; at 60 fps on an idle machine the episode 7 snow hub
uses ~14.7 of 16.7 ms per VBlank, VU interpreter ~65%). The only log
lines are the known boot-time ones (`syscall 0x5B ... not implemented`, `unimplemented eret at
0x00239AA4`). Each episode's job content beyond the hub was not played.

## Files

- `scripts/warp.py`, `scripts/warp_schema.txt`, `scripts/warp_tasks.txt`, `scripts/warp_run.ps1`
- `scripts/run_game.ps1 -Card`, `runtime/warp.c`, entry hooks: `recomp/hooks.py`, `PS2_HOOK` /
  `ps2_hook` (runtime/ps2_runtime.h/.c)
- `tests/unit/warp/` (no game needed); `tests/run_tests.py` checks the hooks' byte guard

## Appendix: jobs (`warp.py list`)

```text
0  Prologue: Cairo Museum Break-In (Cairo), hub world 0  [jobs in play order: name, title, first task, played as]
   m0_intro                 Cairo Museum Break-in          t0_security_shutdown               sly
1  Episode 1: The Black Chateau (Paris), hub world 2  [jobs in play order: name, title, first task, played as]
   m1_sat                   Satellite Sabotage             t1_sat_intro                       sly
   m1_recon                 Breaking and Entering          t1_recon_intro                     sly
   m1_bug                   Bug Dimitri's Office           t1_bug_ext_intro                   sly
   m1_follow_dimitri        Follow Dimitri                 t1_follow_intro                    sly
   m1_waterpump             Waterpump Destruction          t1_waterpump_intro                 murray
   m1_brawl                 Silence the Alarms             t1_brawl_intro                     murray
   m1_neela                 Moonlight Rendezvous           t1_neela_intro                     sly
   m1_moulin_power          Theater Pickpocketing          t1_moulin_intro_exterior           sly
   m1_disco                 Disco Demolitions              t1_disco_ext_intro                 bentley
   m1_heist                 Operation: Thunder Beak        t1_heist_intro                     bentley
   m1_boss                  Printing Press Duel            t1_dimitri_boss_intro              sly
2  Episode 2: A Starry Eyed Encounter (India), hub world 8  [jobs in play order: name, title, first task, played as]
   m2_recon                 Recon the Ballroom             t2_recon_intro                     sly
   m2_dance_audition_fake   Ballroom Dance Party           t2_dance_audition_fake_intro       ?
   m2_lower_drawbridge      Lower the Drawbridge           t2_lower_drawbridge_intro          sly
   m2_steal_tuxedo          Steal a Tuxedo                 t2_steal_tuxedo_intro              sly
   m2_dance_audition        Dominate the Dance Floor       t2_dance_audition_intro            ?
   m2_turret                Battle the Chopper             t2_turret_intro                    murray
   m2_boardroom_brawl       Boardroom Brawl                t2_boardroom_brawl_intro           sly
   m2_rc_chopper            RC Bombing Run                 t2_rc_chopper_intro                bentley
   m2_elephant_rampage      Elephant Rampage               t2_elephant_rampage_intro          sly
   m2_heist_bomb_bridge     Bomb the Bridge                t2_bomb_bridge_intro               bentley
   m2_heist_dance           Tango with Carmelita           t2_heist_dance_opening             ?
   m2_heist_cover_murray    Clear the way for Murray       t2_heist_cover_murray              murray
3  Episode 3: The Predator Awakes (India), hub world 12  [jobs in play order: name, title, first task, played as]
   m3_recon                 Spice Room Recon               t3_recon_intro                     sly
   m3_waterbug              Water Bug Run                  t3_waterbug_intro                  sly
   m3_free_elephant         Freeing the Elephant           t3_free_elephant_intro             sly
   m3_lead_rajan            Leading Rajan                  t3_lead_rajan_intro                bentley
   m3_turret                Blow the Dam                   t3_turret_intro                    bentley
   m3_steal_ruby            Rip-off the Ruby               t3_steal_ruby_intro                sly
   m3_heartneela            Neyla's Secret                 t3_heartneela_intro                sly
   m3_tntbarrel             Spice Grinder Destruction      t3_tntbarrel_intro_ext             sly
   m3_heist                 Operation: Wet Tiger           t3_heist_intro                     murray
   m3_boss                  Showdown with Rajan            t3_boss_intro                      sly
4  Episode 4: Jailbreak (Prague), hub world 14  [jobs in play order: name, title, first task, played as]
   m4_recon                 Eavesdrop on Contessa          t4_recon_intro                     bentley
   m4_hack                  Train Hack                     t4_hack_intro                      bentley
   m4_rc_chopper            Wall Bombing                   t4_rc_chopper_intro                bentley
   m4_bomb_bridge           Disguise Bridge                t4_bomb_bridge_intro               sly
   m4_brawl                 Big House Brawl                t4_brawl_intro_ext                 sly
   m4_lightning             Lightning Action               t4_lightning_intro                 sly
   m4_spycam                Code Capture                   t4_spycam_intro                    sly
   m4_pickpocket            Close to Contessa              t4_pickpocket_intro                sly
   m4_heist                 Operation: Trojan Tank         t4_heist_intro                     bentley
5  Episode 5: A Tangled Web (Prague), hub world 17  [jobs in play order: name, title, first task, played as]
   m5_recon                 Know Your Enemy                t5_recon_intro                     sly
   m5_ghost                 Ghost Capture                  t5_ghost_intro                     sly
   m5_ewok_trap             Mojo Trap Action               t5_ewok_trap_intro                 bentley
   m5_kidnap                Kidnap the General             t5_kidnap_intro                    murray
   m5_battlezone            Tank Showdown                  t5_battlezone_intro                murray
   m5_pickpocket            Stealing Voices                t5_pickpocket_intro                sly
   m5_hack                  Crypt Hack                     t5_hack_intro                      bentley
   m5_heist                 Operation: High Road           t5_heist_intro                     murray
6  Episode 6: He Who Tames the Iron Horse (Canada), hub world 27  [jobs in play order: name, title, first task, played as]
   m6_recon                 Cabin Crimes                   t6_recon_intro                     sly
   m6_carmelita             A Friend in Need               t6_carmelita_intro                 sly
   m6_paraglide             Spice in the Sky               t6_paraglide_intro                 sly
   m6_train_red             Ride the Iron Horse            t6_train_red_intro                 bentley
   m6_rc_chopper            Aerial Assault                 t6_rc_chopper_intro                bentley
   m6_kidnap_bearcubs       Bear Cub Kidnapping            t6_kidnap_bearcubs_intro           murray
   m6_train_blue            Theft on the Rails             t6_train_blue_intro                sly
   m6_heist                 Operation: "Choo-Choo"         t6_heist_intro                     murray
7  Episode 7: Menace in the North, Eh? (Canada), hub world 32  [jobs in play order: name, title, first task, played as]
   m7_recon                 Recon the Sawmill              t7_recon_intro                     sly
   m7_bearcave              Bearcave Bugging               t7_bearcave_intro_ext              sly
   m7_robot_war             RC Combat Club                 t7_robot_war_intro_ext             murray
   m7_laser                 Laser Redirection              t7_laser_intro                     sly
   m7_lighthouse            Lighthouse Break In            t7_lighthouse_intro_ext            sly
   m7_boat_hack             Boat Hack                      t7_boat_hack_intro                 bentley
   m7_bear                  Old Grizzle Face               t7_bear_intro                      murray
   m7_thermals              Thermal Ride                   t7_thermals_intro                  sly
   m7_heist                 Operation: Canada Games        t7_heist0_intro                    bentley
   m7_boss                  Brains over Brawn              t7_boss_intro                      bentley
8  Episode 8: Anatomy for Disaster (Blimp), hub world 38  [jobs in play order: name, title, first task, played as]
   m8_recon                 Blimp HQ Recon                 t8_recon_intro_ext                 sly
   m8_post_recon            ?                              t8_post_recon_confront_int         sly
   m8_tntbarrel             Charged TNT Run                t8_tntbarrel_intro                 sly
   m8_engine_room_jt        Murray/Sly Tag Team            t8_engine_room_jt_intro            murray
   m8_engine_room_bentley   Sly/Bentley Conspire           t8_engine_room_bentley_intro       sly
   m8_engine_room_murray    Bentley/Murray Team Up         t8_engine_room_murray_intro        bentley
   m8_megajump              Mega-Jump Job                  t8_megajump_intro                  sly
   m8_turret                Carmelita's Gunner             t8_turret_intro                    sly
   m8_boss                  Showdown with Clock-La         t8_debris_fall_intro               sly
```
