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
    /* (reimagine2) the whole frame's candidates for the world matrix (unlit entries only: characters and lit props are
     * never the world) and its lit entries (their matrix, light block), decided once the frame is complete (reim_finish) */
    struct { float m[16]; int n; } cand[32];
    int ncand, nlent, done;
    struct { float m[16]; uint32_t lb; } lent[384];
    float pv_old[16]; int pv_old_n;           /* (2, diagnostics) what 0.1.1214 would have taken (the best segment's most common) */
} reim_scene;
/* (local lights) the frame data's light record (runtime/r2/r2.h r2_light) */
typedef struct {
    float pos[3], r_out, col[3], r_in, amb, dif, cw;
    uint32_t id, kind, flags, pad[2];
} re_light_rec;
_Static_assert(sizeof(re_light_rec) == 64, "r2_light is 64 bytes");
/* the latest game frame's light table (copied when its first segment is noted: the frame data does not outlive it) */
static struct { re_light_rec l[256]; int n; uint32_t serial; uint64_t age; } RLIN;
static reim_scene RS, RS_last;
static struct { float cam[3], pre[3]; int have, pre_ok; uint32_t pre_v; uint64_t switches, cuts, old_wrong, flips; } RW;   /* (2) the world camera of the last frame */
static struct {
    int env, log, dbg, ss, shafts, fog, grade, smap, sm_size;
    float sm_k, sm_dist;
    float sss_k, sss_len, sss_th, bl_t, bl_k, bl_i, halo, sat, con, vib, expo, fog0, fogd, fogmax, hfog, hfog_h, vig, sh_i,
        night, stat_a, rim, spec, clar, key_i, ao_r, ao_k, ao_min;
    /* the key light kept from frame to frame (smoothed) */
    float key[3], keyc[3], keyw;
    float kv[3];                         /* the key light in view space (the last post pass): the surface maps' relief */
    float cand[3]; int pend;             /* (fix) a different key light seen for pend frames in a row */
    uint64_t posts, maps, map_reuse, map_fd, map_ops, key_snaps;   /* (fix) statistics */
    float relief, cavity, nmap;
    int have_key;
    int shaft_key;                       /* (2) shafts from the key light only (0: 0.1.1214's brightest far texels) */
    float fire_k;                        /* (2) warm glow lighting its surroundings */
    float cas;                           /* (2) contrast-adaptive sharpening of the picture (0: off) */
    int sh_at;                           /* (2) cut-out casters through their alpha test (PS2_REIM_SH_AT) */
    int v2;                              /* (2) PS2_REIM2=0: none of reimagine2's changes */
    int sc_on, sc_size, sc_every;        /* (2) the static shadow cache (PS2_REIM_SC=0: off), its size, its update interval */
    uint64_t sc_fresh, sc_moves;
} RE;
enum { RT_SHH, RT_SH, RT_SH2, RT_B6, RT_SHAFT, RT_PIC, RT_ST0, RT_ST1, RT_SCENE, RT_LOC, RT_N };
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
    float smR, lm_last[16], smp_last[12], lc_last[20]; int have_last;
    /* (reimagine2) the static shadow cache (two maps: moved by copying), its light-space grid centre, depth range, the
     * key light it was drawn for */
    ID3D11Texture2D* sc[2];
    ID3D11DepthStencilView* sc_dsv[2];
    ID3D11ShaderResourceView* sc_srv[2];
    int sc_n, sc_cur, sc_valid;
    float sc_c[2], sc_z0, sc_zr, sc_R, sc_key[3];
    uint64_t sc_age;
    ID3D11DepthStencilState* ds_always;
    float ppv[16], pndc[4]; int have_prev; uint32_t prev_serial;   /* (fix) the last frame's world x clip, ndc mapping */   /* (fix) the sphere radius kept; the last map's matrix */
    uint64_t frames;
} RR;

/* PS2_REIM_FIX=0: 0.1.1207's behaviour (for before / after measurements) */
static int re_fix(void) { static int v = -1; if (v < 0) { const char* e = getenv("PS2_REIM_FIX"); v = !(e && *e == '0'); } return v && r8p_on(R8P_REIMFIX); }
/* (reimagine2) a switchable change: experiment bit b (PS2_REIM_X=<mask>) is off in A phases and on in B phases of PS2_GPU_AB
 * with PS2_GPU_AB_R8=4096 (one run, the same camera path for both); not in the mask: its default def */
static int re_x(int b, int def) {
    static int m = -1;
    if (m < 0) { const char* e = getenv("PS2_REIM_X"); m = e && *e ? (int)strtol(e, NULL, 0) : 0; }
    return (m & b) ? r8p_on(R8P_REIMX) : def;
}
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
    e = getenv("PS2_REIM_SHAFT_KEY"); RE.shaft_key = !(e && *e == '0');
    RE.fire_k = lt_envf("PS2_REIM_FIRE", 0.5f);
    RE.cas = lt_envf("PS2_REIM_CAS", 0.6f);
    e = getenv("PS2_REIM_SH_AT"); RE.sh_at = !(e && *e == '0');
    e = getenv("PS2_REIM_SC"); RE.sc_on = !(e && *e == '0');
    RE.sc_size = (int)lt_envf("PS2_REIM_SC_SIZE", 4096.0f);
    if (RE.sc_size < 512) RE.sc_size = 512;
    if (RE.sc_size > 8192) RE.sc_size = 8192;
    RE.sc_every = (int)lt_envf("PS2_REIM_SC_EVERY", 4.0f);
    if (RE.sc_every < 1) RE.sc_every = 1;
    e = getenv("PS2_REIM2");                            /* (=0: 0.1.1214's Reimagined, for before / after evidence) */
    RE.v2 = !(e && *e == '0');
    if (!RE.v2) { RE.shaft_key = 0; RE.fire_k = 0.0f; RE.cas = 0.0f; RE.sh_at = 0; RE.sc_on = 0; }
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

/* one lit entry's four lights (light block q, entry matrix em) into the frame's world-direction clusters (s->ipv: the
 * world x clip's inverse) */
