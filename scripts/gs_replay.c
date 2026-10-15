/* Render a GS recording (runtime/gs_record.c, PS2_GS_RECORD) with the GS code it is linked with,
 * writing every VBlank's picture as <outdir>/frame_<vblank>.png. Built and run by
 * scripts/gs_replay.ps1 against any checkout's runtime/, so two versions of the renderer can be
 * compared on exactly the same input (scripts/compare_frames.py).
 *
 *   gs_replay <recording> <outdir> [workers] [--backend sw|hw|both] [--adapter hw|warp] [--metrics file]
 *
 * workers > 0: through the GS thread with that many pixel workers (deferred drawing), else
 * synchronously. outdir "-": no pictures (benchmark).
 *
 * Backends (notes/d3d11-renderer.md 10.1; built with GS_REPLAY_GPU when runtime/gpu exists):
 *   sw    the software renderer's pictures (default; the only mode without runtime/gpu)
 *   hw    the GPU backend's pictures, read back from the GPU (M0: the all-fallback backend, i.e. the
 *         sw picture uploaded, composed and presented by the render thread)
 *   both  both, compared per VBlank; differing frames are written as frame_N_sw/_hw/_diff.png (at
 *         most 20) and the exit code is 3 if any pixel differs
 * Every mode prints "sw-hash": a 64-bit FNV-1a over the size and RGB of every sw picture in
 * order, so renderer changes that must not change the output can be checked without PNGs.
 * --metrics writes per-VBlank JSON: differing pixel counts per channel above |d| 0/2/16, PSNR,
 * bbox, D3D calls, front/render/GPU ms. Uses only the public GS entry points, so it builds with
 * older GS code too. */
#include "gs_draw.h"
#if __has_include("gs_setup.h")
#include "gs_setup.h"                  /* gs_sw_draw (--decode-only) */
#define GS_REPLAY_SETUP 1
#endif
#include <x86intrin.h>

#include <stdlib.h>
#include <windows.h>

#ifdef GS_REPLAY_GPU
#include "gpu/gpu.h"
#include "gpu/gpu_front.h"
#include "gpu/gpu_verify.h"
#include <stddef.h>
#endif

void debug_write_png(const char* path, const uint32_t* bgrx, int w, int h, int stride);   /* debug.c */

/* neighbours of the GS code in the game */
void intc_raise(int cause) { (void)cause; }
void dmac_raise(int ch) { (void)ch; }
void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    (void)unit; (void)phys; (void)data; (void)size; (void)is_write;
}

enum { B_SW, B_HW, B_BOTH, B_HWDRY, B_HWSUB };
#define FRONT_MODE(b) ((b) == B_HWDRY || (b) == B_HWSUB)        /* the GPU frontend consumes the stream */
#define VERIFY_MODE(b) ((b) == B_HW || (b) == B_BOTH)            /* hw pictures compared with sw */
static const char* outdir;
static int backend = B_SW;
static uint64_t sw_hash = 1469598103934665603ull;
static uint32_t pictures;
static double other_ms_per_vblank;
static double gs_tsc_per_ms = 1e6;       /* TSC ticks per ms (QueryThreadCycleTime units), measured at start */       /* hwsubmit: CPU of threads other than replay + render (driver) */
static volatile long progress, stage;    /* watchdog: stage 0 replay, 1 GS sync, 2 verify flush, 3 GPU finish */

static void hash_picture(const uint32_t* frame, int ok, int w, int h, int stride) {
    uint64_t hsh = sw_hash;
#define MIX(v) do { hsh ^= (uint64_t)(v); hsh *= 1099511628211ull; } while (0)
    if (!ok) { MIX(0xDEADu); sw_hash = hsh; return; }
    MIX((uint32_t)w); MIX((uint32_t)h);
    for (int y = 0; y < h; y++) {
        const uint32_t* r = frame + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x++) MIX(r[x] & 0xFFFFFF);
    }
#undef MIX
    sw_hash = hsh;
}

static void save_png(const char* suffix, const uint32_t* frame, int w, int h, int stride, uint32_t vblank) {
    if (!strcmp(outdir, "-")) return;     /* "-": no pictures (benchmark) */
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%06u%s.png", outdir, vblank, suffix);
    debug_write_png(path, frame, w, h, stride);
}

#ifdef GS_REPLAY_GPU
/* ---- hw / both: per-VBlank results ---- */
typedef struct { gpu_pic_diff d; } frame_rec;
static frame_rec* recs;
static size_t nrecs, caprecs;
static gpu_frame_stats* sts;                     /* per picture, in the same order as recs */
static size_t nsts, capsts;
static uint32_t diff_frames, png_diffs;
static uint64_t diff_pixels;
static double min_psnr = 999.0;

static void take_stats(void) {
    gpu_frame_stats st[256];
    int n;
    while ((n = gpu_take_frame_stats(st, 256)) > 0)
        for (int i = 0; i < n; i++) {
            if (nsts == capsts) {
                capsts = capsts ? capsts * 2 : 4096;
                sts = realloc(sts, capsts * sizeof *sts);
                if (!sts) { fprintf(stderr, "out of memory\n"); exit(1); }
            }
            sts[nsts++] = st[i];
        }
}

static void diff_png(const gpu_pic_diff* d, const uint32_t* sw, const uint32_t* hw) {
    static uint32_t heat[1024 * 1024];
    for (int y = 0; y < d->h; y++)
        for (int x = 0; x < d->w; x++) {
            uint32_t a = sw[y * d->w + x], b = hw[y * d->w + x];
            int m = 0;
            for (int s = 0; s < 24; s += 8) {
                int e = abs((int)((a >> s) & 0xFF) - (int)((b >> s) & 0xFF));
                if (e > m) m = e;
            }
            uint32_t g = (a >> 8 & 0xFF) / 4;          /* dim sw picture underneath */
            heat[y * d->w + x] = m ? 0xFF0000u | (uint32_t)(m * 8 > 255 ? 255 : m * 8) << 8 : g << 16 | g << 8 | g;
        }
    save_png("_diff", heat, d->w, d->h, d->w, d->vblank);
}

