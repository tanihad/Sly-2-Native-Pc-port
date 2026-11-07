/* (ai) Reimagined textures (notes/reimagined.md, notes/ai-textures.md): under Look = Reimagined the Enhanced images of
 * gpu_tex.h are replaced by Real-ESRGAN x4plus images built on the player's own PC, and every image gets a surface map
 * (normal, height, cavity) for the lighting (gpu_tex_normals.h). Included once, at the end of gpu_tex.h.
 *
 *   flow     an Enhanced image is linked (et_link) -> its content hash names a cache file (<user dir>\textures\
 *            reimagined\<model>-v<RA_VER>\<hash>.jpg | .png): if there, it is decoded on the worker thread and uploaded
 *            over the Enhanced image (a few per frame); else the source texels are read back (staging, no stall), the
 *            worker prepares padded tiles (colour under transparent texels, wrap or edge padding by the addressing and
 *            the content, alpha kept apart), the net (ai_up.c) runs time-sliced at the start of frames within a GPU-time
 *            budget, cs_ai_post crops / adds alpha into an upload region, the image is built over the Enhanced one
 *            (as an HD image: mips, coverage) and read back for the worker to write the cache file. Until then the
 *            Enhanced image shows (Faithful's).
 *   budget   GPU time per frame: what the frame leaves of 80 % of the frame interval (from the last frames' GPU time),
 *            between PS2_AI_MS_MIN (0.5) and PS2_AI_MS (4) ms; ms per GFLOP measured with timestamps. Uploads of cached
 *            images: at most RA_LOAD_TEXELS per frame. Nothing runs under Faithful / Retro (bit-identical).
 *   memory   the net's buffers (~240 MB, created on the worker thread when free video memory allows: DXGI budget - use >
 *            the buffers + 512 MB), the upload regions (16 MB), the surface maps (half the colour classes' size in
 *            RGBA8: ~200 MB at the pre-sized classes). Low video memory (use > 97 % of the budget): the net is released
 *            (images built so far stay), the maps are dropped above 99 %.
 *   config   gpu_tex_look (gfx.c: graphics.look = reimagined); PS2_TEX_AI=0|1 wins. PS2_AI_WEIGHTS (the converted
 *            weights: scripts/ai_model.py export; default <exe dir>\models\x4plus.bin, then <user dir>\models\x4plus.bin),
 *            PS2_AI_MS, PS2_AI_MS_MIN, PS2_AI_GRAIN (0), PS2_AI_RESTORE (1: faint lines the net erased), PS2_AI_CLARITY (0.8: mid-band luminance boost),
 *            PS2_AI_PAD (12), PS2_AI_LOG=1, PS2_AI_NET=0 (cache only),
 *            PS2_TEX_NRM=0 (no maps), PS2_TEX_NRM_K (10: slope), PS2_TEX_DUMP=3 (also <hash>_ai.png / _nrm.png). */
#include <dxgi1_4.h>
#include "ai_up.h"
extern double frame_rate_target_hz(void) __attribute__((weak));   /* (runtime/frame_rate.c; 0: unlimited) */

#define RA_VER 3                         /* the pipeline's version: part of the cache folder's name (a change rebuilds) */
#define RA_TILE 128
#define RA_ITEMS 32768                   /* hash -> item (open addressing) */
#define RA_SRCN 16                       /* source read-backs in flight */
#define RA_SAVEN 8                       /* result read-backs in flight (4 MB each) */
#define RA_MAXT 2048                     /* prepared tiles waiting for the net */
#define RA_BMAX 64                       /* tiles per net batch */
#define RA_UP_WORDS (4u << 20)           /* upload regions: 16 MB */
#define RA_LOAD_TEXELS (1u << 20)        /* cached images uploaded per frame */
#define RA_JOBQ 4096
#define RA_REG 64

enum { RI_NONE, RI_WANT, RI_SRC, RI_PREP, RI_TILES, RI_POST, RI_DONE, RI_LOAD, RI_LOADED, RI_EXCL, RI_FAIL };
typedef struct {
    uint64_t hash;
    uint8_t st, lw, lh, flags, ref, cut, opaque, file;
    uint16_t tiles, left;                /* tiles of the texture, not yet through the net */
    int32_t reg;                         /* upload region (index) or -1 */
    uint32_t* rgba;                      /* (worker: a decoded cache file) */
    uint32_t rw, rh;
} ra_item;
typedef struct { int32_t item; uint16_t tx, ty, cw, ch, pw, ph; float* px; } ra_tile;   /* px: r, g, b (0..1), a (0..255) planes */
enum { RJ_INIT = 1, RJ_PREP, RJ_SAVE, RJ_LOAD };
typedef struct { int type; int32_t item; uint32_t* data; uint32_t w, h; uint8_t flags, opaque; } ra_job;

static struct {
    int env, env_on, log, pad, net_env, nrm_env, dump;
    float ms, ms_min, grain, nrm_k, restore, clarity;
    int on, was, failed, init, inited_thread, net_state;   /* net_state: 0 none, 1 creating, 2 ready, -1 unavailable */
    wchar_t dir[600];
    char model[32];
    uint64_t* have; int nhave;           /* cache files (sorted hashes) */
    ra_item* it;
    /* worker */
    CRITICAL_SECTION cs;
    HANDLE ev, th;
    ra_job jq[RA_JOBQ]; uint32_t jh, jt;
    ra_tile tq[RA_MAXT]; uint32_t tn;    /* prepared tiles (FIFO) */
    int32_t loaded[256]; uint32_t nloaded;
    ai_gpu* net; ai_gpu* net_new; char net_err[256];
    double net_mb;
    /* GPU */
    ID3D11Buffer *up, *al, *pj, *pcb, *nok, *res;
    ID3D11ShaderResourceView *up_srv, *al_srv, *pj_srv, *nok_srv;
    ID3D11UnorderedAccessView *up_uav, *nok_uav, *res_uav;
    struct { uint32_t at, n; int32_t item; } reg[RA_REG];
    struct { ID3D11Buffer* st; int32_t item; uint32_t frame; uint32_t w, h; } src[RA_SRCN];
    struct { ID3D11Buffer* st; int32_t item; uint32_t frame, words; } sav[RA_SAVEN];
    /* the running batch */
    int bon, bn; ra_tile bt[RA_BMAX]; uint32_t bframe;
    /* timing */
    ID3D11Query* tq_[4][2]; uint8_t tbusy[4]; double tgf[4]; uint32_t thead;
    double ms_per_gf, last_ms, frame_ms;
    /* statistics */
    uint64_t n_net, n_load, n_saved, n_excl, n_tiles, n_fail, n_low;
    double gf_total, ms_total, worst_step;
    uint32_t step_hist[80];              /* GPU time of the net per frame, 0.25 ms buckets */
    uint32_t frames_busy;
} RA = { 0 };

/* the surface maps (gpu_tex_normals.h) */
static struct {
    int on;
    ID3D11Texture2D* arr[ET_NCLS];
    ID3D11ShaderResourceView* srv[ET_NCLS];
    ID3D11UnorderedAccessView* uav[ET_NCLS][ET_MAXMIP];
    uint32_t cap[ET_NCLS];
    uint64_t bytes;
} ETN = { 0 };

static int ra_cfg(void) {
    if (!RA.env) {
        RA.env = 1;
        const char* e = getenv("PS2_TEX_AI");
        RA.env_on = e && *e ? *e != '0' : -1;
        e = getenv("PS2_AI_LOG"); RA.log = e && *e == '1';
        e = getenv("PS2_AI_NET"); RA.net_env = !(e && *e == '0');
        e = getenv("PS2_TEX_NRM"); RA.nrm_env = !(e && *e == '0');
        RA.pad = (int)et_envf("PS2_AI_PAD", 12.0f);
        if (RA.pad < 4) RA.pad = 4;
        if (RA.pad > 32) RA.pad = 32;
        RA.ms = et_envf("PS2_AI_MS", 4.0f);
        RA.ms_min = et_envf("PS2_AI_MS_MIN", 0.5f);
        if (RA.ms_min > RA.ms) RA.ms_min = RA.ms;
        RA.grain = et_envf("PS2_AI_GRAIN", 0.0f);
        RA.restore = et_envf("PS2_AI_RESTORE", 1.0f);
        RA.clarity = et_envf("PS2_AI_CLARITY", 0.8f);
        RA.nrm_k = et_envf("PS2_TEX_NRM_K", 10.0f);
        RA.ms_per_gf = 1.0 / 6.0;        /* (6 TFLOPS until measured) */
    }
    return RA.env_on >= 0 ? RA.env_on : tcf_look > 0;
}

