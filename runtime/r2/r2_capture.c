/* R2 on the EE thread: the render-list hooks (notes/modern-renderer.md 2.2; recomp/patches.py "r2m_*").
 *
 *   r2_cap_alloc  jr $ra of the render-list allocator f_001CD998: the entry ($v0) comes from an object's submitter.
 *                 Entries the static world tree (0x1D5808) writes itself never pass here: R1's native candidates.
 *   r2_cap_entry  f_00157FA0, before 0x158378 (after the render class switch, before the entry's DMA CALLs are
 *                 written): $s2 = entry {+0 priority, +4 sort key, +8 CALL #1, +0xC CALL #2}, $s0 = priority,
 *                 $s1 = render class, $s5 -> the sorted pointer array, $s4 = entries left. $sp = f_00157FA0's frame:
 *                 VU 0 / VU 1 (screen and depth constants) at +0x8160 / +0x8170, VU 2..5 (world -> clip) at +0x00.
 *   r2_cap_done   before 0x1583FC (after the last class's end function): the frame's chain is complete.
 *
 * A candidate entry (static world, render class 0 = the main VU1 program, a level on the allowlist) whose mesh DMA
 * decodes (r2_mesh.c; once per address and level, under a time budget per frame) becomes native:
 *   hybrid   one DIRECT packet into the frame builder (DMAS 0x2DEA28, as aspect.c's hints) carrying R2H_DRAW + its
 *            words (and R2H_FRAME once per frame; at the start of a run of native entries VIF MSCAL 0x3AB0 + MSCAL 0 ahead
 *            of it: the previous exact mesh's pending tail kicks are drawn before the native run, as in an all-exact
 *            frame, with VIF1's double-buffer parity kept), then the entry's mesh CALL is zeroed (form 0, the whole
 *            static packet at +8; form 1, the mesh DMA at +0xC, the per-frame header at +8 stays: it only uploads VU
 *            constants). When the next entry will not be native, the packet also carries the state epilogue: the A+D
 *            registers of the entry's last material pass (TEX0 with its CLUT load, TEX1, CLAMP, TEST, ALPHA, FOGCOL),
 *            so exact draws after a native run start from the GS state an all-exact frame leaves.
 *   bracket  the CALLs stay; R2H_BEGIN + words before them, MSCAL 0x3AB0 + MSCAL 0 (the entry's own tail kicks) and
 *            R2H_END after them: the frontend sees both versions (PS2_R2_CHECK compares them).
 * Everything else is exact. Off (the default), nothing here writes anything: the GS stream is the game's own. */
#include "r2.h"
#include "ps2_runtime.h"
#include "settings.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int gpu_hw_active(void);                 /* gpu/gpu_hw.c */
uint32_t ps2_vblank_count(void);

#define RENDER_LIST 0x003B3980u
#define RENDER_LIST_MAX 0x2000u
#define DMAS 0x002DEA28u                 /* the frame builder (aspect.c) */
#define CLASS_TABLE 0x002BDD20u          /* priority -> render class */
#define WID_CURRENT 0x003D4A68u          /* game state blob 0x3D4A60 + 8: widCurrent (notes/warp.md) */
#define GS_HINT_REG 0x7Fu
#define MSCAL(a) (0x14000000u | ((a) >> 3))

volatile int r2_mode;

/* ---- the meshes of this level: DMA address -> handle (the store, r2_mesh.c, keeps the meshes per generation) ---- */
#define HASH_N 32768u
typedef struct {
    uint32_t hk[HASH_N];                 /* DMA address (0: empty, ~0: removed) */
    uint32_t hv[HASH_N];                 /* handle, or 0x80000000 | reason (decode failed) */
    uint8_t hform[HASH_N];
    uint8_t fp[HASH_N][32];              /* the first 32 bytes of the chain: content changed -> decode again */
} mesh_map;
static mesh_map* MM;
static uint32_t cur_wid = 0xFFFFFFFFu;
static int have_gen;

static void new_generation(uint32_t wid) {
    if (!MM && !(MM = calloc(1, sizeof *MM))) return;
    memset(MM->hk, 0, sizeof MM->hk);
    r2_store_new_gen();
    cur_wid = wid;
    have_gen = 1;
}

