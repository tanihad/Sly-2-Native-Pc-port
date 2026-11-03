/* (r2-m8, R8) Modern frame targets ("MFT"): a modern gameplay frame drawn at any resolution with MSAA instead of the exact
 * path's integer sample grid (notes/modern-renderer.md "R8 as built"; shaders/mft.hlsl). Included once by gpu_exec.c
 * (it uses the executor's state and helpers); the frontend is unchanged - the render thread decides per frame.
 *
 *   decision   mft_plan walks the frame's ops before it runs: a modern frame (R2 segments, mixed), MFT on in the settings,
 *              the PRESENT showing one display-family target 1:1 (gpu_present_mft_layout), and every op touching a
 *              display-family target one of: CLEAR, R2 segment, FF triangle / sprite / line-pixel / thin-line draw with
 *              GS-memory textures, the shadow pipeline as the fused sequence (gpu_exec.c shfuse: head, volumes, tail), a
 *              post pass (NPOST) after the resolve point. Anything else (memory ops on those targets, target views, ROV
 *              draws, ALIAS16 runs that do not fuse): the frame runs on the exact grid as before.
 *   surfaces   per display-family target a twin (mtarget): the MS surface at the render resolution (colour RGBA8 holding
 *              the GS bytes, or D32 for Z) and the picture at the output resolution (single sample). GS pixel x covers
 *              d3d [x s, (x + 1) s) of either (s: pixels per GS pixel, render or output).
 *   phases     A (the 3D) on the MS surfaces; at the resolve point - the 2D class's mark (R2H_MARK) moved back over the
 *              class switch's clears, or the first post pass, or the PRESENT - the frame's picture is resolved
 *              (ResolveSubresource, at a render scale below 100% then upscaled: Catmull-Rom); phase B (the HUD, the
 *              motion blur) draws on the pictures at the output resolution. The PRESENT shows the picture 1:1.
 *   truth      each twin knows where its target's newest content is (grid / MS / picture) and converts on demand
 *              (mft_need): grid -> MFT when a modern frame starts after exact ones, MFT -> grid before an exact frame
 *              (or a read-back) uses the target. That makes switching seamless in both directions. */

enum { MT_GRID, MT_MS, MT_PIC };
typedef struct {
    int w, h, n;                         /* MS surface (render resolution; n samples) */
    float sx, sy;                        /* its pixels per GS pixel */
    ID3D11Texture2D* ms;
    ID3D11RenderTargetView* rtv;         /* colour: RGBA8 UNORM (the raw GS bytes) */
    ID3D11ShaderResourceView* srv;       /* colour: UINT (svol / shfuse); depth: R32_FLOAT */
    ID3D11DepthStencilView* dsv;
    int pw, ph;                          /* picture (output resolution, one sample) */
    float psx, psy;
    ID3D11Texture2D* pic;
    ID3D11RenderTargetView* prtv;
    ID3D11ShaderResourceView* psrv;      /* colour UNORM float (copies, upscale); depth R32_FLOAT */
    ID3D11UnorderedAccessView* puav;     /* colour R32_UINT (cs_npost) */
    ID3D11ShaderResourceView* psrv_u;    /* colour UINT (DATE marks after the resolve) */
    ID3D11DepthStencilView* pdsv;
    int alias;                           /* the MS surface is the picture (one sample at the output size) */
    int truth;                           /* MT_* */
    int depth;
} mtarget;

static mtarget MT[MAX_TARGETS + 1];

/* the configuration: gfx.c (settings) or the environment (PS2_MFT=0|1, PS2_MFT_SCALE=<%>, PS2_MFT_MSAA=1|2|4|8) */
static volatile LONG mcf_on = -1, mcf_pct = 100, mcf_msaa = 4;
static int mcf_env_on = -1, mcf_env_pct, mcf_env_msaa;   /* (PS2_MFT, PS2_MFT_SCALE, PS2_MFT_MSAA: they win over the settings) */
void gpu_mft_config(int on, int pct, int msaa) {
    InterlockedExchange(&mcf_pct, pct < 25 ? 25 : pct > 400 ? 400 : pct);
    InterlockedExchange(&mcf_msaa, msaa >= 8 ? 8 : msaa >= 4 ? 4 : msaa >= 2 ? 2 : 1);
    InterlockedExchange(&mcf_on, on ? 1 : 0);
}

enum { MW_OFF, MW_NOT_MODERN, MW_LAYOUT, MW_MEM, MW_TIER, MW_CLASS, MW_VIEW, MW_TARGET, MW_A16, MW_R2CLIP, MW_PHASE, MW_NPOST,
       MW_SIZE, MW_ALLOC, MW_N };
static const char* const mw_names[MW_N] = { "off", "not modern", "display layout", "memory op", "ROV draw", "primitive class",
                                            "target view", "other target", "ALIAS16", "R2 clip", "after resolve", "post pass",
                                            "size", "allocation" };

typedef struct {
    size_t head, vol0, tail, next;       /* the head's ALIAS16, the first op after its clear, the tail's ALIAS16, after the tail */
    uint32_t head_slots;
    shf_plan L;                          /* the tail (+ copy 0 and the clear: L.k.c[0], L.k.clr) */
    int nflat;                           /* flat passes between the volumes (each ends an epoch) */
    uint32_t flat[3][4];                 /* their mode, alpha, fmask, vertex colour */
    size_t flat_pos[3];
} mft_shp;

static struct {
    int env;                             /* environment overrides read */
    int on;                              /* this frame runs on the MFT surfaces */
    int phase;                           /* 0: A (MS, render resolution), 1: B (pictures, output resolution) */
    size_t ph_pos[16];                   /* the phase changes: at op ph_pos[i] the phase becomes ph_val[i] */
    int ph_val[16], nph, iph;
    int n;                               /* samples */
    float rsx, rsy, osx, osy;            /* render / output pixels per GS pixel */
    int gw, gh;                          /* the surfaces' GS size (the display-family targets' largest) */
    int ow, oh;                          /* the picture's output size */
    int ct_b, zt_b, bound;               /* bound surfaces (phase-dependent) */
    int cb_phase;                        /* mft_cb holds this phase's scale (-1 none) */
    ID3D11Buffer* cb;
    float cbv[24];
    ID3D11SamplerState* lin;
    ID3D11Texture2D* spare;              /* the shadow pass's other surface (colour, MS) */
    ID3D11RenderTargetView* spare_rtv;
    ID3D11ShaderResourceView* spare_srv;
    int spare_w, spare_h, spare_n;
    ID3D11Texture2D* cnt;                /* shadow-volume counters: (x * n + sample, y), R32_UINT */
    ID3D11UnorderedAccessView* cnt_uav;
    ID3D11ShaderResourceView* cnt_srv;
    int cnt_w, cnt_h;
    ID3D11Buffer* scb;                   /* mshf_cb (the fused shadow pass) */
    ID3D11Texture2D* sds[2];             /* DATE without a depth target: a stencil surface per phase (MS / picture size) */
    ID3D11DepthStencilView* sds_dsv[2];
    int sds_w[2], sds_h[2], sds_n[2];
    ID3D11DepthStencilState* ds_date[8][2];   /* depth class x DATM: + the stencil test (bit 0 EQUAL DATM) */
    ID3D11DepthStencilState* ds_mark;    /* the alpha-bit-to-stencil pass */
    ID3D11Texture2D* tmp;                /* resolve target at the render resolution (render scale < 100%) */
    ID3D11ShaderResourceView* tmp_srv;
    int tmp_w, tmp_h;
    size_t shp_at;                       /* the planned shadow sequences (op positions) */
    mft_shp shp[4];
    int nshp;
    int sv_bb[4];                        /* the volumes' samples drawn this sequence (surface pixels) */
    int shp_fail;                        /* (diagnostics) why the last shadow sequence did not plan: 1 head, 2 tail, 3 A */
    size_t shp_failpos;                  /* (diagnostics) where the tail was expected */
    int mv_on, mv_view;                  /* (r2-m8) motion vectors (PS2_MFT_MV=1), their picture (PS2_MFT_MV_VIEW=1) */
    ID3D11Texture2D *mv, *mvr;           /* MS surface, resolved */
    ID3D11RenderTargetView* mv_rtv;
    ID3D11ShaderResourceView* mvr_srv;
    int mv_w, mv_h, mv_n;
    size_t r2op[256];                    /* this frame's R2 segments drawn in phase A (op positions) */
    int nr2;
    uint64_t mv_frames;
    /* statistics (PS2_R2_STATS) */
    uint64_t frames, grid, why[MW_N], to_grid, from_grid, resolves, draws, svols, n_dl, n_load, n_xcopy, n_mat;
    int why_last;
    /* (mft-pop) pops: runs of 1 .. MFT_POP_MAX grid frames between modern frames - a gameplay frame on the grid, a
     * visible resolution / AA pop; menus and loading screens are longer runs. Frames MFT is off for (settings, A/B
     * phases) end a run without counting it. */
    uint32_t grid_run, grid_v0;
    int grid_why0, grid_off;
    uint64_t pops, pop_frames, pop_why[MW_N];
} M = { .cb_phase = -1 };
#define MFT_POP_MAX 8

static int mft_px(int native, float s) { return (int)ceilf((float)native * s - 0.5f); }   /* first d3d pixel of native x */

static void mft_surf_free(mtarget* m) {
    if (m->alias) { m->ms = NULL; m->rtv = NULL; m->dsv = NULL; if (m->srv == m->psrv) m->srv = NULL; }   /* (the picture's) */
    GPU_RELEASE(m->rtv); GPU_RELEASE(m->srv); GPU_RELEASE(m->dsv); GPU_RELEASE(m->ms);
    GPU_RELEASE(m->prtv); GPU_RELEASE(m->psrv); GPU_RELEASE(m->puav); GPU_RELEASE(m->pdsv); GPU_RELEASE(m->psrv_u); GPU_RELEASE(m->pic);
    memset(m, 0, sizeof *m);
}

static void mft_target_free(int id) { if (id > 0 && id <= MAX_TARGETS) mft_surf_free(&MT[id]); }

static void mft_release(void) {
    for (int i = 0; i <= MAX_TARGETS; i++) mft_surf_free(&MT[i]);
    GPU_RELEASE(M.cb); GPU_RELEASE(M.lin);
    GPU_RELEASE(M.spare_rtv); GPU_RELEASE(M.spare_srv); GPU_RELEASE(M.spare);
    GPU_RELEASE(M.cnt_srv); GPU_RELEASE(M.cnt_uav); GPU_RELEASE(M.cnt); GPU_RELEASE(M.scb);
    GPU_RELEASE(M.tmp_srv); GPU_RELEASE(M.tmp);
    for (int i = 0; i < 2; i++) { GPU_RELEASE(M.sds_dsv[i]); GPU_RELEASE(M.sds[i]); M.sds_w[i] = M.sds_h[i] = M.sds_n[i] = 0; }
    for (int i = 0; i < 8; i++) { GPU_RELEASE(M.ds_date[i][0]); GPU_RELEASE(M.ds_date[i][1]); }
    GPU_RELEASE(M.ds_mark);
    GPU_RELEASE(M.mv_rtv); GPU_RELEASE(M.mv); GPU_RELEASE(M.mvr_srv); GPU_RELEASE(M.mvr);
    M.mv_w = M.mv_h = M.mv_n = 0;
    M.spare_w = M.spare_h = M.spare_n = M.cnt_w = M.cnt_h = M.tmp_w = M.tmp_h = 0;
    M.cb_phase = -1;
    M.on = 0;
}

static ID3D11Texture2D* mtex(int w, int h, int n, DXGI_FORMAT f, UINT bind) {
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1; td.Format = f;
    td.SampleDesc.Count = (UINT)n; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind;
    ID3D11Texture2D* t = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &t))) return NULL;
    return t;
}
static ID3D11RenderTargetView* mrtv(ID3D11Texture2D* t, int ms) {
    D3D11_RENDER_TARGET_VIEW_DESC d;
    memset(&d, 0, sizeof d);
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.ViewDimension = ms ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* v = NULL;
    if (FAILED(ID3D11Device_CreateRenderTargetView(gpu_rt.dev, (ID3D11Resource*)t, &d, &v))) return NULL;
    return v;
}
static ID3D11ShaderResourceView* msrv(ID3D11Texture2D* t, int ms, DXGI_FORMAT f) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d;
    memset(&d, 0, sizeof d);
    d.Format = f;
    d.ViewDimension = ms ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    if (!ms) d.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* v = NULL;
    if (FAILED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)t, &d, &v))) return NULL;
    return v;
}
static ID3D11DepthStencilView* mdsv(ID3D11Texture2D* t, int ms) {
    D3D11_DEPTH_STENCIL_VIEW_DESC d;
    memset(&d, 0, sizeof d);
    d.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;   /* (the stencil: DATE, mft_rov) */
    d.ViewDimension = ms ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    ID3D11DepthStencilView* v = NULL;
    if (FAILED(ID3D11Device_CreateDepthStencilView(gpu_rt.dev, (ID3D11Resource*)t, &d, &v))) return NULL;
    return v;
}

