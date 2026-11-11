/* (tex) Enhanced textures of modern frames (notes/enhanced-textures.md; shaders/tex.hlsl, shaders/etex.hlsli). Included
 * once by gpu_exec.c after the decoded texture cache (texdec, TD): an enhanced image hangs off a decoded entry.
 *
 *   what    art textures of the 3D (R2 materials, exact phase-A triangles from the decoded cache) get an image of their
 *           own: dithered areas resolved to their shading first (cs_et_dd), an edge-directed 4x upscale with an unsharp
 *           mask at the source scale, a full mip chain (alpha-tested
 *           images keep their coverage), sampled trilinear + anisotropic. Or an HD-pack image: <user dir>\textures\
 *           <hash>.png. Render targets, CLUT-animated / per-frame textures, nearest / region-addressed ones, sprites, the
 *           HUD and particles keep the GS's sampling.
 *   flow    a decoded entry is noted (et_use: R2's td_r2, the FF prepass td_state) -> at the start of the next frame its
 *           content hash is computed on the GPU (cs_et_hash) and read back through a staging ring (no stall) -> an image
 *           with that hash is shared, an HD file is loaded on a worker thread, else the image is built (budgeted per
 *           frame) -> et_tab (decoded base / 64 -> class, slice) points the pixel shaders at it. A new decode of the entry
 *           (et_decoded) unlinks it in stream order; a second content change rejects it (dynamic texture).
 *   memory  square Texture2DArray classes 64 .. 2048, grown by doubling within PS2_TEX_MB (default 1024); slices of
 *           images unused for a while are reused (LRU) when a class is full.
 *   cost    Original (graphics.textures = original, PS2_TEX=0): nothing runs, no resource exists, the original
 *           pipelines draw. Enhanced: the R2_ETEX / ETEX permutations; hashing + building at most PS2_TEX_FRAME output
 *           texels per frame.
 *   config  gpu_tex_config (gfx.c: graphics.textures); PS2_TEX=0|1 wins. Tests: PS2_TEX_UP=0 (mips + anisotropic of the
 *           original images only), PS2_TEX_SCALE=2|4, PS2_TEX_SHARP, PS2_TEX_SIGMA_C / _SIGMA_A / _EDGE, PS2_TEX_ANISO,
 *           PS2_TEX_DEDITHER=0 (no dither resolve), PS2_TEX_DD_RANGE / _DENSE / _SHARP,
 *           PS2_TEX_BIAS, PS2_TEX_MB, PS2_TEX_FRAME, PS2_TEX_HD=0, PS2_TEX_DUMP=1, PS2_TEX_LOG=1, PS2_TEX_SPLIT=1. In-run
 *           A/B: PS2_GPU_AB=<frames> PS2_GPU_AB_R8=128 (R8P_TEX). */

static volatile LONG tcf_on = 0;         /* graphics.textures (gfx.c); 0 until the settings say otherwise (tools, tests) */
void gpu_tex_config(int on) { InterlockedExchange(&tcf_on, on ? 1 : 0); }
static volatile LONG tcf_look = 0;       /* (ai) graphics.look = reimagined (gfx.c): Reimagined textures (gpu_tex_ai.h) */
void gpu_tex_look(int reimagined) { InterlockedExchange(&tcf_look, reimagined ? 1 : 0); }
#include "gpu_tex_normals.h"             /* (ai) the surface maps' interface for the lighting */
static int etn_on(void);                 /* (ai) gpu_tex_ai.h: hooks of Reimagined (nothing runs under Faithful / Retro) */
static float etn_k(void);
static void etn_grow(int c, uint32_t oc, uint32_t nc);
static void etn_run(int cls, uint32_t a, uint32_t n);
static int ra_cfg(void);
static int ra_init(void);
static void ra_note(int32_t idx, int32_t id);
static void ra_frame(int* bound);
static void ra_release(void);
static int ra_on_now;                    /* (ai) Reimagined textures this frame */

#define ET_NCLS 6                        /* square classes: 64 << c texels */
#define ET_MAXMIP 12
#define ET_TAB_N (TD_WORDS >> 6)         /* et_tab entries: decoded base / 64 */
#define ET_RING 4                        /* staging buffers of hash read-backs (frames of latency) */
#define ET_HREQ 256                      /* hash requests per frame */
#define ET_MAP 16384                     /* hash -> slot */
#define ET_SCR_WORDS (2048u * 2048u * 4u / 3u + 64u)   /* scratch: a 2048 level chain */
#define ET_DD_WORDS (1u << 20)           /* a batch's dither-resolved sources + weights (cs_et_dd -> cs_et_up) */
enum { ES_NONE, ES_WANT, ES_HASHING, ES_QUEUED, ES_READY, ES_HDLOAD, ES_REJECT };

typedef struct {                         /* per decoded entry (the index of TD.ent) */
    uint8_t st, nchg, flags, ref;        /* state; content changes seen; addressing (1 clamp u, 2 clamp v); alpha-test ref */
    uint8_t lw, lh;
    uint32_t ver, base;                  /* the decoded version / base the state belongs to */
    int32_t slot, next;                  /* image slot (cls << 16 | slice) or -1; next entry on the same slot */
    uint64_t hash;                       /* the content's (with flags / ref mixed in: the image's key) */
    uint64_t raw;                        /* the content's alone (the HD pack's file name) */
    uint32_t chg;                        /* the frame of the last content change */
} et_ent;
typedef struct { uint64_t hash; uint32_t last; int32_t first; uint8_t used, hd, ai; } et_slot;   /* (ai: the AI image is in it) */
typedef struct { int32_t ent; uint32_t ver; } et_req;

static struct {
    int env, env_on, up, aniso, hd, dump, log, split, scale, init, failed, dbg;
    float sharp, bias, sig_c, sig_a, edge, flat, detail, dd, dd_rng, dd_sharp, dd_dense;
    uint32_t budget_mb, frame_out;
    ID3D11Texture2D* arr[ET_NCLS];
    ID3D11ShaderResourceView* srv[ET_NCLS];
    ID3D11UnorderedAccessView* uav[ET_NCLS][ET_MAXMIP];
    uint32_t cap[ET_NCLS];
    et_slot* sl[ET_NCLS];
    uint64_t bytes;
    ID3D11Buffer *tab, *lv, *hist, *hout, *cb, *pcb, *jobs, *hjobs, *ddb;   /* (jobs, hjobs: the batches' job tables) */
    ID3D11ShaderResourceView *tab_srv, *jobs_srv, *hjobs_srv, *ddb_srv;
    ID3D11UnorderedAccessView *tab_uav, *lv_uav, *hist_uav, *hout_uav, *ddb_uav;
    ID3D11Buffer* stage[ET_RING];
    uint32_t stage_n[ET_RING];
    et_req req[ET_RING][ET_HREQ];
    uint32_t ring;
    ID3D11SamplerState* smp[2];
    float pcb_val[4];
    et_ent e[TD_HASH];
    uint64_t map_h[ET_MAP];
    int32_t map_s[ET_MAP];
    int32_t q[TD_HASH];
    uint32_t qn;
    uint32_t frame, out_frame;
    uint64_t n_hash, n_build, n_share, n_hd, n_reject, n_evict, n_grow, n_texels, n_redec, n_change;
    int resets;
    double cpu_max;                      /* (statistics) the slowest et_frame_begin, ms */
} ET = { 0 };
#define ET_TQ 8                          /* (PS2_TEX_TIME: et_frame_begin) */
static struct { int on; ID3D11Query* q[ET_TQ][2]; uint32_t tex[ET_TQ]; uint8_t busy[ET_TQ]; uint32_t head;
                double ms, mx; uint64_t texels; uint32_t n; } ETT = { -1 };