/* ---- settings, statistics ------------------------------------------------------------------------------------------ */
static int cfg_done, env_mode = -1, check_on, levels_all, nlevels;
static int kinds_world = 1, kinds_props = 1;   /* (r2-m2) PS2_R2_KINDS=world,props */
static int clip_on = 1;                        /* (r2-m2) PS2_R2_CLIP=0: guard-band-crossing meshes stay exact */
static uint32_t levels[32];
static double budget_ms = 3.0;
static LARGE_INTEGER qpf;

typedef struct {
    uint32_t frames, cand, native, exact_level, exact_class, exact_decode, exact_budget, exact_dmas, exact_header, exact_fade, exact_guard,
        decoded, decode_fail, bytes_hint, runs, epilogues, cand_props, native_props, exact_outline, outlines, outlines_parsed, clipped;
    double decode_ms, cap_ms;
    uint32_t reason[R2M_N];
} r2_stats;
static r2_stats st, st_report;
static int stats_on;

static void configure(void) {
    cfg_done = 1;
    QueryPerformanceFrequency(&qpf);
    const char* e = getenv("PS2_R2");
    if (e && *e) {
        if (!_stricmp(e, "off") || !strcmp(e, "0")) env_mode = R2_OFF;
        else if (!_stricmp(e, "hybrid") || !_stricmp(e, "on") || !strcmp(e, "1") || !_stricmp(e, "modern")) env_mode = R2_HYBRID;
        else if (!_stricmp(e, "bracket")) env_mode = R2_BRACKET;
        else fprintf(stderr, "[r2] PS2_R2='%s' is not off, hybrid or bracket: ignored\n", e);
    }
    e = getenv("PS2_R2_LEVELS");                 /* "all" or world ids (default: the ep3 hub, world 12) */
    if (e && *e) {
        if (!_stricmp(e, "all")) levels_all = 1;
        else for (const char* p = e; *p && nlevels < 32;) {
            char* end;
            unsigned long v = strtoul(p, &end, 0);
            if (end == p) break;
            levels[nlevels++] = (uint32_t)v;
            p = *end ? end + 1 : end;
        }
    } else levels[nlevels++] = 12;
    e = getenv("PS2_R2_KINDS");                  /* (r2-m2) "world", "props" or both (default) */
    if (e && *e) { kinds_world = strstr(e, "world") != NULL; kinds_props = strstr(e, "prop") != NULL; }
    e = getenv("PS2_R2_CLIP");
    if (e && *e == '0') clip_on = 0;
    e = getenv("PS2_R2_BUDGET_MS");
    if (e && *e) budget_ms = atof(e);
    e = getenv("PS2_R2_STATS");
    stats_on = e && *e && *e != '0';
    e = getenv("PS2_R2_CHECK");
    check_on = e && *e && *e != '0';
    if (env_mode >= 0)
        fprintf(stderr, "[r2] modern renderer: %s (PS2_R2), levels %s\n",
                env_mode == R2_HYBRID ? "hybrid" : env_mode == R2_BRACKET ? "bracket" : "off", levels_all ? "all" : "allowlist");
}

/* the effective mode for the frame being built */
static int mode_now(void) {
    if (!cfg_done) configure();
    int m = env_mode >= 0 ? env_mode : ps2_settings.renderer_modern ? R2_HYBRID : R2_OFF;
    if (m != R2_OFF && !gpu_hw_active()) m = R2_OFF;     /* only the GPU renderer can draw R2's segments */
    r2_mode = m;
    return m;
}

void r2_set_mode(int mode) { (void)mode; }

static double now_ms(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)qpf.QuadPart;
}

