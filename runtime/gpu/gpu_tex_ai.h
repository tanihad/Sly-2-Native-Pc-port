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
 *            weights: scripts/ai_model.py export; default <exe dir>\models\x4plus.bin, then <exe dir>\x4plus.bin (where a
 *            1.0.x launcher's update puts it: scripts/build_game.py), then <user dir>\models\x4plus.bin),
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
#define RA_LOAD_TEXELS (1u << 18)        /* cached images uploaded per frame (1 M: render-thread peaks of ~30 ms at a hub load) */
#define RA_JOBQ 4096
#define RA_REG 64

enum { RI_NONE, RI_WANT, RI_SRC, RI_PREP, RI_TILES, RI_POST, RI_DONE, RI_LOAD, RI_LOADED, RI_EXCL, RI_FAIL,
       RI_DECODE, RI_LFAIL, RI_DEFER };   /* (tex-prebuild: a loader decodes it; its file failed; built / loaded while shown) */
typedef struct {
    uint64_t hash;
    uint8_t st, lw, lh, flags, ref, cut, opaque, file;
    uint8_t bad, pre, waiters, gen;      /* (tex-prebuild) bad file; a preload (no entry waits); entries wait (ES_AILOAD);
                                          * a generation (a loader's result for an item reset meanwhile is dropped) */
    uint8_t alias;                       /* (tex-opt) its cache file is the other alpha-test key's (ra_file_of) */
    uint16_t tiles, left;                /* tiles of the texture, not yet through the net */
    int32_t reg;                         /* upload region (index) or -1 */
    uint32_t* rgba;                      /* (worker: a decoded cache file) */
    uint32_t rw, rh;
} ra_item;
typedef struct { int32_t item; uint16_t tx, ty, cw, ch, pw, ph; float* px; } ra_tile;   /* px: r, g, b (0..1), a (0..255) planes */
enum { RJ_INIT = 1, RJ_PREP, RJ_SAVE, RJ_LOAD, RJ_MANI };   /* (RJ_MANI: write a level's list - tex-prebuild) */
typedef struct { int type; int32_t item; uint32_t* data; uint32_t w, h; uint8_t flags, opaque; } ra_job;

static struct {
    int env, env_on, log, pad, net_env, nrm_env, dump, half;
    float ms, ms_min, grain, nrm_k, restore, clarity;
    int on, was, failed, init, inited_thread, net_state;   /* net_state: 0 none, 1 creating, 2 ready, -1 unavailable */
    wchar_t dir[600];
    char model[32];
    uint64_t* have; int nhave, have_cap; /* cache files (sorted hashes; render thread only) */
    ra_item* it;
    /* worker */
    CRITICAL_SECTION cs;
    HANDLE ev, th;
    ra_job jq[RA_JOBQ]; uint32_t jh, jt;
    ra_tile tq[RA_MAXT]; uint32_t tn;    /* prepared tiles (FIFO) */
    int32_t loaded[512]; uint32_t nloaded;
    ai_gpu* net; ai_gpu* net_new; char net_err[256];
    ID3D11ComputeShader* kcs[16];          /* (created on the worker: a first use on the render thread cost up to 70 ms) */
    ID3D11ComputeShader* kcs_new[16];      /* (reimagine-crash) the worker's new shaders and read-back buffers, handed over */
    ID3D11Buffer* src_new[RA_SRCN];        /* with net_new (under cs) and installed by the render thread (ra_net_poll) */
    ID3D11Buffer* sav_new[RA_SAVEN];
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

/* (tex-prebuild) Ready before first sight (notes/reimagined.md): PS2_AI_READY=0 turns all of it off (the 0.1.1247 flow)
 *   direct    a texture whose AI image is in the cache gets it at its first resolve (ES_AILOAD: the original sampling
 *             shows until it is there, as it does until an Enhanced image is built) - never an Enhanced image first
 *   loaders   cache files are decoded by RA_LDN threads (on-demand requests before preloads), not behind the net's
 *             creation and the tile work on the worker
 *   levels    every level (widCurrent) keeps a list of the textures it showed (<cache>\levels\w<world>.bin); when the
 *             level changes (the game writes widCurrent as its load starts) the list's AI images are decoded and built
 *             into free image slots during the loading screen (a bigger per-frame budget while no 3D is drawn), so the
 *             level's textures find them at their first use
 *   defer     an AI image that would replace an Enhanced image a draw used in the last 2 frames waits (RI_DEFER) until the
 *             texture is out of sight or no 3D is drawn (loading, menus): built images are written to the cache and
 *             loaded again then - no visible swap (a cold cache's first visit) */
#define RA_LDN 6                         /* loader threads at most (a third of the CPU's threads, at least 2) */
#define RA_LQ 8192                       /* per queue */
#define RA_LBYTES (160u << 20)           /* decoded images waiting for their upload, at most */
#define RA_RECN 8192                     /* textures per level list */
#define RA_DEFN 4096
#define RA_PRE_GRACE 2400u               /* a preloaded image is not reused for another one for this many frames */
typedef struct { uint64_t key; uint8_t lw, lh, flags, ref; uint32_t pad; } ra_rec;
static struct {
    int on, nth;
    HANDLE ev, th[RA_LDN];
    int32_t q[2][RA_LQ]; uint32_t qh[2], qt[2];   /* 0: on demand, 1: preloads */
    uint64_t bytes;                      /* decoded, not uploaded yet */
    /* the level and its list */
    uint32_t world, wframe, uses_last;
    ra_rec* rec; uint32_t nrec; uint64_t* set; int dirty; uint32_t saved;
    uint32_t mreq; int mstate; ra_rec* mbuf; uint32_t mn;   /* (a list read by a loader: mstate 1 asked, 2 reading, 3 ready) */
    ra_rec* mbuf2; uint32_t mn2, last_world;   /* (the title: also the list of the level played last - "Resume Game" loads it) */
    ra_rec* pre; uint32_t npre, ipre;    /* the level list's images still to queue */
    int32_t defer[RA_DEFN]; uint32_t ndefer;
    uint32_t n_wait;
    ID3D11Query* tq[4][2]; uint8_t tbusy[4]; uint32_t ttex[4], thead; double ms_per_m, last_ms;   /* (GPU time of the
                                          * cached images' builds: their own rate - no upscale, so cheaper than Enhanced's) */
    uint64_t n_direct, n_pre_q, n_pre_built, n_pre_noslot, n_defer, n_defer_done, n_timeout, n_lfail, n_new;
} RQ = { .on = -1 };

/* (tex-prebuild) PS2_AI_POPLOG=1: what the player sees of the AI images (notes/reimagined.md "Ready before first
 * sight"). The level (the game's widCurrent, read from EE memory), the first frame with 3D art after a level change
 * ("first 3D frame": play starts), and from then on: AI images that replaced an Enhanced image already shown (swaps; visible
 * = a draw used it in the last 2 frames), and each texture's first image (AI or Enhanced) with its delay after the first
 * draw that used it (the original sampling shows meanwhile). Lines "ai-pop:" every 1200 frames and at level changes. */
extern uint8_t* ps2_ram __attribute__((weak));   /* (ps2_runtime.c; tools: none) */
#define RA_WID_CURRENT 0x003D4A68u               /* widCurrent (notes/warp.md; r2_capture.c WID_CURRENT) */
static struct {
    int on;
    uint32_t world, wframe, first3d, play, gap;
    uint64_t swap, swap_vis, swap_play, swap_play_vis, first_ai, first_enh, lat[6];
} RP = { -1 };
static uint32_t ra_world_now(void) {
    return &ps2_ram && ps2_ram ? *(volatile uint32_t*)(ps2_ram + RA_WID_CURRENT) : 0xFFFFFFFFu;
}
static void ra_pop_line(const char* why) {
    gpu_log("ai-pop: %s frame %u v%u world %u (first 3D frame %u): swaps after it %llu (visible %llu), before %llu (visible %llu); "
            "first images AI %llu, Enhanced %llu; delay after the first draw <=3 frames %llu, <=7 %llu, <=15 %llu, <=63 %llu, "
            "<=255 %llu, more %llu", why, ET.frame, gpu_stats_cur()->vblank, RP.world, RP.first3d,
            (unsigned long long)RP.swap_play, (unsigned long long)RP.swap_play_vis,
            (unsigned long long)(RP.swap - RP.swap_play), (unsigned long long)(RP.swap_vis - RP.swap_play_vis),
            (unsigned long long)RP.first_ai, (unsigned long long)RP.first_enh, (unsigned long long)RP.lat[0],
            (unsigned long long)RP.lat[1], (unsigned long long)RP.lat[2], (unsigned long long)RP.lat[3],
            (unsigned long long)RP.lat[4], (unsigned long long)RP.lat[5]);
}
/* (ra_frame, every frame) the level and the first 3D frame after a change; ET.uses = the last frame's art uses */
static void ra_pop_frame(uint32_t uses) {
    if (RP.on < 0) { const char* e = getenv("PS2_AI_POPLOG"); RP.on = e && *e == '1'; }
    if (!RP.on) return;
    uint32_t w = ra_world_now();
    if (w != RP.world) {
        if (RP.play) ra_pop_line("level left:");
        gpu_log("ai-pop: frame %u v%u t %.0f ms level %u -> %u", ET.frame, gpu_stats_cur()->vblank, (double)GetTickCount64(), RP.world, w);
        uint32_t keep = RP.on;
        memset(&RP, 0, sizeof RP);
        RP.on = (int)keep; RP.world = w; RP.wframe = ET.frame;
    }
    if (!uses && !RP.gap) {              /* (the level change comes with the title's / last level's frames: play starts after
                                          * the loading screen - frames without 3D art) */
        RP.gap = 1;
        gpu_log("ai-pop: frame %u v%u t %.0f ms world %u: loading (%u frames after the level change)", ET.frame - 1,
                gpu_stats_cur()->vblank, (double)GetTickCount64(), w, ET.frame - 1 - RP.wframe);
    }
    if (!RP.play && uses && RP.gap) {
        RP.play = 1; RP.first3d = ET.frame - 1;
        gpu_log("ai-pop: frame %u v%u t %.0f ms world %u: first 3D frame (%u frames after the level change, %u art uses)", RP.first3d,
                gpu_stats_cur()->vblank, (double)GetTickCount64(), w, RP.first3d - RP.wframe, uses);
    }
    if (ET.frame % 1200 == 0) ra_pop_line("");
}
/* (et_link) entry idx shows slot s: its first image since it was noted? */
static int ra_rec_add(const ra_rec* r);
static void ra_link_note(int32_t idx, const et_slot* s) {
    et_ent* e = &ET.e[idx];
    if (RQ.on > 0 && RQ.world < 0xFFFFFFF0u && !s->hd && (e->lw > e->lh ? e->lw : e->lh) <= 8) {   /* (the level's list) */
        ra_rec r = { e->hash, e->lw, e->lh, e->flags, e->ref, 0 };
        if (ra_rec_add(&r)) { RQ.dirty = 1; RQ.n_new++; }
    }
    if (RP.on <= 0 || !RP.play) return;
    if (!e->want) return;                /* (counted once per noting) */
    uint32_t d = ET.frame - e->want;
    e->want = 0;
    if (s->ai) RP.first_ai++; else RP.first_enh++;
    RP.lat[d <= 3 ? 0 : d <= 7 ? 1 : d <= 15 ? 2 : d <= 63 ? 3 : d <= 255 ? 4 : 5]++;
}
/* (ra_apply) the AI image replaces the image of slot s: a swap where it was shown */
static void ra_pop_swap(const et_slot* s) {
    if (RP.on <= 0 || s->first < 0) return;
    int vis = s->last + 2 >= ET.frame;
    RP.swap++; RP.swap_vis += vis;
    if (RP.play) { RP.swap_play++; RP.swap_play_vis += vis; }
}


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
        e = getenv("PS2_AI_HALF"); RA.half = !(e && *e == '0');   /* (tex-opt: the half-float net; 0 = 0.1.1267's float one) */
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
/* (tex-opt, PS2_TEX_DUMP=3) level 0 of an S x S RGBA8 subresource (a BC7 image's staging copy) as kind 2 (_ai) / 3 (_nrm) */
static void et_dump_from(ID3D11Resource* src, uint32_t S, int kind, uint64_t raw) {
    if (!src || et_dump_seen(raw, kind)) return;
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = td.Height = S; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* t = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &t))) return;
    ID3D11DeviceContext_CopySubresourceRegion(gpu_rt.ctx, (ID3D11Resource*)t, 0, 0, 0, 0, src, 0, NULL);
    et_dump_add((ID3D11Resource*)t, S, S, kind, raw);
}