static float et_envf(const char* n, float d) { const char* e = getenv(n); return e && *e ? (float)atof(e) : d; }
static void et_env(void) {
    if (ET.env) return;
    ET.env = 1;
    const char* e = getenv("PS2_TEX");
    ET.env_on = e && *e ? *e != '0' : -1;
    e = getenv("PS2_TEX_UP"); ET.up = !(e && *e == '0');
    e = getenv("PS2_TEX_HD"); ET.hd = !(e && *e == '0');
    e = getenv("PS2_TEX_DUMP"); ET.dump = e && *e ? atoi(e) : 0;
    e = getenv("PS2_TEX_LOG"); ET.log = e && *e == '1';
    e = getenv("PS2_TEX_SPLIT"); ET.split = e && *e == '1';
    e = getenv("PS2_TEX_DBG"); ET.dbg = e && *e ? atoi(e) : 0;   /* (1: enhanced texels red, 2: grey by class) */
    ET.scale = (int)et_envf("PS2_TEX_SCALE", 4.0f);
    if (ET.scale != 2 && ET.scale != 4 && ET.scale != 8) ET.scale = 4;
    ET.aniso = (int)et_envf("PS2_TEX_ANISO", 16.0f);
    if (ET.aniso < 1) ET.aniso = 1;
    if (ET.aniso > 16) ET.aniso = 16;
    ET.sharp = et_envf("PS2_TEX_SHARP", 2.2f);     /* unsharp mask at the source scale */
    ET.sig_c = et_envf("PS2_TEX_SIGMA_C", 0.35f);  /* steered kernel: across the edge (source texels) */
    ET.sig_a = et_envf("PS2_TEX_SIGMA_A", 1.2f);   /*                 along it */
    ET.edge = et_envf("PS2_TEX_EDGE", 0.03f);      /* gradients below this are noise: no steering */
    ET.flat = et_envf("PS2_TEX_FLAT", 6.0f);       /* local range (0..255) below which a texel is smoothed (palette steps) */
    ET.detail = et_envf("PS2_TEX_DETAIL", 18.0f);  /* ... above which it gets the whole unsharp mask */
    ET.dd = et_envf("PS2_TEX_DEDITHER", 1.0f);     /* dither resolve strength (0: off) */
    ET.dd_rng = et_envf("PS2_TEX_DD_RANGE", 40.0f); /* ... below this local contrast (0..255) */
    ET.dd_sharp = et_envf("PS2_TEX_DD_SHARP", 1.0f); /* share of the unsharp mask kept on resolved texels */
    ET.dd_dense = et_envf("PS2_TEX_DD_DENSE", 4.0f); /* dither-like texels of 25 from which an area is resolved */
    ET.bias = et_envf("PS2_TEX_BIAS", -0.75f);       /* LOD bias of the sampling */
    ET.budget_mb = (uint32_t)et_envf("PS2_TEX_MB", 1024.0f);
    ET.frame_out = (uint32_t)et_envf("PS2_TEX_FRAME", 1048576.0f);   /* output texels built per frame */
}
static int et_cfg(void) {                /* the setting (work happens while it is on, whatever the A/B phase) */
    et_env();
    return !ET.failed && (ET.env_on >= 0 ? ET.env_on : tcf_on > 0 || tcf_look > 0);
}
static int et_enabled(void) { return et_cfg() && ET.init && r8p_on(R8P_TEX); }   /* draws use the images */

static uint32_t et_slice_bytes(int cls) { uint64_t s = 64ull << cls; return (uint32_t)(s * s * 4 * 4 / 3); }

static void et_free_class(int c) {
    for (int m = 0; m < ET_MAXMIP; m++) GPU_RELEASE(ET.uav[c][m]);
    GPU_RELEASE(ET.srv[c]);
    GPU_RELEASE(ET.arr[c]);
}

static void et_release_late(void);      /* (the dump read-backs and the HD upload buffer, below) */
static void et_release(void) {
    ra_release();                        /* (ai) */
    et_release_late();
    for (int c = 0; c < ET_NCLS; c++) {
        et_free_class(c);
        free(ET.sl[c]);
        ET.sl[c] = NULL;
        ET.cap[c] = 0;
    }
    GPU_RELEASE(ET.tab_srv); GPU_RELEASE(ET.tab_uav); GPU_RELEASE(ET.tab);
    GPU_RELEASE(ET.lv_uav); GPU_RELEASE(ET.lv);
    GPU_RELEASE(ET.hist_uav); GPU_RELEASE(ET.hist);
    GPU_RELEASE(ET.hout_uav); GPU_RELEASE(ET.hout);
    GPU_RELEASE(ET.ddb_srv); GPU_RELEASE(ET.ddb_uav); GPU_RELEASE(ET.ddb);
    for (int k = 0; k < ET_TQ; k++) { GPU_RELEASE(ETT.q[k][0]); GPU_RELEASE(ETT.q[k][1]); ETT.busy[k] = 0; }
    GPU_RELEASE(ET.cb); GPU_RELEASE(ET.pcb);
    GPU_RELEASE(ET.jobs_srv); GPU_RELEASE(ET.jobs); GPU_RELEASE(ET.hjobs_srv); GPU_RELEASE(ET.hjobs);
    for (int r = 0; r < ET_RING; r++) { GPU_RELEASE(ET.stage[r]); ET.stage_n[r] = 0; }
    GPU_RELEASE(ET.smp[0]); GPU_RELEASE(ET.smp[1]);
    memset(ET.e, 0, sizeof ET.e);
    for (int i = 0; i < TD_HASH; i++) ET.e[i].slot = -1;
    memset(ET.map_h, 0, sizeof ET.map_h);
    ET.qn = 0;
    ET.bytes = 0;
    ET.init = 0;
}

static ID3D11Buffer* et_buf(UINT bytes, UINT bind, D3D11_USAGE usage, UINT cpu) {
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = bytes; bd.Usage = usage; bd.BindFlags = bind; bd.CPUAccessFlags = cpu;
    ID3D11Buffer* b = NULL;
    if (FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &b))) return NULL;
    return b;
}

