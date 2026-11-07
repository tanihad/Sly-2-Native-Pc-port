/* (reimagine) Look = Reimagined: the modern frame's lighting reimagined (notes/reimagined-lighting.md; shaders/light.hlsl
 * REIM). Included once by gpu_light.h: everything here runs only while lt_reim() (graphics.look = reimagined, or
 * PS2_REIMAGINE=1), so Faithful / Retro never reach it.
 *
 *   scene    light_note gives every R2 segment of the frame to reim_note: the world x clip matrix shared by most entries
 *            (the static world's: its object space is the world) and the lit entries' light blocks -> the key light's
 *            world direction and colour (the brightest directional light the game lights its characters with). */

#define RE_NCL 16
typedef struct {
    float pv[16], ipv[16];               /* world x clip (row vectors) and its inverse */
    int pv_ok, pv_n;
    /* the lit entries' lights, clustered by world direction (a directional light repeats with one direction) */
    struct { float d[3], c[3], br; int n; } cl[RE_NCL];
    int ncl;
    uint32_t ops[256];                   /* the frame's R2 segments (fd->cmd offsets: the shadow casters) */
    int nops;
    const gpu_front_frame_data* fd;
    uint32_t serial;
    int nlit;
} reim_scene;
static reim_scene RS, RS_last;
static struct {
    int env, log, dbg, ss, shafts, fog, grade, smap, sm_size;
    float sm_k, sm_dist;
    float sss_k, sss_len, sss_th, bl_t, bl_k, bl_i, halo, sat, con, vib, expo, fog0, fogd, fogmax, hfog, hfog_h, vig, sh_i,
        night, stat_a, rim, spec, clar, key_i, ao_r, ao_k, ao_min;
    /* the key light kept from frame to frame (smoothed) */
    float key[3], keyc[3], keyw;
    float kv[3];                         /* the key light in view space (the last post pass): the surface maps' relief */
    float relief, cavity, nmap;
    int have_key;
} RE;
enum { RT_SH, RT_SH2, RT_B6, RT_SHAFT, RT_PIC, RT_ST0, RT_ST1, RT_SCENE, RT_N };
static struct {
    ID3D11Texture2D* t[RT_N];
    ID3D11RenderTargetView* rtv[RT_N];
    ID3D11ShaderResourceView* srv[RT_N];
    int tw[RT_N], th[RT_N];
    DXGI_FORMAT tf[RT_N];
    ID3D11Buffer* cb;
    int st_cur;                          /* which statistics texture holds the latest */
    ID3D11Texture2D* sm;                 /* the key light's shadow map (R32, depth: 1 at the light) */
    ID3D11DepthStencilView* sm_dsv;
    ID3D11ShaderResourceView* sm_srv;
    int sm_n;
    uint64_t sm_frames, sm_draws;
    uint64_t frames;
} RR;

static void re_env(void) {
    if (RE.env) return;
    RE.env = 1;
    const char* e = getenv("PS2_REIM_LOG"); RE.log = e && *e ? atoi(e) : 0;
    e = getenv("PS2_REIM_DBG"); RE.dbg = e && *e ? atoi(e) : 0;      /* 1 shadow, 2 shafts, 3 world bands, 4 fog colour */
    e = getenv("PS2_REIM_SS"); RE.ss = !(e && *e == '0');
    e = getenv("PS2_REIM_SHAFTS"); RE.shafts = !(e && *e == '0');
    e = getenv("PS2_REIM_FOG"); RE.fog = !(e && *e == '0');
    e = getenv("PS2_REIM_GRADE"); RE.grade = !(e && *e == '0');
    e = getenv("PS2_REIM_SMAP"); RE.smap = !(e && *e == '0');   /* the key light's shadow map */
    RE.sm_size = (int)lt_envf("PS2_REIM_SM_SIZE", 2048.0f);
    if (RE.sm_size < 256) RE.sm_size = 256;
    if (RE.sm_size > 8192) RE.sm_size = 8192;
    RE.sm_k = lt_envf("PS2_REIM_SM_K", 0.5f);         /* its strength */
    RE.sm_dist = lt_envf("PS2_REIM_SM_DIST", 3500.0f); /* its reach from the camera (world units) */
    RE.sss_k = lt_envf("PS2_REIM_SS_K", 0.32f);      /* shadow strength (shadow map and contact shadows) */
    RE.key_i = lt_envf("PS2_REIM_KEY_I", 0.40f);     /* the key light's added share where it reaches */
    RE.sss_len = lt_envf("PS2_REIM_SS_LEN", 160.0f); /* its march length, world units */
    RE.sss_th = lt_envf("PS2_REIM_SS_TH", 60.0f);    /* an occluder's assumed thickness */
    RE.bl_t = lt_envf("PS2_REIM_BLOOM_T", 0.55f);
    RE.bl_k = lt_envf("PS2_REIM_BLOOM_K", 0.20f);
    RE.bl_i = lt_envf("PS2_REIM_BLOOM_I", 3.0f);
    RE.halo = lt_envf("PS2_REIM_HALO", 0.85f);       /* the wider bloom levels' weight */
    RE.sat = lt_envf("PS2_REIM_SAT", 1.06f);
    RE.con = lt_envf("PS2_REIM_CONTRAST", 1.12f);
    RE.vib = lt_envf("PS2_REIM_VIBRANCE", 0.35f);
    RE.expo = lt_envf("PS2_REIM_EXPOSURE", 1.06f);
    RE.fog0 = lt_envf("PS2_REIM_FOG0", 3000.0f);
    RE.fogd = lt_envf("PS2_REIM_FOGD", 1.0f / 30000.0f);
    RE.fogmax = lt_envf("PS2_REIM_FOGMAX", 0.22f);
    RE.hfog = lt_envf("PS2_REIM_HFOG", 1.0f / 20000.0f);
    RE.hfog_h = lt_envf("PS2_REIM_HFOG_H", 300.0f);
    RE.vig = lt_envf("PS2_REIM_VIG", 0.16f);
    RE.clar = lt_envf("PS2_REIM_CLARITY", 0.18f);   /* local contrast */
    RE.sh_i = lt_envf("PS2_REIM_SHAFT_I", 1.0f);
    RE.night = lt_envf("PS2_REIM_NIGHT", -1.0f);     /* (tests: 0 day .. 1 night; default from the picture) */
    RE.stat_a = lt_envf("PS2_REIM_STAT_A", 0.06f);   /* the statistics' smoothing per frame */
    RE.rim = lt_envf("PS2_REIM_RIM", 1.1f);          /* rim light on lit meshes (Faithful: 0.5) */
    RE.spec = lt_envf("PS2_REIM_SPEC", 0.45f);
    RE.relief = lt_envf("PS2_REIM_RELIEF", 0.6f);   /* surface maps (next/ai): relief lit from the key light's side */
    RE.cavity = lt_envf("PS2_REIM_CAVITY", 0.6f);   /* ... their cavity deepening creases */
    RE.nmap = lt_envf("PS2_REIM_NMAP", 1.0f);       /* ... their normals on the characters' lighting */     /* specular highlights on lit meshes (Faithful: none) */
    RE.ao_r = lt_envf("PS2_REIM_AO_R", 80.0f);
    RE.ao_k = lt_envf("PS2_REIM_AO_K", 3.5f);
    RE.ao_min = lt_envf("PS2_REIM_AO_MIN", 0.3f);
}