static void verify_cb(const gpu_pic_diff* d, const uint32_t* sw, const uint32_t* hw, void* user) {
    (void)user;
    if (nrecs == caprecs) {
        caprecs = caprecs ? caprecs * 2 : 4096;
        recs = realloc(recs, caprecs * sizeof *recs);
        if (!recs) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    progress++;
    memset(&recs[nrecs], 0, sizeof recs[nrecs]);
    recs[nrecs++].d = *d;
    if (d->diff_px) {
        diff_frames++;
        diff_pixels += d->diff_px;
        if (diff_frames <= 5)
            fprintf(stderr, "vblank %u: %u pixels differ (bbox %d,%d-%d,%d, PSNR %.2f%s%s)\n", d->vblank, d->diff_px,
                    d->bx0, d->by0, d->bx1, d->by1, d->psnr, d->size_mismatch ? ", size mismatch" : "",
                    d->readback_error ? ", read-back error" : "");
    }
    if (d->psnr < min_psnr) min_psnr = d->psnr;
    if (backend == B_HW && hw) save_png("", hw, d->w, d->h, d->w, d->vblank);
    if (backend == B_BOTH && d->diff_px && hw && png_diffs < 20) {
        png_diffs++;
        save_png("_sw", sw, d->w, d->h, d->w, d->vblank);
        save_png("_hw", hw, d->w, d->h, d->w, d->vblank);
        diff_png(d, sw, hw);
    }
    if ((nrecs & 63) == 0) take_stats();
}

static int cmp_dbl(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static double median(double* v, size_t n) {
    if (!n) return -1;
    qsort(v, n, sizeof *v, cmp_dbl);
    return n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

static void write_metrics(const char* path, const char* rec, const char* adapter, double replay_ms) {
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    const gpu_caps* c = gpu_get_caps();
    const char* json_rec = rec;
    fprintf(f, "{\n  \"recording\": \"");
    for (const char* p = json_rec; *p; p++) fprintf(f, *p == '\\' ? "\\\\" : *p == '"' ? "\\\"" : "%c", *p);
    fprintf(f, "\",\n  \"backend\": \"%s\",\n  \"adapter\": \"%s\",\n", backend == B_HW ? "hw" : "both", adapter);
    fprintf(f, "  \"caps\": {\"name\": \"%s\", \"feature_level\": \"%x\", \"rov\": %d, \"typed_uav_load\": %d, "
               "\"dual_source\": %d, \"cb_offsets\": %d, \"full\": %d},\n",
            c->adapter, c->feature_level, c->rovs, c->typed_uav_load, c->dual_source, c->cb_offsets, c->full);
    double* rm = malloc((nrecs + 1) * sizeof *rm);
    double* gm = malloc((nrecs + 1) * sizeof *gm);
    size_t nr = 0, ng = 0;
    fprintf(f, "  \"frames\": [\n");
    for (size_t i = 0; i < nrecs; i++) {
        const gpu_pic_diff* d = &recs[i].d;
        const gpu_frame_stats* s = i < nsts ? &sts[i] : NULL;
        fprintf(f, "    {\"vblank\": %u, \"w\": %d, \"h\": %d, \"diff_px\": %u, \"r\": [%u, %u, %u], \"g\": [%u, %u, %u], "
                   "\"b\": [%u, %u, %u], \"psnr\": %.3f, \"bbox\": [%d, %d, %d, %d]",
                d->vblank, d->w, d->h, d->diff_px, d->diff[0][0], d->diff[0][1], d->diff[0][2], d->diff[1][0],
                d->diff[1][1], d->diff[1][2], d->diff[2][0], d->diff[2][1], d->diff[2][2], d->psnr, d->bx0, d->by0,
                d->bx1, d->by1);
        if (d->size_mismatch || d->readback_error)
            fprintf(f, ", \"hw_w\": %d, \"hw_h\": %d, \"readback_error\": %d", d->hw_w, d->hw_h, d->readback_error);
        if (s) {
            fprintf(f, ", \"draws\": %u, \"dispatches\": %u, \"copies\": %u, \"uploads\": %u, \"upload_bytes\": %u, "
                       "\"front_ms\": %.4f, \"render_ms\": %.4f, \"gpu_ms\": %.4f",
                    s->draws, s->dispatches, s->copies, s->uploads, s->upload_bytes, s->front_ms, s->render_ms, s->gpu_ms);
            rm[nr++] = s->render_ms;
            if (s->gpu_ms >= 0) gm[ng++] = s->gpu_ms;
        }
        fprintf(f, "}%s\n", i + 1 < nrecs ? "," : "");
    }
    fprintf(f, "  ],\n");
    double rmed = median(rm, nr), gmed = median(gm, ng);
    fprintf(f, "  \"summary\": {\"frames\": %zu, \"diff_frames\": %u, \"diff_px\": %llu, \"min_psnr\": %.3f, "
               "\"sw_hash\": \"%016llx\", \"replay_ms\": %.1f, \"render_ms_median\": %.4f, \"gpu_ms_median\": %.4f}\n}\n",
            nrecs, diff_frames, (unsigned long long)diff_pixels, min_psnr, (unsigned long long)sw_hash, replay_ms, rmed, gmed);
    free(rm);
    free(gm);
    fclose(f);
}

/* ---- hwdry: frontend counters per VBlank (notes/d3d11-renderer.md M1) ---- */
static gpu_front_frame* ff;
static size_t nff, capff;

static void take_front(void) {
    gpu_front_frame b[64];
    int n;
    while ((n = gpu_front_take_frames(b, 64)) > 0)
        for (int i = 0; i < n; i++) {
            if (nff == capff) {
                capff = capff ? capff * 2 : 1024;
                ff = realloc(ff, capff * sizeof *ff);
                if (!ff) { fprintf(stderr, "out of memory\n"); exit(1); }
            }
            ff[nff++] = b[i];
        }
}

#define U32F(name) { #name, offsetof(gpu_front_frame, name), 0 }
#define U64F(name) { #name, offsetof(gpu_front_frame, name), 1 }
#define ARRF(name, n) { #name, offsetof(gpu_front_frame, name), 2 + (n) }
static const struct { const char* name; size_t off; int kind; } front_fields[] = {
    U32F(kicks), U32F(nowrite), U32F(culled), U32F(skipped), ARRF(tier, GPU_TIER_N), ARRF(cls, GPU_PC_N), U32F(linepix),
    ARRF(rov_why, GPU_ROV_N), ARRF(punt_why, GPU_PUNT_N),
    U32F(d3d_draws), U32F(d3d_draws_commute), U32F(ff_draws), U32F(rov_draws), U32F(split_extra), U32F(rov_runs), U32F(zresolves),
    U32F(alias16_sprites), U32F(alias16_dispatches), U32F(alias16_selfcopy), U32F(clears_c), U32F(clears_z),
    U32F(loads), U32F(load_blocks), U32F(shadow_copies), U32F(downloads), U32F(download_blocks), U32F(patches),
    U32F(patch_bytes), U32F(xfers), U32F(xfer_bytes), U32F(xfer_local), U32F(xfer_read), U32F(clut_loads),
    U32F(clut_slots), U32F(clut_new), U32F(clut_gpu_reads), U32F(targets_live), U32F(targets_created), U32F(targets_evicted), U32F(target_overlaps),
    U32F(state_changes), U32F(states), U32F(vertices), U32F(indices), U32F(records_z), U32F(records_full),
    U32F(records_sprite), U32F(records_linepix), U64F(vtx_bytes), U64F(idx_bytes), U64F(rec_bytes), U64F(state_bytes),
    U64F(cmd_bytes), U64F(data_bytes), U32F(atst_kicks), U32F(atst_pass), U32F(atst_fail), U32F(atst_straddle),
    U32F(cd_as_kicks), U32F(as_over128), ARRF(zprims, GPU_ZF_N), ARRF(zmin, GPU_ZF_N), ARRF(zmax, GPU_ZF_N),
    U32F(z32_over), U32F(tex_vram), U32F(tex_view_c32), U32F(tex_view_alpha), U32F(tex_alias16), U32F(region_clamp),
    U32F(tbw_npot), U32F(selfcheck_bad),
};

static void json_front(FILE* f, const gpu_front_frame* g) {
    fprintf(f, "\"vblank\": %u, \"front_ms\": %.4f, \"thread_ms\": %.4f", g->vblank, g->front_ms, g->thread_ms);
    for (size_t k = 0; k < sizeof front_fields / sizeof *front_fields; k++) {
        const uint8_t* p = (const uint8_t*)g + front_fields[k].off;
        if (front_fields[k].kind == 0) fprintf(f, ", \"%s\": %u", front_fields[k].name, *(const uint32_t*)p);
        else if (front_fields[k].kind == 1) fprintf(f, ", \"%s\": %llu", front_fields[k].name, (unsigned long long)*(const uint64_t*)p);
        else {
            fprintf(f, ", \"%s\": [", front_fields[k].name);
            for (int i = 0; i < front_fields[k].kind - 2; i++) fprintf(f, "%s%u", i ? ", " : "", ((const uint32_t*)p)[i]);
            fprintf(f, "]");
        }
    }
}

/* summary: sums over all VBlanks and maxima per VBlank, plus the worst VBlanks */
static void front_report(const char* metrics, const char* rec, double replay_ms, uint32_t frames) {
    gpu_front_frame sum, mx;
    memset(&sum, 0, sizeof sum);
    memset(&mx, 0, sizeof mx);
    double fms_sum = 0, fms_max = 0;
    uint32_t zlo[GPU_ZF_N], zhi[GPU_ZF_N];
    for (int i = 0; i < GPU_ZF_N; i++) { zlo[i] = 0xFFFFFFFFu; zhi[i] = 0; }
    for (size_t i = 0; i < nff; i++) {
        const gpu_front_frame* g = &ff[i];
        fms_sum += g->front_ms;
        if (g->front_ms > fms_max) fms_max = g->front_ms;
        for (size_t k = 0; k < sizeof front_fields / sizeof *front_fields; k++) {
            size_t off = front_fields[k].off;
            int n = front_fields[k].kind >= 2 ? front_fields[k].kind - 2 : 1;
            if (front_fields[k].kind == 1) {
                uint64_t v = *(const uint64_t*)((const uint8_t*)g + off);
                *(uint64_t*)((uint8_t*)&sum + off) += v;
                if (v > *(uint64_t*)((uint8_t*)&mx + off)) *(uint64_t*)((uint8_t*)&mx + off) = v;
            } else
                for (int j = 0; j < n; j++) {
                    uint32_t v = ((const uint32_t*)((const uint8_t*)g + off))[j];
                    ((uint32_t*)((uint8_t*)&sum + off))[j] += v;
                    if (v > ((uint32_t*)((uint8_t*)&mx + off))[j]) ((uint32_t*)((uint8_t*)&mx + off))[j] = v;
                }
        }
        for (int z = 0; z < GPU_ZF_N; z++)
            if (g->zprims[z]) { if (g->zmin[z] < zlo[z]) zlo[z] = g->zmin[z]; if (g->zmax[z] > zhi[z]) zhi[z] = g->zmax[z]; }
    }
    double n = nff ? (double)nff : 1;
    fprintf(stderr, "hwdry: %zu VBlanks (%u replayed), frontend %.3f ms/VBlank mean, %.3f max\n", nff, frames, fms_sum / n, fms_max);
    fprintf(stderr, "hwdry: tiers FF %u ROV %u ALIAS16 %u PUNT %u | classes tri %u sprite %u line %u point %u, %u line pixels\n",
            sum.tier[0], sum.tier[1], sum.tier[2], sum.tier[3], sum.cls[0], sum.cls[1], sum.cls[2], sum.cls[3], sum.linepix);
    fprintf(stderr, "hwdry: per VBlank max: D3D draws %u (FF %u, ROV %u, split +%u, zresolves %u), ROV runs %u, ALIAS16 %u sprites / %u dispatches, "
                    "states %u, state changes %u, records %u, vertices %u, kicks %u\n",
            mx.d3d_draws, mx.ff_draws, mx.rov_draws, mx.split_extra, mx.zresolves, mx.rov_runs, mx.alias16_sprites,
            mx.alias16_dispatches, mx.states, mx.state_changes,
            mx.records_z + mx.records_full + mx.records_sprite + mx.records_linepix, mx.vertices, mx.kicks);
    fprintf(stderr, "hwdry: commutation batching estimate: max %u D3D draws per VBlank, mean %.1f\n", mx.d3d_draws_commute, sum.d3d_draws_commute / n);
    fprintf(stderr, "hwdry: per VBlank mean: D3D draws %.1f, ROV runs %.2f, ALIAS16 sprites %.1f / dispatches %.2f, states %.1f, "
                    "upload MB vtx %.2f idx %.2f rec %.2f\n",
            sum.d3d_draws / n, sum.rov_runs / n, sum.alias16_sprites / n, sum.alias16_dispatches / n, sum.states / n,
            sum.vtx_bytes / n / 1e6, sum.idx_bytes / n / 1e6, sum.rec_bytes / n / 1e6);
    fprintf(stderr, "hwdry: ROV why: ref %u colclamp %u fbmsk %u date %u blend %u C>128 %u pabe/dthe %u z32-high %u straddle %u\n",
            sum.rov_why[0], sum.rov_why[1], sum.rov_why[2], sum.rov_why[3], sum.rov_why[4], sum.rov_why[5], sum.rov_why[6],
            sum.rov_why[7], sum.rov_why[8]);
    fprintf(stderr, "hwdry: PUNT %u (target overlap %u, frame fmt %u, tex foreign %u, tex fmt %u, alias16 prim %u, alias16 tex %u)\n",
            sum.tier[3], sum.punt_why[0], sum.punt_why[1], sum.punt_why[2], sum.punt_why[3], sum.punt_why[4], sum.punt_why[5]);
    fprintf(stderr, "hwdry: pass A (ATST + AFAIL != KEEP) %u kicks: all-pass %u all-fail %u straddle %u (%.2f%%); lerp blends with C=As %u, As > 128 %u (%.2f%%)\n",
            sum.atst_kicks, sum.atst_pass, sum.atst_fail, sum.atst_straddle,
            sum.atst_kicks ? 100.0 * sum.atst_straddle / sum.atst_kicks : 0.0, sum.cd_as_kicks, sum.as_over128,
            sum.cd_as_kicks ? 100.0 * sum.as_over128 / sum.cd_as_kicks : 0.0);
    static const char* zn[GPU_ZF_N] = { "Z32", "Z24", "Z16", "Z16S" };
    for (int z = 0; z < GPU_ZF_N; z++)
        if (sum.zprims[z]) fprintf(stderr, "hwdry: %s: %u prims, vertex Z %08X..%08X\n", zn[z], sum.zprims[z], zlo[z], zhi[z]);
    fprintf(stderr, "hwdry: Z32 vertices above 0x3F000000: %u; clears colour %u Z %u; loads %u (%u blocks), shadow copies %u, "
                    "downloads %u (%u blocks), alias16 self-copies %u\n",
            sum.z32_over, sum.clears_c, sum.clears_z, sum.loads, sum.load_blocks, sum.shadow_copies, sum.downloads,
            sum.download_blocks, sum.alias16_selfcopy);
    fprintf(stderr, "hwdry: uploads %u (%u bytes) -> %u patches (%u bytes), max %u patches/VBlank; TRXDIR=1 %u, TRXDIR=2 %u; "
                    "CLUT new slots %u, max distinct/VBlank %u, loads over GPU words %u; targets created %u evicted %u max live %u, same-kind overlaps %u\n",
            sum.xfers, sum.xfer_bytes, sum.patches, sum.patch_bytes, mx.patches, sum.xfer_read, sum.xfer_local, sum.clut_new,
            mx.clut_slots, sum.clut_gpu_reads, sum.targets_created, sum.targets_evicted, mx.targets_live, sum.target_overlaps);
    fprintf(stderr, "hwdry: textures: vram %u, CT32 target views %u, alpha-index views %u, CT16 (ALIAS16) %u; REGION_CLAMP %u, "
                    "TBW not 2^n %u; selfcheck mismatches %u\n",
            sum.tex_vram, sum.tex_view_c32, sum.tex_view_alpha, sum.tex_alias16, sum.region_clamp, sum.tbw_npot, sum.selfcheck_bad);
    /* the three VBlanks with the most predicted D3D draws */
    size_t picked[3];
    for (int w = 0; w < 3 && w < (int)nff; w++) {
        size_t best = 0;
        int have = 0;
        for (size_t i = 0; i < nff; i++) {
            int seen = 0;
            for (int j = 0; j < w; j++) seen |= picked[j] == i;
            if (!seen && (!have || ff[i].d3d_draws > ff[best].d3d_draws)) { best = i; have = 1; }
        }
        picked[w] = best;
        const gpu_front_frame* g = &ff[best];
        fprintf(stderr, "hwdry: worst VBlank %u: %u D3D draws, %u ROV runs, %u states, %u records, %u kicks, %.3f ms\n",
                g->vblank, g->d3d_draws, g->rov_runs, g->states,
                g->records_z + g->records_full + g->records_sprite + g->records_linepix, g->kicks, g->front_ms);
    }
    gpu_front_dump_targets(stderr);
    if (!metrics) return;
    FILE* f = fopen(metrics, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", metrics); return; }
    fprintf(f, "{\n  \"recording\": \"");
    for (const char* p = rec; *p; p++) fprintf(f, *p == '\\' ? "\\\\" : *p == '"' ? "\\\"" : "%c", *p);
    fprintf(f, "\",\n  \"backend\": \"hwdry\",\n  \"replay_ms\": %.1f,\n  \"frames\": [\n", replay_ms);
    for (size_t i = 0; i < nff; i++) {
        fprintf(f, "    {");
        json_front(f, &ff[i]);
        fprintf(f, "}%s\n", i + 1 < nff ? "," : "");
    }
    fprintf(f, "  ],\n  \"sum\": {");
    json_front(f, &sum);
    fprintf(f, "},\n  \"max\": {");
    json_front(f, &mx);
    fprintf(f, "},\n  \"front_ms_mean\": %.4f, \"front_ms_max\": %.4f\n}\n", fms_sum / n, fms_max);
    fclose(f);
}

/* --stream-hash (hwdry): a hash of every frame's command stream and data, so frontend optimisations
 * that must not change its output can be checked */
static uint64_t stream_hash = 1469598103934665603ull;
static void hash_bytes_into(const void* p, size_t n) {
    const uint8_t* b = p;
    uint64_t h = stream_hash;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    h ^= n; h *= 1099511628211ull;
    stream_hash = h;
}
static void stream_sink(const gpu_front_frame_data* fd, void* user) {
    (void)user;
    hash_bytes_into(fd->cmd, fd->cmd_bytes); hash_bytes_into(fd->vtx, fd->vtx_bytes); hash_bytes_into(fd->idx, fd->idx_count * 4);
    hash_bytes_into(fd->rec, fd->rec_bytes); hash_bytes_into(fd->states, fd->state_count * 96); hash_bytes_into(fd->data, fd->data_bytes);
}

/* ---- hwsubmit (M2): the render thread executed every frame; its per-frame costs ---- */
static double pct(double* v, size_t n, double p) {
    if (!n) return 0;
    qsort(v, n, sizeof *v, cmp_dbl);
    size_t i = (size_t)(p * (double)(n - 1) + 0.5);
    return v[i < n ? i : n - 1];
}

/* per VBlank: CPU of the replay thread (the GS thread: decode, plus the frontend or its proxy) */
static double* gsms;
static size_t ngsms, capgsms;

/* GS-side CPU per game frame (VBlank with >= 1000 primitives): the replay thread and, with
 * --front-thread, the frontend thread; steady = without replay-start frames (targets created, downloads) */
static void thread_report(int front_thread) {
    size_t n = nff < ngsms ? nff : ngsms;
    double *a = malloc((n + 1) * sizeof *a), *b = malloc((n + 1) * sizeof *b);
    double amax = 0, bmax = 0, amax_st = 0, bmax_st = 0;
    size_t ng = 0;
    uint32_t wa = 0, wb = 0;
    for (size_t i = 1; i < n; i++) {                /* (the first VBlank holds the start-up) */
        const gpu_front_frame* q = &ff[i];
        if (q->kicks < 1000) continue;
        double x = gsms[i], y = front_thread ? q->thread_ms : 0;
        a[ng] = x; b[ng++] = y;
        if (x > amax) amax = x;
        if (y > bmax) bmax = y;
        if (q->targets_created || q->downloads) continue;
        if (x > amax_st) { amax_st = x; wa = q->vblank; }
        if (y > bmax_st) { bmax_st = y; wb = q->vblank; }
    }
    double amed = pct(a, ng, 0.5), bmed = pct(b, ng, 0.5), a95 = pct(a, ng, 0.95), b95 = pct(b, ng, 0.95);
    fprintf(stderr, "threads: per game frame (%zu): GS thread median %.3f ms, p95 %.3f, max %.3f (steady %.3f at vblank %u)",
            ng, amed, a95, amax, amax_st, wa);
    if (front_thread) fprintf(stderr, "; frontend thread median %.3f, p95 %.3f, max %.3f (steady %.3f at vblank %u)", bmed, b95, bmax, bmax_st, wb);
    fprintf(stderr, "\n");
    fprintf(stderr, "thread-bench game=%zu gs_med=%.4f gs_p95=%.4f gs_max=%.4f gs_max_steady=%.4f ft_med=%.4f ft_p95=%.4f ft_max=%.4f ft_max_steady=%.4f\n",
            ng, amed, a95, amax, amax_st, bmed, b95, bmax, bmax_st);
    free(a); free(b);
}

static int submit_report(const char* metrics, const char* rec, double replay_ms, uint32_t frames) {
    (void)frames;
    size_t n = nsts < nff ? nsts : nff;
    if (nsts != nff) fprintf(stderr, "hwsubmit: %zu render frames vs %zu frontend frames\n", nsts, nff);
    double *r = malloc((n + 1) * sizeof *r), *c = malloc((n + 1) * sizeof *c), *u = malloc((n + 1) * sizeof *u);
    double *g = malloc((n + 1) * sizeof *g), *fr = malloc((n + 1) * sizeof *fr), *sb = malloc((n + 1) * sizeof *sb);
    double sum_sub = 0, sum_cpu = 0, sum_draws = 0, sum_calls = 0;
    size_t ng = 0, ngpu = 0, worst = 0, nstart = 0;
    double rmax_st = 0, cmax_st = 0, gmax_st = 0;
    uint32_t worst_st = 0;
    uint32_t draws_max = 0, calls_max = 0, pred_max = 0;
    for (size_t i = 0; i < n; i++) {
        const gpu_frame_stats* s = &sts[i];
        const gpu_front_frame* q = &ff[i];
        if (s->vblank != q->vblank) { fprintf(stderr, "hwsubmit: frame order mismatch at %zu (%u vs %u)\n", i, s->vblank, q->vblank); return 1; }
        if (s->render_ms > sts[worst].render_ms) worst = i;
        if (s->draws > draws_max) draws_max = s->draws;
        if (s->state_calls > calls_max) calls_max = s->state_calls;
        if (q->d3d_draws > pred_max) pred_max = q->d3d_draws;
        if (q->kicks < 1000) continue;    /* game frames (as gpu_front_bench.ps1) */
        r[ng] = s->render_ms; c[ng] = s->render_cpu_ms; u[ng] = s->upload_ms; fr[ng] = q->front_ms;
        sb[ng] = s->render_ms - s->upload_ms;
        if (s->gpu_ms >= 0) g[ngpu++] = s->gpu_ms;
        sum_sub += s->render_ms - s->upload_ms;
        sum_cpu += s->render_cpu_ms;
        sum_draws += s->draws;
        sum_calls += s->draws + s->dispatches + s->copies + s->uploads + s->state_calls;
        ng++;
        /* steady state: not a replay-start frame (targets created, downloads: the first frames) */
        if (q->targets_created || q->downloads) { nstart++; continue; }
        if (s->render_ms > rmax_st) { rmax_st = s->render_ms; worst_st = s->vblank; }
        if (s->render_cpu_ms > cmax_st) cmax_st = s->render_cpu_ms;
        if (s->gpu_ms > gmax_st) gmax_st = s->gpu_ms;
    }
    double us_draw = sum_draws ? sum_sub * 1000.0 / sum_draws : 0;
    double us_call = sum_calls ? sum_sub * 1000.0 / sum_calls : 0;
    double rmed = pct(r, ng, 0.5), rmax = pct(r, ng, 1.0), r95 = pct(r, ng, 0.95);
    double cmed = pct(c, ng, 0.5), cmax = pct(c, ng, 1.0), umed = pct(u, ng, 0.5), umax = pct(u, ng, 1.0);
    double smed = pct(sb, ng, 0.5), smax = pct(sb, ng, 1.0);
    double gmed = pct(g, ngpu, 0.5), gmax = pct(g, ngpu, 1.0), fmed = pct(fr, ng, 0.5), fmax = pct(fr, ng, 1.0);
    const gpu_frame_stats* w = n ? &sts[worst] : NULL;
    fprintf(stderr, "hwsubmit: %zu frames (%zu game frames); render thread per game frame: wall median %.3f ms, p95 %.3f, max %.3f; "
                    "CPU median %.3f, max %.3f; upload median %.3f, max %.3f; submit (wall - upload) median %.3f, max %.3f\n",
            n, ng, rmed, r95, rmax, cmed, cmax, umed, umax, smed, smax);
    fprintf(stderr, "hwsubmit: D3D draws max %u (frontend predicted %u), state calls max %u; %.3f us per draw, %.3f us per D3D call "
                    "(submit wall / count, game frames); GPU median %.3f ms, max %.3f; frontend median %.3f ms, max %.3f\n",
            draws_max, pred_max, calls_max, us_draw, us_call, gmed, gmax, fmed, fmax);
    if (w)
        fprintf(stderr, "hwsubmit: worst frame vblank %u: %.3f ms wall (%.3f CPU, %.3f upload), %u draws, %u dispatches, %u copies, "
                        "%u uploads (%u bytes), %u state calls, GPU %.3f ms\n",
                w->vblank, w->render_ms, w->render_cpu_ms, w->upload_ms, w->draws, w->dispatches, w->copies, w->uploads,
                w->upload_bytes, w->state_calls, w->gpu_ms);
    /* one machine-readable line for scripts/gpu_submit_bench.ps1 */
    fprintf(stderr, "submit-bench frames=%zu game=%zu render_med=%.4f render_p95=%.4f render_max=%.4f cpu_med=%.4f cpu_max=%.4f "
                    "upload_med=%.4f upload_max=%.4f submit_med=%.4f submit_max=%.4f us_draw=%.4f us_call=%.4f draws_max=%u pred_max=%u "
                    "gpu_med=%.4f gpu_max=%.4f front_med=%.4f front_max=%.4f other_ms=%.4f other_game_ms=%.4f render_max_steady=%.4f cpu_max_steady=%.4f gpu_max_steady=%.4f start_frames=%zu worst_steady_vblank=%u\n",
            n, ng, rmed, r95, rmax, cmed, cmax, umed, umax, smed, smax, us_draw, us_call, draws_max, pred_max, gmed, gmax, fmed, fmax,
            other_ms_per_vblank, ng ? other_ms_per_vblank * (double)n / (double)ng : 0.0, rmax_st, cmax_st, gmax_st, nstart, worst_st);
    fprintf(stderr, "hwsubmit: steady state (game frames without target creation or downloads, %zu start frames left out): worst %.3f ms wall at vblank %u, CPU max %.3f, GPU max %.3f\n",
            nstart, rmax_st, worst_st, cmax_st, gmax_st);
    if (metrics) {
        FILE* f = fopen(metrics, "w");
        if (f) {
            fprintf(f, "{\n  \"recording\": \"");
            for (const char* p = rec; *p; p++) fprintf(f, *p == '\\' ? "\\\\" : *p == '"' ? "\\\"" : "%c", *p);
            fprintf(f, "\",\n  \"backend\": \"hwsubmit\",\n  \"replay_ms\": %.1f,\n  \"frames\": [\n", replay_ms);
            for (size_t i = 0; i < n; i++) {
                const gpu_frame_stats* s = &sts[i];
                fprintf(f, "    {\"vblank\": %u, \"render_ms\": %.4f, \"render_cpu_ms\": %.4f, \"upload_ms\": %.4f, \"gpu_ms\": %.4f, "
                           "\"draws\": %u, \"dispatches\": %u, \"copies\": %u, \"uploads\": %u, \"upload_bytes\": %u, \"state_calls\": %u, ",
                        s->vblank, s->render_ms, s->render_cpu_ms, s->upload_ms, s->gpu_ms, s->draws, s->dispatches, s->copies,
                        s->uploads, s->upload_bytes, s->state_calls);
                json_front(f, &ff[i]);
                fprintf(f, "}%s\n", i + 1 < n ? "," : "");
            }
            fprintf(f, "  ]\n}\n");
            fclose(f);
        }
    }
    free(r); free(c); free(u); free(g); free(fr); free(sb);
    return 0;
}
#endif

/* the picture of a VBlank (on the GS thread's pixel worker with workers, else on this thread) */
static void picture(const uint32_t* frame, int ok, int w, int h, uint32_t vblank) {
    pictures++;
    progress++;
    hash_picture(frame, ok, w, h, 1024);
#ifdef GS_REPLAY_GPU
    if (VERIFY_MODE(backend)) { gpu_verify_picture(frame, ok, w, h, 1024, vblank); return; }
#endif
    if (ok) save_png("", frame, w, h, 1024, vblank);
}

static int rd(FILE* f, void* p, size_t n) { return fread(p, 1, n, f) == n; }

/* --vram-hash: a 64-bit hash of all of gs_vram after every VBlank (synchronous replay only), one
 * line "vblank hash" per VBlank, so refactors that must not change the software renderer can be
 * checked on every byte of GS memory, not only on the displayed pictures. */
static FILE* vram_hash_file;
static uint64_t vram_hash_all = 1469598103934665603ull;

static void hash_vram(uint32_t vblank) {
    const uint64_t* w = (const uint64_t*)gs_vram;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < GS_VRAM_SIZE / 8; i += 4) {
        h = (h ^ w[i]) * 1099511628211ull;
        h = (h ^ w[i + 1]) * 1099511628211ull;
        h = (h ^ w[i + 2]) * 1099511628211ull;
        h = (h ^ w[i + 3]) * 1099511628211ull;
    }
    fprintf(vram_hash_file, "%u %016llx\n", vblank, (unsigned long long)h);
    vram_hash_all = (vram_hash_all ^ h) * 1099511628211ull;
}

/* --profile: a sampling profiler of the replay thread (the GS thread's work: decode, frontend or sw
 * rasterizer). Every 0.25 ms the replay thread is suspended, the function it is in (self) and the
 * functions on its stack (inclusive, x64 unwind) are counted by the executable's COFF symbols (the
 * method of runtime/perf.c's PS2_PROFILE). Printed at the end. */
typedef struct { uint32_t rva; const char* name; } psym;
static psym* psyms;
static int npsyms;
static uint8_t* pimage;
static uint64_t pimage_size;
static uint32_t *pself, *pincl, psamples, poutside;
static HANDLE ptarget;
static volatile LONG pstop;

static int psym_cmp(const void* a, const void* b) {
    uint32_t x = ((const psym*)a)->rva, y = ((const psym*)b)->rva;
    return x < y ? -1 : x > y;
}

static void prof_symbols(void) {
    pimage = (uint8_t*)GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS64* mnt = (const IMAGE_NT_HEADERS64*)(pimage + ((const IMAGE_DOS_HEADER*)pimage)->e_lfanew);
    pimage_size = mnt->OptionalHeader.SizeOfImage;
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) return;
    FILE* f = _wfopen(path, L"rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) { fclose(f); return; }
    fclose(f);
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(buf + ((const IMAGE_DOS_HEADER*)buf)->e_lfanew);
    const IMAGE_FILE_HEADER* fh = &nt->FileHeader;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    if (!fh->PointerToSymbolTable) { fprintf(stderr, "[profile] no COFF symbols\n"); return; }
    const uint8_t* tab = buf + fh->PointerToSymbolTable;
    const char* strtab = (const char*)tab + fh->NumberOfSymbols * 18ull;
    psyms = malloc(sizeof(psym) * fh->NumberOfSymbols);
    for (uint32_t i = 0; i < fh->NumberOfSymbols; i++) {
        const IMAGE_SYMBOL* s = (const IMAGE_SYMBOL*)(tab + 18ull * i);
        int sn = s->SectionNumber;
        if (sn > 0 && sn <= fh->NumberOfSections && (sec[sn - 1].Characteristics & IMAGE_SCN_CNT_CODE) &&
            (s->StorageClass == IMAGE_SYM_CLASS_EXTERNAL || s->StorageClass == IMAGE_SYM_CLASS_STATIC)) {
            const char* name;
            if (s->N.Name.Short) { char* n = calloc(1, 9); memcpy(n, s->N.ShortName, 8); name = n; }
            else name = strtab + s->N.Name.Long;
            if (name[0] != '.') psyms[npsyms++] = (psym){ sec[sn - 1].VirtualAddress + s->Value, name };
        }
        i += s->NumberOfAuxSymbols;
    }
    qsort(psyms, (size_t)npsyms, sizeof(psym), psym_cmp);
    pself = calloc((size_t)npsyms + 1, 4);
    pincl = calloc((size_t)npsyms + 1, 4);
}

static int psym_of(uint64_t pc) {
    if (pc < (uint64_t)pimage || pc >= (uint64_t)pimage + pimage_size) return -1;
    uint32_t rva = (uint32_t)(pc - (uint64_t)pimage);
    int lo = 0, hi = npsyms;
    while (lo < hi) { int mid = (lo + hi) / 2; if (psyms[mid].rva <= rva) lo = mid + 1; else hi = mid; }
    return lo - 1;
}

static DWORD WINAPI prof_main(LPVOID arg) {
    (void)arg;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x2, TIMER_ALL_ACCESS);
    while (!pstop) {
        LARGE_INTEGER due;
        due.QuadPart = -2500;
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
        else Sleep(1);
        CONTEXT c;
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_FULL;
        if (SuspendThread(ptarget) == (DWORD)-1) continue;
        if (!GetThreadContext(ptarget, &c)) { ResumeThread(ptarget); continue; }
        MEMORY_BASIC_INFORMATION mbi;
        uint64_t lo = c.Rsp, hi = 0;
        if (VirtualQuery((LPCVOID)(uintptr_t)c.Rsp, &mbi, sizeof mbi)) hi = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        int seen[32], nseen = 0;
        for (int depth = 0; depth < 32 && c.Rip && c.Rsp >= lo && c.Rsp + 8 <= hi; depth++) {
            int s = psym_of(c.Rip);
            if (depth == 0) { if (s >= 0) pself[s]++; else poutside++; }
            if (s >= 0) {
                int dup = 0;
                for (int k = 0; k < nseen; k++) dup |= seen[k] == s;
                if (!dup) { seen[nseen++] = s; pincl[s]++; }
            }
            DWORD64 base = 0, prev = c.Rsp;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, NULL);
            if (!fe) { c.Rip = *(const DWORD64*)(uintptr_t)c.Rsp; c.Rsp += 8; }
            else { PVOID hd; DWORD64 fr; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &hd, &fr, NULL); }
            if (c.Rsp <= prev) break;
        }
        ResumeThread(ptarget);
        psamples++;
    }
    if (timer) CloseHandle(timer);
    return 0;
}