/* ---- (tex-opt) BC7 classes: the opaque AI images compressed (notes/reimagined.md "Lighter and faster") -----------------
 *   what     under Reimagined an opaque AI image (a JPEG cache file, or a net build without alpha) is built as before (mips,
 *            surface map) but into a one-slice RGBA8 staging array of its class, then BC7-encoded on the GPU (shaders/
 *            bc7.hlsl, every level) into a slot of the class's BC7 array (ET.sl[ET_BC + c]); its map is copied to the BC7
 *            class's map array. et_tab info bit 31 sends the shaders there (etex.hlsli et_bclass, etn.hlsli). 1 byte per
 *            texel instead of 4. Images with alpha (cut-outs, glass), excluded and Enhanced images stay RGBA8, whose classes
 *            are then pre-sized small (et_grow first_bc: 205 instead of 790 MB).
 *   quality  scripts/bc7_tool.c encode over the caches (notes): every opaque image >= 43 dB PSNR, median 55 dB.
 *   config   PS2_TEX_BC7=0: the AI images stay RGBA8 (0.1.1267's memory layout). Faithful / Retro: nothing of this. */
#define EBC_BLK 11                       /* block scratch textures: 256 >> j blocks square (j = 0 .. 10) */
static struct {
    int env, failed;
    ID3D11Texture2D* arr[ET_NCLS];      /* BC7 classes (ET.cap[ET_BC + c] slices) */
    ID3D11ShaderResourceView* srv[ET_NCLS];
    ID3D11Texture2D* narr[ET_NCLS];     /* their surface maps (RGBA8, 32 << c, the slot's slice) */
    ID3D11ShaderResourceView* nsrv[ET_NCLS];
    uint32_t ncap[ET_NCLS];
    ID3D11Texture2D* stg[ET_NCLS];      /* one-slice staging: the colour levels and the map levels of the image being built */
    ID3D11ShaderResourceView* stg_srv[ET_NCLS];
    ID3D11UnorderedAccessView* stg_uav[ET_NCLS][ET_MAXMIP];
    ID3D11Texture2D* nstg[ET_NCLS];
    ID3D11UnorderedAccessView* nstg_uav[ET_NCLS][ET_MAXMIP];
    ID3D11Texture2D* blk[EBC_BLK];
    ID3D11UnorderedAccessView* blk_uav[EBC_BLK];
    ID3D11Buffer *cb, *nok0;             /* (nok0: the staging build's map flag goes here, not into RA.nok) */
    ID3D11UnorderedAccessView* nok0_uav;
    uint64_t bytes, nbytes, n_bc, n_rgba;
} EBC;
static int ebc_env(void) {
    if (!EBC.env) { const char* e = getenv("PS2_TEX_BC7"); EBC.env = e && *e == '0' ? -1 : 1; }
    return EBC.env > 0;
}
static int ebc_small(void) { return ra_on_now && ebc_env() && !EBC.failed; }
static int ebc_on(void) { return ebc_small() && RA.init > 0; }
static void ebc_bind_ps(void) {
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 50, 6, EBC.srv);
    if (etn_enabled()) ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 56, 6, EBC.nsrv);
}
static void ebc_free(void) {
    for (int c = 0; c < ET_NCLS; c++) {
        GPU_RELEASE(EBC.srv[c]); GPU_RELEASE(EBC.arr[c]);
        GPU_RELEASE(EBC.nsrv[c]); GPU_RELEASE(EBC.narr[c]); EBC.ncap[c] = 0;
        for (int m = 0; m < ET_MAXMIP; m++) { GPU_RELEASE(EBC.stg_uav[c][m]); GPU_RELEASE(EBC.nstg_uav[c][m]); }
        GPU_RELEASE(EBC.stg_srv[c]); GPU_RELEASE(EBC.stg[c]); GPU_RELEASE(EBC.nstg[c]);
    }
    for (int j = 0; j < EBC_BLK; j++) { GPU_RELEASE(EBC.blk_uav[j]); GPU_RELEASE(EBC.blk[j]); }
    GPU_RELEASE(EBC.cb); GPU_RELEASE(EBC.nok0_uav); GPU_RELEASE(EBC.nok0);
    EBC.bytes = EBC.nbytes = 0;
}
static uint64_t ebc_slice_bytes(int c) { uint64_t s = 64ull << c; return s * s * 4 / 3; }   /* (1 byte per texel) */
static ID3D11Texture2D* ebc_tex(uint32_t S, uint32_t nm, uint32_t n, DXGI_FORMAT f, UINT bind) {
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = td.Height = S; td.MipLevels = nm; td.ArraySize = n; td.Format = f;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind;
    ID3D11Texture2D* t = NULL;
    return SUCCEEDED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &t)) ? t : NULL;
}
static ID3D11ShaderResourceView* ebc_srv_of(ID3D11Texture2D* t, DXGI_FORMAT f, uint32_t nm, uint32_t n) {
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Format = f; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Texture2DArray.MipLevels = nm; sd.Texture2DArray.ArraySize = n;
    ID3D11ShaderResourceView* v = NULL;
    return t && SUCCEEDED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)t, &sd, &v)) ? v : NULL;
}
/* the class's BC7 array (and its maps) grown by doubling, keeping what it holds; 0: no room (the budget, video memory) */
static int ebc_grow(int c) {
    static const uint32_t first[ET_NCLS] = { 64, 256, 512, 256, 64, 0 };   /* (the RGBA8 pre-size's slices at a quarter of the bytes,
                                          * 256 x 256 doubled: a hub + the title's list - ep2 - outgrew 256 during play) */
    uint32_t oc = ET.cap[ET_BC + c], nc = oc ? oc * 2 : first[c];
    if (!nc || nc > 2048) return 0;
    if (ET.bytes + EBC.bytes + (uint64_t)(nc - oc) * ebc_slice_bytes(c) > (uint64_t)ET.budget_mb << 20) return 0;
    uint32_t S = 64u << c, nm = (uint32_t)c + 7u;
    ID3D11Texture2D* t = ebc_tex(S, nm, nc, DXGI_FORMAT_BC7_UNORM, D3D11_BIND_SHADER_RESOURCE);
    ID3D11ShaderResourceView* v = ebc_srv_of(t, DXGI_FORMAT_BC7_UNORM, nm, nc);
    ID3D11Texture2D* nt = NULL;
    ID3D11ShaderResourceView* nv = NULL;
    if (v && RA.nrm_env && ETN.on) {     /* (the maps: as etn_grow's, read-only - the staging map is copied in) */
        nt = ebc_tex(S / 2, nm - 1, nc, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        nv = ebc_srv_of(nt, DXGI_FORMAT_R8G8B8A8_UNORM, nm - 1, nc);
        if (!nv) { GPU_RELEASE(nt); }
    }
    et_slot* sl = v ? realloc(ET.sl[ET_BC + c], nc * sizeof(et_slot)) : NULL;
    if (!sl) { GPU_RELEASE(v); GPU_RELEASE(t); GPU_RELEASE(nv); GPU_RELEASE(nt); return 0; }
    memset(sl + oc, 0, (nc - oc) * sizeof(et_slot));
    for (uint32_t j = oc; j < nc; j++) sl[j].first = -1;
    ET.sl[ET_BC + c] = sl;
    if (EBC.arr[c])
        for (uint32_t j = 0; j < oc; j++)
            for (uint32_t m = 0; m < nm; m++)
                ID3D11DeviceContext_CopySubresourceRegion(gpu_rt.ctx, (ID3D11Resource*)t, m + j * nm, 0, 0, 0, (ID3D11Resource*)EBC.arr[c], m + j * nm, NULL);
    if (nt && EBC.narr[c] && EBC.ncap[c] == oc)
        for (uint32_t j = 0; j < oc; j++)
            for (uint32_t m = 0; m + 1 < nm; m++)
                ID3D11DeviceContext_CopySubresourceRegion(gpu_rt.ctx, (ID3D11Resource*)nt, m + j * (nm - 1), 0, 0, 0, (ID3D11Resource*)EBC.narr[c], m + j * (nm - 1), NULL);
    GPU_RELEASE(EBC.srv[c]); GPU_RELEASE(EBC.arr[c]); GPU_RELEASE(EBC.nsrv[c]); GPU_RELEASE(EBC.narr[c]);
    EBC.arr[c] = t; EBC.srv[c] = v; EBC.narr[c] = nt; EBC.nsrv[c] = nv;
    EBC.ncap[c] = nt ? nc : 0;
    EBC.bytes += (uint64_t)(nc - oc) * ebc_slice_bytes(c);
    EBC.nbytes = 0;
    for (int k = 0; k < ET_NCLS; k++) EBC.nbytes += (uint64_t)EBC.ncap[k] * (32ull << k) * (32ull << k) * 4u * 4u / 3u;
    ET.cap[ET_BC + c] = nc;
    ET.n_grow++;
    if (RA.log) gpu_log("ai: BC7 class %ux%u: %u slices (%.0f MB, maps %.0f MB)", S, S, nc, EBC.bytes / 1048576.0, EBC.nbytes / 1048576.0);
    return 1;
}
/* the staging arrays of class c and the block scratch (made at first use) */
static int ebc_stage(int c) {
    if (EBC.stg[c] && (!RA.nrm_env || !ETN.on || EBC.nstg[c])) return 1;
    uint32_t S = 64u << c, nm = (uint32_t)c + 7u;
    if (!EBC.stg[c]) {
        EBC.stg[c] = ebc_tex(S, nm, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        EBC.stg_srv[c] = ebc_srv_of(EBC.stg[c], DXGI_FORMAT_R8G8B8A8_UNORM, nm, 1);
        for (uint32_t m = 0; EBC.stg_srv[c] && m < nm; m++) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
            memset(&ud, 0, sizeof ud);
            ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
            ud.Texture2DArray.MipSlice = m; ud.Texture2DArray.ArraySize = 1;
            if (FAILED(ID3D11Device_CreateUnorderedAccessView(gpu_rt.dev, (ID3D11Resource*)EBC.stg[c], &ud, &EBC.stg_uav[c][m]))) return 0;
        }
        if (!EBC.stg_srv[c]) return 0;
    }
    if (RA.nrm_env && ETN.on && !EBC.nstg[c]) {
        EBC.nstg[c] = ebc_tex(S / 2, nm - 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        for (uint32_t m = 0; EBC.nstg[c] && m + 1 < nm; m++) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
            memset(&ud, 0, sizeof ud);
            ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
            ud.Texture2DArray.MipSlice = m; ud.Texture2DArray.ArraySize = 1;
            if (FAILED(ID3D11Device_CreateUnorderedAccessView(gpu_rt.dev, (ID3D11Resource*)EBC.nstg[c], &ud, &EBC.nstg_uav[c][m]))) return 0;
        }
        if (!EBC.nstg[c]) return 0;
    }
    for (int j = 0; j < EBC_BLK && !EBC.blk[j]; j++) {
        uint32_t b = 256u >> j ? 256u >> j : 1u;
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = td.Height = b; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &EBC.blk[j])) ||
            FAILED(ID3D11Device_CreateUnorderedAccessView(gpu_rt.dev, (ID3D11Resource*)EBC.blk[j], NULL, &EBC.blk_uav[j]))) return 0;
    }
    if (!EBC.cb) EBC.cb = et_buf(32, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
    if (!EBC.nok0) EBC.nok0 = raw_default(ET_NSL * 4096u * 4u, NULL, NULL, &EBC.nok0_uav);
    return EBC.cb && EBC.nok0;
}
/* a BC7 slot of class c for an AI image: a free one, the class grown (a preload: only while no 3D is drawn, as ra_alloc),
 * or the least recently used one unused for 10 s (a preload: unused since the level changed) whose entries go back to
 * unknown; -1: none */
