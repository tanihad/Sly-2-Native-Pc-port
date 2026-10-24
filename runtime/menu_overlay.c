/* The Options menu and the frame rate counter over the presented picture (menu_overlay.h).
 *
 * (pc-menu2) Direct3D 11 presenters draw the menu's primitive list (menu_draw.c menu_build: boxes, capsules, arrows,
 * glyphs of the game's font) as one batch of quads at every present - a few hundred quads, no CPU picture, no upload
 * but the vertices - so the menu follows the mouse at the display's refresh (the GPU presenter presents the menu between
 * the game's pictures: gpu_present.c gpu_present_menu). The glyph atlas is uploaded once (and again when the font
 * changes from the Windows stand-in to the game's own). GDI keeps a CPU picture (menu_draw), redrawn when the menu
 * changes. */
#define COBJMACROS
#include "menu_overlay.h"
#include "menu.h"
#include "menu_draw.h"
#include "settings.h"
#include "sly_font.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

void debug_write_png(const char* path, const uint32_t* bgrx, int w, int h, int stride);   /* debug.c */

static SRWLOCK lock = SRWLOCK_INIT;
static volatile LONG fps_x100;
static char shot_path[512];
static volatile LONG shot_pending;
static volatile LONG draws;                      /* (pc-menu2) menu pictures drawn (measurements) */

void overlay_set_fps(double fps) { InterlockedExchange(&fps_x100, (LONG)(fps * 100.0 + 0.5)); }

void overlay_request_shot(const char* path) {
    AcquireSRWLockExclusive(&lock);
    snprintf(shot_path, sizeof shot_path, "%s", path);
    ReleaseSRWLockExclusive(&lock);
    InterlockedExchange(&shot_pending, 1);
}

int overlay_wants_frames(void) { return menu_is_open(); }
long overlay_draw_count(void) { return draws; }