typedef struct { const char* name; uint32_t n; } phit;
static int phit_cmp(const void* a, const void* b) { uint32_t x = ((const phit*)a)->n, y = ((const phit*)b)->n; return x > y ? -1 : x < y; }

static void prof_report(void) {
    if (!psamples) return;
    phit* h = calloc((size_t)npsyms + 2, sizeof *h);
    for (int pass = 0; pass < 2; pass++) {
        int nh = 0;
        for (int s = 0; s < npsyms; s++) { uint32_t v = pass ? pincl[s] : pself[s]; if (v) h[nh++] = (phit){ psyms[s].name, v }; }
        if (!pass && poutside) h[nh++] = (phit){ "[outside the executable]", poutside };
        qsort(h, (size_t)nh, sizeof *h, phit_cmp);
        fprintf(stderr, "[profile] %u samples of the replay thread, %s:\n", psamples, pass ? "inclusive" : "self");
        for (int k = 0; k < nh && k < (pass ? 30 : 40); k++) fprintf(stderr, "  %6.2f%%  %7u  %s\n", 100.0 * h[k].n / psamples, h[k].n, h[k].name);
    }
    free(h);
}

/* --pin (benchmarks): the replay, frontend and render threads each on its own performance core, so that
 * per-thread CPU times do not depend on landing on an efficiency core or sharing a core (hybrid CPUs) */
