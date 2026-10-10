/* GS local memory transfers (BITBLTBUF / TRXPOS / TRXREG / TRXDIR).
 *
 *  - host->local: the data is a packed pixel stream in raster order over the TRXREG rectangle:
 *    32-bit formats 4 bytes/pixel, 24-bit 3 bytes (packed), 16-bit 2, PSMT8/T8H 1, PSMT4/T4HL/T4HH
 *    4 bits (first pixel in the low nibble). The stream may arrive in pieces of any size.
 *  - local->host: the same packing, produced on demand.
 *  - local->local: performed at once, pixel by pixel in the order TRXPOS.DIR selects
 *    (0: up-left to down-right, 1: down-left to up-right, 2: up-right to down-left,
 *    3: down-right to up-left), so overlapping copies behave as the game expects.
 * Rectangle coordinates wrap at 2048 in X and Y ((TRXPOS + offset) % 2048). A rectangle wider than
 * the buffer is not clipped: its pixels are addressed with the normal page arithmetic and so land
 * in the following pages, as on hardware. */
#include "gs_mem_swizzle.h"

enum { XF_NONE, XF_IN, XF_OUT };

static struct {
    int      mode;
    int      psm, bpp;
    uint32_t bp, bw;
    uint32_t x0, y0, w, h;       /* rectangle */
    uint32_t cx, cy;             /* next pixel, relative to (x0, y0) */
    uint8_t  carry[4];           /* host->local: bytes of a pixel split across calls */
    uint32_t ncarry;
    uint8_t  out[1024];          /* local->host: encoded bytes not yet handed out */
    uint32_t out_pos, out_len;
} xf;

/* Write n stream pixels, row by row; stops (and ends the transfer) at the end of the rectangle. */
static void put_pixels(const uint32_t* v, uint32_t n) {
    while (n && xf.mode == XF_IN) {
        uint32_t seg = xf.w - xf.cx;
        if (seg > n) seg = n;
        uint32_t y = (xf.y0 + xf.cy) & 2047u, x = xf.x0 + xf.cx;
        switch (xf.psm) {                    /* hoist the common formats out of the generic path */
        case PSMCT32:
            for (uint32_t i = 0; i < seg; i++) gs_vram_wr32(gs_addr32(gs_swz_ct32, xf.bp, xf.bw, (x + i) & 2047u, y), v[i]);
            break;
        case PSMT8:
            for (uint32_t i = 0; i < seg; i++) gs_vram[gs_addr8(xf.bp, xf.bw, (x + i) & 2047u, y)] = (uint8_t)v[i];
            break;
        default:
            for (uint32_t i = 0; i < seg; i++) gs_px_write(xf.psm, xf.bp, xf.bw, (x + i) & 2047u, y, v[i]);
            break;
        }
        v += seg; n -= seg; xf.cx += seg;
        if (xf.cx == xf.w) {
            xf.cx = 0;
            if (++xf.cy == xf.h) xf.mode = XF_NONE;
        }
    }
}

/* Read up to n stream pixels; returns how many (fewer only at the end of the rectangle). */
static uint32_t get_pixels(uint32_t* v, uint32_t n) {
    uint32_t got = 0;
    while (got < n && xf.mode == XF_OUT) {
        uint32_t seg = xf.w - xf.cx;
        if (seg > n - got) seg = n - got;
        uint32_t y = (xf.y0 + xf.cy) & 2047u, x = xf.x0 + xf.cx;
        if (xf.psm == PSMCT32)
            for (uint32_t i = 0; i < seg; i++) v[got + i] = gs_vram_rd32(gs_addr32(gs_swz_ct32, xf.bp, xf.bw, (x + i) & 2047u, y));
        else
            for (uint32_t i = 0; i < seg; i++) v[got + i] = gs_px_read(xf.psm, xf.bp, xf.bw, (x + i) & 2047u, y);
        got += seg; xf.cx += seg;
        if (xf.cx == xf.w) {
            xf.cx = 0;
            if (++xf.cy == xf.h) xf.mode = XF_NONE;
        }
    }
    return got;
}

static uint32_t decode_px(const uint8_t* p, uint32_t bytes) {
    switch (bytes) {
    case 4: return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    case 3: return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
    case 2: return (uint32_t)p[0] | (uint32_t)p[1] << 8;
    default: return p[0];
    }
}