static int et_init(void) {
    if (ET.init) return 1;
    if (ET.failed || !gpu_rt.dev) return 0;
    for (int i = 0; i < TD_HASH; i++) ET.e[i].slot = -1;
    ET.tab = raw_default(ET_TAB_N * 8u, NULL, &ET.tab_srv, &ET.tab_uav);
    ET.lv = raw_default(ET_SCR_WORDS * 4u, NULL, NULL, &ET.lv_uav);
    ET.hist = raw_default(256u * 13u * 256u * 4u, NULL, NULL, &ET.hist_uav);   /* (ET_JOBS x ET_HIST_WORDS) */
    ET.jobs = raw_default(256u * 64u, NULL, &ET.jobs_srv, NULL);
    ET.hjobs = raw_default(ET_HREQ * 64u, NULL, &ET.hjobs_srv, NULL);
    ET.hout = raw_default(ET_HREQ * 16u, NULL, NULL, &ET.hout_uav);
    ET.ddb = raw_default(ET_DD_WORDS * 4u, NULL, &ET.ddb_srv, &ET.ddb_uav);
    ET.cb = et_buf(64, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
    ET.pcb = et_buf(16, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
    int ok = ET.tab && ET.lv && ET.hist && ET.hout && ET.cb && ET.pcb && ET.jobs && ET.hjobs && ET.ddb;
    for (int r = 0; r < ET_RING && ok; r++) {
        ET.stage[r] = et_buf(ET_HREQ * 16u, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
        ok = ET.stage[r] != NULL;
    }
    for (int s = 0; s < 2 && ok; s++) {
        D3D11_SAMPLER_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Filter = ET.aniso > 1 ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = s ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxAnisotropy = (UINT)ET.aniso;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ok = SUCCEEDED(ID3D11Device_CreateSamplerState(gpu_rt.dev, &sd, &ET.smp[s]));
    }
    if (!ok || !gpu_cs(GPU_SH_cs_et_up) || !gpu_cs(GPU_SH_cs_et_dd) || !gpu_cs(GPU_SH_cs_et_hash)) {
        gpu_log("tex: cannot create the Enhanced textures' resources (%d%d%d%d%d%d%d%d %d %p %p): Original textures", !!ET.tab, !!ET.lv, !!ET.hist, !!ET.hout, !!ET.cb, !!ET.pcb, !!ET.jobs, !!ET.hjobs, ok, (void*)gpu_cs(GPU_SH_cs_et_up), (void*)gpu_cs(GPU_SH_cs_et_hash));
        et_release();
        ET.failed = 1;
        return 0;
    }
    ET.init = 1;
    gpu_log("tex: Enhanced textures on (scale %dx, sharpen %.2f, anisotropic %dx, budget %u MB%s)", ET.up ? ET.scale : 1,
            ET.sharp, ET.aniso, ET.budget_mb, ET.hd ? ", HD folder" : "");
    return 1;
}

/* et_tab entry for decoded base `base`: (dec, class | slice << 3), or zeros */
static void et_tab_write(uint32_t base, uint32_t dec, uint32_t info) {
    uint32_t v[2] = { dec, info };
    D3D11_BOX b = { (base >> 6) * 8u, 0, 0, (base >> 6) * 8u + 8u, 1, 1 };
    ID3D11DeviceContext_UpdateSubresource(gpu_rt.ctx, (ID3D11Resource*)ET.tab, 0, &b, v, 0, 0);
}

static et_slot* et_slot_of(int32_t id) { return id < 0 ? NULL : &ET.sl[id >> 16][id & 0xFFFF]; }

static void et_unlink(int32_t idx) {
    et_ent* e = &ET.e[idx];
    et_slot* s = et_slot_of(e->slot);
    if (s) {
        for (int32_t* p = &s->first; *p >= 0; p = &ET.e[*p].next)
            if (*p == idx) { *p = e->next; break; }
        et_tab_write(e->base, 0, 0);
    }
    e->slot = -1;
    e->next = -1;
}

/* (reimagine) visible changes of an already enhanced texture (PS2_TEX_SWAPLOG=1: a summary, 2: every change): its image
 * replaced by another, by the AI image, or by the original sampling. A texture should change at most once (Enhanced ->
 * AI) after it first shows enhanced. */
static struct { int on; int32_t prev[TD_HASH], prev2[TD_HASH]; uint16_t n[TD_HASH]; uint32_t base[TD_HASH]; uint64_t total, game; uint32_t frame; } ESW;
static void et_swap_note(int32_t idx, char kind) {
    if (ESW.on < 0) return;
    if (!ESW.on) { const char* v = getenv("PS2_TEX_SWAPLOG"); ESW.on = v && *v ? atoi(v) : -1; if (ESW.on < 0) return;
                   for (int i = 0; i < TD_HASH; i++) ESW.prev[i] = ESW.prev2[i] = -1; }
    if (ESW.base[idx] != ET.e[idx].base) { ESW.base[idx] = ET.e[idx].base; ESW.n[idx] = 0; ESW.prev[idx] = ESW.prev2[idx] = -1; }
    if (kind == 'L') {                  /* (linked to a slot: a change only when another image showed before) */
        int32_t p = ESW.prev[idx], p2 = ESW.prev2[idx];
        ESW.prev[idx] = ET.e[idx].slot;
        if (p < 0 || p == ET.e[idx].slot) return;
        ESW.prev2[idx] = p;
        /* (another image because the game put other content there: its change, not ours - unless it flips back) */
        if (p2 != ET.e[idx].slot) { ESW.game++; return; }
    }
    if (kind == 'O' && ESW.prev[idx] < 0) return;   /* (never enhanced: nothing changes) */
    if (kind == 'O') ESW.prev[idx] = -1;
    ESW.n[idx]++;
    ESW.total++;
    if (ESW.on >= 2) gpu_log("tex-swap: frame %u entry %d base %05X %c (%u)", ET.frame, idx, ET.e[idx].base, kind, ESW.n[idx]);
}
/* (reimagine) PS2_REIM_FIX=0: the 0.1.1207 behaviour (content changes, build budget), for before / after measurements */
static int et_fix_on(void) { static int v = -1; if (v < 0) { const char* e = getenv("PS2_REIM_FIX"); v = !(e && *e == '0'); } return v; }
static void et_swap_summary(void) {
    if (ESW.on <= 0) return;
    uint32_t h[4] = { 0, 0, 0, 0 }, worst = 0;
    for (int i = 0; i < TD_HASH; i++) { uint32_t k = ESW.n[i]; h[k > 3 ? 3 : k]++; if (k > worst) worst = k; }
    gpu_log("tex-swap: frame %u: %llu changes; textures with 1: %u, 2: %u, 3+: %u (most %u); content the game changed %llu", ET.frame,
            (unsigned long long)ESW.total, h[1], h[2], h[3], worst, (unsigned long long)ESW.game);
}

static void et_link(int32_t idx, int32_t id) {
    et_ent* e = &ET.e[idx];
    et_slot* s = et_slot_of(id);
    e->slot = id;
    e->next = s->first;
    s->first = idx;
    s->last = ET.frame;
    e->st = ES_READY;
    et_tab_write(e->base, e->base + 1, (uint32_t)(id >> 16) | (uint32_t)(id & 0xFFFF) << 3);
    et_swap_note(idx, 'L');
    if (ra_on_now) ra_note(idx, id);     /* (ai) its AI image: from the cache or built */
}

static int32_t et_map_find(uint64_t h) {
    uint32_t i0 = (uint32_t)(h >> 40) & (ET_MAP - 1);
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t k = (i0 + i) & (ET_MAP - 1);
        if (!ET.map_h[k]) return -1;
        if (ET.map_h[k] == h) {
            et_slot* s = et_slot_of(ET.map_s[k]);
            return s && s->used && s->hash == h ? ET.map_s[k] : -1;
        }
    }
    return -1;
}
static void et_map_put(uint64_t h, int32_t id) {
    uint32_t i0 = (uint32_t)(h >> 40) & (ET_MAP - 1);
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t k = (i0 + i) & (ET_MAP - 1);
        et_slot* s = ET.map_h[k] ? et_slot_of(ET.map_s[k]) : NULL;
        if (!ET.map_h[k] || ET.map_h[k] == h || !s || !s->used || s->hash != ET.map_h[k]) {
            ET.map_h[k] = h; ET.map_s[k] = id;
            return;
        }
    }
    /* (a crowded run: rebuild the table from the live slots) */
    memset(ET.map_h, 0, sizeof ET.map_h);
    for (int c = 0; c < ET_NCLS; c++)
        for (uint32_t j = 0; j < ET.cap[c]; j++)
            if (ET.sl[c][j].used && ET.sl[c][j].hash != h) et_map_put(ET.sl[c][j].hash, (int32_t)(c << 16 | j));
    ET.map_h[(uint32_t)(h >> 40) & (ET_MAP - 1)] = h;
    ET.map_s[(uint32_t)(h >> 40) & (ET_MAP - 1)] = id;
}

/* a class with room for n more slices (grown by doubling, copying what it holds) */
static int et_grow(int c) {
    static const uint32_t first[ET_NCLS] = { 64, 256, 256, 256, 64, 4 };   /* (~790 MB: a hub's images (ep1: 690) without growing: a growth copies the array, a frame-time spike) */
    uint32_t oc = ET.cap[c], nc = oc ? oc * 2 : first[c];
    if (nc > 4096) return 0;
    if (ET.bytes + (uint64_t)(nc - oc) * et_slice_bytes(c) > (uint64_t)ET.budget_mb << 20) return 0;
    uint32_t S = 64u << c, nm = (uint32_t)c + 7u;
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = td.Height = S; td.MipLevels = nm; td.ArraySize = nc; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ID3D11Texture2D* t = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &t))) return 0;
    et_slot* sl = realloc(ET.sl[c], nc * sizeof(et_slot));
    if (!sl) { ID3D11Texture2D_Release(t); return 0; }
    memset(sl + oc, 0, (nc - oc) * sizeof(et_slot));
    for (uint32_t j = oc; j < nc; j++) sl[j].first = -1;
    ET.sl[c] = sl;
    if (ET.arr[c])
        for (uint32_t j = 0; j < oc; j++)
            for (uint32_t m = 0; m < nm; m++)
                ID3D11DeviceContext_CopySubresourceRegion(gpu_rt.ctx, (ID3D11Resource*)t, (m + j * nm), 0, 0, 0,
                                                          (ID3D11Resource*)ET.arr[c], (m + j * nm), NULL);
    et_free_class(c);
    ET.arr[c] = t;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Texture2DArray.MipLevels = nm; sd.Texture2DArray.ArraySize = nc;
    int ok = SUCCEEDED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)t, &sd, &ET.srv[c]));
    for (uint32_t m = 0; m < nm && ok; m++) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
        memset(&ud, 0, sizeof ud);
        ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
        ud.Texture2DArray.MipSlice = m; ud.Texture2DArray.ArraySize = nc;
        ok = SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(gpu_rt.dev, (ID3D11Resource*)t, &ud, &ET.uav[c][m]));
    }
    ET.bytes += (uint64_t)(nc - oc) * et_slice_bytes(c);
    ET.cap[c] = nc;
    ET.n_grow++;
    if (!ok) { gpu_log("tex: class %u views failed", S); ET.failed = 1; return 0; }
    if (ET.log) gpu_log("tex: class %ux%u: %u slices (%.0f MB in all)", S, S, nc, ET.bytes / 1048576.0);
    if (ra_on_now) etn_grow(c, oc, nc);  /* (ai) the surface maps follow */
    return 1;
}

/* a free slot of class c (a new slice, or the least recently used image unused for 10 s) */
static int32_t et_alloc(int c) {
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t j = 0; j < ET.cap[c]; j++)
            if (!ET.sl[c][j].used) return (int32_t)(c << 16 | j);
        if (pass == 0 && et_grow(c)) continue;
        break;
    }
    uint32_t best = 0xFFFFFFFFu, bl = ET.frame;
    for (uint32_t j = 0; j < ET.cap[c]; j++)
        if (ET.sl[c][j].last + 600 < ET.frame && ET.sl[c][j].last < bl) { bl = ET.sl[c][j].last; best = j; }
    if (best == 0xFFFFFFFFu) return -1;
    et_slot* s = &ET.sl[c][best];
    while (s->first >= 0) {              /* (its entries go back to unknown: a later use builds again) */
        int32_t i = s->first;
        et_swap_note(i, 'O');
        et_unlink(i);
        ET.e[i].st = ES_NONE;
    }
    s->used = 0;
    s->ai = 0;
    ET.n_evict++;
    return (int32_t)(c << 16 | best);
}

#define ET_JOBS 256                      /* jobs (images) per batch; hash requests per frame */
#define ET_HIST_WORDS (13u * 256u)       /* per job: 12 level histograms, then the scales */
typedef struct { uint32_t src, lw, lh, flags, S, slice, lv, hist, ref, nm, dd, pad[5]; } et_job;   /* (tex.hlsl job_load) */
static struct {
    et_job j[ET_JOBS];
    int32_t id[ET_JOBS];
    uint32_t n, lv_used, dd_used;
    ID3D11ShaderResourceView* src;
} EB;

static void et_cb_set(uint32_t level, uint32_t first) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(gpu_rt.ctx, (ID3D11Resource*)ET.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    uint32_t* w = m.pData;
    uint32_t b[4] = { level, first, 0, 0 };
    float p[4] = { ET.sharp, ET.sig_c, ET.sig_a, ET.edge }, q[4] = { ET.flat, ET.detail, ET.dd, ET.dd_rng }, r[4] = { ET.dd_sharp, ra_on_now ? etn_k() : 0.0f, ET.dd_dense, 0.0f };
    memcpy(w, b, 16); memcpy(w + 4, p, 16); memcpy(w + 8, q, 16); memcpy(w + 12, r, 16);
    ID3D11DeviceContext_Unmap(gpu_rt.ctx, (ID3D11Resource*)ET.cb, 0);
}

