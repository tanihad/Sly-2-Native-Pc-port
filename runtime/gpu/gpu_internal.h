/* Render-thread internals of the D3D11 backend (gpu_d3d11.c, gpu_present.c, gpu_conf.c).
 * Everything here belongs to the render thread; the GS side only uses gpu.h and gpu_cmd.h. */
#pragma once
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include "gpu.h"
#include "gpu_cmd.h"

#include <stdio.h>
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_5.h>

#define GPU_RELEASE(p) do { if (p) { IUnknown_Release((IUnknown*)(p)); (p) = NULL; } } while (0)

/* Interface IDs, defined locally so no dxguid/uuid import library is needed (as window.c). */
extern const IID gpu_IID_IDXGIDevice, gpu_IID_IDXGIFactory2, gpu_IID_IDXGIFactory5, gpu_IID_IDXGISwapChain2,
    gpu_IID_ID3D11Texture2D, gpu_IID_ID3D11DeviceContext1, gpu_IID_ID3D11Device1;

typedef struct {
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    ID3D11DeviceContext1* ctx1;          /* D3D11.1 (constant buffer offsets) */
    gpu_config cfg;
    gpu_caps caps;
    int lost;                            /* device removed: commands are skipped */
    /* per-picture counters (gpu_present.c fills gpu_frame_stats from them) */
    uint32_t n_draws, n_dispatches, n_copies, n_uploads, upload_bytes;
} gpu_rt_state;

extern gpu_rt_state gpu_rt;

void gpu_log(const char* fmt, ...);

/* shader objects created on first use from the build-time blobs (build/gen/gpu_shaders.h) */
ID3D11VertexShader*  gpu_vs(int id);
ID3D11PixelShader*   gpu_ps(int id);
ID3D11ComputeShader* gpu_cs(int id);
void gpu_shaders_release(void);

/* gpu_present.c */
int  gpu_present_init(void);             /* swap chain or offscreen target, samplers, queries */
void gpu_present_release(void);
void gpu_exec_picture(const void* payload);
void gpu_exec_present(const void* payload);
void gpu_exec_readback(const void* payload);
int  gpu_present_poll(int block);        /* finish read-backs / timestamps; returns 1 while some pending */
void gpu_present_drain(void);            /* block until every read-back and timestamp has completed */

/* payloads */
typedef struct {
    const uint32_t* px;                  /* in the data ring (NULL: display off) */
    int w, h, stride;
    uint32_t vblank;
    double front_ms;
} gpu_op_picture;
typedef struct { uint32_t vblank; } gpu_op_present;
typedef struct { gpu_readback* rb; } gpu_op_readback;