/* ---- the surface maps ---------------------------------------------------------------------------------------------- */
static int etn_on(void) { return ra_on_now && ETN.on && RA.nrm_env; }
static float etn_k(void) { return RA.nrm_k; }
static int etn_enabled(void) { return etn_on() && et_enabled(); }
static ID3D11ShaderResourceView* etn_srv(int cls) { return cls >= 0 && cls < ET_NCLS && etn_on() ? ETN.srv[cls] : NULL; }
static ID3D11ShaderResourceView* etn_ok_srv(void) { return etn_on() ? RA.nok_srv : NULL; }
static void etn_bind_ps(void) {
    ID3D11ShaderResourceView* s[7] = { ETN.srv[0], ETN.srv[1], ETN.srv[2], ETN.srv[3], ETN.srv[4], ETN.srv[5], RA.nok_srv };
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, ETN_SLOT0, 7, s);
}
static void etn_free(void) {
    for (int c = 0; c < ET_NCLS; c++) {
        for (int m = 0; m < ET_MAXMIP; m++) GPU_RELEASE(ETN.uav[c][m]);
        GPU_RELEASE(ETN.srv[c]);
        GPU_RELEASE(ETN.arr[c]);
        ETN.cap[c] = 0;
    }
    ETN.bytes = 0;
    ETN.on = 0;
}
/* (et_grow) colour class c now has nc slices (oc before): the map class follows, keeping what it holds */
static void etn_grow(int c, uint32_t oc, uint32_t nc) {
    if (!ra_on_now || !RA.nrm_env || !RA.nok) return;
    uint32_t S = 32u << c, nm = (uint32_t)c + 6u;
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = td.Height = S; td.MipLevels = nm; td.ArraySize = nc; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ID3D11Texture2D* t = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &t))) { gpu_log("ai: surface maps %ux%u: out of video memory, none", S, S); etn_free(); RA.nrm_env = 0; return; }
    if (ETN.arr[c] && ETN.cap[c] == oc)
        for (uint32_t j = 0; j < oc; j++)
            for (uint32_t m = 0; m < nm; m++)
                ID3D11DeviceContext_CopySubresourceRegion(gpu_rt.ctx, (ID3D11Resource*)t, m + j * nm, 0, 0, 0, (ID3D11Resource*)ETN.arr[c], m + j * nm, NULL);
    for (int m = 0; m < ET_MAXMIP; m++) GPU_RELEASE(ETN.uav[c][m]);
    GPU_RELEASE(ETN.srv[c]);
    GPU_RELEASE(ETN.arr[c]);
    ETN.arr[c] = t;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Texture2DArray.MipLevels = nm; sd.Texture2DArray.ArraySize = nc;
    int ok = SUCCEEDED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)t, &sd, &ETN.srv[c]));
    for (uint32_t m = 0; m < nm && ok; m++) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
        memset(&ud, 0, sizeof ud);
        ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
        ud.Texture2DArray.MipSlice = m; ud.Texture2DArray.ArraySize = nc;
        ok = SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(gpu_rt.dev, (ID3D11Resource*)t, &ud, &ETN.uav[c][m]));
    }
    if (!ok) { gpu_log("ai: surface map views failed: none"); etn_free(); RA.nrm_env = 0; return; }
    ETN.bytes += (uint64_t)(nc - (ETN.cap[c] == oc ? oc : 0)) * S * S * 4u * 4u / 3u;
    ETN.cap[c] = nc;
    ETN.on = 1;
    if (RA.log) gpu_log("ai: surface maps %ux%u: %u slices (%.0f MB in all)", S, S, nc, ETN.bytes / 1048576.0);
}
/* (et_batch_run, after a class's mips) the maps of jobs a .. a + n - 1 of class cls, every level, from the colour levels in
 * the scratch (u0 is still bound to it, t1 the job table) */
static void etn_run(int cls, uint32_t a, uint32_t n) {
    if (!etn_on() || !ETN.arr[cls] || ETN.cap[cls] < ET.cap[cls]) return;
    ID3D11DeviceContext* c = gpu_rt.ctx;
    uint32_t S = 32u << cls, nm = (uint32_t)cls + 6u;
    ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_etn), NULL, 0);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 6, 1, &RA.nok_uav, NULL);
    for (uint32_t k = 0; k < nm; k++) {
        uint32_t sz = S >> k;
        ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 5, 1, &ETN.uav[cls][k], NULL);
        et_cb_set(k, a);
        ID3D11DeviceContext_Dispatch(c, (sz + 7) / 8, (sz + 7) / 8, n);
    }
    ID3D11UnorderedAccessView* nu[2] = { NULL, NULL };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 5, 2, nu, NULL);
    gpu_rt.n_dispatches += nm;
}

/* PS2_TEX_DUMP=3: level 0 of slot id's surface map as <hash>_nrm.png (r, g normal, b height, a cavity) */
static void et_dump_nrm(int32_t id, uint64_t raw) {
    int cls = id >> 16;
    if (!ETN.arr[cls] || et_dump_seen(raw, 3)) return;
    uint32_t S = 32u << cls, nm = (uint32_t)cls + 6u, slice = (uint32_t)(id & 0xFFFF);
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = td.Height = S; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* t = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &t))) return;
    ID3D11DeviceContext_CopySubresourceRegion(gpu_rt.ctx, (ID3D11Resource*)t, 0, 0, 0, 0, (ID3D11Resource*)ETN.arr[cls], slice * nm, NULL);
    et_dump_add((ID3D11Resource*)t, S, S, 3, raw);
}
/* (PS2_TEX_DUMP=3) level 0 of the AI image now in slot id as <hash>_ai.png */
static void et_dump_ai(int32_t id, uint64_t raw) {
    int cls = id >> 16;
    if (et_dump_seen(raw, 2)) return;
    uint32_t S = 64u << cls, nm = (uint32_t)cls + 7u, slice = (uint32_t)(id & 0xFFFF);
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = td.Height = S; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* t = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &t))) return;
    ID3D11DeviceContext_CopySubresourceRegion(gpu_rt.ctx, (ID3D11Resource*)t, 0, 0, 0, 0, (ID3D11Resource*)ET.arr[cls], slice * nm, NULL);
    et_dump_add((ID3D11Resource*)t, S, S, 2, raw);
}

/* ---- items ----------------------------------------------------------------------------------------------------------- */
static int32_t ra_find(uint64_t h, int add) {
    if (!RA.it) { if (!add) return -1; RA.it = calloc(RA_ITEMS, sizeof *RA.it); if (!RA.it) return -1; }
    uint32_t i0 = (uint32_t)(h >> 32) & (RA_ITEMS - 1);
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t k = (i0 + i) & (RA_ITEMS - 1);
        ra_item* t = &RA.it[k];
        if (t->hash == h) return (int32_t)k;
        if (!t->hash) {
            if (!add) return -1;
            memset(t, 0, sizeof *t);
            t->hash = h; t->reg = -1;
            return (int32_t)k;
        }
    }
    return -1;
}
static int ra_have(uint64_t h) {
    return RA.nhave && bsearch(&h, RA.have, (size_t)RA.nhave, sizeof h, et_hd_cmp) != NULL;
}
static void ra_path(uint64_t h, int png, wchar_t* out, size_t cap) {
    _snwprintf(out, cap, L"%ls\\%016llx.%ls", RA.dir, (unsigned long long)h, png ? L"png" : L"jpg");
    out[cap - 1] = 0;
}