static void et_cs_unbind(void) {
    ID3D11DeviceContext* c = gpu_rt.ctx;
    ID3D11UnorderedAccessView* nu[5] = { NULL, NULL, NULL, NULL, NULL };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 5, nu, NULL);
    ID3D11ShaderResourceView* ns[3] = { NULL, NULL, NULL };
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 3, ns);
}

/* the batch's images: per class one dispatch per pass and mip level over all of its jobs (tex.hlsl) */
static void et_batch_run(void) {
    if (!EB.n) return;
    ID3D11DeviceContext* c = gpu_rt.ctx;
    /* jobs sorted by class (a pass binds one class's mip level) */
    for (uint32_t i = 1; i < EB.n; i++)
        for (uint32_t k = i; k > 0 && (EB.id[k - 1] >> 16) > (EB.id[k] >> 16); k--) {
            et_job tj = EB.j[k]; EB.j[k] = EB.j[k - 1]; EB.j[k - 1] = tj;
            int32_t ti = EB.id[k]; EB.id[k] = EB.id[k - 1]; EB.id[k - 1] = ti;
        }
    int anyref = 0;
    for (uint32_t i = 0; i < EB.n; i++) { EB.j[i].hist = i * ET_HIST_WORDS; anyref |= EB.j[i].ref != 0; }
    D3D11_BOX bx = { 0, 0, 0, EB.n * (uint32_t)sizeof(et_job), 1, 1 };
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource*)ET.jobs, 0, &bx, EB.j, 0, 0);
    if (anyref) { static const UINT z[4] = { 0, 0, 0, 0 }; ID3D11DeviceContext_ClearUnorderedAccessViewUint(c, ET.hist_uav, z); }
    ID3D11ShaderResourceView* s2[2] = { EB.src, ET.jobs_srv };
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 2, s2);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 3, 1, &ET.cb);
    for (uint32_t a = 0; a < EB.n;) {
        int cls = EB.id[a] >> 16;
        uint32_t b = a, ref = 0, dw = 0, dh = 0;
        for (; b < EB.n && (EB.id[b] >> 16) == cls; b++) {
            ref |= EB.j[b].ref;
            if (EB.j[b].dd != 0xFFFFFFFFu) {
                if ((1u << EB.j[b].lw) > dw) dw = 1u << EB.j[b].lw;
                if ((1u << EB.j[b].lh) > dh) dh = 1u << EB.j[b].lh;
            }
        }
        uint32_t n = b - a, S = 64u << cls, nm = (uint32_t)cls + 7u;
        ID3D11UnorderedAccessView* u[3] = { ET.lv_uav, ET.uav[cls][0], ET.hist_uav };
        ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 3, u, NULL);
        et_cb_set(0, a);
        if (dw) {                        /* the dither resolve of the sources (tex.hlsl cs_et_dd): written through u4, read
                                          * by the upscale through t2 (an SRV: the cached load path) */
            ID3D11ShaderResourceView* n1 = NULL;
            ID3D11UnorderedAccessView* nu1 = NULL;
            ID3D11DeviceContext_CSSetShaderResources(c, 2, 1, &n1);
            ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 4, 1, &ET.ddb_uav, NULL);
            ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_et_dd), NULL, 0);
            ID3D11DeviceContext_Dispatch(c, (dw + 7) / 8, (dh + 7) / 8, n);
            ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 4, 1, &nu1, NULL);
            ID3D11DeviceContext_CSSetShaderResources(c, 2, 1, &ET.ddb_srv);
            gpu_rt.n_dispatches++;
        }
        ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_et_up), NULL, 0);
        ID3D11DeviceContext_Dispatch(c, (S + 7) / 8, (S + 7) / 8, n);
        ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_et_mip), NULL, 0);
        for (uint32_t k = 1; k < nm; k++) {
            uint32_t sz = S >> k;
            ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 1, 1, &ET.uav[cls][k], NULL);
            et_cb_set(k, a);
            ID3D11DeviceContext_Dispatch(c, (sz + 7) / 8, (sz + 7) / 8, n);
        }
        if (ref) {
            ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_et_cov), NULL, 0);
            ID3D11DeviceContext_Dispatch(c, 1, 1, n);
            ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_et_store), NULL, 0);
            for (uint32_t k = 1; k < nm; k++) {
                uint32_t sz = S >> k;
                ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 1, 1, &ET.uav[cls][k], NULL);
                et_cb_set(k, a);
                ID3D11DeviceContext_Dispatch(c, (sz + 7) / 8, (sz + 7) / 8, n);
            }
        }
        if (ra_on_now) etn_run(cls, a, n);   /* (ai) the surface maps from the levels in the scratch */
        gpu_rt.n_dispatches += nm * (ref ? 2 : 1) + 1;
        a = b;
    }
    et_cs_unbind();
    EB.n = 0;
    EB.lv_used = 0;
    EB.dd_used = 0;
}

/* queue the image of a decoded texture (base, 2^lw x 2^lh) for slot id; 0: no room in this batch (the caller runs it
 * and tries again) */
static int et_build(const et_ent* e, int32_t id, ID3D11ShaderResourceView* src, uint32_t src_base, int hd) {
    int cls = id >> 16;
    uint32_t S = 64u << cls, words = S * S * 4u / 3u + 16u;
    int dd = !hd && ET.up && ET.dd > 0.0f;   /* (the dither resolve: built images only; the source again + weights) */
    uint32_t dw = dd ? 2u << (e->lw + e->lh) : 0u, dbase = EB.n ? EB.dd_used : 0u;
    if (EB.n && (EB.n >= ET_JOBS || EB.src != src || EB.lv_used + words > ET_SCR_WORDS || dbase + dw > ET_DD_WORDS)) return 0;
    et_job* j = &EB.j[EB.n];
    memset(j, 0, sizeof *j);
    j->src = src_base; j->lw = e->lw; j->lh = e->lh; j->flags = e->flags | (hd || !ET.up ? 4u : 0u);
    j->S = S; j->slice = (uint32_t)(id & 0xFFFF); j->lv = EB.lv_used; j->ref = e->ref; j->nm = (uint32_t)cls + 7u;
    j->dd = dd ? dbase : 0xFFFFFFFFu;
    EB.dd_used = dbase + dw;
    EB.id[EB.n++] = id;
    EB.src = src;
    EB.lv_used += words;
    ET.n_texels += (uint64_t)S * S;
    ET.out_frame += S * S;
    return 1;
}

/* the class of an image of 2^lw x 2^lh texels (upscaled) */
static int et_class_of(uint32_t lw, uint32_t lh, int hd_w) {
    uint32_t m = 1u << (lw > lh ? lw : lh);
    uint32_t S = hd_w ? (uint32_t)hd_w : ET.up ? m * (uint32_t)ET.scale : m;
    int c = 0;
    while (c < ET_NCLS - 1 && (64u << c) < S) c++;
    if (!hd_w && c > 4) c = 4;           /* (built images: 1024 at most) */
    return c;
}

/* (td_r2 / td_state) the decoded entry te is used at base by an art texture: flags = addressing, ref = the alpha test's
 * reference when the use cuts out (GEQUAL / GREATER), else 0 */
static void et_use(const td_ent* te, uint32_t lw, uint32_t lh, uint32_t flags, uint32_t ref) {
    if (!et_cfg()) return;
    int32_t idx = (int32_t)(te - TD.ent);
    et_ent* e = &ET.e[idx];
    if (e->st == ES_READY) { et_slot* s = et_slot_of(e->slot); if (s) s->last = ET.frame; return; }
    if (e->st != ES_NONE) return;
    if ((lw > lh ? lw : lh) < 4) { e->st = ES_REJECT; return; }   /* (tiny: nothing to gain) */
    e->st = ES_WANT;
    e->lw = (uint8_t)lw; e->lh = (uint8_t)lh;
    e->flags = (uint8_t)flags; e->ref = (uint8_t)ref;
    e->base = te->base;
    e->ver = te->ver;
}

/* (td_r2, td_run) entry te is decoded again (in stream order). Its pages were written, which often leaves the content as
 * it was (a neighbour's upload into the same page): the image stays linked until the new content's hash says otherwise
 * (et_resolve). A first change (a level's new texture at the same place) builds a new image; a second one soon after
 * (CLUT animation, per-frame uploads) leaves the texture to the original sampling for the rest of the cache's life. */
static void et_decoded(const td_ent* te) {
    if (!ET.init) return;
    int32_t idx = (int32_t)(te - TD.ent);
    et_ent* e = &ET.e[idx];
    if (e->st == ES_NONE || e->st == ES_REJECT || e->st == ES_WANT) return;
    if (e->st == ES_QUEUED) {            /* (its build would read the new content under the old key) */
        for (uint32_t i = 0; i < ET.qn; i++) if (ET.q[i] == idx) { ET.q[i] = ET.q[--ET.qn]; break; }
    }
    e->st = ES_WANT;
    e->base = te->base;
    e->ver = te->ver;
    ET.n_redec++;
}