static void re_lights(reim_scene* s, const float* em, const float* q, const gpu_front_frame_data* fd, uint32_t i) {
    float om[16];
    re_mul4(em, s->ipv, om);             /* object -> world */
    s->nlit++;
    for (int k = 0; k < 4; k++) {
        float d[3] = { q[0 * 4 + k], q[1 * 4 + k], q[2 * 4 + k] };
        const float* c = q + (4 + k) * 4;
        float w[3];
        for (int j = 0; j < 3; j++) w[j] = d[0] * om[0 * 4 + j] + d[1] * om[1 * 4 + j] + d[2] * om[2 * 4 + j];
        float l = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        float br = c[0] * 0.3f + c[1] * 0.59f + c[2] * 0.11f;
        if (RE.log >= 2 && fd->vblank % 300u < 2u && s->nlit <= 6)
            gpu_log("reim: v%u ent %u light %d obj %.3f %.3f %.3f world %.3f %.3f %.3f |%.3f| col %.3f %.3f %.3f %.3f thr %.3f amb %.3f %.3f %.3f",
                    fd->vblank, i, k, d[0], d[1], d[2], l > 0 ? w[0] / l : 0, l > 0 ? w[1] / l : 0, l > 0 ? w[2] / l : 0, l, c[0], c[1], c[2], c[3],
                    q[9 * 4 + k], q[8 * 4], q[8 * 4 + 1], q[8 * 4 + 2]);
        /* f = min(d + d^3, threshold) x colour: with a negative colour the light comes from -direction */
        if (!(l > 1e-6f) || !(fabsf(br) > 1e-3f)) continue;
        float sg = br < 0.0f ? -1.0f : 1.0f;
        float u[3] = { sg * w[0] / l, sg * w[1] / l, sg * w[2] / l }, cc[3] = { sg * c[0], sg * c[1], sg * c[2] };
        br = fabsf(br);
        int f = -1;
        for (int j = 0; j < s->ncl && f < 0; j++) {
            float dd = s->cl[j].d[0] * u[0] + s->cl[j].d[1] * u[1] + s->cl[j].d[2] * u[2];
            if (dd > 0.995f * s->cl[j].n) f = j;    /* (d: the sum of n unit vectors) */
        }
        if (f < 0 && s->ncl < RE_NCL) { f = s->ncl++; memset(&s->cl[f], 0, sizeof s->cl[f]); }
        if (f < 0) continue;
        for (int j = 0; j < 3; j++) { s->cl[f].d[j] += u[j]; s->cl[f].c[j] += cc[j]; }
        s->cl[f].br += br;
        s->cl[f].n++;
    }
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
    if (fb->pad && RLIN.serial != fb->serial) {   /* (local lights) the frame's light table (gpu_r2front.c FL) */
        size_t lo = (size_t)(fb->pad - 1u) * 16u;
        uint32_t ln = 0;
        if (lo + 16u <= fd->data_bytes) memcpy(&ln, fd->data + lo, 4);
        if (ln && ln <= 256u && lo + 16u + (size_t)ln * sizeof(re_light_rec) <= fd->data_bytes) {
            memcpy(RLIN.l, fd->data + lo + 16u, (size_t)ln * sizeof(re_light_rec));
            RLIN.n = (int)ln;
            RLIN.serial = fb->serial;
            RLIN.age = 0;
        }
    }
    uint32_t n = o->nent;
    if (o->ent_off + (size_t)n * sizeof(gpu_r2_entry) > fd->data_bytes) return;
    const gpu_r2_entry* ent = (const gpu_r2_entry*)(fd->data + o->ent_off);
    if (RE.v2) {
        /* (reimagine2) the frame's unlit entries' matrices counted over every segment, the lit entries kept: reim_finish
         * decides. 0.1.1214 took the most common matrix of whichever segment had the most entries with one matrix, lit or
         * not: where few world entries were drawn (under trees, a wall in front of the camera) a group of plants or Sly's
         * own entries could win - every position rebuilt from the depth then sat in the wrong space and the shadows
         * jumped about ("confused where to show") */
        {                                /* (diagnostics: 0.1.1214's choice, to count the frames it got wrong) */
            int cand[8], cn[8], nc = 0, best = -1, bestn = 0;
            for (uint32_t i = 0; i < n && i < 512; i++) {
                int f = -1;
                for (int j = 0; j < nc && f < 0; j++) if (!memcmp(ent[i].m, ent[cand[j]].m, 64)) f = j;
                if (f >= 0) cn[f]++;
                else if (nc < 8) { cand[nc] = (int)i; cn[nc++] = 1; }
            }
            for (int j = 0; j < nc; j++) if (cn[j] > bestn) { bestn = cn[j]; best = cand[j]; }
            if (best >= 0 && bestn > RS.pv_old_n && bestn >= 3) { memcpy(RS.pv_old, ent[best].m, 64); RS.pv_old_n = bestn; }
        }
        for (uint32_t i = 0; i < n && i < 2048; i++) {
            uint32_t xq = ent[i].pad[0];
            if (xq) {
                size_t lb = (size_t)(xq - 1) * 16u;
                if (lb + 11 * 16 <= fd->data_bytes && RS.nlent < (int)(sizeof RS.lent / sizeof RS.lent[0])) {
                    memcpy(RS.lent[RS.nlent].m, ent[i].m, 64);
                    RS.lent[RS.nlent++].lb = (uint32_t)lb;
                }
                continue;
            }
            int f = -1;
            for (int j = 0; j < RS.ncand && f < 0; j++) if (!memcmp(ent[i].m, RS.cand[j].m, 64)) f = j;
            if (f >= 0) RS.cand[f].n++;
            else if (RS.ncand < 32) { memcpy(RS.cand[RS.ncand].m, ent[i].m, 64); RS.cand[RS.ncand++].n = 1; }
        }
        return;
    }
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
        re_lights(&RS, ent[i].m, (const float*)(fd->data + lb), fd, i);   /* T0..T2 [0..2], colours [4..7], ambient [8], thresholds [9] */
    }
    if (RE.log && fd->vblank % 300u < 2u) {
        float cam[4] = { RS.ipv[8] / RS.ipv[11], RS.ipv[9] / RS.ipv[11], RS.ipv[10] / RS.ipv[11], 0 };   /* (clip x = y = w = 0) */
        gpu_log("reim: v%u seg ents %u world matrix x%d (pv row0 %.4g %.4g %.4g %.4g row2 %.4g %.4g %.4g %.4g) cam %.1f %.1f %.1f; %d light clusters",
                fd->vblank, n, RS.pv_n, RS.pv[0], RS.pv[1], RS.pv[2], RS.pv[3], RS.pv[8], RS.pv[9], RS.pv[10], RS.pv[11],
                cam[0], cam[1], cam[2], RS.ncl);
    }
}

/* (reimagine2) the frame's world matrix, once all its segments are noted: of the unlit entries' matrices (>= 3 entries),
 * the one whose camera continues the last frame's (the world camera moves a little per frame; another object's matrix
 * puts its "camera" where that object is), else (the first frame, a camera cut) the one most entries share over the
 * whole frame; then the lit entries' lights in that world */
