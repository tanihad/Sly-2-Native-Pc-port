/* R2, the modern renderer (notes/modern-renderer.md): the game's own prelit meshes drawn natively through a modern GPU
 * pipeline, the exact GS path drawing everything else into the same targets (a "hybrid frame").
 *
 * Milestone R1 covered the static world tree's leaves (render-list entries written by 0x1D5808) of the ep3 hub; R2
 * (r2-m2) every prelit mesh - static world and glob props (objects with cel outlines stay exact) - in every level,
 * meshes crossing the VU's guard band GPU-clipped, draws consolidated; R3 / R4 (r2-m3) lit and skinned meshes (light
 * block + bones as per-entry extra qwords, skinning and lighting in the vertex shader) and the outline entries (the
 * outline program in C, r2_outline.c: its packets decoded by the GS thread at the entry's place). Data flow:
 *
 *   EE thread    r2_capture.c: hooks in the game's render-list emission (f_00157FA0, recomp/patches.py "r2m_*")
 *                classify each entry; a native entry's prebuilt mesh DMA is decoded once (r2_mesh.c) into a mesh
 *                record, the entry's CALL is dropped, its per-frame inputs (W x C, fade) go into the frame's scene slot
 *                (r2_slot) and one R2H_RUN hint per run of native entries (GS register 0x7F, gs_hint.h) marks the
 *                place in the game's own DMA chain (bracket mode: the inputs inline, recordings replay offline)
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
    R2H_XQ = 0x27,         /* (r2-m3, bracket) an extra block: arg = its index in the frame (0 starts a frame's set),
                              value = qwords; 4 x value R2H_DATA words follow. R2D_EXTRA entries name it by index */
    R2H_OUTLINE = 0x28,    /* (r2-m3, hybrid) a native outline entry: arg = slot (4 bits) | qword offset << 4 in the slot's pool
                              (there: the packets' qword count, then the GIF packets, r2_outline.c), value = serial; the GS
                              thread decodes the packets in place (gs_draw.c) */
    R2H_PART = 0x29,       /* (r2-m6, R6) the particle class (render class 2, VU1 program 376109499) begins (arg 1) or ends
                              (arg 0); value = frame serial. Hybrid and bracket mode: the frontend draws the particles' exact
                              triangles in that span through the native particle pipeline (gpu_front.c "part") */
    R2H_MARK = 0x2A,       /* (r2-m8, R8) a point of the frame: arg 1 = the 2D class (render class 5, prio 21: HUD, text) begins
                              (its first entry, after the class switch's clears); value = frame serial. Hybrid mode only. The
                              frontend emits GPU_FOP_MARK; the executor resolves a modern frame's picture there (HUD after the
                              resolve, gpu_exec_mft.h) */
};
/* R2H_DRAW / R2H_BEGIN words, in order: [fade (R2D_FADE)] [world matrix, 16 words (unless R2D_IDENTITY)] [the mesh DMA
 * address | form << 31 (offline replays resolve the handle from a RAM dump with it)] */
enum { R2D_IDENTITY = 1,   /* the entry's world matrix is the identity (no matrix words) */
       R2D_FADE = 2,       /* a fade word follows (VU 8.x != 1.0) */
       R2D_CLIP = 4,       /* (r2-m2) the mesh crosses the VU's guard band (R2E_CLIP) */
       R2D_EXTRA = 8,      /* (r2-m3) after the address word: the index of the frame's R2H_XQ block (extra qwords) */
       R2D_FANS = 16 };    /* (r2-m5b) bracket A/B: the entry's exact triangle fans (the VU clipper's) stay drawn */

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
typedef struct {                 /* 96 bytes */
    float m[16];                 /* world x clip (W x C, the VU's 0x0070 arithmetic) */
    float fade;                  /* VU 8.x */
    uint32_t handle;             /* mesh */
    uint32_t addr;               /* the mesh DMA | form << 31 (diagnostics) */
    uint32_t flags;              /* R2E_* */
    uint32_t xoff, xn;           /* (r2-m3) lit / skinned: the entry's extra qwords in the slot's pool (r2_lit_block) */
    uint32_t pad[2];
} r2_slot_ent;
#define R2_SLOT_XQ 32768u        /* (r2-m3) extra qwords per slot (light blocks, bone palettes) */
enum { R2E_CLIP = 1,             /* the mesh's bounds cross the VU's guard band: R2 draws its triangles inside it (r2_gs_clip) */
       R2E_FANS = 2,             /* (r2-m5b) and the VU's clipper draws the others (r2_clip.c packets after the entry) */
       R2E_RUNS = 4 };           /* (r2-m5b) a run table (slot qwords at ent.pad[0] - 1): R2 draws only the triangles the VU draws as strips */
