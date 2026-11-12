/* (ai) Real-ESRGAN x4 (RRDBNet) inference on D3D11 compute shaders, and a plain C reference of the same network
 * (notes/ai-textures.md). Used by the standalone tool scripts/ai_up_tool.c and, time-sliced over frames, by the game's
 * Reimagined textures (gpu_tex_ai.h, notes/reimagined.md). */
#ifndef AI_UP_H
#define AI_UP_H
#include <stddef.h>
#include <stdint.h>

typedef struct ai_weights ai_weights;
typedef struct ai_gpu ai_gpu;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11UnorderedAccessView;
struct ID3D11Buffer;

/* weights.bin as written by scripts/ai_model.py export (SLYAIW01); NULL + err on failure */
ai_weights* ai_weights_load(const char* path, char* err, size_t errn);
void ai_weights_free(ai_weights* w);

/* CPU reference (slow): in = 3 float planes w x h (0..1), out = 3 planes 4w x 4h (not clamped) */
int ai_cpu_forward(const ai_weights* wt, const float* in, int w, int h, float* out);

/* GPU (tool): hlsl_path = runtime/gpu/shaders/ai_up.hlsl (compiled on first use with the system's d3dcompiler_47.dll).
 * max_pixels bounds one batch (images x padded pixels; ~10.4 KB of buffers per input pixel). */
ai_gpu* ai_gpu_create(struct ID3D11Device* dev, struct ID3D11DeviceContext* ctx, const ai_weights* wt,
                      const char* hlsl_path, size_t max_pixels, char* err, size_t errn);
/* GPU (game): the permutations come from fn(key) (an ID3D11ComputeShader*, AddRef'd here; compiled at build time).
 * key = in | out << 4 | r1 << 8 | r2 << 12 | o2 << 16 (buffer indices, 8 = none): ai_up.hlsl BIN / BOUT / BR1 / BR2 / BO2 */
typedef void* (*ai_cs_fn)(uint32_t key, void* user);
/* (tex-opt) half: AI_GPU_HALF = feature buffers 1..6 as half floats (ai_up.hlsl AI_HALF; the key gains half << 20),
 * AI_GPU_HALF_W = the weights too; 0 = all float (the cache's x4plus-v3 images were built that way) */
enum { AI_GPU_HALF = 1, AI_GPU_HALF_W = 2 };
ai_gpu* ai_gpu_create_cs(struct ID3D11Device* dev, struct ID3D11DeviceContext* ctx, const ai_weights* wt, ai_cs_fn fn,
                         void* user, size_t max_pixels, int half, char* err, size_t errn);
/* n images of w x h, each 3 float planes (in: n*3*w*h, out: n*3*16*w*h); n * w * h <= max_pixels (tool: all at once) */
int ai_gpu_run(ai_gpu* g, int n, int w, int h, const float* in, float* out);

/* time-sliced runs (game): begin plans n images of w x h; the input (3 float planes per image) is uploaded with
 * ai_gpu_upload or written into AI_BUF_IMG by a shader; step issues dispatches (layers split into bands of 8 output rows)
 * worth about `budget` GFLOP and returns what it issued; done when every layer is issued. The output is AI_BUF_OUT
 * (3 planes of 4w x 4h per image). */
enum { AI_BUF_IMG = 0, AI_BUF_OUT = 7 };
int ai_gpu_begin(ai_gpu* g, int n, int w, int h);
void ai_gpu_upload(ai_gpu* g, const float* in);
double ai_gpu_step(ai_gpu* g, double budget);
int ai_gpu_done(const ai_gpu* g);
int ai_gpu_failed(const ai_gpu* g);
double ai_gpu_total_gflop(const ai_gpu* g);
struct ID3D11UnorderedAccessView* ai_gpu_uav(ai_gpu* g, int which);
struct ID3D11Buffer* ai_gpu_buffer(ai_gpu* g, int which);

size_t ai_gpu_bytes(const ai_gpu* g);     /* the buffers allocated so far (weights + features + staging) */
size_t ai_gpu_max_pixels(const ai_gpu* g);
int ai_gpu_blocks(const ai_gpu* g);       /* RRDB blocks (23: x4plus, 6: anime 6B) */
void ai_gpu_destroy(ai_gpu* g);

#endif