/* ---- the worker thread: weights + net creation, tile preparation, cache files (WIC) ------------------------------------ */
static void ra_push(ra_job j) {
    EnterCriticalSection(&RA.cs);
    if (RA.jh - RA.jt < RA_JOBQ) RA.jq[RA.jh++ % RA_JOBQ] = j;
    else { free(j.data); if (j.item >= 0 && RA.it) RA.it[j.item].st = RI_FAIL; }
    LeaveCriticalSection(&RA.cs);
    SetEvent(RA.ev);
}
static void* ra_cs_of(uint32_t key, void* user);
static void ra_init_net(void) {
    wchar_t wp[700] = L"";
    const char* e = getenv("PS2_AI_WEIGHTS");
    char path[800] = "";
    if (e && *e) snprintf(path, sizeof path, "%s", e);
    else {
        GetModuleFileNameW(NULL, wp, 600);
        wchar_t* sl = wcsrchr(wp, L'\\');
        if (sl) { wcscpy(sl + 1, L"models\\x4plus.bin"); }
        if (GetFileAttributesW(wp) == INVALID_FILE_ATTRIBUTES && settings_user_path)
            settings_user_path(L"models\\x4plus.bin", wp, 600);
        WideCharToMultiByte(CP_UTF8, 0, wp, -1, path, sizeof path, NULL, NULL);
    }
    char err[256] = "";
    ai_weights* w = ai_weights_load(path, err, sizeof err);
    ai_gpu* g = NULL;
    if (w) {
        size_t P = (size_t)(RA_TILE + 2 * RA.pad) * (RA_TILE + 2 * RA.pad);
        g = ai_gpu_create_cs(gpu_rt.dev, gpu_rt.ctx, w, ra_cs_of, NULL, P, err, sizeof err);
        ai_weights_free(w);
    }
    EnterCriticalSection(&RA.cs);
    RA.net_new = g;
    if (!g) snprintf(RA.net_err, sizeof RA.net_err, "%s", err[0] ? err : "no weights");
    RA.net_state = g ? 2 : -1;
    LeaveCriticalSection(&RA.cs);
}

static int ra_tiles_axis(const float* L, int w, int h, int axis) {   /* (ai_textures.py tiles_axis) */
    double seam = 0, inner = 0, edge = 0;
    if (axis == 0) {
        if (w < 4) return 1;
        for (int y = 0; y < h; y++) {
            const float* r = L + (size_t)y * w;
            seam += fabsf(r[0] - r[w - 1]);
            edge += fabsf(r[w - 2] - r[w - 1]) + fabsf(r[0] - r[1]);
            for (int x = 0; x < w - 1; x++) inner += fabsf(r[x] - r[x + 1]);
        }
        seam /= h; edge /= 2.0 * h; inner /= (double)h * (w - 1);
    } else {
        if (h < 4) return 1;
        for (int x = 0; x < w; x++) {
            seam += fabsf(L[x] - L[(size_t)(h - 1) * w + x]);
            edge += fabsf(L[(size_t)(h - 2) * w + x] - L[(size_t)(h - 1) * w + x]) + fabsf(L[x] - L[w + x]);
        }
        for (int y = 0; y < h - 1; y++)
            for (int x = 0; x < w; x++) inner += fabsf(L[(size_t)y * w + x] - L[(size_t)(y + 1) * w + x]);
        seam /= w; edge /= 2.0 * w; inner /= (double)w * (h - 1);
    }
    double m = inner > edge ? inner : edge;
    return seam <= (2.0 * m > 6.0 ? 2.0 * m : 6.0);
}
static int ra_idx(int i, int n, int wrap) { return wrap ? ((i % n) + n) % n : i < 0 ? 0 : i >= n ? n - 1 : i; }

/* the source (GS words) -> tiles queued for the net; or the item excluded */
static void ra_prep(ra_job* j) {
    ra_item* it = &RA.it[j->item];
    int w = (int)j->w, h = (int)j->h, n = w * h;
    const uint32_t* s = j->data;
    float* pl = malloc((size_t)n * 5 * sizeof(float));   /* r, g, b (0..255), alpha (0..255), luminance */
    uint8_t* known = malloc((size_t)n);
    if (!pl || !known) { free(pl); free(known); it->st = RI_FAIL; return; }
    float *R = pl, *G = pl + n, *B = pl + 2 * n, *A = pl + 3 * n, *L = pl + 4 * n;
    int opaque = 1, nmid = 0, nsoft = 0, amin = 255, amax = 0, nk = 0;
    double ms = 0, ms2 = 0;
    for (int i = 0; i < n; i++) {
        uint32_t v = s[i], a = (v >> 24) * 255u / 128u;
        if (a > 255) a = 255;
        R[i] = (float)(v & 255); G[i] = (float)((v >> 8) & 255); B[i] = (float)((v >> 16) & 255); A[i] = (float)a;
        known[i] = a >= 8;
        nk += known[i];
        if (a < 250) opaque = 0;
        if (a > 16 && a < 239) nmid++;
        if (a > 8 && a < 247) { nsoft++; ms += a; ms2 += (double)a * a; }
        if ((int)a < amin) amin = (int)a;
        if ((int)a > amax) amax = (int)a;
    }
    /* soft alpha (glows, smoke, snow flakes, light shafts): the net turns soft blobs into rings - Enhanced keeps them */
    double sd = nsoft ? sqrt(fmax(0.0, ms2 / nsoft - (ms / nsoft) * (ms / nsoft))) : 0.0;
    if (!opaque && nsoft > n / 5 && sd > 10.0) { it->st = RI_EXCL; RA.n_excl++; free(pl); free(known); return; }
    /* small sprites (<= 32 texels, transparent all along the border: snow flakes, bubbles, orbs): drawn blended over the
     * scene, the net's crisp rim reads as a ring - Enhanced keeps them soft */
    if (!opaque && w <= 32 && h <= 32) {
        int edge = 0;
        for (int x = 0; x < w; x++) edge |= A[x] >= 8 || A[(h - 1) * w + x] >= 8;
        for (int y = 0; y < h; y++) edge |= A[y * w] >= 8 || A[y * w + w - 1] >= 8;
        if (!edge) { it->st = RI_EXCL; RA.n_excl++; free(pl); free(known); return; }
    }
    it->opaque = (uint8_t)opaque;
    it->cut = (uint8_t)(!opaque && nmid < n / 10 && amin < 128 && amax >= 128);
    int wx = !(j->flags & 1), wy = !(j->flags & 2);
    /* colour under transparent texels: the mean of known neighbours, grown outwards */
    if (nk && nk < n) {
        int left = n - nk;
        uint8_t* nxt = malloc((size_t)n);
        while (left > 0 && nxt) {
            int upd = 0;
            memcpy(nxt, known, (size_t)n);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    int i = y * w + x;
                    if (known[i]) continue;
                    float sr = 0, sg = 0, sb = 0;
                    int k = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int q = ra_idx(y + dy, h, 1) * w + ra_idx(x + dx, w, 1);
                            if (known[q]) { sr += R[q]; sg += G[q]; sb += B[q]; k++; }
                        }
                    if (k) { R[i] = sr / k; G[i] = sg / k; B[i] = sb / k; nxt[i] = 1; upd++; }
                }
            memcpy(known, nxt, (size_t)n);
            left -= upd;
            if (!upd) break;
        }
        free(nxt);
    }
    for (int i = 0; i < n; i++) L[i] = 0.3f * R[i] + 0.59f * G[i] + 0.11f * B[i];
    if (wx) wx = ra_tiles_axis(L, w, h, 0);   /* (REPEAT: wrapped where the content continues across its edge) */
    if (wy) wy = ra_tiles_axis(L, w, h, 1);
    int P = RA.pad, nt = 0;
    ra_tile tl[16];
    for (int ty = 0; ty < h && nt < 16; ty += RA_TILE)
        for (int tx = 0; tx < w && nt < 16; tx += RA_TILE) {
            int cw = w - tx < RA_TILE ? w - tx : RA_TILE, ch = h - ty < RA_TILE ? h - ty : RA_TILE;
            int pw = cw + 2 * P, ph = ch + 2 * P;
            size_t pp = (size_t)pw * ph;
            float* px = malloc(pp * 4 * sizeof(float));
            if (!px) continue;
            for (int y = 0; y < ph; y++) {
                int sy = ra_idx(ty - P + y, h, wy) * w;
                for (int x = 0; x < pw; x++) {
                    int q = sy + ra_idx(tx - P + x, w, wx);
                    size_t o = (size_t)y * pw + x;
                    px[o] = R[q] * (1.0f / 255.0f); px[pp + o] = G[q] * (1.0f / 255.0f); px[2 * pp + o] = B[q] * (1.0f / 255.0f);
                    px[3 * pp + o] = A[q];
                }
            }
            ra_tile t = { j->item, (uint16_t)tx, (uint16_t)ty, (uint16_t)cw, (uint16_t)ch, (uint16_t)pw, (uint16_t)ph, px };
            tl[nt++] = t;
        }
    free(pl); free(known);
    EnterCriticalSection(&RA.cs);
    if (RA.tn + (uint32_t)nt > RA_MAXT) {
        LeaveCriticalSection(&RA.cs);
        for (int k = 0; k < nt; k++) free(tl[k].px);
        it->st = RI_WANT;                /* (full: prepared again later) */
        return;
    }
    for (int k = 0; k < nt; k++) RA.tq[RA.tn++] = tl[k];
    it->tiles = it->left = (uint16_t)nt;
    it->st = RI_TILES;
    LeaveCriticalSection(&RA.cs);
}