/* the twin of target id at the frame's sizes (recreated when they changed: its content goes back to the grid first) */
static int mft_materialize(int id);
static int mft_surf(int id) {
    xtarget* t = tgt((uint32_t)id);
    if (!t) return 0;
    mtarget* m = &MT[id];
    int w = (int)ceilf((float)M.gw * M.rsx), h = (int)ceilf((float)M.gh * M.rsy);
    int pw = (int)ceilf((float)M.gw * M.osx), ph = (int)ceilf((float)M.gh * M.osy);
    if (m->ms && m->w == w && m->h == h && m->n == M.n && m->pw == pw && m->ph == ph && m->depth == t->depth &&
        m->sx == M.rsx && m->sy == M.rsy && m->psx == M.osx && m->psy == M.osy)
        return 1;
    if (m->ms && m->truth != MT_GRID) mft_materialize(id);
    unbind_all();
    mft_surf_free(m);
    m->depth = t->depth;
    m->w = w; m->h = h; m->n = M.n; m->sx = M.rsx; m->sy = M.rsy;
    m->pw = pw; m->ph = ph; m->psx = M.osx; m->psy = M.osy;
    int ms = M.n > 1;
    m->alias = !ms && w == pw && h == ph;
    if (m->depth) {
        m->pic = mtex(pw, ph, 1, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
        m->pdsv = m->pic ? mdsv(m->pic, 0) : NULL;
        m->psrv = m->pic ? msrv(m->pic, 0, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS) : NULL;
        if (m->alias) { m->ms = m->pic; m->dsv = m->pdsv; m->srv = m->psrv; }
        else {
            m->ms = mtex(w, h, M.n, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
            m->dsv = m->ms ? mdsv(m->ms, ms) : NULL;
            m->srv = m->ms ? msrv(m->ms, ms, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS) : NULL;
        }
        if (!m->pdsv || !m->psrv || !m->dsv || !m->srv) { mft_surf_free(m); return 0; }
    } else {
        m->pic = mtex(pw, ph, 1, DXGI_FORMAT_R8G8B8A8_TYPELESS,
                      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        if (m->pic) {
            m->prtv = mrtv(m->pic, 0);
            m->psrv = msrv(m->pic, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
            m->psrv_u = msrv(m->pic, 0, DXGI_FORMAT_R8G8B8A8_UINT);
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
            memset(&ud, 0, sizeof ud);
            ud.Format = DXGI_FORMAT_R32_UINT;
            ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            if (FAILED(ID3D11Device_CreateUnorderedAccessView(gpu_rt.dev, (ID3D11Resource*)m->pic, &ud, &m->puav))) m->puav = NULL;
        }
        if (m->alias) { m->ms = m->pic; m->rtv = m->prtv; m->srv = m->pic ? msrv(m->pic, 0, DXGI_FORMAT_R8G8B8A8_UINT) : NULL; }
        else {
            m->ms = mtex(w, h, M.n, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
            m->rtv = m->ms ? mrtv(m->ms, ms) : NULL;
            m->srv = m->ms ? msrv(m->ms, ms, DXGI_FORMAT_R8G8B8A8_UINT) : NULL;
        }
        if (!m->prtv || !m->psrv || !m->psrv_u || !m->puav || !m->rtv || !m->srv) { mft_surf_free(m); return 0; }
    }
    m->truth = MT_GRID;                  /* (the grid holds the target's content until something is drawn here) */
    return 1;
}

/* mft_cb (shaders/mft.hlsl): written only when it changes */
static void mft_cb_write(void) {
    if (!M.cb) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof M.cbv;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &M.cb))) return;
    }
    ID3D11DeviceContext_UpdateSubresource(gpu_rt.ctx, (ID3D11Resource*)M.cb, 0, NULL, M.cbv, 0, 0);
    ID3D11Buffer* b = M.cb;
    ID3D11DeviceContext_VSSetConstantBuffers(gpu_rt.ctx, 6, 1, &b);
    ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 6, 1, &b);
    X.state_calls += 3;
}
/* the scale of the phase's surfaces into mft_cb (no sub-draw overrides) */
static void mft_cb_scale(int pic) {
    if (M.cb_phase == pic) return;
    float sx = pic ? M.osx : M.rsx, sy = pic ? M.osy : M.rsy;
    M.cbv[0] = sx; M.cbv[1] = sy; M.cbv[2] = 1.0f / sx; M.cbv[3] = 1.0f / sy;
    uint32_t* u = (uint32_t*)M.cbv;
    u[4] = pic ? 1u : (uint32_t)M.n;
    u[5] = u[6] = u[7] = 0;
    mft_cb_write();
    M.cb_phase = pic;
}

/* a pass over d3d rect r of the bound target with ps (vs_mft_quad); mft_cb's rect / value / source words as given */
static void mft_quad(const float r[4], int ps) {
    memcpy(&M.cbv[8], r, 16);
    mft_cb_write();
    M.cb_phase = -2;                     /* (the scale words may be the other phase's now: written again before draws) */
    set_topo(1);
    set_vs(gpu_vs(GPU_SH_vs_mft_quad));
    set_ps(gpu_ps(ps));
    ID3D11DeviceContext_Draw(gpu_rt.ctx, 4, 0);
    gpu_rt.n_draws++;
}

static ID3D11SamplerState* mft_sampler(void) {
    if (M.lin) return M.lin;
    D3D11_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(gpu_rt.dev, &sd, &M.lin);
    return M.lin;
}

/* bind rtv / dsv of a pass's own (not the executor's cached binding) at the REF viewport */
static void mft_bind_raw(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv) {
    if (X.b_t1) set_t1(NULL);
    if (X.b_t2) set_t2(NULL);
    /* (the views the passes use: t12, t14..t19 - never t13, the decoded texture cache of the draws) */
    ID3D11ShaderResourceView* none[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 12, 1, none);
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 14, 6, none);
    ID3D11DeviceContext_OMSetRenderTargets(gpu_rt.ctx, rtv ? 1 : 0, rtv ? &rtv : NULL, dsv);
    X.mode = 7;
    X.b_ct = X.b_zt = -1;
    M.bound = 0;
    set_viewport(-REF_VP, -REF_VP);
    set_scissor(0, 0, 8192, 8192);
    X.state_calls += 2;
}

/* source mapping words: source position = d3d pixel centre * (ax, ay) + (bx, by); source size w, h */
static void mft_src(float ax, float ay, float bx, float by, int w, int h) {
    M.cbv[16] = ax; M.cbv[17] = ay; M.cbv[18] = bx; M.cbv[19] = by;
    M.cbv[20] = (float)w; M.cbv[21] = (float)h; M.cbv[22] = 1.0f / (float)w; M.cbv[23] = 1.0f / (float)h;
}

static ID3D11DepthStencilState* mft_ds_write(void) { return X.ds_write; }

/* ---- truth conversions ------------------------------------------------------------------------------------------ */

/* the grid target's colour / depth into the twin's MS surface (pic 0) or picture (pic 1); nr: only that native rect
 * [x0, x1) x [y0, y1) (blocks the exact path loaded), NULL: all */
static int mft_from_grid_r(int id, int pic, const int* nr) {
    xtarget* t = tgt((uint32_t)id);
    mtarget* m = &MT[id];
    if (!t || !m->ms) return 0;
    int pc = PR.cur;
    prof_mark(PR_MFT_RES);
    float s_x = pic ? m->psx : m->sx, s_y = pic ? m->psy : m->sy;
    int w = pic ? m->pw : m->w, h = pic ? m->ph : m->h;
    float r[4] = { 0.0f, 0.0f, (float)w, (float)h };
    if (nr) { r[0] = (float)mft_px(nr[0], s_x); r[1] = (float)mft_px(nr[1], s_y); r[2] = (float)mft_px(nr[2], s_x); r[3] = (float)mft_px(nr[3], s_y); }
    /* MFT pixel centre p: GS g = p / s - 0.5; the grid's continuous texel coordinate g k + 0.5 */
    mft_src((float)t->kx / s_x, (float)t->ky / s_y, 0.5f * (1.0f - (float)t->kx), 0.5f * (1.0f - (float)t->ky), t->w, t->h);
    if (m->depth) {
        if (nr) ensure_dsv(t, nr[0], nr[1], nr[2] - 1, nr[3] - 1);
        else ensure_dsv(t, 0, 0, t->w / t->kx - 1, t->h / t->ky - 1);
        unbind_all();
        mft_bind_raw(NULL, pic ? m->pdsv : m->dsv);
        ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 17, 1, &t->srv);
        set_depth(X.ds_write);
        set_blend(blend_state(BL_OFF, 0));
        mft_quad(r, GPU_SH_ps_mft_zfrom);
    } else {
        unbind_all();
        mft_bind_raw(pic ? m->prtv : m->rtv, NULL);
        ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 18, 1, &t->srv);
        ID3D11SamplerState* s = mft_sampler();
        ID3D11DeviceContext_PSSetSamplers(gpu_rt.ctx, 0, 1, &s);
        set_depth(X.ds_off);
        set_blend(blend_state(BL_OFF, 15));
        mft_quad(r, GPU_SH_ps_mft_copy);
    }
    ID3D11ShaderResourceView* none3[3] = { NULL, NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 16, 3, none3);
    prof_mark(pc);
    M.from_grid++;
    return 1;
}
static int mft_from_grid(int id, int pic) { return mft_from_grid_r(id, pic, NULL); }

/* the native rect of target t's blocks: n local block numbers (u16, or gpu_dl_entry.local with stride 16) */
static int mft_blocks_rect(const xtarget* t, const uint8_t* p, uint32_t n, size_t stride, int r[4]) {
    int bw = t->bcols ? t->nat_w / t->bcols : 8;
    r[0] = r[1] = 1 << 20; r[2] = r[3] = -1;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t b;
        memcpy(&b, p + i * stride, 2);
        int bx = (b % t->bcols) * bw, by = (b / t->bcols) * 8;
        if (bx < r[0]) r[0] = bx;
        if (by < r[1]) r[1] = by;
        if (bx + bw > r[2]) r[2] = bx + bw;
        if (by + 8 > r[3]) r[3] = by + 8;
    }
    return t->bcols && r[2] > r[0];
}

static int mft_tmp(int w, int h) {
    if (M.tmp && M.tmp_w == w && M.tmp_h == h) return 1;
    GPU_RELEASE(M.tmp_srv); GPU_RELEASE(M.tmp);
    M.tmp = mtex(w, h, 1, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE);
    M.tmp_srv = M.tmp ? msrv(M.tmp, 0, DXGI_FORMAT_R8G8B8A8_UNORM) : NULL;
    if (!M.tmp_srv) { GPU_RELEASE(M.tmp); M.tmp_w = M.tmp_h = 0; return 0; }
    M.tmp_w = w; M.tmp_h = h;
    return 1;
}

/* MS surface -> picture: the resolve (and the upscale to the output resolution); depth: sample 0, nearest */
static int mft_resolve(int id) {
    mtarget* m = &MT[id];
    if (!m->ms) return 0;
    if (m->alias) return 1;
    int pc = PR.cur;
    prof_mark(PR_MFT_RES);
    float r[4] = { 0.0f, 0.0f, (float)m->pw, (float)m->ph };
    if (m->depth) {
        unbind_all();
        mft_bind_raw(NULL, m->pdsv);
        ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 16, 1, &m->srv);
        mft_src(m->sx / m->psx, m->sy / m->psy, 0.0f, 0.0f, m->w, m->h);
        set_depth(X.ds_write);
        set_blend(blend_state(BL_OFF, 0));
        mft_quad(r, m->n > 1 ? GPU_SH_ps_mft_zto1 : GPU_SH_ps_mft_zto0);
        ID3D11ShaderResourceView* none = NULL;
        ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 16, 1, &none);
    } else if (m->w == m->pw && m->h == m->ph) {
        unbind_all();
        if (m->n > 1) ID3D11DeviceContext_ResolveSubresource(gpu_rt.ctx, (ID3D11Resource*)m->pic, 0, (ID3D11Resource*)m->ms, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        else ID3D11DeviceContext_CopyResource(gpu_rt.ctx, (ID3D11Resource*)m->pic, (ID3D11Resource*)m->ms);
        gpu_rt.n_copies++;
    } else {
        unbind_all();
        if (!mft_tmp(m->w, m->h)) { prof_mark(pc); return 0; }
        if (m->n > 1) ID3D11DeviceContext_ResolveSubresource(gpu_rt.ctx, (ID3D11Resource*)M.tmp, 0, (ID3D11Resource*)m->ms, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        else ID3D11DeviceContext_CopyResource(gpu_rt.ctx, (ID3D11Resource*)M.tmp, (ID3D11Resource*)m->ms);
        mft_bind_raw(m->prtv, NULL);
        ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 18, 1, &M.tmp_srv);
        ID3D11SamplerState* s = mft_sampler();
        ID3D11DeviceContext_PSSetSamplers(gpu_rt.ctx, 0, 1, &s);
        mft_src(m->sx / m->psx, m->sy / m->psy, 0.0f, 0.0f, m->w, m->h);
        set_depth(X.ds_off);
        set_blend(blend_state(BL_OFF, 15));
        mft_quad(r, GPU_SH_ps_mft_upscale);
        ID3D11ShaderResourceView* none = NULL;
        ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 18, 1, &none);
    }
    prof_mark(pc);
    M.resolves++;
    return 1;
}

/* picture -> MS surface (a target drawn in phase B, then again in phase A: rare) */
static int mft_unresolve(int id) {
    mtarget* m = &MT[id];
    if (!m->ms) return 0;
    if (m->alias) return 1;
    int pc = PR.cur;
    prof_mark(PR_MFT_RES);
    float r[4] = { 0.0f, 0.0f, (float)m->w, (float)m->h };
    mft_src(m->psx / m->sx, m->psy / m->sy, 0.0f, 0.0f, m->pw, m->ph);
    unbind_all();
    if (m->depth) {
        mft_bind_raw(NULL, m->dsv);
        ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 17, 1, &m->psrv);
        set_depth(X.ds_write);
        set_blend(blend_state(BL_OFF, 0));
        mft_quad(r, GPU_SH_ps_mft_zfrom);
    } else {
        mft_bind_raw(m->rtv, NULL);
        ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 18, 1, &m->psrv);
        ID3D11SamplerState* s = mft_sampler();
        ID3D11DeviceContext_PSSetSamplers(gpu_rt.ctx, 0, 1, &s);
        set_depth(X.ds_off);
        set_blend(blend_state(BL_OFF, 15));
        mft_quad(r, GPU_SH_ps_mft_copy);
    }
    ID3D11ShaderResourceView* none3[3] = { NULL, NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 16, 3, none3);
    prof_mark(pc);
    return 1;
}

/* the twin's content back into the grid target (an exact frame or a read-back uses it next); nr: only that native rect
 * [x0, x1) x [y0, y1) (a download of some blocks), NULL: all */
