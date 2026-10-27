/* (r2-m1) Level-1 vertex oracle of the modern renderer (notes/modern-renderer.md 5): R2's mesh decoder and vertex
 * function against what the game's VU1 program actually outputs.
 *
 *   r2_mesh_check <vu recording> <trace dir> [frame n] [--verbose]
 *
 * Decodes every static-world entry of the traced frame (frame_<n>.txt + ram_<n>.bin, runtime/r2_trace.c) with
 * runtime/r2/r2_mesh.c, replays the VU recording (PS2_VU_RECORD) with the interpreter and, for every XGKICK of a prelit
 * batch (pass A / pass B vertices and the material A+D packets), compares the VU's output with r2_vertex_eval on the
 * decoded vertex, with the recording's own frame constants (VU 0..5) and the batch header's matrix / fade. A batch is
 * recognised by its input vertices in the recorded VU memory (the decoded mesh's first position words), so the trace
 * frame and the recording may be different frames of the same scene. Prints the level-1 gate quantities: XY / Z / fog
 * differences, S/T/Q relative error, RGBA mismatches, the primitive set (ADC: triangles drawn by the VU that R2's index
 * lists lack, and the reasons the VU culled triangles R2 draws: backface by the winding of R2's triangle, off screen,
 * clipped by the VU's clipper). Built by scripts/r2_mesh_check.ps1. */
#include "vu_internal.h"
#include "vu_record.h"
#include "r2/r2.h"

#include <math.h>
#include <stdlib.h>
#include <windows.h>
#include <xmmintrin.h>

void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    (void)unit; (void)phys; (void)data; (void)size; (void)is_write;
}
uint32_t ps2_vblank_count(void) { return 0; }

/* ---- kicks of the current program ---------------------------------------------------------------------------- */
#define MAXK 64
static struct { uint32_t addr; uint8_t pkt[16 * 300]; uint32_t qw; } K[MAXK];
static int nk;
void gif_path1_kick(const uint8_t* vu1_data, uint32_t qw_addr) {
    if (nk >= MAXK) return;
    K[nk].addr = qw_addr & 0x3FF;
    uint32_t n = 0, a = qw_addr & 0x3FF;
    for (int guard = 0; guard < 8 && n < 300; guard++) {      /* copy the packet (tags + data) */
        const uint64_t* q = (const uint64_t*)(vu1_data + a * 16);
        uint32_t nloop = (uint32_t)(q[0] & 0x7FFF), eop = (uint32_t)((q[0] >> 15) & 1), nreg = (uint32_t)(q[0] >> 60);
        if (!nreg) nreg = 16;
        uint32_t len = 1 + nloop * nreg;
        for (uint32_t i = 0; i < len && n < 300; i++, n++) memcpy(K[nk].pkt + n * 16, vu1_data + ((a + i) & 0x3FF) * 16, 16);
        a = (a + len) & 0x3FF;
        if (eop) break;
    }
    K[nk].qw = n;
    nk++;
}

/* ---- decoded meshes ------------------------------------------------------------------------------------------ */
typedef struct { r2_mesh* m; uint32_t b; } bref;
#define HN 65536
static uint64_t hkey[HN];
static bref hval[HN];
/* a sub-batch's whole input (positions, w words, UV, both colours) in canonical form: from a decoded mesh or from the
 * recorded VU memory (4 qw per vertex: UV, colour A, colour B, position) */
static uint64_t hstep(uint64_t h, uint32_t v) { h ^= v; h *= 0x100000001B3ull; return h ^ (h >> 31); }
static uint64_t key_mesh(const r2_vtx* v, uint32_t n) {
    uint64_t h = 0xCBF29CE484222325ull ^ n;
    for (uint32_t k = 0; k < n; k++, v++) {
        uint32_t p[3];
        memcpy(p, &v->x, 12);
        h = hstep(h, p[0]); h = hstep(h, p[1]); h = hstep(h, p[2]); h = hstep(h, v->wbits);
        h = hstep(h, (uint16_t)v->u | (uint32_t)(uint16_t)v->v << 16); h = hstep(h, v->col[0]); h = hstep(h, v->col[1]);
    }
    return h ? h : 1;
}
static uint64_t key_vu(const uint32_t* q, uint32_t n) {
    uint64_t h = 0xCBF29CE484222325ull ^ n;
    for (uint32_t k = 0; k < n; k++, q += 16) {
        h = hstep(h, q[12]); h = hstep(h, q[13]); h = hstep(h, q[14]); h = hstep(h, q[15]);
        h = hstep(h, (q[0] & 0xFFFF) | (q[1] & 0xFFFF) << 16);
        h = hstep(h, (q[4] & 255) | (q[5] & 255) << 8 | (q[6] & 255) << 16 | (q[7] & 255) << 24);
        h = hstep(h, (q[8] & 255) | (q[9] & 255) << 8 | (q[10] & 255) << 16 | (q[11] & 255) << 24);
    }
    return h ? h : 1;
}
static void put(uint64_t k, bref v) {
    for (uint32_t i = (uint32_t)k & (HN - 1), n = 0; n < HN; n++, i = (i + 1) & (HN - 1))
        if (!hkey[i] || hkey[i] == k) { hkey[i] = k; hval[i] = v; return; }
}
static bref* get(uint64_t k) {
    for (uint32_t i = (uint32_t)k & (HN - 1), n = 0; n < HN && hkey[i]; n++, i = (i + 1) & (HN - 1))
        if (hkey[i] == k) return &hval[i];
    return NULL;
}

