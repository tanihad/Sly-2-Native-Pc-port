/* High-level emulation of the IOP's module loader (LOADFILE) and heap (IOPHEAP) RPC servers.
 * Modules aren't actually run: each load is logged and reported as successful, and the
 * services those modules provide are emulated by their own RPC handlers. */
#include "ps2_internal.h"

#define LOADFILE_SID 0x80000006u
#define IOPHEAP_SID  0x80000003u

/* IOP heap: a bump allocator over the part of IOP RAM not used for RPC buffers. */
#define HEAP_START 0x00140000u
#define HEAP_END   0x001F0000u
static uint32_t heap_top = HEAP_START;

static uint32_t heap_alloc(uint32_t size) {
    size = (size + 63) & ~63u;
    if (heap_top + size > HEAP_END) {
        fprintf(stderr, "[iop] heap exhausted (%u bytes requested)\n", size);
        return 0;
    }
    uint32_t a = heap_top;
    heap_top += size;
    return a;
}

/* Find a printable module name: the path for file loads, or the IRX's module name for
 * buffers (IRX files are ELFs; the name sits in the .iopmod section, so just look for the
 * longest printable string near the start as a cheap identification). */
static void describe_buffer(uint32_t iop_addr, char* name, size_t cap) {
    const uint8_t* p = iop_mem(iop_addr);
    name[0] = 0;
    if (!p || memcmp(p, "\x7f" "ELF", 4) != 0) { snprintf(name, cap, "(not an ELF at 0x%08X)", iop_addr); return; }
    size_t best = 0, best_len = 0;
    for (size_t i = 0; i < 0x400; i++) {
        size_t n = 0;
        while (i + n < 0x400 && p[i + n] >= 0x20 && p[i + n] < 0x7F) n++;
        if (n > best_len && n < 40) { best = i; best_len = n; }
        i += n;
    }
    snprintf(name, cap, "%.*s", (int)best_len, (const char*)p + best);
}

static int next_module_id = 1;

int iop_modules_rpc(ps2_ctx* ctx, uint32_t sid, uint32_t fno, const uint8_t* args, uint32_t send_size,
                    uint8_t* out, uint32_t recv_size) {
    (void)ctx;
    uint32_t a[4] = { 0 };
    memcpy(a, args, send_size < sizeof a ? send_size : sizeof a);

    if (sid == IOPHEAP_SID) {
        uint32_t r = 0;
        switch (fno) {
        case 1: r = heap_alloc(a[0]); break;                    /* AllocIopHeap(size) */
        case 2: r = 0; break;                                   /* FreeIopHeap: never reused */
        default: return 0;
        }
        if (recv_size >= 4) memcpy(out, &r, 4);
        if (ps2_log_sif) fprintf(stderr, "[iop] heap fn %u(%u) -> 0x%08X\n", fno, a[0], r);
        return 1;
    }

    if (sid == LOADFILE_SID) {
        char name[256];
        switch (fno) {
        case 255:                                               /* version: matches IOPRP300 */
            if (recv_size >= 4) memcpy(out, "3000", 4);
            return 1;
        case 0: case 1:                                         /* load module / ELF from a path */
            /* args: argument length, then the path string at offset 8 */
            snprintf(name, sizeof name, "%.*s", (int)(send_size > 8 ? send_size - 8 : 0), (const char*)args + 8);
            break;
        case 6:                                                 /* load module from IOP memory */
            describe_buffer(a[0], name, sizeof name);
            break;
        default:
            return 0;
        }
        int id = next_module_id++;
        fprintf(stderr, "[iop] load module #%d: %s\n", id, name);
        uint32_t reply[2] = { (uint32_t)id, 0 };                /* module id, module's result */
        memcpy(out, reply, recv_size < 8 ? recv_size : 8);
        return 1;
    }
    return 0;
}
