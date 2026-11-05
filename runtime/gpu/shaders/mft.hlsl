// (r2-m8, R8) Modern frame targets ("MFT", gpu_exec_mft.h; notes/modern-renderer.md "R8 as built"): a modern gameplay
// frame drawn on surfaces of any resolution with MSAA instead of the exact path's integer sample grid. A GS pixel x covers
// d3d pixels [x s, (x + 1) s) of a surface (pixel centres symmetric: d3d = (GS + 0.5) * s), s = surface pixels per GS
// pixel (float, per axis). Every draw keeps the exact path's own per-fragment arithmetic (gs.hlsl: texture with the
// 4-bit bilinear, CLUTs, TFX, fog, alpha test, the FF output stage's blend terms), but positions are rasterized at the
// surface's resolution and depth is the hardware's (the GS Z / 2^20 at the vertices, interpolated: MSAA keeps it per
// sample):
//   triangles     attributes from the frontend's triangle record evaluated at the fragment's GS position (the GS's own
//                 planes: Gouraud / flat colour, fog, FST U / V), S/T/Q from the vertices (hardware, as the FF fast path)
//   sprites,      single-rate: every fragment takes its native pixel's value (gs.hlsl gs_frag at the native pixel), as
//   line pixels   the exact path's storage-aligned sprites; a line pixel is a native pixel's square (PS2 outlines)
//   thin lines    (outlines = thin) coverage from the fragment's distance to the line, in GS units
//   volumes       the shadow pipeline's G counter per sample (SV_Coverage after the early depth test), then one
//                 per-sample pass with the exact path's fused arithmetic (postfn.hlsli, gpu_exec.c shfuse)
#define FF 1
#define SPEC_NOVIEW 1
#ifndef PRIM
#define PRIM 0
#endif
#if PRIM == 0
#define STQ_FAST 1
#define DC_VTX 1
#endif
#ifndef MS
#define MS 1                             // the colour / depth surfaces are multisampled (Texture2DMS views)
#endif
#ifndef SVOL_LITE
#define SVOL_LITE 0                      // (r8-perf) volumes: flat colour and TEST / ALPHA straight from the record and state
#endif
#include "gs.hlsl"
#include "postfn.hlsli"

cbuffer mft_cb : register(b6) {
    float4 m_s;                          // sx, sy (surface pixels per GS pixel), 1 / sx, 1 / sy
    uint4 m_cfg;                         // samples; flags (1: blend class override in bits 8..11, 2: DATE against m_date);
                                         // primitive base (sub-draws of ROV draws); ATST half override (1 pass, 2 fail)
    float4 m_rect;                       // quads: d3d rect x0, y0, x1, y1
    float4 m_val;                        // fills: colour (0..1) / depth (x)
    float4 m_src;                        // resampling: source position = (d3d pixel centre) * (x, y) + (z, w), source pixels
    float4 m_src2;                       // resampling: source size w, h, 1 / w, 1 / h
};

// (ROV draws drawn approximately on the surfaces: per sub-draw overrides of the draw's constants)
void mft_cfg_override() {
    if ((m_cfg.y & 1u) != 0u) ff_blend = (m_cfg.y >> 8) & 15u;
    if (m_cfg.w != 0u) atst_mode = m_cfg.w;
}
// (DATE, after the resolve point only: the destination as it was before the sub-draw, a copy)
Texture2D<uint4> m_date : register(t19);
bool mft_date_fails(int2 P, uint test) {
    if ((m_cfg.y & 2u) == 0u || ((test >> 14) & 1u) == 0u) return false;
    return (m_date.Load(int3(P, 0)).w >> 7) != ((test >> 15) & 1u);
}

float4 mft_ndc(float2 d3d, float z) { return float4(d3d.x * (1.0 / 8192.0), -d3d.y * (1.0 / 8192.0), z, 1.0); }   // (REF viewport)

// ---- triangles -------------------------------------------------------------------------------------------------------
struct VM {
    float4 pos : SV_Position;
    noperspective float3 stq : TEXCOORD0;
    nointerpolation uint4 dc : DC;       // rec_first, -, ff_blend | atst_mode << 8 | draw_flags << 16, dual_bias
    nointerpolation uint sidx : SIDX;
};

VM vs_mft_tri(VIn i) {
    load_draw(i.did);
    VM o;
    // (the vertex Z: gpu_front.c emit_vertex, bit 31 = no early-depth bound)
    o.pos = mft_ndc(float2((i.xy.x + 0.5) * m_s.x, (i.xy.y + 0.5) * m_s.y), gs_z_encode(i.zv & 0x7FFFFFFFu));
    o.stq = i.stq;
    o.dc = uint4(rec_first, 0u, ff_blend | (atst_mode << 8) | (draw_flags << 16), asuint(dual_bias));
    o.sidx = i.fog_state >> 8;
    return o;
}

void mft_statics(uint4 dc, uint sidx) {
    rec_first = dc.x; ff_blend = dc.z & 0xFFu; atst_mode = (dc.z >> 8) & 0xFFu; draw_flags = dc.z >> 16;
    dual_bias = asfloat(dc.w);
    g_vstate = sidx;
    kx = 1u; ky = 1u; vkx = 1u; vky = 1u; vbase = 0u; vfbw = 1u; vflags = 0u; one = 1.0; probe_x = probe_y = 0xFFFFFFFFu;
}