/* ---- statistics ------------------------------------------------------------------------------------------------ */
static uint64_t n_vtx, xy_hist[6], z_hist[6], f_bad, rgba_bad, stq_bad, adc_extra, adc_missing, culled_back, culled_front,
    culled_off, culled_clip, packets, packet_bad, kicks_vertex, kicks_clip, kicks_unknown, programs_seen, batches_seen,
    tri_drawn, n_used, nocull_drawn, area_pos_culled, area_neg_culled, area_pos_drawn, area_neg_drawn, z_max, xy_max;
static double stq_max, model_ex, model_ez;
static uint64_t model_bad;
static long cur_prog;
static int verbose;

static int bucket(int64_t d) { d = d < 0 ? -d : d; return d == 0 ? 0 : d <= 1 ? 1 : d <= 2 ? 2 : d <= 4 ? 3 : d <= 16 ? 4 : 5; }

typedef struct { r2_mesh* m; uint32_t b; float mat[16]; float fade; int valid; } bufbatch;
static bufbatch BB[2][2];                /* [buffer 0x032 / 0x219][sub-batch] as last set up */

static int bufidx(uint32_t a) { return a >= 0x219 ? 1 : 0; }

static void check_vertices(const bufbatch* bb, int pass, const uint8_t* pkt, uint32_t qw, const r2_frame_block* fb) {
    const r2_batch* B = &bb->m->b[bb->b];
    if (pass >= B->npass) { kicks_unknown++; return; }
    const r2_pass* P = &B->pass[pass];
    uint32_t nloop = *(const uint32_t*)pkt & 0x7FFF;
    if (nloop != B->vcount || qw < 1 + 4 * nloop) { kicks_unknown++; return; }
    kicks_vertex++;
    int csel = P->csel;
    r2_gsv prev[2];
    /* vertices of triangles the VU draws (the gate's quantities are about what is drawn: vertices of clipped or culled
     * triangles near or behind the camera have no meaning in GS coordinates) */
    uint8_t used[256] = { 0 };
    for (uint32_t k = 2; k < nloop && k < 256; k++) {
        const uint32_t* xz = (const uint32_t*)(pkt + (1 + 4 * k + 3) * 16);
        if (!((xz[3] >> 15) & 1)) used[k] = used[k - 1] = used[k - 2] = 1;
    }
    for (uint32_t k = 0; k < nloop; k++) {
        const uint32_t* st = (const uint32_t*)(pkt + (1 + 4 * k) * 16);
        const uint32_t* col = (const uint32_t*)(pkt + (1 + 4 * k + 1 + csel) * 16);
        const uint32_t* xyz = (const uint32_t*)(pkt + (1 + 4 * k + 3) * 16);
        r2_gsv o;
        r2_vertex_eval(&bb->m->vtx[B->vfirst + k], bb->mat, fb, B, csel, bb->fade, &o);
        n_vtx++;
        int use = k < 256 && used[k];
        n_used += (uint64_t)use;
        int64_t dx = (int64_t)(xyz[0] & 0xFFFF) - o.x, dy = (int64_t)(xyz[1] & 0xFFFF) - o.y;
        int64_t dz = (int64_t)((xyz[2] >> 4) & 0xFFFFFF) - (int64_t)o.z;
        int64_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, az = dz < 0 ? -dz : dz;
        if (use) {                       /* the linear screen model of the GPU vertex shader against the VU's bits */
            r2_screen_model sm;
            if (r2_screen_model_of(fb, &sm)) {
                const r2_vtx* v = &bb->m->vtx[B->vfirst + k];
                const float* mm = bb->mat;
                double cx = v->x * mm[0] + v->y * mm[4] + v->z * mm[8] + mm[12], cz = v->x * mm[2] + v->y * mm[6] + v->z * mm[10] + mm[14];
                double cw = v->x * mm[3] + v->y * mm[7] + v->z * mm[11] + mm[15];
                double mx = sm.bx + sm.ax * cx / cw, mz = sm.bz + sm.az * cz / cw;
                double ex = fabs(mx - (double)(xyz[0] & 0xFFFF)), ez = fabs(mz - (double)((xyz[2] >> 4) & 0xFFFFFF));
                if (ex > model_ex) model_ex = ex;
                if (ez > model_ez) model_ez = ez;
            } else model_bad++;
            xy_hist[bucket(ax > ay ? ax : ay)]++;
            z_hist[bucket(az)]++;
            if ((uint64_t)(ax > ay ? ax : ay) > xy_max) xy_max = (uint64_t)(ax > ay ? ax : ay);
            if ((uint64_t)az > z_max) z_max = (uint64_t)az;
        }
        uint32_t f = (xyz[3] >> 4) & 0xFF;
        if (f != o.f && (f > o.f ? f - o.f : o.f - f) > 1) f_bad++;
        float s, t, q;
        memcpy(&s, &st[0], 4); memcpy(&t, &st[1], 4); memcpy(&q, &st[2], 4);
        double es = fabs((double)s - o.s) / (fabs(o.s) + 1e-30), et = fabs((double)t - o.t) / (fabs(o.t) + 1e-30),
               eq = fabs((double)q - o.q) / (fabs(o.q) + 1e-30);
        double e = es > et ? es : et;
        e = e > eq ? e : eq;
        if (use && (fabs(o.s) > 1e-6 || fabs(s) > 1e-6) && e > stq_max) stq_max = e;
        if (use && e > 1.0 / 65536 && (fabs(o.s) > 1e-6 || fabs(o.t) > 1e-6)) stq_bad++;
        uint32_t rgba = (col[0] & 255) | (col[1] & 255) << 8 | (col[2] & 255) << 16 | (col[3] & 255) << 24;
        if (rgba != o.rgba) {
            rgba_bad++;
            if (verbose && rgba_bad < 10) printf("  rgba %08X want %08X (mesh %08X batch %u v %u csel %d fade %g)\n", rgba, o.rgba,
                                                 bb->m->addr, bb->b, k, csel, bb->fade);
        }
        if (verbose && use && (ax > 1 || ay > 1 || az > 2) && n_vtx < 2000000) {
            static int shown;
            if (shown++ < 10) printf("  xyz %u,%u,%u want %d,%d,%u (mesh %08X batch %u v %u)\n", xyz[0] & 0xFFFF, xyz[1] & 0xFFFF,
                                     (xyz[2] >> 4) & 0xFFFFFF, o.x, o.y, o.z, bb->m->addr, bb->b, k);
        }
        /* primitive set: the triangle ending at k (k >= 2) */
        int adc = (xyz[3] >> 15) & 1;
        if (k >= 2) {
            int in_adc = o.adc;
            if (!adc && in_adc) adc_extra++;                     /* the VU draws a triangle R2's lists lack */
            if (!adc) tri_drawn++;
            /* the winding of R2's triangle (strip order, odd triangles swapped) in GS screen coordinates */
            const r2_gsv *A = &prev[0], *Bv = &prev[1];
            double ax2 = (double)Bv->x - A->x, ay2 = (double)Bv->y - A->y, bx2 = (double)o.x - A->x, by2 = (double)o.y - A->y;
            double area = ax2 * by2 - ay2 * bx2;
            if (k & 1) area = -area;
            if (!in_adc) {
                if (adc) {
                    uint32_t m = xyz[3] & 0xFFFF;                /* the VU's marks: +0x4000 back, +0x1000 off screen, +0x2000 clip */
                    uint32_t base = bb->m->vtx[B->vfirst + k].wbits & 0xFFFF;
                    uint32_t added = (m - (base + ((o.f << 4) - ((base >> 4 & 0xFF) << 4)))) & 0xF000;
                    (void)added;
                    if ((m & 0x3000) == 0x1000) culled_clip++;
                    else if ((m & 0x3000) == 0x3000) { if (area > 0) area_pos_culled++; else area_neg_culled++; culled_back++; }
                    else culled_off++;
                } else if (B->cull) {
                    if (area > 0) area_pos_drawn++; else area_neg_drawn++;
                } else nocull_drawn++;
            }
        }
        prev[0] = prev[1];
        prev[1] = o;
    }
}

