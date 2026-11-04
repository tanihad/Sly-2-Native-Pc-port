/* (r2-m5b, R5 part 2) Meshes crossing the VU's guard band, native: R2 draws their triangles that need no clipping and
 * the VU's own clipper draws the others, run on the EE thread in its own arithmetic.
 *
 * The main program (376110651) transforms a prelit sub-batch with 0x06D0 and marks each strip triangle in the w word of
 * its last vertex (0x0C60, called when one of its three vertices is outside the guard band |x|, |y|, |z| <= w):
 *   - all three beyond the far plane (z < -w) or the near plane (z > w), or all three outside one side of the screen (the
 *     clip coordinates scaled by (7.93, 18.1, 1, 1) against w: the screen inside the GS's guard band): +0x1000, culled;
 *   - otherwise +0x2000 and [10].z = 1: the strip skips it (ADC) and the clipper 0x2540 draws it after the sub-batch's
 *     strips: per marked triangle its three records are copied to VU 0x0E, 0x2150 takes them back to clip space
 *     ((screen - VU 1) / VU 0 x w; S, T, Q x w; colours ITOF0) and clips them plane by plane (0x1E88: +z, -z, +x, -x, +y,
 *     -y, each only when the CLIP flags say a vertex is outside it; Sutherland-Hodgman in place, t = d1 / (d1 - d0) by
 *     DIV), projects the polygon again (fog w from VU 9.x / 9.y / 9.z), 0x1DC8 culls it by the sign of its area against
 *     the strip's parity, and it is XGKICKed as a fan: pass A's A+D packet, the fan (GIF tag at VU 0x0D: NLOOP | EOP, the
 *     PRIM / REGS of the material packet's second tag), pass B's A+D packet and the fan again with pass B's tag, pass A's
 *     A+D packet again.
 * Here: the clip coordinates of every vertex in the VU's arithmetic (0x07F8 under the VU MXCSR, as r2_matmul_fast), the
 * marks of 0x0C60 per triangle, and for each marked triangle its three records exactly as 0x06D0 / 0x0BE0 leave them in
 * a private VU1's memory (vu.c vu_private_run), where the game's own recompiled 0x2540 runs: the packets it kicks are
 * the exact path's, bit for bit (scripts/r2_mesh_check.c compares them with a VU recording). R2's clip pipeline
 * (shaders/r2.hlsl r2_gs_clip) draws the triangles all inside and leaves out the others; the packets are decoded by the
 * GS thread after the entry's segment (R2H_OUTLINE), so the entry ends its run.
 * Rigid prelit sub-batches only (lit / skinned ones stay exact). */
#include "r2.h"
#include "vu_internal.h"
#include "vu_ops.h"

#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>

#pragma STDC FENV_ACCESS ON
#pragma clang fp contract(off)

#define CLIP_X 0x200u                  /* the private VU's buffer (vi4): its header qwords and records */
#define CLIP_REC 0x20u                 /* the three records (relative) */
#define CLIP_PKA 0x40u                 /* pass A's material packet (relative: +1 fan tag, +2 A+D) */
#define CLIP_PKB 0x50u
#define CLIP_RET (0x34C8u / 8u)        /* 0x2540's caller's return address: ISW [10].x, then the E bit */
#define R2_CLIP_ZMAX 0xFFE00u          /* gpu_front.h GPU_Z_FF_MAX (GPU_ZE_ONE - 4 x GPU_ZB_MARGIN) */

uint32_t ps2_vblank_count(void);
static vu_state cv;
static _Alignas(16) uint8_t cv_data[16384], cv_micro[16384];
static vu_op cv_cache[2048];
static int cv_state;                   /* 0 not set up, 1 ready, -1 no image */
int r2_clip_why;                       /* why the last r2_clip_entry said no: 1 batch kind, 2 w <= 0, 3 full, 4 size,
                                          5 fan Z beyond the FF range, 6 fan Q no clipped vertex has (stray-tri) */

int r2_clip_setup(const uint8_t* micro) {
    if (cv_state) return cv_state > 0;
    if (micro) memcpy(cv_micro, micro, sizeof cv_micro);
    else if (!vu_compiled_image("376110651", cv_micro, sizeof cv_micro)) { cv_state = -1; return 0; }
    vu_private_init(&cv, cv_data, cv_micro, cv_cache);
    cv_state = 1;
    return 1;
}

