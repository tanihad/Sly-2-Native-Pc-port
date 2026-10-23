/* The Options menu and the frame rate counter over the presented picture (menu_overlay.h). */
#define COBJMACROS
#include "menu_overlay.h"
#include "menu.h"
#include "menu_draw.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

void debug_write_png(const char* path, const uint32_t* bgrx, int w, int h, int stride);   /* debug.c */

/* ---- the CPU pictures (layers) ------------------------------------------------------------------------------- */

enum { L_MENU, L_FPS, NLAYER };
typedef struct {
    uint32_t* px;
    int w, h, x, y, cap;                 /* size, place in the output, allocated pixels */
    int on;
    uint32_t serial;                     /* bumped when px changes (uploads follow) */
} layer;

static SRWLOCK lock = SRWLOCK_INIT;
static layer layers[NLAYER];
static uint32_t drawn_version = 0xFFFFFFFFu;
static int drawn_w, drawn_h;
static volatile LONG fps_x100;
static int fps_drawn = -1, fps_shown_flag = -1;
static char shot_path[512];
static volatile LONG shot_pending;

void overlay_set_fps(double fps) { InterlockedExchange(&fps_x100, (LONG)(fps * 100.0 + 0.5)); }

void overlay_request_shot(const char* path) {
    AcquireSRWLockExclusive(&lock);
    snprintf(shot_path, sizeof shot_path, "%s", path);
    ReleaseSRWLockExclusive(&lock);
    InterlockedExchange(&shot_pending, 1);
}

static int layer_size(layer* l, int w, int h) {
    if ((size_t)w * h > (size_t)l->cap) {
        free(l->px);
        l->px = malloc((size_t)w * h * 4);
        l->cap = l->px ? w * h : 0;
        if (!l->px) return 0;
    }
    l->w = w;
    l->h = h;
    return 1;
}

/* bring the layers up to date for an output of w x h; lock held. Returns the number of visible layers. */
static int update(int w, int h) {
    int n = 0;
    uint32_t ver = menu_version();
    layer* m = &layers[L_MENU];
    if (ver != drawn_version || w != drawn_w || h != drawn_h) {
        int open = menu_is_open();
        if (open && layer_size(m, w, h)) {
            m->on = menu_draw(m->px, w, h, w);
            m->x = m->y = 0;
        } else {
            m->on = 0;
        }
        m->serial++;
        drawn_version = ver;
        drawn_w = w;
        drawn_h = h;
    }
    n += m->on;
    layer* f = &layers[L_FPS];
    int show = ps2_settings.show_fps && !m->on;
    int fx = (int)fps_x100;
    float u = (float)h / 1080.0f;
    if (u < 0.4f) u = 0.4f;
    int fw = (int)(150 * u), fh = (int)(44 * u);
    if (show && (fx / 10 != fps_drawn || fps_shown_flag != show || f->w != fw || f->h != fh)) {
        if (layer_size(f, fw, fh)) {
            menu_draw_fps(f->px, fw, fh, fw, fx / 100.0, u);
            f->serial++;
            fps_drawn = fx / 10;
        }
    }
    fps_shown_flag = show;
    f->on = show && f->px;
    f->x = (int)(14 * u);
    f->y = (int)(14 * u);
    n += f->on;
    return n;
}

/* ---- Direct3D 11 ----------------------------------------------------------------------------------------------- */

static const char shader_src[] =
    "Texture2D tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs_main(uint id : SV_VertexID) {\n"
    "    V o;\n"
    "    float2 t = float2((id << 1) & 2, id & 2);\n"
    "    o.pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);\n"
    "    o.uv = t;\n"
    "    return o;\n"
    "}\n"
    "float4 ps_main(V i) : SV_Target {\n"
    "    return tex.Sample(smp, i.uv).bgra;\n"     /* BGRA bytes in an RGBA texture; premultiplied */
    "}\n";

typedef struct {
    ID3D11Device* dev;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11BlendState* blend;
    ID3D11SamplerState* smp;
    ID3D11RasterizerState* rs;
    ID3D11DepthStencilState* ds;
    ID3D11Texture2D* tex[NLAYER];
    ID3D11ShaderResourceView* srv[NLAYER];
    int tw[NLAYER], th[NLAYER];
    uint32_t serial[NLAYER];
    int failed;
} dres;
static dres res[2];

#define REL(p) do { if (p) { IUnknown_Release((IUnknown*)(p)); (p) = NULL; } } while (0)

static void res_free(dres* r) {
    for (int k = 0; k < NLAYER; k++) { REL(r->srv[k]); REL(r->tex[k]); }
    REL(r->vs); REL(r->ps); REL(r->blend); REL(r->smp); REL(r->rs); REL(r->ds);
    memset(r, 0, sizeof *r);
}

