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
    R2H_RUN = 0x26,        /* (r2-m2, hybrid) a run of native entries: arg = slot (4 bits) | run index << 4, value = the
                              slot's frame serial; the entries' data are in the scene slot (r2_slot), not in the stream */
};
/* R2H_DRAW / R2H_BEGIN words, in order: [fade (R2D_FADE)] [world matrix, 16 words (unless R2D_IDENTITY)] [the mesh DMA
 * address | form << 31 (offline replays resolve the handle from a RAM dump with it)] */
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

/* ---- scene slots (r2-m2): a ring of per-frame records shared by the EE thread (writer) and the GPU frontend (reader).
 * Hybrid mode puts each native entry's per-frame data here instead of into the DMA chain: the stream carries one R2H_RUN
 * hint per run of native entries. The EE opens slot serial % R2_SLOTS at the frame's first entry (serial 0 while it
 * writes) and publishes it at the frame's end (serial, release order); the chain is kicked only after that, so the
 * frontend always finds the frame complete (it checks the serial before and after copying: a slot reused while it reads
 * counts as stale and the run is dropped). Bracket mode keeps the inline words (recordings replay offline). */
#define R2_SLOTS 16u
#define R2_SLOT_ENTS 4096u
#define R2_SLOT_RUNS 1024u
typedef struct {                 /* 80 bytes */
    float m[16];                 /* world x clip (W x C, the VU's 0x0070 arithmetic) */
    float fade;                  /* VU 8.x */
    uint32_t handle;             /* mesh */
    uint32_t addr;               /* the mesh DMA | form << 31 (diagnostics) */
    uint32_t flags;              /* R2E_* */
} r2_slot_ent;
enum { R2E_CLIP = 1 };           /* the mesh's bounds cross the VU's guard band: clipped triangles on the GPU */
typedef struct {
    volatile uint32_t serial;    /* (atomic: 0 while the EE writes it) */
    uint32_t nent, nrun;
    r2_frame_block fb;
    uint32_t run_first[R2_SLOT_RUNS], run_count[R2_SLOT_RUNS];
    r2_slot_ent ent[R2_SLOT_ENTS];
} r2_slot;
const r2_slot* r2_slot_at(uint32_t index);       /* NULL: not allocated */

/* ---- decoded meshes (r2_mesh.c) ------------------------------------------------------------------------------------- */
typedef struct {                 /* 32 bytes, also the GPU vertex layout (shaders/r2.hlsl R2V) */
    float x, y, z;
    uint32_t wbits;              /* the input position's w word: ~2^23 float, mantissa bits 4..11 fog base, bit 15 ADC */
    int16_t u, v;                /* 4.12 */
    uint32_t col[2];             /* colour A, colour B: R | G << 8 | B << 16 | A << 24 (V4-5 << 3, alpha from S-8) */
    uint32_t batch;              /* (r2-m2) the sub-batch of the mesh it belongs to (its materials: shaders/r2.hlsl) */
} r2_vtx;

typedef struct {                 /* one pass (material packet) of a sub-batch */
    uint64_t tex0, tex1, clamp, test, alpha, fogcol;
    uint32_t prim;               /* PRIM bits of the packet's vertex GIF tag */
    uint8_t csel;                /* 0: RGBAQ from colour A (REGS 4F12), 1: colour B (41F2) */
    /* (r2-m2) classified once at decode (r2_pass_classify): */
    uint8_t bl;                  /* blend class R2G_BLEND_* (r2gpu.h; the exact path's FF classes), 0xFF: unsupported */
    uint8_t texm;                /* 1 nearest, 2 bilinear (TEX1 with LCM 1, K: MMAG or MMIN) */
    uint8_t nocolor;             /* the blend writes no colour (A == B, D == Cd): only the alpha byte */
    uint32_t mflags;             /* the material's static GPU_R2D_* bits (gpu_front.h): CSEL ABE FGE TCC TEXM */
    uint32_t alpha_enc;          /* ALPHA as the shader reads it: A | B << 2 | C << 4 | D << 6 | FIX << 8 */
} r2_pass;
void r2_pass_classify(r2_pass* P);