static int mft_materialize_r(int id, const int* nr) {
    mtarget* m = &MT[id];
    xtarget* t = tgt((uint32_t)id);
    if (!m->ms || m->truth == MT_GRID) return 1;
    if (!t) { m->truth = MT_GRID; return 0; }
    int pc = PR.cur;
    int nx0 = nr ? nr[0] : 0, ny0 = nr ? nr[1] : 0, nx1 = nr ? nr[2] : t->w / t->kx, ny1 = nr ? nr[3] : t->h / t->ky;
    float r[4] = { (float)(nx0 * t->kx), (float)(ny0 * t->ky), (float)(nx1 * t->kx), (float)(ny1 * t->ky) };
    if (m->depth) {
        /* depth: sample 0 of the MS surface (or the picture), nearest, into the DSV; zpatch is stale from now on */
        int from_pic = m->truth == MT_PIC;
        float s_x = from_pic ? m->psx : m->sx, s_y = from_pic ? m->psy : m->sy;
        ensure_dsv(t, nx0, ny0, nx1 - 1, ny1 - 1);
        unbind_all();
        prof_mark(PR_MFT_RES);
        mft_bind_raw(NULL, t->dsv);
        ID3D11ShaderResourceView* s = from_pic ? m->psrv : m->srv;
        ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 16, 1, &s);
        /* grid sample q: GS q / k; the twin's pixel (q / k + 0.5) s */
        mft_src(s_x / (float)t->kx, s_y / (float)t->ky, 0.5f * s_x * (1.0f - 1.0f / (float)t->kx),
                0.5f * s_y * (1.0f - 1.0f / (float)t->ky), from_pic ? m->pw : m->w, from_pic ? m->ph : m->h);
        set_depth(X.ds_write);
        set_blend(blend_state(BL_OFF, 0));
        mft_quad(r, !from_pic && m->n > 1 ? GPU_SH_ps_mft_zto1 : GPU_SH_ps_mft_zto0);
        ID3D11ShaderResourceView* none = NULL;
        ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 16, 1, &none);
        zt_mark(t, nx0, ny0, nx1 - 1, ny1 - 1, ZT_DSV);
    } else {
        if (m->truth == MT_MS && !mft_resolve(id)) { m->truth = MT_GRID; return 0; }
        unbind_all();
        prof_mark(PR_MFT_RES);
        mft_bind_raw(t->rtv, NULL);
        ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 18, 1, &m->psrv);
        ID3D11SamplerState* s = mft_sampler();
        ID3D11DeviceContext_PSSetSamplers(gpu_rt.ctx, 0, 1, &s);
        mft_src(m->psx / (float)t->kx, m->psy / (float)t->ky, 0.5f * m->psx * (1.0f - 1.0f / (float)t->kx),
                0.5f * m->psy * (1.0f - 1.0f / (float)t->ky), m->pw, m->ph);
        set_depth(X.ds_off);
        set_blend(blend_state(BL_OFF, 15));
        mft_quad(r, GPU_SH_ps_mft_copy);
        ID3D11ShaderResourceView* none = NULL;
        ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 18, 1, &none);
    }
    unbind_all();
    prof_mark(pc);
    m->truth = MT_GRID;
    M.to_grid++;
    return 1;
}
static int mft_materialize(int id) { return mft_materialize_r(id, NULL); }

static void mft_materialize_all(void) {
    for (int i = 1; i <= MAX_TARGETS; i++)
        if (MT[i].ms && MT[i].truth != MT_GRID) { M.n_mat++; mft_materialize(i); }
}

/* the twin of id holds the target's newest content in the representation `want` (MT_MS / MT_PIC) */
static int mft_need(int id, int want) {
    if (!id) return 1;
    if (!mft_surf(id)) return 0;
    mtarget* m = &MT[id];
    if (m->alias) { if (m->truth == MT_GRID && !mft_from_grid(id, 1)) return 0; m->truth = want; return 1; }
    if (m->truth == want) return 1;
    int ok = m->truth == MT_GRID ? mft_from_grid(id, want == MT_PIC)
           : want == MT_PIC ? mft_resolve(id) : mft_unresolve(id);
    if (ok) m->truth = want;
    return ok;
}

/* ---- the plan ------------------------------------------------------------------------------------------------------ */

static int mft_enabled(void) {
    if (!M.env) {
        M.env = 1;
        const char* e = getenv("PS2_MFT");
        if (e && *e) mcf_env_on = *e != '0';
        e = getenv("PS2_MFT_SCALE");
        if (e && *e) mcf_env_pct = atoi(e) < 25 ? 25 : atoi(e) > 400 ? 400 : atoi(e);
        e = getenv("PS2_MFT_MV");
        M.mv_on = e && *e == '1';
        e = getenv("PS2_MFT_MV_VIEW");
        M.mv_view = M.mv_on && e && *e == '1';
        e = getenv("PS2_MFT_MSAA");
        if (e && *e) { int n = atoi(e); mcf_env_msaa = n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1; }
    }
    if (mcf_env_pct) mcf_pct = mcf_env_pct;
    if (mcf_env_msaa) mcf_msaa = mcf_env_msaa;
    return mcf_env_on >= 0 ? mcf_env_on : mcf_on > 0;
}

/* the shadow sequence starting at ALIAS16 op pos (gpu_exec.c shf_head's head, then volumes in epochs: a flat post pass on B
 * - a G bit plane written over the whole frame - ends an epoch; then shf_tail's tail) */
static int mft_shadow_seq(const gpu_front_frame_data* fd, size_t pos, mft_shp* q) {
    memset(q, 0, sizeof *q);
    M.shp_fail = 1;
    if (!shf_layout()) return 0;
    const gpu_fop_alias16* a = (const gpu_fop_alias16*)(fd->cmd + pos);
    if (a->h.size < sizeof *a || a->mode != 1) return 0;
    size_t p = pos + a->h.size;
    const gpu_fop_draw* d = (const gpu_fop_draw*)(fd->cmd + p);
    if (p + sizeof *d > fd->cmd_bytes || d->h.op != GPU_FOP_DRAW || d->ct != a->target || d->tex == a->target) return 0;
    int bt = d->tex, w, h;
    uint32_t c0[4];
    if (!shf_copy(d, bt, a->psm, c0, &w, &h)) return 0;
    q->head_slots = draw_slots(d, 1);
    p += d->h.size;
    const gpu_fop_alias16* e = (const gpu_fop_alias16*)(fd->cmd + p);
    if (p + sizeof(gpu_fop) > fd->cmd_bytes || e->h.op != GPU_FOP_ALIAS16_END || e->target != a->target) return 0;
    p += e->h.size;
    const gpu_fop_clear* cl = (const gpu_fop_clear*)(fd->cmd + p);
    if (p + sizeof *cl > fd->cmd_bytes || cl->h.op != GPU_FOP_CLEAR || cl->target != bt || cl->zclear || cl->mask != 2) return 0;
    xtarget* tb = tgt((uint32_t)bt);
    int cx1 = cl->x1 > 0 ? cl->x1 : tb ? tb->nat_w : 0, cy1 = cl->y1 > 0 ? cl->y1 : tb ? tb->nat_h : 0;
    if (cx1 != w || cy1 != h) return 0;
    q->vol0 = p + cl->h.size;
    int nvol = 0;
    for (p = q->vol0; p + sizeof(gpu_fop) <= fd->cmd_bytes;) {
        const gpu_fop* o = (const gpu_fop*)(fd->cmd + p);
        if (!o->size) return 0;
        if (o->op == GPU_FOP_DRAW) {
            const gpu_fop_draw* v = (const gpu_fop_draw*)o;
            if ((v->feat & GPU_DF_SVOL) && v->ct == bt) nvol++;
            else if ((v->feat & GPU_DF_NPOST) && v->ct == bt && !v->tex && v->cls == GPU_PC_SPRITE) {
                static npost_plan P;
                if (q->nflat >= 3 || !npost_prep(v, &P) || !(P.k.mode & 256u) || !P.whole || P.bb[0] || P.bb[1] || P.bb[2] != w ||
                    P.bb[3] != h)
                    return 0;
                q->flat[q->nflat][0] = P.k.mode; q->flat[q->nflat][1] = P.k.alpha;
                q->flat[q->nflat][2] = P.k.fmask; q->flat[q->nflat][3] = P.k.vcol;
                q->flat_pos[q->nflat++] = p;
            } else return 0;
        } else if (o->op != GPU_FOP_ROV_END) break;
        p += o->size;
    }
    if (!nvol) return 0;
    M.shp_fail = 2;
    M.shp_failpos = p;
    if (!shf_tail(fd, p, &q->L) || q->L.B != bt || q->L.A != a->target || q->L.W != w || q->L.H != h) return 0;
    M.shp_fail = 3;
    memcpy(q->L.k.c[0], c0, sizeof c0);
    q->L.k.clr[0] = 1; q->L.k.clr[1] = cl->value; q->L.k.clr[2] = 0x0000FF00u;
    q->L.k.np2[2] = 1;
    q->head = pos; q->tail = p; q->next = q->L.next;
    /* the other frame A: only the bits copy 0 wrote into it may be read back (MFT keeps no copy of A's MS content) */
    const uint32_t* cc0 = q->L.k.c[0];
    for (int i = 1; i < 3; i++) {
        const uint32_t* c = q->L.k.c[i];
        if (!(c[3] & 1u << 19) || !(c[3] & 1u << 18)) continue;
        if (!(cc0[3] & 1u << 19) || !(cc0[3] & 1u << 20) || ((c[3] >> 16) & 1u) != ((cc0[3] >> 17) & 1u)) return 0;
        /* copy i's written bits must depend only on source bits copy 0 wrote: checked over every 16-bit source value */
        uint32_t wb = c[3] & 0xFFFFu, wb0 = cc0[3] & 0xFFFFu;
        static uint32_t ck[4], cwb0, cres;
        if (memcmp(ck, c, 16) || cwb0 != wb0) {
            memcpy(ck, c, 16);
            cwb0 = wb0;
            cres = 1;
            uint32_t texa = c[0], fl = c[1], vc = c[2];
            for (uint32_t v = 0; v < 65536 && cres; v += 1) {
                uint32_t outs[2];
                for (int k = 0; k < 2; k++) {
                    uint32_t src = k ? v & wb0 : v;
                    uint32_t rgb = ((src & 0x1Fu) << 3) | ((src & 0x3E0u) << 6) | ((src & 0x7C00u) << 9);
                    uint32_t ta0 = texa & 0xFF, aem = (texa >> 15) & 1, ta1 = (texa >> 16) & 0xFF;
                    uint32_t a = (src & 0x8000u) ? ta1 : (aem && !(src & 0x7FFFu)) ? 0 : ta0;
                    uint32_t t = rgb | a << 24, tf = (fl >> 2) & 3, tcc = (fl & 16) != 0, col;
                    uint32_t cr = vc & 255, cg = (vc >> 8) & 255, cb = (vc >> 16) & 255, ca = vc >> 24;
                    uint32_t tr = t & 255, tg = (t >> 8) & 255, tb = (t >> 16) & 255, ta = t >> 24;
                    if (tf == 0) {
                        uint32_t r = (cr * tr) >> 7, g = (cg * tg) >> 7, b = (cb * tb) >> 7, aa = tcc ? (ca * ta) >> 7 : ca;
                        col = (r > 255 ? 255 : r) | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b) << 16 | (aa > 255 ? 255 : aa) << 24;
                    } else if (tf == 1) col = tcc ? t : (t & 0xFFFFFFu) | (vc & 0xFF000000u);
                    else { cres = 0; break; }
                    if (fl & 1) col |= 0x80000000u;
                    uint32_t c16 = ((col >> 3) & 0x1F) | ((col >> 6) & 0x3E0) | ((col >> 9) & 0x7C00) | ((col >> 16) & 0x8000);
                    outs[k] = c16 & wb;
                }
                if (cres && outs[0] != outs[1]) cres = 0;
            }
        }
        if (!cres) return 0;
    }
    M.shp_fail = 0;
    return 1;
}

/* (mft-pop) while mft_plan scans a frame's ops: the family each target will have at the scanned op. The frame's own
 * GPU_FOP_TARGET / GPU_FOP_RELEASE ops change it before the draws that follow run (a target created this frame - e.g. the
 * HUD's Z target, released after 120 frames unused and created again when a HUD element with Z returns - starts square
 * and is paired into the display family by its first draw with a display colour target); the executor's xtarget state
 * is still the previous frame's. -1: no change in this frame so far. */
static int8_t mft_pfam[MAX_TARGETS + 1];
static int mft_planning;

/* is target id one MFT twins (display family) */
static int mft_disp(int id) {
    if (mft_planning && id > 0 && id <= MAX_TARGETS && mft_pfam[id] >= 0) return mft_pfam[id] == 1;
    xtarget* t = tgt((uint32_t)id);
    return t && t->family == 1;
}

static int mft_no(int why) {
    M.why[why]++;
    M.why_last = why;
    static int log = -1, budget = 400;   /* (r2-flicker, diagnostics) PS2_MFT_LOG: every grid frame after a modern one, and why */
    if (log < 0) log = getenv("PS2_MFT_LOG") != NULL;
    if (log && why != MW_OFF && budget > 0 && M.frames && !M.grid_run) {
        budget--;
        const gpu_front_frame_data* fd = X.fd;
        uint32_t nop = 0, ndraw = 0, ndisp = 0, nr2 = 0, nclr = 0, npres = 0, nother = 0;   /* (what the frame holds) */
        for (size_t p = 0; fd && p + sizeof(gpu_fop) <= fd->cmd_bytes;) {
            const gpu_fop* h = (const gpu_fop*)(fd->cmd + p);
            if (!h->size) break;
            nop++;
            if (h->op == GPU_FOP_DRAW) {
                const gpu_fop_draw* d = (const gpu_fop_draw*)h;
                ndraw++;
                ndisp += mft_disp(d->ct) || (d->zt && mft_disp(d->zt));
            } else if (h->op == GPU_FOP_R2) nr2++;
            else if (h->op == GPU_FOP_CLEAR) nclr++;
            else if (h->op == GPU_FOP_PRESENT) npres++;
            else nother++;
            p += h->size;
        }
        gpu_log("mft: grid frame v%u (%s) after %llu modern: %u ops (%u draws, %u on display targets, %u R2, %u clears, %u presents, %u other)",
                fd ? fd->vblank : 0u, mw_names[why], (unsigned long long)M.frames, nop, ndraw, ndisp, nr2, nclr, npres, nother);
    }
    return 0;
}

/* a post-pass candidate: sprites reading a target view (the motion blur, flat bit-plane passes; npost_prep decides - also
 * without the frontend's NPOST mark, which needs an R2 hint within the last VBlank) */