static void reim_finish(reim_scene* s) {
    if (!RE.v2 || s->done) return;
    s->done = 1;
    if (!s->fd || s->fd != X.fd) return;
    int maxn = 0;
    for (int j = 0; j < s->ncand; j++) if (s->cand[j].n > maxn) maxn = s->cand[j].n;
    if (maxn < 3) return;
    float inv[32][16], cam[32][3];
    int okj[32], best = -1;
    float bd = 1e30f;
    for (int j = 0; j < s->ncand; j++) {
        okj[j] = s->cand[j].n >= 3 && re_inv4(s->cand[j].m, inv[j]) && fabsf(inv[j][11]) > 1e-20f;
        if (!okj[j]) continue;
        for (int k = 0; k < 3; k++) cam[j][k] = inv[j][8 + k] / inv[j][11];
        if (RW.have && s->cand[j].n * 4 >= maxn) {
            float d = 0.0f;
            for (int k = 0; k < 3; k++) d += (cam[j][k] - RW.cam[k]) * (cam[j][k] - RW.cam[k]);
            if (d < bd) { bd = d; best = j; }
        }
    }
    if (best < 0 || bd > 600.0f * 600.0f) {   /* (none continues: the first frame or a cut - the most common) */
        if (RW.have) {
            RW.cuts++;
            if (RE.log) gpu_log("reim-world: v%u the world camera jumped (a cut or a load): the most common matrix again", s->fd->vblank);
            RW.pre_v = s->fd->vblank; memcpy(RW.pre, RW.cam, sizeof RW.pre); RW.pre_ok = 1;
        }
        best = -1;
        for (int j = 0; j < s->ncand; j++) if (okj[j] && (best < 0 || s->cand[j].n > s->cand[best].n)) best = j;
        if (best < 0) return;
    }
    for (int j = 0; j < s->ncand; j++) if (okj[j] && s->cand[j].n > s->cand[best].n) { RW.switches++; break; }   /* (the count alone: another) */
    if (s->pv_old_n && memcmp(s->pv_old, s->cand[best].m, 64)) RW.old_wrong++;
    memcpy(s->pv, s->cand[best].m, 64);
    memcpy(s->ipv, inv[best], 64);
    s->pv_ok = 1; s->pv_n = s->cand[best].n;
    if (RW.pre_ok && s->fd->vblank != RW.pre_v && s->fd->vblank - RW.pre_v < 60u) {   /* (a jump straight back: the choice flipped) */
        float d = 0.0f;
        for (int k = 0; k < 3; k++) d += (cam[best][k] - RW.pre[k]) * (cam[best][k] - RW.pre[k]);
        float e = 0.0f;
        for (int k = 0; k < 3; k++) e += (cam[best][k] - RW.cam[k]) * (cam[best][k] - RW.cam[k]);
        if (d < 600.0f * 600.0f && e > 600.0f * 600.0f) {
            RW.flips++; RW.pre_ok = 0;
            if (RE.log) gpu_log("reim-world: v%u the world camera flipped back (the world matrix choice went wrong)", s->fd->vblank);
        }
    } else if (RW.pre_ok && s->fd->vblank - RW.pre_v >= 60u) RW.pre_ok = 0;
    memcpy(RW.cam, cam[best], sizeof RW.cam);
    RW.have = 1;
    s->ncl = 0; s->nlit = 0;
    for (int i = 0; i < s->nlent; i++) re_lights(s, s->lent[i].m, (const float*)(s->fd->data + s->lent[i].lb), s->fd, (uint32_t)i);
    if (RE.log >= 3)
        gpu_log("reim-world: v%u %d candidates (most %d), world x%d cam %.0f %.0f %.0f, %d lit entries, %d clusters", s->fd->vblank,
                s->ncand, maxn, s->pv_n, RW.cam[0], RW.cam[1], RW.cam[2], s->nlent, s->ncl);
}
static float reim_rim(void) { re_env(); return RE.rim; }
static float reim_spec(void) { re_env(); return RE.spec; }
/* (the R2 pipelines' r2_reim / r2_reim2: the key light's direction in the picture plane, relief, cavity, normal maps) */
static void reim_r2k(float* k) {
    re_env();
    memset(k, 0, 8 * sizeof(float));
    k[0] = RE.kv[0]; k[1] = RE.kv[1]; k[2] = RE.relief; k[3] = RE.cavity; k[4] = RE.nmap;
}   /* (the R2 pipelines' rim light under Reimagined) */

/* ---- (local lights) the game's own point lights (notes/reimagined-lighting.md "Local lights") ---------------------
 * The static world's light table (lamps, torches, fires, windows and the designers' fill lights; r2_capture.c copies it
 * at each frame's start, Reimagined only) lights the picture's surfaces with the VU's own light model (light.hlsl
 * ps_reim_local, half res). Here: per light a weight that eases (0.25 s) in when the game adds it or it enters the list
 * and out when it leaves (never a pop); lights whose sphere misses the view are not sent but keep their weight (no fade-in
 * when the camera turns towards one); cool fills count less than warm sources; fires (orange, small) flicker gently and
 * deterministically. Quality: Low none, Medium 32 lights, High 64, Ultra 96 (gpu_reim_lights_config from gfx.c).
 * PS2_REIM_LIGHTS=0: none (also the EE copy); _N (count), _I (gain), _FLOOR, _COOL, _FLICK, _Q (quality 0..3). */
#define RL_MAX 96
static volatile LONG rl_cfg_level = 2;   /* graphics quality: 0 Low (off) .. 3 Ultra */
void gpu_reim_lights_config(int level) { InterlockedExchange(&rl_cfg_level, level); }
static struct {
    int env, on, nmax, q;
    float gain, floor, cool, flick, d0, d1, core, pool;
    uint32_t id[256];                    /* per table slot: the light it holds (r2_light.id), its weight, selected */
    float w[256];
    uint8_t sel[256];
    float t;                             /* the flicker's clock (1 / frame rate per post pass: deterministic) */
    float cb[(5 + RL_MAX * 3) * 4];      /* light.hlsl reim_lights: rl_n, rl_pv0..3, rl[] */
    int n;
    ID3D11Buffer* buf;
    uint64_t frames, sum_n, sum_vis, max_n;
} RL;
static void rl_env(void) {
    if (RL.env) return;
    RL.env = 1;
    const char* e = getenv("PS2_REIM_LIGHTS"); RL.on = !(e && *e == '0');
    RL.nmax = (int)lt_envf("PS2_REIM_LIGHTS_N", -1.0f);
    RL.q = (int)lt_envf("PS2_REIM_LIGHTS_Q", -1.0f);
    RL.gain = lt_envf("PS2_REIM_LIGHTS_I", 3.0f);
    RL.floor = lt_envf("PS2_REIM_LIGHTS_FLOOR", 0.025f);
    RL.cool = lt_envf("PS2_REIM_LIGHTS_COOL", 0.3f);    /* the cool (blue) fills' share */
    RL.flick = lt_envf("PS2_REIM_LIGHTS_FLICK", 0.06f);
    RL.d0 = lt_envf("PS2_REIM_LIGHTS_D0", 6000.0f);     /* fade with the distance from the camera to the light's sphere */
    RL.d1 = lt_envf("PS2_REIM_LIGHTS_D1", 12000.0f);
    RL.core = lt_envf("PS2_REIM_LIGHTS_CORE", 0.35f);   /* the pool's core (share of the outer radius: half the light there) */
    if (RL.core < 0.05f) RL.core = 0.05f;
    RL.pool = lt_envf("PS2_REIM_LIGHTS_POOL", 1.0f);     /* the pool falloff's share (0: the game's ramp) */
}
static int rl_count(void) {
    rl_env();
    if (!RL.on) return 0;
    int q = RL.q >= 0 ? RL.q : (int)rl_cfg_level;
    int n = q <= 0 ? 0 : q == 1 ? 32 : q == 2 ? 64 : 96;
    if (RL.nmax >= 0) n = RL.nmax;
    return n < 0 ? 0 : n > RL_MAX ? RL_MAX : n;
}
static float rl_smooth(float a, float b, float x) { float t = (x - a) / (b - a); t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t; return t * t * (3.0f - 2.0f * t); }
/* how much of a light the surroundings get: warm sources (lamps, fires, windows) fully, the cool (blue) fills that tint
 * the characters with moonlight RL.cool, and the wide fills placed to light characters (outer radius beyond ~1200) less */
