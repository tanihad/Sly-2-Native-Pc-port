/* (light) Enhanced lighting of modern frames (notes/enhanced-lighting.md; shaders/light.hlsl). Included once by
 * gpu_exec_mft.h: it runs on the modern frame's surfaces only (MFT frames of the Modern renderer), so Reference, the
 * integer grid and every exact-path test never see it, and with Lighting = Original nothing here runs at all.
 *
 *   when    once per game frame, at its resolve point (the A -> B phase change before the HUD, or the PRESENT of a frame
 *           without one): the frame's 3D is complete in the MS surfaces, the HUD is not drawn yet. A frame that goes
 *           back to phase A (ep1: a late native entry after the outlines) is not processed twice: the R2 frame block's
 *           serial says which game frame the surfaces hold.
 *   what    ambient occlusion from the frame's depth (half resolution, 12 samples, depth-aware blur and upsample) and
 *           bloom of the picture's bright parts (half-resolution bright pass, a 5-level dual-filter pyramid), composited
 *           onto the picture in one dual-source blend: picture x AO + bloom, RGB only (the alpha byte is the GS's).
 *   depth   the GS Z is affine in 1 / w: from the frame block's screen model (GS Z = bz + az z / w) and an entry's
 *           world x clip matrix (clip z = a w + b for every entry of the frame) light_proj gets 1 / w = Za d + Zb, and the
 *           view x / y per picture pixel from the screen model's ax / ay and the matrix's x / w column lengths.
 *   config  gpu_light_config (gfx.c: graphics.lighting); PS2_LIGHT=0|1 wins. Per feature (tests, not in the menu):
 *           PS2_LIGHT_AO=0, PS2_LIGHT_BLOOM=0; tuning PS2_LIGHT_AO_R / _AO_K / _AO_MIN / _AO_FADE, PS2_LIGHT_BLOOM_T /
 *           _BLOOM_K / _BLOOM_I; PS2_LIGHT_LOG=1 (the projection, every 600 frames). In-run A/B: PS2_GPU_AB=<frames>
 *           PS2_GPU_AB_R8=64 (R8P_LIGHT: Original in A phases, Enhanced in B). */

static volatile LONG lcf_on = 0;         /* graphics.lighting (gfx.c); 0 until the settings say otherwise (tools, tests) */
void gpu_light_config(int on) { InterlockedExchange(&lcf_on, on ? 1 : 0); }
static volatile LONG lcf_reim = 0;       /* (reimagine) graphics.look = reimagined (gfx.c) */
void gpu_look_config(int reimagined) { InterlockedExchange(&lcf_reim, reimagined ? 1 : 0); }

enum { LT_W, LT_AO0, LT_AO1, LT_B1, LT_B2, LT_B3, LT_B4, LT_B5, LT_N };
static struct {
    int env, env_on, env_reim, ao, bloom, ppl, log, split, dbg;
    float rim, soft, ao_r, ao_k, ao_min, ao_f0, ao_f1, ao_pow, ao_max, ao_bias, bl_t, bl_k, bl_i, bl_sat;
    ID3D11Texture2D* t[LT_N];
    ID3D11RenderTargetView* rtv[LT_N];
    ID3D11ShaderResourceView* srv[LT_N];
    int tw[LT_N], th[LT_N];
    ID3D11Buffer* cb;
    ID3D11BlendState* bs_comp;
    /* the game frame the surfaces hold (the last R2 segment of phase A) */
    int have, ct, zt;
    uint32_t serial, done;
    gpu_r2_frame fb;
    uint32_t xyo;
    float ent[16];
    int ent_ok;
    /* statistics */
    uint64_t frames, skipped, noproj;
} LT;