typedef struct {
    volatile uint32_t serial;    /* (atomic: 0 while the EE writes it) */
    uint32_t nent, nrun;
    r2_frame_block fb;
    uint32_t run_first[R2_SLOT_RUNS], run_count[R2_SLOT_RUNS];
    r2_slot_ent ent[R2_SLOT_ENTS];
    uint32_t nxq;                /* (r2-m3) */
    float xq[R2_SLOT_XQ][4];
} r2_slot;
const r2_slot* r2_slot_at(uint32_t index);       /* NULL: not allocated (r2_mesh.c) */
r2_slot* r2_slot_ring(void);                     /* the writer (EE): the ring, allocated on first use */

/* ---- decoded meshes (r2_mesh.c) ------------------------------------------------------------------------------------- */
typedef struct {                 /* 48 bytes, also the GPU vertex layout (shaders/r2.hlsl R2V) */
    float x, y, z;               /* (r2-m3) skinned: the input's rest position */
    uint32_t wbits;              /* the input position's w word: ~2^23 float, mantissa bits 4..11 fog base, bit 15 ADC */
    int16_t u, v;                /* 4.12 */
    uint32_t col[2];             /* colour A, colour B: R | G << 8 | B << 16 | A << 24 (V4-5 << 3, alpha from S-8) */
    uint32_t batch;              /* (r2-m2) the sub-batch of the mesh it belongs to (its materials: shaders/r2.hlsl) */
    /* (r2-m3) lit / skinned sub-batches (the VU's 0x1050 / 0x16B0 / 0x1940 inputs of the vertex's source record) */
    uint32_t nrm;                /* normal, V4-8 signed: x | y << 8 | z << 16 | w << 24 (bytes) */
    uint32_t lcol;               /* colour V4-8: R | G << 8 | B << 16 | A << 24 */
    uint32_t wts;                /* bone weights V4-8 (unsigned bytes) */
    uint32_t pad;
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
    uint8_t lit;                 /* (r2-m3) 1: colours from the light set (0x1050) */
    uint8_t skin;                /* (r2-m3) 0 rigid, 1 positions skinned (0x16B0), 2 positions + normals (0x1940) */
    uint16_t bone[4];            /* (r2-m3) skin: the 4 bones' header qword offsets ([12]: 4 rows each) */
    uint16_t tanim;              /* (r2-m3) 0x0D28's per-frame qword ([15].z) in the header: UV offset (x, y ITOF12) */
    uint8_t wide;                /* (r2-m3) colour words above 255 (only the low byte reaches the GS unless faded) */
    uint8_t pad2;
    float bright;                /* (r2-m3) lit: ITOF12 of [16].x (added to the light block's ambient w) */
    r2_pass pass[3];
    uint32_t vu9[4];             /* (r2-m5b) the header's [9] (fog offset / scale / w max: the clipper's projection) */
    uint32_t pk[2][8][4];        /* (r2-m5b) pass 0 / 1 material packets as in VU memory from their qword 1: the fan GIF tag
                                    template, the A+D packet (r2_clip.c runs the VU's clipper on them) */
} r2_batch;

/* (r2-m5) the RAM a decoded chain read (its DMA tags and data, a copy): the game rewrites mesh DMA in place while a
 * level streams in, so a cached decode is used only while r2_src_same holds (r2_capture.c, every use) */
typedef struct r2_src r2_src;
int r2_src_same(const r2_src* s, const uint8_t* ram);
void r2_src_free(r2_src* s);
uint64_t r2_mesh_hash(const uint8_t* ram, uint32_t addr);   /* (diagnostics) m->hash of the chain at addr now */
extern uint32_t r2_hash_bytes;                               /* the bytes it walked */