/* general 4 x 4 inverse (row-major); 0: singular */
static int re_inv4(const float* m, float* o) {
    double a[4][8];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-30) return 0;
        if (p != c) for (int k = 0; k < 8; k++) { double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        double iv = 1.0 / a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] *= iv;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            double f = a[r][c];
            if (f != 0.0) for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) o[r * 4 + c] = (float)a[r][c + 4];
    return 1;
}
static void re_mul4(const float* a, const float* b, float* o) {   /* o = a b (row-major) */
    float t[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) t[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
    memcpy(o, t, sizeof t);
}

static void reim_note(const gpu_front_frame_data* fd, const gpu_fop_r2* o, const gpu_r2_frame* fb) {
    re_env();
    if (fb->serial != RS.serial) {       /* a new game frame: the last one's result stays for the post pass */
        if (RS.serial) RS_last = RS;
        memset(&RS, 0, sizeof RS);
        RS.serial = fb->serial;
    }
    if (RS.nops < 256) RS.ops[RS.nops++] = (uint32_t)((const uint8_t*)o - fd->cmd);
    RS.fd = fd;
    uint32_t n = o->nent;
    if (o->ent_off + (size_t)n * sizeof(gpu_r2_entry) > fd->data_bytes) return;
    const gpu_r2_entry* ent = (const gpu_r2_entry*)(fd->data + o->ent_off);
    /* the most common matrix of the segment (the static world: object space = world space) */
    int best = -1, bestn = 0;
    {
        int cand[8], cn[8], nc = 0;      /* (the first 8 distinct matrices and how often each appears) */
        for (uint32_t i = 0; i < n && i < 512; i++) {
            int f = -1;
            for (int j = 0; j < nc && f < 0; j++) if (!memcmp(ent[i].m, ent[cand[j]].m, 64)) f = j;
            if (f >= 0) cn[f]++;
            else if (nc < 8) { cand[nc] = (int)i; cn[nc++] = 1; }
        }
        for (int j = 0; j < nc; j++) if (cn[j] > bestn) { bestn = cn[j]; best = cand[j]; }
    }
    if (best >= 0 && bestn > RS.pv_n && bestn >= 3) {
        float inv[16];
        if (re_inv4(ent[best].m, inv)) { memcpy(RS.pv, ent[best].m, 64); memcpy(RS.ipv, inv, 64); RS.pv_ok = 1; RS.pv_n = bestn; }
    }
    if (!RS.pv_ok) return;
    /* lit entries: their light blocks' lights in world space */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t xq = ent[i].pad[0];
        if (!xq) continue;
        size_t lb = (size_t)(xq - 1) * 16u;
        if (lb + 11 * 16 > fd->data_bytes) continue;
        const float* q = (const float*)(fd->data + lb);   /* T0..T2 [0..2], colours [4..7], ambient [8], thresholds [9] */
        float om[16];
        re_mul4(ent[i].m, RS.ipv, om);    /* object -> world */
        RS.nlit++;
        for (int k = 0; k < 4; k++) {
            float d[3] = { q[0 * 4 + k], q[1 * 4 + k], q[2 * 4 + k] };
            const float* c = q + (4 + k) * 4;
            float w[3];
            for (int j = 0; j < 3; j++) w[j] = d[0] * om[0 * 4 + j] + d[1] * om[1 * 4 + j] + d[2] * om[2 * 4 + j];
            float l = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
            float br = c[0] * 0.3f + c[1] * 0.59f + c[2] * 0.11f;
            if (RE.log >= 2 && fd->vblank % 300u < 2u && RS.nlit <= 6)
                gpu_log("reim: v%u ent %u light %d obj %.3f %.3f %.3f world %.3f %.3f %.3f |%.3f| col %.3f %.3f %.3f %.3f thr %.3f amb %.3f %.3f %.3f",
                        fd->vblank, i, k, d[0], d[1], d[2], l > 0 ? w[0] / l : 0, l > 0 ? w[1] / l : 0, l > 0 ? w[2] / l : 0, l, c[0], c[1], c[2], c[3],
                        q[9 * 4 + k], q[8 * 4], q[8 * 4 + 1], q[8 * 4 + 2]);
            /* f = min(d + d^3, threshold) x colour: with a negative colour the light comes from -direction */
            if (!(l > 1e-6f) || !(fabsf(br) > 1e-3f)) continue;
            float sg = br < 0.0f ? -1.0f : 1.0f;
            float u[3] = { sg * w[0] / l, sg * w[1] / l, sg * w[2] / l }, cc[3] = { sg * c[0], sg * c[1], sg * c[2] };
            br = fabsf(br);
            int f = -1;
            for (int j = 0; j < RS.ncl && f < 0; j++) {
                float dd = RS.cl[j].d[0] * u[0] + RS.cl[j].d[1] * u[1] + RS.cl[j].d[2] * u[2];
                if (dd > 0.995f * RS.cl[j].n) f = j;    /* (d: the sum of n unit vectors) */
            }
            if (f < 0 && RS.ncl < RE_NCL) { f = RS.ncl++; memset(&RS.cl[f], 0, sizeof RS.cl[f]); }
            if (f < 0) continue;
            for (int j = 0; j < 3; j++) { RS.cl[f].d[j] += u[j]; RS.cl[f].c[j] += cc[j]; }
            RS.cl[f].br += br;
            RS.cl[f].n++;
        }
    }
    if (RE.log && fd->vblank % 300u < 2u) {
        float cam[4] = { RS.ipv[8] / RS.ipv[11], RS.ipv[9] / RS.ipv[11], RS.ipv[10] / RS.ipv[11], 0 };   /* (clip x = y = w = 0) */
        gpu_log("reim: v%u seg ents %u world matrix x%d (pv row0 %.4g %.4g %.4g %.4g row2 %.4g %.4g %.4g %.4g) cam %.1f %.1f %.1f; %d light clusters",
                fd->vblank, n, RS.pv_n, RS.pv[0], RS.pv[1], RS.pv[2], RS.pv[3], RS.pv[8], RS.pv[9], RS.pv[10], RS.pv[11],
                cam[0], cam[1], cam[2], RS.ncl);
    }
}

