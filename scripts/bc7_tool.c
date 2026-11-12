/* (tex-opt) BC7 tool for the Reimagined textures (notes/reimagined.md "Lighter and faster"): runtime/gpu/shaders/bc7.hlsl
 * on a headless D3D11 device, decoded back by the GPU's own BC7 sampler (the reference: what the game would show).
 * Build: clang -O2 scripts/bc7_tool.c -o build/tools/bc7_tool.exe -ld3d11 -ldxgi -lole32 -lwindowscodecs -luuid
 *
 *   bc7_tool probe                          the BC7 partition / anchor tables read back from the hardware decoder (HLSL text)
 *   bc7_tool encode <bc7.hlsl> <dir> <csv>  every <key>.jpg|png of a Reimagined cache folder: level 0 + its mips (box) as
 *                                           the game holds them (GS alpha), BC7-encoded, decoded by the GPU; per texture
 *                                           PSNR / max error of RGB and alpha (level 0 and all levels) into csv, timing
 *   bc7_tool dedupe <dir>                   cache files whose keys differ only in the addressing / alpha-test bits (60..62)
 *   bc7_tool diff <dirA> <dirB> <csv>       same-named images of two folders: PSNR / max error (RGB, alpha) per file
 *   bc7_tool mkbatch <dir> <in.bin> [n]     net inputs for scripts/ai_up_tool.c batch: every opaque cache image box-filtered
 *                                           back to its source size (<= 128), edge-padded by 12 (the game's tiles)
 *   bc7_tool cmpout <a.bin> <b.bin> <csv>   two ai_up batch outputs (AIO1): per image PSNR / max error, worst first
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ID3D11Device* dev;
static ID3D11DeviceContext* ctx;
typedef HRESULT(WINAPI* compile_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                    UINT, ID3DBlob**, ID3DBlob**);
static compile_fn comp;
static IWICImagingFactory* wic;

static double now(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
static int init(void) {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, &got, &ctx))) return -1;
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    comp = dc ? (compile_fn)(void*)GetProcAddress(dc, "D3DCompile") : NULL;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void**)&wic);
    return comp && wic ? 0 : -1;
}
static ID3D11ComputeShader* cs_from(const char* src, size_t n, const char* name, const char* entry, const D3D_SHADER_MACRO* m) {
    ID3DBlob *code = NULL, *msg = NULL;
    ID3D11ComputeShader* cs = NULL;
    if (FAILED(comp(src, n, name, m, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &msg))) {
        fprintf(stderr, "%s: %s\n", name, msg ? (const char*)ID3D10Blob_GetBufferPointer(msg) : "compile failed");
        return NULL;
    }
    ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &cs);
    ID3D10Blob_Release(code);
    if (msg) ID3D10Blob_Release(msg);
    return cs;
}
static char* read_file(const char* p, size_t* n) {
    FILE* f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = calloc(1, (size_t)l + 1);
    *n = fread(b, 1, (size_t)l, f);
    fclose(f);
    return b;
}

/* the decoder: level `mip` of slice `slice` of a Texture2DArray -> RGBA8 words */
static const char* dec_src =
    "Texture2DArray<float4> t : register(t0); RWStructuredBuffer<uint> o : register(u0);\n"
    "cbuffer c : register(b0) { uint w, h, slice, mip; };\n"
    "[numthreads(8, 8, 1)] void cs(uint3 id : SV_DispatchThreadID) { if (id.x >= w || id.y >= h) return;\n"
    "  float4 v = t.Load(int4(id.x, id.y, slice, mip)); uint4 b = (uint4)(saturate(v) * 255.0 + 0.5);\n"
    "  o[id.y * w + id.x] = b.x | (b.y << 8) | (b.z << 16) | (b.w << 24); }\n";
static ID3D11ComputeShader* dec_cs;
static ID3D11Buffer *dec_buf, *dec_st, *cb;
static ID3D11UnorderedAccessView* dec_uav;
static ID3D11Buffer* mkbuf(UINT bytes, UINT bind, D3D11_USAGE u, UINT cpu, UINT stride) {
    D3D11_BUFFER_DESC d;
    memset(&d, 0, sizeof d);
    d.ByteWidth = bytes; d.Usage = u; d.BindFlags = bind; d.CPUAccessFlags = cpu;
    if (stride) { d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = stride; }
    ID3D11Buffer* b = NULL;
    ID3D11Device_CreateBuffer(dev, &d, NULL, &b);
    return b;
}
static void cb_set(const uint32_t* v) {
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    memcpy(m.pData, v, 32);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)cb, 0);
}
static void dec_init(void) {
    if (dec_cs) return;
    dec_cs = cs_from(dec_src, strlen(dec_src), "dec", "cs", NULL);
    dec_buf = mkbuf(2048 * 2048 * 4, D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0, 4);
    dec_st = mkbuf(2048 * 2048 * 4, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, 0);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
    memset(&ud, 0, sizeof ud);
    ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = 2048 * 2048;
    ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource*)dec_buf, &ud, &dec_uav);
    if (!cb) cb = mkbuf(32, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, 0);
}
static void decode(ID3D11ShaderResourceView* srv, uint32_t w, uint32_t h, uint32_t slice, uint32_t mip, uint32_t* out) {
    dec_init();
    uint32_t v[8] = { w, h, slice, mip, 0, 0, 0, 0 };
    cb_set(v);
    ID3D11DeviceContext_CSSetShader(ctx, dec_cs, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &dec_uav, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_Dispatch(ctx, (w + 7) / 8, (h + 7) / 8, 1);
    ID3D11ShaderResourceView* ns = NULL;
    ID3D11UnorderedAccessView* nu = NULL;
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &ns);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &nu, NULL);
    D3D11_BOX bx = { 0, 0, 0, w * h * 4, 1, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource*)dec_st, 0, 0, 0, 0, (ID3D11Resource*)dec_buf, 0, &bx);
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)dec_st, 0, D3D11_MAP_READ, 0, &m);
    memcpy(out, m.pData, (size_t)w * h * 4);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)dec_st, 0);
}