static int mft_post_cand(const gpu_fop_draw* d) {
    return d->tier == GPU_TIER_FF && d->cls == GPU_PC_SPRITE && d->tex && (d->feat & (GPU_DF_NPOST | GPU_DF_VIEW));
}

/* the grid target gets the twin's content, which stays the truth too (a read of the target: a download, a copy source) */
static void mft_sync_grid(int id) {
    mtarget* m = &MT[id];
    if (!m->ms || m->truth == MT_GRID) return;
    int t = m->truth;
    mft_materialize(id);
    m->truth = t;
}

/* ---- ROV-tier draws on the surfaces: the output merger's form of each primitive's state -------------------------------
 * The ROV tier draws what the output merger cannot do exactly; on the modern frame's surfaces (no exactness promise, and
 * no UAV of a multisampled target) its draws run through the FF pipelines with the nearest output-merger state: a wrapping
 * add (COLCLAMP 0) saturates, a blend factor above 1 clamps, DATE tests a copy of the destination (after the resolve
 * point only). Byte-partial write masks, other blend formulas, PABE, 16-bit frames: not approximated (the frame stays
 * exact). The draw's primitives may differ in state (ROV draws key on bindings only): consecutive primitives of one
 * state form a sub-draw. */
typedef struct { int blend, wm, depth, split, afail, date; } mft_ffs;
static int mft_state_ff(const gpu_staterec* s, int has_z, mft_ffs* o) {
    uint32_t f = s->flags, t = s->test, al = s->alpha;
    memset(o, 0, sizeof *o);
    if (f & (GPU_SF_FB16 | GPU_SF_PABE)) return 0;
    int ate = t & 1, atst = (t >> 1) & 7, afail = (t >> 12) & 3;
    int c_ok = 1, a_ok = 1, zw_ok = 1;
    if (ate && atst == 0) {              /* NEVER: what AFAIL writes */
        if (afail == 0) c_ok = zw_ok = 0;
        else if (afail == 1) zw_ok = 0;
        else if (afail == 2) c_ok = 0;
        else { zw_ok = 0; a_ok = 0; }
    } else if (ate && atst != 1 && afail != 0) { o->split = 1; o->afail = afail; }
    int wm = 0;
    for (int b = 0; b < 4; b++) {
        uint32_t m = (s->fmask >> (8 * b)) & 0xFF;
        if (m != 0 && m != 0xFF) return 0;
        if (m != 0xFF) wm |= 1 << b;
    }
    if (!c_ok) wm = 0;
    if (!a_ok) wm &= 7;
    int A = (int)(al & 3), B = (int)((al >> 2) & 3), C = (int)((al >> 4) & 3), D = (int)((al >> 6) & 3);
    int bl = BL_OFF;
    if (f & GPU_SF_ABE) {
        int uses_cd = A == 1 || B == 1 || C == 1 || D == 1;
        if (A == B) { if (D == 1) { bl = BL_NOCOLOR; wm &= 8; } }
        else if (!uses_cd) bl = BL_OFF;
        else if (C != 1 && A == 0 && B == 2 && D == 1) bl = BL_ADD;
        else if (C != 1 && A == 2 && B == 0 && D == 1) bl = BL_REVSUB;
        else if (C != 1 && A == 0 && B == 1 && D == 1) bl = BL_DUAL;
        else return 0;
    }
    int depth = 0;
    uint32_t z = s->zinfo;
    if (has_z && ((z >> 3) & 1) && ((z >> 4) & 3)) {
        int ztst = (int)(z & 3), zread = ztst == 2 || ztst == 3, zwrite = !((z >> 2) & 1) && zw_ok;
        if (ztst == 0) { wm = 0; zwrite = 0; zread = 1; }
        if (zread || zwrite) depth = ztst * 2 + zwrite;
    }
    o->blend = bl; o->wm = wm; o->depth = depth; o->date = (t >> 14) & 1;
    return 1;
}

/* primitive i of draw d: its StateRec */
static const gpu_staterec* mft_prim_state(const gpu_front_frame_data* fd, const gpu_fop_draw* d, uint32_t i) {
    size_t off;
    uint32_t si;
    switch (d->cls) {
    case GPU_PC_TRI: off = d->rec_first + (size_t)i * ((d->ps_key & GPU_PSK_FF_FAST) ? 128u : 256u); if (off + 116 > fd->rec_bytes) return NULL; memcpy(&si, fd->rec + off + 112, 4); break;
    case GPU_PC_SPRITE: off = d->rec_first + (size_t)i * 64u; if (off + 24 > fd->rec_bytes) return NULL; memcpy(&si, fd->rec + off + 20, 4); break;
    case GPU_PC_THIN: off = d->rec_first + (size_t)i * 128u; if (off + 32 > fd->rec_bytes) return NULL; memcpy(&si, fd->rec + off + 28, 4); break;
    default: off = d->rec_first + (size_t)i * 32u; if (off + 16 > fd->rec_bytes) return NULL; memcpy(&si, fd->rec + off + 12, 4); si >>= 8; break;
    }
    if (si >= fd->state_count) return NULL;
    return (const gpu_staterec*)(fd->states + (size_t)si * GPU_STATE_BYTES);
}
static uint32_t mft_nprims(const gpu_fop_draw* d) { return d->cls == GPU_PC_TRI ? d->count / 3u : d->count; }

static int mft_rov_ok(const gpu_front_frame_data* fd, const gpu_fop_draw* d, int phase_b) {
    uint32_t n = mft_nprims(d);
    int has_z = d->zt && mft_disp(d->zt);
    for (uint32_t i = 0; i < n; i++) {
        const gpu_staterec* s = mft_prim_state(fd, d, i);
        mft_ffs o;
        int ok = s && mft_state_ff(s, has_z, &o);
        (void)phase_b;
        if (!ok) {
            static int log = -1;
            if (log < 0) { const char* e = getenv("PS2_MFT_LOG"); log = e ? 30 : 0; }
            if (log > 0) {
                log--;
                if (!s) gpu_log("mft: ROV primitive %u of %u: no state", i, n);
                else gpu_log("mft: ROV primitive %u of %u: flags %X test %X alpha %X fmask %08X zinfo %X", i, n, s->flags, s->test,
                             s->alpha, s->fmask, s->zinfo);
            }
            return 0;
        }
    }
    return 1;
}

static void mft_draw_range(const gpu_fop_draw* d, uint32_t slot, uint32_t p0, uint32_t np) {
    if (d->cls == GPU_PC_TRI) ID3D11DeviceContext_DrawIndexedInstanced(gpu_rt.ctx, np * 3u, 1, d->first + p0 * 3u, 0, slot);
    else ID3D11DeviceContext_DrawInstanced(gpu_rt.ctx, 4, np, 0, slot);
    gpu_rt.n_draws++;
    M.draws++;
}

/* DATE on the surfaces: the depth-stencil states (the stencil's bit 0 = the destination's alpha bit 7, written by the mark
 * pass; the draws test it: DATM 0 EQUAL 0, DATM 1 NOT_EQUAL 0 - the reference stays 0) */
static ID3D11DepthStencilState* mft_ds_date(int depth, int datm) {
    depth &= 7;
    if (M.ds_date[depth][datm]) return M.ds_date[depth][datm];
    static const D3D11_COMPARISON_FUNC f[4] = { D3D11_COMPARISON_NEVER, D3D11_COMPARISON_ALWAYS, D3D11_COMPARISON_GREATER_EQUAL,
                                                D3D11_COMPARISON_GREATER };
    D3D11_DEPTH_STENCIL_DESC dd;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = depth ? TRUE : FALSE;
    dd.DepthWriteMask = (depth & 1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = depth ? f[depth >> 1] : D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = TRUE;
    dd.StencilReadMask = 1;
    dd.StencilWriteMask = 0;
    D3D11_DEPTH_STENCILOP_DESC op = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                      datm ? D3D11_COMPARISON_NOT_EQUAL : D3D11_COMPARISON_EQUAL };
    dd.FrontFace = op;
    dd.BackFace = op;
    ID3D11Device_CreateDepthStencilState(gpu_rt.dev, &dd, &M.ds_date[depth][datm]);
    return M.ds_date[depth][datm];
}
static ID3D11DepthStencilState* mft_ds_mark(void) {
    if (M.ds_mark) return M.ds_mark;
    D3D11_DEPTH_STENCIL_DESC dd;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = FALSE;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = TRUE;
    dd.StencilReadMask = 0xFF;
    dd.StencilWriteMask = 1;
    D3D11_DEPTH_STENCILOP_DESC op = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_INCR_SAT, D3D11_COMPARISON_ALWAYS };
    dd.FrontFace = op;
    dd.BackFace = op;
    ID3D11Device_CreateDepthStencilState(gpu_rt.dev, &dd, &M.ds_mark);
    return M.ds_mark;
}
/* a stencil surface for DATE draws without a depth target (phase 0: MS at the render size, 1: the picture's) */
static ID3D11DepthStencilView* mft_sds(int phase, int w, int h, int n) {
    if (M.sds[phase] && M.sds_w[phase] == w && M.sds_h[phase] == h && M.sds_n[phase] == n) return M.sds_dsv[phase];
    GPU_RELEASE(M.sds_dsv[phase]); GPU_RELEASE(M.sds[phase]);
    M.sds[phase] = mtex(w, h, n, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_DEPTH_STENCIL);
    M.sds_dsv[phase] = M.sds[phase] ? mdsv(M.sds[phase], n > 1) : NULL;
    M.sds_w[phase] = w; M.sds_h[phase] = h; M.sds_n[phase] = n;
    return M.sds_dsv[phase];
}

static int mft_draw_setup(const gpu_fop_draw* d, int zuse, int force_dual);
/* a ROV draw: its primitives in runs of one output-merger state, each through the FF pipelines (mft_draw's shaders) */
static void mft_rov(const gpu_front_frame_data* fd, const gpu_fop_draw* d, uint32_t slot) {
    uint32_t n = mft_nprims(d);
    int has_z = d->zt && mft_disp(d->zt);
    if (!mft_draw_setup(d, has_z, 1)) return;   /* (the dual-source shaders: any blend class per sub-draw) */
    uint32_t* u = (uint32_t*)M.cbv;
    mtarget* c = &MT[d->ct];
    int ph = M.phase;
    for (uint32_t p0 = 0; p0 < n;) {
        mft_ffs a, b;
        const gpu_staterec* s = mft_prim_state(fd, d, p0);
        if (!s || !mft_state_ff(s, has_z, &a)) { p0++; continue; }
        uint32_t p1 = p0 + 1;
        for (; p1 < n; p1++) {
            const gpu_staterec* s2 = mft_prim_state(fd, d, p1);
            if (!s2 || !mft_state_ff(s2, has_z, &b) || memcmp(&a, &b, sizeof a)) break;
        }
        ID3D11DepthStencilView* dsv = NULL;
        int datm = (s->test >> 15) & 1;
        if (a.date) {                    /* the destination's alpha bit 7 into the stencil, over the draw's box */
            if ((has_z && !mft_need(d->zt, ph ? MT_PIC : MT_MS)) || !mft_need(d->ct, ph ? MT_PIC : MT_MS)) { p0 = p1; continue; }
            dsv = has_z ? (ph ? MT[d->zt].pdsv : MT[d->zt].dsv) : mft_sds(ph, ph ? c->pw : c->w, ph ? c->ph : c->h, ph ? 1 : c->n);
            if (!dsv) { p0 = p1; continue; }
            int pc = PR.cur;
            prof_mark(PR_MFT_RES);
            ID3D11DeviceContext_ClearDepthStencilView(gpu_rt.ctx, dsv, D3D11_CLEAR_STENCIL, 1.0f, 0);
            mft_bind_raw(NULL, dsv);
            ID3D11ShaderResourceView* src = ph ? c->psrv_u : c->srv;
            ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 12, 1, &src);
            set_depth(mft_ds_mark());
            set_blend(blend_state(BL_OFF, 0));
            float sx = ph ? M.osx : M.rsx, sy = ph ? M.osy : M.rsy;
            float r[4] = { (float)mft_px(d->bbox[0] - 1, sx), (float)mft_px(d->bbox[1] - 1, sy), (float)mft_px(d->bbox[2] + 2, sx),
                           (float)mft_px(d->bbox[3] + 2, sy) };
            mft_quad(r, !ph && c->n > 1 ? GPU_SH_ps_mft_date1 : GPU_SH_ps_mft_date0);
            ID3D11ShaderResourceView* none = NULL;
            ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 12, 1, &none);
            prof_mark(pc);
            /* the draw's own bindings again, the stencil surface as its depth-stencil */
            ID3D11RenderTargetView* rtv = ph ? c->prtv : c->rtv;
            ID3D11DeviceContext_OMSetRenderTargets(gpu_rt.ctx, 1, &rtv, dsv);
            M.bound = 0;
            mft_draw_setup(d, 0, 1);     /* (shaders, scissor; the binding above stays: mft_bind sees M.bound 0 -> rebinds) */
            ID3D11DeviceContext_OMSetRenderTargets(gpu_rt.ctx, 1, &rtv, dsv);
            M.bound = 0;
        }
        for (int half = 0; half < 1 + a.split; half++) {
            int depth = a.depth, wm = a.wm;
            if (half) { if (a.afail == 2) wm = 0; else { depth &= ~1; if (a.afail == 3) wm &= 7; } }
            if (!wm && !(depth & 1)) continue;
            u[5] = 1u | (uint32_t)a.blend << 8;
            u[6] = p0;
            u[7] = a.split ? (uint32_t)(half + 1) : 0u;
            mft_cb_write();
            set_blend(blend_state(a.blend, wm));
            set_depth(a.date ? mft_ds_date(has_z ? depth : 0, datm) : depth_state(depth));
            if (d->cls == GPU_PC_TRI) mft_draw_range(d, slot, p0, p1 - p0);
            else { ID3D11DeviceContext_DrawInstanced(gpu_rt.ctx, 4, p1 - p0, 0, slot); gpu_rt.n_draws++; M.draws++; }
            if (d->ct && wm) c->truth = ph ? MT_PIC : MT_MS;
            if (has_z && (depth & 1)) MT[d->zt].truth = ph ? MT_PIC : MT_MS;
        }
        if (a.date) { M.bound = 0; mft_draw_setup(d, has_z, 1); }   /* (the draw's own binding for the next run) */
        p0 = p1;
    }
    u[5] = u[6] = u[7] = 0;
    mft_cb_write();
    M.cb_phase = M.phase;
}