// the fragment at d3d pixel position pos: the triangle record's planes at its GS position (gs.hlsl gs_frag, triangle
// part, at a fractional offset (fi, fj) from native pixel (px, py) instead of a sample of the integer grid)
bool mft_tri_frag(float4 pos, float3 istq, uint prim, uint sidx, out Frag f, out St st) {
    float gx = pos.x * m_s.z - 0.5, gy = pos.y * m_s.w - 0.5;
    int px = (int)floor(gx), py = (int)floor(gy);
    float fi = gx - (float)px, fj = gy - (float)py;
    f.P = int2(pos.xy);
    f.px = px; f.py = py;
    f.U = 0; f.V = 0; f.lq = 1.0; f.uf = 0.0; f.vf = 0.0; f.z = 0u;
    uint rb = rec_first + prim * TRI_REC;
    f.state = sidx;
    f.rb = rb;
    st = load_state(f.state);
    uint scanmsk = (st.flags >> 14) & 3u;
    if ((scanmsk == 2u && (py & 1) == 0) || (scanmsk == 3u && (py & 1) != 0)) { f.col = 0u; f.vcol = 0u; f.fog = 0u; return false; }
    bool tme = !SPEC_NOTEX && (st.flags & SF_TME) != 0u, fst = (st.flags & SF_FST) != 0u;
    int by0 = (int)(recs.Load(rb + 108u) << 16) >> 16;
    int rows = py - by0;
    int a[7];
    uint nk = tme && fst ? 7u : (st.flags & SF_FGE) != 0u ? 5u : 4u;
    // (r8-perf) the record's rows / steps in wide loads (the same words)
    uint4 r0 = recs.Load4(rb + 24u), y0 = recs.Load4(rb + 52u), x0 = recs.Load4(rb + 80u);
    uint3 r1 = recs.Load3(rb + 40u), y1 = recs.Load3(rb + 68u), x1 = recs.Load3(rb + 96u);
    uint rw[7] = { r0.x, r0.y, r0.z, r0.w, r1.x, r1.y, r1.z };
    uint dyw[7] = { y0.x, y0.y, y0.z, y0.w, y1.x, y1.y, y1.z };
    uint dxw[7] = { x0.x, x0.y, x0.z, x0.w, x1.x, x1.y, x1.z };
    [unroll] for (uint k = 0u; k < 7u; k++) {
        a[k] = 0;
        if (k < nk) {
            uint dyk = dyw[k], dxk = dxw[k];
            uint v = rw[k] + dyk * (uint)rows + dxk * (uint)px;
            v += (uint)(int)floor((float)asint(dyk) * fj);
            v += (uint)(int)floor((float)asint(dxk) * fi);
            a[k] = asint(v);
        }
    }
    f.col = clamp255i(a[0] >> 16) | (clamp255i(a[1] >> 16) << 8) | (clamp255i(a[2] >> 16) << 16) | (clamp255i(a[3] >> 16) << 24);
    f.fog = clamp255i(a[4] >> 16);
    f.lq = asfloat(recs.Load(rb + 116u));
    if (tme) {
        if (fst) { f.U = a[5] >> 12; f.V = a[6] >> 12; }
        else {
            uint t0 = st.tex0clamp.x, t1 = st.tex0clamp.y;
            uint tw = (t0 >> 26) & 15u, th = ((t0 >> 30) | (t1 << 2)) & 15u;
            float tw16 = (float)(16u << (tw > 10u ? 10u : tw)), th16 = (float)(16u << (th > 10u ? 10u : th));
            float iq = 1.0 / istq.z;
            precise float su = istq.x * iq;
            precise float tv = istq.y * iq;
            precise float su16 = su * tw16;
            precise float tv16 = tv * th16;
            f.U = f2i(su16);
            f.V = f2i(tv16);
            f.lq = istq.z;
        }
    }
    f.vcol = f.col;
    uint col = f.col;
    if (tme && (st.tlev & 3u) != 0u) {
        uint t = tex_sample(st, f.U, f.V, f.lq);
        col = tfx((st.flags >> 2) & 3u, (st.flags & SF_TCC) != 0u, col, t);
    }
    if (st.flags & SF_FGE) {
        uint fv = f.fog, fc = st.fogcol;
        uint r = (fv * (col & 255u) + (255u - fv) * (fc & 255u)) >> 8;
        uint g = (fv * ((col >> 8) & 255u) + (255u - fv) * ((fc >> 8) & 255u)) >> 8;
        uint b = (fv * ((col >> 16) & 255u) + (255u - fv) * ((fc >> 16) & 255u)) >> 8;
        col = r | (g << 8) | (b << 16) | (col & 0xFF000000u);
    }
    f.col = col;
    return true;
}

