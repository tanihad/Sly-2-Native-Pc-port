/* The modern renderer's segment executor (r2_render.c), called by the exact path's frame executor (gpu/gpu_exec.c) for
 * GPU_FOP_R2 on the render thread, with the exact path's own device, targets and views as native handles. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "gpu_front.h"

/* (tex) how a material uses its texture (Enhanced textures, runtime/gpu/gpu_tex.h): CLAMP, TEX1, TEST, the GPU_R2D_* flags
 * and the pass (0 base, 1 highlight, 2 third) */
typedef struct r2_tex_use { uint64_t clamp, tex1, test; uint32_t mflags, pass; } r2_tex_use;

typedef struct {
    void* dev;                           /* ID3D11Device* */
    void* ctx;                           /* ID3D11DeviceContext* (immediate) */
    void* rtv;                           /* the segment's colour target (RGBA8 UNORM view of the raw GS bytes) */
    void* dsv;                           /* its depth target (z / 2^20) */
    int kx, ky;                          /* their sample factors */
    void* vram_srv;                      /* t0: vram_buf (raw) */
    void* swz_srv;                       /* t6: swizzle tables */
    void* fdata_srv;                     /* t7: the frame's data arena (raw) */
    void* iota_vb;                       /* draw ids: element i holds i */
    float dual_bias;                     /* dual-source blend source bias (gpu_exec.c, per GPU) */
    /* (r2-m7b) the decoded texture cache (gpu_exec.c td_r2): a material texture's decoded word + 1 (0: none), its buffer (t13);
     * (tex) the pass's other registers, for Enhanced textures' classification (gpu_tex.h) */
    uint32_t (*texdec)(void* user, uint64_t tex0, const struct r2_tex_use* use);
    void* texdec_user;
    void* texdec_srv;
    /* (r2-m8, R8) a modern frame's targets (gpu_exec_mft.h): rtv / dsv are the MFT surfaces, d3d = (GS + 0.5) * (sx, sy),
     * hardware depth (no SV_Depth), msaa samples (> 1: alpha-to-coverage for pass-A cut-outs) */
    int mft, msaa;
    int mv;                              /* (r2-m8) the motion-vector pass: pass-A draws through r2_vs_mv / r2_ps_mv, depth GEQUAL */
    float sx, sy;
    /* (r8-perf) the GPU optimisations of modern frames on (R2X_* bits; gpu_exec.c r8p_on), and diagnostics */
    int opt;
    int dbg_cull;                        /* (diagnostics, PS2_R8P_SKIP) every draw issued, nothing rasterized (empty scissor) */
    /* (light) Enhanced lighting (modern frames): lit sub-batches lit per pixel (r2.hlsl R2_ENH), rim light strength */
    int enh;
    float rim;
    /* (reimagine) Look = Reimagined: the R2_ENH=2 pixel shaders (specular of strength spec, the rim, the normal hook) */
    int reim;
    float spec;
    float reim_k[8];                     /* (reimagine) r2.hlsl r2_reim, r2_reim2: the key light on screen, relief, cavity, normal maps */
    /* (reimagine) the shadow-caster pass: the segment's pass-A draws that write Z, depth only, into dsv (a size x size
     * map) through r2.hlsl r2_vs_shadow with these 32 floats (inverse world x clip, world -> light clip) */
    int shadow, shadow_size;
    const float* shadow_m;
    int shadow_at;                       /* (reimagine2) cut-out draws through r2_vs/ps_shadow_at (their texture's alpha test) */
    int shadow_sel;                      /* (reimagine2) 0: every caster; 1: the static world's only (entry matrix == shadow_pv);
                                            2: the others only (characters, props: the per-frame map) */
    const float* shadow_pv;              /* (reimagine2) the frame's world x clip (16 floats) */
    /* (tex) Enhanced textures (modern frames): the R2_ETEX pipelines; etex_bind binds their resources (after the
     * materials' texdec calls, which may build images) */
    int etex;
    void (*etex_bind)(void);
    /* (shadows) shadow_sel 3: the lit entries only (a light block: the game lights them in the VU - characters, lit props),
     * drawn from the camera into the game-lit mask (gpu_reim.h); shadow_h: the target's height (0: shadow_size, square) */
    int shadow_h;
} r2_exec_in;
enum { R2X_BATCH = 1, R2X_STATE = 2 };

/* the segment's draws; returns how many were issued. The host re-establishes its own bindings afterwards. */
int r2_render_segment(const gpu_fop_r2* o, const uint8_t* fdata, size_t fdata_bytes, const r2_exec_in* in);
void r2_render_release(void);            /* device loss, stop: every GPU resource (meshes upload again) */
void r2_render_stats(uint64_t* draws, uint64_t* segments, uint64_t* uploads, uint64_t* upload_bytes, uint64_t* missing);