typedef struct {                 /* a sub-batch: one triangle strip, drawn with each pass in turn */
    uint32_t vfirst, vcount;     /* vertices in the mesh's array */
    uint32_t ifirst, icount;     /* its triangles (indices into the mesh's array; ADC triangles left out; every triangle
                                    with the winding of the strip's first) */
    float fog_scale, fog_wmax;   /* VU 9.y, 9.z of its header */
    uint8_t npass, cull;         /* cull: the VU's backface test is on ([11].x & 0x80 == 0) */
    uint8_t amin[2], amax[2];    /* vertex alpha range of colour A, colour B (the alpha test's outcome per draw) */
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
    uint32_t gpu_mbase;          /* (r2-m2) its material records (3 per sub-batch: pass 0..2) in the generation's arena */
} r2_mesh;

/* decode result codes (fallback reasons, counted) */
enum { R2M_OK, R2M_TAG, R2M_VIF, R2M_MSCAL, R2M_HEADER, R2M_DESC, R2M_PACKET, R2M_VERTEX, R2M_SIZE, R2M_FADE, R2M_N };
extern const char* const r2m_reason[R2M_N];

/* Decode the chain at addr (form 0 / 1) from EE memory ram (32 MB); *out is malloc'ed (r2_mesh_free). Returns R2M_*. */
int r2_mesh_decode(const uint8_t* ram, uint32_t addr, int form, r2_mesh** out);
void r2_mesh_free(r2_mesh* m);

/* ---- the mesh store (r2_mesh.c): handle -> mesh, per level generation (ring of 4) ------------------------------- */
const r2_mesh* r2_mesh_get(uint32_t handle);   /* any thread; NULL when gone */
uint32_t r2_store_gen(void);
uint32_t r2_store_new_gen(void);               /* the writer: a new level; the generation 4 back is freed */
uint32_t r2_store_add(r2_mesh* m);             /* the writer: published, its handle (0: full) */
int r2_store_put(uint32_t handle, r2_mesh* m); /* (offline replays) at a recorded handle */
/* (offline replays) a handle the store does not know: decode the mesh at addr / form from the replay's RAM (NULL) */
extern const r2_mesh* (*r2_mesh_resolve)(uint32_t handle, uint32_t addr, int form);

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
void r2_matmul(const float w[16], const float c[16], float out[16]);   /* out = W x C (row vectors), VU-exact (0x0070) */
void r2_matmul_fast(const float w[16], const float c[16], float out[16]);   /* the same through the MXCSR (r2-m2) */
/* the VU's FMAC operations (round toward zero, flush to zero), the references of shaders/r2_vu.hlsli */
float r2_vu_mul(float a, float b);
float r2_vu_add(float a, float b);
float r2_vu_rcp(float w);                 /* DIV Q, 1.0, w */
/* The VU's float-bits screen transform as a linear one (GPU vertex shader): GS X (12.4) = bx + ax * x/w, Y likewise,
 * GS Z = bz + az * z/w, inside the ranges the VU produces for visible vertices. 0: the constants do not have that form
 * (R2 then stays off for the frame). */
typedef struct { float ax, bx, ay, by, az, bz; } r2_screen_model;
int r2_screen_model_of(const r2_frame_block* fb, r2_screen_model* m);
/* the header of a mesh DMA entry (form 1, the entry's CALL #1): walks it like r2_mesh_decode and returns the matrix
 * (VU 0..3) and VU 8 it uploads (1), or 0 when it does something else */
int r2_header_parse(const uint8_t* ram, uint32_t addr, float w[16], float vu8[4]);

/* ---- EE side (r2_capture.c, called from recomp/patches.py patches) --------------------------------------------- */
struct ps2_ctx;
void r2_cap_alloc(struct ps2_ctx* ctx);   /* render-list allocator returns: $v0 = entry */
void r2_cap_entry(struct ps2_ctx* ctx);   /* before 0x158378: $s2 = entry, $s0 = priority, $s1 = class */
void r2_cap_done(struct ps2_ctx* ctx);    /* before 0x1583FC: frame end */
enum { R2_OFF, R2_HYBRID, R2_BRACKET };
extern volatile int r2_mode;              /* effective mode for the next frame (r2_set_mode) */
void r2_set_mode(int mode);               /* settings / renderer start-stop; R2_OFF when the GPU renderer is not running */
void r2_stats_line(char* buf, size_t n);  /* per-second statistics (PS2_R2_STATS) */
