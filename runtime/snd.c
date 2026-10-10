/* High-level emulation of the 989snd sound library's IOP RPC servers (silent for now).
 *
 * The EE side batches sound commands into one RPC call on server 0x123456. The reply buffer
 * holds one result per command between two 0xFFFFFFFF sentinel words; the EE only accepts it
 * when both sentinels are present (proof the IOP's DMA arrived completely). */
#include "ps2_internal.h"

#define SND_SID_CMD   0x00123456u
#define SND_SID_OTHER 0x00123457u

int snd_rpc(ps2_ctx* ctx, uint32_t sid, uint32_t fno, const uint8_t* args, uint32_t send_size,
            uint8_t* out, uint32_t recv_size) {
    (void)ctx; (void)args; (void)send_size;
    if (sid == SND_SID_CMD) {
        if (recv_size >= 8) {
            uint32_t sentinel = 0xFFFFFFFFu;
            memcpy(out, &sentinel, 4);
            memcpy(out + (recv_size & ~3u) - 4, &sentinel, 4);
        }
        if (ps2_log_sif) fprintf(stderr, "[snd] batch fn %u (%u bytes of results)\n", fno, recv_size);
        return 1;
    }
    if (sid == SND_SID_OTHER) {
        if (ps2_log_sif) fprintf(stderr, "[snd] 0x123457 fn %u\n", fno);
        return 1;                                   /* zeros */
    }
    return 0;
}