static const GUID ra_fmt_jpeg = { 0x19e4a5aa, 0x5662, 0x4fc5, { 0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57 } };
static const GUID ra_fmt_png = { 0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf } };
static const GUID ra_px_bgr24 = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c } };
static const GUID ra_px_bgra32 = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };
/* a cache file: JPEG (quality 0.93, no chroma subsampling) for opaque images, PNG for the others; written to .tmp and
 * moved over (a crash never leaves a half file under the real name) */
static int ra_save(IWICImagingFactory* f, ra_job* j, const ra_item* it) {
    int png = !j->opaque;
    wchar_t path[700], tmp[720];
    ra_path(it->hash, png, path, 700);
    _snwprintf(tmp, 720, L"%ls.tmp", path);
    tmp[719] = 0;
    uint32_t w = j->w, h = j->h, bpp = png ? 4u : 3u;
    uint8_t* px = malloc((size_t)w * h * bpp);
    if (!px) return 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t v = j->data[i], a = (v >> 24) * 255u / 128u;
        uint8_t* o = px + i * bpp;
        o[0] = (uint8_t)(v >> 16); o[1] = (uint8_t)(v >> 8); o[2] = (uint8_t)v;
        if (png) o[3] = (uint8_t)(a > 255 ? 255 : a);
    }
    IWICStream* st = NULL;
    IWICBitmapEncoder* enc = NULL;
    IWICBitmapFrameEncode* fr = NULL;
    IPropertyBag2* bag = NULL;
    int ok = 0;
    if (SUCCEEDED(IWICImagingFactory_CreateStream(f, &st)) && SUCCEEDED(IWICStream_InitializeFromFilename(st, tmp, GENERIC_WRITE)) &&
        SUCCEEDED(IWICImagingFactory_CreateEncoder(f, png ? &ra_fmt_png : &ra_fmt_jpeg, NULL, &enc)) &&
        SUCCEEDED(IWICBitmapEncoder_Initialize(enc, (IStream*)st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(enc, &fr, &bag))) {
        if (!png && bag) {
            PROPBAG2 opt[2];
            VARIANT v[2];
            memset(opt, 0, sizeof opt);
            memset(v, 0, sizeof v);
            opt[0].pstrName = (LPOLESTR)L"ImageQuality"; v[0].vt = VT_R4; v[0].fltVal = 0.93f;
            opt[1].pstrName = (LPOLESTR)L"JpegYCrCbSubsampling"; v[1].vt = VT_UI1; v[1].bVal = 3;   /* (4:4:4) */
            IPropertyBag2_Write(bag, 2, opt, v);
        }
        WICPixelFormatGUID pf = png ? ra_px_bgra32 : ra_px_bgr24;
        if (SUCCEEDED(IWICBitmapFrameEncode_Initialize(fr, bag)) && SUCCEEDED(IWICBitmapFrameEncode_SetSize(fr, w, h)) &&
            SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(fr, &pf)) && IsEqualGUID(&pf, png ? &ra_px_bgra32 : &ra_px_bgr24) &&
            SUCCEEDED(IWICBitmapFrameEncode_WritePixels(fr, h, w * bpp, w * h * bpp, px)) &&
            SUCCEEDED(IWICBitmapFrameEncode_Commit(fr)) && SUCCEEDED(IWICBitmapEncoder_Commit(enc)))
            ok = 1;
    }
    if (bag) IPropertyBag2_Release(bag);
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) IWICStream_Release(st);
    free(px);
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp);
    return ok;
}

static DWORD WINAPI ra_thread(LPVOID unused) {
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    HMODULE ole = LoadLibraryW(L"ole32.dll");
    typedef HRESULT(WINAPI * coinit_fn)(LPVOID, DWORD);
    typedef HRESULT(WINAPI * cocreate_fn)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
    coinit_fn coinit = ole ? (coinit_fn)(void*)GetProcAddress(ole, "CoInitializeEx") : NULL;
    cocreate_fn cocreate = ole ? (cocreate_fn)(void*)GetProcAddress(ole, "CoCreateInstance") : NULL;
    IWICImagingFactory* f = NULL;
    if (coinit && cocreate) {
        coinit(NULL, COINIT_MULTITHREADED);
        cocreate(&et_clsid_wic, NULL, CLSCTX_INPROC_SERVER, &et_iid_wic, (void**)&f);
    }
    if (!f) gpu_log("ai: no Windows image codec (WIC): the Reimagined cache is not written / read");
    for (;;) {
        WaitForSingleObject(RA.ev, INFINITE);
        for (;;) {
            ra_job j;
            EnterCriticalSection(&RA.cs);
            int any = RA.jt != RA.jh;
            if (any) j = RA.jq[RA.jt++ % RA_JOBQ];
            LeaveCriticalSection(&RA.cs);
            if (!any) break;
            ra_item* it = j.item >= 0 ? &RA.it[j.item] : NULL;
            if (j.type == RJ_INIT) ra_init_net();
            else if (j.type == RJ_PREP) ra_prep(&j);
            else if (j.type == RJ_SAVE) {
                int ok = f && ra_save(f, &j, it);
                EnterCriticalSection(&RA.cs);
                if (ok) { it->file = 1; RA.n_saved++; }
                LeaveCriticalSection(&RA.cs);
            } else if (j.type == RJ_LOAD) {
                et_hdjob hj;
                memset(&hj, 0, sizeof hj);
                hj.raw = it->hash;
                wchar_t path[700];
                ra_path(it->hash, 0, path, 700);
                int ok = f && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && et_hd_decode(f, path, &hj);
                if (!ok && f) { ra_path(it->hash, 1, path, 700); ok = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && et_hd_decode(f, path, &hj); }
                EnterCriticalSection(&RA.cs);
                if (ok && hj.w == (4u << it->lw) && hj.h == (4u << it->lh) && RA.nloaded < 256) {
                    it->rgba = hj.px; it->rw = hj.w; it->rh = hj.h; it->st = RI_LOADED;
                    RA.loaded[RA.nloaded++] = j.item;
                } else {
                    free(hj.px);
                    it->file = 0;        /* (a bad / missing / foreign file: built again) */
                    it->st = RI_WANT;
                }
                LeaveCriticalSection(&RA.cs);
            }
            free(j.data);
        }
    }
    return 0;
}