static float rl_kind(const re_light_rec* L) {
    float warm = rl_smooth(-0.1f, 0.3f, L->col[0] - L->col[2]);
    float big = rl_smooth(1200.0f, 2500.0f, L->r_out);
    return (RL.cool + (1.0f - RL.cool) * warm) * (1.0f - 0.6f * big);
}
/* this frame's light list into RL.cb (light.hlsl reim_lights); pv: world x clip (row vectors), cam: the camera (world) */
static int reim_lights_build(const float* pv, const float* cam, const float* ndc, double hz) {
    int nmax = rl_count();
    RL.n = 0;
    memset(RL.cb, 0, 20 * sizeof(float));
    if (++RLIN.age > 30u) RLIN.n = 0;    /* (no table for half a second: a level without lights, or another look) */
    if (!nmax || !RLIN.n) return 0;
    float dt = (float)(1.0 / (hz > 1.0 ? hz : 60.0));
    RL.t += dt;
    if (RL.t > 3600.0f) RL.t -= 3600.0f;
    float ease = 1.0f - expf(-dt / 0.25f);
    /* the picture's planes: ndc x / y between the picture's edges (ndc[0..3]: x0, x1, y0, y1; the clip volume is the VU's
     * guard band, ~8 x the picture), 15 % margin each side; and in front of the camera */
    float pl[5][4];
    for (int k = 0; k < 4; k++) {
        float* p = pl[k];
        int j = k >> 1;
        float lo = fminf(ndc[2 * j], ndc[2 * j + 1]), hi = fmaxf(ndc[2 * j], ndc[2 * j + 1]), m = (hi - lo) * 0.15f;
        lo -= m; hi += m;
        for (int r = 0; r < 4; r++) p[r] = (k & 1) ? hi * pv[r * 4 + 3] - pv[r * 4 + j] : pv[r * 4 + j] - lo * pv[r * 4 + 3];
    }
    for (int r = 0; r < 4; r++) pl[4][r] = pv[r * 4 + 3];
    for (int k = 0; k < 5; k++) { float l = sqrtf(pl[k][0] * pl[k][0] + pl[k][1] * pl[k][1] + pl[k][2] * pl[k][2]); if (l > 1e-20f) for (int r = 0; r < 4; r++) pl[k][r] /= l; }
    int cand[256], nc = 0, nvis = 0;
    float pri[256];
    uint8_t vis[256];
    for (int i = 0; i < RLIN.n; i++) {
        const re_light_rec* L = &RLIN.l[i];
        uint32_t s = L->id & 0xFFu;
        if (RL.id[s] != L->id) { RL.id[s] = L->id; RL.w[s] = 0.0f; RL.sel[s] = 0; }   /* (a new light: it fades in) */
        vis[i] = 1;
        for (int k = 0; k < 5 && vis[i]; k++)
            if (pl[k][0] * L->pos[0] + pl[k][1] * L->pos[1] + pl[k][2] * L->pos[2] + pl[k][3] < -L->r_out) vis[i] = 0;
        if (!vis[i]) continue;
        nvis++;
        float dx = L->pos[0] - cam[0], dy = L->pos[1] - cam[1], dz = L->pos[2] - cam[2];
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        float fd = 1.0f - rl_smooth(RL.d0, RL.d1, dist - L->r_out);
        float pk = fmaxf(L->col[0], fmaxf(L->col[1], L->col[2])) * (2.0f * fabsf(L->dif) + fmaxf(L->amb, 0.0f));
        if (!(fd > 0.0f) || !(pk > 1e-3f)) { vis[i] = 2; continue; }   /* (too far / black: not selected, fades out) */
        pri[nc] = fd * pk * rl_kind(L) * L->r_out / fmaxf(dist, L->r_out) * (RL.sel[s] ? 1.25f : 1.0f);   /* (hysteresis) */
        cand[nc++] = i;
    }
    /* the nmax most important (insertion sort by priority, descending) */
    for (int a = 1; a < nc; a++) {
        int ci = cand[a]; float cp = pri[a]; int b = a - 1;
        while (b >= 0 && pri[b] < cp) { pri[b + 1] = pri[b]; cand[b + 1] = cand[b]; b--; }
        pri[b + 1] = cp; cand[b + 1] = ci;
    }
    uint8_t chosen[256];
    memset(chosen, 0, sizeof chosen);
    for (int a = 0; a < nc && a < nmax; a++) chosen[cand[a]] = 1;
    for (int i = 0; i < RLIN.n; i++) {   /* (the weights: in view only - outside it a light lights nothing visible) */
        uint32_t s = RLIN.l[i].id & 0xFFu;
        if (!vis[i]) continue;
        RL.sel[s] = chosen[i];
        RL.w[s] += ((chosen[i] ? 1.0f : 0.0f) - RL.w[s]) * ease;
    }
    memcpy(RL.cb + 4, pv, 16 * sizeof(float));   /* (debug: the lights' places) */
    float* o = RL.cb + 20;
    int n = 0;
    for (int pass = 0; pass < 2; pass++)   /* (the chosen first, then those still fading out while room is left) */
    for (int i = 0; i < RLIN.n; i++) {
        const re_light_rec* L = &RLIN.l[i];
        uint32_t s = L->id & 0xFFu;
        if (!vis[i] || chosen[i] != (pass == 0) || RL.w[s] < 0.002f || n >= RL_MAX || (pass && n >= nmax + 16)) continue;
        float dx = L->pos[0] - cam[0], dy = L->pos[1] - cam[1], dz = L->pos[2] - cam[2];
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        float fd = 1.0f - rl_smooth(RL.d0, RL.d1, dist - L->r_out);
        float k = 2.0f * RL.gain * RL.w[s] * fd * rl_kind(L);
        /* fires: orange and small - a gentle flicker of three slow sines (phases from the light's id) */
        if (RL.flick > 0.0f && L->col[0] > 0.5f && L->col[1] < 0.72f * L->col[0] && L->col[2] < 0.45f * L->col[0] && L->r_out < 2000.0f) {
            uint32_t h = L->id * 2654435761u;
            float p1 = (float)(h & 1023u) * 0.00614f, p2 = (float)((h >> 10) & 1023u) * 0.00614f, p3 = (float)((h >> 20) & 1023u) * 0.00614f;
            float f = 0.5f * sinf(RL.t * 10.7f + p1) + 0.3f * sinf(RL.t * 27.0f + p2) + 0.2f * sinf(RL.t * 57.2f + p3);
            k *= 1.0f + RL.flick * f;
        }
        float dif = -fabsf(L->dif), amb = L->amb;
        if (!(fabsf(dif) > 1e-4f)) { dif = -0.25f; amb = 0.1f; }
        float span = L->r_out - L->r_in;
        o[0] = L->pos[0]; o[1] = L->pos[1]; o[2] = L->pos[2]; o[3] = 1.0f / (span > 1.0f ? span : 1.0f);
        o[4] = L->col[0] * k; o[5] = L->col[1] * k; o[6] = L->col[2] * k; o[7] = L->r_out;
        o[8] = dif; o[9] = amb; o[10] = -amb / dif; o[11] = L->r_out * L->r_out;
        o += 12;
        n++;
    }
    RL.cb[0] = (float)n;
    RL.cb[1] = RE.dbg == 5 ? 1.0f : 0.0f;
    RL.cb[2] = 1.0f / (RL.core * RL.core);
    RL.cb[3] = RL.pool;
    RL.n = n;
    RL.frames++; RL.sum_n += (uint64_t)n; RL.sum_vis += (uint64_t)nvis;
    if ((uint64_t)n > RL.max_n) RL.max_n = (uint64_t)n;
    if (RE.log && RL.frames % 600 == 1) {
        gpu_log("reim-lights: %d in the table, %d in view, %d drawn (cap %d; average %.1f drawn / %.1f in view, most %llu)", RLIN.n, nvis, n, nmax,
                (double)RL.sum_n / (double)RL.frames, (double)RL.sum_vis / (double)RL.frames, (unsigned long long)RL.max_n);
        if (RE.log >= 2) {
            int bi = -1; float bd = 1e30f;
            for (int i = 0; i < RLIN.n; i++) {
                const re_light_rec* L = &RLIN.l[i];
                float dx = L->pos[0] - cam[0], dy = L->pos[1] - cam[1], dz = L->pos[2] - cam[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (d < bd) { bd = d; bi = i; }
            }
            gpu_log("reim-lights: camera %.0f %.0f %.0f; nearest light %d at %.0f %.0f %.0f (%.0f away, radius %.0f); ndc %.3f..%.3f %.3f..%.3f",
                    cam[0], cam[1], cam[2], bi, bi >= 0 ? RLIN.l[bi].pos[0] : 0.0f, bi >= 0 ? RLIN.l[bi].pos[1] : 0.0f, bi >= 0 ? RLIN.l[bi].pos[2] : 0.0f,
                    bd, bi >= 0 ? RLIN.l[bi].r_out : 0.0f, ndc[0], ndc[1], ndc[2], ndc[3]);
            for (int i = 0, k = 0; i < RLIN.n && k < 24; i++) {   /* (where the drawn lights are in the picture: 0..1) */
                const re_light_rec* L = &RLIN.l[i];
                if (vis[i] != 1 || RL.w[L->id & 0xFFu] < 0.01f) continue;
                float c[4];
                for (int j = 0; j < 4; j++) c[j] = L->pos[0] * pv[j] + L->pos[1] * pv[4 + j] + L->pos[2] * pv[8 + j] + pv[12 + j];
                if (!(c[3] > 1e-3f)) continue;
                k++;
                gpu_log("reim-lights:   #%u at %.0f %.0f %.0f r %.0f..%.0f col %.2f %.2f %.2f amb %.3f dif %.3f -> picture %.3f %.3f, %.0f away, weight %.2f",
                        L->id & 0xFFu, L->pos[0], L->pos[1], L->pos[2], L->r_in, L->r_out, L->col[0], L->col[1], L->col[2], L->amb, L->dif,
                        (c[0] / c[3] - ndc[0]) / (ndc[1] - ndc[0]), (c[1] / c[3] - ndc[2]) / (ndc[3] - ndc[2]), c[3], RL.w[L->id & 0xFFu]);
            }
        }
    }
    if (!n) return 0;
    if (!RL.buf) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = (UINT)sizeof RL.cb; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &RL.buf))) { RL.buf = NULL; RL.n = 0; return 0; }
    }
    ID3D11DeviceContext_UpdateSubresource(gpu_rt.ctx, (ID3D11Resource*)RL.buf, 0, NULL, RL.cb, 0, 0);
    return n;
}

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
    for (int i = 0; i < 2; i++) { GPU_RELEASE(RR.sc_dsv[i]); GPU_RELEASE(RR.sc_srv[i]); GPU_RELEASE(RR.sc[i]); }
    RR.sc_n = 0; RR.sc_valid = 0;
    GPU_RELEASE(RR.ds_always);
    GPU_RELEASE(RL.buf);                 /* (local lights) */
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
    ID3D11ShaderResourceView* none[15] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 14, 15, none);   /* (t14..t28) */
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
    if (re_fix()) {
        /* (fix) steady: the key light changes only when another one has won for 90 frames in a row, and then at once
         * (0.1.1207 eased towards each frame's winner: the light set changes as entries come and go, so the shadows swam
         * continuously); a frame without a light from above keeps the current one */
        if (!RE.have_key) {
            memcpy(RE.key, d, sizeof d); memcpy(RE.keyc, c, sizeof c);
            RE.have_key = 1; RE.pend = 0; RE.key_snaps++;
        } else if (best > 0.0f && RE.key[0] * d[0] + RE.key[1] * d[1] + RE.key[2] * d[2] < 0.995f) {
            if (RE.pend > 0 && RE.cand[0] * d[0] + RE.cand[1] * d[1] + RE.cand[2] * d[2] > 0.995f) RE.pend++;
            else { memcpy(RE.cand, d, sizeof d); RE.pend = 1; }
            if (RE.pend >= 90) { memcpy(RE.key, d, sizeof d); memcpy(RE.keyc, c, sizeof c); RE.pend = 0; RE.key_snaps++; RR.smR = 0.0f; }
        } else RE.pend = 0;
        if (RE.log >= 3)
            gpu_log("reim-key: cand %.4f %.4f %.4f score %.3f key %.4f %.4f %.4f pend %d", d[0], d[1], d[2], best, RE.key[0], RE.key[1],
                    RE.key[2], RE.pend);
        return;
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
/* an n x n depth map (R32: DSV D32_FLOAT, SRV R32_FLOAT); 0: failed (nothing kept) */
static int sm_tex(ID3D11Texture2D** t, ID3D11DepthStencilView** dsv, ID3D11ShaderResourceView** srv, int n) {
    GPU_RELEASE(*dsv); GPU_RELEASE(*srv); GPU_RELEASE(*t);
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = td.Height = (UINT)n; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, t))) { *t = NULL; return 0; }
    D3D11_DEPTH_STENCIL_VIEW_DESC dd;
    memset(&dd, 0, sizeof dd);
    dd.Format = DXGI_FORMAT_D32_FLOAT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Format = DXGI_FORMAT_R32_FLOAT; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    if (FAILED(ID3D11Device_CreateDepthStencilView(gpu_rt.dev, (ID3D11Resource*)*t, &dd, dsv)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)*t, &sd, srv))) {
        GPU_RELEASE(*dsv); GPU_RELEASE(*srv); GPU_RELEASE(*t);
        return 0;
    }
    return 1;
}