#if PRIM == 0
FOut ps_mft_tri(VM i, uint prim : SV_PrimitiveID) {
    mft_statics(i.dc, i.sidx);
    mft_cfg_override();
    Frag f;
    St st;
    if (((m_cfg.y >> 16) & 8u) != 0u) {  // (r8-perf cost experiment, PS2_R8P_SKIP: a constant colour, no fragment evaluation)
        st = load_state(i.sidx);
        f.col = 0x80404040u; f.z = 0u; f.P = int2(i.pos.xy); f.px = 0; f.py = 0; f.U = 0; f.V = 0; f.lq = 1.0; f.uf = 0.0;
        f.vf = 0.0; f.fog = 0u; f.vcol = f.col; f.state = i.sidx; f.rb = 0u;
        return ff_out(f, st);
    }
    if (!mft_tri_frag(i.pos, i.stq, prim + m_cfg.z, i.sidx, f, st)) discard;
    if (mft_date_fails(int2(i.pos.xy), st.test)) discard;
    return ff_out(f, st);
}

// ---- the shadow volumes (gs.hlsl ps_gs SVOL): each covered sample that passes the early depth test and DATE adds the
// primitive's G step into its counter (sample x * samples + s, 16-bit lanes; one band of rows per epoch: the volumes
// between two of the shadow pipeline's flat passes); the fused pass below adds them into G in order --------------------
RWTexture2D<uint> m_cnt : register(u1);
#if MS
Texture2DMS<uint4> m_col : register(t12);
uint mft_col(uint2 P, uint s) { uint4 c = m_col.Load(int2(P), s); return c.x | (c.y << 8) | (c.z << 16) | (c.w << 24); }
#else
Texture2D<uint4> m_col : register(t12);
uint mft_col(uint2 P, uint s) { uint4 c = m_col.Load(int3(P, 0)); return c.x | (c.y << 8) | (c.z << 16) | (c.w << 24); }
#endif

[earlydepthstencil]
void ps_mft_svol(VM i, uint prim : SV_PrimitiveID, uint cov : SV_Coverage) {
    mft_statics(i.dc, i.sidx);
    Frag f;
    St st;
    uint dbg = m_cfg.y >> 16;            // (r8-perf cost experiments, PS2_R8P_SKIP: 1 no atomics, 2 no record, 4 no DATE loads)
    if (cov == 0u) return;
#if SVOL_LITE
    // (r8-perf) a volume primitive is flat (gpu_front.c svol: no IIP, no texture, no fog, no SCANMSK): where its record's G
    // and A planes have no gradients, every fragment's colour is the planes' row value (mft_tri_frag adds only the
    // gradients' terms), and the tests read only TEST / ALPHA of its state: no per-fragment plane evaluation
    uint rbl = rec_first + prim * TRI_REC;   // (as mft_tri_frag: m_cfg.z is the epoch's counter row here)
    uint4 rw = recs.Load4(rbl + 24u), gy = recs.Load4(rbl + 52u), gx = recs.Load4(rbl + 80u);
    if ((gy.y | gy.w | gx.y | gx.w) == 0u && (dbg & 2u) == 0u) {
        f.col = (clamp255i(asint(rw.y) >> 16) << 8) | (clamp255i(asint(rw.w) >> 16) << 24);
        uint sb = i.sidx * STATE_BYTES;
        st.test = states.Load(sb + 40u);
        st.alpha = states.Load(sb + 52u);
    } else
#endif
    if ((dbg & 2u) != 0u) {
        f.col = 0x80008000u; st.test = 0u; st.alpha = 0x8008u | (2u << 4);
    } else
    if (!mft_tri_frag(i.pos, i.stq, prim, i.sidx, f, st)) return;
    uint col = f.col, As = col >> 24, test = st.test;
    if ((dbg & 4u) != 0u) test &= ~(1u << 14);
    if ((test & 1u) && !atst_pass((test >> 1) & 7u, (test >> 4) & 0xFFu, As)) {
        uint afail = (test >> 12) & 3u;
        if (afail == 0u || afail == 2u) return;   // KEEP, ZB_ONLY: no colour
    }
    bool date = ((test >> 14) & 1u) != 0u;
    uint datm = (test >> 15) & 1u;
    uint al = st.alpha;
    uint asel = al & 3u, csel = (al >> 4) & 3u, fix = (al >> 8) & 255u;
    int cs = (int)((col >> 8) & 255u);
    uint ns = m_cfg.x;
    uint2 P = uint2(i.pos.xy);
    // (two samples per word, 16-bit lanes: a lane overflows into the next only after 257 adds of up to 255 - far more
    //  volumes than overlap anywhere; the fused pass takes the low byte. m_cfg.z: the epoch's first row. The adds of the
    //  samples sharing a word are one atomic)
    uint acc = 0u, word = 0xFFFFFFFFu;
    [loop] for (uint s = 0u; s < ns; s++) {
        uint fl = P.x * ns + s;
        if ((fl >> 1) != word) {
            if (acc != 0u && (dbg & 1u) == 0u) InterlockedAdd(m_cnt[uint2(word, P.y + m_cfg.z)], acc);
            acc = 0u;
            word = fl >> 1;
        }
        if (((cov >> s) & 1u) == 0u) continue;
        uint dst = (date || csel == 1u) ? mft_col(P, s) : 0u;
        if (date && (dst >> 31) != datm) continue;
        int cc = csel == 0u ? (int)As : csel == 1u ? (int)(dst >> 24) : (int)fix;
        uint d = (uint)(((asel == 0u ? cs : -cs) * cc) >> 7);
        acc += (d & 255u) << ((fl & 1u) * 16u);
    }
    if ((dbg & 1u) != 0u) return;
    if (acc != 0u) InterlockedAdd(m_cnt[uint2(word, P.y + m_cfg.z)], acc);
}
#endif