/* ---- GPU side (render thread) ------------------------------------------------------------------------------------------ */
static void* ra_cs_of(uint32_t key, void* user) {
    (void)user;
    switch (key) {                       /* (shaders/perms.txt cs_ai_*: every buffer combination ai_up.c's plan uses) */
    case 0x38810: return gpu_cs(GPU_SH_cs_ai_38810);
    case 0x88833: return gpu_cs(GPU_SH_cs_ai_88833);
    case 0x88844: return gpu_cs(GPU_SH_cs_ai_88844);
    case 0x88343: return gpu_cs(GPU_SH_cs_ai_88343);
    case 0x88434: return gpu_cs(GPU_SH_cs_ai_88434);
    case 0x21343: return gpu_cs(GPU_SH_cs_ai_21343);
    case 0x22343: return gpu_cs(GPU_SH_cs_ai_22343);
    case 0x22434: return gpu_cs(GPU_SH_cs_ai_22434);
    case 0x88114: return gpu_cs(GPU_SH_cs_ai_88114);
    case 0x88113: return gpu_cs(GPU_SH_cs_ai_88113);
    case 0x88851: return gpu_cs(GPU_SH_cs_ai_88851);
    case 0x88865: return gpu_cs(GPU_SH_cs_ai_88865);
    case 0x88856: return gpu_cs(GPU_SH_cs_ai_88856);
    case 0x88875: return gpu_cs(GPU_SH_cs_ai_88875);
    default: return NULL;
    }
}

static const IID ra_iid_adapter3 = { 0x645967a4, 0x1392, 0x4310, { 0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd } };
/* local video memory: the process's use and the OS's budget for it (MB); 0 when unknown */
static int ra_vram(double* use, double* budget) {
    static IDXGIAdapter3* a3;
    static int tried;
    *use = *budget = 0;
    if (!a3 && !tried) {
        tried = 1;
        IDXGIDevice* dd = NULL;
        IDXGIAdapter* ad = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(gpu_rt.dev, &gpu_IID_IDXGIDevice, (void**)&dd)) && SUCCEEDED(IDXGIDevice_GetAdapter(dd, &ad)))
            IDXGIAdapter_QueryInterface(ad, &ra_iid_adapter3, (void**)&a3);
        if (ad) IDXGIAdapter_Release(ad);
        if (dd) IDXGIDevice_Release(dd);
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO li;
    if (!a3 || FAILED(IDXGIAdapter3_QueryVideoMemoryInfo(a3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &li))) return 0;
    *use = li.CurrentUsage / 1048576.0;
    *budget = li.Budget / 1048576.0;
    return 1;
}

static void ra_list(void) {
    if (!settings_user_path || !settings_user_path(L"textures", RA.dir, 400)) { RA.dir[0] = 0; return; }
    CreateDirectoryW(RA.dir, NULL);
    wcscat(RA.dir, L"\\reimagined");
    CreateDirectoryW(RA.dir, NULL);
    size_t l = wcslen(RA.dir);
    _snwprintf(RA.dir + l, 600 - l, L"\\x4plus-v%d", RA_VER);
    RA.dir[599] = 0;
    CreateDirectoryW(RA.dir, NULL);
    wchar_t pat[620];
    _snwprintf(pat, 620, L"%ls\\*.*", RA.dir);
    pat[619] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    int cap = 0;
    uint64_t bytes = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcslen(fd.cFileName) != 20 || (_wcsicmp(fd.cFileName + 16, L".jpg") && _wcsicmp(fd.cFileName + 16, L".png"))) continue;
            wchar_t* end = NULL;
            uint64_t v = _wcstoui64(fd.cFileName, &end, 16);
            if (end != fd.cFileName + 16) continue;
            if (RA.nhave == cap) { cap = cap ? cap * 2 : 256; uint64_t* a = realloc(RA.have, cap * sizeof *a); if (!a) break; RA.have = a; }
            RA.have[RA.nhave++] = v;
            bytes += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (RA.nhave) qsort(RA.have, (size_t)RA.nhave, sizeof *RA.have, et_hd_cmp);
    gpu_log("ai: Reimagined textures: %d in the cache (%.0f MB, %ls)", RA.nhave, bytes / 1048576.0, RA.dir);
}

static int ra_init(void) {
    if (RA.init) return RA.init > 0;
    RA.init = -1;
    if (!(RA.up = raw_default(RA_UP_WORDS * 4u, NULL, &RA.up_srv, &RA.up_uav))) return 0;
    size_t P = (size_t)(RA_TILE + 2 * RA.pad) * (RA_TILE + 2 * RA.pad);
    if (!(RA.al = raw_default((UINT)(P * 4u), NULL, &RA.al_srv, NULL))) return 0;
    if (!(RA.pj = raw_default(RA_BMAX * 64u, NULL, &RA.pj_srv, NULL))) return 0;
    if (!(RA.res = raw_default((UINT)(P * 12u), NULL, NULL, &RA.res_uav))) return 0;
    if (!(RA.nok = raw_default(ET_NCLS * 4096u * 4u, NULL, &RA.nok_srv, &RA.nok_uav))) return 0;
    RA.pcb = et_buf(32, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
    if (!RA.pcb || !gpu_cs(GPU_SH_cs_ai_post) || !gpu_cs(GPU_SH_cs_ai_res) || !gpu_cs(GPU_SH_cs_etn)) return 0;
    for (int i = 0; i < RA_REG; i++) RA.reg[i].item = -1;
    for (int i = 0; i < RA_SRCN; i++) RA.src[i].item = -1;
    for (int i = 0; i < RA_SAVEN; i++) RA.sav[i].item = -1;
    if (!RA.inited_thread) {
        InitializeCriticalSection(&RA.cs);
        RA.ev = CreateEventW(NULL, FALSE, FALSE, NULL);
        RA.th = RA.ev ? CreateThread(NULL, 0, ra_thread, NULL, 0, NULL) : NULL;
        RA.inited_thread = RA.th ? 1 : -1;
    }
    if (RA.inited_thread < 0) return 0;
    if (!RA.it) RA.it = calloc(RA_ITEMS, sizeof *RA.it);
    if (!RA.it) return 0;
    ra_list();
    RA.init = 1;
    const char* e = getenv("PS2_TEX_DUMP");
    RA.dump = e && *e == '3';
    gpu_log("ai: Reimagined textures on (x4plus, padding %d, grain %.2f, restore %.2f, clarity %.2f, GPU %.1f..%.1f ms per frame, "
            "surface maps %s)", RA.pad, RA.grain, RA.restore, RA.clarity, RA.ms_min, RA.ms, RA.nrm_env ? "on" : "off");
    return 1;
}

/* the net: created on the worker when the video memory allows it (once; again after a low-memory release) */
static void ra_net_poll(void) {
    if (!RA.net_env) return;
    if (RA.net_state == 0) {
        double use, bud;
        size_t P = (size_t)(RA_TILE + 2 * RA.pad) * (RA_TILE + 2 * RA.pad);
        double need = P * 10.5 * 1024.0 / 1048576.0 + 64.0;
        if (ra_vram(&use, &bud) && bud > 0 && bud - use < need + 512.0) {
            if (RA.frames_busy++ % 600 == 0) gpu_log("ai: %.0f of %.0f MB video memory in use: no room for the net (%.0f MB) - Faithful's textures", use, bud, need);
            RA.n_low++;
            return;
        }
        RA.net_state = 1;
        ra_job j = { RJ_INIT, -1, NULL, 0, 0, 0, 0 };
        ra_push(j);
        return;
    }
    if (RA.net_state != 2 && RA.net_state != -1) return;
    EnterCriticalSection(&RA.cs);
    ai_gpu* g = RA.net_new;
    RA.net_new = NULL;
    int st = RA.net_state;
    LeaveCriticalSection(&RA.cs);
    if (g) {
        RA.net = g;
        RA.net_mb = ai_gpu_bytes(g) / 1048576.0;
        gpu_log("ai: x4plus net ready (%d blocks, %.0f MB of buffers)", ai_gpu_blocks(g), RA.net_mb);
        RA.net_state = 3;
    } else if (st == -1) {
        gpu_log("ai: no net (%s): Reimagined uses its cache and Faithful's textures", RA.net_err);
        RA.net_state = 4;
    }
}

static int32_t ra_reg_alloc(uint32_t n, int32_t item) {
    int free_i = -1;
    for (int i = 0; i < RA_REG && free_i < 0; i++) if (RA.reg[i].item < 0) free_i = i;
    if (free_i < 0 || n > RA_UP_WORDS) return -1;
    uint32_t at = 0;
    for (int pass = 0; pass < RA_REG + 1; pass++) {   /* (first fit: the lowest address past every overlapping region) */
        int moved = 0;
        for (int i = 0; i < RA_REG; i++)
            if (RA.reg[i].item >= 0 && at < RA.reg[i].at + RA.reg[i].n && RA.reg[i].at < at + n) { at = RA.reg[i].at + RA.reg[i].n; moved = 1; }
        if (!moved) break;
    }
    if (at + n > RA_UP_WORDS) return -1;
    RA.reg[free_i].at = at; RA.reg[free_i].n = n; RA.reg[free_i].item = item;
    return free_i;
}
static void ra_reg_free(int32_t r) { if (r >= 0) RA.reg[r].item = -1; }

/* build the image in upload region r over the Enhanced image of item i (its slot) */
static int ra_apply(int32_t i, int* bound) {
    ra_item* it = &RA.it[i];
    int32_t id = et_map_find(it->hash);
    et_slot* s = et_slot_of(id);
    if (!s || !s->used || s->hd || s->hash != it->hash) return 0;
    et_ent t;
    memset(&t, 0, sizeof t);
    t.lw = (uint8_t)(it->lw + 2); t.lh = (uint8_t)(it->lh + 2); t.flags = it->flags; t.ref = it->ref;
    if (!*bound) {
        ID3D11ShaderResourceView* n1 = NULL;
        ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 13, 1, &n1);
        *bound = 1;
    }
    if (!et_build(&t, id, RA.up_srv, RA.reg[it->reg].at, 1)) { et_batch_run(); et_build(&t, id, RA.up_srv, RA.reg[it->reg].at, 1); }
    et_batch_run();
    s->ai = 1;
    if (RA.dump) { et_dump_ai(id, it->hash); et_dump_nrm(id, it->hash); }
    return 1;
}