/* (td_prepass: the cache was reset) every entry is gone: images stay, keyed by hash, for when the textures return */
static void et_reset(void) {
    if (!ET.init) return;
    for (int c = 0; c < ET_NCLS; c++)
        for (uint32_t j = 0; j < ET.cap[c]; j++) ET.sl[c][j].first = -1;
    memset(ET.e, 0, sizeof ET.e);
    for (int i = 0; i < TD_HASH; i++) ET.e[i].slot = ET.e[i].next = -1;
    static const UINT z[4] = { 0, 0, 0, 0 };
    ID3D11DeviceContext_ClearUnorderedAccessViewUint(gpu_rt.ctx, ET.tab_uav, z);
    for (int r = 0; r < ET_RING; r++) ET.stage_n[r] = 0;   /* (pending hashes refer to the old entries) */
    ET.qn = 0;
    ET.resets++;
}

/* ---- PS2_TEX_DUMP=1: every original image as <user dir>\textures\dump\<hash>.png (the HD pack's names), =2 also the
 * enhanced level 0 as <hash>_enh.png (tuning). Read back a few frames later; written on the render thread (debugging) -- */
#define ET_DJ 128
typedef struct { ID3D11Resource* st; uint32_t w, h, frame, tex; uint64_t raw; } et_dj;
static et_dj ET_dump[ET_DJ];
extern int settings_user_path(const wchar_t* name, wchar_t* out, size_t cap) __attribute__((weak));   /* (settings.c; tools: none) */
static uint64_t ET_dumped[8192];

static int et_dump_seen(uint64_t raw, int enh) {   /* 1: already dumped (else marks it) */
    uint64_t k = raw ^ (uint64_t)enh * 0x9E3779B97F4A7C15ull;
    if (!k) k = 1;
    for (uint32_t i = 0; i < 8192; i++) {
        uint64_t* s = &ET_dumped[((uint32_t)(k >> 40) + i) & 8191];
        if (*s == k) return 1;
        if (!*s) { *s = k; return 0; }
    }
    return 1;
}