static uint64_t pcore_mask[64];
static int npcores;

static void find_pcores(void) {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    uint8_t* buf = malloc(len);
    if (!buf || !GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) { free(buf); return; }
    int best = 0;
    for (int pass = 0; pass < 2; pass++)
        for (DWORD off = 0; off < len;) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
            int ec = p->Processor.EfficiencyClass;
            if (!pass && ec > best) best = ec;
            if (pass && ec == best && npcores < 64 && p->Processor.GroupMask[0].Group == 0) {
                uint64_t m = p->Processor.GroupMask[0].Mask;
                pcore_mask[npcores++] = m & (~m + 1);    /* first logical processor of the core */
            }
            off += p->Size;
        }
    free(buf);
}

/* Watchdog: a replay that makes no progress for 60 s prints where it is and exits with code 4, so
 * a harness (gpu_diff_all.ps1) never waits forever on a deadlock. */
static DWORD WINAPI watchdog(LPVOID arg) {
    (void)arg;
    long last = -1;
    int still = 0;
    for (;;) {
        Sleep(1000);
        long p = progress;
        if (p != last) { last = p; still = 0; continue; }
        if (++still < 60) continue;
        fprintf(stderr, "WATCHDOG: no progress for 60 s (stage %ld, %u pictures delivered)\n", stage, pictures);
#ifdef GS_REPLAY_GPU
        if (backend != B_SW) {
            char buf[512];
            gpu_debug_state(buf, sizeof buf);
            fprintf(stderr, "WATCHDOG: %zu compared, verify waits for vblank %ld | %s\n", nrecs, gpu_verify_waiting - 1, buf);
        }
#endif
        fflush(stderr);
        ExitProcess(4);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: gs_replay <recording> <outdir> [workers] [--backend sw|hw|both] [--adapter hw|warp] "
                        "[--metrics file]\n");
        return 2;
    }
    outdir = argv[2];
    int workers = 0, decode_only = 0, front_only = 0, front_ref = 0, front_straddle = 0, front_selfcheck = 0, with_sw = 0, scale = 1, pace = 0, profile = 0, shash = 0, front_thread = 0, pin = 0;
    const char* adapter = "hw";
    const char* metrics = NULL;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
            const char* b = argv[++i];
            backend = !strcmp(b, "hw") ? B_HW : !strcmp(b, "both") ? B_BOTH : !strcmp(b, "hwdry") ? B_HWDRY : !strcmp(b, "hwsubmit") ? B_HWSUB : B_SW;
            if (strcmp(b, "sw") && backend == B_SW) { fprintf(stderr, "unknown backend %s\n", b); return 2; }
        } else if (!strcmp(argv[i], "--adapter") && i + 1 < argc) adapter = argv[++i];
        else if (!strcmp(argv[i], "--metrics") && i + 1 < argc) metrics = argv[++i];
        else if (!strcmp(argv[i], "--front-only")) front_only = 1;
        else if (!strcmp(argv[i], "--decode-only")) decode_only = 1;
        else if (!strcmp(argv[i], "--ref")) front_ref = 1;
        else if (!strcmp(argv[i], "--straddle-rov")) front_straddle = 1;
        else if (!strcmp(argv[i], "--selfcheck")) front_selfcheck = 1;
        else if (!strcmp(argv[i], "--with-sw")) with_sw = 1;
        else if (!strcmp(argv[i], "--pace")) pace = 1;
        else if (!strcmp(argv[i], "--profile")) profile = 1;
        else if (!strcmp(argv[i], "--stream-hash")) shash = 1;
        else if (!strcmp(argv[i], "--front-thread")) front_thread = 1;
        else if (!strcmp(argv[i], "--pin")) pin = 1;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vram-hash") && i + 1 < argc) {
            vram_hash_file = fopen(argv[++i], "w");
            if (!vram_hash_file) { fprintf(stderr, "cannot write %s\n", argv[i]); return 1; }
        } else workers = atoi(argv[i]);
    }
    if (vram_hash_file && workers > 0) { fprintf(stderr, "--vram-hash needs a synchronous replay (workers 0)\n"); return 2; }
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
#ifndef GS_REPLAY_GPU
    if (backend != B_SW) { fprintf(stderr, "this runtime has no GPU backend (runtime/gpu): only --backend sw\n"); return 2; }
    (void)adapter; (void)metrics; (void)front_only; (void)front_ref; (void)front_straddle; (void)front_selfcheck; (void)with_sw; (void)scale;