/* (et_link) entry idx now shows slot id: an Enhanced image whose AI image may exist or be built */
static void ra_note(int32_t idx, int32_t id) {
    (void)idx;
    et_slot* s = et_slot_of(id);
    if (!s || s->hd || s->ai) return;
    const et_ent* e = &ET.e[idx];
    if ((e->lw > e->lh ? e->lw : e->lh) > 8) return;   /* (sources above 256 texels: Enhanced) */
    int32_t i = ra_find(s->hash, 1);
    if (i < 0) return;
    ra_item* it = &RA.it[i];
    if (it->st == RI_NONE || it->st == RI_DONE) {
        it->lw = e->lw; it->lh = e->lh; it->flags = e->flags; it->ref = e->ref;
        if (it->file || ra_have(it->hash)) {
            it->file = 1;
            it->st = RI_LOAD;
            ra_job j = { RJ_LOAD, i, NULL, 0, 0, 0, 0 };
            ra_push(j);
        } else it->st = RI_WANT;
    }
}

static void ra_time_poll(void) {
    for (int k = 0; k < 4; k++) {
        if (!RA.tbusy[k]) continue;
        UINT64 a, b;
        if (ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)RA.tq_[k][0], &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)RA.tq_[k][1], &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        RA.tbusy[k] = 0;
        if (!gpu_ts_freq || b < a || RA.tgf[k] <= 0.05) continue;
        double ms = (double)(b - a) * 1000.0 / (double)gpu_ts_freq;
        RA.last_ms = ms;
        RA.ms_total += ms;
        if (ms > RA.worst_step) RA.worst_step = ms;
        RA.step_hist[ms * 4.0 < 79.0 ? (int)(ms * 4.0) : 79]++;
        double r = ms / RA.tgf[k];
        if (r > 0.01 && r < 5.0) RA.ms_per_gf = RA.ms_per_gf * 0.8 + r * 0.2;
    }
}

/* the GPU time this frame may spend on the net */
static double ra_budget_ms(void) {
    double hz = frame_rate_target_hz ? frame_rate_target_hz() : 60.0;
    double lim = RA.ms;
    if (hz > 1.0 && gpu_frame_ms_last > 0.0) {
        double others = gpu_frame_ms_last - RA.last_ms;   /* (the frame's own GPU time without the net's) */
        double room = 0.8 * 1000.0 / hz - others;
        if (room < lim) lim = room;
    } else if (hz <= 1.0) lim = RA.ms_min * 2.0;          /* (unlimited frame rate: a small fixed share) */
    if (ET.out_frame) lim -= 1.2 * ET.out_frame / 1048576.0;   /* (this frame's Enhanced builds) */
    return lim < RA.ms_min ? RA.ms_min : lim;
}

/* start a net batch from the prepared tiles (same padded size) */
static void ra_batch_start(void) {
    ra_tile b[RA_BMAX];
    int n = 0;
    size_t maxp = ai_gpu_max_pixels(RA.net);
    EnterCriticalSection(&RA.cs);
    if (RA.tn) {
        uint16_t pw = RA.tq[0].pw, ph = RA.tq[0].ph;
        size_t P = (size_t)pw * ph;
        uint32_t k = 0;
        for (uint32_t i = 0; i < RA.tn; i++) {
            ra_tile t = RA.tq[i];
            if (t.pw == pw && t.ph == ph && n < RA_BMAX && (size_t)(n + 1) * P <= maxp) b[n++] = t;
            else RA.tq[k++] = t;
        }
        RA.tn = k;
    }
    LeaveCriticalSection(&RA.cs);
    if (!n) return;
    /* upload regions for the textures (whole textures: their tiles may come in later batches) */
    for (int k = 0; k < n; k++) {
        ra_item* it = &RA.it[b[k].item];
        if (it->reg >= 0) continue;
        it->reg = ra_reg_alloc(16u << (it->lw + it->lh), b[k].item);
        if (it->reg < 0) {               /* (no room yet: these tiles go back to the queue's front) */
            EnterCriticalSection(&RA.cs);
            int m = n - k;
            if (RA.tn + (uint32_t)m <= RA_MAXT) {
                memmove(RA.tq + m, RA.tq, RA.tn * sizeof RA.tq[0]);
                memcpy(RA.tq, b + k, (size_t)m * sizeof b[0]);
                RA.tn += (uint32_t)m;
            } else for (int q = k; q < n; q++) { free(b[q].px); RA.it[b[q].item].st = RI_FAIL; }
            LeaveCriticalSection(&RA.cs);
            n = k;
            break;
        }
    }
    if (!n) return;
    uint32_t pw = b[0].pw, ph = b[0].ph;
    size_t P = (size_t)pw * ph;
    float* rgb = malloc((size_t)n * 3 * P * sizeof(float));
    float* al = malloc((size_t)n * P * sizeof(float));
    if (!rgb || !al || ai_gpu_begin(RA.net, n, (int)pw, (int)ph)) {
        free(rgb); free(al);
        for (int k = 0; k < n; k++) { free(b[k].px); RA.it[b[k].item].st = RI_FAIL; }
        return;
    }
    for (int k = 0; k < n; k++) {
        memcpy(rgb + (size_t)k * 3 * P, b[k].px, 3 * P * sizeof(float));
        memcpy(al + (size_t)k * P, b[k].px + 3 * P, P * sizeof(float));
        free(b[k].px);
        b[k].px = NULL;
        RA.bt[k] = b[k];
    }
    ai_gpu_upload(RA.net, rgb);
    D3D11_BOX bx = { 0, 0, 0, (UINT)((size_t)n * P * 4), 1, 1 };
    ID3D11DeviceContext_UpdateSubresource(gpu_rt.ctx, (ID3D11Resource*)RA.al, 0, &bx, al, 0, 0);
    free(rgb); free(al);
    RA.bn = n;
    RA.bon = 1;
    RA.bframe = ET.frame;
    RA.n_tiles += (uint64_t)n;
}