static int32_t ebc_alloc(int c, int pre) {
    int b = ET_BC + c;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t j = 0; j < ET.cap[b]; j++) if (!ET.sl[b][j].used) return (int32_t)(b << 16 | j);
        if (pass || (pre && ET.cap[b] && RQ.uses_last) || !ebc_grow(c)) break;
    }
    uint32_t best = 0xFFFFFFFFu, bl = 0xFFFFFFFFu;
    for (uint32_t j = 0; j < ET.cap[b]; j++) {
        const et_slot* s = &ET.sl[b][j];
        int ok = pre ? s->last < RQ.wframe && s->last + 2 < ET.frame : s->last + 600 < ET.frame;
        if (ok && s->last < bl) { bl = s->last; best = j; }
    }
    if (best == 0xFFFFFFFFu) return -1;
    et_slot* s = &ET.sl[b][best];
    while (s->first >= 0) {
        int32_t k = s->first;
        et_swap_note(k, 'O');
        et_unlink(k);
        ET.e[k].st = ES_NONE;
    }
    s->used = 0;
    s->ai = 0;
    ET.n_evict++;
    return (int32_t)(b << 16 | best);
}
/* build template t's image from the upload region at src_base (words) into a new BC7 slot of class c: the usual build
 * (mips, coverage, surface map) into the staging arrays, then every level encoded and copied in. The slot (used, ai,
 * hash) is the caller's to fill. -1: no slot / resources (the caller builds an RGBA8 image instead) */