#else
    if (FRONT_MODE(backend) && workers > 0) { fprintf(stderr, "--backend hwdry/hwsubmit replay synchronously (workers 0)\n"); return 2; }
    if (backend != B_SW && backend != B_HWDRY) {
        gpu_config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.adapter = !strcmp(adapter, "warp") ? GPU_ADAPTER_WARP : GPU_ADAPTER_HW;
        cfg.timestamps = 1;
        if (!gpu_start(&cfg)) { fprintf(stderr, "GPU backend unavailable\n"); return 1; }
        gpu_caps_print(gpu_get_caps());
        if (VERIFY_MODE(backend) && !gpu_verify_init(4, verify_cb, NULL)) { fprintf(stderr, "out of memory\n"); return 1; }
    }
#endif
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    setvbuf(f, NULL, _IOFBF, 1 << 22);
    char magic[8];
    static uint8_t vram[GS_VRAM_SIZE];
    static uint16_t clut[512];
    static uint64_t regs[0x64];
    if (!rd(f, magic, 8) || memcmp(magic, "GSREC001", 8) || !rd(f, vram, sizeof vram) || !rd(f, clut, sizeof clut) ||
        !rd(f, regs, sizeof regs)) {
        fprintf(stderr, "%s: not a GS recording\n", argv[1]);
        return 1;
    }
    GS_INIT();

    /* CLUT buffer: load it from a 16x16 PSMCT32 image (CSM1 order: entries 8-15 and 16-23 of every
     * 32 swapped), which fills all 512 halves (entry i: low half i, high half i + 256) */
    {
        uint32_t img[256];
        for (int i = 0; i < 256; i++)
            img[(i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1)] = clut[i] | (uint32_t)clut[i + 256] << 16;
        const uint32_t cbp = 0;
        gs_exec_write_reg(0x50, (uint64_t)cbp << 32 | 1ull << 48 | 0ull << 56);   /* BITBLTBUF: CT32, width 64 */
        gs_exec_write_reg(0x51, 0);                                               /* TRXPOS */
        gs_exec_write_reg(0x52, 16 | 16ull << 32);                                /* TRXREG 16x16 */
        gs_exec_write_reg(0x53, 0);                                               /* TRXDIR host->local */
        gs_exec_image_write(img, sizeof img);
        /* TEX0_1: PSMT8, CPSM CT32, CSM1, CSA 0, CLD 1 */
        gs_exec_write_reg(0x06, 0x13ull << 20 | (uint64_t)cbp << 37 | 1ull << 61);
    }
    /* general registers as last written, without their side effects (drawing, transfers, CLUT
     * loads); TEX2 is already folded into TEX0 */
    for (uint32_t r = 0; r < 0x64; r++) {
        if (r == 0x04 || r == 0x05 || r == 0x0C || r == 0x0D || r == 0x16 || r == 0x17 ||
            r == 0x53 || r == 0x54 || (r >= 0x60 && r <= 0x62))
            continue;
        uint64_t v = regs[r];
        if (r == 0x06 || r == 0x07) v &= ~(7ull << 61);                          /* CLD 0 */
        gs_exec_write_reg(r, v);
    }
    memcpy(gs_vram, vram, sizeof vram);
    gs_tex_flush_all();                          /* nothing decoded from the CLUT upload area stays */
