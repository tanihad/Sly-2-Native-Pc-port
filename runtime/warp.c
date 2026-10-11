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
 * alone. */
#include "hw.h"

#include <stdlib.h>

#define FN_TRANSITION   0x0022B490u      /* queue a level load: (world, desc, warp, ?, flags, ?) */
#define RESUME_RA       0x00165FE4u      /* return address of that call in 0x165E38 (resume) */
#define PENDING_TASK    0x002DEB48u      /* task to start once the next level has loaded */
#define WORLD_DESCS     0x003E1C40u      /* pointer to the per-world disc descriptors (64 bytes each) */

static int warp_state;                   /* 0 off, 1 waiting for the resume, 2 done */
static uint32_t warp_oid;
static int warp_wid = -1;

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, ps2_ram + (a & (PS2_RAM_SIZE - 1)), 4); return v; }
static void wr32(uint32_t a, uint32_t v) { memcpy(ps2_ram + (a & (PS2_RAM_SIZE - 1)), &v, 4); }

int warp_init(void) {
    const char* e = getenv("PS2_WARP_TASK");
    if (!e || !*e) return 0;
    char* end;
    warp_oid = (uint32_t)strtoul(e, &end, 0);
    if (*end == ':') warp_wid = (int)strtol(end + 1, NULL, 0);
    fprintf(stderr, "[warp] will start task %u%s on \"Resume Game\"\n", warp_oid, warp_wid >= 0 ? " in its world" : "");
    warp_state = 1;
    return 1;
}

void warp_on_call(ps2_ctx* ctx, uint32_t addr) {
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
