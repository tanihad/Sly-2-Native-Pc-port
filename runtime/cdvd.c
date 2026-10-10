/* High-level emulation of the IOP's CD/DVD driver (Sony libcdvd RPC servers), backed by an ISO.
 * Set PS2_ISO to the disc image path. Server IDs and function numbers follow Sony's libcdvd
 * (also documented by ps2sdk). */
#include "ps2_internal.h"

#include <stdlib.h>

#define SECTOR 2048u

#define CD_SID_INIT   0x80000592u
#define CD_SID_SCMD   0x80000593u     /* "S-commands": status queries, answered immediately */
#define CD_SID_NCMD   0x80000595u     /* "N-commands": reads and seeks */
#define CD_SID_SEARCH 0x80000596u     /* sceCdSearchFile */
#define CD_SID_READY  0x80000597u     /* sceCdDiskReady */

#define SCECdPS2DVD   0x14
#define SCECdComplete 0x02

static FILE* iso;
static uint64_t iso_sectors;

static void open_iso(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    const char* path = getenv("PS2_ISO");
    if (!path) { fprintf(stderr, "[cdvd] PS2_ISO not set: no disc\n"); return; }
    iso = fopen(path, "rb");
    if (!iso) { fprintf(stderr, "[cdvd] cannot open %s\n", path); return; }
    _fseeki64(iso, 0, SEEK_END);
    iso_sectors = (uint64_t)_ftelli64(iso) / SECTOR;
    fprintf(stderr, "[cdvd] disc image %s (%llu sectors)\n", path, (unsigned long long)iso_sectors);
}

static void put32(uint8_t* out, uint32_t recv_size, uint32_t off, uint32_t v) {
    if (off + 4 <= recv_size) memcpy(out + off, &v, 4);
}

/* Read `count` 2048-byte sectors starting at `lbn` into a host buffer. Shared with the IOP's
 * cdvdman (iop_sif.c), which reads into IOP RAM. */
int cdvd_read_host(uint32_t lbn, uint32_t count, void* dst) {
    open_iso();
    if (!iso || (uint64_t)lbn + count > iso_sectors) {
        fprintf(stderr, "[cdvd] read lbn %u x%u out of range\n", lbn, count);
        return 0;
    }
    _fseeki64(iso, (int64_t)lbn * SECTOR, SEEK_SET);
    return fread(dst, SECTOR, count, iso) == count;
}

/* Read `count` 2048-byte sectors starting at `lbn` into EE memory at `dst`. */
static int read_sectors(ps2_ctx* ctx, uint32_t lbn, uint32_t count, uint32_t dst) {
    static uint8_t buf[SECTOR * 64];
    while (count) {
        uint32_t n = count > 64 ? 64 : count;
        if (!cdvd_read_host(lbn, n, buf)) return 0;
        uint8_t* p = ps2_fast(dst);
        if (p && ps2_fast(dst + n * SECTOR - 1)) memcpy(p, buf, n * SECTOR);
        else for (uint32_t k = 0; k < n * SECTOR; k++) ps2_w8(ctx, dst + k, buf[k]);
        dst += n * SECTOR; lbn += n; count -= n;
    }
    return 1;
}

int cdvd_rpc(ps2_ctx* ctx, uint32_t sid, uint32_t fno, const uint8_t* args, uint32_t send_size,
             uint8_t* out, uint32_t recv_size) {
    uint32_t a[8] = { 0 };
    memcpy(a, args, send_size < sizeof a ? send_size : sizeof a);

    switch (sid) {
    case CD_SID_INIT:
        open_iso();
        put32(out, recv_size, 0, 1);                              /* success */
        return 1;

    case CD_SID_SCMD:
        switch (fno) {
        case 3:  put32(out, recv_size, 0, iso ? SCECdPS2DVD : 0); return 1;   /* GetDiskType */
        case 4:  put32(out, recv_size, 0, 0); return 1;                       /* GetError: none */
        default: return 0;
        }

    case CD_SID_READY:
        put32(out, recv_size, 0, SCECdComplete);
        return 1;

    case CD_SID_NCMD:
        switch (fno) {
        case 1: {                                                 /* Read: lbn, sectors, buf, mode */
            if (ps2_log_sif) fprintf(stderr, "[cdvd] read lbn %u x%u -> 0x%08X\n", a[0], a[1], a[2]);
            int ok = read_sectors(ctx, a[0], a[1], a[2]);
            put32(out, recv_size, 0, ok ? 1 : 0);
            return 1;
        }
        case 5: case 6: case 7: case 8:                           /* Seek, Standby, Stop, Pause */
            put32(out, recv_size, 0, 1);
            return 1;
        case 14:                                                  /* DiskReady */
            put32(out, recv_size, 0, iso ? SCECdComplete : 6);
            return 1;
        default: return 0;
        }

    default:
        return 0;
    }
}
