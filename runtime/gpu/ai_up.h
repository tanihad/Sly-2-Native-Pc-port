/* (ai) Real-ESRGAN x4 (RRDBNet) inference on D3D11 compute shaders, and a plain C reference of the same network
 * (notes/ai-textures.md). Phase 1: used by the standalone tool scripts/ai_up_tool.c; not linked into the game. */
#ifndef AI_UP_H
#define AI_UP_H
#include <stddef.h>
#include <stdint.h>

typedef struct ai_weights ai_weights;
typedef struct ai_gpu ai_gpu;
struct ID3D11Device;
struct ID3D11DeviceContext;

/* weights.bin as written by scripts/ai_model.py export (SLYAIW01); NULL + err on failure */
ai_weights* ai_weights_load(const char* path, char* err, size_t errn);
void ai_weights_free(ai_weights* w);

/* CPU reference (slow): in = 3 float planes w x h (0..1), out = 3 planes 4w x 4h (not clamped) */
int ai_cpu_forward(const ai_weights* wt, const float* in, int w, int h, float* out);

/* GPU: hlsl_path = runtime/gpu/shaders/ai_up.hlsl (compiled at start with the system's d3dcompiler_47.dll).
 * max_pixels bounds one batch (images x padded pixels; the 4x feature buffers are 4 KB per input pixel each). */
ai_gpu* ai_gpu_create(struct ID3D11Device* dev, struct ID3D11DeviceContext* ctx, const ai_weights* wt,
                      const char* hlsl_path, size_t max_pixels, char* err, size_t errn);
/* n images of w x h, each 3 float planes (in: n*3*w*h, out: n*3*16*w*h); n * w * h <= max_pixels */
int ai_gpu_run(ai_gpu* g, int n, int w, int h, const float* in, float* out);
size_t ai_gpu_bytes(const ai_gpu* g);     /* the buffers allocated so far (weights + features + staging) */
void ai_gpu_destroy(ai_gpu* g);

#endif