static float reim_rim(void) { re_env(); return RE.rim; }
static float reim_spec(void) { re_env(); return RE.spec; }
/* (the R2 pipelines' r2_reim / r2_reim2: the key light's direction in the picture plane, relief, cavity, normal maps) */
static void reim_r2k(float* k) {
    re_env();
    memset(k, 0, 8 * sizeof(float));
    k[0] = RE.kv[0]; k[1] = RE.kv[1]; k[2] = RE.relief; k[3] = RE.cavity; k[4] = RE.nmap;
}   /* (the R2 pipelines' rim light under Reimagined) */

/* ---- the post pass -------------------------------------------------------------------------------------------- */
static int light_proj(float view[4], float z[2]);
static int lt_tex(int i, int w, int h, DXGI_FORMAT f);
static void lt_pass(ID3D11RenderTargetView* rtv, int w, int h, int ps, ID3D11BlendState* bs);
static void lt_srv(int slot, ID3D11ShaderResourceView* v);
static void lt_cb(const float* v, int n);

static void reim_release(void) {
    for (int i = 0; i < RT_N; i++) { GPU_RELEASE(RR.rtv[i]); GPU_RELEASE(RR.srv[i]); GPU_RELEASE(RR.t[i]); RR.tw[i] = RR.th[i] = 0; }
    GPU_RELEASE(RR.cb);
    GPU_RELEASE(RR.sm_dsv); GPU_RELEASE(RR.sm_srv); GPU_RELEASE(RR.sm);
    RR.sm_n = 0;
    RE.have_key = 0;
}
/* texture i: w x h of format f (mips: a full chain, GenerateMips; rt 0: a copy destination, shader view only) */
static int rr_tex(int i, int w, int h, DXGI_FORMAT f, int mips, int rt) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (RR.t[i] && RR.tw[i] == w && RR.th[i] == h && RR.tf[i] == f) return 1;
    GPU_RELEASE(RR.rtv[i]); GPU_RELEASE(RR.srv[i]); GPU_RELEASE(RR.t[i]);
    RR.tw[i] = RR.th[i] = 0;
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = mips ? 0 : 1; td.ArraySize = 1; td.Format = f;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rt ? D3D11_BIND_RENDER_TARGET : 0);
    td.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &RR.t[i]))) { RR.t[i] = NULL; return 0; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd, *psd = NULL;
    if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS) {
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        psd = &sd;
    }
    if ((rt && FAILED(ID3D11Device_CreateRenderTargetView(gpu_rt.dev, (ID3D11Resource*)RR.t[i], NULL, &RR.rtv[i]))) ||
        FAILED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)RR.t[i], psd, &RR.srv[i]))) {
        GPU_RELEASE(RR.rtv[i]); GPU_RELEASE(RR.srv[i]); GPU_RELEASE(RR.t[i]);
        return 0;
    }
    if (rt) { static const float zero[4] = { 0, 0, 0, 0 }; ID3D11DeviceContext_ClearRenderTargetView(gpu_rt.ctx, RR.rtv[i], zero); }
    RR.tw[i] = w; RR.th[i] = h; RR.tf[i] = f;
    return 1;
}
static void rr_unbind(void) {
    ID3D11ShaderResourceView* none[12] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 14, 12, none);
    ID3D11RenderTargetView* nr[2] = { NULL, NULL };
    ID3D11DeviceContext_OMSetRenderTargets(gpu_rt.ctx, 2, nr, NULL);
}

