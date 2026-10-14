/* Picture comparison and the "both" verifier (gpu_verify.h). */
#include "gpu_verify.h"
#include "gpu.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void gpu_pic_compare(const uint32_t* sw, int sw_stride, const uint32_t* hw, int hw_stride, int w, int h,
                     gpu_pic_diff* d) {
    uint32_t vb = d->vblank;
    memset(d, 0, sizeof *d);
    d->vblank = vb;
    d->w = d->hw_w = w;
    d->h = d->hw_h = h;
    d->bx0 = d->by0 = d->bx1 = d->by1 = -1;
    double se = 0;
    for (int y = 0; y < h; y++) {
        const uint32_t* a = sw + (size_t)y * (size_t)sw_stride;
        const uint32_t* b = hw + (size_t)y * (size_t)hw_stride;
        for (int x = 0; x < w; x++) {
            uint32_t pa = a[x], pb = b[x];
            if (((pa ^ pb) & 0xFFFFFF) == 0) continue;
            d->diff_px++;
            for (int c = 0; c < 3; c++) {                       /* R G B = bits 16, 8, 0 */
                int sh = 16 - 8 * c;
                int e = abs((int)((pa >> sh) & 0xFF) - (int)((pb >> sh) & 0xFF));
                if (e > 0) d->diff[c][0]++;
                if (e > 2) d->diff[c][1]++;
                if (e > 16) d->diff[c][2]++;
                se += (double)e * e;
            }
            if (d->bx0 < 0 || x < d->bx0) d->bx0 = x;
            if (x > d->bx1) d->bx1 = x;
            if (d->by0 < 0) d->by0 = y;
            d->by1 = y;
        }
    }
    double mse = w > 0 && h > 0 ? se / ((double)w * h * 3) : 0;
    d->psnr = mse > 0 ? 10.0 * log10(255.0 * 255.0 / mse) : 999.0;
}

/* ---- verifier ---- */

typedef struct {
    uint32_t* sw;                        /* sw copy, stride w */
    uint32_t* hw;                        /* read-back target */
    int w, h, busy;
    uint32_t vblank;
    gpu_readback rb;
} slot;

#define MAXPX (1024 * 1024)

static slot* slots;
static int nslots, head, used;
static gpu_verify_cb callback;
static void* cb_user;

int gpu_verify_init(int depth, gpu_verify_cb cb, void* user) {
    if (depth < 1) depth = 1;
    if (depth > 8) depth = 8;
    slots = calloc((size_t)depth, sizeof *slots);
    if (!slots) return 0;
    for (int i = 0; i < depth; i++) {
        slots[i].sw = malloc(MAXPX * 4);
        slots[i].hw = malloc(MAXPX * 4);
        if (!slots[i].sw || !slots[i].hw) return 0;
    }
    nslots = depth;
    head = used = 0;
    callback = cb;
    cb_user = user;
    return 1;
}

volatile long gpu_verify_waiting;        /* 1 + vblank whose read-back is being waited for, else 0 */

static void retire(void) {
    slot* s = &slots[head];
    gpu_verify_waiting = (long)s->vblank + 1;
    gpu_wait_readback(&s->rb);
    gpu_verify_waiting = 0;
    gpu_pic_diff d;
    memset(&d, 0, sizeof d);
    d.vblank = s->vblank;
    if (s->rb.status != 0) {
        d.w = s->w;
        d.h = s->h;
        d.readback_error = 1;
        d.diff_px = (uint32_t)(s->w * s->h);
        d.psnr = 0;
        d.bx0 = d.by0 = 0;
        d.bx1 = s->w - 1;
        d.by1 = s->h - 1;
    } else if (s->rb.w != s->w || s->rb.h != s->h) {
        d.w = s->w;
        d.h = s->h;
        d.hw_w = s->rb.w;
        d.hw_h = s->rb.h;
        d.size_mismatch = 1;
        d.diff_px = (uint32_t)(s->w * s->h);
        d.psnr = 0;
    } else {
        gpu_pic_compare(s->sw, s->w, s->hw, s->w, s->w, s->h, &d);
    }
    if (callback) callback(&d, s->sw, s->rb.status == 0 && !d.size_mismatch ? s->hw : NULL, cb_user);
    s->busy = 0;
    head = (head + 1) % nslots;
    used--;
}

void gpu_verify_picture(const uint32_t* frame, int ok, int w, int h, int stride, uint32_t vblank) {
    if (!slots) return;
    if (used == nslots) retire();
    slot* s = &slots[(head + used) % nslots];
    if (!ok || !frame || w <= 0 || h <= 0 || (size_t)w * (size_t)h > MAXPX) {
        ok = 0;
        w = 640;                         /* display off: the hw backend shows 640x448 black */
        h = 448;
        memset(s->sw, 0, (size_t)w * h * 4);
    } else {
        for (int y = 0; y < h; y++) memcpy(s->sw + (size_t)y * w, frame + (size_t)y * stride, (size_t)w * 4);
    }
    s->w = w;
    s->h = h;
    s->vblank = vblank;
    s->busy = 1;
    s->rb.dst = s->hw;
    s->rb.cap_pixels = MAXPX;
    gpu_picture(ok ? frame : NULL, ok, w, h, stride, vblank);
    gpu_read_picture(&s->rb);
    used++;
}

void gpu_verify_flush(void) {
    while (used) retire();
}

void gpu_verify_free(void) {
    gpu_verify_flush();
    for (int i = 0; i < nslots; i++) { free(slots[i].sw); free(slots[i].hw); }
    free(slots);
    slots = NULL;
    nslots = 0;
}
