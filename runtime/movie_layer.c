/* Movies at the source's resolution (movie_layer.h, notes/movies.md). */
#include "movie_layer.h"
#include "gs_hint.h"
#include "gs_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

void gs_write_reg(uint32_t r, uint64_t v);       /* gs_thread.c */

#define NSLOT  8                         /* pictures in flight between the GS thread and the presenters */
#define NENTRY 64                        /* VBlank -> slot */
#define MAX_W  1024
#define MAX_H  1024

static int enabled = -1;
static int on_ee(void) {
    if (enabled < 0) { const char* e = getenv("PS2_MOVIE_LAYER"); enabled = !(e && *e == '0'); }
    return enabled;
}

void movie_layer_ee(int on) {
    if (!on_ee()) return;
    gs_write_reg(GS_HINT_REG, gs_hint_make(on ? GS_HINT_MOVIE_BEGIN : GS_HINT_MOVIE_END, 0, 0));
}

/* ---- GS thread ---------------------------------------------------------------------------------- */
static struct { uint32_t* px; int w, h; volatile LONG serial; } slot[NSLOT];
static struct { volatile LONG vblank_p1, slot; } entry[NENTRY];   /* vblank + 1 (0 = none), slot or -1 */
static int active;
static int cur = -1;                     /* the slot of the last movie sprite */
static int state;                        /* this VBlank: 0 nothing drawn, 1 the movie sprite last, 2 something after it */
static int last_slot = -1;               /* the decision of the last VBlank that drew */
static uint32_t k_tbp, k_tbw, k_mod;
static int k_u0, k_v0, k_u1, k_v1;
static uint64_t k_uploads;
static LONG serials;
static unsigned n_layer, n_fallback, n_snap;

int movie_gs_active(void) { return active; }

void movie_gs_hint(int on) {
    if (on == active) return;
    active = on;
    state = 0;
    last_slot = -1;
    if (on) {
        cur = -1;
        n_layer = n_fallback = n_snap = 0;
        fprintf(stderr, "[movie] begin: the decoded picture is shown at the output's resolution\n");
    } else {
        fprintf(stderr, "[movie] end: %u VBlanks shown from the decoded picture, %u from the GS picture (something drawn over it), "
                "%u pictures\n", n_layer, n_fallback, n_snap);
    }
}

static uint32_t modulate(uint32_t c, uint32_t mod) {
    uint32_t r = ((c >> 16) & 0xFF) * (mod & 0xFF) >> 7, g = ((c >> 8) & 0xFF) * ((mod >> 8) & 0xFF) >> 7,
             b = (c & 0xFF) * ((mod >> 16) & 0xFF) >> 7;
    return 0xFF000000u | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

void movie_gs_sprite(uint32_t tbp, uint32_t tbw, int u0, int v0, int u1, int v1, uint32_t mod, uint64_t uploads) {
    if (!active) return;
    int w = u1 - u0, h = v1 - v0;
    if (w < 64 || h < 64 || w > MAX_W || h > MAX_H || u0 < 0 || v0 < 0 || tbw == 0) { movie_gs_other(); return; }
    if (cur < 0 || uploads != k_uploads || tbp != k_tbp || tbw != k_tbw || mod != k_mod || u0 != k_u0 || v0 != k_v0 ||
        u1 != k_u1 || v1 != k_v1) {
        int s = (cur + 1) % NSLOT;
        if (!slot[s].px) slot[s].px = malloc((size_t)MAX_W * MAX_H * 4);
        if (!slot[s].px) { movie_gs_other(); return; }
        uint32_t* d = slot[s].px;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t v = gs_mem_read(PSMCT32, tbp, tbw, (u0 + x) & 2047, (v0 + y) & 2047);
                uint32_t c = 0xFF000000u | (v & 0xFF) << 16 | (v & 0xFF00) | ((v >> 16) & 0xFF);   /* ABGR -> ARGB */
                d[(size_t)y * w + x] = mod ? modulate(c, mod) : c;
            }
        slot[s].w = w;
        slot[s].h = h;
        InterlockedExchange(&slot[s].serial, InterlockedIncrement(&serials));
        cur = s;
        n_snap++;
        k_tbp = tbp; k_tbw = tbw; k_mod = mod; k_u0 = u0; k_v0 = v0; k_u1 = u1; k_v1 = v1; k_uploads = uploads;
    }
    state = 1;
}

void movie_gs_other(void) {
    if (active && state == 1) state = 2;
    else if (active && state == 0) state = 3;      /* (before any movie sprite in this VBlank: covered if one follows) */
}

void movie_gs_vblank(uint32_t vblank) {
    int s = -1;
    if (active) {
        if (state == 1) s = cur;
        else if (state == 0) s = last_slot;        /* nothing drawn: the picture is the last one */
        if (state) last_slot = s;
        if (state) { if (s >= 0) n_layer++; else n_fallback++; }
    }
    state = 0;
    int e = (int)(vblank % NENTRY);
    InterlockedExchange(&entry[e].slot, s);
    InterlockedExchange(&entry[e].vblank_p1, (LONG)(vblank + 1));
}

/* ---- presenters ---------------------------------------------------------------------------------- */
int movie_layer_get(uint32_t vblank, const uint32_t** px, int* w, int* h, uint32_t* serial) {
    int e = (int)(vblank % NENTRY);
    if ((uint32_t)entry[e].vblank_p1 != vblank + 1) return 0;
    int s = (int)entry[e].slot;
    if (s < 0 || s >= NSLOT || !slot[s].px) return 0;
    *px = slot[s].px;
    *w = slot[s].w;
    *h = slot[s].h;
    if (serial) *serial = (uint32_t)slot[s].serial;
    return 1;
}