/* ---- probe: one block per partition, endpoints that tell the subsets apart, every index bit 1 (anchors decode as 1) */
typedef struct { uint32_t w[4]; int pos; } bits;
static void put(bits* b, uint32_t v, int n) {
    for (int i = 0; i < n; i++, b->pos++) if (v >> i & 1) b->w[b->pos >> 5] |= 1u << (b->pos & 31);
}
static int probe(void) {
    uint32_t blocks[2][64][4];
    for (int p = 0; p < 64; p++) {
        bits b = { { 0 }, 0 };   /* mode 3: 2 subsets, RGB 7 bits + p-bit per endpoint, 2-bit indices */
        put(&b, 8, 4); put(&b, (uint32_t)p, 6);
        for (int ch = 0; ch < 3; ch++) { put(&b, 0, 7); put(&b, 0, 7); put(&b, 0, 7); put(&b, 127, 7); }
        put(&b, 0, 1); put(&b, 0, 1); put(&b, 0, 1); put(&b, 1, 1);
        put(&b, 0x3FFFFFFFu, 30);
        if (b.pos != 128) return fprintf(stderr, "mode 3 bits %d\n", b.pos), 1;
        memcpy(blocks[0][p], b.w, 16);
        bits c = { { 0 }, 0 };   /* mode 2: 3 subsets, RGB 5 bits, 2-bit indices */
        put(&c, 4, 3); put(&c, (uint32_t)p, 6);
        for (int ch = 0; ch < 3; ch++) { put(&c, 0, 5); put(&c, 0, 5); put(&c, 0, 5); put(&c, 15, 5); put(&c, 0, 5); put(&c, 31, 5); }
        put(&c, 0x1FFFFFFFu, 29);
        if (c.pos != 128) return fprintf(stderr, "mode 2 bits %d\n", c.pos), 1;
        memcpy(blocks[1][p], c.w, 16);
    }
    for (int t = 0; t < 2; t++) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = 256; td.Height = 4; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_BC7_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = { blocks[t], 64 * 16, 0 };
        ID3D11Texture2D* tx = NULL;
        ID3D11ShaderResourceView* srv = NULL;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        memset(&vd, 0, sizeof vd);
        vd.Format = DXGI_FORMAT_BC7_UNORM; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray.MipLevels = 1; vd.Texture2DArray.ArraySize = 1;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, &sd, &tx)) || FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)tx, &vd, &srv)))
            return fprintf(stderr, "BC7 texture failed\n"), 1;
        static uint32_t px[256 * 4];
        decode(srv, 256, 4, 0, 0, px);
        printf(t == 0 ? "static const uint bc7_p2[64] = {" : "static const uint bc7_p3[64] = {");   /* 2 bits per pixel: subset */
        uint32_t anc[64][2];
        for (int p = 0; p < 64; p++) {
            uint32_t m = 0;
            anc[p][0] = anc[p][1] = 0;
            for (int i = 0; i < 16; i++) {
                uint32_t r = px[(i >> 2) * 256 + p * 4 + (i & 3)] & 255, s, a;
                if (t == 0) { s = r > 40; a = s && r < 160; }
                else { s = r < 20 ? 0 : r < 140 && r != 84 ? 1 : 2; a = (r > 20 && r < 60) || (r > 60 && r < 100); if (r > 20 && r < 60) s = 1; if (r > 60 && r < 100) s = 2; }
                m |= s << (2 * i);
                if (a) anc[p][s - 1] = (uint32_t)i;
            }
            printf("%s0x%08X", p ? (p % 8 ? ", " : ",\n    ") : "\n    ", m);
        }
        printf(" };\n");
        printf(t == 0 ? "static const uint bc7_a2[64] = {" : "static const uint bc7_a3[64] = {");   /* anchors: subset 1 | subset 2 << 4 */
        for (int p = 0; p < 64; p++) printf("%s%u", p ? (p % 16 ? ", " : ",\n    ") : "\n    ", anc[p][0] | (t ? anc[p][1] << 4 : 0));
        printf(" };\n");
        ID3D11ShaderResourceView_Release(srv);
        ID3D11Texture2D_Release(tx);
    }
    return 0;
}

