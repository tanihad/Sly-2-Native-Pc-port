/* The IOP's module loader (LOADFILE) and heap (IOPHEAP) RPC servers.
 *
 * Modules the recompiler translated (989snd_Library and spiop, see iop.h) are loaded and started
 * natively (iop_loader.c) unless PS2_AUDIO=0. Every other module is not run: each load is logged
 * and reported as successful, and the services those modules provide are emulated by their own
 * RPC handlers (cdvd.c, pad.c, mc.c, and snd.c as the sound fallback). */
#include "ps2_internal.h"
#include "iop.h"

#define LOADFILE_SID 0x80000006u
#define IOPHEAP_SID  0x80000003u

/* LOADFILE fn 6 argument block (Sony's loadfile protocol, also in ps2sdk's loadfile.c):
 * module address, argument length, a path buffer (unused here), then the arguments. */
#define LF_PATH_MAX 252
#define LF_ARG_MAX  252
#define LF_ARGS_OFF (8 + LF_PATH_MAX)

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
        case 1: r = iop_alloc(a[0]); break;                     /* AllocIopHeap(size) */
        case 2: r = (uint32_t)iop_free(a[0]); break;            /* FreeIopHeap(addr) */
        default: return 0;
        }
        if (recv_size >= 4) memcpy(out, &r, 4);
        if (ps2_log_sif) fprintf(stderr, "[iop] heap fn %u(0x%X) -> 0x%08X\n", fno, a[0], r);
        return 1;
    }

    if (sid == LOADFILE_SID) {
        char name[256];
        int32_t modres = 0;
        int native = 0;
        switch (fno) {
        case 255:                                               /* version: matches IOPRP300 */
            if (recv_size >= 4) memcpy(out, "3000", 4);
            return 1;
        case 0: case 1:                                         /* load module / ELF from a path */
            /* args: argument length, then the path string at offset 8 */
            snprintf(name, sizeof name, "%.*s", (int)(send_size > 8 ? send_size - 8 : 0), (const char*)args + 8);
            break;
        case 6: {                                               /* load module from IOP memory */
            describe_buffer(a[0], name, sizeof name);
            uint32_t arg_len = a[1] > LF_ARG_MAX ? LF_ARG_MAX : a[1];
            if (send_size < LF_ARGS_OFF + arg_len) arg_len = send_size > LF_ARGS_OFF ? send_size - LF_ARGS_OFF : 0;
            const char* margs = (const char*)args + LF_ARGS_OFF;
            if (arg_len) {
                char shown[LF_ARG_MAX + 1];
                for (uint32_t i = 0; i < arg_len; i++) shown[i] = margs[i] ? margs[i] : ' ';
                shown[arg_len] = 0;
                fprintf(stderr, "[iop] module arguments: %s\n", shown);
            }
            native = iop_load_module(a[0], margs, arg_len, &modres);
            break;
        }
        default:
            return 0;
        }
        int id = next_module_id++;
        fprintf(stderr, "[iop] load module #%d: %s%s\n", id, name, native ? " (recompiled, running)" : "");
        uint32_t reply[2] = { (uint32_t)id, (uint32_t)modres }; /* module id, module's start result */
        memcpy(out, reply, recv_size < 8 ? recv_size : 8);
        return 1;
    }
    return 0;
}
