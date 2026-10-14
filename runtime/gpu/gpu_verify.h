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

int  gpu_verify_init(int depth, gpu_verify_cb cb, void* user);   /* depth: pictures in flight (1-8) */
void gpu_verify_picture(const uint32_t* frame, int ok, int w, int h, int stride, uint32_t vblank);
void gpu_verify_flush(void);             /* compare everything still in flight */
void gpu_verify_free(void);
extern volatile long gpu_verify_waiting;   /* 1 + VBlank whose read-back is awaited (diagnostics) */