/* (reimagine2) the frame's casters, all or one kind (r2_render_segment in.shadow_sel: 1 the static world, 2 the rest),
 * into dsv (n x n) through m32 (inverse world x clip, world -> the map's clip) */
static void sm_draw(const reim_scene* S, ID3D11DepthStencilView* dsv, int n, const float* m32, int sel) {
    const gpu_front_frame_data* fd = S->fd;
    for (int i = 0; i < S->nops; i++) {
        const gpu_fop_r2* o = (const gpu_fop_r2*)(fd->cmd + S->ops[i]);
        r2_exec_in in = { gpu_rt.dev, gpu_rt.ctx, NULL, dsv, 1, 1, X.vram_srv, X.swz_srv, X.dat.srv, X.iota,
                          gpu_rt.caps.warp ? 0.49609375f : 0.42f, td_r2, NULL, NULL };
        in.texdec_srv = TD.srv;
        in.mft = 1; in.msaa = M.n; in.sx = M.rsx; in.sy = M.rsy;
        in.shadow = 1; in.shadow_size = n; in.shadow_m = m32;
        in.shadow_at = re_x(1, RE.sh_at);   /* (2: cut-outs cast their shapes) */
        in.shadow_sel = sel; in.shadow_pv = S->pv;
        RR.sm_draws += (uint64_t)r2_render_segment(o, fd->data, fd->data_bytes, &in);
    }
}

/* (reimagine2) the static shadow cache moved by (dx, dy) texels: its content copied into the other map with the offset
 * (light.hlsl ps_reim_sccopy, depth written), which becomes the current one */
static void sc_move(int dx, int dy) {
    int nc = RR.sc_n, from = RR.sc_cur, to = from ^ 1;
    if (!RR.ds_always) {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        if (FAILED(ID3D11Device_CreateDepthStencilState(gpu_rt.dev, &dd, &RR.ds_always))) { RR.ds_always = NULL; return; }
    }
    float cb[36];
    memset(cb, 0, sizeof cb);
    cb[0] = cb[1] = (float)nc; cb[2] = cb[3] = 1.0f / (float)nc;   /* (l_dst) */
    cb[4] = cb[5] = (float)nc; cb[6] = cb[7] = 1.0f / (float)nc;   /* (l_src: the old map's size) */
    cb[32] = (float)dx; cb[33] = (float)dy;                          /* (l_dir: the offset) */
    unbind_all();
    bind_frame_buffers();                /* (the state caches: the R2 casters drew around them) */
    lt_cb(cb, 36);
    ID3D11Buffer* b = LT.cb;
    ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 7, 1, &b);
    ID3D11DeviceContext_OMSetRenderTargets(gpu_rt.ctx, 0, NULL, RR.sc_dsv[to]);
    set_depth(RR.ds_always);
    set_vs(gpu_vs(GPU_SH_vs_light));
    set_topo(0);
    set_viewport(nc, nc);
    set_scissor(0, 0, nc, nc);
    set_blend(blend_state(BL_OFF, 15));
    set_ps(gpu_ps(GPU_SH_ps_reim_sccopy));
    lt_srv(27, RR.sc_srv[from]);
    ID3D11DeviceContext_Draw(gpu_rt.ctx, 3, 0);
    gpu_rt.n_draws++;
    ID3D11ShaderResourceView* none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 27, 1, &none);
    unbind_all();
    set_depth(X.ds_off);
    RR.sc_cur = to;
}