/* ---- per frame ---------------------------------------------------------------------------------------------------------- */
static int fr_open;                       /* an entry of this frame has been seen */
static int fr_mode;                       /* the mode for the whole frame (decided at its first entry) */
static int fr_level_ok, fr_model_ok, fr_block_sent;
static uint32_t fr_serial;
static r2_frame_block fr_block;
static double fr_decode_ms;
static uint8_t alloc_mark[RENDER_LIST_MAX];   /* entries made by the allocator this frame */
static r2_slot* SL;                       /* (r2-m2) the scene slot ring (hybrid; r2_mesh.c r2_slot_ring) */
static r2_slot* fr_slot;                  /* this frame's slot (hybrid; NULL: none) */
static int prev_native;                   /* the previous entry was native (a run is open) */
static int prev_class = -1;
static int bracket_open;                  /* bracket mode: the previous entry's END is due */
static int epi_due;                       /* hybrid: an epilogue is owed before the next exact entry */
static r2_pass epi_pass;

void r2_cap_alloc(ps2_ctx* ctx) {
    uint32_t i = (ctx->r[2].uw[0] - RENDER_LIST) / 16u;
    if (i < RENDER_LIST_MAX) alloc_mark[i] = 1;
}

/* ---- the frame builder -------------------------------------------------------------------------------------------- */
/* Appends one quadword of VIF codes (vif[0..2] then DIRECT n) and n quadwords of GIF data (a PACKED A+D tag and the
 * pairs) at the write position, inside a CNT of its own when none is open. 0: it cannot be written now. */
static int dmas_put(ps2_ctx* ctx, uint32_t v0, uint32_t v1, const uint64_t* ad, uint32_t npairs) {
    uint32_t end = ps2_r32(ctx, DMAS + 0x00), cnt = ps2_r32(ctx, DMAS + 0x08), cur = ps2_r32(ctx, DMAS + 0x0C);
    uint32_t vif_words = ps2_r32(ctx, DMAS + 0x10), gif_open = ps2_r32(ctx, DMAS + 0x14);
    if (gif_open || ((cur & 15) && !vif_words) || !npairs || npairs > 120) return 0;
    uint32_t nqw = 2 + npairs;                          /* VIF quadword, GIF tag, pairs */
    uint32_t pad = (16 - (cur & 15)) & 15;
    uint32_t need = pad + (nqw + (cnt ? 0u : 1u)) * 16u;
    if (cur + need > end) return 0;
    for (; cur & 15; cur += 4) ps2_w32(ctx, cur, 0);
    if (!cnt) {
        ps2_w64(ctx, cur, 0x10000000ull | nqw);         /* DMA CNT, QWC nqw; VIF NOP NOP */
        ps2_w64(ctx, cur + 8, 0);
        cur += 16;
    }
    ps2_w32(ctx, cur, v0);
    ps2_w32(ctx, cur + 4, v1);
    ps2_w32(ctx, cur + 8, 0);
    ps2_w32(ctx, cur + 12, 0x50000000u | (npairs + 1));  /* DIRECT */
    ps2_w64(ctx, cur + 16, (uint64_t)npairs | (1ull << 15) | (1ull << 60));   /* NLOOP, EOP, PACKED, NREG 1 */
    ps2_w64(ctx, cur + 24, 0xE);                        /* A+D */
    cur += 32;
    for (uint32_t i = 0; i < npairs; i++, cur += 16) {
        ps2_w64(ctx, cur, ad[2 * i]);
        ps2_w64(ctx, cur + 8, ad[2 * i + 1]);
    }
    ps2_w32(ctx, DMAS + 0x0C, cur);
    st.bytes_hint += need;
    return 1;
}

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* A+D pairs of the hint stream */
typedef struct { uint64_t p[2 * 64]; uint32_t n; } adlist;
static void ad_hint(adlist* l, uint32_t kind, uint32_t arg, uint32_t value) {
    if (l->n >= 64) return;
    l->p[2 * l->n] = r2h_make(kind, arg, value);
    l->p[2 * l->n + 1] = GS_HINT_REG;
    l->n++;
}
static void ad_reg(adlist* l, uint32_t reg, uint64_t v) {
    if (l->n >= 64) return;
    l->p[2 * l->n] = v;
    l->p[2 * l->n + 1] = reg;
    l->n++;
}
static void ad_epilogue(adlist* l, const r2_pass* P) {
    ad_reg(l, 0x06, P->tex0);            /* TEX0_1 (CLD 1: the CLUT buffer as the last material left it) */
    ad_reg(l, 0x14, P->tex1);
    ad_reg(l, 0x08, P->clamp);
    ad_reg(l, 0x47, P->test);
    ad_reg(l, 0x42, P->alpha);
    ad_reg(l, 0x3D, P->fogcol);
}