/* a draw on display targets MFT can run: 0 no (why), 1 yes */
static int mft_draw_ok(const gpu_front_frame_data* fd, const gpu_fop_draw* d, int phase_b, int* why) {
    if (d->tier == GPU_TIER_ROV) {       /* (approximated: mft_rov) */
        static int norov = -1;           /* (diagnostics) PS2_MFT_NOROV=1: frames with ROV draws stay exact */
        if (norov < 0) { const char* e = getenv("PS2_MFT_NOROV"); norov = e && *e == '1'; }
        if (norov || (d->feat & GPU_DF_SVOL) || d->cls >= GPU_PC_N) { *why = MW_TIER; return 0; }   /* (volumes: in a shadow sequence only) */
        if (getenv("PS2_MFT_LOG")) {     /* (diagnostics) the first primitive's state of each distinct ROV draw */
            static uint32_t seen[64];
            static int ns;
            const gpu_staterec* s0 = mft_prim_state(fd, d, 0);
            uint32_t k = s0 ? s0->flags * 31u + s0->test * 17u + s0->alpha * 7u + s0->fmask + (uint32_t)d->cls : 0;
            int found = 0;
            for (int i = 0; i < ns; i++) found |= seen[i] == k;
            if (!found && ns < 64 && s0) {
                seen[ns++] = k;
                gpu_log("mft: ROV draw cls %u n %u bb %d,%d-%d,%d: flags %X test %X alpha %X fmask %08X zinfo %X", d->cls, d->count,
                        d->bbox[0], d->bbox[1], d->bbox[2], d->bbox[3], s0->flags, s0->test, s0->alpha, s0->fmask, s0->zinfo);
            }
        }
        if ((d->feat & (GPU_DF_VIEW | GPU_DF_HIRES)) || (d->tex && tex_kind(d->ps_key) >= TK_VIEW_C32)) { *why = MW_VIEW; return 0; }
        if ((d->ct && !mft_disp(d->ct)) || (d->zt && !mft_disp(d->zt))) { *why = MW_TARGET; return 0; }
        if (!mft_rov_ok(fd, d, phase_b)) { *why = MW_TIER; return 0; }
        return 1;
    }
    if (d->tier != GPU_TIER_FF) { *why = MW_TIER; return 0; }
    if (d->cls >= GPU_PC_N) { *why = MW_CLASS; return 0; }
    if ((d->feat & (GPU_DF_VIEW | GPU_DF_HIRES)) || (d->tex && tex_kind(d->ps_key) >= TK_VIEW_C32)) {
        if (mft_post_cand(d)) {
            if (!phase_b) { *why = MW_NPOST; return 0; }
            static npost_plan P;
            if (!npost_prep(d, &P)) { *why = MW_NPOST; return 0; }
            int src = (P.k.mode & 2u) ? d->ct : d->tex;
            if (!mft_disp(src)) { *why = MW_VIEW; return 0; }
            return 1;
        }
        *why = MW_VIEW;
        return 0;
    }
    if ((d->ct && !mft_disp(d->ct)) || (d->zt && (d->depth >> 1) && !mft_disp(d->zt))) { *why = MW_TARGET; return 0; }
    (void)fd;
    return 1;
}

static int mft_plan_ops(const gpu_front_frame_data* fd);
static int mft_plan(const gpu_front_frame_data* fd) {
    memset(mft_pfam, -1, sizeof mft_pfam);
    mft_planning = 1;
    int on = mft_plan_ops(fd);
    mft_planning = 0;
    return on;
}

static int mft_plan_ops(const gpu_front_frame_data* fd) {
    M.on = 0;
    M.nshp = 0;
    if (!mft_enabled()) return mft_no(MW_OFF);
    if (!X.mixed || X.ref || gpu_rt.lost) return mft_no(MW_NOT_MODERN);
    if (ab_off()) return mft_no(MW_OFF); /* (PS2_GPU_AB: the exact grid in A phases, in-run comparisons) */
    int has_r2 = 0, why = -1, tgw = 0, tgh = 0;
    size_t present = ~(size_t)0;
    /* the phases: A (3D, MS) until the resolve point, B (HUD, post passes) after it, A again where the next game frame's 3D
     * begins in the same VBlank's ops */
    int phase = 0, in_run = 0;
    size_t run0 = 0;                     /* the first op of the current run of clears (the 2D class switch's) */
    M.nph = 0;
#define PH_SET(p, v) do { if (M.nph < 16) { M.ph_pos[M.nph] = (p); M.ph_val[M.nph++] = (v); } phase = (v); } while (0)
    for (size_t pos = 0; pos + sizeof(gpu_fop) <= fd->cmd_bytes;) {
        const gpu_fop* h = (const gpu_fop*)(fd->cmd + pos);
        if (!h->size) break;
        if (h->op == GPU_FOP_CLEAR || h->op == GPU_FOP_ROV_END || h->op == GPU_FOP_CLUT || h->op == GPU_FOP_PATCH) {
            if (!in_run) { in_run = 1; run0 = pos; }
        } else if (h->op != GPU_FOP_MARK) in_run = 0;
        switch (h->op) {
        case GPU_FOP_TARGET: {           /* (a display target created or grown: the surfaces' size covers it) */
            const gpu_fop_target* o = (const gpu_fop_target*)h;
            if (o->id > 0 && o->id <= MAX_TARGETS) {   /* (mft-pop) */
                static int log = -1;
                if (log < 0) log = getenv("PS2_MFT_LOG") != NULL;
                if (log && mft_disp(o->id) != (o->family == 1))
                    gpu_log("mft: v%u target %u kind %u base %X family %u in the frame (%s)", fd->vblank, o->id, o->kind, o->base,
                            o->family, tgt(o->id) ? "live" : "created");
                mft_pfam[o->id] = o->family == 1 ? 1 : 0;
            }
            if (o->family == 1) {
                if (o->fbw * 64 > tgw) tgw = o->fbw * 64;
                if (o->rows > tgh) tgh = o->rows;
            }
            break;
        }
        case GPU_FOP_RELEASE: {          /* (mft-pop: the id may be created again later in the frame) */
            const gpu_fop_target* o = (const gpu_fop_target*)h;
            if (o->id > 0 && o->id <= MAX_TARGETS) mft_pfam[o->id] = 0;
            break;
        }
        case GPU_FOP_LOAD: case GPU_FOP_DOWNLOAD: case GPU_FOP_XCOPY:
            break;                       /* (on display targets: the grid synced first, mft_op) */
        case GPU_FOP_COPY_SHADOW:
            if (mft_disp(((const gpu_fop_shadow*)h)->target)) why = MW_MEM;
            break;
        case GPU_FOP_ALIAS16: {
            const gpu_fop_alias16* a = (const gpu_fop_alias16*)h;
            if (!mft_disp(a->target)) break;
            mft_shp q;
            if (M.nshp >= 4 || !mft_shadow_seq(fd, pos, &q)) { why = MW_A16; break; }
            if (phase) PH_SET(pos, 0);   /* (after the resolve: the next game frame's 3D) */
            M.shp[M.nshp++] = q;
            pos = q.next;
            in_run = 0;
            continue;
        }
        case GPU_FOP_ALIAS16_END:
            if (mft_disp(((const gpu_fop_alias16*)h)->target)) why = MW_A16;
            break;
        case GPU_FOP_DRAW: {
            const gpu_fop_draw* d = (const gpu_fop_draw*)h;
            int disp = mft_disp(d->ct) || (d->zt && mft_disp(d->zt)) || (d->tex && mft_disp(d->tex));
            if (!disp) break;
            int w = -1;
            if (mft_post_cand(d) && !phase) PH_SET(pos, 1);   /* (a post pass: the resolve point at the latest) */
            if (!mft_draw_ok(fd, d, phase, &w)) why = w;
            break;
        }
        case GPU_FOP_R2: {
            const gpu_fop_r2* o = (const gpu_fop_r2*)h;
            if (phase) PH_SET(pos, 0);   /* (after the resolve: the next game frame's 3D - a VBlank's ops can hold the end of
                                            one frame and the start of the next) */
            if ((size_t)o->draw_off + (size_t)o->ndraw * sizeof(gpu_r2_draw) > fd->data_bytes) { why = MW_R2CLIP; break; }
            const gpu_r2_draw* dr = (const gpu_r2_draw*)(fd->data + o->draw_off);
            for (uint32_t i = 0; i < o->ndraw; i++) if ((dr[i].state >> 13) & 1) { why = MW_R2CLIP; break; }
            if (!mft_disp(o->ct) || !mft_disp(o->zt)) why = MW_TARGET;
            has_r2 = 1;
            break;
        }
        case GPU_FOP_MARK:               /* (the 2D class: phase B from the class switch's clears before it) */
            if (((const gpu_fop_mark*)h)->kind == GPU_MARK_2D && !phase) PH_SET(in_run ? run0 : pos, 1);
            break;
        case GPU_FOP_PRESENT:
            present = pos;
            break;
        default:
            break;
        }
        if (why >= 0) {
            static int log = -1;         /* PS2_MFT_LOG=<n>: every n-th rejected frame's op (diagnostics) */
            static uint32_t nrej;
            if (log < 0) { const char* e = getenv("PS2_MFT_LOG"); log = e ? atoi(e) : 0; }
            if (log > 0 && ++nrej % (uint32_t)log == 0) {
                char s[200];
                prof_op_sig(fd, (uint32_t)pos, PR_OTHER, s, sizeof s);
                size_t l = strlen(s);
                if (h->op == GPU_FOP_DRAW) {
                    const gpu_fop_draw* d = (const gpu_fop_draw*)h;
                    snprintf(s + l, sizeof s - l, " tier %u cls %u feat %X n %u bb %d,%d-%d,%d", d->tier, d->cls, d->feat, d->count,
                             d->bbox[0], d->bbox[1], d->bbox[2], d->bbox[3]);
                }
                gpu_log("mft: v%u exact (%s%s) at @%u: %s", fd->vblank, mw_names[why],
                        why == MW_A16 ? (M.shp_fail == 1 ? " head" : M.shp_fail == 2 ? " tail" : M.shp_fail == 3 ? " A" : "") : "",
                        (unsigned)pos, s);
                if (why == MW_A16) {     /* (the ops that follow; a tail: from where it was expected) */
                    size_t p = M.shp_fail == 2 ? M.shp_failpos : pos + h->size;
                    if (M.shp_fail == 2) gpu_log("mft:   tail at @%u of %u bytes:", (unsigned)p, (unsigned)fd->cmd_bytes);
                    for (int k = 0; k < (M.shp_fail == 2 ? 12 : 8) && p + sizeof(gpu_fop) <= fd->cmd_bytes; k++) {
                        const gpu_fop* q = (const gpu_fop*)(fd->cmd + p);
                        if (!q->size) break;
                        prof_op_sig(fd, (uint32_t)p, PR_OTHER, s, sizeof s);
                        if (q->op == GPU_FOP_CLEAR) {
                            const gpu_fop_clear* c = (const gpu_fop_clear*)q;
                            gpu_log("mft:   + clear target %u z %u value %08X mask %X rect %d,%d", c->target, c->zclear, c->value, c->mask, c->x1, c->y1);
                        } else if (q->op == GPU_FOP_COPY_SHADOW) {
                            const gpu_fop_shadow* c = (const gpu_fop_shadow*)q;
                            gpu_log("mft:   + copy shadow target %u rect %d,%d-%d,%d", c->target, c->x0, c->y0, c->x1, c->y1);
                        } else if (q->op == GPU_FOP_ALIAS16 || q->op == GPU_FOP_ALIAS16_END) {
                            const gpu_fop_alias16* c = (const gpu_fop_alias16*)q;
                            gpu_log("mft:   + alias16%s target %u mode %u psm %X", q->op == GPU_FOP_ALIAS16 ? "" : " end", c->target, c->mode, c->psm);
                        } else if (q->op == GPU_FOP_DRAW) {
                            const gpu_fop_draw* d = (const gpu_fop_draw*)q;
                            gpu_log("mft:   + %s tier %u n %u bb %d,%d-%d,%d", s, d->tier, d->count, d->bbox[0], d->bbox[1], d->bbox[2], d->bbox[3]);
                        } else gpu_log("mft:   + %s", s);
                        p += q->size;
                    }
                }
            }
            return mft_no(why);
        }
        pos += h->size;
    }
    if (!has_r2) return mft_no(MW_NOT_MODERN);
    if (present == ~(size_t)0) return mft_no(MW_LAYOUT);
    /* the display: one circuit of a display target, 1:1, at buffer (0, 0) */
    const gpu_fop_present* po = (const gpu_fop_present*)(fd->cmd + present);
    if (po->di_off == ~0u || po->di_off + sizeof(gpu_display_info) > fd->data_bytes) return mft_no(MW_LAYOUT);
    const gpu_display_info* di = (const gpu_display_info*)(fd->data + po->di_off);
    gpu_compose_in in;
    memset(&in, 0, sizeof in);
    in.priv = di->priv;
    in.flicker_off = (di->gfx & GPU_GFX_FLICKER_OFF) != 0;
    in.fields = (di->gfx & GPU_GFX_FIELDS) != 0;
    for (int i = 0; i < 2; i++) {
        xtarget* t = tgt(po->src[i]);
        in.tex[i] = t && !t->depth && mft_disp(po->src[i]) ? t->srv_u : NULL;
    }
    int circ, x0, y0, lw, lh;
    if (!gpu_present_mft_layout(&in, &circ, &x0, &y0, &lw, &lh) || x0 || y0) return mft_no(MW_LAYOUT);
    int ow, oh;
    if (!gpu_present_output_rect(di->aspect_w, di->aspect_h, &ow, &oh)) return mft_no(MW_LAYOUT);
    /* the surfaces' GS size: the display targets' largest */
    int gw = tgw, gh = tgh;
    for (int i = 1; i <= MAX_TARGETS; i++) {
        xtarget* t = tgt((uint32_t)i);
        if (!t || !mft_disp(i)) continue;
        if (t->nat_w > gw) gw = t->nat_w;
        if (t->nat_h > gh) gh = t->nat_h;
    }
    if (gw <= 0 || gh <= 0 || gw > 2048 || gh > 2048) return mft_no(MW_SIZE);
    /* (only the displayed rect is drawn: rows below it - Sly's targets have 256 or 257 for 224 shown - are clipped) */
    if (gw > lw) gw = lw;
    if (gh > lh) gh = lh;
    int pct = (int)mcf_pct, n = (int)mcf_msaa;
    if (n > 1) {
        UINT q1 = 0, q2 = 0;
        if (FAILED(ID3D11Device_CheckMultisampleQualityLevels(gpu_rt.dev, DXGI_FORMAT_R8G8B8A8_UNORM, (UINT)n, &q1)) || !q1 ||
            FAILED(ID3D11Device_CheckMultisampleQualityLevels(gpu_rt.dev, DXGI_FORMAT_D32_FLOAT, (UINT)n, &q2)) || !q2)
            n = n > 2 ? n / 2 : 1;       /* (the next lower count; 2x is required at feature level 11) */
    }
    M.osx = (float)ow / (float)lw;
    M.osy = (float)oh / (float)lh;
    M.rsx = M.osx * (float)pct / 100.0f;
    M.rsy = M.osy * (float)pct / 100.0f;
    if (gw * M.osx > 8192.0f || gh * M.osy > 8192.0f || gw * M.rsx > 8192.0f || gh * M.rsy > 8192.0f) return mft_no(MW_SIZE);
    M.gw = gw; M.gh = gh; M.ow = ow; M.oh = oh; M.n = n;
#undef PH_SET
    M.on = 1;
    M.phase = 0;
    M.iph = 0;
    M.cb_phase = -1;
    M.bound = 0;
    M.shp_at = 0;
    M.nr2 = 0;
    return 1;
}