static float lt_envf(const char* n, float d) { const char* e = getenv(n); return e && *e ? (float)atof(e) : d; }
static void lt_env(void) {
    if (LT.env) return;
    LT.env = 1;
    const char* e = getenv("PS2_LIGHT");
    LT.env_on = e && *e ? *e != '0' : -1;
    e = getenv("PS2_REIMAGINE");
    LT.env_reim = e && *e ? *e != '0' : -1;
    e = getenv("PS2_LIGHT_AO"); LT.ao = !(e && *e == '0');
    e = getenv("PS2_LIGHT_BLOOM"); LT.bloom = !(e && *e == '0');
    e = getenv("PS2_LIGHT_PPL"); LT.ppl = !(e && *e == '0');      /* per-pixel lighting of lit meshes (r2.hlsl R2_ENH) */
    e = getenv("PS2_LIGHT_LOG"); LT.log = e && *e == '1';
    e = getenv("PS2_LIGHT_DBG"); LT.dbg = e && *e ? atoi(e) : 0;   /* (1: the occlusion alone, 2: the bloom alone) */
    e = getenv("PS2_LIGHT_SPLIT"); LT.split = e && *e == '1';   /* (pictures: Enhanced on the right half only) */
    LT.soft = lt_envf("PS2_LIGHT_SOFT", 6.0f);        /* soft shadow edges: penumbra radius, output pixels at 1440p (0: hard) */
    LT.rim = lt_envf("PS2_LIGHT_RIM", 0.5f);          /* rim light on lit meshes (0: none) */
    LT.ao_r = lt_envf("PS2_LIGHT_AO_R", 60.0f);         /* radius, world units (Sly: ~190 units tall crouching) */
    LT.ao_k = lt_envf("PS2_LIGHT_AO_K", 3.0f);          /* strength */
    LT.ao_min = lt_envf("PS2_LIGHT_AO_MIN", 0.35f);     /* darkest factor */
    LT.ao_f0 = lt_envf("PS2_LIGHT_AO_FADE0", 4000.0f);    /* fades out from here ... */
    LT.ao_f1 = lt_envf("PS2_LIGHT_AO_FADE", 10000.0f);    /* ... to here (view depth, world units) */
    LT.ao_pow = lt_envf("PS2_LIGHT_AO_POW", 1.0f);
    LT.ao_max = lt_envf("PS2_LIGHT_AO_MAXPX", 48.0f);   /* largest radius, half-res texels */
    LT.ao_bias = lt_envf("PS2_LIGHT_AO_BIAS", 0.06f);
    LT.bl_t = lt_envf("PS2_LIGHT_BLOOM_T", 0.80f);      /* threshold (0..1 of the picture's channel) */
    LT.bl_k = lt_envf("PS2_LIGHT_BLOOM_K", 0.10f);      /* soft knee */
    LT.bl_i = lt_envf("PS2_LIGHT_BLOOM_I", 1.20f);      /* intensity */
    LT.bl_sat = lt_envf("PS2_LIGHT_BLOOM_SAT", 0.85f);
}
/* (reimagine) Look = Reimagined (graphics.look; PS2_REIMAGINE=0|1 wins; A/B: PS2_GPU_AB_R8=1024): every Enhanced
 * feature plus the Reimagined passes (gpu_reim.h), whatever graphics.lighting says */
static int lt_reim(void) {
    lt_env();
    int on = LT.env_reim >= 0 ? LT.env_reim : lcf_reim > 0;
    return on && r8p_on(R8P_REIM) && r8p_on(R8P_LIGHT);
}
static int lt_enabled(void) {
    lt_env();
    int on = LT.env_on >= 0 ? LT.env_on : lcf_on > 0;
    if (!on && LT.env_on < 0 && lt_reim()) on = 1;
    return on && (LT.ao || LT.bloom || LT.ppl || LT.soft > 0.0f) && r8p_on(R8P_LIGHT);
}

#include "gpu_reim.h"                    /* (reimagine) Look = Reimagined */

static void light_release(void) {
    for (int i = 0; i < LT_N; i++) { GPU_RELEASE(LT.rtv[i]); GPU_RELEASE(LT.srv[i]); GPU_RELEASE(LT.t[i]); LT.tw[i] = LT.th[i] = 0; }
    GPU_RELEASE(LT.cb);
    GPU_RELEASE(LT.bs_comp);
    LT.have = 0;
    reim_release();                      /* (reimagine) */
}