/* (r2-m2) Objects with cel outlines stay exact until R4 (their outline lines tie in Z with their mesh,
 * notes/modern-renderer.md 7 R2): the world matrices the frame's outline entries (render class 1) upload. A prop whose
 * matrix is among them belongs to an outlined object. */
#define OUTL_MAX 128
static float outl_w[OUTL_MAX][16];
static int n_outl, outl_unparsed;

static void collect_outlines(ps2_ctx* ctx) {
    n_outl = 0;
    outl_unparsed = 0;
    uint32_t left = ctx->r[20].uw[0], p = ctx->r[21].uw[0];   /* $s4 entries left (this one included), $s5 -> its pointer */
    if (left > RENDER_LIST_MAX) left = RENDER_LIST_MAX;
    for (uint32_t i = 0; i < left; i++) {
        uint32_t e = ps2_r32(ctx, p + 4 * i);
        if (e < RENDER_LIST || e >= RENDER_LIST + RENDER_LIST_MAX * 16) continue;
        uint32_t prio = ps2_r32(ctx, e);
        if (prio > 25 || ps2_r32(ctx, CLASS_TABLE + prio * 4) != 1) continue;
        uint32_t c1 = ps2_r32(ctx, e + 8);
        float w[16], vu8[4];
        st.outlines++;
        if (!c1 || !r2_header_parse(ps2_ram, c1, w, vu8)) { outl_unparsed++; continue; }
        st.outlines_parsed++;
        if (n_outl < OUTL_MAX) memcpy(outl_w[n_outl++], w, sizeof w);
    }
}

static int outlined(const float w[16]) {
    for (int i = 0; i < n_outl; i++)
        if (!memcmp(outl_w[i], w, 16 * sizeof(float))) return 1;
    return 0;
}

static void frame_begin(ps2_ctx* ctx) {
    fr_open = 1;
    fr_mode = mode_now();
    fr_block_sent = 0;
    fr_decode_ms = 0;
    prev_native = 0;
    prev_class = -1;
    bracket_open = 0;
    epi_due = 0;
    st.frames++;
    if (fr_mode == R2_OFF) return;
    uint32_t wid = ps2_r32(ctx, WID_CURRENT);
    if (wid != cur_wid || !have_gen) new_generation(wid);
    fr_level_ok = levels_all;
    for (int i = 0; i < nlevels && !fr_level_ok; i++) fr_level_ok = levels[i] == wid;
    uint32_t sp = ctx->r[29].uw[0];
    for (int i = 0; i < 4; i++) {
        uint32_t a = ps2_r32(ctx, sp + 0x8160 + 4 * (uint32_t)i), b = ps2_r32(ctx, sp + 0x8170 + 4 * (uint32_t)i);
        memcpy(&fr_block.c0[i], &a, 4);
        memcpy(&fr_block.c1[i], &b, 4);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t a = ps2_r32(ctx, sp + 4 * (uint32_t)i);
        memcpy(&fr_block.clip[i], &a, 4);
    }
    r2_screen_model sm;
    fr_model_ok = r2_screen_model_of(&fr_block, &sm);
    if (kinds_props) collect_outlines(ctx);
    fr_serial++;
    if (!fr_serial) fr_serial = 1;                /* (0 marks a slot being written) */
    fr_slot = NULL;
    if (fr_mode == R2_HYBRID) {
        if (!SL) SL = r2_slot_ring();
        if (SL) {
            fr_slot = &SL[fr_serial % R2_SLOTS];
            __atomic_store_n(&fr_slot->serial, 0u, __ATOMIC_RELEASE);
            __atomic_thread_fence(__ATOMIC_RELEASE);
            fr_slot->nent = fr_slot->nrun = 0;
            fr_slot->fb = fr_block;
        }
    }
}

