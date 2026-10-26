/* (r2-re) Debug-only render-list tracer for the modern-renderer research (notes/wip/r2-re.md). Off by default.
 *
 *   PS2_R2_TRACE=<vblank>[:<frames>[:<dir>]]   from the first game frame emitted at or after that VBlank, trace <frames>
 *                                              consecutive game frames (default 1) into <dir> (default build\r2trace)
 *   PS2_R2_TRACE_RAM=0                         no EE RAM dump (default: one 32 MB dump per traced frame, for
 *                                              scripts\r2_frame.py; delete them after use)
 *
 * Two instruction patches (recomp/patches.py "r2_*"):
 *   r2_alloc   at jr $ra of the render-list allocator f_001CD998: $v0 = the new 16-byte entry, $ra = the submitter,
 *              $s0-$s7 = the submitter's callee-saved registers (its object pointers) -> remembered per entry
 *   r2_frame   in f_00157FA0 after the sorted list was emitted into the frame's VIF1 chain (before the list end pointer
 *              0x3D4A50 is cleared): writes frame_<n>.txt (entries in sorted order: priority, sort key, the two DMA CALL
 *              addresses, the submitter and its registers; DMAS state) and ram_<n>.bin.
 * The game is never changed: both patches only read memory. */
#include "hw.h"
#include "ps2_internal.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define R2_MAX 16384
typedef struct { uint32_t entry, ra, s[8]; } r2_alloc_rec;
static r2_alloc_rec recs[R2_MAX];
static uint32_t nrecs;
static int state = -1;                     /* -1 unknown, 0 off, 1 armed */
static uint32_t from_vb, frames_left = 1, frame_no, want_ram = 1;
static char dir[260] = "build\\r2trace";

static void init(void) {
    const char* e = getenv("PS2_R2_TRACE");
    state = 0;
    if (!e || !*e) return;
    from_vb = (uint32_t)strtoul(e, NULL, 0);
    const char* c = strchr(e, ':');
    if (c) {
        frames_left = (uint32_t)strtoul(c + 1, NULL, 0);
        if (!frames_left) frames_left = 1;
        c = strchr(c + 1, ':');
        if (c && c[1]) { strncpy(dir, c + 1, sizeof dir - 1); dir[sizeof dir - 1] = 0; }
    }
    const char* r = getenv("PS2_R2_TRACE_RAM");
    if (r && *r == '0') want_ram = 0;
    CreateDirectoryA(dir, NULL);
    state = 1;
    fprintf(stderr, "[r2] render-list trace armed: VBlank %u, %u frames -> %s\n", from_vb, frames_left, dir);
}

static int active(void) {
    if (state < 0) init();
    return state == 1 && ps2_vblank_count() >= from_vb && frames_left;
}

void r2_trace_alloc(ps2_ctx* ctx) {
    if (!active()) return;
    if (nrecs >= R2_MAX) return;
    r2_alloc_rec* r = &recs[nrecs++];
    r->entry = ctx->r[2].uw[0];
    r->ra = ctx->r[31].uw[0];
    for (int i = 0; i < 8; i++) r->s[i] = ctx->r[16 + i].uw[0];
}

void r2_trace_frame(ps2_ctx* ctx) {
    if (!active()) { nrecs = 0; return; }
    char path[300];
    snprintf(path, sizeof path, "%s\\frame_%u.txt", dir, frame_no);
    FILE* f = fopen(path, "w");
    if (!f) { state = 0; return; }
    uint32_t start = 0x003B3980u, end = ps2_r32(ctx, 0x003D4A50u), n = (end - start) / 16;
    uint32_t sp = ctx->r[29].uw[0];
    fprintf(f, "# r2 frame %u vblank %u entries %u allocs %u\n", frame_no, ps2_vblank_count(), n, nrecs);
    fprintf(f, "# DMAS 0x2DEA28: %08X %08X %08X %08X %08X %08X; frame globals 0x2DEB14 -> %08X\n",
            ps2_r32(ctx, 0x2DEA28u), ps2_r32(ctx, 0x2DEA2Cu), ps2_r32(ctx, 0x2DEA30u), ps2_r32(ctx, 0x2DEA34u),
            ps2_r32(ctx, 0x2DEA38u), ps2_r32(ctx, 0x2DEA3Cu), ps2_r32(ctx, 0x2DEB14u));
    fprintf(f, "# columns: order entry prio key(hex) key(float) call1 call2 ra s0..s7\n");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = ps2_r32(ctx, sp + 0x80 + 4 * i);   /* f_00157FA0's sorted pointer array */
        if (e < start || e >= end) e = start + 16 * i;
        uint32_t pr = ps2_r32(ctx, e), key = ps2_r32(ctx, e + 4), c1 = ps2_r32(ctx, e + 8), c2 = ps2_r32(ctx, e + 12);
        float kf;
        memcpy(&kf, &key, 4);
        const r2_alloc_rec* r = NULL;
        for (uint32_t k = 0; k < nrecs; k++) if (recs[k].entry == e) { r = &recs[k]; }
        fprintf(f, "%u %08X %u %08X %g %08X %08X", i, e, pr, key, kf, c1, c2);
        if (r) {
            fprintf(f, " %08X", r->ra);
            for (int k = 0; k < 8; k++) fprintf(f, " %08X", r->s[k]);
        }
        fprintf(f, "\n");
    }
    fclose(f);
    if (want_ram) {
        snprintf(path, sizeof path, "%s\\ram_%u.bin", dir, frame_no);
        FILE* r = fopen(path, "wb");
        if (r) { fwrite(ps2_ram, 1, PS2_RAM_SIZE, r); fclose(r); }
    }
    fprintf(stderr, "[r2] frame %u traced at VBlank %u: %u entries\n", frame_no, ps2_vblank_count(), n);
    frame_no++;
    frames_left--;
    nrecs = 0;
}