/* ---- execution -------------------------------------------------------------------------------------------------- */

/* the phase's surfaces of ct / zt bound (their content moved there first) */
static int mft_bind(int ct, int zt) {
    int want = M.phase ? MT_PIC : MT_MS;
    if ((ct && !mft_need(ct, want)) || (zt && !mft_need(zt, want))) return 0;
    if (M.bound && M.ct_b == ct && M.zt_b == zt && X.mode == 7) { mft_cb_scale(M.phase); return 1; }
    mtarget *c = ct ? &MT[ct] : NULL, *z = zt ? &MT[zt] : NULL;
    ID3D11RenderTargetView* rtv = c ? (M.phase ? c->prtv : c->rtv) : NULL;
    ID3D11DepthStencilView* dsv = z ? (M.phase ? z->pdsv : z->dsv) : NULL;
    if (X.b_t1) set_t1(NULL);
    if (X.b_t2) set_t2(NULL);
    ID3D11DeviceContext_OMSetRenderTargets(gpu_rt.ctx, rtv ? 1 : 0, rtv ? &rtv : NULL, dsv);
    X.mode = 7;
    X.b_ct = X.b_zt = -1;
    M.bound = 1; M.ct_b = ct; M.zt_b = zt;
    set_viewport(-REF_VP, -REF_VP);
    mft_cb_scale(M.phase);
    X.state_calls++;
    return 1;
}

static void mft_scissor(const int16_t sc[4]) {
    float sx = M.phase ? M.osx : M.rsx, sy = M.phase ? M.osy : M.rsy;
    set_scissor(mft_px(sc[0], sx), mft_px(sc[1], sy), mft_px(sc[2] + 1, sx), mft_px(sc[3] + 1, sy));
}

/* a draw's bindings, scissor and shaders on the phase's surfaces */
static int mft_draw_setup(const gpu_fop_draw* d, int zuse, int force_dual) {
    /* a depth-only draw keeps the bound colour surface (write mask 0): the outlines' Z pass alternates with their colour
     * pass, and rebinding multisampled targets per draw is not free */
    int ct = d->ct;
    if (!ct && zuse && M.bound && X.mode == 7 && M.zt_b == d->zt && M.ct_b && !d->wmask) ct = M.ct_b;
    if (!mft_bind(ct, zuse ? d->zt : 0)) return 0;
    mft_scissor(d->sc);
    int dual = d->blend == BL_DUAL || force_dual, ps;
    if (d->cls == GPU_PC_TRI) {
        set_topo(0);
        set_vs(gpu_vs(GPU_SH_vs_mft_tri));
        /* (perf-p2's specialisation: no texture at all / no general LOD / the decoded texture cache) */
        int fast = (d->ps_key & GPU_PSK_FF_FAST) != 0, notex = !(d->feat & GPU_DF_TEX), lod = (d->feat & GPU_DF_LOD) != 0;
        int tdd = fast && !notex && !lod && TD.ndpos && td_draw(X.curpos);
        if (tdd) TD.draws++;
        static const int t[2][2][3] = { { { GPU_SH_ps_mft_t0s, GPU_SH_ps_mft_t0sl, GPU_SH_ps_mft_t0sn },
                                          { GPU_SH_ps_mft_t0f, GPU_SH_ps_mft_t0fl, GPU_SH_ps_mft_t0fn } },
                                        { { GPU_SH_ps_mft_t1s, GPU_SH_ps_mft_t1sl, GPU_SH_ps_mft_t1sn },
                                          { GPU_SH_ps_mft_t1f, GPU_SH_ps_mft_t1fl, GPU_SH_ps_mft_t1fn } } };
        ps = tdd ? (dual ? GPU_SH_ps_mft_t1fd : GPU_SH_ps_mft_t0fd) : t[dual][fast][notex ? 2 : lod ? 1 : 0];
    } else {
        set_topo(1);
        int sprite = d->cls == GPU_PC_SPRITE, thin = d->cls == GPU_PC_THIN, notex = !(d->feat & GPU_DF_TEX);
        set_vs(gpu_vs(sprite ? GPU_SH_vs_mft_sprite : thin ? GPU_SH_vs_mft_thin : GPU_SH_vs_mft_linepix));
        ps = sprite ? (notex ? (dual ? GPU_SH_ps_mft_s1n : GPU_SH_ps_mft_s0n) : (dual ? GPU_SH_ps_mft_s1 : GPU_SH_ps_mft_s0))
           : thin ? (dual ? GPU_SH_ps_mft_n1 : GPU_SH_ps_mft_n0)
                  : notex ? (dual ? GPU_SH_ps_mft_l1n : GPU_SH_ps_mft_l0n) : (dual ? GPU_SH_ps_mft_l1 : GPU_SH_ps_mft_l0);
    }
    set_ps(gpu_ps(ps));
    return 1;
}

/* an FF draw on the phase's surfaces (exec_draw_ff's two halves of an ATST split, blend and depth classes) */
static void mft_draw(const gpu_fop_draw* d, uint32_t slot) {
    int zuse = d->zt && (d->depth >> 1) != 0;
    if (!mft_draw_setup(d, zuse, 0)) return;
    int zw = 0;
    for (int half = 0; half < 2; half++) {
        int depth = zuse ? d->depth : 0, wm = d->wmask;
        if (half == 0) {
            if (d->split && !(d->split & 1)) continue;
        } else {
            if (!(d->split & 2)) break;
            if (d->afail == 2) wm = 0;
            else { depth &= ~1; if (d->afail == 3) wm &= 7; }
        }
        if (!wm && !(depth & 1)) continue;
        set_blend(blend_state(d->blend, wm));
        set_depth(depth_state(depth));
        UINT s = slot + (uint32_t)(half && d->split == 3);
        if (d->cls == GPU_PC_TRI) ID3D11DeviceContext_DrawIndexedInstanced(gpu_rt.ctx, d->count, 1, d->first, 0, s);
        else ID3D11DeviceContext_DrawInstanced(gpu_rt.ctx, 4, d->count, 0, s);
        gpu_rt.n_draws++;
        M.draws++;
        zw |= depth & 1;
    }
    int want = M.phase ? MT_PIC : MT_MS;
    if (d->ct) MT[d->ct].truth = want;
    if (zuse && zw) MT[d->zt].truth = want;
}

static void mft_r2(const gpu_front_frame_data* fd, const gpu_fop_r2* o) {
    if (!mft_bind(o->ct, o->zt)) return;
    prof_mark(PR_R2);
    if (!iota_ensure(o->draw_off / 32u + o->ndraw + 1)) return;
    if (TD.on != 0) td_ensure();
    mtarget *c = &MT[o->ct], *z = &MT[o->zt];
    r2_exec_in in = { gpu_rt.dev, gpu_rt.ctx, c->rtv, z->dsv, 1, 1, X.vram_srv, X.swz_srv, X.dat.srv, X.iota,
                      gpu_rt.caps.warp ? 0.49609375f : 0.42f, td_r2, NULL, NULL };
    in.texdec_srv = TD.srv;
    in.mft = 1;
    in.msaa = M.n;
    in.sx = M.rsx;
    in.sy = M.rsy;
    int n = r2_render_segment(o, fd->data, fd->data_bytes, &in);
    gpu_rt.n_draws += (uint32_t)n;
    r2_segs++;
    r2_draws_total += (uint64_t)n;
    c->truth = MT_MS;
    if (!(o->flags & GPU_R2F_ZMSK)) z->truth = MT_MS;
    bind_frame_buffers();
    ID3D11Buffer* b = M.cb;              /* (r2's own constant buffers may sit in other slots; ours again) */
    ID3D11DeviceContext_VSSetConstantBuffers(gpu_rt.ctx, 6, 1, &b);
    ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 6, 1, &b);
    X.mode = 0;
    X.b_ct = X.b_zt = -1;
    M.bound = 0;
    if (M.mv_on && M.nr2 < 256) M.r2op[M.nr2++] = (const uint8_t*)o - fd->cmd;   /* (the motion-vector pass draws it again) */
}

/* ---- (r2-m8) motion vectors: the frame's R2 segments again into an R16G16_FLOAT surface (r2.hlsl r2_vs_mv: current -
 * previous position of every native entry's pass-A surface, render-surface pixels), resolved to one sample per pixel at the
 * end of phase A. Nothing reads them yet: the base for TAA / FSR / DLSS. On with PS2_MFT_MV=1 (else no cost at all);
 * PS2_MFT_MV_VIEW=1 shows them over the picture. Exact draws (particles, clipper fans, HUD) carry none (0). */
static int mft_mv_ensure(int w, int h, int n) {
    if (M.mv && M.mv_w == w && M.mv_h == h && M.mv_n == n) return 1;
    GPU_RELEASE(M.mv_rtv); GPU_RELEASE(M.mv); GPU_RELEASE(M.mvr_srv); GPU_RELEASE(M.mvr);
    M.mv_w = M.mv_h = M.mv_n = 0;
    M.mv = mtex(w, h, n, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    M.mvr = mtex(w, h, 1, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE | (n == 1 ? D3D11_BIND_RENDER_TARGET : 0));
    if (!M.mv || !M.mvr) return 0;
    D3D11_RENDER_TARGET_VIEW_DESC d;
    memset(&d, 0, sizeof d);
    d.Format = DXGI_FORMAT_R16G16_FLOAT;
    d.ViewDimension = n > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(ID3D11Device_CreateRenderTargetView(gpu_rt.dev, (ID3D11Resource*)M.mv, &d, &M.mv_rtv))) return 0;
    M.mvr_srv = msrv(M.mvr, 0, DXGI_FORMAT_R16G16_FLOAT);
    if (!M.mvr_srv) return 0;
    M.mv_w = w; M.mv_h = h; M.mv_n = n;
    return 1;
}

static void mft_mv_pass(const gpu_front_frame_data* fd) {
    if (!M.nr2) return;
    int pc = PR.cur;
    prof_mark(PR_MFT_MV);
    const gpu_fop_r2* o0 = (const gpu_fop_r2*)(fd->cmd + M.r2op[0]);
    mtarget* c = &MT[o0->ct];
    if (c->ms && mft_mv_ensure(c->w, c->h, c->n)) {
        unbind_all();
        static const float zero[4] = { 0, 0, 0, 0 };
        ID3D11DeviceContext_ClearRenderTargetView(gpu_rt.ctx, M.mv_rtv, zero);
        for (int i = 0; i < M.nr2; i++) {
            const gpu_fop_r2* o = (const gpu_fop_r2*)(fd->cmd + M.r2op[i]);
            mtarget* z = &MT[o->zt];
            if (!z->dsv || z->w != c->w || z->h != c->h) continue;
            r2_exec_in in = { gpu_rt.dev, gpu_rt.ctx, M.mv_rtv, z->dsv, 1, 1, X.vram_srv, X.swz_srv, X.dat.srv, X.iota,
                              gpu_rt.caps.warp ? 0.49609375f : 0.42f, td_r2, NULL, NULL };
            in.texdec_srv = TD.srv;
            in.mft = 1; in.mv = 1; in.msaa = M.n; in.sx = M.rsx; in.sy = M.rsy;
            r2_render_segment(o, fd->data, fd->data_bytes, &in);
        }
        unbind_all();
        if (c->n > 1) ID3D11DeviceContext_ResolveSubresource(gpu_rt.ctx, (ID3D11Resource*)M.mvr, 0, (ID3D11Resource*)M.mv, 0, DXGI_FORMAT_R16G16_FLOAT);
        else ID3D11DeviceContext_CopyResource(gpu_rt.ctx, (ID3D11Resource*)M.mvr, (ID3D11Resource*)M.mv);
        bind_frame_buffers();
        ID3D11Buffer* b = M.cb;
        ID3D11DeviceContext_VSSetConstantBuffers(gpu_rt.ctx, 6, 1, &b);
        ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 6, 1, &b);
        X.mode = 0;
        M.bound = 0;
        M.mv_frames++;
    }
    M.nr2 = 0;
    prof_mark(pc);
}

