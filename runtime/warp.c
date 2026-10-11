/* PS2_WARP_TASK: start the game directly inside a job (debug tool, off by default; every build).
 *
 *   PS2_WARP_TASK=<task object id>[:<world id>]
 *
 * scripts/warp.py makes a memory-card save in which every task a job depends on is done, so the
 * game resumes in the episode's hub with that job available (notes/warp.md). With this variable set
 * as well (warp.py make --job prints the value; warp_run.ps1 -Direct sets it), the job also starts
 * by itself: when the title screen's "Resume Game" (0x165E38) queues the load of the hub through the
 * level-transition function 0x22B490 (the call returning to 0x165FE4), the call is turned into the
 * one the game makes when a job is started from another level (0x1651D8): load the task's world
 * (descriptor *(0x3E1C40) + wid * 0x40, as 0x210990 computes it; the hub when no world is given),
 * remember the task object id in 0x2DEB48 and set transition flag 8. After the load the game itself
 * starts the task (0x165280: task state 1, the task's character as player, its start point).
 *
 * The hook runs at the entry of the recompiled 0x22B490 (an entry hook, recomp/hooks.py: PS2_HOOK ->
 * ps2_hook, present in release builds too), changes its argument registers once and never touches
 * the game otherwise. It checks the caller's instructions first, so another executable is left
 * alone.
 *
 * PS2_TASK_SET / PS2_TASK_FILE: change a task's state the way the game does when a job stage is
 * finished (debug tool, off by default; every build), e.g. to see what follows a stage that
 * scripted input cannot win:
 *
 *   PS2_TASK_SET=<task oid>:<state>[@<vblank>],...    (state 2 = completed, 3 = closed; ':?' queries)
 *   PS2_TASK_FILE=<path>                             (lines "<task oid>:<state>" appended while it runs)
 *
 * At the top of the game's main loop (entry hook on 0x20E890, called first by the loop at 0x1A7588,
 * so no game work is in progress) the hook calls the game's own functions on the main thread:
 * 0x15E938 (world 0x3DD490, class 0x105, oid) finds the task object as 0x165280 does, and 0x164DF0
 * (task, state) changes its state, which the game propagates to the successor tasks (and autosaves).
 * The registers are saved around the calls and restored, so the main loop goes on as if nothing ran.
 * Task oids: scripts/warp_tasks.txt. A boss's stages follow its own state machine, not the task
 * states: completing a boss task does not end the stage being played. */
#include "hw.h"

#include <stdlib.h>

#define FN_TRANSITION   0x0022B490u      /* queue a level load: (world, desc, warp, ?, flags, ?) */
#define RESUME_RA       0x00165FE4u      /* return address of that call in 0x165E38 (resume) */
#define PENDING_TASK    0x002DEB48u      /* task to start once the next level has loaded */
#define WORLD_DESCS     0x003E1C40u      /* pointer to the per-world disc descriptors (64 bytes each) */

#define FN_FRAME_TOP    0x0020E890u      /* first call of the main loop (jal at 0x1A7588) */
#define FRAME_TOP_RA    0x001A7590u
#define FN_FIND_OBJECT  0x0015E938u      /* (world, class, oid, 0) -> object */
#define FN_TASK_STATE   0x00164DF0u      /* (task, state): change state, propagate to successors */
#define GAME_WORLD      0x003DD490u
#define CLASS_TASK      0x105u
#define TASK_STATE_OFS  0x54u

static int warp_state;                   /* 0 off, 1 waiting for the resume, 2 done */
static uint32_t warp_oid;
static int warp_wid = -1;

static struct { uint32_t oid, vblank; int state; } task_req[64];   /* PS2_TASK_SET / PS2_TASK_FILE */
static int task_n, task_on;
static const char* task_file;
static long task_file_pos;

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, ps2_ram + (a & (PS2_RAM_SIZE - 1)), 4); return v; }
static void wr32(uint32_t a, uint32_t v) { memcpy(ps2_ram + (a & (PS2_RAM_SIZE - 1)), &v, 4); }

/* "<oid>:<state>[@<vblank>]" items separated by commas, spaces or newlines */
static void task_parse(const char* s) {
    while (s && *s) {
        char* end;
        unsigned long oid = strtoul(s, &end, 0);
        if (end != s && *end == ':' && task_n < 64) {
            const char* p = end + 1;
            long state = -1;                               /* "<oid>:?" only prints the state */
            if (*p == '?') end = (char*)p + 1;
            else state = strtol(p, &end, 0);
            uint32_t vb = 0;
            if (end != p && *end == '@') vb = (uint32_t)strtoul(end + 1, &end, 0);
            if (end != p) {
                task_req[task_n].oid = (uint32_t)oid;
                task_req[task_n].state = (int)state;
                task_req[task_n++].vblank = vb;
                fprintf(stderr, "[task] queued: task %lu %s %ld%s\n", oid, state < 0 ? "query" : "-> state",
                        state, vb ? " (at a VBlank)" : "");
            }
        }
        s = end != s ? end : s + 1;
        while (*s && (*s == ',' || *s == ' ' || *s == '\r' || *s == '\n' || *s == '\t')) s++;
    }
}

