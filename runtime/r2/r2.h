/* R2, the modern renderer (notes/modern-renderer.md): the game's own prelit meshes drawn natively through a modern GPU
 * pipeline, the exact GS path drawing everything else into the same targets (a "hybrid frame").
 *
 * Milestone R1 covers the static world tree's leaves (render-list entries written by 0x1D5808) of the levels on an
 * allowlist (the ep3 hub). Data flow:
 *
 *   EE thread    r2_capture.c: hooks in the game's render-list emission (f_00157FA0, recomp/patches.py "r2m_*")
 *                classify each entry; a native entry's prebuilt mesh DMA is decoded once (r2_mesh.c) into a mesh
 *                record, the entry's CALL is dropped and NATIVE_DRAW hints (GS register 0x7F, gs_hint.h) carry the
 *                mesh handle and the entry's per-frame inputs through the game's own DMA chain
 *   GS thread    gs_draw.c hint_write: R2 hints go to the frontend hook gs_front->r2 (the proxy ships them in stream
 *                order to the frontend thread, gpu_fthread.c)
 *   frontend     gpu_front.c gf_r2_*: a run of consecutive native draws becomes one GPU_FOP_R2 segment: targets of the
 *                current FRAME / ZBUF, ownership (stale blocks loaded, blocks GPU-written afterwards), texture pages
 *                current in vram_buf, the draw records in the frame's data arena
 *   render       r2_render.c: the segment's draws on the exact path's own RTV / DSV (r2gpu over the shared D3D11
 *                device), in the game's order, Original materials (shaders/r2.hlsl)
 *
 * Modes (PS2_R2 overrides [graphics] renderer_mode): off (the exact path only: the GS stream is the game's own),
 * hybrid (native entries drawn by R2, their CALLs dropped), bracket (verification: the hints bracket each native entry's
 * CALLs, which stay; the frontend ignores them or checks the exact primitives against R2's vertex function, PS2_R2_CHECK). */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* ---- hints (GS register 0x7F; gs_hint.h kinds 0x20..0x2F) --------------------------------------------------------------
 * The 64-bit payload: bits 0-7 kind, 8-31 arg (24 bits), 32-63 value. */
enum {
    R2H_FRAME = 0x20,      /* arg = number of R2H_DATA words that follow (the frame block, r2_frame_block); value = frame serial */
    R2H_DRAW = 0x21,       /* a native entry: arg = flags (R2D_*), value = mesh handle; R2H_DATA words follow (r2_entry_block) */
    R2H_DATA = 0x22,       /* one 32-bit payload word (value); arg = index of the word in its block */
    R2H_BEGIN = 0x23,      /* bracket mode: the exact primitives of a native-capable entry follow; value = mesh handle */
    R2H_END = 0x24,        /* bracket mode: they end */
    R2H_EPILOGUE = 0x25,   /* (diagnostic) the state epilogue of a run follows */
};
enum { R2D_IDENTITY = 1,   /* the entry's world matrix is the identity (no matrix words) */
       R2D_FADE = 2 };     /* a fade word follows (VU 8.x != 1.0) */

static inline uint64_t r2h_make(uint32_t kind, uint32_t arg, uint32_t value) {
    return (uint64_t)(kind & 0xFF) | ((uint64_t)(arg & 0xFFFFFF) << 8) | ((uint64_t)value << 32);
}
static inline int r2h_is(uint64_t v) { uint32_t k = (uint32_t)(v & 0xFF); return k >= 0x20 && k <= 0x2F; }

/* the per-frame block (R2H_FRAME + its words): the VU1 constants every prelit transform reads */
typedef struct {
    float c0[4];           /* VU 0: screen / depth scale (x, y, z; w unused here) */
    float c1[4];           /* VU 1: screen / depth offset (float-bits trick: XYZF2 takes the raw bits) */
    float clip[16];        /* VU 2..5: world -> clip, rows (p' = x * row0 + y * row1 + z * row2 + row3) */
} r2_frame_block;
#define R2_FRAME_WORDS ((uint32_t)(sizeof(r2_frame_block) / 4))

/* ---- decoded meshes (r2_mesh.c) ------------------------------------------------------------------------------------- */
typedef struct {                 /* 32 bytes, also the GPU vertex layout (shaders/r2.hlsl R2V) */
    float x, y, z;
    uint32_t wbits;              /* the input position's w word: ~2^23 float, mantissa bits 4..11 fog base, bit 15 ADC */
    int16_t u, v;                /* 4.12 */
    uint32_t col[2];             /* colour A, colour B: R | G << 8 | B << 16 | A << 24 (V4-5 << 3, alpha from S-8) */
    uint32_t pad;
} r2_vtx;

