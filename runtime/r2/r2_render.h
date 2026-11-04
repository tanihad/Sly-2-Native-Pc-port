/* The modern renderer's segment executor (r2_render.c), called by the exact path's frame executor (gpu/gpu_exec.c) for
 * GPU_FOP_R2 on the render thread, with the exact path's own device, targets and views as native handles. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "gpu_front.h"

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
    /* (r2-m7b) the decoded texture cache (gpu_exec.c td_r2): a material texture's decoded word + 1 (0: none), its buffer (t13) */
    uint32_t (*texdec)(void* user, uint64_t tex0);
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
} r2_exec_in;
enum { R2X_BATCH = 1, R2X_STATE = 2 };

/* the segment's draws; returns how many were issued. The host re-establishes its own bindings afterwards. */
int r2_render_segment(const gpu_fop_r2* o, const uint8_t* fdata, size_t fdata_bytes, const r2_exec_in* in);
void r2_render_release(void);            /* device loss, stop: every GPU resource (meshes upload again) */
void r2_render_stats(uint64_t* draws, uint64_t* segments, uint64_t* uploads, uint64_t* upload_bytes, uint64_t* missing);