static inline float bf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float cl(float f) { return vu_sclean(fb(f)); }
/* the FMAC under the VU MXCSR (round toward zero, FTZ / DAZ): operands cleaned, the raw host result as vu_fmac */
static inline float m_(float a, float b) { return _mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(cl(a)), _mm_set_ss(cl(b)))); }
static inline float a_(float a, float b) { return _mm_cvtss_f32(_mm_add_ss(_mm_set_ss(cl(a)), _mm_set_ss(cl(b)))); }

/* the private VU's XGKICKs: the packet (tags until EOP) appended to the output */
static void kick(void* user, const uint8_t* data, uint32_t a) {
    r2_fanbuf* o = (r2_fanbuf*)user;
    for (int guard = 0; guard < 16; guard++) {
        const uint32_t* t = (const uint32_t*)(data + (a & 0x3FF) * 16u);
        uint64_t lo = (uint64_t)t[0] | (uint64_t)t[1] << 32;
        uint32_t nloop = (uint32_t)(lo & 0x7FFF), eop = (uint32_t)((lo >> 15) & 1), flg = (uint32_t)((lo >> 58) & 3), nreg = (uint32_t)(lo >> 60);
        if (!nreg) nreg = 16;
        uint32_t len = 1 + (flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop);
        if (o->n + len * 16 > o->cap || len > 0x200) { o->overflow = 1; return; }
        for (uint32_t i = 0; i < len; i++) memcpy(o->out + o->n + i * 16, data + ((a + i) & 0x3FF) * 16u, 16);
        o->n += len * 16;
        a = (a + len) & 0x3FF;
        if (eop) return;
    }
    o->overflow = 1;
}

/* CLIP's judgement (vu_ops.h vu_clip_judge) */
static inline uint32_t judge(float x, float y, float z, float w) { return vu_clip_judge(fb(x), fb(y), fb(z), fb(w)); }

static float cc[R2_CLIP_VMAX][4];      /* the vertices' clip coordinates (0x07F8) */
static uint8_t oc[R2_CLIP_VMAX], os_[R2_CLIP_VMAX];   /* their CLIP flags, unscaled and scaled (0x0C60's vf31) */

/* record k of the sub-batch (strip vertex v) as 0x06D0 / 0x0BE0 leave it: STQ + w, colour A, colour B, XYZF */
static void record(const r2_vtx* v, const float* c, const r2_frame_block* f, float fade, uint32_t* q) {
    uint32_t flags;
    float Q = bf(vu_div(0x3F800000u, fb(c[3]), &flags));     /* DIV Q, vf00w, vf19w */
    float st[4] = { m_((float)v->u * (1.0f / 4096.0f), Q), m_((float)v->v * (1.0f / 4096.0f), Q), m_(1.0f, Q), c[3] };
    memcpy(q, st, 16);
    for (int k = 0; k < 2; k++) {
        uint32_t w = v->col[k];
        for (int j = 0; j < 4; j++) q[4 + 4 * k + j] = (w >> (8 * j)) & 255u;
    }
    if (fade < 1.0f) {                                         /* 0x0BE0: colour A's alpha, colour B's RGB x VU 8.x */
        float a = m_((float)q[7], fade);
        q[7] = (uint32_t)(int32_t)a;                           /* (FTOI0 of 0 .. 255: truncation) */
        for (int j = 0; j < 3; j++) q[8 + j] = (uint32_t)(int32_t)m_((float)q[8 + j], fade);
    }
    float p[4];                                                /* VU 1 + VU 0 x (MINI(clip, FLT_MAX) x Q) */
    for (int j = 0; j < 3; j++) p[j] = a_(a_(f->c1[j], 0.0f), m_(f->c0[j], m_(c[j], Q)));
    p[3] = 0.0f;
    memcpy(q + 12, p, 16);
}

/* every vertex's clip coordinates and CLIP flags (0x07F8: MULAx / MADDAy / MADDAz / MADDw x 1.0, 4 lanes at once;
 * 0x0C60's scaled copy: x 7.93, y 18.1) */