typedef struct {                 /* one pass (material packet) of a sub-batch */
    uint64_t tex0, tex1, clamp, test, alpha, fogcol;
    uint32_t prim;               /* PRIM bits of the packet's vertex GIF tag */
    uint8_t csel;                /* 0: RGBAQ from colour A (REGS 4F12), 1: colour B (41F2) */
    uint8_t pad[3];
} r2_pass;

typedef struct {                 /* a sub-batch: one triangle strip, drawn with each pass in turn */
    uint32_t vfirst, vcount;     /* vertices in the mesh's array */
    uint32_t ifirst, icount;     /* its triangles (indices into the mesh's array; ADC triangles left out; every triangle
                                    with the winding of the strip's first) */
    float fog_scale, fog_wmax;   /* VU 9.y, 9.z of its header */
    uint8_t npass, cull;         /* cull: the VU's backface test is on ([11].x & 0x80 == 0) */
    uint8_t pad[2];
    r2_pass pass[3];
} r2_batch;

typedef struct r2_mesh {
    uint32_t handle;             /* gen << 20 | index */
    uint32_t addr, bytes;        /* the DMA chain decoded, the bytes it reads */
    uint64_t hash;               /* of those bytes */
    int form;                    /* 0: whole static packet (header inside), 1: mesh DMA (header from the entry) */
    int has_w;                   /* the chain itself uploads the world matrix and VU 8 (form 0) */
    float w[16], fade;           /* (has_w) */
    float bmin[3], bmax[3];
    r2_vtx* vtx;
    uint32_t nvtx;
    uint32_t* idx;
    uint32_t nidx;
    r2_batch* b;
    uint32_t nb;
    uint32_t last_pass;          /* index into b[nb-1].pass of the last pass drawn (state epilogue) */
    /* render thread (r2_render.c): residency in the generation's arena */
    volatile int gpu_state;      /* 0 not uploaded, 1 resident, -1 failed */
    uint32_t gpu_vbase, gpu_ibase;
} r2_mesh;

/* decode result codes (fallback reasons, counted) */
enum { R2M_OK, R2M_TAG, R2M_VIF, R2M_MSCAL, R2M_HEADER, R2M_DESC, R2M_PACKET, R2M_VERTEX, R2M_SIZE, R2M_FADE, R2M_N };
extern const char* const r2m_reason[R2M_N];

/* Decode the chain at addr (form 0 / 1) from EE memory ram (32 MB); *out is malloc'ed (r2_mesh_free). Returns R2M_*. */
int r2_mesh_decode(const uint8_t* ram, uint32_t addr, int form, r2_mesh** out);
void r2_mesh_free(r2_mesh* m);

/* ---- the mesh store (r2_capture.c): handle -> mesh, per level generation ----------------------------------------- */
const r2_mesh* r2_mesh_get(uint32_t handle);   /* any thread; NULL when gone */
uint32_t r2_store_gen(void);

/* ---- vertex function (r2_vertex.c): the C twin of shaders/r2.hlsl's vertex shader (the in-game check, offline tools) */
typedef struct {
    int32_t x, y;                /* GS 12.4 (XYOFFSET not subtracted) */
    uint32_t z;                  /* GS Z */
    uint32_t f;                  /* fog */
    float s, t, q;
    uint32_t rgba;
    int adc;                     /* input ADC (the triangle ending here is not drawn) */
} r2_gsv;
/* the VU's output for vertex v of a batch: m = the 4x4 (rows) W x C, fb = frame constants, fade = VU 8.x */
void r2_vertex_eval(const r2_vtx* v, const float m[16], const r2_frame_block* fb, const r2_batch* b, int csel,
                    float fade, r2_gsv* o);
void r2_matmul(const float w[16], const float c[16], float out[16]);   /* out = W x C (row vectors) */

/* ---- EE side (r2_capture.c, called from recomp/patches.py patches) --------------------------------------------- */
struct ps2_ctx;
void r2_cap_alloc(struct ps2_ctx* ctx);   /* render-list allocator returns: $v0 = entry */
void r2_cap_entry(struct ps2_ctx* ctx);   /* before 0x158378: $s2 = entry, $s0 = priority, $s1 = class */
void r2_cap_done(struct ps2_ctx* ctx);    /* before 0x1583FC: frame end */
enum { R2_OFF, R2_HYBRID, R2_BRACKET };
extern volatile int r2_mode;              /* effective mode for the next frame (r2_set_mode) */
void r2_set_mode(int mode);               /* settings / renderer start-stop; R2_OFF when the GPU renderer is not running */
void r2_stats_line(char* buf, size_t n);  /* per-second statistics (PS2_R2_STATS) */