static void check_material(const bufbatch* bb, int pass, const uint8_t* pkt, uint32_t qw) {
    const r2_batch* B = &bb->m->b[bb->b];
    if (pass >= B->npass || qw < 7) { packet_bad++; return; }
    const r2_pass* P = &B->pass[pass];
    packets++;
    for (int k = 0; k < 6; k++) {
        const uint32_t* r = (const uint32_t*)(pkt + (1 + k) * 16);
        uint64_t v = (uint64_t)r[0] | (uint64_t)r[1] << 32, want;
        switch (r[2] & 0xFF) {
        case 0x06: want = P->tex0; break;
        case 0x14: want = P->tex1; break;
        case 0x08: want = P->clamp; break;
        case 0x47: want = P->test; break;
        case 0x42: want = P->alpha; break;
        case 0x3D: want = P->fogcol; break;
        default: want = ~v; break;
        }
        if (v != want) {
            packet_bad++;
            if (verbose && packet_bad < 40) printf("  packet reg %02X %016llX want %016llX (mesh %08X batch %u pass %d) program %ld\n", r[2] & 0xFF,
                                                   (unsigned long long)v, (unsigned long long)want, bb->m->addr, bb->b, pass, cur_prog);
            return;
        }
    }
}

/* ---- (r2-m3) the lit routines (0x2EF8 / 0x2F80 / 0x2FC8): every recorded program's sub-batches decoded from its input
 * memory (r2_lit_decode_vu), the light block (r2_lights_prep from the raw light set for 0x2EF8, else the block in
 * memory), the bone palette from memory; after the run, the VU's own results in the buffer (skinned positions, colour A
 * / colour B, 0x0298's light block) against r2_lit_eval / r2_lights_prep. ---------------------------------------------- */