void overlay_d3d11_release(ID3D11Device* dev) {
    AcquireSRWLockExclusive(&lock);
    for (int k = 0; k < 2; k++) if (res[k].dev == dev) res_free(&res[k]);
    ReleaseSRWLockExclusive(&lock);
}

static dres* res_get(ID3D11Device* dev) {
    for (int k = 0; k < 2; k++) if (res[k].dev == dev) return res[k].failed ? NULL : &res[k];
    dres* r = !res[0].dev ? &res[0] : !res[1].dev ? &res[1] : &res[0];
    if (r->dev) res_free(r);
    r->dev = dev;
    static HMODULE comp;
    if (!comp) comp = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile compile = comp ? (pD3DCompile)(void*)GetProcAddress(comp, "D3DCompile") : NULL;
    ID3DBlob *vsb = NULL, *psb = NULL;
    HRESULT hr = compile ? compile(shader_src, sizeof shader_src - 1, "overlay", NULL, NULL, "vs_main", "vs_4_0", 0, 0, &vsb, NULL) : E_FAIL;
    if (SUCCEEDED(hr)) hr = compile(shader_src, sizeof shader_src - 1, "overlay", NULL, NULL, "ps_main", "ps_4_0", 0, 0, &psb, NULL);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &r->vs);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &r->ps);
    REL(vsb); REL(psb);
    if (SUCCEEDED(hr)) {
        D3D11_BLEND_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;               /* premultiplied */
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = ID3D11Device_CreateBlendState(dev, &bd, &r->blend);
    }
    if (SUCCEEDED(hr)) {
        D3D11_SAMPLER_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;                  /* one texel per output pixel */
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = ID3D11Device_CreateSamplerState(dev, &sd, &r->smp);
    }
    if (SUCCEEDED(hr)) {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        hr = ID3D11Device_CreateRasterizerState(dev, &rd, &r->rs);
    }
    if (SUCCEEDED(hr)) {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        hr = ID3D11Device_CreateDepthStencilState(dev, &dd, &r->ds);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[menu] overlay: cannot create its Direct3D 11 objects (hr=0x%08lX): the menu is not drawn\n", (unsigned long)hr);
        res_free(r);
        r->dev = dev;
        r->failed = 1;
        return NULL;
    }
    return r;
}

static int upload(dres* r, ID3D11DeviceContext* ctx, int k) {
    layer* l = &layers[k];
    if (!r->tex[k] || r->tw[k] != l->w || r->th[k] != l->h) {
        REL(r->srv[k]); REL(r->tex[k]);
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = (UINT)l->w;
        td.Height = (UINT)l->h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(r->dev, &td, NULL, &r->tex[k])) ||
            FAILED(ID3D11Device_CreateShaderResourceView(r->dev, (ID3D11Resource*)r->tex[k], NULL, &r->srv[k]))) {
            REL(r->srv[k]); REL(r->tex[k]);
            return 0;
        }
        r->tw[k] = l->w;
        r->th[k] = l->h;
        r->serial[k] = l->serial - 1;
    }
    if (r->serial[k] != l->serial) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)r->tex[k], 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return 0;
        for (int y = 0; y < l->h; y++) memcpy((uint8_t*)m.pData + (size_t)y * m.RowPitch, l->px + (size_t)y * l->w, (size_t)l->w * 4);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)r->tex[k], 0);
        r->serial[k] = l->serial;
    }
    return 1;
}

/* the back buffer -> PNG (a requested screenshot) */
static void shot(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int w, int h) {
    ID3D11Resource* src = NULL;
    ID3D11RenderTargetView_GetResource(rtv, &src);
    if (!src) return;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D*)src, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D* st = NULL;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &st))) {
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)st, src);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)st, 0, D3D11_MAP_READ, 0, &m))) {
            uint32_t* px = malloc((size_t)w * h * 4);
            if (px) {
                int bgra = td.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
                for (int y = 0; y < h; y++) {
                    const uint32_t* s = (const uint32_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch);
                    for (int x = 0; x < w; x++) {
                        uint32_t v = s[x];   /* RGBA bytes -> BGRX */
                        px[(size_t)y * w + x] = bgra ? v : (v & 0xFF00FF00u) | (v & 0xFF) << 16 | (v >> 16 & 0xFF);
                    }
                }
                debug_write_png(shot_path, px, w, h, w);
                fprintf(stderr, "[menu] screenshot %dx%d -> %s\n", w, h, shot_path);
                free(px);
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)st, 0);
        }
        REL(st);
    }
    REL(src);
}