/* the batch's net is done: tiles -> upload regions (cs_ai_post); finished textures -> their slots + cache files */
static void ra_batch_finish(int* bound) {
    ID3D11DeviceContext* c = gpu_rt.ctx;
    uint32_t jw[RA_BMAX * 16];
    memset(jw, 0, sizeof jw);
    uint32_t pw = RA.bt[0].pw, ph = RA.bt[0].ph, cw = RA.bt[0].cw, ch = RA.bt[0].ch;
    for (int k = 0; k < RA.bn; k++) {
        const ra_tile* t = &RA.bt[k];
        const ra_item* it = &RA.it[t->item];
        uint32_t* j = jw + k * 16;
        j[0] = RA.reg[it->reg].at; j[1] = 4u << it->lw; j[2] = 4u * t->tx; j[3] = 4u * t->ty;
        j[4] = 4u * t->cw; j[5] = 4u * t->ch; j[6] = t->pw; j[7] = t->ph; j[8] = (it->cut ? 1u : 0u) | (it->opaque ? 2u : 0u);
    }
    D3D11_BOX jb = { 0, 0, 0, (UINT)RA.bn * 64u, 1, 1 };
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource*)RA.pj, 0, &jb, jw, 0, 0);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(c, (ID3D11Resource*)RA.pcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        uint32_t u[4] = { 0, (uint32_t)RA.pad, 0, 0 };
        float fl[4] = { RA.grain, RA.restore, RA.clarity, 0 };
        memcpy(m.pData, u, 16); memcpy((char*)m.pData + 16, fl, 16);
        ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)RA.pcb, 0);
    }
    if (!*bound) {
        ID3D11ShaderResourceView* n1 = NULL;
        ID3D11DeviceContext_PSSetShaderResources(c, 13, 1, &n1);
        *bound = 1;
    }
    ID3D11UnorderedAccessView* u[4] = { ai_gpu_uav(RA.net, AI_BUF_OUT), ai_gpu_uav(RA.net, AI_BUF_IMG), RA.up_uav, RA.res_uav };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 4, u, NULL);
    ID3D11ShaderResourceView* s2[2] = { RA.al_srv, RA.pj_srv };
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 2, s2);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 4, 1, &RA.pcb);
    if (RA.restore > 0) {                /* (the restore's residuals first) */
        ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_ai_res), NULL, 0);
        ID3D11DeviceContext_Dispatch(c, (pw + 7) / 8, (ph + 7) / 8, (UINT)RA.bn);
        gpu_rt.n_dispatches++;
    }
    ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_ai_post), NULL, 0);
    ID3D11DeviceContext_Dispatch(c, (4 * cw + 7) / 8, (4 * ch + 7) / 8, (UINT)RA.bn);
    gpu_rt.n_dispatches++;
    ID3D11UnorderedAccessView* nu[4] = { NULL, NULL, NULL, NULL };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 4, nu, NULL);
    ID3D11ShaderResourceView* ns[2] = { NULL, NULL };
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 2, ns);
    ID3D11Buffer* nb = NULL;
    ID3D11DeviceContext_CSSetConstantBuffers(c, 4, 1, &nb);
    for (int k = 0; k < RA.bn; k++) {
        ra_item* it = &RA.it[RA.bt[k].item];
        if (it->left) it->left--;
        if (!it->left && it->st == RI_TILES) { it->st = RI_POST; RA.n_net++; }
    }
    RA.bon = 0;
    RA.bn = 0;
}

