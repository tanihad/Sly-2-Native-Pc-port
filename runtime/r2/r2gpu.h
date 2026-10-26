/* r2gpu: the modern renderer's thin graphics layer (notes/modern-renderer-api.md 2.3, notes/modern-renderer.md 2.1 layer
 * 4). C99, opaque handles, no D3D / SDL types above it. Backends: Direct3D 11 on the exact path's own device, context and
 * targets (r2gpu_d3d11.c); later SDL3 GPU and Vulkan.
 *
 * Objects: devices (wrapping a backend device), buffers (vertex, index, constant, raw), pipelines (shaders + vertex layout,
 * baked at creation: created at start, never while playing) and fixed-function state (blend / depth / raster, from a small
 * closed set, cached by key). Commands are immediate on the device's context. Targets, views and the vertex buffer of draw
 * ids come from the host (the exact path) as native handles. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct r2g_dev r2g_dev;
typedef struct r2g_buf r2g_buf;
typedef struct r2g_pipe r2g_pipe;

enum { R2G_BUF_VERTEX, R2G_BUF_INDEX, R2G_BUF_CONSTANT, R2G_BUF_RAW };
enum { R2G_USAGE_DEFAULT, R2G_USAGE_DYNAMIC };

/* vertex attributes of a pipeline: element format, byte offset, buffer slot (slot 1: per instance) */
enum { R2G_F32x2, R2G_F32x3, R2G_F32x4, R2G_U32, R2G_S16x2, R2G_U8x4 };
typedef struct { const char* semantic; int format, offset, slot; } r2g_attr;
typedef struct {
    const void* vs; size_t vs_size;      /* compiled shaders (DXBC for D3D11, from build/gen/gpu_shaders.h) */
    const void* ps; size_t ps_size;
    const r2g_attr* attrs; int nattrs;
} r2g_pipe_desc;

/* fixed-function state: the GS blend classes (gpu_front.c BL_*), write mask, depth test / write, culling */
enum { R2G_BLEND_OFF, R2G_BLEND_NOCOLOR, R2G_BLEND_ADD, R2G_BLEND_REVSUB, R2G_BLEND_DUAL };
enum { R2G_Z_OFF, R2G_Z_ALWAYS, R2G_Z_GEQUAL, R2G_Z_GREATER };
enum { R2G_CULL_NONE, R2G_CULL_FRONT, R2G_CULL_BACK };
typedef struct { uint8_t blend, wmask, ztest, zwrite, cull; } r2g_state;

r2g_dev* r2g_d3d11_create(void* device, void* context);   /* ID3D11Device*, ID3D11DeviceContext* (borrowed) */
void r2g_destroy(r2g_dev* d);

r2g_buf* r2g_buf_create(r2g_dev* d, int kind, int usage, uint32_t bytes, const void* init);
void r2g_buf_free(r2g_buf* b);
uint32_t r2g_buf_size(const r2g_buf* b);
void r2g_buf_write(r2g_dev* d, r2g_buf* b, uint32_t offset, const void* data, uint32_t bytes);   /* default: update */
void* r2g_buf_map(r2g_dev* d, r2g_buf* b, int discard);   /* dynamic buffers: discard or no-overwrite */
void r2g_buf_unmap(r2g_dev* d, r2g_buf* b);
void r2g_buf_copy(r2g_dev* d, r2g_buf* dst, uint32_t dst_off, r2g_buf* src, uint32_t src_off, uint32_t bytes);

r2g_pipe* r2g_pipe_create(r2g_dev* d, const r2g_pipe_desc* desc);
void r2g_pipe_free(r2g_pipe* p);

/* commands */
void r2g_bind_pipe(r2g_dev* d, r2g_pipe* p);
void r2g_bind_state(r2g_dev* d, const r2g_state* s);       /* (cached state objects; redundant binds filtered) */
void r2g_bind_vertex(r2g_dev* d, int slot, r2g_buf* b, uint32_t stride);
void r2g_bind_vertex_native(r2g_dev* d, int slot, void* buffer, uint32_t stride);   /* ID3D11Buffer* */
void r2g_bind_index(r2g_dev* d, r2g_buf* b);
void r2g_bind_constants(r2g_dev* d, int slot, r2g_buf* b);   /* vertex and pixel stages */
void r2g_bind_srv_native(r2g_dev* d, int slot, void* srv);   /* vertex and pixel stages: ID3D11ShaderResourceView* */
void r2g_bind_targets_native(r2g_dev* d, void* rtv, void* dsv);
void r2g_viewport(r2g_dev* d, float x, float y, float w, float h);
void r2g_scissor(r2g_dev* d, int x0, int y0, int x1, int y1);
void r2g_draw_indexed(r2g_dev* d, uint32_t count, uint32_t first_index, int32_t base_vertex, uint32_t first_instance);
void r2g_draw(r2g_dev* d, uint32_t count, uint32_t first_vertex);
uint32_t r2g_draws(const r2g_dev* d);   /* draw calls issued (statistics) */
/* the host's own bindings may have changed under it (the exact path drew): forget the filtered state */
void r2g_invalidate(r2g_dev* d);