static double now_s(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

static ui_list glist;                            /* the GPU path's list (lock held) */

static double fps_shown(void) { return ps2_settings.show_fps ? (double)fps_x100 / 100.0 : -1.0; }

/* ---- Direct3D 11 ----------------------------------------------------------------------------------------------- */

static const char shader_src[] =
    "Texture2D<float> atlas : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct VI { float2 pos : POSITION; float2 loc : TEXCOORD0; float4 c0 : COLOR0; float4 c2 : COLOR1; float4 p0 : TEXCOORD1;\n"
    "            float4 p1 : TEXCOORD2; };\n"
    "struct VO { float4 pos : SV_Position; float2 loc : TEXCOORD0; float4 c0 : COLOR0; nointerpolation float4 c2 : COLOR1;\n"
    "            nointerpolation float4 p0 : TEXCOORD1; nointerpolation float4 p1 : TEXCOORD2; };\n"
    "VO vs_main(VI i) { VO o; o.pos = float4(i.pos, 0.0, 1.0); o.loc = i.loc; o.c0 = i.c0; o.c2 = i.c2; o.p0 = i.p0; o.p1 = i.p1;\n"
    "                   return o; }\n"
    "float4 ps_main(VO i) : SV_Target {\n"
    "    int k = (int)(i.p1.x + 0.5);\n"
    "    if (k == 0) {\n"                                               /* glyph (+ outline) */
    "        float a = atlas.Sample(smp, i.loc);\n"
    "        if (i.p1.y > 0.0) {\n"
    "            float o = a;\n"
    "            [unroll] for (int j = 0; j < 12; j++) {\n"
    "                float an = j * 0.5235988; float2 d = float2(cos(an), sin(an)) * i.p1.yz;\n"
    "                o = max(o, atlas.Sample(smp, i.loc + d)); o = max(o, atlas.Sample(smp, i.loc + 0.5 * d));\n"
    "            }\n"
    "            return i.c0 * a + i.c2 * saturate(o - a);\n"
    "        }\n"
    "        return i.c0 * a;\n"
    "    }\n"
    "    if (k == 1) {\n"                                               /* rounded box, border */
    "        float2 q = abs(i.loc) - i.p0.xy + i.p0.z;\n"
    "        float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - i.p0.z;\n"
    "        float cov = saturate(0.5 - d);\n"
    "        if (i.p0.w > 0.0) { float inn = saturate(0.5 - (d + i.p0.w)); return i.c0 * inn + i.c2 * (cov - inn); }\n"
    "        return i.c0 * cov;\n"
    "    }\n"
    "    if (k == 2) {\n"                                               /* capsule */
    "        float2 pa = i.loc - i.p0.xy, ba = i.p0.zw - i.p0.xy;\n"
    "        float h = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-6));\n"
    "        float d = length(pa - ba * h) - i.p1.y * 0.5;\n"
    "        return i.c0 * saturate(0.5 - d);\n"
    "    }\n"
    "    float2 l = i.loc; float hx = i.p0.x, hy = i.p0.y;\n"            /* arrow */
    "    if (abs(i.p0.z) > 1.5) l = l.yx;\n"
    "    if (i.p0.z < 0.0) l.x = -l.x;\n"
    "    float2 n1 = normalize(float2(hy, -2.0 * hx)), n2 = normalize(float2(hy, 2.0 * hx));\n"
    "    float d = max(max(dot(l - float2(hx, 0.0), n1), dot(l - float2(hx, 0.0), n2)), -hx - l.x);\n"
    "    return i.c0 * saturate(0.5 - d);\n"
    "}\n";

typedef struct { float x, y, lx, ly; uint32_t c0, c2; float p0[4], p1[4]; } vtx;
#define MAX_VERTS (UI_MAX_PRIMS * 6)

typedef struct {
    ID3D11Device* dev;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11InputLayout* il;
    ID3D11BlendState* blend;
    ID3D11SamplerState* smp;
    ID3D11RasterizerState* rs;
    ID3D11DepthStencilState* ds;
    ID3D11Buffer* vb;
    ID3D11Texture2D* atlas;
    ID3D11ShaderResourceView* atlas_srv;
    uint32_t atlas_serial;
    const slyf_atlas* atlas_src;
    int failed;
} dres;
static dres res[2];
static vtx* verts;

#define REL(p) do { if (p) { IUnknown_Release((IUnknown*)(p)); (p) = NULL; } } while (0)

static void res_free(dres* r) {
    REL(r->atlas_srv); REL(r->atlas); REL(r->vb); REL(r->vs); REL(r->ps); REL(r->il); REL(r->blend); REL(r->smp); REL(r->rs);
    REL(r->ds);
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
    if (!verts) verts = malloc(sizeof *verts * MAX_VERTS);
    static HMODULE comp;
    if (!comp) comp = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile compile = comp ? (pD3DCompile)(void*)GetProcAddress(comp, "D3DCompile") : NULL;
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    HRESULT hr = verts && compile ? compile(shader_src, sizeof shader_src - 1, "menu", NULL, NULL, "vs_main", "vs_4_0", 0, 0, &vsb, &err) : E_FAIL;
    if (SUCCEEDED(hr)) hr = compile(shader_src, sizeof shader_src - 1, "menu", NULL, NULL, "ps_main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err);
    if (FAILED(hr) && err) fprintf(stderr, "[menu] overlay shader: %s\n", (const char*)ID3D10Blob_GetBufferPointer(err));
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &r->vs);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &r->ps);
    if (SUCCEEDED(hr)) {
        static const D3D11_INPUT_ELEMENT_DESC el[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        hr = ID3D11Device_CreateInputLayout(dev, el, 6, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &r->il);
    }
    REL(vsb); REL(psb); REL(err);
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
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;                 /* (trilinear over the atlas's mipmaps) */
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MipLODBias = -0.25f;                                      /* (a touch sharper than the box filter) */
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
        rd.ScissorEnable = TRUE;
        hr = ID3D11Device_CreateRasterizerState(dev, &rd, &r->rs);
    }
    if (SUCCEEDED(hr)) {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        hr = ID3D11Device_CreateDepthStencilState(dev, &dd, &r->ds);
    }
    if (SUCCEEDED(hr)) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = (UINT)(sizeof(vtx) * MAX_VERTS);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &r->vb);
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