/* ---- images ------------------------------------------------------------------------------------------------------------ */
static uint32_t* load_img(const wchar_t* path, uint32_t* w, uint32_t* h) {   /* RGBA8, alpha as decoded (255 = opaque) */
    IWICBitmapDecoder* d = NULL;
    IWICBitmapFrameDecode* fr = NULL;
    IWICFormatConverter* cv = NULL;
    uint32_t* px = NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(wic, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(d, 0, &fr)) && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(wic, &cv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource*)fr, &GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0.0,
                                                 WICBitmapPaletteTypeCustom))) {
        UINT W = 0, H = 0;
        IWICFormatConverter_GetSize(cv, &W, &H);
        px = malloc((size_t)W * H * 4);
        if (px && FAILED(IWICFormatConverter_CopyPixels(cv, NULL, W * 4, W * H * 4, (BYTE*)px))) { free(px); px = NULL; }
        *w = W; *h = H;
    }
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (d) IWICBitmapDecoder_Release(d);
    return px;
}
typedef struct { double se_rgb, se_a; int max_rgb, max_a; uint64_t n; } err_t;
static void err_add(err_t* e, const uint32_t* a, const uint32_t* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        for (int c = 0; c < 4; c++) {
            int d = (int)(a[i] >> (8 * c) & 255) - (int)(b[i] >> (8 * c) & 255);
            if (d < 0) d = -d;
            if (c < 3) { e->se_rgb += (double)d * d; if (d > e->max_rgb) e->max_rgb = d; }
            else { e->se_a += (double)d * d; if (d > e->max_a) e->max_a = d; }
        }
    }
    e->n += n;
}
static double psnr(double se, uint64_t n, double peak) { return se <= 0 ? 99.0 : 10.0 * log10(peak * peak * n / se); }