static int lt_tex(int i, int w, int h, DXGI_FORMAT f) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (LT.t[i] && LT.tw[i] == w && LT.th[i] == h) return 1;
    GPU_RELEASE(LT.rtv[i]); GPU_RELEASE(LT.srv[i]); GPU_RELEASE(LT.t[i]);
    LT.tw[i] = LT.th[i] = 0;
    LT.t[i] = mtex(w, h, 1, f, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    if (!LT.t[i]) return 0;
    if (FAILED(ID3D11Device_CreateRenderTargetView(gpu_rt.dev, (ID3D11Resource*)LT.t[i], NULL, &LT.rtv[i])) ||
        FAILED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)LT.t[i], NULL, &LT.srv[i]))) {
        GPU_RELEASE(LT.rtv[i]); GPU_RELEASE(LT.srv[i]); GPU_RELEASE(LT.t[i]);
        return 0;
    }
    LT.tw[i] = w; LT.th[i] = h;
    return 1;
}

/* the frame's R2 segment (mft_r2): remember what the post pass needs - the targets, the frame block and an entry matrix
 * whose z column is a w column multiple (the frame's projection) */
static void light_note(const gpu_front_frame_data* fd, const gpu_fop_r2* o) {
    if (!lt_enabled()) { LT.have = 0; return; }
    if (o->frame_off + sizeof(gpu_r2_frame) > fd->data_bytes) return;
    const gpu_r2_frame* fb = (const gpu_r2_frame*)(fd->data + o->frame_off);
    if (!LT.have || fb->serial != LT.serial || !LT.ent_ok) {
        LT.ent_ok = 0;
        for (uint32_t i = 0; i < o->nent && i < 32 && !LT.ent_ok; i++) {
            if (o->ent_off + (size_t)(i + 1) * sizeof(gpu_r2_entry) > fd->data_bytes) break;
            const gpu_r2_entry* e = (const gpu_r2_entry*)(fd->data + o->ent_off) + i;
            const float* m = e->m;
            float c3 = m[3] * m[3] + m[7] * m[7] + m[11] * m[11];
            float c0 = m[0] * m[0] + m[4] * m[4] + m[8] * m[8];
            if (!(c3 > 1e-12f && c3 < 1e12f) || !(c0 > 1e-12f && c0 < 1e12f)) continue;
            float a = (m[2] * m[3] + m[6] * m[7] + m[10] * m[11]) / c3;
            float r = 0.0f, n2 = 0.0f;
            for (int k = 0; k < 3; k++) { float d = m[k * 4 + 2] - a * m[k * 4 + 3]; r += d * d; n2 += m[k * 4 + 2] * m[k * 4 + 2]; }
            if (!(r <= 1e-6f * n2)) continue;
            memcpy(LT.ent, m, sizeof LT.ent);
            LT.ent_ok = 1;
        }
    }
    if (lt_reim()) reim_note(fd, o, fb);   /* (reimagine) */
    LT.fb = *fb;
    LT.serial = fb->serial;
    LT.xyo = o->xyoffset;
    LT.ct = o->ct;
    LT.zt = o->zt;
    LT.have = 1;
}

/* the frame's projection as the shader's constants (light.hlsl l_view, l_z); 0: not derivable */
static int light_proj(float view[4], float z[2]) {
    if (!LT.ent_ok) return 0;
    const float* m = LT.ent;
    const gpu_r2_frame* f = &LT.fb;
    float c3 = m[3] * m[3] + m[7] * m[7] + m[11] * m[11], l3 = sqrtf(c3);
    float a = (m[2] * m[3] + m[6] * m[7] + m[10] * m[11]) / c3;
    float b = m[14] - a * m[15];
    float fxy[2], oxy[2];
    for (int j = 0; j < 2; j++) {        /* x / y column: its share along the w column (off-centre) and the rest's length */
        float d = (m[j] * m[3] + m[4 + j] * m[7] + m[8 + j] * m[11]) / c3;
        float q = 0.0f;
        for (int k = 0; k < 3; k++) { float v = m[k * 4 + j] - d * m[k * 4 + 3]; q += v * v; }
        fxy[j] = sqrtf(q) / l3;
        oxy[j] = d;
    }
    if (!(fxy[0] > 1e-6f) || !(fxy[1] > 1e-6f) || !(fabsf(f->ax) > 1e-6f) || !(fabsf(f->ay) > 1e-6f) ||
        !(fabsf(f->az * b) > 1e-12f))
        return 0;
    /* picture pixel centre p -> GS 12.4: ((p / os) - 0.5) 16 + XYOFFSET; ndc = (GS - b) / a; view = (ndc - off) w / f */
    float xo = (float)(LT.xyo & 0xFFFFu), yo = (float)(LT.xyo >> 16);
    view[0] = 16.0f / (M.osx * f->ax) / fxy[0];
    view[1] = ((xo - 8.0f - f->bx) / f->ax - oxy[0]) / fxy[0];
    view[2] = 16.0f / (M.osy * f->ay) / fxy[1];
    view[3] = ((yo - 8.0f - f->by) / f->ay - oxy[1]) / fxy[1];
    /* GS Z = bz + az (a + b / w) = d 2^20 -> 1 / w = (d 2^20 - bz - az a) / (az b) */
    z[0] = 1048576.0f / (f->az * b);
    z[1] = -(f->bz + f->az * a) / (f->az * b);
    return 1;
}