/* (diagnostics) the resolved motion vectors over target id's picture */
static void mft_mv_view(int id) {
    mtarget* m = &MT[id];
    if (!M.mvr_srv || !m->prtv) return;
    unbind_all();
    mft_bind_raw(m->prtv, NULL);
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 18, 1, &M.mvr_srv);
    mft_src(M.rsx / M.osx, M.rsy / M.osy, 0.0f, 0.0f, M.mv_w, M.mv_h);
    set_depth(X.ds_off);
    set_blend(blend_state(BL_OFF, 7));
    float r[4] = { 0.0f, 0.0f, (float)m->pw, (float)m->ph };
    mft_quad(r, GPU_SH_ps_mft_mvview);
    ID3D11ShaderResourceView* none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 18, 1, &none);
    unbind_all();
}

static void mft_clear(const gpu_fop_clear* o) {
    xtarget* t = tgt(o->target);
    if (!t) return;
    int want = M.phase ? MT_PIC : MT_MS;
    mtarget* m = &MT[o->target];
    if (!mft_surf(o->target)) return;
    float sx = M.phase ? M.osx : M.rsx, sy = M.phase ? M.osy : M.rsy;
    int sw = M.phase ? m->pw : m->w, sh = M.phase ? m->ph : m->h;
    int x1 = o->x1 > 0 ? mft_px(o->x1, sx) : sw, y1 = o->y1 > 0 ? mft_px(o->y1, sy) : sh;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
    int whole = x1 >= sw && y1 >= sh;
    int pc = PR.cur;
    prof_mark(PR_CLEAR);
    if (t->depth) {
        /* (the rest of the surface keeps its content: the twin must hold it first unless all of it is cleared) */
        if (!whole && !mft_need(o->target, want)) { prof_mark(pc); return; }
        float z = (float)o->zclear * (1.0f / 1048576.0f);
        if (z > 1.0f) z = 1.0f;
        if (whole) {
            unbind_all();
            M.bound = 0;
            ID3D11DeviceContext_ClearDepthStencilView(gpu_rt.ctx, M.phase ? m->pdsv : m->dsv, D3D11_CLEAR_DEPTH, z, 0);
        } else {
            unbind_all();
            mft_bind_raw(NULL, M.phase ? m->pdsv : m->dsv);
            M.cbv[12] = z;
            set_depth(X.ds_write);
            set_blend(blend_state(BL_OFF, 0));
            float r[4] = { 0.0f, 0.0f, (float)x1, (float)y1 };
            mft_quad(r, GPU_SH_ps_mft_zfill);
        }
    } else {
        int mask = (int)(o->mask & 15);
        if ((!whole || mask != 15) && !mft_need(o->target, want)) { prof_mark(pc); return; }
        float v[4] = { (float)(o->value & 255) / 255.0f, (float)((o->value >> 8) & 255) / 255.0f,
                       (float)((o->value >> 16) & 255) / 255.0f, (float)(o->value >> 24) / 255.0f };
        if (whole && mask == 15) {
            unbind_all();
            M.bound = 0;
            ID3D11DeviceContext_ClearRenderTargetView(gpu_rt.ctx, M.phase ? m->prtv : m->rtv, v);
        } else {
            unbind_all();
            mft_bind_raw(M.phase ? m->prtv : m->rtv, NULL);
            memcpy(&M.cbv[12], v, sizeof v);
            set_depth(X.ds_off);
            set_blend(blend_state(BL_OFF, mask));
            float r[4] = { 0.0f, 0.0f, (float)x1, (float)y1 };
            mft_quad(r, GPU_SH_ps_mft_fill);
        }
    }
    m->truth = want;
    unbind_all();
    M.bound = 0;
    prof_mark(pc);
}

/* a post pass after the resolve point (the motion blur): cs_npost on the pictures, one thread per output pixel */
static int mft_npost(const gpu_fop_draw* d) {
    static npost_plan P;
    if (!npost_prep(d, &P)) return 0;
    if (!P.nr) return 1;
    if (!npcb_ensure()) return 0;
    npost_consts k = P.k;
    int same = (k.mode & 2u) != 0;
    int src = same ? d->ct : d->tex;
    if (!mft_need(d->ct, MT_PIC) || (!same && !mft_need(src, MT_PIC))) return 0;
    mtarget *t = &MT[d->ct], *v = &MT[src];
    if (X.mode) unbind_all();
    M.bound = 0;
    ID3D11DeviceContext* c = gpu_rt.ctx;
    ID3D11ShaderResourceView* none2[2] = { NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(c, 1, 2, none2);
    X.b_t1 = X.b_t2 = NULL;
    ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_npost), NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(c, 5, 1, &X.clut_srv);
    ID3D11UnorderedAccessView* u[2] = { t->puav, same ? NULL : v->puav };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 2, u, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 2, 1, &X.npcb);
    for (int i = 0; i < (P.whole ? 1 : P.nr); i++) {
        const int* q = P.whole ? P.bb : P.rc[i];
        k.rect[0] = (uint32_t)mft_px(q[0], M.osx); k.rect[1] = (uint32_t)mft_px(q[1], M.osy);
        k.rect[2] = (uint32_t)mft_px(q[2], M.osx); k.rect[3] = (uint32_t)mft_px(q[3], M.osy);
        if (k.rect[2] > (uint32_t)t->pw) k.rect[2] = (uint32_t)t->pw;
        if (k.rect[3] > (uint32_t)t->ph) k.rect[3] = (uint32_t)t->ph;
        if (k.rect[0] >= k.rect[2] || k.rect[1] >= k.rect[3]) continue;
        if (!npcb_write(&k)) break;
        ID3D11DeviceContext_Dispatch(c, (k.rect[2] - k.rect[0] + 7) / 8, (k.rect[3] - k.rect[1] + 7) / 8, 1);
        gpu_rt.n_dispatches++;
    }
    ID3D11UnorderedAccessView* nu[2] = { NULL, NULL };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 2, nu, NULL);
    X.state_calls += 10;
    XC.npost_draws++;
    t->truth = MT_PIC;
    return 1;
}

/* ---- the shadow pipeline: volumes into per-sample counters (one band of rows per epoch), then the fused pass ---------
 * counters: R32_UINT, two samples per word (16-bit lanes), sample f = x * n + s at (f / 2, epoch * rows + y) */
static int mft_cnt_ensure(int w, int h) {
    if (M.cnt && M.cnt_w >= w && M.cnt_h >= h) return 1;
    GPU_RELEASE(M.cnt_srv); GPU_RELEASE(M.cnt_uav); GPU_RELEASE(M.cnt);
    if (w < M.cnt_w) w = M.cnt_w;
    if (h < M.cnt_h) h = M.cnt_h;
    M.cnt_w = M.cnt_h = 0;
    M.cnt = mtex(w, h, 1, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    if (!M.cnt) return 0;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
    memset(&ud, 0, sizeof ud);
    ud.Format = DXGI_FORMAT_R32_UINT;
    ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    if (FAILED(ID3D11Device_CreateUnorderedAccessView(gpu_rt.dev, (ID3D11Resource*)M.cnt, &ud, &M.cnt_uav))) { GPU_RELEASE(M.cnt); return 0; }
    M.cnt_srv = msrv(M.cnt, 0, DXGI_FORMAT_R32_UINT);
    if (!M.cnt_srv) { GPU_RELEASE(M.cnt_uav); GPU_RELEASE(M.cnt); return 0; }
    static const UINT zero[4] = { 0, 0, 0, 0 };
    ID3D11DeviceContext_ClearUnorderedAccessViewUint(gpu_rt.ctx, M.cnt_uav, zero);
    M.cnt_w = w; M.cnt_h = h;
    return 1;
}

static void mft_svol(const gpu_fop_draw* d, uint32_t slot, int epoch) {
    mtarget* m = &MT[d->ct];
    int zuse = d->zt && (d->depth >> 1) != 0;
    if (!mft_need(d->ct, MT_MS) || (zuse && !mft_need(d->zt, MT_MS))) return;
    mtarget* z = zuse ? &MT[d->zt] : NULL;
    if (X.b_t1) set_t1(NULL);
    if (X.b_t2) set_t2(NULL);
    ID3D11UnorderedAccessView* u = M.cnt_uav;
    ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(gpu_rt.ctx, 0, NULL, z ? z->dsv : NULL, 1, 1, &u, NULL);
    X.mode = 7;
    X.b_ct = X.b_zt = -1;
    M.bound = 0;
    set_viewport(-REF_VP, -REF_VP);
    mft_cb_scale(0);
    uint32_t* cu = (uint32_t*)M.cbv;
    if (cu[6] != (uint32_t)(epoch * m->h)) { cu[6] = (uint32_t)(epoch * m->h); mft_cb_write(); }
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 12, 1, &m->srv);
    mft_scissor(d->sc);
    set_topo(0);
    set_vs(gpu_vs(GPU_SH_vs_mft_tri));
    int fast = (d->ps_key & GPU_PSK_FF_FAST) != 0, msv = m->n > 1;
    set_ps(gpu_ps(fast ? (msv ? GPU_SH_ps_mft_svol_f1 : GPU_SH_ps_mft_svol_f0) : (msv ? GPU_SH_ps_mft_svol_s1 : GPU_SH_ps_mft_svol_s0)));
    set_blend(blend_state(BL_OFF, 0));
    set_depth(depth_state(zuse ? d->depth & ~1 : 0));
    ID3D11DeviceContext_DrawIndexedInstanced(gpu_rt.ctx, d->count, 1, d->first, 0, slot);
    gpu_rt.n_draws++;
    M.svols++;
    /* the counters this draw may touch (its native bbox +- 1) */
    int bx0 = mft_px(d->bbox[0] - 1, m->sx), by0 = mft_px(d->bbox[1] - 1, m->sy);
    int bx1 = mft_px(d->bbox[2] + 2, m->sx), by1 = mft_px(d->bbox[3] + 2, m->sy);
    if (bx0 < M.sv_bb[0]) M.sv_bb[0] = bx0;
    if (by0 < M.sv_bb[1]) M.sv_bb[1] = by0;
    if (bx1 > M.sv_bb[2]) M.sv_bb[2] = bx1;
    if (by1 > M.sv_bb[3]) M.sv_bb[3] = by1;
    ID3D11ShaderResourceView* none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 12, 1, &none);
    unbind_all();
}

static int mft_spare(const mtarget* m) {
    if (M.spare && M.spare_w == m->w && M.spare_h == m->h && M.spare_n == m->n) return 1;
    GPU_RELEASE(M.spare_rtv); GPU_RELEASE(M.spare_srv); GPU_RELEASE(M.spare);
    M.spare_w = M.spare_h = M.spare_n = 0;
    M.spare = mtex(m->w, m->h, m->n, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    if (!M.spare) return 0;
    M.spare_rtv = mrtv(M.spare, m->n > 1);
    M.spare_srv = msrv(M.spare, m->n > 1, DXGI_FORMAT_R8G8B8A8_UINT);
    if (!M.spare_rtv || !M.spare_srv) { GPU_RELEASE(M.spare_rtv); GPU_RELEASE(M.spare_srv); GPU_RELEASE(M.spare); return 0; }
    M.spare_w = m->w; M.spare_h = m->h; M.spare_n = m->n;
    return 1;
}

typedef struct { uint32_t svbb[4], c[3][4], clr[4], np[4], np2[4], ep[4], flat[3][4]; } mshf_consts;   /* (mft.hlsl mshf_cb) */
_Static_assert(sizeof(mshf_consts) == 176, "mshf_cb");

/* the fused tail of sequence q on B: B' = f(B, counters) into the spare surface, which becomes B's */
static void mft_shfuse(const mft_shp* q, int epochs) {
    int B = q->L.B;
    mtarget* m = &MT[B];
    if (!mft_need(B, MT_MS) || !mft_spare(m)) return;
    mshf_consts k;
    memset(&k, 0, sizeof k);
    memcpy(k.c, q->L.k.c, sizeof k.c);
    memcpy(k.clr, q->L.k.clr, sizeof k.clr);
    memcpy(k.np, q->L.k.np, sizeof k.np);
    memcpy(k.np2, q->L.k.np2, sizeof k.np2);
    memcpy(k.flat, q->flat, sizeof k.flat);
    k.ep[0] = (uint32_t)epochs;
    k.ep[1] = (uint32_t)m->h;
    int has_cnt = M.sv_bb[2] > M.sv_bb[0];
    if (has_cnt) {
        k.svbb[0] = (uint32_t)(M.sv_bb[0] < 0 ? 0 : M.sv_bb[0]); k.svbb[1] = (uint32_t)(M.sv_bb[1] < 0 ? 0 : M.sv_bb[1]);
        k.svbb[2] = (uint32_t)(M.sv_bb[2] > m->w ? m->w : M.sv_bb[2]); k.svbb[3] = (uint32_t)(M.sv_bb[3] > m->h ? m->h : M.sv_bb[3]);
    }
    if (!M.scb) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof(mshf_consts);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &M.scb))) return;
    }
    ID3D11DeviceContext_UpdateSubresource(gpu_rt.ctx, (ID3D11Resource*)M.scb, 0, NULL, &k, 0, 0);
    int pc = PR.cur;
    prof_mark(PR_MFT_SH);
    unbind_all();
    /* the pass covers the whole surface when the frame (shf_tail: the darken covers W x H native) does */
    int W = mft_px(q->L.W, m->sx), H = mft_px(q->L.H, m->sy);
    if (W < m->w || H < m->h)            /* (the rest of the surface: a copy first) */
        ID3D11DeviceContext_CopyResource(gpu_rt.ctx, (ID3D11Resource*)M.spare, (ID3D11Resource*)m->ms);
    ID3D11DepthStencilView* sds = m->n > 1 ? mft_sds(0, m->w, m->h, m->n) : NULL;
    if (sds) ID3D11DeviceContext_ClearDepthStencilView(gpu_rt.ctx, sds, D3D11_CLEAR_STENCIL, 1.0f, 0);
    mft_bind_raw(M.spare_rtv, sds);
    mft_cb_scale(0);
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 12, 1, &m->srv);
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 14, 1, &M.cnt_srv);
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 5, 1, &X.clut_srv);
    ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 5, 1, &M.scb);
    set_depth(X.ds_off);
    set_blend(blend_state(BL_OFF, 15));
    float r[4] = { 0.0f, 0.0f, (float)(W < m->w ? W : m->w), (float)(H < m->h ? H : m->h) };
    if (sds) {                           /* (MSAA: the pixels whose samples agree once per pixel - they mark the stencil -, the rest
                                            per sample: the deferred renderers' edge / interior split) */
        prof_mark(PR_MFT_FUSE);
        set_depth(mft_ds_mark());
        mft_quad(r, GPU_SH_ps_mft_shfuse_px);
        set_depth(mft_ds_date(0, 0));
        mft_quad(r, GPU_SH_ps_mft_shfuse1);
    } else {
        prof_mark(PR_MFT_FUSE);
        mft_quad(r, GPU_SH_ps_mft_shfuse0);
    }
    ID3D11ShaderResourceView* none[3] = { NULL, NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 12, 3, none);
    unbind_all();
    td_bind_ps();                        /* (t13: the decoded textures again) */
    /* the counters read: back to 0 (their rect in every epoch's band) */
    if (has_cnt) {
        int x0 = (int)k.svbb[0] * m->n / 2, x1 = ((int)k.svbb[2] * m->n + 1) / 2;
        D3D11_RECT rc[4];
        int nr = 0;
        for (int e = 0; e < epochs && e < 4; e++) {
            rc[nr].left = x0; rc[nr].right = x1;
            rc[nr].top = (LONG)k.svbb[1] + e * m->h; rc[nr].bottom = (LONG)k.svbb[3] + e * m->h;
            nr++;
        }
        static const float zero[4] = { 0, 0, 0, 0 };
        if (X.clearview) ID3D11DeviceContext1_ClearView(gpu_rt.ctx1, (ID3D11View*)M.cnt_uav, zero, rc, (UINT)nr);
        else { static const UINT zu[4] = { 0, 0, 0, 0 }; ID3D11DeviceContext_ClearUnorderedAccessViewUint(gpu_rt.ctx, M.cnt_uav, zu); }
    }
    /* the spare is B's surface now */
    if (m->alias) {                      /* (the picture texture stays: copy back) */
        ID3D11DeviceContext_CopyResource(gpu_rt.ctx, (ID3D11Resource*)m->ms, (ID3D11Resource*)M.spare);
    } else {
        ID3D11Texture2D* t = m->ms; ID3D11RenderTargetView* rv = m->rtv; ID3D11ShaderResourceView* sv = m->srv;
        m->ms = M.spare; m->rtv = M.spare_rtv; m->srv = M.spare_srv;
        M.spare = t; M.spare_rtv = rv; M.spare_srv = sv;
    }
    m->truth = MT_MS;
    prof_mark(pc);
    XC.npost_draws++;
}