// ---- sprites and line / point pixels: the native pixel's value (single rate) ---------------------------------------
struct VQ {
    float4 pos : SV_Position;
    nointerpolation uint rec : REC;
    nointerpolation uint did : DID;
};

#if PRIM == 1
VQ vs_mft_sprite(VIn vin, uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    uint did = vin.did - inst;
    load_draw(did);
    uint base = rec_first + (inst + m_cfg.z) * 64u;
    uint2 r = recs.Load2(base);
    float x0 = (float)((int)(r.x << 16) >> 16), y0 = (float)((int)r.x >> 16);
    float x1 = (float)((int)(r.y << 16) >> 16), y1 = (float)((int)r.y >> 16);
    VQ o;
    o.pos = mft_ndc(float2(((vid & 1u) ? x1 : x0) * m_s.x, ((vid & 2u) ? y1 : y0) * m_s.y), gs_z_encode(recs.Load(base + 12u)));
    o.rec = base;
    o.did = did;
    return o;
}
#endif
#if PRIM == 2
VQ vs_mft_linepix(VIn vin, uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    uint did = vin.did - inst;
    load_draw(did);
    uint base = rec_first + (inst + m_cfg.z) * 32u;
    uint r = recs.Load(base);
    float px = (float)((int)(r << 16) >> 16), py = (float)((int)r >> 16);
    VQ o;
    o.pos = mft_ndc(float2((px + ((vid & 1u) ? 1.0 : 0.0)) * m_s.x, (py + ((vid & 2u) ? 1.0 : 0.0)) * m_s.y),
                    gs_z_encode(recs.Load(base + 4u)));
    o.rec = base;
    o.did = did;
    return o;
}
#endif
#if PRIM == 2
// (r8-perf) a merged draw of a line-pixel run (gpu_exec_mft.h lm_plan): instance k's entry at m_cfg.z + k of the table holds
// its draw's slot relative to the run's first (StartInstanceLocation) and its instance in that draw
ByteAddressBuffer m_lmtab : register(t26);
VQ vs_mft_linepix_m(VIn vin, uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    uint2 e = m_lmtab.Load2((m_cfg.z + inst) * 8u);
    uint did = vin.did - inst + e.x;
    load_draw(did);
    uint base = rec_first + e.y * 32u;
    uint r = recs.Load(base);
    float px = (float)((int)(r << 16) >> 16), py = (float)((int)r >> 16);
    VQ o;
    o.pos = mft_ndc(float2((px + ((vid & 1u) ? 1.0 : 0.0)) * m_s.x, (py + ((vid & 2u) ? 1.0 : 0.0)) * m_s.y),
                    gs_z_encode(recs.Load(base + 4u)));
    o.rec = base;
    o.did = did;
    return o;
}
#endif
#if PRIM == 1 || PRIM == 2
FOut ps_mft_q(VQ i) {
    load_draw(i.did);
    mft_cfg_override();
    kx = 1u; ky = 1u;
    float2 g = floor(i.pos.xy * m_s.zw);            // the native pixel (its d3d rect holds the fragment's centre)
    Frag f;
    St st;
    if (!gs_frag(float4(g + 0.5, 0.0, 1.0), i.rec, float3(0.0, 0.0, 1.0), 0u, f, st)) discard;
    if (mft_date_fails(int2(i.pos.xy), st.test)) discard;
    return ff_out(f, st);
}
#endif

// ---- thin analytic lines (gs.hlsl PRIM 3 at the fragment's GS position) ---------------------------------------------
#if PRIM == 3
VQ vs_mft_thin(VIn vin, uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    uint did = vin.did - inst;
    load_draw(did);
    uint base = rec_first + (inst + m_cfg.z) * 128u;
    float4 g = asfloat(recs.Load4(base));            // lo, hi, a0, len (12.4)
    float2 o = asfloat(recs.Load2(base + 16u));      // o0, od (12.4)
    uint flags = recs.Load(base + 24u);
    float w = asfloat(recs.Load(base + 108u));
    bool xm = (flags & 1u) != 0u;
    float km = xm ? m_s.x : m_s.y, kn = xm ? m_s.y : m_s.x;
    float m = ((vid & 1u) ? g.y / 16.0 + 1.0 / km : g.x / 16.0 - 1.0 / km);
    float t = (m * 16.0 - g.z) / g.w;
    float h = w + 1.0 / kn;
    float n = (o.x + o.y * t) / 16.0 + ((vid & 2u) ? h : -h);
    float2 p = xm ? float2(m, n) : float2(n, m);
    float2 zz = asfloat(recs.Load2(base + 32u));
    float tc = clamp(t, (g.x - g.z) / g.w, (g.y - g.z) / g.w);
    VQ r;
    r.pos = mft_ndc(float2((p.x + 0.5) * m_s.x, (p.y + 0.5) * m_s.y), clamp(floor(zz.x + zz.y * tc), 0.0, 1048576.0) * (1.0 / 1048576.0));
    r.rec = base;
    r.did = did;
    return r;
}