static void local_to_local(uint64_t bitbltbuf, uint64_t trxpos, uint32_t w, uint32_t h) {
    int spsm = (int)(bitbltbuf >> 24 & 0x3F), dpsm = (int)(bitbltbuf >> 56 & 0x3F);
    uint32_t sbp = (uint32_t)(bitbltbuf & 0x3FFF), sbw = (uint32_t)(bitbltbuf >> 16 & 0x3F);
    uint32_t dbp = (uint32_t)(bitbltbuf >> 32 & 0x3FFF), dbw = (uint32_t)(bitbltbuf >> 48 & 0x3F);
    uint32_t sx = (uint32_t)(trxpos & 0x7FF), sy = (uint32_t)(trxpos >> 16 & 0x7FF);
    uint32_t dx = (uint32_t)(trxpos >> 32 & 0x7FF), dy = (uint32_t)(trxpos >> 48 & 0x7FF);
    uint32_t dir = (uint32_t)(trxpos >> 59 & 3);
    if (!gs_psm_bpp(spsm) || !gs_psm_bpp(dpsm))
        gs_mem_warn(GSW_XFER_BAD_PSM, "local->local transfer with invalid PSM 0x%02X -> 0x%02X", spsm, dpsm);
    else if (gs_psm_bpp(spsm) != gs_psm_bpp(dpsm))
        gs_mem_warn(GSW_XFER_BPP, "local->local transfer between PSM 0x%02X and 0x%02X (different bits per pixel)", spsm, dpsm);

    for (uint32_t jj = 0; jj < h; jj++) {
        uint32_t j = (dir & 1) ? h - 1 - jj : jj;          /* DIR bit 0: bottom row first */
        uint32_t ys = (sy + j) & 2047u, yd = (dy + j) & 2047u;
        if (spsm == dpsm && (spsm == PSMCT32 || spsm == PSMZ32)) {   /* common case: whole words */
            const uint16_t* st = spsm == PSMCT32 ? gs_swz_ct32 : gs_swz_z32;
            for (uint32_t ii = 0; ii < w; ii++) {
                uint32_t i = (dir & 2) ? w - 1 - ii : ii;  /* DIR bit 1: rightmost column first */
                gs_vram_wr32(gs_addr32(st, dbp, dbw, (dx + i) & 2047u, yd),
                             gs_vram_rd32(gs_addr32(st, sbp, sbw, (sx + i) & 2047u, ys)));
            }
            continue;
        }
        for (uint32_t ii = 0; ii < w; ii++) {
            uint32_t i = (dir & 2) ? w - 1 - ii : ii;
            gs_px_write(dpsm, dbp, dbw, (dx + i) & 2047u, yd, gs_px_read(spsm, sbp, sbw, (sx + i) & 2047u, ys));
        }
    }
}

void gs_mem_xfer_start(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, uint32_t dir) {
    uint32_t w = (uint32_t)(trxreg & 0xFFF), h = (uint32_t)(trxreg >> 32 & 0xFFF);
    dir &= 3;
    xf.mode = XF_NONE;
    xf.ncarry = 0;
    xf.out_pos = xf.out_len = 0;
    switch (dir) {
    case 0:                                  /* host -> local: destination fields */
    case 1:                                  /* local -> host: source fields */
        if (dir == 0) {
            xf.bp = (uint32_t)(bitbltbuf >> 32 & 0x3FFF); xf.bw = (uint32_t)(bitbltbuf >> 48 & 0x3F);
            xf.psm = (int)(bitbltbuf >> 56 & 0x3F);
            xf.x0 = (uint32_t)(trxpos >> 32 & 0x7FF); xf.y0 = (uint32_t)(trxpos >> 48 & 0x7FF);
        } else {
            xf.bp = (uint32_t)(bitbltbuf & 0x3FFF); xf.bw = (uint32_t)(bitbltbuf >> 16 & 0x3F);
            xf.psm = (int)(bitbltbuf >> 24 & 0x3F);
            xf.x0 = (uint32_t)(trxpos & 0x7FF); xf.y0 = (uint32_t)(trxpos >> 16 & 0x7FF);
        }
        xf.bpp = gs_psm_bpp(xf.psm);
        if (!xf.bpp) {
            gs_mem_warn(GSW_XFER_BAD_PSM, "%s transfer with invalid PSM 0x%02X (treated as PSMCT32)",
                        dir == 0 ? "host->local" : "local->host", xf.psm);
            xf.psm = PSMCT32;
            xf.bpp = 32;
        }
        xf.w = w; xf.h = h; xf.cx = xf.cy = 0;
        if (w && h) xf.mode = dir == 0 ? XF_IN : XF_OUT;
        break;
    case 2:
        local_to_local(bitbltbuf, trxpos, w, h);
        break;
    default:                                 /* 3: transmission off */
        break;
    }
}