/* at a planned shadow sequence's head: the volumes (an epoch per flat pass), then the fused tail; *next / *slots: the ops
 * it consumed */
static int mft_shadow(const gpu_front_frame_data* fd, const mft_shp* q, uint32_t slot, size_t* next, uint32_t* slots) {
    uint32_t s = slot + q->head_slots;
    M.sv_bb[0] = M.sv_bb[1] = 1 << 30; M.sv_bb[2] = M.sv_bb[3] = -(1 << 30);
    int epochs = q->nflat + 1;
    mtarget* m = &MT[q->L.B];
    int pc = PR.cur;
    prof_mark(PR_MFT_SH);
    int ok = mft_need(q->L.B, MT_MS) && mft_cnt_ensure((m->w * m->n + 1) / 2, m->h * epochs);
    int epoch = 0;
    for (size_t p = q->vol0; p < q->tail;) {
        const gpu_fop* h = (const gpu_fop*)(fd->cmd + p);
        if (!h->size) break;
        if (h->op == GPU_FOP_DRAW) {
            const gpu_fop_draw* d = (const gpu_fop_draw*)h;
            if (TD.jnext < TD.njob) td_run((uint32_t)p);
            if (epoch < q->nflat && p == q->flat_pos[epoch]) epoch++;
            else if (ok) mft_svol(d, s, epoch);
            s += draw_slots(d, 1);
        }
        p += h->size;
    }
    prof_mark(pc);
    if (ok) mft_shfuse(q, epochs);
    uint32_t* cu = (uint32_t*)M.cbv;
    if (cu[6]) { cu[6] = 0; mft_cb_write(); }
    *next = q->next;
    *slots = s - slot + q->L.slots;
    return 1;
}

/* the PRESENT of an MFT frame: the displayed target's picture */
static void mft_present(const gpu_front_frame_data* fd, const gpu_fop_present* o) {
    const gpu_display_info* di = (const gpu_display_info*)(fd->data + o->di_off);
    gpu_compose_in in;
    memset(&in, 0, sizeof in);
    in.priv = di->priv;
    in.flicker_off = (di->gfx & GPU_GFX_FLICKER_OFF) != 0;
    in.fields = (di->gfx & GPU_GFX_FIELDS) != 0;
    for (int i = 0; i < 2; i++) { xtarget* t = tgt(o->src[i]); in.tex[i] = t && !t->depth ? t->srv_u : NULL; }
    int circ, x0, y0, lw, lh;
    int id = gpu_present_mft_layout(&in, &circ, &x0, &y0, &lw, &lh) ? o->src[circ] : 0;
    if (!id || !MT[id].ms || MT[id].truth == MT_GRID) {   /* (a picture of the grid, e.g. the first modern frame's) */
        mft_materialize_all();
        exec_present(fd, o);
        return;
    }
    mft_mv_pass(fd);                     /* (a frame without a resolve point before its PRESENT) */
    if (!mft_need(id, MT_PIC)) { mft_materialize_all(); exec_present(fd, o); return; }
    unbind_all();
    M.bound = 0;
    mtarget* m = &MT[id];
    if (M.mv_view) mft_mv_view(id);
    int w = M.ow < m->pw ? M.ow : m->pw, h = M.oh < m->ph ? M.oh : m->ph;
    gpu_present_mft_picture(m->pic, w, h, (uint32_t)di->priv[15]);
    if (di->rb) gpu_present_readback((gpu_readback*)di->rb);
    gpu_rt.cur_vblank_ms = di->vblank_ms;
    gpu_rt.cur_behind = atomic_load(&inflight) > 1 + gpu_present_pace_frames();
    gpu_stats_cur()->scale = 0;
    if (di->flags & GPU_DI_SHOW) gpu_present_show(di->aspect_w, di->aspect_h, (int)((di->gfx >> GPU_GFX_SCALER_SHIFT) & 15));
    present_done = 1;
}

/* one op of an MFT frame; 1: done (*next, *slots: what it consumed), 0: the exact path runs it (memory ops, other targets) */
static int mft_op(const gpu_front_frame_data* fd, size_t pos, uint32_t slot, size_t* next, uint32_t* slots) {
    const gpu_fop* h = (const gpu_fop*)(fd->cmd + pos);
    *next = pos + h->size;
    *slots = 0;
    while (M.iph < M.nph && pos >= M.ph_pos[M.iph]) {
        int np = M.ph_val[M.iph++];
        if (!M.phase && np) mft_mv_pass(fd);   /* (the end of phase A: the frame's motion vectors) */
        M.phase = np;
    }
    switch (h->op) {
    case GPU_FOP_CLEAR: {
        const gpu_fop_clear* o = (const gpu_fop_clear*)h;
        if (!mft_disp(o->target)) return 0;
        mft_clear(o);
        return 1;
    }
    case GPU_FOP_ROV_END: {
        const gpu_fop_rov_end* o = (const gpu_fop_rov_end*)h;
        return mft_disp(o->ct) || mft_disp(o->zt);
    }
    case GPU_FOP_DOWNLOAD: {             /* (the exact path reads the grid target: it gets the twin's content first, those blocks) */
        const gpu_fop_blocks* o = (const gpu_fop_blocks*)h;
        xtarget* t = tgt(o->target);
        mtarget* m = &MT[o->target];
        int r[4];
        if (t && mft_disp(o->target) && m->ms && m->truth != MT_GRID && o->data_off + (size_t)o->n * sizeof(gpu_dl_entry) <= fd->data_bytes &&
            mft_blocks_rect(t, fd->data + o->data_off, o->n, sizeof(gpu_dl_entry), r)) {
            int tr = m->truth;
            mft_materialize_r(o->target, r);
            m->truth = tr;
            M.n_dl++;
            M.bound = 0;
        }
        return 0;
    }
    case GPU_FOP_LOAD: {                 /* (the exact path writes blocks of the grid target from GS memory: then into the twin) */
        const gpu_fop_blocks* o = (const gpu_fop_blocks*)h;
        xtarget* t = tgt(o->target);
        mtarget* m = &MT[o->target];
        int r[4];
        if (!t || !mft_disp(o->target) || !m->ms || m->truth == MT_GRID) return 0;
        M.n_load++;
        if (o->data_off + (size_t)o->n * 2 > fd->data_bytes || !mft_blocks_rect(t, fd->data + o->data_off, o->n, 2, r)) {
            mft_materialize(o->target);
            M.bound = 0;
            return 0;
        }
        exec_load(o);
        int tr = m->truth;
        if (!m->alias && tr == MT_PIC) mft_from_grid_r(o->target, 1, r);
        else mft_from_grid_r(o->target, tr == MT_PIC, r);
        M.bound = 0;
        return 1;
    }
    case GPU_FOP_XCOPY: {
        const gpu_fop_xcopy* o = (const gpu_fop_xcopy*)h;
        M.n_xcopy++;
        if (mft_disp(o->src)) mft_sync_grid(o->src);
        if (mft_disp(o->target)) mft_materialize(o->target);
        M.bound = 0;
        return 0;
    }
    case GPU_FOP_ALIAS16:
        for (int i = 0; i < M.nshp; i++)
            if (M.shp[i].head == pos) return mft_shadow(fd, &M.shp[i], slot, next, slots);
        return 0;
    case GPU_FOP_DRAW: {
        const gpu_fop_draw* d = (const gpu_fop_draw*)h;
        int disp = mft_disp(d->ct) || (d->zt && mft_disp(d->zt)) || (d->tex && mft_disp(d->tex));
        if (!disp) return 0;
        *slots = draw_slots(d, 1);
        if (PR.on > 0) {
            int zo = d->zt && (d->depth >> 1) != 0;
            prof_mark(d->cls == GPU_PC_TRI ? ((d->feat & GPU_DF_PART) ? PR_PART : zo ? PR_FF_FAST_Z : PR_FF_FAST)
                      : d->cls == GPU_PC_SPRITE ? PR_FF_SPRITE : PR_FF_LINE);   /* (line / point pixels, thin lines) */
        }
        if (mft_post_cand(d)) {
            if (!mft_npost(d)) M.why[MW_NPOST]++;
            return 1;
        }
        if (d->tier == GPU_TIER_ROV) { mft_rov(fd, d, slot); return 1; }
        mft_draw(d, slot);
        return 1;
    }
    case GPU_FOP_R2:
        mft_r2(fd, (const gpu_fop_r2*)h);
        return 1;
    case GPU_FOP_MARK:
        return 1;
    case GPU_FOP_PRESENT: {
        const gpu_fop_present* o = (const gpu_fop_present*)h;
        if (o->di_off == ~0u || o->di_off + sizeof(gpu_display_info) > fd->data_bytes) return 0;
        prof_mark(PR_PRESENT);
        mft_present(fd, o);
        return 1;
    }
    default:
        return 0;
    }
}

/* the frame's start: plan it; an exact frame first gets the twins' content back into the grid */
static int mft_frame_begin(const gpu_front_frame_data* fd) {
    int on = mft_plan(fd);
    static int log = -1, stats = -1;
    if (log < 0) log = getenv("PS2_MFT_LOG") != NULL;
    if (stats < 0) { const char* e = getenv("PS2_R2_STATS"); stats = e && *e && *e != '0'; }
    if (on) {
        if (M.grid_run && M.frames && !M.grid_off && M.grid_run <= MFT_POP_MAX) {   /* (mft-pop) a pop */
            M.pops++;
            M.pop_frames += M.grid_run;
            M.pop_why[M.grid_why0]++;
            if (log || stats) gpu_log("mft: pop: %u grid frame(s) v%u..v%u between modern frames (first: %s)", M.grid_run, M.grid_v0,
                             M.grid_v0 + M.grid_run - 1, mw_names[M.grid_why0]);
        }
        M.grid_run = 0;
        M.grid_off = 0;
        M.frames++;
    } else {
        if (!M.grid_run) { M.grid_v0 = fd->vblank; M.grid_why0 = M.why_last; }
        if (M.why_last == MW_OFF) M.grid_off = 1;
        M.grid_run++;
        M.grid++;
        mft_materialize_all();
    }
    if (stats && (M.frames + M.grid) % 300 == 0) {
        char line[640];
        int o = snprintf(line, sizeof line, "mft: %llu modern frames, %llu exact (", (unsigned long long)M.frames, (unsigned long long)M.grid);
        for (int i = 0; i < MW_N && o < (int)sizeof line - 40; i++)
            if (M.why[i]) o += snprintf(line + o, sizeof line - (size_t)o, "%s %llu, ", mw_names[i], (unsigned long long)M.why[i]);
        o += snprintf(line + o, sizeof line - (size_t)o, "); pops %llu (%llu frames", (unsigned long long)M.pops, (unsigned long long)M.pop_frames);
        for (int i = 0; i < MW_N && o < (int)sizeof line - 40; i++)
            if (M.pop_why[i]) o += snprintf(line + o, sizeof line - (size_t)o, ", %s %llu", mw_names[i], (unsigned long long)M.pop_why[i]);
        snprintf(line + o, sizeof line - (size_t)o, "); to grid %llu (downloads %llu, loads %llu, copies %llu, all %llu), from grid %llu, resolves %llu, draws %llu, volumes %llu; %dx%d -> %dx%d, %d samples",
                 (unsigned long long)M.to_grid, (unsigned long long)M.n_dl, (unsigned long long)M.n_load, (unsigned long long)M.n_xcopy, (unsigned long long)M.n_mat, (unsigned long long)M.from_grid, (unsigned long long)M.resolves,
                 (unsigned long long)M.draws, (unsigned long long)M.svols, (int)ceilf(M.gw * M.rsx), (int)ceilf(M.gh * M.rsy),
                 M.ow, M.oh, M.n);
        gpu_log("%s", line);
    }
    return on;
}