FOut ps_mft_thin(VQ i) {
    load_draw(i.did);
    mft_cfg_override();
    kx = 1u; ky = 1u;
    uint rb = i.rec;
    Frag f;
    f.state = recs.Load(rb + 28u);
    f.rb = rb;
    St st = load_state(f.state);
    f.P = int2(i.pos.xy);
    float gx = i.pos.x * m_s.z - 0.5, gy = i.pos.y * m_s.w - 0.5;   // the fragment's GS position
    f.px = (int)floor(gx + 0.5); f.py = (int)floor(gy + 0.5);
    f.U = 0; f.V = 0; f.uf = 0.0; f.vf = 0.0; f.lq = 1.0; f.z = 0u;
    bool tme = (st.flags & SF_TME) != 0u, fst = (st.flags & SF_FST) != 0u;
    float4 g = asfloat(recs.Load4(rb));
    float2 o = asfloat(recs.Load2(rb + 16u));
    uint lf = recs.Load(rb + 24u);
    bool xm = (lf & 1u) != 0u;
    float mj = (xm ? gx : gy) * 16.0, mn = (xm ? gy : gx) * 16.0;
    if (mj < g.x || mj >= g.y) discard;
    float t = (mj - g.z) / g.w;
    float d = (mn - (o.x + o.y * t)) / 16.0;
    float w = asfloat(recs.Load(rb + 108u));
    uint cov = 0x80u;
    if ((lf & 2u) != 0u) {
        float c = 1.0 - abs(d) / w;
        if (c <= 0.0) discard;
        cov = (uint)round(c * 128.0);
        if (cov == 0u) discard;
    } else if (abs(d) >= w * 0.5) discard;
    float4 c0 = asfloat(recs.Load4(rb + 40u)), cx = asfloat(recs.Load4(rb + 56u));
    float4 cv = floor(c0 + cx * t);
    uint ca = (lf & 2u) != 0u ? cov : clamp255i((int)cv.w);
    f.col = clamp255i((int)cv.x) | (clamp255i((int)cv.y) << 8) | (clamp255i((int)cv.z) << 16) | (ca << 24);
    float2 ff = asfloat(recs.Load2(rb + 72u));
    f.fog = clamp255i((int)floor(ff.x + ff.y * t));
    f.lq = asfloat(recs.Load(rb + 104u));
    if (tme) {
        uint t0 = st.tex0clamp.x, t1 = st.tex0clamp.y;
        uint tw = (t0 >> 26) & 15u, th = ((t0 >> 30) | (t1 << 2)) & 15u;
        float tw16 = (float)(16u << (tw > 10u ? 10u : tw)), th16 = (float)(16u << (th > 10u ? 10u : th));
        float4 s01 = asfloat(recs.Load4(rb + 80u));
        float2 s23 = asfloat(recs.Load2(rb + 96u));
        float s = s01.x + s01.w * t, tt = s01.y + s23.x * t;
        if (fst) { f.U = (int)floor(s); f.V = (int)floor(tt); }
        else {
            float q = s01.z + s23.y * t, iq = 1.0 / q;
            precise float su = s * iq;
            precise float tv = tt * iq;
            precise float su16 = su * tw16;
            precise float tv16 = tv * th16;
            f.U = f2i(su16);
            f.V = f2i(tv16);
            f.lq = q;
        }
    }
    f.vcol = f.col;
    uint col = f.col;
    if (tme && (st.tlev & 3u) != 0u) col = tfx((st.flags >> 2) & 3u, (st.flags & SF_TCC) != 0u, col, tex_sample(st, f.U, f.V, f.lq));
    if (st.flags & SF_FGE) {
        uint fv = f.fog, fc = st.fogcol;
        uint r = (fv * (col & 255u) + (255u - fv) * (fc & 255u)) >> 8;
        uint gg = (fv * ((col >> 8) & 255u) + (255u - fv) * ((fc >> 8) & 255u)) >> 8;
        uint b = (fv * ((col >> 16) & 255u) + (255u - fv) * ((fc >> 16) & 255u)) >> 8;
        col = r | (gg << 8) | (b << 16) | (col & 0xFF000000u);
    }
    f.col = col;
    if (mft_date_fails(int2(i.pos.xy), st.test)) discard;
    return ff_out(f, st);
}
#endif

// ---- passes over a rectangle (PRIM 9: the utility permutations) -----------------------------------------------------
#if PRIM == 9
float4 vs_mft_quad(uint vid : SV_VertexID) : SV_Position {
    return mft_ndc(float2((vid & 1u) ? m_rect.z : m_rect.x, (vid & 2u) ? m_rect.w : m_rect.y), 0.0);
}
float4 ps_mft_fill(float4 pos : SV_Position) : SV_Target { return m_val; }
float ps_mft_zfill(float4 pos : SV_Position) : SV_Depth { return m_val.x; }