#ifdef GS_REPLAY_GPU
    if (FRONT_MODE(backend)) {                   /* the frontend consumes the stream next to (or instead of) sw */
        gpu_front_config fc;
        memset(&fc, 0, sizeof fc);
        fc.sw = backend == B_HWSUB ? with_sw : !front_only;
        fc.ref = front_ref;
        fc.straddle_rov = front_straddle;
        fc.selfcheck = front_selfcheck;
        fc.thread = front_thread && !fc.sw;    /* (the frontend thread needs the software rasterizer off) */
        if (front_thread && fc.sw) fprintf(stderr, "--front-thread ignored: the software rasterizer is on\n");
        fc.scale = scale > 0 ? scale : 1;
        gpu_front_start(&fc);
        if (backend == B_HWSUB) gpu_front_set_sink(gpu_submit_sink, NULL);   /* frames -> render thread (M2) */
        if (pin) {
            find_pcores();
            if (npcores >= 4) {
                SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)pcore_mask[1]);
                gpu_front_thread_affinity(pcore_mask[2]);
                if (backend == B_HWSUB) gpu_render_thread_affinity(pcore_mask[3]);
                fprintf(stderr, "pinned: replay %llx, frontend %llx, render %llx (%d performance cores)\n", (unsigned long long)pcore_mask[1],
                        (unsigned long long)pcore_mask[2], (unsigned long long)pcore_mask[3], npcores);
            } else fprintf(stderr, "--pin: fewer than 4 performance cores found, not pinned\n");
        }
        else if (shash) gpu_front_set_sink(stream_sink, NULL);
    }