int r2_clip_prep(const r2_mesh* m, const float M[16]) {
    if (m->nvtx > R2_CLIP_VMAX) return 0;
    unsigned int old = _mm_getcsr();
    _mm_setcsr(VU_MXCSR);
    __m128 r0 = _mm_loadu_ps(M), r1 = _mm_loadu_ps(M + 4), r2 = _mm_loadu_ps(M + 8), r3 = _mm_loadu_ps(M + 12);
    const __m128 k31 = _mm_setr_ps(bf(0x40FDC28Fu), bf(0x4190CCCDu), 1.0f, 1.0f);
    for (uint32_t i = 0; i < m->nvtx; i++) {
        const r2_vtx* v = &m->vtx[i];
        __m128 acc = _mm_mul_ps(r0, _mm_set1_ps(v->x));
        acc = _mm_add_ps(acc, _mm_mul_ps(r1, _mm_set1_ps(v->y)));
        acc = _mm_add_ps(acc, _mm_mul_ps(r2, _mm_set1_ps(v->z)));
        acc = _mm_add_ps(acc, _mm_mul_ps(r3, _mm_set1_ps(1.0f)));
        _mm_storeu_ps(cc[i], acc);
        float s[4];
        _mm_storeu_ps(s, _mm_mul_ps(acc, k31));
        oc[i] = (uint8_t)judge(cc[i][0], cc[i][1], cc[i][2], cc[i][3]);
        os_[i] = (uint8_t)judge(s[0], s[1], s[2], s[3]);
    }
    _mm_setcsr(old);
    return 1;
}

/* 0x0C60's verdict on the triangle ending at strip vertex k of sub-batch b (after r2_clip_prep): R2CV_* */
int r2_clip_verdict(const r2_mesh* m, uint32_t b, uint32_t k) {
    const r2_batch* B = &m->b[b];
    uint32_t i0 = B->vfirst + k - 2, i1 = i0 + 1, i2 = i0 + 2;
    if (k < 2 || ((m->vtx[i2].wbits >> 15) & 1)) return R2CV_ADC;   /* (0x0C60 returns: no triangle) */
    uint32_t a = oc[i0], bb = oc[i1], c = oc[i2];
    if (!(a | bb | c)) return R2CV_STRIP;
    if (a & bb & c & 0x30) return R2CV_CULLED;                 /* all beyond the far or the near plane */
    if (os_[i0] & os_[i1] & os_[i2] & 0x0F) return R2CV_CULLED; /* all off one side of the screen */
    return R2CV_CLIP;
}

/* sub-batch b's clipped triangles through the private VU's 0x2540 (after r2_clip_prep); -1: not supported, else the
 * number of triangles that kicked fans */