static void lt_cb(const float* v, int n) {
    if (!LT.cb) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 9 * 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &LT.cb))) return;
    }
    (void)n;
    ID3D11DeviceContext_UpdateSubresource(gpu_rt.ctx, (ID3D11Resource*)LT.cb, 0, NULL, v, 0, 0);
}

/* one pass: target i (or rtv), pixel shader ps, sources bound by the caller */
static void lt_pass(ID3D11RenderTargetView* rtv, int w, int h, int ps, ID3D11BlendState* bs) {
    ID3D11DeviceContext_OMSetRenderTargets(gpu_rt.ctx, 1, &rtv, NULL);
    set_viewport(w, h);
    set_scissor(0, 0, w, h);
    set_blend(bs);
    set_ps(gpu_ps(ps));
    ID3D11DeviceContext_Draw(gpu_rt.ctx, 3, 0);
    gpu_rt.n_draws++;
}
static void lt_srv(int slot, ID3D11ShaderResourceView* v) { ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, (UINT)slot, 1, &v); }
static void lt_unbind(void) {
    ID3D11ShaderResourceView* none[4] = { NULL, NULL, NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 14, 4, none);
    ID3D11RenderTargetView* nr = NULL;
    ID3D11DeviceContext_OMSetRenderTargets(gpu_rt.ctx, 1, &nr, NULL);
}

/* the post pass on the frame's picture (at the resolve point; the colour target still holds the frame's 3D in its MS
 * surface) */