static void frame_reset(void) {
    fr_open = 0;
    memset(alloc_mark, 0, sizeof alloc_mark);
}

/* the mesh of a DMA chain: cached per generation, decoded under the frame's budget; handle or 0 (*reason set) */
static uint32_t mesh_for(uint32_t addr, int form, int* reason) {
    mesh_map* s = MM;
    if (!s || !addr || (addr & 0x80000000u) || addr + 64 > PS2_RAM_SIZE) { *reason = R2M_TAG; return 0; }
    const uint8_t* head = ps2_ram + (addr & 0x1FFFFF0u);
    uint32_t h = (addr * 2654435761u) >> 17;
    for (uint32_t n = 0; n < HASH_N; n++, h = (h + 1) & (HASH_N - 1)) {
        if (!s->hk[h]) break;
        if (s->hk[h] == addr && s->hform[h] == form) {
            if (memcmp(s->fp[h], head, 32)) { s->hk[h] = 0xFFFFFFFFu; break; }   /* (content changed: removed) */
            if (s->hv[h] & 0x80000000u) { *reason = (int)(s->hv[h] & 0xFF); return 0; }
            if (!r2_mesh_get(s->hv[h])) { s->hk[h] = 0xFFFFFFFFu; break; }
            return s->hv[h];
        }
    }
    if (fr_decode_ms > budget_ms) { *reason = -1; return 0; }
    double t0 = now_ms();
    r2_mesh* m = NULL;
    int r = r2_mesh_decode(ps2_ram, addr, form, &m);
    uint32_t handle = 0;
    if (!r && !(handle = r2_store_add(m))) { r2_mesh_free(m); m = NULL; r = R2M_SIZE; }
    double dt = now_ms() - t0;
    fr_decode_ms += dt;
    st.decode_ms += dt;
    for (h = (addr * 2654435761u) >> 17; s->hk[h] && s->hk[h] != 0xFFFFFFFFu; h = (h + 1) & (HASH_N - 1)) {}
    s->hk[h] = addr;
    s->hform[h] = (uint8_t)form;
    memcpy(s->fp[h], head, 32);
    if (r) {
        s->hv[h] = 0x80000000u | (uint32_t)r;
        st.decode_fail++;
        st.reason[r]++;
        *reason = r;
        return 0;
    }
    s->hv[h] = handle;
    st.decoded++;
    return handle;
}

/* would the entry be a native candidate (class, form, cached decode)? (decides whether a native entry carries the
 * state epilogue: the next one is exact) */
static int likely_native(ps2_ctx* ctx, uint32_t e) {
    if (e < RENDER_LIST || e >= RENDER_LIST + RENDER_LIST_MAX * 16) return 0;
    int prop = alloc_mark[(e - RENDER_LIST) / 16u];
    uint32_t prio = ps2_r32(ctx, e);
    if (prio > 25 || ps2_r32(ctx, CLASS_TABLE + prio * 4) != 0) return 0;
    uint32_t c1 = ps2_r32(ctx, e + 8), c2 = ps2_r32(ctx, e + 12);
    if (!c1 || !MM || (prop ? !kinds_props || !c2 : !kinds_world)) return 0;
    uint32_t addr = c2 ? c2 : c1;
    uint32_t h = (addr * 2654435761u) >> 17;
    for (uint32_t n = 0; n < HASH_N && MM->hk[h]; n++, h = (h + 1) & (HASH_N - 1))
        if (MM->hk[h] == addr) return !(MM->hv[h] & 0x80000000u);
    return 1;                            /* not seen yet: probably (a wrong guess only moves the epilogue) */
}

/* Is the mesh inside the VU's clipping volume (|x|, |y|, |z| <= w: its guard band)? A triangle crossing it is clipped by
 * the VU program's own clipper (0x2540: the clipped fans' new vertices in its arithmetic), whose Z a GPU's clipping does
 * not reproduce to the unit - the depth ties of later exact draws (shadow volumes on the ground) need it. Such meshes
 * (near the camera) stay exact: the 8 corners of the mesh's bounds in clip space. */