/* an RGBA PNG (stored deflate blocks: no compressor needed for a debugging dump) */
static void et_png_chunk(FILE* f, const char* type, const uint8_t* d, uint32_t n) {
    static uint32_t tab[256];
    if (!tab[1])
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; }
    uint8_t h[8] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n, (uint8_t)type[0], (uint8_t)type[1], (uint8_t)type[2], (uint8_t)type[3] };
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < 4; i++) c = tab[(c ^ (uint8_t)type[i]) & 255] ^ (c >> 8);
    for (uint32_t i = 0; i < n; i++) c = tab[(c ^ d[i]) & 255] ^ (c >> 8);
    c = ~c;
    uint8_t t[4] = { (uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
    fwrite(t, 1, 4, f);
}
static void et_png_write(const wchar_t* path, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t pitch) {
    FILE* f = _wfopen(path, L"wb");
    if (!f) return;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ih[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                       (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 6, 0, 0, 0 };
    et_png_chunk(f, "IHDR", ih, 13);
    size_t rl = (size_t)h * (1 + 4 * (size_t)w), nb = (rl + 65534) / 65535;
    uint8_t* raw = malloc(rl);
    uint8_t* z = malloc(2 + rl + nb * 5 + 4);
    if (raw && z) {
        uint8_t* p = raw;
        for (uint32_t y = 0; y < h; y++) { *p++ = 0; memcpy(p, rgba + (size_t)y * pitch, 4 * (size_t)w); p += 4 * (size_t)w; }
        uint8_t* q = z;
        *q++ = 0x78; *q++ = 0x01;
        uint32_t a = 1, b = 0;
        for (size_t o = 0; o < rl; o += 65535) {
            size_t n = rl - o < 65535 ? rl - o : 65535;
            *q++ = o + n == rl; *q++ = (uint8_t)n; *q++ = (uint8_t)(n >> 8); *q++ = (uint8_t)~n; *q++ = (uint8_t)(~n >> 8);
            memcpy(q, raw + o, n);
            for (size_t i = 0; i < n; i++) { a = (a + raw[o + i]) % 65521; b = (b + a) % 65521; }
            q += n;
        }
        uint32_t ad = b << 16 | a;
        *q++ = (uint8_t)(ad >> 24); *q++ = (uint8_t)(ad >> 16); *q++ = (uint8_t)(ad >> 8); *q++ = (uint8_t)ad;
        et_png_chunk(f, "IDAT", z, (uint32_t)(q - z));
        et_png_chunk(f, "IEND", NULL, 0);
    }
    free(raw); free(z);
    fclose(f);
}

static void et_dump_add(ID3D11Resource* st, uint32_t w, uint32_t h, int tex, uint64_t raw) {
    for (int i = 0; i < ET_DJ; i++)
        if (!ET_dump[i].st) { et_dj d = { st, w, h, ET.frame, (uint32_t)tex, raw }; ET_dump[i] = d; return; }
    ID3D11Resource_Release(st);
}

/* the original image of entry idx (its decoded words, as they are now) */
static void et_dump_orig(const et_ent* e) {
    if (et_dump_seen(e->raw, 0)) return;
    uint32_t w = 1u << e->lw, h = 1u << e->lh;
    ID3D11Buffer* s = et_buf(w * h * 4u, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    if (!s) return;
    D3D11_BOX bx = { e->base * 4u, 0, 0, (e->base + w * h) * 4u, 1, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(gpu_rt.ctx, (ID3D11Resource*)s, 0, 0, 0, 0, (ID3D11Resource*)TD.buf, 0, &bx);
    et_dump_add((ID3D11Resource*)s, w, h, 0, e->raw);
}

/* level 0 of the image in slot id */
static void et_dump_enh(int32_t id, uint64_t raw) {
    if (et_dump_seen(raw, 1)) return;
    int cls = id >> 16;
    uint32_t S = 64u << cls, nm = (uint32_t)cls + 7u, slice = (uint32_t)(id & 0xFFFF);
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = td.Height = S; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* t = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &t))) return;
    ID3D11DeviceContext_CopySubresourceRegion(gpu_rt.ctx, (ID3D11Resource*)t, 0, 0, 0, 0, (ID3D11Resource*)ET.arr[cls],
                                              slice * nm, NULL);
    et_dump_add((ID3D11Resource*)t, S, S, 1, raw);
}

static void et_dump_poll(void) {
    wchar_t dir[600];
    for (int i = 0; i < ET_DJ; i++) {
        et_dj* d = &ET_dump[i];
        if (!d->st || ET.frame - d->frame < 3) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        if (ID3D11DeviceContext_Map(gpu_rt.ctx, d->st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
        if (settings_user_path && settings_user_path(L"textures", dir, 560)) {
            CreateDirectoryW(dir, NULL);
            wcscat(dir, L"\\dump");
            CreateDirectoryW(dir, NULL);
            wchar_t path[700];
            static const wchar_t* const sfx[4] = { L"", L"_enh", L"_ai", L"_nrm" };   /* (ai: 2, 3) */
            _snwprintf(path, 700, L"%ls\\%016llx%ls.png", dir, (unsigned long long)d->raw, sfx[d->tex & 3]);
            path[699] = 0;
            uint8_t* px = malloc((size_t)d->w * d->h * 4);
            if (px) {
                for (uint32_t y = 0; y < d->h; y++) {
                    const uint8_t* s = (const uint8_t*)m.pData + (size_t)y * (d->tex ? m.RowPitch : d->w * 4u);
                    uint8_t* o = px + (size_t)y * d->w * 4;
                    for (uint32_t x = 0; x < d->w; x++) {   /* (GS alpha 0x80 = 1.0 -> PNG 255) */
                        o[x * 4] = s[x * 4]; o[x * 4 + 1] = s[x * 4 + 1]; o[x * 4 + 2] = s[x * 4 + 2];
                        uint32_t a = d->tex == 3 ? s[x * 4 + 3] : (uint32_t)s[x * 4 + 3] * 255u / 128u;
                        o[x * 4 + 3] = (uint8_t)(a > 255 ? 255 : a);
                    }
                }
                et_png_write(path, px, d->w, d->h, d->w * 4);
                free(px);
            }
        }
        ID3D11DeviceContext_Unmap(gpu_rt.ctx, d->st, 0);
        ID3D11Resource_Release(d->st);
        d->st = NULL;
    }
}

/* ---- the HD-pack folder: <user dir>\textures\<hash>.png (16 hex digits: the original image's content hash, as
 * PS2_TEX_DUMP names it) replaces that image. The folder is listed once (when Enhanced textures start); a file is
 * decoded with Windows' own image decoder (WIC, loaded at run time) on a worker thread and uploaded at a frame start.
 * Any size: its width and height must be powers of two up to 2048 (else it is skipped, with a log line); alpha 255 =
 * the GS's 0x80 (opaque). ----------------------------------------------------------------------------------------------- */
#include <wincodec.h>
#define ET_HDMIX 0x5D3C9A7E11F04B2Bull   /* (an HD image's key: never equal to a built image's) */
#define ET_HDQ 256
typedef struct { uint64_t raw; uint32_t w, h; uint32_t* px; int state; } et_hdjob;   /* state 1 queued, 2 done, 3 failed */
static struct {
    int listed, n, thread_on;
    uint64_t* have;                      /* hashes with a file (sorted) */
    et_hdjob q[ET_HDQ];
    CRITICAL_SECTION cs;
    HANDLE ev, th;
    ID3D11Buffer* buf;
    ID3D11ShaderResourceView* srv;
} EH;
extern const wchar_t* settings_user_dir(void) __attribute__((weak));

static int et_hd_cmp(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}
static void et_hd_list(void) {
    EH.listed = 1;
    if (!ET.hd || !settings_user_path) return;
    wchar_t pat[600];
    if (!settings_user_path(L"textures", pat, 580)) return;
    wcscat(pat, L"\\*.png");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    static int cap;                      /* (reimagine-crash: a second listing starts empty, within its own capacity) */
    EH.n = 0;
    do {
        if (wcslen(fd.cFileName) != 20) continue;   /* (16 hex digits + ".png") */
        wchar_t* end = NULL;
        uint64_t v = _wcstoui64(fd.cFileName, &end, 16);
        if (end != fd.cFileName + 16) continue;
        if (EH.n >= cap) { int nc = cap ? cap * 2 : 64; uint64_t* a = realloc(EH.have, (size_t)nc * sizeof *a); if (!a) break; EH.have = a; cap = nc; }
        EH.have[EH.n++] = v;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (EH.n) qsort(EH.have, (size_t)EH.n, sizeof *EH.have, et_hd_cmp);
    gpu_log("tex: HD folder: %d replacement textures", EH.n);
}
static int et_hd_has(uint64_t raw) {
    if (!EH.n) return 0;
    return bsearch(&raw, EH.have, (size_t)EH.n, sizeof raw, et_hd_cmp) != NULL;
}

/* the worker: decodes queued files with WIC into RGBA8 */
static const GUID et_clsid_wic = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID et_iid_wic = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID et_fmt_rgba = { 0xf5c7ad2d, 0x6a8d, 0x43dd, { 0xa7, 0xa8, 0xa2, 0x99, 0x35, 0x26, 0x1a, 0xe9 } };
static int et_hd_decode(IWICImagingFactory* f, const wchar_t* path, et_hdjob* j) {
    IWICBitmapDecoder* d = NULL;
    IWICBitmapFrameDecode* fr = NULL;
    IWICFormatConverter* cv = NULL;
    int ok = 0;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(d, 0, &fr)) && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &cv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource*)fr, &et_fmt_rgba, WICBitmapDitherTypeNone, NULL, 0.0,
                                                 WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        IWICFormatConverter_GetSize(cv, &w, &h);
        if (w >= 1 && h >= 1 && w <= 2048 && h <= 2048 && !(w & (w - 1)) && !(h & (h - 1))) {
            j->px = malloc((size_t)w * h * 4);
            if (j->px && SUCCEEDED(IWICFormatConverter_CopyPixels(cv, NULL, w * 4, w * h * 4, (BYTE*)j->px))) {
                for (size_t i = 0; i < (size_t)w * h; i++) {   /* (alpha 255 -> the GS's 0x80) */
                    uint32_t p = j->px[i], a = ((p >> 24) * 128u + 127u) / 255u;
                    j->px[i] = (p & 0xFFFFFFu) | a << 24;
                }
                j->w = w; j->h = h;
                ok = 1;
            }
        } else gpu_log("tex: HD %016llx.png is %ux%u: width and height must be powers of two up to 2048", (unsigned long long)j->raw, w, h);
    }
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (d) IWICBitmapDecoder_Release(d);
    if (!ok) { free(j->px); j->px = NULL; }
    return ok;
}
static DWORD WINAPI et_hd_thread(LPVOID unused) {
    (void)unused;
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
    if (!f) gpu_log("tex: no Windows image decoder (WIC): the HD folder is not used");
    for (;;) {
        WaitForSingleObject(EH.ev, INFINITE);
        for (;;) {
            et_hdjob* j = NULL;
            EnterCriticalSection(&EH.cs);
            for (int i = 0; i < ET_HDQ && !j; i++) if (EH.q[i].state == 1) j = &EH.q[i];
            LeaveCriticalSection(&EH.cs);
            if (!j) break;
            wchar_t path[700];
            int ok = 0;
            if (f && settings_user_path && settings_user_path(L"textures", path, 600)) {
                size_t l = wcslen(path);
                _snwprintf(path + l, 700 - l, L"\\%016llx.png", (unsigned long long)j->raw);
                path[699] = 0;
                ok = et_hd_decode(f, path, j);
            }
            EnterCriticalSection(&EH.cs);
            j->state = ok ? 2 : 3;
            LeaveCriticalSection(&EH.cs);
        }
    }
    return 0;
}

/* entry idx waits for the HD image of its content (queued once per hash) */
static void et_hd_request(int32_t idx) {
    et_ent* e = &ET.e[idx];
    e->st = ES_HDLOAD;
    if (!EH.thread_on) {
        InitializeCriticalSection(&EH.cs);
        EH.ev = CreateEventW(NULL, FALSE, FALSE, NULL);
        EH.th = EH.ev ? CreateThread(NULL, 0, et_hd_thread, NULL, 0, NULL) : NULL;
        EH.thread_on = EH.th ? 1 : -1;
    }
    if (EH.thread_on < 0) { e->st = ES_NONE; ET.hd = 0; return; }
    EnterCriticalSection(&EH.cs);
    int have = 0, slot = -1;
    for (int i = 0; i < ET_HDQ; i++) {
        if (EH.q[i].state && EH.q[i].raw == e->raw) have = 1;
        if (!EH.q[i].state && slot < 0) slot = i;
    }
    if (!have && slot >= 0) { EH.q[slot].raw = e->raw; EH.q[slot].px = NULL; EH.q[slot].state = 1; }
    LeaveCriticalSection(&EH.cs);
    if (!have && slot < 0) { e->st = ES_WANT; return; }   /* (the queue is full: asked again at the next hash) */
    SetEvent(EH.ev);
}

/* (frame start) finished HD files: their images, linked to every entry waiting for them */
static int et_hd_poll(int* bound) {
    if (EH.thread_on <= 0) return 0;
    int n = 0;
    for (int i = 0; i < ET_HDQ; i++) {
        EnterCriticalSection(&EH.cs);
        int st = EH.q[i].state;
        LeaveCriticalSection(&EH.cs);
        if (st != 2 && st != 3) continue;
        et_hdjob* j = &EH.q[i];
        int32_t id = -1;
        uint64_t key = j->raw ^ ET_HDMIX;
        if (st == 2) {
            if (!EH.buf) EH.buf = raw_default(2048u * 2048u * 4u, NULL, &EH.srv, NULL);
            uint32_t S = j->w > j->h ? j->w : j->h;
            int cls = et_class_of(0, 0, (int)(S < 64 ? 64 : S));
            id = EH.buf ? et_alloc(cls) : -1;
            if (id >= 0) {
                D3D11_BOX bx = { 0, 0, 0, j->w * j->h * 4u, 1, 1 };
                ID3D11DeviceContext_UpdateSubresource(gpu_rt.ctx, (ID3D11Resource*)EH.buf, 0, &bx, j->px, 0, 0);
                et_slot* s = et_slot_of(id);
                s->used = 1; s->hd = 1; s->ai = 0; s->hash = key; s->first = -1; s->last = ET.frame;
                et_ent t;
                memset(&t, 0, sizeof t);
                for (uint32_t k = 0; k < 12; k++) { if ((1u << k) == j->w) t.lw = (uint8_t)k; if ((1u << k) == j->h) t.lh = (uint8_t)k; }
                for (int32_t k = 0; k < TD_HASH; k++)   /* (addressing and alpha test of a use) */
                    if (ET.e[k].st == ES_HDLOAD && ET.e[k].raw == j->raw) { t.flags = ET.e[k].flags; t.ref = ET.e[k].ref; break; }
                if (!*bound) {
                    ID3D11ShaderResourceView* n1 = NULL;
                    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 13, 1, &n1);
                    *bound = 1;
                }
                if (!et_build(&t, id, EH.srv, 0, 1)) { et_batch_run(); et_build(&t, id, EH.srv, 0, 1); }
                et_batch_run();              /* (one upload buffer: each HD image is built before the next one comes) */
                et_map_put(key, id);
                ET.n_hd++;
            }
        }
        for (int32_t k = 0; k < TD_HASH; k++) {
            et_ent* e = &ET.e[k];
            if (e->st != ES_HDLOAD || e->raw != j->raw) continue;
            if (id >= 0) et_link(k, id);
            else { e->st = ES_QUEUED; if (ET.qn < TD_HASH) ET.q[ET.qn++] = k; else e->st = ES_NONE; }   /* (failed: built) */
        }
        if (st == 3 || id < 0) {         /* (no HD image: this hash is built from now on) */
            for (int k = 0; k < EH.n; k++) if (EH.have[k] == j->raw) EH.have[k] = 0;
            if (EH.n) qsort(EH.have, (size_t)EH.n, sizeof *EH.have, et_hd_cmp);
        }
        free(j->px);
        j->px = NULL;
        EnterCriticalSection(&EH.cs);
        j->state = 0;
        LeaveCriticalSection(&EH.cs);
        n++;
    }
    return n;
}