typedef struct { uint64_t key; wchar_t path[600]; } entry;
static int list_dir(const char* dir, entry** out) {
    wchar_t pat[600];
    _snwprintf(pat, 600, L"%hs\\*.*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    int n = 0, cap = 0;
    *out = NULL;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (wcslen(fd.cFileName) != 20 || (_wcsicmp(fd.cFileName + 16, L".jpg") && _wcsicmp(fd.cFileName + 16, L".png"))) continue;
        if (n == cap) { cap = cap ? cap * 2 : 256; *out = realloc(*out, (size_t)cap * sizeof **out); }
        (*out)[n].key = _wcstoui64(fd.cFileName, NULL, 16);
        _snwprintf((*out)[n].path, 600, L"%hs\\%ls", dir, fd.cFileName);
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* GS alpha (the game's decode: 255 -> 0x80) and back */
static uint32_t to_gs(uint32_t p) { uint32_t a = ((p >> 24) * 128u + 127u) / 255u; return (p & 0xFFFFFFu) | a << 24; }

/* ---- encode: per file the chain (level 0 + 2x2 box mips) in an RGBA8 array, BC7 per level, decoded by the GPU -------- */
static int encode(const char* hlsl, const char* dir, const char* csv) {
    size_t ns;
    char* src = read_file(hlsl, &ns);
    if (!src) return fprintf(stderr, "cannot read %s\n", hlsl), 1;
    D3D_SHADER_MACRO mac[9] = { { 0 } };   /* BC7_DEFINES="A=1,B" (experiments) */
    static char dfs[256];
    int nmac = 0;
    if (getenv("BC7_DEFINES")) {
        snprintf(dfs, sizeof dfs, "%s", getenv("BC7_DEFINES"));
        for (char* tok = strtok(dfs, ","); tok && nmac < 8; tok = strtok(NULL, ",")) {
            char* eq = strchr(tok, '=');
            if (eq) *eq = 0;
            mac[nmac].Name = tok; mac[nmac].Definition = eq ? eq + 1 : "1"; nmac++;
        }
    }
    ID3D11ComputeShader* enc = cs_from(src, ns, hlsl, "cs_bc7", mac);
    if (!enc) return 1;
    dec_init();
    entry* ent;
    int n = list_dir(dir, &ent);
    FILE* fo = fopen(csv, "w");
    if (!fo) return 1;
    fprintf(fo, "key,w,h,alpha,psnr_rgb0,max_rgb0,psnr_a0,max_a0,psnr_rgb_all,max_rgb_all,max_a_all,modes\n");
    D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, qt = { D3D11_QUERY_TIMESTAMP, 0 };
    ID3D11Query *qdj, *q0, *q1;
    ID3D11Device_CreateQuery(dev, &qd, &qdj);
    ID3D11Device_CreateQuery(dev, &qt, &q0);
    ID3D11Device_CreateQuery(dev, &qt, &q1);
    double gpu_ms = 0, mtex = 0, t0 = now();
    int done = 0, n_alpha = 0;
    uint32_t* dec = malloc(2048 * 2048 * 4);
    uint32_t* lv = malloc(2048 * 2048 * 4 * 2);
    uint64_t modes_all[8] = { 0 };
    for (int f = 0; f < n; f++) {
        uint32_t w, h;
        uint32_t* px = load_img(ent[f].path, &w, &h);
        if (!px || w < 4 || h < 4 || w > 2048 || h > 2048) { free(px); continue; }
        int alpha = 0;
        for (size_t i = 0; i < (size_t)w * h; i++) { px[i] = to_gs(px[i]); alpha |= (px[i] >> 24) != 128; }
        if (alpha) { n_alpha++; fprintf(fo, "%016llx,%u,%u,1,,,,,,,,kept RGBA8\n", (unsigned long long)ent[f].key, w, h); free(px); continue; }
        if (w != h) {                    /* (the classes are square: the game stretches the short axis - bilinear here) */
            uint32_t S = w > h ? w : h;
            uint32_t* sq = malloc((size_t)S * S * 4);
            for (uint32_t y = 0; y < S; y++)
                for (uint32_t x = 0; x < S; x++) {
                    float fx = (x + 0.5f) * w / S - 0.5f, fy = (y + 0.5f) * h / S - 0.5f;
                    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
                    float ax = fx - x0, ay = fy - y0;
                    int x1 = x0 + 1 < (int)w ? x0 + 1 : (int)w - 1, y1 = y0 + 1 < (int)h ? y0 + 1 : (int)h - 1;
                    if (x0 < 0) x0 = 0;
                    if (y0 < 0) y0 = 0;
                    uint32_t o = 0;
                    for (int c = 0; c < 4; c++) {
                        float v = (1 - ay) * ((1 - ax) * (px[y0 * w + x0] >> (8 * c) & 255) + ax * (px[y0 * w + x1] >> (8 * c) & 255)) +
                                  ay * ((1 - ax) * (px[y1 * w + x0] >> (8 * c) & 255) + ax * (px[y1 * w + x1] >> (8 * c) & 255));
                        o |= (uint32_t)(v + 0.5f) << (8 * c);
                    }
                    sq[y * S + x] = o;
                }
            free(px);
            px = sq; w = h = S;
        }
        uint32_t nm = 1;
        while ((w >> nm) >= 1 && nm < 12) nm++;   /* (levels down to 1 x 1) */
        nm = 0; for (uint32_t s = w; s; s >>= 1) nm++;
        /* the chain (box: the test only needs plausible lower levels; level 0 is the game's image) */
        uint32_t* L[12];
        L[0] = px;
        size_t off = 0;
        for (uint32_t k = 1; k < nm; k++) {
            uint32_t s = w >> k, ps = w >> (k - 1);
            L[k] = lv + off; off += (size_t)s * s;
            for (uint32_t y = 0; y < s; y++)
                for (uint32_t x = 0; x < s; x++) {
                    uint32_t o = 0;
                    for (int c = 0; c < 4; c++) {
                        uint32_t sum = 0;
                        for (int j = 0; j < 4; j++) sum += L[k - 1][(2 * y + (j >> 1)) * ps + 2 * x + (j & 1)] >> (8 * c) & 255;
                        o |= ((sum + 2) / 4) << (8 * c);
                    }
                    L[k][y * s + x] = o;
                }
        }
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = td.Height = w; td.MipLevels = nm; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd[12];
        for (uint32_t k = 0; k < nm; k++) { sd[k].pSysMem = L[k]; sd[k].SysMemPitch = (w >> k) * 4; sd[k].SysMemSlicePitch = 0; }
        ID3D11Texture2D *rt = NULL, *bt = NULL, *ut[12] = { 0 };
        ID3D11ShaderResourceView *rs = NULL, *bs = NULL;
        ID3D11UnorderedAccessView* uu[12] = { 0 };
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        memset(&vd, 0, sizeof vd);
        vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray.MipLevels = nm; vd.Texture2DArray.ArraySize = 1;
        ID3D11Device_CreateTexture2D(dev, &td, sd, &rt);
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)rt, &vd, &rs);
        td.Format = DXGI_FORMAT_BC7_UNORM;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &bt);
        vd.Format = DXGI_FORMAT_BC7_UNORM;
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)bt, &vd, &bs);
        for (uint32_t k = 0; k < nm; k++) {   /* the blocks: R32G32B32A32_UINT, one texel per block */
            uint32_t b = ((w >> k) + 3) / 4;
            D3D11_TEXTURE2D_DESC u;
            memset(&u, 0, sizeof u);
            u.Width = u.Height = b; u.MipLevels = 1; u.ArraySize = 1; u.Format = DXGI_FORMAT_R32G32B32A32_UINT;
            u.SampleDesc.Count = 1; u.Usage = D3D11_USAGE_DEFAULT; u.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            ID3D11Device_CreateTexture2D(dev, &u, NULL, &ut[k]);
            ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource*)ut[k], NULL, &uu[k]);
        }
        ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous*)qdj);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous*)q0);
        ID3D11DeviceContext_CSSetShader(ctx, enc, NULL, 0);
        ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &rs);
        for (uint32_t k = 0; k < nm; k++) {
            uint32_t s = w >> k, b = (s + 3) / 4;
            uint32_t v[8] = { 0, k, b, b, s, s, 0, 0 };
            cb_set(v);
            ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &cb);
            ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &uu[k], NULL);
            ID3D11DeviceContext_Dispatch(ctx, (b + 7) / 8, (b + 7) / 8, 1);
        }
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous*)q1);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous*)qdj);
        ID3D11UnorderedAccessView* nu = NULL;
        ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &nu, NULL);
        for (uint32_t k = 0; k < nm; k++) {
            uint32_t b = ((w >> k) + 3) / 4;
            D3D11_BOX bx = { 0, 0, 0, b, b, 1 };
            ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource*)bt, k, 0, 0, 0, (ID3D11Resource*)ut[k], 0, &bx);
        }
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 a = 0, z = 0;
        while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous*)qdj, &dj, sizeof dj, 0) != S_OK) {}
        ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous*)q0, &a, sizeof a, 0);
        ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous*)q1, &z, sizeof z, 0);
        if (!dj.Disjoint && z > a) { gpu_ms += (double)(z - a) * 1000.0 / (double)dj.Frequency; mtex += (double)w * w * 4.0 / 3.0 / 1048576.0; }
        err_t e0 = { 0 }, ea = { 0 };
        for (uint32_t k = 0; k < nm; k++) {
            uint32_t s = w >> k;
            decode(bs, s, s, 0, k, dec);
            for (size_t i = 0; i < (size_t)s * s; i++) {   /* (BC7 alpha holds 0..255 for the GS's 0..0x80: back) */
                uint32_t A = dec[i] >> 24;
                dec[i] = (dec[i] & 0xFFFFFFu) | ((A * 128u + 127u) / 255u) << 24;
            }
            if (k == 0) err_add(&e0, L[0], dec, (size_t)s * s);
            err_add(&ea, L[k], dec, (size_t)s * s);
        }
        /* the modes used at level 0 (the block's lowest set bit) */
        {
            ID3D11Texture2D* st = NULL;
            uint32_t b = w / 4;
            D3D11_TEXTURE2D_DESC u;
            memset(&u, 0, sizeof u);
            u.Width = u.Height = b; u.MipLevels = 1; u.ArraySize = 1; u.Format = DXGI_FORMAT_R32G32B32A32_UINT;
            u.SampleDesc.Count = 1; u.Usage = D3D11_USAGE_STAGING; u.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Device_CreateTexture2D(dev, &u, NULL, &st);
            ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)st, (ID3D11Resource*)ut[0]);
            D3D11_MAPPED_SUBRESOURCE m;
            ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)st, 0, D3D11_MAP_READ, 0, &m);
            uint32_t cnt[9] = { 0 };
            for (uint32_t y = 0; y < b; y++)
                for (uint32_t x = 0; x < b; x++) {
                    uint32_t v = ((uint32_t*)((char*)m.pData + y * m.RowPitch))[x * 4];
                    int md = 0;
                    while (md < 8 && !(v >> md & 1)) md++;
                    cnt[md]++;
                }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)st, 0);
            ID3D11Texture2D_Release(st);
            char ms[128] = "";
            for (int md = 0; md < 8; md++) { modes_all[md] += cnt[md]; if (cnt[md]) { char t[24]; snprintf(t, 24, "%s%d:%u", ms[0] ? " " : "", md, cnt[md]); strcat(ms, t); } }
            fprintf(fo, "%016llx,%u,%u,%d,%.2f,%d,%.2f,%d,%.2f,%d,%d,%s\n", (unsigned long long)ent[f].key, w, h, alpha,
                    psnr(e0.se_rgb, e0.n * 3, 255), e0.max_rgb, psnr(e0.se_a, e0.n, 128), e0.max_a, psnr(ea.se_rgb, ea.n * 3, 255),
                    ea.max_rgb, ea.max_a, ms);
        }
        for (uint32_t k = 0; k < nm; k++) { ID3D11UnorderedAccessView_Release(uu[k]); ID3D11Texture2D_Release(ut[k]); }
        ID3D11ShaderResourceView_Release(rs); ID3D11ShaderResourceView_Release(bs);
        ID3D11Texture2D_Release(rt); ID3D11Texture2D_Release(bt);
        free(px);
        done++;
    }
    fclose(fo);
    printf("bc7: %d images with alpha kept RGBA8\n", n_alpha);
    printf("bc7: %d images, %.1f M texels (with mips), GPU %.1f ms = %.3f ms per M texels; wall %.1f s\n", done, mtex, gpu_ms,
           mtex > 0 ? gpu_ms / mtex : 0.0, now() - t0);
    printf("bc7: level-0 blocks per mode:");
    for (int md = 0; md < 8; md++) printf(" %d:%llu", md, (unsigned long long)modes_all[md]);
    printf("\n");
    return 0;
}