void gs_mem_xfer_write(const void* data, uint32_t bytes) {
    const uint8_t* p = (const uint8_t*)data;
    if (xf.mode != XF_IN) {
        if (bytes) gs_mem_warn(GSW_XFER_IDLE_DATA, "host->local data (%u bytes) with no transfer active", bytes);
        return;
    }
    uint32_t v[256];
    if (xf.bpp == 4) {
        while (bytes && xf.mode == XF_IN) {
            uint32_t n = 0;
            while (bytes && n < 256) { uint8_t b = *p++; bytes--; v[n++] = b & 15u; v[n++] = (uint32_t)b >> 4; }
            put_pixels(v, n);
        }
    } else {
        uint32_t bpx = (uint32_t)xf.bpp / 8;
        if (xf.ncarry) {                      /* finish the pixel split across calls */
            while (xf.ncarry < bpx && bytes) { xf.carry[xf.ncarry++] = *p++; bytes--; }
            if (xf.ncarry < bpx) return;
            xf.ncarry = 0;
            v[0] = decode_px(xf.carry, bpx);
            put_pixels(v, 1);
        }
        while (bytes >= bpx && xf.mode == XF_IN) {
            uint32_t n = 0;
            switch (bpx) {
            case 4: while (bytes >= 4 && n < 256) { v[n++] = decode_px(p, 4); p += 4; bytes -= 4; } break;
            case 3: while (bytes >= 3 && n < 256) { v[n++] = decode_px(p, 3); p += 3; bytes -= 3; } break;
            case 2: while (bytes >= 2 && n < 256) { v[n++] = decode_px(p, 2); p += 2; bytes -= 2; } break;
            default: while (bytes && n < 256) { v[n++] = *p++; bytes--; } break;
            }
            put_pixels(v, n);
        }
        if (xf.mode == XF_IN && bytes) {
            memcpy(xf.carry, p, bytes);
            xf.ncarry = bytes;
            bytes = 0;
        }
    }
    /* Padding after the last pixel (up to the end of the final qword) is normal; more is not. */
    if (bytes >= 16)
        gs_mem_warn(GSW_XFER_EXCESS, "host->local transfer got %u bytes more than its rectangle", bytes);
}

/* Encode the next batch of pixels into xf.out. */
static void refill_out(void) {
    uint32_t v[256], n = get_pixels(v, 256), len = 0;
    uint8_t* o = xf.out;
    switch (xf.bpp) {
    case 32: for (uint32_t i = 0; i < n; i++, len += 4) { uint32_t c = v[i]; memcpy(o + len, &c, 4); } break;
    case 24: for (uint32_t i = 0; i < n; i++, len += 3) { o[len] = (uint8_t)v[i]; o[len + 1] = (uint8_t)(v[i] >> 8); o[len + 2] = (uint8_t)(v[i] >> 16); } break;
    case 16: for (uint32_t i = 0; i < n; i++, len += 2) { o[len] = (uint8_t)v[i]; o[len + 1] = (uint8_t)(v[i] >> 8); } break;
    case 8:  for (uint32_t i = 0; i < n; i++) o[len++] = (uint8_t)v[i]; break;
    default:                                 /* 4: pairs of pixels, low nibble first; n is even except at the end */
        for (uint32_t i = 0; i < n; i += 2) o[len++] = (uint8_t)((v[i] & 15u) | (i + 1 < n ? (v[i + 1] & 15u) << 4 : 0));
        break;
    }
    xf.out_pos = 0;
    xf.out_len = len;
}

uint32_t gs_mem_xfer_read(void* data, uint32_t bytes) {
    uint8_t* o = (uint8_t*)data;
    uint32_t done = 0;
    while (done < bytes) {
        if (xf.out_pos < xf.out_len) {
            uint32_t n = xf.out_len - xf.out_pos;
            if (n > bytes - done) n = bytes - done;
            memcpy(o + done, xf.out + xf.out_pos, n);
            xf.out_pos += n;
            done += n;
            continue;
        }
        if (xf.mode != XF_OUT) break;
        refill_out();
    }
    if (done < bytes) {
        if (!done) gs_mem_warn(GSW_XFER_READ_IDLE, "local->host read (%u bytes) with no transfer data", bytes);
        memset(o + done, 0, bytes - done);
    }
    return done;
}

int gs_mem_xfer_active(void) {
    return xf.mode != XF_NONE || xf.out_pos < xf.out_len;
}