#endif

    if (pin && !FRONT_MODE(backend)) { find_pcores(); if (npcores >= 2) SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)pcore_mask[1]); }
    if (workers > 0) gs_thread_start(workers, picture);
    /* the recording is read into memory first, so the timing below is the GS's alone */
    long start = ftell(f);
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)(ftell(f) - start);
    fseek(f, start, SEEK_SET);
    uint8_t* all = malloc(size + 16);
    if (!all || !rd(f, all, size)) { fprintf(stderr, "cannot read the recording\n"); return 1; }
    fclose(f);
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceFrequency(&qf);
#ifdef GS_REPLAY_SETUP
    if (decode_only) gs_sw_draw = 0;     /* register decode and vertex queue only: the cost every backend pays */
#else
    (void)decode_only;
#endif
    /* benchmarks of the decode / frontend alone: the hw backend never composes a software picture, so
     * gs_display is skipped there (outdir "-") */
    int no_picture = !strcmp(outdir, "-") && !vram_hash_file &&
                     (decode_only || (backend == B_HWDRY && front_only) || (backend == B_HWSUB && !with_sw));
    /* --pace: VBlanks at 59.94 Hz, as in the game (render-thread times without a GPU backlog) */
    HANDLE pace_timer = pace ? CreateWaitableTimerExW(NULL, NULL, 0x2 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS) : NULL;
    {   /* TSC rate (QueryThreadCycleTime counts TSC ticks) */
        LARGE_INTEGER fa, fb, ff_;
        QueryPerformanceFrequency(&ff_);
        QueryPerformanceCounter(&fa);
        uint64_t t_ = __rdtsc();
        do QueryPerformanceCounter(&fb); while (fb.QuadPart - fa.QuadPart < ff_.QuadPart / 50);
        gs_tsc_per_ms = (double)(__rdtsc() - t_) / ((double)(fb.QuadPart - fa.QuadPart) * 1000.0 / (double)ff_.QuadPart);
    }
    HANDLE prof_thread = NULL;
    if (profile) {
        prof_symbols();
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &ptarget,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
        prof_thread = CreateThread(NULL, 0, prof_main, NULL, 0, NULL);
        if (prof_thread) SetThreadPriority(prof_thread, THREAD_PRIORITY_TIME_CRITICAL);
    }
    ULONG64 tc0 = 0, tc1 = 0, rt0 = 0, rt1 = 0;
    FILETIME pc0[4], pc1[4];
    GetProcessTimes(GetCurrentProcess(), &pc0[0], &pc0[1], &pc0[2], &pc0[3]);