/* time <bc7.hlsl> <dir> [reps]: every opaque square image uploaded first, then all encodes (every level) back to back
 * between two timestamps (no read-backs in between: the GPU stays busy, its clocks up); the best of reps */
static int time_enc(const char* hlsl, const char* dir, int reps) {
    size_t ns;
    char* src = read_file(hlsl, &ns);
    if (!src) return 1;
    D3D_SHADER_MACRO mac[9] = { { 0 } };
    static char dfs[256];
    int nmac = 0;
    if (getenv("BC7_DEFINES")) {
        snprintf(dfs, sizeof dfs, "%s", getenv("BC7_DEFINES"));
        for (char* tok = strtok(dfs, ","); tok && nmac < 8; tok = strtok(NULL, ",")) {
            char* eq = strchr(tok, '=');
            if (eq) *eq = 0;
            mac[nmac].Name = tok; mac[nmac].Definition = eq ? eq + 1 : "1"; nmac++;
        }
    }
    ID3D11ComputeShader* enc = cs_from(src, ns, hlsl, "cs_bc7", mac);
    if (!enc) return 1;
    dec_init();
    entry* ent;
    int n = list_dir(dir, &ent), m = 0;
    typedef struct { ID3D11ShaderResourceView* srv; uint32_t w, nm; } timg;
    timg* im = calloc((size_t)n + 1, sizeof *im);
    ID3D11UnorderedAccessView* bu[12] = { 0 };
    for (int k = 0; k < 12; k++) {      /* (one block scratch per size, shared: the timing only) */
        uint32_t b = (2048u >> k) / 4u;
        if (!b) b = 1;
        D3D11_TEXTURE2D_DESC u;
        memset(&u, 0, sizeof u);
        u.Width = u.Height = b; u.MipLevels = 1; u.ArraySize = 1; u.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        u.SampleDesc.Count = 1; u.Usage = D3D11_USAGE_DEFAULT; u.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        ID3D11Texture2D* t = NULL;
        ID3D11Device_CreateTexture2D(dev, &u, NULL, &t);
        ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource*)t, NULL, &bu[k]);
    }
    double mtex = 0;
    for (int f = 0; f < n; f++) {
        uint32_t w, h;
        uint32_t* px = load_img(ent[f].path, &w, &h);
        int op = px != NULL;
        for (size_t i = 0; px && i < (size_t)w * h; i++) op &= (px[i] >> 24) == 255;
        if (!op || w != h || w < 4) { free(px); continue; }
        uint32_t nm = 0;
        for (uint32_t s = w; s; s >>= 1) nm++;
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = td.Height = w; td.MipLevels = nm; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        ID3D11Texture2D* t = NULL;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &t);
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource*)t, 0, NULL, px, w * 4, 0);
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        memset(&vd, 0, sizeof vd);
        vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray.MipLevels = nm; vd.Texture2DArray.ArraySize = 1;
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)t, &vd, &im[m].srv);
        ID3D11DeviceContext_GenerateMips(ctx, im[m].srv);
        im[m].w = w; im[m].nm = nm;
        mtex += (double)w * w * 4.0 / 3.0 / 1048576.0;
        m++;
        free(px);
    }
    D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, qt = { D3D11_QUERY_TIMESTAMP, 0 };
    ID3D11Query *qdj, *q0, *q1;
    ID3D11Device_CreateQuery(dev, &qd, &qdj);
    ID3D11Device_CreateQuery(dev, &qt, &q0);
    ID3D11Device_CreateQuery(dev, &qt, &q1);
    ID3D11DeviceContext_CSSetShader(ctx, enc, NULL, 0);
    double best = 1e30;
    for (int r = 0; r < reps; r++) {
        ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous*)qdj);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous*)q0);
        for (int i = 0; i < m; i++) {
            ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &im[i].srv);
            for (uint32_t k = 0; k < im[i].nm; k++) {
                uint32_t s = im[i].w >> k, b = (s + 3) / 4, j = 0;
                while (j < 11 && (2048u >> j) / 4u > b) j++;
                uint32_t v[8] = { 0, k, b, b, s, s, 0, 0 };
                cb_set(v);
                ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &cb);
                ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &bu[j], NULL);
                ID3D11DeviceContext_Dispatch(ctx, (b + 7) / 8, (b + 7) / 8, 1);
            }
        }
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous*)q1);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous*)qdj);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 a = 0, z = 0;
        while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous*)qdj, &dj, sizeof dj, 0) != S_OK) {}
        while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous*)q1, &z, sizeof z, 0) != S_OK) {}
        ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous*)q0, &a, sizeof a, 0);
        double ms = (double)(z - a) * 1000.0 / (double)dj.Frequency;
        if (!dj.Disjoint && ms < best) best = ms;
    }
    printf("bc7 time: %d images, %.1f M texels with mips: best of %d %.1f ms = %.3f ms per M texels\n", m, mtex, reps, best, best / mtex);
    return 0;
}