void overlay_d3d11_draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int w, int h) {
    if (!dev || !ctx || !rtv || w <= 0 || h <= 0) return;
    AcquireSRWLockExclusive(&lock);
    int n = update(w, h);
    dres* r = n ? res_get(dev) : NULL;
    if (r) {
        /* save what is changed */
        ID3D11RenderTargetView* rtv0 = NULL;
        ID3D11DepthStencilView* dsv0 = NULL;
        ID3D11BlendState* bs0 = NULL;
        FLOAT bf0[4];
        UINT mask0 = 0, nvp = 1, sref0 = 0;
        D3D11_VIEWPORT vp0;
        ID3D11RasterizerState* rs0 = NULL;
        ID3D11DepthStencilState* ds0 = NULL;
        D3D11_PRIMITIVE_TOPOLOGY topo0;
        ID3D11InputLayout* il0 = NULL;
        ID3D11VertexShader* vs0 = NULL;
        ID3D11PixelShader* ps0 = NULL;
        ID3D11ShaderResourceView* srv0 = NULL;
        ID3D11SamplerState* smp0 = NULL;
        ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &rtv0, &dsv0);
        ID3D11DeviceContext_OMGetBlendState(ctx, &bs0, bf0, &mask0);
        ID3D11DeviceContext_OMGetDepthStencilState(ctx, &ds0, &sref0);
        ID3D11DeviceContext_RSGetViewports(ctx, &nvp, &vp0);
        ID3D11DeviceContext_RSGetState(ctx, &rs0);
        ID3D11DeviceContext_IAGetPrimitiveTopology(ctx, &topo0);
        ID3D11DeviceContext_IAGetInputLayout(ctx, &il0);
        ID3D11DeviceContext_VSGetShader(ctx, &vs0, NULL, NULL);
        ID3D11DeviceContext_PSGetShader(ctx, &ps0, NULL, NULL);
        ID3D11DeviceContext_PSGetShaderResources(ctx, 0, 1, &srv0);
        ID3D11DeviceContext_PSGetSamplers(ctx, 0, 1, &smp0);

        static const FLOAT zero[4] = { 0, 0, 0, 0 };
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_OMSetBlendState(ctx, r->blend, zero, 0xFFFFFFFFu);
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, r->ds, 0);
        ID3D11DeviceContext_RSSetState(ctx, r->rs);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
        ID3D11DeviceContext_VSSetShader(ctx, r->vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, r->ps, NULL, 0);
        ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &r->smp);
        for (int k = 0; k < NLAYER; k++) {
            layer* l = &layers[k];
            if (!l->on || !upload(r, ctx, k)) continue;
            D3D11_VIEWPORT vp = { (FLOAT)l->x, (FLOAT)l->y, (FLOAT)l->w, (FLOAT)l->h, 0.0f, 1.0f };
            ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &r->srv[k]);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
        }
        /* restore */
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv0, dsv0);
        ID3D11DeviceContext_OMSetBlendState(ctx, bs0, bf0, mask0);
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds0, sref0);
        if (nvp) ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp0);
        ID3D11DeviceContext_RSSetState(ctx, rs0);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, topo0);
        ID3D11DeviceContext_IASetInputLayout(ctx, il0);
        ID3D11DeviceContext_VSSetShader(ctx, vs0, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps0, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv0);
        ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &smp0);
        REL(rtv0); REL(dsv0); REL(bs0); REL(ds0); REL(rs0); REL(il0); REL(vs0); REL(ps0); REL(srv0); REL(smp0);
    }
    if (InterlockedExchange(&shot_pending, 0)) shot(dev, ctx, rtv, w, h);
    ReleaseSRWLockExclusive(&lock);
}

/* ---- GDI -------------------------------------------------------------------------------------------------- */

typedef BOOL (WINAPI* alpha_blend_fn)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);

void overlay_gdi_draw(void* hdc, int w, int h) {
    static alpha_blend_fn alpha_blend;
    static int tried;
    if (!tried) { tried = 1; alpha_blend = (alpha_blend_fn)(void*)GetProcAddress(LoadLibraryW(L"msimg32.dll"), "AlphaBlend"); }
    if (!alpha_blend || w <= 0 || h <= 0) return;
    AcquireSRWLockExclusive(&lock);
    if (update(w, h)) {
        HDC mem = CreateCompatibleDC((HDC)hdc);
        for (int k = 0; k < NLAYER && mem; k++) {
            layer* l = &layers[k];
            if (!l->on) continue;
            BITMAPINFO bi;
            memset(&bi, 0, sizeof bi);
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = l->w;
            bi.bmiHeader.biHeight = -l->h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* bits = NULL;
            HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
            if (!bmp) continue;
            memcpy(bits, l->px, (size_t)l->w * l->h * 4);
            HGDIOBJ old = SelectObject(mem, bmp);
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            alpha_blend((HDC)hdc, l->x, l->y, l->w, l->h, mem, 0, 0, l->w, l->h, bf);
            SelectObject(mem, old);
            DeleteObject(bmp);
        }
        if (mem) DeleteDC(mem);
    }
    ReleaseSRWLockExclusive(&lock);
}