#ifdef GS_REPLAY_GPU
    if (backend == B_HWSUB) rt0 = gpu_render_thread_cycles();
#endif
    QueryThreadCycleTime(GetCurrentThread(), &tc0);
    uint64_t r0 = __rdtsc();
    QueryPerformanceCounter(&q0);
    uint32_t frames = 0;
    for (size_t pos = 0; pos + 8 <= size;) {
        uint32_t h[2];
        memcpy(h, all + pos, 8);
        pos += 8;
        if (h[0] == 1) {
            uint64_t v;
            memcpy(&v, all + pos, 8);
            pos += 8;
            gs_write_reg(h[1], v);
        } else if (h[0] == 2) {
            if (pos + h[1] > size) break;
            gs_image_write(all + pos, h[1]);
            pos += (h[1] + 3) & ~3u;
        } else if (h[0] == 3) {
            progress++;
            uint64_t snap[GS_PRIV_SNAPSHOT];
            if (pos + sizeof snap > size) break;
            memcpy(snap, all + pos, sizeof snap);
            pos += sizeof snap;
            for (int i = 0; i < 15; i++) gs_priv_access(0x12000000u + 16u * (uint32_t)i, &snap[i], 8, 1);
#ifdef GS_REPLAY_GPU
            if (FRONT_MODE(backend)) {
                gpu_front_vblank(snap, h[1]);
                if ((frames & 63) == 63) take_front();
                static ULONG64 last_tc;
                ULONG64 tc = 0;
                QueryThreadCycleTime(GetCurrentThread(), &tc);
                if (ngsms == capgsms) { capgsms = capgsms ? capgsms * 2 : 1024; gsms = realloc(gsms, capgsms * sizeof *gsms); }
                gsms[ngsms++] = last_tc ? (double)(tc - last_tc) / gs_tsc_per_ms : 0;
                last_tc = tc;
            }
#endif
            gs_vsync();
            if (pace) {
                LARGE_INTEGER now, due;
                QueryPerformanceCounter(&now);
                double target = (double)(frames + 1) * (1000.0 / 59.94);
                double el = (double)(now.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart;
                if (el < target) {
                    due.QuadPart = -(LONGLONG)((target - el) * 10000.0);
                    if (pace_timer && SetWaitableTimer(pace_timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(pace_timer, INFINITE);
                    else Sleep((DWORD)(target - el));
                }
            }
            if (no_picture) { pictures++; progress++; }
            else if (workers > 0) gs_queue_vsync(h[1]);
            else {
                static uint32_t frame[1024 * 1024];
                int w = 0, hh = 0;
                int ok = gs_display(frame, 1024, &w, &hh);
                picture(frame, ok, w, hh, h[1]);
                if (vram_hash_file) hash_vram(h[1]);
            }
            frames++;
        } else {
            fprintf(stderr, "bad record tag %u\n", h[0]);
            return 1;
        }
    }
    stage = 1;
    progress++;
    if (workers > 0) gs_sync();
#ifdef GS_REPLAY_GPU
    if (FRONT_MODE(backend)) gpu_front_drain();       /* the frontend thread has processed everything */
    if (backend == B_HWSUB) gpu_submit_wait_idle();   /* every frame executed: the render thread's CPU is complete */
#endif
    QueryPerformanceCounter(&q1);
    uint64_t r1 = __rdtsc();
    QueryThreadCycleTime(GetCurrentThread(), &tc1);
#ifdef GS_REPLAY_GPU
    if (backend == B_HWSUB) rt1 = gpu_render_thread_cycles();
#endif
    GetProcessTimes(GetCurrentProcess(), &pc1[0], &pc1[1], &pc1[2], &pc1[3]);
    if (pace_timer) CloseHandle(pace_timer);
    if (prof_thread) { pstop = 1; WaitForSingleObject(prof_thread, INFINITE); prof_report(); }
    double ms = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart;
    {   /* CPU time of this thread (robust against a loaded machine), TSC cycles converted with the TSC rate */
        double tsc_hz = ms > 0 ? (double)(r1 - r0) / (ms / 1000.0) : 1;
        double cpu_ms = (double)(tc1 - tc0) / tsc_hz * 1000.0;
        fprintf(stderr, "replay thread CPU %.1f ms (%.3f ms per VBlank)\n", cpu_ms, frames ? cpu_ms / frames : 0.0);
        if (FRONT_MODE(backend) && front_thread) {
            double ft_ms = (double)gpu_front_thread_cycles() / tsc_hz * 1000.0;
            fprintf(stderr, "frontend thread CPU %.1f ms (%.3f ms per VBlank)\n", ft_ms, frames ? ft_ms / frames : 0.0);
        }
        if (backend == B_HWSUB) {    /* the render thread, and every other thread (D3D driver workers, ...) */
            double rt_ms = (double)(rt1 - rt0) / tsc_hz * 1000.0;
            uint64_t k0 = (uint64_t)pc0[2].dwHighDateTime << 32 | pc0[2].dwLowDateTime, u0 = (uint64_t)pc0[3].dwHighDateTime << 32 | pc0[3].dwLowDateTime;
            uint64_t k1 = (uint64_t)pc1[2].dwHighDateTime << 32 | pc1[2].dwLowDateTime, u1 = (uint64_t)pc1[3].dwHighDateTime << 32 | pc1[3].dwLowDateTime;
            double proc_ms = (double)((k1 - k0) + (u1 - u0)) / 10000.0;
            fprintf(stderr, "render thread CPU %.1f ms (%.3f ms per VBlank); process CPU %.1f ms, other threads (driver) %.3f ms per VBlank\n",
                    rt_ms, frames ? rt_ms / frames : 0.0, proc_ms, frames ? (proc_ms - cpu_ms - rt_ms) / frames : 0.0);
            other_ms_per_vblank = frames ? (proc_ms - cpu_ms - rt_ms) / frames : 0.0;
        }
    }
    fprintf(stderr, "replayed %u VBlanks in %.1f ms (%.2f ms per VBlank, %d pixel workers)\n", frames, ms,
            frames ? ms / frames : 0.0, workers);
    fprintf(stderr, "sw-hash %016llx over %u pictures\n", (unsigned long long)sw_hash, pictures);
    if (vram_hash_file) {
        fprintf(stderr, "vram-hash %016llx over %u VBlanks\n", (unsigned long long)vram_hash_all, frames);
        fclose(vram_hash_file);
    }
    int rc = 0;
#ifdef GS_REPLAY_GPU
    if (backend == B_HWDRY) {
        take_front();
        front_report(metrics, argv[1], ms, frames);
        thread_report(front_thread);
        if (shash) fprintf(stderr, "stream-hash %016llx\n", (unsigned long long)stream_hash);
        gpu_front_stop();
        uint32_t punts = 0;
        for (size_t i = 0; i < nff; i++) punts += ff[i].tier[GPU_TIER_PUNT];
        if (punts) rc = 5;                       /* the M1 gate: no PUNT */
    }
    if (backend == B_HWSUB) {
        stage = 3;
        progress++;
        gpu_submit_wait_idle();
        gpu_finish();
        take_stats();
        take_front();
        rc = submit_report(metrics, argv[1], ms, frames);
        thread_report(front_thread);
        gpu_front_stop();
        gpu_stop();
    }
    if (VERIFY_MODE(backend)) {
        stage = 2;
        progress++;
        gpu_verify_flush();                     /* the GS thread is idle: this thread is the producer now */
        stage = 3;
        progress++;
        gpu_finish();
        take_stats();
        fprintf(stderr, "%s: %zu pictures compared, %u differ (%llu pixels), min PSNR %.2f dB\n",
                backend == B_HW ? "hw" : "both", nrecs, diff_frames, (unsigned long long)diff_pixels, min_psnr);
        if (metrics) write_metrics(metrics, argv[1], adapter, ms);
        gpu_verify_free();
        gpu_stop();
        if (backend == B_BOTH && diff_frames) rc = 3;
    }
#endif
    return rc;
}