typedef struct r2_mesh {
    uint32_t handle;             /* gen << 20 | index */
    r2_src* src;                 /* (r2-m5) its source bytes */
    uint32_t flips;              /* (r2-m5) VIF1 double-buffer flips of the chain (its MSCALs) mod 2 */
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
    /* (r2-m3) lit / skinned meshes: the entry carries a light block and a bone palette (r2_lit_block) */
    int lit;                     /* some sub-batch is lit or skinned */
    int any_lit, any_skin, any_wide;
    uint32_t nbones;             /* (unused) */
    uint32_t hq_hi;              /* the entry's extra data: the header's qwords R2_HQ_LO .. hq_hi - 1 (bones, UV offsets) */
    uint16_t lpos_off;           /* 0x0298's object position: [11].w and [12].x of the first buffer (point lights) */
    uint16_t lpos_bone;
    /* render thread (r2_render.c): residency in the generation's arena */
    volatile int gpu_state;      /* 0 not uploaded, 1 resident, -1 failed */
    uint32_t gpu_vbase, gpu_ibase;
    uint32_t gpu_mbase;          /* (r2-m2) its material records (3 per sub-batch: pass 0..2) in the generation's arena */
    uint32_t* gpu_tdec;          /* (r2-m7b) per material record: the decoded texture word it holds (r2_render.c), or NULL */
    uint32_t gpu_tseg;           /* (r2-m7b) the segment that last checked them */
} r2_mesh;

/* decode result codes (fallback reasons, counted) */
enum { R2M_OK, R2M_TAG, R2M_VIF, R2M_MSCAL, R2M_HEADER, R2M_DESC, R2M_PACKET, R2M_VERTEX, R2M_SIZE, R2M_FADE, R2M_N };
extern const char* const r2m_reason[R2M_N];

/* Decode the chain at addr (form 0 / 1) from EE memory ram (32 MB); *out is malloc'ed (r2_mesh_free). Returns R2M_*. */
int r2_mesh_decode(const uint8_t* ram, uint32_t addr, int form, r2_mesh** out);
void r2_mesh_free(r2_mesh* m);
/* (r2-m3, oracle) the lit routine ra's sub-batches in a VU1 data memory image (1024 qw) with TOP = top */
int r2_lit_decode_vu(const uint32_t (*vu)[4], uint32_t top, uint32_t ra, r2_mesh** out);

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
/* (r2-m3) after a successful r2_header_parse: the header buffer's qwords 0..n-1 as uploaded (NULL when not written) */
const uint32_t* r2_header_qw(uint32_t i);

/* ---- (r2-m3) lit / skinned meshes (the main program's 0x0298 / 0x1050 / 0x16B0 / 0x1940; notes/wip/r2-m3.md) ---------
 * The light block of an entry: 0x0298's result ([21..30] of the header buffer: the 4 lights' directions in object space
 * transposed and / 127, colours x z x 2, ambient sum x 2, thresholds -y / z) and VU 10 (the frame's fog / brightness
 * qword): R2_LIGHT_QW quadwords. The bone palette follows it in the entry's extra data: the header's qwords 0x21 ..
 * 0x21 + 4 nbones (4 rows per bone, object -> world). */
#define R2_LIGHT_QW 11
#define R2_BONES_MAX 64
#define R2_HQ_LO 0x15u           /* an entry's extra data = light block (R2_LIGHT_QW qw), then header qwords 0x15 .. hq_hi */
#define R2_HQ_MAX 0x140u
/* hdr(i) = the header buffer's qword i (r2_header_qw); 0: the light set is not there (the entry stays exact) */
int r2_lights_prep(const uint32_t* (*hdr)(uint32_t), const r2_mesh* m, const float vu10[4], float out[R2_LIGHT_QW][4]);
/* the vertex's position (skinned when the sub-batch is) and colour A / B (lit: 0x1050), as the VU computes them (round
 * toward zero, the microcode's order): the C twin of shaders/r2.hlsl's lit path. hq = the header's qwords from R2_HQ_LO. */
void r2_lit_eval(const r2_vtx* v, const r2_batch* b, const float lb[R2_LIGHT_QW][4], const float (*hq)[4], float pos[3],
                 uint32_t col[2]);