Texture2D<float4> m_tex : register(t18);
SamplerState m_lin : register(s0);
// colour from another surface: bilinear at the source position of the pixel centre (grid <-> MFT, render scale)
float4 ps_mft_copy(float4 pos : SV_Position) : SV_Target {
    float2 p = pos.xy * m_src.xy + m_src.zw;
    return m_tex.SampleLevel(m_lin, p * m_src2.zw, 0.0);
}
// the upscale of a resolved picture (render scale < 100%): Catmull-Rom in 9 bilinear taps (the well-known separable
// bicubic through the hardware filter), clamped to the 2 x 2 neighbourhood's range (no ringing on the HUD's edges)
float4 ps_mft_upscale(float4 pos : SV_Position) : SV_Target {
    float2 p = pos.xy * m_src.xy + m_src.zw;            // source pixels (centres at + 0.5)
    float2 tc = p - 0.5, f0 = floor(tc), f = tc - f0;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2, o12 = w2 / w12;
    float2 t0 = (f0 - 0.5) * m_src2.zw, t3 = (f0 + 2.5) * m_src2.zw, t12 = (f0 + 0.5 + o12) * m_src2.zw;
    float4 r = m_tex.SampleLevel(m_lin, float2(t0.x, t0.y), 0.0) * w0.x * w0.y
             + m_tex.SampleLevel(m_lin, float2(t12.x, t0.y), 0.0) * w12.x * w0.y
             + m_tex.SampleLevel(m_lin, float2(t3.x, t0.y), 0.0) * w3.x * w0.y
             + m_tex.SampleLevel(m_lin, float2(t0.x, t12.y), 0.0) * w0.x * w12.y
             + m_tex.SampleLevel(m_lin, float2(t12.x, t12.y), 0.0) * w12.x * w12.y
             + m_tex.SampleLevel(m_lin, float2(t3.x, t12.y), 0.0) * w3.x * w12.y
             + m_tex.SampleLevel(m_lin, float2(t0.x, t3.y), 0.0) * w0.x * w3.y
             + m_tex.SampleLevel(m_lin, float2(t12.x, t3.y), 0.0) * w12.x * w3.y
             + m_tex.SampleLevel(m_lin, float2(t3.x, t3.y), 0.0) * w3.x * w3.y;
    float2 c = (f0 + 0.5) * m_src2.zw;
    float4 a = m_tex.SampleLevel(m_lin, c, 0.0), b = m_tex.SampleLevel(m_lin, c + float2(m_src2.z, 0.0), 0.0);
    float4 d = m_tex.SampleLevel(m_lin, c + float2(0.0, m_src2.w), 0.0), e = m_tex.SampleLevel(m_lin, c + m_src2.zw, 0.0);
    return clamp(r, min(min(a, b), min(d, e)), max(max(a, b), max(d, e)));
}
// (diagnostics, PS2_MFT_MV_VIEW=1) the motion vectors over the picture: 0.5 grey = none, red / green = x / y (16 px per 0.5)
float4 ps_mft_mvview(float4 pos : SV_Position) : SV_Target {
    float2 p = pos.xy * m_src.xy + m_src.zw;
    float2 mv = m_tex.Load(int3(clamp(int2(p), int2(0, 0), int2(m_src2.xy) - 1), 0)).xy;
    return float4(saturate(0.5 + mv / 32.0), 0.5, 1.0);
}
// depth from the exact grid's depth target (its D32 SRV, z / 2^20) into a surface: the nearest grid sample
Texture2D<float> m_zgrid : register(t17);
float ps_mft_zfrom(float4 pos : SV_Position) : SV_Depth {
    float2 p = pos.xy * m_src.xy + m_src.zw;
    int2 q = clamp(int2(p), int2(0, 0), int2(m_src2.xy) - 1);
    return m_zgrid.Load(int3(q, 0));
}
// depth from a surface (sample 0) into the exact grid's depth target
#if MS
Texture2DMS<float> m_zsurf : register(t16);
float mft_zsurf(int2 q) { return m_zsurf.Load(q, 0); }
#else
Texture2D<float> m_zsurf : register(t16);
float mft_zsurf(int2 q) { return m_zsurf.Load(int3(q, 0)); }
#endif
float ps_mft_zto(float4 pos : SV_Position) : SV_Depth {
    float2 p = pos.xy * m_src.xy + m_src.zw;
    int2 q = clamp(int2(p), int2(0, 0), int2(m_src2.xy) - 1);
    return mft_zsurf(q);
}