static int inside_guard(const r2_mesh* m, const float mc[16]) {
    for (int k = 0; k < 8; k++) {
        float x = k & 1 ? m->bmax[0] : m->bmin[0], y = k & 2 ? m->bmax[1] : m->bmin[1], z = k & 4 ? m->bmax[2] : m->bmin[2];
        float c[4];
        for (int j = 0; j < 4; j++) c[j] = x * mc[j] + y * mc[4 + j] + z * mc[8 + j] + mc[12 + j];
        float lim = c[3] * 0.999f;
        if (!(c[3] > 0.0f) || c[0] > lim || c[0] < -lim || c[1] > lim || c[1] < -lim || c[2] > lim || c[2] < -lim) return 0;
    }
    return 1;
}

static void close_bracket(ps2_ctx* ctx, int cls) {
    if (!bracket_open) return;
    bracket_open = 0;
    adlist l = { .n = 0 };
    ad_hint(&l, R2H_END, 0, 0);
    /* the entry's own tail kicks first (the main program's continuations), unless a class switch did it */
    int same = cls == 0 && prev_class == 0;
    dmas_put(ctx, same ? MSCAL(0x3AB0) : 0, same ? MSCAL(0x0000) : 0, l.p, l.n);
}

static void owe_epilogue(ps2_ctx* ctx) {
    if (!epi_due) return;
    adlist l = { .n = 0 };
    ad_epilogue(&l, &epi_pass);
    if (dmas_put(ctx, 0, 0, l.p, l.n)) { st.epilogues++; epi_due = 0; }   /* (else: retried before the next exact entry) */
}

static double fr_cap_ms, fr_begin_ms;         /* (stats) this frame's capture time, its frame_begin part */