/* finished textures (RI_POST): onto their slots, then read back for the cache file (the region is free after the copy) */
static void ra_post_poll(int* bound) {
    for (int r = 0; r < RA_REG; r++) {
        int32_t i = RA.reg[r].item;
        if (i < 0 || RA.it[i].st != RI_POST) continue;
        ra_item* it = &RA.it[i];
        int sv = -1;
        for (int k = 0; k < RA_SAVEN && sv < 0; k++) if (RA.sav[k].item < 0) sv = k;
        if (sv < 0) break;               /* (all read-backs busy: next frame) */
        uint32_t words = 16u << (it->lw + it->lh);
        if (!RA.sav[sv].st) RA.sav[sv].st = et_buf(RA_UP_WORDS / 4u * 4u, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
        ra_apply(i, bound);
        if (RA.sav[sv].st && words <= RA_UP_WORDS / 4u) {
            D3D11_BOX bx = { RA.reg[r].at * 4u, 0, 0, (RA.reg[r].at + words) * 4u, 1, 1 };
            ID3D11DeviceContext_CopySubresourceRegion(gpu_rt.ctx, (ID3D11Resource*)RA.sav[sv].st, 0, 0, 0, 0, (ID3D11Resource*)RA.up, 0, &bx);
            RA.sav[sv].item = i; RA.sav[sv].frame = ET.frame; RA.sav[sv].words = words;
        }
        ra_reg_free(it->reg);
        it->reg = -1;
        it->st = RI_DONE;
    }
    for (int k = 0; k < RA_SAVEN; k++) {
        if (RA.sav[k].item < 0 || ET.frame - RA.sav[k].frame < 2) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        if (ID3D11DeviceContext_Map(gpu_rt.ctx, (ID3D11Resource*)RA.sav[k].st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
        ra_item* it = &RA.it[RA.sav[k].item];
        uint32_t* px = malloc((size_t)RA.sav[k].words * 4);
        if (px) {
            memcpy(px, m.pData, (size_t)RA.sav[k].words * 4);
            ra_job j = { RJ_SAVE, RA.sav[k].item, px, 4u << it->lw, 4u << it->lh, 0, it->opaque };
            ra_push(j);
        }
        ID3D11DeviceContext_Unmap(gpu_rt.ctx, (ID3D11Resource*)RA.sav[k].st, 0);
        RA.sav[k].item = -1;
    }
}

static double ra_pct(double p) {         /* (statistics) a percentile of the net's GPU time per frame */
    uint32_t n = 0, c = 0;
    for (int i = 0; i < 80; i++) n += RA.step_hist[i];
    for (int i = 0; i < 80; i++) { c += RA.step_hist[i]; if (n && c >= p * n) return (i + 1) * 0.25; }
    return 0;
}

/* the frame's Reimagined work (et_frame_body, after Enhanced's): cached images up, source read-backs, the net */
static void ra_frame(int* bound) {
    ID3D11DeviceContext* c = gpu_rt.ctx;
    if (!ra_init()) {
        if (!RA.failed) { gpu_log("ai: cannot create the Reimagined textures' resources: Faithful's textures"); RA.failed = 1; }
        return;
    }
    ra_net_poll();
    ra_time_poll();
    if (RA.net && ET.frame % 120 == 0) {   /* (video memory: the net goes when the game needs the room) */
        double use, bud;
        if (ra_vram(&use, &bud) && bud > 0 && use > 0.97 * bud && !RA.bon) {
            gpu_log("ai: video memory %.0f of %.0f MB: the net is released (built images stay)", use, bud);
            ai_gpu_destroy(RA.net);
            RA.net = NULL;
            RA.net_state = 0;
            RA.n_low++;
            if (use > 0.99 * bud && ETN.on) { gpu_log("ai: surface maps dropped (video memory)"); etn_free(); RA.nrm_env = 0; }
        }
    }
    /* 1. cached images decoded since: up into a region, built over the slot */
    uint32_t up = 0;
    for (;;) {
        EnterCriticalSection(&RA.cs);
        int32_t i = RA.nloaded ? RA.loaded[0] : -1;
        LeaveCriticalSection(&RA.cs);
        if (i < 0) break;
        ra_item* it = &RA.it[i];
        uint32_t words = it->rw * it->rh;
        if (up && up + words > RA_LOAD_TEXELS) break;
        int32_t r = ra_reg_alloc(words, i);
        if (r < 0) break;
        D3D11_BOX bx = { RA.reg[r].at * 4u, 0, 0, (RA.reg[r].at + words) * 4u, 1, 1 };
        ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource*)RA.up, 0, &bx, it->rgba, 0, 0);
        it->reg = r;
        ra_apply(i, bound);
        ra_reg_free(r);
        it->reg = -1;
        free(it->rgba);
        it->rgba = NULL;
        it->st = RI_DONE;
        RA.n_load++;
        up += words;
        EnterCriticalSection(&RA.cs);
        memmove(RA.loaded, RA.loaded + 1, (RA.nloaded - 1) * sizeof RA.loaded[0]);
        RA.nloaded--;
        LeaveCriticalSection(&RA.cs);
    }
    /* 2. source read-backs: finished ones to the worker, new ones for items that want one */
    for (int k = 0; k < RA_SRCN; k++) {
        if (RA.src[k].item < 0 || ET.frame - RA.src[k].frame < 2) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        if (ID3D11DeviceContext_Map(c, (ID3D11Resource*)RA.src[k].st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
        uint32_t n = RA.src[k].w * RA.src[k].h;
        uint32_t* px = malloc((size_t)n * 4);
        ra_item* it = &RA.it[RA.src[k].item];
        if (px) {
            memcpy(px, m.pData, (size_t)n * 4);
            it->st = RI_PREP;
            ra_job j = { RJ_PREP, RA.src[k].item, px, RA.src[k].w, RA.src[k].h, it->flags, 0 };
            ra_push(j);
        } else it->st = RI_WANT;
        ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)RA.src[k].st, 0);
        RA.src[k].item = -1;
    }
    if (RA.net && TD.buf) {
        int k = 0;
        for (int32_t i = 0; i < RA_ITEMS && k < RA_SRCN; i++) {
            ra_item* it = &RA.it[i];
            if (!it->hash || it->st != RI_WANT) continue;
            while (k < RA_SRCN && RA.src[k].item >= 0) k++;
            if (k >= RA_SRCN) break;
            /* a live entry with this content (the slot's chain) */
            int32_t id = et_map_find(it->hash);
            et_slot* s = et_slot_of(id);
            int32_t ei = s ? s->first : -1;
            while (ei >= 0) {
                const et_ent* e = &ET.e[ei];
                const td_ent* t = &TD.ent[ei];
                if (e->st == ES_READY && t->live && t->ver == e->ver && t->base == e->base) break;
                ei = ET.e[ei].next;
            }
            if (ei < 0) { it->st = RI_NONE; continue; }   /* (noted again at its next link) */
            uint32_t w = 1u << it->lw, h = 1u << it->lh;
            if (!RA.src[k].st) RA.src[k].st = et_buf(256u * 256u * 4u, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
            if (!RA.src[k].st) break;
            D3D11_BOX bx = { ET.e[ei].base * 4u, 0, 0, (ET.e[ei].base + w * h) * 4u, 1, 1 };
            ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource*)RA.src[k].st, 0, 0, 0, 0, (ID3D11Resource*)TD.buf, 0, &bx);
            RA.src[k].item = i; RA.src[k].frame = ET.frame; RA.src[k].w = w; RA.src[k].h = h;
            it->st = RI_SRC;
        }
    }
    /* 3. the net: a batch started, stepped within the budget, finished */
    ra_post_poll(bound);
    if (!RA.net) return;
    if (!RA.bon) ra_batch_start();
    if (!RA.bon) return;
    double ms = ra_budget_ms();
    double gf = ms / RA.ms_per_gf;
    uint32_t ts = RA.thead % 4;
    int timed = 0;
    if (!RA.tq_[ts][0]) {
        D3D11_QUERY_DESC qt = { D3D11_QUERY_TIMESTAMP, 0 };
        ID3D11Device_CreateQuery(gpu_rt.dev, &qt, &RA.tq_[ts][0]);
        ID3D11Device_CreateQuery(gpu_rt.dev, &qt, &RA.tq_[ts][1]);
    }
    timed = !RA.tbusy[ts] && RA.tq_[ts][0] && RA.tq_[ts][1];
    if (timed) ID3D11DeviceContext_End(c, (ID3D11Asynchronous*)RA.tq_[ts][0]);
    if (!*bound) {
        ID3D11ShaderResourceView* n1 = NULL;
        ID3D11DeviceContext_PSSetShaderResources(c, 13, 1, &n1);
        *bound = 1;
    }
    double done = ai_gpu_step(RA.net, gf);
    gpu_rt.n_dispatches += 1;
    if (timed) {
        ID3D11DeviceContext_End(c, (ID3D11Asynchronous*)RA.tq_[ts][1]);
        RA.tgf[ts] = done;
        RA.tbusy[ts] = 1;
        RA.thead++;
    }
    RA.gf_total += done;
    if (ai_gpu_failed(RA.net)) {
        gpu_log("ai: the net failed (a missing shader permutation): no more Reimagined builds");
        for (int k = 0; k < RA.bn; k++) RA.it[RA.bt[k].item].st = RI_FAIL;
        ai_gpu_destroy(RA.net);
        RA.net = NULL;
        RA.net_state = 4;
        RA.bon = 0;
        return;
    }
    if (ai_gpu_done(RA.net)) ra_batch_finish(bound);
    if (RA.log && ET.frame % 600 == 0) {
        uint32_t queued = 0, want = 0;
        for (int32_t i = 0; i < RA_ITEMS; i++) if (RA.it[i].hash) { queued += RA.it[i].st == RI_TILES || RA.it[i].st == RI_PREP; want += RA.it[i].st == RI_WANT; }
        gpu_log("ai: %llu built by the net (%llu tiles), %llu from the cache, %llu saved, %llu excluded; %u in the net's queue, %u waiting; "
                "%.1f TFLOP, GPU %.0f ms in all (%.3f ms per GFLOP; per frame p50 %.2f, p99 %.2f, worst %.2f ms)",
                (unsigned long long)RA.n_net, (unsigned long long)RA.n_tiles, (unsigned long long)RA.n_load, (unsigned long long)RA.n_saved,
                (unsigned long long)RA.n_excl, queued, want, RA.gf_total / 1000.0, RA.ms_total, RA.ms_per_gf, ra_pct(0.5), ra_pct(0.99),
                RA.worst_step);
    }
}

/* (et_release) the GPU side goes; the worker, the items and the cache list stay */
static void ra_release(void) {
    etn_free();
    if (RA.bon) for (int k = 0; k < RA.bn; k++) RA.it[RA.bt[k].item].st = RI_NONE;
    RA.bon = 0; RA.bn = 0;
    if (RA.net) { ai_gpu_destroy(RA.net); RA.net = NULL; RA.net_state = 0; }
    GPU_RELEASE(RA.up_srv); GPU_RELEASE(RA.up_uav); GPU_RELEASE(RA.up);
    GPU_RELEASE(RA.al_srv); GPU_RELEASE(RA.al);
    GPU_RELEASE(RA.pj_srv); GPU_RELEASE(RA.pj);
    GPU_RELEASE(RA.res_uav); GPU_RELEASE(RA.res);
    GPU_RELEASE(RA.nok_srv); GPU_RELEASE(RA.nok_uav); GPU_RELEASE(RA.nok);
    GPU_RELEASE(RA.pcb);
    for (int k = 0; k < RA_SRCN; k++) { GPU_RELEASE(RA.src[k].st); RA.src[k].item = -1; }
    for (int k = 0; k < RA_SAVEN; k++) { GPU_RELEASE(RA.sav[k].st); RA.sav[k].item = -1; }
    for (int k = 0; k < 4; k++) { GPU_RELEASE(RA.tq_[k][0]); GPU_RELEASE(RA.tq_[k][1]); RA.tbusy[k] = 0; }
    for (int r = 0; r < RA_REG; r++) RA.reg[r].item = -1;
    if (RA.it)                           /* (items in flight on the GPU start again; the worker's own finish) */
        for (int32_t i = 0; i < RA_ITEMS; i++) {
            ra_item* it = &RA.it[i];
            it->reg = -1;
            if (it->st == RI_SRC || it->st == RI_POST || it->st == RI_DONE || it->st == RI_WANT) it->st = RI_NONE;
        }
    if (RA.inited_thread > 0) {
        EnterCriticalSection(&RA.cs);
        for (uint32_t k = 0; k < RA.tn; k++) { free(RA.tq[k].px); RA.it[RA.tq[k].item].st = RI_NONE; }
        RA.tn = 0;
        LeaveCriticalSection(&RA.cs);
    }
    RA.init = 0;
    RA.failed = 0;
}