static int reim_smap(const reim_scene* S, const float* rndc, const float* rzab, int pw, int ph, float* lm, float* smp, float* lc) {
    int n = RE.sm_size;
    if (!S->nops) { RE.map_ops++; return 0; }
    if (S->fd != X.fd || !X.fd) { RE.map_fd++; return 0; }
    if (!RR.sm || RR.sm_n != n) {
        RR.sm_n = 0;
        if (!sm_tex(&RR.sm, &RR.sm_dsv, &RR.sm_srv, n)) return 0;
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
    if (re_fix()) {                      /* (fix) a radius that does not change with the camera's small FOV changes */
        R = ceilf(R / 512.0f) * 512.0f;
        if (R > RR.smR || R < RR.smR - 1536.0f) RR.smR = R;
        R = RR.smR;
    }
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
    if (re_fix()) { z0 = floorf(z0 / 256.0f) * 256.0f; zr += 256.0f; }   /* (fix: the depths do not shift with the camera) */
    for (int j = 0; j < 3; j++) { lm[j * 4] = Lx[j] / R; lm[j * 4 + 1] = Ly[j] / R; lm[j * 4 + 2] = Lz[j] / zr; lm[j * 4 + 3] = 0.0f; }
    lm[12] = -cx / R; lm[13] = -cy / R; lm[14] = -z0 / zr; lm[15] = 1.0f;
    smp[0] = smp[1] = 1.0f / (float)n; smp[2] = texel; smp[3] = D * 0.7f;
    smp[4] = D; smp[5] = RE.sm_k; smp[6] = Lz[0]; smp[7] = Lz[1]; smp[8] = Lz[2];
    smp[9] = re_fix() ? 1.0f : 0.0f;   /* (fix: the normal-offset receiver bias) */
    if (re_fix()) smp[3] = D * 0.5f;   /* (fix: fading out from half the reach: the far texels are coarse) */
    /* the casters */
    float m32[32];
    memcpy(m32, ip, 64);
    memcpy(m32 + 16, lm, 64);
    prof_mark(PR_REIM_SM);
    memset(lc, 0, 20 * sizeof(float));
    int cache = RE.v2 && re_x(16, RE.sc_on);
    if (cache && RR.sc_n != RE.sc_size) {
        RR.sc_n = 0; RR.sc_valid = 0;
        if (sm_tex(&RR.sc[0], &RR.sc_dsv[0], &RR.sc_srv[0], RE.sc_size) && sm_tex(&RR.sc[1], &RR.sc_dsv[1], &RR.sc_srv[1], RE.sc_size))
            RR.sc_n = RE.sc_size;
    }
    if (cache && RR.sc_n) {
        /* (reimagine2) the static world's casters go into a light-space cache that keeps them: the game draws only what its
         * camera sees, so a tree canopy above the picture, a wall behind the camera, cast nothing in a per-frame map - their
         * shadows on the ground in view came and went as the camera turned and moved (under trees: "confused where to
         * show"). The cache (sc_size^2, half size Rc = 1.5 R: finer than the per-frame map) is centred on a coarse grid in
         * the light's plane; when the camera's sphere nears its edge it moves by whole grid steps and its content moves
         * with it (sc_move); a new key light or radius starts it again. Static casters are drawn into it every sc_every
         * frames (GEQUAL: what was ever seen stays), the rest (characters, props) into the per-frame map every frame. */
        int nc = RR.sc_n;
        float Rc = ceilf(R * 1.5f / 1024.0f) * 1024.0f, step = Rc / 8.0f;
        float px = cen[0] * Lx[0] + cen[1] * Lx[1] + cen[2] * Lx[2], py = cen[0] * Ly[0] + cen[1] * Ly[1] + cen[2] * Ly[2];
        int fresh = !RR.sc_valid || RR.sc_R != Rc || RR.sc_key[0] * Lz[0] + RR.sc_key[1] * Lz[1] + RR.sc_key[2] * Lz[2] < 0.99999f ||
                    cz - R < RR.sc_z0 + 256.0f || cz + R + E > RR.sc_z0 + RR.sc_zr - 256.0f;
        int drew = 0;
        if (fresh) {
            RR.sc_c[0] = roundf(px / step) * step; RR.sc_c[1] = roundf(py / step) * step;
            RR.sc_z0 = floorf((cz - Rc - 2048.0f) / 256.0f) * 256.0f; RR.sc_zr = 2.0f * Rc + E + 4096.0f;
            RR.sc_R = Rc;
            memcpy(RR.sc_key, Lz, sizeof RR.sc_key);
            unbind_all();
            ID3D11DeviceContext_ClearDepthStencilView(gpu_rt.ctx, RR.sc_dsv[RR.sc_cur], D3D11_CLEAR_DEPTH, 0.0f, 0);
            RR.sc_valid = 1; RR.sc_age = 0; RE.sc_fresh++; drew = 1;
        } else if (fabsf(px - RR.sc_c[0]) > Rc - R || fabsf(py - RR.sc_c[1]) > Rc - R) {
            float c0 = roundf(px / step) * step, c1 = roundf(py / step) * step, tx = 2.0f * Rc / (float)nc;
            sc_move((int)lroundf((c0 - RR.sc_c[0]) / tx), (int)lroundf(-(c1 - RR.sc_c[1]) / tx));
            RR.sc_c[0] = c0; RR.sc_c[1] = c1;
            RE.sc_moves++; drew = 1;
        }
        float* lmc = lc;                 /* (light.hlsl r_lc0..3: world -> the cache's clip; r_scp) */
        for (int j = 0; j < 3; j++) { lmc[j * 4] = Lx[j] / Rc; lmc[j * 4 + 1] = Ly[j] / Rc; lmc[j * 4 + 2] = Lz[j] / RR.sc_zr; lmc[j * 4 + 3] = 0.0f; }
        lmc[12] = -RR.sc_c[0] / Rc; lmc[13] = -RR.sc_c[1] / Rc; lmc[14] = -RR.sc_z0 / RR.sc_zr; lmc[15] = 1.0f;
        lc[16] = 1.0f / (float)nc; lc[17] = 2.0f * Rc / (float)nc; lc[18] = RE.sm_k; lc[19] = 0.0f;
        if (drew || RR.sc_age % (uint64_t)RE.sc_every == 0) {
            float c32[32];
            memcpy(c32, ip, 64);
            memcpy(c32 + 16, lmc, 64);
            unbind_all();
            sm_draw(S, RR.sc_dsv[RR.sc_cur], nc, c32, 1);
        }
        if (RE.log >= 3)
            gpu_log("reim-sc: v%u %s c %.0f %.0f z0 %.0f zr %.0f Rc %.0f p %.0f %.0f cz %.0f age %llu", X.fd ? X.fd->vblank : 0u,
                    fresh ? "fresh" : drew ? "moved" : "keep", RR.sc_c[0], RR.sc_c[1], RR.sc_z0, RR.sc_zr, Rc, px, py, cz,
                    (unsigned long long)RR.sc_age);
        RR.sc_age++;
        unbind_all();
        ID3D11DeviceContext_ClearDepthStencilView(gpu_rt.ctx, RR.sm_dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
        sm_draw(S, RR.sm_dsv, n, m32, 2);
    } else {
        unbind_all();
        ID3D11DeviceContext_ClearDepthStencilView(gpu_rt.ctx, RR.sm_dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
        sm_draw(S, RR.sm_dsv, n, m32, 0);
    }
    unbind_all();
    bind_frame_buffers();
    ID3D11Buffer* b = M.cb;
    ID3D11DeviceContext_VSSetConstantBuffers(gpu_rt.ctx, 6, 1, &b);
    ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 6, 1, &b);
    X.mode = 0;
    M.bound = 0;
    if (RE.log >= 3) gpu_log("reim-map: v%u serial %u ops %d casters %llu R %.0f c %.0f %.0f %.0f pv x%d %.5f %.5f %.5f cam %.0f %.0f %.0f", X.fd ? X.fd->vblank : 0u, S->serial, S->nops, (unsigned long long)RR.sm_draws, R, cx, cy, cz, S->pv_n, S->pv[0], S->pv[5], S->pv[10], ip[8] / ip[11], ip[9] / ip[11], ip[10] / ip[11]);
    RR.sm_frames++;
    if (re_fix()) { memcpy(RR.lm_last, lm, sizeof RR.lm_last); memcpy(RR.smp_last, smp, sizeof RR.smp_last); memcpy(RR.lc_last, lc, sizeof RR.lc_last); RR.have_last = 1; }
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
    reim_finish((reim_scene*)S);         /* (2: the world matrix of the complete frame) */
    int bw[7], bh[7];
    bw[1] = hw; bh[1] = hh;
    for (int i = 2; i <= 6; i++) { bw[i] = (bw[i - 1] + 1) / 2; bh[i] = (bh[i - 1] + 1) / 2; }
    if (!lt_tex(LT_W, hw, hh, DXGI_FORMAT_R32_FLOAT) || !lt_tex(LT_AO0, hw, hh, DXGI_FORMAT_R8_UNORM) ||
        !lt_tex(LT_AO1, hw, hh, DXGI_FORMAT_R8_UNORM) || !rr_tex(RT_SH, hw, hh, DXGI_FORMAT_R8G8_UNORM, 0, 1) || !rr_tex(RT_SH2, hw, hh, DXGI_FORMAT_R8G8_UNORM, 0, 1) || !rr_tex(RT_SHH, hw, hh, DXGI_FORMAT_R8G8_UNORM, 0, 1) ||
        !rr_tex(RT_B6, bw[6], bh[6], DXGI_FORMAT_R11G11B10_FLOAT, 0, 1) ||
        !rr_tex(RT_SHAFT, bw[2], bh[2], DXGI_FORMAT_R11G11B10_FLOAT, 0, 1) ||
        !rr_tex(RT_PIC, pw, ph, DXGI_FORMAT_R8G8B8A8_TYPELESS, 0, 0) ||
        !rr_tex(RT_ST0, 4, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 1) || !rr_tex(RT_ST1, 4, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 1) ||
        !rr_tex(RT_SCENE, hw, hh, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1))
        { prof_mark(pc); return; }
    int nloc = 0;                        /* (local lights) this frame's count (0: the composite skips them) */
    for (int i = 1; i <= 5; i++)
        if (!lt_tex(LT_B1 + i - 1, bw[i], bh[i], DXGI_FORMAT_R11G11B10_FLOAT)) { prof_mark(pc); return; }
    if (!RR.cb) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 40 * 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
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
    if (re_fix() && ao) { lao[0] = LT.ao_r; lao[1] = LT.ao_k; }   /* (fix: Faithful's occlusion radius and strength - the stronger one's noise crawled under motion) */
    lao2[0] = LT.ao_f0; lao2[1] = LT.ao_f1; lao2[2] = RE.ao_min; lao2[3] = LT.ao_pow;
    if (re_fix()) lao2[2] = LT.ao_min;
    lbl[0] = RE.bl_t; lbl[1] = RE.bl_k; lbl[2] = RE.bl_i; lbl[3] = 1.0f;
    ldir[2] = 1.0f / (float)pw; ldir[3] = 1.0f / (float)ph;
    /* reim_cb */
    float rc[160];
    memset(rc, 0, sizeof rc);
    float *rw = rc, *rndc = rc + 16, *rzab = rc + 20, *rkey = rc + 24, *rsss = rc + 28, *rgr = rc + 32, *rfog = rc + 36,
          *rhf = rc + 40, *rvig = rc + 44, *rsh = rc + 48, *rbl = rc + 52, *rmisc = rc + 56, *rkc = rc + 60, *rlm = rc + 64, *rsmp = rc + 80, *rppv = rc + 92, *rpndc = rc + 108, *rptaa = rc + 112, *rks = rc + 116, *rlc = rc + 120;
    int shkey = re_x(4, RE.shaft_key);
    rks[2] = shkey ? 0.0f : -1.0f;       /* (2: no key light in front: no shafts) */
    rks[3] = re_x(8, 1) ? RE.fire_k : 0.0f;
    rptaa[1] = re_x(2, 1) ? RE.cas : 0.0f;   /* (2) the sharpening */
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
        /* (2) the key light's place in the picture (a point at infinity): the light shafts come from there only */
        if (!shkey) rks[2] = -1.0f;
        else {
            float cs = dc[3] / sqrtf(c3);    /* (the cosine to the camera's forward axis) */
            if (cs > 0.02f && rndc[0] != 0.0f && rndc[2] != 0.0f) {
                float u = ((dc[0] / dc[3]) - rndc[1]) / rndc[0] / (float)pw, v = ((dc[1] / dc[3]) - rndc[3]) / rndc[2] / (float)ph;
                float out = fmaxf(fmaxf(-u, u - 1.0f), fmaxf(-v, v - 1.0f));
                float fr = cs < 0.3f ? (cs - 0.02f) / 0.28f : 1.0f;
                rks[0] = fminf(fmaxf(u, -2.0f), 3.0f); rks[1] = fminf(fmaxf(v, -2.0f), 3.0f);
                rks[2] = fr * (out <= 0.0f ? 1.0f : out >= 0.8f ? 0.0f : 1.0f - out / 0.8f);
            }
        }
        float kv[3] = { (dc[0] - o2[0] * dc[3]) / f2[0], (dc[1] - o2[1] * dc[3]) / f2[1], dc[3] };
        float l = sqrtf(kv[0] * kv[0] + kv[1] * kv[1] + kv[2] * kv[2]);
        if (l > 1e-6f) { rkey[0] = kv[0] / l; rkey[1] = kv[1] / l; rkey[2] = kv[2] / l; rkey[3] = RE.ss ? RE.sss_k : 0.0f; }
        memcpy(RE.kv, rkey, sizeof RE.kv);
        memcpy(rkc, RE.keyc, sizeof RE.keyc);
        rkc[3] = RE.key_i;
        if (rl_count() && re_x(32, 1) && rr_tex(RT_LOC, hw, hh, DXGI_FORMAT_R11G11B10_FLOAT, 0, 1) && fabsf(S->ipv[11]) > 1e-20f) {   /* (local lights) */
            float cam[3] = { S->ipv[8] / S->ipv[11], S->ipv[9] / S->ipv[11], S->ipv[10] / S->ipv[11] };
            float ndc[4] = { rndc[1], rndc[0] * (float)pw + rndc[1], rndc[3], rndc[2] * (float)ph + rndc[3] };
            nloc = reim_lights_build(pv, cam, ndc, frame_rate_target_hz ? frame_rate_target_hz() : 60.0);
        }
        RE.posts++;
        if (RE.smap && rkey[3] > 0.0f) {
            int okm = S == &RS && reim_smap(S, rndc, rzab, pw, ph, rlm, rsmp, rlc);
            if (okm) RE.maps++;
            else if (re_fix() && RR.have_last) {   /* (fix: 0.1.1207 drew no map shadows on such frames - a blink) */
                memcpy(rlm, RR.lm_last, sizeof RR.lm_last); memcpy(rsmp, RR.smp_last, sizeof RR.smp_last); memcpy(rlc, RR.lc_last, sizeof RR.lc_last); RE.map_reuse++;
            } else memset(rsmp, 0, 12 * sizeof(float));
        }
        if (RE.log && RE.posts % 600 == 0)
            gpu_log("reim-stats: %llu post passes, %llu maps drawn, %llu reused (no segments %llu, another frame's data %llu), key changes %llu, "
                    "shadow cache %llu new, %llu moves, world matrix: %llu frames not the most common, %llu cuts, %llu frames 0.1.1214 took another",
                    (unsigned long long)RE.posts, (unsigned long long)RE.maps, (unsigned long long)RE.map_reuse,
                    (unsigned long long)RE.map_ops, (unsigned long long)RE.map_fd, (unsigned long long)RE.key_snaps,
                    (unsigned long long)RE.sc_fresh, (unsigned long long)RE.sc_moves, (unsigned long long)RW.switches, (unsigned long long)RW.cuts, (unsigned long long)RW.old_wrong);
    }
    if (re_fix() && have_world && RR.have_prev && RR.prev_serial != S->serial) {   /* (fix: the key-light term's history) */
        memcpy(rppv, RR.ppv, sizeof RR.ppv); memcpy(rpndc, RR.pndc, sizeof RR.pndc); rptaa[0] = 0.9f;
    }
    if (re_fix() && have_world) { memcpy(RR.ppv, S->pv, sizeof RR.ppv); memcpy(RR.pndc, rndc, sizeof RR.pndc); RR.have_prev = 1; RR.prev_serial = S->serial; }
    else RR.have_prev = 0;
    rsss[0] = RE.sss_len; rsss[1] = RE.sss_th; rsss[2] = 2.0f; rsss[3] = 5000.0f;
    if (re_fix() && rsmp[5] > 0.0f) rsss[0] = 0.0f;   /* (fix: the shadow map alone; the contact march crawled under motion) */
    rgr[0] = RE.grade ? RE.sat : 1.0f; rgr[1] = RE.grade ? RE.con : 1.0f; rgr[2] = RE.grade ? RE.vib : 0.0f; rgr[3] = RE.grade ? RE.expo : 1.0f;
    rfog[0] = RE.fog0; rfog[1] = RE.fogd; rfog[2] = RE.fog && have_world ? RE.fogmax : 0.0f; rfog[3] = RE.hfog;
    rhf[0] = RE.hfog_h; rhf[1] = 250.0f;
    rvig[0] = RE.vig; rvig[1] = 0.55f; rvig[2] = 1.15f; rvig[3] = RE.grade ? RE.clar : 0.0f;
    if (re_fix() && rvig[3] > 0.08f && !getenv("PS2_REIM_CLARITY")) rvig[3] = 0.08f;   /* (fix: local contrast amplified every small flicker under motion: a hint only) */
    rsh[0] = RE.shafts ? RE.sh_i : 0.0f; rsh[1] = 0.965f; rsh[2] = 0.85f; rsh[3] = 6000.0f;
    rbl[0] = 1.0f; rbl[1] = RE.halo;
    rmisc[0] = RE.stat_a; rmisc[1] = RE.grade ? RE.night : 0.0f;
    if (re_fix()) {                      /* (fix: the same adaptation time at any frame rate, ~0.7 s) */
        double hz = frame_rate_target_hz ? frame_rate_target_hz() : 60.0;
        rmisc[0] = (float)(1.0 - exp(-1.0 / (0.7 * (hz > 1.0 ? hz : 240.0))));
    }
    rmisc[2] = (float)(RE.dbg == 9 ? (int)((RR.frames / 30u) % 5u) : RE.dbg);   /* (9: the debug views in turn, 30 frames each) */
    rmisc[3] = (float)(RR.frames & 0xFFFF);
    float* rloc = rc + 140;              /* (local lights) light.hlsl r_loc */
    rloc[0] = nloc > 0 ? 1.0f : 0.0f; rloc[1] = 1.0f; rloc[2] = RL.floor;
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
    prof_mark(PR_REIM_SH);
    LT_SIZE(dst, hw, hh); LT_SIZE(src, hw, hh);
    lt_cb(cb, 36);
    bufs[0] = LT.cb;
    ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 7, 2, bufs);
    lt_srv(17, z->srv);
    lt_pass(LT.rtv[LT_W], hw, hh, ms ? GPU_SH_ps_light_lin1 : GPU_SH_ps_light_lin0, off);
    rr_unbind();
    if (nloc > 0) {                      /* (local lights) the game's point lights' light, half res */
        prof_mark(PR_REIM_LL);
        ID3D11Buffer* lb = RL.buf;
        ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 9, 1, &lb);
        lt_srv(14, LT.srv[LT_W]);
        lt_pass(RR.rtv[RT_LOC], hw, hh, GPU_SH_ps_reim_local, off);
        rr_unbind();
        prof_mark(PR_REIM_SH);
    }
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
        if (rptaa[0] > 0.0f) {           /* (fix) the temporal resolve: RT_SH + history -> RT_SH2, kept as RT_SH and the history */
            lt_cb(cb, 36);
            lt_srv(14, LT.srv[LT_W]); lt_srv(25, RR.srv[RT_SH]); lt_srv(26, RR.srv[RT_SHH]);
            lt_pass(RR.rtv[RT_SH2], hw, hh, GPU_SH_ps_reim_shtaa, off);
            rr_unbind();
            ID3D11DeviceContext_CopyResource(gpu_rt.ctx, (ID3D11Resource*)RR.t[RT_SH], (ID3D11Resource*)RR.t[RT_SH2]);
        }
        if (re_fix()) ID3D11DeviceContext_CopyResource(gpu_rt.ctx, (ID3D11Resource*)RR.t[RT_SHH], (ID3D11Resource*)RR.t[RT_SH]);
    } else {
        static const float one[4] = { 1, 0, 0, 0 };   /* (visible, N.L 0: nothing changes) */
        ID3D11DeviceContext_ClearRenderTargetView(gpu_rt.ctx, RR.rtv[RT_SH], one);
    }
    /* bloom: the bright pass + the scene colour (MRT), the pyramid (6 levels), its upsample with the halo weight */
    prof_mark(PR_REIM_BL);
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
    prof_mark(PR_REIM_CO);
    ID3D11DeviceContext_CopyResource(gpu_rt.ctx, (ID3D11Resource*)RR.t[RT_PIC], (ID3D11Resource*)c->pic);
    LT_SIZE(dst, pw, ph); LT_SIZE(src, hw, hh);
    lt_cb(cb, 36);
    lt_srv(14, LT.srv[LT_W]); lt_srv(15, LT.srv[LT_AO0]); lt_srv(16, LT.srv[LT_B1]); lt_srv(17, z->srv); lt_srv(18, stv);
    lt_srv(19, RR.srv[RT_PIC]); lt_srv(20, RR.srv[RT_SH]); lt_srv(21, RR.srv[RT_SHAFT]); lt_srv(22, RR.srv[RT_SCENE]);
    if (nloc > 0) lt_srv(28, RR.srv[RT_LOC]);   /* (local lights) */
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