// the shadow pipeline's fused pass (post.hlsl cs_shfuse) per sample of the frame's surface: B' = f(B, counters), written
// into the other surface of the pair (ping-pong). In the game's order: copy 0 (the deferred shuffle 1, into A), the G
// clear, then per epoch the volumes' counter into G and the flat pass that ends the epoch (gpu_exec_mft.h), then the
// tail's copies (shuffles 2 + 3) and the darken. The counters are cleared by the executor afterwards.
cbuffer mshf_cb : register(b5) {
    uint4 sf_svbb;                       // counter samples (surface pixels) [x0, x1) x [y0, y1) (x1 = 0: no counter)
    uint4 sf_c[3];                       // copy draws (post.hlsl shf_cb)
    uint4 sf_clr;                        // G clear of B after copy 0: on, value, byte mask
    uint4 sf_np;                         // the darken: mode, alpha, fmask, vertex colour
    uint4 sf_np2;                        // CLUT word, on, -, -
    uint4 sf_ep;                         // epochs (flat passes + 1), counter rows per epoch, -, -
    uint4 sf_flat[3];                    // the flat passes ending epochs 0..2: mode, alpha, fmask, vertex colour
};
Texture2D<uint> m_cntr : register(t14);
#if MS
Texture2DMS<uint4> m_col : register(t12);
uint mft_col(uint2 P, uint s) { uint4 c = m_col.Load(int2(P), s); return c.x | (c.y << 8) | (c.z << 16) | (c.w << 24); }
#else
Texture2D<uint4> m_col : register(t12);
uint mft_col(uint2 P, uint s) { uint4 c = m_col.Load(int3(P, 0)); return c.x | (c.y << 8) | (c.z << 16) | (c.w << 24); }
#endif
// sample s's counter of epoch e (0 outside the volumes' box)
uint mft_cnt_at(uint2 P, uint s, uint e) {
    bool inb = P.x >= sf_svbb.x && P.x < sf_svbb.z && P.y >= sf_svbb.y && P.y < sf_svbb.w;
    uint fl = P.x * m_cfg.x + s;
    uint n = 0u;
    if (inb) n = (m_cntr.Load(int3(fl >> 1, P.y + e * sf_ep.y, 0)) >> ((fl & 1u) * 16u)) & 255u;
    return n;
}
uint mft_shfuse_word(uint2 P, uint s, uint w) {
    uint a = 0u;                         // (the other frame's word: only bits copy 0 wrote are read, gpu_exec_mft.h checks)
    [unroll] for (uint k = 0u; k < 3u; k++) {
        uint4 c = sf_c[k];
        if ((c.w & (1u << 19)) != 0u) {
            uint src = (c.w & (1u << 18)) != 0u ? a : w;
            if ((c.w & (1u << 20)) != 0u) a = sf_copy(c, src, a);
            else w = sf_copy(c, src, w);
        }
        if (k == 0u) {
            if (sf_clr.x != 0u) w = (w & ~sf_clr.z) | (sf_clr.y & sf_clr.z);
            [loop] for (uint e = 0u; e < sf_ep.x; e++) {
                uint n = mft_cnt_at(P, s, e);
                w = (w & 0xFFFF00FFu) | ((((w >> 8) + n) & 255u) << 8);
                if (e + 1u < sf_ep.x) { uint4 q = sf_flat[e]; w = npost_word(w, w, q.x, q.y, q.z, q.w, 0u); }
            }
        }
    }
    if (sf_np2.y != 0u) w = npost_word(w, w, sf_np.x, sf_np.y, sf_np.z, sf_np.w, sf_np2.x);
    return w;
}
float4 mft_unorm(uint w) {
    return float4((float)(w & 255u), (float)((w >> 8) & 255u), (float)((w >> 16) & 255u), (float)(w >> 24)) * (1.0 / 255.0);
}
#ifndef SOFT
#define SOFT 0
#endif
#if SOFT
// (light) Enhanced lighting: soft shadow edges (gpu_light.h; sf_ep.z = the penumbra radius in surface pixels, float bits).
// The game's shadow is the fused pass's word with the volumes' counters; a pixel's word with the counters of a pixel
// outside every volume is the same pixel unshadowed, with those of one inside it shadowed. The share of a disc of samples
// around the pixel that lies inside the volumes blends the two (RGB; the alpha byte - read by later destination-alpha
// tests - stays the game's), so the hard edge becomes a penumbra about twice the radius wide.
bool mft_soft_in(uint2 Q, uint s) {
    bool any = false;
    [loop] for (uint e = 0u; e < sf_ep.x; e++) any = any || mft_cnt_at(Q, s, e) != 0u;
    return any;
}
uint mft_shfuse_word_q(uint2 P, uint2 Q, uint s, uint w) {   // mft_shfuse_word with the counters of pixel Q
    uint a = 0u;
    [unroll] for (uint k = 0u; k < 3u; k++) {
        uint4 c = sf_c[k];
        if ((c.w & (1u << 19)) != 0u) {
            uint src = (c.w & (1u << 18)) != 0u ? a : w;
            if ((c.w & (1u << 20)) != 0u) a = sf_copy(c, src, a);
            else w = sf_copy(c, src, w);
        }
        if (k == 0u) {
            if (sf_clr.x != 0u) w = (w & ~sf_clr.z) | (sf_clr.y & sf_clr.z);
            [loop] for (uint e = 0u; e < sf_ep.x; e++) {
                uint n = mft_cnt_at(Q, s, e);
                w = (w & 0xFFFF00FFu) | ((((w >> 8) + n) & 255u) << 8);
                if (e + 1u < sf_ep.x) { uint4 q = sf_flat[e]; w = npost_word(w, w, q.x, q.y, q.z, q.w, 0u); }
            }
        }
    }
    if (sf_np2.y != 0u) w = npost_word(w, w, sf_np.x, sf_np.y, sf_np.z, sf_np.w, sf_np2.x);
    return w;
}
float4 mft_shfuse_soft(uint2 P, uint s, uint w) {
    uint S = mft_shfuse_word_q(P, P, s, w);
    float4 res = mft_unorm(S);
    float R = asfloat(sf_ep.z);
    int ri = (int)ceil(R);
    int2 hi = int2(sf_svbb.zw) + 64;     // (outside the volumes' box every counter is 0)
    // (cost) only near the volumes' box, and only where a sample at the radius left / right / up / down disagrees with
    // the pixel (an edge); everywhere else the game's word as it is
    bool nearb = (int)P.x + ri >= (int)sf_svbb.x && (int)P.x - ri < (int)sf_svbb.z && (int)P.y + ri >= (int)sf_svbb.y &&
                 (int)P.y - ri < (int)sf_svbb.w;
    [branch] if (nearb) {
        bool inP = mft_soft_in(P, s);
        bool edge = false;
        [unroll] for (uint j = 0u; j < 4u; j++) {
            int2 o = j == 0u ? int2(ri, 0) : j == 1u ? int2(-ri, 0) : j == 2u ? int2(0, ri) : int2(0, -ri);
            edge = edge || mft_soft_in(uint2(clamp(int2(P) + o, int2(0, 0), hi)), s) != inP;
        }
        [branch] if (edge) {
            float rot = frac(52.9829189 * frac(dot((float2)P, float2(0.06711056, 0.00583715)))) * 6.2831853;
            uint nin = inP ? 1u : 0u, nt = 1u;
            int2 qin = int2(-1, -1), qout = int2(-1, -1);
            [unroll] for (uint i = 0u; i < 12u; i++) {
                float a = (float)i * 2.39996323 + rot, r = R * sqrt(((float)i + 0.5) / 12.0);
                int2 Q = clamp(int2(P) + int2(round(r * float2(cos(a), sin(a)))), int2(0, 0), hi);
                bool b = mft_soft_in(uint2(Q), s);
                nin += b ? 1u : 0u;
                nt++;
                if (b) qin = Q; else qout = Q;
            }
            float cov = (float)nin / (float)nt;
            int2 qo = inP ? qout : qin;  // inside: the pixel unshadowed (an outside neighbour's counters); outside: shadowed
            [branch] if (qo.x >= 0) {
                float4 o = mft_unorm(mft_shfuse_word_q(P, uint2(qo), s, w));
                res.rgb = inP ? lerp(o.rgb, res.rgb, cov) : lerp(res.rgb, o.rgb, cov);
            }
        }
    }
    return res;
}
#endif
#if MS
// per sample (the pixels whose samples differ: the stencil marks the others, done by ps_mft_shfuse_px)
float4 ps_mft_shfuse(float4 pos : SV_Position, uint s : SV_SampleIndex) : SV_Target {
    uint2 P = uint2(pos.xy);
#if SOFT
    return mft_shfuse_soft(P, s, mft_col(P, s));
#else
    return mft_unorm(mft_shfuse_word(P, s, mft_col(P, s)));
#endif
}
// per pixel: a pixel whose samples hold one word and one count in every epoch (most of them) is done once for all its
// samples; the others are discarded (left to the per-sample pass)
float4 ps_mft_shfuse_px(float4 pos : SV_Position) : SV_Target {
    uint2 P = uint2(pos.xy);
    uint w = mft_col(P, 0u);
    bool same = true;
    [loop] for (uint s = 1u; s < m_cfg.x; s++) {
        same = same && mft_col(P, s) == w;
        [loop] for (uint e = 0u; e < sf_ep.x; e++) same = same && mft_cnt_at(P, s, e) == mft_cnt_at(P, 0u, e);
    }
    if (!same) discard;
#if SOFT
    return mft_shfuse_soft(P, 0u, w);
#else
    return mft_unorm(mft_shfuse_word(P, 0u, w));
#endif
}
#else
float4 ps_mft_shfuse(float4 pos : SV_Position) : SV_Target {
    uint2 P = uint2(pos.xy);
#if SOFT
    return mft_shfuse_soft(P, 0u, mft_col(P, 0u));
#else
    return mft_unorm(mft_shfuse_word(P, 0u, mft_col(P, 0u)));
#endif
}
#endif
#if MS
// (r8-perf, diagnostics: PS2_R8P_VERIFY) two results of one step compared per sample: colour words, depth bits; counts of
// the differing samples and the first differing pixel (lowest y << 16 | x)
Texture2DMS<uint4> v_ca : register(t22);
Texture2DMS<uint4> v_cb : register(t23);
Texture2DMS<float> v_za : register(t24);
Texture2DMS<float> v_zb : register(t25);
RWByteAddressBuffer v_out : register(u0);
[numthreads(8, 8, 1)]
void cs_mft_cmp(uint3 id : SV_DispatchThreadID) {
    uint w, h, n;
    v_ca.GetDimensions(w, h, n);
    if (id.x >= w || id.y >= h) return;
    uint dc = 0u, dz = 0u;
    [loop] for (uint s = 0u; s < n; s++) {
        if (any(v_ca.Load(int2(id.xy), s) != v_cb.Load(int2(id.xy), s))) dc++;
        if (asuint(v_za.Load(int2(id.xy), s)) != asuint(v_zb.Load(int2(id.xy), s))) dz++;
    }
    uint o;
    if (dc != 0u) v_out.InterlockedAdd(0, dc, o);
    if (dz != 0u) v_out.InterlockedAdd(4, dz, o);
    if (dc != 0u || dz != 0u) v_out.InterlockedMin(8, id.x | (id.y << 16), o);
}
#endif
// DATE on the surfaces: the stencil's bit 0 = the colour's alpha bit 7 (per sample; the stencil was cleared, samples whose
// bit is 0 are discarded); the DATE draws then test the stencil
#if MS
void ps_mft_date(float4 pos : SV_Position, uint s : SV_SampleIndex) {
#else
void ps_mft_date(float4 pos : SV_Position) {
    uint s = 0u;
#endif
    if ((mft_col(uint2(pos.xy), s) >> 31) == 0u) discard;
}
#endif