static int cmp_u64(const void* a, const void* b) { uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b; return x < y ? -1 : x > y; }
static int dedupe(const char* dir) {
    entry* e;
    int n = list_dir(dir, &e);
    uint64_t* raw = malloc((size_t)n * 16);
    for (int i = 0; i < n; i++) { raw[2 * i] = e[i].key & ~(7ull << 60); raw[2 * i + 1] = (uint64_t)i; }
    qsort(raw, (size_t)n, 16, cmp_u64);
    int groups = 0, extra = 0, same = 0, near_ = 0, diff = 0;
    uint64_t bytes = 0, xb = 0;
    int kinds[8] = { 0 };
    for (int i = 0; i < n;) {
        int j = i + 1;
        while (j < n && raw[2 * j] == raw[2 * i]) j++;
        groups++;
        if (j - i > 1) {
            int a0 = (int)raw[2 * i + 1];
            uint32_t w0, h0;
            uint32_t* p0 = load_img(e[a0].path, &w0, &h0);
            for (int k = i + 1; k < j; k++) {
                int ak = (int)raw[2 * k + 1];
                uint32_t w, h;
                uint32_t* p = load_img(e[ak].path, &w, &h);
                WIN32_FILE_ATTRIBUTE_DATA fa;
                if (GetFileAttributesExW(e[ak].path, GetFileExInfoStandard, &fa)) xb += fa.nFileSizeLow;
                kinds[((e[a0].key ^ e[ak].key) >> 60) & 7]++;
                extra++;
                if (!p || !p0 || w != w0 || h != h0) { diff++; free(p); continue; }
                err_t er = { 0 };
                err_add(&er, p0, p, (size_t)w * h);
                double ps = psnr(er.se_rgb, er.n * 3, 255);
                if (ps >= 60) same++; else if (ps >= 40) near_++; else diff++;
                free(p);
            }
            free(p0);
        }
        i = j;
    }
    for (int i = 0; i < n; i++) { WIN32_FILE_ATTRIBUTE_DATA fa; if (GetFileAttributesExW(e[i].path, GetFileExInfoStandard, &fa)) bytes += fa.nFileSizeLow; }
    printf("%s: %d files (%.1f MB), %d distinct contents; %d extra files (%.2f MB) for keys differing only in bits 60..62: "
           ">= 60 dB alike %d, 40-60 dB %d, < 40 dB / other size %d\n", dir, n, bytes / 1e6, groups, extra, xb / 1e6, same, near_, diff);
    printf("  key differences (bit 0 = alpha test, 1 = clamp u, 2 = clamp v):");
    for (int k = 1; k < 8; k++) printf(" %d:%d", k, kinds[k]);
    printf("\n");
    return 0;
}