void r2_cap_entry(ps2_ctx* ctx) {
    if (!fr_open) {
        double tb = stats_on ? now_ms() : 0;
        frame_begin(ctx);
        fr_cap_ms = 0;
        fr_begin_ms = stats_on ? now_ms() - tb : 0;
        fr_cap_ms = fr_begin_ms;
    }
    int mode = fr_mode;
    if (mode == R2_OFF) return;
    double t0 = stats_on ? now_ms() : 0;
    uint32_t e = ctx->r[18].uw[0], prio = ctx->r[16].uw[0];
    int cls = (int)ctx->r[17].uw[0];
    uint32_t idx = (e - RENDER_LIST) / 16u;
    if (mode == R2_BRACKET) close_bracket(ctx, cls);
    int native = 0, why = 0;
    uint32_t handle = 0, c1 = ps2_r32(ctx, e + 8), c2 = ps2_r32(ctx, e + 12);
    float w[16], vu8[4] = { 1, 1, 0, 0 }, M[16];
    static const float I4[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    int form = c2 ? 1 : 0, ident = 0;
    uint32_t eflags = 0;
    int prop = idx < RENDER_LIST_MAX && alloc_mark[idx];       /* (r2-m2) an object's entry (glob prelit props) */
    if (idx < RENDER_LIST_MAX && c1 && (prop ? kinds_props && c2 && cls == 0 : kinds_world)) {
        st.cand++;
        if (prop) st.cand_props++;
        if (!fr_level_ok || !fr_model_ok) { why = 1; st.exact_level++; }
        else if (cls != 0) { why = 2; st.exact_class++; }
        else {
            int reason = 0;
            handle = mesh_for(form ? c2 : c1, form, &reason);
            if (!handle) { why = 3; if (reason < 0) st.exact_budget++; else st.exact_decode++; }
            else {
                const r2_mesh* m = r2_mesh_get(handle);
                if (form) {
                    if (!r2_header_parse(ps2_ram, c1, w, vu8)) { why = 4; st.exact_header++; }
                } else {
                    memcpy(w, m->w, sizeof w);
                    vu8[0] = m->fade;
                }
                if (!why && (f2u(vu8[3]) & 0xFFFF)) { why = 5; st.exact_fade++; }   /* 1D10's alternate A+D qword */
                if (!why && prop && outlined(w)) { why = 7; st.exact_outline++; }
                if (!why) {
                    ident = !memcmp(w, I4, sizeof w);
                    if (ident) memcpy(M, fr_block.clip, sizeof M);   /* (I x C is C exactly: products by 1, sums with 0) */
                    else r2_matmul_fast(w, fr_block.clip, M);
                    if (!inside_guard(m, M)) {
                        if (clip_on) eflags |= R2E_CLIP;    /* (r2-m2) GPU-clipped triangles (shaders/r2.hlsl r2_gs_clip) */
                        else { why = 6; st.exact_guard++; }
                    }
                }
                if (!why) native = 1;
            }
        }
    }
    if (native && prop) st.native_props++;
    if (!native) {
        if (mode == R2_HYBRID && prev_native) owe_epilogue(ctx);
        prev_native = 0;
        prev_class = cls;
        if (stats_on) { double dt_ = now_ms() - t0; st.cap_ms += dt_; fr_cap_ms += dt_; }
        return;
    }
    const r2_mesh* m = r2_mesh_get(handle);
    adlist l = { .n = 0 };
    if (mode == R2_HYBRID) {
        /* (r2-m2) the entry's data into the scene slot; the stream gets one R2H_RUN hint per run */
        r2_slot* S = fr_slot;
        int start = !prev_native;
        if (!S || S->nent >= R2_SLOT_ENTS || (start && S->nrun >= R2_SLOT_RUNS)) {
            st.exact_dmas++;
            if (prev_native) owe_epilogue(ctx);
            prev_native = 0;
            prev_class = cls;
            if (stats_on) { double dt_ = now_ms() - t0; st.cap_ms += dt_; fr_cap_ms += dt_; }
            return;
        }
        if (start) {
            int flush = prev_class == 0;     /* the previous exact mesh's pending tail first, as in an all-exact frame */
            ad_hint(&l, R2H_RUN, (fr_serial % R2_SLOTS) | S->nrun << 4, fr_serial);
            if (!dmas_put(ctx, flush ? MSCAL(0x3AB0) : 0, flush ? MSCAL(0x0000) : 0, l.p, l.n)) {
                st.exact_dmas++;
                prev_class = cls;
                if (stats_on) { double dt_ = now_ms() - t0; st.cap_ms += dt_; fr_cap_ms += dt_; }
                return;
            }
            S->run_first[S->nrun] = S->nent;
            S->run_count[S->nrun] = 0;
            S->nrun++;
            st.runs++;
        }
        r2_slot_ent* se = &S->ent[S->nent++];
        memcpy(se->m, M, sizeof M);
        se->fade = vu8[0];
        se->handle = handle;
        se->addr = (form ? c2 : c1) | (uint32_t)form << 31;
        se->flags = eflags;
        S->run_count[S->nrun - 1]++;
        st.native++;
        if (eflags & R2E_CLIP) st.clipped++;
        if (form) ps2_w32(ctx, e + 12, 0);           /* the mesh DMA; the header packet at +8 stays */
        else ps2_w32(ctx, e + 8, 0);                 /* the whole static packet */
        prev_native = 1;
        prev_class = cls;
        epi_pass = m->b[m->nb - 1].pass[m->last_pass];
        epi_due = 1;
        /* the state epilogue now when the next entry will be exact (ahead of a class switch's own packets) */
        uint32_t next = ctx->r[20].uw[0] > 1 ? ps2_r32(ctx, ctx->r[21].uw[0] + 4) : 0;
        if (!next || !likely_native(ctx, next)) owe_epilogue(ctx);
        if (stats_on) { double dt_ = now_ms() - t0; st.cap_ms += dt_; fr_cap_ms += dt_; }
        return;
    }
    if (!fr_block_sent) {
        const uint32_t* fw = (const uint32_t*)&fr_block;
        ad_hint(&l, R2H_FRAME, R2_FRAME_WORDS, fr_serial);
        for (uint32_t i = 0; i < R2_FRAME_WORDS; i++) ad_hint(&l, R2H_DATA, i, fw[i]);
    }
    /* bracket mode: the entry's data inline (offline replays of recordings need them in the stream) */
    (void)m;
    int fade = vu8[0] != 1.0f;
    uint32_t flags = (ident ? R2D_IDENTITY : 0) | (fade ? R2D_FADE : 0) | (eflags & R2E_CLIP ? R2D_CLIP : 0);
    ad_hint(&l, R2H_BEGIN, flags, handle);
    uint32_t k = 0;
    if (fade) ad_hint(&l, R2H_DATA, k++, f2u(vu8[0]));
    if (!ident) for (int i = 0; i < 16; i++) ad_hint(&l, R2H_DATA, k++, f2u(w[i]));
    ad_hint(&l, R2H_DATA, k++, (form ? c2 : c1) | (uint32_t)form << 31);
    /* the previous exact mesh's pending tail first: it must not fall inside this entry's bracket */
    int run_start = !prev_native && prev_class == 0;
    if (!dmas_put(ctx, run_start ? MSCAL(0x3AB0) : 0, run_start ? MSCAL(0x0000) : 0, l.p, l.n)) {
        st.exact_dmas++;
        prev_native = 0;
        prev_class = cls;
        if (stats_on) { double dt_ = now_ms() - t0; st.cap_ms += dt_; fr_cap_ms += dt_; }
        return;
    }
    fr_block_sent = 1;
    st.native++;
    bracket_open = 1;
    prev_native = 1;
    prev_class = cls;
    if (stats_on) { double dt_ = now_ms() - t0; st.cap_ms += dt_; fr_cap_ms += dt_; }
}

void r2_cap_done(ps2_ctx* ctx) {
    if (fr_open && fr_mode == R2_BRACKET) close_bracket(ctx, -1);
    if (fr_open && fr_mode == R2_HYBRID) owe_epilogue(ctx);
    if (fr_open && fr_slot) {                     /* publish the frame's scene slot (the chain is kicked after this) */
        __atomic_store_n(&fr_slot->serial, fr_serial, __ATOMIC_RELEASE);
        fr_slot = NULL;
    }
    if (stats_on && fr_open && fr_cap_ms > 3.0)       /* (a slow capture: its parts) */
        fprintf(stderr, "[r2] slow capture v%u: %.2f ms (frame begin %.2f, decode %.2f)\n", ps2_vblank_count(), fr_cap_ms,
                fr_begin_ms, fr_decode_ms);
    frame_reset();
    if (stats_on) {
        static uint32_t last_vb;
        uint32_t vb = ps2_vblank_count();
        if (vb - last_vb >= 60) {
            last_vb = vb;
            char line[512];
            r2_stats_line(line, sizeof line);
            fprintf(stderr, "[r2] v%u %s\n", vb, line);
            memset(&st, 0, sizeof st);
        }
    }
}

void r2_stats_line(char* buf, size_t n) {
    const r2_stats* s = &st;
    double f = s->frames ? (double)s->frames : 1.0;
    int o = snprintf(buf, n, "mode %d gen %u: per frame %.0f candidates (%.0f props), %.0f native (%.0f props, %.1f clipped; %.1f runs, %.1f "
                             "epilogues), exact: level %.0f class %.0f decode %.0f budget %.0f header %.0f fade %.0f guard %.0f outline %.1f dmas %.0f; "
                             "outlines %.1f (%.1f parsed); decoded %u (failed %u, %.1f ms); capture %.3f ms/frame, hints %.1f KB/frame",
                     r2_mode, r2_store_gen(), s->cand / f, s->cand_props / f, s->native / f, s->native_props / f, s->clipped / f, s->runs / f,
                     s->epilogues / f, s->exact_level / f, s->exact_class / f, s->exact_decode / f, s->exact_budget / f, s->exact_header / f,
                     s->exact_fade / f, s->exact_guard / f, s->exact_outline / f, s->exact_dmas / f, s->outlines / f, s->outlines_parsed / f,
                     s->decoded, s->decode_fail, s->decode_ms, s->cap_ms / f, s->bytes_hint / f / 1024.0);
    for (int r = 1; r < R2M_N && o > 0 && (size_t)o < n; r++)
        if (s->reason[r]) o += snprintf(buf + o, n - (size_t)o, " [%s %u]", r2m_reason[r], s->reason[r]);
}