int r2_clip_batch(const r2_mesh* m, uint32_t b, float fade, const r2_frame_block* f, r2_fanbuf* o) {
    const r2_batch* B = &m->b[b];
    int ntri = 0, nfan = 0, ok = 1;
    unsigned int old = 0;
    for (uint32_t k = 2; k < B->vcount && ok; k++) {
        if (r2_clip_verdict(m, b, k) != R2CV_CLIP) continue;
        uint32_t i0 = B->vfirst + k - 2, i1 = i0 + 1, i2 = i0 + 2;
        if (!ntri++) {                                     /* the sub-batch's header in the private VU */
            if (B->lit || B->skin || B->wide || B->tanim || !B->npass) { r2_clip_why = 1; return -1; }
            if (cv_state <= 0 && !r2_clip_setup(NULL)) { r2_clip_why = 1; return -1; }
            old = _mm_getcsr();
            _mm_setcsr(VU_MXCSR);
            memset(cv_data + (CLIP_X + 9) * 16u, 0, 16 * 7);
            memcpy(cv_data + 0 * 16u, f->c0, 16);
            memcpy(cv_data + 1 * 16u, f->c1, 16);
            memcpy(cv_data + (CLIP_X + 9) * 16u, B->vu9, 16);
            uint32_t one = 1, fl = B->cull ? 0u : 0x80u;
            memcpy(cv_data + (CLIP_X + 10) * 16u + 8, &one, 4);
            memcpy(cv_data + (CLIP_X + 11) * 16u, &fl, 4);
            memset(cv_data + (CLIP_X + CLIP_PKA) * 16u, 0, 16 * 0x20);
            memcpy(cv_data + (CLIP_X + CLIP_PKA + 1) * 16u, B->pk[0], 16 * 8);
            if (B->npass > 1) memcpy(cv_data + (CLIP_X + CLIP_PKB + 1) * 16u, B->pk[1], 16 * 8);
            memset(cv_data + 13 * 16u, 0, 16);
        }
        uint32_t* rq = (uint32_t*)(cv_data + (CLIP_X + CLIP_REC) * 16u);
        record(&m->vtx[i0], cc[i0], f, fade, rq);
        record(&m->vtx[i1], cc[i1], f, fade, rq + 16);
        record(&m->vtx[i2], cc[i2], f, fade, rq + 32);
        rq[32 + 15] = 0x9000u;                             /* the mark 0x2540 tests: ADC, (w & 0x3000) == 0x1000 */
        vu_state* st = &cv;
        memset(st->vf, 0, sizeof st->vf);
        st->vf[0].f[3] = 1.0f;
        uint32_t v3[4] = { CLIP_PKA, B->npass > 1 ? CLIP_PKB : 0u, 0u, 0u };
        memcpy(&st->vf[3], v3, 16);
        memset(&st->acc, 0, sizeof st->acc);
        memset(st->vi, 0, sizeof st->vi);
        st->vi[4] = (uint16_t)CLIP_X;
        st->vi[8] = (uint16_t)((k & 1) ? 0xFFFFu : 0u);   /* 0: the strip's parity (vi07) 1 at even k, -1: 0 */
        st->vi[10] = (uint16_t)(CLIP_REC + 8);
        st->vi[11] = (uint16_t)(CLIP_REC + 12);
        st->vi[15] = (uint16_t)CLIP_RET;
        st->status = st->mac = st->clip = st->clip_pipe = 0;
        uint32_t before = o->n;
        vu_private_run(st, 0x2540u / 8u, kick, o);
        _mm_setcsr(VU_MXCSR);
        static int dbg = -1;                           /* (diagnostics) R2_CLIP_DBG=1: fans with a Z beyond 0xFFE00 */
        if (dbg < 0) dbg = getenv("R2_CLIP_DBG") != NULL;
        if (dbg) {
            static int shown;
            for (uint32_t p = before; p + 16 <= o->n && shown < 20;) {
                const uint32_t* t = (const uint32_t*)(o->out + p);
                uint32_t nloop = t[0] & 0x7FFF, nreg = t[1] >> 28;
                if (nreg == 4) {
                    for (uint32_t v = 0; v < nloop; v++)
                        if (((((const uint32_t*)(o->out + p + (1 + 4 * v + 3) * 16))[2] >> 4) & 0xFFFFFF) > 0xFFE00u) {
                            shown++;
                            fprintf(stderr, "[r2clip] high Z: mesh %08X batch %u k %u cull %d\n", m->addr, b, k, B->cull);
                            for (int j = 0; j < 3; j++) {
                                const float* c = cc[i0 + (uint32_t)j];
                                const uint32_t* r = rq + 16 * j;
                                fprintf(stderr, "  v%d clip %g %g %g %g (oc %02X os %02X) rec %08X %08X %08X %08X | %08X %08X %08X\n", j, c[0], c[1],
                                        c[2], c[3], oc[i0 + (uint32_t)j], os_[i0 + (uint32_t)j], r[0], r[1], r[2], r[3], r[12], r[13], r[14]);
                            }
                            break;
                        }
                    p += (1 + 4 * nloop) * 16;
                } else p += (1 + nloop) * 16;
            }
        }
        o->tris++;
        if (o->overflow) { ok = 0; r2_clip_why = 3; }
        else if (o->n != before) { o->fans++; nfan++; }
        else o->culled++;
    }
    if (ntri) _mm_setcsr(old);
    return ok ? nfan : -1;
}

/* The entry: every sub-batch's strip triangles R2 draws as runs of its index list (rt: words [total, nb, per sub-batch
 * nruns, then start | count << 16 in triangles]); the clipped ones' packets into o. */