/* the key light of the frame: the brightest light cluster coming from above (a directional light repeats over the
 * lit entries); none: a high light from the camera's side. Smoothed from frame to frame (a cut: at once). */
static void reim_key(const reim_scene* s) {
    float best = 0.0f, d[3] = { 0.35f, 0.25f, 0.90f }, c[3] = { 1.0f, 0.95f, 0.85f };
    for (int i = 0; i < s->ncl; i++) {
        float l = sqrtf(s->cl[i].d[0] * s->cl[i].d[0] + s->cl[i].d[1] * s->cl[i].d[1] + s->cl[i].d[2] * s->cl[i].d[2]);
        if (!(l > 1e-6f)) continue;
        float up = s->cl[i].d[2] / l;
        float sc = s->cl[i].br * sqrtf((float)s->cl[i].n) * (up < 0.25f ? 0.0f : up > 0.5f ? 1.0f : (up - 0.25f) * 4.0f);
        if (sc > best) {
            best = sc;
            for (int j = 0; j < 3; j++) { d[j] = s->cl[i].d[j] / l; c[j] = s->cl[i].c[j] / (float)s->cl[i].n; }
        }
    }
    if (best <= 0.0f) {                  /* (no light from above: the fallback, in world space) */
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int j = 0; j < 3; j++) d[j] /= l;
    }
    if (!RE.have_key || RE.key[0] * d[0] + RE.key[1] * d[1] + RE.key[2] * d[2] < 0.7f) {
        memcpy(RE.key, d, sizeof d); memcpy(RE.keyc, c, sizeof c);
        RE.have_key = 1;
    } else {
        float l = 0.0f;
        for (int j = 0; j < 3; j++) { RE.key[j] += (d[j] - RE.key[j]) * 0.08f; RE.keyc[j] += (c[j] - RE.keyc[j]) * 0.08f; l += RE.key[j] * RE.key[j]; }
        l = sqrtf(l);
        for (int j = 0; j < 3; j++) RE.key[j] /= l;
    }
}

/* the key light's shadow map: an orthographic view along the key light around the camera's frustum slice up to
 * RE.sm_dist (a bounding sphere, its centre snapped to the map's texels: no crawling edges while the camera moves or
 * turns), the frame's R2 segments drawn again as casters (r2_render_segment, in.shadow). Fills light.hlsl's r_lm* / r_smp*
 * (lm: 16 floats, smp: 12). 0: no map this frame. */