int warp_init(void) {
    const char* ts = getenv("PS2_TASK_SET");
    task_file = getenv("PS2_TASK_FILE");
    if (task_file && !*task_file) task_file = NULL;
    const char* e = getenv("PS2_WARP_TASK");
    int any = (e && *e) || (ts && *ts) || task_file;
    if (!any) return 0;
    if (ts && *ts) task_parse(ts);
    task_on = task_n > 0 || task_file;
    if (!e || !*e) return task_on;
    char* end;
    warp_oid = (uint32_t)strtoul(e, &end, 0);
    if (*end == ':') warp_wid = (int)strtol(end + 1, NULL, 0);
    fprintf(stderr, "[warp] will start task %u%s on \"Resume Game\"\n", warp_oid, warp_wid >= 0 ? " in its world" : "");
    warp_state = 1;
    return 1;
}

/* new lines of PS2_TASK_FILE since the last look */
static void task_poll_file(void) {
    FILE* f = fopen(task_file, "rb");
    if (!f) return;
    char buf[1024];
    if (fseek(f, task_file_pos, SEEK_SET) == 0) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        while (n > 0 && buf[n - 1] != '\n') n--;          /* whole lines only */
        buf[n] = 0;
        task_file_pos += (long)n;
        if (n) task_parse(buf);
    }
    fclose(f);
}

/* Calls the game's task functions on the main thread at the top of its main loop. */
static void task_run_requests(ps2_ctx* ctx) {
    static int busy, polls;
    static ps2_ctx saved;
    if (busy) return;
    if (task_file && polls++ % 15 == 0) task_poll_file();
    for (int i = 0; i < task_n; i++) {
        if (task_req[i].vblank > ps2_vblank_count()) continue;
        uint32_t oid = task_req[i].oid;
        int state = task_req[i].state;
        task_req[i--] = task_req[--task_n];
        busy = 1;
        saved = *ctx;
        ctx->r[REG_SP].ud[0] = (uint64_t)(int64_t)(int32_t)((ctx->r[REG_SP].uw[0] - 0x400u) & ~15u);
        ctx->r[REG_A0].ud[0] = GAME_WORLD;
        ctx->r[REG_A1].ud[0] = CLASS_TASK;
        ctx->r[REG_A2].ud[0] = oid;
        ctx->r[REG_A3].ud[0] = 0;
        ctx->r[REG_RA].ud[0] = FRAME_TOP_RA;
        ps2_call(ctx, FN_FIND_OBJECT);
        uint32_t task = ctx->r[REG_V0].uw[0];
        uint32_t before = task ? rd32(task + TASK_STATE_OFS) : 0;
        if (task && state >= 0) {
            ctx->r[REG_A0].ud[0] = (uint64_t)(int64_t)(int32_t)task;
            ctx->r[REG_A1].ud[0] = (uint64_t)(int64_t)state;
            ctx->r[REG_RA].ud[0] = FRAME_TOP_RA;
            ps2_call(ctx, FN_TASK_STATE);
        }
        *ctx = saved;
        busy = 0;
        if (task && state < 0)
            fprintf(stderr, "[task] v%u: task %u (object 0x%08X) state %u\n", ps2_vblank_count(), oid, task, before);
        else if (task)
            fprintf(stderr, "[task] v%u: task %u (object 0x%08X) state %u -> %d, now %u\n", ps2_vblank_count(), oid,
                    task, before, state, rd32(task + TASK_STATE_OFS));
        else
            fprintf(stderr, "[task] v%u: task %u is not in this level\n", ps2_vblank_count(), oid);
    }
}

void warp_on_call(ps2_ctx* ctx, uint32_t addr) {
    if (task_on && addr == FN_FRAME_TOP && ctx->r[REG_RA].uw[0] == FRAME_TOP_RA) {
        static int checked;
        if (!checked) {                                    /* this game's main loop: jal 0x20E890 there */
            checked = rd32(FRAME_TOP_RA - 8) == 0x0C083A24u ? 1 : -1;
            if (checked < 0) fprintf(stderr, "[task] unexpected code at 0x%08X; PS2_TASK_SET/FILE ignored\n", FRAME_TOP_RA - 8);
        }
        if (checked > 0) task_run_requests(ctx);
    }
    if (warp_state != 1 || addr != FN_TRANSITION || ctx->r[REG_RA].uw[0] != RESUME_RA) return;
    warp_state = 2;
    /* the caller must be the expected code: jal 0x22B490 with flags 0x40 in its delay setup */
    if (rd32(RESUME_RA - 8) != 0x0C08AD24u || rd32(RESUME_RA - 12) != 0x24080040u) {
        fprintf(stderr, "[warp] unexpected code at 0x%08X: not this game's resume; PS2_WARP_TASK ignored\n", RESUME_RA - 8);
        return;
    }
    if (warp_wid >= 0) ctx->r[REG_A1].uw[0] = rd32(WORLD_DESCS) + (uint32_t)warp_wid * 0x40u;   /* 0x210990 */
    ctx->r[REG_A2].uw[0] = 0x189;                         /* default start point; the task sets its own */
    ctx->r[REG_T0].uw[0] |= 8;                            /* start the pending task after the load */
    wr32(PENDING_TASK - 8, 0xFFFFFFFFu);                  /* as 0x1651D8 does */
    wr32(PENDING_TASK - 4, 0xFFFFFFFFu);
    wr32(PENDING_TASK, warp_oid);
    fprintf(stderr, "[warp] v%u: resume turned into starting task %u (world %d, descriptor 0x%08X)\n",
            ps2_vblank_count(), warp_oid, warp_wid, ctx->r[REG_A1].uw[0]);
}
