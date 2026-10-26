/* r2gpu's Direct3D 11 backend (r2gpu.h): the exact path's ID3D11Device and immediate context, borrowed (the render
 * thread's; resources may also be created from other threads, the D3D11 device is free-threaded). */
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include "r2gpu.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>

#define REL(p) do { if (p) { IUnknown_Release((IUnknown*)(p)); (p) = NULL; } } while (0)

struct r2g_buf {
    ID3D11Buffer* b;
    ID3D11ShaderResourceView* srv;
    uint32_t bytes;
    int kind, usage;
};

struct r2g_pipe {
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11GeometryShader* gs;
    ID3D11InputLayout* il;
};

#define NBLEND 5
struct r2g_dev {
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    ID3D11BlendState* bs[NBLEND][16];
    ID3D11DepthStencilState* ds[4][2];
    ID3D11RasterizerState* rs[3];
    /* filtered bindings */
    r2g_state cur;
    int cur_valid;
    r2g_pipe* pipe;
    uint32_t draws;
};

r2g_dev* r2g_d3d11_create(void* device, void* context) {
    r2g_dev* d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->dev = device;
    d->ctx = context;
    return d;
}

void r2g_destroy(r2g_dev* d) {
    if (!d) return;
    for (int i = 0; i < NBLEND; i++) for (int k = 0; k < 16; k++) REL(d->bs[i][k]);
    for (int i = 0; i < 4; i++) for (int k = 0; k < 2; k++) REL(d->ds[i][k]);
    for (int i = 0; i < 3; i++) REL(d->rs[i]);
    free(d);
}

void r2g_invalidate(r2g_dev* d) { d->cur_valid = 0; d->pipe = NULL; }
uint32_t r2g_draws(const r2g_dev* d) { return d->draws; }

/* ---- buffers ----------------------------------------------------------------------------------------------------- */
r2g_buf* r2g_buf_create(r2g_dev* d, int kind, int usage, uint32_t bytes, const void* init) {
    r2g_buf* b = calloc(1, sizeof *b);
    if (!b) return NULL;
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = (bytes + 15) & ~15u;
    bd.Usage = usage == R2G_USAGE_DYNAMIC ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    bd.CPUAccessFlags = usage == R2G_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : 0;
    switch (kind) {
    case R2G_BUF_VERTEX: bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; break;
    case R2G_BUF_INDEX: bd.BindFlags = D3D11_BIND_INDEX_BUFFER; break;
    case R2G_BUF_CONSTANT: bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; break;
    default:
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        break;
    }
    D3D11_SUBRESOURCE_DATA sd = { init, 0, 0 };
    if (FAILED(ID3D11Device_CreateBuffer(d->dev, &bd, init ? &sd : NULL, &b->b))) { free(b); return NULL; }
    if (kind == R2G_BUF_RAW) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        memset(&vd, 0, sizeof vd);
        vd.Format = DXGI_FORMAT_R32_TYPELESS;
        vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        vd.BufferEx.NumElements = bd.ByteWidth / 4;
        vd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (FAILED(ID3D11Device_CreateShaderResourceView(d->dev, (ID3D11Resource*)b->b, &vd, &b->srv))) { REL(b->b); free(b); return NULL; }
    }
    b->bytes = bd.ByteWidth;
    b->kind = kind;
    b->usage = usage;
    return b;
}

void r2g_buf_free(r2g_buf* b) {
    if (!b) return;
    REL(b->srv);
    REL(b->b);
    free(b);
}

uint32_t r2g_buf_size(const r2g_buf* b) { return b ? b->bytes : 0; }

void r2g_buf_write(r2g_dev* d, r2g_buf* b, uint32_t offset, const void* data, uint32_t bytes) {
    if (!bytes) return;
    D3D11_BOX box = { offset, 0, 0, offset + bytes, 1, 1 };
    ID3D11DeviceContext_UpdateSubresource(d->ctx, (ID3D11Resource*)b->b, 0, b->kind == R2G_BUF_CONSTANT ? NULL : &box, data, 0, 0);
}

void* r2g_buf_map(r2g_dev* d, r2g_buf* b, int discard) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(d->ctx, (ID3D11Resource*)b->b, 0, discard ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m)))
        return NULL;
    return m.pData;
}

void r2g_buf_unmap(r2g_dev* d, r2g_buf* b) { ID3D11DeviceContext_Unmap(d->ctx, (ID3D11Resource*)b->b, 0); }