/* the glyph atlas (every mip level), again when the font changes */
static int atlas_upload(dres* r) {
    const slyf_atlas* a = slyf_current();
    if (!a) return 0;
    if (r->atlas && r->atlas_src == a) return 1;
    REL(r->atlas_srv); REL(r->atlas);
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)a->w[0];
    td.Height = (UINT)a->h[0];
    td.MipLevels = (UINT)a->levels;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init[SLYF_LEVELS];
    for (int k = 0; k < a->levels; k++) { init[k].pSysMem = a->px[k]; init[k].SysMemPitch = (UINT)a->w[k]; init[k].SysMemSlicePitch = 0; }
    if (FAILED(ID3D11Device_CreateTexture2D(r->dev, &td, init, &r->atlas)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(r->dev, (ID3D11Resource*)r->atlas, NULL, &r->atlas_srv))) {
        REL(r->atlas_srv); REL(r->atlas);
        return 0;
    }
    r->atlas_src = a;
    return 1;
}

static uint32_t rgba_premul(uint32_t c) {               /* 0xAARRGGBB straight -> R8G8B8A8 premultiplied */
    uint32_t a = c >> 24, r = (c >> 16 & 255) * a / 255, g = (c >> 8 & 255) * a / 255, b = (c & 255) * a / 255;
    return a << 24 | b << 16 | g << 8 | r;
}

