/* Early depth rejection probe (pc-research, notes/perf-plan.md): does the GPU still reject hidden fragments before
 * the pixel shader when the shader writes depth (SV_Depth, as runtime/gpu's exact integer Z does), or writes
 * conservative depth (SV_DepthLessEqual / SV_DepthGreaterEqual) in either test direction?
 *
 * Headless (no window): a 2048 x 1344 RGBA8 + D32 target (3x 16:9 field buffer), an occluder quad, then N full-screen
 * quads behind it with an expensive pixel shader; GPU time of the hidden quads from timestamp queries, against the
 * same quads in front (all shaded). ratio = hidden / visible: ~0 = early rejection works, ~1 = every fragment shaded.
 *
 *   clang -O1 -o build\earlyz_probe.exe scripts\earlyz_probe.c -ld3d11 -ldxgi -ldxguid
 *   earlyz_probe.exe [quads (default 16)] [shader loop iterations (default 64)]
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT(WINAPI* compile_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                     UINT, ID3DBlob**, ID3DBlob**);
static compile_fn compile;

static const char* hlsl =
    "cbuffer C : register(b0) { float zv; uint iters; float zbias; uint pad; };\n"
    "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p = float2((id & 1) ? 1.0 : -1.0, (id & 2) ? -1.0 : 1.0);\n"
    "  return float4(p, zv, 1.0); }\n"
    "float4 heavy(float4 pos) { float4 a = pos * 0.001; [loop] for (uint i = 0; i < iters; i++) a = frac(a * 1.37 + a.yzwx * 0.71 + 0.13); return a; }\n"
    "struct O { float4 c : SV_Target0;\n"
    "#if ZMODE == 1\n float z : SV_Depth;\n#elif ZMODE == 2\n float z : SV_DepthLessEqual;\n#elif ZMODE == 3\n float z : SV_DepthGreaterEqual;\n#endif\n"
    "};\n"
    "#if ZMODE >= 2\n#define PI noperspective centroid\n#else\n#define PI\n#endif\nO ps(PI float4 pos : SV_Position) { O o; o.c = heavy(pos);\n"
    "#if ZMODE != 0\n o.z = pos.z + zbias;\n#endif\n"
    " return o; }\n";

static ID3D11Device* dev;
static ID3D11DeviceContext* ctx;

static ID3DBlob* cc(const char* entry, const char* target, int zmode) {
    char zm[8];
    snprintf(zm, sizeof zm, "%d", zmode);
    D3D_SHADER_MACRO defs[] = { { "ZMODE", zm }, { NULL, NULL } };
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = compile(hlsl, strlen(hlsl), "probe", defs, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) { fprintf(stderr, "compile %s: %s\n", entry, err ? (char*)ID3D10Blob_GetBufferPointer(err) : "?"); exit(1); }
    return code;
}

int main(int argc, char** argv) {
    int quads = argc > 1 ? atoi(argv[1]) : 16;
    unsigned iters = argc > 2 ? (unsigned)atoi(argv[2]) : 64;
    char dll[MAX_PATH];
    GetSystemDirectoryA(dll, MAX_PATH);
    strcat(dll, "\\d3dcompiler_47.dll");
    HMODULE m = LoadLibraryA(dll);
    compile = m ? (compile_fn)(void*)GetProcAddress(m, "D3DCompile") : NULL;
    if (!compile) { fprintf(stderr, "no d3dcompiler_47\n"); return 1; }
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx))) {
        fprintf(stderr, "no device\n"); return 1;
    }
    const UINT W = 2048, H = 1344;
    D3D11_TEXTURE2D_DESC td = { W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
    ID3D11Texture2D *ct, *dt;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &ct);
    td.Format = DXGI_FORMAT_D32_FLOAT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &dt);
    ID3D11RenderTargetView* rtv; ID3D11DepthStencilView* dsv;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)ct, NULL, &rtv);
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource*)dt, NULL, &dsv);
    D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    ID3D11Buffer* cb;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &cb);
    ID3DBlob* vb = cc("vs", "vs_5_0", 0);
    ID3D11VertexShader* vsh;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &vsh);
    ID3D11PixelShader* psh[4];
    for (int z = 0; z < 4; z++) {
        ID3DBlob* b = cc("ps", "ps_5_0", z);
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &psh[z]);
    }
    D3D11_DEPTH_STENCIL_DESC dd = { TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_GREATER, FALSE, 0, 0 };
    ID3D11DepthStencilState *dsG, *dsGE, *dsL;
    ID3D11Device_CreateDepthStencilState(dev, &dd, &dsG);
    dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    ID3D11Device_CreateDepthStencilState(dev, &dd, &dsGE);
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    ID3D11Device_CreateDepthStencilState(dev, &dd, &dsL);
    D3D11_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(ctx, vsh, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, dsv);
    D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
    ID3D11Query *qdis, *q0, *q1, *q2;
    ID3D11Device_CreateQuery(dev, &qd, &qdis);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    ID3D11Device_CreateQuery(dev, &qd, &q0); ID3D11Device_CreateQuery(dev, &qd, &q1); ID3D11Device_CreateQuery(dev, &qd, &q2);

    /* cases: occluder shader mode, test shader mode, depth func, direction (greater: near = big z) */
    struct { const char* name; int occ, zm; ID3D11DepthStencilState* ds; int greater; } cases[] = {
        { "no depth output, GREATER (control)", 0, 0, dsG, 1 },
        { "no depth output, LESS (control)", 0, 0, dsL, 0 },
        { "SV_Depth, GREATER (the renderer today)", 1, 1, dsG, 1 },
        { "SV_Depth, GEQUAL", 1, 1, dsGE, 1 },
        { "SV_Depth hidden quads, occluder plain, GREATER", 0, 1, dsG, 1 },
        { "SV_DepthLessEqual, GREATER (M9 attempt)", 2, 2, dsG, 1 },
        { "SV_DepthLessEqual hidden, occluder plain, GREATER", 0, 2, dsG, 1 },
        { "SV_DepthLessEqual, GEQUAL", 2, 2, dsGE, 1 },
        { "SV_DepthGreaterEqual, LESS (reversed depth)", 3, 3, dsL, 0 },
        { "SV_DepthGreaterEqual hidden, occluder plain, LESS", 0, 3, dsL, 0 },
        { "SV_DepthGreaterEqual, GREATER (wrong direction)", 3, 3, dsG, 1 },
        { "SV_DepthLessEqual, LESS (wrong direction)", 2, 2, dsL, 0 },
    };
    printf("target %ux%u, %d full-screen quads, %u loop iterations\n", W, H, quads, iters);
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        double t[2] = { 0, 0 };
        for (int hidden = 0; hidden < 2; hidden++) {
            double best = 1e30;
            for (int rep = 0; rep < 7; rep++) {
                float clear[4] = { 0, 0, 0, 0 };
                ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
                ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv, D3D11_CLEAR_DEPTH, cases[c].greater ? 0.0f : 1.0f, 0);
                ID3D11DeviceContext_OMSetDepthStencilState(ctx, cases[c].ds, 0);
                ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous*)qdis);
                /* occluder: near (greater: 0.8, less: 0.2), cheap */
                struct { float zv; unsigned it; float bias; unsigned pad; } k = { cases[c].greater ? 0.8f : 0.2f, 1, 0, 0 };
                ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource*)cb, 0, NULL, &k, 0, 0);
                ID3D11DeviceContext_PSSetShader(ctx, psh[cases[c].occ], NULL, 0);
                ID3D11DeviceContext_Draw(ctx, 4, 0);
                ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous*)q0);
                /* test quads: hidden = behind the occluder; visible = in front (each one in front of the last) */
                ID3D11DeviceContext_PSSetShader(ctx, psh[cases[c].zm], NULL, 0);
                for (int i = 0; i < quads; i++) {
                    float z = hidden ? (cases[c].greater ? 0.3f - 0.01f * i : 0.7f + 0.01f * i)
                                     : (cases[c].greater ? 0.81f + 0.01f * i : 0.19f - 0.01f * i);
                    k.zv = z; k.it = iters;
                    k.bias = (cases[c].zm == 2) ? -0.0001f : (cases[c].zm == 3) ? 0.0001f : 0.0f;
                    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource*)cb, 0, NULL, &k, 0, 0);
                    ID3D11DeviceContext_Draw(ctx, 4, 0);
                }
                ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous*)q1);
                ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous*)qdis);
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
                UINT64 a, b;
                while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous*)qdis, &dj, sizeof dj, 0) != S_OK) {}
                ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous*)q0, &a, sizeof a, 0);
                ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous*)q1, &b, sizeof b, 0);
                if (dj.Disjoint) { rep--; continue; }
                double ms = (double)(b - a) * 1000.0 / (double)dj.Frequency;
                if (rep > 0 && ms < best) best = ms;
            }
            t[hidden] = best;
        }
        printf("%-52s visible %7.3f ms  hidden %7.3f ms  ratio %.3f\n", cases[c].name, t[0], t[1], t[1] / t[0]);
    }
    return 0;
}