static int diff_dirs(const char* da, const char* db, const char* csv) {
    entry* e;
    int n = list_dir(da, &e);
    FILE* fo = fopen(csv, "w");
    fprintf(fo, "key,w,h,psnr_rgb,max_rgb,psnr_a,max_a\n");
    int m = 0;
    for (int i = 0; i < n; i++) {
        wchar_t pb[700];
        const wchar_t* nm = wcsrchr(e[i].path, L'\\') + 1;
        _snwprintf(pb, 700, L"%hs\\%ls", db, nm);
        if (GetFileAttributesW(pb) == INVALID_FILE_ATTRIBUTES) continue;
        uint32_t w, h, w2, h2;
        uint32_t *a = load_img(e[i].path, &w, &h), *b = load_img(pb, &w2, &h2);
        if (a && b && w == w2 && h == h2) {
            err_t er = { 0 };
            err_add(&er, a, b, (size_t)w * h);
            fprintf(fo, "%016llx,%u,%u,%.2f,%d,%.2f,%d\n", (unsigned long long)e[i].key, w, h, psnr(er.se_rgb, er.n * 3, 255), er.max_rgb,
                    psnr(er.se_a, er.n, 255), er.max_a);
            m++;
        }
        free(a); free(b);
    }
    fclose(fo);
    printf("diff: %d pairs -> %s\n", m, csv);
    return 0;
}