static void light_post(void) {
    if (!LT.have || !lt_enabled()) return;
    if (LT.serial == LT.done) { LT.skipped++; return; }
    int ct = LT.ct, zt = LT.zt;
    if (ct <= 0 || ct > MAX_TARGETS || zt <= 0 || zt > MAX_TARGETS) return;
    mtarget *c = &MT[ct], *z = &MT[zt];
    if (!c->ms || !z->ms || c->truth != MT_MS || z->truth != MT_MS || c->depth || !z->depth) return;
    float view[4], zz[2];
    int proj = light_proj(view, zz);
    if (!proj) LT.noproj++;
    int ao = LT.ao && proj;
    if (!ao && !LT.bloom) { LT.done = LT.serial; return; }
    int pc = PR.cur;
    if (!mft_need(ct, MT_PIC)) return;   /* (the resolve, as the next phase-B draw would do it) */
    LT.done = LT.serial;
    prof_mark(PR_LIGHT);
    if (lt_reim()) { reim_post(c, z, pc); return; }   /* (reimagine) Look = Reimagined: its own post pass */
    int pw = c->pw, ph = c->ph, hw = (pw + 1) / 2, hh = (ph + 1) / 2;
    if (!lt_tex(LT_W, hw, hh, DXGI_FORMAT_R32_FLOAT) || !lt_tex(LT_AO0, hw, hh, DXGI_FORMAT_R8_UNORM) ||
        !lt_tex(LT_AO1, hw, hh, DXGI_FORMAT_R8_UNORM)) { prof_mark(pc); return; }
    int bw[6], bh[6];
    bw[1] = hw; bh[1] = hh;
    for (int i = 2; i <= 5; i++) { bw[i] = (bw[i - 1] + 1) / 2; bh[i] = (bh[i - 1] + 1) / 2; }
    for (int i = 1; i <= 5; i++)
        if (!lt_tex(LT_B1 + i - 1, bw[i], bh[i], DXGI_FORMAT_R11G11B10_FLOAT)) { prof_mark(pc); return; }
    if (!LT.bs_comp) {
        D3D11_BLEND_DESC bd;
        memset(&bd, 0, sizeof bd);
        D3D11_RENDER_TARGET_BLEND_DESC* r = &bd.RenderTarget[0];
        r->BlendEnable = TRUE;
        r->SrcBlend = D3D11_BLEND_ONE; r->DestBlend = D3D11_BLEND_SRC1_COLOR; r->BlendOp = D3D11_BLEND_OP_ADD;
        r->SrcBlendAlpha = D3D11_BLEND_ZERO; r->DestBlendAlpha = D3D11_BLEND_ONE; r->BlendOpAlpha = D3D11_BLEND_OP_ADD;
        r->RenderTargetWriteMask = 7;
        if (FAILED(ID3D11Device_CreateBlendState(gpu_rt.dev, &bd, &LT.bs_comp))) { prof_mark(pc); return; }
    }
    /* constants (light.hlsl light_cb) */
    float cb[36];
    memset(cb, 0, sizeof cb);
    float* dst = cb; float* src = cb + 4; float* map = cb + 8; float* lv = cb + 12; float* lz = cb + 16;
    float* lao = cb + 20; float* lao2 = cb + 24; float* lbl = cb + 28; float* ldir = cb + 32;
    map[0] = M.rsx / M.osx; map[1] = M.rsy / M.osy; map[2] = (float)pw / (float)hw; map[3] = (float)ph / (float)hh;
    if (proj) { memcpy(lv, view, 16); lz[0] = zz[0]; lz[1] = zz[1]; }
    lz[2] = 1e6f; lz[3] = (float)LT.dbg;
    lao[0] = LT.ao_r; lao[1] = ao ? LT.ao_k : 0.0f; lao[2] = LT.ao_max; lao[3] = LT.ao_bias;
    lao2[0] = LT.ao_f0; lao2[1] = LT.ao_f1; lao2[2] = LT.ao_min; lao2[3] = LT.ao_pow;
    lbl[0] = LT.bl_t; lbl[1] = LT.bl_k; lbl[2] = LT.bloom ? LT.bl_i : 0.0f; lbl[3] = LT.bl_sat;
    ldir[2] = 1.0f / (float)pw; ldir[3] = 1.0f / (float)ph;
#define LT_SIZE(v, w, h) do { (v)[0] = (float)(w); (v)[1] = (float)(h); (v)[2] = 1.0f / (float)(w); (v)[3] = 1.0f / (float)(h); } while (0)
    if (LT.log && LT.frames % 600 == 0)
        gpu_log("light: v%u serial %u view %.6g %.6g %.6g %.6g 1/w = %.6g d + %.6g; os %.3f %.3f rs %.3f %.3f; xyo %X; screen %g %g %g %g %g %g",
                X.fd ? X.fd->vblank : 0u, LT.serial, view[0], view[1], view[2], view[3], zz[0], zz[1], M.osx, M.osy, M.rsx, M.rsy,
                LT.xyo, LT.fb.ax, LT.fb.bx, LT.fb.ay, LT.fb.by, LT.fb.az, LT.fb.bz);
    LT.frames++;
    unbind_all();
    ID3D11Buffer* b = LT.cb;
    set_vs(gpu_vs(GPU_SH_vs_light));
    set_topo(0);
    set_depth(X.ds_off);
    ID3D11SamplerState* smp = mft_sampler();
    ID3D11DeviceContext_PSSetSamplers(gpu_rt.ctx, 0, 1, &smp);
    ID3D11ShaderResourceView* none[4] = { NULL, NULL, NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 14, 4, none);
    ID3D11BlendState* off = blend_state(BL_OFF, 15);
    int ms = z->n > 1 && !z->alias;
    if (ao) {
        LT_SIZE(dst, hw, hh); LT_SIZE(src, hw, hh);
        lt_cb(cb, 36);
        if (!b) b = LT.cb;
        ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 7, 1, &b);
        lt_srv(17, z->srv);
        lt_pass(LT.rtv[LT_W], hw, hh, ms ? GPU_SH_ps_light_lin1 : GPU_SH_ps_light_lin0, off);
        lt_unbind();
        lt_srv(14, LT.srv[LT_W]);
        lt_pass(LT.rtv[LT_AO0], hw, hh, GPU_SH_ps_light_ao, off);
        lt_unbind();
        ldir[0] = 1.0f; ldir[1] = 0.0f;
        lt_cb(cb, 36);
        lt_srv(14, LT.srv[LT_W]); lt_srv(15, LT.srv[LT_AO0]);
        lt_pass(LT.rtv[LT_AO1], hw, hh, GPU_SH_ps_light_blur, off);
        lt_unbind();
        ldir[0] = 0.0f; ldir[1] = 1.0f;
        lt_cb(cb, 36);
        lt_srv(14, LT.srv[LT_W]); lt_srv(15, LT.srv[LT_AO1]);
        lt_pass(LT.rtv[LT_AO0], hw, hh, GPU_SH_ps_light_blur, off);
        lt_unbind();
        ldir[0] = ldir[1] = 0.0f;
    }
    if (LT.bloom) {
        LT_SIZE(dst, hw, hh); LT_SIZE(src, pw, ph);
        lt_cb(cb, 36);
        if (!b) b = LT.cb;
        ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 7, 1, &b);
        lt_srv(16, c->psrv);
        lt_pass(LT.rtv[LT_B1], hw, hh, GPU_SH_ps_light_bright, off);
        lt_unbind();
        for (int i = 2; i <= 5; i++) {
            LT_SIZE(dst, bw[i], bh[i]); LT_SIZE(src, bw[i - 1], bh[i - 1]);
            lt_cb(cb, 36);
            lt_srv(16, LT.srv[LT_B1 + i - 2]);
            lt_pass(LT.rtv[LT_B1 + i - 1], bw[i], bh[i], GPU_SH_ps_light_down, off);
            lt_unbind();
        }
        ID3D11BlendState* add = blend_state(BL_ADD, 15);
        for (int i = 4; i >= 1; i--) {
            LT_SIZE(dst, bw[i], bh[i]); LT_SIZE(src, bw[i + 1], bh[i + 1]);
            lt_cb(cb, 36);
            lt_srv(16, LT.srv[LT_B1 + i]);
            lt_pass(LT.rtv[LT_B1 + i - 1], bw[i], bh[i], GPU_SH_ps_light_up, add);
            lt_unbind();
        }
    }
    /* composite: picture x AO + bloom (RGB) */
    LT_SIZE(dst, pw, ph); LT_SIZE(src, hw, hh);
    lt_cb(cb, 36);
    ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 7, 1, &b);
    if (ao) { lt_srv(14, LT.srv[LT_W]); lt_srv(15, LT.srv[LT_AO0]); lt_srv(17, z->srv); }
    if (LT.bloom) lt_srv(16, LT.srv[LT_B1]);
    if (LT.split) {                      /* (PS2_LIGHT_SPLIT=1: the left half stays Original) */
        ID3D11DeviceContext_OMSetRenderTargets(gpu_rt.ctx, 1, &c->prtv, NULL);
        set_viewport(pw, ph);
        set_scissor(pw / 2, 0, pw, ph);
        set_blend(LT.bs_comp);
        set_ps(gpu_ps(ms ? GPU_SH_ps_light_comp1 : GPU_SH_ps_light_comp0));
        ID3D11DeviceContext_Draw(gpu_rt.ctx, 3, 0);
    } else
    lt_pass(c->prtv, pw, ph, ms ? GPU_SH_ps_light_comp1 : GPU_SH_ps_light_comp0, LT.bs_comp);
    lt_unbind();
#undef LT_SIZE
    /* the executor's bindings are stale now */
    X.mode = 0;
    X.b_ct = X.b_zt = -1;
    M.bound = 0;
    M.cb_phase = -1;
    ID3D11Buffer* mb = M.cb;
    ID3D11DeviceContext_VSSetConstantBuffers(gpu_rt.ctx, 6, 1, &mb);
    ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 6, 1, &mb);
    prof_mark(pc);
}