/* the list -> vertices (6 per primitive); returns the count */
static int fill_vertices(const ui_list* L, int w, int h) {
    int n = 0;
    float sx = 2.0f / (float)w, sy = 2.0f / (float)h;
    for (int i = 0; i < L->n && n + 6 <= MAX_VERTS; i++) {
        const ui_prim* q = &L->p[i];
        float x0 = q->x, y0 = q->y, x1 = q->x + q->w, y1 = q->y + q->h;
        float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        vtx base;
        memset(&base, 0, sizeof base);
        base.c0 = rgba_premul(q->c0);
        base.c2 = rgba_premul(q->c2);
        uint32_t cbot = base.c0;
        switch (q->kind) {
        case UI_GLYPH:
            base.p1[0] = 0;
            if (q->p[0] > 0) { base.p1[1] = (q->u1 - q->u0) / q->w * q->p[0]; base.p1[2] = (q->v1 - q->v0) / q->h * q->p[0]; }
            break;
        case UI_BOX:
            base.p0[0] = q->w / 2; base.p0[1] = q->h / 2; base.p0[2] = q->p[0]; base.p0[3] = q->p[1];
            base.p1[0] = 1;
            cbot = rgba_premul(q->c1);
            break;
        case UI_CAPSULE:
            memcpy(base.p0, q->p, sizeof base.p0);
            base.p1[0] = 2; base.p1[1] = q->u0;
            break;
        default: {
            int vert = q->p[0] > 1.5f || q->p[0] < -1.5f;
            base.p0[0] = vert ? q->h / 2 : q->w / 2; base.p0[1] = vert ? q->w / 2 : q->h / 2; base.p0[2] = q->p[0];
            base.p1[0] = 3;
            break;
        }
        }
        static const int corner[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };
        for (int c = 0; c < 6; c++) {
            vtx* v = &verts[n++];
            *v = base;
            float px = corner[c][0] ? x1 : x0, py = corner[c][1] ? y1 : y0;
            v->x = px * sx - 1.0f;
            v->y = 1.0f - py * sy;
            if (corner[c][1]) v->c0 = cbot;
            switch (q->kind) {
            case UI_GLYPH: v->lx = corner[c][0] ? q->u1 : q->u0; v->ly = corner[c][1] ? q->v1 : q->v0; break;
            case UI_CAPSULE: v->lx = px; v->ly = py; break;
            default: v->lx = px - cx; v->ly = py - cy; break;
            }
        }
    }
    return n;
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

/* (pc-menu2) PS2_MENU_STATS=1: every 2 s while the menu is open, how often it was drawn, how long after an input its
 * change was drawn (input -> the picture that shows it is handed to Present) and the CPU time of a picture */
static struct { int on, draws, changes; double t0, lat_sum, lat_max, cpu_sum, cpu_max; uint32_t ver; } ms;

static void stats_note(double t_start, double t_end, int open) {
    if (!ms.on) { const char* e = getenv("PS2_MENU_STATS"); ms.on = e && *e && *e != '0' ? 1 : -1; }
    if (ms.on < 0 || !open) { ms.t0 = 0; return; }
    if (!ms.t0) { ms.t0 = t_end; ms.draws = ms.changes = 0; ms.lat_sum = ms.lat_max = ms.cpu_sum = ms.cpu_max = 0; }
    ms.draws++;
    double cpu = (t_end - t_start) * 1000.0;
    ms.cpu_sum += cpu;
    if (cpu > ms.cpu_max) ms.cpu_max = cpu;
    uint32_t v = menu_version();
    double in = menu_last_input_time();
    if (v != ms.ver) {
        ms.ver = v;
        double lat = (t_end - in) * 1000.0;
        if (in > 0 && lat >= 0 && lat < 500.0) {
            ms.changes++;
            ms.lat_sum += lat;
            if (lat > ms.lat_max) ms.lat_max = lat;
        }
    }
    if (t_end - ms.t0 >= 2.0) {
        double s = t_end - ms.t0;
        fprintf(stderr, "[menu] stats: %.1f pictures/s, %d changes: input -> picture avg %.2f ms, max %.2f ms; CPU per picture avg %.3f ms, "
                "max %.3f ms\n", ms.draws / s, ms.changes, ms.changes ? ms.lat_sum / ms.changes : 0.0, ms.lat_max, ms.cpu_sum / ms.draws,
                ms.cpu_max);
        ms.t0 = t_end;
        ms.draws = ms.changes = 0;
        ms.lat_sum = ms.lat_max = ms.cpu_sum = ms.cpu_max = 0;
    }
}

void overlay_d3d11_draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int w, int h) {
    if (!dev || !ctx || !rtv || w <= 0 || h <= 0) return;
    AcquireSRWLockExclusive(&lock);
    double t_start = now_s();
    if (!glist.p) glist.p = malloc(sizeof *glist.p * UI_MAX_PRIMS);
    int open = menu_is_open();
    double fps = fps_shown();
    int n = glist.p && (open || fps >= 0) ? menu_build(&glist, w, h, now_s(), fps) : 0;
    dres* r = n ? res_get(dev) : NULL;
    if (r && atlas_upload(r)) {
        int nv = fill_vertices(&glist, w, h);
        D3D11_MAPPED_SUBRESOURCE m;
        if (nv && SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)r->vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, verts, sizeof(vtx) * (size_t)nv);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)r->vb, 0);
            /* save what is changed */
            ID3D11RenderTargetView* rtv0 = NULL;
            ID3D11DepthStencilView* dsv0 = NULL;
            ID3D11BlendState* bs0 = NULL;
            FLOAT bf0[4];
            UINT mask0 = 0, nvp = 1, sref0 = 0, nsc = 1, stride0 = 0, off0 = 0;
            D3D11_VIEWPORT vp0;
            D3D11_RECT sc0;
            ID3D11RasterizerState* rs0 = NULL;
            ID3D11DepthStencilState* ds0 = NULL;
            D3D11_PRIMITIVE_TOPOLOGY topo0;
            ID3D11InputLayout* il0 = NULL;
            ID3D11VertexShader* vs0 = NULL;
            ID3D11PixelShader* ps0 = NULL;
            ID3D11ShaderResourceView* srv0 = NULL;
            ID3D11SamplerState* smp0 = NULL;
            ID3D11Buffer* vb0 = NULL;
            ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &rtv0, &dsv0);
            ID3D11DeviceContext_OMGetBlendState(ctx, &bs0, bf0, &mask0);
            ID3D11DeviceContext_OMGetDepthStencilState(ctx, &ds0, &sref0);
            ID3D11DeviceContext_RSGetViewports(ctx, &nvp, &vp0);
            ID3D11DeviceContext_RSGetScissorRects(ctx, &nsc, &sc0);
            ID3D11DeviceContext_RSGetState(ctx, &rs0);
            ID3D11DeviceContext_IAGetPrimitiveTopology(ctx, &topo0);
            ID3D11DeviceContext_IAGetInputLayout(ctx, &il0);
            ID3D11DeviceContext_IAGetVertexBuffers(ctx, 0, 1, &vb0, &stride0, &off0);
            ID3D11DeviceContext_VSGetShader(ctx, &vs0, NULL, NULL);
            ID3D11DeviceContext_PSGetShader(ctx, &ps0, NULL, NULL);
            ID3D11DeviceContext_PSGetShaderResources(ctx, 0, 1, &srv0);
            ID3D11DeviceContext_PSGetSamplers(ctx, 0, 1, &smp0);

            static const FLOAT zero[4] = { 0, 0, 0, 0 };
            UINT stride = sizeof(vtx), off = 0;
            D3D11_VIEWPORT vp = { 0.0f, 0.0f, (FLOAT)w, (FLOAT)h, 0.0f, 1.0f };
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
            ID3D11DeviceContext_OMSetBlendState(ctx, r->blend, zero, 0xFFFFFFFFu);
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, r->ds, 0);
            ID3D11DeviceContext_RSSetState(ctx, r->rs);
            ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
            ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_IASetInputLayout(ctx, r->il);
            ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &r->vb, &stride, &off);
            ID3D11DeviceContext_VSSetShader(ctx, r->vs, NULL, 0);
            ID3D11DeviceContext_PSSetShader(ctx, r->ps, NULL, 0);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &r->atlas_srv);
            ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &r->smp);
            /* runs of primitives with the same clip rectangle */
            for (int i = 0; i < glist.n && 6 * i < nv;) {
                int c = glist.p[i].clip, j = i;
                while (j < glist.n && 6 * j < nv && glist.p[j].clip == c) j++;
                D3D11_RECT sr = { 0, 0, w, h };
                if (c && c < glist.nclip) {
                    const int* cr = glist.clip[c];
                    sr.left = cr[0]; sr.top = cr[1]; sr.right = cr[0] + cr[2]; sr.bottom = cr[1] + cr[3];
                }
                ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sr);
                ID3D11DeviceContext_Draw(ctx, (UINT)(6 * (j - i)), (UINT)(6 * i));
                i = j;
            }
            InterlockedIncrement(&draws);
            /* restore */
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv0, dsv0);
            ID3D11DeviceContext_OMSetBlendState(ctx, bs0, bf0, mask0);
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds0, sref0);
            if (nvp) ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp0);
            if (nsc) ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sc0);
            ID3D11DeviceContext_RSSetState(ctx, rs0);
            ID3D11DeviceContext_IASetPrimitiveTopology(ctx, topo0);
            ID3D11DeviceContext_IASetInputLayout(ctx, il0);
            ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb0, &stride0, &off0);
            ID3D11DeviceContext_VSSetShader(ctx, vs0, NULL, 0);
            ID3D11DeviceContext_PSSetShader(ctx, ps0, NULL, 0);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv0);
            ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &smp0);
            REL(rtv0); REL(dsv0); REL(bs0); REL(ds0); REL(rs0); REL(il0); REL(vs0); REL(ps0); REL(srv0); REL(smp0); REL(vb0);
        }
    }
    stats_note(t_start, now_s(), open);
    if (InterlockedExchange(&shot_pending, 0)) shot(dev, ctx, rtv, w, h);
    ReleaseSRWLockExclusive(&lock);
}