int r2_clip_entry(const r2_mesh* m, const float M[16], float fade, const r2_frame_block* f, r2_fanbuf* o, uint32_t* last_fanned,
                  uint32_t* rt, uint32_t rtcap) {
    r2_clip_why = 0;
    if (m->any_skin || !r2_clip_prep(m, M)) { r2_clip_why = 4; return 0; }
    uint32_t n0 = o->n, w = 2;
    *last_fanned = 0;
    for (uint32_t b = 0; b < m->nb; b++) {
        const r2_batch* B = &m->b[b];
        if (w + 1 > rtcap) { r2_clip_why = 3; o->n = n0; return 0; }
        uint32_t nrw = w++, nr = 0, j = 0, rs = 0, inrun = 0, nclip = 0;
        for (uint32_t k = 2; k < B->vcount; k++) {
            int v = r2_clip_verdict(m, b, k);
            if (v == R2CV_ADC) continue;                       /* (not in R2's index list) */
            int in = v == R2CV_STRIP;
            if (in) {
                uint32_t i0 = B->vfirst + k - 2;
                if (!(cc[i0][3] > 0.0f && cc[i0 + 1][3] > 0.0f && cc[i0 + 2][3] > 0.0f)) { r2_clip_why = 2; o->n = n0; return 0; }
            }
            nclip += v == R2CV_CLIP;
            if (in && !inrun) { rs = j; inrun = 1; }
            else if (!in && inrun) {
                if (w + 1 > rtcap) { r2_clip_why = 3; o->n = n0; return 0; }
                rt[w++] = rs | (j - rs) << 16; nr++; inrun = 0;
            }
            j++;
        }
        if (inrun) {
            if (w + 1 > rtcap) { r2_clip_why = 3; o->n = n0; return 0; }
            rt[w++] = rs | (j - rs) << 16; nr++;
        }
        rt[nrw] = nr;
        if (nclip) {
            int r = r2_clip_batch(m, b, fade, f, o);
            if (r < 0) { o->n = n0; o->overflow = 0; return 0; }
            if (r) *last_fanned = b + 1;
        }
    }
    /* a fan vertex with a Z beyond the exact path's FF range (degenerate objects: clip coordinates ~1e27, vertices on
     * both sides of w = 0) would put the rest of the frame's Z on the ROV tier (gpu_front.c z_high): such entries stay
     * exact.
     * (stray-tri) The same degenerate objects (w ~2^69: a hidden object's matrix) also give fans whose Z is in range but
     * whose vertex Q is not one a clipped vertex can have (2^-69 .. 2^-91, negative, FLT_MAX): the private clipper's
     * packets for them are not the VU's (an all-exact frame draws no such triangle), and drawn they are the one-frame
     * stray triangles of the owner's report - huge or sliver wedges across the screen, flat (S/Q, T/Q off the texture).
     * Such entries stay exact too (r2_clip_why 6). PS2_R2_FANQ=0: off (diagnostics). */
    static int qlog = -1, qchk = -1;     /* (PS2_R2_FANQ_LOG=n: log the first n such vertices) */
    if (qlog < 0) { const char* e = getenv("PS2_R2_FANQ_LOG"); qlog = e ? atoi(e) : 0; e = getenv("PS2_R2_FANQ"); qchk = !(e && *e == '0'); }
    for (uint32_t p = n0; p + 16 <= o->n;) {
        const uint32_t* t = (const uint32_t*)(o->out + p);
        uint32_t nloop = t[0] & 0x7FFF, nreg = t[1] >> 28;
        if (nreg == 4) {
            for (uint32_t v = 0; v < nloop; v++) {
                if (((((const uint32_t*)(o->out + p + (1 + 4 * v + 3) * 16))[2] >> 4) & 0xFFFFFF) > R2_CLIP_ZMAX) {
                    r2_clip_why = 5; o->n = n0; return 0;
                }
                float q;
                memcpy(&q, (const uint8_t*)o->out + p + (1 + 4 * v) * 16 + 8, 4);
                if ((t[2] & 0xFu) == 2u && !(q > 1e-18f && q < 1e18f)) {   /* (the first register STQ: REGS 4F12 / 41F2) */
                    if (qlog > 0) {
                        qlog--;
                        fprintf(stderr, "[fanq] v%u mesh %08X vertex %u of %u: Q %g (regs %08X)\n", ps2_vblank_count(), m->addr, v, nloop, q, t[2]);
                    }
                    if (qchk) { r2_clip_why = 6; o->n = n0; return 0; }
                }
            }
            p += (1 + 4 * nloop) * 16;
        } else p += (1 + nloop) * 16;
    }
    rt[0] = w;
    rt[1] = m->nb;
    return 1;
}