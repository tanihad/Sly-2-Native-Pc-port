/* Picture comparison and the "both" picture verifier (notes/d3d11-renderer.md 10.1): the software
 * renderer's picture of every VBlank goes to the hw backend, the GPU picture is read back
 * (asynchronously, a few pictures deep) and compared with the software one.
 * Producer side: call from the thread that delivers pictures (gs_thread.c's picture callback). */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t vblank;
    int w, h;                            /* sw size; hw size differs -> size_mismatch */
    int hw_w, hw_h, size_mismatch, readback_error;
    uint32_t diff_px;                    /* pixels where any of R, G, B differs */
    uint32_t diff[3][3];                 /* [channel R G B][|d| > 0, > 2, > 16] pixel counts */
    int bx0, by0, bx1, by1;              /* bounding box of differing pixels (bx0 = -1: none) */
    double psnr;                         /* RGB PSNR in dB; 999 when identical */
} gpu_pic_diff;

/* compare two BGRX pictures (only R, G, B) */
void gpu_pic_compare(const uint32_t* sw, int sw_stride, const uint32_t* hw, int hw_stride, int w, int h,
                     gpu_pic_diff* d);

/* called for every compared picture, in VBlank order, on the producer thread */
typedef void (*gpu_verify_cb)(const gpu_pic_diff* d, const uint32_t* sw, const uint32_t* hw, void* user);

/* GS memory of the GPU backend (hw: gpu_exec_read_memory of the GPU blocks) against the software
 * renderer's (sw), with the default mixed mode's tolerance classes of notes/d3d11-renderer.md 10.2 as
 * gs_replay's lockstep applies them (exact = 0: colour words of GPU targets may differ by up to 16 in
 * at most 2.5% (> 2) / 0.1% (> 16) of the words, 0.1% of the Z words; CPU data exact). */
typedef struct {
    uint64_t c1, c2, c16, cbig;          /* colour words by their largest byte difference: 1, 2, 3..16, > 16 */
    uint64_t z, cpu;                     /* Z words, CPU-owned words differing */
    uint64_t nc, nz;                     /* colour / Z words of GPU targets */
    uint32_t blocks;                     /* 256-byte blocks differing */
    int over;                            /* beyond the tolerance (exact: any difference) */
    int first_block;                     /* (diagnostics) first differing block, -1 none */
} gpu_mem_diff;
/* writer_of[16384]: the writer target of each 256-byte block (0: CPU data); kind_of(id): 0 colour, > 0 Z */
void gpu_mem_compare(const uint8_t* hw, const uint8_t* sw, const uint16_t* writer_of, int (*kind_of)(uint32_t id),
                     int exact, gpu_mem_diff* d);

int  gpu_verify_init(int depth, gpu_verify_cb cb, void* user);   /* depth: pictures in flight (1-8) */
void gpu_verify_picture(const uint32_t* frame, int ok, int w, int h, int stride, uint32_t vblank);
void gpu_verify_flush(void);             /* compare everything still in flight */
void gpu_verify_free(void);
extern volatile long gpu_verify_waiting;   /* 1 + VBlank whose read-back is awaited (diagnostics) */