static void et_release_late(void) {
    EB.n = 0;
    EB.lv_used = 0;
    EB.dd_used = 0;
    for (int i = 0; i < ET_DJ; i++) if (ET_dump[i].st) { ID3D11Resource_Release(ET_dump[i].st); ET_dump[i].st = NULL; }
    GPU_RELEASE(EH.srv);
    GPU_RELEASE(EH.buf);
    if (EH.thread_on > 0) {              /* (decoded files not yet uploaded are dropped; their entries ask again) */
        EnterCriticalSection(&EH.cs);
        for (int i = 0; i < ET_HDQ; i++) if (EH.q[i].state == 2 || EH.q[i].state == 3) { free(EH.q[i].px); EH.q[i].px = NULL; EH.q[i].state = 0; }
        LeaveCriticalSection(&EH.cs);
    }
}

/* a hashed entry: keep its image (same content), share one, load an HD file, or queue a build */
static void et_resolve(int32_t idx) {
    et_ent* e = &ET.e[idx];
    et_slot* cur = et_slot_of(e->slot);
    if (cur) {
        if (cur->hash == e->hash || cur->hash == (e->raw ^ ET_HDMIX)) { e->st = ES_READY; cur->last = ET.frame; return; }
        et_unlink(idx);                  /* (the content changed) */
        if (!et_fix_on() && e->nchg && ET.frame - e->chg < 1800) { e->st = ES_REJECT; ET.n_reject++; et_swap_note(idx, 'O'); return; }
        if (!et_fix_on()) { e->nchg++; e->chg = ET.frame; }
        ET.n_change++;
    }
    /* (reimagine) a content seen before (its image is in the cache) is shared at once: textures the game swaps in and out
     * at one place (characters' parts streamed into a shared area) keep their images instead of falling back to the
     * original sampling on the second change, which made Sly's textures flip between AI, Enhanced and original. Only NEW
     * contents count: four within 30 s is an animation (CLUT cycling), left to the original sampling. */
    if (et_fix_on() && !(ET.hd && et_hd_has(e->raw))) {
        int32_t sid = et_map_find(e->hash);
        if (sid >= 0) { et_link(idx, sid); ET.n_share++; return; }
        if (cur) {
            if (ET.frame - e->chg >= 3600) { e->nchg = 0; e->chg = ET.frame; }
            if (++e->nchg >= 4) { e->st = ES_REJECT; ET.n_reject++; et_swap_note(idx, 'O'); return; }
        }
    }
    if (ET.hd && et_hd_has(e->raw)) {    /* (an HD-pack file: its image, shared by every entry of that content) */
        int32_t hid = et_map_find(e->raw ^ ET_HDMIX);
        if (hid >= 0) et_link(idx, hid);
        else et_hd_request(idx);
        return;
    }
    int32_t id = et_map_find(e->hash);
    if (id >= 0) { et_link(idx, id); ET.n_share++; return; }
    if (ET.qn < TD_HASH) { ET.q[ET.qn++] = idx; e->st = ES_QUEUED; }
}

/* the start of a frame's execution (before the prepass): read-backs, hash requests, builds within the frame's budget */
static void et_frame_body(void);
/* (reimagine) the builds' GPU time per frame (timestamps around them, read back a few frames later) and the cap that
 * follows from it: what the frame leaves of 80 % of the frame interval at the player's frame rate (from the last
 * frame's GPU time without the builds), 0.25..2 ms, converted with the measured ms per million output texels. The fixed
 * 1 M texels a frame of 0.1.1207 cost ~3 ms under Reimagined (surface maps too): 10 ms frames at 120 fps. */
extern double frame_rate_target_hz(void) __attribute__((weak));
static struct { ID3D11Query* q[8][2]; uint8_t busy[8]; uint32_t tex[8], head; double ms_per_m, last_ms; uint64_t n, over; void* dev; uint32_t cap; } ETB = { .ms_per_m = 3.0 };
static void etb_poll(void) {
    if (ETB.dev != (void*)gpu_rt.dev) { memset(ETB.q, 0, sizeof ETB.q); memset(ETB.busy, 0, sizeof ETB.busy); ETB.dev = gpu_rt.dev; }   /* (a new device: its own queries) */
    for (int k = 0; k < 8; k++) {
        if (!ETB.busy[k]) continue;
        UINT64 a, b;
        if (ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)ETB.q[k][0], &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)ETB.q[k][1], &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        ETB.busy[k] = 0;
        if (!gpu_ts_freq || b < a || ETB.tex[k] < 16384) continue;
        double ms = (double)(b - a) * 1000.0 / (double)gpu_ts_freq;
        ETB.last_ms = ms;
        double r = ms / ((double)ETB.tex[k] / 1048576.0);
        if (r > 0.05 && r < 50.0) ETB.ms_per_m = ETB.ms_per_m * 0.8 + r * 0.2;
    }
}
static uint32_t etb_cap(void) {
    if (!et_fix_on()) return ET.frame_out;
    double hz = frame_rate_target_hz ? frame_rate_target_hz() : 60.0;
    double room = 2.0;
    if (hz > 1.0 && gpu_frame_ms_last > 0.0) {
        room = 0.8 * 1000.0 / hz - (gpu_frame_ms_last - ETB.last_ms);
        if (room > 2.0) room = 2.0;
    } else if (hz <= 1.0) room = 1.0;
    if (room < 0.25) room = 0.25;
    double t = room / ETB.ms_per_m * 1048576.0;
    uint32_t cap = t > (double)ET.frame_out ? ET.frame_out : t < 65536.0 ? 65536u : (uint32_t)t;
    return cap;
}
/* PS2_TEX_TIME=1: the GPU time of the frame-start work (hashes + builds) of frames that built images, from timestamps
 * around it (a ring read back a few frames later): "tex-time" lines, ms per frame and per million output texels */