static int32_t ebc_build(const et_ent* t, int c, uint32_t src_base, int pre, uint64_t hash, int* bound) {
    if (!ebc_stage(c) || !gpu_cs(GPU_SH_cs_bc7)) { if (!EBC.failed) gpu_log("ai: BC7 resources failed: AI images stay RGBA8"); EBC.failed = 1; return -1; }
    int32_t id = ebc_alloc(c, pre);
    if (id < 0) return -1;
    ID3D11DeviceContext* x = gpu_rt.ctx;
    uint32_t S = 64u << c, nm = (uint32_t)c + 7u, slice = (uint32_t)(id & 0xFFFF);
    if (!*bound) {
        ID3D11ShaderResourceView* n1 = NULL;
        ID3D11DeviceContext_PSSetShaderResources(x, 13, 1, &n1);
        *bound = 1;
    }
    et_batch_run();                      /* (one job alone in its batch: it writes slice 0 of the staging arrays) */
    ID3D11UnorderedAccessView *sv[ET_MAXMIP], *sn[ET_MAXMIP], *sk = RA.nok_uav;
    memcpy(sv, ET.uav[c], sizeof sv);
    memcpy(sn, ETN.uav[c], sizeof sn);
    memcpy(ET.uav[c], EBC.stg_uav[c], sizeof sv);
    if (EBC.nstg[c]) memcpy(ETN.uav[c], EBC.nstg_uav[c], sizeof sn);
    RA.nok_uav = EBC.nok0_uav;
    uint32_t cap0 = ETN.cap[c];
    if (!EBC.nstg[c] || !ETN.arr[c]) ETN.cap[c] = 0;   /* (no maps: etn_run skips) */
    else if (ETN.cap[c] < ET.cap[c]) ETN.cap[c] = ET.cap[c];
    et_build(t, (int32_t)(c << 16), RA.up_srv, src_base, 1);
    et_batch_run();
    ETN.cap[c] = cap0;
    memcpy(ET.uav[c], sv, sizeof sv);
    memcpy(ETN.uav[c], sn, sizeof sn);
    RA.nok_uav = sk;
    /* the levels: one thread per block into the scratch (R32G32B32A32_UINT), copied into the BC7 slice */
    ID3D11DeviceContext_CSSetShader(x, gpu_cs(GPU_SH_cs_bc7), NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(x, 0, 1, &EBC.stg_srv[c]);
    ID3D11DeviceContext_CSSetConstantBuffers(x, 0, 1, &EBC.cb);
    for (uint32_t k = 0; k < nm; k++) {
        uint32_t s = S >> k, b = (s + 3) / 4, j = k + 4u - (uint32_t)c;
        if (j >= EBC_BLK) j = EBC_BLK - 1;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(x, (ID3D11Resource*)EBC.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            uint32_t v[8] = { 0, k, b, b, s, s, 0, 0 };
            memcpy(m.pData, v, sizeof v);
            ID3D11DeviceContext_Unmap(x, (ID3D11Resource*)EBC.cb, 0);
        }
        ID3D11DeviceContext_CSSetUnorderedAccessViews(x, 0, 1, &EBC.blk_uav[j], NULL);
        ID3D11DeviceContext_Dispatch(x, (b + 7) / 8, (b + 7) / 8, 1);
        ID3D11UnorderedAccessView* nu = NULL;
        ID3D11DeviceContext_CSSetUnorderedAccessViews(x, 0, 1, &nu, NULL);
        D3D11_BOX bx = { 0, 0, 0, b, b, 1 };
        ID3D11DeviceContext_CopySubresourceRegion(x, (ID3D11Resource*)EBC.arr[c], k + slice * nm, 0, 0, 0, (ID3D11Resource*)EBC.blk[j], 0, &bx);
    }
    ID3D11ShaderResourceView* ns = NULL;
    ID3D11Buffer* nb = NULL;
    ID3D11DeviceContext_CSSetShaderResources(x, 0, 1, &ns);
    ID3D11DeviceContext_CSSetConstantBuffers(x, 0, 1, &nb);
    gpu_rt.n_dispatches += nm;
    /* its map: the staging map's levels into the slot; the flag for the shaders */
    uint32_t has = 0;
    if (EBC.nstg[c] && EBC.narr[c] && EBC.ncap[c] > slice && ETN.on) {
        for (uint32_t m = 0; m + 1 < nm; m++)
            ID3D11DeviceContext_CopySubresourceRegion(x, (ID3D11Resource*)EBC.narr[c], m + slice * (nm - 1), 0, 0, 0, (ID3D11Resource*)EBC.nstg[c], m, NULL);
        has = 1;
    }
    if (RA.nok) {
        uint32_t o = ((uint32_t)(ET_BC + c) * 4096u + slice) * 4u;
        D3D11_BOX fb = { o, 0, 0, o + 4u, 1, 1 };
        ID3D11DeviceContext_UpdateSubresource(x, (ID3D11Resource*)RA.nok, 0, &fb, &has, 0, 0);
    }
    if (RA.dump) { et_dump_from((ID3D11Resource*)EBC.stg[c], S, 2, hash); if (has) et_dump_from((ID3D11Resource*)EBC.nstg[c], S / 2, 3, hash); }
    EBC.n_bc++;
    return id;
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
/* the net's kernels + the tiles' finish: created on the worker (the driver compiles each big kernel for tens of ms) */
static const struct { uint32_t key; int id; } ra_cs_ids[16] = {
    { 0x38810, GPU_SH_cs_ai_38810 }, { 0x88833, GPU_SH_cs_ai_88833 }, { 0x88844, GPU_SH_cs_ai_88844 }, { 0x88343, GPU_SH_cs_ai_88343 },
    { 0x88434, GPU_SH_cs_ai_88434 }, { 0x21343, GPU_SH_cs_ai_21343 }, { 0x22343, GPU_SH_cs_ai_22343 }, { 0x22434, GPU_SH_cs_ai_22434 },
    { 0x88114, GPU_SH_cs_ai_88114 }, { 0x88113, GPU_SH_cs_ai_88113 }, { 0x88851, GPU_SH_cs_ai_88851 }, { 0x88865, GPU_SH_cs_ai_88865 },
    { 0x88856, GPU_SH_cs_ai_88856 }, { 0x88875, GPU_SH_cs_ai_88875 }, { 1, GPU_SH_cs_ai_post }, { 2, GPU_SH_cs_ai_res } };
/* (tex-opt) the half-float net's (PS2_AI_HALF, default 1: ai_up.h AI_GPU_HALF_BIG | AI_GPU_HALF_W; key + 0x79 << 20) */
#define RA_HALF (AI_GPU_HALF_BIG | AI_GPU_HALF_W)
#define RA_HKEY (0x79u << 20)
static const int ra_cs_half[14] = {
    GPU_SH_cs_ai_38810h, GPU_SH_cs_ai_88833h, GPU_SH_cs_ai_88844h, GPU_SH_cs_ai_88343h, GPU_SH_cs_ai_88434h, GPU_SH_cs_ai_21343h,
    GPU_SH_cs_ai_22343h, GPU_SH_cs_ai_22434h, GPU_SH_cs_ai_88114h, GPU_SH_cs_ai_88113h, GPU_SH_cs_ai_88851h, GPU_SH_cs_ai_88865h,
    GPU_SH_cs_ai_88856h, GPU_SH_cs_ai_88875h };
/* (worker) the net's compute shaders into cs[16] (the device is free-threaded; nothing of RA is touched here: the render
 * thread installs them with the net, ra_net_poll) */
static int ra_make_cs(ID3D11ComputeShader** cs) {
    for (int k = 0; k < 16; k++) {
        const gpu_shader_blob* b = &gpu_shaders[RA.half && k < 14 ? ra_cs_half[k] : ra_cs_ids[k].id];
        if (FAILED(ID3D11Device_CreateComputeShader(gpu_rt.dev, b->code, b->size, NULL, &cs[k]))) {
            for (int j = 0; j < 16; j++) GPU_RELEASE(cs[j]);
            return 0;
        }
    }
    return 1;
}
static void ra_init_net(void) {
    wchar_t wp[700] = L"";
    const char* e = getenv("PS2_AI_WEIGHTS");
    char path[800] = "";
    if (e && *e) snprintf(path, sizeof path, "%s", e);
    else {
        GetModuleFileNameW(NULL, wp, 600);
        wchar_t* sl = wcsrchr(wp, L'\\');
        if (sl) { wcscpy(sl + 1, L"models\\x4plus.bin"); }
        if (sl && GetFileAttributesW(wp) == INVALID_FILE_ATTRIBUTES) wcscpy(sl + 1, L"x4plus.bin");   /* (a 1.0.x launcher's update) */
        if (GetFileAttributesW(wp) == INVALID_FILE_ATTRIBUTES && settings_user_path)
            settings_user_path(L"models\\x4plus.bin", wp, 600);
        WideCharToMultiByte(CP_UTF8, 0, wp, -1, path, sizeof path, NULL, NULL);
    }
    char err[256] = "";
    ai_weights* w = ai_weights_load(path, err, sizeof err);
    ai_gpu* g = NULL;
    /* (reimagine-crash) everything is made through the device (free-threaded) into locals and handed over under the lock;
     * the render thread installs it (ra_net_poll). The immediate context is never touched here (ai_gpu_create_cs only
     * keeps the pointer for the runs, which happen on the render thread), and no RA field the render thread uses or
     * releases (ra_release on a Look switch) is written from this thread */
    ID3D11ComputeShader* cs[16] = { NULL };
    ID3D11Buffer* src[RA_SRCN] = { NULL };
    ID3D11Buffer* sav[RA_SAVEN] = { NULL };
    if (w && !ra_make_cs(cs)) { snprintf(err, sizeof err, "cannot create its shaders"); ai_weights_free(w); w = NULL; }
    if (w) {
        size_t P = (size_t)(RA_TILE + 2 * RA.pad) * (RA_TILE + 2 * RA.pad);
        g = ai_gpu_create_cs(gpu_rt.dev, gpu_rt.ctx, w, ra_cs_of, NULL, P, RA.half ? RA_HALF : 0, err, sizeof err);
        ai_weights_free(w);
        for (int k = 0; g && k < RA_SRCN; k++)   /* (the read-back buffers too: not created on the render thread) */
            src[k] = et_buf(256u * 256u * 4u, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
        for (int k = 0; g && k < RA_SAVEN; k++)
            sav[k] = et_buf(RA_UP_WORDS, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    }
    if (!g) {
        for (int k = 0; k < 16; k++) GPU_RELEASE(cs[k]);
        for (int k = 0; k < RA_SRCN; k++) GPU_RELEASE(src[k]);
        for (int k = 0; k < RA_SAVEN; k++) GPU_RELEASE(sav[k]);
    }
    EnterCriticalSection(&RA.cs);
    RA.net_new = g;
    memcpy(RA.kcs_new, cs, sizeof cs);
    memcpy(RA.src_new, src, sizeof src);
    memcpy(RA.sav_new, sav, sizeof sav);
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

/* ---- (tex-prebuild) level lists and the loader threads ------------------------------------------------------------------- */
static void ra_level_path(uint32_t world, wchar_t* out, size_t cap, int tmp) {
    _snwprintf(out, cap, L"%ls\\levels\\w%u.bin%ls", RA.dir, world, tmp ? L".tmp" : L"");
    out[cap - 1] = 0;
}
/* (worker, RJ_MANI) a level's list: "SLYAIL01", count, records (ra_rec); .tmp + rename */
static void ra_level_write(uint32_t world, const ra_rec* r, uint32_t n) {
    wchar_t dir[640], path[700], tmp[700];
    _snwprintf(dir, 640, L"%ls\\levels", RA.dir);
    dir[639] = 0;
    CreateDirectoryW(dir, NULL);
    if (world == 0xFFFFFFFFu) {          /* (the level played last: its world id) */
        _snwprintf(path, 700, L"%ls\\last.bin", dir);
        path[699] = 0;
        FILE* f = _wfopen(path, L"wb");
        if (f) { fwrite(r, 4, 1, f); fclose(f); }
        return;
    }
    ra_level_path(world, path, 700, 0);
    ra_level_path(world, tmp, 700, 1);
    FILE* f = _wfopen(tmp, L"wb");
    if (!f) return;
    int ok = fwrite("SLYAIL01", 1, 8, f) == 8 && fwrite(&n, 4, 1, f) == 1 && fwrite(r, sizeof *r, n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp);
}
/* (loader) a level's list, or NULL */
static ra_rec* ra_level_read(uint32_t world, uint32_t* n) {
    wchar_t path[700];
    ra_level_path(world, path, 700, 0);
    *n = 0;
    FILE* f = _wfopen(path, L"rb");
    if (!f) return NULL;
    char m[8];
    uint32_t c = 0;
    ra_rec* r = NULL;
    if (fread(m, 1, 8, f) == 8 && !memcmp(m, "SLYAIL01", 8) && fread(&c, 4, 1, f) == 1 && c && c <= RA_RECN &&
        (r = malloc((size_t)c * sizeof *r)) != NULL && fread(r, sizeof *r, c, f) == c)
        *n = c;
    else { free(r); r = NULL; }
    fclose(f);
    return r;
}

/* (tex-opt) the alpha test (key bit 60) does not change an AI image (only the coverage of its mips, built at load): a
 * texture drawn with and without it shares one cache file - found under the other key, never built twice (the caches:
 * ~1 % of the files were such pairs, all identical) */
#define RA_REFBIT (1ull << 60)
static uint64_t n_alias;
static int ra_file_of(ra_item* it) {
    if (it->bad) return 0;
    if (it->file || ra_have(it->hash)) return 1;
    if (ra_have(it->hash ^ RA_REFBIT)) { it->alias = 1; n_alias++; return 1; }
    return 0;
}
static uint64_t ra_fkey(const ra_item* it) { return it->hash ^ (it->alias ? RA_REFBIT : 0ull); }
/* queue item i for a loader (q 0: on demand, 1: preload); its state is RI_LOAD */
static void ra_load_push(int32_t i, int q) {
    EnterCriticalSection(&RA.cs);
    if (RQ.qh[q] - RQ.qt[q] < RA_LQ) RQ.q[q][RQ.qh[q]++ % RA_LQ] = i;
    else RA.it[i].st = RI_NONE;          /* (full: asked again at its next use) */
    LeaveCriticalSection(&RA.cs);
    SetEvent(RQ.ev);
}
static DWORD WINAPI ra_loader(LPVOID unused) {
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
    for (;;) {
        WaitForSingleObject(RQ.ev, 20);
        for (;;) {
            int32_t i = -1;
            uint8_t gen = 0;
            uint32_t mw = 0;
            int mani = 0;
            EnterCriticalSection(&RA.cs);
            if (RQ.mstate == 1) { RQ.mstate = 2; mw = RQ.mreq; mani = 1; }
            else if (RQ.bytes < RA_LBYTES && RA.nloaded < 512 - RA_LDN)
                for (int q = 0; q < 2 && i < 0; q++)
                    while (RQ.qt[q] != RQ.qh[q] && i < 0) {
                        int32_t k = RQ.q[q][RQ.qt[q]++ % RA_LQ];
                        if (RA.it && RA.it[k].st == RI_LOAD) { i = k; RA.it[k].st = RI_DECODE; gen = RA.it[k].gen; }   /* (stale / repeated requests skipped) */
                    }
            int more = RQ.mstate == 1 || RQ.qt[0] != RQ.qh[0] || RQ.qt[1] != RQ.qh[1];
            LeaveCriticalSection(&RA.cs);
            if (mani) {
                uint32_t n = 0;
                ra_rec* r = ra_level_read(mw, &n);
                uint32_t n2 = 0, lw = 0;
                ra_rec* r2 = NULL;
                if (mw == 0) {               /* (the title: the level played last comes next, most likely) */
                    wchar_t lp[700];
                    _snwprintf(lp, 700, L"%ls\\levels\\last.bin", RA.dir);
                    lp[699] = 0;
                    FILE* lf = _wfopen(lp, L"rb");
                    if (lf) { if (fread(&lw, 4, 1, lf) != 1) lw = 0; fclose(lf); }
                    if (lw && lw < 0xFFFFFFF0u) r2 = ra_level_read(lw, &n2);
                }
                EnterCriticalSection(&RA.cs);
                if (RQ.mstate == 2 && RQ.mreq == mw) {
                    free(RQ.mbuf); RQ.mbuf = r; RQ.mn = n;
                    free(RQ.mbuf2); RQ.mbuf2 = r2; RQ.mn2 = n2; RQ.last_world = lw;
                    RQ.mstate = 3;
                } else { free(r); free(r2); }   /* (the level changed again meanwhile) */
                LeaveCriticalSection(&RA.cs);
                continue;
            }
            if (i < 0) break;
            if (more) SetEvent(RQ.ev);       /* (the other loaders) */
            ra_item* it = &RA.it[i];
            et_hdjob hj;
            memset(&hj, 0, sizeof hj);
            hj.raw = it->hash;
            wchar_t path[700];
            ra_path(ra_fkey(it), 0, path, 700);
            int ok = f && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && et_hd_decode(f, path, &hj);
            if (!ok && f) { ra_path(ra_fkey(it), 1, path, 700); ok = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && et_hd_decode(f, path, &hj); }
            int op = ok && hj.px;            /* (tex-opt) no alpha below 0x80: a BC7 image */
            for (size_t q = 0; op && q < (size_t)hj.w * hj.h; q++) op = (hj.px[q] >> 24) == 0x80u;
            EnterCriticalSection(&RA.cs);
            if (it->st == RI_DECODE && it->gen == gen) {   /* (else ra_release reset it meanwhile) */
                if (ok && hj.w == (4u << it->lw) && hj.h == (4u << it->lh)) {
                    it->rgba = hj.px; it->rw = hj.w; it->rh = hj.h; it->st = RI_LOADED; it->opaque = (uint8_t)op;
                    RQ.bytes += (uint64_t)hj.w * hj.h * 4u;
                    hj.px = NULL;
                } else it->st = RI_LFAIL;    /* (a bad / missing / foreign file: built again) */
                RA.loaded[RA.nloaded++] = i;
            }
            LeaveCriticalSection(&RA.cs);
            free(hj.px);
        }
    }
    return 0;
}
static int ra_ready_on(void) {
    if (RQ.on < 0) { const char* e = getenv("PS2_AI_READY"); RQ.on = !(e && *e == '0'); }
    return RQ.on;
}
static void ra_loaders_start(void) {
    if (RQ.nth || !ra_ready_on()) return;
    RQ.ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors / 3;
    n = n < 2 ? 2 : n > RA_LDN ? RA_LDN : n;
    for (int k = 0; RQ.ev && k < n; k++) {
        RQ.th[k] = CreateThread(NULL, 0, ra_loader, NULL, 0, NULL);
        if (RQ.th[k]) RQ.nth++;
    }
    if (!RQ.nth) { gpu_log("ai: no loader threads: the cache is read by the worker"); RQ.on = 0; }
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
            else if (j.type == RJ_MANI) ra_level_write(j.w, (const ra_rec*)j.data, j.h);   /* (tex-prebuild) */
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
                ra_path(ra_fkey(it), 0, path, 700);
                int ok = f && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && et_hd_decode(f, path, &hj);
                if (!ok && f) { ra_path(ra_fkey(it), 1, path, 700); ok = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && et_hd_decode(f, path, &hj); }
                int op = ok && hj.px;        /* (tex-opt) no alpha below 0x80: a BC7 image */
                for (size_t q = 0; op && q < (size_t)hj.w * hj.h; q++) op = (hj.px[q] >> 24) == 0x80u;
                EnterCriticalSection(&RA.cs);
                if (ok && hj.w == (4u << it->lw) && hj.h == (4u << it->lh) && RA.nloaded < 512 - RA_LDN) {
                    it->rgba = hj.px; it->rw = hj.w; it->rh = hj.h; it->st = RI_LOADED; it->opaque = (uint8_t)op;
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
    (void)user;                          /* (shaders/perms.txt cs_ai_*: every buffer combination ai_up.c's plan uses) */
    for (int k = 0; k < 14; k++) if ((ra_cs_ids[k].key | (RA.half ? RA_HKEY : 0u)) == key) return RA.kcs[k];
    return NULL;
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
    /* (tex-prebuild) built in a local buffer: the loaders and the worker read RA.dir while Reimagined turns on again */
    wchar_t dir[600];
    if (!settings_user_path || !settings_user_path(L"textures", dir, 400)) { RA.dir[0] = 0; return; }
    CreateDirectoryW(dir, NULL);
    wcscat(dir, L"\\reimagined");
    CreateDirectoryW(dir, NULL);
    size_t l = wcslen(dir);
    _snwprintf(dir + l, 600 - l, L"\\x4plus-v%d", RA_VER);
    dir[599] = 0;
    CreateDirectoryW(dir, NULL);
    if (wcscmp(dir, RA.dir)) wcscpy(RA.dir, dir);
    wchar_t pat[620];
    _snwprintf(pat, 620, L"%ls\\*.*", RA.dir);
    pat[619] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    /* (reimagine-crash) the list is read again at every Reimagined on (Look switched back): it starts empty and grows from
     * its own capacity. 0.1.1226 kept the old count with a capacity of 0 here, so the second listing appended past the end
     * of the first one's buffer (owner: 637 entries in a 1024-entry buffer, then 1348 more written after them): heap
     * corruption -> 0xC0000005 in ntdll / the driver soon after "Reimagined textures on" */
    RA.nhave = 0;
    int cap = RA.have_cap;
    uint64_t bytes = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcslen(fd.cFileName) != 20 || (_wcsicmp(fd.cFileName + 16, L".jpg") && _wcsicmp(fd.cFileName + 16, L".png"))) continue;
            wchar_t* end = NULL;
            uint64_t v = _wcstoui64(fd.cFileName, &end, 16);
            if (end != fd.cFileName + 16) continue;
            if (RA.nhave >= cap) {
                int nc = cap ? cap * 2 : 256;
                uint64_t* a = realloc(RA.have, (size_t)nc * sizeof *a);
                if (!a) break;
                RA.have = a; cap = RA.have_cap = nc;
            }
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
    if (!(RA.nok = raw_default(ET_NSL * 4096u * 4u, NULL, &RA.nok_srv, &RA.nok_uav))) return 0;   /* (tex-opt: + the BC7 classes) */
    RA.pcb = et_buf(32, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
    if (!RA.pcb || !gpu_cs(GPU_SH_cs_etn)) return 0;
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
    ra_loaders_start();                  /* (tex-prebuild) */
    RQ.world = 0xFFFFFFFEu;              /* (the level's list is read at the next frame) */
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
        double need = RA.half ? P * 5.6 * 1024.0 / 1048576.0 + 32.0 : P * 10.5 * 1024.0 / 1048576.0 + 64.0;   /* (tex-opt: half ~159 MB) */
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
    ID3D11ComputeShader* cs[16];
    ID3D11Buffer* src[RA_SRCN];
    ID3D11Buffer* sav[RA_SAVEN];
    memcpy(cs, RA.kcs_new, sizeof cs); memset(RA.kcs_new, 0, sizeof RA.kcs_new);
    memcpy(src, RA.src_new, sizeof src); memset(RA.src_new, 0, sizeof RA.src_new);
    memcpy(sav, RA.sav_new, sizeof sav); memset(RA.sav_new, 0, sizeof RA.sav_new);
    LeaveCriticalSection(&RA.cs);
    /* (reimagine-crash) the worker's objects installed here, on the render thread */
    for (int k = 0; k < 16; k++) if (cs[k]) { GPU_RELEASE(RA.kcs[k]); RA.kcs[k] = cs[k]; }
    for (int k = 0; k < RA_SRCN; k++) if (src[k]) { if (!RA.src[k].st) RA.src[k].st = src[k]; else GPU_RELEASE(src[k]); }
    for (int k = 0; k < RA_SAVEN; k++) if (sav[k]) { if (!RA.sav[k].st) RA.sav[k].st = sav[k]; else GPU_RELEASE(sav[k]); }
    if (g) {
        if (RA.net) ai_gpu_destroy(RA.net);   /* (not expected: one net at a time) */
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

/* ---- (tex-prebuild) render thread: direct images, level lists, preloads, deferred swaps ------------------------------------ */
/* a draw used slot s's image in the last 2 frames (3D is being drawn): replacing it now would show */
static int ra_visible(const et_slot* s) { return RQ.uses_last && s->first >= 0 && s->last + 2 >= ET.frame; }
static void ra_defer_add(int32_t i) {
    RQ.n_defer++;
    if (RQ.ndefer < RA_DEFN) { RA.it[i].st = RI_DEFER; RQ.defer[RQ.ndefer++] = i; }
    else RA.it[i].st = RI_NONE;          /* (full: noted again at a later link) */
}
/* (et_resolve) entry idx has no image yet: it waits for its AI image when the cache has one (1) */
static int ra_direct(int32_t idx) {
    if (RQ.on <= 0 || !RQ.nth || !RA.it) return 0;
    et_ent* e = &ET.e[idx];
    if ((e->lw > e->lh ? e->lw : e->lh) > 8) return 0;   /* (sources above 256 texels: Enhanced) */
    int32_t i = ra_find(e->hash, 1);
    if (i < 0) return 0;
    ra_item* it = &RA.it[i];
    if (!ra_file_of(it)) return 0;
    switch (it->st) {
    case RI_NONE: case RI_DONE: case RI_DEFER:
        it->lw = e->lw; it->lh = e->lh; it->flags = e->flags; it->ref = e->ref;
        it->file = 1; it->pre = 0; it->st = RI_LOAD;
        ra_load_push(i, 0);
        break;
    case RI_LOAD:
        if (it->pre) { it->pre = 0; ra_load_push(i, 0); }   /* (a queued preload: now on demand) */
        break;
    case RI_DECODE: case RI_LOADED: break;
    default: return 0;                   /* (excluded, failed, or the net is on it: Enhanced first) */
    }
    e->st = ES_AILOAD;
    e->wait = ET.frame;
    it->waiters = 1;
    RQ.n_wait++;
    RQ.n_direct++;
    return 1;
}
/* entries waiting for item i's image: linked to slot id, or (id < 0) back to an Enhanced build */
static void ra_waiters(int32_t i, int32_t id) {
    ra_item* it = &RA.it[i];
    if (!it->waiters) return;
    it->waiters = 0;
    for (int32_t k = 0; k < TD_HASH; k++) {
        et_ent* e = &ET.e[k];
        if (e->st != ES_AILOAD || e->hash != it->hash) continue;
        if (RQ.n_wait) RQ.n_wait--;
        if (id >= 0) et_link(k, id);
        else if (ET.qn < TD_HASH) { ET.q[ET.qn++] = k; e->st = ES_QUEUED; }
        else e->st = ES_NONE;
    }
}
/* entries waiting too long (a loader backlog): an Enhanced image after all (its AI image then comes as a deferred swap) */
static void ra_wait_poll(void) {
    if (!RQ.n_wait || ET.frame % 16) return;
    uint32_t n = 0;
    for (int32_t k = 0; k < TD_HASH; k++) {
        et_ent* e = &ET.e[k];
        if (e->st != ES_AILOAD) continue;
        if (ET.frame - e->wait < 360) { n++; continue; }
        if (ET.qn < TD_HASH) { ET.q[ET.qn++] = k; e->st = ES_QUEUED; } else e->st = ES_NONE;
        RQ.n_timeout++;
    }
    RQ.n_wait = n;
}
/* a slot of class c for a new AI image: et_alloc's for a texture in use; for a preload a free slot (a class not made
 * yet is made at its first size - no copy; a full one grows only while no 3D is drawn: the boot, a loading screen - never
 * in play, where a growth's copy would cost a frame), else one of the previous level's (unused since the level changed),
 * never an image of this level */
static int32_t ra_alloc(int c, int pre) {
    if (!pre) return et_alloc(c);
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t j = 0; j < ET.cap[c]; j++) if (!ET.sl[c][j].used) return (int32_t)(c << 16 | j);
        if (pass || !(ET.cap[c] == 0 || !RQ.uses_last) || !et_grow(c)) break;
    }
    uint32_t best = 0xFFFFFFFFu, bl = 0xFFFFFFFFu;
    for (uint32_t j = 0; j < ET.cap[c]; j++) {
        const et_slot* s = &ET.sl[c][j];
        if (s->last < RQ.wframe && s->last + 2 < ET.frame && s->last < bl) { bl = s->last; best = j; }
    }
    if (best == 0xFFFFFFFFu) {
        static uint32_t told;
        if ((RA.log || RP.on > 0) && !(told & 1u << c)) { told |= 1u << c; gpu_log("ai-pre: frame %u: no image slot of class %u (%u) for a preload", ET.frame, 64u << c, ET.cap[c]); }
        return -1;
    }
    et_slot* s = &ET.sl[c][best];
    while (s->first >= 0) {              /* (as et_alloc: its entries go back to unknown) */
        int32_t k = s->first;
        et_swap_note(k, 'O');
        et_unlink(k);
        ET.e[k].st = ES_NONE;
    }
    s->used = 0;
    s->ai = 0;
    ET.n_evict++;
    return (int32_t)(c << 16 | best);
}
/* item i's decoded image into a new slot (no image of its texture exists): 1 built, 0 no slot, -1 no room this frame */
static int ra_build_new(int32_t i, int* bound) {
    ra_item* it = &RA.it[i];
    uint32_t S = 4u << (it->lw > it->lh ? it->lw : it->lh);
    int cls = et_class_of(0, 0, (int)(S < 64 ? 64 : S));
    uint32_t words = it->rw * it->rh;
    int32_t r = ra_reg_alloc(words, i);
    if (r < 0) return -1;
    et_ent t;
    memset(&t, 0, sizeof t);
    t.lw = (uint8_t)(it->lw + 2); t.lh = (uint8_t)(it->lh + 2); t.flags = it->flags; t.ref = it->ref;
    D3D11_BOX bx = { RA.reg[r].at * 4u, 0, 0, (RA.reg[r].at + words) * 4u, 1, 1 };
    int32_t id = -1;
    if (ebc_on() && it->opaque) {        /* (tex-opt) an opaque image: into a BC7 slot */
        ID3D11DeviceContext_UpdateSubresource(gpu_rt.ctx, (ID3D11Resource*)RA.up, 0, &bx, it->rgba, 0, 0);
        id = ebc_build(&t, cls, RA.reg[r].at, it->pre, it->hash, bound);
        if (id >= 0) {
            et_slot* s = et_slot_of(id);
            s->used = 1; s->hd = 0; s->ai = 1; s->hash = it->hash; s->first = -1;
            s->last = ET.frame + (it->pre ? RA_PRE_GRACE : 0u);
        }
    }
    if (id < 0) {
        id = ra_alloc(cls, it->pre);
        if (id < 0) { ra_reg_free(r); return 0; }
        ID3D11DeviceContext_UpdateSubresource(gpu_rt.ctx, (ID3D11Resource*)RA.up, 0, &bx, it->rgba, 0, 0);
        et_slot* s = et_slot_of(id);
        s->used = 1; s->hd = 0; s->ai = 1; s->hash = it->hash; s->first = -1;
        s->last = ET.frame + (it->pre ? RA_PRE_GRACE : 0u);
        if (!*bound) {
            ID3D11ShaderResourceView* n1 = NULL;
            ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 13, 1, &n1);
            *bound = 1;
        }
        if (!et_build(&t, id, RA.up_srv, RA.reg[r].at, 1)) { et_batch_run(); et_build(&t, id, RA.up_srv, RA.reg[r].at, 1); }
        et_batch_run();
        EBC.n_rgba++;
        if (RA.dump) { et_dump_ai(id, it->hash); et_dump_nrm(id, it->hash); }
    }
    et_map_put(it->hash, id);
    ra_reg_free(r);
    if (it->pre) RQ.n_pre_built++;
    ra_waiters(i, id);
    return 1;
}
/* deferred AI images whose texture is out of sight now: loaded again from the cache (the file was written meanwhile) */
static void ra_defer_poll(void) {
    uint32_t k = 0, pushed = 0;
    while (k < RQ.ndefer) {
        int32_t i = RQ.defer[k];
        ra_item* it = &RA.it[i];
        int drop = it->st != RI_DEFER;
        if (!drop) {
            et_slot* s = et_slot_of(et_map_find(it->hash));
            if (!s || s->ai || s->hd) { it->st = s && s->ai ? RI_DONE : RI_NONE; drop = 1; }
            else if (pushed < 32 && !ra_visible(s) && ra_file_of(it)) {
                it->pre = 0; it->st = RI_LOAD;
                ra_load_push(i, 0);
                pushed++; RQ.n_defer_done++; drop = 1;
            }
        }
        if (drop) RQ.defer[k] = RQ.defer[--RQ.ndefer]; else k++;
    }
}
/* the level's list: a texture shown in it (once per level) */
static int ra_rec_add(const ra_rec* r) {
    if (!RQ.set) {
        RQ.set = calloc(2u * RA_RECN, sizeof *RQ.set);
        RQ.rec = malloc(RA_RECN * sizeof *RQ.rec);
        if (!RQ.set || !RQ.rec) { free(RQ.set); free(RQ.rec); RQ.set = NULL; RQ.rec = NULL; return 0; }
    }
    if (RQ.nrec >= RA_RECN) return 0;
    uint64_t k = r->key ? r->key : 1;
    uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 50) & (2u * RA_RECN - 1);
    while (RQ.set[h]) { if (RQ.set[h] == k) return 0; h = (h + 1) & (2u * RA_RECN - 1); }
    RQ.set[h] = k;
    RQ.rec[RQ.nrec++] = *r;
    return 1;
}
static void ra_level_flush(void) {       /* (written by the worker) */
    /* (not while its file is being read: the merge is not in yet and the write would lose it) */
    if (!RQ.dirty || RQ.world >= 0xFFFFFFF0u || !RQ.nrec || !RA.dir[0] || RA.inited_thread <= 0 || RQ.mstate) return;
    ra_rec* c = malloc((size_t)RQ.nrec * sizeof *c);
    if (!c) return;
    memcpy(c, RQ.rec, (size_t)RQ.nrec * sizeof *c);
    ra_job j = { RJ_MANI, -1, (uint32_t*)c, RQ.world, RQ.nrec, 0, 0 };
    ra_push(j);
    RQ.dirty = 0;
    RQ.saved = ET.frame;
}
static int ra_vram(double* use, double* budget);
static void ra_pre_line(const char* why) {
    double use = 0, bud = 0;
    ra_vram(&use, &bud);
    gpu_log("ai-pre: %sframe %u level %u: %u listed (%u new); preloads queued %llu, built %llu, no slot %llu; direct %llu, waiting %u, "
            "timed out %llu, bad files %llu; deferred swaps %llu (%llu out of sight since, %u waiting); video memory %.0f of %.0f MB "
            "(images %.0f MB, surface maps %.0f MB; BC7 images %.0f MB + maps %.0f MB, %llu BC7 / %llu RGBA8 AI images built, "
            "%llu shared alpha-test files); "
            "cached images built at %.2f ms per M texels", why, ET.frame, RQ.world,
            RQ.nrec, (unsigned)RQ.n_new, (unsigned long long)RQ.n_pre_q, (unsigned long long)RQ.n_pre_built,
            (unsigned long long)RQ.n_pre_noslot, (unsigned long long)RQ.n_direct, RQ.n_wait, (unsigned long long)RQ.n_timeout,
            (unsigned long long)RQ.n_lfail, (unsigned long long)RQ.n_defer, (unsigned long long)RQ.n_defer_done, RQ.ndefer, use, bud,
            ET.bytes / 1048576.0, ETN.bytes / 1048576.0, EBC.bytes / 1048576.0, EBC.nbytes / 1048576.0,
            (unsigned long long)EBC.n_bc, (unsigned long long)EBC.n_rgba, (unsigned long long)n_alias, RQ.ms_per_m);
}
/* (ra_frame) the level: a change reads its list and queues its images; the list is written now and then */
static void ra_level_frame(uint32_t uses) {
    RQ.uses_last = uses;
    if (RQ.on <= 0 || !RQ.nth) return;
    uint32_t w = ra_world_now();
    if (w != RQ.world) {
        if (RA.log || RP.on > 0) ra_pre_line("level left: ");
        ra_level_flush();
        if (w && w < 0xFFFFFFF0u && RA.dir[0] && RA.inited_thread > 0) {   /* (the level played last: levels\\last.bin) */
            uint32_t* lw = malloc(4);
            if (lw) { *lw = w; ra_job j = { RJ_MANI, -1, lw, 0xFFFFFFFFu, 0, 0, 0 }; ra_push(j); }
        }
        RQ.world = w; RQ.wframe = ET.frame; RQ.nrec = 0; RQ.n_new = 0;
        if (RQ.set) memset(RQ.set, 0, 2u * RA_RECN * sizeof *RQ.set);
        free(RQ.pre); RQ.pre = NULL; RQ.npre = RQ.ipre = 0;
        EnterCriticalSection(&RA.cs);
        RQ.mreq = w;
        RQ.mstate = w < 0xFFFFFFF0u && RA.dir[0] ? 1 : 0;
        LeaveCriticalSection(&RA.cs);
        SetEvent(RQ.ev);
    }
    if (RQ.mstate == 3) {
        EnterCriticalSection(&RA.cs);
        ra_rec* r = RQ.mbuf;
        uint32_t n = RQ.mn;
        ra_rec* r2 = RQ.mbuf2;
        uint32_t n2 = RQ.mn2, lw = RQ.last_world;
        RQ.mbuf = NULL; RQ.mbuf2 = NULL; RQ.mstate = 0;
        LeaveCriticalSection(&RA.cs);
        for (uint32_t k = 0; r && k < n; k++) ra_rec_add(&r[k]);
        if (r2 && n2) {                  /* (the preloads: the title's, then the last level's - not in the title's list) */
            ra_rec* c = realloc(r, ((size_t)n + n2) * sizeof *c);
            if (c) { memcpy(c + n, r2, (size_t)n2 * sizeof *c); r = c; n += n2; }
        }
        free(r2);
        RQ.pre = r; RQ.npre = n; RQ.ipre = 0;
        if (RA.log || RP.on > 0) gpu_log("ai-pre: frame %u level %u: %u textures in its list (+ %u of level %u, played last)", ET.frame, w,
                                         n - n2, n2, lw);
    }
    for (uint32_t m = 0; RQ.pre && RQ.ipre < RQ.npre && m < 256; m++) {   /* (its images to the loaders, in the order seen) */
        const ra_rec* r = &RQ.pre[RQ.ipre++];
        if (r->lw > 8 || r->lh > 8) continue;
        et_slot* s = et_slot_of(et_map_find(r->key));
        if (s && (s->ai || s->hd)) {          /* (resident already: kept from reuse for a while) */
            if (s->last < ET.frame + RA_PRE_GRACE) s->last = ET.frame + RA_PRE_GRACE;
            continue;
        }
        int32_t i = ra_find(r->key, 1);
        if (i < 0) continue;
        ra_item* it = &RA.it[i];
        if (it->st != RI_NONE && it->st != RI_DONE) continue;
        if (!ra_file_of(it)) continue;
        it->lw = r->lw; it->lh = r->lh; it->flags = r->flags; it->ref = r->ref;
        it->file = 1; it->pre = 1; it->st = RI_LOAD;
        ra_load_push(i, 1);
        RQ.n_pre_q++;
    }
    if (RQ.dirty && ET.frame - RQ.saved > 3600) ra_level_flush();
    if ((RA.log || RP.on > 0) && ET.frame % 1200 == 0) ra_pre_line("");
}

/* build the image in upload region r over the Enhanced image of item i (its slot); -1: not now (it is shown: deferred) */
static int ra_apply(int32_t i, int* bound) {
    ra_item* it = &RA.it[i];
    int32_t id = et_map_find(it->hash);
    et_slot* s = et_slot_of(id);
    if (!s || !s->used || s->hd || s->hash != it->hash) return 0;
    if (RQ.on > 0 && ra_visible(s)) return -1;
    et_ent t;
    memset(&t, 0, sizeof t);
    t.lw = (uint8_t)(it->lw + 2); t.lh = (uint8_t)(it->lh + 2); t.flags = it->flags; t.ref = it->ref;
    if (ebc_on() && it->opaque && (id >> 16) < ET_BC) {   /* (tex-opt) into a BC7 slot; the entries move there */
        int32_t nid = ebc_build(&t, id >> 16, RA.reg[it->reg].at, 0, it->hash, bound);
        if (nid >= 0) {
            et_slot* ns = et_slot_of(nid);
            ra_pop_swap(s);
            ns->used = 1; ns->hd = 0; ns->ai = 1; ns->hash = s->hash; ns->first = -1; ns->last = s->last;
            while (s->first >= 0) {
                int32_t k = s->first;
                et_ent* e = &ET.e[k];
                s->first = e->next;
                e->slot = nid; e->next = ns->first; ns->first = k;
                et_tab_write(e->base, e->base + 1, et_info(nid));
                et_swap_note(k, 'A');
                if (ESW.on > 0) ESW.prev[k] = nid;   /* (the same image, moved: no change seen at its next link) */
            }
            s->used = 0; s->ai = 0; s->hash = 0;   /* (the Enhanced image's slot is free again) */
            et_map_put(ns->hash, nid);
            return 1;
        }
    }
    if (!*bound) {
        ID3D11ShaderResourceView* n1 = NULL;
        ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 13, 1, &n1);
        *bound = 1;
    }
    if (!et_build(&t, id, RA.up_srv, RA.reg[it->reg].at, 1)) { et_batch_run(); et_build(&t, id, RA.up_srv, RA.reg[it->reg].at, 1); }
    et_batch_run();
    if (ebc_on()) EBC.n_rgba++;
    ra_pop_swap(s);
    s->ai = 1;
    for (int32_t k = s->first; k >= 0; k = ET.e[k].next) et_swap_note(k, 'A');   /* (reimagine: Enhanced -> AI, once) */
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
        if (RQ.on && RQ.nth) {               /* (tex-prebuild) through the loaders; a bad file is built again */
            if (ra_file_of(it)) { it->file = 1; it->pre = 0; it->st = RI_LOAD; ra_load_push(i, 0); }
            else it->st = RI_WANT;
        } else if (it->file || ra_have(it->hash)) {
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
    if (et_fix_on() && lim > 0.8 * 1000.0 / (hz > 1.0 ? hz : 60.0) * 0.25) lim = 0.8 * 1000.0 / (hz > 1.0 ? hz : 60.0) * 0.25;   /* (reimagine: at most a quarter of the frame) */
    if (ET.out_frame) lim -= (et_fix_on() ? ETB.ms_per_m : 1.2) * ET.out_frame / 1048576.0;   /* (this frame's Enhanced builds) */
    double mn = et_fix_on() ? 0.25 : RA.ms_min;   /* (reimagine: a frame already over its interval gets a smaller floor) */
    return lim < mn ? mn : lim;
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
        ID3D11DeviceContext_CSSetShader(c, RA.kcs[15], NULL, 0);
        ID3D11DeviceContext_Dispatch(c, (pw + 7) / 8, (ph + 7) / 8, (UINT)RA.bn);
        gpu_rt.n_dispatches++;
    }
    ID3D11DeviceContext_CSSetShader(c, RA.kcs[14], NULL, 0);
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
        if (et_fix_on() && ET.out_frame && ET.out_frame >= ETB.cap) break;   /* (reimagine: within the frame's build time) */
        uint32_t words = 16u << (it->lw + it->lh);
        if (!RA.sav[sv].st) RA.sav[sv].st = et_buf(RA_UP_WORDS / 4u * 4u, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
        int ap = ra_apply(i, bound);
        if (RA.sav[sv].st && words <= RA_UP_WORDS / 4u) {
            D3D11_BOX bx = { RA.reg[r].at * 4u, 0, 0, (RA.reg[r].at + words) * 4u, 1, 1 };
            ID3D11DeviceContext_CopySubresourceRegion(gpu_rt.ctx, (ID3D11Resource*)RA.sav[sv].st, 0, 0, 0, 0, (ID3D11Resource*)RA.up, 0, &bx);
            RA.sav[sv].item = i; RA.sav[sv].frame = ET.frame; RA.sav[sv].words = words;
        }
        ra_reg_free(it->reg);
        it->reg = -1;
        if (ap < 0) ra_defer_add(i);     /* (tex-prebuild: shown right now - from its cache file once out of sight) */
        else it->st = RI_DONE;
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
    uint32_t uses = ET.uses;             /* (tex-prebuild) the last frame's art uses: 0 = no 3D (loading, menus) */
    ET.uses = 0;
    ra_pop_frame(uses);
    if (!ra_init()) {
        if (!RA.failed) { gpu_log("ai: cannot create the Reimagined textures' resources: Faithful's textures"); RA.failed = 1; }
        return;
    }
    ra_level_frame(uses);
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
    if (RQ.on > 0 && RQ.nth) {           /* (tex-prebuild) */
        ra_wait_poll();
        if (RQ.ndefer) ra_defer_poll();
        /* the builds' GPU time (timestamps, read back a few frames later): ms per million texels */
        if (RQ.ms_per_m <= 0.0) RQ.ms_per_m = 1.5;
        for (int k = 0; k < 4; k++) {
            UINT64 a, b;
            if (!RQ.tbusy[k] || ID3D11DeviceContext_GetData(c, (ID3D11Asynchronous*)RQ.tq[k][0], &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                ID3D11DeviceContext_GetData(c, (ID3D11Asynchronous*)RQ.tq[k][1], &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            RQ.tbusy[k] = 0;
            if (!gpu_ts_freq || b < a || RQ.ttex[k] < 65536) continue;
            RQ.last_ms = (double)(b - a) * 1000.0 / (double)gpu_ts_freq;
            double r = RQ.last_ms / (RQ.ttex[k] / 1048576.0);
            if (r > 0.05 && r < 50.0) RQ.ms_per_m = RQ.ms_per_m * 0.8 + r * 0.2;
        }
        uint32_t ts = RQ.thead % 4;
        int timed = 0;
        if (RA.nloaded && !RQ.tbusy[ts]) {
            if (!RQ.tq[ts][0]) {
                D3D11_QUERY_DESC qt = { D3D11_QUERY_TIMESTAMP, 0 };
                ID3D11Device_CreateQuery(gpu_rt.dev, &qt, &RQ.tq[ts][0]);
                ID3D11Device_CreateQuery(gpu_rt.dev, &qt, &RQ.tq[ts][1]);
            }
            timed = RQ.tq[ts][0] && RQ.tq[ts][1];
            if (timed) ID3D11DeviceContext_End(c, (ID3D11Asynchronous*)RQ.tq[ts][0]);
        }
        /* the share of the frame: 256 K texels; while textures wait for their image (the original shows) what the frame leaves
         * of 80 % of its interval, <= 2 ms; while no 3D is drawn (a loading screen, the boot) <= 5 ms */
        uint32_t lim = RA_LOAD_TEXELS, cap = ETB.cap;
        if (RQ.n_wait || !uses) {
            double hz = frame_rate_target_hz ? frame_rate_target_hz() : 60.0;
            double room = (hz > 1.0 ? 0.8 * 1000.0 / hz : 12.0) -
                          (gpu_frame_ms_last > 0.0 ? gpu_frame_ms_last - ETB.last_ms - RQ.last_ms : 0.0);
            double mx = uses ? 2.0 : 5.0;
            room = room > mx ? mx : room < 0.5 ? 0.5 : room;
            room -= (et_fix_on() ? ETB.ms_per_m : 1.2) * ET.out_frame / 1048576.0;   /* (this frame's Enhanced builds) */
            double t = room / RQ.ms_per_m * 1048576.0;
            uint32_t tl = t > 4194304.0 ? 4194304u : t < 262144.0 ? 262144u : (uint32_t)t;
            if (uses && tl > (2u << 20)) tl = 2u << 20;
            lim = tl;
            cap = ET.out_frame + tl;
        }
        for (;;) {
            EnterCriticalSection(&RA.cs);
            int32_t i = RA.nloaded ? RA.loaded[0] : -1;
            LeaveCriticalSection(&RA.cs);
            if (i < 0) break;
            ra_item* it = &RA.it[i];
            int res = 0;                 /* 1 built, 2 nothing to do, 3 deferred, 0 dropped */
            uint32_t words = 0;
            if (it->st != RI_LOADED && it->st != RI_LFAIL) { /* (stale) */ }
            else if (it->st == RI_LOADED && !it->rgba) it->st = RI_NONE;
            else if (it->st == RI_LFAIL) {    /* (a bad file: built again by the net, its entries get Enhanced images meanwhile) */
                RQ.n_lfail++;
                it->bad = 1; it->file = 0; it->alias = 0; it->st = RI_NONE;
                ra_waiters(i, -1);
            } else {
                words = it->rw * it->rh;
                if (up && up + words > lim) break;
                if (et_fix_on() && ET.out_frame && ET.out_frame >= cap) break;
                int32_t id = et_map_find(it->hash);
                et_slot* s = et_slot_of(id);
                if (s && (s->ai || s->hd)) { res = 2; if (s->ai) ra_waiters(i, id); }
                else if (s && ra_visible(s)) res = 3;
                else if (s) {            /* (over its Enhanced image, out of sight) */
                    int32_t r = ra_reg_alloc(words, i);
                    if (r < 0) break;
                    D3D11_BOX bx = { RA.reg[r].at * 4u, 0, 0, (RA.reg[r].at + words) * 4u, 1, 1 };
                    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource*)RA.up, 0, &bx, it->rgba, 0, 0);
                    it->reg = r;
                    res = ra_apply(i, bound) < 0 ? 3 : 1;
                    ra_reg_free(r);
                    it->reg = -1;
                } else {
                    res = ra_build_new(i, bound);
                    if (res < 0) break;
                    if (!res) { RQ.n_pre_noslot++; ra_waiters(i, -1); }
                }
                free(it->rgba);
                it->rgba = NULL;
                if (res == 3) ra_defer_add(i);
                else it->st = res ? RI_DONE : RI_NONE;
                if (res == 1) RA.n_load++;
                up += res == 1 ? words : 0;
                EnterCriticalSection(&RA.cs);
                RQ.bytes -= RQ.bytes >= (uint64_t)words * 4u ? (uint64_t)words * 4u : RQ.bytes;
                LeaveCriticalSection(&RA.cs);
            }
            EnterCriticalSection(&RA.cs);
            memmove(RA.loaded, RA.loaded + 1, (RA.nloaded - 1) * sizeof RA.loaded[0]);
            RA.nloaded--;
            LeaveCriticalSection(&RA.cs);
        }
        if (timed && up) {
            ID3D11DeviceContext_End(c, (ID3D11Asynchronous*)RQ.tq[ts][1]);
            RQ.ttex[ts] = up;
            RQ.tbusy[ts] = 1;
            RQ.thead++;
        }
    }
    for (; !(RQ.on > 0 && RQ.nth);) {   /* (PS2_AI_READY=0: 0.1.1247's) */
        EnterCriticalSection(&RA.cs);
        int32_t i = RA.nloaded ? RA.loaded[0] : -1;
        LeaveCriticalSection(&RA.cs);
        if (i < 0) break;
        ra_item* it = &RA.it[i];
        uint32_t words = it->rw * it->rh;
        if (up && up + words > RA_LOAD_TEXELS) break; if (et_fix_on() && ET.out_frame && ET.out_frame >= ETB.cap) break;   /* (reimagine: within the frame's build time) */
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
    if (RA.log && ET.frame % 600 == 0) {
        uint32_t queued = 0, want = 0;
        for (int32_t i = 0; i < RA_ITEMS; i++) if (RA.it[i].hash) { queued += RA.it[i].st == RI_TILES || RA.it[i].st == RI_PREP; want += RA.it[i].st == RI_WANT; }
        gpu_log("ai: %llu built by the net (%llu tiles), %llu from the cache, %llu saved, %llu excluded; %u in the net's queue, %u waiting; "
                "%.1f TFLOP, GPU %.0f ms in all (%.3f ms per GFLOP; per frame p50 %.2f, p99 %.2f, worst %.2f ms)",
                (unsigned long long)RA.n_net, (unsigned long long)RA.n_tiles, (unsigned long long)RA.n_load, (unsigned long long)RA.n_saved,
                (unsigned long long)RA.n_excl, queued, want, RA.gf_total / 1000.0, RA.ms_total, RA.ms_per_gf, ra_pct(0.5), ra_pct(0.99),
                RA.worst_step);
    }
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
}

/* (et_release) the GPU side goes; the worker, the items and the cache list stay */
static void ra_release(void) {
    if (RQ.on > 0 && RQ.nth) ra_level_flush();   /* (tex-prebuild: the level's list is kept) */
    etn_free();
    ebc_free();                          /* (tex-opt; et_release frees the BC7 classes' slot tables) */
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
    if (RA.inited_thread > 0) EnterCriticalSection(&RA.cs);   /* (reimagine-crash: the worker writes the items' states too) */
    if (RA.it)                           /* (items in flight on the GPU start again; the worker's own finish) */
        for (int32_t i = 0; i < RA_ITEMS; i++) {
            ra_item* it = &RA.it[i];
            it->reg = -1;
            if (it->st == RI_SRC || it->st == RI_POST || it->st == RI_DONE || it->st == RI_WANT) it->st = RI_NONE;
            if (RQ.on > 0 && RQ.nth) {   /* (tex-prebuild) queued / decoding / decoded / deferred images: dropped (a loader
                                          * finishing one sees the state changed and frees it) */
                if (it->st == RI_LOAD || it->st == RI_DECODE || it->st == RI_LOADED || it->st == RI_LFAIL || it->st == RI_DEFER) {
                    free(it->rgba);
                    it->rgba = NULL;
                    it->st = RI_NONE;
                    it->gen++;
                }
                it->waiters = 0;
            }
        }
    if (RA.inited_thread > 0) {
        for (uint32_t k = 0; k < RA.tn; k++) { free(RA.tq[k].px); RA.it[RA.tq[k].item].st = RI_NONE; }
        RA.tn = 0;
        if (RQ.on > 0 && RQ.nth) {
            RA.nloaded = 0;
            RQ.bytes = 0;
            RQ.qt[0] = RQ.qh[0]; RQ.qt[1] = RQ.qh[1];
            RQ.mstate = 0;
            free(RQ.mbuf); RQ.mbuf = NULL;
            free(RQ.mbuf2); RQ.mbuf2 = NULL;
        }
        LeaveCriticalSection(&RA.cs);
    }
    for (int k = 0; k < 4; k++) { GPU_RELEASE(RQ.tq[k][0]); GPU_RELEASE(RQ.tq[k][1]); RQ.tbusy[k] = 0; }
    if (RQ.on > 0) {
        RQ.ndefer = 0; RQ.n_wait = 0; RQ.uses_last = 0;
        RQ.world = 0xFFFFFFFEu; RQ.nrec = 0; RQ.dirty = 0;
        if (RQ.set) memset(RQ.set, 0, 2u * RA_RECN * sizeof *RQ.set);
        free(RQ.pre); RQ.pre = NULL; RQ.npre = RQ.ipre = 0;
    }
    RA.init = 0;
    RA.failed = 0;
}