static int mkbatch(const char* dir, const char* outp, int lim) {
    entry* e;
    int n = list_dir(dir, &e), m = 0;
    FILE* f = fopen(outp, "wb");
    uint32_t hd[2] = { 0x31424941u, 0 };   /* "AIB1" */
    fwrite(hd, 4, 2, f);
    for (int i = 0; i < n && (lim <= 0 || m < lim); i++) {
        uint32_t w, h;
        uint32_t* px = load_img(e[i].path, &w, &h);
        int op = px != NULL;
        for (size_t k = 0; px && k < (size_t)w * h; k++) op &= (px[k] >> 24) == 255;
        if (!op || w < 16 || h < 16) { free(px); continue; }
        uint32_t sw = w / 4, sh = h / 4, cw = sw > 128 ? 128 : sw, ch = sh > 128 ? 128 : sh, P = 12, pw = cw + 2 * P, ph = ch + 2 * P;
        uint8_t* o = malloc((size_t)pw * ph * 3);
        for (uint32_t y = 0; y < ph; y++)
            for (uint32_t x = 0; x < pw; x++) {
                int sx = (int)x - (int)P, sy = (int)y - (int)P;
                sx = sx < 0 ? 0 : sx >= (int)cw ? (int)cw - 1 : sx;
                sy = sy < 0 ? 0 : sy >= (int)ch ? (int)ch - 1 : sy;
                for (int c = 0; c < 3; c++) {
                    uint32_t s = 0;
                    for (int j = 0; j < 16; j++) s += px[(sy * 4 + (j >> 2)) * w + sx * 4 + (j & 3)] >> (8 * c) & 255;
                    o[(y * pw + x) * 3 + c] = (uint8_t)((s + 8) / 16);
                }
            }
        uint32_t d[2] = { pw, ph };
        fwrite(d, 4, 2, f);
        fwrite(o, 1, (size_t)pw * ph * 3, f);
        free(o); free(px);
        m++;
    }
    fseek(f, 4, SEEK_SET);
    fwrite(&m, 4, 1, f);
    fclose(f);
    printf("mkbatch: %d images -> %s\n", m, outp);
    return 0;
}
static uint8_t** read_out(const char* p, uint32_t* n, uint32_t** dims) {
    FILE* f = fopen(p, "rb");
    uint32_t hd[2];
    if (!f || fread(hd, 4, 2, f) != 2) return NULL;
    *n = hd[1];
    uint8_t** im = calloc(hd[1], sizeof *im);
    *dims = calloc(hd[1] * 2, 4);
    for (uint32_t i = 0; i < hd[1]; i++) {
        fread(*dims + 2 * i, 4, 2, f);
        size_t b = (size_t)(*dims)[2 * i] * (*dims)[2 * i + 1] * 3;
        im[i] = malloc(b);
        fread(im[i], 1, b, f);
    }
    fclose(f);
    return im;
}
static int cmpout(const char* a, const char* b, const char* csv) {
    uint32_t na, nb, *da, *db;
    uint8_t **A = read_out(a, &na, &da), **B = read_out(b, &nb, &db);
    if (!A || !B || na != nb) return fprintf(stderr, "outputs differ in count\n"), 1;
    FILE* fo = fopen(csv, "w");
    fprintf(fo, "image,w,h,psnr,max\n");
    double worst = 99, se_all = 0;
    uint64_t n_all = 0;
    int mx_all = 0;
    for (uint32_t i = 0; i < na; i++) {
        size_t n = (size_t)da[2 * i] * da[2 * i + 1] * 3;
        double se = 0;
        int mx = 0;
        for (size_t k = 0; k < n; k++) { int d = (int)A[i][k] - (int)B[i][k]; if (d < 0) d = -d; se += (double)d * d; if (d > mx) mx = d; }
        double ps = psnr(se, n, 255);
        fprintf(fo, "%u,%u,%u,%.2f,%d\n", i, da[2 * i], da[2 * i + 1], ps, mx);
        if (ps < worst) worst = ps;
        if (mx > mx_all) mx_all = mx;
        se_all += se; n_all += n;
    }
    fclose(fo);
    printf("cmpout: %u images: worst PSNR %.2f dB, overall %.2f dB, max abs error %d / 255\n", na, worst, psnr(se_all, n_all, 255), mx_all);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2 || init()) { fprintf(stderr, "usage: see scripts/bc7_tool.c (needs D3D11 + WIC)\n"); return 2; }
    if (!strcmp(argv[1], "probe")) return probe();
    if (!strcmp(argv[1], "encode") && argc == 5) return encode(argv[2], argv[3], argv[4]);
    if (!strcmp(argv[1], "dedupe") && argc == 3) return dedupe(argv[2]);
    if (!strcmp(argv[1], "time") && argc >= 4) return time_enc(argv[2], argv[3], argc > 4 ? atoi(argv[4]) : 5);
    if (!strcmp(argv[1], "diff") && argc == 5) return diff_dirs(argv[2], argv[3], argv[4]);
    if (!strcmp(argv[1], "mkbatch") && argc >= 4) return mkbatch(argv[2], argv[3], argc > 4 ? atoi(argv[4]) : 0);
    if (!strcmp(argv[1], "cmpout") && argc == 5) return cmpout(argv[2], argv[3], argv[4]);
    fprintf(stderr, "usage: see scripts/bc7_tool.c\n");
    return 2;
}