/* ---- (r2-m3, R4) the outline program (188054331) in C (r2_outline.c): routine addr (0x0738 set-up: *x = 0x24B - TOP
 * on entry; 0x07F0 a batch: *x = the set-up's buffer) on the VU1 data memory image vu (modified as the VU would), the
 * GIF packets it XGKICKs appended to out (cap bytes, *nout used). 0: unknown routine or out full. */
int r2_outline_run(uint32_t (*vu)[4], uint32_t addr, uint32_t* x, uint8_t* out, uint32_t cap, uint32_t* nout);
/* an outline entry's mesh chain (after its header) as a script of VU writes and MSCALs (r2_mesh.c) */
typedef struct r2_oscript r2_oscript;
int r2_outline_decode(const uint8_t* ram, uint32_t hdr, uint32_t mesh, r2_oscript** out);
void r2_outline_free(r2_oscript* s);
uint32_t r2_outline_flips(const r2_oscript* s);  /* (r2-m5) the chain's MSCALs mod 2 (VIF1 DBF flips a dropped chain owes) */
int r2_outline_same(const r2_oscript* s, const uint8_t* ram);   /* (r2-m5) the mesh chain unchanged since the decode */
/* the entry's packets this frame: VU 0..5 from fb, the header's qwords (hq, as r2_header_qw), the script; unwritten
 * memory = fill (a decode check runs it with two fills: equal packets = the script reads only what it writes) */
int r2_outline_exec(const r2_oscript* s, const uint32_t* (*hq)(uint32_t), const r2_frame_block* fb, uint32_t fill, uint8_t* out,
                    uint32_t cap, uint32_t* nout);

/* ---- (r2-m5b, R5 part 2) the VU clipper of meshes crossing the guard band (r2_clip.c) -------------------------------
 * r2_clip_entry: the entry's triangles the VU clips (0x0C60's marks), each run through the game's own 0x2540 on a private
 * VU1 (vu.c vu_private_run); the GIF packets it kicks (fans with their material A+D packets) appended to o. 1: done
 * (*last_fanned = 1 + the last sub-batch that kicked fans, 0: none), 0: the entry is not supported (stays exact).
 * r2_clip_setup: the private VU's micro memory (NULL: the linked recompiled unit 376110651; tools pass a recording's) */
#define R2_CLIP_VMAX 4096u
typedef struct { uint8_t* out; uint32_t n, cap, overflow; uint32_t tris, fans, culled; } r2_fanbuf;
int r2_clip_setup(const uint8_t* micro);
extern int r2_clip_why;           /* the last r2_clip_entry's no: 1 sub-batch kind, 2 a strip triangle with w <= 0, 3 full, 4 size */
/* (also for the oracle) the vertices' clip coordinates / flags; 0x0C60's verdict on the triangle ending at strip vertex k
 * of sub-batch b; one sub-batch's clipped triangles (-1: unsupported, else how many kicked fans) */
enum { R2CV_ADC, R2CV_STRIP, R2CV_CULLED, R2CV_CLIP };
int r2_clip_prep(const r2_mesh* m, const float M[16]);
int r2_clip_verdict(const r2_mesh* m, uint32_t b, uint32_t k);
int r2_clip_batch(const r2_mesh* m, uint32_t b, float fade, const r2_frame_block* f, r2_fanbuf* o);
int r2_clip_entry(const r2_mesh* m, const float M[16], float fade, const r2_frame_block* f, r2_fanbuf* o, uint32_t* last_fanned,
                  uint32_t* rt, uint32_t rtcap);

/* ---- EE side (r2_capture.c, called from recomp/patches.py patches) --------------------------------------------- */
struct ps2_ctx;
void r2_cap_alloc(struct ps2_ctx* ctx);   /* render-list allocator returns: $v0 = entry */
void r2_cap_entry(struct ps2_ctx* ctx);   /* before 0x158378: $s2 = entry, $s0 = priority, $s1 = class */
void r2_cap_done(struct ps2_ctx* ctx);    /* before 0x1583FC: frame end */
enum { R2_OFF, R2_HYBRID, R2_BRACKET };
extern volatile int r2_mode;              /* effective mode for the next frame (r2_set_mode) */
void r2_set_mode(int mode);               /* settings / renderer start-stop; R2_OFF when the GPU renderer is not running */
void r2_stats_line(char* buf, size_t n);  /* per-second statistics (PS2_R2_STATS) */