static int reim_smap(const reim_scene* S, const float* rndc, const float* rzab, int pw, int ph, float* lm, float* smp) {
    int n = RE.sm_size;
    if (!S->nops || S->fd != X.fd || !X.fd) return 0;
    if (!RR.sm || RR.sm_n != n) {
        GPU_RELEASE(RR.sm_dsv); GPU_RELEASE(RR.sm_srv); GPU_RELEASE(RR.sm);
        RR.sm_n = 0;
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = td.Height = (UINT)n; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32_TYPELESS;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &RR.sm))) { RR.sm = NULL; return 0; }
        D3D11_DEPTH_STENCIL_VIEW_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.Format = DXGI_FORMAT_D32_FLOAT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_R32_FLOAT; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        if (FAILED(ID3D11Device_CreateDepthStencilView(gpu_rt.dev, (ID3D11Resource*)RR.sm, &dd, &RR.sm_dsv)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)RR.sm, &sd, &RR.sm_srv))) {
            GPU_RELEASE(RR.sm_dsv); GPU_RELEASE(RR.sm_srv); GPU_RELEASE(RR.sm);
            return 0;
        }
        RR.sm_n = n;
    }
    /* the frustum slice [near, D] in the world: its corners through the inverse world x clip */
    const float* ip = S->ipv;
    float D = RE.sm_dist, nx[2] = { rndc[1], rndc[0] * (float)pw + rndc[1] }, ny[2] = { rndc[3], rndc[2] * (float)ph + rndc[3] };
    float ws[2] = { 50.0f, D }, cor[8][3], cen[3] = { 0, 0, 0 };
    for (int k = 0; k < 8; k++) {
        float w = ws[k >> 2], c[4] = { nx[k & 1] * w, ny[(k >> 1) & 1] * w, rzab[0] * w + rzab[1], w }, h[4];
        for (int j = 0; j < 4; j++) h[j] = c[0] * ip[j] + c[1] * ip[4 + j] + c[2] * ip[8 + j] + c[3] * ip[12 + j];
        if (!(fabsf(h[3]) > 1e-20f)) return 0;
        for (int j = 0; j < 3; j++) { cor[k][j] = h[j] / h[3]; cen[j] += cor[k][j] * 0.125f; }
    }
    float R = 0.0f;
    for (int k = 0; k < 8; k++) {
        float d2 = 0.0f;
        for (int j = 0; j < 3; j++) d2 += (cor[k][j] - cen[j]) * (cor[k][j] - cen[j]);
        if (d2 > R) R = d2;
    }
    R = ceilf(sqrtf(R) / 64.0f) * 64.0f;
    if (!(R > 1.0f && R < 1e6f)) return 0;
    const float* Lz = RE.key;
    float Lx[3], Ly[3];
    if (fabsf(Lz[2]) < 0.99f) { Lx[0] = -Lz[1]; Lx[1] = Lz[0]; Lx[2] = 0.0f; }   /* (up x Lz) */
    else { Lx[0] = 0.0f; Lx[1] = Lz[2]; Lx[2] = -Lz[1]; }
    float l = sqrtf(Lx[0] * Lx[0] + Lx[1] * Lx[1] + Lx[2] * Lx[2]);
    for (int j = 0; j < 3; j++) Lx[j] /= l;
    Ly[0] = Lz[1] * Lx[2] - Lz[2] * Lx[1]; Ly[1] = Lz[2] * Lx[0] - Lz[0] * Lx[2]; Ly[2] = Lz[0] * Lx[1] - Lz[1] * Lx[0];
    float texel = 2.0f * R / (float)n;
    float cx = floorf((cen[0] * Lx[0] + cen[1] * Lx[1] + cen[2] * Lx[2]) / texel) * texel;
    float cy = floorf((cen[0] * Ly[0] + cen[1] * Ly[1] + cen[2] * Ly[2]) / texel) * texel;
    float cz = cen[0] * Lz[0] + cen[1] * Lz[1] + cen[2] * Lz[2];
    float E = 4000.0f, z0 = cz - R, zr = 2.0f * R + E;   /* (casters up to E above the sphere, towards the light) */
    for (int j = 0; j < 3; j++) { lm[j * 4] = Lx[j] / R; lm[j * 4 + 1] = Ly[j] / R; lm[j * 4 + 2] = Lz[j] / zr; lm[j * 4 + 3] = 0.0f; }
    lm[12] = -cx / R; lm[13] = -cy / R; lm[14] = -z0 / zr; lm[15] = 1.0f;
    smp[0] = smp[1] = 1.0f / (float)n; smp[2] = texel; smp[3] = D * 0.7f;
    smp[4] = D; smp[5] = RE.sm_k; smp[6] = Lz[0]; smp[7] = Lz[1]; smp[8] = Lz[2];
    /* the casters */
    float m32[32];
    memcpy(m32, ip, 64);
    memcpy(m32 + 16, lm, 64);
    prof_mark(PR_REIM_SM);
    unbind_all();
    ID3D11DeviceContext_ClearDepthStencilView(gpu_rt.ctx, RR.sm_dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
    const gpu_front_frame_data* fd = S->fd;
    for (int i = 0; i < S->nops; i++) {
        const gpu_fop_r2* o = (const gpu_fop_r2*)(fd->cmd + S->ops[i]);
        r2_exec_in in = { gpu_rt.dev, gpu_rt.ctx, NULL, RR.sm_dsv, 1, 1, X.vram_srv, X.swz_srv, X.dat.srv, X.iota,
                          gpu_rt.caps.warp ? 0.49609375f : 0.42f, td_r2, NULL, NULL };
        in.texdec_srv = TD.srv;
        in.mft = 1; in.msaa = M.n; in.sx = M.rsx; in.sy = M.rsy;
        in.shadow = 1; in.shadow_size = n; in.shadow_m = m32;
        RR.sm_draws += (uint64_t)r2_render_segment(o, fd->data, fd->data_bytes, &in);
    }
    unbind_all();
    bind_frame_buffers();
    ID3D11Buffer* b = M.cb;
    ID3D11DeviceContext_VSSetConstantBuffers(gpu_rt.ctx, 6, 1, &b);
    ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 6, 1, &b);
    X.mode = 0;
    M.bound = 0;
    RR.sm_frames++;
    prof_mark(PR_LIGHT);
    if (RE.log && RR.sm_frames % 600 == 1)
        gpu_log("reim: shadow map %d: sphere R %.0f at %.0f %.0f %.0f, %d segments, %.1f casters a frame", n, R, cen[0], cen[1], cen[2],
                S->nops, (double)RR.sm_draws / (double)RR.sm_frames);
    return 1;
}