static void et_time_poll(void) {
    for (uint32_t k = 0; k < ET_TQ; k++) {
        if (!ETT.busy[k]) continue;
        UINT64 a, b;
        if (ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)ETT.q[k][0], &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)ETT.q[k][1], &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        ETT.busy[k] = 0;
        if (!gpu_ts_freq || b < a) continue;
        double ms = (double)(b - a) * 1000.0 / (double)gpu_ts_freq;
        ETT.ms += ms; ETT.texels += (uint64_t)ETT.tex[k] << 10; ETT.n++;
        if (ms > ETT.mx) ETT.mx = ms;
        if (ETT.n % 20 == 0)
            gpu_log("tex-time: %u frames with builds: GPU %.3f ms on average, max %.2f; %.3f ms per million output texels", ETT.n,
                    ETT.ms / ETT.n, ETT.mx, ETT.texels ? ETT.ms / ((double)ETT.texels / 1048576.0) : 0.0);
    }
}
static void et_frame_begin(void) {
    ET.frame++;
    {                                    /* (ai) Reimagined on / off: every image is built again with / without its parts */
        static int was;
        int ra = et_cfg() && ra_cfg();
        if (ra != was && ET.init) et_release();
        was = ra;
        ra_on_now = ra;
    }
    if (!et_cfg() || !et_init()) return;
    if (ra_on_now && !ra_init()) ra_on_now = 0;
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceCounter(&q0);
    uint64_t h0 = ET.n_hash, b0 = ET.n_build, g0 = ET.n_grow, s0 = ET.n_share;
    if (ETT.on < 0) { const char* e = getenv("PS2_TEX_TIME"); ETT.on = e && *e == '1'; }
    uint32_t ts = ETT.head % ET_TQ;
    int timed = 0;
    if (ETT.on) {
        et_time_poll();
        if (!ETT.q[ts][0]) {
            D3D11_QUERY_DESC qt = { D3D11_QUERY_TIMESTAMP, 0 };
            ID3D11Device_CreateQuery(gpu_rt.dev, &qt, &ETT.q[ts][0]);
            ID3D11Device_CreateQuery(gpu_rt.dev, &qt, &ETT.q[ts][1]);
        }
        timed = !ETT.busy[ts] && ETT.q[ts][0] && ETT.q[ts][1];
        if (timed) ID3D11DeviceContext_End(gpu_rt.ctx, (ID3D11Asynchronous*)ETT.q[ts][0]);
    }
    et_frame_body();
    if (timed && ET.out_frame) {
        ID3D11DeviceContext_End(gpu_rt.ctx, (ID3D11Asynchronous*)ETT.q[ts][1]);
        ETT.tex[ts] = ET.out_frame >> 10;
        ETT.busy[ts] = 1;
        ETT.head++;
    }
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&qf);
    double ms = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart;   /* (the render thread's cost) */
    if (ms > ET.cpu_max) ET.cpu_max = ms;
    if (ET.log && (ms > 2.0 || ET.n_grow != g0))
        gpu_log("tex: frame %u start %.2f ms: %llu hashes, %llu builds (%u K texels), %llu shared, %llu class growths", ET.frame, ms,
                (unsigned long long)(ET.n_hash - h0), (unsigned long long)(ET.n_build - b0), ET.out_frame >> 10,
                (unsigned long long)(ET.n_share - s0), (unsigned long long)(ET.n_grow - g0));
    if (ET.log && ET.frame % 600 == 0) gpu_log("tex: frame start CPU max %.2f ms", ET.cpu_max);
    if (ET.frame % 1200 == 0) et_swap_summary();
}
static void et_frame_body(void) {
    ID3D11DeviceContext* c = gpu_rt.ctx;
    ET.out_frame = 0;
    if (ET.dump) et_dump_poll();
    if (!EH.listed) et_hd_list();
    /* 1. hashes of earlier frames */
    for (uint32_t k = 1; k <= ET_RING; k++) {
        uint32_t r = (ET.ring + k) % ET_RING;
        if (!ET.stage_n[r]) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        if (ID3D11DeviceContext_Map(c, (ID3D11Resource*)ET.stage[r], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK)
            continue;
        const uint32_t* w = m.pData;
        for (uint32_t i = 0; i < ET.stage_n[r]; i++) {
            et_req q = ET.req[r][i];
            et_ent* e = &ET.e[q.ent];
            if (e->st != ES_HASHING || e->ver != q.ver || !TD.ent[q.ent].live || TD.ent[q.ent].ver != q.ver) continue;
            e->raw = (uint64_t)w[i * 4 + 1] << 32 | w[i * 4];
            e->hash = e->raw ^ (uint64_t)e->flags << 61 ^ (uint64_t)(e->ref != 0) << 60;
            if (!e->hash) e->hash = 1;
            if (ET.dump) et_dump_orig(e);
            et_resolve(q.ent);
        }
        ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)ET.stage[r], 0);
        ET.stage_n[r] = 0;
    }
    int bound = 0;
    /* 2. hash requests for the entries noted since: one dispatch, one group per request */
    uint32_t r = ET.ring;
    if (!ET.stage_n[r] && TD.srv) {
        uint32_t n = 0;
        static et_job hj[ET_HREQ];
        for (int32_t i = 0; i < TD_HASH && n < ET_HREQ; i++) {
            et_ent* e = &ET.e[i];
            if (e->st != ES_WANT) continue;
            const td_ent* t = &TD.ent[i];
            if (!t->live || t->ver == 0xFFFFFFFFu || t->base != e->base) { e->st = ES_NONE; continue; }
            memset(&hj[n], 0, sizeof hj[n]);
            hj[n].src = e->base; hj[n].lw = e->lw; hj[n].lh = e->lh;
            e->st = ES_HASHING;
            e->ver = t->ver;
            ET.req[r][n].ent = i;
            ET.req[r][n].ver = t->ver;
            n++;
        }
        if (n) {
            ID3D11ShaderResourceView* n1 = NULL;
            ID3D11DeviceContext_PSSetShaderResources(c, 13, 1, &n1);
            bound = 1;
            D3D11_BOX jb = { 0, 0, 0, n * (uint32_t)sizeof(et_job), 1, 1 };
            ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource*)ET.hjobs, 0, &jb, hj, 0, 0);
            ID3D11ShaderResourceView* s2[2] = { TD.srv, ET.hjobs_srv };
            ID3D11DeviceContext_CSSetShaderResources(c, 0, 2, s2);
            ID3D11UnorderedAccessView* u3[4] = { NULL, NULL, NULL, ET.hout_uav };
            ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 4, u3, NULL);
            ID3D11DeviceContext_CSSetConstantBuffers(c, 3, 1, &ET.cb);
            et_cb_set(0, 0);
            ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_et_hash), NULL, 0);
            ID3D11DeviceContext_Dispatch(c, 1, 1, n);
            et_cs_unbind();
            D3D11_BOX bx = { 0, 0, 0, n * 16u, 1, 1 };
            ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource*)ET.stage[r], 0, 0, 0, 0, (ID3D11Resource*)ET.hout, 0, &bx);
            ET.stage_n[r] = n;
            ET.ring = (ET.ring + 1) % ET_RING;
            ET.n_hash += n;
            gpu_rt.n_dispatches++;
        }
    }
    /* 3. HD-pack images decoded since, then builds (queued into one batch, run together) */
    if (ET.hd) et_hd_poll(&bound);
    uint32_t qi = 0;
    uint32_t cap = etb_cap();
    ETB.cap = cap;   /* (the AI images' applies share it: gpu_tex_ai.h) */
    int tb = 0;
    uint32_t tk = ETB.head % 8;
    if (et_fix_on()) {                   /* (the builds timed: the cap's rate) */
        etb_poll();
        if (!ETB.q[tk][0]) {
            D3D11_QUERY_DESC qt = { D3D11_QUERY_TIMESTAMP, 0 };
            ID3D11Device_CreateQuery(gpu_rt.dev, &qt, &ETB.q[tk][0]);
            ID3D11Device_CreateQuery(gpu_rt.dev, &qt, &ETB.q[tk][1]);
        }
        tb = !ETB.busy[tk] && ETB.q[tk][0] && ETB.q[tk][1] && ET.qn;
        if (tb) ID3D11DeviceContext_End(c, (ID3D11Asynchronous*)ETB.q[tk][0]);
    }
    for (; qi < ET.qn && ET.out_frame < cap; qi++) {
        int32_t idx = ET.q[qi];
        et_ent* e = &ET.e[idx];
        const td_ent* t = &TD.ent[idx];
        if (e->st != ES_QUEUED) continue;
        if (!t->live || t->ver != e->ver || t->base != e->base) { e->st = ES_WANT; continue; }
        int32_t id = et_map_find(e->hash);
        if (id < 0) {
            int cls = et_class_of(e->lw, e->lh, 0);
            if (EB.n >= ET_JOBS) break;
            id = et_alloc(cls);
            if (id < 0) { e->st = ES_NONE; ET.n_reject++; continue; }   /* (budget: tried again at a later use) */
            et_slot* s = et_slot_of(id);
            s->used = 1; s->hd = 0; s->ai = 0; s->hash = e->hash; s->first = -1; s->last = ET.frame;
            if (!bound) {
                ID3D11ShaderResourceView* n1 = NULL;
                ID3D11DeviceContext_PSSetShaderResources(c, 13, 1, &n1);
                bound = 1;
            }
            if (!et_build(e, id, TD.srv, e->base, 0)) { et_batch_run(); et_build(e, id, TD.srv, e->base, 0); }
            et_map_put(e->hash, id);
            ET.n_build++;
            if (ET.dump == 2) { et_batch_run(); et_dump_enh(id, e->raw); }
        } else ET.n_share++;
        et_link(idx, id);
    }
    et_batch_run();
    if (tb) {
        ID3D11DeviceContext_End(c, (ID3D11Asynchronous*)ETB.q[tk][1]);
        ETB.tex[tk] = ET.out_frame;
        ETB.busy[tk] = 1;
        ETB.head++;
    }
    if (ET.out_frame) gpu_stats_cur()->tex_built += ET.out_frame >> 10;   /* (the cost of frames with builds: tex-build) */
    memmove(ET.q, ET.q + qi, (ET.qn - qi) * sizeof ET.q[0]);
    ET.qn -= qi;
    if (ra_on_now) ra_frame(&bound);     /* (ai) cached AI images, read-backs, the net's slice of this frame */
    if (bound) {
        et_cs_unbind();
        ID3D11DeviceContext_PSSetShaderResources(c, 13, 1, &TD.srv);
        X.state_calls += 10;
    }
    if (ET.log && ET.frame % 600 == 0)
        gpu_log("tex: %llu hashed, %llu built (%.1f M texels), %llu HD, %llu shared, %llu re-decoded (%llu changed), %llu rejected, "
                "%llu evicted, %u queued; %.0f MB",
                (unsigned long long)ET.n_hash, (unsigned long long)ET.n_build, ET.n_texels / 1e6, (unsigned long long)ET.n_hd, (unsigned long long)ET.n_share,
                (unsigned long long)ET.n_redec, (unsigned long long)ET.n_change, (unsigned long long)ET.n_reject,
                (unsigned long long)ET.n_evict, ET.qn, ET.bytes / 1048576.0);
}

/* the pixel shaders' bindings (t30..t36, s4 / s5, b8); w: the surface width (PS2_TEX_SPLIT) */
static void et_bind_ps(int w) {
    ID3D11DeviceContext* c = gpu_rt.ctx;
    float v[4] = { exp2f(ET.bias), ET.split ? (float)(w / 2) : -1.0f, (float)ET.dbg, 0.0f };
    if (memcmp(v, ET.pcb_val, sizeof v)) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(c, (ID3D11Resource*)ET.pcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, v, sizeof v);
            ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)ET.pcb, 0);
            memcpy(ET.pcb_val, v, sizeof v);
        }
    }
    ID3D11ShaderResourceView* s[7] = { ET.tab_srv, ET.srv[0], ET.srv[1], ET.srv[2], ET.srv[3], ET.srv[4], ET.srv[5] };
    ID3D11DeviceContext_PSSetShaderResources(c, 30, 7, s);
    ID3D11DeviceContext_PSSetSamplers(c, 4, 2, ET.smp);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 8, 1, &ET.pcb);
    if (ra_on_now && etn_enabled()) etn_bind_ps();   /* (ai) the surface maps for the lighting */
}

#include "gpu_tex_ai.h"                  /* (ai) Reimagined textures + surface maps */