static uint64_t lit_progs, lit_dec_fail[R2M_N], lit_vtx, lit_pos_bad, lit_cola_bad[3], lit_colb_bad[3], lit_lb_bad, lit_lb_n,
    lit_skin_vtx, lit_lit_vtx;
static const uint32_t (*lit_mem)[4];
static uint32_t lit_x;
static const uint32_t* lit_hdr(uint32_t i) { return lit_mem[(lit_x + i) & 0x3FF]; }
static int cdiff(uint32_t a, uint32_t b) {
    int m = 0;
    for (int c = 0; c < 4; c++) { int d = (int)((a >> (8 * c)) & 255) - (int)((b >> (8 * c)) & 255); d = d < 0 ? -d : d; if (d > m) m = d; }
    return m;
}
static void lit_check(uint32_t ra, uint32_t top, const uint32_t* in, const vu_rec_head* h) {
    static uint32_t img[1024][4];
    memcpy(img, in, sizeof img);
    r2_mesh* m;
    int r = r2_lit_decode_vu((const uint32_t(*)[4])img, top, ra, &m);
    lit_progs++;
    if (r) {
        lit_dec_fail[r]++;
        extern int r2_fail_line;
        static int shown;
        uint32_t Xf = (0x24B - top) & 0x3FF;
        const uint32_t *q12 = img[(Xf + 12) & 0x3FF], *q15 = img[(Xf + 15) & 0x3FF], *q19 = img[(Xf + 19) & 0x3FF], *q16 = img[(Xf + 16) & 0x3FF];
        if (verbose && shown++ < 30) printf("  lit decode %s (line %d) prog %ld ra %04X flags %X [12] %X %X %X %X [15] %X %X %X %X [16] %X %X %X %X [19] %X %X %X %X\n",
                                            r2m_reason[r], r == R2M_VERTEX ? r2_fail_line : 0, cur_prog, ra, img[(Xf + 11) & 0x3FF][0],
                                            q12[0], q12[1], q12[2], q12[3], q15[0], q15[1], q15[2], q15[3], q16[0], q16[1], q16[2], q16[3], q19[0], q19[1], q19[2], q19[3]);
        return;
    }
    uint32_t X = (0x24B - top) & 0x3FF;
    /* the image as the routine sees it (0x2F80 copies the header first) */
    if (ra == 0x2F80) {
        for (uint32_t i = 0; i < 10; i++) memcpy(img[(X + i) & 0x3FF], img[(top + i) & 0x3FF], 16);
        uint32_t e = img[(X + 11) & 0x3FF][1] & 0x3FF;
        for (uint32_t i = 0x15; i < e && i < 0x200; i++) memcpy(img[(X + i) & 0x3FF], img[(top + i) & 0x3FF], 16);
    }
    float lb[R2_LIGHT_QW][4], vu10[4];
    memcpy(vu10, img[10], 16);
    lit_mem = (const uint32_t(*)[4])img;
    lit_x = X;
    int need_prep = ra == 0x2EF8 && (img[(X + 11) & 0x3FF][0] & 2);
    if (need_prep) {
        if (!r2_lights_prep(lit_hdr, m, vu10, lb)) { lit_dec_fail[R2M_HEADER]++; r2_mesh_free(m); return; }
    } else {
        for (int i = 0; i < 10; i++) memcpy(lb[i], img[(X + 21 + (uint32_t)i) & 0x3FF], 16);
        memcpy(lb[10], vu10, 16);
    }
    float bones[R2_BONES_MAX * 4][4];
    for (uint32_t i = 0; i < R2_BONES_MAX * 4; i++) memcpy(bones[i], img[(X + R2_HQ_LO + i) & 0x3FF], 16);
    /* run */
    vu_state* st = &vu_st[1];
    memcpy(vu_data_mem(1), in, h->data_size);
    memcpy(st->vf, h->vf, sizeof st->vf);
    st->acc = h->acc;
    memcpy(st->vi, h->vi, sizeof st->vi);
    st->I = h->I; st->Q = h->Q; st->P = h->P; st->R = h->R;
    st->status = h->status; st->mac = h->mac; st->clip = h->clip;
    st->top = h->top; st->itop = h->itop;
    nk = 0;
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(VU_MXCSR);
    vu_run(st, h->addr);
    _mm_setcsr(csr);
    const uint32_t(*out)[4] = (const uint32_t(*)[4])vu_data_mem(1);
    if (need_prep) {
        for (int i = 0; i < 10; i++) {
            if (i == 3) continue;                      /* ([24]: the game's transpose leaves it unused) */
            lit_lb_n++;
            if (memcmp(lb[i], out[(X + 21 + (uint32_t)i) & 0x3FF], 16)) {
                lit_lb_bad++;
                if (verbose && lit_lb_bad < 6) {
                    const float* o = (const float*)out[(X + 21 + (uint32_t)i) & 0x3FF];
                    printf("  light block [%d]: %g %g %g %g want %g %g %g %g\n", 21 + i, lb[i][0], lb[i][1], lb[i][2], lb[i][3], o[0], o[1], o[2], o[3]);
                }
            }
        }
    }
    for (uint32_t b = 0; b < m->nb; b++) {
        const r2_batch* B = &m->b[b];
        uint32_t start = img[(X + (b ? 17 : 13)) & 0x3FF][0] & 0x3FF;
        for (uint32_t k = 0; k < B->vcount; k++) {
            const r2_vtx* v = &m->vtx[B->vfirst + k];
            float pos[3];
            uint32_t col[2];
            r2_lit_eval(v, B, (const float(*)[4])lb, (const float(*)[4])bones, pos, col);
            uint32_t a = (X + start + 4 * k) & 0x3FF;
            lit_vtx++;
            if (B->skin) {
                lit_skin_vtx++;
                if (memcmp(pos, out[(a + 3) & 0x3FF], 12)) {
                    lit_pos_bad++;
                    if (verbose && lit_pos_bad < 6) {
                        const float* o = (const float*)out[(a + 3) & 0x3FF];
                        printf("  pos %.7g %.7g %.7g want %.7g %.7g %.7g (prog %ld batch %u v %u skin %d)\n", pos[0], pos[1], pos[2], o[0], o[1], o[2], cur_prog, b, k, B->skin);
                    }
                }
            }
            if (B->lit) {
                lit_lit_vtx++;
                const uint32_t *oa = out[(a + 1) & 0x3FF], *ob = out[(a + 2) & 0x3FF];
                uint32_t wa = (oa[0] & 255) | (oa[1] & 255) << 8 | (oa[2] & 255) << 16 | (oa[3] & 255) << 24;
                uint32_t wb = (ob[0] & 255) | (ob[1] & 255) << 8 | (ob[2] & 255) << 16 | (ob[3] & 255) << 24;
                int da = cdiff(col[0], wa), db = cdiff(col[1], wb);
                lit_cola_bad[da == 0 ? 0 : da == 1 ? 1 : 2]++;
                lit_colb_bad[db == 0 ? 0 : db == 1 ? 1 : 2]++;
                if (verbose && (da > 1 || db > 1)) {
                    static int shown;
                    if (shown++ < 8) printf("  colour A %08X want %08X, B %08X want %08X (prog %ld batch %u v %u nrm %08X col %08X)\n", col[0], wa,
                                            col[1], wb, cur_prog, b, k, v->nrm, v->lcol);
                }
            }
        }
    }
    r2_mesh_free(m);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: r2_mesh_check <vu recording> <trace dir> [frame] [--verbose]\n"); return 2; }
    int fn = 0;
    for (int i = 3; i < argc; i++) { if (!strcmp(argv[i], "--verbose")) verbose = 1; else fn = atoi(argv[i]); }
    char path[512];
    snprintf(path, sizeof path, "%s\\ram_%d.bin", argv[2], fn);
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    uint8_t* ram = malloc(0x2000000);
    if (fread(ram, 1, 0x2000000, f) != 0x2000000) return 2;
    fclose(f);
    snprintf(path, sizeof path, "%s\\frame_%d.txt", argv[2], fn);
    f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    char line[1024];
    int entries = 0, decoded = 0, reasons[R2M_N] = { 0 };
    uint64_t verts = 0, tris = 0, batches = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        unsigned idx, e, prio, key, c1, c2;
        float kf;
        int n = sscanf(line, "%u %x %u %x %g %x %x", &idx, &e, &prio, &key, &kf, &c1, &c2);
        if (n != 7) continue;
        char* p = line;
        int cols = 0;
        for (char* t = strtok(p, " \n"); t; t = strtok(NULL, " \n")) cols++;
        if (cols != 7) continue;                                  /* allocator entries carry the submitter */
        entries++;
        r2_mesh* m;
        int r = r2_mesh_decode(ram, c2 ? c2 : c1, c2 ? 1 : 0, &m);
        reasons[r]++;
        if (r) { if (verbose) printf("entry %u prio %u c1 %08X c2 %08X: %s\n", idx, prio, c1, c2, r2m_reason[r]); continue; }
        decoded++;
        verts += m->nvtx;
        tris += m->nidx / 3;
        batches += m->nb;
        for (uint32_t b = 0; b < m->nb; b++) {
            bref br = { m, b };
            put(key_mesh(&m->vtx[m->b[b].vfirst], m->b[b].vcount), br);
        }
    }
    fclose(f);
    printf("frame %d: %d static-world entries, %d decoded (%llu batches, %llu vertices, %llu triangles); fallbacks:", fn, entries,
           decoded, (unsigned long long)batches, (unsigned long long)verts, (unsigned long long)tris);
    for (int r = 1; r < R2M_N; r++) if (reasons[r]) printf(" %s %d", r2m_reason[r], reasons[r]);
    printf("\n");

    /* ---- replay ---- */
    f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc((size_t)size);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) return 2;
    fclose(f);
    vu_set_vif_tops(1, 0, 0);
    const uint8_t* last_micro = NULL;
    const unsigned csr = _mm_getcsr();
    for (long pos = 0; pos + (long)sizeof(vu_rec_head) <= size;) {
        const vu_rec_head* h = (const vu_rec_head*)(buf + pos);
        if (memcmp(h->magic, "VUP1", 4)) break;
        pos += sizeof *h;
        const uint8_t* micro = NULL;
        if (h->micro_size) { micro = buf + pos; pos += h->micro_size; if (h->idx == 1) last_micro = micro; }
        const uint8_t* data = buf + pos;
        pos += h->data_size;
        pos += sizeof(vu_rec_tail);
        cur_prog++;
        if (h->idx != 1 || !last_micro) continue;
        uint32_t ra = h->addr * 8u;
        vu_state* st = &vu_st[1];
        memcpy(vu_micro_mem(1), last_micro, (st->pcmask + 1) * 8u);
        memcpy(vu_data_mem(1), data, h->data_size);
        memcpy(st->vf, h->vf, sizeof st->vf);
        st->acc = h->acc;
        memcpy(st->vi, h->vi, sizeof st->vi);
        st->I = h->I; st->Q = h->Q; st->P = h->P; st->R = h->R;
        st->status = h->status; st->mac = h->mac; st->clip = h->clip;
        st->top = h->top; st->itop = h->itop;
        int prelit = ra == 0x2E20 || ra == 0x2E68 || ra == 0x2EB0 || ra == 0x2EE8;
        if (ra == 0x2EF8 || ra == 0x2F80 || ra == 0x2FC8) {
            lit_check(ra, h->top, (const uint32_t*)data, h);
            memcpy(vu_data_mem(1), data, h->data_size);    /* (the replay below runs it again from the same state) */
        }
        /* identify the batches this program sets up, from its input memory */
        const uint32_t* mem = (const uint32_t*)data;
        r2_frame_block fb;
        memcpy(fb.c0, mem + 0 * 4, 16);
        memcpy(fb.c1, mem + 1 * 4, 16);
        memcpy(fb.clip, mem + 2 * 4, 64);
        if (!prelit && ra != 0x0000 && ra != 0x3AB0) {         /* another routine (lit meshes ...) takes over a buffer */
            if (ra == 0x3358) memset(&BB[bufidx(h->top)][1], 0, sizeof(bufbatch));
            else memset(BB[bufidx((0x24B - h->top) & 0x3FF)], 0, 2 * sizeof(bufbatch));
        }
        if (prelit) {
            programs_seen++;
            uint32_t X = ra == 0x2EE8 ? h->top : (0x24B - h->top) & 0x3FF;
            int sub = ra == 0x2EE8;
            const uint32_t* d = mem + (X + (sub ? 17 : 13)) * 4;
            uint32_t start = d[0] & 0x3FF, n = d[3] & 0x7FFF;
            bref* br = start + 4 * n <= 0x3FF ? get(key_vu(mem + (X + start) * 4, n)) : NULL;
            bufbatch* bb = &BB[bufidx(X)][sub];
            memset(bb, 0, sizeof *bb);
            if (!sub) memset(&BB[bufidx(X)][1], 0, sizeof *bb);    /* a new header in X: its old sub-batch 2 is gone */
            if (br && verbose && br->m->addr == 0x015AE890 && br->b == 0) {
                const uint32_t* c = mem + (X + start + 1) * 4;
                printf("  input colour A of v0: %X %X %X %X, decoded %08X; prog %04X top %03X X %03X\n", c[0], c[1], c[2], c[3],
                       br->m->vtx[br->m->b[0].vfirst].col[0], ra, h->top, X);
            }
            if (br) {
                batches_seen++;
                bb->m = br->m; bb->b = br->b; bb->valid = 1;
                /* the header of the buffer (2E68 copies it from the other one first) */
                uint32_t H = ra == 0x2E68 ? h->top : X;
                float w[16];
                for (int i = 0; i < 16; i++) memcpy(&w[i], mem + (H + (uint32_t)(i >> 2)) * 4 + (i & 3), 4);
                memcpy(&bb->fade, mem + (H + 8) * 4, 4);
                if (ra != 0x2E20 && ra != 0x2E68) {              /* 2EB0 / 2EE8: M already in 4..7 of the buffer */
                    for (int i = 0; i < 16; i++) memcpy(&bb->mat[i], mem + (X + 4 + (uint32_t)(i >> 2)) * 4 + (i & 3), 4);
                } else if (ra == 0x2E68) {
                    for (int i = 0; i < 16; i++) memcpy(&bb->mat[i], mem + (H + 4 + (uint32_t)(i >> 2)) * 4 + (i & 3), 4);
                } else {
                    r2_matmul(w, fb.clip, bb->mat);
                }
            }
        }
        if (getenv("R2_DEBUG_PROG") && cur_prog >= atol(getenv("R2_DEBUG_PROG")) - 3 && cur_prog <= atol(getenv("R2_DEBUG_PROG"))) {
            printf("program %ld: %04X top %03X\n", cur_prog, ra, h->top);
            for (int bi = 0; bi < 2; bi++) {
                uint32_t Xb = bi ? 0x219 : 0x032;
                const uint32_t* q = mem + Xb * 4;
                printf("  buf %03X: [8] %08X %08X %08X %08X [10] %X %X %X [11] %X %X [13] %X %X %X %X [14] %X %X %X [17] %X %X %X %X [18] %X %X %X  TEX0@15 %08X%08X @1E %08X%08X\n",
                       Xb, q[32], q[33], q[34], q[35], q[40], q[41], q[42], q[44], q[45], q[52], q[53], q[54], q[55], q[56], q[57], q[58],
                       q[68], q[69], q[70], q[71], q[72], q[73], q[74], q[(0x15 + 3) * 4 + 1], q[(0x15 + 3) * 4], q[(0x1E + 3) * 4 + 1], q[(0x1E + 3) * 4]);
                for (int s = 0; s < 2; s++) if (BB[bi][s].valid) printf("    BB[%d][%d] mesh %08X batch %u\n", bi, s, BB[bi][s].m->addr, BB[bi][s].b);
            }
        }
        nk = 0;
        _mm_setcsr(VU_MXCSR);
        vu_run(st, h->addr);
        _mm_setcsr(csr);
        if (!prelit && ra != 0x3AB0) continue;
        /* attribute the kicks: material packets (A+D tag) and vertex packets (tristrip) by their VU address */
        for (int k = 0; k < nk; k++) {
            uint32_t a = K[k].addr;
            const uint64_t* tq = (const uint64_t*)K[k].pkt;
            uint32_t nreg = (uint32_t)(tq[0] >> 60), regs = (uint32_t)(tq[1] & 0xFFFF);
            int found = 0;
            for (int bi = 0; bi < 2 && !found; bi++)
                for (int s = 0; s < 2 && !found; s++) {
                    bufbatch* bb = &BB[bi][s];
                    if (!bb->valid) continue;
                    uint32_t Xb = bi ? 0x219 : 0x032;
                    const r2_batch* B = &bb->m->b[bb->b];
                    (void)B;
                    /* the batch's vertex buffer start: its descriptor's input address, read from the current memory */
                    const uint32_t* dm = (const uint32_t*)vu_data_mem(1) + (Xb + (s ? 17 : 13)) * 4;
                    const uint32_t* pk = (const uint32_t*)vu_data_mem(1) + (Xb + (s ? 18 : 14)) * 4;
                    if (nreg == 4 && a == ((Xb + (dm[0] & 0x3FF) - 1) & 0x3FF)) {
                        int pass = regs == 0x4F12 ? 0 : regs == 0x41F2 ? 1 : -1;
                        /* which packet: A uses colour A (pass[0] normally); match by REGS */
                        int pi = -1;
                        for (int q = 0; q < bb->m->b[bb->b].npass; q++) if (bb->m->b[bb->b].pass[q].csel == (pass == 1)) { pi = q; break; }
                        if (pi < 0) kicks_unknown++;
                        else check_vertices(bb, pi, K[k].pkt, K[k].qw, &fb);
                        found = 1;
                    } else if (nreg == 1 && (regs & 0xF) == 0xE) {
                        for (int q = 0; q < 3; q++)
                            if (pk[q] && a == ((Xb + (pk[q] & 0x3FF) + 2) & 0x3FF)) { check_material(bb, q, K[k].pkt, K[k].qw); found = 1; break; }
                    }
                }
            if (!found) {
                if (a == 13) kicks_clip++;
                else kicks_unknown++;
            }
        }
    }
    printf("lit routines: %llu programs (decode failures:", (unsigned long long)lit_progs);
    for (int r = 1; r < R2M_N; r++) if (lit_dec_fail[r]) printf(" %s %llu", r2m_reason[r], (unsigned long long)lit_dec_fail[r]);
    printf("); light blocks: %llu qwords, %llu differ; vertices %llu (%llu skinned: positions differing %llu; %llu lit: colour A "
           "exact / +-1 / more %llu %llu %llu, colour B %llu %llu %llu)\n", (unsigned long long)lit_lb_n, (unsigned long long)lit_lb_bad,
           (unsigned long long)lit_vtx, (unsigned long long)lit_skin_vtx, (unsigned long long)lit_pos_bad, (unsigned long long)lit_lit_vtx,
           (unsigned long long)lit_cola_bad[0], (unsigned long long)lit_cola_bad[1], (unsigned long long)lit_cola_bad[2],
           (unsigned long long)lit_colb_bad[0], (unsigned long long)lit_colb_bad[1], (unsigned long long)lit_colb_bad[2]);
    printf("replay: %llu prelit programs, %llu with a decoded batch; kicks: %llu vertex packets checked, %llu clipper, %llu unattributed\n",
           (unsigned long long)programs_seen, (unsigned long long)batches_seen, (unsigned long long)kicks_vertex,
           (unsigned long long)kicks_clip, (unsigned long long)kicks_unknown);
    printf("vertices %llu (%llu of triangles the VU draws: the histograms): XY |d| 0/1/2/<=4/<=16/>16: %llu %llu %llu %llu %llu %llu (max %llu)\n",
           (unsigned long long)n_vtx, (unsigned long long)n_used,
           (unsigned long long)xy_hist[0], (unsigned long long)xy_hist[1], (unsigned long long)xy_hist[2], (unsigned long long)xy_hist[3],
           (unsigned long long)xy_hist[4], (unsigned long long)xy_hist[5], (unsigned long long)xy_max);
    printf("  Z |d| 0/1/2/<=4/<=16/>16: %llu %llu %llu %llu %llu %llu (max %llu)\n", (unsigned long long)z_hist[0],
           (unsigned long long)z_hist[1], (unsigned long long)z_hist[2], (unsigned long long)z_hist[3], (unsigned long long)z_hist[4],
           (unsigned long long)z_hist[5], (unsigned long long)z_max);
    printf("  fog off by > 1: %llu; STQ rel > 2^-16: %llu (max %.3g); RGBA mismatches: %llu\n", (unsigned long long)f_bad,
           (unsigned long long)stq_bad, stq_max, (unsigned long long)rgba_bad);
    printf("  linear screen model (GPU vertex shader) vs the VU: max |dX| %.3f, max |dZ| %.3f (frames without a model %llu)\n", model_ex, model_ez, (unsigned long long)model_bad);
    printf("  material packets %llu, mismatching %llu\n", (unsigned long long)packets, (unsigned long long)packet_bad);
    printf("triangles: VU draws %llu; drawn by the VU but ADC in R2's input %llu; culled by the VU: backface %llu (R2 winding area > 0: %llu, < 0: %llu), "
           "off screen %llu, clipped %llu; drawn (backface test on) with R2 winding area > 0: %llu, < 0: %llu; drawn (test off) %llu\n",
           (unsigned long long)tri_drawn, (unsigned long long)adc_extra, (unsigned long long)culled_back,
           (unsigned long long)area_pos_culled, (unsigned long long)area_neg_culled, (unsigned long long)culled_off,
           (unsigned long long)culled_clip, (unsigned long long)area_pos_drawn, (unsigned long long)area_neg_drawn, (unsigned long long)nocull_drawn);
    return 0;
}