void r2g_buf_copy(r2g_dev* d, r2g_buf* dst, uint32_t dst_off, r2g_buf* src, uint32_t src_off, uint32_t bytes) {
    D3D11_BOX box = { src_off, 0, 0, src_off + bytes, 1, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(d->ctx, (ID3D11Resource*)dst->b, 0, dst_off, 0, 0, (ID3D11Resource*)src->b, 0, &box);
}

/* ---- pipelines ----------------------------------------------------------------------------------------------------- */
r2g_pipe* r2g_pipe_create(r2g_dev* d, const r2g_pipe_desc* desc) {
    r2g_pipe* p = calloc(1, sizeof *p);
    if (!p) return NULL;
    static const DXGI_FORMAT fmt[] = { DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                       DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R8G8B8A8_UINT };
    D3D11_INPUT_ELEMENT_DESC el[16];
    int n = desc->nattrs > 16 ? 16 : desc->nattrs;
    for (int i = 0; i < n; i++) {
        const r2g_attr* a = &desc->attrs[i];
        el[i].SemanticName = a->semantic;
        el[i].SemanticIndex = 0;
        el[i].Format = fmt[a->format];
        el[i].InputSlot = (UINT)a->slot;
        el[i].AlignedByteOffset = (UINT)a->offset;
        el[i].InputSlotClass = a->slot ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
        el[i].InstanceDataStepRate = a->slot ? 1 : 0;
    }
    if (FAILED(ID3D11Device_CreateVertexShader(d->dev, desc->vs, desc->vs_size, NULL, &p->vs)) ||
        FAILED(ID3D11Device_CreatePixelShader(d->dev, desc->ps, desc->ps_size, NULL, &p->ps)) ||
        (n > 0 && FAILED(ID3D11Device_CreateInputLayout(d->dev, el, (UINT)n, desc->vs, desc->vs_size, &p->il))) ||
        (desc->gs && FAILED(ID3D11Device_CreateGeometryShader(d->dev, desc->gs, desc->gs_size, NULL, &p->gs)))) {
        r2g_pipe_free(p);
        return NULL;
    }
    return p;
}

void r2g_pipe_free(r2g_pipe* p) {
    if (!p) return;
    REL(p->vs);
    REL(p->ps);
    REL(p->gs);
    REL(p->il);
    free(p);
}

/* ---- state ---------------------------------------------------------------------------------------------------------- */
static ID3D11BlendState* blend_obj(r2g_dev* d, int cls, int wmask) {
    if (cls < 0 || cls >= NBLEND) cls = R2G_BLEND_OFF;
    wmask &= 15;
    if (d->bs[cls][wmask]) return d->bs[cls][wmask];
    D3D11_BLEND_DESC bd;
    memset(&bd, 0, sizeof bd);
    D3D11_RENDER_TARGET_BLEND_DESC* r = &bd.RenderTarget[0];
    r->RenderTargetWriteMask = (UINT8)wmask;
    r->SrcBlendAlpha = D3D11_BLEND_ONE;          /* the alpha byte: As (the GS writes the source alpha) */
    r->DestBlendAlpha = D3D11_BLEND_ZERO;
    r->BlendOpAlpha = D3D11_BLEND_OP_ADD;
    r->SrcBlend = D3D11_BLEND_ONE;
    r->DestBlend = D3D11_BLEND_ZERO;
    r->BlendOp = D3D11_BLEND_OP_ADD;
    switch (cls) {                               /* (the exact path's table, gpu_exec.c blend_state) */
    case R2G_BLEND_ADD: r->BlendEnable = TRUE; r->DestBlend = D3D11_BLEND_ONE; break;
    case R2G_BLEND_REVSUB: r->BlendEnable = TRUE; r->DestBlend = D3D11_BLEND_ONE; r->BlendOp = D3D11_BLEND_OP_REV_SUBTRACT; break;
    case R2G_BLEND_DUAL: r->BlendEnable = TRUE; r->SrcBlend = D3D11_BLEND_SRC1_ALPHA; r->DestBlend = D3D11_BLEND_INV_SRC1_ALPHA; break;
    default: break;
    }
    ID3D11Device_CreateBlendState(d->dev, &bd, &d->bs[cls][wmask]);
    return d->bs[cls][wmask];
}

static ID3D11DepthStencilState* depth_obj(r2g_dev* d, int ztest, int zwrite) {
    ztest &= 3;
    zwrite = zwrite != 0;
    if (d->ds[ztest][zwrite]) return d->ds[ztest][zwrite];
    D3D11_DEPTH_STENCIL_DESC dd;
    memset(&dd, 0, sizeof dd);
    static const D3D11_COMPARISON_FUNC f[4] = { D3D11_COMPARISON_ALWAYS, D3D11_COMPARISON_ALWAYS, D3D11_COMPARISON_GREATER_EQUAL,
                                                D3D11_COMPARISON_GREATER };
    dd.DepthEnable = ztest != R2G_Z_OFF;
    dd.DepthWriteMask = zwrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = f[ztest];
    ID3D11Device_CreateDepthStencilState(d->dev, &dd, &d->ds[ztest][zwrite]);
    return d->ds[ztest][zwrite];
}

static ID3D11RasterizerState* raster_obj(r2g_dev* d, int cull) {
    if (cull < 0 || cull > 2) cull = 0;
    if (d->rs[cull]) return d->rs[cull];
    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = cull == R2G_CULL_FRONT ? D3D11_CULL_FRONT : cull == R2G_CULL_BACK ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    ID3D11Device_CreateRasterizerState(d->dev, &rd, &d->rs[cull]);
    return d->rs[cull];
}

void r2g_bind_state(r2g_dev* d, const r2g_state* s) {
    int all = !d->cur_valid;
    if (all || s->blend != d->cur.blend || s->wmask != d->cur.wmask)
        ID3D11DeviceContext_OMSetBlendState(d->ctx, blend_obj(d, s->blend, s->wmask), NULL, 0xFFFFFFFFu);
    if (all || s->ztest != d->cur.ztest || s->zwrite != d->cur.zwrite)
        ID3D11DeviceContext_OMSetDepthStencilState(d->ctx, depth_obj(d, s->ztest, s->zwrite), 0);
    if (all || s->cull != d->cur.cull) ID3D11DeviceContext_RSSetState(d->ctx, raster_obj(d, s->cull));
    d->cur = *s;
    d->cur_valid = 1;
}

/* ---- commands ---------------------------------------------------------------------------------------------------------- */
void r2g_bind_pipe(r2g_dev* d, r2g_pipe* p) {
    if (d->pipe == p) return;
    ID3D11DeviceContext_VSSetShader(d->ctx, p->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(d->ctx, p->ps, NULL, 0);
    ID3D11DeviceContext_GSSetShader(d->ctx, p->gs, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(d->ctx, p->il);
    ID3D11DeviceContext_IASetPrimitiveTopology(d->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d->pipe = p;
}

void r2g_bind_vertex(r2g_dev* d, int slot, r2g_buf* b, uint32_t stride) {
    UINT st = stride, off = 0;
    ID3D11DeviceContext_IASetVertexBuffers(d->ctx, (UINT)slot, 1, &b->b, &st, &off);
}

void r2g_bind_vertex_native(r2g_dev* d, int slot, void* buffer, uint32_t stride) {
    ID3D11Buffer* b = buffer;
    UINT st = stride, off = 0;
    ID3D11DeviceContext_IASetVertexBuffers(d->ctx, (UINT)slot, 1, &b, &st, &off);
}

void r2g_bind_index(r2g_dev* d, r2g_buf* b) { ID3D11DeviceContext_IASetIndexBuffer(d->ctx, b->b, DXGI_FORMAT_R32_UINT, 0); }

void r2g_bind_constants(r2g_dev* d, int slot, r2g_buf* b) {
    ID3D11DeviceContext_VSSetConstantBuffers(d->ctx, (UINT)slot, 1, &b->b);
    ID3D11DeviceContext_PSSetConstantBuffers(d->ctx, (UINT)slot, 1, &b->b);
    ID3D11DeviceContext_GSSetConstantBuffers(d->ctx, (UINT)slot, 1, &b->b);
}

void r2g_end(r2g_dev* d) {
    ID3D11DeviceContext_GSSetShader(d->ctx, NULL, NULL, 0);
    ID3D11Buffer* nb = NULL;
    ID3D11DeviceContext_GSSetConstantBuffers(d->ctx, 1, 1, &nb);
    d->pipe = NULL;
}

void r2g_bind_srv_native(r2g_dev* d, int slot, void* srv) {
    ID3D11ShaderResourceView* v = srv;
    ID3D11DeviceContext_VSSetShaderResources(d->ctx, (UINT)slot, 1, &v);
    ID3D11DeviceContext_GSSetShaderResources(d->ctx, (UINT)slot, 1, &v);
    ID3D11DeviceContext_PSSetShaderResources(d->ctx, (UINT)slot, 1, &v);
}

void r2g_bind_srv(r2g_dev* d, int slot, r2g_buf* b) { r2g_bind_srv_native(d, slot, b ? b->srv : NULL); }

void r2g_bind_targets_native(r2g_dev* d, void* rtv, void* dsv) {
    ID3D11RenderTargetView* r = rtv;
    ID3D11DeviceContext_OMSetRenderTargets(d->ctx, r ? 1 : 0, r ? &r : NULL, (ID3D11DepthStencilView*)dsv);
}

void r2g_viewport(r2g_dev* d, float x, float y, float w, float h) {
    D3D11_VIEWPORT vp = { x, y, w, h, 0.0f, 1.0f };
    ID3D11DeviceContext_RSSetViewports(d->ctx, 1, &vp);
}

void r2g_scissor(r2g_dev* d, int x0, int y0, int x1, int y1) {
    D3D11_RECT r = { x0, y0, x1, y1 };
    ID3D11DeviceContext_RSSetScissorRects(d->ctx, 1, &r);
}

void r2g_draw_indexed(r2g_dev* d, uint32_t count, uint32_t first_index, int32_t base_vertex, uint32_t first_instance) {
    ID3D11DeviceContext_DrawIndexedInstanced(d->ctx, count, 1, first_index, base_vertex, first_instance);
    d->draws++;
}

void r2g_draw(r2g_dev* d, uint32_t count, uint32_t first_vertex) {
    ID3D11DeviceContext_Draw(d->ctx, count, first_vertex);
    d->draws++;
}