/* ---- GDI: a CPU picture of the same list, redrawn when the menu changes ------------------------------------------- */

typedef struct { uint32_t* px; int w, h, cap; } layer;
static layer gl_menu, gl_fps;
static uint32_t gdi_version = 0xFFFFFFFFu;
static int gdi_w, gdi_h, gdi_fps = -1;

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

typedef BOOL (WINAPI* alpha_blend_fn)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);

static void gdi_blend(HDC hdc, HDC mem, alpha_blend_fn ab, const uint32_t* px, int x, int y, int w, int h) {
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) return;
    memcpy(bits, px, (size_t)w * h * 4);
    HGDIOBJ old = SelectObject(mem, bmp);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    ab(hdc, x, y, w, h, mem, 0, 0, w, h, bf);
    SelectObject(mem, old);
    DeleteObject(bmp);
}

void overlay_gdi_draw(void* hdc, int w, int h) {
    static alpha_blend_fn alpha_blend;
    static int tried;
    if (!tried) { tried = 1; alpha_blend = (alpha_blend_fn)(void*)GetProcAddress(LoadLibraryW(L"msimg32.dll"), "AlphaBlend"); }
    if (!alpha_blend || w <= 0 || h <= 0) return;
    AcquireSRWLockExclusive(&lock);
    HDC mem = CreateCompatibleDC((HDC)hdc);
    if (mem && menu_is_open()) {
        uint32_t ver = menu_version();
        if ((ver != gdi_version || w != gdi_w || h != gdi_h) && layer_size(&gl_menu, w, h)) {
            menu_animate(0);
            menu_draw(gl_menu.px, w, h, w);
            gdi_version = ver; gdi_w = w; gdi_h = h;
        }
        if (gl_menu.px) gdi_blend((HDC)hdc, mem, alpha_blend, gl_menu.px, 0, 0, w, h);
        InterlockedIncrement(&draws);
    } else if (mem && ps2_settings.show_fps) {
        gdi_version = 0xFFFFFFFFu;
        float u = (float)h / 1080.0f;
        if (u < 0.4f) u = 0.4f;
        int fw = (int)(150 * u), fh = (int)(44 * u), fx = (int)fps_x100 / 10;
        if ((fx != gdi_fps || gl_fps.w != fw || gl_fps.h != fh) && layer_size(&gl_fps, fw, fh)) {
            menu_draw_fps(gl_fps.px, fw, fh, fw, fx / 10.0, u);
            gdi_fps = fx;
        }
        if (gl_fps.px) gdi_blend((HDC)hdc, mem, alpha_blend, gl_fps.px, (int)(14 * u), (int)(14 * u), fw, fh);
    }
    if (mem) DeleteDC(mem);
    if (InterlockedExchange(&shot_pending, 0)) {  /* the window's picture, as drawn (a requested screenshot) */
        HDC m2 = CreateCompatibleDC((HDC)hdc);
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = NULL;
        HBITMAP bmp = m2 ? CreateDIBSection(m2, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
        if (bmp) {
            HGDIOBJ old = SelectObject(m2, bmp);
            BitBlt(m2, 0, 0, w, h, (HDC)hdc, 0, 0, SRCCOPY);
            GdiFlush();
            debug_write_png(shot_path, bits, w, h, w);
            fprintf(stderr, "[menu] screenshot %dx%d (GDI) -> %s\n", w, h, shot_path);
            SelectObject(m2, old);
            DeleteObject(bmp);
        }
        if (m2) DeleteDC(m2);
    }
    ReleaseSRWLockExclusive(&lock);
}