/* the Reimagined post pass (light_post's place; the picture's 3D is complete, the HUD not drawn yet) */
static void reim_post(mtarget* c, mtarget* z, int pc) {
    re_env();
    int pw = c->pw, ph = c->ph, hw = (pw + 1) / 2, hh = (ph + 1) / 2;
    float view[4], zz[2];
    int proj = light_proj(view, zz);
    if (!proj) LT.noproj++;
    const reim_scene* S = RS.serial == LT.serial ? &RS : &RS_last;
    int bw[7], bh[7];
    bw[1] = hw; bh[1] = hh;
    for (int i = 2; i <= 6; i++) { bw[i] = (bw[i - 1] + 1) / 2; bh[i] = (bh[i - 1] + 1) / 2; }
    if (!lt_tex(LT_W, hw, hh, DXGI_FORMAT_R32_FLOAT) || !lt_tex(LT_AO0, hw, hh, DXGI_FORMAT_R8_UNORM) ||
        !lt_tex(LT_AO1, hw, hh, DXGI_FORMAT_R8_UNORM) || !rr_tex(RT_SH, hw, hh, DXGI_FORMAT_R8G8_UNORM, 0, 1) || !rr_tex(RT_SH2, hw, hh, DXGI_FORMAT_R8G8_UNORM, 0, 1) ||
        !rr_tex(RT_B6, bw[6], bh[6], DXGI_FORMAT_R11G11B10_FLOAT, 0, 1) ||
        !rr_tex(RT_SHAFT, bw[2], bh[2], DXGI_FORMAT_R11G11B10_FLOAT, 0, 1) ||
        !rr_tex(RT_PIC, pw, ph, DXGI_FORMAT_R8G8B8A8_TYPELESS, 0, 0) ||
        !rr_tex(RT_ST0, 4, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 1) || !rr_tex(RT_ST1, 4, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 1) ||
        !rr_tex(RT_SCENE, hw, hh, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1))
        { prof_mark(pc); return; }
    for (int i = 1; i <= 5; i++)
        if (!lt_tex(LT_B1 + i - 1, bw[i], bh[i], DXGI_FORMAT_R11G11B10_FLOAT)) { prof_mark(pc); return; }
    if (!RR.cb) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 32 * 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &RR.cb))) { prof_mark(pc); return; }
    }
    /* light_cb (light.hlsl) with the Reimagined occlusion and bloom */
    float cb[36];
    memset(cb, 0, sizeof cb);
    float* dst = cb; float* src = cb + 4; float* map = cb + 8; float* lv = cb + 12; float* lz = cb + 16;
    float* lao = cb + 20; float* lao2 = cb + 24; float* lbl = cb + 28; float* ldir = cb + 32;
    map[0] = M.rsx / M.osx; map[1] = M.rsy / M.osy; map[2] = (float)pw / (float)hw; map[3] = (float)ph / (float)hh;
    if (proj) { memcpy(lv, view, 16); lz[0] = zz[0]; lz[1] = zz[1]; }
    lz[2] = 1e6f; lz[3] = 0.0f;
    int ao = LT.ao && proj;
    lao[0] = RE.ao_r; lao[1] = ao ? RE.ao_k : 0.0f; lao[2] = LT.ao_max; lao[3] = LT.ao_bias;
    lao2[0] = LT.ao_f0; lao2[1] = LT.ao_f1; lao2[2] = RE.ao_min; lao2[3] = LT.ao_pow;
    lbl[0] = RE.bl_t; lbl[1] = RE.bl_k; lbl[2] = RE.bl_i; lbl[3] = 1.0f;
    ldir[2] = 1.0f / (float)pw; ldir[3] = 1.0f / (float)ph;
    /* reim_cb */
    float rc[128];
    memset(rc, 0, sizeof rc);
    float *rw = rc, *rndc = rc + 16, *rzab = rc + 20, *rkey = rc + 24, *rsss = rc + 28, *rgr = rc + 32, *rfog = rc + 36,
          *rhf = rc + 40, *rvig = rc + 44, *rsh = rc + 48, *rbl = rc + 52, *rmisc = rc + 56, *rkc = rc + 60, *rlm = rc + 64, *rsmp = rc + 80;
    int have_world = proj && S->pv_ok;
    if (have_world) {
        const float* pv = S->pv;
        memcpy(rw, S->ipv, 64);
        const gpu_r2_frame* f = &LT.fb;
        float xo = (float)(LT.xyo & 0xFFFFu), yo = (float)(LT.xyo >> 16);
        rndc[0] = 16.0f / (M.osx * f->ax); rndc[1] = (xo - 8.0f - f->bx) / f->ax;
        rndc[2] = 16.0f / (M.osy * f->ay); rndc[3] = (yo - 8.0f - f->by) / f->ay;
        float c3 = pv[3] * pv[3] + pv[7] * pv[7] + pv[11] * pv[11];
        float a = (pv[2] * pv[3] + pv[6] * pv[7] + pv[10] * pv[11]) / c3;
        rzab[0] = a; rzab[1] = pv[14] - a * pv[15];
        rzab[2] = S->ipv[11] != 0.0f ? S->ipv[10] / S->ipv[11] : 0.0f;   /* the camera's height (clip x = y = w = 0) */
        /* the key light -> view space (light.hlsl l_pos: x = (clip x - ox clip w) / fx, y the same, z = clip w) */
        reim_key(S);
        float dc[4];
        for (int j = 0; j < 4; j++) dc[j] = RE.key[0] * pv[j] + RE.key[1] * pv[4 + j] + RE.key[2] * pv[8 + j];
        float o2[2], f2[2];
        for (int j = 0; j < 2; j++) {
            o2[j] = (pv[j] * pv[3] + pv[4 + j] * pv[7] + pv[8 + j] * pv[11]) / c3;
            float q = 0.0f;
            for (int k = 0; k < 3; k++) { float v = pv[k * 4 + j] - o2[j] * pv[k * 4 + 3]; q += v * v; }
            f2[j] = sqrtf(q / c3);
        }
        float kv[3] = { (dc[0] - o2[0] * dc[3]) / f2[0], (dc[1] - o2[1] * dc[3]) / f2[1], dc[3] };
        float l = sqrtf(kv[0] * kv[0] + kv[1] * kv[1] + kv[2] * kv[2]);
        if (l > 1e-6f) { rkey[0] = kv[0] / l; rkey[1] = kv[1] / l; rkey[2] = kv[2] / l; rkey[3] = RE.ss ? RE.sss_k : 0.0f; }
        memcpy(RE.kv, rkey, sizeof RE.kv);
        memcpy(rkc, RE.keyc, sizeof RE.keyc);
        rkc[3] = RE.key_i;
        if (RE.smap && rkey[3] > 0.0f && S == &RS && !reim_smap(S, rndc, rzab, pw, ph, rlm, rsmp)) memset(rsmp, 0, 12 * sizeof(float));
    }
    rsss[0] = RE.sss_len; rsss[1] = RE.sss_th; rsss[2] = 2.0f; rsss[3] = 5000.0f;
    rgr[0] = RE.grade ? RE.sat : 1.0f; rgr[1] = RE.grade ? RE.con : 1.0f; rgr[2] = RE.grade ? RE.vib : 0.0f; rgr[3] = RE.grade ? RE.expo : 1.0f;
    rfog[0] = RE.fog0; rfog[1] = RE.fogd; rfog[2] = RE.fog && have_world ? RE.fogmax : 0.0f; rfog[3] = RE.hfog;
    rhf[0] = RE.hfog_h; rhf[1] = 250.0f;
    rvig[0] = RE.vig; rvig[1] = 0.55f; rvig[2] = 1.15f; rvig[3] = RE.grade ? RE.clar : 0.0f;
    rsh[0] = RE.shafts ? RE.sh_i : 0.0f; rsh[1] = 0.965f; rsh[2] = 0.85f; rsh[3] = 6000.0f;
    rbl[0] = 1.0f; rbl[1] = RE.halo;
    rmisc[0] = RE.stat_a; rmisc[1] = RE.grade ? RE.night : 0.0f;
    rmisc[2] = (float)(RE.dbg == 9 ? (int)((RR.frames / 30u) % 5u) : RE.dbg);   /* (9: the debug views in turn, 30 frames each) */
    rmisc[3] = (float)(RR.frames & 0xFFFF);
    ID3D11DeviceContext_UpdateSubresource(gpu_rt.ctx, (ID3D11Resource*)RR.cb, 0, NULL, rc, 0, 0);
#define LT_SIZE(v, w, h) do { (v)[0] = (float)(w); (v)[1] = (float)(h); (v)[2] = 1.0f / (float)(w); (v)[3] = 1.0f / (float)(h); } while (0)
    if (RE.log && LT.frames % 600 == 0)
        gpu_log("reim: v%u serial %u world %d key %.3f %.3f %.3f (view %.3f %.3f %.3f) col %.3f %.3f %.3f clusters %d cam z %.1f",
                X.fd ? X.fd->vblank : 0u, LT.serial, have_world, RE.key[0], RE.key[1], RE.key[2], rkey[0], rkey[1], rkey[2],
                RE.keyc[0], RE.keyc[1], RE.keyc[2], S->ncl, rzab[2]);
    LT.frames++;
    RR.frames++;
    unbind_all();
    set_vs(gpu_vs(GPU_SH_vs_light));
    set_topo(0);
    set_depth(X.ds_off);
    ID3D11SamplerState* smp = mft_sampler();
    ID3D11DeviceContext_PSSetSamplers(gpu_rt.ctx, 0, 1, &smp);
    rr_unbind();
    ID3D11BlendState* off = blend_state(BL_OFF, 15);
    int ms = z->n > 1 && !z->alias;
    ID3D11Buffer* bufs[2] = { LT.cb, RR.cb };
    /* depth, occlusion */
    LT_SIZE(dst, hw, hh); LT_SIZE(src, hw, hh);
    lt_cb(cb, 36);
    bufs[0] = LT.cb;
    ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 7, 2, bufs);
    lt_srv(17, z->srv);
    lt_pass(LT.rtv[LT_W], hw, hh, ms ? GPU_SH_ps_light_lin1 : GPU_SH_ps_light_lin0, off);
    rr_unbind();
    if (ao) {
        lt_srv(14, LT.srv[LT_W]);
        lt_pass(LT.rtv[LT_AO0], hw, hh, GPU_SH_ps_light_ao, off);
        rr_unbind();
        for (int k = 0; k < 2; k++) {
            ldir[0] = k ? 0.0f : 1.0f; ldir[1] = k ? 1.0f : 0.0f;
            lt_cb(cb, 36);
            lt_srv(14, LT.srv[LT_W]); lt_srv(15, LT.srv[k ? LT_AO1 : LT_AO0]);
            lt_pass(LT.rtv[k ? LT_AO0 : LT_AO1], hw, hh, GPU_SH_ps_light_blur, off);
            rr_unbind();
        }
        ldir[0] = ldir[1] = 0.0f;
        lt_cb(cb, 36);
    }
    /* screen-space shadow along the key light */
    if (rkey[3] > 0.0f) {
        lt_srv(14, LT.srv[LT_W]);
        if (RR.sm_srv && rsmp[5] > 0.0f) lt_srv(24, RR.sm_srv);
        lt_pass(RR.rtv[RT_SH], hw, hh, GPU_SH_ps_reim_shadow, off);
        rr_unbind();
        for (int k = 0; k < 2; k++) {
            ldir[0] = k ? 0.0f : 1.0f; ldir[1] = k ? 1.0f : 0.0f;
            lt_cb(cb, 36);
            lt_srv(14, LT.srv[LT_W]); lt_srv(25, k ? RR.srv[RT_SH2] : RR.srv[RT_SH]);
            lt_pass(k ? RR.rtv[RT_SH] : RR.rtv[RT_SH2], hw, hh, GPU_SH_ps_reim_blur2, off);
            rr_unbind();
        }
        ldir[0] = ldir[1] = 0.0f;
    } else {
        static const float one[4] = { 1, 0, 0, 0 };   /* (visible, N.L 0: nothing changes) */
        ID3D11DeviceContext_ClearRenderTargetView(gpu_rt.ctx, RR.rtv[RT_SH], one);
    }
    /* bloom: the bright pass + the scene colour (MRT), the pyramid (6 levels), its upsample with the halo weight */
    LT_SIZE(dst, hw, hh); LT_SIZE(src, pw, ph);
    lt_cb(cb, 36);
    lt_srv(16, c->psrv); lt_srv(18, RR.srv[RR.st_cur ? RT_ST1 : RT_ST0]);   /* (last frame's statistics: the threshold) */
    {
        ID3D11RenderTargetView* r2[2] = { LT.rtv[LT_B1], RR.rtv[RT_SCENE] };
        ID3D11DeviceContext_OMSetRenderTargets(gpu_rt.ctx, 2, r2, NULL);
        set_viewport(hw, hh);
        set_scissor(0, 0, hw, hh);
        set_blend(off);
        set_ps(gpu_ps(GPU_SH_ps_reim_bright));
        ID3D11DeviceContext_Draw(gpu_rt.ctx, 3, 0);
        gpu_rt.n_draws++;
    }
    rr_unbind();
    ID3D11DeviceContext_GenerateMips(gpu_rt.ctx, RR.srv[RT_SCENE]);
    ID3D11RenderTargetView* brtv[7] = { NULL, LT.rtv[LT_B1], LT.rtv[LT_B2], LT.rtv[LT_B3], LT.rtv[LT_B4], LT.rtv[LT_B5], RR.rtv[RT_B6] };
    ID3D11ShaderResourceView* bsrv[7] = { NULL, LT.srv[LT_B1], LT.srv[LT_B2], LT.srv[LT_B3], LT.srv[LT_B4], LT.srv[LT_B5], RR.srv[RT_B6] };
    for (int i = 2; i <= 6; i++) {
        LT_SIZE(dst, bw[i], bh[i]); LT_SIZE(src, bw[i - 1], bh[i - 1]);
        lt_cb(cb, 36);
        lt_srv(16, bsrv[i - 1]);
        lt_pass(brtv[i], bw[i], bh[i], GPU_SH_ps_light_down, off);
        rr_unbind();
    }
    ID3D11BlendState* add = blend_state(BL_ADD, 15);
    for (int i = 5; i >= 1; i--) {
        LT_SIZE(dst, bw[i], bh[i]); LT_SIZE(src, bw[i + 1], bh[i + 1]);
        lt_cb(cb, 36);
        lt_srv(16, bsrv[i + 1]);
        lt_pass(brtv[i], bw[i], bh[i], GPU_SH_ps_reim_up, add);
        rr_unbind();
    }
    /* the scene's statistics (smoothed), then the shafts towards the source */
    int sc = RR.st_cur, sn = sc ^ 1;
    LT_SIZE(dst, 4, 1); LT_SIZE(src, hw, hh);
    lt_cb(cb, 36);
    lt_srv(14, LT.srv[LT_W]); lt_srv(16, bsrv[4]); lt_srv(22, RR.srv[RT_SCENE]); lt_srv(23, RR.srv[sc ? RT_ST1 : RT_ST0]); lt_srv(15, LT.srv[LT_AO0]); lt_srv(20, RR.srv[RT_SH]);   /* (the light left on average: [3]) */
    lt_pass(RR.rtv[sn ? RT_ST1 : RT_ST0], 4, 1, GPU_SH_ps_reim_stats, off);
    rr_unbind();
    RR.st_cur = sn;
    ID3D11ShaderResourceView* stv = RR.srv[sn ? RT_ST1 : RT_ST0];
    if (rsh[0] > 0.0f) {
        LT_SIZE(dst, bw[2], bh[2]); LT_SIZE(src, hw, hh);
        lt_cb(cb, 36);
        lt_srv(14, LT.srv[LT_W]); lt_srv(16, bsrv[1]); lt_srv(18, stv);
        lt_pass(RR.rtv[RT_SHAFT], bw[2], bh[2], GPU_SH_ps_reim_shafts, off);
        rr_unbind();
    }
    /* composite + grade: from a copy of the picture onto it (RGB) */
    ID3D11DeviceContext_CopyResource(gpu_rt.ctx, (ID3D11Resource*)RR.t[RT_PIC], (ID3D11Resource*)c->pic);
    LT_SIZE(dst, pw, ph); LT_SIZE(src, hw, hh);
    lt_cb(cb, 36);
    lt_srv(14, LT.srv[LT_W]); lt_srv(15, LT.srv[LT_AO0]); lt_srv(16, LT.srv[LT_B1]); lt_srv(17, z->srv); lt_srv(18, stv);
    lt_srv(19, RR.srv[RT_PIC]); lt_srv(20, RR.srv[RT_SH]); lt_srv(21, RR.srv[RT_SHAFT]); lt_srv(22, RR.srv[RT_SCENE]);
    {
        static const float zero[4] = { 0, 0, 0, 0 };
        if (rsh[0] <= 0.0f) ID3D11DeviceContext_ClearRenderTargetView(gpu_rt.ctx, RR.rtv[RT_SHAFT], zero);
    }
    if (LT.split) {                      /* (PS2_LIGHT_SPLIT=1: the left half stays as it was) */
        ID3D11DeviceContext_OMSetRenderTargets(gpu_rt.ctx, 1, &c->prtv, NULL);
        set_viewport(pw, ph);
        set_scissor(pw / 2, 0, pw, ph);
        set_blend(blend_state(BL_OFF, 7));
        set_ps(gpu_ps(ms ? GPU_SH_ps_reim_comp1 : GPU_SH_ps_reim_comp0));
        ID3D11DeviceContext_Draw(gpu_rt.ctx, 3, 0);
    } else
    lt_pass(c->prtv, pw, ph, ms ? GPU_SH_ps_reim_comp1 : GPU_SH_ps_reim_comp0, blend_state(BL_OFF, 7));
    rr_unbind();
#undef LT_SIZE
    X.mode = 0;
    X.b_ct = X.b_zt = -1;
    M.bound = 0;
    M.cb_phase = -1;
    ID3D11Buffer* mb = M.cb;
    ID3D11DeviceContext_VSSetConstantBuffers(gpu_rt.ctx, 6, 1, &mb);
    ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 6, 1, &mb);
    prof_mark(pc);
}